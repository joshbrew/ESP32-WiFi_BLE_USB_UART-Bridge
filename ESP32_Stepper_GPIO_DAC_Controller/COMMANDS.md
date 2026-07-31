# ESP32 stepper + dual DAC command reference

The same command grammar is available over USB serial, the Wi-Fi browser/HTTP command endpoint, BLE UART RX, optional Classic Bluetooth SPP, and optional auxiliary UART.

Commands are case-insensitive. Send one command per line for a batch.

## Default interfaces

```text
USB serial: 115200 baud
Wi-Fi AP: ESP32-Stepper-WiFi
Wi-Fi password: stepper32
Portal: http://192.168.4.1/
BLE name: ESP32-StepperBLE
```

BLE UUIDs:

```text
Service:   6E400001-B5A3-F393-E0A9-E50E24DCCA9E
RX/write:  6E400002-B5A3-F393-E0A9-E50E24DCCA9E
TX/notify: 6E400003-B5A3-F393-E0A9-E50E24DCCA9E
```

## Core

```text
Ping
Help
Status
ConfigRead
BootModeStatus
USBStatus
HeapStatus
BLEStatus
StopAll
Reboot
```

## Boot policy and self-test

```text
ProductionMode
DebugMode
BootModeStatus
SelfTestStart
SelfTestStatus
SelfTestResume
SelfTestAbort
SelfTestClear
```

## Status indicators

```text
IndicatorStatus
IndicatorTest
IndicatorConnectionTest
IndicatorActivityTest
Indicator16Test
Indicator17Test
```

Current indicator pins:

```text
Connection: GPIO23
Activity: GPIO5
```

## Stepper moves

Direction values:

```text
1 = CW
2 = CCW
```

Move by step count:

```text
RPM:<rpm>,<steps>,<direction>
```

Move by angle:

```text
DEG:<rpm>,<degrees>,<direction>
```

Examples:

```text
RPM:10,2048,1
DEG:10,360,2
```

Preset moves:

```text
MoveFullCW
MoveFullCCW
MoveHalfCW
MoveHalfCCW
TestFullSpeedRev
TestHalfSpeedRev
TestMinSpeedRev
TestFullSpeedRevCCW
TestHalfSpeedRevCCW
TestMinSpeedRevCCW
DebugSpinCW
DebugSpinCCW
RunStartupTest
FastWiperTest
```

## Stepper control and tuning

```text
Setup
GetMotorStats
Stop
CoilsOff
SetRevSteps:<steps>
SetMinRPM:<rpm>
SetMaxRPM:<rpm>
SetStartRPM:<rpm>
SetRampRPM:<rpm-per-second>
SetMinStepIntervalUs:<microseconds>
StepMode:4
StepMode:8
HoldTorque:1
HoldTorque:0
PrintStepOrder
NextStepOrder
StepOrder:<four unique digits from 1 to 4>
```

Example wiring-order override:

```text
StepOrder:2143
```

Stop the motor before changing its tuning or step order.

## DAC and GPIO output commands

Status and DAC1 reference calibration:

```text
DACStatus
OutputStatus
DACRefMV:<2500-to-3600>
```

DAC1 analog target:

```text
DAC1:MV:<millivolts>
```

Digital GPIO26 does not accept an `MV` command. `DAC2:MV:...` is rejected.

Enable and release:

```text
DAC1:ON
DAC1:OFF
GPIO26:ON
GPIO26:OFF
DAC2:ON          compatibility alias for GPIO26:ON
DAC2:OFF         compatibility alias for GPIO26:OFF
DACAll:ON
DACAll:OFF
OutputAll:ON
OutputAll:OFF
```

Timed tests:

```text
DAC1:TEST3S      500 mV for three seconds
GPIO26:TEST3S    HIGH for three seconds
DAC2:TEST3S      compatibility alias
DACTest3S        test both outputs
```

Safety timeout:

```text
DAC1:TIMEOUTMS:<milliseconds>
DAC1:TIMEOUTSEC:<seconds>
GPIO26:TIMEOUTMS:<milliseconds>
GPIO26:TIMEOUTSEC:<seconds>
```

A timeout of `0` means unlimited. The `DAC2:` prefix remains accepted for GPIO26 timeout commands.

Saved boot state:

```text
DAC1:BOOT:ON
DAC1:BOOT:OFF
GPIO26:BOOT:ON
GPIO26:BOOT:OFF
DACSave
DACLoad
DACDefaults
DACErase
```

The `DAC2:` prefix remains accepted for GPIO26 boot-state commands. GPIO26 HIGH is approximately the ESP32 3.3 V logic rail and is intended to control an external analog switch. The external 4 V signal must not be connected to the ESP32 pin.

## Explicit transport output

```text
Send:<payload>
SendBLE:<payload>
SendWiFi:<payload>
SendUSB:<payload>
SendSerial:<payload>
SendUART:<payload>
SendSPP:<payload>
SendStatus
```

`Send:<payload>` forwards to every available output except the command source.

## Radio profile selection

```text
RadioStatus
ModeWiFi
ModeWiFiBLE
ModeWiFiBLEP
ModeBLE
ModeBTSerial
ModeUSB
RadioBoot:WIFI
RadioBoot:WIFI_BLE
RadioBoot:WIFI_BLE_P
RadioBoot:BLE
RadioBoot:SPP
RadioBoot:USB
```

Profile changes use a saved trial, reboot, health check, and last-known-good rollback.

## Radio enable flags

```text
WiFi:ON
WiFi:OFF
BLE:ON
BLE:OFF
ClassicBT:ON
ClassicBT:OFF
SPP:ON
SPP:OFF
```

These commands update the desired boot profile. Bluetooth profile changes are applied through the managed reboot path.

## Wi-Fi configuration

```text
WiFiMode:AP
WiFiMode:STA
WiFiMode:APSTA
WiFiFallbackAP:ON
WiFiFallbackAP:OFF
WiFiTxPower:LOW
WiFiTxPower:MAX
WiFiTxPower:<supported dBm>
WiFiStaSSID:<ssid>
WiFiStaPassword:<password>
WiFiStaClear
WiFiApSSID:<ssid>
WiFiApPassword:<password>
```

Supported explicit Wi-Fi TX power values:

```text
19.5, 19, 18.5, 17, 15, 13, 11, 8.5, 7, 5, 2, -1 dBm
```

## Saved radio configuration

```text
ConfigApply
ConfigSave
ConfigLoad
ConfigDefaults
ConfigErase
```

## Browser and BLE handoff

```text
BLEWebHandoff
BLEWebCancel
WebRestart
BleAdvertise
```

`BLEWebHandoff` is used by the browser when the active profile is `WIFI_BLE` or `WIFI_BLE_P`.

## Demo flows

USB sanity:

```text
Ping
Status
HeapStatus
GetMotorStats
DACStatus
```

Stepper wiring check:

```text
CoilsOff
SetMaxRPM:10
DEG:5,90,1
DEG:5,90,2
CoilsOff
```

DAC channel 1 test:

```text
DACRefMV:3300
DAC1:MV:500
DAC1:TEST3S
```

Fail-safe DAC output:

```text
DAC1:MV:1200
DAC1:TIMEOUTSEC:10
DAC1:BOOT:OFF
DACSave
DAC1:ON
```

Digital switch-control output:

```text
GPIO26:TIMEOUTSEC:10
GPIO26:BOOT:OFF
DACSave
GPIO26:ON
GPIO26:OFF
```

Wi-Fi command to BLE output:

```text
SendBLE:hello from Wi-Fi
```

BLE command to browser output:

```text
SendWiFi:hello from BLE
```

Emergency release:

```text
StopAll
```

## OTA

OTA is driven from the browser console rather than a text command.

```text
1. Compile an application .bin with an OTA-capable partition layout.
2. Open the portal.
3. Expand Firmware OTA.
4. Select the .bin.
5. Upload.
```

The browser can begin from BLE, but the firmware binary always transfers over Wi-Fi.
