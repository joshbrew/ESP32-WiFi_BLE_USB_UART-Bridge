class SimulatedGPIO:
    """Explicit simulation only; hardware initialization never falls back to this."""
    def __init__(self, config):
        self.active = False
        self.interlock_open = False
        self.pin = config["dispenser_pin"]

    def configure(self, pin, active_high):
        self.write(False)
        self.pin = pin

    def write(self, active):
        self.active = active

    def is_interlock_open(self):
        return self.interlock_open

    def close(self):
        self.write(False)


class PiGPIO:
    def __init__(self, config):
        from gpiozero import DigitalInputDevice
        self.output = None
        self.interlock = None
        try:
            self.configure(config["dispenser_pin"], config["active_high"])
            if config["interlock_pin"] is not None:
                # An open wire pulls to the unsafe level. A closed contact to
                # the opposite rail permits operation; gpiozero is_active = closed.
                self.interlock = DigitalInputDevice(
                    config["interlock_pin"], pull_up=config["interlock_open_high"],
                )
        except Exception:
            self.close()
            raise

    def configure(self, pin, active_high):
        from gpiozero import OutputDevice
        if self.output:
            self.output.off()
            self.output.close()
            self.output = None
        self.output = OutputDevice(pin, active_high=active_high, initial_value=False)

    def write(self, active):
        self.output.value = active

    def is_interlock_open(self):
        return bool(self.interlock and not self.interlock.is_active)

    def close(self):
        if self.output:
            self.output.off()
            self.output.close()
            self.output = None
        if self.interlock:
            self.interlock.close()
            self.interlock = None
