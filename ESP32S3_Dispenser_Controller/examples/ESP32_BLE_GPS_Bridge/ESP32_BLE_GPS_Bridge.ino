// ESP32 BLE central: forward live, unsigned MAVLink GPS from a UART telemetry
// source to DroneGelBLE. Configure the address and UART pins before uploading.
// This expects MAVLink GPS_RAW_INT + GLOBAL_POSITION_INT, not NMEA sentences.
#include <Arduino.h>
#include <BLEDevice.h>
#include "../../src/core/MavlinkPosition.h"

constexpr char CONTROLLER_ADDRESS[] = "00:00:00:00:00:00"; // Replace with dispenser BLE MAC.
constexpr int GPS_RX_PIN = 16;
constexpr int GPS_TX_PIN = 17;
constexpr uint32_t GPS_BAUD = 57600;
constexpr char SERVICE[] = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
constexpr char GPS_CHANNEL[] = "6e400004-b5a3-f393-e0a9-e50e24dcca9e";
BLEClient *client = nullptr;
BLERemoteCharacteristic *gpsChannel = nullptr;
uint8_t bytes[280];
size_t used = 0, expected = 0;
uint32_t startedAt = 0, retryAt = 0;

void setup() {
  Serial.begin(115200);
  Serial2.setRxBufferSize(2048);
  Serial2.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  BLEDevice::init("GPS-Bridge");
  client = BLEDevice::createClient();
}

void loop() {
  if (!client || !client->isConnected()) {
    gpsChannel = nullptr; used = expected = 0;
    // Discard buffered telemetry during downtime; never replay it on reconnect.
    while (Serial2.available()) Serial2.read();
    if (static_cast<int32_t>(millis() - retryAt) < 0) { delay(5); return; }
    retryAt = millis() + 3000;
    if (!client || !client->connect(BLEAddress(CONTROLLER_ADDRESS))) return;
    auto *service = client->getService(BLEUUID(SERVICE));
    gpsChannel = service ? service->getCharacteristic(BLEUUID(GPS_CHANNEL)) : nullptr;
    if (!gpsChannel || !gpsChannel->canWrite()) { client->disconnect(); return; }
    while (Serial2.available()) Serial2.read();
    Serial.println("GPS BLE connected; select GeoSource:BLE on the dispenser.");
  }
  // The controller has a bounded receive queue. Limit the UART source to
  // GPS messages at 2-5 Hz; avoid forwarding a complete high-rate telemetry bus.
  while (Serial2.available() && client->isConnected()) {
    const uint8_t byte = Serial2.read();
    if (used && millis() - startedAt > 500) used = expected = 0;
    if (!used) {
      if (byte != 0xFD && byte != 0xFE) continue;
      startedAt = millis();
    }
    bytes[used++] = byte;
    if (used == 2) expected = bytes[1] + (bytes[0] == 0xFD ? 12 : 8);
    if (used == 3 && bytes[0] == 0xFD && (bytes[2] & 1)) expected += 13;
    if (!expected || used < expected) continue;
    size_t offset = 0;
    MavlinkPosition::Message message{};
    if (MavlinkPosition::next(bytes, used, offset, message)) {
      for (size_t start = 0; start < used && client->isConnected(); start += 20) {
        if (millis() - startedAt > 500) { client->disconnect(); break; }
        // Response provides backpressure; frame assembly can span BLE writes.
        gpsChannel->writeValue(bytes + start, min(size_t(20), used - start), true);
      }
    }
    used = expected = 0;
  }
  delay(2);
}
