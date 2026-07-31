# v5.15.42 OTA and indicator pin update

- Corrected the browser OTA request to use a multipart file upload.
- Added exact file-size, extension, ESP32 image-header, chunk-order, overflow, and final-length checks.
- Finalizes with `Update.end(false)` and schedules reboot only after the completion response is queued.
- Aborts an interrupted upload without selecting the partial image.
- Allows the BLE console to initiate OTA through an automatic BLE-to-Wi-Fi handoff.
- Moved connection indication to GPIO15 and activity indication to GPIO2.
- Added default-on build switches for all status indication and for the boot blink test independently.
- Isolated the blocking boot LED pattern in `StatusIndicators::runBootSelfTest()`.
- Preserved the v5.15.41 coexistence heap and delivery behavior.
