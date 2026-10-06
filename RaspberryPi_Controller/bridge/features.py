import asyncio
import json
from pathlib import Path

from .advanced import Advanced
from .indicators import Indicators
from .platform import AdminClient
from .radio import RadioManager
from .selftest import SelfTests
from .store import Store
from .updates import Updates


class Features:
    def __init__(self, controller):
        self.controller = controller
        self.admin_task = None
        self.admin_busy = False
        self.advanced = None
        self.indicators = None
        try:
            controller.admin = AdminClient(controller.config)
            controller.radio = RadioManager(controller, controller.admin)
            controller.updates = Updates(controller, controller.admin)
            self.system_store = Store(controller.config["data_dir"], "system.json")
            self.system = self.system_store.load_generic(dict(production=True))
            if self.system not in ({"production": True}, {"production": False}):
                raise ValueError("invalid saved boot policy")
            if controller.config["hardware_profile"] == "stepper_dac":
                self.advanced = Advanced(controller.config, controller.clock)
            controller.advanced = self.advanced
            self.indicators = Indicators(controller.config, controller.clock)
            controller.indicators = self.indicators
            controller.self_tests = SelfTests(controller)
        except Exception:
            self.close_hardware()
            raise

    @property
    def busy(self):
        c = self.controller
        return self.admin_busy or c.radio.busy or c.updates.busy or c.self_tests.active

    def schedule_admin(self, operation, source, request_id):
        if self.busy:
            raise ValueError("administrative operation already active")
        self.controller.stop_all(operation)
        self.controller.clear_queue()
        self.admin_busy = True
        async def run():
            scheduled = False
            try:
                result = await self.controller.admin.run(operation)
                scheduled = not result.get("simulated", False)
                self.controller.publish("[DONE] " + operation + " scheduled", source, request_id, "status")
            except Exception as error:
                self.controller.publish(str(error), source, request_id, "error")
            finally:
                self.admin_busy = scheduled
        self.admin_task = asyncio.create_task(run())

    def handle(self, line, source, request_id):
        c = self.controller
        cmd, _, value = line.partition(":")
        cmd = cmd.upper()
        message = None
        if cmd in {"PRODUCTIONMODE", "DEBUGMODE", "BOOTMODESTATUS"}:
            if cmd != "BOOTMODESTATUS":
                candidate = dict(production=cmd == "PRODUCTIONMODE")
                self.system_store.save_generic(candidate)
                self.system = candidate
            message = json.dumps(dict(production=self.system["production"], bootMode="production" if self.system["production"] else "debug"))
        elif cmd == "REBOOT":
            self.schedule_admin("reboot", source, request_id)
            message = "[ACK] reboot scheduled; outputs stopped"
        elif cmd.startswith("SELFTEST"):
            message = c.self_tests.handle(cmd)
        elif cmd.startswith("INDICATOR"):
            if cmd == "INDICATORSTATUS": pass
            elif cmd == "INDICATORTEST": c.indicators.test()
            elif cmd in {"INDICATORCONNECTIONTEST", "INDICATOR16TEST"}: c.indicators.test("connection")
            elif cmd in {"INDICATORACTIVITYTEST", "INDICATOR17TEST"}: c.indicators.test("activity")
            else: return False
            message = json.dumps(c.indicators.state())
        elif cmd == "BLEADVERTISE":
            c.radio.schedule(c.radio.runtime(c.radio.wifi_enabled, True, False), source, request_id)
            message = "[ACK] BLE advertising requested"
        elif cmd == "WIFILR":
            raise ValueError("ESP32 proprietary Wi-Fi LR is not supported by Raspberry Pi radios")
        elif cmd in c.radio.COMMANDS:
            if cmd in {"CONFIGSAVE", "CONFIGAPPLY"}:
                from .radio import validate_radio
                validate_radio(c.radio.settings)
            c.radio.handle(cmd, value, source, request_id)
            message = "[ACK] " + cmd
        elif c.advanced and cmd in c.advanced.COMMANDS:
            if cmd not in {"GETMOTORSTATS", "DACSTATUS", "OUTPUTSTATUS", "PRINTSTEPORDER", "STOP", "COILSOFF"} and not (value.upper() == "OFF" and cmd in {"DAC1", "GPIO26", "DACALL", "OUTPUTALL"}):
                c.routines.require_idle()
            if c.routines.running and (cmd in {"STOP", "COILSOFF"} or value.upper() == "OFF"):
                c.routines.stop("manual motor stop")
            c.advanced.handle(line)
            message = json.dumps(c.advanced.state(), separators=(",", ":"))
        if message is None:
            return False
        c.publish(message, source, request_id, "status")
        return True

    def memory(self):
        available = rss = None
        try:
            for line in Path("/proc/meminfo").read_text().splitlines():
                if line.startswith("MemAvailable:"): available = int(line.split()[1]) * 1024
            import os
            rss = int(Path("/proc/self/statm").read_text().split()[1]) * os.sysconf("SC_PAGE_SIZE")
        except (OSError, ValueError, AttributeError):
            pass
        return dict(availableBytes=available, processRssBytes=rss)

    async def start(self):
        if not self.system["production"] and self.controller.self_tests.report["phase"] not in {"paused", "running"}:
            self.controller.self_tests.start()

    def close_hardware(self):
        if self.advanced: self.advanced.close()
        if self.indicators: self.indicators.close()

    async def close(self):
        await self.controller.self_tests.close()
        await self.controller.radio.close()
        if self.admin_task:
            self.admin_task.cancel()
            await asyncio.gather(self.admin_task, return_exceptions=True)
        self.close_hardware()
