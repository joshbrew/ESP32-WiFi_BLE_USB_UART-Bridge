#include "EventBus.h"

#include <string.h>

namespace {

String cleanEventText(const String &value) {
  String cleaned = value;
  cleaned.trim();

  bool unwrapped = true;
  while (unwrapped) {
    unwrapped = false;
    static const char *const wrappers[] = {"CommandController(", "ccFoundError("};
    for (const char *wrapper : wrappers) {
      const size_t prefixLength = strlen(wrapper);
      if (
        cleaned.length() > prefixLength &&
        cleaned.startsWith(wrapper) &&
        cleaned.endsWith(")")
      ) {
        cleaned = cleaned.substring(
          static_cast<unsigned int>(prefixLength),
          cleaned.length() - 1
        );
        cleaned.trim();
        unwrapped = true;
        break;
      }
    }
  }

  return cleaned;
}

void copyEventTextBounded(char *destination, size_t capacity, const String &value) {
  if (capacity == 0) {
    return;
  }
  size_t length = value.length();
  if (length >= capacity) {
    length = capacity - 1;
  }
  memcpy(destination, value.c_str(), length);
  destination[length] = '\0';
}

}  // namespace

EventBus::EventBus()
  : head_(0),
    count_(0),
    nextId_(1),
    mux_(portMUX_INITIALIZER_UNLOCKED) {}

uint32_t EventBus::publish(
  EventLevel level,
  const String &text,
  CommandSource source,
  const String &requestId,
  TransportMask targetMask,
  bool rawPayload
) {
  // Finish all Arduino String work before entering the cross-core critical
  // section. The lock now protects only the fixed-size ring write and counters,
  // keeping motor-task publication latency bounded.
  EventRecord prepared{};
  prepared.timestampMs = millis();
  prepared.level = level;
  prepared.source = source;
  prepared.targetMask = targetMask == TRANSPORT_MASK_DEFAULT
    ? defaultTransportMaskForEvent(source)
    : static_cast<TransportMask>(targetMask & TRANSPORT_MASK_ALL);
  prepared.rawPayload = rawPayload;
  copyEventTextBounded(prepared.requestId, sizeof(prepared.requestId), requestId);
  copyEventTextBounded(prepared.text, sizeof(prepared.text), rawPayload ? text : cleanEventText(text));

  portENTER_CRITICAL(&mux_);
  prepared.id = nextId_++;
  records_[head_] = prepared;
  head_ = static_cast<uint16_t>((head_ + 1) % AppConfig::EVENT_CAPACITY);
  if (count_ < AppConfig::EVENT_CAPACITY) {
    count_++;
  }
  portEXIT_CRITICAL(&mux_);
  return prepared.id;
}

uint32_t EventBus::oldestId() const {
  portENTER_CRITICAL(&mux_);
  const uint32_t value = oldestIdUnlocked();
  portEXIT_CRITICAL(&mux_);
  return value;
}

uint32_t EventBus::latestId() const {
  portENTER_CRITICAL(&mux_);
  const uint32_t value = latestIdUnlocked();
  portEXIT_CRITICAL(&mux_);
  return value;
}

uint16_t EventBus::count() const {
  portENTER_CRITICAL(&mux_);
  const uint16_t value = count_;
  portEXIT_CRITICAL(&mux_);
  return value;
}

bool EventBus::nextAfter(uint32_t &cursor, EventRecord &record, bool &gap) const {
  portENTER_CRITICAL(&mux_);
  gap = false;

  if (count_ == 0) {
    portEXIT_CRITICAL(&mux_);
    return false;
  }

  const uint32_t oldest = oldestIdUnlocked();
  const uint32_t latest = latestIdUnlocked();

  if (cursor + 1 < oldest) {
    cursor = oldest - 1;
    gap = true;
  }

  if (cursor >= latest) {
    portEXIT_CRITICAL(&mux_);
    return false;
  }

  const uint32_t wanted = cursor + 1;
  const uint16_t oldestIndex = static_cast<uint16_t>(
    (head_ + AppConfig::EVENT_CAPACITY - count_) % AppConfig::EVENT_CAPACITY
  );
  const uint16_t offset = static_cast<uint16_t>(wanted - oldest);
  const uint16_t index = static_cast<uint16_t>(
    (oldestIndex + offset) % AppConfig::EVENT_CAPACITY
  );

  record = records_[index];
  cursor = record.id;
  portEXIT_CRITICAL(&mux_);
  return true;
}

uint32_t EventBus::oldestIdUnlocked() const {
  return count_ == 0 ? nextId_ : nextId_ - count_;
}

uint32_t EventBus::latestIdUnlocked() const {
  return count_ == 0 ? 0 : nextId_ - 1;
}
