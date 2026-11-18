#include "Drv8871Motor.h"
#include "../util/TextUtil.h"

void Drv8871Motor::begin(const DriverWiring &wiring) {
#if DRONE_CFG_ENABLE_DRV8871
  count_ = wiring.dcCount;
  for (size_t i = 0; i < count_; ++i) {
    channels_[i].pins = wiring.dc[i];
    digitalWrite(wiring.dc[i].in1, LOW); digitalWrite(wiring.dc[i].in2, LOW);
    pinMode(wiring.dc[i].in1, OUTPUT); pinMode(wiring.dc[i].in2, OUTPUT);
  }
  stop();
#else
  (void)wiring;
#endif
}

#if DRONE_CFG_ENABLE_DRV8871
void Drv8871Motor::idle(size_t index, bool brake) {
  auto &channel = channels_[index];
  if (channel.mode == (brake ? Mode::Brake : Mode::Coast)) return;
  if (channel.mode == Mode::Drive) { ledcDetach(channel.pins.in2); pinMode(channel.pins.in2, OUTPUT); }
  digitalWrite(channel.pins.in2, brake ? HIGH : LOW);
  digitalWrite(channel.pins.in1, brake ? HIGH : LOW);
  channel.mode = brake ? Mode::Brake : Mode::Coast;
  channel.duration = 0;
}
bool Drv8871Motor::drive(size_t index, uint8_t level) {
  auto &channel = channels_[index];
  if (channel.mode != Mode::Drive) {
    digitalWrite(channel.pins.in1, HIGH);
    if (!ledcAttach(channel.pins.in2, 20000, 8)) { idle(index, false); return false; }
    channel.mode = Mode::Drive;
  }
  if (ledcWrite(channel.pins.in2, 255 - level)) return true;
  idle(index, false); return false;
}
#endif

void Drv8871Motor::stop() {
#if DRONE_CFG_ENABLE_DRV8871
  for (size_t i = 0; i < count_; ++i) idle(i, false);
#endif
}
bool Drv8871Motor::busy() const {
#if DRONE_CFG_ENABLE_DRV8871
  for (size_t i = 0; i < count_; ++i) if (channels_[i].mode == Mode::Drive) return true;
#endif
  return false;
}
void Drv8871Motor::service() {
#if DRONE_CFG_ENABLE_DRV8871
  const uint32_t now = millis();
  for (size_t i = 0; i < count_; ++i) {
    const auto &channel = channels_[i];
    if (channel.mode == Mode::Drive && uint32_t(now - channel.started) >= channel.duration) idle(i, false);
  }
#endif
}
bool Drv8871Motor::validatePulse(const String &line, uint32_t limit, String &reason) const {
#if DRONE_CFG_ENABLE_DRV8871
  uint32_t duration = 0, level = 0, motor = 0;
  if (TextUtil::parseMotorPulse(line, duration, level, &motor) && motor <= count_ && duration <= limit) return true;
  reason = "motor pulse needs a configured driver, level 1..255 and a duration within the active limit";
#else
  (void)line; (void)limit; reason = "DRV8871 is not enabled in this build";
#endif
  return false;
}
bool Drv8871Motor::command(const String &line, bool armed, bool ready, bool gpioBusy,
  uint32_t limit, String &message, bool &success) {
  if (!TextUtil::startsWithIgnoreCase(line, "Motor")) return false;
  success = false;
#if !DRONE_CFG_ENABLE_DRV8871
  (void)armed; (void)ready; (void)gpioBusy; (void)limit;
  message = "DRV8871 is not enabled; set DRONE_CFG_ENABLE_DRV8871 to 1 and rebuild/upload";
#else
  const int colon = line.indexOf(':'); const String verb = colon < 0 ? line : line.substring(0, colon);
  const bool brake = verb.equalsIgnoreCase("MotorStop"), coast = verb.equalsIgnoreCase("MotorCoast");
  if (brake || coast) {
    uint32_t motor = 0;
    if (colon >= 0 && (!TextUtil::parseUnsigned32(line.substring(colon + 1), motor) || !motor || motor > count_)) { message = "invalid motor number"; return true; }
    for (size_t i = 0; i < count_; ++i) if (!motor || motor == i + 1) idle(i, brake && armed && ready);
    message = brake && armed && ready ? "DRV8871 brake applied" : "DRV8871 coasting"; success = true; return true;
  }
  if (line.equalsIgnoreCase("MotorStatus")) { message = stateJson(limit); success = true; return true; }
  const bool pulse = verb.equalsIgnoreCase("MotorPulse"), start = verb.equalsIgnoreCase("MotorStart"), setLevel = verb.equalsIgnoreCase("MotorLevel");
  if (!(pulse || start || setLevel)) { message = "unknown Motor command"; return true; }
  uint32_t motor = 1, level = channels_[0].level, duration = limit;
  if (pulse) {
    if (!TextUtil::parseMotorPulse(line, duration, level, &motor)) { message = "MotorPulse needs ms,level[,motor]"; return true; }
  } else if (colon >= 0) {
    const int comma = line.indexOf(',', colon + 1);
    if (!TextUtil::parseUnsigned32(comma < 0 ? line.substring(colon + 1) : line.substring(colon + 1, comma), level) ||
        (comma >= 0 && !TextUtil::parseUnsigned32(line.substring(comma + 1), motor))) { message = "MotorStart/MotorLevel needs level[,motor]"; return true; }
  } else if (setLevel) { message = "MotorLevel needs level[,motor]"; return true; }
  if (!motor || motor > count_ || level > 255) { message = "motor or drive level invalid"; return true; }
  const size_t index = motor - 1; auto &channel = channels_[index];
  if (setLevel && !level) { idle(index, false); channel.level = 0; message = "DRV8871 selected motor stopped"; success = true; return true; }
  if (setLevel && channel.mode != Mode::Drive) {
    channel.level = static_cast<uint8_t>(level); message = "DRV8871 idle level saved for this boot"; success = true; return true;
  }
  if (!armed || !ready || gpioBusy) { message = "DRV8871 requires Arm, an open interlock and an inactive GPIO output"; return true; }
  if (setLevel) {
    if (channel.mode == Mode::Drive && !drive(index, static_cast<uint8_t>(level))) { message = "DRV8871 PWM update failed"; return true; }
    channel.level = static_cast<uint8_t>(level); message = "DRV8871 level updated"; success = true; return true;
  }
  if (!level || !duration || duration > limit || channel.mode == Mode::Drive) { message = "motor already running, zero level, or runtime beyond active limit"; return true; }
  if (!drive(index, static_cast<uint8_t>(level))) { message = "DRV8871 PWM setup failed"; return true; }
  channel.level = static_cast<uint8_t>(level); channel.started = millis(); channel.duration = duration; active_ = static_cast<uint8_t>(index);
  message = "DRV8871 motor=" + String(motor) + " level=" + String(level) + " durationMs=" + String(duration); success = true;
#endif
  return true;
}
String Drv8871Motor::stateJson(uint32_t limit) const {
#if DRONE_CFG_ENABLE_DRV8871
  const uint32_t now = millis();
  const auto &active = channels_[active_];
  const char *mode = busy() ? "drive" : active.mode == Mode::Brake ? "brake" : "coast";
  String json = "{\"mode\":\"" + String(mode) + "\",\"level\":" + String(active.level) +
    ",\"maxRunMs\":" + String(limit) + ",\"activeMotor\":" + String(active_ + 1) +
    ",\"in1\":" + String(active.pins.in1) + ",\"in2\":" + String(active.pins.in2) + ",\"channels\":[";
  for (size_t i = 0; i < count_; ++i) {
    if (i) json += ','; const auto &channel = channels_[i];
    const char *state = channel.mode == Mode::Drive ? "drive" : channel.mode == Mode::Brake ? "brake" : "coast";
    const uint32_t elapsed = now - channel.started;
    json += "[" + String(i + 1) + "," + String(channel.pins.in1) + "," + String(channel.pins.in2) + "," + String(channel.level) + ",\"" + state + "\"," +
      String(channel.mode == Mode::Drive && elapsed < channel.duration ? channel.duration - elapsed : 0) + "]";
  }
  return json + "]}";
#else
  (void)limit; return "null";
#endif
}

uint32_t Drv8871Motor::remainingMs(uint32_t now) const {
  uint32_t remaining = 0;
#if DRONE_CFG_ENABLE_DRV8871
  for (size_t i = 0; i < count_; ++i) {
    const auto &channel = channels_[i]; const uint32_t elapsed = now - channel.started;
    if (channel.mode == Mode::Drive && elapsed < channel.duration && channel.duration - elapsed > remaining)
      remaining = channel.duration - elapsed;
  }
#else
  (void)now;
#endif
  return remaining;
}
