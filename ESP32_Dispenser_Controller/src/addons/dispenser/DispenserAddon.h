#ifndef DRONE_GEL_CONTROLLER_DISPENSERADDON_H
#define DRONE_GEL_CONTROLLER_DISPENSERADDON_H

#include <Arduino.h>

#include "../../config/AppConfig.h"
#include "../DeviceAddon.h"

// Primary hardware profile for the aircraft. This class deliberately exposes
// application words (arm, dispense, stop) instead of raw GPIO writes. Every
// activation is bounded and every reset starts disarmed with the output safe.
class DispenserAddon : public DeviceAddon {
 public:
  explicit DispenserAddon(EventBus &events);

  const char *name() const override;
  void begin() override;
  void service() override;
  bool canAcceptCommand(const String &command) const override;
  bool handleCommand(
    const String &command,
    CommandSource source,
    const String &requestId
  ) override;
  bool stopAll(CommandSource source, const String &requestId) override;
  bool isBusy() const override;
  bool hasActiveOutput() const override;
  bool hasTimedOperationActive() const override;
  bool canStartRoutine(String &reason) const override;
  bool blocksExternalCommandDuringRoutine(const String &command) const override;
  void appendStateJson(String &json, bool compact) const override;
  void publishHelp(
    EventBus &events,
    CommandSource source,
    const String &requestId
  ) const override;
  void publishStatus(
    EventBus &events,
    CommandSource source,
    const String &requestId,
    bool configuration
 ) const override;

 private:
  struct StoredProfile {
    uint32_t magic;
    uint16_t version;
    uint8_t used;
    uint8_t activeHigh;
    int32_t outputPin;
    uint32_t defaultPulseMs;
    uint32_t maxPulseMs;
    uint32_t armTimeoutMs;
    char name[AppConfig::DISPENSER_PROFILE_NAME_BYTES + 1];
    uint32_t checksum;
  };
  static_assert(sizeof(StoredProfile) <= 48, "Payload profile record unexpectedly grew.");

  bool startDispense(
    uint32_t durationMs,
    CommandSource source,
    const String &requestId,
    const char *reason
  );
  void stopDispense(
    CommandSource source,
    const String &requestId,
    const char *reason,
    bool announce
  );
  void disarm(
    CommandSource source,
    const String &requestId,
    const char *reason,
    bool announce,
    bool fault
  );
  bool arm(CommandSource source, const String &requestId);
  void applyInactivePin(int pin) const;
  void writeOutput(bool active);
  bool interlockOpen() const;
  bool validatePin(int pin, String &reason) const;
  bool validateTimings(
    uint32_t defaultPulseMs,
    uint32_t maxPulseMs,
    uint32_t armTimeoutMs,
    String &reason
  ) const;
  bool setConfiguredPin(int pin, String &reason);
  bool setDefaultPulse(uint32_t value, String &reason);
  bool setMaxPulse(uint32_t value, String &reason);
  bool setArmTimeout(uint32_t value, String &reason);
  void setDefaults();
  bool loadProfileLibrary();
  int findProfile(const String &name) const;
  int findFreeProfileSlot() const;
  bool validProfileName(const String &name) const;
  bool validStoredProfile(const StoredProfile &profile) const;
  uint32_t profileChecksum(const StoredProfile &profile) const;
  void initializeProfile(StoredProfile &profile, const String &name) const;
  bool saveProfileSlot(uint8_t slot, const StoredProfile &profile) const;
  bool persistActiveProfile(int8_t slot) const;
  bool eraseProfileSlot(uint8_t slot);
  bool eraseAllProfiles();
  void applyProfile(const StoredProfile &profile);
  void listProfiles(CommandSource source, const String &requestId) const;
  void showProfile(
    uint8_t slot,
    CommandSource source,
    const String &requestId
  ) const;
  String currentProfileName() const;
  uint8_t storedProfileCount() const;
  bool loadSettings();
  bool saveSettings();
  bool eraseSettings();
  uint32_t remainingDispenseMs(uint32_t now) const;
  uint32_t remainingArmMs(uint32_t now) const;
  String statusText() const;
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
  int outputPin_;
  bool activeHigh_;
  uint32_t defaultPulseMs_;
  uint32_t maxPulseMs_;
  uint32_t armTimeoutMs_;
  StoredProfile profiles_[AppConfig::DISPENSER_PROFILE_MAX_COUNT];
  int8_t activeProfileSlot_;
  bool profileModified_;
  bool standaloneSettingsActive_;
  bool initialized_;
  bool armed_;
  bool dispensing_;
  bool faulted_;
  uint32_t armedAtMs_;
  uint32_t dispenseStartedAtMs_;
  uint32_t dispenseEndsAtMs_;
  uint32_t dispenseCount_;
  uint32_t totalDispenseMs_;
  uint32_t lastServiceAtMs_;
  uint32_t maxServiceGapMs_;
  uint32_t maxStopLatenessMs_;
  uint32_t lateStopCount_;
  String lastReason_;
};

#endif  // DRONE_GEL_CONTROLLER_DISPENSERADDON_H
