#!/usr/bin/env python3
"""Provider JSON on stdin -> ESP32 /api/position, or unsigned MAVLink bench feed.

No XAG SDK is assumed. The provider must supply the aircraft measurement time,
not the bridge receive time. Requires Python 3.10+, no third-party packages.
"""
import argparse
import json
import math
import socket
import struct
import sys
import threading
import time
import urllib.error
import urllib.request


def parse_sample(line):
    value = json.loads(line)
    lat = float(value["latitude"])
    lon = float(value["longitude"])
    accuracy = float(value["accuracyMeters"])
    measured = float(value["measuredAtUnixMs"])
    if not all(math.isfinite(v) for v in (lat, lon, accuracy, measured)):
        raise ValueError("non-finite measurement")
    if not (-90 <= lat <= 90 and -180 <= lon <= 180 and (accuracy == -1 or 0 <= accuracy <= 100000)):
        raise ValueError("measurement out of range")
    return lat, lon, accuracy, measured


def position_body(sample, now_ms):
    lat, lon, accuracy, measured = sample
    age = math.ceil(now_ms - measured)
    if age < 0 or age > 3000:
        raise ValueError("measurement must be no more than 3 seconds old and not in the future")
    return f"{lat:.7f},{lon:.7f},{accuracy:.3f},{age}".encode("ascii")


def mavlink_frame(message_id, payload, sequence, system, component):
    """MAVLink 2, common.xml CRC extras, independent bitwise X25 CRC."""
    payload = payload.rstrip(b"\0") or b"\0"
    header = bytes((len(payload), 0, 0, sequence, system, component)) + message_id.to_bytes(3, "little")
    crc = 0xFFFF
    for byte in header + payload + bytes(({24: 24, 33: 104}[message_id],)):
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x8408 if crc & 1 else 0)
    return b"\xfd" + header + payload + struct.pack("<H", crc)


def mavlink_position(sample, boot_ms, sequence, system=1, component=1):
    lat, lon, accuracy, measured = sample
    gps = bytearray(52)
    struct.pack_into("<QiiiHHHHBB", gps, 0, int(measured * 1000), round(lat * 1e7),
                     round(lon * 1e7), 0, 65535, 65535, 65535, 65535, 3, 10)
    if accuracy >= 0:
        struct.pack_into("<I", gps, 34, max(1, round(accuracy * 1000)))
    global_position = struct.pack("<IiiiihhhH", boot_ms, round(lat * 1e7), round(lon * 1e7),
                                  0, 0, 0, 0, 0, 65535)
    return (mavlink_frame(24, bytes(gps), sequence, system, component) +
            mavlink_frame(33, global_position, (sequence + 1) % 256, system, component))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", help="ESP32 HTTP base URL, or station IP with --mavlink")
    parser.add_argument("--mavlink", action="store_true", help="send a bench MAVLink UDP fixture")
    parser.add_argument("--port", type=int, default=14550)
    parser.add_argument("--system", type=int, default=1)
    parser.add_argument("--component", type=int, default=1)
    args = parser.parse_args()
    if not (1 <= args.port <= 65535 and 1 <= args.system <= 255 and 1 <= args.component <= 255):
        parser.error("invalid port or system/component ID")
    latest = [None]
    lock = threading.Lock()

    def read_provider():
        for line in sys.stdin:
            try:
                if len(line) > 4096:
                    raise ValueError("provider line too long")
                sample = parse_sample(line)
            except (ValueError, KeyError, TypeError) as error:
                print(f"Invalid provider sample: {error}", file=sys.stderr)
                sample = None
            with lock:
                latest[0] = sample

    threading.Thread(target=read_provider, daemon=True).start()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM) if args.mavlink else None
    epoch_ms = time.time() * 1000 - 1
    sequence = 0
    last_error = None
    try:
        while True:
            with lock:
                sample = latest[0]
            try:
                if sample is None:
                    raise ValueError("waiting for a valid aircraft measurement")
                body = position_body(sample, time.time() * 1000)
                if sock:
                    boot_ms = max(1, int(sample[3] - epoch_ms)) & 0xFFFFFFFF
                    sock.sendto(mavlink_position(sample, boot_ms, sequence, args.system, args.component),
                                (args.target, args.port))
                    sequence = (sequence + 2) % 256
                else:
                    request = urllib.request.Request(args.target.rstrip("/") + "/api/position", body,
                                                     {"Content-Type": "text/plain"}, method="POST")
                    with urllib.request.urlopen(request, timeout=1) as response:
                        if response.status != 200:
                            raise ValueError(f"position endpoint returned {response.status}")
                last_error = None
            except (ValueError, OSError, urllib.error.URLError) as error:
                # Stop transmitting on lost/stale fix; controller's 3-second
                # freshness window expires without refreshing cached telemetry.
                message = str(error)
                if message != last_error:
                    print(message, file=sys.stderr)
                    last_error = message
            time.sleep(0.2)
    except KeyboardInterrupt:
        pass
    finally:
        if sock:
            sock.close()


if __name__ == "__main__":
    main()
