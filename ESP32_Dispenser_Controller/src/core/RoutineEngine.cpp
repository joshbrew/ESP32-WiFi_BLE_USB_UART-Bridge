#include "RoutineEngine.h"

#include <Preferences.h>
#include <ctype.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#include "../util/TextUtil.h"

namespace {

constexpr const char *PREFERENCES_NAMESPACE = "drone-routine";
constexpr uint32_t ROUTINE_MAGIC = 0x44524F4EUL;  // "DRON"
constexpr uint16_t ROUTINE_VERSION = 3;  // 32-bit repeats; record size unchanged.

String slotKey(uint8_t slot) {
  return "slot" + String(slot);
}

String trimmedAfter(const String &value, unsigned int offset) {
  String result = value.substring(offset);
  result.trim();
  return result;
}

}  // namespace

RoutineEngine::RoutineEngine(EventBus &events, DeviceAddon &addon)
  : events_(events),
    addon_(addon),
    submitter_(nullptr),
    submitContext_(nullptr),
    routines_{},
    saved_{},
    lastRunSucceeded_(false),
    active_(false),
    activeSlot_(0),
    stepIndex_(0),
    repeatIndex_(0),
    waiting_(false),
    runStartedAtMs_(0),
    waitStartedAtMs_(0),
    waitUntilMs_(0),
    runSource_(CommandSource::INTERNAL),
    runRequestId_(),
    lastResult_("never run") {}

void RoutineEngine::begin() {
  static_assert(offsetof(StoredRoutine, repeatCount) == 8 && offsetof(StoredRoutine, steps) == 28,
    "Saved routine migration requires the existing record offsets");
  memset(routines_, 0, sizeof(routines_));
  memset(saved_, 0, sizeof(saved_));

  Preferences preferences;
  if (!preferences.begin(PREFERENCES_NAMESPACE, true)) {
    events_.publish(
      EventLevel::STATUS,
      "[ROUTINE] no saved routine namespace; empty library ready",
      CommandSource::INTERNAL
    );
    return;
  }

  uint8_t loaded = 0;
  uint8_t rejected = 0;
  for (uint8_t slot = 0; slot < AppConfig::ROUTINE_MAX_COUNT; slot++) {
    const String key = slotKey(slot);
    const size_t storedLength = preferences.getBytesLength(key.c_str());
    if (storedLength == 0) {
      continue;
    }
    if (storedLength != sizeof(StoredRoutine)) {
      rejected++;
      continue;
    }
    StoredRoutine candidate{};
    const bool read = preferences.getBytes(key.c_str(), &candidate, sizeof(candidate)) == sizeof(candidate);
    // Version 2 had one repeat byte followed by the name and three padding
    // bytes. Verify the original bytes before shifting the name into version 3.
    if (read && candidate.version == 2 && candidate.checksum == checksum(candidate)) {
      auto *bytes = reinterpret_cast<uint8_t *>(&candidate);
      const uint8_t repeats = bytes[8];
      if (repeats > 0 && repeats <= 20 && bytes[9 + AppConfig::ROUTINE_NAME_BYTES] == 0) {
        memmove(candidate.name, bytes + 9, sizeof(candidate.name));
        candidate.repeatCount = repeats;
        candidate.version = ROUTINE_VERSION;
        candidate.checksum = checksum(candidate);
      }
    }
    if (
      read &&
      validStoredRoutine(candidate)
    ) {
      routines_[slot] = candidate;
      saved_[slot] = true;
      loaded++;
    } else {
      rejected++;
    }
  }
  preferences.end();

  events_.publish(
    EventLevel::STATUS,
    "[ROUTINE] library ready saved=" + String(loaded) +
      " rejected=" + String(rejected) +
      " capacity=" + String(AppConfig::ROUTINE_MAX_COUNT),
    CommandSource::INTERNAL
  );
}

void RoutineEngine::configureSubmitter(CommandSubmitter submitter, void *context) {
  submitter_ = submitter;
  submitContext_ = context;
}

bool RoutineEngine::recognizesCommand(const String &command) const {
  String work = command;
  work.trim();
  return TextUtil::startsWithIgnoreCase(work, "Routine");
}

bool RoutineEngine::handleCommand(
  const String &command,
  CommandSource source,
  const String &requestId
) {
  String work = command;
  work.trim();
  if (!recognizesCommand(work)) {
    return false;
  }

  if (work.equalsIgnoreCase("RoutineStatus")) {
    publish(EventLevel::STATUS, source, requestId, statusText());
    return true;
  }

  if (work.equalsIgnoreCase("RoutineList")) {
    bool any = false;
    for (uint8_t slot = 0; slot < AppConfig::ROUTINE_MAX_COUNT; slot++) {
      if (!routines_[slot].used) {
        continue;
      }
      any = true;
      publish(
        EventLevel::STATUS,
        source,
        requestId,
        "[ROUTINE] slot=" + String(slot) + " name=" + routines_[slot].name +
          " steps=" + String(routines_[slot].count) +
          " repeats=" + String(routines_[slot].repeatCount)
      );
    }
    if (!any) {
      publish(EventLevel::STATUS, source, requestId, "[ROUTINE] no routines saved or edited");
    }
    return true;
  }

  if (work.equalsIgnoreCase("RoutineStop")) {
    stop(source, requestId, "operator requested RoutineStop", true);
    return true;
  }

  if (TextUtil::startsWithIgnoreCase(work, "RoutineCreate:")) {
    const String name = trimmedAfter(work, 14);
    if (!validName(name)) {
      error(source, requestId, "routine name must be 1-15 letters, digits, '-' or '_'");
      return true;
    }
    int slot = findRoutine(name);
    if (slot < 0) {
      slot = findFreeSlot();
    }
    if (slot < 0) {
      error(source, requestId, "routine library is full; erase a routine first");
      return true;
    }
    if (active_ && activeSlot_ == static_cast<uint8_t>(slot)) {
      error(source, requestId, "stop the active routine before editing it");
      return true;
    }
    initializeRoutine(routines_[slot], name);
    saved_[slot] = false;
    publish(
      EventLevel::STATUS,
      source,
      requestId,
      "[ROUTINE] editing name=" + name + " slot=" + String(slot) +
        "; add steps, set repeats, then save"
    );
    return true;
  }

  if (TextUtil::startsWithIgnoreCase(work, "RoutineAdd:")) {
    String remainder = trimmedAfter(work, 11);
    const int separator = remainder.indexOf(':');
    if (separator <= 0) {
      error(source, requestId, "use RoutineAdd:name:WAIT:ms, :DISPENSE:ms, :WAIT_IDLE, or :COMMAND:text");
      return true;
    }
    String name = remainder.substring(0, separator);
    name.trim();
    const int slot = findRoutine(name);
    if (slot < 0) {
      error(source, requestId, "unknown routine; send RoutineCreate:" + name + " first");
      return true;
    }
    if (active_ && activeSlot_ == static_cast<uint8_t>(slot)) {
      error(source, requestId, "stop the active routine before editing it");
      return true;
    }
    String reason;
    if (!addStep(routines_[slot], remainder.substring(separator + 1), reason)) {
      error(source, requestId, reason);
    } else {
      saved_[slot] = false;
      publish(
        EventLevel::STATUS,
        source,
        requestId,
        "[ROUTINE] " + name + " step=" + String(routines_[slot].count) + " added: " + reason
      );
    }
    return true;
  }

  if (TextUtil::startsWithIgnoreCase(work, "RoutineRepeat:")) {
    String remainder = trimmedAfter(work, 14);
    const int separator = remainder.indexOf(':');
    if (separator <= 0) {
      error(source, requestId, "use RoutineRepeat:name:count");
      return true;
    }
    String name = remainder.substring(0, separator);
    name.trim();
    const int slot = findRoutine(name);
    uint32_t repeats = 0;
    const String repeatSpec = trimmedAfter(remainder, separator + 1);
    const bool continuous = repeatSpec.equalsIgnoreCase("FOREVER");
    if (slot < 0) {
      error(source, requestId, "unknown routine " + name);
    } else if (active_ && activeSlot_ == static_cast<uint8_t>(slot)) {
      error(source, requestId, "stop the active routine before editing it");
    } else if (!continuous && (!TextUtil::parseUnsigned32(repeatSpec, repeats) || repeats < 1)) {
      error(
        source,
        requestId,
        "repeat count must be positive or FOREVER"
      );
    } else {
      routines_[slot].repeatCount = repeats;
      saved_[slot] = false;
      publish(
        EventLevel::STATUS,
        source,
        requestId,
        "[ROUTINE] " + name + " repeats=" + String(repeats) + "; use RoutineSave to persist"
      );
    }
    return true;
  }

  static const char *const namedPrefixes[] = {
    "RoutineSave:", "RoutineRun:", "RoutineShow:", "RoutineErase:"
  };
  for (const char *prefix : namedPrefixes) {
    if (!TextUtil::startsWithIgnoreCase(work, prefix)) {
      continue;
    }
    const String name = trimmedAfter(work, strlen(prefix));
    const int slot = findRoutine(name);
    if (slot < 0) {
      error(source, requestId, "unknown routine " + name);
      return true;
    }
    if (String(prefix).equalsIgnoreCase("RoutineSave:")) {
      if (routines_[slot].count == 0) {
        error(source, requestId, "cannot save an empty routine");
      } else if (saveSlot(static_cast<uint8_t>(slot))) {
        publish(EventLevel::STATUS, source, requestId, "[ROUTINE] saved " + name);
      } else {
        error(source, requestId, "could not save routine " + name);
      }
    } else if (String(prefix).equalsIgnoreCase("RoutineRun:")) {
      startRoutine(static_cast<uint8_t>(slot), source, requestId);
    } else if (String(prefix).equalsIgnoreCase("RoutineShow:")) {
      showRoutine(static_cast<uint8_t>(slot), source, requestId);
    } else {
      if (active_ && activeSlot_ == static_cast<uint8_t>(slot)) {
        stop(source, requestId, "active routine erased", false);
      }
      if (eraseSlot(static_cast<uint8_t>(slot))) {
        publish(EventLevel::WARNING, source, requestId, "[ROUTINE] erased " + name);
      } else {
        error(source, requestId, "could not erase routine " + name);
      }
    }
    return true;
  }

  error(source, requestId, "unknown routine command; send Help");
  return true;
}

void RoutineEngine::service() {
  if (!active_) {
    return;
  }

  const uint32_t now = millis();
  String readiness;
  if (!addon_.canStartRoutine(readiness)) {
    finish(false, "hardware safety condition changed: " + readiness);
    return;
  }

  StoredRoutine &routine = routines_[activeSlot_];
  if (!routine.used || routine.count == 0) {
    finish(false, "active routine record became invalid");
    return;
  }

  if (stepIndex_ >= routine.count) {
    // Do not finish or repeat while the final submitted hardware action is
    // still active. This prevents a trailing dispense or motor move from being
    // truncated by the routine-completion safe shutdown.
    if (addon_.isBusy() || addon_.hasActiveOutput()) {
      return;
    }
    if (!routine.repeatCount || repeatIndex_ + 1 < routine.repeatCount) {
      // Saturate the displayed counter so START_WAIT never runs again on wrap.
      if (repeatIndex_ < UINT32_MAX - 1) repeatIndex_++;
      stepIndex_ = 0;
      waiting_ = false;
      publish(
        EventLevel::STATUS,
        runSource_,
        runRequestId_,
        "[ROUTINE] repeat " + String(repeatIndex_ + 1) + "/" + String(routine.repeatCount)
      );
      return;
    }
    finish(true, "routine complete");
    return;
  }

  const StoredStep &step = routine.steps[stepIndex_];
  const StepType type = static_cast<StepType>(step.type);

  if (type == StepType::START_WAIT && repeatIndex_ > 0) {
    stepIndex_++;
    return;
  }
  if (type == StepType::WAIT || type == StepType::START_WAIT) {
    if (!waiting_) {
      waiting_ = true;
      waitStartedAtMs_ = now;
      waitUntilMs_ = now + step.value;
      return;
    }
    if (static_cast<uint32_t>(now - waitStartedAtMs_) < step.value) {
      return;
    }
    waiting_ = false;
    stepIndex_++;
    return;
  }

  if (type == StepType::WAIT_IDLE) {
    if (!waiting_) {
      waiting_ = true;
      waitStartedAtMs_ = now;
    }
    if (!addon_.isBusy() && !addon_.hasActiveOutput()) {
      waiting_ = false;
      stepIndex_++;
      return;
    }
    return;
  }

  if (type == StepType::PIN_OUTPUT && waiting_) {
    const bool busy = addon_.isBusy() || addon_.hasActiveOutput();
    if (outputPhase_ == 0) {
      if (busy) outputPhase_ = 1;
      else if (now - waitStartedAtMs_ > 1000) finish(false, "output command did not start");
      return;
    }
    if (outputPhase_ == 1) {
      if (busy) return;
      outputPhase_ = 2; waitStartedAtMs_ = now;
    }
    if (now - waitStartedAtMs_ < step.value) return;
    waiting_ = false; stepIndex_++; return;
  }
  if (type == StepType::COMMAND || type == StepType::PIN_OUTPUT) {
    if (submitter_ == nullptr || submitContext_ == nullptr) {
      finish(false, "command dispatcher unavailable");
      return;
    }
    const String requestId = "routine-" + String(routine.name);
    if (!submitter_(submitContext_, CommandSource::INTERNAL, String(step.command), requestId)) {
      finish(false, "command queue rejected step " + String(stepIndex_ + 1));
      return;
    }
    publish(
      EventLevel::INFO,
      runSource_,
      runRequestId_,
      "[ROUTINE] step=" + String(stepIndex_ + 1) + " command=" + step.command
    );
    if (type == StepType::PIN_OUTPUT) {
      waiting_ = true; waitStartedAtMs_ = now;
      outputPhase_ = addon_.isBusy() || addon_.hasActiveOutput() ? 1 : 0;
    } else stepIndex_++;
    return;
  }

  finish(false, "routine contains an invalid step type");
}

bool RoutineEngine::stop(
  CommandSource source,
  const String &requestId,
  const String &reason,
  bool announce
) {
  const bool wasActive = active_;
  active_ = false;
  waiting_ = false;
  stepIndex_ = 0;
  repeatIndex_ = 0;
  const bool hardwareSafe = addon_.stopAll(source, requestId);
  lastResult_ = wasActive ? "stopped: " + reason : "idle";
  if (wasActive) lastRunSucceeded_ = false;
  if (announce) {
    publish(
      wasActive ? EventLevel::WARNING : EventLevel::INFO,
      source,
      requestId,
      wasActive ? "[ROUTINE] stopped and hardware made safe: " + reason : "[ROUTINE] already idle"
    );
  }
  return hardwareSafe;
}

bool RoutineEngine::isActive() const {
  return active_;
}

bool RoutineEngine::runNamed(const String &name, CommandSource source, const String &requestId) {
  const int slot = findRoutine(name);
  if (slot < 0 || !saved_[slot]) {
    error(source, requestId, "routine must be saved before coordinate execution");
    return false;
  }
  return startRoutine(static_cast<uint8_t>(slot), source, requestId);
}

bool RoutineEngine::hasSaved(const String &name) const {
  const int slot = findRoutine(name);
  return slot >= 0 && saved_[slot] && routines_[slot].count > 0;
}
bool RoutineEngine::lastRunSucceeded() const { return lastRunSucceeded_; }

String RoutineEngine::stateJson(bool compact) const {
  uint8_t stored = 0;
  for (const StoredRoutine &routine : routines_) {
    if (routine.used) {
      stored++;
    }
  }
  const uint32_t waitElapsed = millis() - waitStartedAtMs_;
  const uint32_t delayRemaining =
    active_ && stepIndex_ < routines_[activeSlot_].count && waiting_ &&
      static_cast<StepType>(routines_[activeSlot_].steps[stepIndex_].type) == StepType::START_WAIT &&
      waitElapsed < routines_[activeSlot_].steps[stepIndex_].value
      ? routines_[activeSlot_].steps[stepIndex_].value - waitElapsed : 0;
  char fields[256];
  snprintf(fields, sizeof(fields), "{\"active\":%s,\"stored\":%u,\"capacity\":%u,\"name\":\"%s\",\"step\":%u,\"steps\":%u,\"repeat\":%u,\"repeats\":%u,\"delayRemainingMs\":%lu",
    active_ ? "true" : "false", static_cast<unsigned>(stored), static_cast<unsigned>(AppConfig::ROUTINE_MAX_COUNT),
    active_ ? routines_[activeSlot_].name : "", static_cast<unsigned>(active_ ? stepIndex_ + 1 : 0),
    static_cast<unsigned>(active_ ? routines_[activeSlot_].count : 0), static_cast<unsigned>(active_ ? repeatIndex_ + 1 : 0),
    static_cast<unsigned>(active_ ? routines_[activeSlot_].repeatCount : 0), static_cast<unsigned long>(delayRemaining));
  String json(fields);
  // Compact arrays keep all four saved buttons within the bounded state budget.
  // [name, startDelayMs, pulseMs, gapMs, repeats, saved, stepCount, outputCount]
  json += ",\"library\":[";
  bool first = true;
  for (uint8_t slot = 0; slot < AppConfig::ROUTINE_MAX_COUNT; ++slot) {
    const StoredRoutine &routine = routines_[slot];
    if (!routine.used) continue;
    uint32_t delayMs = 0, pulseMs = 0, gapMs = 0;
    uint8_t pulseCount = 0, waitCount = 0, outputs = 0;
    bool simple = true;
    for (uint8_t i = 0; i < routine.count; ++i) {
      const StoredStep &step = routine.steps[i];
      if (static_cast<StepType>(step.type) == StepType::START_WAIT) delayMs = step.value;
      if (static_cast<StepType>(step.type) == StepType::WAIT) { gapMs = step.value; ++waitCount; }
      const String command(step.command);
      if (static_cast<StepType>(step.type) == StepType::PIN_OUTPUT) { ++outputs; simple = false; }
      if (static_cast<StepType>(step.type) == StepType::COMMAND) {
        if (TextUtil::startsWithIgnoreCase(command, "Dispense:")) {
          TextUtil::parseUnsigned32(command.substring(9), pulseMs); ++pulseCount;
        } else simple = false;
      }
    }
    if (!simple || pulseCount != 1 || waitCount > 1) pulseMs = 0;
    if (!first) json += ',';
    first = false;
    // Names were validated on create/load; quotes and escapes are disallowed.
    char entry[128];
    snprintf(entry, sizeof(entry), "[\"%s\",%lu,%lu,%lu,%u,%s,%u,%u]", routine.name,
      static_cast<unsigned long>(delayMs), static_cast<unsigned long>(pulseMs), static_cast<unsigned long>(gapMs),
      static_cast<unsigned>(routine.repeatCount), saved_[slot] ? "true" : "false", static_cast<unsigned>(routine.count), static_cast<unsigned>(outputs));
    json += entry;
  }
  json += "]";
  if (!compact) {
    json += ",\"elapsedMs\":" + String(active_ ? millis() - runStartedAtMs_ : 0);
    json += ",\"lastResult\":\"" + TextUtil::jsonEscape(lastResult_) + "\"";
  }
  json += "}";
  return json;
}

String RoutineEngine::statusText() const {
  uint8_t stored = 0;
  for (const StoredRoutine &routine : routines_) {
    if (routine.used) {
      stored++;
    }
  }
  char fields[192];
  snprintf(fields, sizeof(fields), "routines stored=%u/%u active=%s", static_cast<unsigned>(stored),
    static_cast<unsigned>(AppConfig::ROUTINE_MAX_COUNT), active_ ? "on" : "off");
  String text(fields);
  if (active_) {
    snprintf(fields, sizeof(fields), " name=%s step=%u/%u repeat=%u/%u elapsedMs=%lu", routines_[activeSlot_].name,
      static_cast<unsigned>(stepIndex_ + 1), static_cast<unsigned>(routines_[activeSlot_].count),
      static_cast<unsigned>(repeatIndex_ + 1), static_cast<unsigned>(routines_[activeSlot_].repeatCount),
      static_cast<unsigned long>(millis() - runStartedAtMs_));
    text += fields;
  }
  text += " last="; text += lastResult_;
  return text;
}

void RoutineEngine::publishHelp(CommandSource source, const String &requestId) const {
  publish(
    EventLevel::STATUS,
    source,
    requestId,
    "Routines: RoutineCreate:name RoutineAdd:name:START_WAIT:ms RoutineAdd:name:DISPENSE:ms RoutineAdd:name:OUTPUT:pin,pulse,gap RoutineAdd:name:WAIT:ms RoutineAdd:name:WAIT_IDLE"
  );
  publish(
    EventLevel::STATUS,
    source,
    requestId,
    "Routines: RoutineAdd:name:COMMAND:hardware-command RoutineRepeat:name:count RoutineSave:name RoutineRun:name"
  );
  publish(
    EventLevel::STATUS,
    source,
    requestId,
    "Routines: RoutineStop RoutineStatus RoutineList RoutineShow:name RoutineErase:name"
  );
}

int RoutineEngine::findRoutine(const String &name) const {
  for (uint8_t slot = 0; slot < AppConfig::ROUTINE_MAX_COUNT; slot++) {
    if (routines_[slot].used && String(routines_[slot].name).equalsIgnoreCase(name)) {
      return slot;
    }
  }
  return -1;
}

int RoutineEngine::findFreeSlot() const {
  for (uint8_t slot = 0; slot < AppConfig::ROUTINE_MAX_COUNT; slot++) {
    if (!routines_[slot].used) {
      return slot;
    }
  }
  return -1;
}

bool RoutineEngine::validName(const String &name) const {
  if (name.length() == 0 || name.length() > AppConfig::ROUTINE_NAME_BYTES) {
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

bool RoutineEngine::validStoredRoutine(const StoredRoutine &routine) const {
  if (
    routine.magic != ROUTINE_MAGIC ||
    routine.version != ROUTINE_VERSION ||
    routine.used != 1 ||
    routine.count == 0 ||
    routine.count > AppConfig::ROUTINE_MAX_STEPS ||
    routine.name[AppConfig::ROUTINE_NAME_BYTES] != '\0' ||
    !validName(String(routine.name)) ||
    routine.checksum != checksum(routine)
  ) {
    return false;
  }
  for (uint8_t index = 0; index < routine.count; index++) {
    const StoredStep &step = routine.steps[index];
    const StepType type = static_cast<StepType>(step.type);
    if ((type == StepType::WAIT || type == StepType::START_WAIT) && step.value > AppConfig::ROUTINE_MAX_WAIT_MS) {
      return false;
    }
    if (type == StepType::COMMAND || type == StepType::PIN_OUTPUT) {
      if (step.command[AppConfig::ROUTINE_COMMAND_BYTES] != '\0') {
        return false;
      }
      String reason;
      if (!safeRoutineCommand(String(step.command), reason)) {
        return false;
      }
      if (type == StepType::PIN_OUTPUT && !TextUtil::startsWithIgnoreCase(String(step.command), "DispensePin:")) return false;
    } else if (type != StepType::WAIT && type != StepType::WAIT_IDLE && type != StepType::START_WAIT) {
      return false;
    }
    if (type == StepType::START_WAIT && index != 0) return false;
  }
  return true;
}

bool RoutineEngine::safeRoutineCommand(const String &command, String &reason) const {
  String work = command;
  work.trim();
  if (work.length() == 0 || work.length() > AppConfig::ROUTINE_COMMAND_BYTES) {
    reason = "stored command is empty or too long";
    return false;
  }

  if (TextUtil::startsWithIgnoreCase(work, "Dispense:") || TextUtil::startsWithIgnoreCase(work, "DispensePin:")) {
    uint32_t durationMs = 0;
    int pin = -1;
    if (!TextUtil::parseDispense(work, pin, durationMs)) {
      reason = "stored dispense duration exceeds the configured safety limit";
      return false;
    }
    reason = "validated bounded dispense command";
    return true;
  }

  static const char *const allowedPrefixes[] = {
    "DAC1:MV:",
    "RPM:", "DEG:",
  };
  for (const char *prefix : allowedPrefixes) {
    if (TextUtil::startsWithIgnoreCase(work, prefix)) {
      reason = "validated hardware command";
      return true;
    }
  }

  static const char *const allowedExact[] = {
    "DAC1:ON", "DAC1:OFF", "DAC1:TEST3S",
    "MoveFullCW", "MoveFullCCW", "MoveHalfCW", "MoveHalfCCW",
    "TestFullSpeedRev", "TestHalfSpeedRev", "TestMinSpeedRev",
    "TestFullSpeedRevCCW", "TestHalfSpeedRevCCW", "TestMinSpeedRevCCW",
    "Stop", "CoilsOff", "DACAll:OFF", "OutputAll:OFF", "GPIO26:OFF"
  };
  for (const char *candidate : allowedExact) {
    if (work.equalsIgnoreCase(candidate)) {
      reason = "validated hardware command";
      return true;
    }
  }
  reason = "command is not on the routine hardware allowlist";
  return false;
}

uint32_t RoutineEngine::checksum(const StoredRoutine &routine) const {
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&routine);
  const size_t length = offsetof(StoredRoutine, checksum);
  uint32_t hash = 2166136261UL;
  for (size_t index = 0; index < length; index++) {
    hash ^= bytes[index];
    hash *= 16777619UL;
  }
  return hash;
}

void RoutineEngine::initializeRoutine(StoredRoutine &routine, const String &name) {
  memset(&routine, 0, sizeof(routine));
  routine.magic = ROUTINE_MAGIC;
  routine.version = ROUTINE_VERSION;
  routine.used = 1;
  routine.repeatCount = 1;
  name.toCharArray(routine.name, sizeof(routine.name));
}

bool RoutineEngine::addStep(StoredRoutine &routine, const String &specValue, String &reason) {
  if (routine.count >= AppConfig::ROUTINE_MAX_STEPS) {
    reason = "routine already has the maximum " + String(AppConfig::ROUTINE_MAX_STEPS) + " steps";
    return false;
  }

  String spec = specValue;
  spec.trim();
  StoredStep step{};

  const bool startWait = TextUtil::startsWithIgnoreCase(spec, "START_WAIT:");
  if (startWait && routine.count != 0) {
    reason = "START_WAIT must be the first step";
    return false;
  }
  if (TextUtil::startsWithIgnoreCase(spec, "OUTPUT:")) {
    // pin,pulse,gap. Keep the existing record size and ten-step capacity.
    const int first = spec.indexOf(',', 7);
    const int last = first < 0 ? -1 : spec.indexOf(',', first + 1);
    uint32_t pulse = 0; int pin = -1; String command;
    if (last >= 0) command = "DispensePin:" + spec.substring(7, last);
    if (last < 0 || !TextUtil::parseUnsigned32(spec.substring(last + 1), step.value) ||
        !TextUtil::parseDispense(command, pin, pulse)) { reason = "OUTPUT needs pin,pulseMs,gapMs"; return false; }
    step.type = static_cast<uint8_t>(StepType::PIN_OUTPUT);
    command.toCharArray(step.command, sizeof(step.command));
  } else if (startWait || TextUtil::startsWithIgnoreCase(spec, "WAIT:")) {
    uint32_t waitMs = 0;
    if (
      !TextUtil::parseUnsigned32(spec.substring(startWait ? 11 : 5), waitMs) ||
      waitMs > AppConfig::ROUTINE_MAX_WAIT_MS
    ) {
      reason = "WAIT must be 0 to " + String(AppConfig::ROUTINE_MAX_WAIT_MS) + " ms";
      return false;
    }
    step.type = static_cast<uint8_t>(startWait ? StepType::START_WAIT : StepType::WAIT);
    step.value = waitMs;
    reason = "WAIT " + String(waitMs) + " ms";
  } else if (spec.equalsIgnoreCase("WAIT_IDLE")) {
    step.type = static_cast<uint8_t>(StepType::WAIT_IDLE);
    reason = "WAIT_IDLE";
  } else {
    String command;
    if (TextUtil::startsWithIgnoreCase(spec, "DISPENSE:")) {
      uint32_t durationMs = 0;
      if (
        !TextUtil::parseUnsigned32(spec.substring(9), durationMs) ||
        durationMs == 0
      ) {
        reason = "DISPENSE must be 1 to 4294967295 ms";
        return false;
      }
      command = "Dispense:" + String(durationMs);
    } else if (TextUtil::startsWithIgnoreCase(spec, "COMMAND:")) {
      command = spec.substring(8);
      command.trim();
    } else {
      reason = "step must start with WAIT:, WAIT_IDLE, DISPENSE:, or COMMAND:";
      return false;
    }
    if (!safeRoutineCommand(command, reason)) {
      return false;
    }
    step.type = static_cast<uint8_t>(StepType::COMMAND);
    command.toCharArray(step.command, sizeof(step.command));
    reason = "COMMAND " + command;
  }

  routine.steps[routine.count++] = step;
  return true;
}

bool RoutineEngine::saveSlot(uint8_t slot) {
  if (slot >= AppConfig::ROUTINE_MAX_COUNT || !routines_[slot].used) {
    return false;
  }
  routines_[slot].checksum = checksum(routines_[slot]);
  Preferences preferences;
  if (!preferences.begin(PREFERENCES_NAMESPACE, false)) {
    return false;
  }
  const String key = slotKey(slot);
  const bool ok =
    preferences.putBytes(key.c_str(), &routines_[slot], sizeof(StoredRoutine)) ==
      sizeof(StoredRoutine);
  preferences.end();
  if (ok) saved_[slot] = true;
  return ok;
}

bool RoutineEngine::eraseSlot(uint8_t slot) {
  if (slot >= AppConfig::ROUTINE_MAX_COUNT) {
    return false;
  }
  Preferences preferences;
  if (!preferences.begin(PREFERENCES_NAMESPACE, false)) {
    return false;
  }
  bool ok = true;
  const String key = slotKey(slot);
  if (preferences.isKey(key.c_str())) {
    ok = preferences.remove(key.c_str());
  }
  preferences.end();
  if (ok) {
    memset(&routines_[slot], 0, sizeof(routines_[slot]));
    saved_[slot] = false;
  }
  return ok;
}

void RoutineEngine::showRoutine(
  uint8_t slot,
  CommandSource source,
  const String &requestId
) const {
  if (slot >= AppConfig::ROUTINE_MAX_COUNT || !routines_[slot].used) {
    return;
  }
  const StoredRoutine &routine = routines_[slot];
  publish(
    EventLevel::STATUS,
    source,
    requestId,
    "[ROUTINE] name=" + String(routine.name) + " steps=" + String(routine.count) +
      " repeats=" + String(routine.repeatCount)
  );
  for (uint8_t index = 0; index < routine.count; index++) {
    const StoredStep &step = routine.steps[index];
    String description;
    switch (static_cast<StepType>(step.type)) {
      case StepType::WAIT: description = "WAIT:" + String(step.value); break;
      case StepType::START_WAIT: description = "START_WAIT:" + String(step.value); break;
      case StepType::WAIT_IDLE: description = "WAIT_IDLE"; break;
      case StepType::COMMAND: description = "COMMAND:" + String(step.command); break;
      case StepType::PIN_OUTPUT: description = String(step.command) + " gapMs=" + String(step.value); break;
      case StepType::EMPTY:
      default: description = "INVALID"; break;
    }
    publish(
      EventLevel::STATUS,
      source,
      requestId,
      "[ROUTINE] step=" + String(index + 1) + " " + description
    );
  }
}

bool RoutineEngine::startRoutine(
  uint8_t slot,
  CommandSource source,
  const String &requestId
) {
  if (active_) {
    error(source, requestId, "routine " + String(routines_[activeSlot_].name) + " is already active");
    return false;
  }
  if (slot >= AppConfig::ROUTINE_MAX_COUNT || !routines_[slot].used || routines_[slot].count == 0) {
    error(source, requestId, "routine is empty or unavailable");
    return false;
  }
  if (addon_.isBusy() || addon_.hasActiveOutput()) {
    error(source, requestId, "routine start blocked: hardware is already busy or active");
    return false;
  }
  String readiness;
  if (!addon_.canStartRoutine(readiness)) {
    error(source, requestId, "routine start blocked: " + readiness);
    return false;
  }
  uint64_t onceMs = 0, cycleMs = 0;
  const StoredRoutine &routine = routines_[slot];
  for (uint8_t i = 0; i < routine.count; ++i) {
    const StoredStep &step = routine.steps[i];
    const StepType type = static_cast<StepType>(step.type);
    if (type == StepType::START_WAIT) onceMs += step.value;
    else if (type == StepType::WAIT) cycleMs += step.value;
    else if (type == StepType::COMMAND || type == StepType::PIN_OUTPUT) {
      const String command(step.command);
      if (!addon_.validateRoutineCommand(command, readiness)) {
        error(source, requestId, "routine start blocked: " + readiness); return false;
      }
      uint32_t pulse = 0;
      int pin = -1;
      if (TextUtil::parseDispense(command, pin, pulse)) cycleMs += pulse;
      if (type == StepType::PIN_OUTPUT) cycleMs += step.value;
    }
  }
  const uint64_t overhead = onceMs + 1000; // Service/queue margin.
  const uint64_t duration = !routine.repeatCount || (cycleMs && routine.repeatCount > (UINT64_MAX - overhead) / cycleMs)
    ? UINT64_MAX : overhead + cycleMs * routine.repeatCount;
  if (!addon_.canRunRoutineFor(duration, readiness)) {
    error(source, requestId, "routine exceeds optional arm limit: " + readiness); return false;
  }

  active_ = true;
  lastRunSucceeded_ = false;
  activeSlot_ = slot;
  stepIndex_ = 0;
  repeatIndex_ = 0;
  waiting_ = false;
  runStartedAtMs_ = millis();
  waitStartedAtMs_ = 0;
  waitUntilMs_ = 0;
  runSource_ = source;
  runRequestId_ = requestId;
  lastResult_ = "running " + String(routines_[slot].name);
  publish(
    EventLevel::WARNING,
    source,
    requestId,
    "[ROUTINE] started name=" + String(routines_[slot].name) +
      " steps=" + String(routines_[slot].count) +
      " repeats=" + String(routines_[slot].repeatCount)
  );
  return true;
}

void RoutineEngine::finish(bool success, const String &reason) {
  const String name = active_ ? String(routines_[activeSlot_].name) : String();
  const CommandSource source = runSource_;
  const String requestId = runRequestId_;
  active_ = false;
  lastRunSucceeded_ = success;
  waiting_ = false;
  stepIndex_ = 0;
  repeatIndex_ = 0;
  addon_.stopAll(CommandSource::INTERNAL, "routine-safe");
  lastResult_ = String(success ? "complete: " : "failed: ") + reason;
  publish(
    success ? EventLevel::STATUS : EventLevel::ERROR,
    source,
    requestId,
    String(success ? "[DONE] " : "[FAULT] ") + "routine " + name + " " + reason +
      "; hardware made safe"
  );
}

void RoutineEngine::publish(
  EventLevel level,
  CommandSource source,
  const String &requestId,
  const String &message
) const {
  events_.publish(level, message, source, requestId);
}

void RoutineEngine::error(
  CommandSource source,
  const String &requestId,
  const String &message
) const {
  publish(EventLevel::ERROR, source, requestId, "[ERROR] " + message);
}
