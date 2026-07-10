# Stepper + DAC addon

This folder is the complete hardware-specific feature pack for the current
controller.

- `StepperDacAddon` implements the `DeviceAddon` contract used by the base.
- `StepperController` owns the motor task, queue, motion profile, and coil safety.
- `DacController` owns both hardware DAC channels, timers, and release behavior.

The base command/transport framework should not include these controller headers
outside the selected-addon branch in the main `.ino`.

To build the transport-only base, comment out `APP_ADDON_STEPPER_DAC` in `src/config/AppConfig.h`.
