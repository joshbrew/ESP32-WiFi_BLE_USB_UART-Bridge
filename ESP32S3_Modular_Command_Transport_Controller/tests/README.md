# S3 host checks

These tests compile the actual indicator controller with deterministic GPIO,
RGB, serial, and time stubs. No controller or attached hardware is operated.
They cover reserved N16R8 pins, no discrete GPIO ownership, color priority,
cached RGB frames, command expiry, timer rollover, production/debug boot tests,
and manual self-test completion.

From this sketch folder, with a C++17 compiler:

```sh
c++ -std=c++17 -Itests/host src/hardware/StatusIndicators.cpp tests/host/indicator_tests.cpp -o /tmp/s3-indicator-tests
/tmp/s3-indicator-tests
```

Repeat with `-DAPP_STATUS_RGB_PIN=38` for the v1.1 LED revision. MSVC equivalents
are `/std:c++17 /EHsc /Itests\host`, with `/Fe` and `/Fo` directed outside the
sketch. If indicators are disabled in AppConfig, the same test verifies no GPIO
or RGB-driver calls occur.

The project-wide source/asset checks are `python tools/project_tool.py validate`.
Run `npm ci` then `python tools/project_tool.py all` after editing web assets.
The reference firmware build uses Arduino ESP32 core 3.3.10, ESP32S3 Dev Module,
16 MB QIO flash, OPI PSRAM, and `app3M_fat9M_16MB` partitions.

Real USB enumeration, RGB hardware, Wi-Fi/BLE coexistence, subscriptions,
radio-profile recovery, and OTA still need bench checks on the actual board.
