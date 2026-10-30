# ESP32-S3 Modular Command + Transport Controller

This separate transport-only firmware targets ESP32-S3-DevKitC-1-N16R8 using
the **ESP32S3 Dev Module** Arduino board profile. The original LOLIN32 variant
remains in `../ESP32_Modular_Command_Transport_Controller`. It provides a shared command system over USB serial, Wi-Fi HTTP, BLE UART, and optional auxiliary UART. It also includes a browser console, bounded event queues, radio-profile rollback, status indicators, a self-test framework, and validated HTTP OTA.

## Table of contents

- [What this firmware does](#what-this-firmware-does)
- [Hardware assumptions](#hardware-assumptions)
- [Build switches](#build-switches)
- [Status indicator pins](#status-indicator-pins)
- [Build and flash](#build-and-flash)
- [Default radio behavior](#default-radio-behavior)
- [Web console](#web-console)
- [BLE UART service](#ble-uart-service)
- [Command transports](#command-transports)
- [Radio profiles](#radio-profiles)
- [Outbound send routing](#outbound-send-routing)
- [HTTP and coexistence limits](#http-and-coexistence-limits)
- [Firmware OTA](#firmware-ota)
- [Self-test](#self-test)
- [Command list summary](#command-list-summary)
- [Demo flows](#demo-flows)
- [Source layout](#source-layout)
- [Adding a user hardware addon](#adding-a-user-hardware-addon)
- [Troubleshooting](#troubleshooting)
- [Notes before deployment](#notes-before-deployment)

## What this firmware does

- Boots into a saved radio profile with transactional rollback if the new profile fails.
- Exposes the same command queue through USB serial, Wi-Fi, BLE, optional SPP, and optional auxiliary UART.
- Hosts a compact browser console at the ESP32 HTTP address.
- Supports Web Bluetooth using a Nordic-UART-style BLE service.
- Routes explicit `Send*` payloads between active transports without echo loops.
- Keeps HTTP request count, body storage, response storage, and BLE output bounded for Wi-Fi/BLE coexistence.
- Supports application firmware OTA through the browser.
- Includes connection and activity indicators that can be compiled out completely.
- Includes a persistent multi-boot self-test and restore framework.
- Leaves the hardware addon slot empty for a user program.

## Hardware assumptions

Target: ESP32-S3-DevKitC-1-N16R8, with 16 MB quad flash, 8 MB octal PSRAM,
and an addressable RGB LED. The supplied board diagram connects its LED to
**GPIO48**. Espressif v1.1 boards instead use GPIO38; edit `APP_STATUS_RGB_PIN`
in `src/config/AppConfig.h` for that revision.

The base firmware owns only the onboard RGB LED. It starts with an empty
`DeviceAddon`; there is no dispenser, stepper, or DAC output in this build.
Wi-Fi and BLE are supported; Classic Bluetooth SPP is unavailable on ESP32-S3
and enabling its build switch produces a clear compile error.

Use GPIO numbers printed on the board, not header positions. GPIO23 does not
exist on S3; GPIO26..37 belong to memory or are not exposed on this module.
In particular, GPIO35..37 are occupied by the N16R8 octal PSRAM despite being
shown on the headers. Preserve USB GPIO19/20 and the COM bridge's GPIO43/44.
Avoid boot straps GPIO0/3/45/46 and both RGB revision pins GPIO38/48.

`AppConfig::isAvailableAddonPin()` supplies a conservative header allowlist:
GPIO1, 2, 4..18, 21, 39..42, and 47. GPIO39..42 also serve external JTAG;
leave JTAG Adapter disabled when assigning them to an addon. Auxiliary UART
starts disabled with RX/TX `-1`; assign two distinct available pins before
enabling it (for example RX17/TX18). Compile-time checks reject reserved pins.

Hardware references: [DevKitC-1 pinout](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.0.html),
[RGB revision change](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.1.html),
[N16R8 module pin reservations](https://documentation.espressif.com/esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf).

## Build switches

Edit `src/config/AppConfig.h`:

```cpp
#define APP_ENABLE_WIFI
#define APP_ENABLE_BLE
// #define APP_ENABLE_CLASSIC_BT_SPP

#define APP_ENABLE_HTTP_OTA
#define APP_ENABLE_STATUS_INDICATORS
#define APP_ENABLE_STATUS_LED_BOOT_TEST
// #define APP_ENABLE_MDNS
// #define APP_ENABLE_AUX_UART

// #define APP_ENABLE_VERBOSE_COEX_HTTP_DIAGNOSTICS
```

Behavior:

```text
APP_ENABLE_WIFI                  Compile Wi-Fi, HTTP, DNS setup portal, and OTA transport
APP_ENABLE_BLE                   Compile BLE UART command transport
APP_ENABLE_CLASSIC_BT_SPP        Compile Classic Bluetooth serial transport
APP_ENABLE_HTTP_OTA              Compile browser application OTA when Wi-Fi is enabled
APP_ENABLE_STATUS_INDICATORS     Drive the onboard addressable RGB LED
APP_ENABLE_STATUS_LED_BOOT_TEST  Run the startup LED pulse sequence
APP_ENABLE_MDNS                  Start mDNS when Wi-Fi is active
APP_ENABLE_AUX_UART              Enable the optional hardware UART transport
```

Commenting out `APP_ENABLE_STATUS_INDICATORS` removes LED GPIO and RGB-driver activity.
`APP_STATUS_RGB_BRIGHTNESS` controls brightness from 1 to 255 (default 24).

## Status indicator pins

| Visible color | Meaning |
|---|---|
| Green | Connected; blinks while seeking a connection |
| Blue | Command/activity pulse; takes priority over green |
| Amber | An addon reports active work or commands remain queued |
| Off | No connection or activity |

The RGB LED is on GPIO48 by default. No external indicator wiring or extra
LED library is needed. Arduino core `rgbLedWrite()` sends frames only when the
visible color changes. Legacy discrete connection/activity pin fields report
`-1`; indicator status/JSON identifies the RGB backend and its physical pin.

Configuration is in `src/config/AppConfig.h`; all runtime and self-test behavior
is in `src/hardware/StatusIndicators.h/.cpp`.

```text
IndicatorStatus
IndicatorTest              green twice, then blue three times
IndicatorConnectionTest    four green pulses
IndicatorActivityTest      four blue pulses
```

Production boot briefly shows green then blue. Comment out only
`APP_ENABLE_STATUS_LED_BOOT_TEST` to retain runtime indication without the
startup sequence.

## Build and flash

### Arduino IDE

1. Open `ESP32S3_Modular_Command_Transport_Controller.ino` in this folder.
2. Use Espressif Arduino ESP32 core **3.3.10** (the reference build).
3. Install maintained `ESP32Async/AsyncTCP` and `ESP32Async/ESPAsyncWebServer`.
   Reference versions are **3.5.0** and **3.12.1**, respectively.
4. Connect to the connector labeled **USB**, and select its Arduino port.
5. Set Tools as follows, then compile and upload:

| Tools setting | Value |
|---|---|
| Board | **ESP32S3 Dev Module** |
| USB CDC On Boot | **Enabled** |
| CPU Frequency | 240MHz (WiFi) |
| Core Debug Level | None |
| USB DFU On Boot | Disabled |
| Erase All Flash Before Sketch Upload | Disabled |
| Events Run On | Core 1 |
| Flash Mode | QIO 80MHz |
| Flash Size | **16MB (128Mb)** |
| JTAG Adapter | Disabled |
| Arduino Runs On | Core 1 |
| USB Firmware MSC On Boot | Disabled |
| Partition Scheme | **16M Flash (3MB APP/9.9MB FATFS)** |
| PSRAM | **OPI PSRAM** |
| Upload Mode | UART0 / Hardware CDC |
| Upload Speed | 921600 |
| USB Mode | Hardware CDC and JTAG |
| Zigbee Mode | Disabled |

N16R8 uses quad flash and octal PSRAM: select QIO flash and OPI PSRAM.
The partition scheme gives two 3,145,728-byte application slots for OTA.
`Huge APP`/`No OTA` layouts cannot be used with this firmware updater.

The **USB** connector supports uploading and Serial Monitor with the settings
above. If its port does not appear for the first upload, hold **BOOT**, tap
**RESET**, then release **BOOT** and select the resulting port. The port can
change when the sketch starts; reselect it and use **115200 baud**.

The **COM** connector (UART on some diagrams) is the USB-to-UART bridge and
also supports uploading. Set **USB CDC On Boot = Disabled** when using COM for
Serial Monitor so `Serial` is routed to that connector.

### Arduino CLI

Run from this sketch directory, with a build directory outside the sources:

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32s3 --board-options FlashSize=16M,FlashMode=qio,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,USBMode=hwcdc,CDCOnBoot=cdc --build-path "$env:TEMP/esp32s3-modular-build" .
```

The reference build is Arduino IDE/CLI. The old LOLIN32 PlatformIO environment
is not included in this S3 folder. `build_opt.h` retains the AsyncTCP core and
stack configuration beside the sketch.

### Generate and validate project assets

Node/npm is required only when changing web assets. Existing generated files
compile without installing JavaScript packages.

```powershell
npm ci
python tools/project_tool.py all
```

This rebuilds the embedded gzip portal and standalone console, checks
JavaScript and HTML bindings, validates source layout, OTA and radio rollback
markers, RGB ownership, and the absence of payload-specific implementations.
Run `python tools/project_tool.py validate` to inspect existing generated files.
See [host indicator tests](tests/README.md) for the hardware-independent checks.

## Default radio behavior

First boot defaults:

```text
Boot profile: WIFI
Wi-Fi role: AP+STA
Fallback AP: enabled
Station credentials: blank
AP SSID: ESP32S3-Controller
AP password: esp32control
Portal URL: http://192.168.4.1/
```

With no station credentials, the setup AP starts and no router association is attempted.

Saved settings in NVS override these defaults. Use `ConfigDefaults` for volatile defaults or `ConfigErase` followed by a reboot to clear saved radio configuration.

## Web console

The hosted portal provides:

```text
HTTP command submission
Event log polling
Live controller state
BLE Web Bluetooth connection
Radio profile controls
Indicator tests
Persistent self-test controls
Application firmware OTA
```

Join the default AP and open:

```text
http://192.168.4.1/
```

A standalone console is also generated at:

```text
web/standalone_console.html
```

Open it locally in Chrome or Edge, set the ESP32 URL, and use it without storing the full standalone page in ESP32 flash.

## BLE UART service

Default advertised name:

```text
ESP32S3-Controller-BLE
```

Nordic-UART-style UUIDs:

```text
Service:  6E400001-B5A3-F393-E0A9-E50E24DCCA9E
RX/write: 6E400002-B5A3-F393-E0A9-E50E24DCCA9E
TX/notify:6E400003-B5A3-F393-E0A9-E50E24DCCA9E
```

Enable TX notifications before expecting command responses or `SendBLE` payloads. The browser writes commands in 20-byte chunks for compatibility with clients that remain at the default ATT MTU.

## Command transports

The same text command grammar is accepted from:

```text
USB serial
Wi-Fi HTTP command endpoint
BLE RX write characteristic
Auxiliary UART when compiled
```

Commands are case-insensitive. A batch may contain one command per line. The dispatcher queues at most eight commands and services them without blocking radio work.

Normal command responses return to their originating transport plus local serial outputs. Cross-transport delivery happens only through explicit `Send*` commands.

## Radio profiles

```text
ModeWiFi
ModeWiFiBLE
ModeWiFiBLEP
ModeBLE
ModeUSB
RadioBoot:WIFI
RadioBoot:WIFI_BLE
RadioBoot:WIFI_BLE_P
RadioBoot:BLE
RadioBoot:USB
```

Profile meanings:

```text
WIFI        Wi-Fi and HTTP only
WIFI_BLE    Adaptive Wi-Fi + BLE coexistence; idle BLE advertising may become dormant
WIFI_BLE_P  Persistent coexistence stress profile; Wi-Fi and BLE stay active
BLE         BLE command transport only
SPP         Unavailable on ESP32-S3; requests are rejected
USB         USB serial only
```

A profile change is saved as a trial transaction and reboots. The firmware commits it as last-known-good only after the new boot stabilizes above the configured heap thresholds. A failed trial rolls back on the next boot.

## Outbound send routing

```text
Send:<payload>
SendBLE:<payload>
SendWiFi:<payload>
SendUSB:<payload>
SendSerial:<payload>
SendUART:<payload>
SendSPP:<payload>
SendStatus
```

Examples:

```text
SendBLE:hello from Wi-Fi
SendWiFi:hello from BLE
SendUSB:hello from the browser
Send:hello to every other active output
```

`SendBLE` requires a connected BLE client with TX notifications enabled. `SendWiFi` requires a running portal and a recently active browser client.

## HTTP and coexistence limits

The firmware intentionally refuses new work before heap exhaustion:

```text
Maximum active HTTP requests: 2
Maximum active reserved HTTP storage: 4352 bytes
Required remaining free heap after reservation: 9000 bytes
Required largest free block: 6000 bytes
HTTP receive timeout: 4 seconds
HTTP ACK timeout: 2500 ms
BLE output recovery floor: 10500 bytes free, 5000-byte largest block
```

BLE output is retained and retried when heap pressure is temporary. It is not discarded merely because a browser request briefly consumes coexistence heap.

## Firmware OTA

OTA uses the inactive application partition and a multipart Wi-Fi upload. The browser validates the selected file before transmission, and firmware validates it again while streaming.

Checks include:

```text
.bin filename
At least 1 KiB
ESP32 image magic byte 0xE9
Declared X-Firmware-Size
Image fits the inactive application slot
Strict in-order chunk offsets
No chunk may exceed declared length
Final received byte count equals declared length
Update.end(false) validates the exact complete image
```

A disconnect during upload aborts `Update` and leaves the current application selected. Reboot begins only after the success response has been queued.

When OTA is started from the BLE console, the page sends `StopAll`, disconnects BLE to release coexistence heap, verifies the HTTP endpoint, then uploads the binary over Wi-Fi. The firmware image is not sent through the 20-byte BLE command channel.

## Self-test

The persistent self-test exercises the base command system, indicators, radio configuration, profile transitions, state persistence, and restore behavior. Hardware-addon tests are absent because the base build has no custom addon.

```text
SelfTestStart
SelfTestStatus
SelfTestResume
SelfTestAbort
SelfTestClear
```

The sweep may reboot several times while testing radio profiles. It stores checkpoints in NVS and restores the original radio, system, and addon state when complete or aborted.

## Command list summary

### Core and status

```text
Ping
Help
Status
ConfigRead
USBStatus
HeapStatus
IndicatorStatus
BLEStatus
StopAll
Reboot
```

### Boot and self-test

```text
ProductionMode
DebugMode
BootModeStatus
SelfTestStart
SelfTestStatus
SelfTestResume
SelfTestAbort
SelfTestClear
```

### Indicators

```text
IndicatorTest
IndicatorConnectionTest
IndicatorActivityTest
```

### Radio profiles and runtime control

```text
RadioStatus
ModeWiFi
ModeWiFiBLE
ModeWiFiBLEP
ModeBLE
ModeUSB
RadioBoot:<profile>
WiFi:ON|OFF
BLE:ON|OFF
ClassicBT:ON|OFF
SPP:ON|OFF
BLEWebHandoff
BLEWebCancel
WebRestart
```

### Wi-Fi configuration

```text
WiFiMode:AP|STA|APSTA
WiFiFallbackAP:ON|OFF
WiFiTxPower:LOW|MAX|<supported dBm>
WiFiLR:ON|OFF
WiFiStaSSID:<ssid>
WiFiStaPassword:<password>
WiFiStaClear
WiFiApSSID:<ssid>
WiFiApPassword:<password>
ConfigApply
ConfigSave
ConfigLoad
ConfigDefaults
ConfigErase
```

LR is OFF by default. ON requires an LR-capable ESP32 peer and prevents ordinary
phone/router Wi-Fi connections. Read the [LR and Bluetooth relay guide](../docs/WIFI_LR.md)
before applying it; use `ConfigApply` to restart Wi-Fi and `ConfigSave` to persist.

### Explicit output routing

```text
Send:<payload>
SendBLE:<payload>
SendWiFi:<payload>
SendUSB:<payload>
SendSerial:<payload>
SendUART:<payload>
SendSPP:<payload>
SendStatus
```

See `COMMANDS.md` for the compact command reference.

## Demo flows

### 1. USB sanity check

At 115200 baud:

```text
Ping
Status
HeapStatus
RadioStatus
Help
```

### 2. Connect to the setup portal

```text
SSID: ESP32S3-Controller
Password: esp32control
URL: http://192.168.4.1/
```

Then send:

```text
Status
IndicatorTest
```

### 3. Enable Wi-Fi + BLE

```text
ModeWiFiBLE
```

The command saves a trial profile and reboots. Reconnect to Wi-Fi, open the console, press **Connect BLE**, and select `ESP32S3-Controller-BLE`.

### 4. Wi-Fi to BLE bridge test

With BLE connected and notifications enabled:

```text
SendBLE:hello from Wi-Fi
```

Expected BLE output:

```text
hello from Wi-Fi
```

### 5. BLE to browser test

Send from BLE:

```text
SendWiFi:hello from BLE
```

The web browser must have polled recently so it is considered an active Wi-Fi destination.

### 6. Configure station Wi-Fi

```text
WiFiStaSSID:My Network
WiFiStaPassword:correct horse battery staple
WiFiMode:APSTA
ConfigSave
ConfigApply
```

Passwords are redacted in logs and never echoed in configuration readback.

### 7. Run indicator tests independently

```text
IndicatorConnectionTest
IndicatorActivityTest
IndicatorTest
```

### 8. Upload firmware

1. Compile with an OTA-capable partition layout.
2. Export the application `.bin`.
3. Open the portal.
4. Expand **Firmware OTA**.
5. Select the application `.bin` and upload.
6. Confirm the serial boot line reports the expected new firmware version.

## Source layout

```text
ESP32S3_Modular_Command_Transport_Controller/
├── ESP32S3_Modular_Command_Transport_Controller.ino  Small Arduino entrypoint
├── README.md                                   Main project guide
├── COMMANDS.md                                 Compact command reference
├── build_opt.h                                 Arduino compile flags
├── src/
│   ├── addons/
│   │   ├── DeviceAddon.h                       Neutral addon interface
│   │   └── README.md                           Addon ownership rules
│   ├── config/AppConfig.h                      Build switches, pins, names, limits
│   ├── core/                                   Queue, router, events, self-test, tasks
│   ├── hardware/StatusIndicators.*             All LED ownership and tests
│   ├── radio/RadioManager.*                    Profiles, Wi-Fi, NVS rollback
│   ├── transports/                             USB, UART, BLE, SPP, explicit sends
│   ├── util/                                   Bounded text and buffer helpers
│   └── web/                                    HTTP server, OTA, generated portal bytes
├── web/
│   ├── index.html                              Hosted portal source
│   ├── app.js                                  Browser transport and OTA logic
│   ├── app.css                                 Portal styling
│   └── standalone_console.html                 Generated local console
├── tools/
│   ├── project_tool.py                         Portal builder and source validator
│   └── install_async_libraries.*               Library install helpers
└── docs/
    ├── ADDON_GUIDE.md
    ├── OTA.md
    └── SOURCE_LAYOUT.md
```

The Arduino sketch stays boring. New hardware logic belongs in a derived `DeviceAddon`, not in the radio, web, or transport layers.

## Adding a user hardware addon

`src/addons/DeviceAddon.h` defines the extension points:

```text
name()
begin()
service()
canAcceptCommand()
handleCommand()
stopAll()
isBusy()
hasActiveOutput()
hasTimedOperationActive()
appendStateJson()
publishHelp()
publishStatus()
queueStartupTests()
self-test snapshot and restore hooks
```

Create a derived class under `src/addons/`, then instantiate that class in the main sketch in place of:

```cpp
DeviceAddon deviceAddon;
```

Keep the implementation nonblocking. Long work should use its own FreeRTOS task, timer, or small state machine. All user commands still enter through `CommandDispatcher`, and output should be published through `EventBus`.

See `docs/ADDON_GUIDE.md` for a pasteable skeleton.

## Troubleshooting

### The browser says `Failed to fetch`

Check serial heap diagnostics first:

```text
HeapStatus
RadioStatus
```

The current build admits small HTTP requests at the tested Wi-Fi/BLE coexistence heap range. Repeated failures can still indicate a stale browser connection, an old firmware build, a wrong IP address, or a non-OTA partition layout during upload.

### BLE commands work but BLE output is missing

Enable TX notifications on:

```text
6E400003-B5A3-F393-E0A9-E50E24DCCA9E
```

Then run:

```text
SendStatus
SendBLE:test
```

A connected client without notifications is reported as `connected-no-notify`.

### RGB LED stays dark or has unexpected colors

Use `IndicatorTest` and check `APP_STATUS_RGB_PIN`: 48 for the supplied board
diagram, 38 for Espressif v1.1. This is an addressable LED, not a single
active-high GPIO LED. Verify the board's RGB power/link jumper if fitted.

### Radio profile change reboots twice or rolls back

That is the transactional safety path. A trial profile must stabilize before being committed. Serial output will show:

```text
[BOOT][RADIO][TRIAL]
[BOOT][RADIO][OK]
```

or:

```text
[BOOT][RADIO][ROLLBACK]
```

### OTA is rejected immediately

Check:

```text
The selected file is an application .bin
The first byte is 0xE9
The partition layout has two application slots
The image fits the inactive slot
APP_ENABLE_HTTP_OTA is enabled
Wi-Fi is part of the active profile
```

### Compile fails on missing AsyncTCP or ESPAsyncWebServer

Run:

```powershell
tools\install_async_libraries.cmd
```

or install the maintained ESP32Async libraries manually.

### A removed motor or DAC command returns unknown command

That is expected in this base build. The custom stepper/DAC implementation and its UI controls are no longer compiled. Use the previous hardware-specific firmware or add a new `DeviceAddon` implementation.

## Notes before deployment

- Add authentication before exposing command or OTA endpoints on an untrusted network.
- Use BLE pairing/bonding or application-level authorization for deployed devices.
- Change the default AP password.
- Confirm the selected partition scheme and OTA rollback strategy.
- Test long-duration Wi-Fi/BLE coexistence with the final board, antenna, power supply, and traffic pattern.
- Keep memory, USB, COM, boot straps, and onboard RGB pins free of addon wiring.
- Keep user addon commands bounded and nonblocking.
