#ifndef ESP32_MODULAR_CONTROLLER_STEPPERDACADDON_H
#define ESP32_MODULAR_CONTROLLER_STEPPERDACADDON_H

#include "../DeviceAddon.h"
#include "DacController.h"
#include "StepperController.h"

class StepperDacAddon : public DeviceAddon {
 public:
  StepperDacAddon(EventBus &events, StepperController &stepper, DacController &dac);

  const char *name() const override;
  void begin() override;
  void service() override;
  bool canAcceptCommand(const String &command) const override;
  bool handleCommand(const String &command, CommandSource source, const String &requestId) override;
  bool stopAll(CommandSource source, const String &requestId) override;
  bool isBusy() const override;
  bool hasActiveOutput() const override;
  bool hasTimedOperationActive() const override;
  void appendStateJson(String &json, bool compact) const override;
  void publishHelp(EventBus &events, CommandSource source, const String &requestId) const override;
  void publishStatus(EventBus &events, CommandSource source, const String &requestId, bool configuration) const override;
  void queueStartupTests(CommandDispatcher &dispatcher) const override;
  bool captureSelfTestState(Preferences &snapshot, String &defaultNamespaces, String &reason) override;
  bool loadSelfTestState(Preferences &snapshot, String &reason) override;
  bool restorePersistentSelfTestState(Preferences &snapshot, String &reason) override;
  bool verifyPersistentSelfTestState(Preferences &snapshot, String &reason) const override;
  uint16_t runtimeRestoreCommandCount() const override;
  String runtimeRestoreCommand(uint16_t index) const override;

 private:
  struct RuntimeSnapshot {
    long motorStepsPerRev;
    float motorMinRpm;
    float motorMaxRpm;
    float motorStartRpm;
    float motorRampRpm;
    uint32_t motorMinIntervalUs;
    uint8_t motorStepMode;
    String motorStepOrder;
    bool motorHoldTorque;
    uint16_t dacReferenceMv;
    uint16_t dacTargetMv[2];
    bool dacBootEnabled[2];
    uint32_t dacTimeoutMs[2];
  };

  void publishChunked(
    EventBus &events,
    CommandSource source,
    const String &requestId,
    const String &prefix,
    const String &text
  ) const;

  EventBus &events_;
  StepperController &stepper_;
  DacController &dac_;
  RuntimeSnapshot snapshot_;
  bool snapshotLoaded_;
};

#endif  // ESP32_MODULAR_CONTROLLER_STEPPERDACADDON_H
