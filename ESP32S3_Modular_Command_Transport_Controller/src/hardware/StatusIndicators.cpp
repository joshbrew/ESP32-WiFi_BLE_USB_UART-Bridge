#include "StatusIndicators.h"

#if APP_BOARD_ESP32_S3 && APP_STATUS_INDICATORS_ENABLED
#include <esp32-hal-rgb-led.h>
#endif

StatusIndicators::StatusIndicators()
  : activityPulseStartedMs_(0),
    seekingStartedAtMs_(0),
    previousSeekingConnection_(false),
    activityPulseArmed_(false),
    connected_(false),
    seekingConnection_(false),
    operationActive_(false),
    connectionOutputOn_(false),
    activityOutputOn_(false),
    selfTestPhase_(SelfTestPhase::IDLE),
    selfTestCycle_(0),
    selfTestConnectionCycles_(0),
    selfTestActivityCycles_(0),
    selfTestPhaseEndsAtMs_(0),
    selfTestCompletedPending_(false),
    rgbColor_(UINT32_MAX) {}

void StatusIndicators::begin(bool extendedBootSelfTest, bool suppressBootSelfTest) {
#if !APP_STATUS_INDICATORS_ENABLED
  (void)extendedBootSelfTest;
  (void)suppressBootSelfTest;
  Serial.println("[BOOT][LED] indicators compiled out");
  Serial.flush();
  return;
#else
  if (!AppConfig::STATUS_USE_RGB) {
    pinMode(AppConfig::PIN_STATUS_CONNECTION, OUTPUT);
    pinMode(AppConfig::PIN_STATUS_ACTIVITY, OUTPUT);
  }
  forceOutputs(false, false);

  Serial.printf(
    "[BOOT][LED] enabled rgb=%s rgbPin=%d connection=%d activity=%d bootTest=%s\n",
    AppConfig::STATUS_USE_RGB ? "yes" : "no",
    AppConfig::STATUS_USE_RGB ? AppConfig::PIN_STATUS_RGB : -1,
    AppConfig::PIN_STATUS_CONNECTION,
    AppConfig::PIN_STATUS_ACTIVITY,
    AppConfig::STATUS_LED_BOOT_SELF_TEST ? "on" : "off"
  );
  Serial.flush();

  if (suppressBootSelfTest) {
    Serial.println("[BOOT][LED] resume path: boot animation skipped");
    Serial.flush();
  } else if (AppConfig::STATUS_LED_BOOT_SELF_TEST) {
    runBootSelfTest(extendedBootSelfTest);
  }

  seekingStartedAtMs_ = millis();
  previousSeekingConnection_ = false;
#endif
}

void StatusIndicators::runBootSelfTest(bool extendedBootSelfTest) {
  if (extendedBootSelfTest) {
    Serial.printf(
      "[BOOT][LED] connection indicator: %u-pulse self-test\n",
      static_cast<unsigned>(AppConfig::STATUS_CONNECTION_BOOT_SELF_TEST_CYCLES)
    );
    for (
      uint8_t cycle = 0;
      cycle < AppConfig::STATUS_CONNECTION_BOOT_SELF_TEST_CYCLES;
      cycle++
    ) {
      setOutputs(true, false);
      logPinState("[BOOT][LED]", AppConfig::PIN_STATUS_CONNECTION, true);
      delay(AppConfig::STATUS_LED_BOOT_SELF_TEST_ON_MS);
      setOutputs(false, false);
      logPinState("[BOOT][LED]", AppConfig::PIN_STATUS_CONNECTION, false);
      delay(AppConfig::STATUS_LED_BOOT_SELF_TEST_OFF_MS);
    }

    Serial.printf(
      "[BOOT][LED] activity indicator: %u-pulse self-test\n",
      static_cast<unsigned>(AppConfig::STATUS_ACTIVITY_BOOT_SELF_TEST_CYCLES)
    );
    for (
      uint8_t cycle = 0;
      cycle < AppConfig::STATUS_ACTIVITY_BOOT_SELF_TEST_CYCLES;
      cycle++
    ) {
      setOutputs(false, true);
      logPinState("[BOOT][LED]", AppConfig::PIN_STATUS_ACTIVITY, true);
      delay(AppConfig::STATUS_LED_BOOT_SELF_TEST_ON_MS);
      setOutputs(false, false);
      logPinState("[BOOT][LED]", AppConfig::PIN_STATUS_ACTIVITY, false);
      delay(AppConfig::STATUS_LED_BOOT_SELF_TEST_OFF_MS);
    }
  } else {
    Serial.println("[BOOT][LED] cursory sequential connection then activity test");
    setOutputs(true, false);
    logPinState("[BOOT][LED]", AppConfig::PIN_STATUS_CONNECTION, true);
    delay(AppConfig::STATUS_LED_PRODUCTION_CURSORY_TEST_MS);
    setOutputs(false, false);
    logPinState("[BOOT][LED]", AppConfig::PIN_STATUS_CONNECTION, false);
    delay(AppConfig::STATUS_LED_BOOT_SELF_TEST_OFF_MS);
    setOutputs(false, true);
    logPinState("[BOOT][LED]", AppConfig::PIN_STATUS_ACTIVITY, true);
    delay(AppConfig::STATUS_LED_PRODUCTION_CURSORY_TEST_MS);
    setOutputs(false, false);
    logPinState("[BOOT][LED]", AppConfig::PIN_STATUS_ACTIVITY, false);
  }
  forceOutputs(false, false);
  Serial.flush();
}

void StatusIndicators::noteCommandReceived() {
  if (!AppConfig::ENABLE_STATUS_INDICATORS) {
    return;
  }
  activityPulseStartedMs_.store(millis(), std::memory_order_relaxed);
  activityPulseArmed_.store(true, std::memory_order_release);
}

void StatusIndicators::service(
  bool connected,
  bool seekingConnection,
  bool operationActive
) {
  connected_.store(connected, std::memory_order_relaxed);
  seekingConnection_.store(seekingConnection, std::memory_order_relaxed);
  operationActive_.store(operationActive, std::memory_order_relaxed);

  if (!AppConfig::ENABLE_STATUS_INDICATORS) {
    return;
  }

  const uint32_t nowMs = millis();
  if (selfTestPhase_ != SelfTestPhase::IDLE) {
    serviceSelfTest(nowMs);
    return;
  }

  if (seekingConnection && !previousSeekingConnection_) {
    seekingStartedAtMs_ = nowMs;
  }
  previousSeekingConnection_ = seekingConnection;

  const uint32_t seekingElapsedMs = nowMs - seekingStartedAtMs_;
  const bool blinkOn =
    seekingConnection &&
    ((seekingElapsedMs / AppConfig::CONNECTION_LED_BLINK_MS) & 1U) == 0U;
  const bool connectionOn = connected || blinkOn;

  bool pulseActive = false;
  if (activityPulseArmed_.load(std::memory_order_acquire)) {
    const uint32_t startedAtMs =
      activityPulseStartedMs_.load(std::memory_order_relaxed);
    pulseActive = static_cast<uint32_t>(nowMs - startedAtMs) <
      AppConfig::ACTIVITY_LED_PULSE_MS;
    if (!pulseActive) {
      activityPulseArmed_.store(false, std::memory_order_release);
    }
  }
  setOutputs(connectionOn, operationActive || pulseActive);
}

bool StatusIndicators::startSelfTest() {
  return startTest(
    AppConfig::STATUS_CONNECTION_BOOT_SELF_TEST_CYCLES,
    AppConfig::STATUS_ACTIVITY_BOOT_SELF_TEST_CYCLES
  );
}

bool StatusIndicators::startConnectionTest() {
  return startTest(AppConfig::STATUS_SINGLE_PIN_TEST_CYCLES, 0);
}

bool StatusIndicators::startActivityTest() {
  return startTest(0, AppConfig::STATUS_SINGLE_PIN_TEST_CYCLES);
}

bool StatusIndicators::startTest(uint8_t connectionCycles, uint8_t activityCycles) {
  if (
    !AppConfig::ENABLE_STATUS_INDICATORS ||
    selfTestPhase_ != SelfTestPhase::IDLE ||
    (connectionCycles == 0 && activityCycles == 0)
  ) {
    return false;
  }

  selfTestCycle_ = 0;
  selfTestConnectionCycles_ = connectionCycles;
  selfTestActivityCycles_ = activityCycles;
  selfTestCompletedPending_ = false;
  const uint32_t nowMs = millis();
  if (selfTestConnectionCycles_ > 0) {
    beginConnectionPulse(nowMs);
  } else {
    beginActivityPulse(nowMs);
  }
  return true;
}

bool StatusIndicators::consumeSelfTestCompleted() {
  if (!selfTestCompletedPending_) {
    return false;
  }
  selfTestCompletedPending_ = false;
  return true;
}

bool StatusIndicators::isSelfTestActive() const {
  return AppConfig::ENABLE_STATUS_INDICATORS && selfTestPhase_ != SelfTestPhase::IDLE;
}

void StatusIndicators::serviceSelfTest(uint32_t nowMs) {
  if (static_cast<int32_t>(nowMs - selfTestPhaseEndsAtMs_) < 0) {
    return;
  }

  switch (selfTestPhase_) {
    case SelfTestPhase::CONNECTION_ON:
      setOutputs(false, false);
      logPinState("[LEDTEST]", AppConfig::PIN_STATUS_CONNECTION, false);
      selfTestPhase_ = SelfTestPhase::CONNECTION_OFF;
      selfTestPhaseEndsAtMs_ = nowMs + AppConfig::STATUS_LED_BOOT_SELF_TEST_OFF_MS;
      break;

    case SelfTestPhase::CONNECTION_OFF:
      selfTestCycle_++;
      if (selfTestCycle_ < selfTestConnectionCycles_) {
        beginConnectionPulse(nowMs);
      } else if (selfTestActivityCycles_ > 0) {
        selfTestCycle_ = 0;
        beginActivityPulse(nowMs);
      } else {
        finishSelfTest();
      }
      break;

    case SelfTestPhase::ACTIVITY_ON:
      setOutputs(false, false);
      logPinState("[LEDTEST]", AppConfig::PIN_STATUS_ACTIVITY, false);
      selfTestPhase_ = SelfTestPhase::ACTIVITY_OFF;
      selfTestPhaseEndsAtMs_ = nowMs + AppConfig::STATUS_LED_BOOT_SELF_TEST_OFF_MS;
      break;

    case SelfTestPhase::ACTIVITY_OFF:
      selfTestCycle_++;
      if (selfTestCycle_ < selfTestActivityCycles_) {
        beginActivityPulse(nowMs);
      } else {
        finishSelfTest();
      }
      break;

    case SelfTestPhase::IDLE:
      break;
  }
}

void StatusIndicators::beginConnectionPulse(uint32_t nowMs) {
  setOutputs(true, false);
  logPinState("[LEDTEST]", AppConfig::PIN_STATUS_CONNECTION, true);
  selfTestPhase_ = SelfTestPhase::CONNECTION_ON;
  selfTestPhaseEndsAtMs_ = nowMs + AppConfig::STATUS_LED_BOOT_SELF_TEST_ON_MS;
}

void StatusIndicators::beginActivityPulse(uint32_t nowMs) {
  setOutputs(false, true);
  logPinState("[LEDTEST]", AppConfig::PIN_STATUS_ACTIVITY, true);
  selfTestPhase_ = SelfTestPhase::ACTIVITY_ON;
  selfTestPhaseEndsAtMs_ = nowMs + AppConfig::STATUS_LED_BOOT_SELF_TEST_ON_MS;
}

void StatusIndicators::finishSelfTest() {
  selfTestPhase_ = SelfTestPhase::IDLE;
  selfTestCycle_ = 0;
  selfTestConnectionCycles_ = 0;
  selfTestActivityCycles_ = 0;
  selfTestCompletedPending_ = true;
  forceOutputs(false, false);
  Serial.println("[LEDTEST] complete; indicators off");
}

String StatusIndicators::statusText() const {
  String text = "enabled=" + String(AppConfig::ENABLE_STATUS_INDICATORS ? "yes" : "no");
  text += " bootTest=" + String(AppConfig::STATUS_LED_BOOT_SELF_TEST ? "yes" : "no");
  text += " type=" + String(AppConfig::STATUS_USE_RGB ? "rgb" : "discrete");
  if (AppConfig::STATUS_USE_RGB) {
    text += " rgbPin=" + String(AppConfig::PIN_STATUS_RGB);
    text += " colors=green:connection,blue:command,amber:operation";
  }
  text += " connectionPin=" + String(AppConfig::PIN_STATUS_CONNECTION);
  text += " connectionOutput=" + String(
    connectionOutputOn_.load(std::memory_order_relaxed) ? "HIGH" : "LOW"
  );
  text += " connected=" + String(
    connected_.load(std::memory_order_relaxed) ? "yes" : "no"
  );
  text += " seeking=" + String(
    seekingConnection_.load(std::memory_order_relaxed) ? "yes" : "no"
  );
  text += " activityPin=" + String(AppConfig::PIN_STATUS_ACTIVITY);
  text += " activityOutput=" + String(
    activityOutputOn_.load(std::memory_order_relaxed) ? "HIGH" : "LOW"
  );
  text += " operationActive=" + String(
    operationActive_.load(std::memory_order_relaxed) ? "yes" : "no"
  );
  text += " commandPulse=" + String(
    activityPulseArmed_.load(std::memory_order_relaxed) ? "active" : "idle"
  );
  text += " selfTest=" + String(isSelfTestActive() ? "active" : "idle");
  text += " connectionMode=" + String(
    connected_.load(std::memory_order_relaxed)
      ? "solid-connected"
      : (seekingConnection_.load(std::memory_order_relaxed) ? "blink-seeking" : "off-disabled")
  );
  text += " polarity=" + String(
    AppConfig::STATUS_LED_ACTIVE_HIGH ? "active-high" : "active-low"
  );
  return text;
}

String StatusIndicators::stateJson() const {
  String json = "{";
  json += "\"enabled\":" + String(AppConfig::ENABLE_STATUS_INDICATORS ? "true" : "false");
  json += ",\"bootTestEnabled\":" + String(AppConfig::STATUS_LED_BOOT_SELF_TEST ? "true" : "false");
  json += ",\"rgb\":" + String(AppConfig::STATUS_USE_RGB ? "true" : "false");
  json += ",\"rgbPin\":" + String(AppConfig::STATUS_USE_RGB ? AppConfig::PIN_STATUS_RGB : -1);
  json += ",\"connectionPin\":" + String(AppConfig::PIN_STATUS_CONNECTION);
  json += ",\"activityPin\":" + String(AppConfig::PIN_STATUS_ACTIVITY);
  json += ",\"activeHigh\":" + String(
    AppConfig::STATUS_LED_ACTIVE_HIGH ? "true" : "false"
  );
  json += ",\"connected\":" + String(
    connected_.load(std::memory_order_relaxed) ? "true" : "false"
  );
  json += ",\"seeking\":" + String(
    seekingConnection_.load(std::memory_order_relaxed) ? "true" : "false"
  );
  json += ",\"operationActive\":" + String(
    operationActive_.load(std::memory_order_relaxed) ? "true" : "false"
  );
  json += ",\"commandPulse\":" + String(
    activityPulseArmed_.load(std::memory_order_relaxed) ? "true" : "false"
  );
  json += ",\"selfTestActive\":" + String(isSelfTestActive() ? "true" : "false");
  json += ",\"connectionOutputHigh\":" + String(
    connectionOutputOn_.load(std::memory_order_relaxed) ? "true" : "false"
  );
  json += ",\"activityOutputHigh\":" + String(
    activityOutputOn_.load(std::memory_order_relaxed) ? "true" : "false"
  );
  json += "}";
  return json;
}

void StatusIndicators::setOutputs(bool connectionOn, bool activityOn) {
  if (!AppConfig::ENABLE_STATUS_INDICATORS) {
    return;
  }
  if (AppConfig::STATUS_USE_RGB) {
    connectionOutputOn_.store(connectionOn, std::memory_order_relaxed);
    activityOutputOn_.store(activityOn, std::memory_order_relaxed);
    writeRgb(connectionOn, activityOn, false);
    return;
  }
  if (connectionOn != connectionOutputOn_.load(std::memory_order_relaxed)) {
    connectionOutputOn_.store(connectionOn, std::memory_order_relaxed);
    writePin(AppConfig::PIN_STATUS_CONNECTION, connectionOn);
  }
  if (activityOn != activityOutputOn_.load(std::memory_order_relaxed)) {
    activityOutputOn_.store(activityOn, std::memory_order_relaxed);
    writePin(AppConfig::PIN_STATUS_ACTIVITY, activityOn);
  }
}

void StatusIndicators::forceOutputs(bool connectionOn, bool activityOn) {
  connectionOutputOn_.store(connectionOn, std::memory_order_relaxed);
  activityOutputOn_.store(activityOn, std::memory_order_relaxed);
  if (!AppConfig::ENABLE_STATUS_INDICATORS) {
    return;
  }
  if (AppConfig::STATUS_USE_RGB) {
    writeRgb(connectionOn, activityOn, true);
    return;
  }
  writePin(AppConfig::PIN_STATUS_CONNECTION, connectionOn);
  writePin(AppConfig::PIN_STATUS_ACTIVITY, activityOn);
}

void StatusIndicators::writeRgb(bool connectionOn, bool activityOn, bool force) {
#if APP_BOARD_ESP32_S3 && APP_STATUS_INDICATORS_ENABLED
  const uint8_t brightness = AppConfig::STATUS_RGB_BRIGHTNESS;
  const bool operating = activityOn && operationActive_.load(std::memory_order_relaxed);
  // Activity takes priority so a command remains visible while connected.
  const uint8_t red = operating ? brightness : 0;
  const uint8_t green = operating ? brightness / 2 : (connectionOn && !activityOn ? brightness : 0);
  const uint8_t blue = activityOn && !operating ? brightness : 0;
  const uint32_t color = (uint32_t(red) << 16) | (uint32_t(green) << 8) | blue;
  // Send an addressable-LED frame only on a visible change, not every task tick.
  if (force || color != rgbColor_) {
    rgbLedWrite(AppConfig::PIN_STATUS_RGB, red, green, blue);
    rgbColor_ = color;
  }
#else
  (void)connectionOn;
  (void)activityOn;
  (void)force;
#endif
}

void StatusIndicators::writePin(int pin, bool on) {
#if APP_STATUS_INDICATORS_ENABLED
  const bool electricalHigh = AppConfig::STATUS_LED_ACTIVE_HIGH ? on : !on;
  digitalWrite(pin, electricalHigh ? HIGH : LOW);
#else
  (void)pin;
  (void)on;
#endif
}

void StatusIndicators::logPinState(const char *prefix, int pin, bool on) const {
  if (AppConfig::STATUS_USE_RGB) {
    Serial.printf("%s RGB GPIO%d %s\n", prefix, AppConfig::PIN_STATUS_RGB, on ? "on" : "off");
    return;
  }
  Serial.printf("%s GPIO%d %s\n", prefix, pin, on ? "HIGH" : "LOW");
}
