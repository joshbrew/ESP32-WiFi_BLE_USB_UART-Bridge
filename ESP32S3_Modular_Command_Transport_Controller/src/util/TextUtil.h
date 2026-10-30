#ifndef ESP32_MODULAR_CONTROLLER_TEXTUTIL_H
#define ESP32_MODULAR_CONTROLLER_TEXTUTIL_H

#include <Arduino.h>

// GO HERE for shared parsing, JSON escaping, and secret redaction helpers. Any
// new credential-bearing command prefix must be added to redactedCommand().
namespace TextUtil {

bool startsWithIgnoreCase(const String &value, const char *prefix);
bool parseOnOff(String value, bool &result);
bool parseLong(String value, long &result);
bool parseUnsigned32(String value, uint32_t &result);
bool parseFloat(String value, float &result);
String boolWord(bool value);
String jsonBool(bool value);
String jsonEscape(const String &value);
String redactedCommand(const String &command);

}  // namespace TextUtil

#endif  // ESP32_MODULAR_CONTROLLER_TEXTUTIL_H
