#ifndef ESP32_STEPPER_DUAL_DAC_DACCONTROLLER_H
#define ESP32_STEPPER_DUAL_DAC_DACCONTROLLER_H

#include <Arduino.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/timers.h>

#include "../../config/AppConfig.h"
#include "../../core/AppTypes.h"
#include "../../core/EventBus.h"

// EDIT HERE to add DAC commands, calibration behavior, boot policies, or new
// analog channels. Keep hardware-independent command parsing in handleCommand(),
// state changes behind stateMutex_, and deadline enforcement in the RTOS timers.
class DacController {
 public:
  struct SettingsSnapshot {
    uint16_t referenceMv;
    uint16_t targetMv[AppConfig::DAC_CHANNEL_COUNT];
    bool bootEnabled[AppConfig::DAC_CHANNEL_COUNT];
    uint32_t maxOnMs[AppConfig::DAC_CHANNEL_COUNT];
  };

  explicit DacController(EventBus &events);

  void begin();
  void service();

  bool handleCommand(
    const String &command,
    CommandSource source,
    const String &requestId
  );

  void allOff(CommandSource source, const String &requestId, bool announce = true);
  bool anyOutputActive() const;
  bool hasTimedOperationActive() const;
  String statusText() const;
  String stateJson() const;
  String webStateJson() const;
  SettingsSnapshot settingsSnapshot() const;

 private:
  struct Channel {
    uint16_t targetMv;
    bool enabled;
    bool bootEnabled;
    uint32_t maxOnMs;
    uint32_t enabledAtMs;
    bool oneShot;
    uint32_t oneShotEndsAtMs;
  };

  struct TimerContext {
    DacController *owner;
    uint8_t index;
  };

  static void releaseTimerThunk(TimerHandle_t timer);
  void serviceChannelTimeout(uint8_t index, bool timerCallback);

  void setDefaults();
  bool loadSettings(bool announce, CommandSource source, const String &requestId);
  bool saveSettings();
  bool eraseSettings();

  bool setEnabled(
    uint8_t index,
    bool enabled,
    CommandSource source,
    const String &requestId,
    bool announce = true
  );
  bool setMillivolts(
    uint8_t index,
    uint16_t millivolts,
    CommandSource source,
    const String &requestId,
    bool announce = true
  );
  bool startTest3s(
    uint8_t index,
    CommandSource source,
    const String &requestId,
    bool announce = true
  );

  bool lockState(TickType_t timeout = portMAX_DELAY) const;
  void unlockState() const;
  void cancelOneShotUnlocked(uint8_t index);
  void applyUnlocked(uint8_t index);
  void scheduleReleaseTimerUnlocked(uint8_t index);
  void stopReleaseTimerUnlocked(uint8_t index);
  uint16_t outputMillivoltsUnlocked(uint8_t index) const;
  uint8_t millivoltsToCodeUnlocked(uint16_t millivolts) const;
  int parseChannelPrefix(const String &command, unsigned int &consumed) const;
  String channelName(uint8_t index) const;
  void publishConfigReadback(
    CommandSource source,
    const String &requestId,
    const String &reason
  );

  void publish(
    EventLevel level,
    CommandSource source,
    const String &requestId,
    const String &message
  );
  void error(CommandSource source, const String &requestId, const String &message);

  EventBus &events_;
  Channel channels_[AppConfig::DAC_CHANNEL_COUNT];
  int pins_[AppConfig::DAC_CHANNEL_COUNT];
  uint16_t referenceMv_;
  mutable SemaphoreHandle_t stateMutex_;
  TimerHandle_t releaseTimers_[AppConfig::DAC_CHANNEL_COUNT];
  bool releaseTimerArmed_[AppConfig::DAC_CHANNEL_COUNT];
  TimerContext timerContexts_[AppConfig::DAC_CHANNEL_COUNT];
  uint32_t lastFallbackServiceAtMs_;
};

#endif  // ESP32_STEPPER_DUAL_DAC_DACCONTROLLER_H
