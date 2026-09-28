#ifndef DRONE_GEL_CONTROLLER_QUICKCONFIG_H
#define DRONE_GEL_CONTROLLER_QUICKCONFIG_H

/*
  BEGINNER QUICK CONFIGURATION

  This file sits beside ESP32_Dispenser_Controller.ino and appears as a
  neighboring tab in Arduino IDE. Change only the values you need, leave the
  applicator disconnected while testing, then rebuild and upload the sketch.

  Set DRONE_USE_QUICK_CONFIG to 0 to ignore the overrides below and use every
  safe fallback from src/config/AppConfig.h instead. You normally leave it at 1.

  These are compile-time defaults. A standalone configuration saved with
  DispenserSave, or a named payload profile selected at runtime, still wins at
  boot after a firmware update. Check DispenserStatus for the active maxMs and
  armMs. To raise an older saved 2000 ms limit, disarm, send
  DispenserArmTimeout:120000 followed by DispenserMaxPulse:60000, then save with
  DispenserSave or PayloadProfileSave:<active-name>. DispenserErase and reboot
  instead restores the compiled defaults; named profile records remain until
  PayloadProfileDelete or PayloadProfileEraseAll is used.
*/

#define DRONE_USE_QUICK_CONFIG 1

#if DRONE_USE_QUICK_CONFIG

// 1 = drone dispenser (normal aircraft build)
// 2 = stepper + DAC (advanced bench build; never use on the aircraft by accident)
#define DRONE_CFG_HARDWARE_PROFILE 1

// DRONE PAYLOAD OUTPUT
// GPIO26 drives only the low-voltage control input of the external analog
// switch. Never connect caulk-gun power or an inductive load directly to it.
// ACTIVE_HIGH 1 means HIGH activates the switch; use 0 for active-low hardware.
#define DRONE_CFG_DISPENSER_PIN 26
#define DRONE_CFG_DISPENSER_ACTIVE_HIGH 1
// All times are milliseconds: 60000 = 60 seconds; 120000 = 2 minutes.
// DEFAULT is used by the legacy GPIO26:ON command, not by Dispense:milliseconds.
// MAX is the hard upper bound for manual and routine pulses. A saved profile
// can choose a lower maximum but cannot exceed this compiled ceiling.
// A pulse must fit entirely inside the time remaining since the last Arm.
// Keep the arming window longer than MAX so a full-length pulse can start.
// If too little time remains, send Arm again before requesting the pulse.
#define DRONE_CFG_DISPENSER_DEFAULT_PULSE_MS 250UL
#define DRONE_CFG_DISPENSER_MAX_PULSE_MS 60000UL
// Arm automatically expires after this time and never survives a restart.
// This is a readiness window, not an output-on duration. Saved profiles may
// choose a shorter window but cannot extend this compiled ceiling.
#define DRONE_CFG_DISPENSER_ARM_TIMEOUT_MS 120000UL

// OPTIONAL EXTERNAL INTERLOCK
// Set the pin to a valid GPIO input when a physical or flight-controller enable
// signal is fitted. -1 disables the input. ACTIVE_HIGH 1 means HIGH = permitted.
// The external circuit should bias the signal to the NOT-permitted state.
#define DRONE_CFG_INTERLOCK_PIN -1
#define DRONE_CFG_INTERLOCK_ACTIVE_HIGH 1

// STATUS LED OUTPUTS
// CONNECTION indicates radio state; ACTIVITY flashes for commands/events.
// ACTIVE_HIGH 1 suits an LED wired from GPIO through a resistor to ground.
// Feature and boot-test enable switches remain in AppConfig.h.
#define DRONE_CFG_STATUS_CONNECTION_PIN 23
#define DRONE_CFG_STATUS_ACTIVITY_PIN 5
#define DRONE_CFG_STATUS_LED_ACTIVE_HIGH 1

// LOCAL WI-FI ACCESS POINT
// Keep both values inside quotes. SSIDs may contain 1 to 32 characters and WPA2
// passwords must contain 8 to 63 characters. Change the default before field use.
#define DRONE_CFG_WIFI_AP_SSID "Drone-Gel-Controller"
#define DRONE_CFG_WIFI_AP_PASSWORD "password"

// ADVANCED STEPPER + DAC BENCH PINOUT
// Everything below is ignored by profile 1. These four outputs connect to the
// ULN2003 IN1..IN4 inputs; the ESP32 must not power the stepper motor itself.
#define DRONE_CFG_STEPPER_IN1_PIN 18
#define DRONE_CFG_STEPPER_IN2_PIN 19
#define DRONE_CFG_STEPPER_IN3_PIN 27
#define DRONE_CFG_STEPPER_IN4_PIN 14
// The classic ESP32 analog DAC output must be GPIO25 or GPIO26. DIGITAL_OUTPUT
// is the optional 0/3.3 V compatibility channel and must use a different pin.
#define DRONE_CFG_DAC_ANALOG_PIN 25
#define DRONE_CFG_DIGITAL_OUTPUT_PIN 26

#endif  // DRONE_USE_QUICK_CONFIG

#endif  // DRONE_GEL_CONTROLLER_QUICKCONFIG_H
