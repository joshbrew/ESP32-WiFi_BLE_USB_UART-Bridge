"""Keep newly documented ESP32 commands visible to the Pi port's CI."""
import re
import unittest
from pathlib import Path

from bridge.advanced import Advanced
from bridge.core import HELP
from bridge.geo import GeoMission
from bridge.radio import RadioManager


class CommandInventory(unittest.TestCase):
    def test_current_esp32_commands_have_support_or_explicit_exception(self):
        root = Path(__file__).resolve().parents[2]
        supported = {word.split(":", 1)[0].upper() for word in HELP.split()}
        supported |= Advanced.COMMANDS | RadioManager.COMMANDS | GeoMission.COMMANDS
        supported |= {"DISPENSERARM", "DISPENSERDISARM", "DISPENSEROFF", "GPIO26", "BLEWEBHANDOFF", "BLEWEBCANCEL", "INDICATOR16TEST", "INDICATOR17TEST"}
        exceptions = {"WIFILR": "ESP32 proprietary radio; explicit Pi error and relay documented"}
        for project in ("ESP32_Dispenser_Controller", "ESP32_Modular_Command_Transport_Controller"):
            text = (root / project / "COMMANDS.md").read_text(encoding="utf-8")
            if project.endswith("Transport_Controller"): text = text.split("## Commands", 1)[1]
            documented = set()
            for block in re.findall(r"```text\n(.*?)```", text, flags=re.S):
                for line in block.splitlines():
                    match = re.match(r"^([A-Za-z][A-Za-z0-9]*)(?=:|\s|$)", line)
                    if match: documented.add(match.group(1).upper())
            documented |= {token.upper() for token in re.findall(r"`(Geo[A-Za-z]+)(?=:|`)", text)}
            self.assertFalse(documented - supported - set(exceptions), f"unported commands from {project}: {documented - supported - set(exceptions)}")
