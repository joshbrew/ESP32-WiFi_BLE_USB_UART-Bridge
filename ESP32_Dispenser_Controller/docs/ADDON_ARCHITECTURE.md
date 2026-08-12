# Hardware addon architecture

The command, event, transport, radio, web, OTA, routine, and self-test layers are
the reusable firmware base. One board-specific hardware profile is selected in
the root-level `QuickConfig.h` tab beside the Arduino sketch.

Normal aircraft selection:

```cpp
#define DRONE_CFG_HARDWARE_PROFILE 1
```

Advanced bench selection:

```cpp
#define DRONE_CFG_HARDWARE_PROFILE 2
```

Exactly one profile is compiled because both profiles can own GPIO26. Advanced
build systems may still define one `APP_ADDON_*` macro directly; that direct
selection takes precedence over the quick profile number.

## Dispenser profile

```text
src/addons/dispenser/
|-- DispenserAddon.cpp
`-- DispenserAddon.h
```

`DispenserAddon` owns the payload state machine, GPIO safety, arming window,
bounded pulses, optional external interlock, and validated pin configuration.
Its `canStartRoutine()` hook prevents automation from bypassing arming, while
`blocksExternalCommandDuringRoutine()` prevents a manual start command from
overlapping timing owned by an active routine.

## Stepper/DAC profile

```text
src/addons/stepper_dac/
|-- StepperDacAddon.cpp/.h
|-- StepperController.cpp/.h
|-- DacController.cpp/.h
`-- README.md
```

This is preserved for supervised advanced hardware work and is compiled out of
the normal payload build.

## Add another hardware profile

Derive from `DeviceAddon`. A small sensor profile normally implements:

```cpp
class SensorNodeAddon : public DeviceAddon {
 public:
  const char *name() const override { return "sensor-node"; }
  void begin() override;
  void service() override;
  bool handleCommand(
    const String &command,
    CommandSource source,
    const String &requestId
  ) override;
};
```

Override the safe-stop, busy, active-output, state JSON, or routine-readiness
hooks whenever the hardware can move or activate an external device. Add a
build switch, include the header in the `.ino`, construct one `deviceAddon`, and
add a mutual-exclusion check for shared pins.

## Command flow

1. A transport submits a complete line to `CommandDispatcher`.
2. `CommandRouter` handles global commands.
3. Routine-editing commands are offered to `RoutineEngine`.
4. Hardware commands are offered to the selected `DeviceAddon`.
5. Saved routine actions re-enter through `CommandDispatcher`, preserving the
   same validation and event path as a human command.
6. Results are published to `EventBus` and routed back to the command source and
   wired recovery transports.

`StopAll` is admitted as a queue barrier, discards older pending work, and then
stops the routine. The routine stop invokes the addon's safe-stop hook, so
completion, failure, operator stop, OTA, and managed radio reboot converge on
the same serialized hardware-safe path.
