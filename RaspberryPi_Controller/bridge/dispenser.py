"""Dispenser deadlines are enforced independently of HTTP/BLE and disk work."""
import threading
import time
import math
from .config import reserved_pins, UINT32_MAX


SETTINGS = ("pin", "activeHigh", "defaultPulseMs", "maxPulseMs", "armTimeoutMs")


def validate_settings(config, settings):
    if not isinstance(settings, dict) or set(settings) != set(SETTINGS):
        raise ValueError("invalid dispenser settings")
    pin = settings["pin"]
    if type(pin) is not int or pin not in config["allowed_output_pins"] or pin == config["interlock_pin"] or pin in reserved_pins(config):
        raise ValueError("unavailable BCM pin")
    if type(settings["activeHigh"]) is not bool:
        raise ValueError("invalid output polarity")
    for field in SETTINGS[2:]:
        if type(settings[field]) is not int or not (1 if field == "defaultPulseMs" else 0) <= settings[field] <= UINT32_MAX:
            raise ValueError("invalid timing value")
    if settings["maxPulseMs"] and settings["defaultPulseMs"] > settings["maxPulseMs"]:
        raise ValueError("default pulse exceeds maximum pulse")
    if settings["armTimeoutMs"] and settings["armTimeoutMs"] < (settings["maxPulseMs"] or settings["defaultPulseMs"]):
        raise ValueError("arm timeout must accommodate a maximum/default pulse")
    for field, ceiling in (("maxPulseMs", "max_pulse_ms"), ("armTimeoutMs", "arm_timeout_ms")):
        if config[ceiling] and (not settings[field] or settings[field] > config[ceiling]):
            raise ValueError("settings exceed installation timing ceilings")


class Dispenser:
    def __init__(self, config, gpio, clock=time.monotonic, watchdog=True):
        self.config, self.gpio, self.clock = config, gpio, clock
        self.defaults = dict(zip(SETTINGS, (config["dispenser_pin"], config["active_high"],
                             config["default_pulse_ms"], config["max_pulse_ms"], config["arm_timeout_ms"])))
        self.settings = dict(self.defaults)
        self.lock = threading.RLock()
        self.armed_until = self.pulse_until = 0
        self.faulted = False
        self.count = 0
        self.max_lateness_ms = 0
        self.reason = "boot"
        self.closed = threading.Event()
        self.thread = None
        self.safety_permit = lambda: True
        self.gpio.write(False)
        if watchdog:
            self.thread = threading.Thread(target=self._watch, name="dispenser-deadlines", daemon=True)
            self.thread.start()

    def validate(self, settings):
        validate_settings(self.config, settings)

    def apply(self, settings):
        self.validate(settings)
        with self.lock:
            self.require_disarmed()
            try:
                self.gpio.configure(settings["pin"], settings["activeHigh"])
                self.settings = dict(settings)
            except Exception:
                self.faulted = True
                raise

    def require_disarmed(self):
        with self.lock:
            if self.armed_until or self.pulse_until:
                raise ValueError("disarm before changing settings")

    def arm(self):
        with self.lock:
            self.tick()
            if self.pulse_until:
                raise ValueError("cannot rearm during a pulse")
            if self.faulted or self.gpio.is_interlock_open():
                raise ValueError("interlock/fault blocks arming; close interlock and Disarm to clear")
            self.armed_until = self.clock() + self.settings["armTimeoutMs"] / 1000 if self.settings["armTimeoutMs"] else math.inf
            self.reason = "armed"

    def dispense(self, duration):
        with self.lock:
            self.tick()
            if type(duration) is not int or not 1 <= duration <= (self.settings["maxPulseMs"] or UINT32_MAX):
                raise ValueError("pulse exceeds configured limit")
            if self.faulted or not self.armed_until:
                raise ValueError("dispenser is disarmed or faulted")
            if self.pulse_until:
                raise ValueError("a pulse is already active")
            deadline = self.clock() + duration / 1000
            if deadline > self.armed_until:
                raise ValueError("complete pulse must fit remaining arm window; Arm again")
            # Set the deadline before the output so even an output-driver error
            # has a known safe shutdown path.
            self.pulse_until = deadline
            try:
                self.gpio.write(True)
            except Exception:
                self.faulted = True
                self.stop(True, "output fault")
                raise
            self.count += 1
            self.reason = "dispensing"

    def stop(self, disarm=True, reason="stopped"):
        with self.lock:
            self.gpio.write(False)
            self.pulse_until = 0
            if disarm:
                self.armed_until = 0
            self.reason = reason

    def disarm(self):
        with self.lock:
            self.stop()
            if not self.gpio.is_interlock_open():
                self.faulted = False

    def tick(self):
        with self.lock:
            now = self.clock()
            if not self.safety_permit() and (self.armed_until or self.pulse_until):
                self.stop(True, "position stale; sequence output inhibited")
            elif self.gpio.is_interlock_open():
                if self.armed_until or self.pulse_until:
                    self.stop(True, "interlock opened")
                    self.faulted = True
            elif self.armed_until and now >= self.armed_until:
                self.stop(True, "arm window expired")
            elif self.pulse_until and now >= self.pulse_until:
                self.max_lateness_ms = max(self.max_lateness_ms, (now - self.pulse_until) * 1000)
                self.stop(False, "pulse complete")

    def state(self):
        with self.lock:
            self.tick()
            now = self.clock()
            return dict(self.settings, armed=bool(self.armed_until), dispensing=bool(self.pulse_until),
                        faulted=self.faulted, interlockConfigured=self.config["interlock_pin"] is not None,
                        interlockOpen=self.gpio.is_interlock_open(), count=self.count,
                        remainingMs=max(0, int((self.pulse_until - now) * 1000)),
                        armRemainingMs=UINT32_MAX if math.isinf(self.armed_until) else max(0, int((self.armed_until - now) * 1000)),
                        armUnlimited=bool(self.armed_until) and not self.settings["armTimeoutMs"],
                        installationMaxPulseMs=self.config["max_pulse_ms"], installationArmTimeoutMs=self.config["arm_timeout_ms"],
                        maxStopLatenessMs=round(self.max_lateness_ms, 2), lastReason=self.reason)

    def _watch(self):
        while not self.closed.wait(0.005):
            try:
                self.tick()
            except Exception:
                with self.lock:
                    self.faulted = True
                    self.armed_until = self.pulse_until = 0
                    self.reason = "GPIO watchdog fault"
                    try:
                        self.gpio.write(False)
                    except Exception:
                        pass

    def close(self):
        if self.closed.is_set():
            return
        self.closed.set()
        if self.thread:
            self.thread.join(timeout=1)
        try:
            self.stop(True, "shutdown")
        finally:
            self.gpio.close()
