#ifndef ESP32_STEPPER_DUAL_DAC_LINEASSEMBLER_H
#define ESP32_STEPPER_DUAL_DAC_LINEASSEMBLER_H

#include <Arduino.h>

// Converts arbitrary serial/BLE chunks into newline-delimited commands. GO HERE
// to change framing rules or the maximum interactive command length.
class LineAssembler {
 public:
  static constexpr size_t MAX_LINE_LENGTH = 384;

  LineAssembler() : overflowed_(false) {
    buffer_.reserve(MAX_LINE_LENGTH);
  }

  template <typename LineCallback, typename ErrorCallback>
  void push(char value, LineCallback onLine, ErrorCallback onError) {
    if (value == '\r') {
      return;
    }

    if (value == '\n') {
      if (overflowed_) {
        onError("input line exceeded 384 bytes");
      } else {
        buffer_.trim();
        if (buffer_.length() > 0) {
          onLine(buffer_);
        }
      }
      clear();
      return;
    }

    if (overflowed_) {
      return;
    }

    if (buffer_.length() >= MAX_LINE_LENGTH) {
      overflowed_ = true;
      return;
    }

    buffer_ += value;
  }

  template <typename LineCallback, typename ErrorCallback>
  void flushPending(LineCallback onLine, ErrorCallback onError) {
    if (overflowed_) {
      onError("input line exceeded 384 bytes");
      clear();
      return;
    }

    buffer_.trim();
    if (buffer_.length() > 0) {
      onLine(buffer_);
    }
    clear();
  }

  bool hasPending() const {
    return overflowed_ || buffer_.length() > 0;
  }

  void clear() {
    buffer_ = "";
    overflowed_ = false;
  }

 private:
  String buffer_;
  bool overflowed_;
};

#endif  // ESP32_STEPPER_DUAL_DAC_LINEASSEMBLER_H
