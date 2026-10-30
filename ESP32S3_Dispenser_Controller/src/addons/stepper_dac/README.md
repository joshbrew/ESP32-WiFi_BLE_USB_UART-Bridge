# Stepper + DAC addon

This folder is the optional advanced bench profile. The normal aircraft build
uses `src/addons/dispenser/` and compiles this implementation out.

- `StepperDacAddon` implements the `DeviceAddon` contract used by the base.
- `StepperController` owns the motor task, queue, motion profile, and coil safety.
- `DacController` owns DAC1 on GPIO25 plus the digital HIGH/LOW output on GPIO26, including timers and release behavior.

The base command/transport framework should not include these controller headers
outside the selected-addon branch in the main `.ino`.

To select this profile, set `DRONE_CFG_HARDWARE_PROFILE` to `2` in the root-level
`QuickConfig.h` tab. The stepper, analog DAC, and digital output pin overrides
are grouped directly below it. Use this profile only on compatible bench hardware.
