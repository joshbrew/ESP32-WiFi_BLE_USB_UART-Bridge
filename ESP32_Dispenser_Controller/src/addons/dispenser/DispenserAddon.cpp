#include "DispenserAddon.h"

#include <Preferences.h>
#include <ctype.h>
#include <stddef.h>
#include <string.h>

#include "../../config/AppConfig.h"
#include "../../util/TextUtil.h"

#if APP_DRONE_DISPENSER_ADDON_ENABLED

namespace {

constexpr const char *PREFERENCES_NAMESPACE = "drone-disp";
constexpr const char *ACTIVE_PROFILE_KEY = "activeprof";
constexpr uint32_t PROFILE_MAGIC = 0x5041594CUL;  // "PAYL"
constexpr uint16_t PROFILE_VERSION = 1;

String profileKey(uint8_t slot) {
  return "prof" + String(slot);
}

String valueAfterPrefix(const String &value, const char *prefix) {
  String result = value.substring(static_cast<unsigned int>(strlen(prefix)));
  result.trim();
  return result;
}

bool isSafeOutputCandidate(int pin) {
  // Conservative classic-ESP32 list: excludes flash, input-only, and the most
  // troublesome boot-strapping pins. Expand only after checking the target board.
  static const int candidates[] = {
    13, 14, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33
  };
  for (const int candidate : candidates) {
    if (pin == candidate) {
      return true;
    }
  }
  return false;
}

}  // namespace

DispenserAddon::DispenserAddon(EventBus &events)
  : events_(events),
    outputPin_(AppConfig::PIN_DISPENSER),
    activeHigh_(AppConfig::DISPENSER_ACTIVE_HIGH),
    defaultPulseMs_(AppConfig::DISPENSER_DEFAULT_PULSE_MS),
    maxPulseMs_(AppConfig::DISPENSER_MAX_PULSE_MS),
    armTimeoutMs_(AppConfig::DISPENSER_ARM_TIMEOUT_MS),
    profiles_{},
    activeProfileSlot_(-1),
    profileModified_(false),
    standaloneSettingsActive_(false),
    initialized_(false),
    armed_(false),
    dispensing_(false),
    faulted_(false),
    armedAtMs_(0),
    dispenseStartedAtMs_(0),
    dispenseEndsAtMs_(0),
    dispenseCount_(0),
    totalDispenseMs_(0),
    lastServiceAtMs_(0),
    maxServiceGapMs_(0),
    maxStopLatenessMs_(0),
    lateStopCount_(0),
    lastReason_("safe boot") {}

const char *DispenserAddon::name() const {
  return "drone-dispenser";
}

void DispenserAddon::begin() {
  setDefaults();
  memset(profiles_, 0, sizeof(profiles_));
  if (!loadProfileLibrary()) {
    loadSettings();
  }

  // Set the output latch before selecting OUTPUT mode to minimize a boot-time
  // pulse. The external analog-switch circuit must still provide a hardware
  // inactive bias while the ESP32 itself is reset or unpowered.
  applyInactivePin(outputPin_);

  if (AppConfig::PIN_DISPENSER_INTERLOCK >= 0) {
    pinMode(AppConfig::PIN_DISPENSER_INTERLOCK, INPUT);
  }

  initialized_ = true;
  lastServiceAtMs_ = millis();
  armed_ = false;
  dispensing_ = false;
  faulted_ = false;
  lastReason_ = "safe boot; explicit Arm required";
  publish(
    EventLevel::STATUS,
    CommandSource::INTERNAL,
    String(),
    "[READY] dispenser GPIO" + String(outputPin_) +
      " inactive and DISARMED profile=" + currentProfileName() +
      " maxPulseMs=" + String(maxPulseMs_)
  );
}

void DispenserAddon::service() {
  if (!initialized_) {
    return;
  }

  const uint32_t now = millis();
  const uint32_t serviceGapMs = static_cast<uint32_t>(now - lastServiceAtMs_);
  if (serviceGapMs > maxServiceGapMs_) {
    maxServiceGapMs_ = serviceGapMs;
  }
  lastServiceAtMs_ = now;
  if (armed_ && !interlockOpen()) {
    disarm(
      CommandSource::INTERNAL,
      String(),
      "external interlock closed",
      true,
      true
    );
    return;
  }

  if (
    dispensing_ &&
    static_cast<int32_t>(now - dispenseEndsAtMs_) >= 0
  ) {
    const uint32_t latenessMs = static_cast<uint32_t>(now - dispenseEndsAtMs_);
    if (latenessMs > maxStopLatenessMs_) {
      maxStopLatenessMs_ = latenessMs;
    }
    if (latenessMs > 2) {
      lateStopCount_++;
    }
    stopDispense(
      CommandSource::INTERNAL,
      String(),
      "requested pulse complete",
      true
    );
  }

  if (
    armed_ &&
    static_cast<uint32_t>(now - armedAtMs_) >= armTimeoutMs_
  ) {
    disarm(
      CommandSource::INTERNAL,
      String(),
      "arming window expired",
      true,
      false
    );
  }
}

bool DispenserAddon::canAcceptCommand(const String &command) const {
  (void)command;
  // Core and radio commands pass through the same dispatcher, so an addon must
  // not reject lines merely because they belong to another subsystem.
  return true;
}

bool DispenserAddon::handleCommand(
  const String &command,
  CommandSource source,
  const String &requestId
) {
  String work = command;
  work.trim();

  if (work.equalsIgnoreCase("Arm") || work.equalsIgnoreCase("DispenserArm")) {
    arm(source, requestId);
    return true;
  }

  if (work.equalsIgnoreCase("Disarm") || work.equalsIgnoreCase("DispenserDisarm")) {
    disarm(source, requestId, "operator disarmed", true, false);
    return true;
  }

  if (
    work.equalsIgnoreCase("DispenseStop") ||
    work.equalsIgnoreCase("DispenserOff") ||
    work.equalsIgnoreCase("GPIO26:OFF")
  ) {
    stopDispense(source, requestId, "operator stop", true);
    return true;
  }

  if (TextUtil::startsWithIgnoreCase(work, "Dispense:")) {
    uint32_t durationMs = 0;
    if (!TextUtil::parseUnsigned32(work.substring(9), durationMs)) {
      error(source, requestId, "Dispense requires milliseconds, for example Dispense:250");
    } else {
      startDispense(durationMs, source, requestId, "timed dispense command");
    }
    return true;
  }

  if (work.equalsIgnoreCase("GPIO26:ON")) {
    publish(
      EventLevel::WARNING,
      source,
      requestId,
      "legacy GPIO26:ON converted to a bounded " +
        String(defaultPulseMs_) + " ms dispense pulse"
    );
    startDispense(
      defaultPulseMs_,
      source,
      requestId,
      "legacy bounded pulse"
    );
    return true;
  }

  if (
    work.equalsIgnoreCase("DispenserStatus") ||
    work.equalsIgnoreCase("PayloadStatus")
  ) {
    publish(EventLevel::STATUS, source, requestId, statusText());
    return true;
  }

  if (TextUtil::startsWithIgnoreCase(work, "DispenserPin:")) {
    long parsed = 0;
    String reason;
    if (!TextUtil::parseLong(work.substring(13), parsed)) {
      error(source, requestId, "DispenserPin requires an integer GPIO number");
    } else if (armed_ || dispensing_) {
      error(source, requestId, "disarm before changing the dispenser pin");
    } else if (!setConfiguredPin(static_cast<int>(parsed), reason)) {
      error(source, requestId, reason);
    } else {
      publish(
        EventLevel::STATUS,
        source,
        requestId,
        "[CONFIG] dispenser output moved to GPIO" + String(outputPin_) +
          "; use DispenserSave or PayloadProfileSave:name to persist"
      );
    }
    return true;
  }

  if (TextUtil::startsWithIgnoreCase(work, "DispenserActiveHigh:")) {
    bool enabled = false;
    if (!TextUtil::parseOnOff(work.substring(20), enabled)) {
      error(source, requestId, "DispenserActiveHigh requires ON or OFF");
    } else if (armed_ || dispensing_) {
      error(source, requestId, "disarm before changing output polarity");
    } else {
      applyInactivePin(outputPin_);
      activeHigh_ = enabled;
      applyInactivePin(outputPin_);
      profileModified_ = true;
      publish(
        EventLevel::WARNING,
        source,
        requestId,
        "[CONFIG] dispenser activeHigh=" + TextUtil::boolWord(activeHigh_) +
          "; verify unloaded hardware, then save standalone or to a profile"
      );
    }
    return true;
  }

  if (TextUtil::startsWithIgnoreCase(work, "DispenserDefaultPulse:")) {
    uint32_t value = 0;
    String reason;
    if (!TextUtil::parseUnsigned32(valueAfterPrefix(work, "DispenserDefaultPulse:"), value)) {
      error(source, requestId, "DispenserDefaultPulse requires milliseconds");
    } else if (armed_ || dispensing_) {
      error(source, requestId, "disarm before changing the default pulse");
    } else if (!setDefaultPulse(value, reason)) {
      error(source, requestId, reason);
    } else {
      publish(
        EventLevel::STATUS,
        source,
        requestId,
        "[CONFIG] defaultPulseMs=" + String(defaultPulseMs_) +
          "; save it manually or into a named payload profile"
      );
    }
    return true;
  }

  if (TextUtil::startsWithIgnoreCase(work, "DispenserMaxPulse:")) {
    uint32_t value = 0;
    String reason;
    if (!TextUtil::parseUnsigned32(valueAfterPrefix(work, "DispenserMaxPulse:"), value)) {
      error(source, requestId, "DispenserMaxPulse requires milliseconds");
    } else if (armed_ || dispensing_) {
      error(source, requestId, "disarm before changing the maximum pulse");
    } else if (!setMaxPulse(value, reason)) {
      error(source, requestId, reason);
    } else {
      publish(
        EventLevel::WARNING,
        source,
        requestId,
        "[CONFIG] maxPulseMs=" + String(maxPulseMs_) +
          " compiledCeilingMs=" + String(AppConfig::DISPENSER_MAX_PULSE_MS)
      );
    }
    return true;
  }

  if (TextUtil::startsWithIgnoreCase(work, "DispenserArmTimeout:")) {
    uint32_t value = 0;
    String reason;
    if (!TextUtil::parseUnsigned32(valueAfterPrefix(work, "DispenserArmTimeout:"), value)) {
      error(source, requestId, "DispenserArmTimeout requires milliseconds");
    } else if (armed_ || dispensing_) {
      error(source, requestId, "disarm before changing the arm timeout");
    } else if (!setArmTimeout(value, reason)) {
      error(source, requestId, reason);
    } else {
      publish(
        EventLevel::STATUS,
        source,
        requestId,
        "[CONFIG] armTimeoutMs=" + String(armTimeoutMs_) +
          " compiledCeilingMs=" + String(AppConfig::DISPENSER_ARM_TIMEOUT_MS)
      );
    }
    return true;
  }

  if (work.equalsIgnoreCase("PayloadProfileList")) {
    listProfiles(source, requestId);
    return true;
  }

  if (TextUtil::startsWithIgnoreCase(work, "PayloadProfileShow:")) {
    const String profileName = valueAfterPrefix(work, "PayloadProfileShow:");
    const int slot = findProfile(profileName);
    if (slot < 0) {
      error(source, requestId, "payload profile not found: " + profileName);
    } else {
      showProfile(static_cast<uint8_t>(slot), source, requestId);
    }
    return true;
  }

  if (TextUtil::startsWithIgnoreCase(work, "PayloadProfileSave:")) {
    const String profileName = valueAfterPrefix(work, "PayloadProfileSave:");
    if (armed_ || dispensing_) {
      error(source, requestId, "disarm before saving a payload profile");
      return true;
    }
    if (!validProfileName(profileName)) {
      error(source, requestId, "profile name must be 1-15 letters, digits, '-' or '_'");
      return true;
    }
    int slot = findProfile(profileName);
    if (slot < 0) {
      slot = findFreeProfileSlot();
    }
    if (slot < 0) {
      error(source, requestId, "payload profile library is full; delete a profile first");
      return true;
    }
    StoredProfile candidate{};
    initializeProfile(candidate, profileName);
    const uint8_t targetSlot = static_cast<uint8_t>(slot);
    if (!saveProfileSlot(targetSlot, candidate)) {
      error(source, requestId, "could not save or verify the payload profile");
      return true;
    }
    profiles_[targetSlot] = candidate;
    if (!persistActiveProfile(static_cast<int8_t>(targetSlot))) {
      error(source, requestId, "profile saved, but its active selection could not be persisted");
      return true;
    }
    activeProfileSlot_ = static_cast<int8_t>(targetSlot);
    profileModified_ = false;
    standaloneSettingsActive_ = false;
    publish(
      EventLevel::STATUS,
      source,
      requestId,
      "[PROFILE] saved and selected name=" + profileName + " slot=" + String(targetSlot)
    );
    return true;
  }

  if (TextUtil::startsWithIgnoreCase(work, "PayloadProfileUse:")) {
    const String profileName = valueAfterPrefix(work, "PayloadProfileUse:");
    const int slot = findProfile(profileName);
    if (slot < 0) {
      error(source, requestId, "payload profile not found: " + profileName);
    } else if (armed_ || dispensing_) {
      error(source, requestId, "disarm before changing payload profiles");
    } else if (!persistActiveProfile(static_cast<int8_t>(slot))) {
      error(source, requestId, "could not persist the selected payload profile");
    } else {
      applyProfile(profiles_[slot]);
      activeProfileSlot_ = static_cast<int8_t>(slot);
      profileModified_ = false;
      standaloneSettingsActive_ = false;
      publish(
        EventLevel::WARNING,
        source,
        requestId,
        "[PROFILE] selected " + String(profiles_[slot].name) +
          " GPIO" + String(outputPin_) + " output remains inactive and DISARMED"
      );
    }
    return true;
  }

  if (TextUtil::startsWithIgnoreCase(work, "PayloadProfileDelete:")) {
    const String profileName = valueAfterPrefix(work, "PayloadProfileDelete:");
    const int slot = findProfile(profileName);
    if (slot < 0) {
      error(source, requestId, "payload profile not found: " + profileName);
    } else if (armed_ || dispensing_) {
      error(source, requestId, "disarm before deleting a payload profile");
    } else if (slot == activeProfileSlot_) {
      error(source, requestId, "select or save another configuration before deleting the active profile");
    } else if (!eraseProfileSlot(static_cast<uint8_t>(slot))) {
      error(source, requestId, "could not erase the payload profile");
    } else {
      publish(EventLevel::STATUS, source, requestId, "[PROFILE] deleted " + profileName);
    }
    return true;
  }

  if (work.equalsIgnoreCase("PayloadProfileEraseAll")) {
    if (armed_ || dispensing_) {
      error(source, requestId, "disarm before erasing payload profiles");
    } else if (!eraseAllProfiles()) {
      error(source, requestId, "could not erase every payload profile");
    } else {
      publish(
        EventLevel::WARNING,
        source,
        requestId,
        "[PROFILE] all named profiles erased; current outputs remain inactive until reboot or reconfiguration"
      );
    }
    return true;
  }

  if (work.equalsIgnoreCase("DispenserSave")) {
    if (armed_ || dispensing_) {
      error(source, requestId, "disarm before saving standalone dispenser settings");
    } else if (saveSettings()) {
      publish(
        EventLevel::STATUS,
        source,
        requestId,
        "[CONFIG] standalone dispenser pin, polarity, and timing settings saved"
      );
    } else {
      error(source, requestId, "could not save dispenser configuration");
    }
    return true;
  }

  if (work.equalsIgnoreCase("DispenserDefaults")) {
    if (armed_ || dispensing_) {
      error(source, requestId, "disarm before restoring dispenser defaults");
    } else {
      const int previousPin = outputPin_;
      applyInactivePin(previousPin);
      setDefaults();
      profileModified_ = true;
      standaloneSettingsActive_ = false;
      applyInactivePin(outputPin_);
      if (previousPin != outputPin_) {
        pinMode(previousPin, INPUT);
      }
      publish(
        EventLevel::STATUS,
        source,
        requestId,
        "[CONFIG] volatile dispenser defaults restored; use DispenserSave to persist"
      );
    }
    return true;
  }

  if (work.equalsIgnoreCase("DispenserErase")) {
    if (armed_ || dispensing_) {
      error(source, requestId, "disarm before erasing dispenser settings");
    } else if (eraseSettings()) {
      publish(
        EventLevel::STATUS,
        source,
        requestId,
        "[CONFIG] standalone settings and active-profile selection erased; named profiles retained"
      );
    } else {
      error(source, requestId, "could not erase dispenser settings");
    }
    return true;
  }

  return false;
}

bool DispenserAddon::stopAll(CommandSource source, const String &requestId) {
  disarm(source, requestId, "StopAll", true, false);
  return !dispensing_ && !armed_;
}

bool DispenserAddon::isBusy() const {
  return dispensing_;
}

bool DispenserAddon::hasActiveOutput() const {
  return dispensing_;
}

bool DispenserAddon::hasTimedOperationActive() const {
  return dispensing_ || armed_;
}

bool DispenserAddon::canStartRoutine(String &reason) const {
  if (!initialized_) {
    reason = "dispenser is not initialized";
    return false;
  }
  if (faulted_) {
    reason = "clear the dispenser fault by checking the interlock and sending Arm";
    return false;
  }
  if (!armed_) {
    reason = "send Arm before starting a payload routine";
    return false;
  }
  if (!interlockOpen()) {
    reason = "external dispenser interlock is closed";
    return false;
  }
  reason = "armed and ready";
  return true;
}

bool DispenserAddon::blocksExternalCommandDuringRoutine(const String &command) const {
  String work = command;
  work.trim();
  return work.equalsIgnoreCase("Arm") ||
    work.equalsIgnoreCase("DispenserArm") ||
    work.equalsIgnoreCase("GPIO26:ON") ||
    work.equalsIgnoreCase("DispenserSave") ||
    work.equalsIgnoreCase("DispenserDefaults") ||
    work.equalsIgnoreCase("DispenserErase") ||
    work.equalsIgnoreCase("PayloadProfileEraseAll") ||
    TextUtil::startsWithIgnoreCase(work, "Dispense:") ||
    TextUtil::startsWithIgnoreCase(work, "DispenserPin:") ||
    TextUtil::startsWithIgnoreCase(work, "DispenserActiveHigh:") ||
    TextUtil::startsWithIgnoreCase(work, "DispenserDefaultPulse:") ||
    TextUtil::startsWithIgnoreCase(work, "DispenserMaxPulse:") ||
    TextUtil::startsWithIgnoreCase(work, "DispenserArmTimeout:") ||
    TextUtil::startsWithIgnoreCase(work, "PayloadProfileSave:") ||
    TextUtil::startsWithIgnoreCase(work, "PayloadProfileUse:") ||
    TextUtil::startsWithIgnoreCase(work, "PayloadProfileDelete:");
}

void DispenserAddon::appendStateJson(String &json, bool compact) const {
  const uint32_t now = millis();
  json += ",\"addon\":{\"name\":\"drone-dispenser\",\"active\":true";
  json += ",\"dispenser\":true,\"stepper\":false,\"dac\":false}";
  json += ",\"dispenser\":{";
  json += "\"pin\":" + String(outputPin_);
  json += ",\"activeHigh\":" + TextUtil::jsonBool(activeHigh_);
  json += ",\"armed\":" + TextUtil::jsonBool(armed_);
  json += ",\"dispensing\":" + TextUtil::jsonBool(dispensing_);
  json += ",\"faulted\":" + TextUtil::jsonBool(faulted_);
  json += ",\"interlockConfigured\":" + TextUtil::jsonBool(AppConfig::PIN_DISPENSER_INTERLOCK >= 0);
  json += ",\"interlockOpen\":" + TextUtil::jsonBool(interlockOpen());
  json += ",\"remainingMs\":" + String(remainingDispenseMs(now));
  json += ",\"armRemainingMs\":" + String(remainingArmMs(now));
  json += ",\"maxPulseMs\":" + String(maxPulseMs_);
  json += ",\"profile\":\"" + TextUtil::jsonEscape(currentProfileName()) + "\"";
  json += ",\"count\":" + String(dispenseCount_);
  json += ",\"maxStopLatenessMs\":" + String(maxStopLatenessMs_);
  if (!compact) {
    json += ",\"profileModified\":" + TextUtil::jsonBool(profileModified_);
    json += ",\"defaultPulseMs\":" + String(defaultPulseMs_);
    json += ",\"armTimeoutMs\":" + String(armTimeoutMs_);
    json += ",\"profilesStored\":" + String(storedProfileCount());
    json += ",\"profilesCapacity\":" + String(AppConfig::DISPENSER_PROFILE_MAX_COUNT);
    json += ",\"totalDispenseMs\":" + String(totalDispenseMs_);
    json += ",\"maxServiceGapMs\":" + String(maxServiceGapMs_);
    json += ",\"lateStopCount\":" + String(lateStopCount_);
    json += ",\"lastReason\":\"" + TextUtil::jsonEscape(lastReason_) + "\"";
  }
  json += "}";
}

void DispenserAddon::publishHelp(
  EventBus &events,
  CommandSource source,
  const String &requestId
) const {
  events.publish(
    EventLevel::STATUS,
    "Dispenser: Arm Disarm Dispense:milliseconds DispenseStop DispenserStatus StopAll",
    source,
    requestId
  );
  events.publish(
    EventLevel::STATUS,
    "Dispenser config: DispenserPin:gpio DispenserActiveHigh:ON|OFF DispenserDefaultPulse:ms DispenserMaxPulse:ms DispenserArmTimeout:ms",
    source,
    requestId
  );
  events.publish(
    EventLevel::STATUS,
    "Payload profiles: PayloadProfileSave:name PayloadProfileUse:name PayloadProfileList PayloadProfileShow:name PayloadProfileDelete:name",
    source,
    requestId
  );
  events.publish(
    EventLevel::STATUS,
    "Persistence: DispenserSave DispenserDefaults DispenserErase PayloadProfileEraseAll",
    source,
    requestId
  );
  events.publish(
    EventLevel::STATUS,
    "Safety: every activation is timed; routines require a separate Arm and cannot persist an armed state",
    source,
    requestId
  );
}

void DispenserAddon::publishStatus(
  EventBus &events,
  CommandSource source,
  const String &requestId,
  bool configuration
) const {
  events.publish(
    EventLevel::STATUS,
    String(configuration ? "[CONFIG] " : "[STATUS] ") + statusText(),
    source,
    requestId
  );
}

bool DispenserAddon::startDispense(
  uint32_t durationMs,
  CommandSource source,
  const String &requestId,
  const char *reason
) {
  if (!initialized_) {
    error(source, requestId, "dispenser is not initialized");
    return false;
  }
  if (!armed_) {
    error(source, requestId, "dispenser is DISARMED; send Arm first");
    return false;
  }
  if (dispensing_) {
    error(
      source,
      requestId,
      "dispenser pulse already active; wait for completion or send DispenseStop"
    );
    return false;
  }
  if (faulted_) {
    error(source, requestId, "dispenser fault is active; check the interlock and send Arm");
    return false;
  }
  if (!interlockOpen()) {
    disarm(source, requestId, "external interlock closed", true, true);
    return false;
  }
  if (durationMs == 0 || durationMs > maxPulseMs_) {
    error(
      source,
      requestId,
      "dispense duration must be 1 to " + String(maxPulseMs_) +
        " ms for payload profile " + currentProfileName()
    );
    return false;
  }

  const uint32_t now = millis();
  const uint32_t armRemainingMs = remainingArmMs(now);
  if (durationMs > armRemainingMs) {
    error(
      source,
      requestId,
      "arming window has only " + String(armRemainingMs) +
        " ms remaining; send Arm again before this pulse"
    );
    return false;
  }
  writeOutput(true);
  dispensing_ = true;
  dispenseStartedAtMs_ = now;
  dispenseEndsAtMs_ = now + durationMs;
  dispenseCount_++;
  totalDispenseMs_ += durationMs;
  lastReason_ = reason;
  publish(
    EventLevel::MOTION,
    source,
    requestId,
    "[RUN] dispensing durationMs=" + String(durationMs) + " GPIO" + String(outputPin_)
  );
  return true;
}

void DispenserAddon::stopDispense(
  CommandSource source,
  const String &requestId,
  const char *reason,
  bool announce
) {
  const bool wasDispensing = dispensing_;
  writeOutput(false);
  dispensing_ = false;
  dispenseStartedAtMs_ = 0;
  dispenseEndsAtMs_ = 0;
  lastReason_ = reason;
  if (announce) {
    publish(
      wasDispensing ? EventLevel::STATUS : EventLevel::INFO,
      source,
      requestId,
      wasDispensing ? String("[DONE] dispenser inactive: ") + reason : "dispenser already inactive"
    );
  }
}

void DispenserAddon::disarm(
  CommandSource source,
  const String &requestId,
  const char *reason,
  bool announce,
  bool fault
) {
  const bool wasArmed = armed_ || dispensing_;
  stopDispense(source, requestId, reason, false);
  armed_ = false;
  armedAtMs_ = 0;
  faulted_ = fault;
  lastReason_ = reason;
  if (announce) {
    publish(
      fault ? EventLevel::ERROR : EventLevel::WARNING,
      source,
      requestId,
      String(fault ? "[FAULT] " : "[SAFE] ") +
        "dispenser DISARMED output inactive reason=" + reason +
        (wasArmed ? "" : " (was already safe)")
    );
  }
}

bool DispenserAddon::arm(CommandSource source, const String &requestId) {
  if (!initialized_) {
    error(source, requestId, "dispenser is not initialized");
    return false;
  }
  if (!interlockOpen()) {
    faulted_ = true;
    error(source, requestId, "cannot arm while the external interlock is closed");
    return false;
  }
  stopDispense(source, requestId, "arming", false);
  armed_ = true;
  faulted_ = false;
  armedAtMs_ = millis();
  lastReason_ = "operator armed";
  publish(
    EventLevel::WARNING,
    source,
    requestId,
    "[ARMED] dispenser ready for " + String(armTimeoutMs_) +
      " ms using profile=" + currentProfileName() +
      "; output remains inactive until Dispense:<ms>"
  );
  return true;
}

void DispenserAddon::applyInactivePin(int pin) const {
  const uint8_t inactiveLevel = activeHigh_ ? LOW : HIGH;
  digitalWrite(pin, inactiveLevel);
  pinMode(pin, OUTPUT);
  digitalWrite(pin, inactiveLevel);
}

void DispenserAddon::writeOutput(bool active) {
  if (!initialized_ && active) {
    return;
  }
  const bool high = active == activeHigh_;
  digitalWrite(outputPin_, high ? HIGH : LOW);
}

bool DispenserAddon::interlockOpen() const {
  if (AppConfig::PIN_DISPENSER_INTERLOCK < 0) {
    return true;
  }
  const bool high = digitalRead(AppConfig::PIN_DISPENSER_INTERLOCK) == HIGH;
  return high == AppConfig::DISPENSER_INTERLOCK_ACTIVE_HIGH;
}

bool DispenserAddon::validatePin(int pin, String &reason) const {
  if (!isSafeOutputCandidate(pin)) {
    reason = "GPIO" + String(pin) + " is not in the conservative dispenser output allowlist";
    return false;
  }
  if (
    AppConfig::ENABLE_STATUS_INDICATORS &&
    (pin == AppConfig::PIN_STATUS_CONNECTION || pin == AppConfig::PIN_STATUS_ACTIVITY)
  ) {
    reason = "GPIO" + String(pin) + " is owned by the status indicators";
    return false;
  }
  if (pin == AppConfig::PIN_DISPENSER_INTERLOCK) {
    reason = "the dispenser output cannot share the external interlock pin";
    return false;
  }
  if (
    AppConfig::ENABLE_AUX_UART &&
    (pin == AppConfig::AUX_UART_RX_PIN || pin == AppConfig::AUX_UART_TX_PIN)
  ) {
    reason = "GPIO" + String(pin) + " is owned by the optional auxiliary UART";
    return false;
  }
  reason = "valid output pin";
  return true;
}

bool DispenserAddon::validateTimings(
  uint32_t defaultPulseMs,
  uint32_t maxPulseMs,
  uint32_t armTimeoutMs,
  String &reason
) const {
  if (defaultPulseMs == 0) {
    reason = "default pulse must be at least 1 ms";
    return false;
  }
  if (maxPulseMs == 0 || maxPulseMs > AppConfig::DISPENSER_MAX_PULSE_MS) {
    reason = "maximum pulse must be 1 to the compiled ceiling of " +
      String(AppConfig::DISPENSER_MAX_PULSE_MS) + " ms";
    return false;
  }
  if (defaultPulseMs > maxPulseMs) {
    reason = "default pulse cannot exceed the profile maximum pulse";
    return false;
  }
  if (armTimeoutMs < maxPulseMs) {
    reason = "arm timeout must accommodate one maximum-length pulse";
    return false;
  }
  if (armTimeoutMs > AppConfig::DISPENSER_ARM_TIMEOUT_MS) {
    reason = "arm timeout cannot exceed the compiled ceiling of " +
      String(AppConfig::DISPENSER_ARM_TIMEOUT_MS) + " ms";
    return false;
  }
  reason = "valid bounded timings";
  return true;
}

bool DispenserAddon::setConfiguredPin(int pin, String &reason) {
  if (!validatePin(pin, reason)) {
    return false;
  }
  if (pin == outputPin_) {
    reason = "pin unchanged";
    return true;
  }
  const int previousPin = outputPin_;
  applyInactivePin(previousPin);
  outputPin_ = pin;
  applyInactivePin(outputPin_);
  pinMode(previousPin, INPUT);
  profileModified_ = true;
  reason = "pin changed";
  return true;
}

bool DispenserAddon::setDefaultPulse(uint32_t value, String &reason) {
  if (!validateTimings(value, maxPulseMs_, armTimeoutMs_, reason)) {
    return false;
  }
  defaultPulseMs_ = value;
  profileModified_ = true;
  reason = "default pulse changed";
  return true;
}

bool DispenserAddon::setMaxPulse(uint32_t value, String &reason) {
  if (!validateTimings(defaultPulseMs_, value, armTimeoutMs_, reason)) {
    return false;
  }
  maxPulseMs_ = value;
  profileModified_ = true;
  reason = "maximum pulse changed";
  return true;
}

bool DispenserAddon::setArmTimeout(uint32_t value, String &reason) {
  if (!validateTimings(defaultPulseMs_, maxPulseMs_, value, reason)) {
    return false;
  }
  armTimeoutMs_ = value;
  profileModified_ = true;
  reason = "arm timeout changed";
  return true;
}

void DispenserAddon::setDefaults() {
  outputPin_ = AppConfig::PIN_DISPENSER;
  activeHigh_ = AppConfig::DISPENSER_ACTIVE_HIGH;
  defaultPulseMs_ = AppConfig::DISPENSER_DEFAULT_PULSE_MS;
  maxPulseMs_ = AppConfig::DISPENSER_MAX_PULSE_MS;
  armTimeoutMs_ = AppConfig::DISPENSER_ARM_TIMEOUT_MS;
}

bool DispenserAddon::loadProfileLibrary() {
  Preferences preferences;
  if (!preferences.begin(PREFERENCES_NAMESPACE, true)) {
    events_.publish(
      EventLevel::STATUS,
      "[PROFILE] empty payload profile library ready capacity=" +
        String(AppConfig::DISPENSER_PROFILE_MAX_COUNT),
      CommandSource::INTERNAL
    );
    return false;
  }

  uint8_t loaded = 0;
  uint8_t rejected = 0;
  for (uint8_t slot = 0; slot < AppConfig::DISPENSER_PROFILE_MAX_COUNT; slot++) {
    const String key = profileKey(slot);
    const size_t storedLength = preferences.getBytesLength(key.c_str());
    if (storedLength == 0) {
      continue;
    }
    StoredProfile candidate{};
    if (
      storedLength == sizeof(candidate) &&
      preferences.getBytes(key.c_str(), &candidate, sizeof(candidate)) == sizeof(candidate) &&
      validStoredProfile(candidate)
    ) {
      profiles_[slot] = candidate;
      loaded++;
    } else {
      rejected++;
    }
  }

  const uint8_t encodedActive = preferences.getUChar(ACTIVE_PROFILE_KEY, 0);
  preferences.end();

  bool activeLoaded = false;
  if (encodedActive > 0) {
    const uint8_t slot = encodedActive - 1;
    if (slot < AppConfig::DISPENSER_PROFILE_MAX_COUNT && profiles_[slot].used) {
      const StoredProfile &profile = profiles_[slot];
      outputPin_ = profile.outputPin;
      activeHigh_ = profile.activeHigh != 0;
      defaultPulseMs_ = profile.defaultPulseMs;
      maxPulseMs_ = profile.maxPulseMs;
      armTimeoutMs_ = profile.armTimeoutMs;
      activeProfileSlot_ = static_cast<int8_t>(slot);
      profileModified_ = false;
      standaloneSettingsActive_ = false;
      activeLoaded = true;
    } else {
      rejected++;
    }
  }

  events_.publish(
    rejected == 0 ? EventLevel::STATUS : EventLevel::WARNING,
    "[PROFILE] library ready saved=" + String(loaded) +
      " rejected=" + String(rejected) +
      " capacity=" + String(AppConfig::DISPENSER_PROFILE_MAX_COUNT) +
      " active=" + String(activeLoaded ? profiles_[activeProfileSlot_].name : "none"),
    CommandSource::INTERNAL
  );
  return activeLoaded;
}

int DispenserAddon::findProfile(const String &name) const {
  for (uint8_t slot = 0; slot < AppConfig::DISPENSER_PROFILE_MAX_COUNT; slot++) {
    if (profiles_[slot].used && String(profiles_[slot].name).equalsIgnoreCase(name)) {
      return slot;
    }
  }
  return -1;
}

int DispenserAddon::findFreeProfileSlot() const {
  for (uint8_t slot = 0; slot < AppConfig::DISPENSER_PROFILE_MAX_COUNT; slot++) {
    if (!profiles_[slot].used) {
      return slot;
    }
  }
  return -1;
}

bool DispenserAddon::validProfileName(const String &name) const {
  if (name.length() == 0 || name.length() > AppConfig::DISPENSER_PROFILE_NAME_BYTES) {
    return false;
  }
  for (size_t index = 0; index < name.length(); index++) {
    const char c = name[index];
    if (!isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') {
      return false;
    }
  }
  return true;
}

bool DispenserAddon::validStoredProfile(const StoredProfile &profile) const {
  if (
    profile.magic != PROFILE_MAGIC ||
    profile.version != PROFILE_VERSION ||
    profile.used != 1 ||
    profile.activeHigh > 1 ||
    profile.name[AppConfig::DISPENSER_PROFILE_NAME_BYTES] != '\0' ||
    !validProfileName(String(profile.name)) ||
    profile.checksum != profileChecksum(profile)
  ) {
    return false;
  }
  String reason;
  return validatePin(profile.outputPin, reason) &&
    validateTimings(
      profile.defaultPulseMs,
      profile.maxPulseMs,
      profile.armTimeoutMs,
      reason
    );
}

uint32_t DispenserAddon::profileChecksum(const StoredProfile &profile) const {
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&profile);
  const size_t length = offsetof(StoredProfile, checksum);
  uint32_t hash = 2166136261UL;
  for (size_t index = 0; index < length; index++) {
    hash ^= bytes[index];
    hash *= 16777619UL;
  }
  return hash;
}

void DispenserAddon::initializeProfile(StoredProfile &profile, const String &name) const {
  memset(&profile, 0, sizeof(profile));
  profile.magic = PROFILE_MAGIC;
  profile.version = PROFILE_VERSION;
  profile.used = 1;
  profile.activeHigh = activeHigh_ ? 1 : 0;
  profile.outputPin = outputPin_;
  profile.defaultPulseMs = defaultPulseMs_;
  profile.maxPulseMs = maxPulseMs_;
  profile.armTimeoutMs = armTimeoutMs_;
  name.toCharArray(profile.name, sizeof(profile.name));
  profile.checksum = profileChecksum(profile);
}

bool DispenserAddon::saveProfileSlot(uint8_t slot, const StoredProfile &profile) const {
  if (slot >= AppConfig::DISPENSER_PROFILE_MAX_COUNT || !validStoredProfile(profile)) {
    return false;
  }
  Preferences preferences;
  if (!preferences.begin(PREFERENCES_NAMESPACE, false)) {
    return false;
  }
  const String key = profileKey(slot);
  const bool written =
    preferences.putBytes(key.c_str(), &profile, sizeof(profile)) == sizeof(profile);
  preferences.end();
  if (!written || !preferences.begin(PREFERENCES_NAMESPACE, true)) {
    return false;
  }
  StoredProfile verified{};
  const bool ok =
    preferences.getBytesLength(key.c_str()) == sizeof(verified) &&
    preferences.getBytes(key.c_str(), &verified, sizeof(verified)) == sizeof(verified) &&
    validStoredProfile(verified) &&
    memcmp(&verified, &profile, sizeof(profile)) == 0;
  preferences.end();
  return ok;
}

bool DispenserAddon::persistActiveProfile(int8_t slot) const {
  if (slot >= static_cast<int8_t>(AppConfig::DISPENSER_PROFILE_MAX_COUNT)) {
    return false;
  }
  Preferences preferences;
  if (!preferences.begin(PREFERENCES_NAMESPACE, false)) {
    return false;
  }
  bool written = true;
  if (slot < 0) {
    if (preferences.isKey(ACTIVE_PROFILE_KEY)) {
      written = preferences.remove(ACTIVE_PROFILE_KEY);
    }
  } else {
    written = preferences.putUChar(ACTIVE_PROFILE_KEY, static_cast<uint8_t>(slot + 1)) > 0;
  }
  preferences.end();
  if (!written || !preferences.begin(PREFERENCES_NAMESPACE, true)) {
    return false;
  }
  const bool verified = slot < 0
    ? !preferences.isKey(ACTIVE_PROFILE_KEY)
    : preferences.getUChar(ACTIVE_PROFILE_KEY, 0) == static_cast<uint8_t>(slot + 1);
  preferences.end();
  return verified;
}

bool DispenserAddon::eraseProfileSlot(uint8_t slot) {
  if (slot >= AppConfig::DISPENSER_PROFILE_MAX_COUNT) {
    return false;
  }
  Preferences preferences;
  if (!preferences.begin(PREFERENCES_NAMESPACE, false)) {
    return false;
  }
  const String key = profileKey(slot);
  bool removed = true;
  if (preferences.isKey(key.c_str())) {
    removed = preferences.remove(key.c_str());
  }
  preferences.end();
  if (!removed) {
    return false;
  }
  if (!preferences.begin(PREFERENCES_NAMESPACE, true)) {
    memset(&profiles_[slot], 0, sizeof(profiles_[slot]));
    return true;
  }
  const bool verified = !preferences.isKey(key.c_str());
  preferences.end();
  if (verified) {
    memset(&profiles_[slot], 0, sizeof(profiles_[slot]));
  }
  return verified;
}

bool DispenserAddon::eraseAllProfiles() {
  Preferences preferences;
  if (!preferences.begin(PREFERENCES_NAMESPACE, false)) {
    return false;
  }
  bool removed = true;
  for (uint8_t slot = 0; slot < AppConfig::DISPENSER_PROFILE_MAX_COUNT; slot++) {
    const String key = profileKey(slot);
    if (preferences.isKey(key.c_str())) {
      removed = preferences.remove(key.c_str()) && removed;
    }
  }
  if (preferences.isKey(ACTIVE_PROFILE_KEY)) {
    removed = preferences.remove(ACTIVE_PROFILE_KEY) && removed;
  }
  preferences.end();
  if (!removed) {
    return false;
  }
  if (!preferences.begin(PREFERENCES_NAMESPACE, true)) {
    memset(profiles_, 0, sizeof(profiles_));
    activeProfileSlot_ = -1;
    standaloneSettingsActive_ = false;
    profileModified_ = true;
    return true;
  }
  bool verified = !preferences.isKey(ACTIVE_PROFILE_KEY);
  for (uint8_t slot = 0; slot < AppConfig::DISPENSER_PROFILE_MAX_COUNT; slot++) {
    const String key = profileKey(slot);
    verified = !preferences.isKey(key.c_str()) && verified;
  }
  preferences.end();
  if (verified) {
    memset(profiles_, 0, sizeof(profiles_));
    activeProfileSlot_ = -1;
    standaloneSettingsActive_ = false;
    profileModified_ = true;
  }
  return verified;
}

void DispenserAddon::applyProfile(const StoredProfile &profile) {
  const int previousPin = outputPin_;
  applyInactivePin(previousPin);
  outputPin_ = profile.outputPin;
  activeHigh_ = profile.activeHigh != 0;
  defaultPulseMs_ = profile.defaultPulseMs;
  maxPulseMs_ = profile.maxPulseMs;
  armTimeoutMs_ = profile.armTimeoutMs;
  applyInactivePin(outputPin_);
  if (previousPin != outputPin_) {
    pinMode(previousPin, INPUT);
  }
  armed_ = false;
  dispensing_ = false;
  faulted_ = false;
  armedAtMs_ = 0;
  dispenseStartedAtMs_ = 0;
  dispenseEndsAtMs_ = 0;
  lastReason_ = "payload profile selected; explicit Arm required";
}

void DispenserAddon::listProfiles(CommandSource source, const String &requestId) const {
  const uint8_t stored = storedProfileCount();
  publish(
    EventLevel::STATUS,
    source,
    requestId,
    "[PROFILE] stored=" + String(stored) + "/" +
      String(AppConfig::DISPENSER_PROFILE_MAX_COUNT) +
      " current=" + currentProfileName()
  );
  for (uint8_t slot = 0; slot < AppConfig::DISPENSER_PROFILE_MAX_COUNT; slot++) {
    if (profiles_[slot].used) {
      showProfile(slot, source, requestId);
    }
  }
}

void DispenserAddon::showProfile(
  uint8_t slot,
  CommandSource source,
  const String &requestId
) const {
  if (slot >= AppConfig::DISPENSER_PROFILE_MAX_COUNT || !profiles_[slot].used) {
    return;
  }
  const StoredProfile &profile = profiles_[slot];
  publish(
    EventLevel::STATUS,
    source,
    requestId,
    "[PROFILE] slot=" + String(slot) + " name=" + String(profile.name) +
      (slot == activeProfileSlot_ ? (profileModified_ ? " active=modified" : " active=yes") : "") +
      " GPIO" + String(profile.outputPin) +
      " activeHigh=" + TextUtil::boolWord(profile.activeHigh != 0) +
      " defaultMs=" + String(profile.defaultPulseMs) +
      " maxMs=" + String(profile.maxPulseMs) +
      " armMs=" + String(profile.armTimeoutMs)
  );
}

String DispenserAddon::currentProfileName() const {
  if (
    activeProfileSlot_ >= 0 &&
    activeProfileSlot_ < static_cast<int8_t>(AppConfig::DISPENSER_PROFILE_MAX_COUNT) &&
    profiles_[activeProfileSlot_].used
  ) {
    return String(profiles_[activeProfileSlot_].name) + (profileModified_ ? "*" : "");
  }
  if (profileModified_) {
    return "volatile";
  }
  return standaloneSettingsActive_ ? "standalone" : "compiled";
}

uint8_t DispenserAddon::storedProfileCount() const {
  uint8_t count = 0;
  for (uint8_t slot = 0; slot < AppConfig::DISPENSER_PROFILE_MAX_COUNT; slot++) {
    if (profiles_[slot].used) {
      count++;
    }
  }
  return count;
}

bool DispenserAddon::loadSettings() {
  Preferences preferences;
  if (!preferences.begin(PREFERENCES_NAMESPACE, true)) {
    return false;
  }
  const bool valid = preferences.getBool("valid", false);
  const int savedPin = preferences.getInt("pin", AppConfig::PIN_DISPENSER);
  const bool savedActiveHigh = preferences.getBool("activehi", AppConfig::DISPENSER_ACTIVE_HIGH);
  const uint32_t savedDefaultPulseMs =
    preferences.getUInt("defms", AppConfig::DISPENSER_DEFAULT_PULSE_MS);
  const uint32_t savedMaxPulseMs =
    preferences.getUInt("maxms", AppConfig::DISPENSER_MAX_PULSE_MS);
  const uint32_t savedArmTimeoutMs =
    preferences.getUInt("armms", AppConfig::DISPENSER_ARM_TIMEOUT_MS);
  preferences.end();

  String reason;
  if (
    valid &&
    validatePin(savedPin, reason) &&
    validateTimings(savedDefaultPulseMs, savedMaxPulseMs, savedArmTimeoutMs, reason)
  ) {
    outputPin_ = savedPin;
    activeHigh_ = savedActiveHigh;
    defaultPulseMs_ = savedDefaultPulseMs;
    maxPulseMs_ = savedMaxPulseMs;
    armTimeoutMs_ = savedArmTimeoutMs;
    activeProfileSlot_ = -1;
    profileModified_ = false;
    standaloneSettingsActive_ = true;
    return true;
  }
  if (valid) {
    events_.publish(
      EventLevel::WARNING,
      "saved dispenser configuration rejected: " + reason + "; compile-time defaults used",
      CommandSource::INTERNAL
    );
  }
  return false;
}

bool DispenserAddon::saveSettings() {
  String reason;
  if (
    !validatePin(outputPin_, reason) ||
    !validateTimings(defaultPulseMs_, maxPulseMs_, armTimeoutMs_, reason)
  ) {
    return false;
  }
  Preferences preferences;
  if (!preferences.begin(PREFERENCES_NAMESPACE, false)) {
    return false;
  }
  bool ok = preferences.putInt("pin", outputPin_) > 0;
  ok = preferences.putBool("activehi", activeHigh_) > 0 && ok;
  ok = preferences.putUInt("defms", defaultPulseMs_) > 0 && ok;
  ok = preferences.putUInt("maxms", maxPulseMs_) > 0 && ok;
  ok = preferences.putUInt("armms", armTimeoutMs_) > 0 && ok;
  ok = preferences.putBool("valid", true) > 0 && ok;
  preferences.end();
  if (!ok || !preferences.begin(PREFERENCES_NAMESPACE, true)) {
    return false;
  }
  const bool verified =
    preferences.getBool("valid", false) &&
    preferences.getInt("pin", -1) == outputPin_ &&
    preferences.getBool("activehi", !activeHigh_) == activeHigh_ &&
    preferences.getUInt("defms", 0) == defaultPulseMs_ &&
    preferences.getUInt("maxms", 0) == maxPulseMs_ &&
    preferences.getUInt("armms", 0) == armTimeoutMs_;
  preferences.end();
  if (!verified || !persistActiveProfile(-1)) {
    return false;
  }
  activeProfileSlot_ = -1;
  profileModified_ = false;
  standaloneSettingsActive_ = true;
  return true;
}

bool DispenserAddon::eraseSettings() {
  Preferences preferences;
  if (!preferences.begin(PREFERENCES_NAMESPACE, false)) {
    return false;
  }
  static const char *const keys[] = {
    "valid", "pin", "activehi", "defms", "maxms", "armms", ACTIVE_PROFILE_KEY
  };
  bool ok = true;
  for (const char *key : keys) {
    if (preferences.isKey(key)) {
      ok = preferences.remove(key) && ok;
    }
  }
  preferences.end();
  if (!ok) {
    return false;
  }
  if (!preferences.begin(PREFERENCES_NAMESPACE, true)) {
    activeProfileSlot_ = -1;
    standaloneSettingsActive_ = false;
    profileModified_ = true;
    return true;
  }
  bool verified = true;
  for (const char *key : keys) {
    verified = !preferences.isKey(key) && verified;
  }
  preferences.end();
  if (verified) {
    activeProfileSlot_ = -1;
    standaloneSettingsActive_ = false;
    profileModified_ = true;
  }
  return verified;
}

uint32_t DispenserAddon::remainingDispenseMs(uint32_t now) const {
  if (!dispensing_ || static_cast<int32_t>(dispenseEndsAtMs_ - now) <= 0) {
    return 0;
  }
  return dispenseEndsAtMs_ - now;
}

uint32_t DispenserAddon::remainingArmMs(uint32_t now) const {
  if (!armed_) {
    return 0;
  }
  const uint32_t elapsed = static_cast<uint32_t>(now - armedAtMs_);
  return elapsed >= armTimeoutMs_
    ? 0
    : armTimeoutMs_ - elapsed;
}

String DispenserAddon::statusText() const {
  const uint32_t now = millis();
  String text = "dispenser=";
  text += faulted_ ? "FAULT" : dispensing_ ? "DISPENSING" : armed_ ? "ARMED" : "DISARMED";
  text += " GPIO" + String(outputPin_);
  text += " activeHigh=" + TextUtil::boolWord(activeHigh_);
  text += " profile=" + currentProfileName();
  text += " defaultMs=" + String(defaultPulseMs_);
  text += " maxMs=" + String(maxPulseMs_);
  text += " armMs=" + String(armTimeoutMs_);
  text += " pulseLeftMs=" + String(remainingDispenseMs(now));
  text += " armLeftMs=" + String(remainingArmMs(now));
  text += " interlock=" + String(interlockOpen() ? "open" : "closed");
  String reason = lastReason_;
  if (reason.length() > 24) {
    reason.remove(24);
    reason += "...";
  }
  text += " reason=" + reason;
  return text;
}

void DispenserAddon::publish(
  EventLevel level,
  CommandSource source,
  const String &requestId,
  const String &message
) const {
  events_.publish(level, message, source, requestId);
}

void DispenserAddon::error(
  CommandSource source,
  const String &requestId,
  const String &message
) const {
  publish(EventLevel::ERROR, source, requestId, "[ERROR] " + message);
}

#endif  // APP_DRONE_DISPENSER_ADDON_ENABLED
