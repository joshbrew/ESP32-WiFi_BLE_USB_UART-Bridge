/*
  ESP32 MODULAR COMMAND + TRANSPORT CONTROLLER

  Build switches, pins, queue sizes, timing limits, and defaults live in
  src/config/AppConfig.h. Arduino compiles the files under src as normal C++
  modules.
*/

#include <Arduino.h>
#include <esp_system.h>
#include "src/config/AppConfig.h"

#include "src/core/CommandDispatcher.h"
#include "src/core/CommandRouter.h"
#include "src/core/CommandSelfTest.h"
#include "src/addons/DeviceAddon.h"
#include "src/core/EventBus.h"
#include "src/radio/RadioManager.h"
#include "src/core/RuntimeTasks.h"
#include "src/hardware/StatusIndicators.h"
#include "src/transports/TransportBridge.h"
#include "src/transports/TransportHub.h"
#include "src/web/WebPortal.h"


EventBus events;
TransportHub transports(events);
WebPortal webPortal(events);

DeviceAddon deviceAddon;

TransportBridge transportBridge(events, transports, webPortal);
StatusIndicators indicators;
RadioManager radios(events, transports, webPortal);
CommandRouter router(
  events,
  deviceAddon,
  transportBridge,
  radios,
  transports,
  indicators
);
CommandDispatcher dispatcher(events, router, indicators);
CommandSelfTest selfTest(events, dispatcher, deviceAddon, radios);
RuntimeTasks runtimeTasks(
  events,
  dispatcher,
  router,
  selfTest,
  deviceAddon,
  radios,
  webPortal,
  transports,
  indicators
);

bool runtimeTaskStarted = false;

static const char *resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external-reset";
    case ESP_RST_SW: return "software-reset";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt-watchdog";
    case ESP_RST_TASK_WDT: return "task-watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    case ESP_RST_UNKNOWN:
    default: return "unknown";
  }
}

static void printHeapCheckpoint(const char *label) {
  Serial.printf(
    "[HEAP] %s free=%u minimum=%u largest=%u\n",
    label,
    static_cast<unsigned>(ESP.getFreeHeap()),
    static_cast<unsigned>(ESP.getMinFreeHeap()),
    static_cast<unsigned>(ESP.getMaxAllocHeap())
  );
  Serial.flush();
}

void setup() {
  transports.begin();
  const esp_reset_reason_t resetReason = esp_reset_reason();
  Serial.printf(
    "[BOOT][RESET] reason=%s code=%d\n",
    resetReasonName(resetReason),
    static_cast<int>(resetReason)
  );
  Serial.println("[BOOT] creating unified command queue");
  Serial.flush();
  dispatcher.begin();
  router.attachDispatcher(&dispatcher);
  router.attachSelfTest(&selfTest);
  router.begin();

  selfTest.begin();
  const bool selfTestResumeBoot = selfTest.isActive();
  if (selfTestResumeBoot) {
    Serial.println("[BOOT][SELFTEST] fast resume active; cosmetic and addon startup tests suppressed");
    Serial.flush();
  }
  printHeapCheckpoint("after-command-core");

  transports.configureCommandSubmitter(CommandDispatcher::commandThunk, &dispatcher);
  transports.configureStateProvider(CommandRouter::webStateThunk, &router);
  webPortal.configure(
    CommandDispatcher::batchThunk,
    &dispatcher,
    CommandRouter::webStateThunk,
    &router
  );

  Serial.println("[BOOT] starting status indicators");
  Serial.flush();
  indicators.begin(!router.isProductionMode(), selfTestResumeBoot);

  Serial.printf("[BOOT][ADDON] selected=%s\n", deviceAddon.name());
  Serial.flush();
  deviceAddon.begin();
  printHeapCheckpoint("after-addon");

  Serial.println("[BOOT] loading radio configuration");
  Serial.flush();
  radios.begin();
  const bool radioTrialBoot = radios.bootTransactionBusy();
  printHeapCheckpoint("after-radios");

  events.publish(
    EventLevel::STATUS,
    String(AppConfig::FIRMWARE_NAME) + " booted addon=" + deviceAddon.name(),
    CommandSource::INTERNAL
  );

  Serial.println("[BOOT] starting network/control task");
  Serial.flush();
  runtimeTaskStarted = runtimeTasks.begin();
  printHeapCheckpoint("after-runtime-task");

  if (selfTestResumeBoot) {
    Serial.println("[BOOT] automatic self-test resume active; addon startup tests suppressed");
    Serial.flush();
  } else if (radioTrialBoot) {
    Serial.println("[BOOT][RADIO][TRIAL] quiet validation boot; addon startup tests suppressed until profile commit");
    Serial.flush();
  } else if (router.isProductionMode()) {
    Serial.println("[BOOT] production mode: addon startup tests skipped");
    Serial.flush();
    events.publish(
      EventLevel::STATUS,
      "[BOOT] production mode ready; addon startup tests skipped",
      CommandSource::INTERNAL
    );
  } else {
    Serial.println("[BOOT] debug mode: queueing selected addon startup tests");
    Serial.flush();
    deviceAddon.queueStartupTests(dispatcher);
  }

  Serial.println("[BOOT] setup complete; runtime command service active");
  Serial.flush();
}

void loop() {
  if (!runtimeTaskStarted) {
    runtimeTasks.serviceOnce();
    delay(1);
    return;
  }
  delay(1000);
}

