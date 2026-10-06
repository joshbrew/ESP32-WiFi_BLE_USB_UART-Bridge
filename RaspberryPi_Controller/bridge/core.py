import asyncio
import copy
import json
import time
from collections import deque, OrderedDict

from . import VERSION
from .dispenser import Dispenser, validate_settings
from .hardware import PiGPIO, SimulatedGPIO
from .routines import Routines, name_key, number
from .store import Store
from .features import Features
from .geo import GeoMission
from .config import UINT32_MAX


HELP = """Ping Help Status ConfigRead USBStatus HeapStatus BLEStatus RadioStatus WiFiStatus SendStatus
Arm Disarm Dispense:ms DispenseStop DispenserStatus PayloadStatus StopAll
DispenserPin:BCM DispenserActiveHigh:ON|OFF DispenserDefaultPulse:ms
DispenserMaxPulse:ms DispenserArmTimeout:ms DispenserSave DispenserDefaults DispenserErase
PayloadProfileList PayloadProfileShow:name PayloadProfileSave:name PayloadProfileUse:name
PayloadProfileDelete:name PayloadProfileEraseAll
RoutineCreate:name RoutineAdd:name:DISPENSE:ms RoutineAdd:name:WAIT:ms
RoutineAdd:name:START_WAIT:ms
RoutineAdd:name:WAIT_IDLE RoutineAdd:name:COMMAND:Dispense:ms RoutineRepeat:name:count
RoutineSave:name RoutineRun:name RoutineStop RoutineStatus RoutineList RoutineShow:name RoutineErase:name
Send:payload SendBLE:payload SendWiFi:payload SendUSB:payload SendSerial:payload SendUART:payload
SendSPP:payload GeoSource:API|MAVLINK GeoClear GeoAdd:lat,lon,radius,routine GeoSave GeoLoad
GeoStart GeoStop GeoStatus GeoList GeoResetPosition GeoPosition:lat,lon,accuracy,ageMs
SelfTestStart SelfTestStatus SelfTestClear
ProductionMode DebugMode BootModeStatus Reboot IndicatorStatus IndicatorTest IndicatorConnectionTest IndicatorActivityTest
SelfTestResume SelfTestAbort ModeWiFi ModeWiFiBLE ModeWiFiBLEP ModeBLE ModeBTSerial ModeUSB RadioBoot:profile
WiFi:ON|OFF BLE:ON|OFF ClassicBT:ON|OFF SPP:ON|OFF BleAdvertise WebRestart
WiFiMode:AP|STA|APSTA WiFiFallbackAP:ON|OFF WiFiTxPower:LOW|MAX|dBm WiFiStaSSID:ssid WiFiStaPassword:password
WiFiStaClear WiFiApSSID:ssid WiFiApPassword:password ConfigApply ConfigSave ConfigLoad ConfigDefaults ConfigErase
Advanced profile: RPM:rpm,steps,direction DEG:rpm,degrees,direction Stop CoilsOff GetMotorStats
MoveFullCW MoveFullCCW MoveHalfCW MoveHalfCCW SetRevSteps:n SetMinRPM:n SetMaxRPM:n SetStartRPM:n SetRampRPM:n
SetMinStepIntervalUs:n StepMode:4|8 HoldTorque:0|1 PrintStepOrder NextStepOrder StepOrder:0123
DACStatus DACRefMV:n DAC1:MV:n DAC1:ON|OFF|TEST3S DACSave DACLoad DACDefaults DACErase
Browser updates use checksummed Pi .zip bundles; see README."""


class AdmissionError(ValueError):
    def __init__(self, message, status=400):
        super().__init__(message)
        self.status = status


class Controller:
    def __init__(self, config, gpio=None, clock=time.monotonic, watchdog=True, gpio_factory=None):
        self.config, self.clock = config, clock
        self.started = clock()
        self.store = Store(config["data_dir"])
        self.dispenser = None
        self.features = None
        try:
            self.saved = self.store.load()
            self.profiles = self.saved["profiles"]
            if not isinstance(self.profiles, dict) or len(self.profiles) > 4:
                raise ValueError("invalid payload profile library")
            for key, settings in self.profiles.items():
                if name_key(key) != key:
                    raise ValueError("invalid payload profile key")
                validate_settings(config, settings)
            if self.saved["settings"] is not None:
                validate_settings(config, self.saved["settings"])
            self.selected = self.saved["selected"]
            if not isinstance(self.selected, str) or self.selected and self.selected not in self.profiles:
                raise ValueError("invalid saved profile selection")
            settings = self.profiles[self.selected] if self.selected else self.saved["settings"]
            # Resolve saved pin/polarity before initializing real GPIO. Driving
            # compiled defaults first could activate a saved active-low circuit.
            boot_config = dict(config)
            if settings:
                boot_config.update(dispenser_pin=settings["pin"], active_high=settings["activeHigh"])
            if gpio is None:
                factory = gpio_factory or (SimulatedGPIO if config["simulate"] or config["hardware_profile"] != "dispenser" else PiGPIO)
                gpio = factory(boot_config)
            elif settings:
                gpio.configure(settings["pin"], settings["activeHigh"])
            self.dispenser = Dispenser(config, gpio, clock, watchdog)
            if settings:
                self.dispenser.settings = dict(settings)
            self.features = Features(self)
            self.routines = Routines(self.dispenser, self.saved["routines"], self.publish, clock, self.advanced)
            self.geo = GeoMission(self)
            self.dispenser.safety_permit = self.geo.permit_output
        except Exception:
            if self.features:
                self.features.close_hardware()
            if self.dispenser:
                self.dispenser.close()
            elif gpio:
                gpio.close()
            raise
        self.events = deque(maxlen=128)
        self.latest = 0
        self.queue = asyncio.Queue(maxsize=8)
        self.history = OrderedDict()
        self.outputs = {}
        self.http_seen = None
        self.ble_running = False
        self.ble_error = ""
        self.tasks = []
        self.publish("controller ready; output disarmed" + (" (SIMULATION)" if config["simulate"] else ""))

    def publish(self, text, source="Internal", request_id="", level="info", raw=False, destinations=None):
        self.latest += 1
        event = dict(i=self.latest, t=str(text)[:4096], s=source, r=request_id, l=level, q=raw)
        # Explicit Send payloads for USB/UART/BLE stay out of the browser log.
        # Ordinary command responses remain visible to the browser like ESP32.
        if destinations is None or "WiFi" in destinations:
            self.events.append(event)
        targets = destinations if destinations is not None else {source, "USB", "UART"}
        line = event["t"] if raw else f"[{self.latest}][{level.upper()}][{source}]" + (f"[{request_id}]" if request_id else "") + " " + event["t"]
        for target in targets:
            output = self.outputs.get(target)
            if output:
                output.send(line + "\n")

    def available(self):
        return {**{key: output.ready for key, output in self.outputs.items()},
                "WiFi": self.radio.wifi_enabled and self.http_seen is not None and self.clock() - self.http_seen < 15}

    def state(self, include_geo_points=True):
        available = self.available()
        payload = self.dispenser.state()
        payload["profile"] = self.selected or "standalone"
        advanced = self.advanced.state() if self.advanced else {}
        return dict(ok=True, firmware="Raspberry Pi Modular Controller", version=VERSION,
                    uptimeMs=int((self.clock() - self.started) * 1000), latestEventId=self.latest,
                    simulate=self.config["simulate"], administrationBusy=self.features.busy,
                    bootMode="production" if self.features.system["production"] else "debug",
                    addon=dict(name="pi-" + self.config["hardware_profile"], active=self.config["hardware_profile"] != "none",
                               dispenser=self.config["hardware_profile"] == "dispenser", stepper=bool(self.advanced), dac=bool(self.advanced)),
                    dispenser=payload, payloadProfiles=self.profiles, routineLibrary=self.routines.library,
                    savedRoutineNames=sorted(self.saved["routines"]), savedRoutineLibrary=self.saved["routines"], routine=self.routines.state(), geo=self.geo.state(include_geo_points),
                    queue=dict(waiting=self.queue.qsize(), capacity=8),
                    send=dict(wifi=available["WiFi"], ble=available.get("BLE", False),
                              usb=available.get("USB", False), uart=available.get("UART", False), spp=available.get("SPP", False)),
                    radio=self.radio.state(), indicators=self.indicators.state(), memory=self.features.memory(),
                    update=self.updates.state(), selfTest=self.self_tests.state(), **advanced)

    def clear_queue(self):
        while not self.queue.empty():
            self.queue.get_nowait()
            self.queue.task_done()

    def stop_all(self, reason="stopped", abort_test=True):
        self.geo.cancel(reason)
        self.routines.stop(reason)
        if abort_test and self.self_tests.active:
            self.self_tests.abort()

    def event_page(self, since, limit):
        # Also reset a cursor from a prior process after the service restarts.
        gap = since > self.latest or bool(self.events and since < self.events[0]["i"] - 1)
        if since > self.latest:
            since = 0
        matches = [item for item in self.events if item["i"] > since]
        page = matches[:limit]
        cursor = page[-1]["i"] if page else self.latest
        return dict(ok=True, events=page, cursor=cursor, more=len(matches) > limit, gap=gap)

    def submit(self, body, source="WiFi", request_id=""):
        if len(body.encode("utf-8")) > 2048 or "\0" in body:
            raise AdmissionError("command body exceeds 2048 bytes or contains NUL")
        lines = [line.strip() for line in body.splitlines() if line.strip()]
        if not lines or len(lines) > 8 or any(len(line.encode("utf-8")) > 256 for line in lines):
            raise AdmissionError("submit 1-8 commands, each at most 256 bytes")
        if len(request_id) > 64 or any(ord(char) < 33 or ord(char) > 126 for char in request_id):
            raise AdmissionError("invalid request ID")
        now = self.clock()
        for key in list(self.history):
            if now - self.history[key][0] > 30:
                del self.history[key]
        key = (source, request_id)
        if request_id and key in self.history:
            _, old_body, result = self.history[key]
            if body != old_body:
                raise AdmissionError("request ID was already used for another body", 409)
            return dict(result, duplicate=True)
        # A stop in a batch is an immediate queue barrier. The remainder of that
        # batch cannot rearm the hardware after the stop.
        stops = {"STOPALL", "DISARM", "DISPENSERDISARM", "ROUTINESTOP", "GEOSTOP", "DISPENSESTOP", "DISPENSEROFF", "GPIO26:OFF"}
        if self.advanced:
            stops.update({"STOP", "COILSOFF", "DAC1:OFF", "DACALL:OFF", "OUTPUTALL:OFF"})
        stopping = next((line for line in lines if line.upper() in stops), None)
        if request_id and len(self.history) >= 64 and not stopping:
            # Preserve every accepted ID for its full retry window; evicting an
            # unexpired pulse ID could turn a delayed retry into another pulse.
            raise AdmissionError("request history busy; retry after 30 seconds", 503)
        if stopping:
            self.clear_queue()
            self.stop_all(stopping)
            if stopping.upper() in {"DISARM", "DISPENSERDISARM"}:
                self.dispenser.disarm()
            lines = [stopping]
            self.publish("[DONE] stopped and disarmed; pending commands cleared", source, request_id, "warning")
        elif self.queue.qsize() + len(lines) > self.queue.maxsize:
            raise AdmissionError("command queue busy", 503)
        before = self.latest - 1 if stopping else self.latest
        if not stopping:
            for line in lines:
                self.queue.put_nowait((line, source, request_id))
        result = dict(ok=True, accepted=True, acceptedLines=len(lines), requestId=request_id,
                      latestEventId=before, duplicate=False)
        if request_id and len(self.history) < 64:
            self.history[key] = (now, body, result)
        return result

    def persist(self):
        candidate = copy.deepcopy(self.saved)
        candidate.update(profiles=self.profiles, selected=self.selected)
        self.store.save(candidate)
        self.saved = candidate

    def execute(self, line, source="WiFi", request_id=""):
        command, _, value = line.partition(":")
        cmd = command.upper()
        self.indicators.note_activity()
        safe_commands = {"PING", "HELP", "STATUS", "CONFIGREAD", "BOOTMODESTATUS", "RADIOSTATUS", "USBSTATUS", "HEAPSTATUS", "BLESTATUS", "SENDSTATUS", "INDICATORSTATUS", "ROUTINESTATUS", "DISPENSERSTATUS", "PAYLOADSTATUS", "SELFTESTSTATUS", "SELFTESTABORT", "STOPALL", "DISARM", "ROUTINESTOP", "COILSOFF", "STOP", "GEOSTATUS", "GEOLIST", "GEOSTOP", "GEOPOSITION", "WIFISTATUS"}
        if self.features.busy and cmd not in safe_commands:
            raise ValueError("administrative operation active; actuation/configuration blocked")
        if self.geo.active and cmd not in safe_commands and cmd not in {"ROUTINELIST", "ROUTINESHOW", "HELP", "DISPENSESTOP", "DISPENSEROFF", "DISPENSERDISARM"} and line.upper() != "GPIO26:OFF":
            raise ValueError("stop coordinate sequence before manual actuation or configuration")
        if cmd == "WIFILR" or (cmd in {"WIFIMODE", "RADIOBOOT"} and value.upper() == "LRONLY"):
            raise ValueError("ESP32 proprietary LR is unavailable on Pi; use an ESP32 LR relay through BLE or serial")
        if self.geo.handle(cmd, value):
            if cmd == "GEOLIST":
                message = json.dumps(self.geo.page(number(value, 500, 0) if value else 0), separators=(",", ":"))
            elif cmd == "GEOSTATUS": message = json.dumps(self.geo.state(False), separators=(",", ":"))
            else: message = f"[ACK] {cmd} points={len(self.geo.plan['points'])} saved={self.geo.saved}"
            self.publish(message, source, request_id, "status")
            return
        if self.features.handle(line, source, request_id):
            return
        if self.config["hardware_profile"] != "dispenser" and (cmd in {"ARM", "DISARM", "DISPENSE", "DISPENSESTOP"} or cmd.startswith("DISPENSER") or cmd.startswith("PAYLOADPROFILE")) and cmd not in {"DISPENSERSTATUS"}:
            raise ValueError("dispenser command requires the dispenser hardware profile")
        message = "[ACK] " + command
        if cmd == "PING":
            message = "PONG"
        elif cmd == "HELP":
            message = HELP
        elif cmd in {"STATUS", "CONFIGREAD", "DISPENSERSTATUS", "PAYLOADSTATUS", "ROUTINESTATUS", "RADIOSTATUS", "WIFISTATUS", "BLESTATUS", "SENDSTATUS", "USBSTATUS", "HEAPSTATUS"}:
            message = json.dumps(self.state(), separators=(",", ":"))
        elif cmd in {"STOPALL", "DISARM", "DISPENSERDISARM", "ROUTINESTOP"}:
            self.stop_all(command)
            self.dispenser.disarm()
        elif cmd in {"DISPENSESTOP", "DISPENSEROFF"} or line.upper() == "GPIO26:OFF":
            self.stop_all("manual stop")
        elif cmd in {"ARM", "DISPENSERARM", "DISPENSE"} or line.upper() == "GPIO26:ON":
            if self.config["hardware_profile"] != "dispenser":
                raise ValueError("output command requires an active hardware profile")
            self.routines.require_idle()
            if cmd in {"ARM", "DISPENSERARM"}:
                self.dispenser.arm()
            else:
                duration = self.dispenser.settings["defaultPulseMs"] if cmd == "GPIO26" else number(value)
                self.dispenser.dispense(duration)
        elif cmd.startswith("DISPENSER"):
            self.routines.require_idle()
            self.dispenser.require_disarmed()
            fields = {"DISPENSERPIN": "pin", "DISPENSERACTIVEHIGH": "activeHigh",
                      "DISPENSERDEFAULTPULSE": "defaultPulseMs", "DISPENSERMAXPULSE": "maxPulseMs",
                      "DISPENSERARMTIMEOUT": "armTimeoutMs"}
            if cmd in fields:
                settings = dict(self.dispenser.settings)
                if cmd == "DISPENSERACTIVEHIGH":
                    if value.upper() not in {"ON", "OFF"}:
                        raise ValueError("polarity requires ON or OFF")
                    settings[fields[cmd]] = value.upper() == "ON"
                else:
                    settings[fields[cmd]] = number(value, UINT32_MAX, 0 if cmd in {"DISPENSERMAXPULSE", "DISPENSERARMTIMEOUT"} else 1)
                self.dispenser.apply(settings)
            elif cmd == "DISPENSERSAVE":
                self.saved["settings"] = dict(self.dispenser.settings)
                self.selected = ""
                self.persist()
            elif cmd == "DISPENSERDEFAULTS":
                self.dispenser.apply(self.dispenser.defaults)
            elif cmd == "DISPENSERERASE":
                self.saved["settings"] = None
                self.selected = ""
                self.persist()
            else:
                raise ValueError("unknown dispenser command; Help")
        elif cmd.startswith("PAYLOADPROFILE"):
            if cmd == "PAYLOADPROFILELIST":
                message = json.dumps(self.profiles)
            elif cmd == "PAYLOADPROFILESHOW":
                message = json.dumps(self.profiles[name_key(value)])
            else:
                self.routines.require_idle()
                self.dispenser.require_disarmed()
                if cmd == "PAYLOADPROFILEERASEALL":
                    self.profiles = {}
                    self.selected = ""
                else:
                    key = name_key(value)
                    if cmd == "PAYLOADPROFILESAVE":
                        if key not in self.profiles and len(self.profiles) >= 4:
                            raise ValueError("profile library is full")
                        self.profiles[key] = dict(self.dispenser.settings)
                        self.selected = key
                    elif cmd == "PAYLOADPROFILEUSE":
                        self.dispenser.apply(self.profiles[key])
                        self.selected = key
                    elif cmd == "PAYLOADPROFILEDELETE":
                        del self.profiles[key]
                        if self.selected == key:
                            self.selected = ""
                    else:
                        raise ValueError("unknown payload profile command")
                self.persist()
        elif cmd.startswith("ROUTINE"):
            message = self.routine_command(cmd, value, source, request_id)
        elif cmd in {"SEND", "SENDBLE", "SENDWIFI", "SENDUSB", "SENDSERIAL", "SENDUART", "SENDSPP"}:
            available = self.available()
            mapping = {"SENDBLE": "BLE", "SENDWIFI": "WiFi", "SENDUSB": "USB", "SENDSERIAL": "USB", "SENDUART": "UART", "SENDSPP": "SPP"}
            if cmd == "SEND":
                targets = {key for key, ready in available.items() if ready and key != source}
            else:
                targets = {mapping[cmd]}
            if not value or not targets or any(not available.get(key, False) for key in targets):
                raise ValueError("send destination is unavailable or payload is empty")
            self.publish(value, source, request_id, raw=True, destinations=targets)
        elif cmd == "BLEWEBHANDOFF" or cmd == "BLEWEBCANCEL":
            message = "Linux BLE transport stays registered; no heap handoff needed"
        else:
            raise ValueError("unsupported command on Pi; send Help (OS controls are documented in README)")
        self.publish(message, source, request_id, "status")

    def routine_command(self, cmd, value, source, request_id):
        if cmd == "ROUTINELIST":
            return json.dumps(list(self.routines.library))
        name, _, spec = value.partition(":")
        key = name_key(name)
        if cmd == "ROUTINESHOW":
            return json.dumps(self.routines.library[key])
        self.routines.require_idle()
        if cmd == "ROUTINECREATE":
            if key not in self.routines.library and len(self.routines.library) >= 4:
                raise ValueError("routine library is full")
            self.routines.library[key] = dict(steps=[], repeats=1)
        elif cmd == "ROUTINEADD":
            steps = self.routines.library[key]["steps"]
            if len(steps) >= 10:
                raise ValueError("routine step limit is 10")
            parsed = self.routines.step(spec)
            if parsed.startswith("START_WAIT:") and steps:
                raise ValueError("START_WAIT must be the first step")
            steps.append(parsed)
        elif cmd == "ROUTINEREPEAT":
            self.routines.library[key]["repeats"] = number(spec)
        elif cmd == "ROUTINESAVE":
            record = self.routines.library[key]
            self.routines.validate(record)
            self.saved["routines"][key] = copy.deepcopy(record)
            self.persist()
        elif cmd == "ROUTINEERASE":
            del self.routines.library[key]
            self.saved["routines"].pop(key, None)
            self.persist()
        elif cmd == "ROUTINERUN":
            self.routines.start(key, source, request_id)
        else:
            raise ValueError("unknown routine command")
        return "[ACK] " + cmd + ":" + key

    async def start(self):
        async def worker():
            while True:
                line, source, request_id = await self.queue.get()
                previous = copy.deepcopy((self.saved, self.profiles, self.selected, self.routines.library))
                try:
                    self.execute(line, source, request_id)
                except Exception as error:
                    self.saved, self.profiles, self.selected, self.routines.library = previous
                    # A configuration/storage/driver failure also makes output safe.
                    self.stop_all("command failed")
                    self.clear_queue()
                    self.publish(str(error), source, request_id, "error")
                finally:
                    self.queue.task_done()

        async def ticker():
            while True:
                self.routines.tick()
                self.geo.tick()
                self.indicators.connected = any(self.available().values())
                self.indicators.busy = self.dispenser.state()["dispensing"] or bool(self.routines.running) or bool(self.advanced and self.advanced.remaining)
                await asyncio.sleep(0.01)

        self.tasks = [asyncio.create_task(worker()), asyncio.create_task(ticker())]

    async def close(self):
        self.stop_all("shutdown", abort_test=False)
        self.geo.close()
        await self.features.close()
        for task in self.tasks:
            task.cancel()
        await asyncio.gather(*self.tasks, return_exceptions=True)
        self.dispenser.close()
