# Source layout

```text
ESP32_Modular_Command_Transport_Controller.ino
  Creates the shared objects and performs the boot sequence.

src/config/AppConfig.h
  Build switches, pins, radio names, queue budgets, timing, heap limits, and OTA constants.

src/addons/DeviceAddon.h
  Neutral hardware extension interface. The base build instantiates this directly.

src/core/
  Command queue, command routing, events, persistent self-test, and runtime service task.

src/hardware/StatusIndicators.*
  The only base hardware GPIO ownership. All LED pin setup and writes stay here.

src/radio/RadioManager.*
  Wi-Fi, DNS, radio profiles, NVS configuration, managed reboots, health trials, and rollback.

src/transports/TransportHub.*
  USB, BLE, SPP, auxiliary UART input/output and BLE notification lifetime guards.

src/transports/TransportBridge.*
  Explicit Send, SendBLE, SendWiFi, SendUSB, SendUART, and SendSPP routing.

src/web/WebPortal.*
  HTTP routes, request admission, cached state/events, browser command bodies, and OTA streaming.

src/web/WebAssets.h
  Generated gzip portal. Edit files under web/ instead, then run project_tool.py.

web/
  Human-editable portal sources and generated standalone console.

tools/project_tool.py
  Builds web artifacts and validates the source tree.
```

The compiled source tree intentionally contains no stepper or DAC implementation. Hardware-specific behavior belongs in a derived `DeviceAddon` under `src/addons/`.
