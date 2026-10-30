#pragma once
#include "Arduino.h"
struct RgbWrite { int pin; uint8_t red, green, blue; };
inline std::vector<RgbWrite> testRgbWrites;
inline void rgbLedWrite(uint8_t pin, uint8_t red, uint8_t green, uint8_t blue) {
  testRgbWrites.push_back({pin, red, green, blue});
}
