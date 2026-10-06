# Raspberry Pi Zero W / Zero 2 W Controller

A Python/Linux version of the modular command bridge and dispenser system, with
its own mobile web console. Both Pi boards use the same source. Use Raspberry Pi
OS Lite **32-bit** for a common image that supports the original ARMv6 Zero W and
the Zero 2 W. Python **3.11+**, NetworkManager, BlueZ and systemd are required;
the installer targets Raspberry Pi OS Bookworm or later.

This is an SD-card application, not an Arduino firmware image. The ESP32 builds
remain separate. See [COMMANDS.md](COMMANDS.md) for the command reference.
The current ESP32 additions are tracked in [FEATURE_PARITY.md](docs/FEATURE_PARITY.md).

## Features and parity

| Feature | Pi implementation |
| --- | --- |
| Web application | Dispenser, profiles, dot patterns, custom routines, motor/DAC controls, radio settings, status, logs, self-tests and update upload |
| Command transports | HTTP, BLE UART, authenticated Classic Bluetooth SPP, USB serial gadget/adapter, GPIO UART |
| Routing | Shared bounded queue, explicit Send/SendBLE/SendSPP/SendWiFi/SendUSB/SendUART routing |
| Dispenser | Arm/disarm, timed pulses, timeout, interlock, polarity/pin configuration, four saved payload profiles |
| Routines | Four routines, ten steps, one-time START_WAIT, WAIT/WAIT_IDLE/allowed actions, 32-bit timing/repeats, repeat until stopped, optional-limit preflight, named Run buttons |
| Coordinate sequences | 500 ordered points, saved routine snapshots, automatic arming at each next point, MAVLink UDP or custom position API, independent stale-position output inhibit, paged stream readback |
| Optional hardware | 28BYJ-48/ULN2003 stepper, external MCP4725 DAC, digital output; select `stepper_dac` |
| Indicators | Optional connection/activity LEDs on separately assigned BCM pins |
| Radio management | Wi-Fi/BLE/SPP/USB profiles, saved next-boot profile, AP/STA, station-to-AP fallback, transmit power, failed-transition rollback |
| Self-tests | Twelve checkpointed software/inactive-state checks, report, abort and resume after interruption, optional check-on-boot policy |
| Application update | Token-protected .zip upload, SHA-256 file verification, immutable releases, startup health check, automatic rollback and boot recovery |
| Persistence | Atomic checksummed records; active output/arming states never restored on boot |
| Deployment | Dedicated account, systemd service, restricted root helper, optional USB gadget setup, Linux CI |

Pi-specific differences:

- The Pi has no ESP32 native analog DAC. The optional profile uses an **MCP4725**
  over I2C. It never writes DAC EEPROM; startup explicitly drives zero.
- A single Wi-Fi interface runs STA with AP fallback for `APSTA`. Set
  `wifi_ap_interface` to a second adapter for simultaneous AP + STA. The console
  reports the actual role. AP mode offers NetworkManager shared networking and
  captive-probe redirects; it does not intercept arbitrary DNS/web traffic.
- `WIFI_BLE_P` uses the same persistent BlueZ peripheral registration as
  `WIFI_BLE`; Linux does not need the ESP32 heap-reclaim handoff.
- Proprietary ESP32 `WiFiLR` is unavailable. Numeric transmit power is subject
  to the Pi driver and regulatory country. `MAX` restores automatic control;
  rollback restores automatic power rather than a previous manual setting.
  An existing ESP32 LR relay can remain the ground radio, with Pi BLE/serial
  carrying its newline command stream; see the [LR guide](../docs/WIFI_LR.md).
- Self-tests deliberately skip physical motion, RF transition/reboot sweeps and
  external wiring certification. They do not auto-actuate or reboot equipment.
- Updates accept Pi application bundles, not ESP32 `.bin` images. OS packages,
  dependencies, boot settings and root helper changes use the installer/SSH.
- Linux scheduling is not hard real time. Pulse and motor workers use independent
  threads; precise motion or guaranteed shutdown requires suitable external
  control hardware. These features have software/simulation coverage, **not a
  physical Zero W/Zero 2 W bench certification**.

## Install

1. Flash Raspberry Pi OS Lite using Imager; choose the board, hostname, country,
   Wi-Fi and SSH settings.
2. Copy/clone the repository to the Pi and connect using SSH.
3. From this directory run:

```sh
sudo sh deploy/install.sh --enable-os-control
```

Open `http://<pi-address>:8080/`, or `http://<hostname>.local:8080/` when mDNS is
available. Installation starts in **simulation**. No actuator or OS radio changes
occur until `simulate` is set to `false` in `/etc/pi-controller/config.json`.
Without `--enable-os-control`, existing Pi OS networking works but radio changes,
controller restart and host reboot commands cannot change the OS.

The installer uses OS packages for aiohttp, serial and GPIO to avoid compiling
extensions on ARMv6; only pure-Python dbus-next is installed with pip. It preserves
configuration and saved libraries on rerun. An initial private update token is
generated in the configuration file without printing it.

```text
/opt/pi-controller/.venv/                  OS-backed Python environment
/opt/pi-controller/releases/<sha256>/     Root-owned application releases
/opt/pi-controller/current               Current release symlink
/etc/pi-controller/config.json           Installation config and private token
/var/lib/pi-controller/                  Saved settings/libraries/checkpoints
/usr/local/lib/pi-controller/            Root-owned OS/update helper and validator
```

```sh
sudo systemctl stop pi-controller
sudo nano /etc/pi-controller/config.json
sudo systemctl start pi-controller
sudo systemctl status pi-controller
sudo journalctl -u pi-controller -f
```

Hardware startup errors fail closed; real hardware never silently switches to
simulation. Saved dispenser settings override installation defaults, within the
installation pin/timeout ceilings. Resolve saved-library pin conflicts before
changing hardware profiles or assigning indicator pins.

The command API is intended for a **trusted local network**. Same-origin checks
block browser submissions from unrelated websites; they are not authentication.
BLE UART accepts commands from nearby clients; SPP requires paired authorization.
Only update installation requires the private token. Keep the service off public
networks. Wi-Fi passwords are redacted from state/log acknowledgements, but are
stored in local protected configuration records. Read the update token via SSH;
the browser never receives it through the state API.

## Dispenser wiring and timing

Default profile: `hardware_profile: "dispenser"`. Set `simulate: false` only after
checking the output circuit and inactive polarity.

| Connection | BCM GPIO | Physical header pin |
| --- | ---: | ---: |
| Dispenser logic output | 26 | 37 |
| Optional dry-contact interlock example | 17 | 11 |
| UART TX / RX | 14 / 15 | 8 / 10 |
| Ground example | — | 6 |
| Optional connection / activity LED | 23 / 24 | 16 / 18 |

GPIO is **3.3 V logic**, not actuator power. Use an appropriate driver, separate
actuator supply and an independent inactive bias. Never connect 5 V logic or
actuator voltage to the header. Pi BCM numbers differ from ESP32 wiring.

For a fail-closed interlock set `interlock_pin: 17` and
`interlock_open_high: true`. A closed contact to ground permits operation; an
open/disconnected wire blocks it using a pull-up. The alternate false setting
uses a pull-down and the closed contact connects to 3.3 V. An interlock opening
while armed stops output and latches a fault; close it and send `Disarm` to clear.
Profiles cannot disable this installation-level interlock.

A dedicated worker checks pulse deadlines/interlock every 5 ms. Status reports
`maxStopLatenessMs`. A killed process, power loss or driver failure cannot guarantee
inactive output: use an external inhibit/maximum-on timer wherever that guarantee
matters. Ground-test with the actuator disconnected first.

```text
Arm
Dispense:250
Disarm
```

Configuration changes require a disarmed, idle dispenser. Profile names are
case-insensitive, 1–15 letters/digits/hyphens/underscores.
Fresh installations use `max_pulse_ms: 0` and `arm_timeout_ms: 0`: optional
maximum/expiry are disabled, matching the ESP32. Every pulse still has an explicit
1–4294967295 ms deadline. Nonzero installation values impose ceilings that saved
settings cannot remove. Existing saved timing limits remain effective on upgrade;
the console's **Remove saved time limits** disarms and clears them only when
installation ceilings permit it. A selected named profile retains its own limits
until edited/saved again.

```text
DispenserDefaultPulse:150
DispenserMaxPulse:750
DispenserArmTimeout:30000
PayloadProfileSave:fine
PayloadProfileUse:fine
```

## Optional stepper / DAC / digital profile

Select `hardware_profile: "stepper_dac"`, enable I2C in `raspi-config`, and reboot
as needed. This profile replaces the dispenser output; use the dispenser profile
for arming and timed dispensing. The advanced digital output is direct ON/OFF.

| Connection | Default wiring |
| --- | --- |
| ULN2003 IN1–IN4 | BCM18,19,20,21 (physical 12,35,38,40) |
| ULN2003 motor supply | Suitable external supply with common ground |
| MCP4725 SDA / SCL | BCM2 / BCM3 (physical 3 / 5), 3.3 V bus |
| MCP4725 address | I2C bus 1, address 0x60; configurable 0x60–0x67 |
| Digital output | BCM26 (physical 37), external driver as needed |

Use a 3.3 V-compatible DAC board and bus pull-ups. `dac_reference_mv` is the actual
DAC supply/reference, default 3300 mV. Pin ownership checks reject overlaps among
motor, digital, interlock and enabled LEDs. The dispenser interlock is not a motor
or analog-output inhibit; use an external inhibit for those channels.

Stepper motion supports full/half stepping, direction, degrees/steps, coil order,
ramp and hold torque. `stepper_max_rpm` and `stepper_min_interval_us` are installation
ceilings. Missed steps are not emitted in a burst. `maxStepLatenessUs` exposes
scheduler delay; conservatively tune speed on the original Zero W. Motor settings
are volatile. DAC reference/level can be saved, while ON state is never saved.

```text
DEG:5,90,1
GetMotorStats
Stop
CoilsOff
DAC1:MV:1000
DAC1:TEST3S
DACSave
```

Enable LEDs with `indicators: true`; use a resistor and proper polarity for each
LED. Defaults BCM23/24 avoid UART and motor pins. `IndicatorTest` exercises both;
individual tests are available. The OS activity LED remains OS-owned.

## Radio profiles and Wi-Fi

Enable OS control during installation or in the root-owned config. Runtime
commands work in simulation without modifying the host OS. Initial networking
continues to use Pi OS/Imager until a saved application radio profile is applied.
Radio changes stop outputs and clear pending work.

```text
WiFiStaSSID:my-network
WiFiStaPassword:my-password
WiFiMode:STA
WiFiFallbackAP:ON
WiFiApSSID:Pi-Controller
WiFiApPassword:my-ap-password
ConfigSave
ConfigApply
```

`ConfigSave` stores settings without applying them. `ConfigApply` runs a trial and
commits on successful OS/transport startup. `ConfigLoad` restores saved settings
in memory. The shipped fallback AP password is `pibridgecontrol`; replace it before
live use. NetworkManager determines the shared-network IP (inspect `ip -4 address
show wlan0`), commonly 10.42.0.1. The HTTP service remains on port 8080.

`ModeWiFi`, `ModeWiFiBLE`, `ModeWiFiBLEP`, `ModeBLE`, `ModeBTSerial`, `ModeUSB`
apply and save a profile. `RadioBoot:WIFI_BLE` sets the next boot profile only.
`WiFi:ON/OFF`, `BLE:ON/OFF`, `SPP:ON/OFF` edit the desired profile. Enabling Wi-Fi
or BLE excludes SPP; enabling SPP excludes Wi-Fi and BLE. Use `ConfigSave` to
persist and `ConfigApply` to apply, matching the current ESP32 command behavior.
The console shows active, desired and saved profiles separately. Serial recovery must
be configured before disabling every radio on real hardware. A radio-only profile
can disconnect the HTTP console; restore Wi-Fi through BLE/SPP/serial or SSH.

The root helper snapshots managed connection files and active connections before
changes, with a 90-second watchdog. Failed changes restore that snapshot; startup
abandons an uncommitted saved profile trial. This guards OS/app failures rather
than proving a remote client can reconnect. Confirm radio changes from a local
serial/SSH recovery link when commissioning hardware.

## BLE and Classic Bluetooth serial

Initial startup flags: `ble: true` and/or `spp: true` (saved radio profiles take
precedence later). BlueZ must be powered: `sudo bluetoothctl power on`. Adapter
selection is `ble_adapter`, default hci0; name `PiBridge` is limited to eight UTF-8
bytes for legacy advertisement size.

```text
Service:   6e400001-b5a3-f393-e0a9-e50e24dcca9e
RX write:  6e400002-b5a3-f393-e0a9-e50e24dcca9e
TX notify: 6e400003-b5a3-f393-e0a9-e50e24dcca9e
SPP UUID:  00001101-0000-1000-8000-00805f9b34fb
```

Subscribe to BLE TX and write newline-terminated UTF-8 commands to RX. Notifications
use 20-byte chunks. `@STATE` returns an `@STATE <JSON>` frame. Use one BLE controller
client; notifications share one UART stream. Retry `BLE:ON`/`SPP:ON` after BlueZ
recovers from a disconnect. SPP serves one client on configured `spp_channel`.

For first SPP pairing, run `bluetoothctl` on the Pi with `agent on`,
`default-agent`, `pairable on`, and temporarily `discoverable on`. Pair from the
client, approve the pairing/service authorization, then turn discoverability off.
SPP does not silently auto-trust devices. Use an RFCOMM serial-terminal client;
Web Bluetooth supports BLE, not Classic SPP.

Web Bluetooth needs HTTPS or localhost. A plain Pi HTTP page still supports HTTP
commands. For a desktop BLE test, forward the page:

```sh
ssh -L 8080:localhost:8080 <user>@<pi-address>
```

Open `http://localhost:8080/` in a Web Bluetooth-capable browser. BLE connects
through the desktop Bluetooth radio. The update panel can request Wi-Fi + BLE,
disconnect BLE and return to HTTP upload. On a remote HTTPS proxy preserve the
same origin; configure trusted proxy scheme handling explicitly. This app does
not trust arbitrary forwarded headers.

## USB serial and GPIO UART

Optional automatic serial gadget setup:

```sh
sudo sh deploy/install.sh --enable-os-control --usb-gadget
sudo reboot
```

The optional tool backs up boot files, preserves the single-line kernel arguments,
adds dwc2/g_serial, sets `/dev/ttyGS0`, and disables its login getty. Use the Pi
**USB data** connector and a data cable, not PWR IN. Existing host-mode/conflicting
gadget configurations are rejected. Composite serial+network gadgets need a
separate configfs setup; do not combine g_serial and g_ether/rpi-usb-gadget.

An attached USB serial adapter can use `usb_device: "/dev/serial/by-id/..."` in
host mode. For GPIO UART enable serial hardware and disable serial login in
`raspi-config`, reboot, then set `uart_device: "/dev/serial0"`. Cross 3.3 V TX/RX
and share ground. The usual Zero W/Zero 2 W mapping uses mini UART on GPIO and
PL011 for Bluetooth; retain Bluetooth when using BLE/SPP. USB and UART must have
distinct device paths. Missing devices reconnect without blocking HTTP.

All stream commands require a newline. Arbitrary bytes are not transparently
bridged; use `SendUART:payload`/`SendUSB:payload` explicitly.

## Routines, boot policy and self-tests

The browser provides a dot builder and custom editor. Edits run from memory;
`RoutineSave` persists them. Direct hardware actions cannot overlap a routine.
Named Run buttons show saved delay/pulse/gap/repeat values; unsaved edits disable
that button until saved. `START_WAIT:ms` is first-step-only and runs once before
all repeats. `WAIT:ms` runs every repeat, including the final gap. Pulse values
are 1–4294967295 ms; delays/gaps 0–4294967295 ms; repeats 1–4294967295.
**Repeat until stopped** saves `RoutineRepeat:name:FOREVER` (stored repeats=0).
It runs the initial delay once, then repeats on/off cycles until stopped. Disable
optional arm expiry before running; maximum pulse and interlock still apply.
Continuous mode survives reboot in the routine library, while execution and
arming never resume automatically. Both the dot builder and custom editor support it.
There is no total routine runtime cap. With nonzero payload limits, preflight
checks every pulse and the estimated duration plus a 1000 ms service margin
against the remaining arm window before any output starts.

```text
RoutineCreate:dots
RoutineAdd:dots:START_WAIT:1000
RoutineAdd:dots:DISPENSE:200
RoutineAdd:dots:WAIT_IDLE
RoutineAdd:dots:WAIT:800
RoutineRepeat:dots:6
RoutineSave:dots
Arm
RoutineRun:dots
```

The dispenser must be armed separately. Completion waits for the last pulse,
then disarms. The advanced profile allows COMMAND steps for moves, DAC/digital
ON/OFF and stop actions. Use WAIT_IDLE after motion and WAIT:ms to time sustained
outputs. Completion/failure/manual stop shuts down every channel. Routine
commands cannot reboot, arm, change radios or invoke another routine.

`ProductionMode` saves a normal boot policy. `DebugMode` saves check-on-boot policy.
`SelfTestStart` requires inactive outputs, then checks config, output inactivity,
non-actuating arm/disarm, queue, storage, radio records, libraries, BLE/SPP and
indicator workers. Missing optional features and physical tests are reported as
skipped. State includes progress/results. `SelfTestAbort` restores the snapshot;
`SelfTestClear` clears the report. An interrupted running checkpoint becomes
paused on startup; `SelfTestResume` continues it explicitly. Checks never resume
actuation from a saved state.

## Coordinate-triggered routines

The console builds a saved sequence of up to 500 ordered coordinates, each
with a radius and a saved routine name. **Start sequence — auto-arm** explicitly
authorizes automatic arming at each next point. It requires a saved plan, fresh
aircraft position, healthy interlock and idle output. Only the next point can
trigger; routines complete and outputs disarm before advancing. Overlapping
points can trigger consecutively. Startup restores the plan, never an enabled
sequence, fix, routine progress or armed output.

Use receive-only MAVLink UDP on port 14550 or a custom `POST /api/position` feed.
The custom feed bypasses the bounded command queue through one latest-sample
slot. Invalid custom fixes revoke the previous fix. Position age above three
seconds stops the sequence, including initial delays and active pulses; the GPIO
worker enforces the inhibit independently of the web/command loop. Known position
accuracy larger than the next radius prevents a new trigger. All stop/disarm
controls cancel the sequence and prevent later repeats from restarting output.
See [COORDINATE_ROUTINES.md](docs/COORDINATE_ROUTINES.md) for setup and provider
contracts, including the shared ESP32 bridge script.

The Pi expands the current ESP32's 256-point capacity to 500. Paste one comma-separated
point per line in the editor; HTTP saves use batches of eight with progress and
bounded retry of busy admission. Plans use a separately bounded 256 KiB file.
Stream `@STATE` keeps summary status small; `@GEO:offset` reads pages of sixteen
points. The BLE console fetches these pages when loading the saved editor plan.
**Test GPS through MAVLink** sends the manually entered coordinate through the
Pi's real local UDP receiver. Its one-second repeat feed supports long bench
tests and stops with stop/disarm, disconnect, source changes, navigation or update.
Use finite routines to advance a plan automatically: a continuous point routine
holds that point while fixes remain fresh, until stopped.

## Updates and recovery

Build a bundle from reviewed source on any development computer:

```sh
python tools/build_update.py --output pi-controller-update.zip
```

Open **Application updates**, select the zip, enter the installation token, and
upload over HTTP. The private token stays in the password field and request header;
it is cleared after success. Never share it in command logs or public URLs.
Updates accept at most 8 MiB; the manifest lists every permitted application/web
file and its SHA-256. Traversal, symlinks, extra paths, checksum mismatches and
invalid Python syntax are rejected. A hash is an integrity check, not a signature:
install only trusted bundles.

Outputs stop before staging. A root helper revalidates the bundle, installs into
a root-owned release directory and atomically switches `current`. It restarts the
service and requires a healthy response from that exact release within 35 seconds.
Failure rolls back. A separate 90-second watchdog and boot recovery service restore
an uncommitted update after interruption/power loss. An applying candidate blocks
actuation until commit. `/api/update/status` reports the result; the page reconnects
automatically. Simulation validates/stages but does not install/restart.

Application bundles cannot modify root helpers, OS files, dependencies or service
units. For those updates rerun `sudo sh deploy/install.sh` from reviewed source.
Use Pi OS package management for OS updates. Old release directories are retained
for recovery; remove unneeded releases manually after verifying the current one.

## HTTP API, bounds and routing

| Endpoint | Purpose |
| --- | --- |
| GET /api/ping | Health/version/release ID, always accessible |
| GET /api/state | Full redacted state and bounded libraries |
| GET /api/events?since=0&limit=16 | Command results/event cursor |
| POST /api/command | UTF-8 text/plain or octet-stream commands |
| POST /api/ota | Pi zip, X-Update-Token; optional X-Update-SHA256 |
| GET /api/update/status | Update progress/result |

Command 202 means queued, not completed. Inspect result events. Batches have 1–8
lines, each at most 256 UTF-8 bytes, body at most 2048 bytes. Busy admission returns
503. `X-Request-ID` deduplicates for 30 seconds with 64 retained IDs; another body
using the same ID returns 409. IDs do not survive restart. Command reads have a
10-second deadline; uploads have a 60-second deadline.

StopAll, Disarm, DispenseStop, RoutineStop, GeoStop, and advanced stop/OFF commands immediately
clear the queue and make outputs inactive. A stop batch executes only the stop.
HTTP stops bypass the browser's pending send chain. BLE/serial have no request IDs;
never blindly retry an actuation command.

Events retain 128 records; cursor reset/wrap is reported with `gap`. Stream output
queues have 32 frames and drop excess frames instead of blocking control. Replies
go to the originating stream and local serial outputs; diagnostic events are
available to HTTP. `Send` targets other available destinations; `Send*` targets
one explicitly. Wi-Fi is available after a poll within 15 seconds; BLE needs a
TX subscription; SPP/serial need a live connection. An active radio profile can
disable HTTP commands while leaving assets/health reachable.

## Local development and validation

```sh
python -m venv .venv
# Activate .venv using your shell.
python -m pip install -r requirements.txt
python -m bridge --simulate
python -m unittest discover -s tests -v
node --check web/app.js
sh -n deploy/install.sh
```

Without a config file the simulator binds only 127.0.0.1:8080. Simulation includes
virtual radio registration, motor, DAC and LEDs without BlueZ/GPIO/OS changes.
Real mode requires Linux and the configured hardware. CI runs the software tests
on Linux, including the SPP Unix socket check skipped on Windows.

Coverage includes deadline/interlock behavior, startup polarity, stop barriers,
retry handling, profiles/routines, motor steps/DAC encoding, radio rollback,
self-test checkpoints, 32-bit timing, initial delays, continuous routine persistence
and stop handling, coordinate ordering and 500-point plans,
MAVLink CRC/truncation/replay checks, test GPS encoding and real local UDP, position API admission,
stale-position watchdog inhibition,
malformed updates, health rollback and boot recovery.
Physical GPIO timing, RF coexistence, USB enumeration and actual installation
still require a bench check on each Pi/OS image.

## Platform references

- [Raspberry Pi OS configuration and UARTs](https://www.raspberrypi.com/documentation/computers/configuration.html)
- [BlueZ GATT API](https://bluez.readthedocs.io/en/latest/gatt-api/)
- [BlueZ advertising API](https://bluez.readthedocs.io/en/latest/advertising-api/)
- [BlueZ Classic Bluetooth profile API](https://bluez.readthedocs.io/en/latest/profile-api/)
- [NetworkManager nmcli](https://networkmanager.dev/docs/api/latest/nmcli.html)
- [NetworkManager keyfile format](https://networkmanager.dev/docs/api/latest/nm-settings-keyfile.html)
- [Microchip MCP4725 data sheet](https://ww1.microchip.com/downloads/en/DeviceDoc/MCP4725-Data-Sheet-20002039E.pdf)
