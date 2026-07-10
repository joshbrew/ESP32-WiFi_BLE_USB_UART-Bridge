# Status indicators

All LED ownership is isolated in this folder.

- `StatusIndicators.h` declares the interface.
- `StatusIndicators.cpp` owns `pinMode()`, `digitalWrite()`, runtime state, activity pulses, and LED self-tests.
- `../config/AppConfig.h` owns the GPIO assignments, active polarity, timings, and build switches.

Current assignments:

| Function | GPIO |
|---|---:|
| Connection / transport state | 23 |
| Command / operation activity | 5 |

The default wiring is active-high: `GPIO -> resistor -> LED anode`, with the LED cathode connected to ESP32 ground. Each LED needs its own resistor. GPIO5 may also drive an onboard active-low LED on some LOLIN32 variants, so the onboard indication can appear inverted relative to an external active-high LED.

Comment out `APP_ENABLE_STATUS_INDICATORS` in `AppConfig.h` to release both GPIO pins completely. In that build, the firmware makes no LED `pinMode()` or `digitalWrite()` calls.

Comment out only `APP_ENABLE_STATUS_LED_BOOT_TEST` to retain runtime indication while skipping startup pulses.
