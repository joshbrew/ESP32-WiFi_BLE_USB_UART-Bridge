#include "StepperDacAddon.h"

#include <cstring>

#include "../../config/AppConfig.h"
#include "../../core/CommandDispatcher.h"
#include "../../core/EventBus.h"
#include "../../util/TextUtil.h"

#if APP_STEPPER_DAC_ADDON_ENABLED

namespace {
constexpr const char *STEPPER_DAC_NVS_NAMESPACE = "step-dac";

bool openAddonPreferences(Preferences &preferences, bool &created) {
  if (preferences.begin(STEPPER_DAC_NVS_NAMESPACE, true)) {
    created = false;
    return true;
  }
  created = true;
  return preferences.begin(STEPPER_DAC_NVS_NAMESPACE, false);
}

void appendDefaultNamespace(String &names, const char *name, bool created) {
  if (!created) {
    return;
  }
  if (names.length() > 0) {
    names += ", ";
  }
  names += name;
}
}  // namespace

StepperDacAddon::StepperDacAddon(
  EventBus &events,
  StepperController &stepper,
  DacController &dac
) : events_(events),
    stepper_(stepper),
    dac_(dac),
    snapshot_{},
    snapshotLoaded_(false) {}

const char *StepperDacAddon::name() const { return "stepper-dac"; }

void StepperDacAddon::begin() {
  Serial.println("[BOOT][ADDON] starting stepper task");
  Serial.flush();
  stepper_.begin();
  Serial.println("[BOOT][ADDON] starting DAC controller");
  Serial.flush();
  dac_.begin();
  events_.publish(EventLevel::STATUS, "[ADDON] stepper-dac ready", CommandSource::INTERNAL);
}

void StepperDacAddon::service() {
  dac_.service();
}

bool StepperDacAddon::canAcceptCommand(const String &command) const {
  return stepper_.canAcceptCommand(command);
}

bool StepperDacAddon::handleCommand(
  const String &command,
  CommandSource source,
  const String &requestId
) {
  if (dac_.handleCommand(command, source, requestId)) {
    return true;
  }
  return stepper_.handleCommand(command, source, requestId);
}

bool StepperDacAddon::stopAll(CommandSource source, const String &requestId) {
  const bool motorStopQueued = stepper_.emergencyStop(source, requestId);
  dac_.allOff(source, requestId, false);
  return motorStopQueued;
}

bool StepperDacAddon::isBusy() const { return stepper_.isBusy(); }
bool StepperDacAddon::hasActiveOutput() const { return dac_.anyOutputActive(); }
bool StepperDacAddon::hasTimedOperationActive() const { return dac_.hasTimedOperationActive(); }

void StepperDacAddon::appendStateJson(String &json, bool compact) const {
  json += ",\"addon\":{\"name\":\"stepper-dac\",\"active\":true}";
  json += ",\"motor\":" + String(compact ? stepper_.webStateJson() : stepper_.stateJson());
  json += ",\"dac\":" + String(compact ? dac_.webStateJson() : dac_.stateJson());
}

void StepperDacAddon::publishHelp(
  EventBus &events,
  CommandSource source,
  const String &requestId
) const {
  events.publish(EventLevel::STATUS, "Motor: Setup GetMotorStats CoilsOff Stop RPM:rpm,steps,dir DEG:rpm,degrees,dir", source, requestId);
  events.publish(EventLevel::STATUS, "Motor setup: SetMaxRPM SetMinRPM SetStartRPM SetRampRPM SetRevSteps StepMode StepOrder HoldTorque", source, requestId);
  events.publish(EventLevel::STATUS, "DAC: DACStatus DACRefMV DAC1:/DAC2: MV ON OFF BOOT TIMEOUT TEST3S; DACSave DACLoad DACDefaults DACErase", source, requestId);
}

void StepperDacAddon::publishChunked(
  EventBus &events,
  CommandSource source,
  const String &requestId,
  const String &prefix,
  const String &text
) const {
  constexpr size_t CHUNK = 132;
  if (text.length() == 0) {
    events.publish(EventLevel::STATUS, prefix, source, requestId);
    return;
  }
  const size_t length = static_cast<size_t>(text.length());
  for (size_t offset = 0; offset < length; offset += CHUNK) {
    const size_t end = offset + CHUNK < length ? offset + CHUNK : length;
    events.publish(
      EventLevel::STATUS,
      prefix + " " + text.substring(
        static_cast<unsigned int>(offset),
        static_cast<unsigned int>(end)
      ),
      source,
      requestId
    );
  }
}

void StepperDacAddon::publishStatus(
  EventBus &events,
  CommandSource source,
  const String &requestId,
  bool configuration
) const {
  const String label = configuration ? "[CONFIG]" : "[STATUS]";
  publishChunked(events, source, requestId, label + " motor", stepper_.statusText());
  publishChunked(events, source, requestId, label + " DAC", dac_.statusText());
}

void StepperDacAddon::queueStartupTests(CommandDispatcher &dispatcher) const {
  if (AppConfig::RUN_DAC1_BOOT_TEST) {
    dispatcher.submit(CommandSource::INTERNAL, "DAC1:TEST3S", "boot-dac1-test");
  }
  if (AppConfig::RUN_STEPPER_BOOT_TEST) {
    dispatcher.submit(CommandSource::INTERNAL, "RunStartupTest", "boot-stepper-test");
  }
}

bool StepperDacAddon::captureSelfTestState(
  Preferences &out,
  String &defaultNamespaces,
  String &reason
) {
  const StepperController::SettingsSnapshot motor = stepper_.settingsSnapshot();
  const DacController::SettingsSnapshot runtimeDac = dac_.settingsSnapshot();

  Preferences stored;
  bool created = false;
  if (!openAddonPreferences(stored, created)) {
    reason = "stepper-dac NVS unavailable";
    return false;
  }
  appendDefaultNamespace(defaultNamespaces, STEPPER_DAC_NVS_NAMESPACE, created);

  const bool valid = stored.getBool("valid", false);
  const uint16_t storedRef = stored.getUShort("refmv", AppConfig::DAC_DEFAULT_REFERENCE_MV);
  uint16_t storedMv[2];
  bool storedBoot[2];
  uint32_t storedTimeout[2];
  for (uint8_t i = 0; i < 2; i++) {
    const String suffix = String(i + 1);
    storedMv[i] = stored.getUShort(("mv" + suffix).c_str(), AppConfig::DAC_DEFAULT_MV);
    storedBoot[i] = stored.getBool(("boot" + suffix).c_str(), false);
    storedTimeout[i] = stored.getULong(("tout" + suffix).c_str(), 0);
  }
  stored.end();

  bool ok = out.putString("addon", name()) == strlen(name());
  ok = out.putBool("dvalid", valid) > 0 && ok;
  ok = out.putUShort("dref", storedRef) > 0 && ok;
  for (uint8_t i = 0; i < 2; i++) {
    const String suffix = String(i + 1);
    ok = out.putUShort(("dmv" + suffix).c_str(), storedMv[i]) > 0 && ok;
    ok = out.putBool(("dboot" + suffix).c_str(), storedBoot[i]) > 0 && ok;
    ok = out.putULong(("dtout" + suffix).c_str(), storedTimeout[i]) > 0 && ok;
  }

  ok = out.putLong("msteps", motor.stepsPerRevolution) > 0 && ok;
  ok = out.putFloat("mmin", motor.minRpm) > 0 && ok;
  ok = out.putFloat("mmax", motor.maxRpm) > 0 && ok;
  ok = out.putFloat("mstart", motor.startRpm) > 0 && ok;
  ok = out.putFloat("mramp", motor.rampRpmPerSecond) > 0 && ok;
  ok = out.putUInt("mint", motor.minIntervalUs) > 0 && ok;
  ok = out.putUChar("mmode", motor.stepMode) > 0 && ok;
  ok = out.putString("morder", motor.stepOrder) == motor.stepOrder.length() && ok;
  ok = out.putBool("mhold", motor.holdTorque) > 0 && ok;

  ok = out.putUShort("drref", runtimeDac.referenceMv) > 0 && ok;
  for (uint8_t i = 0; i < 2; i++) {
    const String suffix = String(i + 1);
    ok = out.putUShort(("drmv" + suffix).c_str(), runtimeDac.targetMv[i]) > 0 && ok;
    ok = out.putBool(("drboot" + suffix).c_str(), runtimeDac.bootEnabled[i]) > 0 && ok;
    ok = out.putUInt(("drtout" + suffix).c_str(), runtimeDac.maxOnMs[i]) > 0 && ok;
  }

  reason = ok ? "stepper-dac snapshot captured" : "stepper-dac snapshot write failed";
  return ok;
}

bool StepperDacAddon::loadSelfTestState(Preferences &in, String &reason) {
  if (in.getString("addon", "") != name()) {
    reason = "self-test snapshot belongs to a different addon";
    return false;
  }
  snapshot_.motorStepsPerRev = in.getLong("msteps", 2038);
  snapshot_.motorMinRpm = in.getFloat("mmin", AppConfig::STEPPER_DEFAULT_MIN_RPM);
  snapshot_.motorMaxRpm = in.getFloat("mmax", AppConfig::STEPPER_DEFAULT_MAX_RPM);
  snapshot_.motorStartRpm = in.getFloat("mstart", AppConfig::STEPPER_DEFAULT_START_RPM);
  snapshot_.motorRampRpm = in.getFloat("mramp", AppConfig::STEPPER_DEFAULT_RAMP_RPM_PER_SECOND);
  snapshot_.motorMinIntervalUs = in.getUInt("mint", 1);
  snapshot_.motorStepMode = in.getUChar("mmode", 8);
  snapshot_.motorStepOrder = in.getString("morder", "1234");
  snapshot_.motorHoldTorque = in.getBool("mhold", false);
  snapshot_.dacReferenceMv = in.getUShort("drref", AppConfig::DAC_DEFAULT_REFERENCE_MV);
  for (uint8_t i = 0; i < 2; i++) {
    const String suffix = String(i + 1);
    snapshot_.dacTargetMv[i] = in.getUShort(("drmv" + suffix).c_str(), AppConfig::DAC_DEFAULT_MV);
    snapshot_.dacBootEnabled[i] = in.getBool(("drboot" + suffix).c_str(), false);
    snapshot_.dacTimeoutMs[i] = in.getUInt(("drtout" + suffix).c_str(), 0);
  }
  snapshotLoaded_ = true;
  reason = "stepper-dac snapshot loaded";
  return true;
}

bool StepperDacAddon::restorePersistentSelfTestState(Preferences &snapshot, String &reason) {
  const bool valid = snapshot.getBool("dvalid", false);
  const uint16_t ref = snapshot.getUShort("dref", AppConfig::DAC_DEFAULT_REFERENCE_MV);
  uint16_t mv[2];
  bool boot[2];
  uint32_t timeout[2];
  for (uint8_t i = 0; i < 2; i++) {
    const String suffix = String(i + 1);
    mv[i] = snapshot.getUShort(("dmv" + suffix).c_str(), AppConfig::DAC_DEFAULT_MV);
    boot[i] = snapshot.getBool(("dboot" + suffix).c_str(), false);
    timeout[i] = snapshot.getUInt(("dtout" + suffix).c_str(), 0);
  }

  Preferences stored;
  if (!stored.begin(STEPPER_DAC_NVS_NAMESPACE, false)) {
    reason = "stepper-dac NVS unavailable";
    return false;
  }
  bool ok = stored.clear();
  if (valid) {
    ok = stored.putUShort("refmv", ref) > 0 && ok;
    for (uint8_t i = 0; i < 2; i++) {
      const String suffix = String(i + 1);
      ok = stored.putUShort(("mv" + suffix).c_str(), mv[i]) > 0 && ok;
      ok = stored.putBool(("boot" + suffix).c_str(), boot[i]) > 0 && ok;
      ok = stored.putULong(("tout" + suffix).c_str(), timeout[i]) > 0 && ok;
    }
    ok = stored.putBool("valid", true) > 0 && ok;
  }
  const bool verified = valid ? stored.getBool("valid", false) : !stored.getBool("valid", false);
  stored.end();
  reason = ok && verified ? "saved stepper-dac configuration restored" : "stepper-dac NVS verification failed";
  return ok && verified;
}

bool StepperDacAddon::verifyPersistentSelfTestState(Preferences &snapshot, String &reason) const {
  if (snapshot.getString("addon", "") != name()) {
    reason = "addon marker mismatch";
    return false;
  }
  const bool valid = snapshot.getBool("dvalid", false);
  const uint16_t ref = snapshot.getUShort("dref", AppConfig::DAC_DEFAULT_REFERENCE_MV);
  uint16_t mv[2];
  bool boot[2];
  uint32_t timeout[2];
  for (uint8_t i = 0; i < 2; i++) {
    const String suffix = String(i + 1);
    mv[i] = snapshot.getUShort(("dmv" + suffix).c_str(), AppConfig::DAC_DEFAULT_MV);
    boot[i] = snapshot.getBool(("dboot" + suffix).c_str(), false);
    timeout[i] = snapshot.getUInt(("dtout" + suffix).c_str(), 0);
  }

  Preferences stored;
  if (!stored.begin(STEPPER_DAC_NVS_NAMESPACE, true)) {
    reason = "stepper-dac NVS unavailable";
    return false;
  }
  bool ok = stored.getBool("valid", false) == valid;
  if (valid) {
    ok = stored.getUShort("refmv", 0) == ref && ok;
    for (uint8_t i = 0; i < 2; i++) {
      const String suffix = String(i + 1);
      ok = stored.getUShort(("mv" + suffix).c_str(), 0) == mv[i] && ok;
      ok = stored.getBool(("boot" + suffix).c_str(), !boot[i]) == boot[i] && ok;
      ok = stored.getULong(("tout" + suffix).c_str(), 0xFFFFFFFFUL) == timeout[i] && ok;
    }
  }
  stored.end();
  reason = ok ? "stepper-dac persisted fields match" : "stepper-dac persisted fields mismatch";
  return ok;
}

uint16_t StepperDacAddon::runtimeRestoreCommandCount() const {
  return snapshotLoaded_ ? 19 : 0;
}

String StepperDacAddon::runtimeRestoreCommand(uint16_t index) const {
  switch (index) {
    case 0: return "coilsoff";
    case 1: return "setminrpm:0.1";
    case 2: return "setmaxrpm:" + String(snapshot_.motorMaxRpm, 3);
    case 3: return "setminrpm:" + String(snapshot_.motorMinRpm, 3);
    case 4: return "setstartrpm:" + String(snapshot_.motorStartRpm, 3);
    case 5: return "setramprpm:" + String(snapshot_.motorRampRpm, 3);
    case 6: return "setminstepintervalus:" + String(snapshot_.motorMinIntervalUs);
    case 7: return "setrevsteps:" + String(snapshot_.motorStepsPerRev);
    case 8: return "stepmode:" + String(snapshot_.motorStepMode);
    case 9: return "steporder:" + snapshot_.motorStepOrder;
    case 10: return String("holdtorque:") + (snapshot_.motorHoldTorque ? "1" : "0");
    case 11: return "dacrefmv:" + String(snapshot_.dacReferenceMv);
    case 12: return "dac1:mv:" + String(snapshot_.dacTargetMv[0]);
    case 13: return String("dac1:boot:") + (snapshot_.dacBootEnabled[0] ? "on" : "off");
    case 14: return "dac1:timeoutms:" + String(snapshot_.dacTimeoutMs[0]);
    case 15: return "dac2:mv:" + String(snapshot_.dacTargetMv[1]);
    case 16: return String("dac2:boot:") + (snapshot_.dacBootEnabled[1] ? "on" : "off");
    case 17: return "dac2:timeoutms:" + String(snapshot_.dacTimeoutMs[1]);
    case 18: return "dacall:off";
    default: return String();
  }
}

#endif  // APP_STEPPER_DAC_ADDON_ENABLED
