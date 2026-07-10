# ESP32 Stepper + Dual DAC RTOS Controller

Firmware version: `2026-07-10-v5.15.43-led-gpio23-5`

This is the hardware-specific version of the ESP32 modular command and transport controller. It combines a nonblocking FreeRTOS stepper driver, two ESP32 hardware DAC outputs, USB serial, Wi-Fi HTTP, BLE UART, optional Classic Bluetooth SPP, optional auxiliary UART, a browser console, persistent configuration, radio-profile rollback, status indicators, self-test, and validated HTTP OTA.

The stepper and DAC implementation is isolated under `src/addons/stepper_dac/`. The companion transport-only repository uses the same radio, web, OTA, command, and indicator framework without owning the motor or DAC pins.

## Table of contents

- [What this firmware does](#what-this-firmware-does)
- [Hardware assumptions](#hardware-assumptions)
- [Pin map](#pin-map)
- [Build switches](#build-switches)
- [Build and flash](#build-and-flash)
- [Boot behavior](#boot-behavior)
- [Web console](#web-console)
- [BLE UART service](#ble-uart-service)
- [Command transports](#command-transports)
- [Stepper behavior](#stepper-behavior)
- [DAC behavior](#dac-behavior)
- [Radio profiles](#radio-profiles)
- [Outbound send routing](#outbound-send-routing)
- [HTTP and coexistence limits](#http-and-coexistence-limits)
- [Firmware OTA](#firmware-ota)
- [Persistent self-test](#persistent-self-test)
- [Command list summary](#command-list-summary)
- [Demo flows](#demo-flows)
- [Source layout](#source-layout)
- [Important project files](#important-project-files)
- [Troubleshooting](#troubleshooting)
- [Notes before deployment](#notes-before-deployment)

## What this firmware does

- Drives a four-input stepper interface from a dedicated FreeRTOS motor task.
- Provides full-step and half-step sequencing, configurable phase order, RPM ramping, degree moves, step-count moves, motion tests, and explicit coil release.
- Controls both native ESP32 DAC outputs independently.
- Supports DAC target voltage, continuous output, three-second test output, boot-enable state, automatic safety timeout, reference calibration, and NVS persistence.
- Exposes the same command queue through USB serial, Wi-Fi, BLE, optional SPP, and optional auxiliary UART.
- Hosts a compact browser console with stepper shortcuts, hold-to-run DAC controls, live state, event logs, radio controls, self-test, and firmware OTA.
- Routes explicit `Send*` payloads between active transports without creating automatic echo loops.
- Keeps HTTP request count, response storage, command storage, and BLE output bounded for Wi-Fi/BLE coexistence.
- Uses transactional radio-profile changes with health checks and last-known-good rollback.
- Provides optional connection and activity indicators with a separately controlled startup blink test.
- Includes a persistent multi-boot self-test that snapshots and restores radio, system, stepper, and DAC settings.

## Hardware assumptions

Primary target:

```text
Classic ESP32 module or LOLIN32-class board
Arduino ESP32 core
USB-to-UART bridge for programming and USB serial
28BYJ-48-class geared stepper through a ULN2003-style driver
Two native ESP32 DAC outputs on GPIO25 and GPIO26
```

### Stepper wiring

The ESP32 pins drive the logic inputs of a transistor driver board. They do not drive the motor coils directly.

```text
GPIO18 -> driver IN1
GPIO19 -> driver IN2
GPIO27 -> driver IN3
GPIO14 -> driver IN4

External 5 V supply -> stepper driver motor supply
ESP32 GND          -> stepper driver GND
External supply GND -> ESP32 GND
```

Use a supply that can handle the motor current without pulling down the ESP32 rail. The default motion profile is intended for a 28BYJ-48 and stays in the low output-shaft RPM range.

### DAC wiring

```text
GPIO25 -> DAC channel 1 signal
GPIO26 -> DAC channel 2 signal
ESP32 GND -> receiving circuit ground
```

The native ESP32 DAC is 8-bit and produces a voltage between ground and its calibrated reference, nominally around 3.3 V. It cannot directly generate 4 V. Use an external analog switch, level stage, op-amp, or external DAC when the receiving circuit needs a 4 V signal.

Treat the DAC outputs as signal sources for high-impedance inputs. Do not use them as power outputs.

### Default LED wiring

```text
GPIO23 -> 330 ohm to 1 kohm resistor -> connection LED anode
LED cathode -> ESP32 GND

GPIO5 -> 330 ohm to 1 kohm resistor -> activity LED anode
LED cathode -> ESP32 GND
```

Each LED needs its own resistor and its own valid ground return. Some LOLIN32 variants also connect an onboard active-low LED to GPIO5, so the onboard LED can appear inverted relative to an external active-high LED.

GPIO5 is a strapping pin. Avoid a strong external pull-up or pull-down that changes its reset level.

## Pin map

| Function | GPIO | Ownership |
|---|---:|---|
| Stepper IN1 | 18 | `StepperController` |
| Stepper IN2 | 19 | `StepperController` |
| Stepper IN3 | 27 | `StepperController` |
| Stepper IN4 | 14 | `StepperController` |
| DAC channel 1 | 25 | `DacController` |
| DAC channel 2 | 26 | `DacController` |
| Connection indicator | 23 | `StatusIndicators`, optional |
| Activity indicator | 5 | `StatusIndicators`, optional |

All pin definitions live in:

```text
src/config/AppConfig.h
```

All indicator routines live in:

```text
src/hardware/StatusIndicators.h
src/hardware/StatusIndicators.cpp
src/hardware/README.md
```

## Build switches

Edit `src/config/AppConfig.h`:

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

// #define APP_ENABLE_VERBOSE_COEX_HTTP_DIAGNOSTICS
```

Behavior:

```text
APP_ADDON_STEPPER_DAC             Compile the stepper and dual-DAC addon
APP_ENABLE_WIFI                   Compile Wi-Fi, HTTP, DNS setup portal, and OTA transport
APP_ENABLE_BLE                    Compile BLE UART command transport
APP_ENABLE_CLASSIC_BT_SPP         Compile Classic Bluetooth serial transport
APP_ENABLE_HTTP_OTA               Compile browser application OTA when Wi-Fi is enabled
APP_ENABLE_STATUS_INDICATORS      Own GPIO23 and GPIO5 for runtime indication
APP_ENABLE_STATUS_LED_BOOT_TEST   Run the startup LED pulse sequence
APP_ENABLE_MDNS                   Start mDNS when Wi-Fi is active
APP_ENABLE_AUX_UART               Enable the optional hardware UART transport
```

Commenting out `APP_ENABLE_STATUS_INDICATORS` removes all LED `pinMode()` and `digitalWrite()` activity from the build.

Commenting out `APP_ADDON_STEPPER_DAC` selects the neutral `DeviceAddon`. The radio, browser, OTA, transport, command, and self-test framework still compiles, but the motor and DAC commands disappear.

## Build and flash

### Arduino IDE

1. Open `ESP32_Stepper_Dual_DAC_RTOS_Controller.ino`.
2. Select the correct classic ESP32 or LOLIN32 board.
3. Install:
   - `ESP32Async/AsyncTCP` 3.4.10 or newer
   - `ESP32Async/ESPAsyncWebServer` 3.11.2 or newer
4. Choose an OTA-capable partition layout with two application slots.
5. Compile and upload normally.

`Huge APP` has no inactive application slot and cannot be used with this HTTP OTA path.

`build_opt.h` keeps the AsyncTCP core and stack settings beside the sketch:

```text
-DCONFIG_ASYNC_TCP_RUNNING_CORE=1
-DCONFIG_ASYNC_TCP_STACK_SIZE=8192
```

### PlatformIO

From the project directory:

```powershell
pio run -c tools/platformio.ini
pio run -c tools/platformio.ini -t upload
pio device monitor -b 115200
```

### Generate and validate project assets

```powershell
python tools/project_tool.py all
```

This rebuilds the embedded gzip portal and standalone console, checks JavaScript syntax and HTML bindings, validates source layout, checks OTA markers, and audits configured pin ownership.

## Boot behavior

The firmware has persistent production and debug boot policies:

```text
DebugMode       Save debug boot policy for the next boot
ProductionMode  Save production boot policy for the next boot
BootModeStatus  Read the saved and active policy
```

Debug mode queues the configured addon startup checks after the command, indicator, radio, and runtime services are ready:

```text
DAC1 three-second 500 mV test, when RUN_DAC1_BOOT_TEST is true
Stepper 360-degree CW/CCW visual test, when RUN_STEPPER_BOOT_TEST is true
```

Production mode skips the stepper and DAC startup actuation tests. The status indicator behavior remains controlled independently by `APP_ENABLE_STATUS_INDICATORS` and `APP_ENABLE_STATUS_LED_BOOT_TEST`.

Self-test resume boots and radio-profile trial boots also suppress addon startup tests so they do not interfere with restore or radio validation.

## Web console

Join the default access point:

```text
SSID: ESP32-Stepper-WiFi
Password: stepper32
URL: http://192.168.4.1/
```

The hosted portal provides:

```text
HTTP command submission
Live controller and addon state
Event log polling
Stepper motion shortcuts
Hold-to-run DAC 1 and DAC 2 buttons
BLE Web Bluetooth connection
Radio profile and Wi-Fi configuration
Indicator tests
Persistent self-test controls
Application firmware OTA
```

The hold-to-run DAC buttons send `DAC1:ON` or `DAC2:ON` while held and send the matching `OFF` command on release, pointer cancellation, lost pointer capture, window blur, page hide, and tab hiding.

A standalone console is generated at:

```text
web/standalone_console.html
```

Open it locally in Chrome or Edge, set the ESP32 URL, and use it without storing the full standalone page in ESP32 flash.

## BLE UART service

Default advertised name:

```text
ESP32-StepperBLE
```

Nordic-UART-style UUIDs:

```text
Service:   6E400001-B5A3-F393-E0A9-E50E24DCCA9E
RX/write:  6E400002-B5A3-F393-E0A9-E50E24DCCA9E
TX/notify: 6E400003-B5A3-F393-E0A9-E50E24DCCA9E
```

Enable TX notifications before expecting command responses or `SendBLE` payloads. The browser writes commands in 20-byte chunks for compatibility with clients that remain at the default ATT MTU.

## Command transports

The same text command grammar is accepted from:

```text
USB serial
Wi-Fi HTTP command endpoint
BLE RX write characteristic
Classic Bluetooth SPP when compiled
Auxiliary UART when compiled
```

Commands are case-insensitive. A batch may contain one command per line.

Current command limits:

```text
Unified command queue: 8 commands
Private motor queue: 8 commands
Maximum normalized command: 256 bytes
Maximum request ID: 23 bytes
```

Stepper commands execute on the motor task. A normal move stays FIFO and begins after the previous move completes. `Stop`, `CoilsOff`, and `StopAll` act as queue barriers so stale movement commands do not run afterward.

Normal responses return to their originating transport plus local serial outputs. Cross-transport delivery happens only through explicit `Send*` commands.

## Stepper behavior

The default profile is configured for a 28BYJ-48-style geared motor:

```text
Default base steps per revolution: 2038
Default minimum RPM: 1
Default maximum RPM: 20
Default start RPM: 2
Default ramp: 15 RPM per second
Default step mode: full-step, effective 2038 steps per revolution
Half-step mode: effective 4076 steps per revolution
Direction 1: CW
Direction 2: CCW
```

Motion is nonblocking. The motor task schedules coil phases while the network/control task continues to service USB, Wi-Fi, BLE, web requests, and commands.

### Direct moves

Move by raw step count:

```text
RPM:<rpm>,<steps>,<direction>
```

Examples:

```text
RPM:10,2048,1
RPM:5,1024,2
```

Move by output-shaft angle:

```text
DEG:<rpm>,<degrees>,<direction>
```

Examples:

```text
DEG:10,360,1
DEG:10,90,2
```

The firmware clamps the requested RPM to the active configured maximum. Direction must be `1` for CW or `2` for CCW.

### Preset moves and tests

```text
MoveFullCW
MoveFullCCW
MoveHalfCW
MoveHalfCCW
TestFullSpeedRev
TestHalfSpeedRev
TestMinSpeedRev
TestFullSpeedRevCCW
TestHalfSpeedRevCCW
TestMinSpeedRevCCW
DebugSpinCW
DebugSpinCCW
RunStartupTest
FastWiperTest
```

### Motor control and status

```text
Setup
GetMotorStats
Stop
CoilsOff
StopAll
```

`Stop` stops active movement. Coil state afterward follows the current hold-torque setting.

`CoilsOff` stops movement and explicitly drives all four stepper outputs low.

`StopAll` clears the unified command queue, queues an urgent motor coil release, and releases both DAC outputs.

### Motor tuning

Stop the motor before changing tuning values.

```text
SetRevSteps:<base-steps-per-revolution>
SetMinRPM:<rpm>
SetMaxRPM:<rpm>
SetStartRPM:<rpm>
SetRampRPM:<rpm-per-second>
SetMinStepIntervalUs:<microseconds>
StepMode:4
StepMode:8
HoldTorque:1
HoldTorque:0
```

`SetRevSteps` is the base sequence steps per revolution. The effective steps per revolution change with full-step versus half-step mode.

`SetMinStepIntervalUs` places a lower bound on the scheduled interval. It can be used to stop aggressive RPM settings from requesting unrealistically fast phase changes.

### Step order

```text
PrintStepOrder
NextStepOrder
StepOrder:<four unique digits from 1 to 4>
```

Example:

```text
StepOrder:2143
```

Use this when the motor wiring order does not match the default logical phase order. Each digit must appear exactly once.

## DAC behavior

The controller owns the two native ESP32 DAC channels:

```text
DAC1 -> GPIO25
DAC2 -> GPIO26
```

Default state:

```text
Reference calibration: 3300 mV
Target for each channel: 500 mV
Boot output: off
Safety timeout: unlimited, value 0
```

The actual output is quantized to an 8-bit DAC code. `DACRefMV` calibrates the code conversion against the measured full-scale output. It does not create a higher supply rail.

### Channel output

```text
DAC1:ON
DAC1:OFF
DAC2:ON
DAC2:OFF
DACAll:ON
DACAll:OFF
```

`ON` continuously drives the channel at its configured target until an `OFF`, `StopAll`, reboot, or configured safety timeout releases it.

### Target voltage

```text
DAC1:MV:<millivolts>
DAC2:MV:<millivolts>
DACRefMV:<2500-to-3600>
```

Examples:

```text
DACRefMV:3290
DAC1:MV:500
DAC2:MV:1200
```

Changing `MV` updates the target. An already enabled channel updates immediately. A disabled channel remains disabled until turned on.

The target must be between 0 and the configured DAC reference. The ESP32 DAC cannot directly output 4 V.

### Timed test output

```text
DAC1:TEST3S
DAC2:TEST3S
DACTest3S
```

The test temporarily drives 500 mV for 3000 ms, then releases the output. It does not overwrite the channel's configured target.

### Automatic safety timeout

```text
DAC1:TIMEOUTMS:<milliseconds>
DAC2:TIMEOUTMS:<milliseconds>
DAC1:TIMEOUTSEC:<seconds>
DAC2:TIMEOUTSEC:<seconds>
```

Examples:

```text
DAC1:TIMEOUTMS:5000
DAC2:TIMEOUTSEC:30
```

A value of `0` means unlimited. Setting a timeout while a channel is already enabled restarts its timeout from that moment.

### Boot state and persistence

```text
DAC1:BOOT:ON
DAC1:BOOT:OFF
DAC2:BOOT:ON
DAC2:BOOT:OFF
DACSave
DACLoad
DACDefaults
DACErase
DACStatus
```

`BOOT` changes the volatile boot-enabled setting. Use `DACSave` to persist DAC reference, channel target, boot state, and timeout to NVS.

`DACLoad` reloads saved DAC settings and applies each saved boot-enabled state immediately.

`DACDefaults` restores volatile defaults and releases both outputs.

`DACErase` clears the saved DAC namespace while leaving current volatile settings unchanged.

## Radio profiles

```text
ModeWiFi
ModeWiFiBLE
ModeWiFiBLEP
ModeBLE
ModeBTSerial
ModeUSB
RadioBoot:WIFI
RadioBoot:WIFI_BLE
RadioBoot:WIFI_BLE_P
RadioBoot:BLE
RadioBoot:SPP
RadioBoot:USB
```

Profile meanings:

```text
WIFI        Wi-Fi and HTTP only
WIFI_BLE    Adaptive Wi-Fi + BLE coexistence, idle BLE advertising may become dormant
WIFI_BLE_P  Persistent coexistence stress profile, Wi-Fi and BLE stay active
BLE         BLE command transport only
SPP         Classic Bluetooth serial only
USB         USB serial only
```

A profile change is saved as a trial transaction and reboots. The firmware commits it as last-known-good only after the new boot stabilizes above the configured heap thresholds. A failed trial rolls back on the next boot.

Default first-boot radio configuration:

```text
Boot profile: WIFI
Wi-Fi role: AP+STA
Fallback AP: enabled
Station credentials: blank
AP SSID: ESP32-Stepper-WiFi
AP password: stepper32
Portal URL: http://192.168.4.1/
```

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

Use an OTA-capable partition scheme with two application slots. Do not use `Huge APP`.

## Persistent self-test

```text
SelfTestStart
SelfTestStatus
SelfTestResume
SelfTestAbort
SelfTestClear
```

The self-test snapshots system, radio, stepper, and DAC state before running. It exercises core commands, indicators, motor tuning, short motor moves, step-order changes, DAC output and timeout behavior, DAC persistence, Wi-Fi configuration parsing, radio profiles, reboot persistence, and restore behavior.

The sweep physically moves the motor and energizes the DAC outputs. Disconnect sensitive external loads before running it.

The sweep may reboot several times while testing radio profiles. It stores checkpoints in NVS and restores the original radio, system, stepper, and DAC state when complete or aborted.

## Command list summary

Commands are case-insensitive.

### Core and status

```text
Ping
Help
Status
ConfigRead
BootModeStatus
USBStatus
HeapStatus
IndicatorStatus
BLEStatus
StopAll
Reboot
```

### Boot policy and self-test

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
Indicator16Test
Indicator17Test
```

The numbered aliases operate on the current GPIO23 and GPIO5 configuration.

### Stepper status, stop, and tests

```text
Setup
GetMotorStats
Stop
CoilsOff
RunStartupTest
FastWiperTest
DebugSpinCW
DebugSpinCCW
MoveFullCW
MoveFullCCW
MoveHalfCW
MoveHalfCCW
TestFullSpeedRev
TestHalfSpeedRev
TestMinSpeedRev
TestFullSpeedRevCCW
TestHalfSpeedRevCCW
TestMinSpeedRevCCW
```

### Stepper moves and tuning

```text
RPM:<rpm>,<steps>,<direction>
DEG:<rpm>,<degrees>,<direction>
SetRevSteps:<steps>
SetMinRPM:<rpm>
SetMaxRPM:<rpm>
SetStartRPM:<rpm>
SetRampRPM:<rpm-per-second>
SetMinStepIntervalUs:<microseconds>
StepMode:4
StepMode:8
StepOrder:<four unique digits>
NextStepOrder
PrintStepOrder
HoldTorque:1
HoldTorque:0
```

### DAC output and settings

```text
DACStatus
DACRefMV:<2500-to-3600>
DAC1:MV:<millivolts>
DAC2:MV:<millivolts>
DAC1:ON
DAC1:OFF
DAC2:ON
DAC2:OFF
DACAll:ON
DACAll:OFF
DAC1:TEST3S
DAC2:TEST3S
DACTest3S
DAC1:BOOT:ON
DAC1:BOOT:OFF
DAC2:BOOT:ON
DAC2:BOOT:OFF
DAC1:TIMEOUTMS:<milliseconds>
DAC2:TIMEOUTMS:<milliseconds>
DAC1:TIMEOUTSEC:<seconds>
DAC2:TIMEOUTSEC:<seconds>
DACSave
DACLoad
DACDefaults
DACErase
```

### Radio profiles and runtime control

```text
RadioStatus
ModeWiFi
ModeWiFiBLE
ModeWiFiBLEP
ModeBLE
ModeBTSerial
ModeUSB
RadioBoot:<profile>
WiFi:ON
WiFi:OFF
BLE:ON
BLE:OFF
ClassicBT:ON
ClassicBT:OFF
SPP:ON
SPP:OFF
BLEWebHandoff
BLEWebCancel
WebRestart
BleAdvertise
```

### Wi-Fi configuration

```text
WiFiMode:AP
WiFiMode:STA
WiFiMode:APSTA
WiFiFallbackAP:ON
WiFiFallbackAP:OFF
WiFiTxPower:LOW
WiFiTxPower:MAX
WiFiTxPower:<supported dBm>
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

Supported explicit Wi-Fi TX power values:

```text
19.5, 19, 18.5, 17, 15, 13, 11, 8.5, 7, 5, 2, -1 dBm
```

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

Open the serial monitor at 115200 baud:

```text
Ping
Status
HeapStatus
RadioStatus
GetMotorStats
DACStatus
```

### 2. Connect to the portal

```text
SSID: ESP32-Stepper-WiFi
Password: stepper32
URL: http://192.168.4.1/
```

Then send:

```text
Status
IndicatorTest
```

### 3. Confirm stepper wiring slowly

```text
CoilsOff
SetMaxRPM:10
SetStartRPM:2
SetRampRPM:10
DEG:5,90,1
DEG:5,90,2
CoilsOff
```

If it buzzes without rotating, test the common phase orders:

```text
PrintStepOrder
NextStepOrder
DEG:5,90,1
```

Repeat `NextStepOrder` until rotation is clean, then record the working order with `PrintStepOrder`.

### 4. Run one complete revolution

```text
DEG:10,360,1
DEG:10,360,2
CoilsOff
```

### 5. Set and test DAC channel 1

```text
DACRefMV:3300
DAC1:MV:500
DAC1:TEST3S
DACStatus
```

### 6. Configure a fail-safe DAC output

```text
DAC1:MV:1200
DAC1:TIMEOUTSEC:10
DAC1:BOOT:OFF
DACSave
DAC1:ON
```

The output releases automatically after ten seconds.

### 7. Hold DAC from the browser

Open the portal and hold **Hold DAC 1**. The page sends:

```text
DAC1:ON
```

Release the button or move away from the page, and it sends:

```text
DAC1:OFF
```

### 8. Emergency release

```text
StopAll
```

This clears queued general commands, prioritizes motor coil release, and releases both DAC outputs.

### 9. Enable Wi-Fi + BLE

```text
ModeWiFiBLE
```

The command saves a trial profile and reboots. Reconnect to Wi-Fi, open the console, press **Connect BLE**, and select `ESP32-StepperBLE`.

### 10. Wi-Fi to BLE bridge test

With BLE connected and notifications enabled:

```text
SendBLE:hello from Wi-Fi
```

### 11. BLE to browser test

Send from BLE:

```text
SendWiFi:hello from BLE
```

The browser must have polled recently so it is considered an active Wi-Fi destination.

### 12. Configure station Wi-Fi

```text
WiFiStaSSID:My Network
WiFiStaPassword:correct horse battery staple
WiFiMode:APSTA
ConfigSave
ConfigApply
```

Passwords are redacted in logs and are not echoed in configuration readback.

### 13. Upload firmware

1. Compile with an OTA-capable partition layout.
2. Export the application `.bin`.
3. Open the portal.
4. Expand **Firmware OTA**.
5. Select the application `.bin` and upload.
6. Confirm the serial boot line reports the expected firmware version.

## Source layout

```text
ESP32_Stepper_Dual_DAC_RTOS_Controller/
├── ESP32_Stepper_Dual_DAC_RTOS_Controller.ino  Small Arduino entrypoint
├── README.md                                   Main project guide
├── COMMANDS.md                                 Compact command reference
├── build_opt.h                                 Arduino compile flags
├── src/
│   ├── addons/
│   │   ├── DeviceAddon.h                       Addon interface
│   │   └── stepper_dac/
│   │       ├── StepperDacAddon.*               Addon integration and state
│   │       ├── StepperController.*             Motor task, queue, motion, phases
│   │       ├── DacController.*                 DAC state, timers, NVS, output
│   │       └── README.md                       Addon ownership notes
│   ├── config/AppConfig.h                      Build switches, pins, names, limits
│   ├── core/                                   Queue, router, events, self-test, tasks
│   ├── hardware/StatusIndicators.*             All LED ownership and tests
│   ├── radio/RadioManager.*                    Profiles, Wi-Fi, NVS rollback
│   ├── transports/                             USB, UART, BLE, SPP, explicit sends
│   ├── util/                                   Bounded text and buffer helpers
│   └── web/                                    HTTP server, OTA, generated portal bytes
├── web/
│   ├── index.html                              Hosted portal source
│   ├── app.js                                  Browser transport, controls, OTA
│   ├── app.css                                 Portal styling
│   └── standalone_console.html                 Generated local console
├── tools/
│   ├── project_tool.py                         Portal builder and source validator
│   ├── platformio.ini                          Optional PlatformIO build
│   └── install_async_libraries.*               Library install helpers
└── docs/
    ├── ADDON_ARCHITECTURE.md
    ├── FLASH_SIZE_AND_OTA.md
    ├── PROJECT_GUIDE.md
    ├── SOURCE_LAYOUT.md
    └── V5_15_*.md                              Version notes
```

The Arduino sketch stays small. Motor behavior belongs in `StepperController`, DAC behavior belongs in `DacController`, and shared radio/web/transport code does not directly touch addon pins.

## Important project files

```text
src/config/AppConfig.h                         Build switches, pins, defaults, limits
src/addons/stepper_dac/StepperController.cpp  Stepper command parser and motor task
src/addons/stepper_dac/DacController.cpp      DAC command parser and safety timers
src/addons/stepper_dac/StepperDacAddon.cpp    Addon integration, help, state, self-test hooks
src/hardware/StatusIndicators.cpp             LED state and test routines
src/radio/RadioManager.cpp                    Radio profiles and saved configuration
src/web/WebPortal.cpp                         HTTP API and OTA receiver
web/index.html                                 Portal layout
web/app.js                                    Browser command, BLE, and OTA logic
COMMANDS.md                                   Compact command reference
```

## Troubleshooting

### The stepper buzzes but does not rotate

Check:

```text
Motor is powered from the correct external supply
Driver and ESP32 share ground
GPIO18, GPIO19, GPIO27, and GPIO14 reach IN1 through IN4
Coil connector is seated correctly
RPM is realistic for the 28BYJ-48 output shaft
```

Then test phase order:

```text
CoilsOff
SetMaxRPM:10
NextStepOrder
DEG:5,90,1
```

Repeat until rotation is clean.

### The stepper gets hot while idle

Disable hold torque and release the coils:

```text
HoldTorque:0
CoilsOff
```

The ULN2003 indicator LEDs should turn off after coil release.

### A move is too fast or stalls

```text
SetMaxRPM:10
SetStartRPM:1
SetRampRPM:5
SetMinStepIntervalUs:1000
```

The 28BYJ-48 is a geared low-speed motor. Large output-shaft RPM values cause buzzing and missed steps.

### The DAC voltage is lower than requested

Measure the actual DAC full-scale voltage with a meter and set:

```text
DACRefMV:<measured-full-scale-mV>
```

Then reapply the channel target. Also verify that the receiving load is high impedance.

### The circuit needs 4 V

The native ESP32 DAC cannot produce 4 V. Use the ESP32 GPIO as the control input for an external analog switch, or use an external DAC or amplifier supplied from a rail above 4 V.

### A DAC output stays active too long

Set and save a safety timeout:

```text
DAC1:TIMEOUTSEC:10
DAC1:BOOT:OFF
DACSave
```

Use `StopAll` for an immediate release of both DAC outputs and the motor coils.

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

### One LED only works when another ground is attached

The first LED does not have an independent return path. Verify continuity from each LED cathode to ESP32 GND with the other LED unplugged. Breadboard power rails are often split in the middle.

### The onboard GPIO5 LED blinks opposite the external LED

The external LED configuration is active-high. Some LOLIN32 onboard LEDs are active-low on GPIO5. This inversion is expected.

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

## Notes before deployment

- Add authentication before exposing command or OTA endpoints on an untrusted network.
- Use BLE pairing, bonding, or application-level authorization for deployed devices.
- Change the default AP password.
- Confirm the selected partition scheme and OTA rollback strategy.
- Test long-duration Wi-Fi/BLE coexistence with the final board, antenna, power supply, motor load, and command traffic.
- Use a separate motor supply with a common ground and adequate bulk decoupling.
- Confirm motor direction and phase order before connecting a mechanism.
- Configure DAC safety timeouts for outputs that can affect external hardware.
- Remember that GPIO25 and GPIO26 remain limited to the ESP32 DAC rail.
- Review GPIO5 boot strapping with the final LED circuit.
