#ifndef DRONE_GEL_CONTROLLER_QUICKCONFIG_H
#define DRONE_GEL_CONTROLLER_QUICKCONFIG_H

/*
  BEGINNER QUICK CONFIGURATION

  This file sits beside ESP32S3_Dispenser_Controller.ino and appears as a
  neighboring tab in Arduino IDE. Change only the values you need, leave the
  applicator disconnected while testing, then rebuild and upload the sketch.

  Set DRONE_USE_QUICK_CONFIG to 0 to ignore the overrides below and use every
  safe fallback from src/config/AppConfig.h instead. You normally leave it at 1.

  These are compile-time defaults. A standalone configuration saved with
  DispenserSave, or a named payload profile selected at runtime, still wins at
  boot after a firmware update. Check DispenserStatus for the active maxMs and
  armMs. To raise an older saved 2000 ms limit, disarm, send
  DispenserArmTimeout:0 followed by DispenserMaxPulse:0, then save with
  DispenserSave or PayloadProfileSave:<active-name>. DispenserErase and reboot
  instead restores the compiled defaults; named profile records remain until
  PayloadProfileDelete or PayloadProfileEraseAll is used.
*/

#ifndef DRONE_USE_QUICK_CONFIG
#define DRONE_USE_QUICK_CONFIG 1
#endif

#if DRONE_USE_QUICK_CONFIG

// Dedicated ESP32-S3 N16R8 firmware. Select ESP32S3 Dev Module in Arduino.
// Addressable onboard RGB LED: GPIO48 on the initial DevKitC-1 and many clones;
// GPIO38 on Espressif v1.1. Confirm your board revision if the LED stays dark.
#ifndef DRONE_CFG_STATUS_RGB_PIN
#define DRONE_CFG_STATUS_RGB_PIN 48
#endif
#define DRONE_CFG_STATUS_RGB_BRIGHTNESS 24

// 1 = drone dispenser (normal aircraft build)
// Keep profile 1: ESP32-S3 has no internal DAC for the legacy bench profile.
#define DRONE_CFG_HARDWARE_PROFILE 1

// DRONE PAYLOAD OUTPUT
// GPIO4 drives the low-voltage control input of the external analog
// switch. Never connect caulk-gun power or an inductive load directly to it.
// ACTIVE_HIGH 1 means HIGH activates the switch; use 0 for active-low hardware.
#define DRONE_CFG_DISPENSER_PIN 4
#define DRONE_CFG_DISPENSER_ACTIVE_HIGH 1
// All times are milliseconds: 3600000 = 60 minutes.
// DEFAULT is used by the legacy GPIO26:ON command, not by Dispense:milliseconds.
// MAX and ARM_TIMEOUT are optional limits; 0 disables each limit.
// Durations use unsigned 32-bit milliseconds (up to about 49.7 days per step).
#define DRONE_CFG_DISPENSER_DEFAULT_PULSE_MS 250UL
#define DRONE_CFG_DISPENSER_MAX_PULSE_MS 0UL
// Arm never survives a restart. A nonzero value enables automatic expiry.
#define DRONE_CFG_DISPENSER_ARM_TIMEOUT_MS 0UL

// OPTIONAL EXTERNAL INTERLOCK
// Set the pin to a valid GPIO input when a physical or flight-controller enable
// signal is fitted. -1 disables the input. ACTIVE_HIGH 1 means HIGH = permitted.
// The external circuit should bias the signal to the NOT-permitted state.
#define DRONE_CFG_INTERLOCK_PIN -1
#define DRONE_CFG_INTERLOCK_ACTIVE_HIGH 1

// LOCAL WI-FI ACCESS POINT
// Keep both values inside quotes. SSIDs may contain 1 to 32 characters and WPA2
// passwords must contain 8 to 63 characters. Change the default before field use.
#define DRONE_CFG_WIFI_AP_SSID "Drone-Gel-Controller"
#define DRONE_CFG_WIFI_AP_PASSWORD "password"

#endif  // DRONE_USE_QUICK_CONFIG

#endif  // DRONE_GEL_CONTROLLER_QUICKCONFIG_H
