# ESP32 controller command reference

The same command grammar is available over USB serial, the Wi-Fi browser/HTTP command endpoint, BLE UART RX, optional Classic Bluetooth SPP, and optional auxiliary UART.

Commands are case-insensitive. Send one command per line for a batch.

## Default interfaces

```text
USB serial: 115200 baud
Wi-Fi AP: ESP32-Controller
Wi-Fi password: esp32control
Portal: http://192.168.4.1/
BLE name: ESP32-Controller-BLE
```

BLE UUIDs:

```text
Service:  6E400001-B5A3-F393-E0A9-E50E24DCCA9E
RX/write: 6E400002-B5A3-F393-E0A9-E50E24DCCA9E
TX/notify:6E400003-B5A3-F393-E0A9-E50E24DCCA9E
```

## Commands

### Core

```text
Ping
Help
Status
ConfigRead
USBStatus
HeapStatus
StopAll
Reboot
```

### Boot mode and self-test

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

### Status indicators

```text
IndicatorStatus
IndicatorTest
IndicatorConnectionTest
IndicatorActivityTest
```

### Explicit transport output

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

### Radio profile selection

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

### Radio enable flags

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

### Wi-Fi configuration

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

### Saved configuration

```text
ConfigApply
ConfigSave
ConfigLoad
ConfigDefaults
ConfigErase
```

### Browser and BLE handoff

```text
BLEWebHandoff
BLEWebCancel
WebRestart
```

`BLEWebHandoff` is used by the browser when the active profile is `WIFI_BLE` or `WIFI_BLE_P`.

## Demo flows

### USB sanity

```text
Ping
Status
HeapStatus
RadioStatus
```

### LED sanity

```text
IndicatorConnectionTest
IndicatorActivityTest
IndicatorTest
```

### Switch to combined Wi-Fi and BLE

```text
ModeWiFiBLE
```

After reboot, reconnect to the AP and connect Web Bluetooth to `ESP32-Controller-BLE`.

### Wi-Fi command to BLE output

```text
SendBLE:hello from Wi-Fi
```

### BLE command to browser output

```text
SendWiFi:hello from BLE
```

### Configure station Wi-Fi

```text
WiFiStaSSID:My Network
WiFiStaPassword:correct horse battery staple
WiFiMode:APSTA
ConfigSave
ConfigApply
```

### Restore defaults

```text
ConfigDefaults
ConfigApply
```

To remove saved NVS radio configuration:

```text
ConfigErase
Reboot
```

### Start full base self-test

```text
SelfTestStart
```

Check progress:

```text
SelfTestStatus
```

Abort and restore:

```text
SelfTestAbort
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

The browser can begin from BLE, but the binary stream always transfers over Wi-Fi.
