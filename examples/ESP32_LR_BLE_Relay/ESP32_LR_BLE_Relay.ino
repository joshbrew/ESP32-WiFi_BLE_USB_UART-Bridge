// Phone BLE UART <-> this relay <-> LR WiFi HTTP <-> controller firmware.
// See ../../docs/WIFI_LR.md. Arduino-ESP32 3.x; ArduinoJson 7.x.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <atomic>

#if !CONFIG_IDF_TARGET_ESP32
#error "This BLE relay example targets classic ESP32; choose an ESP32/LOLIN32 board"
#endif

// Match the remote controller's AP credentials; change both before field use.
constexpr const char *REMOTE_SSID = "ESP32-LR-Remote";
constexpr const char *REMOTE_PASSWORD = "lrbench123";
constexpr const char *REMOTE_URL = "http://192.168.4.1";
constexpr const char *RELAY_BLE_NAME = "ESP32-LR-Relay";
constexpr const char *SERVICE_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr const char *RX_UUID = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr const char *TX_UUID = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr size_t MAX_COMMAND = 255;
constexpr size_t MAX_OUTPUT = 2304;
constexpr uint32_t POLL_MS = 1000;
constexpr uint32_t COMMAND_TTL_MS = 4000;

struct InputChunk { char bytes[MAX_COMMAND + 1]; size_t length; };
struct CommandLine { char text[MAX_COMMAND + 1]; uint32_t receivedAt; };
struct OutputLine { char text[MAX_OUTPUT]; };
QueueHandle_t inputQueue, commandQueue, outputQueue;
BLECharacteristic *txCharacteristic;
BLE2902 *txSubscription;
std::atomic<bool> bleConnected{false}, advertiseAgain{false};
std::atomic<bool> inputOverflow{false}, outputOverflow{false};
std::atomic<bool> rxStreamBroken{false}, statePending{false};
String bleLine, usbLine;
bool bleDiscard = false, usbDiscard = false;

// Only the network worker produces output. BLE callbacks never perform HTTP.
void emitLine(const String &text) {
  OutputLine output{};
  String line = text;
  line.replace("\r", "");
  line.replace("\n", " ");
  if (line.length() >= sizeof(output.text)) {
    line = line.substring(0, sizeof(output.text) - 16) + " [truncated]";
  }
  line.toCharArray(output.text, sizeof(output.text));
  if (xQueueSend(outputQueue, &output, 0) != pdTRUE) outputOverflow = true;
}

class RelayServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *) override { bleConnected = true; }
  void onDisconnect(BLEServer *) override {
    bleConnected = false;
    advertiseAgain = true;
  }
};

class RelayRxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    const auto value = characteristic->getValue();
    if (value.length() == 0) return;
    InputChunk chunk{};
    if (value.length() > MAX_COMMAND) { inputOverflow = true; rxStreamBroken = true; return; }
    chunk.length = value.length();
    memcpy(chunk.bytes, value.c_str(), chunk.length);
    if (xQueueSend(inputQueue, &chunk, 0) != pdTRUE) {
      inputOverflow = true;
      rxStreamBroken = true;
    }
  }
};

void consumeByte(char byte, String &line, bool &discard) {
  if (byte == '\r') return;
  if (byte == '\n') {
    if (!discard) {
      line.trim();
      if (line.length()) {
        if (line == "@STATE" && statePending.exchange(true)) {
          line = ""; // Coalesce browser state polls while an answer is in flight.
          return;
        }
        CommandLine command{};
        line.toCharArray(command.text, sizeof(command.text));
        command.receivedAt = millis();
        const bool stopping = line.equalsIgnoreCase("StopAll") || line.equalsIgnoreCase("RoutineStop") ||
          line.equalsIgnoreCase("GeoStop") || line.equalsIgnoreCase("DispenseStop") || line.equalsIgnoreCase("Disarm");
        if (stopping) xQueueReset(commandQueue);
        if (xQueueSend(commandQueue, &command, 0) != pdTRUE) {
          Serial.println("[RELAY][ERROR] command queue full; command discarded");
          inputOverflow = true;
          if (line == "@STATE") statePending = false;
        }
      }
    }
    line = "";
    discard = false;
  } else if (!discard) {
    if (line.length() >= MAX_COMMAND || byte == '\0') {
      line = "";
      discard = true;
      inputOverflow = true;
    } else {
      line += byte;
    }
  }
}

bool httpJson(const String &path, const CommandLine *command, JsonDocument &response) {
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(1500);
  http.setTimeout(2500);
  if (!http.begin(client, String(REMOTE_URL) + path)) return false;
  http.useHTTP10(true); // Bounded JSON parsing from a stream, without chunk framing.
  int code;
  if (command) {
    static uint32_t sequence = 0;
    static const uint32_t session = esp_random();
    const String requestId = "lr-" + String(session, HEX) + "-" + String(++sequence);
    http.addHeader("Content-Type", "text/plain");
    http.addHeader("X-Request-ID", requestId);
    code = http.POST(String(command->text));
  } else {
    code = http.GET();
  }
  bool ok = code == 200 && http.getSize() >= 0 && http.getSize() <= 4096;
  if (ok) {
    ok = !deserializeJson(response, http.getStream(), DeserializationOption::NestingLimit(8));
  }
  http.end();
  if (!ok) {
    emitLine("[RELAY][ERROR] HTTP " + String(code) +
      (command ? "; delivery uncertain; command will NOT be retried" : "; event poll failed"));
  }
  return ok;
}

void networkWorker(void *) {
  uint32_t cursor = 0, lastPoll = 0, lastCursorCheck = 0;
  bool wasConnected = false, cursorReady = false;
  for (;;) {
    const bool connected = WiFi.status() == WL_CONNECTED;
    if (connected != wasConnected) {
      emitLine(connected ? "[RELAY] LR connected ip=" + WiFi.localIP().toString()
                         : "[RELAY][ERROR] LR disconnected; queued commands discarded");
      wasConnected = connected;
      cursorReady = false;
      // Never deliver a command buffered across an observed link change.
      xQueueReset(commandQueue);
      if (!connected) rxStreamBroken = true;
      statePending = false;
    }
    if (inputOverflow.exchange(false)) emitLine("[RELAY][ERROR] input dropped; resend a complete newline-terminated command");
    if (outputOverflow.exchange(false)) emitLine("[RELAY][ERROR] output dropped; reduce telemetry rate");

    // Initialize at the remote's current cursor instead of replaying old events.
    // A reconnect deliberately starts a new event window.
    if (connected && !cursorReady) {
      JsonDocument snapshot;
      if (httpJson("/api/events?limit=1", nullptr, snapshot) && snapshot["ok"].as<bool>()) {
        cursor = snapshot["cursor"].as<uint32_t>();
        cursorReady = true;
      }
    }
    CommandLine command{};
    if (xQueueReceive(commandQueue, &command, 0) == pdTRUE) {
      if (!connected || !cursorReady || static_cast<uint32_t>(millis() - command.receivedAt) > COMMAND_TTL_MS) {
        emitLine("[RELAY][ERROR] offline, not ready, or expired; command discarded");
        if (String(command.text) == "@STATE") statePending = false;
      } else if (String(command.text) == "@STATE") {
        // @STATE is a BLE transport request, not an HTTP command. Return a
        // compact remote snapshot in the browser console's usual framing.
        JsonDocument remote;
        if (httpJson("/api/state", nullptr, remote)) {
          JsonDocument compact;
          const char *fields[] = {"firmware", "version", "freeHeap", "largestHeapBlock"};
          for (const char *key : fields) compact[key] = remote[key];
          const char *radioFields[] = {"bootModeActive", "wifiState", "ip", "wifiCompiled", "bleCompiled", "sppCompiled", "wifiLRActive"};
          for (const char *key : radioFields) compact["radio"][key] = remote["radio"][key];
          // The console's BLE connection is to this relay. Report that local
          // capability separately from the remote's WiFi mode above.
          compact["radio"]["bleCompiled"] = true;
          compact["send"]["ble"] = bleConnected.load();
          compact["send"]["bleConnected"] = bleConnected.load();
          compact["addon"]["name"] = remote["addon"]["name"];
          compact["addon"] = remote["addon"];
          compact["dispenser"] = remote["dispenser"];
          compact["routine"] = remote["routine"];
          compact["geo"] = remote["geo"];
          compact["ok"] = remote["ok"];
          String line = "@STATE ";
          serializeJson(compact, line);
          if (uxQueueSpacesAvailable(outputQueue) && line.length() < MAX_OUTPUT - 1) {
            emitLine(line);
          } else {
            statePending = false;
          }
        } else {
          statePending = false;
        }
      } else {
        JsonDocument ack;
        if (httpJson("/api/command", &command, ack)) {
          emitLine(ack["ok"].as<bool>() ? "[RELAY][ACK] remote accepted command; execution result follows"
                                      : "[RELAY][ERROR] remote rejected command");
        }
      }
    }
    if (connected && cursorReady && millis() - lastCursorCheck >= 10000) {
      lastCursorCheck = millis();
      JsonDocument latest;
      // The event API echoes an out-of-range 'since' cursor. Probe without
      // 'since' to detect a remote reboot while WiFi association stays up.
      if (httpJson("/api/events?limit=1", nullptr, latest) && latest["ok"].as<bool>()) {
        const uint32_t current = latest["cursor"].as<uint32_t>();
        if (current < cursor) {
          cursor = current;
          emitLine("[RELAY] remote event cursor reset (controller restarted)");
        }
      }
    }
    if (connected && cursorReady && millis() - lastPoll >= POLL_MS && uxQueueSpacesAvailable(outputQueue) >= 3) {
      lastPoll = millis();
      JsonDocument events;
      if (httpJson("/api/events?since=" + String(cursor) + "&limit=2", nullptr, events) && events["ok"].as<bool>()) {
        const uint32_t next = events["cursor"].as<uint32_t>();
        if (events["gap"].as<bool>()) emitLine("[RELAY][WARNING] remote event history gap");
        for (JsonObject event : events["events"].as<JsonArray>()) {
          emitLine(String(event["t"] | ""));
        }
        cursor = next;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void setup() {
  Serial.begin(115200);
  inputQueue = xQueueCreate(16, sizeof(InputChunk));
  commandQueue = xQueueCreate(12, sizeof(CommandLine));
  outputQueue = xQueueCreate(8, sizeof(OutputLine));
  if (!inputQueue || !commandQueue || !outputQueue) {
    Serial.println("[RELAY][ERROR] queue allocation failed");
    while (true) delay(1000);
  }

  // Reserve BLE first, as in the main firmware's coexistence startup.
  BLEDevice::init(RELAY_BLE_NAME);
  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new RelayServerCallbacks());
  BLEService *service = server->createService(SERVICE_UUID);
  txCharacteristic = service->createCharacteristic(TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  txSubscription = new BLE2902();
  txCharacteristic->addDescriptor(txSubscription);
  BLECharacteristic *rx = service->createCharacteristic(RX_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new RelayRxCallbacks());
  service->start();
  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);

  WiFi.persistent(false);
  if (!WiFi.mode(WIFI_STA) || esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR) != ESP_OK) {
    Serial.println("[RELAY][ERROR] LR setup failed; use an LR-capable ESP32");
    while (true) delay(1000);
  }
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  WiFi.setAutoReconnect(true);
  WiFi.begin(REMOTE_SSID, REMOTE_PASSWORD);
  advertising->start();
  if (xTaskCreate(networkWorker, "lr-http", 8192, nullptr, 1, nullptr) != pdPASS) {
    Serial.println("[RELAY][ERROR] network task allocation failed");
    while (true) delay(1000);
  }
  Serial.println("[RELAY] ready; subscribe to BLE TX, send newline-terminated commands to RX");
}

void loop() {
  if (advertiseAgain.exchange(false)) {
    // Drop partial input and notification tails when the phone disconnects.
    bleLine = "";
    bleDiscard = false;
    xQueueReset(inputQueue);
    statePending = false;
    BLEDevice::startAdvertising();
  }
  if (rxStreamBroken.exchange(false)) {
    xQueueReset(inputQueue);
    bleLine = "";
    usbLine = ""; usbDiscard = true;
    bleDiscard = true; // Never splice a partial command around a missing chunk.
  }
  InputChunk chunk{};
  while (xQueueReceive(inputQueue, &chunk, 0) == pdTRUE) {
    for (size_t i = 0; i < chunk.length; ++i) consumeByte(chunk.bytes[i], bleLine, bleDiscard);
  }
  while (Serial.available()) consumeByte(static_cast<char>(Serial.read()), usbLine, usbDiscard);

  static String pendingOutput;
  static size_t offset = 0;
  static uint32_t lastNotify = 0;
  const bool subscribed = bleConnected && txSubscription->getNotifications();
  if (!subscribed) { pendingOutput = ""; offset = 0; statePending = false; }
  if (pendingOutput.length() == 0) {
    OutputLine output{};
    if (xQueueReceive(outputQueue, &output, 0) == pdTRUE) {
      Serial.println(output.text);
      if (!subscribed && String(output.text).startsWith("@STATE ")) statePending = false;
      if (subscribed) { pendingOutput = String(output.text) + "\n"; offset = 0; }
    }
  }
  if (subscribed && pendingOutput.length() && millis() - lastNotify >= 50) {
    const size_t count = min(static_cast<size_t>(20), pendingOutput.length() - offset);
    txCharacteristic->setValue(reinterpret_cast<uint8_t *>(const_cast<char *>(pendingOutput.c_str() + offset)), count);
    txCharacteristic->notify();
    offset += count;
    lastNotify = millis();
    if (offset == pendingOutput.length()) {
      if (pendingOutput.startsWith("@STATE ")) statePending = false;
      pendingOutput = "";
    }
  }
  delay(5);
}
