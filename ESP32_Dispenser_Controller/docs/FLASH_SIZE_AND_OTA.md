# Flash, RAM, and OTA notes

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
Set the quick profile to `2` only for a compatible bench build. Commenting out
`APP_ENABLE_STATUS_INDICATORS` compiles all indicator GPIO activity out.

The hosted portal is gzip-compressed into `src/web/WebAssets.h`. `build_opt.h`
keeps the AsyncTCP stack configuration beside the Arduino sketch.

For `WEMOS LOLIN32`, select `Minimal SPIFFS (Large APPS with OTA)`. It provides
the two large application slots required by this build and its uploader. The
default 1.25 MB application partition is too small, while `Huge APP` does not
provide the inactive application slot required by OTA. The Arduino linker
output is the authoritative application-size check.

With ESP32 core 3.3.10 and the current library set, the warning-clean builds
measure approximately:

| Profile | Application bytes | OTA-slot use | Static RAM |
|---|---:|---:|---:|
| Dispenser | 1,931,127 | 98% | 71,288 bytes |
| Stepper/DAC bench | 1,959,299 | 99% | 71,608 bytes |

The advanced profile has only about 6.6 KiB of application headroom. Before
extending it, compile after every material change. If BLE is unnecessary on the
bench, disabling `APP_ENABLE_BLE` is the most effective reduction. If OTA is
also deliberately removed, disable `APP_ENABLE_HTTP_OTA` before choosing a
non-OTA partition layout.

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
