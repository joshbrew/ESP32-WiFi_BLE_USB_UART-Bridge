#include "CommandSelfTest.h"

#include <esp_system.h>

#include "../config/AppConfig.h"
#include "CommandDispatcher.h"
#include "../radio/RadioManager.h"
#include "../util/TextUtil.h"

namespace {

constexpr const char *SYSTEM_NAMESPACE = "step-system";
constexpr const char *RADIO_NAMESPACE = "step-radio";

constexpr uint32_t QUICK_WAIT_MS = 300;
constexpr uint32_t QUICK_TIMEOUT_MS = 2500;
constexpr uint32_t CONFIG_APPLY_WAIT_MS = 4200;
constexpr uint32_t INDICATOR_WAIT_MS = 5200;
constexpr uint32_t MOTION_TIMEOUT_MS = 90000;
constexpr uint32_t DAC_TIMEOUT_MS = 7000;
constexpr uint32_t FINAL_REBOOT_DELAY_MS = 1200;

bool requestMatches(const char *stored, const String &expected) {
  return expected.length() > 0 && String(stored) == expected;
}

String boolText(bool value) {
  return value ? "true" : "false";
}

bool openSnapshotPreferences(
  Preferences &preferences,
  const char *name,
  bool &createdEmptyNamespace
) {
  if (preferences.begin(name, true)) {
    createdEmptyNamespace = false;
    return true;
  }
  createdEmptyNamespace = true;
  return preferences.begin(name, false);
}

void appendSnapshotDefault(String &names, const char *name, bool createdEmptyNamespace) {
  if (!createdEmptyNamespace) {
    return;
  }
  if (names.length() > 0) {
    names += ", ";
  }
  names += name;
}

}  // namespace

CommandSelfTest::CommandSelfTest(
  EventBus &events,
  CommandDispatcher &dispatcher,
  DeviceAddon &addon,
  RadioManager &radios
) : events_(events),
    dispatcher_(dispatcher),
    addon_(addon),
    radios_(radios),
    phase_(Phase::IDLE),
    directIndex_(0),
    radioModeIndex_(0),
    radioSubIndex_(0),
    runtimeRestoreIndex_(0),
    passCount_(0),
    failCount_(0),
    skipCount_(0),
    runId_(0),
    bootCount_(0),
    active_(false),
    inflight_(false),
    awaitingReboot_(false),
    abortRequested_(false),
    targetRadioMode_(0),
    lastResult_("never run"),
    currentWaitKind_(WaitKind::QUICK),
    currentStartedAtMs_(0),
    currentTimeoutMs_(0),
    currentMinimumMs_(0),
    currentEventCursor_(0),
    currentRunSeen_(false),
    currentActivitySeen_(false),
    currentErrorSeen_(false) {}

const CommandSelfTest::TestCase *CommandSelfTest::directTests() const {
  static const TestCase tests[] = {
    {nullptr, "hElP", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "ping", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "status", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "configread", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "bootmodestatus", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "usbstatus", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "heapstatus", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "sendstatus", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "sendusb:selftest transport payload", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {"SendBLE delivery", nullptr, WaitKind::SKIP, 0, 0, SpecialAction::NONE, "requires a connected BLE client with TX notifications enabled"},
    {"SendWiFi delivery", nullptr, WaitKind::SKIP, 0, 0, SpecialAction::NONE, "requires a running portal with a recently active browser client"},
    {nullptr, "indicatorstatus", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "blestatus", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "selfteststart", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "selfteststatus", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "selftestresume", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {"SelfTestAbort", nullptr, WaitKind::SKIP, 0, 0, SpecialAction::NONE, "would intentionally terminate the sweep; abort/restore is reserved for the user"},
    {"SelfTestClear", nullptr, WaitKind::SKIP, 0, 0, SpecialAction::NONE, "cannot erase the active checkpoint record while the sweep is running"},
    {nullptr, "productionmode", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "debugmode", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {"Restore saved boot-test policy", nullptr, WaitKind::SPECIAL, QUICK_TIMEOUT_MS, 0, SpecialAction::RESTORE_SYSTEM, nullptr},
    {nullptr, "indicator16test", WaitKind::FIXED, INDICATOR_WAIT_MS + 1000, INDICATOR_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "indicator17test", WaitKind::FIXED, INDICATOR_WAIT_MS + 1000, INDICATOR_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "indicatortest", WaitKind::FIXED, INDICATOR_WAIT_MS + 2500, INDICATOR_WAIT_MS + 1400, SpecialAction::NONE, nullptr},

#if APP_STEPPER_DAC_ADDON_ENABLED
    {nullptr, "setup", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "printsteporder", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "getmotorstats", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "setmaxrpm:20", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "setminrpm:10", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "setstartrpm:10", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "setramprpm:1000", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "setminstepintervalus:100", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "setrevsteps:64", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "stepmode:4", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "stepmode:8", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "steporder:2143", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "nextsteporder", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "holdtorque:1", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "holdtorque:0", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "rpm:20,32,1", WaitKind::STEPPER, MOTION_TIMEOUT_MS, 150, SpecialAction::NONE, nullptr},
    {nullptr, "deg:20,90,1", WaitKind::STEPPER, MOTION_TIMEOUT_MS, 150, SpecialAction::NONE, nullptr},
    {nullptr, "deg:20,90,2", WaitKind::STEPPER, MOTION_TIMEOUT_MS, 150, SpecialAction::NONE, nullptr},
    {nullptr, "movefullcw", WaitKind::STEPPER, MOTION_TIMEOUT_MS, 150, SpecialAction::NONE, nullptr},
    {nullptr, "movefullccw", WaitKind::STEPPER, MOTION_TIMEOUT_MS, 150, SpecialAction::NONE, nullptr},
    {nullptr, "movehalfcw", WaitKind::STEPPER, MOTION_TIMEOUT_MS, 150, SpecialAction::NONE, nullptr},
    {nullptr, "movehalfccw", WaitKind::STEPPER, MOTION_TIMEOUT_MS, 150, SpecialAction::NONE, nullptr},
    {nullptr, "testfullspeedrev", WaitKind::STEPPER, MOTION_TIMEOUT_MS, 150, SpecialAction::NONE, nullptr},
    {nullptr, "testfullspeedrevccw", WaitKind::STEPPER, MOTION_TIMEOUT_MS, 150, SpecialAction::NONE, nullptr},
    {nullptr, "testhalfspeedrev", WaitKind::STEPPER, MOTION_TIMEOUT_MS, 150, SpecialAction::NONE, nullptr},
    {nullptr, "testhalfspeedrevccw", WaitKind::STEPPER, MOTION_TIMEOUT_MS, 150, SpecialAction::NONE, nullptr},
    {nullptr, "testminspeedrev", WaitKind::STEPPER, MOTION_TIMEOUT_MS, 150, SpecialAction::NONE, nullptr},
    {nullptr, "testminspeedrevccw", WaitKind::STEPPER, MOTION_TIMEOUT_MS, 150, SpecialAction::NONE, nullptr},
    {nullptr, "debugspincw", WaitKind::STEPPER, MOTION_TIMEOUT_MS, 150, SpecialAction::NONE, nullptr},
    {nullptr, "debugspinccw", WaitKind::STEPPER, MOTION_TIMEOUT_MS, 150, SpecialAction::NONE, nullptr},
    {nullptr, "runstartuptest", WaitKind::STEPPER, MOTION_TIMEOUT_MS, 150, SpecialAction::NONE, nullptr},
    {nullptr, "fastwipertest", WaitKind::STEPPER, MOTION_TIMEOUT_MS, 150, SpecialAction::NONE, nullptr},
    {nullptr, "stop", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "coilsoff", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "stopall", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},

    {nullptr, "dacstatus", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "dacrefmv:3200", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "dac1:mv:250", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "dac2:mv:300", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "dac1:boot:on", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "dac1:boot:off", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "dac2:boot:on", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "dac2:boot:off", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "dac1:timeoutms:500", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "dac2:timeoutsec:1", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "dac1:on", WaitKind::FIXED, QUICK_TIMEOUT_MS, 350, SpecialAction::NONE, nullptr},
    {nullptr, "dac1:off", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "dac2:on", WaitKind::FIXED, QUICK_TIMEOUT_MS, 350, SpecialAction::NONE, nullptr},
    {nullptr, "dac2:off", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "dacall:on", WaitKind::FIXED, QUICK_TIMEOUT_MS, 350, SpecialAction::NONE, nullptr},
    {nullptr, "dacall:off", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "dac1:test3s", WaitKind::DAC, DAC_TIMEOUT_MS, 150, SpecialAction::NONE, nullptr},
    {nullptr, "dac2:test3s", WaitKind::DAC, DAC_TIMEOUT_MS, 150, SpecialAction::NONE, nullptr},
    {nullptr, "dactest3s", WaitKind::DAC, DAC_TIMEOUT_MS, 150, SpecialAction::NONE, nullptr},
    {nullptr, "dacsave", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "dacload", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "dacdefaults", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "dacerase", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {"Restore addon snapshot", nullptr, WaitKind::SPECIAL, QUICK_TIMEOUT_MS, 0, SpecialAction::RESTORE_ADDON, nullptr},
#endif

    {nullptr, "radiostatus", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "wifi:off", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "wifi:on", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "ble:on", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "ble:off", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "classicbt:on", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "classicbt:off", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {"Restore radio after flag parsing", nullptr, WaitKind::SPECIAL, QUICK_TIMEOUT_MS, 0, SpecialAction::RESTORE_RADIO, nullptr},
    {nullptr, "configload", WaitKind::FIXED, CONFIG_APPLY_WAIT_MS + 1800, CONFIG_APPLY_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "wifimode:ap", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "wifimode:sta", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "wifimode:apsta", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "wififallbackap:off", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "wififallbackap:on", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "wifitxpower:low", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "wifitxpower:max", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "wifitxpower:13", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "wifistassid:SelfTest_Network_MiXeD", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "wifistapassword:SelfTestPass123", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "wifiapssid:SelfTest_AP_MiXeD", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "wifiappassword:SelfTestAP123", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "configsave", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "configload", WaitKind::FIXED, CONFIG_APPLY_WAIT_MS + 1800, CONFIG_APPLY_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "wifistaclear", WaitKind::FIXED, CONFIG_APPLY_WAIT_MS + 1800, CONFIG_APPLY_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "configdefaults", WaitKind::FIXED, CONFIG_APPLY_WAIT_MS + 1800, CONFIG_APPLY_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "configerase", WaitKind::QUICK, QUICK_TIMEOUT_MS, QUICK_WAIT_MS, SpecialAction::NONE, nullptr},
    {"Restore radio snapshot", nullptr, WaitKind::SPECIAL, QUICK_TIMEOUT_MS, 0, SpecialAction::RESTORE_RADIO, nullptr},
    {nullptr, "configload", WaitKind::FIXED, CONFIG_APPLY_WAIT_MS + 1800, CONFIG_APPLY_WAIT_MS, SpecialAction::NONE, nullptr},
    {nullptr, "configapply", WaitKind::FIXED, CONFIG_APPLY_WAIT_MS + 1000, CONFIG_APPLY_WAIT_MS, SpecialAction::NONE, nullptr},

    {"Real STA association", nullptr, WaitKind::SKIP, 0, 0, SpecialAction::NONE, "requires user-provided router credentials and network availability"},
    {"BLE browser client connection", nullptr, WaitKind::SKIP, 0, 0, SpecialAction::NONE, "requires a user gesture in the Web Bluetooth chooser"},
    {"Classic BT SPP pairing", nullptr, WaitKind::SKIP, 0, 0, SpecialAction::NONE, "requires an external Bluetooth client to pair and open SPP"},
    {"Runtime BLE UUID mutation", nullptr, WaitKind::SKIP, 0, 0, SpecialAction::NONE, "GATT UUIDs are compile-time constants and have no runtime command"},
    {"HTTP OTA firmware upload", nullptr, WaitKind::SKIP, 0, 0, SpecialAction::NONE, "requires a user-selected compiled .bin file; automatic tests do not flash arbitrary firmware"},
    {nullptr, nullptr, WaitKind::SKIP, 0, 0, SpecialAction::NONE, nullptr}
  };
  return tests;
}

uint16_t CommandSelfTest::directTestCount() const {
  const TestCase *tests = directTests();
  uint16_t count = 0;
  while (tests[count].name != nullptr || tests[count].command != nullptr) {
    count++;
  }
  return count;
}

void CommandSelfTest::begin() {
  loadState();
  if (!active_) {
    return;
  }

  bootCount_++;
  saveState();
  String snapshotReason;
  if (!loadSnapshot(snapshotReason)) {
    active_ = false;
    phase_ = Phase::COMPLETE;
    failCount_++;
    lastResult_ = "FAIL: self-test snapshot unavailable after reboot - " + snapshotReason;
    saveState();
    Serial.println("[BOOT][SELFTEST] " + lastResult_);
    Serial.flush();
    return;
  }

  if (inflight_ && phase_ == Phase::DIRECT) {
    failCount_++;
    lastResult_ = "FAIL: reset occurred before direct test completed";
    directIndex_++;
    inflight_ = false;
    saveState();
  } else if (inflight_ && phase_ == Phase::RESTORE_RUNTIME) {
    failCount_++;
    lastResult_ = "FAIL: reset occurred while restoring runtime settings";
    runtimeRestoreIndex_++;
    inflight_ = false;
    saveState();
  } else if (phase_ == Phase::REBOOT_TEST && awaitingReboot_) {
    passCount_++;
    awaitingReboot_ = false;
    phase_ = Phase::RESTORE_AND_REBOOT;
    lastResult_ = "PASS: Reboot returned to the persisted self-test checkpoint";
    saveState();
  } else if (phase_ == Phase::RESTORE_AND_REBOOT && awaitingReboot_) {
    awaitingReboot_ = false;
    phase_ = Phase::RESTORE_RUNTIME;
    runtimeRestoreIndex_ = 0;
    lastResult_ = "PASS: original persistent configuration booted";
    saveState();
  }

  announceResume();
}

void CommandSelfTest::service() {
  if (!active_) {
    return;
  }

  if (abortRequested_ && phase_ != Phase::ABORTING) {
    phase_ = Phase::ABORTING;
    inflight_ = false;
    awaitingReboot_ = false;
    saveState();
  }

  switch (phase_) {
    case Phase::DIRECT:
      serviceDirectTests();
      break;
    case Phase::RADIO_MODES:
      serviceRadioTests();
      break;
    case Phase::REBOOT_TEST:
      serviceRebootTest();
      break;
    case Phase::RESTORE_AND_REBOOT:
      serviceRestoreAndReboot();
      break;
    case Phase::RESTORE_RUNTIME:
      serviceRestoreRuntime();
      break;
    case Phase::ABORTING:
      serviceAborting();
      break;
    case Phase::COMPLETE:
    case Phase::IDLE:
    default:
      break;
  }
}

bool CommandSelfTest::handleCommand(
  const String &command,
  CommandSource source,
  const String &requestId
) {
  String work = command;
  work.trim();

  if (work.equalsIgnoreCase("SelfTestStart")) {
    start(source, requestId);
    return true;
  }
  if (work.equalsIgnoreCase("SelfTestStatus")) {
    publishStatus(EventLevel::STATUS, statusText(), source, requestId);
    return true;
  }
  if (work.equalsIgnoreCase("SelfTestAbort")) {
    requestAbort(source, requestId);
    return true;
  }
  if (work.equalsIgnoreCase("SelfTestClear")) {
    clearRecord(source, requestId);
    return true;
  }
  if (work.equalsIgnoreCase("SelfTestResume")) {
    if (active_) {
      publishStatus(EventLevel::STATUS, "[SELFTEST] already active; " + statusText(), source, requestId);
    } else {
      publishStatus(EventLevel::WARNING, "[SELFTEST] no active run to resume", source, requestId);
    }
    return true;
  }
  return false;
}

bool CommandSelfTest::isActive() const {
  return active_;
}

String CommandSelfTest::statusText() const {
  String text = "[SELFTEST] active=" + boolText(active_);
  text += " phase=" + phaseName();
  text += " progress=" + String(currentOrdinal()) + "/" + String(totalTestCount());
  text += " pass=" + String(passCount_);
  text += " fail=" + String(failCount_);
  text += " skip=" + String(skipCount_);
  text += " boots=" + String(bootCount_);
  text += " runId=" + String(runId_);
  if (currentName_.length() > 0) {
    text += " current=" + currentName_;
  }
  text += " last=" + lastResult_;
  return text;
}

String CommandSelfTest::stateJson() const {
  String json = "{";
  json += "\"active\":" + TextUtil::jsonBool(active_);
  json += ",\"phase\":\"" + TextUtil::jsonEscape(phaseName()) + "\"";
  json += ",\"current\":" + String(currentOrdinal());
  json += ",\"total\":" + String(totalTestCount());
  json += ",\"pass\":" + String(passCount_);
  json += ",\"fail\":" + String(failCount_);
  json += ",\"skip\":" + String(skipCount_);
  json += ",\"boots\":" + String(bootCount_);
  json += ",\"runId\":" + String(runId_);
  json += ",\"currentName\":\"" + TextUtil::jsonEscape(currentName_) + "\"";
  json += ",\"lastResult\":\"" + TextUtil::jsonEscape(lastResult_) + "\"";
  json += "}";
  return json;
}

String CommandSelfTest::webStateJson() const {
  String compactLastResult = lastResult_;
  if (compactLastResult.length() > 96) {
    compactLastResult.remove(96);
  }

  String json;
  json.reserve(224);
  json = "{\"active\":" + TextUtil::jsonBool(active_);
  json += ",\"phase\":\"" + TextUtil::jsonEscape(phaseName()) + "\"";
  json += ",\"current\":" + String(currentOrdinal());
  json += ",\"total\":" + String(totalTestCount());
  json += ",\"pass\":" + String(passCount_);
  json += ",\"fail\":" + String(failCount_);
  json += ",\"skip\":" + String(skipCount_);
  json += ",\"boots\":" + String(bootCount_);
  json += ",\"lastResult\":\"" + TextUtil::jsonEscape(compactLastResult) + "\"";
  json += "}";
  return json;
}

void CommandSelfTest::start(CommandSource source, const String &requestId) {
  if (active_) {
    publishStatus(EventLevel::WARNING, "[SELFTEST] already active; " + statusText(), source, requestId);
    return;
  }
  if (addon_.isBusy()) {
    publishStatus(EventLevel::ERROR, "[SELFTEST] start rejected: stop the active addon first", source, requestId);
    return;
  }
  if (addon_.hasActiveOutput()) {
    publishStatus(EventLevel::ERROR, "[SELFTEST] start rejected: release active addon outputs first", source, requestId);
    return;
  }
  if (radios_.bootTransactionBusy()) {
    publishStatus(EventLevel::ERROR, "[SELFTEST] start rejected: wait for the current radio transaction to finish", source, requestId);
    return;
  }

  String reason;
  if (!captureSnapshot(reason)) {
    publishStatus(EventLevel::ERROR, "[SELFTEST] could not capture restore snapshot: " + reason, source, requestId);
    return;
  }
  if (reason.indexOf("default state") >= 0) {
    publishStatus(
      EventLevel::WARNING,
      "[SELFTEST] " + reason + "; those namespaces had no saved configuration and will be restored to that default state",
      source,
      requestId
    );
  }

  active_ = true;
  phase_ = Phase::DIRECT;
  directIndex_ = 0;
  radioModeIndex_ = 0;
  radioSubIndex_ = 0;
  runtimeRestoreIndex_ = 0;
  passCount_ = 0;
  failCount_ = 0;
  skipCount_ = 0;
  bootCount_ = 0;
  runId_ = static_cast<uint32_t>(esp_random());
  inflight_ = false;
  awaitingReboot_ = false;
  abortRequested_ = false;
  targetRadioMode_ = 0;
  lastResult_ = reason + "; starting";
  resetTransientState();
  saveState();

  publishStatus(
    EventLevel::WARNING,
    "[SELFTEST] START runId=" + String(runId_) +
      " tests=" + String(totalTestCount()) +
      " addon=" + String(addon_.name()) + " tests and multiple radio reboots may occur; captured NVS/default state and runtime tuning will be restored",
    source,
    requestId
  );
  publishStatus(
    EventLevel::STATUS,
    "[SELFTEST] BUILD wifi=" + String(AppConfig::ENABLE_WIFI ? "included" : "excluded") +
      " ble=" + String(AppConfig::ENABLE_BLE ? "included" : "excluded") +
      " spp=" + String(AppConfig::ENABLE_CLASSIC_BT_SPP ? "included" : "excluded") +
      " httpOta=" + String(AppConfig::ENABLE_HTTP_OTA ? "included" : "excluded") +
      " indicators=" + String(AppConfig::ENABLE_STATUS_INDICATORS ? "included" : "excluded") +
      " bootLedTest=" + String(AppConfig::STATUS_LED_BOOT_SELF_TEST ? "included" : "excluded") +
      " addon=" + String(addon_.name()) +
      "; excluded-stack enable commands must be rejected and dependent tests are skipped",
    source,
    requestId
  );
}

void CommandSelfTest::requestAbort(CommandSource source, const String &requestId) {
  if (!active_) {
    publishStatus(EventLevel::WARNING, "[SELFTEST] no active run", source, requestId);
    return;
  }
  abortRequested_ = true;
  saveState();
  dispatcher_.clearPending();
  addon_.stopAll(source, requestId);
  publishStatus(EventLevel::WARNING, "[SELFTEST] abort requested; restoring the captured NVS snapshot and rebooting", source, requestId);
}

void CommandSelfTest::clearRecord(CommandSource source, const String &requestId) {
  if (active_) {
    publishStatus(EventLevel::ERROR, "[SELFTEST] cannot clear an active run; use SelfTestAbort", source, requestId);
    return;
  }
  Preferences preferences;
  if (!preferences.begin(NAMESPACE, false)) {
    publishStatus(EventLevel::ERROR, "[SELFTEST] storage unavailable", source, requestId);
    return;
  }
  const bool cleared = preferences.clear();
  preferences.end();
  phase_ = Phase::IDLE;
  passCount_ = failCount_ = skipCount_ = 0;
  runId_ = 0;
  bootCount_ = 0;
  lastResult_ = "record cleared";
  publishStatus(cleared ? EventLevel::STATUS : EventLevel::ERROR, cleared ? "[SELFTEST] saved report cleared" : "[SELFTEST] could not clear saved report", source, requestId);
}

void CommandSelfTest::serviceDirectTests() {
  if (directIndex_ >= directTestCount()) {
    phase_ = Phase::RADIO_MODES;
    radioModeIndex_ = 0;
    radioSubIndex_ = 0;
    inflight_ = false;
    saveState();
    publishStatus(EventLevel::STATUS, "[SELFTEST] direct command sweep complete; starting transactional radio profile sweep");
    return;
  }

  const TestCase &test = directTests()[directIndex_];
  if (!inflight_) {
    startDirectTest(test);
    return;
  }
  serviceRunningDirectTest(test);
}

void CommandSelfTest::startDirectTest(const TestCase &test) {
  const String displayName = test.name != nullptr
    ? String(test.name)
    : String(test.command == nullptr ? "unnamed" : test.command);

  const String compileSkipReason = compileAwareSkipReason(test);
  if (compileSkipReason.length() > 0) {
    recordSkip(displayName, compileSkipReason);
    directIndex_++;
    saveState();
    return;
  }

  if (test.waitKind == WaitKind::SKIP) {
    recordSkip(displayName, test.skipReason == nullptr ? "manual interaction required" : test.skipReason);
    directIndex_++;
    saveState();
    return;
  }

  if (test.waitKind == WaitKind::SPECIAL) {
    String reason;
    const bool ok = executeSpecial(test.special, reason);
    currentName_ = displayName;
    finishCurrentTest(ok, reason);
    directIndex_++;
    saveState();
    return;
  }

  submitCurrentCommand(
    displayName,
    String(test.command == nullptr ? "" : test.command),
    test.waitKind,
    test.timeoutMs,
    test.minimumMs
  );
}

void CommandSelfTest::serviceRunningDirectTest(const TestCase &test) {
  scanCurrentTestEvents();
  const uint32_t elapsed = static_cast<uint32_t>(millis() - currentStartedAtMs_);
  const String expectedGuardError = expectedCompileGuardError(test);

  if (currentErrorSeen_) {
    const bool expectedRejection =
      expectedGuardError.length() > 0 &&
      currentErrorText_.indexOf(expectedGuardError) >= 0;
    String reason = currentErrorText_;
    if (expectedRejection) {
      reason = "compile guard correctly rejected the excluded stack";
    }
    finishCurrentTest(expectedRejection, reason);
    directIndex_++;
    saveState();
    return;
  }
  if (elapsed >= currentTimeoutMs_) {
    String reason = "timeout after " + String(elapsed) + " ms";
    if (expectedGuardError.length() > 0) {
      reason = "expected compile guard rejection was not observed";
    }
    finishCurrentTest(false, reason);
    directIndex_++;
    saveState();
    return;
  }
  if (!currentRunSeen_ || elapsed < currentMinimumMs_) {
    return;
  }

  if (expectedGuardError.length() > 0) {
    finishCurrentTest(false, "excluded stack accepted an enable command instead of rejecting it");
    directIndex_++;
    saveState();
    return;
  }

  if (currentWaitKind_ == WaitKind::STEPPER) {
    if (addon_.isBusy()) {
      currentActivitySeen_ = true;
      return;
    }
    if (!currentActivitySeen_ && elapsed < 800) {
      return;
    }
  } else if (currentWaitKind_ == WaitKind::DAC) {
    if (addon_.hasTimedOperationActive()) {
      currentActivitySeen_ = true;
      return;
    }
    if (!currentActivitySeen_ && elapsed < 800) {
      return;
    }
  }

  finishCurrentTest(true, "completed in " + String(elapsed) + " ms");
  directIndex_++;
  saveState();
}

String CommandSelfTest::compileAwareSkipReason(const TestCase &test) const {
  String command = test.command == nullptr ? String() : String(test.command);
  String name = test.name == nullptr ? String() : String(test.name);
  command.toLowerCase();
  name.toLowerCase();

  const bool requiresWifi =
    command.startsWith("wifimode:") ||
    command.startsWith("wififallbackap:") ||
    command.startsWith("wifitxpower:") ||
    command.startsWith("wifistassid:") ||
    command.startsWith("wifistapassword:") ||
    command.startsWith("wifiapssid:") ||
    command.startsWith("wifiappassword:") ||
    command == "wifistaclear" ||
    name == "real sta association";

  const bool requiresBle =
    name == "ble browser client connection" ||
    name == "runtime ble uuid mutation";

  const bool requiresSpp = name == "classic bt spp pairing";
  const bool requiresHttpOta = name == "http ota firmware upload";

  if (requiresWifi && !AppConfig::ENABLE_WIFI) {
    return "requires the compiled Wi-Fi stack";
  }
  if (requiresBle && !AppConfig::ENABLE_BLE) {
    return "requires the compiled BLE stack";
  }
  if (requiresSpp && !AppConfig::ENABLE_CLASSIC_BT_SPP) {
    return "requires the compiled Classic Bluetooth SPP stack";
  }
  if (requiresHttpOta && (!AppConfig::ENABLE_WIFI || !AppConfig::ENABLE_HTTP_OTA)) {
    return "requires compiled Wi-Fi and HTTP OTA support";
  }
  return String();
}

String CommandSelfTest::expectedCompileGuardError(const TestCase &test) const {
  if (test.command == nullptr) {
    return String();
  }

  const String command(test.command);
  if (!AppConfig::ENABLE_WIFI && command.equalsIgnoreCase("WiFi:ON")) {
    return "WiFi is compiled out";
  }
  if (!AppConfig::ENABLE_BLE && command.equalsIgnoreCase("BLE:ON")) {
    return "BLE is compiled out";
  }
  if (
    !AppConfig::ENABLE_CLASSIC_BT_SPP &&
    (command.equalsIgnoreCase("ClassicBT:ON") || command.equalsIgnoreCase("SPP:ON"))
  ) {
    return "Classic Bluetooth SPP is compiled out";
  }
  return String();
}

bool CommandSelfTest::executeSpecial(SpecialAction action, String &reason) {
  switch (action) {
    case SpecialAction::RESTORE_SYSTEM:
      return restoreSystemNvs(reason);
    case SpecialAction::RESTORE_ADDON:
      return restoreAddonPersistent(reason);
    case SpecialAction::RESTORE_RADIO:
      return restoreRadioNvs(reason);
    case SpecialAction::NONE:
    default:
      reason = "no special action";
      return true;
  }
}

void CommandSelfTest::scanCurrentTestEvents() {
  EventRecord record{};
  bool gap = false;
  while (events_.nextAfter(currentEventCursor_, record, gap)) {
    if (!requestMatches(record.requestId, currentRequestId_)) {
      continue;
    }
    const String text(record.text);
    if (text.indexOf("[RUN]") >= 0) {
      currentRunSeen_ = true;
    }
    if (record.level == EventLevel::ERROR) {
      currentErrorSeen_ = true;
      currentErrorText_ = text;
    }
  }
}

void CommandSelfTest::finishCurrentTest(bool passed, const String &reason) {
  if (passed) {
    passCount_++;
  } else {
    failCount_++;
  }
  lastResult_ = String(passed ? "PASS: " : "FAIL: ") + currentName_ + " - " + reason;
  publishStatus(
    passed ? EventLevel::STATUS : EventLevel::ERROR,
    "[SELFTEST] TEST " + String(currentOrdinal()) + "/" + String(totalTestCount()) +
      " " + String(passed ? "PASS " : "FAIL ") + currentName_ + " - " + reason
  );
  inflight_ = false;
  resetTransientState();
}

void CommandSelfTest::recordSkip(const String &name, const String &reason) {
  skipCount_++;
  lastResult_ = "SKIP: " + name + " - " + reason;
  publishStatus(
    EventLevel::WARNING,
    "[SELFTEST] TEST " + String(currentOrdinal()) + "/" + String(totalTestCount()) +
      " SKIP " + name + " - " + reason
  );
}

void CommandSelfTest::serviceRadioTests() {
  if (radioModeIndex_ >= radioModeCount()) {
    phase_ = Phase::REBOOT_TEST;
    awaitingReboot_ = false;
    inflight_ = false;
    saveState();
    publishStatus(EventLevel::STATUS, "[SELFTEST] all boot radio profiles exercised; testing generic Reboot persistence next");
    return;
  }

  if (radioSubIndex_ == 0) {
    if (!awaitingReboot_) {
      beginRadioModeCommand();
      return;
    }

    if (radios_.bootTransactionBusy()) {
      return;
    }

    const String target = radioModeName(radioModeIndex_);
    String healthReason;
    const bool activeMatches = radios_.activeBootModeText() == target;
    const bool healthy = activeMatches && radios_.activeProfileHealthy(healthReason);
    finishRadioMode(
      healthy,
      healthy ? "profile active and healthy" :
        "expected=" + target + " active=" + radios_.activeBootModeText() + " reason=" + healthReason
    );
    return;
  }

  const uint8_t commandIndex = static_cast<uint8_t>(radioSubIndex_ - 1U);
  if (commandIndex >= profileCommandCount(radioModeIndex_)) {
    radioModeIndex_++;
    radioSubIndex_ = 0;
    awaitingReboot_ = false;
    inflight_ = false;
    saveState();
    return;
  }

  if (!inflight_) {
    const String name = profileCommandName(radioModeIndex_, commandIndex);
    const String command = profileCommand(radioModeIndex_, commandIndex);
    const bool restart = command.equalsIgnoreCase("webrestart") || command.equalsIgnoreCase("configapply");
    submitCurrentCommand(
      name,
      command,
      restart ? WaitKind::FIXED : WaitKind::QUICK,
      restart ? CONFIG_APPLY_WAIT_MS + 1000 : QUICK_TIMEOUT_MS,
      restart ? CONFIG_APPLY_WAIT_MS : QUICK_WAIT_MS
    );
    return;
  }

  scanCurrentTestEvents();
  const uint32_t elapsed = static_cast<uint32_t>(millis() - currentStartedAtMs_);
  if (currentErrorSeen_) {
    finishCurrentTest(false, currentErrorText_);
    radioSubIndex_++;
    saveState();
  } else if (elapsed >= currentTimeoutMs_) {
    finishCurrentTest(false, "timeout after " + String(elapsed) + " ms");
    radioSubIndex_++;
    saveState();
  } else if (currentRunSeen_ && elapsed >= currentMinimumMs_) {
    finishCurrentTest(true, "completed in " + String(elapsed) + " ms");
    radioSubIndex_++;
    saveState();
  }
}

void CommandSelfTest::serviceRebootTest() {
  if (!awaitingReboot_) {
    currentName_ = "Reboot";
    currentCommand_ = "reboot";
    currentRequestId_ = "selftest-reboot";
    awaitingReboot_ = true;
    inflight_ = true;
    saveState();
    publishStatus(
      EventLevel::WARNING,
      "[SELFTEST] TEST " + String(currentOrdinal()) + "/" + String(totalTestCount()) +
        " RUN Reboot command=reboot; checkpoint saved"
    );
    if (!dispatcher_.submit(CommandSource::INTERNAL, currentCommand_, currentRequestId_)) {
      awaitingReboot_ = false;
      inflight_ = false;
      failCount_++;
      lastResult_ = "FAIL: Reboot - dispatcher rejected command";
      publishStatus(EventLevel::ERROR, "[SELFTEST] TEST " + String(currentOrdinal()) + "/" + String(totalTestCount()) + " FAIL Reboot - dispatcher rejected command");
      phase_ = Phase::RESTORE_AND_REBOOT;
      saveState();
    }
  }
}

void CommandSelfTest::serviceRestoreAndReboot() {
  if (awaitingReboot_) {
    return;
  }

  String reason;
  dispatcher_.clearPending();
  addon_.stopAll(CommandSource::INTERNAL, "selftest-restore");

  if (!restoreAllPersistent(reason)) {
    lastResult_ = "RETRY: persistent restore - " + reason;
    publishStatus(EventLevel::ERROR, "[SELFTEST] restore failed: " + reason + "; retrying on the next service cycle");
    delay(10);
    return;
  }

  awaitingReboot_ = true;
  inflight_ = false;
  saveState();
  publishStatus(
    EventLevel::WARNING,
    "[SELFTEST] persistent snapshot restored; rebooting into original saved radio profile with addon=" +
      String(addon_.name())
  );
  Serial.flush();
  delay(FINAL_REBOOT_DELAY_MS);
  ESP.restart();
}

void CommandSelfTest::serviceRestoreRuntime() {
  const uint16_t count = runtimeRestoreCommandCount();
  if (runtimeRestoreIndex_ >= count) {
    String reason;
    const bool verified = verifyPersistentRestore(reason);
    if (verified) {
      passCount_++;
      lastResult_ = "PASS: original saved configuration and runtime tuning restored";
    } else {
      failCount_++;
      lastResult_ = "FAIL: final restore verification - " + reason;
    }

    active_ = false;
    phase_ = Phase::COMPLETE;
    inflight_ = false;
    awaitingReboot_ = false;
    abortRequested_ = false;
    saveState();
    publishStatus(
      verified ? EventLevel::STATUS : EventLevel::ERROR,
      "[SELFTEST] COMPLETE runId=" + String(runId_) +
        " pass=" + String(passCount_) +
        " fail=" + String(failCount_) +
        " skip=" + String(skipCount_) +
        " boots=" + String(bootCount_) +
        "; original NVS and runtime settings restored; addon left in its safe stopped state"
    );
    return;
  }

  if (!inflight_) {
    const String command = runtimeRestoreCommand(runtimeRestoreIndex_);
    submitCurrentCommand(
      "RestoreRuntime " + String(runtimeRestoreIndex_ + 1U),
      command,
      WaitKind::QUICK,
      QUICK_TIMEOUT_MS,
      QUICK_WAIT_MS
    );
    return;
  }

  scanCurrentTestEvents();
  const uint32_t elapsed = static_cast<uint32_t>(millis() - currentStartedAtMs_);
  if (currentErrorSeen_) {
    finishCurrentTest(false, currentErrorText_);
    runtimeRestoreIndex_++;
    saveState();
  } else if (elapsed >= currentTimeoutMs_) {
    finishCurrentTest(false, "restore timeout");
    runtimeRestoreIndex_++;
    saveState();
  } else if (currentRunSeen_ && elapsed >= currentMinimumMs_) {
    finishCurrentTest(true, "runtime setting restored");
    runtimeRestoreIndex_++;
    saveState();
  }
}

void CommandSelfTest::serviceAborting() {
  String reason;
  dispatcher_.clearPending();
  addon_.stopAll(CommandSource::INTERNAL, "selftest-abort");
  if (!restoreAllPersistent(reason)) {
    publishStatus(EventLevel::ERROR, "[SELFTEST] abort restore retry: " + reason);
    delay(10);
    return;
  }
  awaitingReboot_ = true;
  phase_ = Phase::RESTORE_AND_REBOOT;
  abortRequested_ = false;
  saveState();
  publishStatus(EventLevel::WARNING, "[SELFTEST] abort snapshot restored; rebooting to original profile");
  Serial.flush();
  delay(FINAL_REBOOT_DELAY_MS);
  ESP.restart();
}

bool CommandSelfTest::captureSnapshot(String &reason) {
  String defaultNamespaces;

  Preferences system;
  bool systemCreated = false;
  if (!openSnapshotPreferences(system, SYSTEM_NAMESPACE, systemCreated)) {
    reason = "system NVS unavailable";
    return false;
  }
  const bool production = system.getBool("production", false);
  system.end();
  appendSnapshotDefault(defaultNamespaces, SYSTEM_NAMESPACE, systemCreated);

  struct RadioSaved {
    bool valid;
    uint8_t profile;
    bool wifiEnabled;
    bool bleEnabled;
    bool sppEnabled;
    bool fallback;
    uint8_t mode;
    int8_t txq;
    String staSsid;
    String staPass;
    String apSsid;
    String apPass;
    uint8_t lastGood;
    uint32_t sequence;
  } radio{};

  Preferences rp;
  bool radioCreated = false;
  if (!openSnapshotPreferences(rp, RADIO_NAMESPACE, radioCreated)) {
    reason = "radio NVS unavailable";
    return false;
  }
  appendSnapshotDefault(defaultNamespaces, RADIO_NAMESPACE, radioCreated);
  radio.valid = rp.getBool("valid", false);
  radio.profile = rp.getUChar("profile", radios_.activeBootModeValue());
  radio.wifiEnabled = rp.getBool("wifi", radio.profile == 0 || radio.profile == 1 || radio.profile == 5);
  radio.bleEnabled = rp.getBool("ble", radio.profile == 1 || radio.profile == 2 || radio.profile == 5);
  radio.sppEnabled = rp.getBool("spp", radio.profile == 3);
  radio.fallback = rp.getBool("fallback", true);
  radio.mode = rp.getUChar("mode", 2);
  radio.txq = rp.getChar("txq", AppConfig::WIFI_TX_POWER_MAX_QUARTER_DBM);
  radio.staSsid = rp.getString("stassid", "");
  radio.staPass = rp.getString("stapass", "");
  radio.apSsid = rp.getString("apssid", AppConfig::DEFAULT_WIFI_AP_SSID);
  radio.apPass = rp.getString("appass", AppConfig::DEFAULT_WIFI_AP_PASSWORD);
  radio.lastGood = rp.getUChar("lastgood", radio.profile);
  radio.sequence = rp.getUInt("txnseq", 0);
  rp.end();

  Preferences out;
  if (!out.begin(NAMESPACE, false)) {
    reason = "self-test NVS unavailable";
    return false;
  }
  out.clear();
  bool ok = out.putBool("snapvalid", true) > 0;
  ok = out.putBool("sprod", production) > 0 && ok;
  ok = out.putBool("rvalid", radio.valid) > 0 && ok;
  ok = out.putUChar("rprofile", radio.profile) > 0 && ok;
  ok = out.putBool("rwifi", radio.wifiEnabled) > 0 && ok;
  ok = out.putBool("rble", radio.bleEnabled) > 0 && ok;
  ok = out.putBool("rspp", radio.sppEnabled) > 0 && ok;
  ok = out.putBool("rfallback", radio.fallback) > 0 && ok;
  ok = out.putUChar("rmode", radio.mode) > 0 && ok;
  ok = out.putChar("rtxq", radio.txq) > 0 && ok;
  ok = out.putString("rstassid", radio.staSsid) == radio.staSsid.length() && ok;
  ok = out.putString("rstapass", radio.staPass) == radio.staPass.length() && ok;
  ok = out.putString("rapssid", radio.apSsid) == radio.apSsid.length() && ok;
  ok = out.putString("rappass", radio.apPass) == radio.apPass.length() && ok;
  ok = out.putUChar("rlastgood", radio.lastGood) > 0 && ok;
  ok = out.putUInt("rseq", radio.sequence) > 0 && ok;

  String addonReason;
  ok = addon_.captureSelfTestState(out, defaultNamespaces, addonReason) && ok;
  out.end();

  if (!ok || !loadSnapshot(reason)) {
    if (reason.length() == 0) {
      reason = addonReason.length() > 0 ? addonReason : String("snapshot write verification failed");
    }
    return false;
  }
  reason = defaultNamespaces.length() > 0
    ? "snapshot captured with default state for " + defaultNamespaces
    : "snapshot captured addon=" + String(addon_.name());
  return true;
}

bool CommandSelfTest::loadSnapshot(String &reason) {
  Preferences in;
  if (!in.begin(NAMESPACE, true)) {
    reason = "self-test NVS unavailable";
    return false;
  }
  if (!in.getBool("snapvalid", false)) {
    in.end();
    reason = "snapshot marker missing";
    return false;
  }
  String addonReason;
  const bool ok = addon_.loadSelfTestState(in, addonReason);
  in.end();
  reason = ok ? "loaded addon=" + String(addon_.name()) : addonReason;
  return ok;
}

bool CommandSelfTest::restoreSystemNvs(String &reason) {
  Preferences snapshot;
  if (!snapshot.begin(NAMESPACE, true)) {
    reason = "snapshot storage unavailable";
    return false;
  }
  const bool production = snapshot.getBool("sprod", false);
  snapshot.end();

  Preferences system;
  if (!system.begin(SYSTEM_NAMESPACE, false)) {
    reason = "system NVS unavailable";
    return false;
  }
  const bool ok = system.putBool("production", production) > 0;
  const bool verified = ok && system.getBool("production", !production) == production;
  system.end();
  reason = verified ? "saved boot-test policy restored" : "boot-test policy verification failed";
  return verified;
}

bool CommandSelfTest::restoreAddonPersistent(String &reason) {
  Preferences snapshot;
  if (!snapshot.begin(NAMESPACE, true)) {
    reason = "snapshot storage unavailable";
    return false;
  }
  const bool ok = addon_.restorePersistentSelfTestState(snapshot, reason);
  snapshot.end();
  return ok;
}

bool CommandSelfTest::restoreRadioNvs(String &reason) {
  Preferences snapshot;
  if (!snapshot.begin(NAMESPACE, true)) {
    reason = "snapshot storage unavailable";
    return false;
  }
  const bool valid = snapshot.getBool("rvalid", false);
  const uint8_t profile = snapshot.getUChar("rprofile", 0);
  const bool wifiEnabled = snapshot.getBool("rwifi", profile == 0 || profile == 1);
  const bool bleEnabled = snapshot.getBool("rble", profile == 1 || profile == 2);
  const bool sppEnabled = snapshot.getBool("rspp", profile == 3);
  const bool fallback = snapshot.getBool("rfallback", true);
  const uint8_t mode = snapshot.getUChar("rmode", 2);
  const int8_t txq = snapshot.getChar("rtxq", AppConfig::WIFI_TX_POWER_MAX_QUARTER_DBM);
  const String staSsid = snapshot.getString("rstassid", "");
  const String staPass = snapshot.getString("rstapass", "");
  const String apSsid = snapshot.getString("rapssid", AppConfig::DEFAULT_WIFI_AP_SSID);
  const String apPass = snapshot.getString("rappass", AppConfig::DEFAULT_WIFI_AP_PASSWORD);
  const uint8_t lastGood = snapshot.getUChar("rlastgood", profile);
  const uint32_t sequence = snapshot.getUInt("rseq", 0);
  snapshot.end();

  Preferences radio;
  if (!radio.begin(RADIO_NAMESPACE, false)) {
    reason = "radio NVS unavailable";
    return false;
  }
  bool ok = radio.clear();
  if (valid) {
    ok = radio.putUChar("profile", profile) > 0 && ok;
    ok = radio.putBool("wifi", wifiEnabled) > 0 && ok;
    ok = radio.putBool("ble", bleEnabled) > 0 && ok;
    ok = radio.putBool("spp", sppEnabled) > 0 && ok;
    ok = radio.putBool("fallback", fallback) > 0 && ok;
    ok = radio.putUChar("mode", mode) > 0 && ok;
    ok = radio.putChar("txq", txq) > 0 && ok;
    ok = radio.putString("stassid", staSsid) == staSsid.length() && ok;
    ok = radio.putString("stapass", staPass) == staPass.length() && ok;
    ok = radio.putString("apssid", apSsid) == apSsid.length() && ok;
    ok = radio.putString("appass", apPass) == apPass.length() && ok;
    ok = radio.putBool("valid", true) > 0 && ok;
  }
  ok = radio.putUChar("lastgood", lastGood) > 0 && ok;
  ok = radio.putUChar("txnprofile", profile) > 0 && ok;
  ok = radio.putUInt("txnseq", sequence) > 0 && ok;
  ok = radio.putBool("txnstarted", false) > 0 && ok;
  ok = radio.putBool("txnpending", false) > 0 && ok;

  const bool verified =
    ok &&
    !radio.getBool("txnpending", true) &&
    !radio.getBool("txnstarted", true) &&
    radio.getUChar("lastgood", 255) == lastGood &&
    radio.getUInt("txnseq", 0xFFFFFFFFUL) == sequence &&
    (
      !valid ||
      (
        radio.getBool("valid", false) &&
        radio.getUChar("profile", 255) == profile &&
        radio.getBool("wifi", !wifiEnabled) == wifiEnabled &&
        radio.getBool("ble", !bleEnabled) == bleEnabled &&
        radio.getBool("spp", !sppEnabled) == sppEnabled &&
        radio.getBool("fallback", !fallback) == fallback &&
        radio.getUChar("mode", 255) == mode &&
        radio.getChar("txq", 127) == txq &&
        radio.getString("stassid", "") == staSsid &&
        radio.getString("stapass", "") == staPass &&
        radio.getString("apssid", "") == apSsid &&
        radio.getString("appass", "") == apPass
      )
    );
  radio.end();
  reason = verified ? "saved radio configuration and transaction markers restored" : "radio NVS verification failed";
  return verified;
}

bool CommandSelfTest::restoreAllPersistent(String &reason) {
  String part;
  if (!restoreSystemNvs(part)) {
    reason = part;
    return false;
  }
  if (!restoreAddonPersistent(part)) {
    reason = part;
    return false;
  }
  if (!restoreRadioNvs(part)) {
    reason = part;
    return false;
  }
  reason = "all persistent namespaces restored";
  return true;
}

bool CommandSelfTest::verifyPersistentRestore(String &reason) {
  Preferences snapshot;
  if (!snapshot.begin(NAMESPACE, true)) {
    reason = "snapshot unavailable";
    return false;
  }
  const bool production = snapshot.getBool("sprod", false);
  const bool radioValid = snapshot.getBool("rvalid", false);
  const uint8_t radioProfile = snapshot.getUChar("rprofile", 0);
  const bool radioWifi = snapshot.getBool("rwifi", radioProfile == 0 || radioProfile == 1 || radioProfile == 5);
  const bool radioBle = snapshot.getBool("rble", radioProfile == 1 || radioProfile == 2 || radioProfile == 5);
  const bool radioSpp = snapshot.getBool("rspp", radioProfile == 3);
  const bool radioFallback = snapshot.getBool("rfallback", true);
  const uint8_t radioRole = snapshot.getUChar("rmode", 2);
  const int8_t radioTxq = snapshot.getChar("rtxq", AppConfig::WIFI_TX_POWER_MAX_QUARTER_DBM);
  const String radioStaSsid = snapshot.getString("rstassid", "");
  const String radioStaPass = snapshot.getString("rstapass", "");
  const String radioApSsid = snapshot.getString("rapssid", AppConfig::DEFAULT_WIFI_AP_SSID);
  const String radioApPass = snapshot.getString("rappass", AppConfig::DEFAULT_WIFI_AP_PASSWORD);
  const uint8_t radioLastGood = snapshot.getUChar("rlastgood", radioProfile);
  const uint32_t radioSequence = snapshot.getUInt("rseq", 0);

  String addonReason;
  const bool addonOk = addon_.verifyPersistentSelfTestState(snapshot, addonReason);
  snapshot.end();

  Preferences system;
  if (!system.begin(SYSTEM_NAMESPACE, true)) {
    reason = "system NVS unavailable";
    return false;
  }
  const bool systemOk = system.getBool("production", !production) == production;
  system.end();

  Preferences radio;
  if (!radio.begin(RADIO_NAMESPACE, true)) {
    reason = "radio NVS unavailable";
    return false;
  }
  bool radioOk =
    radio.getBool("valid", false) == radioValid &&
    radio.getUChar("lastgood", 255) == radioLastGood &&
    radio.getUInt("txnseq", 0xFFFFFFFFUL) == radioSequence &&
    !radio.getBool("txnpending", true) &&
    !radio.getBool("txnstarted", true);
  if (radioValid) {
    radioOk =
      radio.getUChar("profile", 255) == radioProfile &&
      radio.getBool("wifi", !radioWifi) == radioWifi &&
      radio.getBool("ble", !radioBle) == radioBle &&
      radio.getBool("spp", !radioSpp) == radioSpp &&
      radio.getBool("fallback", !radioFallback) == radioFallback &&
      radio.getUChar("mode", 255) == radioRole &&
      radio.getChar("txq", 127) == radioTxq &&
      radio.getString("stassid", "") == radioStaSsid &&
      radio.getString("stapass", "") == radioStaPass &&
      radio.getString("apssid", "") == radioApSsid &&
      radio.getString("appass", "") == radioApPass &&
      radioOk;
  }
  radio.end();

  const bool activeModeOk = !radioValid || radios_.activeBootModeValue() == radioProfile;
  if (!systemOk || !radioOk || !addonOk || !activeModeOk) {
    reason = "system=" + boolText(systemOk) +
      " radio=" + boolText(radioOk) +
      " addon=" + boolText(addonOk) +
      " activeMode=" + boolText(activeModeOk) +
      (addonOk ? String() : " addonReason=" + addonReason);
    return false;
  }
  reason = "all persisted fields, addon state, and active boot profile match the captured snapshot";
  return true;
}

String CommandSelfTest::runtimeRestoreCommand(uint16_t index) const {
  return addon_.runtimeRestoreCommand(index);
}

uint16_t CommandSelfTest::runtimeRestoreCommandCount() const {
  return addon_.runtimeRestoreCommandCount();
}

void CommandSelfTest::beginRadioModeCommand() {
  targetRadioMode_ = radioModeIndex_;
  awaitingReboot_ = true;
  inflight_ = false;
  saveState();
  const String name = "Mode" + radioModeName(radioModeIndex_);
  const String command = radioModeCommand(radioModeIndex_);
  publishStatus(
    EventLevel::WARNING,
    "[SELFTEST] TEST " + String(currentOrdinal()) + "/" + String(totalTestCount()) +
      " RUN " + name + " command=" + command + "; reboot checkpoint saved"
  );
  if (!dispatcher_.submit(CommandSource::INTERNAL, command, "selftest-mode")) {
    awaitingReboot_ = false;
    finishRadioMode(false, "dispatcher rejected radio mode command");
  }
}

void CommandSelfTest::finishRadioMode(bool passed, const String &reason) {
  currentName_ = "Mode" + radioModeName(radioModeIndex_);
  if (passed) {
    passCount_++;
  } else {
    failCount_++;
  }
  lastResult_ = String(passed ? "PASS: " : "FAIL: ") + currentName_ + " - " + reason;
  publishStatus(
    passed ? EventLevel::STATUS : EventLevel::ERROR,
    "[SELFTEST] TEST " + String(currentOrdinal()) + "/" + String(totalTestCount()) +
      " " + String(passed ? "PASS " : "FAIL ") + currentName_ + " - " + reason
  );
  awaitingReboot_ = false;
  radioSubIndex_ = 1;
  saveState();
}

uint8_t CommandSelfTest::radioModeCount() const {
  uint8_t count = 1;
  if (AppConfig::ENABLE_WIFI) count++;
  if (AppConfig::ENABLE_WIFI && AppConfig::ENABLE_BLE) count += 2;
  if (AppConfig::ENABLE_BLE) count++;
  if (AppConfig::ENABLE_CLASSIC_BT_SPP) count++;
  return count;
}

String CommandSelfTest::radioModeName(uint8_t index) const {
  if (AppConfig::ENABLE_WIFI) {
    if (index == 0) return "WIFI";
    index--;
  }
  if (AppConfig::ENABLE_WIFI && AppConfig::ENABLE_BLE) {
    if (index == 0) return "WIFI_BLE";
    index--;
    if (index == 0) return "WIFI_BLE_P";
    index--;
  }
  if (AppConfig::ENABLE_BLE) {
    if (index == 0) return "BLE";
    index--;
  }
  if (AppConfig::ENABLE_CLASSIC_BT_SPP) {
    if (index == 0) return "SPP";
    index--;
  }
  return index == 0 ? "USB" : "UNKNOWN";
}

String CommandSelfTest::radioModeCommand(uint8_t index) const {
  const String mode = radioModeName(index);
  if (mode == "WIFI") return "modewifi";
  if (mode == "WIFI_BLE") return "modewifible";
  if (mode == "WIFI_BLE_P") return "modewifiblep";
  if (mode == "BLE") return "modeble";
  if (mode == "SPP") return "modebtserial";
  if (mode == "USB") return "modeusb";
  return "modeusb";
}

uint8_t CommandSelfTest::profileCommandCount(uint8_t modeIndex) const {
  const String mode = radioModeName(modeIndex);
  if (mode == "WIFI") return 3;
  if (mode == "WIFI_BLE" || mode == "WIFI_BLE_P") return 4;
  if (mode == "BLE") return 2;
  if (mode == "SPP") return 1;
  if (mode == "USB") return 2;
  return 0;
}

String CommandSelfTest::profileCommand(uint8_t modeIndex, uint8_t commandIndex) const {
  static const char *const wifi[] = {"radiostatus", "webrestart", "configapply"};
  static const char *const wifiBle[] = {"bleadvertise", "blewebhandoff", "blewebcancel", "radiostatus"};
  static const char *const ble[] = {"bleadvertise", "blestatus"};
  static const char *const spp[] = {"radiostatus"};
  static const char *const usb[] = {"usbstatus", "radiostatus"};

  const String mode = radioModeName(modeIndex);
  if (mode == "WIFI" && commandIndex < 3) return String(wifi[commandIndex]);
  if ((mode == "WIFI_BLE" || mode == "WIFI_BLE_P") && commandIndex < 4) return String(wifiBle[commandIndex]);
  if (mode == "BLE" && commandIndex < 2) return String(ble[commandIndex]);
  if (mode == "SPP" && commandIndex < 1) return String(spp[commandIndex]);
  if (mode == "USB" && commandIndex < 2) return String(usb[commandIndex]);
  return "status";
}

String CommandSelfTest::profileCommandName(uint8_t modeIndex, uint8_t commandIndex) const {
  return radioModeName(modeIndex) + " " + profileCommand(modeIndex, commandIndex);
}

void CommandSelfTest::submitCurrentCommand(
  const String &name,
  const String &command,
  WaitKind waitKind,
  uint32_t timeoutMs,
  uint32_t minimumMs
) {
  currentName_ = name;
  currentCommand_ = command;
  currentRequestId_ = "selftest-" + String(currentOrdinal());
  if (currentRequestId_.length() > AppConfig::REQUEST_ID_MAX_BYTES) {
    currentRequestId_.remove(AppConfig::REQUEST_ID_MAX_BYTES);
  }
  currentWaitKind_ = waitKind;
  currentStartedAtMs_ = millis();
  currentTimeoutMs_ = timeoutMs;
  currentMinimumMs_ = minimumMs;
  currentEventCursor_ = events_.latestId();
  currentRunSeen_ = false;
  currentActivitySeen_ = false;
  currentErrorSeen_ = false;
  currentErrorText_ = "";
  inflight_ = true;
  saveState();

  publishStatus(
    EventLevel::STATUS,
    "[SELFTEST] TEST " + String(currentOrdinal()) + "/" + String(totalTestCount()) +
      " RUN " + currentName_ + " command=" + safeCommandForLog(currentCommand_)
  );

  if (!dispatcher_.submit(CommandSource::INTERNAL, currentCommand_, currentRequestId_)) {
    currentErrorSeen_ = true;
    currentErrorText_ = "dispatcher rejected command";
  }
}

bool CommandSelfTest::currentCommandFinished() const {
  return !inflight_;
}

uint16_t CommandSelfTest::totalTestCount() const {
  uint16_t total = directTestCount();
  for (uint8_t mode = 0; mode < radioModeCount(); mode++) {
    total += 1U + profileCommandCount(mode);
  }
  total += 1U;
  total += runtimeRestoreCommandCount();
  total += 1U;
  return total;
}

uint16_t CommandSelfTest::currentOrdinal() const {
  if (phase_ == Phase::DIRECT) {
    return static_cast<uint16_t>(directIndex_ + 1U);
  }

  uint16_t ordinal = directTestCount();
  if (phase_ == Phase::RADIO_MODES) {
    for (uint8_t mode = 0; mode < radioModeIndex_; mode++) {
      ordinal += 1U + profileCommandCount(mode);
    }
    ordinal += radioSubIndex_ == 0 ? 1U : static_cast<uint16_t>(1U + radioSubIndex_);
    return ordinal;
  }

  for (uint8_t mode = 0; mode < radioModeCount(); mode++) {
    ordinal += 1U + profileCommandCount(mode);
  }
  if (phase_ == Phase::REBOOT_TEST || phase_ == Phase::RESTORE_AND_REBOOT) {
    return static_cast<uint16_t>(ordinal + 1U);
  }
  ordinal += 1U;
  if (phase_ == Phase::RESTORE_RUNTIME) {
    return static_cast<uint16_t>(ordinal + runtimeRestoreIndex_ + 1U);
  }
  return totalTestCount();
}

bool CommandSelfTest::loadState() {
  Preferences preferences;
  if (!preferences.begin(NAMESPACE, true)) {
    return false;
  }
  active_ = preferences.getBool("active", false);
  phase_ = static_cast<Phase>(preferences.getUChar("phase", static_cast<uint8_t>(Phase::IDLE)));
  directIndex_ = preferences.getUShort("direct", 0);
  radioModeIndex_ = preferences.getUChar("rmodeidx", 0);
  radioSubIndex_ = preferences.getUChar("rsubidx", 0);
  runtimeRestoreIndex_ = preferences.getUShort("restoreidx", 0);
  passCount_ = preferences.getUShort("passes", 0);
  failCount_ = preferences.getUShort("fails", 0);
  skipCount_ = preferences.getUShort("skips", 0);
  runId_ = preferences.getUInt("runid", 0);
  bootCount_ = preferences.getUInt("boots", 0);
  inflight_ = preferences.getBool("inflight", false);
  awaitingReboot_ = preferences.getBool("awaitboot", false);
  abortRequested_ = preferences.getBool("abort", false);
  targetRadioMode_ = preferences.getUChar("target", 0);
  lastResult_ = preferences.getString("last", "never run");
  currentName_ = preferences.getString("cur", "");
  preferences.end();
  return true;
}

bool CommandSelfTest::saveState() {
  Preferences preferences;
  if (!preferences.begin(NAMESPACE, false)) {
    return false;
  }
  bool ok = preferences.putBool("active", active_) > 0;
  ok = preferences.putUChar("phase", static_cast<uint8_t>(phase_)) > 0 && ok;
  ok = preferences.putUShort("direct", directIndex_) > 0 && ok;
  ok = preferences.putUChar("rmodeidx", radioModeIndex_) > 0 && ok;
  ok = preferences.putUChar("rsubidx", radioSubIndex_) > 0 && ok;
  ok = preferences.putUShort("restoreidx", runtimeRestoreIndex_) > 0 && ok;
  ok = preferences.putUShort("passes", passCount_) > 0 && ok;
  ok = preferences.putUShort("fails", failCount_) > 0 && ok;
  ok = preferences.putUShort("skips", skipCount_) > 0 && ok;
  ok = preferences.putUInt("runid", runId_) > 0 && ok;
  ok = preferences.putUInt("boots", bootCount_) > 0 && ok;
  ok = preferences.putBool("inflight", inflight_) > 0 && ok;
  ok = preferences.putBool("awaitboot", awaitingReboot_) > 0 && ok;
  ok = preferences.putBool("abort", abortRequested_) > 0 && ok;
  ok = preferences.putUChar("target", targetRadioMode_) > 0 && ok;
  ok = preferences.putString("last", lastResult_) == lastResult_.length() && ok;
  ok = preferences.putString("cur", currentName_) == currentName_.length() && ok;
  preferences.end();
  return ok;
}

void CommandSelfTest::resetTransientState() {
  currentName_ = "";
  currentCommand_ = "";
  currentRequestId_ = "";
  currentStartedAtMs_ = 0;
  currentTimeoutMs_ = 0;
  currentMinimumMs_ = 0;
  currentEventCursor_ = events_.latestId();
  currentRunSeen_ = false;
  currentActivitySeen_ = false;
  currentErrorSeen_ = false;
  currentErrorText_ = "";
}

void CommandSelfTest::announceResume() {
  const String message =
    "[BOOT][SELFTEST] RESUME runId=" + String(runId_) +
    " phase=" + phaseName() +
    " test=" + String(currentOrdinal()) + "/" + String(totalTestCount()) +
    " pass=" + String(passCount_) +
    " fail=" + String(failCount_) +
    " skip=" + String(skipCount_) +
    " boots=" + String(bootCount_);
  Serial.println(message);
  Serial.flush();
  events_.publish(EventLevel::STATUS, message, CommandSource::INTERNAL, "selftest-resume");
}

void CommandSelfTest::publishStatus(
  EventLevel level,
  const String &message,
  CommandSource source,
  const String &requestId
) {
  events_.publish(level, message, source, requestId);
}

String CommandSelfTest::phaseName() const {
  switch (phase_) {
    case Phase::DIRECT: return "DirectCommands";
    case Phase::RADIO_MODES: return "RadioModes";
    case Phase::REBOOT_TEST: return "RebootTest";
    case Phase::RESTORE_AND_REBOOT: return "RestoreAndReboot";
    case Phase::RESTORE_RUNTIME: return "RestoreRuntime";
    case Phase::COMPLETE: return "Complete";
    case Phase::ABORTING: return "Aborting";
    case Phase::IDLE:
    default: return "Idle";
  }
}

String CommandSelfTest::safeCommandForLog(const String &command) const {
  return TextUtil::redactedCommand(command);
}
