#include "DriverWiring.h"
#include "../util/TextUtil.h"
#include <Preferences.h>
#include <cstring>
#include <stddef.h>

namespace {
constexpr uint32_t MAGIC = 0x44525731U;
bool available(int pin) {
#if DRONE_CFG_BOARD_ESP32_S3
  const bool led = AppConfig::isStatusIndicatorPin(pin);
#else
  const bool led = AppConfig::ENABLE_STATUS_INDICATORS &&
    (pin == AppConfig::PIN_STATUS_CONNECTION || pin == AppConfig::PIN_STATUS_ACTIVITY);
#endif
  if (!AppConfig::isConservativePayloadOutputPin(pin) || led ||
      pin == AppConfig::PIN_DISPENSER || pin == AppConfig::PIN_DISPENSER_INTERLOCK ||
      (AppConfig::ENABLE_AUX_UART && (pin == AppConfig::AUX_UART_RX_PIN || pin == AppConfig::AUX_UART_TX_PIN))) return false;
#if APP_LTE_ENABLED
  if (pin == LteConfig::RX_PIN || pin == LteConfig::TX_PIN) return false;
#endif
  return true;
}
}

void DriverWiring::defaults() {
  memset(this, 0, sizeof(*this)); magic = MAGIC; version = 1;
  dcCount = static_cast<uint8_t>(AppConfig::DRV8871_MOTOR_COUNT);
  stepCount = static_cast<uint8_t>(AppConfig::TMC2209_MOTOR_COUNT);
  tx = AppConfig::TMC2209_TX_PIN; rx = AppConfig::TMC2209_RX_PIN;
  for (size_t i = 0; i < dcCount; ++i) dc[i] = {uint8_t(AppConfig::DRV8871_MOTORS[i].in1), uint8_t(AppConfig::DRV8871_MOTORS[i].in2)};
  for (size_t i = 0; i < stepCount; ++i) {
    const auto &pins = AppConfig::TMC2209_MOTORS[i];
    step[i] = {uint8_t(pins.step), uint8_t(pins.dir), uint8_t(pins.enable), pins.address};
  }
  checksum = hash();
}

uint32_t DriverWiring::hash() const {
  uint32_t value = 2166136261UL;
  const auto *bytes = reinterpret_cast<const uint8_t *>(this);
  for (size_t i = 0; i < offsetof(DriverWiring, checksum); ++i) { value ^= bytes[i]; value *= 16777619UL; }
  return value;
}

bool DriverWiring::valid(String &reason) const {
  if (magic != MAGIC || version != 1 || dcCount > 4 || stepCount > 4) {
    reason = "invalid driver wiring record"; return false;
  }
  bool used[49]{}; bool address[4]{};
  auto take = [&](int pin) {
    if (pin < 0 || pin >= 49 || !available(pin) || used[pin]) return false;
    used[pin] = true; return true;
  };
#if DRONE_CFG_ENABLE_DRV8871
  for (size_t i = 0; i < dcCount; ++i)
    if (!take(dc[i].in1) || !take(dc[i].in2)) { reason = "DRV8871 pins overlap or are reserved/unavailable"; return false; }
#endif
#if DRONE_CFG_ENABLE_TMC2209
  if (stepCount && (!take(tx) || !take(rx))) { reason = "TMC UART pins overlap or are reserved/unavailable"; return false; }
  for (size_t i = 0; i < stepCount; ++i) {
    const auto &pins = step[i];
    if (!take(pins.step) || !take(pins.dir) || !take(pins.enable) || pins.address > 3 || address[pins.address]) {
      reason = "TMC pins must be free/unique and UART addresses must be unique 0..3"; return false;
    }
    address[pins.address] = true;
  }
#endif
  reason = "driver pins validated"; return true;
}

void DriverWiring::load() {
  defaults();
  Preferences prefs; DriverWiring stored{}; String reason;
  if (!prefs.begin("motor-wiring", true)) return;
  const bool loaded = prefs.getBytesLength("pins") == sizeof(stored) &&
    prefs.getBytes("pins", &stored, sizeof(stored)) == sizeof(stored);
  prefs.end();
  if (loaded && stored.checksum == stored.hash() && stored.valid(reason)) *this = stored;
}

bool DriverWiring::save() const {
  String reason; if (!valid(reason)) return false;
  DriverWiring stored = *this; stored.checksum = stored.hash();
  Preferences prefs;
  if (!prefs.begin("motor-wiring", false)) return false;
  const bool written = prefs.putBytes("pins", &stored, sizeof(stored)) == sizeof(stored);
  DriverWiring readback{};
  const bool verified = written && prefs.getBytesLength("pins") == sizeof(stored) &&
    prefs.getBytes("pins", &readback, sizeof(readback)) == sizeof(readback) &&
    memcmp(&stored, &readback, sizeof(stored)) == 0;
  prefs.end(); return verified;
}

bool DriverWiring::owns(int pin) const {
#if DRONE_CFG_ENABLE_DRV8871
  for (size_t i = 0; i < dcCount; ++i) if (pin == dc[i].in1 || pin == dc[i].in2) return true;
#endif
#if DRONE_CFG_ENABLE_TMC2209
  if (stepCount && (pin == tx || pin == rx)) return true;
  for (size_t i = 0; i < stepCount; ++i)
    if (pin == step[i].step || pin == step[i].dir || pin == step[i].enable) return true;
#endif
  return false;
}

bool DriverWiring::update(const String &command, String &reason) {
  const int colon = command.indexOf(':');
  if (colon < 0) { reason = "driver pin command needs arguments"; return false; }
  const String verb = command.substring(0, colon); String fields[5]; size_t count = 0; int offset = colon + 1;
  while (count < 5) {
    const int comma = command.indexOf(',', offset);
    fields[count++] = comma < 0 ? command.substring(offset) : command.substring(offset, comma);
    if (comma < 0) break;
    offset = comma + 1;
    if (count == 5) { reason = "too many driver pin arguments"; return false; }
  }
  if (verb.equalsIgnoreCase("TMCUART")) {
#if DRONE_CFG_ENABLE_TMC2209
    long a = -1, b = -1;
    if (count != 2 || !TextUtil::parseLong(fields[0], a) || !TextUtil::parseLong(fields[1], b) || a < 0 || a > 48 || b < 0 || b > 48) {
      reason = "TMCUART needs ESP32 TX pin,RX pin"; return false;
    }
    tx = static_cast<uint8_t>(a); rx = static_cast<uint8_t>(b);
#else
    reason = "TMC2209 is not compiled in"; return false;
#endif
  } else {
    const bool dcDriver = verb.equalsIgnoreCase("MotorPins"), stepDriver = verb.equalsIgnoreCase("TMCPins");
    if ((!dcDriver && !stepDriver) || (dcDriver && !DRONE_CFG_ENABLE_DRV8871) || (stepDriver && !DRONE_CFG_ENABLE_TMC2209)) {
      reason = "selected motor driver is not compiled in"; return false;
    }
    uint32_t number = 0;
    uint8_t &used = dcDriver ? dcCount : stepCount;
    if (!TextUtil::parseUnsigned32(fields[0], number) || !number || number > 4 || number > uint32_t(used) + 1) {
      reason = "use an existing motor number or the next consecutive number (maximum 4)"; return false;
    }
    if (count == 2 && fields[1].equalsIgnoreCase("OFF")) {
      if (number != used || !used) { reason = "only the last motor can be removed"; return false; }
      --used;
    } else {
      const size_t required = dcDriver ? 3 : 5;
      if (count != required) { reason = dcDriver ? "MotorPins needs motor,IN1,IN2" : "TMCPins needs motor,STEP,DIR,EN,address"; return false; }
      uint8_t values[4]{};
      for (size_t i = 1; i < count; ++i) {
        uint32_t value = 0;
        if (!TextUtil::parseUnsigned32(fields[i], value) || value > (stepDriver && i == 4 ? 3U : 48U)) { reason = "invalid GPIO or UART address"; return false; }
        values[i - 1] = static_cast<uint8_t>(value);
      }
      if (dcDriver) dc[number - 1] = {values[0], values[1]};
      else step[number - 1] = {values[0], values[1], values[2], values[3]};
      if (number > used) used = static_cast<uint8_t>(number);
    }
  }
  return valid(reason);
}

String DriverWiring::stateJson() const {
  String json = "{\"tx\":" + String(tx) + ",\"rx\":" + String(rx) + ",\"drv8871\":[";
  for (size_t i = 0; i < dcCount; ++i) { if (i) json += ','; json += "[" + String(i + 1) + "," + String(dc[i].in1) + "," + String(dc[i].in2) + "]"; }
  json += "],\"tmc2209\":[";
  for (size_t i = 0; i < stepCount; ++i) {
    if (i) json += ','; const auto &pins = step[i];
    json += "[" + String(i + 1) + "," + String(pins.step) + "," + String(pins.dir) + "," + String(pins.enable) + "," + String(pins.address) + "]";
  }
  return json + "]}";
}
