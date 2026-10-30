import asyncio
from pathlib import Path
import sys
import time
import unittest

sys.path.insert(0, str(Path(__file__).parents[1] / "examples"))
import ble_position_bridge as bridge


class BleBridgeTests(unittest.TestCase):
    def test_chunked_raw_gps(self):
        class Client:
            writes = []

            async def write_gatt_char(self, characteristic, packet, response):
                self.writes.append((characteristic, packet, response))

        client = Client()
        sample = (37, -122, 1.25, time.time() * 1000)
        asyncio.run(bridge.send_position(client, sample, 1234, 8))
        self.assertTrue(all(uuid == bridge.GPS_RX and response and len(packet) <= 20
                            for uuid, packet, response in client.writes))
        packet = b"".join(packet for _, packet, _ in client.writes)
        self.assertEqual(packet, bridge.mavlink_position(sample, 1234, 8))

    def test_stale_measurement_not_written(self):
        class Client:
            async def write_gatt_char(self, *_args, **_kwargs):
                raise AssertionError("stale data sent")

        with self.assertRaises(ValueError):
            asyncio.run(bridge.send_position(Client(), (37, -122, 1, time.time() * 1000 - 5000), 1, 0))


if __name__ == "__main__":
    unittest.main()
