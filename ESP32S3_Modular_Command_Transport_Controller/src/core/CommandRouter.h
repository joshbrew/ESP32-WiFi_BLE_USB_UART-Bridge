#ifndef ESP32_MODULAR_CONTROLLER_COMMANDROUTER_H
#define ESP32_MODULAR_CONTROLLER_COMMANDROUTER_H

#include <Arduino.h>
#include <Preferences.h>

#include "AppTypes.h"
#include "../addons/DeviceAddon.h"
#include "EventBus.h"
#include "../radio/RadioManager.h"
#include "../transports/TransportBridge.h"
#include "../hardware/StatusIndicators.h"
#include "../transports/TransportHub.h"

class CommandDispatcher;
class CommandSelfTest;

// GO HERE to register a new command family or a new subsystem controller. Global
// commands are handled first; subsystem commands are delegated in submit().
class CommandRouter {
 public:
  CommandRouter(
    EventBus &events,
    DeviceAddon &addon,
    TransportBridge &bridge,
    RadioManager &radios,
    TransportHub &transports,
    StatusIndicators &indicators
  );

  void begin();
  void attachDispatcher(CommandDispatcher *dispatcher);
  void attachSelfTest(CommandSelfTest *selfTest);
  bool canAccept(const String &line) const;
  void submit(CommandSource source, const String &line, const String &requestId);
  void service();

  bool isProductionMode() const;
  String bootModeText() const;
  String stateJson() const;
  String webStateJson() const;
  static String stateThunk(void *context);
  static String webStateThunk(void *context);

 private:
  String normalize(String command) const;
  bool setProductionMode(bool enabled);
  void publishBootMode(CommandSource source, const String &requestId, const String &reason);
  void publishHelp(CommandSource source, const String &requestId);
  void publishStatus(CommandSource source, const String &requestId);
  void publishConfig(CommandSource source, const String &requestId);
  void publishChunked(
    EventLevel level,
    CommandSource source,
    const String &requestId,
    const String &prefix,
    const String &message
  );
  void publish(
    EventLevel level,
    CommandSource source,
    const String &requestId,
    const String &message
  );
  void error(CommandSource source, const String &requestId, const String &message);

  EventBus &events_;
  DeviceAddon &addon_;
  TransportBridge &bridge_;
  RadioManager &radios_;
  TransportHub &transports_;
  StatusIndicators &indicators_;
  CommandDispatcher *dispatcher_;
  CommandSelfTest *selfTest_;
  bool productionMode_;
  bool rebootPending_;
  uint32_t rebootAtMs_;
};

#endif  // ESP32_MODULAR_CONTROLLER_COMMANDROUTER_H
