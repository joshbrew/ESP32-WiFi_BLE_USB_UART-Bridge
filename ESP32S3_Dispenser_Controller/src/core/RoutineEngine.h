#ifndef DRONE_GEL_CONTROLLER_ROUTINEENGINE_H
#define DRONE_GEL_CONTROLLER_ROUTINEENGINE_H

#include <Arduino.h>

#include "../config/AppConfig.h"
#include "AppTypes.h"
#include "../addons/DeviceAddon.h"
#include "EventBus.h"

// Fixed-size, nonblocking automation shared by every hardware profile.
// Routine steps either wait, wait for the selected addon to become idle, or
// submit one validated hardware command through the normal dispatcher.
class RoutineEngine {
 public:
  RoutineEngine(EventBus &events, DeviceAddon &addon);

  void begin();
  void configureSubmitter(CommandSubmitter submitter, void *context);
  bool recognizesCommand(const String &command) const;
  bool handleCommand(
    const String &command,
    CommandSource source,
    const String &requestId
  );
  void service();
  bool stop(
    CommandSource source,
    const String &requestId,
    const String &reason,
    bool announce = true
  );
  bool isActive() const;
  bool runNamed(const String &name, CommandSource source, const String &requestId);
  bool hasSaved(const String &name) const;
  bool lastRunSucceeded() const;
  String stateJson(bool compact) const;
  String statusText() const;
  void publishHelp(CommandSource source, const String &requestId) const;

 private:
  enum class StepType : uint8_t {
    EMPTY = 0,
    WAIT = 1,
    COMMAND = 2,
    WAIT_IDLE = 3,
    START_WAIT = 4,
    PIN_OUTPUT = 5 // One pin's pulse, followed by its gap; one stored step.
  };

  struct StoredStep {
    uint8_t type;
    uint32_t value;
    char command[AppConfig::ROUTINE_COMMAND_BYTES + 1];
  };

  struct StoredRoutine {
    uint32_t magic;
    uint16_t version;
    uint8_t used;
    uint8_t count;
    uint32_t repeatCount; // 0 means repeat until stopped (FOREVER).
    char name[AppConfig::ROUTINE_NAME_BYTES + 1];
    StoredStep steps[AppConfig::ROUTINE_MAX_STEPS];
    uint32_t checksum;
  };

  int findRoutine(const String &name) const;
  int findFreeSlot() const;
  bool validName(const String &name) const;
  bool validStoredRoutine(const StoredRoutine &routine) const;
  bool safeRoutineCommand(const String &command, String &reason) const;
  uint32_t checksum(const StoredRoutine &routine) const;
  void initializeRoutine(StoredRoutine &routine, const String &name);
  bool addStep(StoredRoutine &routine, const String &spec, String &reason);
  bool saveSlot(uint8_t slot);
  bool eraseSlot(uint8_t slot);
  void showRoutine(
    uint8_t slot,
    CommandSource source,
    const String &requestId
  ) const;
  bool startRoutine(
    uint8_t slot,
    CommandSource source,
    const String &requestId
  );
  void finish(bool success, const String &reason);
  void publish(
    EventLevel level,
    CommandSource source,
    const String &requestId,
    const String &message
  ) const;
  void error(
    CommandSource source,
    const String &requestId,
    const String &message
  ) const;

  EventBus &events_;
  DeviceAddon &addon_;
  CommandSubmitter submitter_;
  void *submitContext_;
  StoredRoutine routines_[AppConfig::ROUTINE_MAX_COUNT];
  bool saved_[AppConfig::ROUTINE_MAX_COUNT];
  bool lastRunSucceeded_;
  bool active_;
  uint8_t activeSlot_;
  uint8_t stepIndex_;
  uint32_t repeatIndex_;
  bool waiting_;
  uint8_t outputPhase_ = 0;
  uint32_t runStartedAtMs_;
  uint32_t waitStartedAtMs_;
  uint32_t waitUntilMs_;
  CommandSource runSource_;
  String runRequestId_;
  String lastResult_;
};

#endif  // DRONE_GEL_CONTROLLER_ROUTINEENGINE_H
