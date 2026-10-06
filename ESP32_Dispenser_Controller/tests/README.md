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
It also checks the production MAVLink 1/2 parser against frames produced using
an independent bitwise CRC, including truncated/invalid/signed packets and
MAVLink 2 zero-trimmed payloads.

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

Firmware compilation was checked with ESP32 Arduino core 3.3.10,
`esp32:esp32:lolin32`, `PartitionScheme=min_spiffs`, maintained Async TCP 3.5.0,
and ESP Async WebServer 3.12.1. The standalone LR relay also uses ArduinoJson 7.4.2.
Radio range, GPIO switching, interlocks, and real flight-controller telemetry
still require bench tests on the actual hardware.
