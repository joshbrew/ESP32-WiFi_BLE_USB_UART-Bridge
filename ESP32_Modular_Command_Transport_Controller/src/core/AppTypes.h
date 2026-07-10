#ifndef ESP32_MODULAR_CONTROLLER_APPTYPES_H
#define ESP32_MODULAR_CONTROLLER_APPTYPES_H

#include <Arduino.h>

// GO HERE when adding a transport-visible source or a new event severity. Update
// both name helpers so logs and JSON remain stable across every interface.
enum class CommandSource : uint8_t {
  USB,
  SPP,
  BLE,
  WIFI,
  UART,
  INTERNAL
};

inline const char *commandSourceName(CommandSource source) {
  switch (source) {
    case CommandSource::USB: return "USB";
    case CommandSource::SPP: return "SPP";
    case CommandSource::BLE: return "BLE";
    case CommandSource::WIFI: return "WiFi";
    case CommandSource::UART: return "UART";
    case CommandSource::INTERNAL: return "Internal";
  }
  return "Unknown";
}

using TransportMask = uint8_t;

constexpr TransportMask TRANSPORT_MASK_NONE = 0;
constexpr TransportMask TRANSPORT_MASK_USB = 1U << 0;
constexpr TransportMask TRANSPORT_MASK_SPP = 1U << 1;
constexpr TransportMask TRANSPORT_MASK_BLE = 1U << 2;
constexpr TransportMask TRANSPORT_MASK_WIFI = 1U << 3;
constexpr TransportMask TRANSPORT_MASK_UART = 1U << 4;
constexpr TransportMask TRANSPORT_MASK_DEFAULT = 1U << 7;
constexpr TransportMask TRANSPORT_MASK_ALL =
  TRANSPORT_MASK_USB |
  TRANSPORT_MASK_SPP |
  TRANSPORT_MASK_BLE |
  TRANSPORT_MASK_WIFI |
  TRANSPORT_MASK_UART;


inline TransportMask defaultTransportMaskForEvent(CommandSource source) {
  // Automatic command/debug traffic returns to its source and to the wired
  // recovery outputs. It does not silently cross between Wi-Fi and BLE; use
  // SendWiFi/SendBLE for deliberate cross-transport payloads.
  constexpr TransportMask recovery = TRANSPORT_MASK_USB | TRANSPORT_MASK_UART;
  switch (source) {
    case CommandSource::WIFI: return recovery | TRANSPORT_MASK_WIFI;
    case CommandSource::BLE: return recovery | TRANSPORT_MASK_BLE;
    case CommandSource::SPP: return recovery | TRANSPORT_MASK_SPP;
    case CommandSource::USB: return recovery;
    case CommandSource::UART: return recovery;
    case CommandSource::INTERNAL: return recovery | TRANSPORT_MASK_WIFI;
  }
  return recovery;
}

inline TransportMask transportMaskForSource(CommandSource source) {
  switch (source) {
    case CommandSource::USB: return TRANSPORT_MASK_USB;
    case CommandSource::SPP: return TRANSPORT_MASK_SPP;
    case CommandSource::BLE: return TRANSPORT_MASK_BLE;
    case CommandSource::WIFI: return TRANSPORT_MASK_WIFI;
    case CommandSource::UART: return TRANSPORT_MASK_UART;
    case CommandSource::INTERNAL: return TRANSPORT_MASK_NONE;
  }
  return TRANSPORT_MASK_NONE;
}

inline bool isExternalCommandSource(CommandSource source) {
  return transportMaskForSource(source) != TRANSPORT_MASK_NONE;
}

enum class EventLevel : uint8_t {
  INFO,
  STATUS,
  MOTION,
  WARNING,
  ERROR
};

inline const char *eventLevelName(EventLevel level) {
  switch (level) {
    case EventLevel::INFO: return "info";
    case EventLevel::STATUS: return "status";
    case EventLevel::MOTION: return "motion";
    case EventLevel::WARNING: return "warning";
    case EventLevel::ERROR: return "error";
  }
  return "info";
}

using CommandSubmitter = bool (*)(
  void *context,
  CommandSource source,
  const String &line,
  const String &requestId
);

using BatchCommandSubmitter = bool (*)(
  void *context,
  CommandSource source,
  const String &body,
  const String &requestId,
  uint16_t &acceptedLines
);

using StateProvider = String (*)(void *context);

#endif  // ESP32_MODULAR_CONTROLLER_APPTYPES_H
