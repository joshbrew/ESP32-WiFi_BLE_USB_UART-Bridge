#ifndef ESP32_MODULAR_CONTROLLER_EVENTBUS_H
#define ESP32_MODULAR_CONTROLLER_EVENTBUS_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>

#include "../config/AppConfig.h"
#include "AppTypes.h"

// GO HERE only when changing the shared output record format. Every transport and
// the web API consume this ring, so keep records fixed-size and bounded.
struct EventRecord {
  uint32_t id;
  uint32_t timestampMs;
  EventLevel level;
  CommandSource source;
  char requestId[AppConfig::REQUEST_ID_MAX_BYTES + 1];
  char text[AppConfig::EVENT_TEXT_BYTES];
  TransportMask targetMask;
  bool rawPayload;
};

class EventBus {
 public:
  EventBus();

  uint32_t publish(
    EventLevel level,
    const String &text,
    CommandSource source = CommandSource::INTERNAL,
    const String &requestId = String(),
    TransportMask targetMask = TRANSPORT_MASK_DEFAULT,
    bool rawPayload = false
  );

  uint32_t oldestId() const;
  uint32_t latestId() const;
  uint16_t count() const;
  bool nextAfter(uint32_t &cursor, EventRecord &record, bool &gap) const;

 private:
  uint32_t oldestIdUnlocked() const;
  uint32_t latestIdUnlocked() const;

  EventRecord records_[AppConfig::EVENT_CAPACITY];
  uint16_t head_;
  uint16_t count_;
  uint32_t nextId_;
  mutable portMUX_TYPE mux_;
};

#endif  // ESP32_MODULAR_CONTROLLER_EVENTBUS_H
