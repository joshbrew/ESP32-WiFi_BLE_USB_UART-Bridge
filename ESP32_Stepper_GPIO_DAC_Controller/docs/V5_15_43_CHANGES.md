# v5.15.43 status LED GPIO update

- Assigned the connection indicator to GPIO23.
- Assigned the activity indicator to GPIO5.
- Kept status LEDs enabled by default and independently removable at compile time.
- Added `src/hardware/README.md` as the direct map to LED configuration and routines.
- Preserved the v5.15.42 OTA implementation and v5.15.41 coexistence behavior.
