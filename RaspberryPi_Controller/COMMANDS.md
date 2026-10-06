# Pi command reference

Commands are case-insensitive; values such as SSIDs/passwords preserve case.
Separate batch commands with newlines. BLE/SPP/USB/UART require the final newline.
`Help` lists commands; `@STATE` on stream transports returns framed JSON. HTTP
accepts up to eight commands per submission; acceptance is not execution success.

## General, diagnostics and routing

| Commands | Behavior |
| --- | --- |
| `Ping`, `Help` | PONG / reference |
| `Status`, `ConfigRead` | Redacted state, profile/routine libraries and feature status |
| `USBStatus`, `HeapStatus`, `BLEStatus`, `RadioStatus`, `WiFiStatus`, `SendStatus` | State including transport and Linux memory status |
| `StopAll` | Clear queue, stop routine/motor/all outputs, disarm, abort active self-test |
| `Send:text` | Raw text to available destinations other than the source |
| `SendBLE:text`, `SendSPP:text`, `SendWiFi:text` | One explicit radio destination |
| `SendUSB:text`, `SendSerial:text`, `SendUART:text` | One explicit serial destination; SendSerial aliases USB |
| `ProductionMode`, `DebugMode`, `BootModeStatus` | Save normal/check-on-boot policy, or inspect it |
| `Reboot`, `WebRestart` | Stop outputs, then schedule host/service restart; requires OS control |

Status never exposes passwords or the update token. `Send` is application routing,
not a transparent binary bridge. Destinations must be available; see README.

## Dispenser profile

Select `hardware_profile: "dispenser"`. Settings require a disarmed, idle output.

| Commands | Behavior |
| --- | --- |
| `Arm`, `DispenserArm` | Arm for configured timeout without activating GPIO |
| `Disarm`, `DispenserDisarm` | Stop/disarm; clear a latched fault after interlock closes |
| `Dispense:ms` | Full bounded pulse must fit remaining arm window |
| `DispenseStop`, `DispenserOff` | Immediate global stop barrier, including disarm |
| `GPIO26:ON`, `GPIO26:OFF` | Default timed dispenser pulse / global stop; logical name uses the configured pin |
| `DispenserStatus`, `PayloadStatus` | State |
| `DispenserPin:BCM` | Installation-approved output pin, no ownership conflicts |
| `DispenserActiveHigh:ON/OFF` | Active-high / active-low output |
| `DispenserDefaultPulse:ms` | Default pulse duration |
| `DispenserMaxPulse:ms` | Maximum pulse, within installation ceiling |
| `DispenserArmTimeout:ms` | Arm window, within installation ceiling |
| `DispenserSave` | Persist standalone settings; clear named profile selection |
| `DispenserDefaults` | Apply installation defaults in memory |
| `DispenserErase` | Clear saved standalone settings/selection; retain named profiles |
| `PayloadProfileList`, `PayloadProfileShow:name` | List / inspect |
| `PayloadProfileSave:name` | Save current settings and select profile |
| `PayloadProfileUse:name` | Apply/select saved profile |
| `PayloadProfileDelete:name`, `PayloadProfileEraseAll` | Delete one / all named records |

Names: 1–15 letters, digits, hyphens or underscores; normalized to lowercase.
Four profiles maximum. Default/pulse duration: 1–4294967295 ms. Maximum pulse and
arm timeout: 0–4294967295 ms; zero disables that limit. Default must fit an enabled
maximum; maximum (or default when maximum is disabled) must fit an enabled arm
timeout. Fresh installation ceilings default to zero (disabled). Existing
nonzero installation ceilings and saved profile limits are preserved.

## Routine commands

| Commands | Behavior |
| --- | --- |
| `RoutineCreate:name` | Create/reset in-memory routine |
| `RoutineAdd:name:DISPENSE:ms` | Append dispenser pulse |
| `RoutineAdd:name:START_WAIT:ms` | First step only; one-time delay before all repeats, 0–4294967295 ms |
| `RoutineAdd:name:WAIT:ms` | Append wait each repeat (0–4294967295 ms) |
| `RoutineAdd:name:WAIT_IDLE` | Wait for current timed action/motor to finish |
| `RoutineAdd:name:COMMAND:command` | Append allowlisted hardware action |
| `RoutineRepeat:name:n` | 1–4294967295 repeats |
| `RoutineSave:name` | Validate and persist |
| `RoutineRun:name` | Run in-memory version; outputs must be healthy/idle |
| `RoutineStop` | Global stop barrier, disarm and clear pending work |
| `RoutineStatus`, `RoutineList`, `RoutineShow:name` | Inspect execution/library/record |
| `RoutineErase:name` | Remove memory and saved record |

Four routines, ten steps each, no total runtime cap. Enabled payload limits are
checked before starting: each pulse must fit the maximum and estimated duration
including a 1000 ms margin must fit the remaining arm window. The dispenser
requires prior arming. Allowed dispenser COMMAND actions are `Dispense:ms` and
`DispenseStop`. Advanced actions include RPM/DEG/move presets, Stop/CoilsOff,
DAC1:MV, DAC1/GPIO26/DACAll/OutputAll:ON/OFF. Test pulses/config/radio/admin commands
are excluded. Completion waits for trailing motion/pulse, then stops all outputs.

## Coordinate sequences

Dispenser profile only. A saved plan can hold 500 ordered points. Source and
points persist; position, enablement and progress never resume after reboot.

| Commands | Behavior |
| --- | --- |
| `GeoClear` | Clear in-memory plan and position; stop first |
| `GeoSource:MAVLINK/API` | Select aircraft position source; clears position, marks plan unsaved |
| `GeoAdd:lat,lon,radiusMeters,routine` | Append next point; lat ±90, lon ±180, radius 0.1–1000 m, saved routine name |
| `GeoSave`, `GeoLoad` | Save / load checksummed plan; loading resets position |
| `GeoList`, `GeoList:offset`, `GeoStatus` | Inspect a 16-point page (default offset 0) / execution and fix summary |
| `GeoStart` | Explicitly authorize auto-arming at points, beginning at point 1; saved nonempty plan and fresh position required |
| `GeoStop` | Immediate global stop barrier; cancel routine, disarm, clear queue |
| `GeoResetPosition` | Clear fix and MAVLink source clock after sender reboot; stop first |
| `GeoPosition:lat,lon,accuracyMeters,ageMs` | Bench API fix; source must be API |

Continuous custom fixes use `POST /api/position` with four comma-separated
plain-text fields, max 128 bytes, accuracy -1 (unknown) or 0–100000 m, age 0–3000
ms. Send 2–5 fixes/second. This latest-sample endpoint bypasses command admission;
invalid custom fixes revoke freshness. Source mismatch cannot override MAVLink.
Freshness expires three seconds after measurement, including network/queue age.
Outputs disarm between points, and stale position/interlock/fault/manual stops
cancel execution. Known accuracy greater than radius prevents new triggers.
Saved routines are copied from persistent storage at the point trigger.
Manual actuation, edits and administrative transitions are blocked while enabled.
See [position integration](docs/COORDINATE_ROUTINES.md) for MAVLink/bridge setup.
HTTP state includes the whole plan. Stream `@STATE` omits the point array to keep
Bluetooth status responsive; `@GEO:offset` returns an `@GEO` JSON page containing
offset/count/points/next, up to sixteen points at a time. Offsets accept 0–500.
The console loads large BLE plans through these pages and shows build/read progress.

## Radio configuration and profiles

Real OS changes require `enable_os_control: true`; simulation makes virtual
changes only. Radio transitions stop/disarm outputs and clear the queue.

| Commands | Behavior |
| --- | --- |
| `ModeWiFi` | Apply/save WIFI profile |
| `ModeWiFiBLE`, `ModeWiFiBLEP` | Apply/save WIFI_BLE / WIFI_BLE_P |
| `ModeBLE` | Apply/save BLE-only profile |
| `ModeBTSerial` | Apply/save SPP-only profile |
| `ModeUSB` | Apply/save serial-only profile; serial recovery required |
| `RadioBoot:profile` | Save next boot only: WIFI, WIFI_BLE, WIFI_BLE_P, BLE, SPP, USB |
| `WiFi:ON/OFF`, `BLE:ON/OFF` | Edit desired profile; enabling excludes SPP |
| `ClassicBT:ON/OFF`, `SPP:ON/OFF` | Edit desired SPP setting; enabling excludes Wi-Fi/BLE |
| `BleAdvertise` | Enable/register BLE peripheral advertising |
| `BleWebHandoff`, `BleWebCancel` | Acknowledge Linux persistent BLE registration; no heap handoff required |
| `WiFiMode:AP/STA/APSTA` | Desired NetworkManager role |
| `WiFiFallbackAP:ON/OFF` | Station-failure AP fallback |
| `WiFiStaSSID:ssid`, `WiFiStaPassword:password` | Station settings in memory |
| `WiFiStaClear` | Clear station credentials, including password |
| `WiFiApSSID:ssid`, `WiFiApPassword:password` | AP settings in memory |
| `WiFiTxPower:LOW/MAX/dBm` | LOW=11 dBm, MAX=automatic, numeric 0–30 subject to driver/country |
| `ConfigSave`, `ConfigLoad` | Save current / load saved settings without OS apply |
| `ConfigApply` | Trial current settings/profile and commit on success |
| `ConfigDefaults`, `ConfigErase` | In-memory defaults / clear saved radio record |
| `WiFiLR:ON/OFF` | Explicit unsupported error: ESP32 proprietary LR |

AP password requires 8–63 characters; station password empty (open network) or
8–63. SSIDs max 32 UTF-8 bytes; passwords max 63 bytes; printable text required.
APSTA uses a second configured interface for concurrency, otherwise STA with AP
fallback. ON/OFF changes require ConfigSave for persistence and ConfigApply for
activation; Mode commands apply/save immediately. A blank password field keeps
the saved value in the console; the explicit open-network checkbox sends an
empty WiFiStaPassword to clear it.

## Optional stepper / external DAC profile

Select `hardware_profile: "stepper_dac"`. This profile has no dispenser arming.
Use external hardware inhibit where needed. Motor settings are volatile.

| Commands | Behavior |
| --- | --- |
| `RPM:rpm,steps,direction` | Move effective steps; direction 1=CW, 2=CCW |
| `DEG:rpm,degrees,direction` | Move degrees using configured effective steps/rev |
| `MoveFullCW/CCW`, `MoveHalfCW/CCW` | 360° / 180° preset at ≤10 RPM |
| `Stop`, `CoilsOff` | Global stop barrier; all channels become inactive and coils release |
| `GetMotorStats` | Motor settings, position, remaining steps, timing lateness |
| `SetRevSteps:n` | Base full-step count per revolution |
| `SetMinRPM:n`, `SetMaxRPM:n`, `SetStartRPM:n` | Speed/ramp limits within installation ceiling |
| `SetRampRPM:n` | Ramp rate in RPM per second |
| `SetMinStepIntervalUs:n` | Minimum interval within installation floor |
| `StepMode:4/8` | Full/half steps; effective steps/rev doubles in half mode |
| `HoldTorque:0/1` | Release/hold coils after normal move completion |
| `PrintStepOrder`, `NextStepOrder`, `StepOrder:0123` | Inspect / rotate / set permutation |
| `DACStatus`, `OutputStatus` | DAC/digital and hardware state |
| `DACRefMV:n` | DAC supply/reference, 2500–3600 mV |
| `DAC1:MV:n` | Desired level, 0–reference mV |
| `DAC1:ON/OFF/TEST3S` | Analog output enabled / stopped / three-second test |
| `GPIO26:ON/OFF/TEST3S` | Direct digital output, configured BCM pin |
| `DACAll:ON/OFF/TEST3S`, `OutputAll:ON/OFF/TEST3S`, `DACTest3S` | Both channels |
| `DACSave`, `DACLoad`, `DACDefaults`, `DACErase` | Save/load/default/erase reference and level; never persist ON state |

The direct command handler can Stop with hold torque, but transport submissions
treat Stop/CoilsOff and output OFF as global stop barriers for predictable recovery.
DAC ON and digital ON remain active until OFF/StopAll unless TEST3S is used.
The MCP4725 uses a volatile register; startup/shutdown explicitly requests zero.

## Indicators and self-tests

| Commands | Behavior |
| --- | --- |
| `IndicatorStatus` | Optional LED pin/state/test status |
| `IndicatorTest` | Two connection pulses, then three activity pulses |
| `IndicatorConnectionTest`, `Indicator16Test` | Connection test aliases (configured Pi BCM pin) |
| `IndicatorActivityTest`, `Indicator17Test` | Activity test aliases (configured Pi BCM pin) |
| `SelfTestStart` | Start twelve software/inactive-state checks |
| `SelfTestResume` | Resume paused checkpoint after interruption |
| `SelfTestAbort` | Cancel and restore snapshot; aborted reports cannot resume |
| `SelfTestClear` | Clear report when inactive |
| `SelfTestStatus` | Progress, pass/fail/skip counts, per-check results |

Physical motion, RF transition/reboot sweeps and wiring tests are skipped. Start
requires outputs off/disarmed. Configuration/actuation is blocked during checks.
DebugMode checks on boot unless a paused checkpoint awaits explicit resume.

## Updates

Updates use HTTP rather than command text: `POST /api/ota` with a Pi `.zip`,
`X-Update-Token`, and optional whole-bundle `X-Update-SHA256`. Multipart uploads
require one file field named `firmware` with a `.zip` filename. Use
`GET /api/update/status` or the browser panel for status. ESP32 binaries, root
helper/OS/dependency changes are not application bundles. See README for building
bundles, rollback, recovery and installation.
