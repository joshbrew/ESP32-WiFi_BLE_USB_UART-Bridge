#ifndef CONTROLLER_TMC2209_MOTOR_H
#define CONTROLLER_TMC2209_MOTOR_H

#include <Arduino.h>
#include "../config/AppConfig.h"
#include "DriverWiring.h"
#if DRONE_CFG_ENABLE_TMC2209
#if !__has_include(<TMCStepper.h>) || !__has_include(<FastAccelStepper.h>)
#error Install TMCStepper 0.7.3 and FastAccelStepper 2.0.1, or use controller setup.
#endif
#include <HardwareSerial.h>
#include <TMCStepper.h>
#include <FastAccelStepper.h>
#endif

// One shared UART, separate STEP/DIR/active-low EN and timer per motor.
// The dispenser addon owns arming, interlock and saved-routine exclusion.
class Tmc2209Motor {
 public:
  void begin();
  void begin(const DriverWiring &wiring);
  void service();
  void stop();
  bool busy() const;
  uint32_t remainingMs(uint32_t now) const;
  bool command(const String &line, bool armed, bool ready, bool occupied,
    uint32_t runLimit, String &message, bool &success);
  bool validatePulse(const String &line, uint32_t runLimit, String &reason) const;
  String stateJson() const;

 private:
#if DRONE_CFG_ENABLE_TMC2209
  struct Channel {
    FastAccelStepper *stepper = nullptr;
    TMC2209Stepper *driver = nullptr;
    bool ready = false;
    bool running = false;
    uint32_t started = 0, duration = 0;
    uint16_t current = AppConfig::TMC2209_CURRENT_MA;
    uint16_t actualCurrent = 0;
    uint16_t microsteps = AppConfig::TMC2209_MICROSTEPS;
  };
  bool configure(size_t index);
  void stopMotor(size_t index);
  HardwareSerial uart_{AppConfig::TMC2209_UART_PORT};
  FastAccelStepperEngine engine_;
  Channel channels_[4]{};
  DriverWiring::StepPins pins_[4]{};
  uint8_t count_ = 0;
  uint8_t tx_ = AppConfig::TMC2209_TX_PIN, rx_ = AppConfig::TMC2209_RX_PIN;
  bool initialized_ = false;
  uint8_t active_ = 0;
#endif
};

#endif
