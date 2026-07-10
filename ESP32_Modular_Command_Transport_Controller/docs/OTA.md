# HTTP OTA

The browser uploads an ESP32 application image as `multipart/form-data` to `/api/ota` and supplies its exact length through `X-Firmware-Size`.

Firmware validates:

```text
.bin filename
Minimum size
Inactive application partition capacity
ESP32 image magic byte 0xE9
Strict sequential chunk offsets
No write beyond the declared image size
Exact final received length
Update.end(false) success
```

A client disconnect aborts the in-progress `Update`. The reboot timer starts only after the HTTP success response has been queued.

Use an Arduino partition layout with two application slots. `Huge APP` cannot support this updater.

BLE can initiate the workflow, but the browser disconnects BLE and uploads the binary over Wi-Fi. This avoids putting a second firmware-transfer protocol into coexistence RAM.
