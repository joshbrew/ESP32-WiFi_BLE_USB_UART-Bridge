#ifndef ESP32_STEPPER_DUAL_DAC_BYTERINGBUFFER_H
#define ESP32_STEPPER_DUAL_DAC_BYTERINGBUFFER_H

#include <Arduino.h>

// Fixed-capacity byte queue for nonblocking transport output. GO HERE only to
// change queue behavior; select each transport's capacity in TransportHub.h.
template <size_t Capacity>
class ByteRingBuffer {
 public:
  ByteRingBuffer() : head_(0), tail_(0) {}

  void clear() {
    head_ = 0;
    tail_ = 0;
  }

  size_t size() const {
    return head_ >= tail_ ? head_ - tail_ : Capacity - tail_ + head_;
  }

  size_t freeSpace() const {
    return Capacity - size() - 1;
  }

  bool push(char value) {
    if (freeSpace() == 0) {
      return false;
    }
    buffer_[head_] = value;
    head_ = (head_ + 1) % Capacity;
    return true;
  }

  bool push(const char *data, size_t length) {
    if (data == nullptr || length > freeSpace()) {
      return false;
    }
    for (size_t index = 0; index < length; index++) {
      buffer_[head_] = data[index];
      head_ = (head_ + 1) % Capacity;
    }
    return true;
  }

  bool push(const String &value) {
    return push(value.c_str(), value.length());
  }

  // Copy queued bytes without consuming them. Transport writers use this with
  // discard() so a short USB/SPP write leaves the unwritten suffix queued.
  size_t peek(char *destination, size_t maximum) const {
    if (destination == nullptr || maximum == 0) {
      return 0;
    }
    size_t cursor = tail_;
    size_t count = 0;
    while (cursor != head_ && count < maximum) {
      destination[count++] = buffer_[cursor];
      cursor = (cursor + 1) % Capacity;
    }
    return count;
  }

  void discard(size_t count) {
    const size_t available = size();
    count = min(count, available);
    tail_ = (tail_ + count) % Capacity;
  }

  size_t pop(char *destination, size_t maximum) {
    const size_t count = peek(destination, maximum);
    discard(count);
    return count;
  }

 private:
  char buffer_[Capacity];
  size_t head_;
  size_t tail_;
};

#endif  // ESP32_STEPPER_DUAL_DAC_BYTERINGBUFFER_H
