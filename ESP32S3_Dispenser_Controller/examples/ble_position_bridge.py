#!/usr/bin/env python3
"""Fresh aircraft JSON on stdin -> MAVLink GPS on the dispenser's BLE channel.

Install bleak on the bridge computer. Input schema and MAVLink encoding are
shared with position_bridge.py. This is a BLE central, not Classic Bluetooth.
"""
import argparse
import asyncio
import contextlib
import sys
import threading
import time

from position_bridge import mavlink_position, parse_sample, position_body

SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
GPS_RX = "6e400004-b5a3-f393-e0a9-e50e24dcca9e"


async def send_position(client, sample, boot_ms, sequence, system=1, component=1):
    packet = mavlink_position(sample, boot_ms, sequence, system, component)
    # Conservative chunks work at the default BLE MTU. A frame may span writes.
    for start in range(0, len(packet), 20):
        position_body(sample, time.time() * 1000)  # Recheck age before each write.
        await client.write_gatt_char(GPS_RX, packet[start:start + 20], response=True)


async def run(args):
    from bleak import BleakClient, BleakScanner
    latest = [None]
    lock = threading.Lock()

    def provider():
        for line in sys.stdin:
            try:
                if len(line) > 4096:
                    raise ValueError("provider line too long")
                sample = parse_sample(line)
            except (ValueError, KeyError, TypeError) as error:
                print(f"Invalid aircraft sample: {error}", file=sys.stderr)
                sample = None
            with lock:
                latest[0] = sample

    threading.Thread(target=provider, daemon=True).start()
    device = await BleakScanner.find_device_by_address(args.address, timeout=10)
    if device is None:
        raise RuntimeError("Controller not found; enable BLE and disconnect other BLE clients")
    async with BleakClient(device, services=[SERVICE]) as client:
        if client.services.get_characteristic(GPS_RX) is None:
            raise RuntimeError("GPS BLE channel missing; update dispenser firmware")
        print("Connected. Select GeoSource:BLE and GeoSave before starting the sequence.", file=sys.stderr)
        sequence, boot_origin, last_measurement = 0, time.time() * 1000 - 1, None
        last_error = None
        while client.is_connected:
            with lock:
                sample = latest[0]
            try:
                if sample is None:
                    raise ValueError("waiting for a valid aircraft measurement")
                position_body(sample, time.time() * 1000)
                if sample[3] != last_measurement:
                    # Measurement time controls boot_ms. Cached fixes cannot
                    # become fresh by repeatedly transmitting them.
                    boot_ms = max(1, int(sample[3] - boot_origin)) & 0xFFFFFFFF
                    await send_position(client, sample, boot_ms, sequence, args.system, args.component)
                    sequence = (sequence + 2) % 256
                    last_measurement = sample[3]
                last_error = None
            except ValueError as error:
                message = str(error)
                if message != last_error:
                    print(message, file=sys.stderr)
                    last_error = message
            await asyncio.sleep(0.2)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("address", help="BLE address (or OS device UUID) of DroneGelBLE")
    parser.add_argument("--system", type=int, default=1)
    parser.add_argument("--component", type=int, default=1)
    args = parser.parse_args()
    if not (1 <= args.system <= 255 and 1 <= args.component <= 255):
        parser.error("system/component must be 1-255")
    with contextlib.suppress(KeyboardInterrupt):
        asyncio.run(run(args))


if __name__ == "__main__":
    main()
