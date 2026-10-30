#include "TestPlatform.h"
#include "../../src/hardware/StatusIndicators.h"
#include "../../src/addons/dispenser/DispenserAddon.h"
#include "esp32-hal-rgb-led.h"
#include "Preferences.h"
#include <cassert>
#include <iostream>

static void command(DispenserAddon &addon, const String &line) {
  assert(addon.handleCommand(line, CommandSource::USB, "board-test"));
}
static std::string state(DispenserAddon &addon) {
  String json; addon.appendStateJson(json, false); return json.value;
}

int main() {
#if DRONE_CFG_BOARD_ESP32_S3
  static_assert(AppConfig::PIN_DISPENSER == 4);
  static_assert(AppConfig::STATUS_USE_RGB);
  static_assert(AppConfig::PIN_STATUS_CONNECTION == -1 && AppConfig::PIN_STATUS_ACTIVITY == -1);
  for (int pin : {0,3,19,20,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,43,44,45,46,48,49})
    assert(!AppConfig::isConservativePayloadOutputPin(pin));
  for (int pin : {1,2,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,21,39,40,41,42,47})
    assert(AppConfig::isConservativePayloadOutputPin(pin));

  // Reject a saved LOLIN32 pin before any output setup, even across a firmware migration.
  Preferences saved; saved.begin("drone-disp"); saved.putBool("valid",true); saved.putInt("pin",26); saved.end();
  EventBus events; DispenserAddon addon(events); addon.begin();
  assert(testLevels[4] == LOW && testModes[4] == OUTPUT);
  assert(state(addon).find("\"pins\":[4]") != std::string::npos);
  assert(testModes.find(26) == testModes.end());
  for (int bad : {0,3,19,20,23,26,35,36,37,38,43,44,45,46,48}) {
    command(addon, "DispenserPin:" + String(bad));
    command(addon, "DispenserOutputs:4," + String(bad));
    assert(state(addon).find("\"pins\":[4]") != std::string::npos);
    assert(testModes.find(bad) == testModes.end());
  }
  command(addon, "DispenserPin:47");
  assert(state(addon).find("\"pins\":[47]") != std::string::npos);
  command(addon, "DispenserOutputs:4,40,47"); command(addon, "DispenserSave");
  DispenserAddon reboot(events); reboot.begin();
  assert(state(reboot).find("\"pins\":[4,40,47]") != std::string::npos);
  command(reboot, "Arm"); command(reboot, "DispensePin:47,10");
  assert(testLevels[47] == HIGH && testLevels[4] == LOW && testLevels[40] == LOW);
  testNow += 10; reboot.service(); assert(testLevels[47] == LOW);
  command(reboot, "GPIO26:ON"); // Compatibility command operates selected output, never GPIO26.
  assert(testLevels[4] == HIGH && testModes.find(26) == testModes.end());
  command(reboot, "GPIO26:OFF"); assert(testLevels[4] == LOW);
  testLevels.clear(); testModes.clear(); testWrites.clear();
#else
  static_assert(AppConfig::PIN_DISPENSER == 26 && !AppConfig::STATUS_USE_RGB);
  static_assert(AppConfig::PIN_STATUS_CONNECTION == 23 && AppConfig::PIN_STATUS_ACTIVITY == 5);
#endif

  StatusIndicators led; led.begin(false, true);
#if DRONE_CFG_BOARD_ESP32_S3
  auto expectRgb = [](int red, int green, int blue) {
    assert(!testRgbWrites.empty()); auto c = testRgbWrites.back();
    assert(c.pin == AppConfig::PIN_STATUS_RGB && c.red == red && c.green == green && c.blue == blue);
    assert(testModes.empty() && testWrites.empty()); // No discrete GPIO writes, including -1.
  };
  const int b = AppConfig::STATUS_RGB_BRIGHTNESS;
  expectRgb(0,0,0);
  led.service(true,false,false); expectRgb(0,b,0);
  size_t writes = testRgbWrites.size(); led.service(true,false,false); assert(testRgbWrites.size() == writes);
  led.noteCommandReceived(); led.service(true,false,false); expectRgb(0,0,b);
  led.service(true,false,true); expectRgb(b,b/2,0);
  testNow += AppConfig::ACTIVITY_LED_PULSE_MS; led.service(true,false,false); expectRgb(0,b,0);
  led.service(false,true,false); expectRgb(0,b,0);
  testNow += AppConfig::CONNECTION_LED_BLINK_MS; led.service(false,true,false); expectRgb(0,0,0);
  assert(led.stateJson().value.find("\"rgb\":true") != std::string::npos);
  assert(led.startSelfTest()); assert(!led.startActivityTest());
  for (int i = 0; i < 20 && led.isSelfTestActive(); ++i) {
    testNow += 250; led.service(false,false,false);
  }
  assert(!led.isSelfTestActive() && led.consumeSelfTestCompleted()); expectRgb(0,0,0);
  led.begin(false); expectRgb(0,0,0); // Production boot test finishes dark.
#else
  assert(testRgbWrites.empty());
  led.service(true,false,false); assert(testLevels[23] == HIGH && testLevels[5] == LOW);
  led.noteCommandReceived(); led.service(true,false,false); assert(testLevels[5] == HIGH);
  testNow += AppConfig::ACTIVITY_LED_PULSE_MS; led.service(false,false,false);
  assert(testLevels[23] == LOW && testLevels[5] == LOW);
#endif
  std::cout << AppConfig::BOARD_NAME << " pin and indicator checks passed\n";
}
