# v5.15.39 changes

Historical only. Current builds use the v5.15.40 `AppConfig.h` layout.

Superseded by v5.15.40, which removes the `.ino` profile import entirely.

- Fixed the accidental block-comment terminator caused by the old `src` glob text.
- Removed `ControllerConfig.h`.
- Moved the complete user-editable build profile back to the top of the Arduino `.ino`.
- Kept normal independently compiled `.h` and `.cpp` modules under grouped `src/` folders.
- Added a profile-only preprocessor guard so `AppConfig.h` can propagate the `.ino` switches to each `.cpp` translation unit without including the sketch body.
- Wrapped the stepper/DAC implementation files in the addon build switch so a transport-only profile does not compile their implementation bodies.
- Updated source validation to reject accidental comment terminators in comment text.
- Preprocessed all 16 addon/Wi-Fi/BLE/SPP profile combinations successfully with host stubs.
