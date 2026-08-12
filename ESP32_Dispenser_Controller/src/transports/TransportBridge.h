#ifndef ESP32_MODULAR_CONTROLLER_TRANSPORTBRIDGE_H
#define ESP32_MODULAR_CONTROLLER_TRANSPORTBRIDGE_H

#include <Arduino.h>

#include "../core/AppTypes.h"

class EventBus;
class TransportHub;
class WebPortal;

// Stateless line sender layered on the shared EventBus. Normal command results
// are already mirrored to active outputs. These commands send an explicit text
// payload to one transport or to every currently available transport except the
// source, without re-entering command intake and without creating bridge loops.
class TransportBridge {
 public:
  TransportBridge(EventBus &events, TransportHub &transports, WebPortal &webPortal);

  bool handleCommand(
    const String &command,
    CommandSource source,
    const String &requestId
  );
  String statusText() const;
  String stateJson() const;

 private:
  bool parsePayload(const String &command, const char *prefix, String &payload) const;
  bool destinationAvailable(CommandSource destination, String &reason) const;
  TransportMask availableDestinations(CommandSource origin) const;
  String transportList(TransportMask mask) const;
  bool sendMask(
    CommandSource origin,
    TransportMask destinations,
    const String &payload,
    const String &requestId
  );
  void error(CommandSource source, const String &requestId, const String &message);

  EventBus &events_;
  TransportHub &transports_;
  WebPortal &webPortal_;
  uint32_t sentCount_;
  uint32_t rejectedCount_;
};

#endif  // ESP32_MODULAR_CONTROLLER_TRANSPORTBRIDGE_H
