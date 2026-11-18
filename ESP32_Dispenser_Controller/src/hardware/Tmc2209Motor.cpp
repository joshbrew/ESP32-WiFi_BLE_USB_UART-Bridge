#include "Tmc2209Motor.h"
#include "../util/TextUtil.h"
#include <limits.h>
#include <new>

void Tmc2209Motor::begin(const DriverWiring &wiring) {
#if DRONE_CFG_ENABLE_TMC2209
  if (!initialized_) {
    count_ = wiring.stepCount; tx_ = wiring.tx; rx_ = wiring.rx;
    for (size_t i = 0; i < count_; ++i) pins_[i] = wiring.step[i];
  }
#else
  (void)wiring;
#endif
  begin();
}

void Tmc2209Motor::begin() {
#if DRONE_CFG_ENABLE_TMC2209
  if (!count_) { initialized_ = true; return; }
  if (!initialized_) {
    for (size_t i = 0; i < count_; ++i) {
      const auto &pins = pins_[i];
      digitalWrite(pins.enable, HIGH); pinMode(pins.enable, OUTPUT); digitalWrite(pins.enable, HIGH);
      digitalWrite(pins.step, LOW); pinMode(pins.step, OUTPUT);
      digitalWrite(pins.dir, LOW); pinMode(pins.dir, OUTPUT);
    }
    uart_.begin(115200, SERIAL_8N1, rx_, tx_);
    engine_.init();
    for (size_t i = 0; i < count_; ++i) {
      auto &channel = channels_[i]; const auto &pins = pins_[i];
      channel.driver = new (std::nothrow) TMC2209Stepper(&uart_, AppConfig::TMC2209_RSENSE, pins.address);
      channel.stepper = engine_.stepperConnectToPin(pins.step);
      if (channel.stepper) {
        channel.stepper->setDirectionPin(pins.dir, true, 200);
        channel.stepper->setEnablePin(pins.enable, true);
        channel.stepper->setAutoEnable(false);
      }
    }
    initialized_ = true;
  }
  stop();
  for (size_t i = 0; i < count_; ++i) configure(i);
#endif
}

#if DRONE_CFG_ENABLE_TMC2209
bool Tmc2209Motor::configure(size_t index) {
  auto &channel = channels_[index];
  channel.ready = false;
  digitalWrite(pins_[index].enable, HIGH);
  if (!channel.driver || !channel.stepper) return false;
  auto &driver = *channel.driver;
  // Read chip identity before writing; a disconnected UART must never enable EN.
  if ((driver.IOIN() >> 24) != 0x21U) return false;
  const uint8_t before = driver.IFCNT();
  driver.begin();
  driver.pdn_disable(true); driver.I_scale_analog(false); driver.internal_Rsense(false);
  driver.mstep_reg_select(true); driver.toff(4); driver.blank_time(24);
  driver.rms_current(channel.current, 0.25f); driver.microsteps(channel.microsteps);
  driver.intpol(true); driver.en_spreadCycle(false); driver.pwm_autoscale(true); driver.VACTUAL(0);
  const uint8_t after = driver.IFCNT();
  if (after == before || (driver.GCONF() & 0xC3U) != 0xC0U || driver.microsteps() != channel.microsteps) return false;
  // IHOLD_IRUN is write-only on TMC2209. Re-send that exact shadow value and
  // require one acknowledged UART write, rather than treating its shadow as
  // a hardware readback. Counter CRC is checked even across an 8-bit rollover.
  const uint8_t currentBefore = driver.IFCNT();
  if (driver.CRCerror) return false;
  driver.IHOLD_IRUN(driver.IHOLD_IRUN());
  const uint8_t currentAfter = driver.IFCNT();
  if (driver.CRCerror || static_cast<uint8_t>(currentAfter - currentBefore) != 1U) return false;
  channel.actualCurrent = driver.rms_current();
  if (driver.CRCerror || !channel.actualCurrent || channel.actualCurrent > AppConfig::TMC2209_MAX_CURRENT_MA) return false;
  channel.ready = true;
  return true;
}
#endif

bool Tmc2209Motor::busy() const {
#if DRONE_CFG_ENABLE_TMC2209
  for (size_t i = 0; i < count_; ++i) if (channels_[i].running) return true;
  return false;
#else
  return false;
#endif
}

#if DRONE_CFG_ENABLE_TMC2209
void Tmc2209Motor::stopMotor(size_t index) {
  digitalWrite(pins_[index].enable, HIGH);
  auto &channel = channels_[index];
  if (channel.stepper) {
    channel.stepper->forceStopAndNewPosition(channel.stepper->getCurrentPosition());
    channel.stepper->disableOutputs();
  }
  channel.running = false;
}
#endif
void Tmc2209Motor::stop() {
#if DRONE_CFG_ENABLE_TMC2209
  for (size_t i = 0; i < count_; ++i) stopMotor(i);
#endif
}
void Tmc2209Motor::service() {
#if DRONE_CFG_ENABLE_TMC2209
  const uint32_t now = millis();
  for (size_t i = 0; i < count_; ++i) {
    const auto &channel = channels_[i];
    const uint32_t elapsed = now - channel.started;
    if (channel.running && (elapsed >= channel.duration || (elapsed >= 10 && !channel.stepper->isRunning()))) stopMotor(i);
  }
#endif
}

bool Tmc2209Motor::validatePulse(const String &line, uint32_t runLimit, String &reason) const {
#if DRONE_CFG_ENABLE_TMC2209
  uint32_t duration = 0, rpm = 0, motor = 0; long direction = 0;
  if (!TextUtil::parseTmcPulse(line, duration, rpm, direction, &motor) ||
      duration > runLimit || duration > AppConfig::TMC2209_MAX_RUN_MS ||
      rpm > AppConfig::TMC2209_MAX_RPM || motor > count_) {
    reason = "TMCPulse needs milliseconds,RPM,direction(+1/-1)[,motor] within configured limits";
    return false;
  }
  if (!initialized_ || !channels_[motor - 1].ready) { reason = "TMC2209 UART not verified; disarm and send TMCInit"; return false; }
  return true;
#else
  (void)line; (void)runLimit; reason = "enable DRONE_CFG_ENABLE_TMC2209 in QuickConfig.h"; return false;
#endif
}

bool Tmc2209Motor::command(const String &line, bool armed, bool ready, bool occupied,
  uint32_t runLimit, String &message, bool &success) {
  if (!TextUtil::startsWithIgnoreCase(line, "TMC")) return false;
  success = false;
#if !DRONE_CFG_ENABLE_TMC2209
  (void)armed; (void)ready; (void)occupied; (void)runLimit;
  message = "TMC2209 is not enabled; set DRONE_CFG_ENABLE_TMC2209 to 1 and rebuild/upload";
#else
  if (line.equalsIgnoreCase("TMCStatus")) { message = stateJson(); success = true; return true; }
  if (line.equalsIgnoreCase("TMCStop") || line.equalsIgnoreCase("TMCDisable") ||
      TextUtil::startsWithIgnoreCase(line, "TMCStop:") || TextUtil::startsWithIgnoreCase(line, "TMCDisable:")) {
    const int separator = line.indexOf(':'); uint32_t number = 0;
    if (separator >= 0 && (!TextUtil::parseUnsigned32(line.substring(separator + 1), number) || !number || number > count_)) {
      message = "invalid TMC motor number"; return true;
    }
    if (!number) stop(); else stopMotor(number - 1);
    message = "TMC2209 stopped; coils released (EN high)"; success = true; return true;
  }
  if (line.equalsIgnoreCase("TMCInit")) {
    if (!count_) { message = "no TMC motors configured; assign pins and restart"; return true; }
    if (armed || occupied || busy()) { message = "Disarm before TMCInit"; return true; }
    begin(); success = true;
    for (size_t i = 0; i < count_; ++i) success = success && channels_[i].ready;
    message = success ? "TMC2209 UART verified; outputs disabled" : "TMC2209 UART verification failed; check VM/VIO, address and RX single-wire wiring";
    return true;
  }
  const int colon = line.indexOf(':');
  const String verb = colon < 0 ? line : line.substring(0, colon);
  const bool pulse = verb.equalsIgnoreCase("TMCPulse"), move = verb.equalsIgnoreCase("TMCMove"), run = verb.equalsIgnoreCase("TMCRun");
  const bool current = verb.equalsIgnoreCase("TMCCurrent"), micro = verb.equalsIgnoreCase("TMCMicrosteps");
  if (colon < 0 || !(pulse || move || run || current || micro)) { message = "TMC command unknown; use TMCStatus, TMCInit, TMCMove, TMCRun, TMCPulse, TMCStop, TMCDisable, TMCCurrent or TMCMicrosteps"; return true; }
  String fields[4]; size_t count = 0; int offset = colon + 1;
  while (count < 4) {
    const int comma = line.indexOf(',', offset);
    fields[count++] = comma < 0 ? line.substring(offset) : line.substring(offset, comma);
    if (comma < 0) break;
    offset = comma + 1;
    if (count == 4) { message = "too many TMC arguments"; return true; }
  }
  const size_t required = pulse ? 3 : (move || run ? 2 : 1);
  uint32_t number = 1;
  if ((count != required && count != required + 1) ||
      (count > required && !TextUtil::parseUnsigned32(fields[required], number)) ||
      !number || number > count_) { message = "TMC argument count or motor number invalid"; return true; }
  const size_t index = number - 1; auto &channel = channels_[index];
  if (current || micro) {
    uint32_t value = 0;
    if (armed || occupied || busy()) { message = "Disarm before changing TMC current or microsteps"; return true; }
    if (!TextUtil::parseUnsigned32(fields[0], value) ||
        (current && (value < 50 || value > AppConfig::TMC2209_MAX_CURRENT_MA)) ||
        (micro && (!value || value > 256 || (value & (value - 1)) != 0))) { message = "TMC current must be 50..350 mA RMS; microsteps must be a power of two from 1..256"; return true; }
    if (!initialized_) { message = "send TMCInit first"; return true; }
    if (current) channel.current = static_cast<uint16_t>(value); else channel.microsteps = static_cast<uint16_t>(value);
    success = configure(index);
    if (success && micro) channel.stepper->setCurrentPosition(0);
    message = success ? "TMC2209 setting verified; motion disabled" : "TMC2209 UART/configuration verification failed; motion disabled";
    return true;
  }
  if (!armed || !ready) { message = "TMC2209 requires Arm and an open interlock"; return true; }
  if (occupied || channel.running) { message = "GPIO output or selected motor is already active; stop or wait"; return true; }
  if (!initialized_ || !channel.ready) { message = "TMC2209 UART not verified; disarm and send TMCInit"; return true; }
  // A driver power-cycle resets GCONF/current control. Never trust a cached
  // boot check when about to energize this channel again.
  if ((channel.driver->IOIN() >> 24) != 0x21U || (channel.driver->GCONF() & 0xC3U) != 0xC0U) {
    channel.ready = false; stopMotor(index);
    message = "TMC2209 UART/configuration lost; disarm and send TMCInit"; return true;
  }

  if (runLimit > AppConfig::TMC2209_MAX_RUN_MS) runLimit = AppConfig::TMC2209_MAX_RUN_MS;
  uint32_t rpm = 0, duration = runLimit; long direction = 1; float degrees = 0;
  if (pulse) {
    uint32_t parsedMotor = 0;
    if (!TextUtil::parseTmcPulse(line, duration, rpm, direction, &parsedMotor)) { message = "TMCPulse needs ms,RPM,direction(+1/-1)[,motor]"; return true; }
  } else if (move) {
    if (!TextUtil::parseFloat(fields[0], degrees) || !TextUtil::parseUnsigned32(fields[1], rpm) ||
        degrees == 0 || degrees > 360000.0f || degrees < -360000.0f) { message = "TMCMove needs nonzero signed degrees,RPM[,motor]"; return true; }
    direction = degrees > 0 ? 1 : -1;
  } else if (!TextUtil::parseUnsigned32(fields[0], rpm) || !TextUtil::parseLong(fields[1], direction) || (direction != 1 && direction != -1)) {
    message = "TMCRun needs RPM,direction(+1/-1)[,motor]"; return true;
  }
  if (!rpm || rpm > AppConfig::TMC2209_MAX_RPM || !duration || duration > runLimit) { message = "TMC speed or runtime exceeds the current payload/arm/motor limit"; return true; }
  const uint32_t stepsPerRev = uint32_t(AppConfig::TMC2209_STEPS_PER_REV) * channel.microsteps;
  const uint32_t hz = (stepsPerRev * rpm + 30U) / 60U;
  const uint32_t acceleration = stepsPerRev * 2U;
  int64_t steps;
  if (move) {
    const double magnitude = degrees > 0 ? degrees : -double(degrees);
    steps = static_cast<int64_t>(magnitude * stepsPerRev / 360.0 + 0.5);
    const uint64_t estimate = (uint64_t(steps) * 1000U + hz - 1) / hz + uint64_t(hz) * 1000U / acceleration + 1000U;
    if (!steps || estimate > runLimit) { message = "TMC move is too small or too long for the active run limit"; return true; }
    duration = static_cast<uint32_t>(estimate);
  } else {
    // Even if the control task stalls, the hardware pulse queue has a finite
    // distance. The task additionally disables EN at the requested deadline.
    steps = (uint64_t(hz) * duration + 999U) / 1000U;
  }
  steps *= direction;
  const int64_t target = int64_t(channel.stepper->getCurrentPosition()) + steps;
  if (steps < INT32_MIN || steps > INT32_MAX || target < INT32_MIN || target > INT32_MAX) { message = "TMC position limit reached; reinitialize while disarmed"; return true; }
  if (static_cast<int>(channel.stepper->setSpeedInHz(hz)) != 0 ||
      static_cast<int>(channel.stepper->setAcceleration(acceleration)) != 0 || !channel.stepper->enableOutputs()) { stopMotor(index); message = "TMC pulse engine setup failed"; return true; }
  if (static_cast<int>(channel.stepper->move(static_cast<int32_t>(steps))) != 0) { stopMotor(index); message = "TMC move was rejected by the pulse engine"; return true; }
  active_ = static_cast<uint8_t>(index); channel.started = millis(); channel.duration = duration; channel.running = true;
  message = "TMC2209 motor=" + String(number) + " running at " + String(rpm) + " RPM; bounded " + String(duration) + " ms";
  success = true;
#endif
  return true;
}

String Tmc2209Motor::stateJson() const {
#if DRONE_CFG_ENABLE_TMC2209
  const uint32_t now = millis();
  const auto &activeChannel = channels_[active_];
  const uint32_t elapsed = now - activeChannel.started;
  String json = "{\"enabled\":true,\"running\":" + String(busy() ? "true" : "false") +
    ",\"activeMotor\":" + String(active_ + 1) + ",\"remainingMs\":" + String(activeChannel.running && elapsed < activeChannel.duration ? activeChannel.duration - elapsed : 0) +
    ",\"maxRunMs\":" + String(AppConfig::TMC2209_MAX_RUN_MS) + ",\"maxRpm\":" + String(AppConfig::TMC2209_MAX_RPM) +
    ",\"maxRmsMa\":" + String(AppConfig::TMC2209_MAX_CURRENT_MA) + ",\"uartTx\":" + String(tx_) +
    ",\"uartRx\":" + String(rx_) + ",\"channels\":[";
  for (size_t i = 0; i < count_; ++i) {
    if (i) json += ',';
    const auto &pins = pins_[i]; const auto &channel = channels_[i];
    json += "[" + String(i + 1) + "," + String(pins.step) + "," + String(pins.dir) + "," + String(pins.enable) + "," + String(pins.address) + "," +
      String(channel.ready ? "true" : "false") + "," + String(channel.current) + "," + String(channel.microsteps) + "," + String(channel.actualCurrent) +
      "," + String(channel.running ? "true" : "false") + "," + String(channel.running && uint32_t(now - channel.started) < channel.duration ? channel.duration - uint32_t(now - channel.started) : 0) + "]";
  }
  return json + "]}";
#else
  return "{\"enabled\":false}";
#endif
}

uint32_t Tmc2209Motor::remainingMs(uint32_t now) const {
  uint32_t remaining = 0;
#if DRONE_CFG_ENABLE_TMC2209
  for (size_t i = 0; i < count_; ++i) {
    const auto &channel = channels_[i]; const uint32_t elapsed = now - channel.started;
    if (channel.running && elapsed < channel.duration && channel.duration - elapsed > remaining)
      remaining = channel.duration - elapsed;
  }
#else
  (void)now;
#endif
  return remaining;
}
