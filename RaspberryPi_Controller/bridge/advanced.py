"""Optional 28BYJ-48/ULN2003, MCP4725 and digital-output bench profile."""
import math
import threading
import time

from .store import Store


def numeric(value, low, high):
    try:
        result = float(value)
    except (ValueError, TypeError):
        raise ValueError("invalid numeric value") from None
    if not math.isfinite(result) or not low <= result <= high:
        raise ValueError(f"value must be between {low} and {high}")
    return result


class AdvancedIO:
    def __init__(self, config):
        self.coils, self.digital, self.bus = [], None, None
        self.code, self.digital_value, self.pattern = 0, False, [0, 0, 0, 0]
        if not config["simulate"]:
            from gpiozero import OutputDevice
            from smbus import SMBus
            try:
                for pin in config["stepper_pins"]:
                    self.coils.append(OutputDevice(pin, initial_value=False))
                self.digital = OutputDevice(config["dispenser_pin"], initial_value=False)
                self.bus = SMBus(config["dac_i2c_bus"])
                self.address = config["dac_i2c_address"]
                self.write_dac(0)
            except Exception:
                self.close()
                raise

    def write_coils(self, pattern):
        for device, value in zip(self.coils, pattern):
            device.value = value
        self.pattern = list(pattern)

    def write_dac(self, code):
        if self.bus:
            # MCP4725 DAC-register command; never writes its EEPROM.
            self.bus.write_i2c_block_data(self.address, 0x40, [code >> 4, (code & 15) << 4])
        self.code = code

    def write_digital(self, active):
        if self.digital:
            self.digital.value = active
        self.digital_value = active

    def close(self):
        for action in (lambda: self.write_coils([0, 0, 0, 0]), lambda: self.write_digital(False)):
            try: action()
            except Exception: pass
        if self.bus:
            try:
                self.write_dac(0)
            except Exception:
                pass
            finally:
                self.bus.close()
                self.bus = None
        for coil in self.coils:
            coil.close()
        self.coils = []
        if self.digital:
            self.digital.close()
            self.digital = None


class Advanced:
    ACTIONS = {"RPM", "DEG", "MOVEFULLCW", "MOVEFULLCCW", "MOVEHALFCW", "MOVEHALFCCW", "STOP", "COILSOFF", "DAC1", "GPIO26", "DACALL", "OUTPUTALL", "DACTEST3S"}
    COMMANDS = ACTIONS | {"GETMOTORSTATS", "SETREVSTEPS", "SETMINRPM", "SETMAXRPM", "SETSTARTRPM", "SETRAMPRPM", "SETMINSTEPINTERVALUS", "STEPMODE", "HOLDTORQUE", "PRINTSTEPORDER", "NEXTSTEPORDER", "STEPORDER", "DACSTATUS", "OUTPUTSTATUS", "DACREFMV", "DACSAVE", "DACLOAD", "DACDEFAULTS", "DACERASE"}
    HALF = ((1, 0, 0, 0), (1, 1, 0, 0), (0, 1, 0, 0), (0, 1, 1, 0),
            (0, 0, 1, 0), (0, 0, 1, 1), (0, 0, 0, 1), (1, 0, 0, 1))

    def __init__(self, config, clock=time.monotonic, io=None, watchdog=True):
        self.config, self.clock = config, clock
        self.io = io or AdvancedIO(config)
        self.lock = threading.RLock()
        self.position = self.remaining = self.phase = 0
        self.direction, self.target_rpm, self.current_rpm = 1, 0, 0
        self.next_step = 0
        self.motion_started = 0
        self.coils_on = False
        self.max_lateness_us = 0
        self.settings = dict(baseStepsPerRev=config["stepper_steps_per_rev"], minRpm=.1,
                             maxRpm=config["stepper_max_rpm"], startRpm=2., rampRpmPerSecond=6.,
                             minIntervalUs=config["stepper_min_interval_us"], stepMode=8,
                             stepOrder="0123", holdTorque=False)
        self.reference = config["dac_reference_mv"]
        self.mv = 500
        self.dac_on = self.digital_on = False
        self.dac_deadline = self.digital_deadline = 0
        self.fault = ""
        self.store = Store(config["data_dir"], "dac.json")
        try:
            saved = self.store.load_generic(None)
            if saved is not None:
                self.validate_dac(saved)
                self.reference, self.mv = saved["referenceMv"], saved["millivolts"]
        except Exception:
            self.io.close()
            raise
        self.closed = threading.Event()
        self.thread = None
        if watchdog:
            self.thread = threading.Thread(target=self.run, name="stepper-dac-deadlines", daemon=True)
            self.thread.start()

    def validate_dac(self, record):
        if not isinstance(record, dict) or set(record) != {"referenceMv", "millivolts"}:
            raise ValueError("invalid saved DAC settings")
        if type(record["referenceMv"]) is not int or not 2500 <= record["referenceMv"] <= 3600:
            raise ValueError("invalid DAC reference")
        if type(record["millivolts"]) is not int or not 0 <= record["millivolts"] <= record["referenceMv"]:
            raise ValueError("invalid DAC millivolts")

    def move_spec(self, cmd, value):
        parts = value.split(",")
        if len(parts) != 3:
            raise ValueError("move expects rpm,steps/degrees,direction (1=CW, 2=CCW)")
        rpm = numeric(parts[0], self.settings["minRpm"], self.settings["maxRpm"])
        count = numeric(parts[1], 0, 1000000)
        if parts[2].strip() not in {"1", "2"}:
            raise ValueError("direction must be 1 or 2")
        effective = self.settings["baseStepsPerRev"] * (2 if self.settings["stepMode"] == 8 else 1)
        if cmd == "RPM" and not count.is_integer():
            raise ValueError("step count must be an integer")
        steps = int(round(count * effective / 360)) if cmd == "DEG" else int(count)
        if not 1 <= steps <= 1000000:
            raise ValueError("move must contain 1-1000000 effective steps")
        return rpm, steps, 1 if parts[2].strip() == "1" else -1

    def validate_routine(self, line):
        cmd, _, value = line.partition(":")
        cmd = cmd.upper()
        if cmd not in self.ACTIONS or cmd == "DACTEST3S":
            raise ValueError("hardware command is not allowed in a routine")
        if cmd in {"RPM", "DEG"}:
            self.move_spec(cmd, value)
        elif cmd in {"DAC1", "GPIO26", "DACALL", "OUTPUTALL"}:
            if cmd == "DAC1" and value.upper().startswith("MV:"):
                numeric(value[3:], 0, self.reference)
            elif value.upper() not in {"ON", "OFF"}:
                raise ValueError("routine output expects ON/OFF or DAC1:MV:millivolts")
        elif value:
            raise ValueError("unexpected action parameter")
        return line

    def require_idle(self):
        if self.remaining:
            raise ValueError("stop the motor before changing configuration")

    def start_move(self, rpm, steps, direction):
        if self.fault:
            raise ValueError("hardware fault: " + self.fault)
        self.require_idle()
        self.target_rpm, self.remaining, self.direction = rpm, steps, direction
        self.current_rpm = min(rpm, self.settings["startRpm"])
        self.next_step = self.motion_started = self.clock()

    def set_output(self, which, active, milliseconds=0):
        if self.fault and active:
            raise ValueError("hardware fault: " + self.fault)
        deadline = self.clock() + milliseconds / 1000 if active and milliseconds else 0
        if which == "dac":
            self.dac_on, self.dac_deadline = active, deadline
            self.io.write_dac(round(self.mv / self.reference * 4095) if active else 0)
        else:
            self.digital_on, self.digital_deadline = active, deadline
            self.io.write_digital(active)

    def handle(self, line):
        cmd, _, value = line.partition(":")
        cmd = cmd.upper()
        if cmd not in self.COMMANDS:
            return False
        with self.lock:
            if cmd in {"RPM", "DEG"}:
                self.start_move(*self.move_spec(cmd, value))
            elif cmd.startswith("MOVE"):
                degrees = 180 if "HALF" in cmd else 360
                self.start_move(*self.move_spec("DEG", f"{min(10, self.settings['maxRpm'])},{degrees},{2 if cmd.endswith('CCW') else 1}"))
            elif cmd in {"STOP", "COILSOFF"}:
                self.remaining = 0
                if cmd == "COILSOFF" or not self.settings["holdTorque"]:
                    self.io.write_coils([0, 0, 0, 0])
                    self.coils_on = False
            elif cmd in {"GETMOTORSTATS", "PRINTSTEPORDER", "DACSTATUS", "OUTPUTSTATUS"}:
                pass
            elif cmd.startswith("SET") or cmd in {"STEPMODE", "HOLDTORQUE", "STEPORDER", "NEXTSTEPORDER"}:
                self.require_idle()
                fields = {"SETREVSTEPS": ("baseStepsPerRev", 1, 1000000), "SETMINRPM": ("minRpm", .1, self.settings["maxRpm"]),
                          "SETMAXRPM": ("maxRpm", self.settings["minRpm"], self.config["stepper_max_rpm"]),
                          "SETSTARTRPM": ("startRpm", .1, self.settings["maxRpm"]), "SETRAMPRPM": ("rampRpmPerSecond", .1, 1000),
                          "SETMINSTEPINTERVALUS": ("minIntervalUs", self.config["stepper_min_interval_us"], 1000000)}
                if cmd in fields:
                    field, low, high = fields[cmd]
                    result = numeric(value, low, high)
                    if field in {"baseStepsPerRev", "minIntervalUs"} and not result.is_integer():
                        raise ValueError("integer required")
                    self.settings[field] = int(result) if field in {"baseStepsPerRev", "minIntervalUs"} else result
                elif cmd == "STEPMODE":
                    if value not in {"4", "8"}:
                        raise ValueError("StepMode must be 4 or 8")
                    self.settings["stepMode"] = int(value)
                    self.phase = 0
                    self.io.write_coils([0, 0, 0, 0]); self.coils_on = False
                elif cmd == "HOLDTORQUE":
                    if value not in {"0", "1"}:
                        raise ValueError("HoldTorque must be 0 or 1")
                    self.settings["holdTorque"] = value == "1"
                    if value == "0":
                        self.io.write_coils([0, 0, 0, 0]); self.coils_on = False
                else:
                    order = self.settings["stepOrder"][1:] + self.settings["stepOrder"][:1] if cmd == "NEXTSTEPORDER" else value
                    if len(order) != 4 or set(order) != set("0123"):
                        raise ValueError("StepOrder must be a permutation of 0123")
                    self.settings["stepOrder"] = order
                    self.io.write_coils([0, 0, 0, 0]); self.coils_on = False
            elif cmd == "DACREFMV":
                reference = int(numeric(value, 2500, 3600))
                if self.mv > reference:
                    raise ValueError("DAC voltage exceeds new reference")
                self.reference = reference
                self.io.write_dac(round(self.mv / self.reference * 4095) if self.dac_on else 0)
            elif cmd in {"DAC1", "GPIO26", "DACALL", "OUTPUTALL", "DACTEST3S"}:
                if cmd == "DACTEST3S":
                    for channel in ("dac", "digital"):
                        self.set_output(channel, True, 3000)
                elif cmd == "DAC1" and value.upper().startswith("MV:"):
                    self.mv = int(numeric(value[3:], 0, self.reference))
                    self.io.write_dac(round(self.mv / self.reference * 4095) if self.dac_on else 0)
                else:
                    if value.upper() not in {"ON", "OFF", "TEST3S"}:
                        raise ValueError("output expects ON, OFF, TEST3S, or DAC1:MV:millivolts")
                    channels = ("dac", "digital") if cmd in {"DACALL", "OUTPUTALL"} else ("dac",) if cmd == "DAC1" else ("digital",)
                    for channel in channels:
                        self.set_output(channel, value.upper() != "OFF", 3000 if value.upper() == "TEST3S" else 0)
            elif cmd == "DACSAVE":
                self.store.save_generic(dict(referenceMv=self.reference, millivolts=self.mv))
            elif cmd == "DACLOAD":
                saved = self.store.load_generic(dict(referenceMv=self.config["dac_reference_mv"], millivolts=500))
                self.validate_dac(saved)
                self.stop()
                self.reference, self.mv = saved["referenceMv"], saved["millivolts"]
            elif cmd == "DACDEFAULTS":
                self.stop()
                self.reference, self.mv = self.config["dac_reference_mv"], 500
            elif cmd == "DACERASE":
                self.store.path.unlink(missing_ok=True)
        return True

    def tick(self):
        with self.lock:
            now = self.clock()
            if self.dac_deadline and now >= self.dac_deadline:
                self.set_output("dac", False)
            if self.digital_deadline and now >= self.digital_deadline:
                self.set_output("digital", False)
            if self.remaining and now >= self.next_step:
                self.max_lateness_us = max(self.max_lateness_us, (now - self.next_step) * 1000000)
                self.current_rpm = min(self.target_rpm, self.settings["startRpm"] + (now - self.motion_started) * self.settings["rampRpmPerSecond"])
                table = self.HALF if self.settings["stepMode"] == 8 else self.HALF[1::2]
                self.phase = (self.phase + self.direction) % len(table)
                pattern = table[self.phase]
                mapped = [0, 0, 0, 0]
                for index, target in enumerate(self.settings["stepOrder"]):
                    mapped[int(target)] = pattern[index]
                self.io.write_coils(mapped)
                self.coils_on = True
                self.remaining -= 1
                self.position += self.direction
                effective = self.settings["baseStepsPerRev"] * (2 if self.settings["stepMode"] == 8 else 1)
                interval = max(self.settings["minIntervalUs"] / 1000000, 60 / (effective * self.current_rpm))
                self.next_step = now + interval  # Never burst missed steps to catch up.
                if not self.remaining and not self.settings["holdTorque"]:
                    self.io.write_coils([0, 0, 0, 0]); self.coils_on = False

    def run(self):
        while not self.closed.wait(.001):
            try:
                self.tick()
            except Exception as error:
                self.fault = str(error)
                try:
                    self.stop()
                finally:
                    self.closed.set()

    def stop(self):
        with self.lock:
            self.remaining = 0
            errors = []
            for action in (lambda: self.io.write_coils([0, 0, 0, 0]), lambda: self.set_output("dac", False), lambda: self.set_output("digital", False)):
                try:
                    action()
                except Exception as error:
                    errors.append(str(error))
            self.coils_on = False
            if errors:
                self.fault = "; ".join(errors)

    def routine_state(self):
        self.tick()
        return dict(healthy=not self.fault, busy=bool(self.remaining or self.dac_deadline or self.digital_deadline))

    def state(self):
        with self.lock:
            return dict(stepper=dict(self.settings, positionSteps=self.position, moving=bool(self.remaining), remainingSteps=self.remaining,
                                     currentRpm=self.current_rpm, targetRpm=self.target_rpm, coilsOn=self.coils_on,
                                     maxStepLatenessUs=round(self.max_lateness_us, 1)),
                        dac=dict(referenceMv=self.reference, millivolts=self.mv, enabled=self.dac_on, digitalEnabled=self.digital_on,
                                 i2cAddress=self.config["dac_i2c_address"], fault=self.fault))

    def close(self):
        self.closed.set()
        if self.thread:
            self.thread.join(timeout=1)
        self.stop()
        self.io.close()
