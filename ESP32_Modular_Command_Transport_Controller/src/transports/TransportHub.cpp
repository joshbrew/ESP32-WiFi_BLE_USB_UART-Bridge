#include "TransportHub.h"

#include "../util/TextUtil.h"

#if APP_CLASSIC_BT_SPP_ENABLED
#include <esp_gap_bt_api.h>
#endif
#include <string.h>

#if APP_BLUETOOTH_ENABLED && (!defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED))
#error Bluetooth is not enabled in this ESP32 build. Comment out the BLE/SPP build switches in src/config/AppConfig.h or select a Bluetooth-capable classic ESP32 target.
#endif

#if APP_CLASSIC_BT_SPP_ENABLED && !defined(CONFIG_BT_SPP_ENABLED)
#error Bluetooth SPP is not enabled. Select a classic ESP32 board profile with SPP support or comment out APP_ENABLE_CLASSIC_BT_SPP.
#endif

#if APP_BLUETOOTH_ENABLED
namespace {

void printBluetoothCheckpoint(const char *stage) {
  const UBaseType_t stackWatermark = uxTaskGetStackHighWaterMark(nullptr);
  Serial.printf(
    "[BOOT][BT][MEM] %s heap=%u minimum=%u largest=%u stackWatermark=%u\n",
    stage,
    static_cast<unsigned>(ESP.getFreeHeap()),
    static_cast<unsigned>(ESP.getMinFreeHeap()),
    static_cast<unsigned>(ESP.getMaxAllocHeap()),
    static_cast<unsigned>(stackWatermark)
  );
  Serial.flush();
}

}  // namespace
#endif

#if APP_BLE_ENABLED
class TransportHub::BleServerCallbacks : public BLEServerCallbacks {
 public:
  explicit BleServerCallbacks(TransportHub &owner) : owner_(owner) {}

  void onConnect(BLEServer *server) override {
    (void)server;
    owner_.onBleConnected();
  }

  void onDisconnect(BLEServer *server) override {
    (void)server;
    owner_.onBleDisconnected();
  }

 private:
  TransportHub &owner_;
};

class TransportHub::BleRxCallbacks : public BLECharacteristicCallbacks {
 public:
  explicit BleRxCallbacks(TransportHub &owner) : owner_(owner) {}

  void onWrite(BLECharacteristic *characteristic) override {
    auto value = characteristic->getValue();
    owner_.onBleWrite(value.c_str(), value.length());
  }

 private:
  TransportHub &owner_;
};

class TransportHub::BleTxDescriptorCallbacks : public BLEDescriptorCallbacks {
 public:
  explicit BleTxDescriptorCallbacks(TransportHub &owner) : owner_(owner) {}

  void onWrite(BLEDescriptor *descriptor) override {
    const uint8_t *value = descriptor == nullptr ? nullptr : descriptor->getValue();
    const bool enabled =
      descriptor != nullptr && descriptor->getLength() >= 1 && value != nullptr &&
      (value[0] & 0x01U) != 0;
    owner_.onBleNotificationsChanged(enabled);
  }

 private:
  TransportHub &owner_;
};
#endif

TransportHub::TransportHub(EventBus &events)
  : events_(events),
    submitter_(nullptr),
    submitContext_(nullptr),
    stateProvider_(nullptr),
    stateContext_(nullptr),
#if APP_AUX_UART_ENABLED
    auxUart_(AppConfig::AUX_UART_PORT),
#endif
    uartRunning_(false),
#if APP_CLASSIC_BT_SPP_ENABLED
    sppDesired_(false),
    sppInitialized_(false),
    sppRunning_(false),
    sppConnected_(false),
    previousSppConnected_(false),
#endif
    bleDesired_(false),
    bleControllerInitialized_(false),
    bleInitialized_(false),
    bleStandalone_(false),
    bleRunning_(false),
    bleConnected_(false),
    bleNotificationsEnabled_(false),
    bleDormant_(false),
    bleRestartPending_(false),
    bleInputOverflowPending_(false),
    bleIdleSubmitPending_(false),
    bleDormancyHeapCheckPending_(false),
    bleDormancyHeapCheckAtMs_(0),
    bleDormancyHeapBefore_(0),
    bleDormancyLargestBefore_(0),
    bleConnectionEvent_(0),
    bleSubscriptionEvent_(0),
    bleConnectedAtMs_(0),
    bleNotificationsEnabledAtMs_(0),
    bleLastPressureWarningAtMs_(0),
#if APP_BLE_ENABLED
    bleServer_(nullptr),
    bleService_(nullptr),
    bleTx_(nullptr),
    bleRx_(nullptr),
    bleTxCccd_(nullptr),
    bleInputMux_(portMUX_INITIALIZER_UNLOCKED),
    bleStateMux_(portMUX_INITIALIZER_UNLOCKED),
    bleNotifyMutex_(nullptr),
#endif
    usbCursor_(0),
#if APP_CLASSIC_BT_SPP_ENABLED
    sppCursor_(0),
#endif
#if APP_BLE_ENABLED
    bleCursor_(0),
#endif
#if APP_AUX_UART_ENABLED
    uartCursor_(0),
#endif
#if APP_BLE_ENABLED
    lastBleNotifyAtMs_(0),
    bleLastInputAtMs_(0),
    bleDirectOutput_(),
    bleDirectOffset_(0),
#endif
    requestCounter_(1) {}


void TransportHub::begin() {
  Serial.begin(AppConfig::USB_BAUD);
  Serial.setTimeout(20);

#if APP_AUX_UART_ENABLED
  if (AppConfig::AUX_UART_RX_PIN >= 0 && AppConfig::AUX_UART_TX_PIN >= 0) {
    auxUart_.begin(
      AppConfig::AUX_UART_BAUD,
      SERIAL_8N1,
      AppConfig::AUX_UART_RX_PIN,
      AppConfig::AUX_UART_TX_PIN
    );
    auxUart_.setTimeout(20);
    uartRunning_ = true;
  }
#endif

  events_.publish(
    EventLevel::STATUS,
    "USB transport name=" + String(AppConfig::USB_SERIAL_NAME) +
      " baud=" + String(AppConfig::USB_BAUD) +
      "; auxUART=" + String(uartRunning_ ? "enabled" : "disabled"),
    CommandSource::INTERNAL
  );
}

void TransportHub::configureCommandSubmitter(CommandSubmitter submitter, void *context) {
  submitter_ = submitter;
  submitContext_ = context;
}

void TransportHub::configureStateProvider(StateProvider provider, void *context) {
  stateProvider_ = provider;
  stateContext_ = context;
}

// ADD A NEW STREAM TRANSPORT by following the USB/SPP/BLE pattern: assemble
// newline input, submit through the shared callback, and drain events by cursor.
void TransportHub::service() {
  serviceUsbInput();
#if APP_CLASSIC_BT_SPP_ENABLED
  serviceSppInput();
#endif
  serviceUartInput();

#if APP_BLE_ENABLED
  int8_t bleConnectionEvent = 0;
  int8_t bleSubscriptionEvent = 0;
  bool bleInputOverflow = false;
  bool bleRestartPending = false;
  portENTER_CRITICAL(&bleStateMux_);
  bleConnectionEvent = bleConnectionEvent_;
  bleConnectionEvent_ = 0;
  bleSubscriptionEvent = bleSubscriptionEvent_;
  bleSubscriptionEvent_ = 0;
  bleInputOverflow = bleInputOverflowPending_;
  bleInputOverflowPending_ = false;
  bleRestartPending = bleRestartPending_;
  bleRestartPending_ = false;
  portEXIT_CRITICAL(&bleStateMux_);

  if (bleConnectionEvent != 0) {
    if (bleConnectionEvent > 0) {
      if (!bleDesired_ || !bleRunning_) {
        if (bleServer_ != nullptr && bleServer_->getConnectedCount() > 0) {
          bleServer_->disconnect(bleServer_->getConnId());
        }
        portENTER_CRITICAL(&bleStateMux_);
        bleConnected_ = false;
        portEXIT_CRITICAL(&bleStateMux_);
      } else {
        bleCursor_ = events_.latestId();
        bleOutput_.clear();
        bleDirectOutput_ = "";
        bleDirectOffset_ = 0;
        events_.publish(
          EventLevel::STATUS,
          "BLE client connected; enable TX notifications for responses",
          CommandSource::INTERNAL
        );
      }
    } else {
      bleAssembler_.clear();
      bleIdleSubmitPending_ = false;
      bleLastInputAtMs_ = 0;
      bleOutput_.clear();
      bleDirectOutput_ = "";
      bleDirectOffset_ = 0;
      events_.publish(
        EventLevel::STATUS,
        "BLE client disconnected",
        CommandSource::INTERNAL
      );
    }
  }

  if (bleSubscriptionEvent != 0) {
    bleCursor_ = events_.latestId();
    bleOutput_.clear();
    bleDirectOutput_ = "";
    bleDirectOffset_ = 0;
    events_.publish(
      EventLevel::STATUS,
      bleSubscriptionEvent > 0
        ? "BLE TX notifications enabled; response channel ready"
        : "BLE TX notifications disabled; commands remain accepted without BLE responses",
      CommandSource::INTERNAL,
      String(),
      static_cast<TransportMask>(TRANSPORT_MASK_USB | TRANSPORT_MASK_UART |
        (bleSubscriptionEvent > 0 ? TRANSPORT_MASK_BLE : TRANSPORT_MASK_NONE))
    );
  }

  serviceBleInput();
  serviceBleIdleSubmit();

#if APP_VERBOSE_COEX_HTTP_DIAGNOSTICS
  if (
    bleDormancyHeapCheckPending_ &&
    static_cast<int32_t>(millis() - bleDormancyHeapCheckAtMs_) >= 0
  ) {
    bleDormancyHeapCheckPending_ = false;
    const uint32_t heapAfter = ESP.getFreeHeap();
    const uint32_t largestAfter = ESP.getMaxAllocHeap();
    Serial.printf(
      "[BLE][DORMANT][MEM] advertising-only sleep freeBefore=%u freeAfter=%u delta=%d largestBefore=%u largestAfter=%u delta=%d hostControllerRetained=true\n",
      static_cast<unsigned>(bleDormancyHeapBefore_),
      static_cast<unsigned>(heapAfter),
      static_cast<int32_t>(heapAfter) - static_cast<int32_t>(bleDormancyHeapBefore_),
      static_cast<unsigned>(bleDormancyLargestBefore_),
      static_cast<unsigned>(largestAfter),
      static_cast<int32_t>(largestAfter) - static_cast<int32_t>(bleDormancyLargestBefore_)
    );
    Serial.flush();
  }
#endif

  // BLE callbacks only copy bytes and set flags. Publish overflow diagnostics
  // here on the control task so callback latency stays bounded.
  if (bleInputOverflow) {
    events_.publish(
      EventLevel::ERROR,
      "BLE input buffer full; packet dropped",
      CommandSource::BLE
    );
  }

  if (bleRestartPending) {
    if (bleDesired_ && bleInitialized_ && bleRunning_ && !bleDormant_) {
      BLEDevice::startAdvertising();
    }
  }

#endif

  serviceOutput();
}

void TransportHub::setSppEnabled(bool enabled) {
#if APP_CLASSIC_BT_SPP_ENABLED
  sppDesired_ = enabled;
  if (enabled) {
    startSpp();
  } else {
    stopSpp();
  }
#else
  if (enabled) {
    events_.publish(
      EventLevel::ERROR,
      "Classic Bluetooth SPP is compiled out; uncomment APP_ENABLE_CLASSIC_BT_SPP in src/config/AppConfig.h and rebuild",
      CommandSource::INTERNAL
    );
  }
#endif
}

bool TransportHub::prepareBleController() {
#if APP_BLE_ENABLED
  return initializeBleController();
#else
  events_.publish(
    EventLevel::ERROR,
    "BLE is compiled out; uncomment APP_ENABLE_BLE in src/config/AppConfig.h and rebuild",
    CommandSource::INTERNAL
  );
  return false;
#endif
}

void TransportHub::setBleEnabled(bool enabled) {
#if APP_BLE_ENABLED
  bleDesired_ = enabled;
  if (enabled) {
    startBle();
  } else {
    stopBle();
  }
#else
  bleDesired_ = false;
  if (enabled) {
    events_.publish(
      EventLevel::ERROR,
      "BLE is compiled out; uncomment APP_ENABLE_BLE in src/config/AppConfig.h and rebuild",
      CommandSource::INTERNAL
    );
  }
#endif
}

void TransportHub::setBleDormant(bool dormant) {
#if APP_BLE_ENABLED
  if (!bleInitialized_ || !bleRunning_) {
    bleDormant_ = dormant;
    return;
  }

  if (dormant == bleDormant_) {
    return;
  }

  if (dormant) {
    if (isBleConnected()) {
      events_.publish(
        EventLevel::WARNING,
        "BLE dormancy deferred because a BLE client is connected",
        CommandSource::INTERNAL
      );
      return;
    }
#if APP_VERBOSE_COEX_HTTP_DIAGNOSTICS
    bleDormancyHeapBefore_ = ESP.getFreeHeap();
    bleDormancyLargestBefore_ = ESP.getMaxAllocHeap();
#endif
    BLEAdvertising *advertising = BLEDevice::getAdvertising();
    if (advertising != nullptr) {
      advertising->stop();
    }
    bleDormant_ = true;
#if APP_VERBOSE_COEX_HTTP_DIAGNOSTICS
    bleDormancyHeapCheckPending_ = true;
    bleDormancyHeapCheckAtMs_ = millis() + AppConfig::BLE_DORMANCY_HEAP_CHECK_DELAY_MS;
#endif
    events_.publish(
      EventLevel::STATUS,
      "BLE soft-dormant: advertising stopped; host/controller heap retained for reliable resume",
      CommandSource::INTERNAL
    );
    return;
  }

  bleDormancyHeapCheckPending_ = false;
  bleDormant_ = false;
  if (!isBleConnected()) {
    BLEDevice::startAdvertising();
  }
  events_.publish(
    EventLevel::STATUS,
    "BLE advertising resumed after WiFi client release",
    CommandSource::INTERNAL
  );
#else
  (void)dormant;
  bleDormant_ = false;
#endif
}

void TransportHub::restartBleAdvertising() {
#if APP_BLE_ENABLED
  if (!bleDesired_) {
    return;
  }
  if (!bleInitialized_) {
    startBle();
    return;
  }
  bleDormant_ = false;
  portENTER_CRITICAL(&bleStateMux_);
  bleNotificationsEnabled_ = false;
  bleNotificationsEnabledAtMs_ = 0;
  bleConnectedAtMs_ = 0;
  portEXIT_CRITICAL(&bleStateMux_);
  BLEDevice::startAdvertising();
#endif
}

bool TransportHub::isSppRunning() const {
#if APP_CLASSIC_BT_SPP_ENABLED
  return sppRunning_;
#else
  return false;
#endif
}

bool TransportHub::isSppConnected() const {
#if APP_CLASSIC_BT_SPP_ENABLED
  return sppConnected_;
#else
  return false;
#endif
}

bool TransportHub::isBleRunning() const {
#if APP_BLE_ENABLED
  return bleRunning_;
#else
  return false;
#endif
}

bool TransportHub::isBleConnected() const {
#if APP_BLE_ENABLED
  portENTER_CRITICAL(&bleStateMux_);
  const bool connected = bleConnected_;
  portEXIT_CRITICAL(&bleStateMux_);
  return connected;
#else
  return false;
#endif
}

bool TransportHub::isBleOutputReady() const {
#if APP_BLE_ENABLED
  portENTER_CRITICAL(&bleStateMux_);
  const bool connected = bleConnected_;
  const bool subscribed = bleNotificationsEnabled_;
  const uint32_t connectedAt = bleConnectedAtMs_;
  const uint32_t subscribedAt = bleNotificationsEnabledAtMs_;
  portEXIT_CRITICAL(&bleStateMux_);
  const uint32_t now = millis();
  return
    bleRunning_ && connected && subscribed && bleTx_ != nullptr &&
    ESP.getFreeHeap() >= AppConfig::BLE_TX_MIN_FREE_HEAP_BYTES &&
    ESP.getMaxAllocHeap() >= AppConfig::BLE_TX_MIN_LARGEST_BLOCK_BYTES &&
    static_cast<uint32_t>(now - connectedAt) >= AppConfig::BLE_NOTIFY_CONNECT_SETTLE_MS &&
    static_cast<uint32_t>(now - subscribedAt) >= AppConfig::BLE_NOTIFY_SUBSCRIPTION_SETTLE_MS;
#else
  return false;
#endif
}

bool TransportHub::isBleDormant() const {
#if APP_BLE_ENABLED
  return bleDormant_;
#else
  return false;
#endif
}

bool TransportHub::isBleInitialized() const {
#if APP_BLE_ENABLED
  return bleInitialized_;
#else
  return false;
#endif
}

bool TransportHub::isUartRunning() const {
  return uartRunning_;
}

bool TransportHub::isTransportAvailable(CommandSource source) const {
  switch (source) {
    case CommandSource::USB:
      return true;
    case CommandSource::SPP:
#if APP_CLASSIC_BT_SPP_ENABLED
      return sppRunning_ && sppConnected_;
#else
      return false;
#endif
    case CommandSource::BLE:
      return bleRunning_ && isBleConnected();
    case CommandSource::UART:
      return uartRunning_;
    case CommandSource::WIFI:
    case CommandSource::INTERNAL:
    default:
      return false;
  }
}

String TransportHub::usbStatusText() const {
  return "USB logical name=" + String(AppConfig::USB_SERIAL_NAME) +
    " baud=" + String(AppConfig::USB_BAUD) +
    "; OS USB device name is defined by the LOLIN32 USB-UART bridge" +
    "; auxUART=" + String(uartRunning_ ? "enabled" : "disabled") +
    " baud=" + String(AppConfig::AUX_UART_BAUD) +
    " rxPin=" + String(AppConfig::AUX_UART_RX_PIN) +
    " txPin=" + String(AppConfig::AUX_UART_TX_PIN);
}

String TransportHub::stateJson() const {
  String json = "{";
  json += "\"usbName\":\"" + TextUtil::jsonEscape(AppConfig::USB_SERIAL_NAME) + "\"";
  json += ",\"usbBaud\":" + String(AppConfig::USB_BAUD);
  json += ",\"usbOsNameFirmwareConfigurable\":false";
  json += ",\"sppCompiled\":" + TextUtil::jsonBool(AppConfig::ENABLE_CLASSIC_BT_SPP);
#if APP_CLASSIC_BT_SPP_ENABLED
  json += ",\"sppDesired\":" + TextUtil::jsonBool(sppDesired_);
  json += ",\"sppInitialized\":" + TextUtil::jsonBool(sppInitialized_);
  json += ",\"sppRunning\":" + TextUtil::jsonBool(sppRunning_);
  json += ",\"sppConnected\":" + TextUtil::jsonBool(sppConnected_);
#else
  json += ",\"sppDesired\":false";
  json += ",\"sppInitialized\":false";
  json += ",\"sppRunning\":false";
  json += ",\"sppConnected\":false";
#endif
  json += ",\"bleCompiled\":" + TextUtil::jsonBool(AppConfig::ENABLE_BLE);
  json += ",\"bleDesired\":" + TextUtil::jsonBool(AppConfig::ENABLE_BLE && bleDesired_);
  json += ",\"bleInitialized\":" + TextUtil::jsonBool(isBleInitialized());
  json += ",\"bleStandalone\":" + TextUtil::jsonBool(AppConfig::ENABLE_BLE && bleStandalone_);
  json += ",\"bleRunning\":" + TextUtil::jsonBool(isBleRunning());
  json += ",\"bleConnected\":" + TextUtil::jsonBool(isBleConnected());
  json += ",\"bleNotifications\":" + TextUtil::jsonBool(isBleOutputReady());
  json += ",\"bleDormant\":" + TextUtil::jsonBool(isBleDormant());
  json += ",\"bleDormancyMode\":\"advertising-only\"";
  json += ",\"bleHeapReleasedWhileDormant\":false";
#if APP_CLASSIC_BT_SPP_ENABLED
  json += ",\"bluetoothStackWarm\":" + TextUtil::jsonBool(sppInitialized_ || bleInitialized_);
#else
  json += ",\"bluetoothStackWarm\":" + TextUtil::jsonBool(bleInitialized_);
#endif
  json += ",\"uartRunning\":" + TextUtil::jsonBool(uartRunning_);
  json += ",\"uartBaud\":" + String(AppConfig::AUX_UART_BAUD);
  json += "}";
  return json;
}

void TransportHub::serviceUsbInput() {
  size_t processed = 0;
  while (processed < AppConfig::USB_INPUT_READ_BUDGET && Serial.available() > 0) {
    const char value = static_cast<char>(Serial.read());
    processed++;
    usbAssembler_.push(
      value,
      [this](const String &line) { submitLine(CommandSource::USB, line); },
      [this](const String &message) { inputError(CommandSource::USB, message); }
    );
  }
}

void TransportHub::serviceSppInput() {
#if APP_CLASSIC_BT_SPP_ENABLED
  if (!sppRunning_) {
    return;
  }

  sppConnected_ = serialBt_.hasClient();
  if (sppConnected_ != previousSppConnected_) {
    previousSppConnected_ = sppConnected_;
    if (sppConnected_) {
      sppCursor_ = events_.latestId();
      sppOutput_.clear();
      events_.publish(
        EventLevel::STATUS,
        "Bluetooth SPP client connected",
        CommandSource::INTERNAL
      );
    } else {
      events_.publish(
        EventLevel::STATUS,
        "Bluetooth SPP client disconnected",
        CommandSource::INTERNAL
      );
    }
  }

  size_t processed = 0;
  while (processed < AppConfig::SPP_INPUT_READ_BUDGET && serialBt_.available() > 0) {
    const char value = static_cast<char>(serialBt_.read());
    processed++;
    sppAssembler_.push(
      value,
      [this](const String &line) { submitLine(CommandSource::SPP, line); },
      [this](const String &message) { inputError(CommandSource::SPP, message); }
    );
  }
#endif
}

void TransportHub::serviceUartInput() {
#if APP_AUX_UART_ENABLED
  if (!uartRunning_) {
    return;
  }

  size_t processed = 0;
  while (processed < AppConfig::UART_INPUT_READ_BUDGET && auxUart_.available() > 0) {
    const char value = static_cast<char>(auxUart_.read());
    processed++;
    uartAssembler_.push(
      value,
      [this](const String &line) { submitLine(CommandSource::UART, line); },
      [this](const String &message) { inputError(CommandSource::UART, message); }
    );
  }
#endif
}

void TransportHub::serviceBleInput() {
#if APP_BLE_ENABLED
  char buffer[AppConfig::BLE_INPUT_READ_BUDGET];
  size_t count = 0;

  portENTER_CRITICAL(&bleInputMux_);
  count = bleInput_.pop(buffer, sizeof(buffer));
  portEXIT_CRITICAL(&bleInputMux_);

  for (size_t index = 0; index < count; index++) {
    bleAssembler_.push(
      buffer[index],
      [this](const String &line) {
        bleIdleSubmitPending_ = false;
        handleBleLine(line);
      },
      [this](const String &message) {
        bleIdleSubmitPending_ = false;
        inputError(CommandSource::BLE, message);
      }
    );
  }

  if (count > 0 && bleAssembler_.hasPending()) {
    bleLastInputAtMs_ = millis();
    bleIdleSubmitPending_ = true;
  }
#endif
}

void TransportHub::serviceBleIdleSubmit() {
#if APP_BLE_ENABLED
  if (!bleIdleSubmitPending_ || !bleAssembler_.hasPending()) {
    bleIdleSubmitPending_ = false;
    return;
  }

  const uint32_t now = millis();
  if (static_cast<uint32_t>(now - bleLastInputAtMs_) < AppConfig::BLE_IDLE_COMMAND_SUBMIT_MS) {
    return;
  }

  bleIdleSubmitPending_ = false;
  bleAssembler_.flushPending(
    [this](const String &line) { handleBleLine(line); },
    [this](const String &message) { inputError(CommandSource::BLE, message); }
  );
#endif
}

void TransportHub::serviceOutput() {
  fillOutputQueue(
    usbCursor_,
    usbOutput_,
    AppConfig::USB_EVENT_FILL_BUDGET,
    "USB",
    TRANSPORT_MASK_USB
  );
  flushUsb();

#if APP_CLASSIC_BT_SPP_ENABLED
  if (sppRunning_ && sppConnected_) {
    fillOutputQueue(
      sppCursor_,
      sppOutput_,
      AppConfig::SPP_EVENT_FILL_BUDGET,
      "SPP",
      TRANSPORT_MASK_SPP
    );
    flushSpp();
  }
#endif

#if APP_BLE_ENABLED
  if (isBleOutputReady()) {
    fillOutputQueue(
      bleCursor_,
      bleOutput_,
      AppConfig::BLE_EVENT_FILL_BUDGET,
      "BLE",
      TRANSPORT_MASK_BLE
    );
    flushBle();
  }
#endif

#if APP_AUX_UART_ENABLED
  if (uartRunning_) {
    fillOutputQueue(
      uartCursor_,
      uartOutput_,
      AppConfig::UART_EVENT_FILL_BUDGET,
      "UART",
      TRANSPORT_MASK_UART
    );
    flushUart();
  }
#endif
}


void TransportHub::handleBleLine(const String &line) {
  String work = line;
  work.trim();

  if (work.equalsIgnoreCase("@STATE")) {
    if (stateProvider_ == nullptr) {
      queueBleDirectResponse("@ERROR state provider unavailable\n");
      return;
    }
    queueBleDirectResponse("@STATE " + stateProvider_(stateContext_) + "\n");
    return;
  }

  submitLine(CommandSource::BLE, work);
}

void TransportHub::queueBleDirectResponse(const String &line) {
#if APP_BLE_ENABLED
  // Direct BLE frames are reserved for browser transport housekeeping. They do
  // not enter EventBus, so periodic state snapshots do not flood USB serial or
  // consume the shared event history. A connected client must subscribe to TX
  // notifications before any GATT notification is attempted.
  if (!isBleOutputReady()) {
    return;
  }
  bleDirectOutput_ = line;
  bleDirectOffset_ = 0;
#else
  (void)line;
#endif
}

void TransportHub::submitLine(CommandSource source, const String &line) {
  const String requestId = makeRequestId(source);
  if (submitter_ == nullptr || !submitter_(submitContext_, source, line, requestId)) {
    events_.publish(
      EventLevel::ERROR,
      "command queue busy; resend command",
      source,
      requestId
    );
  }
}

String TransportHub::makeRequestId(CommandSource source) {
  return String(commandSourceName(source)) + "-" + String(requestCounter_++);
}

void TransportHub::inputError(CommandSource source, const String &message) {
  events_.publish(
    EventLevel::ERROR,
    message,
    source,
    makeRequestId(source)
  );
}

// The classic ESP32 cannot safely upgrade a BLE-only controller to Classic
// Bluetooth later without tearing down the shared controller. Initialize the
// controller in BTDM mode the first time either BLE or SPP is requested, then
// keep SPP hidden until its desired flag is enabled. This is the GO HERE point
// for changing the Bluetooth stack lifecycle or choosing reboot-only toggles.
bool TransportHub::ensureBluetoothDualMode() {
#if APP_CLASSIC_BT_SPP_ENABLED
  if (bleInitialized_ && bleStandalone_) {
    events_.publish(
      EventLevel::ERROR,
      "Classic SPP cannot be enabled after BLE-only startup; enable SPP in saved settings and reboot",
      CommandSource::INTERNAL
    );
    return false;
  }
  if (!sppInitialized_) {
    // BluetoothSerial.begin() configures the controller synchronously, but SPP
    // registration completes asynchronously in ESP_SPP_INIT_EVT. Calling GAP
    // or BLE APIs before that event can enter Bluedroid before its internal
    // command queue is ready and assert in xQueueGenericSend(pxQueue).
    Serial.println("[BOOT][BT] starting shared BTDM controller");
    Serial.flush();

    // Classic SPP is an exclusive boot profile in v5.15. disableBLE=true lets
    // BluetoothSerial avoid reserving the BLE side of the controller. Switching
    // to BLE or WiFi+BLE requires saving a new boot profile and rebooting.
    if (!serialBt_.begin(AppConfig::CLASSIC_BT_NAME, true, false)) {
      events_.publish(
        EventLevel::ERROR,
        "shared Bluetooth dual-mode controller failed to initialize",
        CommandSource::INTERNAL
      );
      return false;
    }

    serialBt_.setTimeout(20);
    sppInitialized_ = true;
  }

  // begin() can return before ESP_SPP_INIT_EVT. isReady() waits on the library's
  // SPP-ready event group, so no GAP or BLE function below can run early.
  if (!serialBt_.isReady(false, AppConfig::BLUETOOTH_READY_TIMEOUT_MS)) {
    events_.publish(
      EventLevel::ERROR,
      "shared Bluetooth controller started but SPP initialization did not become ready",
      CommandSource::INTERNAL
    );
    return false;
  }

  // Give the callback that set the ready bit one scheduler turn to finish before
  // another Bluedroid API reuses the shared Bluetooth command path.
  vTaskDelay(pdMS_TO_TICKS(AppConfig::BLUETOOTH_READY_SETTLE_MS));

  // Retry the hidden state whenever SPP is not running. This call is deliberately
  // after isReady(); moving it above the readiness wait recreates the boot crash.
  if (!sppRunning_) {
    const esp_err_t hiddenResult = esp_bt_gap_set_scan_mode(
      ESP_BT_NON_CONNECTABLE,
      ESP_BT_NON_DISCOVERABLE
    );
    if (hiddenResult != ESP_OK) {
      events_.publish(
        EventLevel::ERROR,
        "shared Bluetooth controller initialized but SPP could not be hidden",
        CommandSource::INTERNAL
      );
      return false;
    }
  }

  Serial.println("[BOOT][BT] shared BTDM controller ready");
  Serial.flush();
  events_.publish(
    EventLevel::STATUS,
    "shared Bluetooth dual-mode controller ready; SPP hidden until enabled",
    CommandSource::INTERNAL
  );
  return true;
#else
  return false;
#endif
}

void TransportHub::startSpp() {
#if APP_CLASSIC_BT_SPP_ENABLED
  if (sppRunning_) {
    return;
  }

  if (bleInitialized_ && bleStandalone_) {
    events_.publish(
      EventLevel::ERROR,
      "Classic SPP needs a reboot when BLE was initialized first; save SPP enabled, then reboot",
      CommandSource::INTERNAL
    );
    return;
  }

  if (!ensureBluetoothDualMode()) {
    return;
  }

  const esp_err_t visibilityResult = esp_bt_gap_set_scan_mode(
    ESP_BT_CONNECTABLE,
    ESP_BT_GENERAL_DISCOVERABLE
  );
  if (visibilityResult != ESP_OK) {
    events_.publish(
      EventLevel::ERROR,
      "Bluetooth SPP could not become discoverable",
      CommandSource::INTERNAL
    );
    return;
  }

  while (serialBt_.available() > 0) {
    serialBt_.read();
  }

  sppRunning_ = true;
  sppConnected_ = false;
  previousSppConnected_ = false;
  sppCursor_ = events_.latestId();
  sppOutput_.clear();
  events_.publish(
    EventLevel::STATUS,
    "Bluetooth SPP enabled name=" + String(AppConfig::CLASSIC_BT_NAME),
    CommandSource::INTERNAL
  );
#endif
}

void TransportHub::stopSpp() {
#if APP_CLASSIC_BT_SPP_ENABLED
  if (!sppRunning_) {
    return;
  }

  if (sppInitialized_) {
    // Close an existing RFCOMM client before hiding the service. BluetoothSerial
    // remains initialized so BLE and later SPP re-enables keep working without a
    // full shared-controller teardown.
    if (serialBt_.hasClient()) {
      serialBt_.disconnect();
    }

    const esp_err_t hiddenResult = esp_bt_gap_set_scan_mode(
      ESP_BT_NON_CONNECTABLE,
      ESP_BT_NON_DISCOVERABLE
    );
    if (hiddenResult != ESP_OK) {
      // Keep sppRunning_ true when GAP refuses the hidden state. Reporting the
      // transport as disabled while it may still be discoverable would be wrong.
      events_.publish(
        EventLevel::ERROR,
        "Bluetooth SPP client disconnected but discoverability could not be disabled; retry ConfigApply or reboot",
        CommandSource::INTERNAL
      );
      return;
    }
  }

  sppRunning_ = false;
  sppConnected_ = false;
  previousSppConnected_ = false;
  while (serialBt_.available() > 0) {
    serialBt_.read();
  }
  sppAssembler_.clear();
  sppOutput_.clear();
  events_.publish(
    EventLevel::STATUS,
    "Bluetooth SPP disabled and undiscoverable; shared stack kept warm for reliable re-enable",
    CommandSource::INTERNAL
  );
#endif
}

#if APP_BLE_ENABLED
bool TransportHub::initializeBleController() {
  if (bleControllerInitialized_) {
    return true;
  }

#if APP_CLASSIC_BT_SPP_ENABLED
  if (sppInitialized_ || sppDesired_) {
    if (!ensureBluetoothDualMode()) {
      return false;
    }
    bleStandalone_ = false;
    Serial.println("[BOOT][BT] reserving BLE controller on shared BTDM controller");
  } else {
    bleStandalone_ = true;
    Serial.println("[BOOT][BT] reserving BLE-only Bluedroid controller without pre-releasing Classic memory");
    events_.publish(
      EventLevel::STATUS,
      "BLE-only startup using Arduino-managed Bluedroid controller; Classic SPP requires save plus reboot",
      CommandSource::INTERNAL
    );
  }
#else
  bleStandalone_ = true;
  Serial.println("[BOOT][BT] reserving BLE-only Bluedroid controller; Classic SPP compiled out");
  events_.publish(
    EventLevel::STATUS,
    "BLE-only firmware build; Classic Bluetooth SPP compiled out",
    CommandSource::INTERNAL
  );
#endif
  Serial.flush();
  printBluetoothCheckpoint("before BLEDevice::init");

  BLEDevice::init(AppConfig::BLE_NAME);
  bleControllerInitialized_ = true;
  printBluetoothCheckpoint("after BLEDevice::init");
  return true;
}

bool TransportHub::initializeBleStack() {
  if (bleInitialized_) {
    return true;
  }
  if (!initializeBleController()) {
    return false;
  }

  Serial.println("[BOOT][BT] creating BLE UART GATT service");
  Serial.flush();
  if (bleNotifyMutex_ == nullptr) {
    bleNotifyMutex_ = xSemaphoreCreateMutex();
    if (bleNotifyMutex_ == nullptr) {
      events_.publish(
        EventLevel::ERROR,
        "BLE notification lifecycle mutex allocation failed",
        CommandSource::INTERNAL
      );
      return false;
    }
  }
  bleServer_ = BLEDevice::createServer();
  printBluetoothCheckpoint("after BLEDevice::createServer");
  if (bleServer_ == nullptr) {
    events_.publish(
      EventLevel::ERROR,
      "BLE server allocation failed",
      CommandSource::INTERNAL
    );
    return false;
  }
  bleServer_->setCallbacks(new BleServerCallbacks(*this));

  bleService_ = bleServer_->createService(AppConfig::BLE_UART_SERVICE_UUID);
  printBluetoothCheckpoint("after BLE service allocation");
  if (bleService_ == nullptr) {
    events_.publish(
      EventLevel::ERROR,
      "BLE service allocation failed",
      CommandSource::INTERNAL
    );
    return false;
  }

  bleTx_ = bleService_->createCharacteristic(
    AppConfig::BLE_UART_TX_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  bleRx_ = bleService_->createCharacteristic(
    AppConfig::BLE_UART_RX_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );
  printBluetoothCheckpoint("after BLE characteristic allocation");
  if (bleTx_ == nullptr || bleRx_ == nullptr) {
    events_.publish(
      EventLevel::ERROR,
      "BLE characteristic allocation failed",
      CommandSource::INTERNAL
    );
    return false;
  }

  bleTxCccd_ = new BLE2902();
  bleTxCccd_->setCallbacks(new BleTxDescriptorCallbacks(*this));
  bleTx_->addDescriptor(bleTxCccd_);
  bleTx_->setValue("ESP32 command transport ready\n");
  bleRx_->setCallbacks(new BleRxCallbacks(*this));
  bleService_->start();
  printBluetoothCheckpoint("after BLE service start");

  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  if (advertising == nullptr) {
    events_.publish(
      EventLevel::ERROR,
      "BLE advertising allocation failed",
      CommandSource::INTERNAL
    );
    return false;
  }
  advertising->addServiceUUID(AppConfig::BLE_UART_SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x06);
  advertising->setMinPreferred(0x12);
  printBluetoothCheckpoint("after BLE advertising configuration");

  bleInitialized_ = true;
  Serial.println(bleStandalone_
    ? "[BOOT][BT] BLE-only UART initialized"
    : "[BOOT][BT] BLE UART initialized on shared BTDM controller");
  Serial.flush();
  return true;
}

void TransportHub::startBle() {
  if (!initializeBleStack()) {
    return;
  }

  if (bleRunning_) {
    return;
  }

  bleDormant_ = false;
  BLEDevice::startAdvertising();
  portENTER_CRITICAL(&bleInputMux_);
  bleInput_.clear();
  portEXIT_CRITICAL(&bleInputMux_);
  bleAssembler_.clear();
  bleIdleSubmitPending_ = false;
  bleLastInputAtMs_ = 0;

  bleRunning_ = true;
  bleCursor_ = events_.latestId();
  bleOutput_.clear();
  bleDirectOutput_.reserve(AppConfig::BLE_DIRECT_OUTPUT_BUFFER_BYTES);
  bleDirectOutput_ = "";
  bleDirectOffset_ = 0;
  events_.publish(
    EventLevel::STATUS,
    "BLE UART enabled name=" + String(AppConfig::BLE_NAME),
    CommandSource::INTERNAL
  );
}

void TransportHub::stopBle() {
  if (!bleRunning_) {
    return;
  }

  portENTER_CRITICAL(&bleStateMux_);
  bleRestartPending_ = false;
  portEXIT_CRITICAL(&bleStateMux_);
  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  if (advertising != nullptr) {
    advertising->stop();
  }
  if (bleServer_ != nullptr && bleServer_->getConnectedCount() > 0) {
    bleServer_->disconnect(bleServer_->getConnId());
  }

  bleRunning_ = false;
  bleDormant_ = false;
  portENTER_CRITICAL(&bleStateMux_);
  bleConnected_ = false;
  bleNotificationsEnabled_ = false;
  bleNotificationsEnabledAtMs_ = 0;
  bleConnectedAtMs_ = 0;
  portEXIT_CRITICAL(&bleStateMux_);
  portENTER_CRITICAL(&bleInputMux_);
  bleInput_.clear();
  portEXIT_CRITICAL(&bleInputMux_);
  bleAssembler_.clear();
  bleIdleSubmitPending_ = false;
  bleLastInputAtMs_ = 0;
  bleOutput_.clear();
  bleDirectOutput_ = "";
  bleDirectOffset_ = 0;
  events_.publish(
    EventLevel::STATUS,
    "BLE command transport disabled; shared Bluetooth stack kept warm for reliable re-enable",
    CommandSource::INTERNAL
  );
}

void TransportHub::onBleConnected() {
  if (bleNotifyMutex_ != nullptr) {
    xSemaphoreTake(bleNotifyMutex_, portMAX_DELAY);
  }
  portENTER_CRITICAL(&bleStateMux_);
  bleConnected_ = true;
  bleNotificationsEnabled_ = false;
  bleNotificationsEnabledAtMs_ = 0;
  bleConnectedAtMs_ = millis();
  bleConnectionEvent_ = 1;
  portEXIT_CRITICAL(&bleStateMux_);
  if (bleNotifyMutex_ != nullptr) {
    xSemaphoreGive(bleNotifyMutex_);
  }
}

void TransportHub::onBleDisconnected() {
  // BLEServer invokes this callback before it removes the peer from its internal
  // map. Sharing this mutex with flushBle() prevents notify() from racing that
  // disconnect path and entering Bluedroid with a half-removed connection.
  if (bleNotifyMutex_ != nullptr) {
    xSemaphoreTake(bleNotifyMutex_, portMAX_DELAY);
  }
  portENTER_CRITICAL(&bleStateMux_);
  bleConnected_ = false;
  bleNotificationsEnabled_ = false;
  bleNotificationsEnabledAtMs_ = 0;
  bleConnectedAtMs_ = 0;
  bleConnectionEvent_ = -1;
  bleRestartPending_ = true;
  portEXIT_CRITICAL(&bleStateMux_);
  if (bleNotifyMutex_ != nullptr) {
    xSemaphoreGive(bleNotifyMutex_);
  }
}

void TransportHub::onBleNotificationsChanged(bool enabled) {
  if (bleNotifyMutex_ != nullptr) {
    xSemaphoreTake(bleNotifyMutex_, portMAX_DELAY);
  }
  portENTER_CRITICAL(&bleStateMux_);
  bleNotificationsEnabled_ = enabled;
  bleNotificationsEnabledAtMs_ = enabled ? millis() : 0;
  bleSubscriptionEvent_ = enabled ? 1 : -1;
  portEXIT_CRITICAL(&bleStateMux_);
  if (bleNotifyMutex_ != nullptr) {
    xSemaphoreGive(bleNotifyMutex_);
  }
}

void TransportHub::onBleWrite(const char *data, size_t length) {
  if (data == nullptr || length == 0) {
    return;
  }

  portENTER_CRITICAL(&bleInputMux_);
  const bool accepted = bleInput_.push(data, length);
  portEXIT_CRITICAL(&bleInputMux_);

  if (!accepted) {
    portENTER_CRITICAL(&bleStateMux_);
    bleInputOverflowPending_ = true;
    portEXIT_CRITICAL(&bleStateMux_);
  }
}

#else
bool TransportHub::initializeBleStack() { return false; }
void TransportHub::startBle() {}
void TransportHub::stopBle() {}
void TransportHub::onBleConnected() {}
void TransportHub::onBleDisconnected() {}
void TransportHub::onBleNotificationsChanged(bool enabled) { (void)enabled; }
void TransportHub::onBleWrite(const char *data, size_t length) {
  (void)data;
  (void)length;
}
#endif

void TransportHub::fillOutputQueue(
  uint32_t &cursor,
  ByteRingBuffer<AppConfig::TRANSPORT_OUTPUT_BUFFER_BYTES> &queue,
  uint8_t eventBudget,
  const char *transportName,
  TransportMask transportMask
) {
  EventRecord record;
  uint8_t emitted = 0;

  while (emitted < eventBudget) {
    bool gap = false;
    uint32_t nextCursor = cursor;
    if (!events_.nextAfter(nextCursor, record, gap)) {
      if (gap) {
        cursor = nextCursor;
      }
      break;
    }

    if ((record.targetMask & transportMask) == 0) {
      cursor = nextCursor;
      continue;
    }

    const String line = formatEvent(record);
    String skipped;
    if (gap) {
      skipped = "[warning][" + String(transportName) +
        "] output skipped overwritten events\n";
    }

    // Reserve space for the gap marker and its first retained event together.
    // Otherwise a nearly full transport queue can append the same gap marker on
    // every service pass without ever advancing its event cursor.
    if (skipped.length() + line.length() > queue.freeSpace()) {
      break;
    }
    if (gap) {
      queue.push(skipped);
    }
    queue.push(line);
    cursor = nextCursor;
    emitted++;
  }
}

void TransportHub::flushUsb() {
  if (usbOutput_.size() == 0) {
    return;
  }
  const size_t available = static_cast<size_t>(max(Serial.availableForWrite(), 0));
  const size_t maximum = min(available, AppConfig::USB_WRITE_BUDGET);
  if (maximum == 0) {
    return;
  }
  char buffer[AppConfig::USB_WRITE_BUDGET];
  const size_t count = usbOutput_.peek(buffer, maximum);
  if (count > 0) {
    const size_t written = Serial.write(reinterpret_cast<const uint8_t *>(buffer), count);
    usbOutput_.discard(min(written, count));
  }
}

void TransportHub::flushSpp() {
#if APP_CLASSIC_BT_SPP_ENABLED
  if (!sppConnected_ || sppOutput_.size() == 0) {
    return;
  }
  char buffer[AppConfig::SPP_WRITE_BUDGET];
  const size_t count = sppOutput_.peek(buffer, sizeof(buffer));
  if (count > 0) {
    const size_t written = serialBt_.write(reinterpret_cast<const uint8_t *>(buffer), count);
    sppOutput_.discard(min(written, count));
  }
#endif
}

#if APP_BLE_ENABLED
void TransportHub::flushBle() {
  if (!isBleOutputReady() || bleTx_ == nullptr) {
    return;
  }
  // Arduino-ESP32 3.3.x keeps the characteristic-to-service accessor private.
  // Track the objects we created instead of reaching into the library internals.
  // The GATT service stays allocated for the lifetime of the warm BLE stack.
  if (!bleInitialized_ || bleServer_ == nullptr || bleService_ == nullptr ||
      bleServer_->getConnectedCount() == 0) {
    portENTER_CRITICAL(&bleStateMux_);
    bleConnected_ = false;
    bleNotificationsEnabled_ = false;
    bleSubscriptionEvent_ = -1;
    portEXIT_CRITICAL(&bleStateMux_);
    bleOutput_.clear();
    bleDirectOutput_ = "";
    bleDirectOffset_ = 0;
    return;
  }
  const uint32_t now = millis();
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t largestBlock = ESP.getMaxAllocHeap();
  if (
    freeHeap < AppConfig::BLE_TX_MIN_FREE_HEAP_BYTES ||
    largestBlock < AppConfig::BLE_TX_MIN_LARGEST_BLOCK_BYTES
  ) {
    // Retain the queued bytes and retry after transient HTTP/TCP allocations are
    // released. Advancing the cursor here would falsely report successful
    // delivery and permanently discard the cross-transport payload.
    if (
      bleLastPressureWarningAtMs_ == 0 ||
      static_cast<uint32_t>(now - bleLastPressureWarningAtMs_) >=
        AppConfig::BLE_TX_PRESSURE_WARNING_INTERVAL_MS
    ) {
      bleLastPressureWarningAtMs_ = now;
      events_.publish(
        EventLevel::WARNING,
        "BLE output paused for heap recovery free=" + String(freeHeap) +
          " largest=" + String(largestBlock),
        CommandSource::INTERNAL,
        String(),
        static_cast<TransportMask>(TRANSPORT_MASK_USB | TRANSPORT_MASK_UART | TRANSPORT_MASK_WIFI)
      );
    }
    return;
  }
  if (static_cast<uint32_t>(now - lastBleNotifyAtMs_) < AppConfig::BLE_NOTIFY_INTERVAL_MS) {
    return;
  }

  char buffer[AppConfig::BLE_SAFE_PAYLOAD_BYTES];
  size_t count = 0;
  bool directFrame = false;

  if (bleOutput_.size() == 0 && bleDirectOffset_ < bleDirectOutput_.length()) {
    directFrame = true;
    count = min(
      sizeof(buffer),
      static_cast<size_t>(bleDirectOutput_.length() - bleDirectOffset_)
    );
    memcpy(buffer, bleDirectOutput_.c_str() + bleDirectOffset_, count);
  } else {
    count = bleOutput_.peek(buffer, sizeof(buffer));
  }

  if (count == 0) {
    return;
  }

  // Serialize the final GATT write against the BLE disconnect and CCCD callbacks.
  // Arduino-ESP32 calls the user disconnect callback before removing its peer
  // record, so this ordinary FreeRTOS mutex closes the check-to-notify race
  // without holding a critical section across Bluedroid calls.
  if (bleNotifyMutex_ == nullptr || xSemaphoreTake(bleNotifyMutex_, 0) != pdTRUE) {
    return;
  }
  const bool ready = isBleOutputReady() && bleServer_->getConnectedCount() > 0;
  if (!ready) {
    xSemaphoreGive(bleNotifyMutex_);
    return;
  }
  bleTx_->setValue(reinterpret_cast<uint8_t *>(buffer), count);
  if (!isBleOutputReady() || bleServer_->getConnectedCount() == 0) {
    xSemaphoreGive(bleNotifyMutex_);
    return;
  }
  bleTx_->notify();
  xSemaphoreGive(bleNotifyMutex_);

  if (directFrame) {
    bleDirectOffset_ += count;
    if (bleDirectOffset_ >= bleDirectOutput_.length()) {
      bleDirectOutput_ = "";
      bleDirectOffset_ = 0;
    }
  } else {
    // BLE notifications are best-effort at the protocol level. The local chunk
    // is consumed after submission; event-ring gap reporting handles a slow client.
    bleOutput_.discard(count);
  }
  lastBleNotifyAtMs_ = now;
}

#else
void TransportHub::flushBle() {}
#endif

void TransportHub::flushUart() {
#if APP_AUX_UART_ENABLED
  if (!uartRunning_ || uartOutput_.size() == 0) {
    return;
  }
  const size_t available = static_cast<size_t>(max(auxUart_.availableForWrite(), 0));
  const size_t maximum = min(available, AppConfig::UART_WRITE_BUDGET);
  if (maximum == 0) {
    return;
  }
  char buffer[AppConfig::UART_WRITE_BUDGET];
  const size_t count = uartOutput_.peek(buffer, maximum);
  if (count > 0) {
    const size_t written = auxUart_.write(reinterpret_cast<const uint8_t *>(buffer), count);
    uartOutput_.discard(min(written, count));
  }
#endif
}

String TransportHub::formatEvent(const EventRecord &record) const {
  if (record.rawPayload) {
    String line(record.text);
    line += '\n';
    return line;
  }

  String line;
  line.reserve(strlen(record.text) + 64);
  line += '[';
  line += String(record.id);
  line += "][";
  line += eventLevelName(record.level);
  line += "][";

  const String sourceName = commandSourceName(record.source);
  const String requestId = record.requestId;
  const String generatedPrefix = sourceName + "-";
  bool generatedRequestId =
    requestId.length() > generatedPrefix.length() &&
    requestId.startsWith(generatedPrefix);
  if (generatedRequestId) {
    for (size_t index = generatedPrefix.length(); index < requestId.length(); index++) {
      if (!isDigit(requestId[index])) {
        generatedRequestId = false;
        break;
      }
    }
  }

  line += sourceName;
  if (generatedRequestId) {
    line += '#';
    line += requestId.substring(generatedPrefix.length());
  }
  line += ']';

  if (requestId.length() > 0 && !generatedRequestId) {
    line += '[';
    line += requestId;
    line += ']';
  }
  line += ' ';
  line += record.text;
  line += '\n';
  return line;
}
