#pragma once
#include <Arduino.h>
#define SERIAL_8N1 0
class HardwareSerial {
 public:
  explicit HardwareSerial(int) {}
  void begin(uint32_t, int, int, int) {}
};
