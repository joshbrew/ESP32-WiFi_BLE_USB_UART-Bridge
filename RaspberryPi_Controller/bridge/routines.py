import copy
import re
import time
import math
from .config import UINT32_MAX


def name_key(name):
    if not isinstance(name, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,15}", name):
        raise ValueError("name must be 1-15 letters, digits, hyphens, or underscores")
    return name.lower()


def number(value, maximum=UINT32_MAX, minimum=1):
    if not re.fullmatch(r"[0-9]{1,10}", value) or not minimum <= int(value) <= maximum:
        raise ValueError(f"value must be {minimum}-{maximum}")
    return int(value)


class Routines:
    def __init__(self, dispenser, saved, publish, clock=time.monotonic, hardware=None):
        self.dispenser, self.publish, self.clock = dispenser, publish, clock
        self.hardware = hardware
        if not isinstance(saved, dict) or len(saved) > 4:
            raise ValueError("invalid saved routine library")
        for key, record in saved.items():
            if name_key(key) != key:
                raise ValueError("invalid routine key")
            self.validate(record)
        self.library = copy.deepcopy(saved)
        self.running = None
        self.last_result = "idle"
        self.source = "Internal"
        self.request_id = ""

    def step(self, spec):
        kind, _, value = spec.partition(":")
        kind = kind.upper()
        if kind == "WAIT_IDLE" and spec.upper() == "WAIT_IDLE":
            return "WAIT_IDLE"
        if kind in {"WAIT", "START_WAIT"}:
            return f"{kind}:{number(value, UINT32_MAX, 0)}"
        if kind == "COMMAND":
            return "COMMAND:" + self.hardware_command(value)
        if kind == "DISPENSE":
            return self.hardware_command(spec)
        raise ValueError("step must be START_WAIT:ms, WAIT:ms, WAIT_IDLE, DISPENSE:ms, or COMMAND:hardware action")

    def hardware_command(self, command):
        if self.hardware:
            return self.hardware.validate_routine(command)
        kind, _, value = command.partition(":")
        if kind.upper() == "DISPENSE":
            return f"Dispense:{number(value)}"
        if command.upper() == "DISPENSESTOP":
            return "DispenseStop"
        raise ValueError("routine hardware command is not allowed")

    def validate(self, record):
        if not isinstance(record, dict) or set(record) != {"steps", "repeats"}:
            raise ValueError("invalid routine record")
        if type(record["repeats"]) is not int or not 0 <= record["repeats"] <= UINT32_MAX:
            raise ValueError("routine repeat count must fit unsigned 32-bit")
        if not isinstance(record["steps"], list) or not 1 <= len(record["steps"]) <= 10:
            raise ValueError("routine must contain 1-10 steps")
        for index, spec in enumerate(record["steps"]):
            if not isinstance(spec, str):
                raise ValueError("invalid routine step")
            parsed = self.step(spec)
            if parsed.startswith("START_WAIT:") and index != 0:
                raise ValueError("START_WAIT must be the first step")

    def require_idle(self):
        if self.running:
            raise ValueError("stop the active routine first")

    def start(self, key, source, request_id, record=None):
        self.require_idle()
        record = self.library[key] if record is None else record
        self.validate(record)
        if self.hardware:
            state = self.hardware.routine_state()
            if not state["healthy"] or state["busy"] or self.hardware.dac_on or self.hardware.digital_on:
                raise ValueError("routine requires healthy, inactive advanced hardware")
        else:
            state = self.dispenser.state()
            if not state["armed"] or state["dispensing"] or state["faulted"]:
                raise ValueError("routine requires armed, idle, healthy dispenser")
            duration = self.estimate_ms(record)
            if not record["repeats"] and self.dispenser.settings["armTimeoutMs"]:
                raise ValueError("continuous routine requires disabled arm expiry")
            if self.dispenser.settings["armTimeoutMs"] and duration > state["armRemainingMs"]:
                raise ValueError("routine exceeds remaining arm window (including 1000 ms service margin)")
        self.running = copy.deepcopy(record)
        self.name, self.index, self.repeat = key, 0, 0
        self.started = self.clock()
        self.wait_until = None
        self.source, self.request_id = source, request_id
        self.last_result = "running"

    def estimate_ms(self, record):
        once = cycle = 0
        for spec in record["steps"]:
            kind, _, value = spec.partition(":")
            kind = kind.upper()
            if kind == "START_WAIT": once += int(value)
            elif kind == "WAIT": cycle += int(value)
            else:
                action = value if kind == "COMMAND" else spec
                if action.upper().startswith("DISPENSE:"):
                    pulse = number(action.partition(":")[2])
                    if self.dispenser.settings["maxPulseMs"] and pulse > self.dispenser.settings["maxPulseMs"]:
                        raise ValueError("routine pulse exceeds active payload maximum")
                    cycle += pulse
        return once + cycle * record["repeats"] + 1000 if record["repeats"] else math.inf

    def stop(self, reason="stopped"):
        self.running = None
        self.dispenser.stop(True, reason)
        if self.hardware:
            self.hardware.stop()
        self.last_result = reason

    def tick(self):
        if not self.running:
            return
        try:
            if self.hardware:
                condition = self.hardware.routine_state()
                if not condition["healthy"]:
                    raise ValueError("advanced hardware fault")
                state = dict(dispensing=condition["busy"])
            else:
                state = self.dispenser.state()
                if not state["armed"] or state["faulted"] or state["interlockOpen"]:
                    raise ValueError("routine safety condition changed")
            steps = self.running["steps"]
            if self.index == len(steps):
                if state["dispensing"]:
                    return  # Allow a trailing pulse to finish in full.
                self.repeat = self.repeat + 1 if self.running["repeats"] else min(self.repeat + 1, UINT32_MAX - 1)
                if self.running["repeats"] and self.repeat >= self.running["repeats"]:
                    self.stop("complete")
                    self.publish("[DONE] routine complete", self.source, self.request_id, "status")
                    return
                self.index = 0
            spec = steps[self.index]
            kind, _, value = spec.partition(":")
            if kind.upper() == "START_WAIT" and self.repeat > 0:
                self.index += 1
                return
            if kind.upper() in {"WAIT", "START_WAIT"}:
                if self.wait_until is None:
                    self.wait_until = self.clock() + int(value) / 1000
                if self.clock() < self.wait_until:
                    return
                self.wait_until = None
            elif kind.upper() == "WAIT_IDLE":
                if state["dispensing"]:
                    return
            else:
                action = value if kind.upper() == "COMMAND" else spec
                action = self.hardware_command(action)
                if self.hardware:
                    self.hardware.handle(action)
                elif action == "DispenseStop":
                    self.stop("routine dispense stop")
                    return
                else:
                    self.dispenser.dispense(int(action.partition(":")[2]))
            self.index += 1
        except Exception as error:
            self.stop(f"failed: {error}")
            self.publish(self.last_result, self.source, self.request_id, "error")

    def state(self):
        return dict(active=bool(self.running), name=getattr(self, "name", "") if self.running else "",
                    step=getattr(self, "index", 0), repeat=getattr(self, "repeat", 0),
                    repeats=self.running["repeats"] if self.running else 0,
                    continuous=bool(self.running) and self.running["repeats"] == 0,
                    delayRemainingMs=max(0, int(((self.wait_until or self.clock()) - self.clock()) * 1000))
                      if self.running and self.index < len(self.running["steps"]) and self.running["steps"][self.index].startswith("START_WAIT:") else 0,
                    lastResult=self.last_result, stored=len(self.library))
