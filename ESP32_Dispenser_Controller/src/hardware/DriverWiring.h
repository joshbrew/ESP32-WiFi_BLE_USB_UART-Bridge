#ifndef CONTROLLER_DRIVER_WIRING_H
#define CONTROLLER_DRIVER_WIRING_H
#include <Arduino.h>
#include "../config/AppConfig.h"

// One bounded, checksummed NVS record; configuration is applied only at boot.
// Motor numbers remain stable, so removal is limited to the last channel.
struct DriverWiring {
  struct DcPins { uint8_t in1, in2; };
  struct StepPins { uint8_t step, dir, enable, address; };
  uint32_t magic;
  uint16_t version;
  uint8_t dcCount, stepCount;
  uint8_t tx, rx;
  uint8_t reserved[2];
  DcPins dc[4];
  StepPins step[4];
  uint32_t checksum;
  void defaults();
  void load();
  bool valid(String &reason) const;
  bool save() const;
  bool owns(int pin) const;
  bool update(const String &command, String &reason);
  String stateJson() const;
  uint32_t hash() const;
};
static_assert(sizeof(DriverWiring) <= 64, "Driver wiring record unexpectedly grew");
#endif
