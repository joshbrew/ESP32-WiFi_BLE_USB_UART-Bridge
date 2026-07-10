# Source layout

The sketch uses normal Arduino C++ modules. Arduino compiles the `.ino` and every
`.cpp` below `src/` as separate translation units.

```text
ESP32_Stepper_Dual_DAC_RTOS_Controller/
├── ESP32_Stepper_Dual_DAC_RTOS_Controller.ino
├── build_opt.h
├── src/
│   ├── addons/
│   │   ├── DeviceAddon.h
│   │   └── stepper_dac/
│   │       ├── StepperDacAddon.cpp/.h
│   │       ├── StepperController.cpp/.h
│   │       └── DacController.cpp/.h
│   ├── config/AppConfig.h
│   ├── core/
│   ├── hardware/
│   │   ├── README.md
│   │   └── StatusIndicators.cpp/.h
│   ├── radio/
│   ├── transports/
│   ├── util/
│   └── web/
├── web/
├── tools/
└── docs/
```

## Configuration and translation units

Feature `#define` switches live in `src/config/AppConfig.h`. The `.ino` and the
normal `.cpp` modules include that header in the usual direction. No header or
implementation file includes the Arduino sketch.

Older versions used `.inc` implementation fragments or imported a guarded part
of the `.ino` to share feature macros. Both patterns are gone. Arduino now
compiles each `.cpp` independently, like a normal C++ project.

## Ownership rules

- The `.ino` creates the selected objects and runs `setup()` and `loop()`.
- `core/` owns command dispatch, routing, events, self-test, and runtime service.
- `transports/` owns USB, UART, BLE, SPP, and deliberate `Send*` routing.
- `radio/` owns boot profiles, NVS radio configuration, and rollback.
- `web/` owns HTTP admission, multipart OTA, BLE-to-Wi-Fi OTA handoff, and generated portal assets.
- `hardware/StatusIndicators.*` owns optional connection/activity GPIO behavior and its isolated boot-test routine.
- `addons/` contains board-specific hardware. The base framework does not know
  stepper or DAC implementation details.

Keep dependencies pointed inward through public headers. A `.cpp` includes its
matching header first, and headers use explicit include guards.
