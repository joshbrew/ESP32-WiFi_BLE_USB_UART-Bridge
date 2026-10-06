import asyncio

from .streams import SerialTransport, BufferedOutput


class SimulatedRadio(BufferedOutput):
    async def start(self):
        pass

    async def close(self):
        self.ready = False
        self.clear()


class TransportManager:
    def __init__(self, controller):
        self.controller = controller
        self.serial = []
        self.ble = self.spp = None
        self.lock = asyncio.Lock()

    async def start(self):
        config = self.controller.config
        self.controller.radio.transport_manager = self
        for source, key in (("USB", "usb_device"), ("UART", "uart_device")):
            if config[key]:
                transport = SerialTransport(self.controller, source, config[key], config["baud"])
                self.serial.append(transport)
                await transport.start()
        await self.configure(self.controller.radio.ble_enabled, self.controller.radio.spp_enabled)

    async def configure(self, ble, spp):
        async with self.lock:
            for name, enabled, source in (("ble", ble, "BLE"), ("spp", spp, "SPP")):
                current = getattr(self, name)
                if current and not self.controller.config["simulate"]:
                    alive = self.controller.ble_running if name == "ble" else current.registered
                    if not alive:
                        await current.close()
                        setattr(self, name, None)
                        current = None
                if current and not enabled:
                    await current.close()
                    setattr(self, name, None)
                    self.controller.outputs.pop(source, None)
                if enabled and current is None:
                    if self.controller.config["simulate"]:
                        transport = SimulatedRadio()
                    elif name == "ble":
                        from .ble import BLETransport
                        transport = BLETransport(self.controller)
                    else:
                        from .spp import SPPTransport
                        transport = SPPTransport(self.controller)
                    try:
                        await asyncio.wait_for(transport.start(), 15)
                    except BaseException:
                        await transport.close()
                        raise
                    setattr(self, name, transport)
                    self.controller.outputs[source] = transport
            self.controller.ble_running = bool(self.ble)
            self.controller.ble_error = ""

    async def close(self):
        await self.configure(False, False)
        for transport in self.serial:
            await transport.close()
