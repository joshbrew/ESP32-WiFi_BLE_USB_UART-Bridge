#include "DacController.h"

#include "../../util/TextUtil.h"

#if APP_STEPPER_DAC_ADDON_ENABLED

namespace {

constexpr const char *DAC_PREFERENCES_NAMESPACE = "step-dac";
constexpr TickType_t TIMER_COMMAND_WAIT_TICKS = 0;
constexpr uint32_t FALLBACK_SERVICE_INTERVAL_MS = 10;

struct SavedDacSettings {
  uint16_t referenceMv;
  uint16_t targetMv[AppConfig::DAC_CHANNEL_COUNT];
  bool bootEnabled[AppConfig::DAC_CHANNEL_COUNT];
  uint32_t maxOnMs[AppConfig::DAC_CHANNEL_COUNT];
};

}  // namespace

DacController::DacController(EventBus &events)
  : events_(events),
    channels_{},
    pins_{AppConfig::PIN_DAC_1, AppConfig::PIN_DIGITAL_OUTPUT},
    referenceMv_(AppConfig::DAC_DEFAULT_REFERENCE_MV),
    stateMutex_(nullptr),
    releaseTimers_{nullptr, nullptr},
    releaseTimerArmed_{false, false},
    timerContexts_{},
    lastFallbackServiceAtMs_(0) {
  setDefaults();
  for (uint8_t index = 0; index < AppConfig::DAC_CHANNEL_COUNT; index++) {
    timerContexts_[index] = {this, index};
  }
}

void DacController::begin() {
#if !defined(CONFIG_IDF_TARGET_ESP32)
#error This firmware requires the original ESP32 DAC implementation used on GPIO25.
#endif

  pinMode(AppConfig::PIN_DIGITAL_OUTPUT, OUTPUT);
  digitalWrite(AppConfig::PIN_DIGITAL_OUTPUT, LOW);

  stateMutex_ = xSemaphoreCreateMutex();
  if (stateMutex_ == nullptr) {
    events_.publish(
      EventLevel::ERROR,
      "could not create DAC state mutex; DAC timing will use polling fallback",
      CommandSource::INTERNAL
    );
  }

  static const char *timerNames[AppConfig::DAC_CHANNEL_COUNT] = {
    "dac1-release",
    "gpio26-release"
  };

  if (stateMutex_ != nullptr) {
    for (uint8_t index = 0; index < AppConfig::DAC_CHANNEL_COUNT; index++) {
      releaseTimers_[index] = xTimerCreate(
        timerNames[index],
        pdMS_TO_TICKS(AppConfig::DAC_TEST_DURATION_MS),
        pdFALSE,
        &timerContexts_[index],
        releaseTimerThunk
      );
      if (releaseTimers_[index] == nullptr) {
        events_.publish(
          EventLevel::WARNING,
          channelName(index) +
            " RTOS release timer unavailable; 10 ms polling fallback active",
          CommandSource::INTERNAL
        );
      }
    }
  }

  loadSettings(false, CommandSource::INTERNAL, String());

  if (lockState()) {
    const uint32_t now = millis();
    for (uint8_t index = 0; index < AppConfig::DAC_CHANNEL_COUNT; index++) {
      Channel &channel = channels_[index];
      channel.enabled = channel.bootEnabled;
      channel.enabledAtMs = channel.enabled ? now : 0;
      channel.oneShot = false;
      channel.oneShotEndsAtMs = 0;
      applyUnlocked(index);
      scheduleReleaseTimerUnlocked(index);
    }
    unlockState();
  }

  events_.publish(
    EventLevel::STATUS,
    "DAC1 GPIO25 analog and GPIO26 digital output ready; RTOS release timers armed",
    CommandSource::INTERNAL
  );
}

void DacController::service() {
  const uint32_t now = millis();
  if (static_cast<uint32_t>(now - lastFallbackServiceAtMs_) < FALLBACK_SERVICE_INTERVAL_MS) {
    return;
  }
  lastFallbackServiceAtMs_ = now;

  for (uint8_t index = 0; index < AppConfig::DAC_CHANNEL_COUNT; index++) {
    serviceChannelTimeout(index, false);
  }
}

// ADD DAC COMMANDS HERE. Keep parsing in this function, mutate channel state only
// while holding stateMutex_, and use applyUnlocked() for all hardware writes.
bool DacController::handleCommand(
  const String &command,
  CommandSource source,
  const String &requestId
) {
  String work = command;
  work.trim();

  if (work.equalsIgnoreCase("DACStatus") || work.equalsIgnoreCase("OutputStatus")) {
    publishConfigReadback(source, requestId, "status");
    return true;
  }

  if (work.equalsIgnoreCase("DACTest3S")) {
    for (uint8_t index = 0; index < AppConfig::DAC_CHANNEL_COUNT; index++) {
      startTest3s(index, source, requestId, false);
    }
    publish(EventLevel::STATUS, source, requestId, "DAC1 running at 500 mV and GPIO26 HIGH for 3000 ms");
    return true;
  }

  if (work.equalsIgnoreCase("DACAll:ON") || work.equalsIgnoreCase("OutputAll:ON")) {
    for (uint8_t index = 0; index < AppConfig::DAC_CHANNEL_COUNT; index++) {
      setEnabled(index, true, source, requestId, false);
    }
    publish(EventLevel::STATUS, source, requestId, "DAC1 enabled and GPIO26 set HIGH continuously");
    return true;
  }

  if (work.equalsIgnoreCase("DACAll:OFF") || work.equalsIgnoreCase("OutputAll:OFF")) {
    allOff(source, requestId, true);
    return true;
  }

  if (work.equalsIgnoreCase("DACSave")) {
    if (saveSettings()) {
      publish(EventLevel::STATUS, source, requestId, "DAC settings saved to NVS and verified");
      publishConfigReadback(source, requestId, "saved");
    } else {
      error(source, requestId, "could not save DAC settings");
    }
    return true;
  }

  if (work.equalsIgnoreCase("DACLoad")) {
    if (loadSettings(true, source, requestId) && lockState()) {
      const uint32_t now = millis();
      for (uint8_t index = 0; index < AppConfig::DAC_CHANNEL_COUNT; index++) {
        Channel &channel = channels_[index];
        channel.enabled = channel.bootEnabled;
        channel.enabledAtMs = channel.enabled ? now : 0;
        channel.oneShot = false;
        channel.oneShotEndsAtMs = 0;
        applyUnlocked(index);
        scheduleReleaseTimerUnlocked(index);
      }
      unlockState();
    }
    publishConfigReadback(source, requestId, "loaded");
    return true;
  }

  if (work.equalsIgnoreCase("DACDefaults")) {
    if (lockState()) {
      setDefaults();
      for (uint8_t index = 0; index < AppConfig::DAC_CHANNEL_COUNT; index++) {
        applyUnlocked(index);
        scheduleReleaseTimerUnlocked(index);
      }
      unlockState();
    }
    publish(EventLevel::STATUS, source, requestId, "volatile output defaults restored; DAC1 released and GPIO26 LOW");
    publishConfigReadback(source, requestId, "defaults restored");
    return true;
  }

  if (work.equalsIgnoreCase("DACErase")) {
    if (eraseSettings()) {
      publish(EventLevel::STATUS, source, requestId, "saved DAC settings erased; volatile settings unchanged");
      publishConfigReadback(source, requestId, "NVS erased");
    } else {
      error(source, requestId, "could not erase DAC settings");
    }
    return true;
  }

  if (TextUtil::startsWithIgnoreCase(work, "DACRefMV:")) {
    String value = work.substring(9);
    value.trim();
    uint32_t parsed = 0;
    if (!TextUtil::parseUnsigned32(value, parsed) || parsed < 2500 || parsed > 3600) {
      error(source, requestId, "DACRefMV must be between 2500 and 3600 mV");
      return true;
    }

    if (lockState()) {
      referenceMv_ = static_cast<uint16_t>(parsed);
      channels_[AppConfig::DAC_ANALOG_CHANNEL_INDEX].targetMv = min(
        channels_[AppConfig::DAC_ANALOG_CHANNEL_INDEX].targetMv,
        referenceMv_
      );
      applyUnlocked(AppConfig::DAC_ANALOG_CHANNEL_INDEX);
      unlockState();
    }
    publish(EventLevel::STATUS, source, requestId, "DAC reference calibration=" + String(parsed) + " mV");
    publishConfigReadback(source, requestId, "reference updated");
    return true;
  }

  unsigned int consumed = 0;
  const int parsedChannel = parseChannelPrefix(work, consumed);
  if (parsedChannel < 0) {
    return false;
  }

  const uint8_t index = static_cast<uint8_t>(parsedChannel);
  String action = work.substring(consumed);
  if (action.startsWith(":")) {
    action.remove(0, 1);
  }
  action.trim();

  if (
    action.equalsIgnoreCase("TEST3S")
  ) {
    startTest3s(index, source, requestId, true);
    return true;
  }

  bool enabled = false;
  if (TextUtil::parseOnOff(action, enabled)) {
    setEnabled(index, enabled, source, requestId, true);
    return true;
  }

  if (TextUtil::startsWithIgnoreCase(action, "MV:")) {
    if (index == AppConfig::DIGITAL_OUTPUT_CHANNEL_INDEX) {
      error(source, requestId, "GPIO26 is digital-only; use GPIO26:ON or GPIO26:OFF");
      return true;
    }
    String value = action.substring(3);
    value.trim();
    uint32_t parsed = 0;

    uint16_t referenceMv = AppConfig::DAC_DEFAULT_REFERENCE_MV;
    if (lockState()) {
      referenceMv = referenceMv_;
      unlockState();
    }
    if (!TextUtil::parseUnsigned32(value, parsed) || parsed > referenceMv) {
      error(source, requestId, channelName(index) + " millivolts must be an integer from 0 to " + String(referenceMv));
      return true;
    }
    setMillivolts(index, static_cast<uint16_t>(parsed), source, requestId, true);
    publishConfigReadback(source, requestId, channelName(index) + " target updated");
    return true;
  }

  if (TextUtil::startsWithIgnoreCase(action, "BOOT:")) {
    String value = action.substring(5);
    if (!TextUtil::parseOnOff(value, enabled)) {
      error(source, requestId, channelName(index) + ":BOOT expects ON or OFF");
      return true;
    }
    if (lockState()) {
      channels_[index].bootEnabled = enabled;
      unlockState();
    }
    publish(
      EventLevel::STATUS,
      source,
      requestId,
      channelName(index) + " boot state=" + TextUtil::boolWord(enabled) + "; use DACSave to persist"
    );
    publishConfigReadback(source, requestId, channelName(index) + " boot state updated");
    return true;
  }

  if (
    TextUtil::startsWithIgnoreCase(action, "TIMEOUTMS:") ||
    TextUtil::startsWithIgnoreCase(action, "TIMEOUTSEC:")
  ) {
    const bool secondsMode = TextUtil::startsWithIgnoreCase(action, "TIMEOUTSEC:");
    String value = action.substring(secondsMode ? 11 : 10);
    value.trim();
    uint32_t parsed = 0;
    if (!TextUtil::parseUnsigned32(value, parsed)) {
      error(source, requestId, channelName(index) + " timeout must be a nonnegative integer");
      return true;
    }
    if (secondsMode && parsed > 4294967UL) {
      error(source, requestId, channelName(index) + " timeout is too large");
      return true;
    }
    const uint32_t timeoutMs = secondsMode ? parsed * 1000UL : parsed;

    if (lockState()) {
      Channel &channel = channels_[index];
      channel.maxOnMs = timeoutMs;
      if (channel.enabled && !channel.oneShot) {
        channel.enabledAtMs = millis();
      }
      scheduleReleaseTimerUnlocked(index);
      unlockState();
    }
    publish(
      EventLevel::STATUS,
      source,
      requestId,
      channelName(index) + " timeoutMs=" + String(timeoutMs) +
        (timeoutMs == 0 ? " unlimited" : "")
    );
    publishConfigReadback(source, requestId, channelName(index) + " timeout updated");
    return true;
  }

  error(source, requestId, "unknown " + channelName(index) + " command");
  return true;
}

void DacController::allOff(CommandSource source, const String &requestId, bool announce) {
  if (lockState()) {
    for (uint8_t index = 0; index < AppConfig::DAC_CHANNEL_COUNT; index++) {
      Channel &channel = channels_[index];
      channel.enabled = false;
      channel.enabledAtMs = 0;
      cancelOneShotUnlocked(index);
      applyUnlocked(index);
      stopReleaseTimerUnlocked(index);
    }
    unlockState();
  }

  if (announce) {
    publish(EventLevel::STATUS, source, requestId, "DAC1 released and GPIO26 set LOW");
  }
}


bool DacController::anyOutputActive() const {
  bool active = false;
  if (!lockState(pdMS_TO_TICKS(2))) {
    return false;
  }
  for (uint8_t index = 0; index < AppConfig::DAC_CHANNEL_COUNT; index++) {
    if (channels_[index].enabled) {
      active = true;
      break;
    }
  }
  unlockState();
  return active;
}

// The activity LED represents work with a finite lifetime. A deliberately
// latched DAC output is a stable output state, not a command that is still
// executing, so it must not hold the activity indicator high forever.
bool DacController::hasTimedOperationActive() const {
  bool active = false;
  if (!lockState(pdMS_TO_TICKS(2))) {
    return false;
  }
  for (uint8_t index = 0; index < AppConfig::DAC_CHANNEL_COUNT; index++) {
    const Channel &channel = channels_[index];
    if (channel.enabled && (channel.oneShot || channel.maxOnMs > 0)) {
      active = true;
      break;
    }
  }
  unlockState();
  return active;
}

String DacController::statusText() const {
  Channel snapshot[AppConfig::DAC_CHANNEL_COUNT]{};
  uint16_t referenceMv = AppConfig::DAC_DEFAULT_REFERENCE_MV;
  const uint32_t now = millis();

  if (lockState()) {
    referenceMv = referenceMv_;
    for (uint8_t index = 0; index < AppConfig::DAC_CHANNEL_COUNT; index++) {
      snapshot[index] = channels_[index];
    }
    unlockState();
  }

  String text = "dacReferenceMv=" + String(referenceMv);
  for (uint8_t index = 0; index < AppConfig::DAC_CHANNEL_COUNT; index++) {
    const Channel &channel = snapshot[index];
    const uint32_t remaining = channel.oneShot &&
      static_cast<int32_t>(channel.oneShotEndsAtMs - now) > 0
        ? static_cast<uint32_t>(channel.oneShotEndsAtMs - now)
        : 0;

    text += " " + channelName(index) + "Pin=" + String(pins_[index]);
    text += " enabled=" + TextUtil::boolWord(channel.enabled);
    if (index == AppConfig::DIGITAL_OUTPUT_CHANNEL_INDEX) {
      text += " mode=digital level=";
      text += channel.enabled ? "HIGH" : "LOW";
    } else {
      const uint16_t outputMv = channel.oneShot ? AppConfig::DAC_DEFAULT_MV : channel.targetMv;
      const uint32_t rounded = referenceMv == 0
        ? 0
        : (static_cast<uint32_t>(outputMv) * 255U + static_cast<uint32_t>(referenceMv) / 2U) /
          static_cast<uint32_t>(referenceMv);
      text += " mode=dac targetMv=" + String(channel.targetMv);
      text += " outputMv=" + String(channel.enabled ? outputMv : 0);
      text += " code=" + String(min<uint32_t>(rounded, 255U));
    }
    text += " boot=" + TextUtil::boolWord(channel.bootEnabled);
    text += " timeoutMs=" + String(channel.maxOnMs);
    text += " oneShot=" + TextUtil::boolWord(channel.oneShot);
    text += " remainingMs=" + String(remaining);
  }
  return text;
}

String DacController::stateJson() const {
  Channel snapshot[AppConfig::DAC_CHANNEL_COUNT]{};
  uint16_t referenceMv = AppConfig::DAC_DEFAULT_REFERENCE_MV;
  const uint32_t now = millis();

  if (lockState()) {
    referenceMv = referenceMv_;
    for (uint8_t index = 0; index < AppConfig::DAC_CHANNEL_COUNT; index++) {
      snapshot[index] = channels_[index];
    }
    unlockState();
  }

  String json = "{";
  json += "\"referenceMv\":" + String(referenceMv);
  json += ",\"channels\":[";

  for (uint8_t index = 0; index < AppConfig::DAC_CHANNEL_COUNT; index++) {
    if (index > 0) {
      json += ',';
    }
    const Channel &channel = snapshot[index];
    const bool digital = index == AppConfig::DIGITAL_OUTPUT_CHANNEL_INDEX;
    const uint32_t remaining = channel.oneShot &&
      static_cast<int32_t>(channel.oneShotEndsAtMs - now) > 0
        ? static_cast<uint32_t>(channel.oneShotEndsAtMs - now)
        : 0;
    const uint16_t outputMv = digital
      ? 0
      : (channel.oneShot ? AppConfig::DAC_DEFAULT_MV : channel.targetMv);
    const uint32_t rounded = digital || referenceMv == 0
      ? 0
      : (static_cast<uint32_t>(outputMv) * 255U + static_cast<uint32_t>(referenceMv) / 2U) /
        static_cast<uint32_t>(referenceMv);

    json += "{\"index\":" + String(index + 1);
    json += ",\"pin\":" + String(pins_[index]);
    json += ",\"type\":\"" + String(digital ? "digital" : "dac") + "\"";
    json += ",\"enabled\":" + TextUtil::jsonBool(channel.enabled);
    json += ",\"high\":" + TextUtil::jsonBool(digital && channel.enabled);
    json += ",\"targetMv\":" + String(digital ? 0 : channel.targetMv);
    json += ",\"outputMv\":" + String(channel.enabled ? outputMv : 0);
    json += ",\"code\":" + String(digital ? (channel.enabled ? 1 : 0) : min<uint32_t>(rounded, 255U));
    json += ",\"bootEnabled\":" + TextUtil::jsonBool(channel.bootEnabled);
    json += ",\"timeoutMs\":" + String(channel.maxOnMs);
    json += ",\"oneShot\":" + TextUtil::jsonBool(channel.oneShot);
    json += ",\"oneShotRemainingMs\":" + String(remaining);
    json += "}";
  }

  json += "]}";
  return json;
}

String DacController::webStateJson() const {
  Channel snapshot[AppConfig::DAC_CHANNEL_COUNT]{};
  if (lockState()) {
    for (uint8_t index = 0; index < AppConfig::DAC_CHANNEL_COUNT; index++) {
      snapshot[index] = channels_[index];
    }
    unlockState();
  }

  String json;
  json.reserve(224);
  json = "{\"channels\":[";
  for (uint8_t index = 0; index < AppConfig::DAC_CHANNEL_COUNT; index++) {
    if (index > 0) {
      json += ',';
    }
    const Channel &channel = snapshot[index];
    const bool digital = index == AppConfig::DIGITAL_OUTPUT_CHANNEL_INDEX;
    const uint16_t outputMv = digital
      ? 0
      : (channel.oneShot ? AppConfig::DAC_DEFAULT_MV : channel.targetMv);
    json += "{\"type\":\"" + String(digital ? "digital" : "dac") + "\"";
    json += ",\"enabled\":" + TextUtil::jsonBool(channel.enabled);
    json += ",\"high\":" + TextUtil::jsonBool(digital && channel.enabled);
    json += ",\"outputMv\":" + String(channel.enabled ? outputMv : 0);
    json += ",\"oneShot\":" + TextUtil::jsonBool(channel.oneShot);
    json += "}";
  }
  json += "]}";
  return json;
}

DacController::SettingsSnapshot DacController::settingsSnapshot() const {
  SettingsSnapshot settings{};
  settings.referenceMv = AppConfig::DAC_DEFAULT_REFERENCE_MV;
  if (!lockState()) {
    return settings;
  }
  settings.referenceMv = referenceMv_;
  for (uint8_t index = 0; index < AppConfig::DAC_CHANNEL_COUNT; index++) {
    settings.targetMv[index] = channels_[index].targetMv;
    settings.bootEnabled[index] = channels_[index].bootEnabled;
    settings.maxOnMs[index] = channels_[index].maxOnMs;
  }
  unlockState();
  return settings;
}

void DacController::releaseTimerThunk(TimerHandle_t timer) {
  if (timer == nullptr) {
    return;
  }
  TimerContext *context = static_cast<TimerContext *>(pvTimerGetTimerID(timer));
  if (context == nullptr || context->owner == nullptr) {
    return;
  }
  context->owner->serviceChannelTimeout(context->index, true);
}

void DacController::serviceChannelTimeout(uint8_t index, bool timerCallback) {
  if (index >= AppConfig::DAC_CHANNEL_COUNT || !lockState(0)) {
    return;
  }

  if (timerCallback) {
    // A one-shot software timer is no longer active when its callback runs.
    // Track this explicitly so OFF-at-boot never sends an unnecessary timer
    // command during startup and rearming remains accurate.
    releaseTimerArmed_[index] = false;
  }

  Channel &channel = channels_[index];
  const uint32_t now = millis();
  String message;
  EventLevel level = EventLevel::STATUS;
  bool released = false;

  if (channel.oneShot) {
    if (static_cast<int32_t>(now - channel.oneShotEndsAtMs) >= 0) {
      channel.oneShot = false;
      channel.oneShotEndsAtMs = 0;
      channel.enabled = false;
      channel.enabledAtMs = 0;
      applyUnlocked(index);
      stopReleaseTimerUnlocked(index);
      released = true;
      message = index == AppConfig::DIGITAL_OUTPUT_CHANNEL_INDEX
        ? "GPIO26 3-second HIGH test complete; output LOW"
        : "DAC1 500 mV 3-second test complete; output released";
    } else if (timerCallback) {
      scheduleReleaseTimerUnlocked(index);
    }
  } else if (channel.enabled && channel.maxOnMs > 0) {
    const uint32_t elapsed = static_cast<uint32_t>(now - channel.enabledAtMs);
    if (elapsed >= channel.maxOnMs) {
      channel.enabled = false;
      channel.enabledAtMs = 0;
      applyUnlocked(index);
      stopReleaseTimerUnlocked(index);
      released = true;
      level = EventLevel::WARNING;
      message = index == AppConfig::DIGITAL_OUTPUT_CHANNEL_INDEX
        ? "GPIO26 safety timeout expired; output LOW"
        : "DAC1 safety timeout expired; output released";
    } else if (timerCallback) {
      scheduleReleaseTimerUnlocked(index);
    }
  } else if (timerCallback) {
    stopReleaseTimerUnlocked(index);
  }

  unlockState();

  if (released) {
    publish(level, CommandSource::INTERNAL, String(), message);
  }
}

void DacController::setDefaults() {
  referenceMv_ = AppConfig::DAC_DEFAULT_REFERENCE_MV;
  for (uint8_t index = 0; index < AppConfig::DAC_CHANNEL_COUNT; index++) {
    channels_[index] = {
      AppConfig::DAC_DEFAULT_MV,
      false,
      false,
      0,
      0,
      false,
      0
    };
  }
}

bool DacController::loadSettings(
  bool announce,
  CommandSource source,
  const String &requestId
) {
  Preferences preferences;
  if (!preferences.begin(DAC_PREFERENCES_NAMESPACE, true)) {
    if (announce) {
      error(source, requestId, "could not open DAC settings storage");
    }
    return false;
  }

  const bool valid = preferences.getBool("valid", false);
  SavedDacSettings loaded{};
  loaded.referenceMv = AppConfig::DAC_DEFAULT_REFERENCE_MV;
  for (uint8_t index = 0; index < AppConfig::DAC_CHANNEL_COUNT; index++) {
    loaded.targetMv[index] = AppConfig::DAC_DEFAULT_MV;
    loaded.bootEnabled[index] = false;
    loaded.maxOnMs[index] = 0;
  }

  if (valid) {
    loaded.referenceMv = constrain(
      preferences.getUShort("refmv", loaded.referenceMv),
      static_cast<uint16_t>(2500),
      static_cast<uint16_t>(3600)
    );
    for (uint8_t index = 0; index < AppConfig::DAC_CHANNEL_COUNT; index++) {
      const String suffix = String(index + 1);
      loaded.targetMv[index] = min(
        preferences.getUShort(("mv" + suffix).c_str(), AppConfig::DAC_DEFAULT_MV),
        loaded.referenceMv
      );
      loaded.bootEnabled[index] = preferences.getBool(("boot" + suffix).c_str(), false);
      loaded.maxOnMs[index] = preferences.getULong(("tout" + suffix).c_str(), 0);
    }
  }
  preferences.end();

  if (valid && lockState()) {
    referenceMv_ = loaded.referenceMv;
    for (uint8_t index = 0; index < AppConfig::DAC_CHANNEL_COUNT; index++) {
      channels_[index].targetMv = loaded.targetMv[index];
      channels_[index].bootEnabled = loaded.bootEnabled[index];
      channels_[index].maxOnMs = loaded.maxOnMs[index];
      channels_[index].enabled = false;
      channels_[index].enabledAtMs = 0;
      channels_[index].oneShot = false;
      channels_[index].oneShotEndsAtMs = 0;
      stopReleaseTimerUnlocked(index);
    }
    unlockState();
  }

  if (announce) {
    publish(
      valid ? EventLevel::STATUS : EventLevel::WARNING,
      source,
      requestId,
      valid ? "saved DAC settings loaded" : "no saved DAC settings found"
    );
  }
  return valid;
}

bool DacController::saveSettings() {
  SavedDacSettings saved{};
  if (lockState()) {
    saved.referenceMv = referenceMv_;
    for (uint8_t index = 0; index < AppConfig::DAC_CHANNEL_COUNT; index++) {
      saved.targetMv[index] = channels_[index].targetMv;
      saved.bootEnabled[index] = channels_[index].bootEnabled;
      saved.maxOnMs[index] = channels_[index].maxOnMs;
    }
    unlockState();
  } else {
    return false;
  }

  Preferences preferences;
  if (!preferences.begin(DAC_PREFERENCES_NAMESPACE, false)) {
    return false;
  }

  bool ok = preferences.putUShort("refmv", saved.referenceMv) > 0;
  for (uint8_t index = 0; index < AppConfig::DAC_CHANNEL_COUNT; index++) {
    const String suffix = String(index + 1);
    ok = preferences.putUShort(("mv" + suffix).c_str(), saved.targetMv[index]) > 0 && ok;
    ok = preferences.putBool(("boot" + suffix).c_str(), saved.bootEnabled[index]) > 0 && ok;
    ok = preferences.putULong(("tout" + suffix).c_str(), saved.maxOnMs[index]) > 0 && ok;
  }
  ok = preferences.putBool("valid", true) > 0 && ok;
  preferences.end();
  if (!ok || !preferences.begin(DAC_PREFERENCES_NAMESPACE, true)) {
    return false;
  }

  bool verified =
    preferences.getBool("valid", false) &&
    preferences.getUShort("refmv", 0) == saved.referenceMv;
  for (uint8_t index = 0; index < AppConfig::DAC_CHANNEL_COUNT; index++) {
    const String suffix = String(index + 1);
    verified =
      preferences.getUShort(("mv" + suffix).c_str(), 0) == saved.targetMv[index] &&
      preferences.getBool(("boot" + suffix).c_str(), !saved.bootEnabled[index]) == saved.bootEnabled[index] &&
      preferences.getULong(("tout" + suffix).c_str(), 0xFFFFFFFFUL) == saved.maxOnMs[index] &&
      verified;
  }
  preferences.end();
  return verified;
}

bool DacController::eraseSettings() {
  Preferences preferences;
  if (!preferences.begin(DAC_PREFERENCES_NAMESPACE, false)) {
    return false;
  }
  const bool ok = preferences.clear();
  preferences.end();
  return ok;
}

bool DacController::setEnabled(
  uint8_t index,
  bool enabled,
  CommandSource source,
  const String &requestId,
  bool announce
) {
  if (index >= AppConfig::DAC_CHANNEL_COUNT) {
    error(source, requestId, "output channel out of range");
    return false;
  }

  uint16_t targetMv = 0;
  if (lockState()) {
    Channel &channel = channels_[index];
    cancelOneShotUnlocked(index);
    channel.enabled = enabled;
    channel.enabledAtMs = enabled ? millis() : 0;
    applyUnlocked(index);
    scheduleReleaseTimerUnlocked(index);
    targetMv = channel.targetMv;
    unlockState();
  } else {
    error(source, requestId, "output state unavailable");
    return false;
  }

  if (announce) {
    const String stateMessage = index == AppConfig::DIGITAL_OUTPUT_CHANNEL_INDEX
      ? channelName(index) + (enabled ? " set HIGH continuously" : " set LOW")
      : channelName(index) + (enabled
          ? " enabled continuously at approximately " + String(targetMv) + " mV"
          : " released");
    publish(EventLevel::STATUS, source, requestId, stateMessage);
  }
  return true;
}

bool DacController::setMillivolts(
  uint8_t index,
  uint16_t millivolts,
  CommandSource source,
  const String &requestId,
  bool announce
) {
  if (index >= AppConfig::DAC_CHANNEL_COUNT) {
    error(source, requestId, "output channel out of range");
    return false;
  }
  if (index == AppConfig::DIGITAL_OUTPUT_CHANNEL_INDEX) {
    error(source, requestId, "GPIO26 is digital-only; millivolt control is unavailable");
    return false;
  }

  uint8_t code = 0;
  if (lockState()) {
    Channel &channel = channels_[index];
    const bool wasOneShot = channel.oneShot;
    cancelOneShotUnlocked(index);
    if (wasOneShot) {
      channel.enabled = false;
      channel.enabledAtMs = 0;
    }
    channel.targetMv = millivolts;
    if (channel.enabled) {
      channel.enabledAtMs = millis();
    }
    applyUnlocked(index);
    scheduleReleaseTimerUnlocked(index);
    code = millivoltsToCodeUnlocked(millivolts);
    unlockState();
  } else {
    error(source, requestId, "output state unavailable");
    return false;
  }

  if (announce) {
    publish(
      EventLevel::STATUS,
      source,
      requestId,
      channelName(index) + " targetMv=" + String(millivolts) + " code=" + String(code)
    );
  }
  return true;
}

bool DacController::startTest3s(
  uint8_t index,
  CommandSource source,
  const String &requestId,
  bool announce
) {
  if (index >= AppConfig::DAC_CHANNEL_COUNT) {
    error(source, requestId, "output channel out of range");
    return false;
  }

  if (lockState()) {
    Channel &channel = channels_[index];
    // TEST3S drives DAC1 at the fixed test voltage or GPIO26 HIGH without
    // changing persistent configuration. The timer always returns the selected
    // output to its inactive state.
    channel.enabled = true;
    channel.enabledAtMs = millis();
    channel.oneShot = true;
    channel.oneShotEndsAtMs = channel.enabledAtMs + AppConfig::DAC_TEST_DURATION_MS;
    applyUnlocked(index);
    scheduleReleaseTimerUnlocked(index);
    unlockState();
  } else {
    error(source, requestId, "output state unavailable");
    return false;
  }

  if (announce) {
    publish(
      EventLevel::STATUS,
      source,
      requestId,
      index == AppConfig::DIGITAL_OUTPUT_CHANNEL_INDEX
        ? "GPIO26 HIGH for 3000 ms"
        : "DAC1 running at 500 mV for 3000 ms"
    );
  }
  return true;
}

bool DacController::lockState(TickType_t timeout) const {
  return stateMutex_ == nullptr || xSemaphoreTake(stateMutex_, timeout) == pdTRUE;
}

void DacController::unlockState() const {
  if (stateMutex_ != nullptr) {
    xSemaphoreGive(stateMutex_);
  }
}

void DacController::cancelOneShotUnlocked(uint8_t index) {
  channels_[index].oneShot = false;
  channels_[index].oneShotEndsAtMs = 0;
}

void DacController::applyUnlocked(uint8_t index) {
  if (index == AppConfig::DIGITAL_OUTPUT_CHANNEL_INDEX) {
    digitalWrite(pins_[index], channels_[index].enabled ? HIGH : LOW);
    return;
  }

  if (channels_[index].enabled) {
    dacWrite(
      static_cast<uint8_t>(pins_[index]),
      millivoltsToCodeUnlocked(outputMillivoltsUnlocked(index))
    );
  } else {
    dacDisable(static_cast<uint8_t>(pins_[index]));
  }
}

void DacController::scheduleReleaseTimerUnlocked(uint8_t index) {
  if (index >= AppConfig::DAC_CHANNEL_COUNT || releaseTimers_[index] == nullptr) {
    return;
  }

  const Channel &channel = channels_[index];
  const uint32_t now = millis();
  uint32_t remainingMs = 0;
  if (channel.oneShot) {
    const int32_t remainingSigned = static_cast<int32_t>(channel.oneShotEndsAtMs - now);
    remainingMs = remainingSigned > 0 ? static_cast<uint32_t>(remainingSigned) : 1;
  } else if (channel.enabled && channel.maxOnMs > 0) {
    const uint32_t elapsed = static_cast<uint32_t>(now - channel.enabledAtMs);
    remainingMs = elapsed < channel.maxOnMs ? channel.maxOnMs - elapsed : 1;
  } else {
    stopReleaseTimerUnlocked(index);
    return;
  }

  // Long safety timeouts are rechecked in bounded chunks. This avoids overflow
  // inside pdMS_TO_TICKS() on cores that perform its math with 32-bit values.
  const uint32_t armMs = min(remainingMs, AppConfig::DAC_TIMER_MAX_ARM_MS);
  const TickType_t ticks = max<TickType_t>(pdMS_TO_TICKS(armMs), 1);
  if (xTimerChangePeriod(releaseTimers_[index], ticks, TIMER_COMMAND_WAIT_TICKS) == pdPASS) {
    releaseTimerArmed_[index] = true;
  }
}

void DacController::stopReleaseTimerUnlocked(uint8_t index) {
  if (
    index < AppConfig::DAC_CHANNEL_COUNT &&
    releaseTimers_[index] != nullptr &&
    releaseTimerArmed_[index]
  ) {
    if (xTimerStop(releaseTimers_[index], TIMER_COMMAND_WAIT_TICKS) == pdPASS) {
      releaseTimerArmed_[index] = false;
    }
  }
}

uint16_t DacController::outputMillivoltsUnlocked(uint8_t index) const {
  if (index >= AppConfig::DAC_CHANNEL_COUNT) {
    return 0;
  }
  return channels_[index].oneShot ? AppConfig::DAC_DEFAULT_MV : channels_[index].targetMv;
}

uint8_t DacController::millivoltsToCodeUnlocked(uint16_t millivolts) const {
  if (referenceMv_ == 0) {
    return 0;
  }
  const uint32_t rounded =
    (static_cast<uint32_t>(millivolts) * 255U + static_cast<uint32_t>(referenceMv_) / 2U) /
    static_cast<uint32_t>(referenceMv_);
  return static_cast<uint8_t>(min<uint32_t>(rounded, 255U));
}

int DacController::parseChannelPrefix(const String &command, unsigned int &consumed) const {
  if (TextUtil::startsWithIgnoreCase(command, "DAC1")) {
    consumed = 4;
    return AppConfig::DAC_ANALOG_CHANNEL_INDEX;
  }
  if (TextUtil::startsWithIgnoreCase(command, "GPIO26")) {
    consumed = 6;
    return AppConfig::DIGITAL_OUTPUT_CHANNEL_INDEX;
  }
  if (TextUtil::startsWithIgnoreCase(command, "DAC2")) {
    consumed = 4;
    return AppConfig::DIGITAL_OUTPUT_CHANNEL_INDEX;
  }
  return -1;
}

String DacController::channelName(uint8_t index) const {
  return index == AppConfig::DIGITAL_OUTPUT_CHANNEL_INDEX ? "GPIO26" : "DAC1";
}

void DacController::publishConfigReadback(
  CommandSource source,
  const String &requestId,
  const String &reason
) {
  Channel snapshot[AppConfig::DAC_CHANNEL_COUNT]{};
  uint16_t referenceMv = AppConfig::DAC_DEFAULT_REFERENCE_MV;
  const uint32_t now = millis();

  if (lockState()) {
    referenceMv = referenceMv_;
    for (uint8_t index = 0; index < AppConfig::DAC_CHANNEL_COUNT; index++) {
      snapshot[index] = channels_[index];
    }
    unlockState();
  }

  publish(
    EventLevel::STATUS,
    source,
    requestId,
    "[CONFIG] outputs " + reason + " dacReferenceMv=" + String(referenceMv)
  );
  for (uint8_t index = 0; index < AppConfig::DAC_CHANNEL_COUNT; index++) {
    const Channel &channel = snapshot[index];
    const bool digital = index == AppConfig::DIGITAL_OUTPUT_CHANNEL_INDEX;
    const uint32_t remaining = channel.oneShot &&
      static_cast<int32_t>(channel.oneShotEndsAtMs - now) > 0
        ? static_cast<uint32_t>(channel.oneShotEndsAtMs - now)
        : 0;
    String message = "[CONFIG] " + channelName(index) +
      " pin=" + String(pins_[index]) +
      " enabled=" + TextUtil::boolWord(channel.enabled);
    if (digital) {
      message += " mode=digital level=";
      message += channel.enabled ? "HIGH" : "LOW";
    } else {
      const uint16_t outputMv = channel.oneShot ? AppConfig::DAC_DEFAULT_MV : channel.targetMv;
      const uint32_t code = referenceMv == 0
        ? 0
        : (static_cast<uint32_t>(outputMv) * 255U + referenceMv / 2U) / referenceMv;
      message += " mode=dac targetMv=" + String(channel.targetMv);
      message += " outputMv=" + String(channel.enabled ? outputMv : 0);
      message += " code=" + String(min<uint32_t>(code, 255U));
    }
    message += " boot=" + TextUtil::boolWord(channel.bootEnabled);
    message += " timeoutMs=" + String(channel.maxOnMs);
    message += " oneShot=" + TextUtil::boolWord(channel.oneShot);
    message += " remainingMs=" + String(remaining);
    publish(EventLevel::STATUS, source, requestId, message);
  }
}

void DacController::publish(
  EventLevel level,
  CommandSource source,
  const String &requestId,
  const String &message
) {
  events_.publish(level, message, source, requestId);
}

void DacController::error(
  CommandSource source,
  const String &requestId,
  const String &message
) {
  events_.publish(EventLevel::ERROR, message, source, requestId);
}

#endif  // APP_STEPPER_DAC_ADDON_ENABLED
