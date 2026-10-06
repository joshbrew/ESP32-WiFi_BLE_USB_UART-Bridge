import asyncio
import copy
import hashlib
import io
import json
import os
import socket
import subprocess
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

from bridge.advanced import Advanced, AdvancedIO
from bridge.config import DEFAULTS, validate
from bridge.core import Controller
from bridge.radio import DEFAULT_RADIO
from bridge.release import build_bundle, validate_bundle, valid_path
from bridge.store import Store
from bridge.transport_manager import TransportManager
from deploy.admin import HostManager, validate_request
from tools.configure_usb_gadget import boot_text
from test_controller import Clock

ROOT = Path(__file__).resolve().parent.parent


class MotorTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.config = dict(DEFAULTS, data_dir=self.directory.name, hardware_profile="stepper_dac")
        self.clock = Clock()
        self.motor = Advanced(self.config, self.clock, watchdog=False)

    def tearDown(self):
        self.motor.close()
        self.directory.cleanup()

    def test_half_full_steps_and_direction(self):
        self.motor.handle("RPM:5,4,1")
        for _ in range(4):
            self.motor.tick(); self.clock.advance(.1)
        self.assertEqual(self.motor.position, 4)
        self.assertFalse(self.motor.coils_on)
        self.motor.handle("StepMode:4")
        self.motor.handle("SetRevSteps:4")
        self.motor.handle("DEG:5,180,2")
        self.assertEqual(self.motor.remaining, 2)
        self.motor.tick(); self.clock.advance(10); self.motor.tick()
        self.assertEqual(self.motor.position, 2)

    def test_late_tick_never_bursts_and_limits_hold(self):
        self.motor.handle("HoldTorque:1")
        self.motor.handle("RPM:5,2,1")
        self.clock.advance(20); self.motor.tick()
        self.assertEqual(self.motor.remaining, 1)
        self.motor.tick()
        self.assertEqual(self.motor.remaining, 1)
        self.clock.advance(1); self.motor.tick()
        self.assertTrue(self.motor.coils_on)
        self.motor.handle("StepMode:4")
        self.assertFalse(self.motor.coils_on)
        for command in ("RPM:13,1,1", "RPM:nan,1,1", "DEG:5,90,3", "RPM:5,1.5,1", "SetMinStepIntervalUs:1", "StepOrder:0012"):
            with self.assertRaises(ValueError): self.motor.handle(command)

    def test_dac_timer_not_extended_by_voltage_change(self):
        self.motor.handle("DAC1:TEST3S")
        self.clock.advance(2); self.motor.handle("DAC1:MV:1000")
        self.assertGreater(self.motor.io.code, 0)
        self.clock.advance(1.01); self.motor.tick()
        self.assertFalse(self.motor.dac_on)
        self.assertEqual(self.motor.io.code, 0)
        self.motor.handle("DACTest3S")
        self.assertTrue(self.motor.digital_on)
        self.motor.stop()
        self.assertFalse(self.motor.digital_on)

    def test_dac_persistence_never_restores_active_output(self):
        self.motor.handle("DAC1:MV:1200"); self.motor.handle("DAC1:ON"); self.motor.handle("DACSave")
        other = Advanced(self.config, self.clock, watchdog=False)
        try:
            self.assertEqual(other.mv, 1200)
            self.assertFalse(other.dac_on)
            self.assertEqual(other.io.code, 0)
        finally: other.close()

    def test_mcp4725_register_encoding(self):
        io_device = AdvancedIO(self.config)
        calls = []
        class Bus:
            def write_i2c_block_data(self, *args): calls.append(args)
            def close(self): pass
        io_device.bus, io_device.address = Bus(), 96
        io_device.write_dac(0xABC)
        self.assertEqual(calls, [(96, 0x40, [0xAB, 0xC0])])
        io_device.close()

    def test_invalid_saved_dac_closes_driver(self):
        Store(self.directory.name, "dac.json").save_generic({"bad": True})
        class IO:
            closed = False
            def close(self): self.closed = True
        device = IO()
        with self.assertRaises(ValueError): Advanced(self.config, self.clock, io=device, watchdog=False)
        self.assertTrue(device.closed)


class FeatureTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.config = dict(DEFAULTS, data_dir=self.directory.name)
        self.controller = Controller(self.config, watchdog=False)
        await self.controller.start()
        self.manager = TransportManager(self.controller)
        await self.manager.start()

    async def asyncTearDown(self):
        await self.manager.close()
        await self.controller.close()
        self.directory.cleanup()

    async def test_radio_profiles_runtime_and_redaction(self):
        c = self.controller
        c.execute("WiFiStaPassword:secret1234")
        c.execute("WiFiStaSSID:bench")
        c.execute("ConfigSave")
        c.execute("ModeWiFiBLE")
        await c.radio.task
        self.assertTrue(c.ble_running)
        self.assertEqual(c.radio.record["lastGood"], "WIFI_BLE")
        self.assertIsNone(c.radio.record["pending"])
        self.assertNotIn("secret1234", json.dumps(c.state()))
        c.execute("SPP:ON")
        self.assertEqual(c.radio.desired, "SPP")
        self.assertIsNone(self.manager.spp)
        c.execute("ConfigSave")
        self.assertEqual(c.radio.record["bootProfile"], "SPP")
        c.execute("ConfigApply"); await c.radio.task
        self.assertIsNotNone(self.manager.spp)
        self.assertFalse(c.radio.wifi_enabled)
        self.assertFalse(c.ble_running)
        c.execute("SPP:OFF"); c.execute("WiFi:ON")
        self.assertIsNotNone(self.manager.spp)
        c.execute("ConfigApply"); await c.radio.task
        self.assertIsNone(self.manager.spp)
        self.assertTrue(c.radio.wifi_enabled)

    async def test_profile_failure_restores_record_and_transports(self):
        c = self.controller
        before = copy.deepcopy(c.radio.record)
        async def admin(operation, **fields):
            if operation == "radio": raise ValueError("network failed")
            return {"ok": True}
        with patch.object(c.admin, "run", side_effect=admin):
            c.execute("ModeWiFiBLE"); await c.radio.task
        self.assertEqual(c.radio.record, before)
        self.assertEqual(c.radio.active, "WIFI")
        self.assertFalse(c.ble_running)
        self.assertIsNone(self.manager.ble)
        self.assertIn("network failed", c.radio.error)

    async def test_runtime_commit_failure_restores_flags(self):
        c = self.controller
        async def admin(operation, **fields):
            if operation == "radioCommit": raise ValueError("commit failed")
            return {"ok": True, "role": "off"}
        with patch.object(c.admin, "run", side_effect=admin):
            c.radio.schedule(c.radio.runtime(False, False, False), "WiFi", "runtime-test")
            await c.radio.task
        self.assertTrue(c.radio.wifi_enabled)

    async def test_radio_trial_startup_reverts_to_last_good(self):
        record = dict(self.controller.radio.record, pending="BLE", bootProfile="BLE")
        self.controller.radio.store.save_generic(record)
        reboot = Controller(self.config, watchdog=False)
        try:
            self.assertEqual(reboot.radio.active, "WIFI")
            self.assertIsNone(reboot.radio.record["pending"])
        finally: await reboot.close()

    async def test_selftest_completes_without_output_and_restores_settings(self):
        c = self.controller
        c.execute("SelfTestStart")
        with self.assertRaises(ValueError): c.execute("Arm")
        await c.self_tests.task
        self.assertEqual(c.self_tests.state()["phase"], "complete")
        self.assertEqual(c.self_tests.state()["fail"], 0)
        self.assertEqual(c.self_tests.state()["current"], 12)
        self.assertEqual(c.dispenser.count, 0)
        self.assertFalse(c.dispenser.state()["armed"])

    async def test_selftest_checkpoint_resume_after_restart(self):
        c = self.controller
        c.execute("SelfTestStart")
        await asyncio.sleep(.12)
        await c.self_tests.close()
        checkpoint = c.self_tests.report["current"]
        self.assertGreater(checkpoint, 0)
        reboot = Controller(self.config, watchdog=False)
        try:
            self.assertEqual(reboot.self_tests.report["phase"], "paused")
            reboot.execute("SelfTestResume")
            await reboot.self_tests.task
            self.assertEqual(reboot.self_tests.state()["current"], 12)
            self.assertEqual(len(reboot.self_tests.report["results"]), 12)
        finally: await reboot.close()

    async def test_stop_aborts_test_and_boot_policy_persists(self):
        c = self.controller
        c.execute("DebugMode")
        c.execute("SelfTestStart")
        c.submit("StopAll")
        await asyncio.sleep(0)
        self.assertEqual(c.self_tests.report["phase"], "aborted")
        self.assertFalse(c.self_tests.active)
        reboot = Controller(self.config, watchdog=False)
        try: self.assertFalse(reboot.features.system["production"])
        finally: await reboot.close()

    async def test_advanced_stop_barrier_cancels_old_moves_and_outputs(self):
        c = Controller(dict(self.config, hardware_profile="stepper_dac"), watchdog=False)
        try:
            c.execute("DAC1:ON"); c.execute("RPM:5,200,1")
            c.submit("RPM:5,100,1")
            c.submit("CoilsOff\nDAC1:ON")
            self.assertEqual(c.queue.qsize(), 0)
            self.assertEqual(c.advanced.remaining, 0)
            self.assertFalse(c.advanced.dac_on)
        finally: await c.close()

    async def test_motor_routine_waits_for_completion(self):
        c = Controller(dict(self.config, hardware_profile="stepper_dac"), watchdog=False)
        try:
            for line in ("RoutineCreate:move", "RoutineAdd:move:COMMAND:RPM:5,2,1", "RoutineAdd:move:WAIT_IDLE", "RoutineAdd:move:COMMAND:DAC1:ON", "RoutineSave:move", "RoutineRun:move"):
                c.execute(line)
            c.routines.tick()
            self.assertTrue(c.advanced.remaining)
            await asyncio.sleep(.05)
            for _ in range(5): c.routines.tick()
            self.assertEqual(c.routines.last_result, "complete")
            self.assertFalse(c.advanced.dac_on)
            self.assertFalse(c.advanced.coils_on)
            with self.assertRaises(ValueError): c.execute("RoutineAdd:move:COMMAND:Reboot")
        finally: await c.close()

    async def test_real_admin_dispatch_keeps_actuation_blocked(self):
        async def admin(*args, **kwargs): return {"ok": True, "scheduled": "reboot"}
        c = self.controller
        with patch.object(c.admin, "run", side_effect=admin):
            c.execute("Reboot"); await c.features.admin_task
        self.assertTrue(c.features.admin_busy)
        with self.assertRaises(ValueError): c.execute("Arm")

    async def test_previous_update_status_cannot_unlock_new_upload(self):
        c = self.controller
        record = dict(phase="committed", candidate="a" * 64)
        (Path(self.directory.name) / "update-status.json").write_text(json.dumps(record))
        c.updates.state()
        reached = asyncio.Event()
        release = asyncio.Event()
        payload = build_bundle(ROOT, "0.2.0-test")
        async def chunks():
            reached.set(); await release.wait(); yield payload
        task = asyncio.create_task(c.updates.upload(chunks()))
        await reached.wait()
        self.assertTrue(c.updates.state()["busy"])
        self.assertEqual(c.updates.state()["phase"], "uploading")
        with self.assertRaises(ValueError): c.execute("Arm")
        with self.assertRaises(ValueError): await c.updates.upload(chunks())
        release.set(); await task
        self.assertFalse(c.updates.busy)
        self.assertEqual(c.updates.state()["phase"], "validated-simulation")

    @unittest.skipUnless(os.name == "posix", "D-Bus Unix descriptors require Linux")
    async def test_spp_socket_input_output_and_disconnect(self):
        from bridge.spp import Profile, SPPTransport
        c = self.controller
        transport = SPPTransport(c)
        local, remote = socket.socketpair()
        remote.setblocking(False)
        try:
            Profile(transport).NewConnection("/device", local.detach(), {})
            c.outputs["SPP"] = transport
            await asyncio.get_running_loop().sock_sendall(remote, b"Ping\n")
            response = await asyncio.wait_for(asyncio.get_running_loop().sock_recv(remote, 4096), 1)
            self.assertIn(b"PONG", response)
            transport.disconnect()
            self.assertFalse(transport.ready)
        finally:
            remote.close(); await transport.close()


class BundleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls): cls.payload = build_bundle(ROOT, "0.2.0-test")

    def test_build_checksums_deterministic(self):
        self.assertEqual(self.payload, build_bundle(ROOT, "0.2.0-test"))
        bundle = validate_bundle(self.payload)
        self.assertEqual(bundle["releaseId"], hashlib.sha256(self.payload).hexdigest())
        self.assertIn("bridge/spp.py", bundle["files"])

    def rewrite(self, transform):
        source = zipfile.ZipFile(io.BytesIO(self.payload))
        entries = {name: source.read(name) for name in source.namelist()}
        transform(entries)
        target = io.BytesIO()
        with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as archive:
            for name, content in entries.items(): archive.writestr(name, content)
        return target.getvalue()

    def test_hash_mismatch_rejected(self):
        payload = self.rewrite(lambda files: files.update({"web/app.js": b"changed"}))
        with self.assertRaises(ValueError): validate_bundle(payload)

    def test_traversal_and_extra_files_rejected(self):
        for name in ("../evil.py", "bridge/../evil.py", "/bridge/x.py", "bridge\\x.py", "bridge/x/y.py", "deploy/admin.py"):
            self.assertFalse(valid_path(name))
        payload = self.rewrite(lambda files: files.update({"../evil.py": b"bad"}))
        with self.assertRaises(ValueError): validate_bundle(payload)

    def test_missing_file_and_symlink_rejected(self):
        payload = self.rewrite(lambda files: files.pop("bridge/core.py"))
        with self.assertRaises(ValueError): validate_bundle(payload)
        output = io.BytesIO()
        with zipfile.ZipFile(output, "w") as archive:
            item = zipfile.ZipInfo("web/app.js"); item.external_attr = 0o120777 << 16
            archive.writestr(item, "elsewhere"); archive.writestr("manifest.json", "{}")
        with self.assertRaises(ValueError): validate_bundle(output.getvalue())


class HostTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        self.calls = []
        self.config = dict(DEFAULTS, enable_os_control=True, update_token="x" * 32)
        def run(args):
            self.calls.append(args)
            return "enabled" if args == ["/usr/bin/nmcli", "radio", "wifi"] else ""
        self.manager = HostManager(self.config, run, self.root / "state", self.root / "install", self.root / "network", self.root / "radio.json")
        self.nofollow = patch.object(os, "O_NOFOLLOW", getattr(os, "O_NOFOLLOW", 0), create=True)
        self.nofollow.start()

    def tearDown(self):
        self.nofollow.stop(); self.directory.cleanup()

    def test_helper_rejects_arbitrary_commands_and_fields(self):
        for request in ({"operation": "shell"}, {"operation": "reboot", "args": "bad"}, {"operation": "update", "path": "bad"}):
            with self.assertRaises(ValueError): validate_request(request, self.config)
        with self.assertRaises(ValueError): validate_request({"operation": "radio", "settings": dict(DEFAULT_RADIO), "wifi": True}, dict(self.config, wifi_interface="wlan0;reboot"))
        self.assertEqual(validate_request({"operation": "update"}, self.config), "update")

    def test_station_failure_falls_back_to_ap_and_rollback_disconnects(self):
        run = self.manager.run
        def fail_station(args):
            if "up" in args and "uuid" in args and args[-1] == "wlan0" and sum("up" in call for call in self.calls) == 0:
                self.calls.append(args); raise subprocess.CalledProcessError(1, args)
            return run(args)
        self.manager.run = fail_station
        settings = dict(DEFAULT_RADIO, staSSID="bench", staPassword="password123")
        result = self.manager.radio(settings, True)
        self.assertEqual(result["role"], "AP")
        self.assertTrue(self.manager.radio_record.exists())
        self.manager.restore_radio()
        self.assertFalse(self.manager.radio_record.exists())
        self.assertFalse((self.manager.network / "PiControllerAP.nmconnection").exists())
        self.assertIn(["/usr/bin/nmcli", "device", "disconnect", "wlan0"], self.calls)
        self.assertFalse(any("password123" in " ".join(call) for call in self.calls))

    def test_keyfile_escapes_credentials_and_ssid(self):
        settings = dict(DEFAULT_RADIO, apSSID="a;[b]", apPassword=" pa\\ssword ")
        self.manager.keyfile("AP", settings)
        text = (self.manager.network / "PiControllerAP.nmconnection").read_text()
        self.assertIn("ssid=97;59;91;98;93;", text)
        self.assertIn("psk=\\spa\\\\ssword\\s", text)

    def update_environment(self):
        payload = build_bundle(ROOT, "0.2.0-test")
        bundle = validate_bundle(payload)
        previous = self.manager.install / "releases" / ("a" * 64)
        previous.mkdir(parents=True)
        self.manager.state.mkdir()
        (self.manager.state / "pending-update.zip").write_bytes(payload)
        real_resolve = Path.resolve
        current = self.manager.install / "current"
        def resolve(path, *args, **kwargs):
            if path == current: return previous
            return real_resolve(path, *args, **kwargs)
        return bundle, previous, resolve

    def test_update_health_failure_restores_previous(self):
        bundle, previous, resolve = self.update_environment()
        with patch.object(Path, "resolve", resolve), patch.object(self.manager, "switch") as switch:
            with self.assertRaises(ValueError): self.manager.apply_update(validate_bundle, lambda ident: False)
        self.assertEqual(switch.call_args.args[0], previous)
        record = json.loads((self.manager.state / "update-status.json").read_text())
        self.assertEqual(record["phase"], "rolled-back")
        self.assertEqual(record["candidate"], bundle["releaseId"])

    def test_update_commit_and_guard_noop(self):
        bundle, previous, resolve = self.update_environment()
        with patch.object(Path, "resolve", resolve), patch.object(self.manager, "switch") as switch:
            result = self.manager.apply_update(validate_bundle, lambda ident: ident == bundle["releaseId"])
            self.manager.recover_update()
        self.assertEqual(switch.call_count, 1)
        self.assertEqual(result["phase"], "committed")
        self.assertFalse((self.manager.state / "pending-update.zip").exists())

    def test_boot_recovery_restores_uncommitted_update(self):
        bundle, previous, resolve = self.update_environment()
        status = dict(phase="applying", candidate=bundle["releaseId"], previous=previous.name)
        (self.manager.state / "update-status.json").write_text(json.dumps(status))
        with patch.object(self.manager, "switch") as switch:
            self.manager.recover_update("b" * 64)
            self.assertFalse(switch.called)
            self.manager.recover_update()
            self.assertEqual(switch.call_args.args[0], previous)

    def test_gadget_setup_preserves_boot_arguments_and_is_idempotent(self):
        config, line = boot_text("[all]\n", "root=PARTUUID=123 quiet modules-load=existing\n")
        self.assertIn("root=PARTUUID=123", line)
        self.assertIn("modules-load=existing,dwc2,g_serial", line)
        self.assertEqual(boot_text(config, line), (config, line))
        with self.assertRaises(ValueError): boot_text("dtoverlay=dwc2,dr_mode=host", "quiet")
        with self.assertRaises(ValueError): boot_text("", "modules-load=dwc2,g_ether")
        with self.assertRaises(ValueError): boot_text("[pi4]\ndtoverlay=dwc2", "quiet")

    def test_pin_ownership(self):
        for overrides in ({"indicators": True, "dispenser_pin": 23}, {"hardware_profile": "stepper_dac", "interlock_pin": 18}, {"hardware_profile": "stepper_dac", "stepper_pins": [18, 18, 20, 21]}):
            with self.assertRaises(ValueError): validate(dict(DEFAULTS, **overrides))


class UpdateHTTPTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        from aiohttp.test_utils import TestClient, TestServer
        from bridge.server import create_app
        self.directory = tempfile.TemporaryDirectory()
        self.token = "private-test-token-12345678"
        self.controller = Controller(dict(DEFAULTS, data_dir=self.directory.name, update_token=self.token), watchdog=False)
        self.client = TestClient(TestServer(create_app(self.controller)))
        await self.client.start_server()

    async def asyncTearDown(self):
        await self.client.close(); self.directory.cleanup()

    async def test_upload_token_validation_checksum_and_safe_simulation(self):
        payload = build_bundle(ROOT, "0.2.0-test")
        headers = {"Content-Type": "application/zip", "X-Update-Token": self.token}
        self.controller.execute("Arm")
        self.controller.execute("Dispense:1000")
        response = await self.client.post("/api/ota", data=payload, headers=headers)
        self.assertEqual(response.status, 202)
        self.assertEqual((await response.json())["phase"], "validated-simulation")
        self.assertFalse(self.controller.dispenser.state()["armed"])
        self.assertFalse(self.controller.updates.busy)
        headers["X-Update-SHA256"] = "0" * 64
        self.assertEqual((await self.client.post("/api/ota", data=payload, headers=headers)).status, 400)

    async def test_malformed_bundle_and_bin_rejected_without_server_error(self):
        headers = {"Content-Type": "application/zip", "X-Update-Token": self.token}
        response = await self.client.post("/api/ota", data=b"bad zip", headers=headers)
        self.assertEqual(response.status, 400)
        self.assertFalse(self.controller.updates.busy)
        self.assertEqual((await self.client.post("/api/ota", data=b"bad", headers={"X-Update-Token": "wrong"})).status, 403)

    async def test_radio_gate_and_captive_redirect(self):
        self.controller.radio.wifi_enabled = False
        self.assertEqual((await self.client.get("/api/state")).status, 503)
        self.assertEqual((await self.client.get("/api/ping")).status, 200)
        response = await self.client.get("/generate_204", allow_redirects=False)
        self.assertEqual(response.status, 302)
        self.assertEqual(response.headers["Location"], "/")
