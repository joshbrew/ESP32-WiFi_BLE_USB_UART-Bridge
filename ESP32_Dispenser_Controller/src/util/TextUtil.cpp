#include "TextUtil.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>

namespace TextUtil {

bool startsWithIgnoreCase(const String &value, const char *prefix) {
  const size_t length = strlen(prefix);
  if (length > static_cast<size_t>(UINT_MAX) || value.length() < length) {
    return false;
  }
  return value.substring(0U, static_cast<unsigned int>(length)).equalsIgnoreCase(prefix);
}

bool parseOnOff(String value, bool &result) {
  value.trim();
  if (value.equalsIgnoreCase("ON")) {
    result = true;
    return true;
  }
  if (value.equalsIgnoreCase("OFF")) {
    result = false;
    return true;
  }
  return false;
}

bool parseLong(String value, long &result) {
  value.trim();
  if (value.length() == 0) {
    return false;
  }

  size_t index = 0;
  bool negative = false;
  if (value[0] == '+' || value[0] == '-') {
    negative = value[0] == '-';
    index = 1;
  }
  if (index >= value.length()) {
    return false;
  }

  const uint64_t limit = negative
    ? static_cast<uint64_t>(LONG_MAX) + 1ULL
    : static_cast<uint64_t>(LONG_MAX);
  uint64_t magnitude = 0;
  for (; index < value.length(); index++) {
    const char c = value[index];
    if (c < '0' || c > '9') {
      return false;
    }
    const uint8_t digit = static_cast<uint8_t>(c - '0');
    if (magnitude > (limit - digit) / 10ULL) {
      return false;
    }
    magnitude = magnitude * 10ULL + digit;
  }

  if (negative) {
    result = magnitude == static_cast<uint64_t>(LONG_MAX) + 1ULL
      ? LONG_MIN
      : -static_cast<long>(magnitude);
  } else {
    result = static_cast<long>(magnitude);
  }
  return true;
}

bool parseUnsigned32(String value, uint32_t &result) {
  value.trim();
  if (value.length() == 0) {
    return false;
  }

  uint64_t parsed = 0;
  for (size_t index = 0; index < value.length(); index++) {
    const char c = value[index];
    if (c < '0' || c > '9') {
      return false;
    }
    const uint8_t digit = static_cast<uint8_t>(c - '0');
    if (parsed > (UINT32_MAX - digit) / 10ULL) {
      return false;
    }
    parsed = parsed * 10ULL + digit;
  }

  result = static_cast<uint32_t>(parsed);
  return true;
}

bool parseFloat(String value, float &result) {
  value.trim();
  if (value.length() == 0) {
    return false;
  }

  char *end = nullptr;
  const float parsed = strtof(value.c_str(), &end);
  if (end == value.c_str() || end == nullptr || *end != '\0' || !isfinite(parsed)) {
    return false;
  }
  result = parsed;
  return true;
}

bool parseMotorPulse(const String &command, uint32_t &duration, uint32_t &level, uint32_t *motor) {
  if (!startsWithIgnoreCase(command, "MotorPulse:")) return false;
  const int comma = command.indexOf(',', 11);
  if (comma < 0 || !parseUnsigned32(command.substring(11, comma), duration) || !duration) return false;
  const int motorSeparator = command.indexOf(',', comma + 1);
  const String levelText = motorSeparator < 0 ? command.substring(comma + 1) :
    command.substring(comma + 1, motorSeparator);
  if (!parseUnsigned32(levelText, level) || level < 1 || level > 255) return false;
  uint32_t number = 1;
  if (motorSeparator >= 0 &&
      (!parseUnsigned32(command.substring(motorSeparator + 1), number) || !number)) return false;
  if (motor) *motor = number;
  return true;
}

bool parseTmcPulse(const String &command, uint32_t &duration, uint32_t &rpm, long &direction, uint32_t *motor) {
  if (!startsWithIgnoreCase(command, "TMCPulse:")) return false;
  const String args = command.substring(9);
  const int a = args.indexOf(','), b = args.indexOf(',', a + 1), c = args.indexOf(',', b + 1);
  uint32_t number = 1;
  if (a < 0 || b < 0 || !parseUnsigned32(args.substring(0, a), duration) || !duration ||
      !parseUnsigned32(args.substring(a + 1, b), rpm) || !rpm ||
      !parseLong(c < 0 ? args.substring(b + 1) : args.substring(b + 1, c), direction) ||
      (direction != 1 && direction != -1) ||
      (c >= 0 && (!parseUnsigned32(args.substring(c + 1), number) || !number))) return false;
  if (motor) *motor = number;
  return true;
}

bool parseDispense(const String &command, int &pin, uint32_t &duration) {
  pin = -1;
  if (startsWithIgnoreCase(command, "Dispense:"))
    return parseUnsigned32(command.substring(9), duration) && duration != 0;
  if (!startsWithIgnoreCase(command, "DispensePin:")) return false;
  const int comma = command.indexOf(',', 12);
  long parsed = 0;
  if (comma < 0 || !parseLong(command.substring(12, comma), parsed) || parsed < 0 || parsed > 39 ||
      !parseUnsigned32(command.substring(comma + 1), duration) || !duration) return false;
  pin = static_cast<int>(parsed); return true;
}

bool isMotorStop(const String &input) {
  String line = input; line.trim();
  const char *const names[] = {"MotorStop", "MotorCoast", "TMCStop", "TMCDisable"};
  for (const char *name : names) {
    if (line.equalsIgnoreCase(name)) return true;
    const String prefix = String(name) + ":";
    if (startsWithIgnoreCase(line, prefix.c_str())) {
      const String number = line.substring(prefix.length());
      return number.length() == 1 && number[0] >= '1' && number[0] <= '4';
    }
  }
  return false;
}

String boolWord(bool value) {
  return value ? "on" : "off";
}

String jsonBool(bool value) {
  return value ? "true" : "false";
}

String jsonEscape(const String &value) {
  String output;
  output.reserve(value.length() + 16);
  for (size_t index = 0; index < value.length(); index++) {
    const char c = value[index];
    switch (c) {
      case '\\': output += "\\\\"; break;
      case '"': output += "\\\""; break;
      case '\n': output += "\\n"; break;
      case '\r': output += "\\r"; break;
      case '\t': output += "\\t"; break;
      default:
        if (static_cast<uint8_t>(c) < 0x20) {
          output += ' ';
        } else {
          output += c;
        }
        break;
    }
  }
  return output;
}

String redactedCommand(const String &command) {
  static const char *prefixes[] = {
    "WiFiStaPassword:",
    "WiFiApPassword:"
  };

  for (const char *prefix : prefixes) {
    if (startsWithIgnoreCase(command, prefix)) {
      return String(prefix) + "***";
    }
  }
  return command;
}

}  // namespace TextUtil
