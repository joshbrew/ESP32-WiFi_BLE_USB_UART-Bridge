# Status LEDs

All status LED behavior is isolated in this folder.

- `StatusIndicators.h` declares the LED interface.
- `StatusIndicators.cpp` owns pin setup, transport/connection indication, activity pulses, and the boot/self-test routines.
- `../config/AppConfig.h` owns the two GPIO assignments, polarity, timings, and default-on build switches.

Current assignments:

| Function | GPIO |
|---|---:|
| Connection / transport state | 23 |
| Command / operation activity | 5 |

To release both pins completely, comment out `APP_ENABLE_STATUS_INDICATORS` in `AppConfig.h`. No `pinMode()` or `digitalWrite()` calls are made for these LEDs in that build.

To retain runtime indication but skip startup blinking, leave status indicators enabled and comment out `APP_ENABLE_STATUS_LED_BOOT_TEST`.
