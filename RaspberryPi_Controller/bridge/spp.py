"""Classic Bluetooth RFCOMM serial server registered through BlueZ Profile1."""
import asyncio
import os
import socket

from dbus_next import DBusError, Variant
from dbus_next.aio import MessageBus
from dbus_next.constants import BusType
from dbus_next.service import ServiceInterface, method

from .streams import BufferedOutput, LineInput


PATH = "/org/pi_controller/spp"
UUID = "00001101-0000-1000-8000-00805f9b34fb"


class Profile(ServiceInterface):
    def __init__(self, transport):
        super().__init__("org.bluez.Profile1")
        self.transport = transport

    @method()
    def Release(self):
        self.transport.disconnect()

    @method()
    def NewConnection(self, device: "o", fd: "h", properties: "a{sv}"):
        if self.transport.socket:
            os.close(fd)
            raise DBusError("org.bluez.Error.Rejected", "one SPP client at a time")
        try:
            self.transport.socket = socket.socket(fileno=fd)
            self.transport.socket.setblocking(False)
            self.transport.device = device
            self.transport.ready = True
            self.transport.task = asyncio.create_task(self.transport.serve())
        except Exception:
            self.transport.disconnect()
            raise

    @method()
    def RequestDisconnection(self, device: "o"):
        if device == self.transport.device:
            self.transport.disconnect()


class SPPTransport(BufferedOutput):
    def __init__(self, controller):
        super().__init__()
        self.controller = controller
        self.bus = self.manager = self.socket = None
        self.device = ""
        self.task = self.monitor_task = None
        self.registered = False
        self.input = LineInput(controller, "SPP", self.send)

    async def start(self):
        self.bus = await MessageBus(bus_type=BusType.SYSTEM, negotiate_unix_fd=True).connect()
        node = await self.bus.introspect("org.bluez", "/org/bluez")
        self.manager = self.bus.get_proxy_object("org.bluez", "/org/bluez", node).get_interface("org.bluez.ProfileManager1")
        self.bus.export(PATH, Profile(self))
        await self.manager.call_register_profile(PATH, UUID, {
            "Name": Variant("s", "Pi Controller Serial"), "Role": Variant("s", "server"),
            "Channel": Variant("q", self.controller.config["spp_channel"]),
            "RequireAuthentication": Variant("b", True), "RequireAuthorization": Variant("b", True),
        })
        self.registered = True
        self.controller.outputs["SPP"] = self
        self.monitor_task = asyncio.create_task(self.monitor())

    async def monitor(self):
        await self.bus.wait_for_disconnect()
        self.registered = False
        self.disconnect()
        self.controller.publish("BlueZ SPP disconnected; retry SPP:ON after Bluetooth recovers", level="warning")

    async def serve(self):
        loop = asyncio.get_running_loop()
        sock = self.socket
        async def reader():
            while True:
                data = await loop.sock_recv(sock, 256)
                if not data:
                    return
                self.input.feed(data)
        async def writer():
            while True:
                frame = await self.queue.get()
                try:
                    await loop.sock_sendall(sock, frame)
                finally:
                    self.queue.task_done()
        tasks = [asyncio.create_task(reader()), asyncio.create_task(writer())]
        try:
            await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
        finally:
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            self.disconnect(cancel=False)

    def disconnect(self, cancel=True):
        self.ready = False
        if self.socket:
            self.socket.close()
            self.socket = None
        if cancel and self.task:
            self.task.cancel()
        self.clear()
        self.input.buffer.clear()
        self.input.discarding = False

    async def close(self):
        self.disconnect()
        tasks = [task for task in (self.task, self.monitor_task) if task]
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        if self.bus:
            if self.registered:
                try:
                    await asyncio.wait_for(self.manager.call_unregister_profile(PATH), 2)
                except Exception:
                    pass
            self.bus.disconnect()
            self.bus = None
        self.registered = False
