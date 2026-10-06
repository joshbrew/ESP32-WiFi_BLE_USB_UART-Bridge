## ESP32 WiFi-BLE-USB-UART Bridge

This repo contains two Arduino projects and a Raspberry Pi Linux port. This is a full suite so you only need to add your own sensor and controller programs on.

#### [ESP32_Modular_Command_Transport_Controller/](https://github.com/joshbrew/ESP32-WiFi_BLE_USB_UART-Bridge/tree/main/ESP32_Modular_Command_Transport_Controller)

Is just the transport layer containing all communication methods and the unified command interface.

#### [ESP32_Dispenser_Controller](https://github.com/joshbrew/ESP32-WiFi_BLE_USB_UART-Bridge/tree/main/ESP32_Dispenser_Controller)

Contains GPIO, DAC, and Stepper commands in a configurable suite.

#### [RaspberryPi_Controller/](RaspberryPi_Controller/README.md)

Raspberry Pi Zero W / Zero 2 W version with a Python service, web console,
Wi-Fi HTTP, BLE UART, Classic Bluetooth serial, USB serial and GPIO UART, plus
timed dispenser controls, profiles, routines with initial delays and 32-bit
timing/repeats, coordinate-triggered sequences using MAVLink/custom position,
optional stepper/external DAC,
radio management, indicators, resumable self-tests and browser application updates
with rollback. Includes simulation, tests, and a Raspberry Pi OS installer. See
its guide for hardware setup and the remaining Pi-specific differences.

#### ESP32 long-range Wi-Fi

Both ESP32 projects include optional LR-only Wi-Fi (`WiFiLR:ON|OFF`, default
OFF). See the [LR setup and Bluetooth relay guide](docs/WIFI_LR.md) for a
phone-to-BLE-to-LR topology and a second ESP32 relay sketch.

#### About

This is based on my prior work with the ESP32 but optimizes a ton, leaving about 33KB of program memory available when including both WiFi and BLE stacks and they can be run simultaneously using the dual core system. Simple defines can exclude different radio code leaving that memory free for other things, if you don't need the full stack. This is a shell for adding on sensors and controls when we want to use the ESP32's radios, and has extensive controls e.g. for power usage and NVS-saved radio configurations (which radios to enable, credentials, names, etc).


This was tested on a cheap ESP-WROOM-32 (WEMOS Lolin32 variant) without PSRAM which can significantly improve throughputs when sharing the radio between both protocols.


License: MIT
