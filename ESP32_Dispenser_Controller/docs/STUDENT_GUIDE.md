# Student guide: first dispenser lab

This lab introduces the application in the same order as a normal Arduino
sketch: configuration, `setup()`, repeated service, commands, then automation.

## 1. Find the approachable layer

Open `ESP32_Stepper_GPIO_DAC_Controller.ino`.

The global objects are the subsystems. `setup()` starts them in a deliberate
order. `loop()` either provides the fallback service call or sleeps while the
FreeRTOS control task performs that service.

Next open the `QuickConfig.h` tab beside the sketch. It selects the hardware
profile and groups the payload pin, polarity, timing bounds, optional interlock,
status LEDs, access-point identity, and advanced bench pinout. Each setting has
a plain-language comment. Leave profile `1` selected for the aircraft.

`src/config/AppConfig.h` contains safe fallbacks, feature switches, queue sizes,
and expert-only limits. You should not need it for the first lab.

Do not begin by editing the radio or web implementation.

## 2. Understand the output

The ESP32 does not switch applicator power. GPIO26 controls an external analog
switch. The firmware represents that physical function as a `DispenserAddon`,
not as a generic pin.

That naming gives the code meaningful rules:

- a dispenser must be armed;
- an activation must have a duration;
- reset and faults must produce the inactive state;
- active state must never be restored from memory.

## 3. Ground test with Serial Monitor

With the applicator disconnected or unloaded, send:

```text
Ping
DispenserStatus
Dispense:100
```

The last command should be rejected because the controller is disarmed.

Now send:

```text
Arm
Dispense:100
DispenserStatus
Disarm
```

Measure the GPIO pulse with an LED, logic probe, or oscilloscope before
connecting the final trigger circuit.

## 4. Follow one command

For `Dispense:100`, trace these files:

1. `CommandDispatcher.cpp` accepts a bounded line into the shared queue.
2. `CommandRouter.cpp` delegates hardware commands to the selected addon.
3. `DispenserAddon.cpp` validates the arm, interlock, and duration.
4. `DispenserAddon::service()` ends the pulse without blocking other work.
5. `EventBus.cpp` records the result for USB, Wi-Fi, or BLE output.

`StopAll` takes a special path through the dispatcher: it replaces older queued
commands with a stop barrier, but still executes on the same control task as the
normal state machine. This avoids racing an asynchronous Wi-Fi or BLE callback
against the GPIO logic.

The runtime payload and routine controllers do not use blocking `delay()` calls.
Short delays are confined to boot indication and imminent-reboot paths. That is
why HTTP, BLE, safety checks, and status updates remain responsive during a
pulse or routine.

## 5. Build a routine

Use the web form or send this batch:

```text
RoutineCreate:lab
RoutineAdd:lab:DISPENSE:100
RoutineAdd:lab:WAIT_IDLE
RoutineAdd:lab:WAIT:900
RoutineRepeat:lab:3
RoutineSave:lab
```

Then:

```text
Arm
RoutineRun:lab
```

Interrupt it with `StopAll` and confirm the output becomes inactive.

## 6. Save two payload profiles

Profiles are a fixed four-slot library, not dynamically allocated plugins. With
the applicator unloaded and the dispenser disarmed, send:

```text
DispenserDefaultPulse:100
DispenserMaxPulse:500
DispenserArmTimeout:20000
PayloadProfileSave:small-dot
DispenserDefaultPulse:300
DispenserMaxPulse:1000
DispenserArmTimeout:30000
PayloadProfileSave:large-dot
PayloadProfileList
PayloadProfileUse:small-dot
```

Confirm `DispenserStatus` reports the selected name and limits. A trailing `*`
on the profile name means the current values were changed but not saved back to
that slot. Profile selection never arms or activates the output.

## 7. Suggested exercises

1. Change `DRONE_CFG_DISPENSER_DEFAULT_PULSE_MS` in `QuickConfig.h`, rebuild,
   and observe the legacy
   `GPIO26:ON` compatibility pulse.
2. Add a new fixed pulse button to `web/index.html` and `web/app.js`.
3. Add an unused external interlock input and verify that closing it faults and
   disarms the controller.
4. Add a new read-only status value to `DispenserAddon::appendStateJson()` and
   display it in the browser.
5. Explain why a routine may contain `Dispense:100` but not `Arm` or `Reboot`.

## 8. Before using actual payload hardware

- Restore production boot mode.
- Confirm the output is inactive through power-up and reset.
- Confirm the external circuit has its own inactive bias.
- Confirm `StopAll`, arm timeout, routine timeout, and interlock behaviour.
- Test with the final battery, radio traffic, wiring, and load while grounded.
