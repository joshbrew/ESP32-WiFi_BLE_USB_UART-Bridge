"""Linux BlueZ GATT peripheral using the ESP32 Nordic UART UUIDs.

Imported only when BLE is enabled. D-Bus annotations are signature strings,
so this module intentionally does not use postponed Python annotations.
"""
import asyncio

from dbus_next import Variant, DBusError
from dbus_next.aio import MessageBus
from dbus_next.constants import BusType, PropertyAccess
from dbus_next.service import ServiceInterface, method, dbus_property

from .streams import BufferedOutput, LineInput


ROOT = "/org/pi_controller"
SERVICE_PATH = ROOT + "/service0"
RX_PATH = SERVICE_PATH + "/char0"
TX_PATH = SERVICE_PATH + "/char1"
AD_PATH = ROOT + "/advertisement0"
SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
READ = PropertyAccess.READ


class UARTService(ServiceInterface):
    def __init__(self):
        super().__init__("org.bluez.GattService1")

    @dbus_property(access=READ)
    def UUID(self) -> "s":
        return SERVICE_UUID

    @dbus_property(access=READ)
    def Primary(self) -> "b":
        return True

    def properties(self):
        return {"UUID": Variant("s", SERVICE_UUID), "Primary": Variant("b", True)}


class Characteristic(ServiceInterface):
    def __init__(self, transport, tx):
        super().__init__("org.bluez.GattCharacteristic1")
        self.transport, self.tx = transport, tx
        self.value = b""
        self.inputs = {}

    @dbus_property(access=READ)
    def UUID(self) -> "s":
        return TX_UUID if self.tx else RX_UUID

    @dbus_property(access=READ)
    def Service(self) -> "o":
        return SERVICE_PATH

    @dbus_property(access=READ)
    def Flags(self) -> "as":
        return ["notify"] if self.tx else ["write", "write-without-response"]

    @dbus_property(access=READ)
    def Value(self) -> "ay":
        return self.value

    @dbus_property(access=READ)
    def Notifying(self) -> "b":
        return self.tx and self.transport.ready

    @method()
    def WriteValue(self, value: "ay", options: "a{sv}"):
        if self.tx:
            raise DBusError("org.bluez.Error.NotPermitted", "TX is notify only")
        if options.get("offset", Variant("q", 0)).value:
            raise DBusError("org.bluez.Error.InvalidOffset", "offset writes unsupported")
        device = options.get("device", Variant("o", "/")).value
        if device not in self.inputs:
            if len(self.inputs) >= 4:
                raise DBusError("org.bluez.Error.InProgress", "too many input streams")
            self.inputs[device] = LineInput(self.transport.controller, "BLE", self.transport.send)
        self.inputs[device].feed(value)

    @method()
    def StartNotify(self):
        if not self.tx:
            raise DBusError("org.bluez.Error.NotSupported", "RX has no notifications")
        self.transport.ready = True
        self.emit_properties_changed({"Notifying": True})

    @method()
    def StopNotify(self):
        if self.tx:
            self.transport.ready = False
            self.transport.clear()
            self.transport.rx.inputs.clear()
            self.emit_properties_changed({"Notifying": False})

    def properties(self):
        return {"UUID": Variant("s", self.UUID), "Service": Variant("o", SERVICE_PATH),
                "Flags": Variant("as", self.Flags), "Value": Variant("ay", self.value),
                "Notifying": Variant("b", self.Notifying)}


class Application(ServiceInterface):
    def __init__(self, service, rx, tx):
        super().__init__("org.freedesktop.DBus.ObjectManager")
        self.objects = {SERVICE_PATH: service, RX_PATH: rx, TX_PATH: tx}

    @method()
    def GetManagedObjects(self) -> "a{oa{sa{sv}}}":
        return {path: {interface.name: interface.properties()} for path, interface in self.objects.items()}


class Advertisement(ServiceInterface):
    def __init__(self, transport):
        super().__init__("org.bluez.LEAdvertisement1")
        self.transport = transport

    @dbus_property(access=READ)
    def Type(self) -> "s":
        return "peripheral"

    @dbus_property(access=READ)
    def ServiceUUIDs(self) -> "as":
        return [SERVICE_UUID]

    @dbus_property(access=READ)
    def LocalName(self) -> "s":
        return self.transport.controller.config["ble_name"]

    @method()
    def Release(self):
        self.transport.ready = False
        self.transport.controller.ble_running = False
        self.transport.controller.ble_error = "BlueZ released advertising; restart the controller service"


class BLETransport(BufferedOutput):
    def __init__(self, controller):
        super().__init__()
        self.controller = controller
        self.bus = None
        self.tasks = []
        self.gatt = self.ads = None
        self.gatt_registered = self.ad_registered = False
        self.rx, self.tx = Characteristic(self, False), Characteristic(self, True)

    async def start(self):
        self.bus = await MessageBus(bus_type=BusType.SYSTEM).connect()
        adapter = "/org/bluez/" + self.controller.config["ble_adapter"]
        node = await self.bus.introspect("org.bluez", adapter)
        proxy = self.bus.get_proxy_object("org.bluez", adapter, node)
        # Do not power adapters or reconfigure the host's Bluetooth policy here.
        props = proxy.get_interface("org.freedesktop.DBus.Properties")
        if not (await props.call_get("org.bluez.Adapter1", "Powered")).value:
            raise RuntimeError("Bluetooth adapter is off; run bluetoothctl power on")
        self.gatt = proxy.get_interface("org.bluez.GattManager1")
        self.ads = proxy.get_interface("org.bluez.LEAdvertisingManager1")
        service = UARTService()
        self.bus.export(ROOT, Application(service, self.rx, self.tx))
        self.bus.export(SERVICE_PATH, service)
        self.bus.export(RX_PATH, self.rx)
        self.bus.export(TX_PATH, self.tx)
        self.bus.export(AD_PATH, Advertisement(self))
        await self.gatt.call_register_application(ROOT, {})
        self.gatt_registered = True
        await self.ads.call_register_advertisement(AD_PATH, {})
        self.ad_registered = True
        self.controller.outputs["BLE"] = self
        self.controller.ble_running = True
        self.tasks = [asyncio.create_task(self.notify()), asyncio.create_task(self.monitor())]

    async def monitor(self):
        await self.bus.wait_for_disconnect()
        self.ready = False
        self.clear()
        self.controller.ble_running = False
        self.controller.ble_error = "D-Bus disconnected; restart the controller service"
        self.controller.publish(self.controller.ble_error, level="error")

    async def notify(self):
        while True:
            frame = await self.queue.get()
            try:
                for offset in range(0, len(frame), 20):
                    if not self.ready:
                        break
                    self.tx.value = frame[offset:offset + 20]
                    self.tx.emit_properties_changed({"Value": self.tx.value})
                    await asyncio.sleep(0.01)
            finally:
                self.queue.task_done()

    async def close(self):
        self.ready = False
        self.controller.ble_running = False
        for task in self.tasks:
            task.cancel()
        await asyncio.gather(*self.tasks, return_exceptions=True)
        if self.bus:
            for registered, manager, path, call in (
                (self.ad_registered, self.ads, AD_PATH, "call_unregister_advertisement"),
                (self.gatt_registered, self.gatt, ROOT, "call_unregister_application"),
            ):
                if registered:
                    try:
                        await asyncio.wait_for(getattr(manager, call)(path), 2)
                    except Exception:
                        pass
            self.bus.disconnect()
