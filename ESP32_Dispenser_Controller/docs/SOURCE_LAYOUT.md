# Source layout

```text
ESP32_Stepper_GPIO_DAC_Controller/
|-- ESP32_Stepper_GPIO_DAC_Controller.ino  Arduino setup/loop entrypoint
|-- QuickConfig.h                          Beginner settings; Arduino IDE tab
|-- README.md                              Project and safety overview
|-- COMMANDS.md                            Command reference
|-- build_opt.h                            AsyncTCP task build options
|-- src/
|   |-- config/AppConfig.h                 Advanced switches, fallbacks, bounds
|   |-- addons/DeviceAddon.h               Hardware-profile interface
|   |-- addons/dispenser/                  Default aircraft payload profile
|   |-- addons/stepper_dac/                Optional advanced bench profile
|   |-- core/RoutineEngine.*               Saved nonblocking timing routines
|   |-- core/                               Commands, events, self-test, runtime
|   |-- radio/                              Wi-Fi/BLE profiles and rollback
|   |-- transports/                         USB, BLE, SPP, UART routing
|   |-- hardware/                           Status indicators
|   `-- web/                                HTTP, OTA, embedded portal bytes
|-- web/                                    Editable browser sources
`-- docs/                                   Student and advanced notes
```

Students should normally read `QuickConfig.h`, the `.ino`, `DispenserAddon.*`,
and `RoutineEngine.*` in that order. `AppConfig.h`, transport, and radio modules
are kept out of the introductory path.
