# S3 RGB status indicator

The base firmware owns only the onboard addressable LED on GPIO48.
`APP_STATUS_RGB_PIN` in `../config/AppConfig.h` may be set to 38 for v1.1 boards.
`APP_STATUS_RGB_BRIGHTNESS` is 1..255, default 24.

- Green: connected; blinking while seeking.
- Blue: command/activity pulse, ahead of connection indication.
- Amber: active addon/queued work, ahead of command pulses.
- Off: no connection/activity, or after a self-test completes.

`IndicatorTest` flashes green twice then blue three times; the connection and
activity tests flash their color four times. The production boot test briefly
shows green then blue. The Arduino core supplies `rgbLedWrite()`; no extra LED
library is needed. Frames are cached between visible changes.

Comment out `APP_ENABLE_STATUS_INDICATORS` to compile out LED GPIO and RGB
driver calls. Comment out only `APP_ENABLE_STATUS_LED_BOOT_TEST` to keep runtime
indication without startup animation. No external LED pins are configured;
legacy discrete pin fields are `-1`, with RGB pin/type reported separately.
