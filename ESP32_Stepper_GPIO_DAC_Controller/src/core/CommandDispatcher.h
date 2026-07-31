#ifndef ESP32_STEPPER_DUAL_DAC_COMMANDDISPATCHER_H
#define ESP32_STEPPER_DUAL_DAC_COMMANDDISPATCHER_H

#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include "../config/AppConfig.h"
#include "AppTypes.h"
#include "EventBus.h"
#include "../hardware/StatusIndicators.h"

class CommandRouter;

// EDIT HERE when changing command admission, batch atomicity, or queue limits.
// Add command meanings in CommandRouter or the owning subsystem controller.
// This class only validates, sequences, and enqueues normalized command lines.
class CommandDispatcher {
 public:
  CommandDispatcher(EventBus &events, CommandRouter &router, StatusIndicators &indicators);

  bool begin();
  void service(uint8_t budget = AppConfig::COMMANDS_PER_SERVICE);
  void clearPending();
  bool hasPendingCommands() const;

  bool submit(
    CommandSource source,
    const String &line,
    const String &requestId
  );

  bool submitBatch(
    CommandSource source,
    const String &body,
    const String &requestId,
    uint16_t &acceptedLines
  );

  String stateJson() const;
  String webStateJson() const;

  static bool commandThunk(
    void *context,
    CommandSource source,
    const String &line,
    const String &requestId
  );

  static bool batchThunk(
    void *context,
    CommandSource source,
    const String &body,
    const String &requestId,
    uint16_t &acceptedLines
  );

 private:
  struct QueuedCommand {
    CommandSource source;
    char requestId[AppConfig::REQUEST_ID_MAX_BYTES + 1];
    char line[AppConfig::COMMAND_MAX_BYTES + 1];
  };

  bool prepare(
    QueuedCommand &destination,
    CommandSource source,
    const String &line,
    const String &requestId
  );
  void reject(CommandSource source, const String &requestId, const String &reason);
  bool lockSubmitter(TickType_t timeout = 0);
  void unlockSubmitter();

  EventBus &events_;
  CommandRouter &router_;
  StatusIndicators &indicators_;
  QueueHandle_t queue_;
  SemaphoreHandle_t submitMutex_;
  std::atomic<uint32_t> acceptedCount_;
  std::atomic<uint32_t> rejectedCount_;
};

#endif  // ESP32_STEPPER_DUAL_DAC_COMMANDDISPATCHER_H
