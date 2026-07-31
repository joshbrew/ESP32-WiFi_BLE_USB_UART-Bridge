#include "CommandRouter.h"

#include "../config/AppConfig.h"
#include "CommandDispatcher.h"
#include "CommandSelfTest.h"
#include "../util/TextUtil.h"

namespace {

constexpr const char *SYSTEM_PREFERENCES_NAMESPACE = "step-system";
constexpr const char *PRODUCTION_MODE_KEY = "production";

static const char *const HELP_ROWS[] = {
  "Core: Ping Help Status ConfigRead USBStatus HeapStatus StopAll Reboot",
  "Boot/test: ProductionMode DebugMode BootModeStatus SelfTestStart SelfTestStatus SelfTestResume SelfTestAbort SelfTestClear",
  "Indicators: IndicatorStatus IndicatorTest IndicatorConnectionTest IndicatorActivityTest",
  "Send: Send:text SendBLE:text SendWiFi:text SendUSB:text SendSerial:text SendUART:text SendSPP:text SendStatus",
  "Radio: ModeWiFi ModeWiFiBLE ModeWiFiBLEP ModeBLE ModeBTSerial ModeUSB RadioBoot:PROFILE RadioStatus BLEStatus BLEWebHandoff BLEWebCancel",
  "Wi-Fi/config: WiFiMode WiFiTxPower WiFiApSSID WiFiApPassword WiFiStaSSID WiFiStaPassword WiFiStaClear ConfigSave ConfigLoad ConfigApply ConfigDefaults ConfigErase",
};

constexpr size_t HELP_ROW_COUNT = sizeof(HELP_ROWS) / sizeof(HELP_ROWS[0]);

}  // namespace

CommandRouter::CommandRouter(
  EventBus &events,
  DeviceAddon &addon,
  TransportBridge &bridge,
  RadioManager &radios,
  TransportHub &transports,
  StatusIndicators &indicators
) : events_(events),
    addon_(addon),
    bridge_(bridge),
    radios_(radios),
    transports_(transports),
    indicators_(indicators),
    dispatcher_(nullptr),
    selfTest_(nullptr),
    productionMode_(false),
    rebootPending_(false),
    rebootAtMs_(0) {}

void CommandRouter::begin() {
  Preferences preferences;
  if (preferences.begin(SYSTEM_PREFERENCES_NAMESPACE, true)) {
    productionMode_ = preferences.getBool(PRODUCTION_MODE_KEY, false);
    preferences.end();
  }

  events_.publish(
    EventLevel::STATUS,
    "[CONFIG] bootMode=" + bootModeText() +
      " startupTests=" + String(productionMode_ ? "skip" : "run") + " source=saved-or-default",
    CommandSource::INTERNAL
  );
}

void CommandRouter::attachDispatcher(CommandDispatcher *dispatcher) {
  dispatcher_ = dispatcher;
}

void CommandRouter::attachSelfTest(CommandSelfTest *selfTest) {
  selfTest_ = selfTest;
}

bool CommandRouter::canAccept(const String &line) const {
  const String command = normalize(line);
  return command.length() == 0 || addon_.canAcceptCommand(command);
}

void CommandRouter::submit(
  CommandSource source,
  const String &line,
  const String &requestId
) {
  const String command = normalize(line);
  if (command.length() == 0) {
    return;
  }

  publish(
    EventLevel::INFO,
    source,
    requestId,
    "[RUN] " + TextUtil::redactedCommand(command)
  );

  const bool emergencyStop = command.equalsIgnoreCase("StopAll");
  if (rebootPending_ && !emergencyStop) {
    error(source, requestId, "controller reboot is pending; command ignored");
    return;
  }

  if (command.equalsIgnoreCase("Ping")) {
    publish(
      EventLevel::STATUS,
      source,
      requestId,
      "PONG uptimeMs=" + String(millis()) + " freeHeap=" + String(ESP.getFreeHeap())
    );
    return;
  }

  if (command.equalsIgnoreCase("Help")) {
    publishHelp(source, requestId);
    return;
  }

  if (command.equalsIgnoreCase("Status")) {
    publishStatus(source, requestId);
    return;
  }

  if (command.equalsIgnoreCase("ConfigRead")) {
    publishConfig(source, requestId);
    return;
  }

  if (command.equalsIgnoreCase("BootModeStatus")) {
    publishBootMode(source, requestId, "readback");
    return;
  }

  if (command.equalsIgnoreCase("ProductionMode")) {
    if (setProductionMode(true)) {
      publishBootMode(source, requestId, "saved and verified; effective on next boot");
    } else {
      error(source, requestId, "could not persist production boot mode");
    }
    return;
  }

  if (command.equalsIgnoreCase("DebugMode")) {
    if (setProductionMode(false)) {
      publishBootMode(source, requestId, "saved and verified; effective on next boot");
    } else {
      error(source, requestId, "could not persist debug boot mode");
    }
    return;
  }

  if (command.equalsIgnoreCase("IndicatorTest")) {
    if (!AppConfig::ENABLE_STATUS_INDICATORS) {
      error(source, requestId, "status indicators are compiled out");
    } else if (indicators_.startSelfTest()) {
      publish(
        EventLevel::STATUS,
        source,
        requestId,
        "[ACK] indicator self-test started: GPIO" +
          String(AppConfig::PIN_STATUS_CONNECTION) + " pulses twice, then GPIO" +
          String(AppConfig::PIN_STATUS_ACTIVITY) + " pulses three times"
      );
    } else {
      error(source, requestId, "indicator self-test is already active");
    }
    return;
  }

  if (
    command.equalsIgnoreCase("IndicatorConnectionTest") ||
    command.equalsIgnoreCase("Indicator16Test")
  ) {
    if (!AppConfig::ENABLE_STATUS_INDICATORS) {
      error(source, requestId, "status indicators are compiled out");
    } else if (indicators_.startConnectionTest()) {
      publish(
        EventLevel::STATUS,
        source,
        requestId,
        "[ACK] GPIO" + String(AppConfig::PIN_STATUS_CONNECTION) +
          " connection-indicator test started: 4 visible pulses; GPIO" +
          String(AppConfig::PIN_STATUS_ACTIVITY) + " held LOW"
      );
    } else {
      error(source, requestId, "indicator self-test is already active");
    }
    return;
  }

  if (
    command.equalsIgnoreCase("IndicatorActivityTest") ||
    command.equalsIgnoreCase("Indicator17Test")
  ) {
    if (!AppConfig::ENABLE_STATUS_INDICATORS) {
      error(source, requestId, "status indicators are compiled out");
    } else if (indicators_.startActivityTest()) {
      publish(
        EventLevel::STATUS,
        source,
        requestId,
        "[ACK] GPIO" + String(AppConfig::PIN_STATUS_ACTIVITY) +
          " activity-indicator test started: 4 visible pulses; GPIO" +
          String(AppConfig::PIN_STATUS_CONNECTION) + " held LOW"
      );
    } else {
      error(source, requestId, "indicator self-test is already active");
    }
    return;
  }

  if (emergencyStop) {
    if (dispatcher_ != nullptr) {
      dispatcher_->clearPending();
    }
    const bool addonStopped = addon_.stopAll(source, requestId);
    if (addonStopped) {
      publish(EventLevel::WARNING, source, requestId, "[DONE] STOP ALL accepted; addon outputs released or stop queued");
    } else {
      error(source, requestId, "STOP ALL could not fully stop the active addon");
    }
    return;
  }

  if (command.equalsIgnoreCase("Reboot")) {
    if (dispatcher_ != nullptr) {
      dispatcher_->clearPending();
    }
    const bool addonStopped = addon_.stopAll(source, requestId);
    publish(
      addonStopped ? EventLevel::WARNING : EventLevel::ERROR,
      source,
      requestId,
      addonStopped
        ? "[ACK] controller reboot scheduled; active addon stopped"
        : "controller reboot scheduled; active addon stop was incomplete"
    );
    rebootPending_ = true;
    rebootAtMs_ = millis() + 1000;
    return;
  }

  if (command.equalsIgnoreCase("USBStatus")) {
    publish(EventLevel::STATUS, source, requestId, transports_.usbStatusText());
    return;
  }

  if (command.equalsIgnoreCase("HeapStatus")) {
    publish(
      EventLevel::STATUS,
      source,
      requestId,
      "[STATUS] heap free=" + String(ESP.getFreeHeap()) +
        " minimum=" + String(ESP.getMinFreeHeap()) +
        " largestBlock=" + String(ESP.getMaxAllocHeap())
    );
    return;
  }

  if (command.equalsIgnoreCase("IndicatorStatus")) {
    publishChunked(EventLevel::STATUS, source, requestId, "[STATUS] indicators", indicators_.statusText());
    publishChunked(EventLevel::STATUS, source, requestId, "[STATUS] radio", radios_.statusText());
    return;
  }

  if (command.equalsIgnoreCase("BleStatus")) {
    publishChunked(EventLevel::STATUS, source, requestId, "[STATUS] radio", radios_.statusText());
    return;
  }


  if (bridge_.handleCommand(command, source, requestId)) {
    return;
  }

  if (selfTest_ != nullptr && selfTest_->handleCommand(command, source, requestId)) {
    return;
  }

  // ADD A NEW SUBSYSTEM COMMAND FAMILY HERE. Hardware commands live in the
  // selected DeviceAddon so the transport/command base can boot without them.
  if (addon_.handleCommand(command, source, requestId)) {
    return;
  }
  if (radios_.handleCommand(command, source, requestId)) {
    return;
  }

  error(source, requestId, "unknown command; send Help");
}

void CommandRouter::service() {
  if (indicators_.consumeSelfTestCompleted()) {
    events_.publish(
      EventLevel::STATUS,
      "[DONE] indicator self-test complete; GPIO" +
        String(AppConfig::PIN_STATUS_CONNECTION) + "=2 pulses GPIO" +
        String(AppConfig::PIN_STATUS_ACTIVITY) + "=3 pulses",
      CommandSource::INTERNAL
    );
  }

  if (rebootPending_ && static_cast<int32_t>(millis() - rebootAtMs_) >= 0) {
    rebootPending_ = false;
    ESP.restart();
  }
}

bool CommandRouter::isProductionMode() const {
  return productionMode_;
}

String CommandRouter::bootModeText() const {
  return productionMode_ ? "production" : "debug";
}

String CommandRouter::stateJson() const {
  String json;
  json.reserve(1500);
  json = "{";
  json += "\"ok\":true";
  json += ",\"firmware\":\"" + TextUtil::jsonEscape(AppConfig::FIRMWARE_NAME) + "\"";
  json += ",\"version\":\"" + TextUtil::jsonEscape(AppConfig::FIRMWARE_VERSION) + "\"";
  json += ",\"uptimeMs\":" + String(millis());
  json += ",\"freeHeap\":" + String(ESP.getFreeHeap());
  json += ",\"minFreeHeap\":" + String(ESP.getMinFreeHeap());
  json += ",\"largestHeapBlock\":" + String(ESP.getMaxAllocHeap());
  json += ",\"latestEventId\":" + String(events_.latestId());
  json += ",\"bootMode\":\"" + bootModeText() + "\"";
  json += ",\"debugStartupTestsEnabled\":" + TextUtil::jsonBool(!productionMode_);
  addon_.appendStateJson(json, false);
  json += ",\"send\":" + bridge_.stateJson();
  json += ",\"radio\":" + radios_.stateJson();
  json += ",\"transports\":" + transports_.stateJson();
  json += ",\"queue\":" + String(dispatcher_ != nullptr ? dispatcher_->stateJson() : "{}");
  json += ",\"indicators\":" + indicators_.stateJson();
  json += ",\"selfTest\":" + String(selfTest_ != nullptr ? selfTest_->stateJson() : "{}");
  json += "}";
  return json;
}

String CommandRouter::webStateJson() const {
  String json;
  json.reserve(AppConfig::WEB_STATE_JSON_BUDGET_BYTES);
  json = "{\"ok\":true";
  json += ",\"firmware\":\"" + TextUtil::jsonEscape(AppConfig::FIRMWARE_NAME) + "\"";
  json += ",\"version\":\"" + TextUtil::jsonEscape(AppConfig::FIRMWARE_VERSION) + "\"";
  json += ",\"freeHeap\":" + String(ESP.getFreeHeap());
  json += ",\"largestHeapBlock\":" + String(ESP.getMaxAllocHeap());
  json += ",\"latestEventId\":" + String(events_.latestId());
  json += ",\"bootMode\":\"" + bootModeText() + "\"";
  addon_.appendStateJson(json, true);
  json += ",\"send\":" + bridge_.stateJson();
  json += ",\"radio\":" + radios_.webStateJson();
  json += ",\"queue\":" + String(dispatcher_ != nullptr ? dispatcher_->webStateJson() : "{}");
  json += ",\"selfTest\":" + String(selfTest_ != nullptr ? selfTest_->webStateJson() : "{}");
  json += "}";
  return json;
}

String CommandRouter::stateThunk(void *context) {
  if (context == nullptr) {
    return "{\"ok\":false,\"error\":\"state context unavailable\"}";
  }
  return static_cast<CommandRouter *>(context)->stateJson();
}

String CommandRouter::webStateThunk(void *context) {
  if (context == nullptr) {
    return "{\"ok\":false,\"error\":\"state context unavailable\"}";
  }
  return static_cast<CommandRouter *>(context)->webStateJson();
}

String CommandRouter::normalize(String command) const {
  command.trim();
  return command;
}

bool CommandRouter::setProductionMode(bool enabled) {
  Preferences preferences;
  if (!preferences.begin(SYSTEM_PREFERENCES_NAMESPACE, false)) {
    return false;
  }
  const bool written = preferences.putBool(PRODUCTION_MODE_KEY, enabled) > 0;
  preferences.end();
  if (!written) {
    return false;
  }

  if (!preferences.begin(SYSTEM_PREFERENCES_NAMESPACE, true)) {
    return false;
  }
  const bool verified =
    preferences.getBool(PRODUCTION_MODE_KEY, !enabled) == enabled;
  preferences.end();
  if (verified) {
    productionMode_ = enabled;
  }
  return verified;
}

void CommandRouter::publishBootMode(
  CommandSource source,
  const String &requestId,
  const String &reason
) {
  publish(
    EventLevel::STATUS,
    source,
    requestId,
    "[CONFIG] bootMode=" + bootModeText() +
      " startupTests=" + String(productionMode_ ? "skip" : "run") +
      " note=" + reason
  );
}

void CommandRouter::publishHelp(CommandSource source, const String &requestId) {
  publish(EventLevel::STATUS, source, requestId, "[HELP] REQUESTED COMMAND TABLE");
  for (size_t index = 0; index < HELP_ROW_COUNT; index++) {
    publish(EventLevel::STATUS, source, requestId, HELP_ROWS[index]);
  }
  addon_.publishHelp(events_, source, requestId);
}


void CommandRouter::publishStatus(CommandSource source, const String &requestId) {
  publishBootMode(source, requestId, "runtime status");
  publishChunked(EventLevel::STATUS, source, requestId, "[STATUS] USB", transports_.usbStatusText());
  addon_.publishStatus(events_, source, requestId, false);
  publishChunked(EventLevel::STATUS, source, requestId, "[STATUS] send", bridge_.statusText());
  publishChunked(EventLevel::STATUS, source, requestId, "[STATUS] radio", radios_.statusText());
  publishChunked(EventLevel::STATUS, source, requestId, "[STATUS] indicators", indicators_.statusText());
  if (selfTest_ != nullptr) {
    publishChunked(EventLevel::STATUS, source, requestId, "[STATUS] selfTest", selfTest_->statusText());
  }
}

void CommandRouter::publishConfig(CommandSource source, const String &requestId) {
  publishBootMode(source, requestId, "configuration readback");
  addon_.publishStatus(events_, source, requestId, true);
  publishChunked(EventLevel::STATUS, source, requestId, "[CONFIG] send", bridge_.statusText());
  publishChunked(EventLevel::STATUS, source, requestId, "[CONFIG] radio", radios_.statusText());
  publishChunked(EventLevel::STATUS, source, requestId, "[CONFIG] indicators", indicators_.statusText());
  if (selfTest_ != nullptr) {
    publishChunked(EventLevel::STATUS, source, requestId, "[CONFIG] selfTest", selfTest_->statusText());
  }
}

void CommandRouter::publishChunked(
  EventLevel level,
  CommandSource source,
  const String &requestId,
  const String &prefix,
  const String &message
) {
  constexpr size_t MAX_PAYLOAD = 190;
  size_t start = 0;
  uint8_t part = 1;
  while (start < message.length()) {
    size_t end = start + MAX_PAYLOAD;
    if (end > message.length()) {
      end = message.length();
    }
    if (end < message.length()) {
      const int split = message.lastIndexOf(' ', static_cast<unsigned int>(end));
      if (split > static_cast<int>(start)) {
        end = static_cast<size_t>(split);
      }
    }

    String chunk = message.substring(
      static_cast<unsigned int>(start),
      static_cast<unsigned int>(end)
    );
    chunk.trim();
    publish(
      level,
      source,
      requestId,
      prefix + " part=" + String(part++) + " " + chunk
    );

    start = end;
    while (start < message.length() && message[start] == ' ') {
      start++;
    }
  }

  if (message.length() == 0) {
    publish(level, source, requestId, prefix + " empty");
  }
}

void CommandRouter::publish(
  EventLevel level,
  CommandSource source,
  const String &requestId,
  const String &message
) {
  events_.publish(level, message, source, requestId);
}

void CommandRouter::error(
  CommandSource source,
  const String &requestId,
  const String &message
) {
  events_.publish(EventLevel::ERROR, message, source, requestId);
}
