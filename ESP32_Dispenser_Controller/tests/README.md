# Regression checks

These checks use mocked hardware and local HTTP; they do not operate a dispenser.

## Actual C++ engines on a host

`host/engine_tests.cpp` compiles the production RoutineEngine, GeoMission, and
TextUtil modules with the real AppConfig and small Arduino/NVS/time/network
stubs. It covers initial delay once, repeats, persistence, cancellation, a full
hour pulse, optional active-profile/arm limits, 300 actual pulses, 32-bit repeat
counts and timed steps, timer rollover, no total-runtime cap, migration of
version-2 saved routines and corrupt-record rejection, 256 GPS points saved,
reloaded, and executed in order, legacy 12-point plan migration, ordered coordinates,
accuracy, fresh/invalid/stale/queued fixes, and a single-slot position mailbox.
Continuous routines are checked for persistence, an initial delay once,
repeated on/off cycling, and stopping during the delay, pulse, and gap.
It also checks the production MAVLink 1/2 parser against frames produced using
an independent bitwise CRC, including truncated/invalid/signed packets and
MAVLink 2 zero-trimmed payloads.
Manual GPS tests send actual generated frames through a mocked UDP socket into
the production receiver. They independently check CRCs and fields, AP/client
operation, outside/inside-radius triggering, a refreshed four-second delay,
stale test-feed shutdown, and invalid position rejection.
Bluetooth tests use the production byte-stream receiver: one-byte and 20-byte
fragments, combined frames, maximum signed frames, noise, bad CRC, incomplete
frame timeout, a mission without Wi-Fi, saved BLE source, refreshed initial
delay, disconnect/stale shutdown, invalid fix, and old data after switching sources.

`host/dispenser_tests.cpp` compiles the actual DispenserAddon with RoutineEngine
and mocked GPIOs. It checks eight outputs in order, pulse/gap timing, no overlap,
initial delay only once, continuous mode, stops during delay/pulse/gap, delayed
command dispatch and never-started output failure, rollover, optional limits,
invalid/duplicate/reserved pins, active-high/low behavior, released removed pins,
standalone and named-profile persistence, and old single-pin profile migration.
Compile it using the same command below, replacing GeoMission.cpp with
src/addons/dispenser/DispenserAddon.cpp and engine_tests.cpp with dispenser_tests.cpp.

From the repository root with a C++17 compiler:

```sh
c++ -std=c++17 -IESP32_Dispenser_Controller/tests/host \
  -include ESP32_Dispenser_Controller/tests/host/TestPlatform.h \
  ESP32_Dispenser_Controller/src/core/RoutineEngine.cpp \
  ESP32_Dispenser_Controller/src/core/GeoMission.cpp \
  ESP32_Dispenser_Controller/src/util/TextUtil.cpp \
  ESP32_Dispenser_Controller/tests/host/engine_tests.cpp \
  -o /tmp/dispenser-engine-tests
/tmp/dispenser-engine-tests
```

MSVC equivalents are `/std:c++17 /EHsc /I... /FI...` after loading the compiler
environment. Put compiler outputs outside the sketch directory.

## Console and bridge

With Node.js, Playwright, and Chrome installed:
Run `npm ci` in `ESP32_Dispenser_Controller` first to install the pinned
build-only JavaScript minifier. Existing generated firmware assets require no
Node packages when compiling or uploading from Arduino IDE.

```sh
node ESP32_Dispenser_Controller/tests/console.spec.cjs
python -m unittest discover -s ESP32_Dispenser_Controller/tests -p 'test_*.py'
node ESP32_Dispenser_Controller/web/build_web_assets.mjs --check
```

The browser test starts a local mock controller. It checks saved buttons,
uncapped input, larger repeat counts, initial-delay commands, coordinate batching/validation,
network setup/credential redaction, stop during a held HTTP request, cancellation
of pending commands, serialized BLE writes, mobile width, and the actual minified
gzip page embedded in firmware. Set
`CONSOLE_SCREENSHOT` to capture the console. The Python tests check measurement
age, invalid fixes, and common MAVLink packet fields.
Bluetooth browser checks discover the dedicated GPS characteristic, verify raw
packet fields and CRCs independently, stream phone GPS, reject cached/old phone
measurements, cancel on Stop, and exercise the production minified BLE GPS path.
Python checks validate acknowledged BLE chunking and stale-measurement rejection
without requiring a Bluetooth device or installing Bleak for tests.
The browser also checks adding/removing pin rows, per-output time and delay,
saving pins, duplicate rejection, default single mode, and batching eight-output
routines within the command queue limit.

Firmware compilation was checked with ESP32 Arduino core 3.3.10,
`esp32:esp32:lolin32`, `PartitionScheme=min_spiffs`, maintained Async TCP 3.5.0,
and ESP Async WebServer 3.12.1. The standalone LR relay also uses ArduinoJson 7.4.2.
Radio range, GPIO switching, interlocks, and real flight-controller telemetry
still require bench tests on the actual hardware.
