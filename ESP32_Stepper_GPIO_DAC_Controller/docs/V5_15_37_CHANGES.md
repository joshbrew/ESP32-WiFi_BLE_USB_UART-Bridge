# v5.15.37

- Fixed Arduino-ESP32 3.3.10 compilation by removing calls to the private `BLECharacteristic::getService()` API.
- The transport now retains its own `BLEService *` created during BLE initialization.
- BLE output readiness is checked using owned server/service/characteristic state plus the public connected-client count.
- A disconnected or invalid BLE output state clears queued BLE output without calling `notify()`.
- Retains the DAC hold/release controls and raw deliberate `Send*` payload behavior from v5.15.36.
