#ifndef ESP32_MODULAR_CONTROLLER_COMMANDSELFTEST_H
#define ESP32_MODULAR_CONTROLLER_COMMANDSELFTEST_H

#include <Arduino.h>
#include <Preferences.h>

#include "AppTypes.h"
#include "../addons/DeviceAddon.h"
#include "EventBus.h"

class CommandDispatcher;
class RadioManager;

class CommandSelfTest {
 public:
  CommandSelfTest(
    EventBus &events,
    CommandDispatcher &dispatcher,
    DeviceAddon &addon,
    RadioManager &radios
  );

  void begin();
  void service();
  bool handleCommand(
    const String &command,
    CommandSource source,
    const String &requestId
  );

  bool isActive() const;
  String statusText() const;
  String stateJson() const;
  String webStateJson() const;

 private:
  enum class Phase : uint8_t {
    IDLE = 0,
    DIRECT = 1,
    RADIO_MODES = 2,
    REBOOT_TEST = 3,
    RESTORE_AND_REBOOT = 4,
    RESTORE_RUNTIME = 5,
    COMPLETE = 6,
    ABORTING = 7
  };

  enum class WaitKind : uint8_t {
    QUICK = 0,
    FIXED = 1,
    ADDON_BUSY = 2,
    ADDON_TIMED = 3,
    SPECIAL = 4,
    SKIP = 5
  };

  enum class SpecialAction : uint8_t {
    NONE = 0,
    RESTORE_SYSTEM = 1,
    RESTORE_ADDON = 2,
    RESTORE_RADIO = 3
  };

  struct TestCase {
    const char *name;
    const char *command;
    WaitKind waitKind;
    uint32_t timeoutMs;
    uint32_t minimumMs;
    SpecialAction special;
    const char *skipReason;
  };


  static constexpr const char *NAMESPACE = "step-selftest";

  void start(CommandSource source, const String &requestId);
  void requestAbort(CommandSource source, const String &requestId);
  void clearRecord(CommandSource source, const String &requestId);

  const TestCase *directTests() const;
  uint16_t directTestCount() const;
  void serviceDirectTests();
  void serviceRadioTests();
  void serviceRebootTest();
  void serviceRestoreAndReboot();
  void serviceRestoreRuntime();
  void serviceAborting();

  void startDirectTest(const TestCase &test);
  void serviceRunningDirectTest(const TestCase &test);
  String compileAwareSkipReason(const TestCase &test) const;
  String expectedCompileGuardError(const TestCase &test) const;
  bool executeSpecial(SpecialAction action, String &reason);
  void scanCurrentTestEvents();
  void finishCurrentTest(bool passed, const String &reason);
  void recordSkip(const String &name, const String &reason);

  bool captureSnapshot(String &reason);
  bool loadSnapshot(String &reason);
  bool restoreSystemNvs(String &reason);
  bool restoreAddonPersistent(String &reason);
  bool restoreRadioNvs(String &reason);
  bool restoreAllPersistent(String &reason);
  bool verifyPersistentRestore(String &reason);

  String runtimeRestoreCommand(uint16_t index) const;
  uint16_t runtimeRestoreCommandCount() const;

  void beginRadioModeCommand();
  void finishRadioMode(bool passed, const String &reason);
  uint8_t radioModeCount() const;
  String radioModeName(uint8_t index) const;
  String radioModeCommand(uint8_t index) const;
  uint8_t profileCommandCount(uint8_t modeIndex) const;
  String profileCommand(uint8_t modeIndex, uint8_t commandIndex) const;
  String profileCommandName(uint8_t modeIndex, uint8_t commandIndex) const;

  void submitCurrentCommand(
    const String &name,
    const String &command,
    WaitKind waitKind,
    uint32_t timeoutMs,
    uint32_t minimumMs
  );
  bool currentCommandFinished() const;
  uint16_t totalTestCount() const;
  uint16_t currentOrdinal() const;

  bool loadState();
  bool saveState();
  void resetTransientState();
  void announceResume();
  void publishStatus(
    EventLevel level,
    const String &message,
    CommandSource source = CommandSource::INTERNAL,
    const String &requestId = String()
  );

  String phaseName() const;
  String safeCommandForLog(const String &command) const;

  EventBus &events_;
  CommandDispatcher &dispatcher_;
  DeviceAddon &addon_;
  RadioManager &radios_;

  Phase phase_;
  uint16_t directIndex_;
  uint8_t radioModeIndex_;
  uint8_t radioSubIndex_;
  uint16_t runtimeRestoreIndex_;
  uint16_t passCount_;
  uint16_t failCount_;
  uint16_t skipCount_;
  uint32_t runId_;
  uint32_t bootCount_;
  bool active_;
  bool inflight_;
  bool awaitingReboot_;
  bool abortRequested_;
  uint8_t targetRadioMode_;
  String lastResult_;


  String currentName_;
  String currentCommand_;
  String currentRequestId_;
  WaitKind currentWaitKind_;
  uint32_t currentStartedAtMs_;
  uint32_t currentTimeoutMs_;
  uint32_t currentMinimumMs_;
  uint32_t currentEventCursor_;
  bool currentRunSeen_;
  bool currentActivitySeen_;
  bool currentErrorSeen_;
  String currentErrorText_;
};

#endif  // ESP32_MODULAR_CONTROLLER_COMMANDSELFTEST_H
