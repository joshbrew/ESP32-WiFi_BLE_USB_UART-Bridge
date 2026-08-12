#ifndef ESP32_STEPPER_DUAL_DAC_STATUSINDICATORS_H
#define ESP32_STEPPER_DUAL_DAC_STATUSINDICATORS_H

#include <Arduino.h>
#include <atomic>

#include "../config/AppConfig.h"

// Connection and activity indication is isolated here so a user application can
// compile all GPIO ownership out without changing radio, command, or addon code.
class StatusIndicators {
 public:
  StatusIndicators();

  void begin(bool extendedBootSelfTest, bool suppressBootSelfTest = false);
  void noteCommandReceived();
  void service(bool connected, bool seekingConnection, bool operationActive);

  bool startSelfTest();
  bool startConnectionTest();
  bool startActivityTest();
  bool consumeSelfTestCompleted();
  bool isSelfTestActive() const;

  String statusText() const;
  String stateJson() const;

 private:
  enum class SelfTestPhase : uint8_t {
    IDLE,
    CONNECTION_ON,
    CONNECTION_OFF,
    ACTIVITY_ON,
    ACTIVITY_OFF
  };

  void runBootSelfTest(bool extendedBootSelfTest);
  void serviceSelfTest(uint32_t nowMs);
  bool startTest(uint8_t connectionCycles, uint8_t activityCycles);
  void beginConnectionPulse(uint32_t nowMs);
  void beginActivityPulse(uint32_t nowMs);
  void finishSelfTest();
  void forceOutputs(bool connectionOn, bool activityOn);
  void setOutputs(bool connectionOn, bool activityOn);
  void writePin(int pin, bool on);
  void logPinState(const char *prefix, int pin, bool on) const;

  std::atomic<uint32_t> activityPulseStartedMs_;
  uint32_t seekingStartedAtMs_;
  bool previousSeekingConnection_;
  std::atomic<bool> activityPulseArmed_;
  std::atomic<bool> connected_;
  std::atomic<bool> seekingConnection_;
  std::atomic<bool> operationActive_;
  std::atomic<bool> connectionOutputOn_;
  std::atomic<bool> activityOutputOn_;

  SelfTestPhase selfTestPhase_;
  uint8_t selfTestCycle_;
  uint8_t selfTestConnectionCycles_;
  uint8_t selfTestActivityCycles_;
  uint32_t selfTestPhaseEndsAtMs_;
  bool selfTestCompletedPending_;
};

#endif  // ESP32_STEPPER_DUAL_DAC_STATUSINDICATORS_H
