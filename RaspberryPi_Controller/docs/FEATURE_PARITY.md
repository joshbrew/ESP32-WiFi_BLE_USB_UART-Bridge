# ESP32 feature audit → Raspberry Pi 0.3.0

Checked the current dispenser and transport command references, routine engine,
coordinate mission/MAVLink receiver, radio handlers, self-test engine and web
console in this repository. The Pi keeps its Linux deployment and existing
transport/hardware adapters while implementing the new command behavior.

| ESP32 addition / feature | Pi support | Verification / platform difference |
| --- | --- | --- |
| Unlimited default maximum/arm expiry; optional saved limits | Yes: zero disables each; nonzero installation ceilings remain enforceable | Tests cover old saved limits, zero limits, full 32-bit values and invalid overflow |
| Pulse/delay/gap up to 4294967295 ms, repeat counts up to 4294967295; no total runtime cap | Yes | Long running commands remain interruptible; no allocation or loop proportional to repeat count |
| START_WAIT first step, once before repeats | Yes: command, dot builder, custom editor, live delay status | Repeated pattern test distinguishes initial delay from each-cycle gaps |
| Pulse/profile and finite arm-window preflight | Yes, including conservative 1000 ms service margin | Rejects before output activation; checks saved coordinate routines too |
| Named saved Run buttons with timing/repeat summary | Yes | Unsaved edits are marked and cannot be launched through the saved button |
| Twelve ordered coordinates, radius and saved routine selection | Expanded to 500 on Pi: all eleven Geo commands, console import/build progress, sixteen-point stream pages | Ordering, accuracy, overlapping points, dateline distance, 500-point persistence and paged readback tested |
| Auto-arm at next point; disarm after routine / between points | Yes, only after explicit GeoStart | Stops cancel the whole sequence; plans never resume enabled after reboot |
| Stale position (>3 seconds), invalid custom sample, interlock/fault stops | Yes | Independent GPIO watchdog inhibits output even with a stalled web/command loop |
| Receive-only MAVLink 1/2 UDP GPS_RAW_INT + GLOBAL_POSITION_INT | Yes: default port 14550, configured system/component | CRC, truncated payloads, ID filters, duplicate/reordered boot clock, wrap, reset, signed-frame rejection and local UDP tested |
| Latest custom position endpoint, 128-byte limit, source isolation | Yes: /api/position | Full command queue cannot block the latest sample; malformed input invalidates prior fix |
| Proprietary position provider contract | Same generic aircraft-fix contract and shared bridge script | No verified XAG/vendor endpoint or handset-location substitute is claimed |
| Open station network; APSTA and AP fallback; Save + Apply | Yes: explicit clear-password checkbox and save/apply control | Concurrent APSTA needs a second interface; single interface uses STA with fallback |
| Desired radio ON/OFF flags, save/apply, mutual exclusion of SPP vs Wi-Fi/BLE | Yes | Active, desired and saved profiles shown; profile apply/rollback tests |
| WiFiLR / LR-only ESP32 link | Unavailable natively | Explicit unsupported error. Use an existing ESP32 relay through Pi BLE/serial for an LR ground link |
| Routing: Send, BLE, Wi-Fi, SPP, USB, UART | Yes | Same newline command language and BLE UART UUIDs; Pi USB is a Linux serial gadget/adapter |
| Dispenser polarity/pin, interlock, four payload profiles | Yes | BCM wiring and installation-approved pin ownership; actual wiring needs a Pi bench check |
| Advanced stepper / DAC / direct digital profile | Yes | External MCP4725 substitutes for ESP32 native DAC; select stepper_dac |
| Indicators; production/debug boot policy; resumable self-tests | Yes | Software/inactive tests are checkpointed. Hardware actuation, RF and reboot sweeps are reported as skipped |
| Browser update upload; health rollback / recovery | Yes, Pi application zip bundles | ESP32 .bin cannot run on Pi; OS/root helper/dependency changes require installer/SSH |

The test suite includes a repository command inventory check. Adding a documented
ESP32 command requires updating Pi support or recording a platform exception.
Software tests and simulator browser checks do not certify GPIO timing, USB,
Bluetooth/RF coexistence or installation on either physical Pi model.
