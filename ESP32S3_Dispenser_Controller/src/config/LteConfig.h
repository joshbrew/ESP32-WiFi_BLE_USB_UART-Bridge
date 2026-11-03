#ifndef ESP32S3_LTE_CONFIG_H
#define ESP32S3_LTE_CONFIG_H

// Optional, ignored local overrides. Never commit SIM/broker credentials.
#if __has_include("LtePrivate.h")
#include "LtePrivate.h"
#endif
#ifndef LTE_APN
#define LTE_APN ""
#endif
#ifndef LTE_APN_USER
#define LTE_APN_USER ""
#endif
#ifndef LTE_APN_PASSWORD
#define LTE_APN_PASSWORD ""
#endif
#ifndef LTE_BROKER_HOST
#define LTE_BROKER_HOST ""
#endif
#ifndef LTE_DEVICE_ID
#define LTE_DEVICE_ID "s3-controller"
#endif
#ifndef LTE_MQTT_USER
#define LTE_MQTT_USER ""
#endif
#ifndef LTE_MQTT_PASSWORD
#define LTE_MQTT_PASSWORD ""
#endif
#ifndef LTE_CA_NAME
#define LTE_CA_NAME "relay-ca"
#endif
#ifndef LTE_UART_BAUD
#define LTE_UART_BAUD 115200
#endif
// Wi-Fi uses the same broker credentials, but needs its own public CA copy.
// Field setup embeds it automatically; never put the CA private key here.
#ifndef LTE_WIFI_FALLBACK
#define LTE_WIFI_FALLBACK APP_WIFI_ENABLED
#endif
#ifndef LTE_WIFI_CA_PEM
#define LTE_WIFI_CA_PEM ""
#endif
namespace LteConfig {
constexpr bool WIFI_FALLBACK = LTE_WIFI_FALLBACK && APP_WIFI_ENABLED;
constexpr const char *WIFI_CA_PEM = LTE_WIFI_CA_PEM;
constexpr int RX_PIN = 17; // ESP32 RX <- shield D8 in SOFT switch position.
constexpr int TX_PIN = 18; // ESP32 TX -> shield D9 in SOFT switch position.
constexpr int UART_PORT = 2;
constexpr uint32_t UART_BAUD = LTE_UART_BAUD;
constexpr uint16_t BROKER_PORT = 8883;
constexpr size_t SNAPSHOT_BYTES = 8192;
constexpr size_t MESSAGE_BYTES = 12288;
constexpr uint32_t COMMAND_TTL_MS = 5000;
constexpr uint32_t ACTIVE_STATE_MS = 1000;
constexpr uint32_t IDLE_STATE_MS = 15000;
constexpr uint32_t TASK_STACK_BYTES = 12288;
constexpr const char *APN = LTE_APN;
constexpr const char *APN_USER = LTE_APN_USER;
constexpr const char *APN_PASSWORD = LTE_APN_PASSWORD;
constexpr const char *HOST = LTE_BROKER_HOST;
constexpr const char *DEVICE_ID = LTE_DEVICE_ID;
constexpr const char *MQTT_USER = LTE_MQTT_USER;
constexpr const char *MQTT_PASSWORD = LTE_MQTT_PASSWORD;
constexpr const char *CA_NAME = LTE_CA_NAME;
static_assert(!APP_LTE_ENABLED ||
  (sizeof(LTE_APN) > 1 && sizeof(LTE_BROKER_HOST) > 1 &&
   sizeof(LTE_MQTT_USER) > 1 && sizeof(LTE_MQTT_PASSWORD) > 1),
  "LTE needs APN, broker hostname, device username and password in LtePrivate.h.");
static_assert(!APP_LTE_ENABLED ||
  (AppConfig::isAvailableAddonPin(RX_PIN) && AppConfig::isAvailableAddonPin(TX_PIN) && RX_PIN != TX_PIN),
  "LTE UART pins must be distinct available S3 GPIOs.");
static_assert(!APP_LTE_ENABLED || !AppConfig::ENABLE_AUX_UART ||
  (AppConfig::AUX_UART_PORT != UART_PORT && RX_PIN != AppConfig::AUX_UART_RX_PIN &&
   RX_PIN != AppConfig::AUX_UART_TX_PIN && TX_PIN != AppConfig::AUX_UART_RX_PIN && TX_PIN != AppConfig::AUX_UART_TX_PIN),
  "LTE and auxiliary UART ownership cannot overlap.");
}
#endif  // ESP32S3_LTE_CONFIG_H
