"""Ordered coordinate triggers using saved dispenser routines and fresh fixes."""
import asyncio
import copy
import json
import math
import re

from .mavlink import positions, test_position
from .routines import name_key, number
from .store import Store


TIMEOUT = 3.0
MAX_POINTS = 500


def decimal(text, low, high):
    if not isinstance(text, str) or not 1 <= len(text.strip()) <= 32 or not re.fullmatch(r"[+\-0-9.eE]+", text.strip()):
        raise ValueError("invalid coordinate number")
    value = float(text)
    if not math.isfinite(value) or not low <= value <= high:
        raise ValueError("coordinate number outside permitted range")
    return value


def point(spec):
    fields = spec.split(",")
    if len(fields) != 4: raise ValueError("point needs latitude,longitude,radiusMeters,savedRoutineName")
    return dict(latitude=decimal(fields[0], -90, 90), longitude=decimal(fields[1], -180, 180),
                radiusMeters=decimal(fields[2], .1, 1000), routine=name_key(fields[3].strip()))


class GeoMission:
    COMMANDS = {"GEOCLEAR", "GEOSOURCE", "GEOADD", "GEOSAVE", "GEOLOAD", "GEOLIST", "GEOSTATUS", "GEOSTART", "GEOSTOP", "GEORESETPOSITION", "GEOPOSITION", "GEOTESTPOSITION"}

    def __init__(self, controller):
        self.controller, self.clock = controller, controller.clock
        self.store = Store(controller.config["data_dir"], "geo.json", max_bytes=262144)
        self.plan = self.store.load_generic(dict(source="MAVLINK", points=[]))
        self.validate(self.plan)
        self.saved = self.store.path.exists()
        self.active = self.running = False
        self.next = 0
        self.result = "idle"
        self.transport = None
        self.udp_error = ""
        self.reset_position()

    def validate(self, plan):
        if not isinstance(plan, dict) or set(plan) != {"source", "points"} or plan["source"] not in {"API", "MAVLINK"}:
            raise ValueError("invalid coordinate plan")
        if not isinstance(plan["points"], list) or len(plan["points"]) > MAX_POINTS:
            raise ValueError(f"coordinate plan has more than {MAX_POINTS} points")
        for item in plan["points"]:
            if not isinstance(item, dict) or set(item) != {"latitude", "longitude", "radiusMeters", "routine"}:
                raise ValueError("invalid coordinate point")
            if any(type(item[key]) not in (int, float) for key in ("latitude", "longitude", "radiusMeters")):
                raise ValueError("invalid coordinate point numbers")
            parsed = point(f"{item['latitude']},{item['longitude']},{item['radiusMeters']},{item['routine']}")
            if parsed != item: raise ValueError("invalid saved coordinate point")

    def reset_position(self):
        self.fix = self.mailbox = None
        self.guard_invalid = False
        self.gps = None
        self.boot_ms = None
        self.test_boot_ms = None

    def send_test_position(self, body):
        if self.plan["source"] != "MAVLINK" or not self.controller.radio.wifi_enabled:
            raise ValueError("test GPS needs MAVLink source and enabled Wi-Fi")
        fields = body.split(",")
        if len(body.encode()) > 128 or len(fields) != 3:
            raise ValueError("test GPS needs latitude,longitude,accuracyMeters")
        lat, lon = decimal(fields[0], -90, 90), decimal(fields[1], -180, 180)
        accuracy = decimal(fields[2], -1, 100000)
        if -1 < accuracy < 0: raise ValueError("unknown accuracy must be -1")
        if self.transport is None or self.transport.is_closing():
            raise ValueError("test GPS needs an active UDP receiver")
        base = self.boot_ms if self.boot_ms is not None else int(self.clock() * 1000) & 0xffffffff
        if self.test_boot_ms is not None and 0 <= (self.test_boot_ms - base) % (1 << 32) < (1 << 31):
            base = self.test_boot_ms
        self.test_boot_ms = (base + 1) & 0xffffffff
        config = self.controller.config
        packet = test_position(lat, lon, accuracy, self.test_boot_ms, config["geo_system_id"], config["geo_component_id"])
        address = self.transport.get_extra_info("sockname")
        host = "127.0.0.1" if address[0] == "0.0.0.0" else "::1" if address[0] == "::" else address[0]
        # Only the ordinary CRC/ID/fix-quality UDP receiver may publish the fix.
        self.transport.sendto(packet, (host, address[1]))

    def fresh(self):
        # Read-only immutable snapshots also used by the dispenser watchdog.
        # Never acquire another lock from inside its GPIO deadline lock.
        fix = self.fix
        if self.guard_invalid or fix is None or self.clock() - fix[3] + fix[4] > TIMEOUT:
            return False
        gps = self.gps
        return self.plan["source"] == "API" or bool(gps and 3 <= gps[0] <= 6 and self.clock() - gps[2] <= TIMEOUT)

    def permit_output(self):
        return not self.active or self.fresh()

    def submit_position(self, body):
        if self.plan["source"] != "API": raise ValueError("select GeoSource:API first")
        received = self.clock()
        try:
            if len(body.encode()) > 128: raise ValueError("position body exceeds 128 bytes")
            fields = body.split(",")
            if len(fields) != 4: raise ValueError("position needs latitude,longitude,accuracyMeters,ageMs")
            lat, lon = decimal(fields[0], -90, 90), decimal(fields[1], -180, 180)
            accuracy = decimal(fields[2], -1, 100000)
            if -1 < accuracy < 0: raise ValueError("unknown accuracy must be -1")
            age = number(fields[3].strip(), 3000, 0) / 1000
        except (ValueError, UnicodeError):
            self.mailbox = (None, received)
            self.guard_invalid = True
            raise
        self.mailbox = ((lat, lon, accuracy, received, age), received)
        return True

    def receive_mavlink(self, data):
        c = self.controller
        if self.plan["source"] != "MAVLINK" or not c.radio.wifi_enabled: return
        now = self.clock()
        for msg in positions(data):
            if (msg["system"], msg["component"]) != (c.config["geo_system_id"], c.config["geo_component_id"]): continue
            if msg["id"] == 24:
                self.gps = (msg["fixType"], msg["accuracyMm"] / 1000 if msg["accuracyMm"] else -1, now)
                if not 3 <= msg["fixType"] <= 6:
                    self.fix = None
                    self.guard_invalid = True
            elif self.gps and 3 <= self.gps[0] <= 6 and now - self.gps[2] <= TIMEOUT:
                boot = msg["bootMs"]
                if self.boot_ms is not None and not 0 < (boot - self.boot_ms) % (1 << 32) < (1 << 31): continue
                if not (-90 <= msg["latitude"] <= 90 and -180 <= msg["longitude"] <= 180):
                    self.fix = None; self.guard_invalid = True; continue
                self.boot_ms = boot
                self.fix = (msg["latitude"], msg["longitude"], self.gps[1], now, 0)
                self.guard_invalid = False

    def distance(self, item):
        latitude, longitude = self.fix[:2]
        delta_lat = math.radians(item["latitude"] - latitude)
        delta_lon = math.radians((item["longitude"] - longitude + 180) % 360 - 180)
        value = math.sin(delta_lat / 2) ** 2 + math.cos(math.radians(latitude)) * math.cos(math.radians(item["latitude"])) * math.sin(delta_lon / 2) ** 2
        return 12742000 * math.atan2(math.sqrt(max(0, value)), math.sqrt(max(0, 1 - value)))

    def require_idle(self):
        if self.active: raise ValueError("stop coordinate sequence before editing or manual actuation")

    def cancel(self, reason):
        self.active = self.running = False
        self.result = reason

    def start(self):
        c = self.controller
        if c.config["hardware_profile"] != "dispenser": raise ValueError("coordinate sequences require dispenser hardware profile")
        self.require_idle()
        c.routines.require_idle()
        self.consume_position()
        if not self.saved or not self.plan["points"] or not self.fresh():
            raise ValueError("start needs a saved nonempty plan and fresh aircraft position")
        state = c.dispenser.state()
        if state["dispensing"] or state["faulted"] or state["interlockOpen"]:
            raise ValueError("start requires healthy, inactive output and closed interlock")
        for item in self.plan["points"]:
            record = c.saved["routines"].get(item["routine"])
            if record is None: raise ValueError("point requires saved routine: " + item["routine"])
            c.routines.validate(record)
            estimate = c.routines.estimate_ms(record)
            if c.dispenser.settings["armTimeoutMs"] and estimate > c.dispenser.settings["armTimeoutMs"]:
                raise ValueError("point routine exceeds configured arm window")
        c.dispenser.disarm()
        c.clear_queue()
        self.next, self.active, self.running, self.result = 0, True, False, "waiting for coordinate"
        c.publish("[GEO] sequence enabled; reached points arm and run saved routines", level="warning")

    def consume_position(self):
        if self.mailbox is not None:
            fix, _ = self.mailbox
            self.mailbox = None
            self.fix = fix
            self.guard_invalid = False if fix else True

    def tick(self):
        self.consume_position()
        if not self.active: return
        c = self.controller
        try:
            if not self.fresh(): raise ValueError("position stale; sequence stopped")
            state = c.dispenser.state()
            if state["faulted"] or state["interlockOpen"]: raise ValueError("interlock/fault stopped sequence")
            if self.running:
                if c.routines.running: return
                if c.routines.last_result != "complete": raise ValueError("routine failed or stopped; sequence stopped")
                self.running = False
                self.next += 1
                if self.next >= len(self.plan["points"]):
                    self.cancel("sequence complete")
                    c.publish("[GEO] sequence complete", level="status")
                    return
            item = self.plan["points"][self.next]
            if self.fix[2] >= 0 and self.fix[2] > item["radiusMeters"]: return
            if self.distance(item) > item["radiusMeters"]: return
            record = copy.deepcopy(c.saved["routines"][item["routine"]])
            c.dispenser.arm()
            c.routines.start(item["routine"], "Internal", "geo-run", record=record)
            self.running, self.result = True, "running coordinate " + str(self.next + 1)
            c.publish(f"[GEO] {self.result} routine={item['routine']}", level="warning")
        except Exception as error:
            c.stop_all(str(error))
            c.clear_queue()
            c.publish("[GEO] " + str(error), level="error")

    def handle(self, command, value):
        if command not in self.COMMANDS: return False
        if command == "GEOPOSITION": self.submit_position(value)
        elif command == "GEOTESTPOSITION": self.send_test_position(value)
        elif command == "GEOSTART": self.start()
        elif command == "GEOSTOP": self.controller.stop_all("coordinate sequence stopped")
        elif command not in {"GEOSTATUS", "GEOLIST"}:
            self.require_idle()
            self.controller.routines.require_idle()
            if command == "GEORESETPOSITION": self.reset_position()
            elif command == "GEOCLEAR":
                self.plan, self.saved, self.next = dict(source="MAVLINK", points=[]), False, 0
                self.reset_position()
            elif command == "GEOSOURCE":
                if value.upper() not in {"API", "MAVLINK"}: raise ValueError("use GeoSource:API or GeoSource:MAVLINK")
                self.plan["source"], self.saved = value.upper(), False
                self.reset_position()
            elif command == "GEOADD":
                if len(self.plan["points"]) >= MAX_POINTS: raise ValueError(f"coordinate point limit is {MAX_POINTS}")
                self.plan["points"].append(point(value)); self.saved = False
            elif command == "GEOSAVE":
                self.validate(self.plan)
                self.store.save_generic(self.plan); self.saved = True
            elif command == "GEOLOAD":
                if not self.store.path.exists(): raise ValueError("no saved coordinate plan")
                plan = self.store.load_generic(None)
                self.validate(plan)
                self.plan, self.saved = plan, True
                self.next = 0
                self.reset_position()
        return True

    def state(self, include_points=True):
        fix = self.fix
        result = dict(active=self.active, saved=self.saved, source=self.plan["source"], next=self.next,
                      count=len(self.plan["points"]), fresh=self.fresh(), running=self.running, result=self.result,
                      latitude=fix[0] if fix else None, longitude=fix[1] if fix else None, accuracy=fix[2] if fix else -1,
                      capacity=MAX_POINTS, udpPort=self.controller.config["geo_udp_port"], udpError=self.udp_error,
                      udpReady=bool(self.transport and not self.transport.is_closing()))
        if include_points: result["points"] = copy.deepcopy(self.plan["points"])
        if fix:
            result["ageMs"] = round((self.clock() - fix[3] + fix[4]) * 1000)
            if self.next < len(self.plan["points"]) and self.fresh(): result["distance"] = round(self.distance(self.plan["points"][self.next]), 2)
        return result

    def page(self, offset):
        if type(offset) is not int or not 0 <= offset <= MAX_POINTS: raise ValueError("invalid coordinate page offset")
        items = copy.deepcopy(self.plan["points"][offset:offset + 16])
        return dict(offset=offset, count=len(self.plan["points"]), points=items, next=offset + len(items))

    async def open_udp(self):
        mission = self
        class Receiver(asyncio.DatagramProtocol):
            def datagram_received(self, data, addr): mission.receive_mavlink(data)
            def error_received(self, error): mission.udp_error = str(error)
        config = self.controller.config
        try:
            self.transport, _ = await asyncio.get_running_loop().create_datagram_endpoint(Receiver,
                local_addr=(config["geo_udp_host"], config["geo_udp_port"]))
        except OSError as error:
            self.udp_error = str(error)
            self.controller.publish("MAVLink receiver unavailable: " + str(error), level="warning")

    def close(self):
        if self.transport:
            self.transport.close(); self.transport = None
        self.cancel("shutdown")
