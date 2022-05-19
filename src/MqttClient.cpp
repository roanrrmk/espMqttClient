/*
Copyright (c) 2022 Bert Melis. All rights reserved.

This work is licensed under the terms of the MIT license.  
For a copy, see <https://opensource.org/licenses/MIT> or
the LICENSE file.
*/

#include "MqttClient.h"

using espMqttClientInternals::Packet;
using espMqttClientInternals::PacketType;
using espMqttClientTypes::DisconnectReason;
using espMqttClientTypes::Error;

#if defined(ESP32)
MqttClient::MqttClient(uint8_t priority, uint8_t core)
#else
MqttClient::MqttClient()
#endif
: _transport(nullptr)
, _onConnectCallback(nullptr)
, _onDisconnectCallback(nullptr)
, _onSubscribeCallback(nullptr)
, _onUnsubscribeCallback(nullptr)
, _onMessageCallback(nullptr)
, _onPublishCallback(nullptr)
, _onErrorCallback(nullptr)
, _clientId(nullptr)
, _ip()
, _host(nullptr)
, _port(1183)
, _useIp(false)
, _keepAlive(15)
, _cleanSession(false)
, _username(nullptr)
, _password(nullptr)
, _willTopic(nullptr)
, _willPayload(nullptr)
, _willPayloadLength(0)
, _willQos(0)
, _willRetain(false)
, _generatedClientId{0}
, _packetId(0)
, _state(DISCONNECTED)
#if defined(ESP32)
, _xSemaphore(nullptr)
, _taskHandle(nullptr)
#endif
, _rxBuffer{0}
, _outbox()
, _bytesSent(0)
, _parser()
, _lastClientActivity(0)
, _lastServerActivity(0)
, _disconnectReason(DisconnectReason::TCP_DISCONNECTED)
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
, _highWaterMark(4294967295)
#endif
  {
#if defined(ESP32)
  snprintf(_generatedClientId, EMC_CLIENTID_LENGTH, "esp32-%06llx", ESP.getEfuseMac());
  _xSemaphore = xSemaphoreCreateMutex();
  EMC_SEMAPHORE_GIVE();  // release before first use
  xTaskCreatePinnedToCore((TaskFunction_t)_loop, "mqttclient", EMC_TASK_STACK_SIZE, this, priority, &_taskHandle, core);
#elif defined(ESP8266)
  snprintf(_generatedClientId, EMC_CLIENTID_LENGTH, "esp8266-%06x", ESP.getChipId());
#endif
  _clientId = _generatedClientId;
}

MqttClient::~MqttClient() {
  disconnect(true);
  _clearQueue(true);
#if defined(ESP32)
  vSemaphoreDelete(_xSemaphore);
  vTaskDelete(_taskHandle);
#endif
}

bool MqttClient::connected() const {
  if (_state == CONNECTED) return true;
  return false;
}

bool MqttClient::connect() {
  bool result = true;
  if (_state == DISCONNECTED) {
    _state = CONNECTINGTCP;
    Packet packet(_cleanSession,
                  _username,
                  _password,
                  _willTopic,
                  _willRetain,
                  _willQos,
                  _willPayload,
                  _willPayloadLength,
                  _keepAlive,
                  _clientId);
    if (packet.size() > 0) {
      EMC_SEMAPHORE_TAKE();
      _outbox.addFront(packet);
      EMC_SEMAPHORE_GIVE();
#if defined(ESP32)
      vTaskResume(_taskHandle);
#endif
    } else {
      emc_log_e("Could not create CONNECT packet");
      _onError(0, Error::OUT_OF_MEMORY);
      result = false;
    }
  }
  return result;
}

bool MqttClient::disconnect(bool force) {
  bool result = true;
  if (_state != CONNECTED) {
    result = false;
  } else {
    if (force) {
      _state = DISCONNECTINGTCP;
    } else {
      _state = DISCONNECTINGMQTT;
    }
  }
  return result;
}

uint16_t MqttClient::subscribe(const char* topic, uint8_t qos) {
  uint16_t packetId = _getNextPacketId();
  if (_state != CONNECTED) {
    packetId = 0;
  } else {
    Packet packet(topic, qos, packetId);
    if (packet.size() > 0) {
      EMC_SEMAPHORE_TAKE();
      _outbox.add(packet);
      EMC_SEMAPHORE_GIVE();
    } else {
      emc_log_e("Could not create SUBSCRIBE packet");
      _onError(packetId, Error::OUT_OF_MEMORY);
      packetId = 0;
    }
  }
  return packetId;
}

uint16_t MqttClient::unsubscribe(const char* topic) {
  uint16_t packetId = _getNextPacketId();
  if (_state != CONNECTED) {
    packetId = 0;
  } else {
    Packet packet(topic, packetId);
    if (packet.size() > 0) {
      EMC_SEMAPHORE_TAKE();
      _outbox.add(packet);
      EMC_SEMAPHORE_GIVE();
    } else {
      emc_log_e("Could not create UNSUBSCRIBE packet");
      _onError(packetId, Error::OUT_OF_MEMORY);
      packetId = 0;
    }
  }
  return packetId;
}

uint16_t MqttClient::publish(const char* topic, uint8_t qos, bool retain, const uint8_t* payload, size_t length) {
  uint16_t packetId = (qos > 0) ? _getNextPacketId() : 1;
  if (_state != CONNECTED) {
    packetId = 0;
  } else {
    Packet packet(topic, payload, length, qos, retain, packetId);
    if (packet.size() > 0) {
      EMC_SEMAPHORE_TAKE();
      _outbox.add(packet);
      EMC_SEMAPHORE_GIVE();
    } else {
      emc_log_e("Could not create PUBLISH packet");
      _onError(packetId, Error::OUT_OF_MEMORY);
      packetId = 0;
    }
  }
  return packetId;
}

uint16_t MqttClient::publish(const char* topic, uint8_t qos, bool retain, const char* payload) {
  size_t len = strlen(payload);
  return publish(topic, qos, retain, reinterpret_cast<const uint8_t*>(payload), len);
}

void MqttClient::clearQueue(bool all) {
  _clearQueue(true);
}

void MqttClient::loop() {
  switch (_state) {
    case DISCONNECTED:
#if defined(ESP32)
      vTaskSuspend(_taskHandle);
#endif
      break;
    case CONNECTINGTCP:
      if (_useIp ? _transport->connect(_ip, _port) : _transport->connect(_host, _port)) {
        #if defined(ARDUINO_ARCH_ESP8266)
        // reset 'sync' and 'nodelay' at every connect. ESP8266 resets to default on disconnect
        _transport->setSync(false);
        #endif
        _transport->setNoDelay(true);
        _state = CONNECTINGMQTT;
        _lastClientActivity = _lastServerActivity = millis();
      } else {
        _state = DISCONNECTINGTCP;
        _clearQueue(false);
        _disconnectReason = DisconnectReason::TCP_DISCONNECTED;
      }
      break;
    case DISCONNECTINGMQTT:
      EMC_SEMAPHORE_TAKE();
      if (_outbox.empty()) {
        Packet packet(PacketType.DISCONNECT);
        if (packet.size() > 0) {
          _outbox.add(packet);
        } else {
          emc_log_e("Could not create DISCONNECT packet");
          _onError(0, Error::OUT_OF_MEMORY);
        }
      }
      EMC_SEMAPHORE_GIVE();
      // fall through to CONNECTED to send out DISCONN packet
    case CONNECTINGMQTT:
      // receipt of CONNACK packet will set state to CONNECTED
      // client however is allowed to send packets before CONNACK is received
      // so we fall through to CONNECTED
    case CONNECTED:
      if (_transport->connected()) {
        // CONNECT packet is first in the queue
        _checkOutgoing();
        _checkIncoming();
        _checkPing();
      } else {
        _state = DISCONNECTINGTCP;
        _clearQueue(false);
        _disconnectReason = DisconnectReason::TCP_DISCONNECTED;
      }
      break;
    case DISCONNECTINGTCP:
#if defined(ARDUINO_ARCH_ESP32)
      _transport->stop();
#elif defined(ARDUINO_ARCH_ESP8266)
      _transport->stop(0);
#endif
      _state = DISCONNECTED;
      _onDisconnect();
      break;
    // all cases covered, no default case
  }
  EMC_YIELD();
}

#if defined(ESP32)
void MqttClient::_loop(MqttClient* c) {
  for (;;) {
    c->loop();
  #if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
    size_t waterMark = uxTaskGetStackHighWaterMark(NULL);
    if (waterMark < c->_highWaterMark) {
      c->_highWaterMark = waterMark;
      emc_log_i("Free stack space: %u/%u", c->_highWaterMark, EMC_TASK_STACK_SIZE);
    }
  #endif
  }
}
#endif

uint16_t MqttClient::_getNextPacketId() {
  uint16_t packetId = 0;
  EMC_SEMAPHORE_TAKE();
  packetId = (++_packetId == 0) ? ++_packetId : _packetId;
  EMC_SEMAPHORE_GIVE();
  return packetId;
}

void MqttClient::_checkOutgoing() {
  EMC_SEMAPHORE_TAKE();
  Packet* packet =  _outbox.getCurrent();

  int32_t wantToWrite = 0;
  int32_t written = 0;
  while (packet && (wantToWrite == written)) {
    // mixing signed with unsigned here but safe because of MQTT packet size limits
    wantToWrite = packet->size() - _bytesSent;
    written = _transport->write(packet->data(_bytesSent), wantToWrite);
    if (written < 0) {
      emc_log_w("Writing error, check connection");
      break;
    }
    _lastClientActivity = millis();
    _bytesSent += written;
    emc_log_i("tx %u/%u", _bytesSent, packet->size());
    if (_bytesSent == packet->size()) {
      if ((packet->data(0)[0] & 0xF0) == PacketType.DISCONNECT) _state = DISCONNECTINGTCP;
      if (packet->packetId() == 0) {
        _outbox.removeCurrent();
      } else {
        // handle with care! millis() returns unsigned 32 bit, token is void*
        packet->token = reinterpret_cast<void*>(millis());
        _outbox.next();
      }
      packet =  _outbox.getCurrent();
      _bytesSent = 0;
    }
  }
  EMC_SEMAPHORE_GIVE();
}

void MqttClient::_checkIncoming() {
  int32_t remainingBufferLength = _transport->read(_rxBuffer, EMC_RX_BUFFER_SIZE);
  if (remainingBufferLength > 0) {
    _lastServerActivity = millis();
    emc_log_i("rx len %i", remainingBufferLength);
    size_t bytesParsed = 0;
    size_t index = 0;
    while (remainingBufferLength > 0) {
      espMqttClientInternals::ParserResult result = _parser.parse(&_rxBuffer[index], remainingBufferLength, &bytesParsed);
      if (result == espMqttClientInternals::ParserResult::PACKET) {
        espMqttClientInternals::MQTTPacketType packetType = _parser.getPacket().fixedHeader.packetType & 0xF0;
        if (_state == CONNECTINGMQTT && packetType != PacketType.CONNACK) {
          emc_log_w("Disconnecting, expected CONNACK - protocol error");
          _state = DISCONNECTINGTCP;
          return;
        }
        switch (packetType & 0xF0) {
          case PacketType.CONNACK:
            _onConnack();
            if (_state != CONNECTED) {
              return;
            }
            break;
          case PacketType.PUBLISH:
            if (_state == DISCONNECTINGMQTT) break;  // stop processing incoming once user has called disconnect
            _onPublish();
            break;
          case PacketType.PUBACK:
            _onPuback();
            break;
          case PacketType.PUBREC:
            _onPubrec();
            break;
          case PacketType.PUBREL:
            _onPubrel();
            break;
          case PacketType.PUBCOMP:
            _onPubcomp();
            break;
          case PacketType.SUBACK:
            _onSuback();
            break;
          case PacketType.UNSUBACK:
            _onUnsuback();
            break;
          case PacketType.PINGRESP:
            // nothing to do
            break;
        }
      } else if (result ==  espMqttClientInternals::ParserResult::PROTOCOL_ERROR) {
        emc_log_w("Disconnecting, protocol error");
        _state = DISCONNECTINGTCP;
        return;
      }
      remainingBufferLength -= bytesParsed;
      index += bytesParsed;
      emc_log_i("Parsed %u - remaining %u", bytesParsed, remainingBufferLength);
      bytesParsed = 0;
    }
  }
}

void MqttClient::_checkPing() {
  if (_keepAlive == 0) return;  // keepalive is disabled

  // disconnect when server was inactive for twice the keepalive time
  if (millis() - _lastServerActivity > 2000 * _keepAlive) {
    emc_log_w("Disconnecting, server exceeded keepalive");
    _state = DISCONNECTINGTCP;
    return;
  }

  // send ping when client was inactive for 0.7 times the keepalive time
  if (millis() - _lastClientActivity > 700 * _keepAlive) {
    emc_log_i("Near keepalive, sending PING");
    Packet packet(PacketType.PINGREQ);
    if (packet.size() > 0) {
      EMC_SEMAPHORE_TAKE();
      _outbox.add(packet);
      EMC_SEMAPHORE_GIVE();
    } else {
      emc_log_e("Could not create PING packet");
    }
  }
}

void MqttClient::_onConnack() {
  if (_parser.getPacket().variableHeader.fixed.connackVarHeader.returnCode == 0x00) {
    _state = CONNECTED;
    if (_parser.getPacket().variableHeader.fixed.connackVarHeader.sessionPresent == 0) {
      _clearQueue(true);
    }
    if (_onConnectCallback) {
      _onConnectCallback(_parser.getPacket().variableHeader.fixed.connackVarHeader.sessionPresent);
    }
  } else {
    _state = DISCONNECTINGTCP;
    // cast is safe because the parser already checked for a valid return code
    _disconnectReason = static_cast<DisconnectReason>(_parser.getPacket().variableHeader.fixed.connackVarHeader.returnCode);
  }
}

void MqttClient::_onPublish() {
  espMqttClientInternals::IncomingPacket p = _parser.getPacket();
  uint8_t qos = (p.fixedHeader.packetType & 0x06) >> 1;  // mask 0x00000110
  bool retain = p.fixedHeader.packetType & 0x01;         // mask 0x00000001
  bool dup = p.fixedHeader.packetType & 0x08;            // mask 0x00001000
  uint16_t packetId = p.variableHeader.fixed.packetId;
  bool callback = true;
  if (qos == 1) {
    if (p.payload.index + p.payload.length == p.payload.total) {
      Packet packet(PacketType.PUBACK, packetId);
      if (packet.size() > 0) {
        EMC_SEMAPHORE_TAKE();
        _outbox.add(packet);
        EMC_SEMAPHORE_GIVE();
      } else {
        emc_log_e("Could not create PUBACK packet");
      }
    }
  } else if (qos == 2) {
    EMC_SEMAPHORE_TAKE();
    espMqttClientInternals::Outbox<espMqttClientInternals::Packet>::Iterator it = _outbox.front();
    while (it.data()) {
      if ((it.data()->data(0)[0] & 0xF0) == PacketType.PUBREC && it.data()->packetId() == packetId) {
        callback = false;
        emc_log_e("QoS2 packet previously delivered");
        break;
      }
      ++it;
    }
    if (p.payload.index + p.payload.length == p.payload.total) {
      Packet packet(PacketType.PUBREC, packetId);
      if (packet.size() > 0) {
        _outbox.add(packet);
      } else {
        emc_log_e("Could not create PUBREC packet");
      }
    }
    EMC_SEMAPHORE_GIVE();
  }
  if (callback && _onMessageCallback) _onMessageCallback({qos, dup, retain, packetId},
                                                         p.variableHeader.topic,
                                                         p.payload.data,
                                                         p.payload.length,
                                                         p.payload.index,
                                                         p.payload.total);
}

void MqttClient::_onPuback() {
  bool callback = false;
  uint16_t idToMatch = _parser.getPacket().variableHeader.fixed.packetId;
  EMC_SEMAPHORE_TAKE();
  espMqttClientInternals::Outbox<espMqttClientInternals::Packet>::Iterator it = _outbox.front();
  while (it.data()) {
    // PUBACKs come in the order PUBs are sent. So we only check the first PUB packet in outbox
    // if it doesn't match the ID, return
    if ((it.data()->data(0)[0] & 0xF0) == PacketType.PUBLISH) {
      if (it.data()->packetId() == idToMatch) {
        callback = true;
        _outbox.remove(it);
        break;
      }
      emc_log_w("Received out of order PUBACK");
      break;
    }
    ++it;
  }
  if (!it.data()) emc_log_w("No matching PUBLISH packet found");
  EMC_SEMAPHORE_GIVE();
  if (callback && _onPublishCallback) _onPublishCallback(idToMatch);
}

void MqttClient::_onPubrec() {
  uint16_t idToMatch = _parser.getPacket().variableHeader.fixed.packetId;
  EMC_SEMAPHORE_TAKE();
  espMqttClientInternals::Outbox<espMqttClientInternals::Packet>::Iterator it = _outbox.front();
  while (it.data()) {
    // PUBRECs come in the order PUBs are sent. So we only check the first PUB packet in outbox
    // if it doesn't match the ID, return
    if ((it.data()->data(0)[0] & 0xF0) == PacketType.PUBLISH) {
      if (it.data()->packetId() == idToMatch) {
        Packet packet(PacketType.PUBREL, idToMatch);
        if (packet.size() > 0) {
          _outbox.add(packet);
        } else {
          emc_log_e("Could not create PUBREL packet");
        }
        _outbox.remove(it);
        break;
      }
      emc_log_w("Received out of order PUBREC");
      break;
    }
    ++it;
  }
  if (!it.data()) emc_log_w("No matching PUBLISH packet found");
  EMC_SEMAPHORE_GIVE();
}

void MqttClient::_onPubrel() {
  uint16_t idToMatch = _parser.getPacket().variableHeader.fixed.packetId;
  EMC_SEMAPHORE_TAKE();
  espMqttClientInternals::Outbox<espMqttClientInternals::Packet>::Iterator it = _outbox.front();
  while (it.data()) {
    // PUBRELs come in the order PUBRECs are sent. So we only check the first PUBREC packet in outbox
    // if it doesn't match the ID, return
    if ((it.data()->data(0)[0] & 0xF0) == PacketType.PUBREC) {
      if (it.data()->packetId() == idToMatch) {
        Packet packet(PacketType.PUBCOMP, idToMatch);
        if (packet.size() > 0) {
          _outbox.add(packet);
        } else {
          emc_log_e("Could not create PUBCOMP packet");
        }
        _outbox.remove(it);
        break;
      }
      emc_log_w("Received out of order PUBREL");
      break;
    }
    ++it;
  }
  if (!it.data()) emc_log_w("No matching PUBREC packet found");
  EMC_SEMAPHORE_GIVE();
}

void MqttClient::_onPubcomp() {
  bool callback = false;
  EMC_SEMAPHORE_TAKE();
  espMqttClientInternals::Outbox<espMqttClientInternals::Packet>::Iterator it = _outbox.front();
  uint16_t idToMatch = _parser.getPacket().variableHeader.fixed.packetId;
  while (it.data()) {
    // PUBCOMPs come in the order PUBRELs are sent. So we only check the first PUBREL packet in outbox
    // if it doesn't match the ID, return
    if ((it.data()->data(0)[0] & 0xF0) == PacketType.PUBREL) {
      if (it.data()->packetId() == idToMatch) {
        Packet packet(PacketType.PUBCOMP, idToMatch);
        if (packet.size() > 0) {
          _outbox.add(packet);
        } else {
          emc_log_e("Could not create PUBCOMP packet");
        }
        callback = true;
        _outbox.remove(it);
        break;
      }
      emc_log_w("Received out of order PUBCOMP");
      break;
    }
    ++it;
  }
  if (!it.data()) emc_log_w("No matching PUBREL packet found");
  EMC_SEMAPHORE_GIVE();
  if (callback && _onPublishCallback) _onPublishCallback(idToMatch);
}

void MqttClient::_onSuback() {
  bool callback = false;
  uint16_t idToMatch = _parser.getPacket().variableHeader.fixed.packetId;
  EMC_SEMAPHORE_TAKE();
  espMqttClientInternals::Outbox<espMqttClientInternals::Packet>::Iterator it = _outbox.front();
  while (it.data()) {
    if (it.data()->packetId() == idToMatch) {
      callback = true;
      _outbox.remove(it);
      break;
    }
    ++it;
  }
  if (!it.data()) emc_log_w("received SUBACK without SUB");
  EMC_SEMAPHORE_GIVE();
  if (callback && _onSubscribeCallback) _onSubscribeCallback(idToMatch, *(_parser.getPacket().payload.data));
}

void MqttClient::_onUnsuback() {
  bool callback = false;
  EMC_SEMAPHORE_TAKE();
  espMqttClientInternals::Outbox<espMqttClientInternals::Packet>::Iterator it = _outbox.front();
  uint16_t idToMatch = _parser.getPacket().variableHeader.fixed.packetId;
  while (it.data()) {
    if (it.data()->packetId() == idToMatch) {
      callback = true;
      _outbox.remove(it);
      break;
    }
    ++it;
  }
  if (!it.data()) emc_log_w("received UNSUBACK without UNSUB");
  EMC_SEMAPHORE_GIVE();
  if (callback && _onUnsubscribeCallback) _onUnsubscribeCallback(idToMatch);
}

void MqttClient::_onDisconnect() {
  if (_onDisconnectCallback) {
    _onDisconnectCallback(_disconnectReason);
  }
}

void MqttClient::_clearQueue(bool clearSession) {
  EMC_SEMAPHORE_TAKE();
  espMqttClientInternals::Outbox<espMqttClientInternals::Packet>::Iterator it = _outbox.front();
  if (clearSession) {
    while (it.data()) {
      _outbox.remove(it);
    }
  } else {
    // keep PUB, PUBREC and PUBREL
    // Spec only mentions PUB and PUBREL but this lib implements method B from point 4.3.3 (Fig. 4.3)
    // and stores the packet id in the PUBREC packet. So we also must keep PUBREC.
    while (it.data()) {
      espMqttClientInternals::MQTTPacketType type = it.data()->data(0)[0] & 0xF0;
      if (type == PacketType.PUBREC ||
          type == PacketType.PUBREL ||
          (type == PacketType.PUBLISH || it.data()->packetId() == 0)) {
        ++it;
      } else {
        _outbox.remove(it);
      }
    }
  }
  EMC_SEMAPHORE_GIVE();
}

void MqttClient::_onError(uint16_t packetId, espMqttClientTypes::Error error) {
  if (_onErrorCallback) {
    _onErrorCallback(packetId, error);
  }
}