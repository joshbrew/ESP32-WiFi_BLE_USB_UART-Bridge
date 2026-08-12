# Drone Gel Dispenser Controller

ESP32 firmware for a drone-mounted caulk-gel dispenser. The normal build uses
GPIO26 to control an external analog switch connected to the applicator trigger.
It provides a simple student-facing Arduino sketch while keeping the more
advanced FreeRTOS, Wi-Fi, BLE, command, routine, logging, and OTA systems in
separate modules.

The optional stepper and DAC hardware from the original project is preserved as
an advanced alternate build profile. It is compiled out of the normal aircraft
firmware.

## Start here

1. Read [docs/STUDENT_GUIDE.md](docs/STUDENT_GUIDE.md).
2. Confirm the external switch is held inactive while the ESP32 is reset.
3. Open `ESP32_Stepper_GPIO_DAC_Controller.ino` in Arduino IDE.
4. Open the neighboring `QuickConfig.h` tab and confirm the pinout.
5. Install the maintained ESP32Async `AsyncTCP` and `ESPAsyncWebServer` libraries.
6. Select `WEMOS LOLIN32` board and `Minimal SPIFFS (Large APPS with OTA)` partition scheme.
7. Upload with the dispenser disconnected or unloaded for the first test.
8. Open Serial Monitor at 115200 baud and send `Status`.

The browser console is available from the default access point:

```text
SSID: Drone-Gel-Controller
Password: password
URL: http://192.168.4.1/
```

## Normal operating sequence

```text
DispenserStatus
Arm
Dispense:250
Disarm
```

`Arm` does not activate GPIO26. It opens a temporary 60-second window during
which a bounded dispense pulse may be requested. Every reset, `StopAll`, routine
completion, fault, or arm timeout leaves the output inactive and disarmed.
The requested pulse must fit entirely inside the remaining arm window; otherwise
the controller asks for a fresh `Arm` instead of starting a truncated pulse.

`GPIO26:ON` remains as a compatibility alias, but it is converted to the default
bounded pulse instead of producing an indefinite HIGH state.

## Hardware connection

```text
ESP32 GPIO26  -> external analog-switch enable/control input
ESP32 GND     -> control-circuit ground
```

GPIO26 is only a 3.3 V logic control signal. It must not receive the switched
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
- Dispense duration must fit the active profile maximum, which can never exceed
  the compiled `DISPENSER_MAX_PULSE_MS` ceiling.
- The default compiled maximum pulse is 2000 ms.
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
future boots. Runtime limits may be smaller than the `QuickConfig.h` ceilings,
never larger. Profiles do not contain `Arm`, output state, or routines.

## Saved timing routines

Routines run without blocking `loop()` or the network task. They are fixed-size,
checksummed records in ESP32 NVS and are only written by `RoutineSave`.

Create a six-dot pattern:

```text
RoutineCreate:dots
RoutineAdd:dots:DISPENSE:200
RoutineAdd:dots:WAIT_IDLE
RoutineAdd:dots:WAIT:800
RoutineRepeat:dots:6
RoutineSave:dots
```

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

## Student settings

`QuickConfig.h` sits directly beside the `.ino` and appears as another Arduino
IDE tab. It contains the settings students are expected to change:

```cpp
#define DRONE_CFG_HARDWARE_PROFILE 1
#define DRONE_CFG_DISPENSER_PIN 26
#define DRONE_CFG_DISPENSER_ACTIVE_HIGH 1
#define DRONE_CFG_DISPENSER_DEFAULT_PULSE_MS 250UL
#define DRONE_CFG_DISPENSER_MAX_PULSE_MS 2000UL
#define DRONE_CFG_DISPENSER_ARM_TIMEOUT_MS 60000UL
#define DRONE_CFG_INTERLOCK_PIN -1
```

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

### Advanced bench profile

```cpp
#define DRONE_CFG_HARDWARE_PROFILE 2
```

Restores the original 28BYJ-48 stepper, GPIO25 DAC, and GPIO26 compatibility
output package. The two profiles are mutually exclusive because both may own
GPIO26. Stepper and DAC boot tests remain disabled unless explicitly enabled in
`AppConfig.h`.

## Source map

```text
ESP32_Stepper_GPIO_DAC_Controller.ino   Small Arduino entrypoint
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
docs/STUDENT_GUIDE.md                   Guided first lab
```

The `.ino` intentionally stays small. Students can follow normal Arduino
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
