#ifndef DRONE_GEL_CONTROLLER_APPCONFIG_H
#define DRONE_GEL_CONTROLLER_APPCONFIG_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include "../../QuickConfig.h"

// Optional cellular uplink. QuickConfig is the normal user-facing switch.
#ifndef APP_ENABLE_LTE
#ifdef DRONE_CFG_ENABLE_LTE
#define APP_ENABLE_LTE DRONE_CFG_ENABLE_LTE
#else
#define APP_ENABLE_LTE 0
#endif
#endif
#define APP_LTE_ENABLED APP_ENABLE_LTE
#if APP_ENABLE_LTE != 0 && APP_ENABLE_LTE != 1
#error APP_ENABLE_LTE must be 0 or 1.
#endif


// ---------------------------------------------------------------------------
// ADVANCED BUILD SETTINGS AND SAFE FALLBACKS
// ---------------------------------------------------------------------------
// Users should start in the QuickConfig.h Arduino tab. If its master switch
// is off, or an individual override is absent, the fallback values in this file
// are used. Advanced build systems may still define one APP_ADDON_* macro
// directly; a direct selection takes precedence over the quick profile number.

#ifndef DRONE_CFG_BOARD_ESP32_S3
#define DRONE_CFG_BOARD_ESP32_S3 1
#endif
#if DRONE_CFG_BOARD_ESP32_S3 != 1
#error This separate firmware targets ESP32-S3 only. Use ESP32_Dispenser_Controller for LOLIN32.
#endif
#if defined(ARDUINO_ARCH_ESP32)
  #if DRONE_CFG_BOARD_ESP32_S3 && !defined(CONFIG_IDF_TARGET_ESP32S3)
    #error Select ESP32S3 Dev Module in Arduino Tools for the S3 board flag.
  #endif
#endif
#ifndef DRONE_CFG_STATUS_RGB_PIN
#define DRONE_CFG_STATUS_RGB_PIN 48
#endif
#ifndef DRONE_CFG_STATUS_RGB_BRIGHTNESS
#define DRONE_CFG_STATUS_RGB_BRIGHTNESS 24
#endif

#ifndef DRONE_CFG_HARDWARE_PROFILE
#define DRONE_CFG_HARDWARE_PROFILE 1
#endif

#if !defined(APP_ADDON_DRONE_DISPENSER) && !defined(APP_ADDON_STEPPER_DAC)
  #if DRONE_CFG_HARDWARE_PROFILE == 1
    #define APP_ADDON_DRONE_DISPENSER
  #elif DRONE_CFG_HARDWARE_PROFILE == 2
    #define APP_ADDON_STEPPER_DAC
  #else
    #error DRONE_CFG_HARDWARE_PROFILE must be 1 (dispenser) or 2 (stepper/DAC).
  #endif
#endif

#ifndef DRONE_CFG_ENABLE_DRV8871
#define DRONE_CFG_ENABLE_DRV8871 0
#endif
#ifndef DRONE_CFG_DRV8871_IN1_PIN
#define DRONE_CFG_DRV8871_IN1_PIN 6
#endif
#ifndef DRONE_CFG_DRV8871_IN2_PIN
#define DRONE_CFG_DRV8871_IN2_PIN 7
#endif
#ifndef DRONE_CFG_DRV8871_MOTOR_PINS
#define DRONE_CFG_DRV8871_MOTOR_PINS { {DRONE_CFG_DRV8871_IN1_PIN, DRONE_CFG_DRV8871_IN2_PIN} }
#endif
#ifndef DRONE_CFG_DRV8871_MAX_RUN_MS
#define DRONE_CFG_DRV8871_MAX_RUN_MS 30000UL
#endif

#ifndef DRONE_CFG_DISPENSER_PIN
#if DRONE_CFG_BOARD_ESP32_S3
#define DRONE_CFG_DISPENSER_PIN 4
#else
#define DRONE_CFG_DISPENSER_PIN 26
#endif
#endif
#ifndef DRONE_CFG_DISPENSER_ACTIVE_HIGH
#define DRONE_CFG_DISPENSER_ACTIVE_HIGH 1
#endif
#ifndef DRONE_CFG_DISPENSER_DEFAULT_PULSE_MS
#define DRONE_CFG_DISPENSER_DEFAULT_PULSE_MS 250UL
#endif
#ifndef DRONE_CFG_DISPENSER_MAX_PULSE_MS
#define DRONE_CFG_DISPENSER_MAX_PULSE_MS 0UL
#endif
#ifndef DRONE_CFG_DISPENSER_ARM_TIMEOUT_MS
#define DRONE_CFG_DISPENSER_ARM_TIMEOUT_MS 0UL
#endif
#ifndef DRONE_CFG_INTERLOCK_PIN
#define DRONE_CFG_INTERLOCK_PIN -1
#endif
#ifndef DRONE_CFG_INTERLOCK_ACTIVE_HIGH
#define DRONE_CFG_INTERLOCK_ACTIVE_HIGH 1
#endif
#ifndef DRONE_CFG_STATUS_CONNECTION_PIN
#define DRONE_CFG_STATUS_CONNECTION_PIN 23
#endif
#ifndef DRONE_CFG_STATUS_ACTIVITY_PIN
#define DRONE_CFG_STATUS_ACTIVITY_PIN 5
#endif
#ifndef DRONE_CFG_STATUS_LED_ACTIVE_HIGH
#define DRONE_CFG_STATUS_LED_ACTIVE_HIGH 1
#endif
#ifndef DRONE_CFG_WIFI_AP_SSID
#define DRONE_CFG_WIFI_AP_SSID "Drone-Gel-Controller"
#endif
#ifndef DRONE_CFG_WIFI_AP_PASSWORD
#define DRONE_CFG_WIFI_AP_PASSWORD "password"
#endif
#ifndef DRONE_CFG_STEPPER_IN1_PIN
#define DRONE_CFG_STEPPER_IN1_PIN 18
#endif
#ifndef DRONE_CFG_STEPPER_IN2_PIN
#define DRONE_CFG_STEPPER_IN2_PIN 19
#endif
#ifndef DRONE_CFG_STEPPER_IN3_PIN
#define DRONE_CFG_STEPPER_IN3_PIN 27
#endif
#ifndef DRONE_CFG_STEPPER_IN4_PIN
#define DRONE_CFG_STEPPER_IN4_PIN 14
#endif
#ifndef DRONE_CFG_DAC_ANALOG_PIN
#define DRONE_CFG_DAC_ANALOG_PIN 25
#endif
#ifndef DRONE_CFG_DIGITAL_OUTPUT_PIN
#define DRONE_CFG_DIGITAL_OUTPUT_PIN 26
#endif

#define APP_ENABLE_WIFI
#define APP_ENABLE_BLE
// #define APP_ENABLE_CLASSIC_BT_SPP

#define APP_ENABLE_HTTP_OTA
#define APP_ENABLE_STATUS_INDICATORS
#define APP_ENABLE_STATUS_LED_BOOT_TEST
// #define APP_ENABLE_MDNS
// #define APP_ENABLE_AUX_UART

// #define APP_ENABLE_VERBOSE_COEX_HTTP_DIAGNOSTICS

// Normalize the build switches into numeric macros that are safe in #if.

#ifdef APP_ADDON_DRONE_DISPENSER
#define APP_DRONE_DISPENSER_ADDON_ENABLED 1
#else
#define APP_DRONE_DISPENSER_ADDON_ENABLED 0
#endif

#ifdef APP_ADDON_STEPPER_DAC
#define APP_STEPPER_DAC_ADDON_ENABLED 1
#else
#define APP_STEPPER_DAC_ADDON_ENABLED 0
#endif

#if APP_DRONE_DISPENSER_ADDON_ENABLED && APP_STEPPER_DAC_ADDON_ENABLED
#error Select only one hardware addon: drone dispenser or stepper/DAC.
#endif
#if DRONE_CFG_BOARD_ESP32_S3 && APP_STEPPER_DAC_ADDON_ENABLED
#error ESP32-S3 has no internal analog DAC; use the dispenser profile (1).
#endif

#ifdef APP_ENABLE_WIFI
#define APP_WIFI_ENABLED 1
#else
#define APP_WIFI_ENABLED 0
#endif

#ifdef APP_ENABLE_BLE
#define APP_BLE_ENABLED 1
#else
#define APP_BLE_ENABLED 0
#endif

#ifdef APP_ENABLE_CLASSIC_BT_SPP
#define APP_CLASSIC_BT_SPP_ENABLED 1
#else
#define APP_CLASSIC_BT_SPP_ENABLED 0
#endif
#if DRONE_CFG_BOARD_ESP32_S3 && APP_CLASSIC_BT_SPP_ENABLED
#error ESP32-S3 supports BLE only; disable APP_ENABLE_CLASSIC_BT_SPP.
#endif

#if APP_BLE_ENABLED || APP_CLASSIC_BT_SPP_ENABLED
#define APP_BLUETOOTH_ENABLED 1
#else
#define APP_BLUETOOTH_ENABLED 0
#endif

#ifdef APP_ENABLE_AUX_UART
#define APP_AUX_UART_ENABLED 1
#else
#define APP_AUX_UART_ENABLED 0
#endif

#if APP_WIFI_ENABLED && defined(APP_ENABLE_MDNS)
#define APP_MDNS_ENABLED 1
#else
#define APP_MDNS_ENABLED 0
#endif

#if APP_WIFI_ENABLED && defined(APP_ENABLE_HTTP_OTA)
#define APP_HTTP_OTA_ENABLED 1
#else
#define APP_HTTP_OTA_ENABLED 0
#endif

#ifdef APP_ENABLE_STATUS_INDICATORS
#define APP_STATUS_INDICATORS_ENABLED 1
#else
#define APP_STATUS_INDICATORS_ENABLED 0
#endif

#if APP_STATUS_INDICATORS_ENABLED && defined(APP_ENABLE_STATUS_LED_BOOT_TEST)
#define APP_STATUS_LED_BOOT_TEST_ENABLED 1
#else
#define APP_STATUS_LED_BOOT_TEST_ENABLED 0
#endif

#if APP_WIFI_ENABLED && APP_BLE_ENABLED && defined(APP_ENABLE_VERBOSE_COEX_HTTP_DIAGNOSTICS)
#define APP_VERBOSE_COEX_HTTP_DIAGNOSTICS 1
#else
#define APP_VERBOSE_COEX_HTTP_DIAGNOSTICS 0
#endif

#ifndef CONFIG_ASYNC_TCP_STACK_SIZE
#define CONFIG_ASYNC_TCP_STACK_SIZE 16384
#endif

#ifndef CONFIG_ASYNC_TCP_RUNNING_CORE
#define CONFIG_ASYNC_TCP_RUNNING_CORE -1
#endif

namespace AppConfig {

constexpr bool BOARD_ESP32_S3 = DRONE_CFG_BOARD_ESP32_S3 != 0;
constexpr const char *BOARD_NAME = BOARD_ESP32_S3 ? "ESP32-S3-DevKitC-1-N16R8" : "WEMOS LOLIN32";
constexpr int MAX_GPIO_NUMBER = BOARD_ESP32_S3 ? 48 : 39;

// QuickConfig.h owns the approachable pinout and payload defaults. Edit below
// for queue sizes, advanced limits, feature switches, and transport throttling.
// Keep these values compile-time constants so ESP32 memory use is predictable.

constexpr uint32_t USB_BAUD = 115200;

// Optional auxiliary hardware UART output. Its build switch is above in this file.
constexpr bool ENABLE_DRONE_DISPENSER_ADDON = APP_DRONE_DISPENSER_ADDON_ENABLED != 0;
constexpr bool ENABLE_STEPPER_DAC_ADDON = APP_STEPPER_DAC_ADDON_ENABLED != 0;
constexpr bool ENABLE_AUX_UART = APP_AUX_UART_ENABLED != 0;
constexpr uint8_t AUX_UART_PORT = 1;
constexpr int AUX_UART_RX_PIN = -1;
constexpr int AUX_UART_TX_PIN = -1;
constexpr uint32_t AUX_UART_BAUD = 115200;
// Logical name used by firmware status, logs, and the web UI. On a classic
// LOLIN32 the operating-system USB product name comes from the external
// USB-to-UART bridge and cannot be changed by ESP32 application firmware.
constexpr const char *USB_SERIAL_NAME = "Drone-Gel-Controller";
constexpr const char *FIRMWARE_NAME = "Drone Gel Dispenser Controller";
constexpr const char *FIRMWARE_VERSION = "2026-10-30-s3-rgb";

// A fresh or erased controller always uses the safe production boot policy.
// DebugMode can still be selected explicitly for a supervised bench session.
constexpr bool DEFAULT_PRODUCTION_MODE = true;

// PRIMARY PAYLOAD OUTPUT
// The board's selected GPIO controls the external switch; it never carries the gel
// signal itself. The external circuit should hold the switch inactive while
// the ESP32 is reset or unpowered.
constexpr int PIN_DRV8871_IN1 = DRONE_CFG_DRV8871_IN1_PIN;
constexpr int PIN_DRV8871_IN2 = DRONE_CFG_DRV8871_IN2_PIN;
struct Drv8871Pins { int in1; int in2; };
constexpr Drv8871Pins DRV8871_MOTORS[] = DRONE_CFG_DRV8871_MOTOR_PINS;
constexpr size_t DRV8871_MOTOR_COUNT = sizeof(DRV8871_MOTORS) / sizeof(DRV8871_MOTORS[0]);
constexpr uint32_t DRV8871_MAX_RUN_MS = DRONE_CFG_DRV8871_MAX_RUN_MS;
constexpr int PIN_DISPENSER = DRONE_CFG_DISPENSER_PIN;
constexpr bool DISPENSER_ACTIVE_HIGH = DRONE_CFG_DISPENSER_ACTIVE_HIGH != 0;
constexpr uint32_t DISPENSER_DEFAULT_PULSE_MS = DRONE_CFG_DISPENSER_DEFAULT_PULSE_MS;
constexpr uint32_t DISPENSER_MAX_PULSE_MS = DRONE_CFG_DISPENSER_MAX_PULSE_MS;
constexpr uint32_t DISPENSER_ARM_TIMEOUT_MS = DRONE_CFG_DISPENSER_ARM_TIMEOUT_MS;
// Named runtime payload profiles use fixed-size RAM and checksummed NVS records.
// Their per-profile limits may be lower, never higher, than the ceilings above.
constexpr uint8_t DISPENSER_PROFILE_MAX_COUNT = 4;
constexpr uint8_t DISPENSER_PROFILE_NAME_BYTES = 15;
constexpr uint8_t DISPENSER_MAX_OUTPUTS = 8;
// Set to a valid input GPIO to require an external flight-controller or
// physical enable signal. -1 compiles the optional interlock out.
constexpr int PIN_DISPENSER_INTERLOCK = DRONE_CFG_INTERLOCK_PIN;
constexpr bool DISPENSER_INTERLOCK_ACTIVE_HIGH = DRONE_CFG_INTERLOCK_ACTIVE_HIGH != 0;

constexpr int PIN_STEPPER_IN1 = DRONE_CFG_STEPPER_IN1_PIN;
constexpr int PIN_STEPPER_IN2 = DRONE_CFG_STEPPER_IN2_PIN;
constexpr int PIN_STEPPER_IN3 = DRONE_CFG_STEPPER_IN3_PIN;
constexpr int PIN_STEPPER_IN4 = DRONE_CFG_STEPPER_IN4_PIN;

// STATUS LED CONFIGURATION
// Runtime and test routines live in src/hardware/StatusIndicators.h/.cpp.
// Comment out APP_ENABLE_STATUS_INDICATORS above to compile out all LED GPIO
// ownership. Comment out only APP_ENABLE_STATUS_LED_BOOT_TEST to keep runtime
// transport indication without the startup blink sequence.
constexpr bool ENABLE_STATUS_INDICATORS = APP_STATUS_INDICATORS_ENABLED != 0;
constexpr bool STATUS_USE_RGB = BOARD_ESP32_S3;
constexpr int PIN_STATUS_RGB = DRONE_CFG_STATUS_RGB_PIN;
constexpr uint8_t STATUS_RGB_BRIGHTNESS = DRONE_CFG_STATUS_RGB_BRIGHTNESS;
constexpr int PIN_STATUS_CONNECTION = STATUS_USE_RGB ? -1 : DRONE_CFG_STATUS_CONNECTION_PIN;
constexpr int PIN_STATUS_ACTIVITY = STATUS_USE_RGB ? -1 : DRONE_CFG_STATUS_ACTIVITY_PIN;
constexpr bool STATUS_LED_ACTIVE_HIGH = DRONE_CFG_STATUS_LED_ACTIVE_HIGH != 0;
constexpr uint32_t CONNECTION_LED_BLINK_MS = 400;
constexpr uint32_t ACTIVITY_LED_PULSE_MS = 180;
constexpr bool STATUS_LED_BOOT_SELF_TEST = APP_STATUS_LED_BOOT_TEST_ENABLED != 0;
constexpr uint8_t STATUS_CONNECTION_BOOT_SELF_TEST_CYCLES = 2;
constexpr uint8_t STATUS_ACTIVITY_BOOT_SELF_TEST_CYCLES = 3;
constexpr uint16_t STATUS_LED_BOOT_SELF_TEST_ON_MS = 220;
constexpr uint16_t STATUS_LED_BOOT_SELF_TEST_OFF_MS = 180;
constexpr uint16_t STATUS_LED_PRODUCTION_CURSORY_TEST_MS = 350;
constexpr uint8_t STATUS_SINGLE_PIN_TEST_CYCLES = 4;

// Addon output configuration. Channel 1 is the ESP32 analog DAC. Channel 2
// uses GPIO26 as a normal push-pull digital output for a clean 0/3.3 V state.
constexpr int PIN_DAC_1 = DRONE_CFG_DAC_ANALOG_PIN;
constexpr int PIN_DIGITAL_OUTPUT = DRONE_CFG_DIGITAL_OUTPUT_PIN;
constexpr uint8_t DAC_CHANNEL_COUNT = 2;
constexpr uint8_t DAC_ANALOG_CHANNEL_INDEX = 0;
constexpr uint8_t DIGITAL_OUTPUT_CHANNEL_INDEX = 1;
constexpr uint16_t DAC_DEFAULT_MV = 500;
constexpr uint16_t DAC_DEFAULT_REFERENCE_MV = 3300;
constexpr uint32_t DAC_TEST_DURATION_MS = 3000;
constexpr uint32_t DAC_TIMER_MAX_ARM_MS = 60000;

constexpr bool ENABLE_WIFI = APP_WIFI_ENABLED != 0;
constexpr bool ENABLE_BLE = APP_BLE_ENABLED != 0;
constexpr bool ENABLE_CLASSIC_BT_SPP = APP_CLASSIC_BT_SPP_ENABLED != 0;
constexpr const char *CLASSIC_BT_NAME = "DroneGelCmd";
constexpr const char *BLE_NAME = "DroneGelBLE";
constexpr const char *BLE_UART_SERVICE_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr const char *BLE_UART_RX_UUID = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr const char *BLE_UART_TX_UUID = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr const char *BLE_MAVLINK_RX_UUID = "6E400004-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr size_t BLE_SAFE_PAYLOAD_BYTES = 20;
constexpr uint32_t BLE_NOTIFY_INTERVAL_MS = 50;
constexpr uint32_t BLE_NOTIFY_CONNECT_SETTLE_MS = 350;
constexpr uint32_t BLE_NOTIFY_SUBSCRIPTION_SETTLE_MS = 250;
constexpr uint32_t BLE_IDLE_COMMAND_SUBMIT_MS = 140;
// A 20-byte GATT notification does not require the same reserve as bringing up
// another radio or HTTP listener. Pause only at the hard floor, retain queued
// output, and resume automatically when transient HTTP allocations are released.
constexpr uint32_t BLE_TX_MIN_FREE_HEAP_BYTES = 10500;
constexpr uint32_t BLE_TX_MIN_LARGEST_BLOCK_BYTES = 5000;
constexpr uint32_t BLE_TX_PRESSURE_WARNING_INTERVAL_MS = 5000;
constexpr size_t BLE_DIRECT_OUTPUT_BUFFER_BYTES = 2816;
// BluetoothSerial.begin() returns before the asynchronous SPP init callback has
// necessarily completed. Do not call GAP or BLE APIs until isReady() succeeds.
constexpr uint32_t BLUETOOTH_READY_TIMEOUT_MS = 5000;
constexpr uint32_t BLUETOOTH_READY_SETTLE_MS = 25;

constexpr const char *DEFAULT_WIFI_AP_SSID = DRONE_CFG_WIFI_AP_SSID;
constexpr const char *DEFAULT_WIFI_AP_PASSWORD = DRONE_CFG_WIFI_AP_PASSWORD;
constexpr const char *MDNS_HOSTNAME = "drone-gel";
// Direct AP access uses 192.168.4.1. mDNS is compiled out by default because
// the responder consumes flash and scarce internal RAM beside Wi-Fi and BLE.
constexpr bool ENABLE_MDNS = APP_MDNS_ENABLED != 0;
constexpr uint32_t WIFI_STA_CONNECT_TIMEOUT_MS = 15000;
// Named Wi-Fi power profiles used by commands, configuration readback, and the portal.
constexpr int8_t WIFI_TX_POWER_LOW_QUARTER_DBM = 44;   // 11 dBm
constexpr int8_t WIFI_TX_POWER_MAX_QUARTER_DBM = 78;   // 19.5 dBm
constexpr uint32_t RADIO_APPLY_GRACE_MS = 2200;
// WIFI_BLE and WIFI_BLE_P reserve the BLE controller while the Arduino setup
// task still has a large contiguous heap, then start Wi-Fi/HTTP before the GATT
// service. WIFI_BLE may pause idle advertising; WIFI_BLE_P keeps both persistent.
constexpr uint32_t WIFI_BLE_BLE_TO_WIFI_SETTLE_MS = 120;
constexpr uint32_t RADIO_HANDOFF_RELEASE_MS = 1800;
constexpr uint32_t BLE_WEB_HANDOFF_WINDOW_MS = 45000;
constexpr uint32_t BLE_DORMANCY_HEAP_CHECK_DELAY_MS = 250;
constexpr bool VERBOSE_COEX_HTTP_DIAGNOSTICS = APP_VERBOSE_COEX_HTTP_DIAGNOSTICS != 0;
constexpr uint32_t COEX_HTTP_DIAGNOSTIC_INTERVAL_MS = 5000;
constexpr uint32_t COEX_HTTP_NO_REQUEST_WARNING_MS = 10000;
// Radio profile changes use a two-boot transaction. The first reboot tests the
// requested stack, and only a healthy stabilized boot becomes last-known-good.
constexpr uint32_t RADIO_MODE_REBOOT_DELAY_MS = 1600;
constexpr uint32_t RADIO_TRIAL_TIMEOUT_MS = 30000;
constexpr uint32_t RADIO_TRIAL_STABILIZE_MS = 2200;
constexpr uint32_t RADIO_TRIAL_MIN_FREE_HEAP_BYTES = 16000;
constexpr uint32_t RADIO_TRIAL_MIN_LARGEST_BLOCK_BYTES = 8000;

constexpr uint16_t EVENT_CAPACITY = 16;
constexpr size_t EVENT_TEXT_BYTES = 192;
constexpr uint8_t WEB_EVENT_DEFAULT_LIMIT = 4;
constexpr uint8_t WEB_EVENT_MAX_LIMIT = 6;
constexpr uint32_t WEB_STATE_CACHE_INTERVAL_MS = 800;
constexpr size_t WEB_STATE_JSON_BUDGET_BYTES = 2560;
constexpr size_t WEB_EVENT_JSON_BUDGET_BYTES = 768;
// Embedded portal responses are paced from PROGMEM so Wi-Fi/BLE coexistence
// never retains an entire TCP window of page data.
constexpr size_t HTTP_ASSET_CHUNK_BYTES = 512;
constexpr size_t HTTP_JSON_RESPONSE_BUDGET_BYTES = 2304;
constexpr uint16_t HTTP_COMMAND_MAX_BYTES = 2048;
// The portal admits only a small number of simultaneous requests. A browser
// that disconnects releases its slot through AsyncWebServerRequest::onDisconnect.
// Command bodies and response payloads share one bounded reservation pool. New
// work is aborted before another allocation when heap headroom or the pool would
// cross these coexistence-safe limits.
constexpr uint8_t HTTP_MAX_ACTIVE_REQUESTS = 2;
constexpr size_t HTTP_MAX_ACTIVE_RESERVED_BYTES = 5120;
// Admission is checked against this remaining-heap floor plus each route's
// declared reservation. This permits small state/event/command requests during
// normal Wi-Fi + BLE coexistence while still refusing work before exhaustion.
constexpr uint32_t HTTP_REMAINING_FREE_HEAP_BYTES = 9000;
constexpr uint32_t HTTP_MIN_LARGEST_BLOCK_BYTES = 6000;
constexpr uint8_t HTTP_REQUEST_HISTORY_SIZE = 4;
constexpr uint32_t HTTP_REQUEST_HISTORY_TTL_MS = 30000;
constexpr uint32_t HTTP_CLIENT_RX_TIMEOUT_SECONDS = 4;
constexpr uint32_t HTTP_CLIENT_ACK_TIMEOUT_MS = 2500;

// HTTP OTA uses the inactive application partition and a multipart browser
// upload. The BLE console can hand off to the same Wi-Fi endpoint before the
// binary stream begins, avoiding a second large BLE service in coexistence RAM.
constexpr bool ENABLE_HTTP_OTA = APP_HTTP_OTA_ENABLED != 0;
constexpr uint8_t OTA_IMAGE_MAGIC = 0xE9;
constexpr size_t OTA_MIN_IMAGE_BYTES = 1024;
constexpr size_t OTA_REQUEST_RESERVATION_BYTES = 1536;
constexpr uint32_t OTA_REBOOT_DELAY_MS = 2500;

constexpr size_t USB_INPUT_READ_BUDGET = 128;
constexpr size_t SPP_INPUT_READ_BUDGET = 128;
constexpr size_t BLE_INPUT_READ_BUDGET = 128;
constexpr size_t UART_INPUT_READ_BUDGET = 128;
constexpr uint8_t USB_EVENT_FILL_BUDGET = 10;
constexpr uint8_t SPP_EVENT_FILL_BUDGET = 4;
constexpr uint8_t BLE_EVENT_FILL_BUDGET = 2;
constexpr uint8_t UART_EVENT_FILL_BUDGET = 4;
constexpr size_t USB_WRITE_BUDGET = 256;
constexpr size_t SPP_WRITE_BUDGET = 128;
constexpr size_t UART_WRITE_BUDGET = 128;
constexpr size_t TRANSPORT_OUTPUT_BUFFER_BYTES = 1024;

constexpr uint8_t COMMAND_QUEUE_CAPACITY = 8;
constexpr uint8_t COMMANDS_PER_SERVICE = 8;
constexpr uint16_t COMMAND_MAX_BYTES = 256;
constexpr uint8_t REQUEST_ID_MAX_BYTES = 23;

// SAVED ROUTINES
// Routines are bounded fixed-size records so RAM and NVS use remain predictable.
// They never arm the dispenser automatically and StopAll always cancels them.
constexpr uint8_t ROUTINE_MAX_COUNT = 4;
constexpr uint8_t ROUTINE_MAX_STEPS = 10;
constexpr uint8_t ROUTINE_NAME_BYTES = 15;
// The longest supported routine hardware command is well below 48 bytes.
// Keeping this bounded saves about 1.25 KiB of always-resident RAM across the
// four fixed routine slots compared with the former general command length.
constexpr uint8_t ROUTINE_COMMAND_BYTES = 48;
constexpr uint32_t ROUTINE_MAX_WAIT_MS = UINT32_MAX; // Timer representation, not a routine cap.
// MAVLink common position receiver; forward whole UDP frames to this port.
constexpr uint16_t GEO_MAX_POINTS = 256;
constexpr uint16_t GEO_MAVLINK_UDP_PORT = 14550;
constexpr uint8_t GEO_MAVLINK_SYSTEM_ID = 1;
constexpr uint8_t GEO_MAVLINK_COMPONENT_ID = 1;

// User-setting guardrails. These cost no flash or RAM and turn unsafe pin
// or timing combinations into clear compiler errors instead of field failures.
constexpr bool isClassicEsp32OutputPin(int pin) {
  return (pin >= 0 && pin <= 5) || (pin >= 12 && pin <= 19) ||
    (pin >= 21 && pin <= 23) || (pin >= 25 && pin <= 27) || pin == 32 || pin == 33;
}

constexpr bool isConservativePayloadOutputPin(int pin) {
  // N16R8: exclude memory GPIO26..37 (including octal PSRAM GPIO35..37),
  // nonexistent GPIO22..25, USB GPIO19/20, UART0 GPIO43/44, strapping
  // GPIO0/3/45/46, and both onboard RGB revision pins (38/48).
  if (BOARD_ESP32_S3) {
    return pin == 1 || pin == 2 || (pin >= 4 && pin <= 18) || pin == 21 ||
      (pin >= 39 && pin <= 42) || pin == 47;
  }
  return
    pin == 13 || pin == 14 || pin == 16 || pin == 17 || pin == 18 ||
    pin == 19 || pin == 21 || pin == 22 || pin == 23 || pin == 25 || pin == 26 ||
    pin == 27 || pin == 32 || pin == 33;
}

constexpr bool isBoardInputPin(int pin) {
  return BOARD_ESP32_S3 ? isConservativePayloadOutputPin(pin) :
    (isClassicEsp32OutputPin(pin) || (pin >= 34 && pin <= 39));
}

constexpr bool isStatusIndicatorPin(int pin) {
  return ENABLE_STATUS_INDICATORS && (STATUS_USE_RGB ? pin == PIN_STATUS_RGB :
    (pin == PIN_STATUS_CONNECTION || pin == PIN_STATUS_ACTIVITY));
}

static_assert(!ENABLE_STATUS_INDICATORS || !STATUS_USE_RGB ||
  PIN_STATUS_RGB == 48 || PIN_STATUS_RGB == 38,
  "DevKitC-1 RGB LED must use GPIO48 (initial) or GPIO38 (v1.1).");
static_assert(DRONE_CFG_STATUS_RGB_BRIGHTNESS >= 1 && DRONE_CFG_STATUS_RGB_BRIGHTNESS <= 255,
  "RGB brightness must be 1..255.");

constexpr bool isDrv8871Pin(int pin) {
  if (!DRONE_CFG_ENABLE_DRV8871) return false;
  for (const auto &motor : DRV8871_MOTORS) {
    if (pin == motor.in1 || pin == motor.in2) return true;
  }
  return false;
}

constexpr bool isAvailableDrv8871Pin(int pin) {
  return isConservativePayloadOutputPin(pin) && !isStatusIndicatorPin(pin) &&
    pin != PIN_DISPENSER && pin != PIN_DISPENSER_INTERLOCK &&
    (!ENABLE_AUX_UART || (pin != AUX_UART_RX_PIN && pin != AUX_UART_TX_PIN));
}

static_assert(!DRONE_CFG_ENABLE_DRV8871 || ENABLE_DRONE_DISPENSER_ADDON,
  "DRV8871 requires the dispenser hardware profile.");
constexpr bool validDrv8871Pins() {
  for (size_t i = 0; i < DRV8871_MOTOR_COUNT; ++i) {
    const auto &motor = DRV8871_MOTORS[i];
    if (!isAvailableDrv8871Pin(motor.in1) || !isAvailableDrv8871Pin(motor.in2) || motor.in1 == motor.in2) {
      return false;
    }
    for (size_t j = 0; j < i; ++j) {
      const auto &earlier = DRV8871_MOTORS[j];
      if (motor.in1 == earlier.in1 || motor.in1 == earlier.in2 ||
          motor.in2 == earlier.in1 || motor.in2 == earlier.in2) return false;
    }
  }
  return true;
}

static_assert(DRV8871_MOTOR_COUNT >= 1 && DRV8871_MOTOR_COUNT <= 4, "Configure 1 to 4 DRV8871 pin pairs.");
static_assert(!DRONE_CFG_ENABLE_DRV8871 || validDrv8871Pins(),
  "DRV8871 inputs need distinct GPIOs free of payload, interlock, LED and auxiliary UART ownership.");
static_assert(!DRONE_CFG_ENABLE_DRV8871 || (DRV8871_MAX_RUN_MS > 0 && DRV8871_MAX_RUN_MS <= 2147483647UL),
  "DRV8871 maximum runtime must be 1..2147483647 ms.");

static_assert(
  !ENABLE_DRONE_DISPENSER_ADDON ||
    DRONE_CFG_DISPENSER_ACTIVE_HIGH == 0 || DRONE_CFG_DISPENSER_ACTIVE_HIGH == 1,
  "DRONE_CFG_DISPENSER_ACTIVE_HIGH must be 0 or 1."
);
static_assert(
  !ENABLE_DRONE_DISPENSER_ADDON ||
    DRONE_CFG_INTERLOCK_ACTIVE_HIGH == 0 || DRONE_CFG_INTERLOCK_ACTIVE_HIGH == 1,
  "DRONE_CFG_INTERLOCK_ACTIVE_HIGH must be 0 or 1."
);
static_assert(
  DRONE_CFG_STATUS_LED_ACTIVE_HIGH == 0 || DRONE_CFG_STATUS_LED_ACTIVE_HIGH == 1,
  "DRONE_CFG_STATUS_LED_ACTIVE_HIGH must be 0 or 1."
);
static_assert(
  !ENABLE_DRONE_DISPENSER_ADDON || isConservativePayloadOutputPin(PIN_DISPENSER),
  "Dispenser GPIO is not in the selected board's conservative output allowlist."
);
static_assert(
  !ENABLE_DRONE_DISPENSER_ADDON || DISPENSER_DEFAULT_PULSE_MS > 0,
  "Default dispenser pulse must be positive."
);
static_assert(
  !ENABLE_DRONE_DISPENSER_ADDON || DISPENSER_MAX_PULSE_MS == 0 || DISPENSER_DEFAULT_PULSE_MS <= DISPENSER_MAX_PULSE_MS,
  "Default dispenser pulse exceeds the maximum pulse limit."
);
static_assert(
  !ENABLE_DRONE_DISPENSER_ADDON || DISPENSER_ARM_TIMEOUT_MS == 0 || DISPENSER_MAX_PULSE_MS <= DISPENSER_ARM_TIMEOUT_MS,
  "Dispenser arm timeout must accommodate one maximum-length pulse."
);
static_assert(
  !ENABLE_DRONE_DISPENSER_ADDON || PIN_DISPENSER_INTERLOCK == -1 ||
    isBoardInputPin(PIN_DISPENSER_INTERLOCK),
  "Interlock must be -1 or an available input on the selected board."
);
static_assert(
  !ENABLE_DRONE_DISPENSER_ADDON || PIN_DISPENSER_INTERLOCK < 0 ||
    PIN_DISPENSER_INTERLOCK != PIN_DISPENSER,
  "Dispenser output and interlock cannot share a GPIO."
);
static_assert(
  !ENABLE_STATUS_INDICATORS || STATUS_USE_RGB ||
    (isClassicEsp32OutputPin(PIN_STATUS_CONNECTION) &&
      isClassicEsp32OutputPin(PIN_STATUS_ACTIVITY) &&
      PIN_STATUS_CONNECTION != PIN_STATUS_ACTIVITY),
  "Status LEDs require two different output-capable, non-flash GPIOs."
);
static_assert(
  !ENABLE_DRONE_DISPENSER_ADDON || PIN_DISPENSER_INTERLOCK < 0 ||
    !isStatusIndicatorPin(PIN_DISPENSER_INTERLOCK),
  "Dispenser interlock conflicts with a status-indicator GPIO."
);
static_assert(
  !ENABLE_DRONE_DISPENSER_ADDON || !isStatusIndicatorPin(PIN_DISPENSER),
  "Dispenser output conflicts with a status-indicator GPIO."
);
static_assert(
  !ENABLE_DRONE_DISPENSER_ADDON || !ENABLE_AUX_UART ||
    (PIN_DISPENSER != AUX_UART_RX_PIN && PIN_DISPENSER != AUX_UART_TX_PIN),
  "Dispenser output conflicts with an auxiliary-UART GPIO."
);
static_assert(!ENABLE_AUX_UART ||
  (isConservativePayloadOutputPin(AUX_UART_RX_PIN) &&
    isConservativePayloadOutputPin(AUX_UART_TX_PIN) && AUX_UART_RX_PIN != AUX_UART_TX_PIN &&
    !isStatusIndicatorPin(AUX_UART_RX_PIN) && !isStatusIndicatorPin(AUX_UART_TX_PIN) &&
    (PIN_DISPENSER_INTERLOCK < 0 || (PIN_DISPENSER_INTERLOCK != AUX_UART_RX_PIN &&
      PIN_DISPENSER_INTERLOCK != AUX_UART_TX_PIN))),
  "Aux UART needs two available, distinct board GPIOs without indicator/interlock conflicts.");
static_assert(
  !ENABLE_STEPPER_DAC_ADDON ||
    (isConservativePayloadOutputPin(PIN_STEPPER_IN1) &&
      isConservativePayloadOutputPin(PIN_STEPPER_IN2) &&
      isConservativePayloadOutputPin(PIN_STEPPER_IN3) &&
      isConservativePayloadOutputPin(PIN_STEPPER_IN4)),
  "Stepper pins must use conservative output-capable classic-ESP32 GPIOs."
);
static_assert(
  !ENABLE_STEPPER_DAC_ADDON ||
    (PIN_STEPPER_IN1 != PIN_STEPPER_IN2 && PIN_STEPPER_IN1 != PIN_STEPPER_IN3 &&
      PIN_STEPPER_IN1 != PIN_STEPPER_IN4 && PIN_STEPPER_IN2 != PIN_STEPPER_IN3 &&
      PIN_STEPPER_IN2 != PIN_STEPPER_IN4 && PIN_STEPPER_IN3 != PIN_STEPPER_IN4),
  "The four stepper inputs must use four different GPIOs."
);
static_assert(
  !ENABLE_STEPPER_DAC_ADDON || PIN_DAC_1 == 25 || PIN_DAC_1 == 26,
  "Classic ESP32 analog DAC output must use GPIO25 or GPIO26."
);
static_assert(
  !ENABLE_STEPPER_DAC_ADDON || isConservativePayloadOutputPin(PIN_DIGITAL_OUTPUT),
  "Advanced digital output is not in the conservative GPIO allowlist."
);
static_assert(
  !ENABLE_STEPPER_DAC_ADDON ||
    (PIN_DAC_1 != PIN_DIGITAL_OUTPUT &&
      PIN_DAC_1 != PIN_STEPPER_IN1 && PIN_DAC_1 != PIN_STEPPER_IN2 &&
      PIN_DAC_1 != PIN_STEPPER_IN3 && PIN_DAC_1 != PIN_STEPPER_IN4 &&
      PIN_DIGITAL_OUTPUT != PIN_STEPPER_IN1 && PIN_DIGITAL_OUTPUT != PIN_STEPPER_IN2 &&
      PIN_DIGITAL_OUTPUT != PIN_STEPPER_IN3 && PIN_DIGITAL_OUTPUT != PIN_STEPPER_IN4),
  "Stepper, analog DAC, and digital output pins must not overlap."
);
static_assert(
  !ENABLE_STEPPER_DAC_ADDON || !ENABLE_STATUS_INDICATORS ||
    (PIN_STATUS_CONNECTION != PIN_STEPPER_IN1 &&
      PIN_STATUS_CONNECTION != PIN_STEPPER_IN2 &&
      PIN_STATUS_CONNECTION != PIN_STEPPER_IN3 &&
      PIN_STATUS_CONNECTION != PIN_STEPPER_IN4 &&
      PIN_STATUS_CONNECTION != PIN_DAC_1 &&
      PIN_STATUS_CONNECTION != PIN_DIGITAL_OUTPUT &&
      PIN_STATUS_ACTIVITY != PIN_STEPPER_IN1 &&
      PIN_STATUS_ACTIVITY != PIN_STEPPER_IN2 &&
      PIN_STATUS_ACTIVITY != PIN_STEPPER_IN3 &&
      PIN_STATUS_ACTIVITY != PIN_STEPPER_IN4 &&
      PIN_STATUS_ACTIVITY != PIN_DAC_1 &&
      PIN_STATUS_ACTIVITY != PIN_DIGITAL_OUTPUT),
  "Advanced hardware pins conflict with a status-indicator GPIO."
);
static_assert(
  sizeof(DRONE_CFG_WIFI_AP_SSID) > 1 && sizeof(DRONE_CFG_WIFI_AP_SSID) <= 33,
  "Wi-Fi AP SSID must be a quoted string containing 1 to 32 characters."
);
static_assert(
  sizeof(DRONE_CFG_WIFI_AP_PASSWORD) >= 9 && sizeof(DRONE_CFG_WIFI_AP_PASSWORD) <= 64,
  "Wi-Fi AP password must be a quoted string containing 8 to 63 characters."
);
static_assert(
  DISPENSER_PROFILE_MAX_COUNT > 0 && DISPENSER_PROFILE_NAME_BYTES > 0,
  "Payload profile capacity and name length must be nonzero."
);
static_assert(ROUTINE_MAX_COUNT > 0 && ROUTINE_MAX_STEPS > 0, "Routine capacity must be nonzero.");
static_assert(
  ROUTINE_COMMAND_BYTES <= COMMAND_MAX_BYTES,
  "Stored routine commands cannot exceed the dispatcher command limit."
);

constexpr uint8_t MOTOR_COMMAND_QUEUE_CAPACITY = 8;
// Safety/overflow bounds for user-editable motor commands. Raise these here if
// a different mechanism legitimately needs a larger range.
constexpr long MOTOR_MAX_BASE_STEPS_PER_REV = 1000000L;
constexpr long MOTOR_MAX_MOVE_STEPS = 100000000L;
constexpr float MOTOR_MIN_CONFIGURED_RPM = 0.1f;
constexpr float MOTOR_MAX_CONFIGURED_RPM = 10000.0f;
constexpr float MOTOR_MAX_RAMP_RPM_PER_SECOND = 100000.0f;
// Keep every scheduled step interval below INT32_MAX microseconds because the
// deadline comparisons intentionally use signed wrap-safe differences.
constexpr uint32_t MOTOR_MAX_STEP_INTERVAL_US = 1000000000UL;
// 28BYJ-48 + ULN2003 motion profile. This geared 5 V motor is reliable in
// the low-RPM range; commanding 100 output-shaft RPM only makes it stall and
// buzz. Keep the defaults together here so the boot test, runtime commands,
// and web interface all describe the same hardware.
constexpr float STEPPER_DEFAULT_MIN_RPM = 1.0f;
constexpr float STEPPER_DEFAULT_MAX_RPM = 20.0f;
constexpr float STEPPER_DEFAULT_START_RPM = 2.0f;
constexpr float STEPPER_DEFAULT_RAMP_RPM_PER_SECOND = 15.0f;
constexpr float STEPPER_FAST_TEST_RPM = 15.0f;
constexpr float STEPPER_DEBUG_RPM = 10.0f;

// Debug-mode boot hardware confirmation. Production mode skips the DAC and
// stepper actuation tests and performs only the brief status-indicator pulse.
constexpr bool RUN_DAC1_BOOT_TEST = false;

// Boot-time visual confirmation. The test ramps to 10 RPM, returns the motor
// to its original position, and releases the coils. Set this false to disable.
constexpr bool RUN_STEPPER_BOOT_TEST = false;
constexpr float STEPPER_BOOT_TEST_RPM = 10.0f;
constexpr float STEPPER_BOOT_TEST_DEGREES = 360.0f;
constexpr bool STEPPER_BOOT_TEST_IMMEDIATE = false;
constexpr uint8_t STEPPER_BOOT_TEST_CYCLES = 1;
constexpr uint16_t STEPPER_BOOT_TEST_DWELL_MS = 250;

constexpr uint32_t MOTOR_TASK_STACK_BYTES = 6144;
constexpr uint32_t MOTOR_STATE_SNAPSHOT_INTERVAL_MS = 20;
constexpr uint32_t MOTOR_SCHEDULER_BREATH_INTERVAL_MS = 100;
constexpr UBaseType_t MOTOR_TASK_PRIORITY = 5;
constexpr BaseType_t MOTOR_TASK_CORE = 1;

// The network/control task no longer initializes Bluetooth. BLE and Classic
// SPP are selected as boot transports and initialized from setup() before Wi-Fi.
// Six KiB is sufficient for bounded service calls; Arduino loop fallback remains
// available if fragmented combined-radio heap cannot reserve the task stack.
constexpr uint32_t NETWORK_TASK_STACK_BYTES = 6144;
constexpr UBaseType_t NETWORK_TASK_PRIORITY = 2;
constexpr BaseType_t NETWORK_TASK_CORE = 0;
constexpr TickType_t NETWORK_TASK_DELAY_TICKS = 1;

constexpr bool isAvailableAddonPin(int pin) {
  return isConservativePayloadOutputPin(pin) && !isStatusIndicatorPin(pin) && !isDrv8871Pin(pin) &&
    (!ENABLE_DRONE_DISPENSER_ADDON || (pin != PIN_DISPENSER && pin != PIN_DISPENSER_INTERLOCK)) &&
    (!ENABLE_STEPPER_DAC_ADDON || (pin != PIN_STEPPER_IN1 && pin != PIN_STEPPER_IN2 &&
      pin != PIN_STEPPER_IN3 && pin != PIN_STEPPER_IN4 && pin != PIN_DIGITAL_OUTPUT));
}
}  // namespace AppConfig
#include "LteConfig.h"

#endif  // DRONE_GEL_CONTROLLER_APPCONFIG_H
