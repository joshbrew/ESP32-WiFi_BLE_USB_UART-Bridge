import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("position_bridge", Path(__file__).parents[1] / "examples/position_bridge.py")
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)


class PositionBridgeTests(unittest.TestCase):
    def test_age_preserved(self):
        sample = bridge.parse_sample('{"latitude":37,"longitude":-122,"accuracyMeters":1.5,"measuredAtUnixMs":1000}')
        self.assertEqual(bridge.position_body(sample, 1100), b"37.0000000,-122.0000000,1.500,100")
        with self.assertRaises(ValueError): bridge.position_body(sample, 4001)
        with self.assertRaises(ValueError): bridge.position_body(sample, 999)

    def test_invalid_fix(self):
        for latitude in (91, float("nan")):
            with self.assertRaises(ValueError):
                bridge.parse_sample('{"latitude":' + str(latitude) + ',"longitude":0,"accuracyMeters":1,"measuredAtUnixMs":1000}')

    def test_common_packet_layout(self):
        packet = bridge.mavlink_position((37, -122, 1.5, 1000), 1234, 0)
        self.assertEqual(packet[0], 0xFD)
        self.assertEqual(packet[7], 24)
        self.assertEqual(packet[10 + 28], 3)
        gps_payload = packet[10:10 + packet[1]].ljust(52, b"\0")
        self.assertEqual(int.from_bytes(gps_payload[34:38], "little"), 1500)
        second = 12 + packet[1]
        self.assertEqual(packet[second], 0xFD)
        self.assertEqual(packet[second + 7], 33)
        self.assertEqual(int.from_bytes(packet[second + 10:second + 14], "little"), 1234)
        self.assertEqual(int.from_bytes(packet[second + 18:second + 22], "little", signed=True), -1220000000)


if __name__ == "__main__": unittest.main()
