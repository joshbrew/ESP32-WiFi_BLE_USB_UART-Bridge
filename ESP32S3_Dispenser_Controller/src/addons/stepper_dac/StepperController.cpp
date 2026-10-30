#include "StepperController.h"

#include <limits.h>
#include <string.h>

#include "../../util/TextUtil.h"

#if APP_STEPPER_DAC_ADDON_ENABLED

namespace {

const uint8_t FULL_STEP_TABLE[4][4] = {
  {1, 1, 0, 0},
  {0, 1, 1, 0},
  {0, 0, 1, 1},
  {1, 0, 0, 1}
};

const uint8_t HALF_STEP_TABLE[8][4] = {
  {1, 0, 0, 0},
  {1, 1, 0, 0},
  {0, 1, 0, 0},
  {0, 1, 1, 0},
  {0, 0, 1, 0},
  {0, 0, 1, 1},
  {0, 0, 0, 1},
  {1, 0, 0, 1}
};

const uint8_t COMMON_ORDERS[][4] = {
  {0, 1, 2, 3},
  {0, 2, 1, 3},
  {0, 1, 3, 2},
  {0, 2, 3, 1},
  {0, 3, 1, 2},
  {0, 3, 2, 1}
};

constexpr int COMMON_ORDER_COUNT = sizeof(COMMON_ORDERS) / sizeof(COMMON_ORDERS[0]);

}  // namespace

StepperController::StepperController(EventBus &events)
  : events_(events),
    pins_{
      AppConfig::PIN_STEPPER_IN1,
      AppConfig::PIN_STEPPER_IN2,
      AppConfig::PIN_STEPPER_IN3,
      AppConfig::PIN_STEPPER_IN4
    },
    logicalOrder_{0, 1, 2, 3},
    currentOrderPreset_(0),
    stepsPerRevolution_(2038),
    minRpm_(AppConfig::STEPPER_DEFAULT_MIN_RPM),
    maxRpm_(AppConfig::STEPPER_DEFAULT_MAX_RPM),
    minStepIntervalUs_(250),
    startRpm_(AppConfig::STEPPER_DEFAULT_START_RPM),
    rampRpmPerSecond_(AppConfig::STEPPER_DEFAULT_RAMP_RPM_PER_SECOND),
    holdTorqueWhenIdle_(false),
    stepMode_(StepMode::FULL),
    motion_{false, 0, 0, DIR_CW, 0.0f, 0.0f, 0, 0, 0, CommandSource::INTERNAL, String()},
    test_{false, false, 0.0f, 0.0f, 0, 0, 0, 0, TestPhase::IDLE, CommandSource::INTERNAL, String(), String()},
    phaseIndex_(0),
    positionSteps_(0),
    commandQueue_(nullptr),
    taskHandle_(nullptr),
    publicState_{},
    publicStateMux_(portMUX_INITIALIZER_UNLOCKED) {}

bool StepperController::begin() {
  for (int pin : pins_) {
    pinMode(pin, OUTPUT);
  }
  releaseCoils();
  updatePublicState();

  commandQueue_ = xQueueCreate(
    AppConfig::MOTOR_COMMAND_QUEUE_CAPACITY,
    sizeof(QueuedCommand)
  );
  if (commandQueue_ == nullptr) {
    events_.publish(
      EventLevel::ERROR,
      "could not create stepper command queue",
      CommandSource::INTERNAL
    );
    return false;
  }

  BaseType_t result = xTaskCreatePinnedToCore(
    taskThunk,
    "stepper-realtime",
    AppConfig::MOTOR_TASK_STACK_BYTES,
    this,
    AppConfig::MOTOR_TASK_PRIORITY,
    &taskHandle_,
#if defined(CONFIG_FREERTOS_UNICORE) && CONFIG_FREERTOS_UNICORE
    0
#else
    AppConfig::MOTOR_TASK_CORE
#endif
  );

  if (result != pdPASS) {
    taskHandle_ = nullptr;
    vQueueDelete(commandQueue_);
    commandQueue_ = nullptr;
    events_.publish(
      EventLevel::ERROR,
      "could not create stepper RTOS task",
      CommandSource::INTERNAL
    );
    return false;
  }

  events_.publish(
    EventLevel::STATUS,
    String("stepper RTOS task ready on GPIO18,19,27,14; 28BYJ-48 profile maxRPM=") +
      String(maxRpm_, 1) + "; startRPM=" + String(startRpm_, 1) +
      "; rampRPMPerSecond=" + String(rampRpmPerSecond_, 1) + "; boot test=" +
      (AppConfig::RUN_STEPPER_BOOT_TEST ? "enabled" : "disabled"),
    CommandSource::INTERNAL
  );
  return true;
}

bool StepperController::handleCommand(
  const String &command,
  CommandSource source,
  const String &requestId
) {
  String work = command;
  work.trim();

  if (!recognizesCommand(work)) {
    return false;
  }

  if (commandQueue_ == nullptr) {
    error(source, requestId, "stepper command queue unavailable");
    return true;
  }

  QueuedCommand queued{};
  queued.source = source;
  work.substring(0, AppConfig::COMMAND_MAX_BYTES).toCharArray(queued.line, sizeof(queued.line));
  requestId.substring(0, AppConfig::REQUEST_ID_MAX_BYTES).toCharArray(
    queued.requestId,
    sizeof(queued.requestId)
  );

  // Stop and coil-release commands are barriers for the private motor queue.
  // They clear already-routed motor work and move to the front. StopAll also
  // clears the upstream unified queue in CommandRouter. Normal moves stay FIFO.
  const bool urgent = isUrgentCommand(work);
  if (urgent) {
    xQueueReset(commandQueue_);
  }
  const BaseType_t queuedResult = urgent
    ? xQueueSendToFront(commandQueue_, &queued, 0)
    : xQueueSend(commandQueue_, &queued, 0);
  if (queuedResult != pdTRUE) {
    error(source, requestId, urgent
      ? "could not queue urgent motor stop"
      : "stepper command queue full; command dropped");
    return true;
  }

  if (taskHandle_ != nullptr) {
    xTaskNotifyGive(taskHandle_);
  }
  return true;
}

bool StepperController::canAcceptCommand(const String &command) const {
  if (!recognizesCommand(command) || isUrgentCommand(command)) {
    return true;
  }
  // If initialization failed, let the router consume the command so the
  // controller can emit its explicit "queue unavailable" error instead of
  // permanently blocking unrelated commands behind it.
  return commandQueue_ == nullptr || uxQueueSpacesAvailable(commandQueue_) > 0;
}

bool StepperController::emergencyStop(
  CommandSource source,
  const String &requestId
) {
  if (commandQueue_ == nullptr) {
    error(source, requestId, "stepper command queue unavailable");
    return false;
  }

  xQueueReset(commandQueue_);
  QueuedCommand queued{};
  queued.source = source;
  String("CoilsOff").toCharArray(queued.line, sizeof(queued.line));
  requestId.substring(0, AppConfig::REQUEST_ID_MAX_BYTES).toCharArray(
    queued.requestId,
    sizeof(queued.requestId)
  );

  if (xQueueSendToFront(commandQueue_, &queued, 0) != pdTRUE) {
    error(source, requestId, "could not queue emergency motor stop");
    return false;
  }
  if (taskHandle_ != nullptr) {
    xTaskNotifyGive(taskHandle_);
  }
  return true;
}


bool StepperController::isBusy() const {
  const PublicState state = snapshot();
  return state.moving || state.testActive || state.queuedCommands > 0;
}

String StepperController::statusText() const {
  const PublicState state = snapshot();
  String text = "position=" + String(state.position);
  text += " baseStepsPerRev=" + String(state.baseStepsPerRev);
  text += " effectiveStepsPerRev=" + String(state.effectiveStepsPerRev);
  text += " minRPM=" + String(state.minRpm, 2);
  text += " maxRPM=" + String(state.maxRpm, 2);
  text += " startRPM=" + String(state.startRpm, 2);
  text += " rampRPMPerSecond=" + String(state.rampRpmPerSecond, 2);
  text += " minIntervalUs=" + String(state.minIntervalUs);
  text += " stepMode=" + String(state.stepMode);
  text += " stepOrder=" + String(state.stepOrder);
  text += " holdTorque=" + String(state.holdTorque ? "on" : "off");
  text += " moving=" + String(state.moving ? "yes" : "no");
  text += " targetRPM=" + String(state.targetRpm, 2);
  text += " currentRPM=" + String(state.currentRpm, 2);
  text += " remaining=" + String(state.remaining);
  text += " queue=" + String(state.queuedCommands);
  return text;
}

void StepperController::publishStatusReadback(
  CommandSource source,
  const String &requestId,
  const char *category
) {
  const PublicState state = snapshot();
  const String prefix = "[" + String(category) + "] motor ";

  String config = prefix + "config position=" + String(state.position);
  config += " baseStepsPerRev=" + String(state.baseStepsPerRev);
  config += " effectiveStepsPerRev=" + String(state.effectiveStepsPerRev);
  config += " minRPM=" + String(state.minRpm, 2);
  config += " maxRPM=" + String(state.maxRpm, 2);
  config += " startRPM=" + String(state.startRpm, 2);
  config += " rampRPMPerSecond=" + String(state.rampRpmPerSecond, 2);
  publish(EventLevel::STATUS, source, requestId, config);

  String runtime = prefix + "runtime minIntervalUs=" + String(state.minIntervalUs);
  runtime += " stepMode=" + String(state.stepMode);
  runtime += " stepOrder=" + String(state.stepOrder);
  runtime += " holdTorque=" + String(state.holdTorque ? "on" : "off");
  runtime += " moving=" + String(state.moving ? "yes" : "no");
  runtime += " targetRPM=" + String(state.targetRpm, 2);
  runtime += " currentRPM=" + String(state.currentRpm, 2);
  runtime += " remaining=" + String(state.remaining);
  runtime += " testActive=" + String(state.testActive ? "yes" : "no");
  runtime += " queue=" + String(state.queuedCommands);
  publish(EventLevel::STATUS, source, requestId, runtime);
}

void StepperController::publishConfigUpdated(
  CommandSource source,
  const String &requestId,
  const String &change
) {
  publish(
    EventLevel::STATUS,
    source,
    requestId,
    "[CONFIG] motor updated scope=runtime " + change
  );
  updatePublicState();
  publishStatusReadback(source, requestId, "CONFIG");
}

String StepperController::stateJson() const {
  const PublicState state = snapshot();
  String json = "{";
  json += "\"positionSteps\":" + String(state.position);
  json += ",\"stepsPerRevolution\":" + String(state.effectiveStepsPerRev);
  json += ",\"stepMode\":" + String(state.stepMode);
  json += ",\"stepOrder\":\"" + String(state.stepOrder) + "\"";
  json += ",\"holdTorque\":" + TextUtil::jsonBool(state.holdTorque);
  json += ",\"moving\":" + TextUtil::jsonBool(state.moving);
  json += ",\"targetRpm\":" + String(state.targetRpm, 2);
  json += ",\"currentRpm\":" + String(state.currentRpm, 2);
  json += ",\"remainingSteps\":" + String(state.remaining);
  json += ",\"testActive\":" + TextUtil::jsonBool(state.testActive);
  json += ",\"queuedCommands\":" + String(state.queuedCommands);
  json += "}";
  return json;
}

String StepperController::webStateJson() const {
  const PublicState state = snapshot();
  String json;
  json.reserve(128);
  json = "{\"moving\":" + TextUtil::jsonBool(state.moving);
  json += ",\"currentRpm\":" + String(state.currentRpm, 2);
  json += ",\"remainingSteps\":" + String(state.remaining);
  json += ",\"testActive\":" + TextUtil::jsonBool(state.testActive);
  json += "}";
  return json;
}

void StepperController::taskThunk(void *context) {
  static_cast<StepperController *>(context)->taskLoop();
}

void StepperController::taskLoop() {
  uint32_t lastSnapshotAtMs = 0;
  uint32_t lastSchedulerBreathAtMs = millis();

  for (;;) {
    // Service an already-due step before checking the motor queue. While motion
    // or an automated test is active, only urgent stop/release commands execute.
    // Normal motor commands remain FIFO and begin after the current move finishes.
    serviceMotion();
    const bool busy = motion_.active || test_.active;
    processQueuedCommands(busy ? 1 : 4, busy);
    serviceMotion();
    serviceTest();

    const uint32_t nowMs = millis();
    if (
      static_cast<uint32_t>(nowMs - lastSnapshotAtMs) >=
        AppConfig::MOTOR_STATE_SNAPSHOT_INTERVAL_MS ||
      (!motion_.active && !test_.active)
    ) {
      updatePublicState();
      lastSnapshotAtMs = nowMs;
    }

    if (motion_.active) {
      // This task is intentionally higher priority for step timing, but a pure
      // microsecond spin loop would keep it Ready forever and starve the core's
      // idle task. Briefly block once per interval so housekeeping and the task
      // watchdog can run. serviceMotion() resynchronizes after the pause and
      // never emits catch-up phases, so step count remains exact.
      if (static_cast<uint32_t>(nowMs - lastSchedulerBreathAtMs) >=
          AppConfig::MOTOR_SCHEDULER_BREATH_INTERVAL_MS) {
        vTaskDelay(1);
        lastSchedulerBreathAtMs = millis();
        continue;
      }

      const int32_t untilStepUs = static_cast<int32_t>(motion_.nextStepAtUs - micros());
      if (untilStepUs > 700) {
        delayMicroseconds(500);
      } else if (untilStepUs > 180) {
        delayMicroseconds(static_cast<uint32_t>(untilStepUs - 100));
      } else if (untilStepUs > 40) {
        delayMicroseconds(static_cast<uint32_t>(untilStepUs - 20));
      } else {
        taskYIELD();
      }
      continue;
    }

    lastSchedulerBreathAtMs = nowMs;
    if (test_.active) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));
    } else {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
    }
  }
}

void StepperController::processQueuedCommands(uint8_t budget, bool urgentOnly) {
  QueuedCommand queued{};
  uint8_t processed = 0;

  while (processed < budget) {
    if (urgentOnly) {
      QueuedCommand next{};
      if (xQueuePeek(commandQueue_, &next, 0) != pdTRUE ||
          !isUrgentCommand(String(next.line))) {
        break;
      }
    }

    if (xQueueReceive(commandQueue_, &queued, 0) != pdTRUE) {
      break;
    }
    if (!processCommand(String(queued.line), queued.source, String(queued.requestId))) {
      error(queued.source, String(queued.requestId), "malformed stepper command");
    }
    processed++;

    // Once a command starts motion or an automated test, leave every later
    // motor command queued. This is what makes command batches execute moves in
    // order instead of letting the last move in a batch overwrite the first.
    if (!urgentOnly && (motion_.active || test_.active)) {
      break;
    }
  }
}

bool StepperController::isUrgentCommand(const String &command) const {
  String work = command;
  work.trim();
  return work.equalsIgnoreCase("Stop") || work.equalsIgnoreCase("CoilsOff");
}

bool StepperController::recognizesCommand(const String &command) const {
  static const char *exact[] = {
    "Setup", "Stop", "CoilsOff", "GetMotorStats", "RunStartupTest", "FastWiperTest",
    "DebugSpinCW", "DebugSpinCCW", "NextStepOrder", "PrintStepOrder",
    "HoldTorque:1", "HoldTorque:0", "MoveFullCW", "MoveFullCCW", "MoveHalfCW", "MoveHalfCCW",
    "TestFullSpeedRev", "TestHalfSpeedRev", "TestMinSpeedRev",
    "TestFullSpeedRevCCW", "TestHalfSpeedRevCCW", "TestMinSpeedRevCCW",
    "StepMode:4", "StepMode:8"
  };

  for (const char *candidate : exact) {
    if (command.equalsIgnoreCase(candidate)) {
      return true;
    }
  }

  static const char *prefixes[] = {
    "StepOrder:", "SetRevSteps:", "SetMinRPM:", "SetMaxRPM:", "SetStartRPM:",
    "SetRampRPM:", "SetMinStepIntervalUs:", "RPM:", "DEG:"
  };
  for (const char *prefix : prefixes) {
    if (TextUtil::startsWithIgnoreCase(command, prefix)) {
      return true;
    }
  }
  return false;
}

// ADD STEPPER COMMANDS HERE after listing them in recognizesCommand(). Commands
// execute on the motor task, so this is the safe place to alter motion state.
bool StepperController::processCommand(
  const String &command,
  CommandSource source,
  const String &requestId
) {
  String work = command;
  work.trim();

  if (work.equalsIgnoreCase("Setup")) {
    publish(EventLevel::STATUS, source, requestId, "direct stepper driver ready");
    return true;
  }
  if (work.equalsIgnoreCase("Stop")) {
    stopMotion(source, requestId, true);
    return true;
  }
  if (work.equalsIgnoreCase("CoilsOff")) {
    stopMotion(source, requestId, false);
    releaseCoils();
    publish(EventLevel::STATUS, source, requestId, "[DONE] motor coils released");
    return true;
  }
  if (work.equalsIgnoreCase("GetMotorStats")) {
    updatePublicState();
    publishStatusReadback(source, requestId);
    return true;
  }
  if (work.equalsIgnoreCase("RunStartupTest")) {
    startTest(
      "startup test",
      AppConfig::STEPPER_BOOT_TEST_RPM,
      AppConfig::STEPPER_BOOT_TEST_DEGREES,
      AppConfig::STEPPER_BOOT_TEST_CYCLES,
      AppConfig::STEPPER_BOOT_TEST_DWELL_MS,
      AppConfig::STEPPER_BOOT_TEST_IMMEDIATE,
      source,
      requestId
    );
    return true;
  }
  if (work.equalsIgnoreCase("FastWiperTest")) {
    startTest("fast wiper test", AppConfig::STEPPER_FAST_TEST_RPM, 90.0f, 3, 80, false, source, requestId);
    return true;
  }
  if (work.equalsIgnoreCase("DebugSpinCW")) {
    startMove(AppConfig::STEPPER_DEBUG_RPM, effectiveStepsPerRevolution(), DIR_CW, source, requestId);
    return true;
  }
  if (work.equalsIgnoreCase("DebugSpinCCW")) {
    startMove(AppConfig::STEPPER_DEBUG_RPM, effectiveStepsPerRevolution(), DIR_CCW, source, requestId);
    return true;
  }
  if (work.equalsIgnoreCase("NextStepOrder")) {
    if (rejectTuningWhileMoving(source, requestId, "step order")) {
      return true;
    }
    currentOrderPreset_ = (currentOrderPreset_ + 1) % COMMON_ORDER_COUNT;
    setStepOrderPreset(currentOrderPreset_);
    publishConfigUpdated(source, requestId, "stepOrder=" + stepOrderString());
    return true;
  }
  if (TextUtil::startsWithIgnoreCase(work, "StepOrder:")) {
    if (rejectTuningWhileMoving(source, requestId, "step order")) {
      return true;
    }
    String digits = work.substring(10);
    if (setStepOrder(digits)) {
      publishConfigUpdated(source, requestId, "stepOrder=" + stepOrderString());
    } else {
      error(source, requestId, "StepOrder expects four unique digits from 1 to 4");
    }
    return true;
  }
  if (work.equalsIgnoreCase("PrintStepOrder")) {
    publishConfigUpdated(source, requestId, "stepOrder=" + stepOrderString());
    return true;
  }
  if (work.equalsIgnoreCase("HoldTorque:1")) {
    holdTorqueWhenIdle_ = true;
    publishConfigUpdated(source, requestId, "holdTorqueWhenIdle=enabled");
    return true;
  }
  if (work.equalsIgnoreCase("HoldTorque:0")) {
    holdTorqueWhenIdle_ = false;
    if (!motion_.active) {
      releaseCoils();
    }
    publishConfigUpdated(source, requestId, "holdTorqueWhenIdle=disabled idleCoils=released");
    return true;
  }

  if (work.equalsIgnoreCase("MoveFullCW")) {
    startMove(maxRpm_ * 0.9f, effectiveStepsPerRevolution(), DIR_CW, source, requestId);
    return true;
  }
  if (work.equalsIgnoreCase("MoveFullCCW")) {
    startMove(maxRpm_ * 0.9f, effectiveStepsPerRevolution(), DIR_CCW, source, requestId);
    return true;
  }
  if (work.equalsIgnoreCase("MoveHalfCW")) {
    startMove(maxRpm_ * 0.9f, effectiveStepsPerRevolution() / 2, DIR_CW, source, requestId);
    return true;
  }
  if (work.equalsIgnoreCase("MoveHalfCCW")) {
    startMove(maxRpm_ * 0.9f, effectiveStepsPerRevolution() / 2, DIR_CCW, source, requestId);
    return true;
  }

  if (work.equalsIgnoreCase("TestFullSpeedRev")) {
    startMove(maxRpm_, effectiveStepsPerRevolution(), DIR_CW, source, requestId);
    return true;
  }
  if (work.equalsIgnoreCase("TestHalfSpeedRev")) {
    startMove(maxRpm_ * 0.5f, effectiveStepsPerRevolution(), DIR_CW, source, requestId);
    return true;
  }
  if (work.equalsIgnoreCase("TestMinSpeedRev")) {
    startMove(minRpm_, effectiveStepsPerRevolution(), DIR_CW, source, requestId);
    return true;
  }
  if (work.equalsIgnoreCase("TestFullSpeedRevCCW")) {
    startMove(maxRpm_, effectiveStepsPerRevolution(), DIR_CCW, source, requestId);
    return true;
  }
  if (work.equalsIgnoreCase("TestHalfSpeedRevCCW")) {
    startMove(maxRpm_ * 0.5f, effectiveStepsPerRevolution(), DIR_CCW, source, requestId);
    return true;
  }
  if (work.equalsIgnoreCase("TestMinSpeedRevCCW")) {
    startMove(minRpm_, effectiveStepsPerRevolution(), DIR_CCW, source, requestId);
    return true;
  }

  if (TextUtil::startsWithIgnoreCase(work, "SetRevSteps:")) {
    if (rejectTuningWhileMoving(source, requestId, "steps per revolution")) {
      return true;
    }
    long value = 0;
    if (TextUtil::parseLong(work.substring(12), value) &&
        value > 0 && value <= AppConfig::MOTOR_MAX_BASE_STEPS_PER_REV) {
      stepsPerRevolution_ = value;
      publishConfigUpdated(source, requestId, "stepsPerRevolution=" + String(value));
    } else {
      error(
        source, requestId,
        "SetRevSteps expects 1 to " + String(AppConfig::MOTOR_MAX_BASE_STEPS_PER_REV)
      );
    }
    return true;
  }
  if (TextUtil::startsWithIgnoreCase(work, "SetMinRPM:")) {
    if (rejectTuningWhileMoving(source, requestId, "minimum RPM")) {
      return true;
    }
    float value = 0.0f;
    if (TextUtil::parseFloat(work.substring(10), value) &&
        value >= AppConfig::MOTOR_MIN_CONFIGURED_RPM && value <= maxRpm_ &&
        value <= AppConfig::MOTOR_MAX_CONFIGURED_RPM) {
      minRpm_ = value;
      publishConfigUpdated(source, requestId, "minRPM=" + String(value, 2));
    } else {
      error(
        source, requestId,
        "SetMinRPM must be " + String(AppConfig::MOTOR_MIN_CONFIGURED_RPM, 1) +
          " to maxRPM"
      );
    }
    return true;
  }
  if (TextUtil::startsWithIgnoreCase(work, "SetMaxRPM:")) {
    if (rejectTuningWhileMoving(source, requestId, "maximum RPM")) {
      return true;
    }
    float value = 0.0f;
    if (TextUtil::parseFloat(work.substring(10), value) &&
        value >= minRpm_ && value <= AppConfig::MOTOR_MAX_CONFIGURED_RPM) {
      maxRpm_ = value;
      publishConfigUpdated(source, requestId, "maxRPM=" + String(value, 2));
    } else {
      error(
        source, requestId,
        "SetMaxRPM must be at least minRPM and no greater than " +
          String(AppConfig::MOTOR_MAX_CONFIGURED_RPM, 0)
      );
    }
    return true;
  }
  if (TextUtil::startsWithIgnoreCase(work, "SetStartRPM:")) {
    if (rejectTuningWhileMoving(source, requestId, "start RPM")) {
      return true;
    }
    float value = 0.0f;
    if (TextUtil::parseFloat(work.substring(12), value) &&
        value >= AppConfig::MOTOR_MIN_CONFIGURED_RPM && value <= maxRpm_) {
      startRpm_ = value;
      publishConfigUpdated(source, requestId, "startRPM=" + String(value, 2));
    } else {
      error(
        source, requestId,
        "SetStartRPM must be " + String(AppConfig::MOTOR_MIN_CONFIGURED_RPM, 1) +
          " to maxRPM"
      );
    }
    return true;
  }
  if (TextUtil::startsWithIgnoreCase(work, "SetRampRPM:")) {
    if (rejectTuningWhileMoving(source, requestId, "RPM ramp")) {
      return true;
    }
    float value = 0.0f;
    if (TextUtil::parseFloat(work.substring(11), value) &&
        value > 0.0f && value <= AppConfig::MOTOR_MAX_RAMP_RPM_PER_SECOND) {
      rampRpmPerSecond_ = value;
      publishConfigUpdated(source, requestId, "rampRPMPerSecond=" + String(value, 2));
    } else {
      error(
        source, requestId,
        "SetRampRPM must be positive and no greater than " +
          String(AppConfig::MOTOR_MAX_RAMP_RPM_PER_SECOND, 0)
      );
    }
    return true;
  }
  if (TextUtil::startsWithIgnoreCase(work, "SetMinStepIntervalUs:")) {
    if (rejectTuningWhileMoving(source, requestId, "minimum step interval")) {
      return true;
    }
    long value = 0;
    if (TextUtil::parseLong(work.substring(21), value) && value > 0 &&
        static_cast<uint32_t>(value) <= AppConfig::MOTOR_MAX_STEP_INTERVAL_US) {
      minStepIntervalUs_ = static_cast<uint32_t>(value);
      publishConfigUpdated(source, requestId, "minStepIntervalUs=" + String(value));
    } else {
      error(
        source, requestId,
        "SetMinStepIntervalUs expects 1 to " +
          String(AppConfig::MOTOR_MAX_STEP_INTERVAL_US)
      );
    }
    return true;
  }

  if (work.equalsIgnoreCase("StepMode:4")) {
    if (rejectTuningWhileMoving(source, requestId, "step mode")) {
      return true;
    }
    stepMode_ = StepMode::FULL;
    phaseIndex_ &= 0x03;
    publishConfigUpdated(source, requestId, "stepMode=4");
    return true;
  }
  if (work.equalsIgnoreCase("StepMode:8")) {
    if (rejectTuningWhileMoving(source, requestId, "step mode")) {
      return true;
    }
    stepMode_ = StepMode::HALF;
    phaseIndex_ &= 0x07;
    publishConfigUpdated(source, requestId, "stepMode=8");
    return true;
  }

  long steps = 0;
  int direction = 0;

  float rpm = 0.0f;
  if (parseRpmMove(work, rpm, steps, direction)) {
    startMove(rpm, steps, direction, source, requestId);
    return true;
  }

  float degrees = 0.0f;
  if (parseDegreeMove(work, rpm, degrees, direction)) {
    startMove(rpm, degreesToSteps(degrees), direction, source, requestId);
    return true;
  }

  return false;
}

bool StepperController::rejectTuningWhileMoving(
  CommandSource source,
  const String &requestId,
  const char *settingName
) {
  if (!motion_.active && !test_.active) {
    return false;
  }
  error(
    source,
    requestId,
    "stop the motor before changing " + String(settingName)
  );
  return true;
}

void StepperController::serviceMotion() {
  if (!motion_.active) {
    return;
  }

  uint32_t nowUs = micros();
  if (motion_.currentRpm < motion_.targetRpm) {
    const float elapsedSeconds = static_cast<float>(nowUs - motion_.lastRampAtUs) / 1000000.0f;
    if (elapsedSeconds > 0.0f) {
      motion_.currentRpm += rampRpmPerSecond_ * elapsedSeconds;
      if (motion_.currentRpm > motion_.targetRpm) {
        motion_.currentRpm = motion_.targetRpm;
      }
      motion_.intervalUs = rpmToIntervalUs(motion_.currentRpm);
      motion_.lastRampAtUs = nowUs;
    }
  }

  if (static_cast<int32_t>(nowUs - motion_.nextStepAtUs) < 0) {
    return;
  }

  // Never emit a burst of catch-up steps after the task was delayed. Back-to-
  // back phase changes can make a geared stepper skip even though the average
  // requested RPM is valid. One physical step is emitted per deadline, then a
  // missed deadline is resynchronized from the current time.
  stepOnce(motion_.direction);
  motion_.remaining--;

  if (motion_.remaining <= 0) {
    finishMotion();
    return;
  }

  const uint32_t scheduledNext = motion_.nextStepAtUs + motion_.intervalUs;
  nowUs = micros();
  motion_.nextStepAtUs = static_cast<int32_t>(nowUs - scheduledNext) >= 0
    ? nowUs + motion_.intervalUs
    : scheduledNext;
}

void StepperController::serviceTest() {
  if (!test_.active || motion_.active) {
    return;
  }

  const uint32_t now = millis();
  if (test_.phase == TestPhase::DWELL_AFTER_CW) {
    if (static_cast<uint32_t>(now - test_.phaseAtMs) < test_.dwellMs) {
      return;
    }
    test_.phase = TestPhase::MOVING_CCW;
    startMove(
      test_.rpm,
      degreesToSteps(test_.degrees),
      DIR_CCW,
      test_.source,
      test_.requestId,
      test_.immediate,
      true
    );
    return;
  }

  if (test_.phase == TestPhase::DWELL_AFTER_CCW) {
    if (static_cast<uint32_t>(now - test_.phaseAtMs) < test_.dwellMs) {
      return;
    }

    test_.cycleIndex++;
    if (test_.cycleIndex >= test_.cyclesTotal) {
      test_.active = false;
      test_.phase = TestPhase::IDLE;
      publish(EventLevel::STATUS, test_.source, test_.requestId, test_.name + " complete");
      return;
    }

    test_.phase = TestPhase::MOVING_CW;
    startMove(
      test_.rpm,
      degreesToSteps(test_.degrees),
      DIR_CW,
      test_.source,
      test_.requestId,
      test_.immediate,
      true
    );
  }
}

void StepperController::finishMotion() {
  const CommandSource source = motion_.source;
  const String requestId = motion_.requestId;
  const long requested = motion_.requested;

  motion_.active = false;
  motion_.remaining = 0;
  if (!holdTorqueWhenIdle_) {
    releaseCoils();
  }

  publish(
    EventLevel::MOTION,
    source,
    requestId,
    "move complete steps=" + String(requested) + " position=" + String(positionSteps_)
  );

  if (!test_.active) {
    return;
  }
  if (test_.phase == TestPhase::MOVING_CW) {
    test_.phase = TestPhase::DWELL_AFTER_CW;
    test_.phaseAtMs = millis();
  } else if (test_.phase == TestPhase::MOVING_CCW) {
    test_.phase = TestPhase::DWELL_AFTER_CCW;
    test_.phaseAtMs = millis();
  }
}

void StepperController::startMove(
  float rpm,
  long steps,
  int direction,
  CommandSource source,
  const String &requestId,
  bool immediate,
  bool fromTest
) {
  if (!fromTest) {
    test_.active = false;
    test_.phase = TestPhase::IDLE;
  }
  if (steps <= 0 || steps > AppConfig::MOTOR_MAX_MOVE_STEPS) {
    error(
      source, requestId,
      "step count must be 1 to " + String(AppConfig::MOTOR_MAX_MOVE_STEPS)
    );
    return;
  }
  if (direction != DIR_CW && direction != DIR_CCW) {
    error(source, requestId, "direction must be 1 for CW or 2 for CCW");
    return;
  }

  rpm = clampFloat(rpm, 0.1f, maxRpm_);
  motion_.active = true;
  motion_.remaining = steps;
  motion_.requested = steps;
  motion_.direction = direction;
  motion_.targetRpm = rpm;
  motion_.currentRpm = immediate ? rpm : min(startRpm_, rpm);
  motion_.intervalUs = rpmToIntervalUs(motion_.currentRpm);
  motion_.source = source;
  motion_.requestId = requestId;

  const uint32_t nowUs = micros();
  motion_.nextStepAtUs = nowUs;
  motion_.lastRampAtUs = nowUs;

  publish(
    EventLevel::MOTION,
    source,
    requestId,
    "move started targetRPM=" + String(rpm, 2) +
      " startRPM=" + String(motion_.currentRpm, 2) +
      " steps=" + String(steps) +
      " direction=" + String(direction == DIR_CW ? "CW" : "CCW") +
      " intervalUs=" + String(motion_.intervalUs) +
      " ramp=" + String(immediate ? "off" : "on")
  );
}

void StepperController::startTest(
  const String &name,
  float rpm,
  float degrees,
  uint8_t cycles,
  uint16_t dwellMs,
  bool immediate,
  CommandSource source,
  const String &requestId
) {
  stopMotion(source, requestId, false);

  test_.active = true;
  test_.immediate = immediate;
  test_.rpm = clampFloat(rpm, 0.1f, maxRpm_);
  test_.degrees = degrees;
  test_.cyclesTotal = max<uint8_t>(cycles, 1);
  test_.cycleIndex = 0;
  test_.dwellMs = dwellMs;
  test_.phaseAtMs = millis();
  test_.phase = TestPhase::MOVING_CW;
  test_.source = source;
  test_.requestId = requestId;
  test_.name = name;

  publish(
    EventLevel::MOTION,
    source,
    requestId,
    name + " started rpm=" + String(test_.rpm, 2) +
      " degrees=" + String(degrees, 1) +
      " cycles=" + String(test_.cyclesTotal)
  );

  startMove(
    test_.rpm,
    degreesToSteps(test_.degrees),
    DIR_CW,
    source,
    requestId,
    immediate,
    true
  );
}

void StepperController::stopMotion(
  CommandSource source,
  const String &requestId,
  bool announce
) {
  const bool wasActive = motion_.active || test_.active;
  motion_.active = false;
  motion_.remaining = 0;
  test_.active = false;
  test_.phase = TestPhase::IDLE;

  if (!holdTorqueWhenIdle_) {
    releaseCoils();
  }
  if (announce) {
    publish(EventLevel::MOTION, source, requestId, wasActive ? "motion stopped" : "motor already idle");
  }
}

void StepperController::applyPhase(int phase) {
  const uint8_t (*table)[4] = stepMode_ == StepMode::FULL ? FULL_STEP_TABLE : HALF_STEP_TABLE;
  for (int logical = 0; logical < 4; logical++) {
    const int physical = logicalOrder_[logical];
    digitalWrite(pins_[physical], table[phase][logical] ? HIGH : LOW);
  }
}

void StepperController::releaseCoils() {
  for (int pin : pins_) {
    digitalWrite(pin, LOW);
  }
}

void StepperController::stepOnce(int direction) {
  const int phaseCount = static_cast<int>(stepMode_);
  if (direction == DIR_CW) {
    phaseIndex_ = (phaseIndex_ + 1) % phaseCount;
    if (positionSteps_ < LONG_MAX) {
      positionSteps_++;
    }
  } else {
    phaseIndex_ = (phaseIndex_ - 1 + phaseCount) % phaseCount;
    if (positionSteps_ > LONG_MIN) {
      positionSteps_--;
    }
  }
  applyPhase(phaseIndex_);
}

bool StepperController::parseRpmMove(
  const String &command,
  float &rpm,
  long &steps,
  int &direction
) const {
  String work = command;
  if (TextUtil::startsWithIgnoreCase(work, "RPM:")) {
    work.remove(0, 4);
  } else {
    return false;
  }
  const int c1 = work.indexOf(',');
  if (c1 < 0) {
    return false;
  }
  const int c2 = work.indexOf(',', static_cast<unsigned int>(c1) + 1U);
  if (c2 < 0) {
    return false;
  }
  const unsigned int u1 = static_cast<unsigned int>(c1);
  const unsigned int u2 = static_cast<unsigned int>(c2);
  long parsedDirection = 0;
  if (!TextUtil::parseFloat(work.substring(0U, u1), rpm) ||
      !TextUtil::parseLong(work.substring(u1 + 1U, u2), steps) ||
      !TextUtil::parseLong(work.substring(u2 + 1U), parsedDirection)) {
    return false;
  }
  direction = static_cast<int>(parsedDirection);
  return rpm > 0.0f && steps > 0 &&
    steps <= AppConfig::MOTOR_MAX_MOVE_STEPS &&
    (direction == DIR_CW || direction == DIR_CCW);
}

bool StepperController::parseDegreeMove(
  const String &command,
  float &rpm,
  float &degrees,
  int &direction
) const {
  String work = command;
  if (TextUtil::startsWithIgnoreCase(work, "DEG:")) {
    work.remove(0, 4);
  } else {
    return false;
  }
  const int c1 = work.indexOf(',');
  if (c1 < 0) {
    return false;
  }
  const int c2 = work.indexOf(',', static_cast<unsigned int>(c1) + 1U);
  if (c2 < 0) {
    return false;
  }
  const unsigned int u1 = static_cast<unsigned int>(c1);
  const unsigned int u2 = static_cast<unsigned int>(c2);
  long parsedDirection = 0;
  if (!TextUtil::parseFloat(work.substring(0U, u1), rpm) ||
      !TextUtil::parseFloat(work.substring(u1 + 1U, u2), degrees) ||
      !TextUtil::parseLong(work.substring(u2 + 1U), parsedDirection)) {
    return false;
  }
  direction = static_cast<int>(parsedDirection);
  const double estimatedSteps =
    static_cast<double>(effectiveStepsPerRevolution()) *
    static_cast<double>(degrees) / 360.0;
  return rpm > 0.0f && degrees > 0.0f &&
    estimatedSteps >= 1.0 &&
    estimatedSteps <= static_cast<double>(AppConfig::MOTOR_MAX_MOVE_STEPS) &&
    (direction == DIR_CW || direction == DIR_CCW);
}

bool StepperController::setStepOrder(String digits) {
  digits.trim();
  if (digits.length() != 4) {
    return false;
  }
  bool used[4] = {false, false, false, false};
  int parsed[4];
  for (size_t index = 0; index < 4U; index++) {
    const char value = digits[index];
    if (value < '1' || value > '4') {
      return false;
    }
    parsed[index] = value - '1';
    if (used[parsed[index]]) {
      return false;
    }
    used[parsed[index]] = true;
  }
  for (size_t index = 0; index < 4U; index++) {
    logicalOrder_[index] = parsed[index];
  }
  currentOrderPreset_ = -1;
  return true;
}

void StepperController::setStepOrderPreset(int preset) {
  preset = constrain(preset, 0, COMMON_ORDER_COUNT - 1);
  for (int index = 0; index < 4; index++) {
    logicalOrder_[index] = COMMON_ORDERS[preset][index];
  }
}

String StepperController::stepOrderString() const {
  String value;
  value.reserve(4);
  for (int index = 0; index < 4; index++) {
    value += String(logicalOrder_[index] + 1);
  }
  return value;
}

float StepperController::clampFloat(float value, float low, float high) const {
  if (value < low) {
    return low;
  }
  if (value > high) {
    return high;
  }
  return value;
}

float StepperController::percentToRpm(int percent) const {
  if (percent <= 1) {
    return minRpm_;
  }
  if (percent >= 100) {
    return maxRpm_;
  }
  return minRpm_ + (maxRpm_ - minRpm_) * static_cast<float>(percent - 1) / 99.0f;
}

uint32_t StepperController::rpmToIntervalUs(float rpm) const {
  rpm = max(rpm, 0.1f);
  const float interval = 60000000.0f /
    (rpm * static_cast<float>(effectiveStepsPerRevolution()));
  return max(static_cast<uint32_t>(interval + 0.5f), minStepIntervalUs_);
}

long StepperController::effectiveStepsPerRevolution() const {
  return stepMode_ == StepMode::HALF ? stepsPerRevolution_ * 2L : stepsPerRevolution_;
}

long StepperController::degreesToSteps(float degrees) const {
  const double calculated =
    static_cast<double>(effectiveStepsPerRevolution()) *
    static_cast<double>(degrees) / 360.0;
  if (calculated <= 1.0) {
    return 1L;
  }
  if (calculated >= static_cast<double>(AppConfig::MOTOR_MAX_MOVE_STEPS)) {
    return AppConfig::MOTOR_MAX_MOVE_STEPS;
  }
  return static_cast<long>(calculated + 0.5);
}

void StepperController::updatePublicState() {
  PublicState next{};
  next.position = positionSteps_;
  next.baseStepsPerRev = stepsPerRevolution_;
  next.effectiveStepsPerRev = effectiveStepsPerRevolution();
  next.minRpm = minRpm_;
  next.maxRpm = maxRpm_;
  next.startRpm = startRpm_;
  next.rampRpmPerSecond = rampRpmPerSecond_;
  next.minIntervalUs = minStepIntervalUs_;
  next.stepMode = static_cast<uint8_t>(stepMode_);
  // Keep the real-time task allocation-free during periodic snapshots.
  for (size_t index = 0; index < 4U; index++) {
    next.stepOrder[index] = static_cast<char>('1' + logicalOrder_[index]);
  }
  next.stepOrder[4] = '\0';
  next.holdTorque = holdTorqueWhenIdle_;
  next.moving = motion_.active;
  next.targetRpm = motion_.targetRpm;
  next.currentRpm = motion_.currentRpm;
  next.remaining = motion_.remaining;
  next.testActive = test_.active;
  next.queuedCommands = commandQueue_ != nullptr ? uxQueueMessagesWaiting(commandQueue_) : 0;

  portENTER_CRITICAL(&publicStateMux_);
  publicState_ = next;
  portEXIT_CRITICAL(&publicStateMux_);
}

StepperController::PublicState StepperController::snapshot() const {
  PublicState state{};
  portENTER_CRITICAL(&publicStateMux_);
  state = publicState_;
  portEXIT_CRITICAL(&publicStateMux_);
  return state;
}

StepperController::SettingsSnapshot StepperController::settingsSnapshot() const {
  const PublicState state = snapshot();
  SettingsSnapshot settings{};
  settings.stepsPerRevolution = state.baseStepsPerRev;
  settings.minRpm = state.minRpm;
  settings.maxRpm = state.maxRpm;
  settings.startRpm = state.startRpm;
  settings.rampRpmPerSecond = state.rampRpmPerSecond;
  settings.minIntervalUs = state.minIntervalUs;
  settings.stepMode = state.stepMode;
  settings.stepOrder = String(state.stepOrder);
  settings.holdTorque = state.holdTorque;
  return settings;
}

void StepperController::publish(
  EventLevel level,
  CommandSource source,
  const String &requestId,
  const String &message
) {
  events_.publish(level, message, source, requestId);
}

void StepperController::error(
  CommandSource source,
  const String &requestId,
  const String &message
) {
  events_.publish(EventLevel::ERROR, message, source, requestId);
}

#endif  // APP_STEPPER_DAC_ADDON_ENABLED
