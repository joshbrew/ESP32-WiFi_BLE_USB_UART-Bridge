# v5.15.41 coexistence delivery

- BLE output is retained and retried during temporary heap pressure instead of being discarded.
- BLE notification admission now uses a hard low-memory floor suitable for 20-byte GATT frames.
- HTTP admission reserves route-specific memory and permits normal requests at the observed Wi-Fi + BLE coexistence heap level.
- The hosted console polls less aggressively to reduce TCP allocation churn.
- Cross-transport acknowledgements say `Queued for` because delivery completes asynchronously.
- Boot logs now print the ESP32 reset reason so watchdog, panic, brownout, and software resets are distinguishable.
