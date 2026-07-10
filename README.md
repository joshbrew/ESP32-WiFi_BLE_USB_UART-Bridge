## ESP32 WiFi-BLE-USB-UART Bridge

This repo contains two Arduino projects.

#### [ESP32_Modular_Command_Transport_Controller/](https://github.com/joshbrew/ESP32-WiFi_BLE_USB_UART-Bridge/tree/main/ESP32_Modular_Command_Transport_Controller)

Is just the transport layer containing all communication methods and the unified command interface.

#### [ESP32_Stepper_Dual_DAC_RTOS_Controller](https://github.com/joshbrew/ESP32-WiFi_BLE_USB_UART-Bridge/tree/main/ESP32_Stepper_Dual_DAC_RTOS_Controller)

Contains an additional demo application with a generic 5V stepper (28BYJ-48 + ULN2003 5V) and 2 DAC pins. This demonstrates how to make modular addons.

#### About

This is based on my prior work with the ESP32 but optimizes a ton, leaving about 33KB of program memory available when including both WiFi and BLE stacks and they can be run simultaneously using the dual core system. Simple defines can exclude different radio code leaving that memory free for other things, if you don't need the full stack. This is a shell for adding on sensors and controls when we want to use the ESP32's radios, and has extensive controls e.g. for power usage and NVS-saved radio configurations (which radios to enable, credentials, names, etc). This is a full suite so you only need to add your own sensor and controller programs on.


This was tested on a cheap ESP-WROOM-32 (WEMOS Lolin32 variant) without PSRAM which can significantly improve throughputs when sharing the radio between both protocols.


License: MIT
