#ifndef DRONE_GEL_CONTROLLER_MAVLINKPOSITION_H
#define DRONE_GEL_CONTROLLER_MAVLINKPOSITION_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Receive-only subset of MAVLink common: GPS_RAW_INT (24) and
// GLOBAL_POSITION_INT (33). Field order/CRC_EXTRA follow mavlink/c_library_v2.
// Signed packets are rejected: this adapter does not verify MAVLink signatures.
namespace MavlinkPosition {
struct Message {
  uint8_t system, component;
  uint32_t id, bootMs, accuracyMm;
  int32_t latitudeE7, longitudeE7;
  uint8_t fixType;
};
inline uint16_t accumulate(uint16_t crc, uint8_t byte) {
  uint8_t tmp = byte ^ static_cast<uint8_t>(crc);
  tmp ^= tmp << 4;
  return (crc >> 8) ^ (static_cast<uint16_t>(tmp) << 8) ^
    (static_cast<uint16_t>(tmp) << 3) ^ (tmp >> 4);
}
inline uint32_t read32(const uint8_t *data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
    (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}
inline bool next(const uint8_t *bytes, size_t length, size_t &offset, Message &message) {
  while (offset < length) {
    const size_t start = offset++;
    const uint8_t magic = bytes[start];
    if (magic != 0xFD && magic != 0xFE) continue;
    const size_t header = magic == 0xFD ? 10 : 6;
    if (start + header > length) return false;
    const size_t payloadLength = bytes[start + 1];
    const uint8_t flags = magic == 0xFD ? bytes[start + 2] : 0;
    const size_t frameLength = header + payloadLength + 2 + ((flags & 1) ? 13 : 0);
    if (start + frameLength > length) continue;
    const uint32_t id = magic == 0xFD
      ? bytes[start + 7] | (static_cast<uint32_t>(bytes[start + 8]) << 8) |
        (static_cast<uint32_t>(bytes[start + 9]) << 16)
      : bytes[start + 5];
    if (flags || (id != 24 && id != 33)) { offset = start + frameLength; continue; }
    uint16_t crc = 0xFFFF;
    for (size_t i = start + 1; i < start + header + payloadLength; ++i) crc = accumulate(crc, bytes[i]);
    crc = accumulate(crc, id == 24 ? 24 : 104);
    const size_t crcOffset = start + header + payloadLength;
    if (crc != (bytes[crcOffset] | (static_cast<uint16_t>(bytes[crcOffset + 1]) << 8))) continue;
    offset = start + frameLength;
    // MAVLink 2 may trim trailing zero bytes, including common base fields.
    uint8_t payload[255]{};
    memcpy(payload, bytes + start + header, payloadLength);
    message = {};
    message.system = bytes[start + (magic == 0xFD ? 5 : 3)];
    message.component = bytes[start + (magic == 0xFD ? 6 : 4)];
    message.id = id;
    if (id == 24) {
      if (payloadLength < (magic == 0xFE ? 30U : 29U)) continue;
      message.fixType = payload[28];
      message.accuracyMm = read32(payload + 34); // MAVLink 2 h_acc extension; zero = absent.
    } else {
      if (payloadLength < (magic == 0xFE ? 28U : 1U)) continue;
      message.bootMs = read32(payload);
      message.latitudeE7 = static_cast<int32_t>(read32(payload + 4));
      message.longitudeE7 = static_cast<int32_t>(read32(payload + 8));
    }
    return true;
  }
  return false;
}
}  // namespace MavlinkPosition
#endif  // DRONE_GEL_CONTROLLER_MAVLINKPOSITION_H
