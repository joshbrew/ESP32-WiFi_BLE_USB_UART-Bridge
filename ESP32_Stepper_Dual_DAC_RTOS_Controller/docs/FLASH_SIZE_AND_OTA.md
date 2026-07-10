# Flash, RAM, and OTA notes

Compile-time switches live in `src/config/AppConfig.h`:

```cpp
#define APP_ADDON_STEPPER_DAC
#define APP_ENABLE_WIFI
#define APP_ENABLE_BLE
// #define APP_ENABLE_CLASSIC_BT_SPP
#define APP_ENABLE_HTTP_OTA
#define APP_ENABLE_STATUS_INDICATORS
#define APP_ENABLE_STATUS_LED_BOOT_TEST
// #define APP_ENABLE_MDNS
// #define APP_ENABLE_AUX_UART
```

Commenting out `APP_ADDON_STEPPER_DAC` excludes the complete stepper/DAC hardware pack. Commenting out `APP_ENABLE_STATUS_INDICATORS` leaves the indicator object and status API present but compiles all GPIO mode and write activity out. Commenting out only `APP_ENABLE_STATUS_LED_BOOT_TEST` keeps runtime connection/activity indication without boot pulses.

The hosted portal is minified and gzip-compressed into `src/web/WebAssets.h`. `build_opt.h` keeps the AsyncTCP stack configuration beside the Arduino sketch.

Use an OTA-capable partition layout with two application slots. The Arduino linker output is the authoritative application-size check. `Huge APP` does not provide the inactive application slot required by this uploader.

## OTA transfer

The portal sends the selected application `.bin` as `multipart/form-data` and includes the exact file length in `X-Firmware-Size`. Firmware rejects the upload before writing when any of these checks fail:

- filename does not end in `.bin`
- declared size is missing, below 1 KiB, or larger than the inactive application slot
- first image byte is not the ESP32 application magic byte `0xE9`
- chunks arrive out of order or exceed the declared size
- the final byte count differs from the declared size
- `Update.end(false)` cannot validate and finalize the exact image

A disconnect during an active upload aborts `Update` and leaves the current application selected. The reboot timer starts only after the final HTTP success response is queued.

The Web Bluetooth console can initiate OTA. It sends `StopAll`, disconnects BLE to release coexistence heap, verifies the Wi-Fi endpoint, and then performs the same multipart Wi-Fi upload. The firmware image is not base64-encoded or pushed through the 20-byte BLE command channel.

## Coexistence limits

Normal Wi-Fi plus BLE operation uses a 9,000-byte remaining free-heap floor and a 6,000-byte largest-block floor for new HTTP work. OTA reserves 1,536 bytes of the bounded HTTP request budget and rejects unrelated new HTTP requests while flash writing is active. BLE output remains queued and pauses only at its hard recovery floor.

Test the final binary on the actual board under portal polling, BLE notifications, motor activity, DAC output, and OTA. Confirm the selected partition scheme, reset reason, post-update firmware version, and rollback/recovery path over USB.
