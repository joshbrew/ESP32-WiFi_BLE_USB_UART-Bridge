"""ESP32 feature parity scenarios, including position loss during actuation."""
import asyncio
import copy
import json
import socket
import struct
import tempfile
import time
import unittest

from bridge.config import DEFAULTS, UINT32_MAX
from bridge.core import Controller
from bridge.hardware import SimulatedGPIO
from bridge.mavlink import positions, test_position
from bridge.geo import MAX_POINTS
from bridge.streams import LineInput
from test_controller import Clock


def frame(ident, payload, system=1, component=1, v2=True, flags=0, sequence=0):
    # Independent bitwise reference encoder, also compatible with the ESP32.
    header = bytes((len(payload), flags, 0, sequence, system, component)) + ident.to_bytes(3, "little") if v2 else bytes((len(payload), sequence, system, component, ident))
    crc = 0xffff
    for byte in header + payload + bytes((24 if ident == 24 else 104,)):
        crc ^= byte
        for _ in range(8): crc = (crc >> 1) ^ (0x8408 if crc & 1 else 0)
    return bytes((0xfd if v2 else 0xfe,)) + header + payload + crc.to_bytes(2, "little") + (bytes(13) if flags & 1 else b"")


def gps(fix=3, accuracy=1000, **options):
    payload = bytearray(52 if options.get("v2", True) else 30)
    payload[28] = fix
    if len(payload) >= 38: struct.pack_into("<I", payload, 34, accuracy)
    return frame(24, bytes(payload).rstrip(b"\0") if options.get("v2", True) else bytes(payload), **options)


def global_fix(boot, lat=37, lon=-122, **options):
    payload = struct.pack("<IiiiihhhH", boot, round(lat * 1e7), round(lon * 1e7), 0, 0, 0, 0, 0, 0)
    return frame(33, payload.rstrip(b"\0") if options.get("v2", True) else payload, **options)


class PositionFrames(unittest.TestCase):
    def test_local_test_encoder_matches_independent_crc_and_field_layout(self):
        packet = test_position(-37.12345675, 122.12345675, .5, 0xffffffff, 7, 9)
        self.assertEqual(len(packet), 90)
        self.assertEqual(packet[:50], frame(24, packet[10:48], system=7, component=9))
        self.assertEqual(packet[50:], frame(33, packet[60:88], system=7, component=9, sequence=1))
        self.assertEqual(struct.unpack_from("<ii", packet, 18), (-371234568, 1221234568))
        self.assertEqual(struct.unpack_from("<I", packet, 44)[0], 500)
        messages = list(positions(packet))
        self.assertEqual(messages[0]["fixType"], 3)
        self.assertEqual(messages[1]["bootMs"], 0xffffffff)

    def test_v1_v2_truncation_and_multiple_frames(self):
        for v2 in (False, True):
            msgs = list(positions(b"noise" + gps(v2=v2) + global_fix(25, v2=v2)))
            self.assertEqual([msg["id"] for msg in msgs], [24, 33])
            self.assertEqual(msgs[0]["fixType"], 3)
            self.assertEqual(msgs[0]["accuracyMm"], 1000 if v2 else 0)
            self.assertEqual((msgs[1]["latitude"], msgs[1]["longitude"]), (37, -122))
        self.assertEqual(list(positions(global_fix(1, 0, 0)))[0]["bootMs"], 1)

    def test_corruption_signing_unknown_flags_and_bounds(self):
        for data in (gps()[:-1], gps(flags=1), gps(flags=2), gps() + bytes(1024), frame(24, bytes(28)), frame(33, b"")):
            self.assertEqual(list(positions(data)), [])
        damaged = bytearray(global_fix(25)); damaged[-1] ^= 1
        self.assertEqual(list(positions(damaged)), [])
        self.assertEqual(len(list(positions(bytes(damaged) + gps() + global_fix(26)))), 2)


class GeoTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.config = copy.deepcopy(DEFAULTS)
        self.config["data_dir"] = self.directory.name
        self.clock = Clock()
        self.gpio = SimulatedGPIO(self.config)
        self.c = Controller(self.config, self.gpio, self.clock, watchdog=False)

    def tearDown(self):
        self.c.geo.close()
        self.c.dispenser.close()
        self.directory.cleanup()

    def command(self, line): self.c.execute(line)

    def routine(self, name="dots", delay=0, pulse=100, gap=0, repeats=1):
        for line in (f"RoutineCreate:{name}", f"RoutineAdd:{name}:START_WAIT:{delay}", f"RoutineAdd:{name}:DISPENSE:{pulse}", f"RoutineAdd:{name}:WAIT_IDLE", f"RoutineAdd:{name}:WAIT:{gap}", f"RoutineRepeat:{name}:{repeats}", f"RoutineSave:{name}"):
            self.command(line)

    def plan(self, source="API", points=("37,-122,5,dots",)):
        self.command("GeoClear")
        self.command("GeoSource:" + source)
        for spec in points: self.command("GeoAdd:" + spec)
        self.command("GeoSave")

    def fix(self, lat=37, lon=-122, accuracy=1, age=0):
        self.c.geo.submit_position(f"{lat},{lon},{accuracy},{age}")
        self.c.geo.tick()

    def tick(self, seconds=.01):
        self.clock.advance(seconds)
        self.c.dispenser.tick()
        self.c.routines.tick()
        self.c.geo.tick()

    def test_full_32_bit_ranges_and_no_runtime_cap(self):
        self.routine(delay=UINT32_MAX, pulse=UINT32_MAX, gap=UINT32_MAX, repeats=UINT32_MAX)
        self.command("Arm"); self.command("RoutineRun:dots")
        self.tick(); self.tick(301)
        self.assertIsNotNone(self.c.routines.running)
        self.assertFalse(self.gpio.active)
        self.assertTrue(self.c.dispenser.state()["armUnlimited"])
        json.dumps(self.c.state(), allow_nan=False)
        self.c.submit("StopAll")
        self.command("Arm"); self.command(f"Dispense:{UINT32_MAX}")
        self.tick(301)
        self.assertTrue(self.gpio.active)
        for line in (f"Dispense:{UINT32_MAX + 1}", "Dispense:0", "RoutineRepeat:dots:0", f"RoutineRepeat:dots:{UINT32_MAX + 1}"):
            with self.assertRaises(ValueError): self.command(line)

    def test_initial_delay_runs_once_gap_runs_each_repeat(self):
        self.routine(delay=200, pulse=30, gap=100, repeats=3)
        self.command("Arm"); self.command("RoutineRun:dots")
        for _ in range(10): self.tick()
        self.assertFalse(self.gpio.active)
        self.assertGreater(self.c.routines.state()["delayRemainingMs"], 0)
        for _ in range(65): self.tick()
        self.assertEqual(self.c.dispenser.count, 3)
        self.assertEqual(self.c.routines.last_result, "complete")
        self.assertFalse(self.c.dispenser.state()["armed"])
        self.command("RoutineCreate:bad")
        self.command("RoutineAdd:bad:WAIT:0")
        with self.assertRaises(ValueError): self.command("RoutineAdd:bad:START_WAIT:10")

    def test_continuous_persists_runs_beyond_finite_count_and_delays_once(self):
        self.routine(delay=200, pulse=30, gap=100, repeats="FOREVER")
        self.assertEqual(self.c.saved["routines"]["dots"]["repeats"], 0)
        other = Controller(self.config, SimulatedGPIO(self.config), self.clock, watchdog=False)
        try:
            self.assertEqual(other.routines.library["dots"]["repeats"], 0)
            self.assertIsNone(other.routines.running)
            self.assertFalse(other.dispenser.state()["armed"])
        finally: other.dispenser.close()
        self.command("Arm"); self.command("RoutineRun:dots")
        for _ in range(10): self.tick()
        self.assertEqual(self.c.dispenser.count, 0)
        for _ in range(300): self.tick()
        self.assertGreater(self.c.dispenser.count, 10)
        self.assertTrue(self.c.routines.state()["continuous"])
        self.assertEqual(self.c.routines.state()["repeats"], 0)
        json.dumps(self.c.state(), allow_nan=False)
        self.c.submit("RoutineStop")
        for _ in range(100): self.tick()
        self.assertIsNone(self.c.routines.running)
        self.assertFalse(self.gpio.active)

    def test_continuous_requires_disabled_expiry_and_honors_pulse_limit(self):
        self.routine(repeats="forever", pulse=100)
        self.command("DispenserArmTimeout:10000"); self.command("Arm")
        with self.assertRaisesRegex(ValueError, "disabled arm expiry"): self.command("RoutineRun:dots")
        self.assertEqual(self.c.dispenser.count, 0)
        self.command("Disarm"); self.command("DispenserArmTimeout:0")
        self.command("DispenserDefaultPulse:50"); self.command("DispenserMaxPulse:50"); self.command("Arm")
        with self.assertRaisesRegex(ValueError, "maximum"): self.command("RoutineRun:dots")
        for spec in ("0", "FOREVERx", "-1", "4294967296"):
            with self.assertRaises(ValueError): self.command("RoutineRepeat:dots:" + spec)

    def test_continuous_counter_saturates_without_replaying_start_delay(self):
        self.routine(delay=9999, repeats="FOREVER")
        self.command("Arm"); self.command("RoutineRun:dots")
        self.c.routines.repeat = UINT32_MAX - 2
        self.c.routines.index = len(self.c.routines.running["steps"])
        self.tick()
        self.assertEqual(self.c.routines.repeat, UINT32_MAX - 1)
        self.assertEqual(self.c.routines.index, 1)
        self.tick(); self.assertTrue(self.gpio.active)
        self.tick(.2)
        for _ in range(8): self.tick()
        self.assertEqual(self.c.routines.repeat, UINT32_MAX - 1)
        self.assertEqual(self.c.routines.state()["delayRemainingMs"], 0)

    def test_continuous_point_holds_sequence_and_stale_fix_still_stops(self):
        self.routine(pulse=30, repeats="FOREVER")
        self.plan(points=("37,-122,5,dots", "37,-122,5,dots"))
        self.fix(); self.command("GeoStart")
        for index in range(500):
            if index % 100 == 0: self.fix()
            self.tick()
        self.assertEqual(self.c.geo.next, 0)
        self.assertTrue(self.c.geo.running)
        self.assertGreater(self.c.dispenser.count, 20)
        self.tick(3.01)
        self.assertFalse(self.c.geo.active)
        self.assertFalse(self.c.dispenser.state()["armed"])

    def test_continuous_interlock_and_stop_aliases_cancel_future_cycles(self):
        self.routine(pulse=10000, repeats="FOREVER")
        for stop in ("StopAll", "Disarm", "RoutineStop", "DispenseStop", "GPIO26:OFF"):
            self.command("Arm"); self.command("RoutineRun:dots")
            for _ in range(3): self.tick()
            self.assertTrue(self.gpio.active)
            self.c.submit(stop)
            before = self.c.dispenser.count
            for _ in range(5): self.tick(20)
            self.assertEqual(self.c.dispenser.count, before)
            self.assertFalse(self.c.dispenser.state()["armed"])
        self.command("Arm"); self.command("RoutineRun:dots")
        for _ in range(3): self.tick()
        self.gpio.interlock_open = True; self.tick()
        self.assertIsNone(self.c.routines.running)
        self.assertFalse(self.gpio.active)

    def test_test_position_requires_mavlink_wifi_and_receiver(self):
        self.command("GeoSource:API")
        with self.assertRaisesRegex(ValueError, "MAVLink source"): self.command("GeoTestPosition:37,-122,1")
        self.command("GeoSource:MAVLINK")
        with self.assertRaisesRegex(ValueError, "active UDP"): self.command("GeoTestPosition:37,-122,1")
        for body in ("91,0,1", "0,181,1", "0,0,-.5", "0,0,100001", "NaN,0,1", "0,0,1,0"):
            with self.assertRaises(ValueError): self.command("GeoTestPosition:" + body)
        self.c.radio.wifi_enabled = False
        with self.assertRaisesRegex(ValueError, "enabled Wi-Fi"): self.command("GeoTestPosition:37,-122,1")

    def test_preflight_rejects_pulse_and_total_before_output(self):
        self.routine(pulse=500, delay=2000, gap=1000, repeats=3)
        self.command("DispenserMaxPulse:400")
        self.command("Arm")
        with self.assertRaisesRegex(ValueError, "maximum"): self.command("RoutineRun:dots")
        self.command("Disarm"); self.command("DispenserMaxPulse:600")
        self.command("DispenserArmTimeout:7500"); self.command("Arm")
        self.clock.advance(.1)
        with self.assertRaisesRegex(ValueError, "remaining arm window"): self.command("RoutineRun:dots")
        self.assertEqual(self.c.dispenser.count, 0)

    def test_old_saved_limits_survive_and_can_be_removed(self):
        self.command("DispenserMaxPulse:60000"); self.command("DispenserArmTimeout:120000")
        self.command("DispenserSave")
        other = Controller(self.config, SimulatedGPIO(self.config), self.clock, watchdog=False)
        try:
            self.assertEqual(other.dispenser.settings["maxPulseMs"], 60000)
            other.execute("DispenserArmTimeout:0"); other.execute("DispenserMaxPulse:0")
            self.assertEqual(other.dispenser.settings["maxPulseMs"], 0)
        finally: other.dispenser.close()

    def test_plan_bounds_saved_state_and_fresh_start(self):
        self.routine()
        self.plan(points=tuple("37,-122,.1,dots" for _ in range(MAX_POINTS)))
        with self.assertRaises(ValueError): self.command("GeoAdd:37,-122,5,dots")
        with self.assertRaises(ValueError): self.command("GeoStart")
        self.command("GeoPosition:37,-122,-1,0")
        self.command("GeoStart")  # Pending latest fix consumed before start.
        self.assertTrue(self.c.geo.active)
        other = Controller(self.config, SimulatedGPIO(self.config), self.clock, watchdog=False)
        try:
            self.assertTrue(other.geo.saved)
            self.assertEqual(len(other.geo.plan["points"]), 500)
            self.assertFalse(other.geo.active)
            self.assertFalse(other.geo.fresh())
        finally: other.dispenser.close()

    def test_large_plan_storage_and_paged_stream_readback(self):
        self.routine(name="abcdefghijklmno")
        self.plan(points=tuple("-0.00012345678901234567,0.00012345678901234567,0.12345678901234567,abcdefghijklmno" for _ in range(500)))
        self.assertGreater(self.c.geo.store.path.stat().st_size, 65536)
        self.command("GeoLoad")
        replies = []
        stream = LineInput(self.c, "BLE", replies.append)
        stream.feed(b"@STATE\n")
        snapshot = json.loads(replies.pop()[7:])
        self.assertEqual(snapshot["geo"]["count"], 500)
        self.assertNotIn("points", snapshot["geo"])
        stream.feed(b"@GEO:0\n@GEO:496\n@GEO:501\n")
        self.assertEqual(len(json.loads(replies[0][5:])["points"]), 16)
        last = json.loads(replies[1][5:])
        self.assertEqual((last["offset"], last["next"], len(last["points"])), (496, 500, 4))
        self.assertTrue(replies[2].startswith("@ERROR"))
        self.assertTrue(all(len(reply.encode()) < 16384 for reply in replies))
        self.command("GeoList:496")
        listing = json.loads(self.c.events[-1]["t"])
        self.assertEqual(listing["next"], 500)

    def test_routine_dispense_stop_cancels_later_pulses(self):
        self.command("RoutineCreate:stoptest")
        self.command("RoutineAdd:stoptest:COMMAND:DispenseStop")
        self.command("RoutineAdd:stoptest:DISPENSE:100")
        self.command("RoutineRepeat:stoptest:3")
        self.command("Arm"); self.command("RoutineRun:stoptest")
        for _ in range(10): self.tick()
        self.assertIsNone(self.c.routines.running)
        self.assertEqual(self.c.dispenser.count, 0)
        self.assertFalse(self.c.dispenser.state()["armed"])

    def test_order_accuracy_saved_snapshot_and_completion(self):
        self.routine()
        self.plan(points=("37,-122,5,dots", "38,-122,5,dots"))
        self.fix(38); self.command("GeoStart"); self.tick()
        self.assertFalse(self.c.geo.running)
        self.fix(37, accuracy=6); self.tick()
        self.assertFalse(self.c.geo.running)
        # Coordinate runs use the persisted routine, not unsaved library changes.
        self.c.routines.library["dots"]["steps"][1] = "Dispense:9999"
        self.fix(37); self.tick()
        for _ in range(30): self.tick()
        self.assertEqual(self.c.geo.next, 1)
        self.assertFalse(self.c.dispenser.state()["armed"])
        self.assertEqual(self.c.dispenser.count, 1)
        self.assertTrue(self.c.geo.active)
        self.fix(38)
        for _ in range(30): self.tick()
        self.assertEqual(self.c.geo.result, "sequence complete")
        self.assertEqual(self.c.dispenser.count, 2)

    def test_overlapping_points_trigger_in_order(self):
        self.routine(pulse=20)
        self.plan(points=("37,-122,5,dots", "37,-122,5,dots"))
        self.fix(); self.command("GeoStart")
        for _ in range(40): self.tick()
        self.assertEqual(self.c.dispenser.count, 2)
        self.assertEqual(self.c.geo.result, "sequence complete")

    def test_stale_during_delay_and_pulse_inhibits_independently(self):
        for delay in (0, 10000):
            self.routine(delay=delay, pulse=10000)
            self.plan(); self.fix(); self.command("GeoStart")
            for _ in range(5): self.tick()
            self.assertEqual(self.gpio.active, delay == 0)
            self.clock.advance(3.01)
            self.c.dispenser.tick()  # No async ticker/command processing needed.
            self.assertFalse(self.gpio.active)
            self.assertFalse(self.c.dispenser.state()["armed"])
            self.c.geo.tick()
            self.assertFalse(self.c.geo.active)
            self.assertIsNone(self.c.routines.running)

    def test_watchdog_thread_stops_with_stalled_event_loop(self):
        self.c.dispenser.close()
        self.c = Controller(self.config, self.gpio, self.clock, watchdog=True)
        self.routine(pulse=10000); self.plan(); self.fix(); self.command("GeoStart")
        for _ in range(5): self.tick()
        self.assertTrue(self.gpio.active)
        self.clock.advance(3.01)
        time.sleep(.04)
        self.assertFalse(self.gpio.active)
        self.assertEqual(self.c.dispenser.armed_until, 0)

    def test_invalid_fix_mailbox_and_measurement_age(self):
        self.routine(pulse=10000); self.plan(); self.fix(); self.command("GeoStart")
        for _ in range(5): self.tick()
        with self.assertRaises(ValueError): self.c.geo.submit_position("37,-122,1,3001")
        self.c.dispenser.tick()
        self.assertFalse(self.gpio.active)
        self.c.geo.tick(); self.assertFalse(self.c.geo.active)
        for body in ("91,0,1,0", "0,181,1,0", "NaN,0,1,0", "0,0,-.5,0", "0,0,1,-1", "0" * 129):
            with self.assertRaises(ValueError): self.c.geo.submit_position(body)
        self.fix(age=2990); self.clock.advance(.02)
        self.assertFalse(self.c.geo.fresh())

    def test_all_stops_and_interlock_during_wait(self):
        self.routine(delay=5000)
        self.plan()
        for stop in ("GeoStop", "StopAll", "Disarm", "DispenseStop", "DispenserOff", "GPIO26:OFF", "RoutineStop", "DispenserDisarm"):
            self.fix(); self.command("GeoStart"); self.tick()
            self.c.submit(stop + "\nArm\nDispense:100")
            self.assertFalse(self.c.geo.active, stop)
            self.assertIsNone(self.c.routines.running)
            self.assertEqual(self.c.queue.qsize(), 0)
            self.assertFalse(self.gpio.active)
        self.fix(38); self.command("GeoStart")
        self.gpio.interlock_open = True; self.tick()
        self.assertFalse(self.c.geo.active)

    def test_active_mission_blocks_manual_and_edits(self):
        self.routine(); self.plan(); self.fix(38); self.command("GeoStart")
        for command in ("Arm", "Dispense:100", "GeoClear", "GeoSource:MAVLINK", "GeoLoad", "GeoResetPosition", "RoutineCreate:new", "ModeWiFi", "SelfTestStart"):
            with self.assertRaises(ValueError): self.command(command)
        self.command("GeoStatus"); self.command("GeoPosition:38,-122,1,0")
        self.assertTrue(self.c.geo.active)

    def test_mavlink_ids_duplicates_wrap_reset_and_invalid_gps(self):
        self.routine(); self.plan(source="MAVLINK")
        self.c.geo.receive_mavlink(gps(system=2) + global_fix(100, system=2))
        self.assertFalse(self.c.geo.fresh())
        self.c.geo.receive_mavlink(global_fix(100))
        self.assertFalse(self.c.geo.fresh())
        self.c.geo.receive_mavlink(gps() + global_fix(0xfffffff0))
        self.assertTrue(self.c.geo.fresh())
        self.clock.advance(2)
        self.c.geo.receive_mavlink(gps() + global_fix(0xfffffff0))
        self.clock.advance(1.01)
        self.assertFalse(self.c.geo.fresh())
        self.c.geo.receive_mavlink(gps() + global_fix(10))
        self.assertTrue(self.c.geo.fresh())
        self.c.geo.receive_mavlink(global_fix(9)); self.assertEqual(self.c.geo.boot_ms, 10)
        self.c.geo.receive_mavlink(gps(fix=2)); self.assertFalse(self.c.geo.fresh())
        self.command("GeoResetPosition")
        self.c.geo.receive_mavlink(gps() + global_fix(1)); self.assertTrue(self.c.geo.fresh())
        with self.assertRaises(ValueError): self.c.geo.submit_position("37,-122,1,0")
        self.assertTrue(self.c.geo.fresh())  # API cannot override this source.

    def test_dateline_distance(self):
        self.plan(points=("0,-179.9999,30,dots",))
        self.fix(0, 179.9999)
        self.assertAlmostEqual(self.c.geo.distance(self.c.geo.plan["points"][0]), 22.24, places=1)

    def test_lr_fails_explicitly(self):
        for command in ("WiFiLR:ON", "WiFiMode:LRONLY", "RadioBoot:LRONLY"):
            with self.assertRaisesRegex(ValueError, "ESP32 proprietary (Wi-Fi )?LR"): self.command(command)


class PositionHTTPTests(unittest.IsolatedAsyncioTestCase):
    async def test_test_command_uses_real_udp_and_triggers_ordered_routine(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.bind(("127.0.0.1", 0))
            self.c.config.update(geo_udp_host="127.0.0.1", geo_udp_port=probe.getsockname()[1])
        await self.c.geo.open_udp()
        self.c.execute("RoutineCreate:dot")
        self.c.execute("RoutineAdd:dot:DISPENSE:10")
        self.c.execute("RoutineSave:dot")
        self.c.execute("GeoAdd:37,-122,5,dot"); self.c.execute("GeoSave")
        # No fix is set synchronously; the normal socket must deliver the packet.
        self.c.execute("GeoTestPosition:38,-122,1")
        self.assertIsNone(self.c.geo.fix)
        for _ in range(30):
            if self.c.geo.fresh(): break
            await asyncio.sleep(.01)
        self.assertTrue(self.c.geo.fresh())
        self.assertEqual(self.c.geo.fix[:2], (38, -122))
        self.c.execute("GeoStart"); self.c.geo.tick()
        self.assertFalse(self.c.geo.running)
        previous = self.c.geo.boot_ms
        self.c.execute("GeoTestPosition:37,-122,0")
        for _ in range(30):
            if self.c.geo.boot_ms != previous: break
            await asyncio.sleep(.01)
        self.c.geo.tick()
        self.assertTrue(self.c.geo.running)
        self.assertEqual(self.c.geo.fix[2], .001)
        self.c.submit("GeoStop")
        self.assertFalse(self.c.geo.active)
        self.assertFalse(self.c.dispenser.state()["armed"])
    async def asyncSetUp(self):
        from aiohttp.test_utils import TestClient, TestServer
        from bridge.server import create_app
        self.directory = tempfile.TemporaryDirectory()
        config = dict(DEFAULTS, data_dir=self.directory.name)
        self.c = Controller(config, SimulatedGPIO(config), watchdog=False)
        self.client = TestClient(TestServer(create_app(self.c, manage_lifecycle=False)))
        await self.client.start_server()

    async def asyncTearDown(self):
        await self.client.close()
        await self.c.close()
        self.directory.cleanup()

    async def test_position_latest_slot_bypasses_full_command_queue(self):
        self.c.execute("GeoSource:API")
        for _ in range(8): self.c.submit("Ping")
        for lat in (37, 38, 39):
            response = await self.client.post("/api/position", data=f"{lat},-122,1,0")
            self.assertEqual(response.status, 200)
        self.assertEqual(self.c.queue.qsize(), 8)
        self.c.geo.tick()
        self.assertEqual(self.c.geo.fix[0], 39)
        self.assertTrue(self.c.geo.fresh())

    async def test_invalid_oversized_utf8_and_content_type_revoke_fix(self):
        self.c.execute("GeoSource:API")
        for data, headers in (("37,-122,1,3001", {}), ("x" * 129, {}), (b"\xff", {"Content-Type": "text/plain"}), ("{}", {"Content-Type": "application/json"})):
            await self.client.post("/api/position", data="37,-122,1,0")
            self.c.geo.tick(); self.assertTrue(self.c.geo.fresh())
            self.assertEqual((await self.client.post("/api/position", data=data, headers=headers)).status, 400)
            self.assertFalse(self.c.geo.fresh())

    async def test_source_and_origin_enforced(self):
        self.assertEqual((await self.client.post("/api/position", data="37,-122,1,0")).status, 400)
        self.c.execute("GeoSource:API")
        response = await self.client.post("/api/position", data="37,-122,1,0", headers={"Origin": "https://unrelated.example"})
        self.assertEqual(response.status, 403)
        self.assertIsNone(self.c.geo.mailbox)

    async def test_real_udp_receiver(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
            sender.bind(("127.0.0.1", 0))
            # Reserve an available receiver port then bind through the application.
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
                probe.bind(("127.0.0.1", 0))
                self.c.config.update(geo_udp_host="127.0.0.1", geo_udp_port=probe.getsockname()[1])
            await self.c.geo.open_udp()
            self.assertIsNotNone(self.c.geo.transport)
            sender.sendto(gps() + global_fix(100), ("127.0.0.1", self.c.config["geo_udp_port"]))
            for _ in range(20):
                if self.c.geo.fresh(): break
                await asyncio.sleep(.01)
            self.assertTrue(self.c.geo.fresh())
