#include "CommandDispatcher.h"

#include <string.h>

#include "CommandRouter.h"

namespace {

void copyCommandTextBounded(char *destination, size_t capacity, const String &value) {
  size_t length = value.length();
  if (length >= capacity) {
    length = capacity - 1;
  }
  memcpy(destination, value.c_str(), length);
  destination[length] = '\0';
}


bool isEmergencyCommand(String line) {
  line.trim();
  return line.equalsIgnoreCase("StopAll");
}

uint16_t countCommandLines(const String &body, bool &valid) {
  valid = true;
  uint16_t count = 0;
  unsigned int start = 0;
  const unsigned int bodyLength = static_cast<unsigned int>(body.length());

  while (start <= bodyLength) {
    const int newline = body.indexOf('\n', start);
    String line = newline >= 0
      ? body.substring(start, static_cast<unsigned int>(newline))
      : body.substring(start);
    line.trim();

    if (line.length() > 0) {
      if (line.length() > AppConfig::COMMAND_MAX_BYTES) {
        valid = false;
        return 0;
      }
      count++;
    }

    if (newline < 0) {
      break;
    }
    start = static_cast<unsigned int>(newline) + 1U;
  }

  return count;
}

}  // namespace

CommandDispatcher::CommandDispatcher(
  EventBus &events,
  CommandRouter &router,
  StatusIndicators &indicators
) : events_(events),
    router_(router),
    indicators_(indicators),
    queue_(nullptr),
    submitMutex_(nullptr),
    acceptedCount_(0),
    rejectedCount_(0) {}


bool CommandDispatcher::begin() {
  if (queue_ != nullptr && submitMutex_ != nullptr) {
    return true;
  }

  queue_ = xQueueCreate(AppConfig::COMMAND_QUEUE_CAPACITY, sizeof(QueuedCommand));
  submitMutex_ = xSemaphoreCreateMutex();
  if (queue_ == nullptr || submitMutex_ == nullptr) {
    if (queue_ != nullptr) {
      vQueueDelete(queue_);
      queue_ = nullptr;
    }
    if (submitMutex_ != nullptr) {
      vSemaphoreDelete(submitMutex_);
      submitMutex_ = nullptr;
    }
    events_.publish(
      EventLevel::ERROR,
      "could not create unified command queue or admission mutex",
      CommandSource::INTERNAL
    );
    return false;
  }

  events_.publish(
    EventLevel::STATUS,
    "unified RTOS command queue ready capacity=" +
      String(AppConfig::COMMAND_QUEUE_CAPACITY),
    CommandSource::INTERNAL
  );
  return true;
}

void CommandDispatcher::service(uint8_t budget) {
  if (queue_ == nullptr) {
    return;
  }

  QueuedCommand command{};
  uint8_t processed = 0;
  while (processed < budget && xQueuePeek(queue_, &command, 0) == pdTRUE) {
    // Preserve FIFO order and apply backpressure instead of dropping a command
    // when a subsystem's private queue is temporarily full. emergency-stop commands stay
    // admissible because the motor controller treats them as queue barriers.
    if (!router_.canAccept(String(command.line))) {
      break;
    }
    if (xQueueReceive(queue_, &command, 0) != pdTRUE) {
      break;
    }
    router_.submit(command.source, String(command.line), String(command.requestId));
    processed++;
  }
}


void CommandDispatcher::clearPending() {
  if (queue_ == nullptr) {
    return;
  }

  // Every producer uses submitMutex_ for admission, so taking the same mutex
  // makes a safety flush atomic relative to a multi-line HTTP batch.
  if (lockSubmitter(portMAX_DELAY)) {
    xQueueReset(queue_);
    unlockSubmitter();
  }
}

bool CommandDispatcher::submit(
  CommandSource source,
  const String &line,
  const String &requestId
) {
  indicators_.noteCommandReceived();

  if (queue_ == nullptr && !begin()) {
    return false;
  }

  if (isEmergencyCommand(line)) {
    events_.publish(
      EventLevel::WARNING,
      "[QUEUED] emergency command accepted for immediate execution",
      source,
      requestId
    );
    router_.submit(source, line, requestId);
    acceptedCount_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  QueuedCommand command{};
  if (!prepare(command, source, line, requestId)) {
    return false;
  }

  if (!lockSubmitter()) {
    reject(source, requestId, "command intake busy; retry or resend command");
    return false;
  }
  const BaseType_t queued = xQueueSend(queue_, &command, 0);
  unlockSubmitter();

  if (queued != pdTRUE) {
    reject(source, requestId, "command queue full; retry or resend command");
    return false;
  }

  acceptedCount_.fetch_add(1, std::memory_order_relaxed);
  events_.publish(
    EventLevel::INFO,
    "[QUEUED] command accepted depth=" +
      String(uxQueueMessagesWaiting(queue_)) + "/" +
      String(AppConfig::COMMAND_QUEUE_CAPACITY),
    source,
    requestId
  );
  return true;
}

bool CommandDispatcher::submitBatch(
  CommandSource source,
  const String &body,
  const String &requestId,
  uint16_t &acceptedLines
) {
  indicators_.noteCommandReceived();
  acceptedLines = 0;

  if (queue_ == nullptr && !begin()) {
    return false;
  }

  String normalized = body;
  normalized.replace("\r", "");

  bool valid = false;
  const uint16_t lineCount = countCommandLines(normalized, valid);
  if (!valid) {
    reject(source, requestId, "a command line exceeded 384 bytes");
    return false;
  }
  if (lineCount == 0) {
    reject(source, requestId, "no command lines found");
    return false;
  }
  if (lineCount == 1 && isEmergencyCommand(normalized)) {
    events_.publish(
      EventLevel::WARNING,
      "[QUEUED] emergency command accepted for immediate execution",
      source,
      requestId
    );
    router_.submit(source, normalized, requestId);
    acceptedCount_.fetch_add(1, std::memory_order_relaxed);
    acceptedLines = 1;
    return true;
  }
  if (lineCount > AppConfig::COMMAND_QUEUE_CAPACITY) {
    reject(source, requestId, "command batch exceeds queue capacity");
    return false;
  }

  // Hold one admission mutex across the capacity check and every enqueue. This
  // makes HTTP batches all-or-nothing relative to USB, BLE, and SPP producers.
  if (!lockSubmitter()) {
    reject(source, requestId, "command intake busy; retry the same request ID");
    return false;
  }
  if (uxQueueSpacesAvailable(queue_) < lineCount) {
    unlockSubmitter();
    reject(source, requestId, "command queue busy; retry the same request ID");
    return false;
  }

  unsigned int start = 0;
  const unsigned int normalizedLength = static_cast<unsigned int>(normalized.length());
  while (start <= normalizedLength) {
    const int newline = normalized.indexOf('\n', start);
    String line = newline >= 0
      ? normalized.substring(start, static_cast<unsigned int>(newline))
      : normalized.substring(start);
    line.trim();

    if (line.length() > 0) {
      QueuedCommand command{};
      if (!prepare(command, source, line, requestId)) {
        unlockSubmitter();
        return false;
      }
      if (xQueueSend(queue_, &command, 0) != pdTRUE) {
        unlockSubmitter();
        reject(source, requestId, "command queue changed while accepting batch");
        return false;
      }
      acceptedCount_.fetch_add(1, std::memory_order_relaxed);
      acceptedLines++;
    }

    if (newline < 0) {
      break;
    }
    start = static_cast<unsigned int>(newline) + 1U;
  }

  unlockSubmitter();
  events_.publish(
    EventLevel::INFO,
    "[QUEUED] batch accepted lines=" + String(acceptedLines) +
      " depth=" + String(uxQueueMessagesWaiting(queue_)) + "/" +
      String(AppConfig::COMMAND_QUEUE_CAPACITY),
    source,
    requestId
  );
  return true;
}

bool CommandDispatcher::hasPendingCommands() const {
  return queue_ != nullptr && uxQueueMessagesWaiting(queue_) > 0;
}

String CommandDispatcher::stateJson() const {
  const UBaseType_t waiting = queue_ != nullptr ? uxQueueMessagesWaiting(queue_) : 0;
  const UBaseType_t spaces = queue_ != nullptr ? uxQueueSpacesAvailable(queue_) : 0;

  String json = "{";
  json += "\"waiting\":" + String(waiting);
  json += ",\"spaces\":" + String(spaces);
  json += ",\"capacity\":" + String(AppConfig::COMMAND_QUEUE_CAPACITY);
  json += ",\"accepted\":" + String(acceptedCount_.load(std::memory_order_relaxed));
  json += ",\"rejected\":" + String(rejectedCount_.load(std::memory_order_relaxed));
  json += "}";
  return json;
}

String CommandDispatcher::webStateJson() const {
  const UBaseType_t waiting = queue_ != nullptr ? uxQueueMessagesWaiting(queue_) : 0;
  String json;
  json.reserve(48);
  json = "{\"waiting\":" + String(waiting);
  json += ",\"capacity\":" + String(AppConfig::COMMAND_QUEUE_CAPACITY);
  json += "}";
  return json;
}

bool CommandDispatcher::commandThunk(
  void *context,
  CommandSource source,
  const String &line,
  const String &requestId
) {
  return context != nullptr &&
    static_cast<CommandDispatcher *>(context)->submit(source, line, requestId);
}

bool CommandDispatcher::batchThunk(
  void *context,
  CommandSource source,
  const String &body,
  const String &requestId,
  uint16_t &acceptedLines
) {
  if (context == nullptr) {
    acceptedLines = 0;
    return false;
  }
  return static_cast<CommandDispatcher *>(context)->submitBatch(
    source,
    body,
    requestId,
    acceptedLines
  );
}

bool CommandDispatcher::prepare(
  QueuedCommand &destination,
  CommandSource source,
  const String &line,
  const String &requestId
) {
  String normalized = line;
  normalized.trim();

  if (normalized.length() == 0 || normalized.length() > AppConfig::COMMAND_MAX_BYTES) {
    reject(source, requestId, "invalid command length");
    return false;
  }

  destination.source = source;
  copyCommandTextBounded(destination.requestId, sizeof(destination.requestId), requestId);
  copyCommandTextBounded(destination.line, sizeof(destination.line), normalized);
  return true;
}

bool CommandDispatcher::lockSubmitter(TickType_t timeout) {
  return submitMutex_ != nullptr && xSemaphoreTake(submitMutex_, timeout) == pdTRUE;
}

void CommandDispatcher::unlockSubmitter() {
  if (submitMutex_ != nullptr) {
    xSemaphoreGive(submitMutex_);
  }
}

void CommandDispatcher::reject(
  CommandSource source,
  const String &requestId,
  const String &reason
) {
  rejectedCount_.fetch_add(1, std::memory_order_relaxed);
  events_.publish(
    EventLevel::ERROR,
    reason,
    source,
    requestId
  );
}
