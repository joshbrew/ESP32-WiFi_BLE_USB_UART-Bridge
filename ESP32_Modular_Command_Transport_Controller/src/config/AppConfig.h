#ifndef ESP32_MODULAR_CONTROLLER_APPCONFIG_H
#define ESP32_MODULAR_CONTROLLER_APPCONFIG_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>

// Build switches. Comment out a line to compile that feature out.
// Every translation unit includes this header, so the project has one normal
// configuration source and no source file includes the Arduino sketch.
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

// EDIT HERE FIRST for pins, defaults, queue sizes, timing limits, radio names,
// and transport throttling. All firmware implementation files live in src/.
// Subsystem behavior belongs in its owning controller. Keep these values
// compile-time constants so ESP32 memory use remains predictable.

constexpr uint32_t USB_BAUD = 115200;

// Optional auxiliary hardware UART output. Its build switch is above in this file.
constexpr bool ENABLE_AUX_UART = APP_AUX_UART_ENABLED != 0;
constexpr uint8_t AUX_UART_PORT = 1;
constexpr int AUX_UART_RX_PIN = -1;
constexpr int AUX_UART_TX_PIN = -1;
constexpr uint32_t AUX_UART_BAUD = 115200;
// Logical name used by firmware status, logs, and the web UI. On a classic
// LOLIN32 the operating-system USB product name comes from the external
// USB-to-UART bridge and cannot be changed by ESP32 application firmware.
constexpr const char *USB_SERIAL_NAME = "ESP32-Modular-Controller";
constexpr const char *FIRMWARE_NAME = "ESP32 Modular Command + Transport Controller";
constexpr const char *FIRMWARE_VERSION = "2026-07-10-v5.16.0-base-controller";


// STATUS LED CONFIGURATION
// Runtime and test routines live in src/hardware/StatusIndicators.h/.cpp.
// Comment out APP_ENABLE_STATUS_INDICATORS above to compile out all LED GPIO
// ownership. Comment out only APP_ENABLE_STATUS_LED_BOOT_TEST to keep runtime
// transport indication without the startup blink sequence.
constexpr bool ENABLE_STATUS_INDICATORS = APP_STATUS_INDICATORS_ENABLED != 0;
constexpr int PIN_STATUS_CONNECTION = 23;
constexpr int PIN_STATUS_ACTIVITY = 5;
constexpr bool STATUS_LED_ACTIVE_HIGH = true;
constexpr uint32_t CONNECTION_LED_BLINK_MS = 400;
constexpr uint32_t ACTIVITY_LED_PULSE_MS = 180;
constexpr bool STATUS_LED_BOOT_SELF_TEST = APP_STATUS_LED_BOOT_TEST_ENABLED != 0;
constexpr uint8_t STATUS_CONNECTION_BOOT_SELF_TEST_CYCLES = 2;
constexpr uint8_t STATUS_ACTIVITY_BOOT_SELF_TEST_CYCLES = 3;
constexpr uint16_t STATUS_LED_BOOT_SELF_TEST_ON_MS = 220;
constexpr uint16_t STATUS_LED_BOOT_SELF_TEST_OFF_MS = 180;
constexpr uint16_t STATUS_LED_PRODUCTION_CURSORY_TEST_MS = 350;
constexpr uint8_t STATUS_SINGLE_PIN_TEST_CYCLES = 4;


constexpr bool ENABLE_WIFI = APP_WIFI_ENABLED != 0;
constexpr bool ENABLE_BLE = APP_BLE_ENABLED != 0;
constexpr bool ENABLE_CLASSIC_BT_SPP = APP_CLASSIC_BT_SPP_ENABLED != 0;
constexpr const char *CLASSIC_BT_NAME = "ESP32-Controller-SPP";
constexpr const char *BLE_NAME = "ESP32-Controller-BLE";
constexpr const char *BLE_UART_SERVICE_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr const char *BLE_UART_RX_UUID = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr const char *BLE_UART_TX_UUID = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";
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
constexpr size_t BLE_DIRECT_OUTPUT_BUFFER_BYTES = 1152;
// BluetoothSerial.begin() returns before the asynchronous SPP init callback has
// necessarily completed. Do not call GAP or BLE APIs until isReady() succeeds.
constexpr uint32_t BLUETOOTH_READY_TIMEOUT_MS = 5000;
constexpr uint32_t BLUETOOTH_READY_SETTLE_MS = 25;

constexpr const char *DEFAULT_WIFI_AP_SSID = "ESP32-Controller";
constexpr const char *DEFAULT_WIFI_AP_PASSWORD = "esp32control";
constexpr const char *MDNS_HOSTNAME = "esp32-controller";
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
constexpr size_t WEB_STATE_JSON_BUDGET_BYTES = 1024;
constexpr size_t WEB_EVENT_JSON_BUDGET_BYTES = 768;
// Embedded portal responses are paced from PROGMEM so Wi-Fi/BLE coexistence
// never retains an entire TCP window of page data.
constexpr size_t HTTP_ASSET_CHUNK_BYTES = 512;
constexpr size_t HTTP_JSON_RESPONSE_BUDGET_BYTES = 1280;
constexpr uint16_t HTTP_COMMAND_MAX_BYTES = 2048;
// The portal admits only a small number of simultaneous requests. A browser
// that disconnects releases its slot through AsyncWebServerRequest::onDisconnect.
// Command bodies and response payloads share one bounded reservation pool. New
// work is aborted before another allocation when heap headroom or the pool would
// cross these coexistence-safe limits.
constexpr uint8_t HTTP_MAX_ACTIVE_REQUESTS = 2;
constexpr size_t HTTP_MAX_ACTIVE_RESERVED_BYTES = 4352;
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


// The network/control task no longer initializes Bluetooth. BLE and Classic
// SPP are selected as boot transports and initialized from setup() before Wi-Fi.
// Six KiB is sufficient for bounded service calls; Arduino loop fallback remains
// available if fragmented combined-radio heap cannot reserve the task stack.
constexpr uint32_t NETWORK_TASK_STACK_BYTES = 6144;
constexpr UBaseType_t NETWORK_TASK_PRIORITY = 2;
constexpr BaseType_t NETWORK_TASK_CORE = 0;
constexpr TickType_t NETWORK_TASK_DELAY_TICKS = 1;

}  // namespace AppConfig

#endif  // ESP32_MODULAR_CONTROLLER_APPCONFIG_H
