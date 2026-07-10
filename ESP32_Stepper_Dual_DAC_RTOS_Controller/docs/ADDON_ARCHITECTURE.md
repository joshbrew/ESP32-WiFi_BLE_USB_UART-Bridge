# Hardware addon architecture

The command, event, transport, radio, web, OTA, self-test, and `Send*` layers are
the reusable firmware base. Board-specific hardware is selected in the build-switch section in `src/config/AppConfig.h`.

```cpp
#define APP_ADDON_STEPPER_DAC
```

Commenting that line builds the transport-only base. The stepper/DAC source,
commands, startup tests, runtime service, and JSON state are then excluded.

## Current addon

The full stepper and DAC feature pack is isolated here:

```text
src/addons/stepper_dac/
├── StepperDacAddon.cpp/.h
├── StepperController.cpp/.h
├── DacController.cpp/.h
└── README.md
```

`StepperDacAddon` is the only object exposed to the base router. It delegates
hardware commands to the two controllers and supplies addon state and safety
hooks.

## Add an ADC or sensor pack

Create a folder such as:

```text
src/addons/sensor_node/
├── SensorNodeAddon.cpp
├── SensorNodeAddon.h
└── SensorNodeConfig.h
```

Derive the addon from `DeviceAddon`. A minimal addon normally implements only:

```cpp
class SensorNodeAddon : public DeviceAddon {
 public:
  const char *name() const override { return "sensor-node"; }
  void begin() override;
  void service() override;
  bool handleCommand(
    const String &command,
    CommandSource source,
    const String &requestId
  ) override;
};
```

`DeviceAddon` already provides safe defaults for emergency stop, busy state,
state JSON, startup tests, and self-test snapshot/restore. Override those hooks
only when the hardware needs them.

Add one build switch to the build-switch section in `src/config/AppConfig.h`, include the addon header in the
`.ino`, and construct exactly one `deviceAddon` object. The dispatcher, portal,
BLE service, Wi-Fi API, USB/UART input, event routing, and radio profiles do not
need hardware-specific edits.

## Command and output flow

1. A transport submits a complete command line to `CommandDispatcher`.
2. `CommandRouter` handles framework commands.
3. Unclaimed commands are offered to the selected addon.
4. Results are published to `EventBus`.
5. Normal results return to their source plus USB/AUX-UART recovery outputs.

Normal Wi-Fi output is not silently mirrored into BLE, and normal BLE output is
not silently mirrored into Wi-Fi. This avoids unnecessary cross-radio GATT work
in coexistence mode.

Explicit routing remains available:

```text
Send:payload
SendBLE:payload
SendWiFi:payload
SendUSB:payload
SendSerial:payload
SendUART:payload
SendSPP:payload
SendStatus
```

A deliberate send delivers the raw payload only. It does not re-enter command
parsing and cannot create a firmware bridge loop.
