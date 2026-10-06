import asyncio
import copy
import json

from .config import validate
from .radio import validate_radio, PROFILES
from .store import Store


class SelfTests:
    def __init__(self, controller):
        self.controller = controller
        self.store = Store(controller.config["data_dir"], "selftest.json")
        self.report = self.store.load_generic(dict(phase="idle", current=0, total=0, results=[], snapshot=None))
        if not isinstance(self.report, dict) or set(self.report) != {"phase", "current", "total", "results", "snapshot"}:
            raise ValueError("invalid saved self-test checkpoint")
        r = self.report
        if (r["phase"] not in {"idle", "running", "paused", "complete", "failed", "aborted"}
                or type(r["current"]) is not int or type(r["total"]) is not int
                or not 0 <= r["current"] <= r["total"] <= 12
                or not isinstance(r["results"], list) or len(r["results"]) != r["current"]
                or any(not isinstance(item, dict) or set(item) != {"check", "result", "detail"}
                       or item["check"] != index + 1 or item["result"] not in {"pass", "fail", "skip"}
                       or not isinstance(item["detail"], str) for index, item in enumerate(r["results"]))):
            raise ValueError("invalid self-test progress")
        if r["snapshot"] is not None:
            snapshot = r["snapshot"]
            if not isinstance(snapshot, dict) or set(snapshot) != {"radio", "dispenser"}:
                raise ValueError("invalid self-test snapshot")
            validate_radio(snapshot["radio"])
            controller.dispenser.validate(snapshot["dispenser"])
        if self.report["phase"] == "running":
            self.report["phase"] = "paused"
        self.active = False
        self.task = None

    def state(self):
        results = self.report["results"]
        return dict(active=self.active, phase=self.report["phase"], current=self.report["current"], total=self.report["total"],
                    **{level: sum(item["result"] == level for item in results) for level in ("pass", "fail", "skip")},
                    lastResult=results[-1]["detail"] if results else "No run recorded", results=results)

    def save(self):
        self.store.save_generic(self.report)

    def restore(self):
        snapshot = self.report["snapshot"]
        self.controller.stop_all("self-test restore", abort_test=False)
        if snapshot:
            self.controller.radio.settings = copy.deepcopy(snapshot["radio"])
            self.controller.dispenser.apply(snapshot["dispenser"])
        self.report["snapshot"] = None

    def start(self, resume=False):
        if self.active or self.controller.radio.busy or self.controller.updates.busy:
            raise ValueError("administrative operation is active")
        self.controller.routines.require_idle()
        self.controller.dispenser.require_disarmed()
        if self.controller.advanced and (self.controller.advanced.remaining or self.controller.advanced.dac_on or self.controller.advanced.digital_on):
            raise ValueError("turn off advanced outputs before self-test")
        if resume:
            if self.report["phase"] != "paused" or not self.report["snapshot"]:
                raise ValueError("no resumable self-test checkpoint")
        else:
            self.report = dict(phase="running", current=0, total=12, results=[],
                               snapshot=dict(radio=copy.deepcopy(self.controller.radio.settings), dispenser=dict(self.controller.dispenser.settings)))
        self.report["phase"] = "running"
        self.active = True
        self.save()
        self.task = asyncio.create_task(self.run())

    def check(self, index):
        c = self.controller
        if index == 0:
            validate(copy.deepcopy(c.config)); return "pass", "installation config and pin ownership valid"
        if index == 1:
            state = c.dispenser.state()
            assert not state["armed"] and not state["dispensing"]
            if c.advanced: assert not c.advanced.remaining and not c.advanced.dac_on and not c.advanced.digital_on
            return "pass", "all outputs inactive"
        if index == 2:
            if c.config["hardware_profile"] != "dispenser": return "skip", "arming test applies to dispenser profile"
            if c.dispenser.state()["interlockOpen"]: return "skip", "interlock open; arming check skipped"
            c.dispenser.arm()
            assert not c.dispenser.state()["dispensing"]
            c.dispenser.disarm()
            return "pass", "Arm then Disarm without activating output"
        if index == 3:
            assert c.queue.maxsize == 8
            return "pass", "bounded command queue available"
        if index == 4:
            probe = Store(c.config["data_dir"], "selftest-probe.json")
            try:
                probe.save_generic(dict(value="roundtrip"))
                assert probe.load_generic(None) == dict(value="roundtrip")
            finally:
                probe.path.unlink(missing_ok=True)
            return "pass", "atomic checksummed storage roundtrip"
        if index == 5:
            validate_radio(c.radio.settings)
            assert c.radio.active in PROFILES
            return "pass", "radio settings and boot profile valid"
        if index == 6:
            for record in c.routines.library.values(): c.routines.validate(record)
            return "pass", "routine library and command allowlist valid"
        if index == 7:
            for settings in c.profiles.values(): c.dispenser.validate(settings)
            return "pass", "payload profile library valid"
        if index == 8:
            if not c.radio.ble_enabled: return "skip", "BLE disabled"
            assert c.ble_running, "BLE registration not running"
            return "pass", "BLE service registered"
        if index == 9:
            if not c.radio.spp_enabled: return "skip", "SPP disabled"
            assert c.radio.transport_manager and c.radio.transport_manager.spp
            return "pass", "SPP profile registered"
        if index == 10:
            if not c.indicators.enabled: return "skip", "indicators disabled"
            assert c.indicators.thread.is_alive()
            return "pass", "indicator worker alive"
        return "skip", "physical actuation, RF transitions and host reboot require supervised hardware tests"

    async def run(self):
        try:
            while self.report["current"] < self.report["total"]:
                index = self.report["current"]
                try:
                    result, detail = self.check(index)
                except Exception as error:
                    result, detail = "fail", str(error) or "check failed"
                self.report["results"].append(dict(check=index + 1, result=result, detail=detail))
                self.report["current"] += 1
                self.save()
                self.controller.publish(f"[SELFTEST] {index + 1}/12 {result}: {detail}", level="status")
                await asyncio.sleep(.05)
            self.restore()
            self.report["phase"] = "complete"
            self.save()
        except asyncio.CancelledError:
            raise
        except Exception as error:
            self.controller.stop_all("self-test failed", abort_test=False)
            self.report["phase"] = "failed"
            self.controller.publish("self-test failed: " + str(error), level="error")
        finally:
            self.active = False

    def abort(self):
        if self.task:
            self.task.cancel()
        self.active = False
        self.restore()
        self.report["phase"] = "aborted"
        self.save()

    def handle(self, cmd):
        if cmd not in {"SELFTESTSTART", "SELFTESTRESUME", "SELFTESTABORT", "SELFTESTCLEAR", "SELFTESTSTATUS"}:
            return None
        if cmd == "SELFTESTSTART": self.start()
        elif cmd == "SELFTESTRESUME": self.start(True)
        elif cmd == "SELFTESTABORT": self.abort()
        elif cmd == "SELFTESTCLEAR":
            if self.active: raise ValueError("abort self-test before clearing it")
            self.report = dict(phase="idle", current=0, total=0, results=[], snapshot=None)
            self.save()
        return json.dumps(self.state(), separators=(",", ":"))

    async def close(self):
        if self.task and not self.task.done():
            self.task.cancel()
            if self.active:
                self.report["phase"] = "paused"
            self.active = False
            self.save()
            await asyncio.gather(self.task, return_exceptions=True)
