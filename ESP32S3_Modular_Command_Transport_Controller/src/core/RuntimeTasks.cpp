#include "RuntimeTasks.h"

#include "../config/AppConfig.h"

RuntimeTasks::RuntimeTasks(
  EventBus &events,
  CommandDispatcher &dispatcher,
  CommandRouter &router,
  CommandSelfTest &selfTest,
  DeviceAddon &addon,
  RadioManager &radios,
  WebPortal &webPortal,
  TransportHub &transports,
  StatusIndicators &indicators
) : events_(events),
    dispatcher_(dispatcher),
    router_(router),
    selfTest_(selfTest),
    addon_(addon),
    radios_(radios),
    webPortal_(webPortal),
    transports_(transports),
    indicators_(indicators),
    networkTask_(nullptr) {}

bool RuntimeTasks::begin() {
  if (networkTask_ != nullptr) {
    return true;
  }

  if (radios_.isCombinedWifiBleProfile()) {
    events_.publish(
      EventLevel::STATUS,
      "combined WiFi/BLE profile uses the Arduino loop service so its dedicated task stack remains available to HTTP and BLE",
      CommandSource::INTERNAL
    );
    Serial.printf(
      "[BOOT][RUNTIME][COEX] dedicated network task omitted; Arduino loop service active free=%u largest=%u reservedStack=%u\n",
      static_cast<unsigned>(ESP.getFreeHeap()),
      static_cast<unsigned>(ESP.getMaxAllocHeap()),
      static_cast<unsigned>(AppConfig::NETWORK_TASK_STACK_BYTES)
    );
    Serial.flush();
    return false;
  }

  const uint32_t freeBefore = ESP.getFreeHeap();
  const uint32_t largestBefore = ESP.getMaxAllocHeap();
  const uint32_t requiredContiguous = AppConfig::NETWORK_TASK_STACK_BYTES + 1024U;
  if (largestBefore < requiredContiguous) {
    events_.publish(
      EventLevel::WARNING,
      "dedicated network/control task skipped: largest block=" +
        String(largestBefore) + " required=" + String(requiredContiguous) +
        "; Arduino loop service active",
      CommandSource::INTERNAL
    );
    Serial.printf(
      "[BOOT][RUNTIME][FALLBACK] free=%u largest=%u required=%u; Arduino loop service active\n",
      static_cast<unsigned>(freeBefore),
      static_cast<unsigned>(largestBefore),
      static_cast<unsigned>(requiredContiguous)
    );
    Serial.flush();
    return false;
  }

  const BaseType_t result = xTaskCreatePinnedToCore(
    networkTaskThunk,
    "network-control",
    AppConfig::NETWORK_TASK_STACK_BYTES,
    this,
    AppConfig::NETWORK_TASK_PRIORITY,
    &networkTask_,
#if defined(CONFIG_FREERTOS_UNICORE) && CONFIG_FREERTOS_UNICORE
    0
#else
    AppConfig::NETWORK_TASK_CORE
#endif
  );

  if (result != pdPASS) {
    networkTask_ = nullptr;
    events_.publish(
      EventLevel::WARNING,
      "could not create dedicated network/control task; Arduino loop service active",
      CommandSource::INTERNAL
    );
    Serial.printf(
      "[BOOT][RUNTIME][FALLBACK] task creation failed free=%u largest=%u stack=%u; Arduino loop service active\n",
      static_cast<unsigned>(ESP.getFreeHeap()),
      static_cast<unsigned>(ESP.getMaxAllocHeap()),
      static_cast<unsigned>(AppConfig::NETWORK_TASK_STACK_BYTES)
    );
    Serial.flush();
    return false;
  }

  events_.publish(
    EventLevel::STATUS,
    "network, command, transport, and addon service task ready stack=" +
      String(AppConfig::NETWORK_TASK_STACK_BYTES) + " core=" +
      String(AppConfig::NETWORK_TASK_CORE),
    CommandSource::INTERNAL
  );
  Serial.printf(
    "[BOOT][RUNTIME][READY] stack=%u freeBefore=%u freeAfter=%u largestBefore=%u largestAfter=%u\n",
    static_cast<unsigned>(AppConfig::NETWORK_TASK_STACK_BYTES),
    static_cast<unsigned>(freeBefore),
    static_cast<unsigned>(ESP.getFreeHeap()),
    static_cast<unsigned>(largestBefore),
    static_cast<unsigned>(ESP.getMaxAllocHeap())
  );
  Serial.flush();
  return true;
}

// ADD PERIODIC NON-REAL-TIME SERVICE CALLS HERE. Keep each call nonblocking and
// bounded so Wi-Fi, command output, indicators, and addon service stay responsive.
// WebPortal::service() refreshes a cached state snapshot only; AsyncTCP owns sockets.
void RuntimeTasks::serviceOnce() {
  radios_.service();
  webPortal_.service();
  transports_.service();
  dispatcher_.service();
  router_.service();
  selfTest_.service();
  addon_.service();
  indicators_.service(
    radios_.hasUserConnection(),
    radios_.isSeekingConnection(),
    dispatcher_.hasPendingCommands() || addon_.isBusy() || addon_.hasTimedOperationActive()
  );
}

bool RuntimeTasks::isRunning() const {
  return networkTask_ != nullptr;
}

void RuntimeTasks::networkTaskThunk(void *context) {
  static_cast<RuntimeTasks *>(context)->networkTaskLoop();
}

void RuntimeTasks::networkTaskLoop() {
  for (;;) {
    serviceOnce();
    vTaskDelay(AppConfig::NETWORK_TASK_DELAY_TICKS);
  }
}
