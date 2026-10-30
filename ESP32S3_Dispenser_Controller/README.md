# ESP32-S3 Gel Dispenser Controller

ESP32 firmware for a drone-mounted caulk-gel dispenser. The normal build uses
GPIO4 on ESP32-S3 N16R8 to control an external analog
switch connected to the applicator trigger.
It provides a simple user-facing Arduino sketch while keeping the more
advanced FreeRTOS, Wi-Fi, BLE, command, routine, logging, and OTA systems in
separate modules.

This is a separate sketch for ESP32-S3-DevKitC-1-N16R8 using the Arduino
**ESP32S3 Dev Module** board profile. The original LOLIN32 firmware remains in
the neighboring `ESP32_Dispenser_Controller` folder. ESP32-S3 has no internal
DAC or Classic Bluetooth SPP; its Wi-Fi and BLE transports remain available.
The BLE transport supports the Arduino core's NimBLE host, including its
automatic notification subscription descriptor. No separate NimBLE library
installation is needed with the checked core.

## Start here

Ordinary Wi-Fi remains the default; long-range radio modes are optional.

1. Read [docs/USER_GUIDE.md](docs/USER_GUIDE.md).
2. Confirm the external switch is held inactive while the ESP32 is reset.
3. Open `ESP32S3_Dispenser_Controller.ino` in Arduino IDE.
4. Open the neighboring `QuickConfig.h` tab and confirm the pinout.
5. Install the maintained ESP32Async `AsyncTCP` and `ESPAsyncWebServer` libraries.
6. Select the exact board and partition settings described below.
7. Upload with the dispenser disconnected or unloaded for the first test.
8. Open Serial Monitor at 115200 baud and send `Status`.

The browser console is available from the default access point:

```text
SSID: Drone-Gel-Controller
Password: password
URL: http://192.168.4.1/
```

### Exact Arduino board settings

- Board package: Espressif `esp32` core 3.3.10 or a compatible release.

#### ESP32-S3-DevKitC-1-N16R8

This folder always targets ESP32-S3. Leave `DRONE_CFG_HARDWARE_PROFILE` at `1`
in `QuickConfig.h`, and select these Arduino Tools settings:

Use the connector labeled **USB** for uploading and Serial Monitor, and select
the port that appears when that cable is connected. These are the complete
Arduino Tools-menu settings for this N16R8 board:

| Tools setting | Value |
|---|---|
| Board | **ESP32S3 Dev Module** (`esp32:esp32:esp32s3`) |
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

The four settings shown in bold after Board differ from the default 4 MB
configuration. N16R8 has 16 MB quad flash plus 8 MB octal PSRAM: keep QIO flash
and OPI PSRAM. The selected partition scheme provides two 3 MiB application
slots for OTA.

If the native USB port does not appear for the first upload, hold **BOOT**, tap
**RESET**, then release **BOOT**, select the resulting port, and upload. The
port may change after the first sketch starts; reselect it for Serial Monitor
at **115200 baud**.

The connector labeled **COM** (UART on some diagrams) is the USB-to-UART
bridge and can also upload firmware. When using COM for Serial Monitor, change
**USB CDC On Boot to Disabled** so `Serial` uses that connector instead.

```text
--fqbn esp32:esp32:esp32s3 --board-options FlashSize=16M,FlashMode=qio,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,USBMode=hwcdc,CDCOnBoot=cdc
```

| Function | GPIO |
|---|---:|
| External dispenser-switch control | 4 |
| Onboard addressable RGB LED | 48 |
| Optional interlock | disabled (`-1`) |

Use the **GPIO numbers printed on the board**, not header positions. Move the
old dispenser control wire from GPIO26 to GPIO4 when changing boards. GPIO26
belongs to the S3 memory interface; GPIO23 does not exist on the S3. GPIO35–37
are reserved for the N16R8 octal PSRAM even though they appear on the headers.
The payload/interlock allowlist also excludes boot straps, USB GPIO19/20,
UART0 GPIO43/44, and both RGB revision pins. Runtime pin commands, saved
profiles, and the portal all use the selected board's allowlist.

The supplied board diagram labels **RGB_LED on GPIO48**. Leave
`DRONE_CFG_STATUS_RGB_PIN 48`; Espressif v1.1 boards instead require `38`.
`DRONE_CFG_STATUS_RGB_BRIGHTNESS` sets brightness from 1 to 255 (default 24).
Green is solid when connected and blinks while seeking a connection; blue
flashes for commands; amber remains on during an operation. Activity takes
priority over connection indication. `IndicatorTest` flashes green twice and
blue three times; `IndicatorConnectionTest` and `IndicatorActivityTest` test
each color separately. No extra LED library is required.

S3 supports Wi-Fi and BLE. Classic Bluetooth SPP and the old internal-DAC bench
profile are rejected at compile time. Saved settings still override compiled
defaults when their pins are valid for the selected board; incompatible saved
pins are rejected before GPIO setup. After migration, inspect `DispenserStatus`
and confirm GPIO4 before arming. The compatibility command `GPIO26:ON` still
means a timed pulse on the **configured** dispenser output; it does not select
physical GPIO26.

Hardware references: [Espressif DevKitC-1 pinout and memory reservations](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.0.html),
[RGB revision difference](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.1.html).

## Normal operating sequence

```text
DispenserStatus
Arm
Dispense:250
Disarm
```

`Arm` does not activate the output. It opens the active profile's temporary arming window during
which a bounded dispense pulse may be requested. Every reset, `StopAll`, routine
completion, fault, or arm timeout leaves the output inactive and disarmed.
The requested pulse must fit entirely inside the remaining arm window; otherwise
the controller asks for a fresh `Arm` instead of starting a truncated pulse.

`GPIO26:ON` remains as a compatibility alias, but it is converted to the default
bounded pulse instead of producing an indefinite HIGH state.

## Hardware connection

```text
ESP32-S3 GPIO4 -> external analog-switch control input
ESP32 GND     -> control-circuit ground
```

The output is only a 3.3 V logic control signal. It must not receive the switched
caulk-gun signal and must not power the actuator.

The external control circuit should provide a hardware inactive bias so the
switch remains off before firmware starts, during reset, and if the ESP32 loses
power. A separate physical or flight-controller interlock can be configured
with `DRONE_CFG_INTERLOCK_PIN` in `QuickConfig.h`.

## Safety state model

```text
DISARMED -> ARMED -> DISPENSING
    ^          |          |
    +----------+----------+
       timeout / stop / fault
```

- The dispenser always starts `DISARMED`.
- The default dispenser initializes its safe output immediately after USB serial,
  before indicator animations, self-test recovery, or radio startup.
- Arming is never written to NVS.
- Pulse, initial delay, and gap use unsigned 32-bit milliseconds (up to
  4294967295 ms, about 49.7 days per step). Repeats accept 1–4294967295.
  There is no total routine runtime or WAIT_IDLE timeout cap.
- Maximum pulse and automatic arming expiry default to disabled (0).
- Saved dispenser settings and named profiles keep their own maximum pulse after
  a firmware update. To remove limits in the active saved setup, use the console's
  **Remove saved time limits**, or disarm and send `DispenserArmTimeout:0` and
  `DispenserMaxPulse:0`, then
  save with `DispenserSave` or `PayloadProfileSave:<active-name>` as appropriate.
- A new dispense request is rejected while a pulse is already active, so
  repeated commands cannot silently extend one activation.
- Closing the optional interlock immediately stops and faults the dispenser.
- Payload profiles never store or restore an armed, active, or faulted state.
- Selecting, saving, or deleting a payload profile requires the dispenser to be
  disarmed and is blocked while a routine owns the payload.
- `StopAll` cancels routines, stops hardware, and disarms the dispenser.
- Emergency stops enter a queue barrier that discards older pending commands;
  hardware routing remains serialized on the control task.
- A saved routine cannot contain `Arm`, configuration, radio, reboot, or erase commands.
- While a routine is active, external commands cannot arm or start an
  overlapping dispense pulse; status and stop commands remain available.
- Routine completion and routine failure both make the addon safe.
- Valid OTA uploads and managed radio reboots force a safe hardware state before
  flash writing or restart.

These are software safeguards. They do not replace an external inactive bias,
an aircraft-level inhibit, suitable power isolation, or ground testing.

## Saved payload profiles

The normal aircraft build provides four fixed, checksummed payload-profile slots
in ESP32 NVS. Each profile stores a GPIO, output polarity, default pulse,
maximum pulse, and arm timeout. The optional interlock is global and cannot be
weakened or disabled by a runtime profile.

Create and select a fine-output setup:

```text
Disarm
DispenserPin:26
DispenserActiveHigh:ON
DispenserDefaultPulse:150
DispenserMaxPulse:750
DispenserArmTimeout:30000
PayloadProfileSave:fine
```

Create another setup the same way, then switch between them while disarmed:

```text
PayloadProfileList
PayloadProfileUse:fine
PayloadProfileShow:fine
```

`PayloadProfileSave` snapshots the current settings and selects that profile for
future boots. Pulse and arming limits are optional; zero disables them.
Profiles do not contain `Arm`, output state, or routines.

## Saved timing routines

Routines run without blocking `loop()` or the network task. They are fixed-size,
checksummed records in ESP32 NVS and are only written by `RoutineSave`.

In the interface, use **Add output** to configure up to eight analog-switch
GPIOs with their own dispense time and delay after each pulse. Disarm and **Save
output pins**, then **Build + save** the named routine. Outputs run one at a time
in row order; each repeat cycles through all rows. **Use default single output**
returns to one GPIO. Stop turns off every output. See the
[multiple-output instructions](docs/COORDINATE_ROUTINES.md#multiple-analog-switch-outputs).

Create a six-dot pattern:

```text
RoutineCreate:dots
RoutineAdd:dots:DISPENSE:200
RoutineAdd:dots:WAIT_IDLE
RoutineAdd:dots:WAIT:800
RoutineRepeat:dots:6
RoutineSave:dots
```

The console's **Initial delay ms** waits once before the first pulse. Check
**Repeat until stopped** to cycle the pulse and gap continuously after that
delay. Both settings are saved with the named routine. Its Run button starts
the saved settings; **Stop routine** stops the output and cancels further cycles.
The command equivalent is `RoutineRepeat:dots:FOREVER` followed by `RoutineSave:dots`.

Run it:

```text
Arm
RoutineRun:dots
```

Stop it immediately:

```text
StopAll
```

`COMMAND:` steps allow the same routine engine to drive optional stepper or DAC
hardware in the advanced profile. Only a small hardware-action allowlist is
accepted; administrative commands cannot be stored in routines.

## User settings

`QuickConfig.h` sits directly beside the `.ino` and appears as another Arduino
IDE tab. It contains the settings users are expected to change:

```cpp
#define DRONE_CFG_HARDWARE_PROFILE 1
#define DRONE_CFG_DISPENSER_PIN 26
#define DRONE_CFG_DISPENSER_ACTIVE_HIGH 1
#define DRONE_CFG_DISPENSER_DEFAULT_PULSE_MS 250UL
#define DRONE_CFG_DISPENSER_MAX_PULSE_MS 0UL
#define DRONE_CFG_DISPENSER_ARM_TIMEOUT_MS 0UL
#define DRONE_CFG_INTERLOCK_PIN -1
```

See [saved routines and drone position](docs/COORDINATE_ROUTINES.md) for the
initial-delay controls, Wi-Fi client setup, ordered coordinates, MAVLink input over Wi-Fi or Bluetooth,
and the position API an XAG integration provider can connect to.

The same tab groups the status LED pins, local access-point identity, and the
advanced stepper/DAC pinout. Set `DRONE_USE_QUICK_CONFIG` to `0` to ignore all
of those overrides and use the safe fallbacks in `src/config/AppConfig.h`.
Incorrect timing ranges, invalid output pins, and pin collisions produce a
clear compiler error. Feature switches, queue sizes, and expert-only limits stay
in `AppConfig.h` so the beginner tab remains short.

Quick settings are compiled defaults. A standalone configuration saved with
`DispenserSave`, or a named selection made with `PayloadProfileSave`/`Use`, wins
at boot. Send `DispenserErase` and reboot when you want the compiled defaults to
take over; named records remain available until individually deleted or erased
with `PayloadProfileEraseAll`.

Runtime pin changes are deliberately conservative:

```text
Disarm
DispenserPin:32
DispenserActiveHigh:ON
DispenserDefaultPulse:200
PayloadProfileSave:alternate
```

The controller rejects flash pins, input-only pins, common boot-strapping pins,
the configured interlock, and pins already owned by status indicators or the
optional auxiliary UART. Rewire and test the new pin with the applicator unloaded.

## Build profiles

### Default aircraft profile

```cpp
#define DRONE_CFG_HARDWARE_PROFILE 1
```

Owns the configured dispenser output. Stepper and DAC implementation files are
compiled to empty translation units and consume no runtime tasks or GPIOs.

### Legacy bench hardware

The original stepper/internal-DAC profile belongs to the LOLIN32 firmware.
Keep hardware profile `1` in this S3 sketch; profile `2` is rejected.

## Source map

```text
ESP32S3_Dispenser_Controller.ino           Small Arduino entrypoint
QuickConfig.h                           Beginner hardware and network defaults
src/config/AppConfig.h                  Advanced limits and build flags
src/addons/dispenser/DispenserAddon.*  Arming, pulse, interlock, safe GPIO config
src/core/RoutineEngine.*                Saved nonblocking routines
src/core/CommandDispatcher.*            Bounded shared command queue
src/core/CommandRouter.*                Global command routing and state JSON
src/radio/RadioManager.*                Wi-Fi/BLE profiles and rollback
src/web/WebPortal.*                     HTTP API, portal, and OTA
web/                                    Editable browser UI sources
src/addons/stepper_dac/                 Optional advanced hardware profile
COMMANDS.md                             Complete command reference
docs/USER_GUIDE.md                      Guided first setup
```

The `.ino` intentionally stays small. Users can follow normal Arduino
`setup()` and `loop()` while each sophisticated subsystem remains independently
testable and replaceable.

## Web console

The hosted page provides:

- prominent armed/disarmed/dispensing state;
- active named payload-profile status and profile command examples;
- fixed and custom bounded pulse buttons;
- a simple dot-pattern routine builder;
- routine, queue, radio, heap, and self-test status;
- an advanced text console using the same commands as USB and BLE;
- firmware OTA over Wi-Fi.

The editable files are `web/index.html`, `web/app.css`, and `web/app.js`. Their
gzip representation served by the ESP32 is stored in `src/web/WebAssets.h`.
`web/standalone_console.html` is a generated, self-contained copy for opening
from a laptop and targeting a controller URL; it is not embedded in firmware.

After changing any frontend source, rebuild both generated files with Node.js:

```text
npm ci
node web/build_web_assets.mjs
```

The script uses the pinned Terser build dependency to compact the embedded
JavaScript and produces deterministic gzip bytes. The standalone console keeps
readable JavaScript. Arduino builds use the checked-in generated assets; npm is
needed only when regenerating them. To check for stale generated files, run:

```text
node web/build_web_assets.mjs --check
```

## Recommended ground-test checklist

1. Disconnect or unload the caulk gun.
2. Power-cycle and verify the switch input remains inactive throughout boot.
3. Confirm `DispenserStatus` reports `DISARMED`.
4. Confirm `Dispense:250` is rejected while disarmed.
5. Arm and measure a 250 ms control pulse.
6. Confirm `DispenseStop`, `Disarm`, and `StopAll` immediately make the pin inactive.
7. Let the arm window expire and confirm dispensing is rejected.
8. Run a short saved routine and interrupt it with `StopAll`.
9. If an interlock is fitted, open it during a pulse and verify a fault shutdown.
10. Repeat with final aircraft power, wiring, radio traffic, and payload hardware.

## Notes

- The project assumes a classic ESP32 because the optional DAC profile uses its
  native GPIO25 DAC implementation.
- GPIO5 is used by the optional activity indicator and is a boot-strapping pin;
  review the final LED circuit.
- Wi-Fi and BLE controls are intended for a trusted local test network in the
  current phase.
- Use `StopAll` as the universal software-safe command. Maintain an independent
  hardware inhibit for actual flight operations.
