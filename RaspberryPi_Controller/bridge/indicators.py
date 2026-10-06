import threading
import time


class Indicators:
    def __init__(self, config, clock=time.monotonic):
        self.config, self.clock = config, clock
        self.enabled = config["indicators"]
        self.pins = (config["connection_led_pin"], config["activity_led_pin"])
        self.devices = []
        self.connected = False
        self.busy = False
        self.activity_until = 0
        self.pattern = None
        self.values = [False, False]
        self.lock = threading.RLock()
        self.closed = threading.Event()
        if self.enabled and not config["simulate"]:
            from gpiozero import OutputDevice
            try:
                for pin in self.pins:
                    self.devices.append(OutputDevice(pin, initial_value=False))
            except Exception:
                self.close()
                raise
        self.thread = None
        if self.enabled:
            self.thread = threading.Thread(target=self.run, name="status-indicators", daemon=True)
            self.thread.start()

    def note_activity(self):
        self.activity_until = self.clock() + 0.12

    def test(self, which="both"):
        if not self.enabled:
            raise ValueError("indicators disabled in installation config")
        with self.lock:
            if self.pattern:
                raise ValueError("indicator test already active")
            # 2 connection pulses then 3 activity pulses, or 4 isolated pulses.
            targets = [0] * 2 + [1] * 3 if which == "both" else [0 if which == "connection" else 1] * 4
            self.pattern = (self.clock(), targets)

    def tick(self):
        if not self.enabled:
            return
        with self.lock:
            now = self.clock()
            values = [self.connected or int(now * 2) % 2 == 0, self.busy or now < self.activity_until]
            if self.pattern:
                start, targets = self.pattern
                index = int((now - start) / .5)
                values = [False, False]
                if index >= len(targets):
                    self.pattern = None
                elif (now - start) % .5 < .25:
                    values[targets[index]] = True
            for index, value in enumerate(values):
                if self.devices:
                    self.devices[index].value = value
            self.values = values

    def run(self):
        while not self.closed.wait(.02):
            try:
                self.tick()
            except Exception:
                self.closed.set()
                for device in self.devices:
                    device.off()

    def state(self):
        return dict(enabled=self.enabled, connectionPin=self.pins[0], activityPin=self.pins[1],
                    connection=self.values[0], activity=self.values[1], testing=bool(self.pattern))

    def close(self):
        self.closed.set()
        thread = getattr(self, "thread", None)
        if thread:
            thread.join(timeout=1)
        for device in self.devices:
            device.off()
            device.close()
        self.devices = []
