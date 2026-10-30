# Status LEDs

All status LED behavior is isolated in this folder.

- `StatusIndicators.h` declares the LED interface.
- `StatusIndicators.cpp` owns pin setup, transport/connection indication, activity pulses, and the boot/self-test routines.
- Root-level `QuickConfig.h` selects the board, RGB pin/brightness, and discrete LED pins/polarity.
- `../config/AppConfig.h` owns indicator timings and default-on build switches.

S3 N16R8 uses one onboard addressable RGB LED on GPIO48 (the supplied diagram).
Set `DRONE_CFG_STATUS_RGB_PIN` to 38 for an Espressif v1.1 board. The Arduino
core's `rgbLedWrite()` drives it; no external LED library is needed. The two
discrete LED pins are unused on S3 and are reported as `-1`. Status text/JSON
reports the RGB backend and its pin.

- Green: solid connected, blinking while seeking a connection.
- Blue: command/activity pulse; takes precedence over green.
- Amber: operation active; takes precedence over command pulses.
- Off: no connection/activity or after self-test completion.

`IndicatorTest` gives two green then three blue pulses. Individual connection
and activity tests give four pulses of their respective color. The short
production boot test displays green then blue. Default brightness is 24/255.
RGB frames are sent only when the visible color changes (or on initialization).

To disable LED ownership, comment out `APP_ENABLE_STATUS_INDICATORS` in
`AppConfig.h`. That build makes no discrete GPIO or RGB-driver calls. On S3,
both possible onboard LED pins remain excluded from the payload allowlist.

To retain runtime indication but skip startup blinking, leave status indicators enabled and comment out `APP_ENABLE_STATUS_LED_BOOT_TEST`.
