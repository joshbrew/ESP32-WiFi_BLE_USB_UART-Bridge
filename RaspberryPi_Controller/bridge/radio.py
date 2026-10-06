import asyncio
import copy
import re

from .store import Store


PROFILES = {
    "WIFI": (True, False, False), "WIFI_BLE": (True, True, False),
    "WIFI_BLE_P": (True, True, False), "BLE": (False, True, False),
    "SPP": (False, False, True), "USB": (False, False, False),
}
DEFAULT_RADIO = dict(wifiMode="STA", fallbackAP=True, staSSID="", staPassword="",
                     apSSID="Pi-Controller", apPassword="pibridgecontrol", txPower="MAX")


def validate_radio(settings):
    if not isinstance(settings, dict) or set(settings) != set(DEFAULT_RADIO):
        raise ValueError("invalid radio settings")
    if settings["wifiMode"] not in {"AP", "STA", "APSTA"} or type(settings["fallbackAP"]) is not bool:
        raise ValueError("invalid Wi-Fi role/fallback")
    for key in ("staSSID", "staPassword", "apSSID", "apPassword"):
        value = settings[key]
        if not isinstance(value, str) or any(ord(char) < 32 or ord(char) == 127 for char in value):
            raise ValueError("Wi-Fi fields must be printable text")
        if len(value.encode()) > (32 if key.endswith("SSID") else 63):
            raise ValueError("SSID/password exceeds Wi-Fi length limit")
    if not settings["apSSID"] or not 8 <= len(settings["apPassword"]) <= 63:
        raise ValueError("AP requires an SSID and an 8-63-character password")
    if settings["staPassword"] and not 8 <= len(settings["staPassword"]) <= 63:
        raise ValueError("station password must be empty or 8-63 characters")
    if settings["txPower"] not in {"LOW", "MAX"}:
        if not isinstance(settings["txPower"], str) or not re.fullmatch(r"(?:[0-9]|[12][0-9]|30)(?:\.[0-9]+)?", settings["txPower"]):
            raise ValueError("Wi-Fi power expects LOW, MAX, or 0-30 decimal dBm")
        try:
            value = float(settings["txPower"])
        except (TypeError, ValueError):
            raise ValueError("Wi-Fi power expects LOW, MAX, or 0-30 dBm") from None
        if not 0 <= value <= 30:
            raise ValueError("Wi-Fi power expects 0-30 dBm")


class RadioManager:
    COMMANDS = {"MODEWIFI", "MODEWIFIBLE", "MODEWIFIBLEP", "MODEBLE", "MODEBTSERIAL", "MODEUSB", "RADIOBOOT",
                "WIFI", "BLE", "CLASSICBT", "SPP", "WIFIMODE", "WIFIFALLBACKAP", "WIFITXPOWER",
                "WIFISTASSID", "WIFISTAPASSWORD", "WIFISTACLEAR", "WIFIAPSSID", "WIFIAPPASSWORD",
                "CONFIGAPPLY", "CONFIGSAVE", "CONFIGLOAD", "CONFIGDEFAULTS", "CONFIGERASE", "WEBRESTART"}

    def __init__(self, controller, admin):
        self.controller, self.admin = controller, admin
        self.store = Store(controller.config["data_dir"], "radio.json")
        initial = "WIFI_BLE" if controller.config["ble"] else "WIFI"
        previously_saved = self.store.path.exists()
        self.record = self.store.load_generic(dict(settings=copy.deepcopy(DEFAULT_RADIO), bootProfile=initial,
                                                  lastGood=initial, pending=None))
        self.validate_record(self.record)
        if self.record["pending"]:
            # A process that never committed its trial restarts in last-known-good.
            self.record["bootProfile"] = self.record["lastGood"]
            self.record["pending"] = None
            self.store.save_generic(self.record)
        self.settings = copy.deepcopy(self.record["settings"])
        self.active = self.record["bootProfile"]
        self.desired = self.active
        self.wifi_enabled, self.ble_enabled, self.spp_enabled = PROFILES[self.active]
        if not previously_saved:
            self.spp_enabled = controller.config["spp"]
        self.role = "OS managed"
        self.error = ""
        self.busy = False
        self.task = None
        self.transport_manager = None
        self.configured = self.store.path.exists()

    def validate_record(self, record):
        if not isinstance(record, dict) or set(record) != {"settings", "bootProfile", "lastGood", "pending"}:
            raise ValueError("invalid saved radio record")
        validate_radio(record["settings"])
        if record["bootProfile"] not in PROFILES or record["lastGood"] not in PROFILES:
            raise ValueError("invalid saved radio profile")
        if record["pending"] is not None and record["pending"] not in PROFILES:
            raise ValueError("invalid pending radio trial")

    def state(self):
        config = self.controller.config
        return dict(wifiCompiled=True, bleCompiled=True, sppCompiled=True,
                    bootModeActive=self.active, bootModeSaved=self.record["bootProfile"], desiredProfile=self.desired,
                    wifiDesired=PROFILES[self.desired][0], bleDesired=PROFILES[self.desired][1], sppDesired=PROFILES[self.desired][2],
                    lastKnownGood=self.record["lastGood"], trialPending=self.record["pending"],
                    wifiEnabled=self.wifi_enabled, bleEnabled=self.ble_enabled, sppEnabled=self.spp_enabled,
                    wifiState=self.role if self.wifi_enabled else "off", wifiMode=self.settings["wifiMode"],
                    fallbackAP=self.settings["fallbackAP"], staSSID=self.settings["staSSID"], apSSID=self.settings["apSSID"],
                    txPower=self.settings["txPower"], apStaConcurrent=config["wifi_interface"] != config["wifi_ap_interface"],
                    bleRunning=self.controller.ble_running, bleError=self.controller.ble_error,
                    sppRunning=bool(self.transport_manager and self.transport_manager.spp),
                    sppConnected=self.controller.available().get("SPP", False),
                    applying=self.busy, lastError=self.error, osControl=config["enable_os_control"])

    def schedule(self, coroutine, source, request_id):
        if self.busy:
            coroutine.close()
            raise ValueError("radio operation already active")
        self.controller.stop_all("radio transition")
        self.controller.clear_queue()
        self.busy = True
        async def run():
            try:
                await coroutine
                self.controller.publish("[DONE] radio operation complete", source, request_id, "status")
                self.error = ""
            except Exception as error:
                self.error = str(error)
                self.controller.publish(self.error, source, request_id, "error")
            finally:
                self.busy = False
        self.task = asyncio.create_task(run())

    async def apply(self, profile=None):
        selected = profile or self.desired
        if selected not in PROFILES:
            raise ValueError("unknown radio profile")
        wifi, ble, spp = PROFILES[selected]
        if not wifi and not (ble or spp) and not (self.controller.config["usb_device"] or self.controller.config["uart_device"] or self.controller.config["simulate"]):
            raise ValueError("USB profile requires a configured serial transport")
        old = (self.active, self.wifi_enabled, self.ble_enabled, self.spp_enabled, self.role)
        old_record = copy.deepcopy(self.record)
        trial = copy.deepcopy(self.record)
        trial["pending"] = selected
        self.store.save_generic(trial)
        self.record = trial
        try:
            if self.transport_manager:
                await self.transport_manager.configure(ble, spp)
            elif (ble or spp) and not self.controller.config["simulate"]:
                raise ValueError("transport manager is not ready")
            result = await self.admin.run("radio", settings=self.settings, wifi=wifi)
            if not wifi and ble and not self.controller.config["simulate"] and not self.controller.ble_running:
                raise ValueError("BLE profile did not start")
            if not wifi and spp and not self.controller.config["simulate"] and not self.transport_manager.spp:
                raise ValueError("SPP profile did not start")
            self.active, self.wifi_enabled, self.ble_enabled, self.spp_enabled = selected, wifi, ble, spp
            self.role = result.get("role", "active")
            await asyncio.sleep(.1 if self.controller.config["simulate"] else 2)
            self.record.update(pending=None, bootProfile=selected, lastGood=selected, settings=copy.deepcopy(self.settings))
            self.store.save_generic(self.record)
            await self.admin.run("radioCommit")
            self.desired = selected
        except BaseException:
            self.active, self.wifi_enabled, self.ble_enabled, self.spp_enabled, self.role = old
            self.record = old_record
            self.store.save_generic(old_record)
            try:
                await self.admin.run("radioRollback")
            except Exception:
                self.error = "network rollback pending in OS watchdog"
            if self.transport_manager:
                await self.transport_manager.configure(old[2], old[3])
            # The root helper restores the network snapshot if apply fails.
            raise

    def handle(self, cmd, value, source, request_id):
        if cmd not in self.COMMANDS:
            return False
        if self.busy:
            raise ValueError("radio operation active")
        modes = {"MODEWIFI": "WIFI", "MODEWIFIBLE": "WIFI_BLE", "MODEWIFIBLEP": "WIFI_BLE_P", "MODEBLE": "BLE", "MODEBTSERIAL": "SPP", "MODEUSB": "USB"}
        if cmd in modes:
            self.schedule(self.apply(modes[cmd]), source, request_id)
        elif cmd == "RADIOBOOT":
            profile = value.upper()
            if profile not in PROFILES:
                raise ValueError("unknown boot radio profile")
            candidate = dict(self.record, bootProfile=profile)
            self.store.save_generic(candidate)
            self.record = candidate
            self.desired = profile
        elif cmd in {"WIFI", "BLE", "CLASSICBT", "SPP"}:
            if value.upper() not in {"ON", "OFF"}:
                raise ValueError("radio setting expects ON or OFF")
            wifi, ble, spp = PROFILES[self.desired]
            enabled = value.upper() == "ON"
            if cmd == "WIFI":
                wifi = enabled
                if enabled: spp = False
            elif cmd == "BLE":
                ble = enabled
                if enabled: spp = False
            else:
                spp = enabled
                if enabled: wifi = ble = False
            self.desired = "SPP" if spp else "WIFI_BLE_P" if wifi and ble and self.desired == "WIFI_BLE_P" else "WIFI_BLE" if wifi and ble else "WIFI" if wifi else "BLE" if ble else "USB"
        elif cmd == "CONFIGAPPLY":
            self.schedule(self.apply(), source, request_id)
        elif cmd == "WEBRESTART":
            self.controller.features.schedule_admin("restart", source, request_id)
        elif cmd in {"CONFIGSAVE", "CONFIGLOAD", "CONFIGDEFAULTS", "CONFIGERASE"}:
            if cmd == "CONFIGSAVE":
                validate_radio(self.settings)
                candidate = dict(self.record, settings=copy.deepcopy(self.settings), bootProfile=self.desired)
                self.store.save_generic(candidate); self.record = candidate
            elif cmd == "CONFIGLOAD":
                record = self.store.load_generic(self.record)
                self.validate_record(record)
                self.record = record; self.settings = copy.deepcopy(record["settings"]); self.desired = record["bootProfile"]
            else:
                self.settings = copy.deepcopy(DEFAULT_RADIO)
                self.desired = "WIFI"
                if cmd == "CONFIGERASE":
                    self.record.update(settings=copy.deepcopy(self.settings), bootProfile="WIFI", lastGood="WIFI", pending=None)
                    self.store.path.unlink(missing_ok=True)
        else:
            settings = copy.deepcopy(self.settings)
            fields = {"WIFIMODE": "wifiMode", "WIFIFALLBACKAP": "fallbackAP", "WIFITXPOWER": "txPower",
                      "WIFISTASSID": "staSSID", "WIFISTAPASSWORD": "staPassword", "WIFIAPSSID": "apSSID", "WIFIAPPASSWORD": "apPassword"}
            if cmd == "WIFISTACLEAR":
                settings.update(staSSID="", staPassword="")
            elif cmd == "WIFIFALLBACKAP":
                if value.upper() not in {"ON", "OFF"}: raise ValueError("fallback expects ON or OFF")
                settings["fallbackAP"] = value.upper() == "ON"
            else:
                settings[fields[cmd]] = value.upper() if cmd in {"WIFIMODE", "WIFITXPOWER"} else value
            # Permit sequential credential editing; enforce full validation at save/apply.
            if cmd not in {"WIFISTAPASSWORD", "WIFIAPPASSWORD"}:
                validate_radio(settings)
            elif any(ord(char) < 32 for char in value) or len(value.encode()) > 63:
                raise ValueError("invalid password")
            self.settings = settings
        return True

    async def runtime(self, wifi, ble, spp):
        old = (self.wifi_enabled, self.ble_enabled, self.spp_enabled, self.role)
        try:
            if not wifi and not ble and not spp and not self.controller.config["simulate"] and not (self.controller.config["usb_device"] or self.controller.config["uart_device"]):
                raise ValueError("turning off all radios requires a serial recovery transport")
            if self.transport_manager:
                await self.transport_manager.configure(ble, spp)
            result = await self.admin.run("radio", settings=self.settings, wifi=wifi)
            self.wifi_enabled, self.ble_enabled, self.spp_enabled = wifi, ble, spp
            self.role = result.get("role", "active")
            await self.admin.run("radioCommit")
        except BaseException:
            self.wifi_enabled, self.ble_enabled, self.spp_enabled, self.role = old
            try:
                await self.admin.run("radioRollback")
            except Exception:
                pass
            if self.transport_manager:
                await self.transport_manager.configure(old[1], old[2])
            raise

    async def close(self):
        if self.task and not self.task.done():
            self.task.cancel()
            await asyncio.gather(self.task, return_exceptions=True)
