#include "RoutineEngine.h"

#include <Preferences.h>
#include <ctype.h>
#include <stddef.h>
#include <string.h>

#include "../util/TextUtil.h"

namespace {

constexpr const char *PREFERENCES_NAMESPACE = "drone-routine";
constexpr uint32_t ROUTINE_MAGIC = 0x44524F4EUL;  // "DRON"
constexpr uint16_t ROUTINE_VERSION = 2;  // 48-byte command records.

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
  memset(routines_, 0, sizeof(routines_));

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
    if (
      preferences.getBytes(key.c_str(), &candidate, sizeof(candidate)) == sizeof(candidate) &&
      validStoredRoutine(candidate)
    ) {
      routines_[slot] = candidate;
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
    if (slot < 0) {
      error(source, requestId, "unknown routine " + name);
    } else if (active_ && activeSlot_ == static_cast<uint8_t>(slot)) {
      error(source, requestId, "stop the active routine before editing it");
    } else if (
      !TextUtil::parseUnsigned32(remainder.substring(separator + 1), repeats) ||
      repeats < 1 || repeats > AppConfig::ROUTINE_MAX_REPEATS
    ) {
      error(
        source,
        requestId,
        "repeat count must be 1 to " + String(AppConfig::ROUTINE_MAX_REPEATS)
      );
    } else {
      routines_[slot].repeatCount = static_cast<uint8_t>(repeats);
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
  if (static_cast<uint32_t>(now - runStartedAtMs_) >= AppConfig::ROUTINE_MAX_RUN_MS) {
    finish(false, "maximum routine run time exceeded");
    return;
  }

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
    if (repeatIndex_ + 1 < routine.repeatCount) {
      repeatIndex_++;
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

  if (type == StepType::WAIT) {
    if (!waiting_) {
      waiting_ = true;
      waitStartedAtMs_ = now;
      waitUntilMs_ = now + step.value;
      return;
    }
    if (static_cast<int32_t>(now - waitUntilMs_) < 0) {
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
    if (
      static_cast<uint32_t>(now - waitStartedAtMs_) >=
        AppConfig::ROUTINE_IDLE_WAIT_TIMEOUT_MS
    ) {
      finish(false, "WAIT_IDLE timed out");
    }
    return;
  }

  if (type == StepType::COMMAND) {
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
    stepIndex_++;
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

String RoutineEngine::stateJson(bool compact) const {
  uint8_t stored = 0;
  for (const StoredRoutine &routine : routines_) {
    if (routine.used) {
      stored++;
    }
  }
  String json = "{";
  json += "\"active\":" + TextUtil::jsonBool(active_);
  json += ",\"stored\":" + String(stored);
  json += ",\"capacity\":" + String(AppConfig::ROUTINE_MAX_COUNT);
  json += ",\"name\":\"";
  json += active_ ? TextUtil::jsonEscape(String(routines_[activeSlot_].name)) : "";
  json += "\"";
  json += ",\"step\":" + String(active_ ? stepIndex_ + 1 : 0);
  json += ",\"steps\":" + String(active_ ? routines_[activeSlot_].count : 0);
  json += ",\"repeat\":" + String(active_ ? repeatIndex_ + 1 : 0);
  json += ",\"repeats\":" + String(active_ ? routines_[activeSlot_].repeatCount : 0);
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
  String text = "routines stored=" + String(stored) + "/" + String(AppConfig::ROUTINE_MAX_COUNT);
  text += " active=" + TextUtil::boolWord(active_);
  if (active_) {
    text += " name=" + String(routines_[activeSlot_].name);
    text += " step=" + String(stepIndex_ + 1) + "/" + String(routines_[activeSlot_].count);
    text += " repeat=" + String(repeatIndex_ + 1) + "/" + String(routines_[activeSlot_].repeatCount);
    text += " elapsedMs=" + String(millis() - runStartedAtMs_);
  }
  text += " last=" + lastResult_;
  return text;
}

void RoutineEngine::publishHelp(CommandSource source, const String &requestId) const {
  publish(
    EventLevel::STATUS,
    source,
    requestId,
    "Routines: RoutineCreate:name RoutineAdd:name:DISPENSE:ms RoutineAdd:name:WAIT:ms RoutineAdd:name:WAIT_IDLE"
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
    routine.repeatCount == 0 ||
    routine.repeatCount > AppConfig::ROUTINE_MAX_REPEATS ||
    routine.name[AppConfig::ROUTINE_NAME_BYTES] != '\0' ||
    !validName(String(routine.name)) ||
    routine.checksum != checksum(routine)
  ) {
    return false;
  }
  for (uint8_t index = 0; index < routine.count; index++) {
    const StoredStep &step = routine.steps[index];
    const StepType type = static_cast<StepType>(step.type);
    if (type == StepType::WAIT && step.value > AppConfig::ROUTINE_MAX_WAIT_MS) {
      return false;
    }
    if (type == StepType::COMMAND) {
      if (step.command[AppConfig::ROUTINE_COMMAND_BYTES] != '\0') {
        return false;
      }
      String reason;
      if (!safeRoutineCommand(String(step.command), reason)) {
        return false;
      }
    } else if (type != StepType::WAIT && type != StepType::WAIT_IDLE) {
      return false;
    }
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

  if (TextUtil::startsWithIgnoreCase(work, "Dispense:")) {
    uint32_t durationMs = 0;
    if (
      !TextUtil::parseUnsigned32(work.substring(9), durationMs) ||
      durationMs == 0 || durationMs > AppConfig::DISPENSER_MAX_PULSE_MS
    ) {
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

  if (TextUtil::startsWithIgnoreCase(spec, "WAIT:")) {
    uint32_t waitMs = 0;
    if (
      !TextUtil::parseUnsigned32(spec.substring(5), waitMs) ||
      waitMs > AppConfig::ROUTINE_MAX_WAIT_MS
    ) {
      reason = "WAIT must be 0 to " + String(AppConfig::ROUTINE_MAX_WAIT_MS) + " ms";
      return false;
    }
    step.type = static_cast<uint8_t>(StepType::WAIT);
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
        durationMs == 0 || durationMs > AppConfig::DISPENSER_MAX_PULSE_MS
      ) {
        reason = "DISPENSE must be 1 to " + String(AppConfig::DISPENSER_MAX_PULSE_MS) + " ms";
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
      case StepType::WAIT_IDLE: description = "WAIT_IDLE"; break;
      case StepType::COMMAND: description = "COMMAND:" + String(step.command); break;
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

void RoutineEngine::startRoutine(
  uint8_t slot,
  CommandSource source,
  const String &requestId
) {
  if (active_) {
    error(source, requestId, "routine " + String(routines_[activeSlot_].name) + " is already active");
    return;
  }
  if (slot >= AppConfig::ROUTINE_MAX_COUNT || !routines_[slot].used || routines_[slot].count == 0) {
    error(source, requestId, "routine is empty or unavailable");
    return;
  }
  if (addon_.isBusy() || addon_.hasActiveOutput()) {
    error(source, requestId, "routine start blocked: hardware is already busy or active");
    return;
  }
  String readiness;
  if (!addon_.canStartRoutine(readiness)) {
    error(source, requestId, "routine start blocked: " + readiness);
    return;
  }

  active_ = true;
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
}

void RoutineEngine::finish(bool success, const String &reason) {
  const String name = active_ ? String(routines_[activeSlot_].name) : String();
  const CommandSource source = runSource_;
  const String requestId = runRequestId_;
  active_ = false;
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
