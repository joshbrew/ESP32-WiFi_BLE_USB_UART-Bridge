"""Bounded MAVLink position receiver and explicit local test encoder."""
import struct
import math


def test_position(latitude, longitude, accuracy, boot_ms, system, component):
    """Encode only the two position reports for the explicit local UDP test."""
    def coordinate(value):
        return int(math.copysign(math.floor(abs(value) * 1e7 + .5), value))
    latitude, longitude = coordinate(latitude), coordinate(longitude)
    gps = bytearray(38)
    struct.pack_into("<ii", gps, 8, latitude, longitude)
    gps[28] = 3
    struct.pack_into("<I", gps, 34, 0 if accuracy < 0 else max(1, int(accuracy * 1000)))
    position = struct.pack("<IiiiihhhH", boot_ms, latitude, longitude, 0, 0, 0, 0, 0, 0)
    frames = []
    for sequence, ident, payload, extra in ((0, 24, gps, 24), (1, 33, position, 104)):
        header = bytes((len(payload), 0, 0, sequence, system, component)) + ident.to_bytes(3, "little")
        data = header + payload
        frames.append(b"\xfd" + data + crc_x25(data + bytes((extra,))).to_bytes(2, "little"))
    return b"".join(frames)


def crc_x25(data):
    crc = 0xFFFF
    for byte in data:
        tmp = (byte ^ (crc & 255))
        tmp = (tmp ^ (tmp << 4)) & 255
        crc = ((crc >> 8) ^ (tmp << 8) ^ (tmp << 3) ^ (tmp >> 4)) & 0xFFFF
    return crc


def positions(data):
    """Yield CRC-checked GPS_RAW_INT/GLOBAL_POSITION_INT from whole datagrams.

    Reject signing/unknown incompatibility flags; no signature verifier is
    claimed. MAVLink 2 trailing zeros are restored before field decoding.
    """
    if len(data) > 1024:
        return
    offset = 0
    while offset < len(data):
        start = offset
        offset += 1
        magic = data[start]
        if magic not in (0xFD, 0xFE): continue
        header = 10 if magic == 0xFD else 6
        if start + header > len(data): return
        size = data[start + 1]
        flags = data[start + 2] if magic == 0xFD else 0
        end = start + header + size + 2 + (13 if flags & 1 else 0)
        if end > len(data): continue
        ident = int.from_bytes(data[start + 7:start + 10], "little") if magic == 0xFD else data[start + 5]
        if flags or ident not in (24, 33):
            offset = end; continue
        checksum_at = start + header + size
        checksum = crc_x25(data[start + 1:checksum_at] + bytes((24 if ident == 24 else 104,)))
        if checksum != int.from_bytes(data[checksum_at:checksum_at + 2], "little"): continue
        offset = end
        minimum = (30 if ident == 24 else 28) if magic == 0xFE else (29 if ident == 24 else 1)
        maximum = 52 if ident == 24 else 28
        if not minimum <= size <= maximum: continue
        payload = data[start + header:checksum_at].ljust(52, b"\0")
        system = data[start + (5 if magic == 0xFD else 3)]
        component = data[start + (6 if magic == 0xFD else 4)]
        if ident == 24:
            yield dict(id=ident, system=system, component=component, fixType=payload[28],
                       accuracyMm=struct.unpack_from("<I", payload, 34)[0])
        else:
            boot, latitude, longitude = struct.unpack_from("<Iii", payload)
            yield dict(id=ident, system=system, component=component, bootMs=boot,
                       latitude=latitude / 1e7, longitude=longitude / 1e7)
