#ifndef ESP32_STEPPER_DUAL_DAC_STEPPERCONTROLLER_H
#define ESP32_STEPPER_DUAL_DAC_STEPPERCONTROLLER_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "../../config/AppConfig.h"
#include "../../core/AppTypes.h"
#include "../../core/EventBus.h"

// EDIT HERE for motor commands, motion profiles, step sequences, or driver pin
// behavior. All physical stepping stays in the dedicated RTOS task. Other
// subsystems must enqueue commands and never toggle stepper pins directly.
class StepperController {
 public:
  struct SettingsSnapshot {
    long stepsPerRevolution;
    float minRpm;
    float maxRpm;
    float startRpm;
    float rampRpmPerSecond;
    uint32_t minIntervalUs;
    uint8_t stepMode;
    String stepOrder;
    bool holdTorque;
  };

  explicit StepperController(EventBus &events);

  bool begin();
  bool handleCommand(
    const String &command,
    CommandSource source,
    const String &requestId
  );
  bool emergencyStop(CommandSource source, const String &requestId);
  bool canAcceptCommand(const String &command) const;

  bool isBusy() const;
  String statusText() const;
  String stateJson() const;
  String webStateJson() const;
  SettingsSnapshot settingsSnapshot() const;

 private:
  enum class StepMode : uint8_t {
    FULL = 4,
    HALF = 8
  };

  enum class TestPhase : uint8_t {
    IDLE,
    MOVING_CW,
    DWELL_AFTER_CW,
    MOVING_CCW,
    DWELL_AFTER_CCW
  };

  struct MotionState {
    bool active;
    long remaining;
    long requested;
    int direction;
    float targetRpm;
    float currentRpm;
    uint32_t intervalUs;
    uint32_t nextStepAtUs;
    uint32_t lastRampAtUs;
    CommandSource source;
    String requestId;
  };

  struct TestState {
    bool active;
    bool immediate;
    float rpm;
    float degrees;
    uint8_t cyclesTotal;
    uint8_t cycleIndex;
    uint16_t dwellMs;
    uint32_t phaseAtMs;
    TestPhase phase;
    CommandSource source;
    String requestId;
    String name;
  };

  struct QueuedCommand {
    CommandSource source;
    char requestId[AppConfig::REQUEST_ID_MAX_BYTES + 1];
    char line[AppConfig::COMMAND_MAX_BYTES + 1];
  };

  struct PublicState {
    long position;
    long baseStepsPerRev;
    long effectiveStepsPerRev;
    float minRpm;
    float maxRpm;
    float startRpm;
    float rampRpmPerSecond;
    uint32_t minIntervalUs;
    uint8_t stepMode;
    char stepOrder[5];
    bool holdTorque;
    bool moving;
    float targetRpm;
    float currentRpm;
    long remaining;
    bool testActive;
    UBaseType_t queuedCommands;
  };

  static constexpr int DIR_CW = 1;
  static constexpr int DIR_CCW = 2;

  static void taskThunk(void *context);
  void taskLoop();
  void processQueuedCommands(uint8_t budget, bool urgentOnly);
  bool isUrgentCommand(const String &command) const;
  bool recognizesCommand(const String &command) const;
  bool processCommand(
    const String &command,
    CommandSource source,
    const String &requestId
  );
  bool rejectTuningWhileMoving(
    CommandSource source,
    const String &requestId,
    const char *settingName
  );

  void serviceMotion();
  void serviceTest();
  void finishMotion();
  void startMove(
    float rpm,
    long steps,
    int direction,
    CommandSource source,
    const String &requestId,
    bool immediate = false,
    bool fromTest = false
  );
  void startTest(
    const String &name,
    float rpm,
    float degrees,
    uint8_t cycles,
    uint16_t dwellMs,
    bool immediate,
    CommandSource source,
    const String &requestId
  );
  void stopMotion(CommandSource source, const String &requestId, bool announce);

  void applyPhase(int phase);
  void releaseCoils();
  void stepOnce(int direction);

  bool parseRpmMove(const String &command, float &rpm, long &steps, int &direction) const;
  bool parseDegreeMove(const String &command, float &rpm, float &degrees, int &direction) const;

  bool setStepOrder(String digits);
  void setStepOrderPreset(int preset);
  String stepOrderString() const;

  float clampFloat(float value, float low, float high) const;
  float percentToRpm(int percent) const;
  uint32_t rpmToIntervalUs(float rpm) const;
  long effectiveStepsPerRevolution() const;
  long degreesToSteps(float degrees) const;

  void updatePublicState();
  PublicState snapshot() const;
  void publishStatusReadback(
    CommandSource source,
    const String &requestId,
    const char *category = "STATUS"
  );
  void publishConfigUpdated(
    CommandSource source,
    const String &requestId,
    const String &change
  );

  void publish(
    EventLevel level,
    CommandSource source,
    const String &requestId,
    const String &message
  );
  void error(CommandSource source, const String &requestId, const String &message);

  EventBus &events_;
  int pins_[4];
  int logicalOrder_[4];
  int currentOrderPreset_;

  long stepsPerRevolution_;
  float minRpm_;
  float maxRpm_;
  uint32_t minStepIntervalUs_;
  float startRpm_;
  float rampRpmPerSecond_;
  bool holdTorqueWhenIdle_;
  StepMode stepMode_;

  MotionState motion_;
  TestState test_;
  int phaseIndex_;
  long positionSteps_;

  QueueHandle_t commandQueue_;
  TaskHandle_t taskHandle_;
  PublicState publicState_;
  mutable portMUX_TYPE publicStateMux_;
};

#endif  // ESP32_STEPPER_DUAL_DAC_STEPPERCONTROLLER_H
