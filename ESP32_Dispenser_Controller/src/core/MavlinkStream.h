#ifndef DRONE_MAVLINK_STREAM_H
#define DRONE_MAVLINK_STREAM_H
#include "MavlinkPosition.h"

// Framing for the dedicated BLE GPS channel. Signed/unsupported packets are
// consumed whole, then rejected by the shared MAVLink decoder.
class MavlinkStream {
 public:
  void reset() { used_ = 0; expected_ = 0; }
  bool push(uint8_t byte, uint32_t receivedAt, uint32_t now,
            MavlinkPosition::Message &message, uint32_t &messageAt) {
    if (used_ && now - startedAt_ > 1000) reset();
    if (!used_) {
      if (byte != 0xFD && byte != 0xFE) return false;
      startedAt_ = receivedAt;
    }
    bytes_[used_++] = byte;
    if (used_ == 2) expected_ = bytes_[1] + (bytes_[0] == 0xFD ? 12 : 8);
    if (used_ == 3 && bytes_[0] == 0xFD && (bytes_[2] & 1)) expected_ += 13;
    if (!expected_ || used_ < expected_) return false;
    size_t offset = 0;
    const bool valid = MavlinkPosition::next(bytes_, used_, offset, message);
    messageAt = startedAt_; reset(); return valid;
  }
 private:
  uint8_t bytes_[280]{};
  uint16_t used_ = 0, expected_ = 0;
  uint32_t startedAt_ = 0;
};
#endif
