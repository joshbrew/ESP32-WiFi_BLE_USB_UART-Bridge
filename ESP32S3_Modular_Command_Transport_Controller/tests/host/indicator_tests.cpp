#include "../../src/hardware/StatusIndicators.h"
#include "esp32-hal-rgb-led.h"
#include <cassert>
#include <iostream>

static void expectColor(int red, int green, int blue) {
  assert(!testRgbWrites.empty());
  const auto color = testRgbWrites.back();
  assert(color.pin == AppConfig::PIN_STATUS_RGB);
  assert(color.red == red && color.green == green && color.blue == blue);
  assert(testModes.empty() && testWrites.empty());
}

int main() {
  static_assert(AppConfig::PIN_STATUS_CONNECTION == -1 && AppConfig::PIN_STATUS_ACTIVITY == -1);
  for (int pin : {0,3,19,20,22,23,24,25,26,27,32,33,34,35,36,37,38,43,44,45,46,48,49})
    assert(!AppConfig::isAvailableAddonPin(pin));
  for (int pin : {1,2,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,21,39,40,41,42,47})
    assert(AppConfig::isAvailableAddonPin(pin));
  StatusIndicators led;
  led.begin(false, true);
#if APP_STATUS_INDICATORS_ENABLED
  const int brightness = AppConfig::STATUS_RGB_BRIGHTNESS;
  expectColor(0,0,0);
  led.service(true,false,false); expectColor(0,brightness,0);
  const auto writes = testRgbWrites.size();
  led.service(true,false,false); assert(testRgbWrites.size() == writes);
  led.noteCommandReceived(); led.service(true,false,false); expectColor(0,0,brightness);
  led.service(true,false,true); expectColor(brightness,brightness/2,0);
  testNow += AppConfig::ACTIVITY_LED_PULSE_MS;
  led.service(true,false,false); expectColor(0,brightness,0);
  led.service(false,true,false); expectColor(0,brightness,0);
  testNow += AppConfig::CONNECTION_LED_BLINK_MS;
  led.service(false,true,false); expectColor(0,0,0);
  assert(led.stateJson().value.find("\"rgb\":true") != std::string::npos);
  assert(led.startSelfTest()); assert(!led.startSelfTest());
  for (int tick = 0; tick < 20 && led.isSelfTestActive(); ++tick) {
    testNow += 250; led.service(false,false,false);
  }
  assert(!led.isSelfTestActive() && led.consumeSelfTestCompleted());
  assert(!led.consumeSelfTestCompleted()); expectColor(0,0,0);
  testNow = UINT32_MAX - 10;
  led.noteCommandReceived(); led.service(false,false,false); expectColor(0,0,brightness);
  testNow += AppConfig::ACTIVITY_LED_PULSE_MS;
  led.service(false,false,false); expectColor(0,0,0);
  led.begin(false); expectColor(0,0,0);
  led.begin(true); expectColor(0,0,0);
#else
  led.noteCommandReceived(); led.service(true,true,true);
  assert(!led.startSelfTest() && !led.isSelfTestActive());
  assert(testRgbWrites.empty() && testModes.empty() && testWrites.empty());
#endif
  std::cout << "PASS S3 pin ownership, RGB priority/cache/timing/self-tests; GPIO"
    << AppConfig::PIN_STATUS_RGB << "\n";
}
