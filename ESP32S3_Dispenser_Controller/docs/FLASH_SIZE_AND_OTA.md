# Flash, RAM, and OTA notes

This separate firmware targets ESP32-S3-DevKitC-1-N16R8. See the root
[board settings and pinout](../README.md#exact-arduino-board-settings).
Use `esp32:esp32:esp32s3`, `FlashSize=16M`, `FlashMode=qio`, `PSRAM=opi`, and
`PartitionScheme=app3M_fat9M_16MB` for two 3,145,728-byte OTA application slots.
Use `USBMode=hwcdc,CDCOnBoot=cdc` for the native USB connector; disable CDC On
Boot to use the COM (USB-to-UART) connector instead.

The beginner hardware-profile switch lives in `QuickConfig.h`:

```cpp
#define DRONE_CFG_HARDWARE_PROFILE 1
```

Advanced compile-time feature switches live in `src/config/AppConfig.h`:

```cpp
#define APP_ENABLE_WIFI
#define APP_ENABLE_BLE
// #define APP_ENABLE_CLASSIC_BT_SPP
#define APP_ENABLE_HTTP_OTA
#define APP_ENABLE_STATUS_INDICATORS
#define APP_ENABLE_STATUS_LED_BOOT_TEST
// #define APP_ENABLE_MDNS
// #define APP_ENABLE_AUX_UART
```

The default dispenser profile excludes the complete stepper/DAC implementation.
Keep profile `1`; the S3 has no internal DAC for profile `2`. Commenting out
`APP_ENABLE_STATUS_INDICATORS` compiles all indicator GPIO activity out.

The hosted portal is gzip-compressed into `src/web/WebAssets.h`. After editing
`web/index.html`, `web/app.css`, or `web/app.js`, run
`node web/build_web_assets.mjs`; use `--check` to detect stale generated assets
without writing. The script requires no npm packages. `build_opt.h` keeps the
AsyncTCP stack configuration beside the Arduino sketch.

The application must fit in one 3 MiB slot. The S3 partition layout uses the
16 MB flash and preserves an inactive slot for OTA; do not choose a No OTA
layout. The installed N16R8 uses quad flash plus 8 MB octal PSRAM, so select
QIO flash and OPI PSRAM. PSRAM does not replace the internal DMA-capable heap
needed by Wi-Fi and Bluetooth; existing heap admission limits remain active.

## OTA transfer

The portal sends the selected application `.bin` as `multipart/form-data` and includes the exact file length in `X-Firmware-Size`. Firmware rejects the upload before writing when any of these checks fail:

- filename does not end in `.bin`
- declared size is missing, below 1 KiB, or larger than the inactive application slot
- first image byte is not the ESP32 application magic byte `0xE9`
- chunks arrive out of order or exceed the declared size
- the final byte count differs from the declared size
- `Update.end(false)` cannot validate and finalize the exact image

A disconnect during an active upload aborts `Update` and leaves the current application selected. The reboot timer starts only after the final HTTP success response is queued.

The Web Bluetooth console can initiate OTA. It sends `StopAll`, disconnects BLE
to release coexistence heap, verifies the Wi-Fi endpoint, and then performs the
same multipart Wi-Fi upload. Independently of the browser, a validated upload
raises a control-task safety stop and enqueues a `StopAll` barrier before flash
writing begins. The firmware image is not base64-encoded or pushed through the
20-byte BLE command channel.

## Coexistence limits

Normal Wi-Fi plus BLE operation uses a 9,000-byte remaining free-heap floor and a 6,000-byte largest-block floor for new HTTP work. OTA reserves 1,536 bytes of the bounded HTTP request budget and rejects unrelated new HTTP requests while flash writing is active. BLE output remains queued and pauses only at its hard recovery floor.

Test the final binary on the actual board under portal polling, BLE notifications,
dispenser pulses, saved routines, and OTA. Confirm the partition scheme, reset
reason, safe GPIO state, and recovery path over USB.
