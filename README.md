## ESP32 WiFi-BLE-USB-UART Bridge

For guided MQTT/TLS hosting and device setup, double-click
[Setup Field Access.cmd](Setup%20Field%20Access.cmd). It installs missing server
dependencies (including Docker/Compose), creates matching credentials/settings
and offers automated modem certificate/clock provisioning and confirmed flashing.
See [the guided setup](docs/FIELD_SETUP.md) for the few account/wiring prerequisites.

Use [controller.cmd](controller.cmd) for `setup`, `ports`, `doctor`, `compile` and
`flash`. Flashing reuses existing binaries, detects USB COM candidates and asks
you to confirm the board/port before writing. Compile can install missing CLI
tools and Arduino packages automatically. All four ESP32 variants are supported;
see [the command-line guide](docs/CONTROLLER_CLI.md).
The [Flash Firmware.cmd](Flash%20Firmware.cmd) double-click launcher uses the same
flash command.

All five controller variants share the compact console/preset layout; dispenser
controls lead their side panel. The two ESP32-S3 variants now include an opt-in
MQTT/TLS LTE transport, with an [HTTPS field-login relay](ESP32S3_Modular_Command_Transport_Controller/relay/README.md)
and [SARA-R4 wiring/provisioning guide](ESP32S3_Modular_Command_Transport_Controller/docs/LTE_FIELD_ACCESS.md).
LTE remains disabled by default. This source/UI iteration has not compiled or
bench-tested firmware, and no remote host has been deployed yet.

This repo contains four Arduino firmware variants and a Raspberry Pi Linux port. This is a full suite so you only need to add your own sensor and controller programs on.

#### [ESP32_Modular_Command_Transport_Controller/](https://github.com/joshbrew/ESP32-WiFi_BLE_USB_UART-Bridge/tree/main/ESP32_Modular_Command_Transport_Controller)

Is just the transport layer containing all communication methods and the unified command interface.

#### [ESP32_Dispenser_Controller](https://github.com/joshbrew/ESP32-WiFi_BLE_USB_UART-Bridge/tree/main/ESP32_Dispenser_Controller)

Contains GPIO, DAC, and Stepper commands in a configurable suite.

#### [ESP32S3_Modular_Command_Transport_Controller/](ESP32S3_Modular_Command_Transport_Controller/README.md)

Separate transport-only firmware for ESP32-S3-DevKitC-1-N16R8 using the
ESP32S3 Dev Module board profile: native USB/COM serial, Wi-Fi HTTP, BLE with
NimBLE subscription support, optional auxiliary UART, onboard RGB status,
radio rollback, self-tests, and OTA. Its README contains the complete 16 MB
flash / 8 MB OPI PSRAM build settings. Classic Bluetooth SPP is unavailable.

#### [ESP32S3_Dispenser_Controller/](ESP32S3_Dispenser_Controller/README.md)

Separate ESP32-S3 N16R8 dispenser firmware with GPIO4 control, onboard RGB
indication, and S3 pin guardrails. See its README for setup and commands.

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

The Arduino firmware variants include optional LR-only Wi-Fi (`WiFiLR:ON|OFF`, default
OFF). See the [LR setup and Bluetooth relay guide](docs/WIFI_LR.md) for a
phone-to-BLE-to-LR topology and a second ESP32 relay sketch.

#### About

This is based on my prior work with the ESP32 but optimizes a ton, leaving about 33KB of program memory available when including both WiFi and BLE stacks and they can be run simultaneously using the dual core system. Simple defines can exclude different radio code leaving that memory free for other things, if you don't need the full stack. This is a shell for adding on sensors and controls when we want to use the ESP32's radios, and has extensive controls e.g. for power usage and NVS-saved radio configurations (which radios to enable, credentials, names, etc).


This was tested on a cheap ESP-WROOM-32 (WEMOS Lolin32 variant) without PSRAM which can significantly improve throughputs when sharing the radio between both protocols.


License: MIT

Host the same field console on your Windows laptop with **Setup Home Access.cmd** or `controller home-setup`. See [home hosting](docs/HOME_HOSTING.md) for automatic Docker setup, saved modem credentials, DNS and router forwarding. S3 LTE builds can also reconnect through configured Wi-Fi with TLS.
