#pragma once
#include "Arduino.h"
#include "../../src/config/AppConfig.h"
#include "../../src/core/AppTypes.h"
#include <vector>
// Replace the platform's event-ring synchronization, keeping production parsers,
// configuration, routine storage, timing, and coordinate logic unchanged.
#define ESP32_STEPPER_DUAL_DAC_EVENTBUS_H
class EventBus {
 public:
  std::vector<String> messages;
  uint32_t publish(EventLevel,const String &text,CommandSource=CommandSource::INTERNAL,const String &request=String(),TransportMask=TRANSPORT_MASK_DEFAULT,bool=false) { messages.push_back(text);return static_cast<uint32_t>(messages.size()); }
};
