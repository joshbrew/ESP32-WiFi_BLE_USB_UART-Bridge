#ifndef ESP32_MODULAR_CONTROLLER_RUNTIMETASKS_H
#define ESP32_MODULAR_CONTROLLER_RUNTIMETASKS_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "CommandDispatcher.h"
#include "CommandRouter.h"
#include "CommandSelfTest.h"
#include "../addons/DeviceAddon.h"
#include "EventBus.h"
#include "../radio/RadioManager.h"
#include "../hardware/StatusIndicators.h"
#include "../transports/TransportHub.h"
#include "../web/WebPortal.h"

// GO HERE when a subsystem needs regular servicing on the control/network core.
// Timing-critical motor work belongs in its own task, not in serviceOnce().
class RuntimeTasks {
 public:
  RuntimeTasks(
    EventBus &events,
    CommandDispatcher &dispatcher,
    CommandRouter &router,
    CommandSelfTest &selfTest,
    DeviceAddon &addon,
    RadioManager &radios,
    WebPortal &webPortal,
    TransportHub &transports,
    StatusIndicators &indicators
  );

  bool begin();
  void serviceOnce();
  bool isRunning() const;

 private:
  static void networkTaskThunk(void *context);
  void networkTaskLoop();

  EventBus &events_;
  CommandDispatcher &dispatcher_;
  CommandRouter &router_;
  CommandSelfTest &selfTest_;
  DeviceAddon &addon_;
  RadioManager &radios_;
  WebPortal &webPortal_;
  TransportHub &transports_;
  StatusIndicators &indicators_;
  TaskHandle_t networkTask_;
};

#endif  // ESP32_MODULAR_CONTROLLER_RUNTIMETASKS_H
