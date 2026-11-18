#ifndef CONTROLLER_DRV8871_MOTOR_H
#define CONTROLLER_DRV8871_MOTOR_H
#include <Arduino.h>
#include "DriverWiring.h"

class Drv8871Motor {
 public:
  void begin(const DriverWiring &wiring);
  void service();
  void stop();
  bool busy() const;
  uint32_t remainingMs(uint32_t now) const;
  bool command(const String &line, bool armed, bool ready, bool gpioBusy,
    uint32_t limit, String &message, bool &success);
  bool validatePulse(const String &line, uint32_t limit, String &reason) const;
  String stateJson(uint32_t limit) const;
 private:
#if DRONE_CFG_ENABLE_DRV8871
  enum class Mode { Coast, Drive, Brake };
  struct Channel { DriverWiring::DcPins pins{}; Mode mode = Mode::Coast; uint8_t level = 128; uint32_t started = 0, duration = 0; };
  void idle(size_t index, bool brake);
  bool drive(size_t index, uint8_t level);
  Channel channels_[4]{};
  uint8_t count_ = 0, active_ = 0;
#endif
};
#endif
