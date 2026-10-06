# Drone Gel Dispenser command reference

Commands are case-insensitive and work over USB serial, Wi-Fi HTTP, BLE UART,
optional Classic Bluetooth SPP, and optional auxiliary UART. Send one command
per line. HTTP may submit an atomic batch of up to eight lines.

## Dispenser

```text
Arm
Disarm
Dispense:<milliseconds>
DispenseStop
DispenserStatus
PayloadStatus
StopAll
```

Rules:

- `Arm` opens a temporary arming window but does not activate the output.
- `Dispense` accepts 1 through the configured maximum pulse duration.
- The complete pulse must fit in the remaining arm window. Send `Arm` again if
  there is not enough time left.
- `Dispense` is rejected while another pulse is active; it cannot extend an
  activation by repeatedly resetting the stop time.
- `DispenseStop` ends the pulse, closes the arm window, and cancels any active
  routine or coordinate sequence. It cannot allow a later repeat to restart output.
- `Disarm` ends the pulse and closes the arm window.
- `StopAll` ends the pulse, disarms, cancels the active routine, and stops any
  optional advanced hardware.

Compatibility aliases:

```text
GPIO26:ON      converted to the default bounded pulse; still requires Arm
GPIO26:OFF     same as DispenseStop
DispenserArm
DispenserDisarm
DispenserOff
```

There is no normal indefinite-on dispenser command.
Maximum pulse and arming expiry default to disabled (0). Saved settings may
impose optional limits; `DispenserMaxPulse:0` and `DispenserArmTimeout:0` disable
them. Pulse/delay/gap values use 32-bit milliseconds, up to 4294967295 ms.
Repeat counts accept 1–4294967295, with no total routine runtime cap.
Send standalone stop/disarm commands
separately from subsequent settings: a stop clears queued commands.

## Dispenser configuration

```text
DispenserPin:<gpio>
DispenserActiveHigh:ON
DispenserActiveHigh:OFF
DispenserDefaultPulse:<milliseconds>
DispenserMaxPulse:<milliseconds>
DispenserArmTimeout:<milliseconds>
DispenserSave
DispenserDefaults
DispenserErase
```

Configuration changes require the dispenser to be disarmed. Changes are
volatile until `DispenserSave` or `PayloadProfileSave`. A profile maximum pulse
or arm timeout may tighten, but cannot exceed, the ceilings compiled from
`QuickConfig.h`. Output activation, arming state, and faults are never saved.

`DispenserSave` stores one standalone configuration and stops selecting a named
profile at boot. `DispenserDefaults` restores the compiled values in RAM only.
`DispenserErase` erases the standalone configuration and active-profile
selection while retaining the named profile library.

## Saved payload profiles

```text
PayloadProfileList
PayloadProfileShow:<name>
PayloadProfileSave:<name>
PayloadProfileUse:<name>
PayloadProfileDelete:<name>
PayloadProfileEraseAll
```

Four fixed, checksummed NVS slots are available. A profile stores the dispenser
GPIO, polarity, default pulse, maximum pulse, and arm timeout. The external
interlock remains a global compiled safety input and cannot be disabled by a
profile. Names use 1-15 letters, digits, hyphens, or underscores.

`PayloadProfileSave` snapshots the current volatile settings, writes or updates
the named slot, and selects it for future boots. `PayloadProfileUse` safely
applies and persists an existing profile. Both require the dispenser to be
disarmed, and profile-changing commands are rejected while a routine is active.
`PayloadProfileEraseAll` removes only the four named records and their active
selection; a separately saved standalone configuration is retained.

Example:

```text
Disarm
DispenserPin:26
DispenserActiveHigh:ON
DispenserDefaultPulse:150
DispenserMaxPulse:750
DispenserArmTimeout:30000
PayloadProfileSave:fine
PayloadProfileList
```

## Saved routines

```text
RoutineCreate:<name>
RoutineAdd:<name>:DISPENSE:<milliseconds>
RoutineAdd:<name>:START_WAIT:<milliseconds>
RoutineAdd:<name>:WAIT:<milliseconds>
RoutineAdd:<name>:WAIT_IDLE
RoutineAdd:<name>:COMMAND:<allowed-hardware-command>
RoutineRepeat:<name>:<count>
RoutineSave:<name>
RoutineRun:<name>
RoutineStop
RoutineStatus
RoutineList
RoutineShow:<name>
RoutineErase:<name>
```

Limits are configured in `AppConfig.h`. The normal build provides four routine
slots and ten steps per routine. Repeat counts accept 1–4294967295. Routine names contain up to 15
letters, digits, hyphens, or underscores.
`START_WAIT` is allowed only as the first step and runs once before all repeats;
`WAIT` runs each repetition. Both accept 0–4294967295 ms. The web console shows
saved routines as named Run buttons. Before starting, the firmware checks each
pulse against the active payload limit and total duration against the remaining
arming window when these optional limits are nonzero. There is no total-runtime
cap. Stop and interlock checks remain active throughout a routine.

A routine must be explicitly saved after editing. Starting a routine does not
arm the dispenser; send `Arm` separately. A routine also cannot start while its
hardware profile is already busy or active. `StopAll`, completion, error,
timeout, or a changed safety condition stops the routine and makes hardware safe.
While it runs, external start/arm commands for the dispenser are rejected so a
manual command cannot overlap the saved timing pattern. Stop and status commands
remain available.

Example:

```text
RoutineCreate:dots
RoutineAdd:dots:DISPENSE:200
RoutineAdd:dots:WAIT_IDLE
RoutineAdd:dots:WAIT:800
RoutineRepeat:dots:6
RoutineSave:dots
Arm
RoutineRun:dots
```

The `COMMAND:` allowlist supports bounded dispenser commands and selected
stepper/DAC actions. It rejects arming, reboot, radio, configuration, erase,
self-test, transport-send, and nested routine commands.

## Coordinate-triggered routines

Select `GeoSource:MAVLINK` or `GeoSource:API`, build up to 12 ordered points with
`GeoAdd:latitude,longitude,radiusMeters,savedRoutineName`, then `GeoSave`.
`GeoStart` explicitly enables automatic arming/routine execution at each next
point; it starts at point 1 and requires a fresh position. `GeoStop` stops output
and the sequence. `GeoStatus`, `GeoList`, `GeoLoad`, `GeoClear`, and
`GeoResetPosition` provide readback/editing and source-clock recovery.
`GeoPosition:latitude,longitude,accuracyMeters,ageMs` supplies custom fixes;
use `POST /api/position` with those four plain-text fields for continuous input.
Position loss for over three seconds stops the sequence. See the
[position integration guide](docs/COORDINATE_ROUTINES.md) for MAVLink, proprietary
provider contracts, timing behavior, and setup examples.

## Core

```text
Ping
Help
Status
ConfigRead
BootModeStatus
USBStatus
HeapStatus
BLEStatus
StopAll
Reboot
```

## Boot policy and self-test

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

Fresh controllers default to production mode. The dispenser never performs an
automatic boot actuation test. The persistent self-test checks arming and
disarming but intentionally skips a physical dispense pulse.

## Status indicators

```text
IndicatorStatus
IndicatorTest
IndicatorConnectionTest
IndicatorActivityTest
```

Default pins are GPIO23 for connection and GPIO5 for activity.

## Radio profiles

```text
RadioStatus
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

Radio profile changes use a saved trial, reboot, health check, and last-known-good
rollback. Pending commands, routines, and addon outputs are stopped before the
managed reboot.

## Wi-Fi configuration

```text
WiFiMode:AP
WiFiMode:STA
WiFiMode:APSTA
WiFiFallbackAP:ON
WiFiFallbackAP:OFF
WiFiTxPower:LOW
WiFiTxPower:MAX
WiFiLR:ON
WiFiLR:OFF
WiFiTxPower:<supported-dBm>
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

`WiFiLR:ON` selects LR-only Wi-Fi on AP and STA. Both ends must be LR-capable
ESP32s; ordinary phones and routers cannot join. OFF is the default. Use
`ConfigApply` to restart Wi-Fi and `ConfigSave` to persist the setting.
`RadioStatus` distinguishes desired and active LR. See the
[LR and BLE relay guide](../docs/WIFI_LR.md) for setup and USB recovery.

## Explicit transport output

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

## Optional advanced stepper/DAC profile

These commands exist only when `DRONE_CFG_HARDWARE_PROFILE` is set to `2` in
`QuickConfig.h` instead of the normal dispenser profile.

Stepper moves:

```text
RPM:<rpm>,<steps>,<direction>
DEG:<rpm>,<degrees>,<direction>
Stop
CoilsOff
GetMotorStats
MoveFullCW
MoveFullCCW
MoveHalfCW
MoveHalfCCW
```

Stepper configuration:

```text
SetRevSteps:<steps>
SetMinRPM:<rpm>
SetMaxRPM:<rpm>
SetStartRPM:<rpm>
SetRampRPM:<rpm-per-second>
SetMinStepIntervalUs:<microseconds>
StepMode:4
StepMode:8
HoldTorque:1
HoldTorque:0
PrintStepOrder
NextStepOrder
StepOrder:<four-unique-digits>
```

DAC and compatibility output:

```text
DACStatus
DACRefMV:<2500-to-3600>
DAC1:MV:<millivolts>
DAC1:ON
DAC1:OFF
DAC1:TEST3S
GPIO26:ON
GPIO26:OFF
DACSave
DACLoad
DACDefaults
DACErase
```

The advanced profile preserves the original behaviour and documentation under
`src/addons/stepper_dac/`. It is intended for supervised bench work rather than
the normal aircraft payload build.
