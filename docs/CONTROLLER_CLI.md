# ESP32 command-line tools

For the complete hosted LTE setup, use **Setup Field Access.cmd** or
`controller field-setup`. It includes server dependency installation and guided
modem provisioning. See [field setup](FIELD_SETUP.md).

Run `controller.cmd` from the repository root in a Windows terminal. It supports Windows PowerShell 5.1 and PowerShell 7. No Python installation is needed. The old **Flash Firmware.cmd** launcher now opens the same `flash` command.

```powershell
.\controller.cmd help
.\controller.cmd ports
.\controller.cmd doctor
.\controller.cmd setup
.\controller.cmd compile -Variant s3-dispenser
.\controller.cmd flash -Variant s3-dispenser
```

`setup` installs missing tools/packages without compiling. `compile` also checks dependencies automatically, builds without uploading, and saves a complete binary set. `flash` uploads existing binaries without compiling. Close Arduino Serial Monitor before flashing.

## Detect and confirm the COM port

The tool reads Windows USB/serial metadata without opening or resetting ports. It recognizes Espressif USB, Adafruit/Feather descriptions, and common CP210x/CH340/FTDI bridges. Bluetooth serial ports are excluded when Windows identifies them. A unique USB candidate is suggested automatically. Multiple candidates open a numbered selection list.

Before every real flash, the tool displays the firmware, COM port, device name and identification reason, then asks:

```text
Is this the correct board and port? Flash now? [y/N]
```

Type `y` or `yes` to flash; Enter or any other answer cancels. This confirmation also applies to an explicit `-Port COM24`. There is no unattended confirmation bypass. The board identity and binary hashes are checked again after confirmation; a disconnected/replaced device or changed binary stops the upload.

A USB descriptor identifies a candidate port, not its exact board model. An Adafruit USB device might be a different Feather family. Choose a firmware variant matching the actual hardware. These profiles target the existing N16R8 DevKitC-1 and classic LOLIN32; no Feather-specific pinout or board profile is inferred from its COM port. If descriptions are unavailable, select a listed port manually. If no ports appear, check the data cable/USB driver; S3 recovery is described below.

## Firmware variants and compile settings

| Variant | Sketch | Board/settings |
|---|---|---|
| `s3-dispenser` | `ESP32S3_Dispenser_Controller` | ESP32S3 Dev Module, 240 MHz, 16 MB, QIO 80 MHz, OPI PSRAM, 16 MB OTA layout |
| `s3-transport` | `ESP32S3_Modular_Command_Transport_Controller` | Same N16R8 settings |
| `lolin32-dispenser` | `ESP32_Dispenser_Controller` | WEMOS LOLIN32, 240 MHz, `min_spiffs` OTA layout |
| `lolin32-transport` | `ESP32_Modular_Command_Transport_Controller` | Same classic ESP32 settings |

The S3 partition selection is `app3M_fat9M_16MB`, with Hardware CDC/JTAG USB mode. `-Serial usb` (default) compiles USB CDC on boot. `-Serial com` compiles serial logging for the UART/COM bridge instead. This chooses the compiled Serial route; either board connector can still upload. The flasher cannot change a binary's compiled USB, PSRAM, pin or feature settings.

```powershell
# Preview compile settings; no installation, compilation or serial access:
.\controller.cmd compile -Variant s3-transport -DryRun

# Compile for serial logging through the COM/UART connector:
.\controller.cmd compile -Variant s3-dispenser -Serial com

# Use more parallel compiler jobs, or explicitly start a clean build:
.\controller.cmd compile -Variant s3-dispenser -Jobs 4
.\controller.cmd compile -Variant s3-dispenser -Clean
```

Build caches are retained between compiles. The default is two compiler jobs; `-Jobs 0` lets Arduino CLI use all available CPU cores. Successful builds are validated before being saved to `precompiled/<variant>/`. The previous set is retained in a sibling `<variant>.previous-<id>` folder. A failed compile or incomplete binary set does not replace the selected saved set. Previous sets can be removed manually when no longer needed.

Embedded web assets are checked in. If you edit `web/` sources yourself, use that sketch's web asset generator before compiling, as described in its README.

The Raspberry Pi application uses its own update workflow; this tool covers the four Arduino variants.

## Automatic setup

Setup reuses Arduino CLI from PATH, a prior portable install, or Arduino IDE's bundled CLI. Arduino CLI 1.2+ is required. If absent, it downloads official Windows x64 Arduino CLI **1.5.1**, verifies the archive against the pinned SHA-256, and installs it under `%LOCALAPPDATA%\ESP32ControllerTools\bin\`. No machine-wide PATH changes or administrator account are needed.

The pinned ESP32 core is **3.3.10**. A compatible core already in `%LOCALAPPDATA%\Arduino15` is reused. Otherwise it is installed into the tool's private data folder. Libraries use a private sketchbook under `%LOCALAPPDATA%\ESP32ControllerTools\sketchbook`, so setup does not replace Arduino IDE's library folders. The CLI configuration, downloads and build caches are also kept under that tools directory. First setup needs Internet access and can download a large ESP32 toolchain; repeat runs skip packages already present at the required version.

| Dependency | Version | Used for |
|---|---|---|
| Async TCP | 3.5.0 | Local HTTP transport |
| ESP Async WebServer | 3.12.1 | Local web console |
| TinyGSM | 0.12.0 | Optional SARA-R4 LTE |
| ArduinoMqttClient | 0.1.8 | Optional LTE MQTT |
| ArduinoJson | 7.3.2 | Optional LTE state/command framing |

```powershell
# Install optional LTE libraries without compiling:
.\controller.cmd setup -Lte

# Compile with the S3 LTE feature enabled:
.\controller.cmd compile -Variant s3-dispenser -Lte
```

LTE compilation requires the selected sketch's ignored `src/config/LtePrivate.h`. `-Lte` enables `APP_ENABLE_LTE=1` for that compile without editing the sketch. A source-enabled LTE switch is also detected for dependency setup. Provision the modem CA/clock and credentials as described in [LTE field access](../ESP32S3_Modular_Command_Transport_Controller/docs/LTE_FIELD_ACCESS.md). LTE is not supported by these LOLIN32 profiles.

Use `-ArduinoCliPath`, `-EsptoolPath` or `-ToolsDirectory` for an explicit location. Flashing can install missing CLI/core tools on demand; it skips library installation. `ports`, `doctor` and `help` are read-only. `setup -DryRun` and `compile -DryRun` do not install or write anything.

## Existing builds and faster repeat uploads

```powershell
.\controller.cmd flash -Variant s3-dispenser
.\controller.cmd flash -Variant s3-dispenser -Port COM24
.\controller.cmd flash -Variant s3-dispenser -BuildDirectory "D:\Firmware\my-s3-build"
.\controller.cmd flash -Variant s3-dispenser -SkipUnchanged
.\controller.cmd flash -Variant s3-dispenser -Baud 460800
.\controller.cmd flash -Variant s3-dispenser -BuildDirectory "D:\Firmware\my-s3-build" -DryRun
```

The complete set includes the matching sketch `.ino.bin`, `.ino.bootloader.bin`, `.ino.partitions.bin`, `boot_app0.bin` when OTA metadata is present, and `build.options.json` when available. You can also use Arduino IDE's **Export Compiled Binary**. The script can obtain a missing OTA initialization image from the matching installed core.

Automatic build selection prefers `precompiled/<variant>/`, then the newest matching completed sketch `build/` export or Arduino IDE cache under `%LOCALAPPDATA%\arduino\sketches`. `-BuildDirectory` overrides that search. The binary date, original sketch path, saved board settings and write offsets are displayed. Source edits newer than the binary are reported; use `compile` when ready to include them.

Default transfer speed is 921600 with compression. `-SkipUnchanged` uses esptool 5.3+ to compare existing flash contents before writing. Lower `-Baud` if the cable/bridge is unreliable. A flash dry run prints the plan without opening serial or asking for confirmation; COM1 is a placeholder if no port was supplied. Use `ports` for real discovery.

The script checks chip IDs, image size, partition boundaries and write overlap. Bootloader addresses are `0x0` for S3 and `0x1000` for classic ESP32. Known S3 metadata with QSPI PSRAM or a flash size other than 16 MB is rejected. Metadata-free exports cannot identify compiled PSRAM/USB settings; verify their original settings.

Only the selected bootloader, partition table, OTA initialization and application are written. No whole-chip erase or padded merged image is used. NVS/filesystem regions outside those writes remain intact with an unchanged partition layout; changing layouts can change how saved data is interpreted. Flashing restarts the controller.

For native S3 USB recovery: hold **BOOT**, tap **RESET**, release **BOOT**, then run `ports` and `flash` again. The recovery COM port can differ from the application's COM port.

## Checks

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tests\flash_precompiled.tests.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tests\controller_cli.tests.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tests\flash_confirmation.tests.ps1
```

The tests use synthetic images, simulated ports, mock package installs and a recording stand-in for esptool. They cover selection/confirmation, changed devices/files, compile profiles, complete-build publication and dry runs. They do not compile firmware or access serial devices. Real Windows port discovery and Arduino CLI board-profile validation were also checked without compiling. A real hardware upload and a full firmware compile remain to be checked when requested.

References: [Arduino CLI installation](https://arduino.github.io/arduino-cli/1.5/installation/), [compile command](https://arduino.github.io/arduino-cli/1.5/commands/arduino-cli_compile/), [esptool flashing](https://docs.espressif.com/projects/esptool/en/latest/esp32/esptool/basic-commands.html).

## Home server setup

`controller home-setup [-Variant s3-dispenser|s3-transport] [-DryRun]` launches the Windows home-host wizard. **Setup Home Access.cmd** is its double-click entry point. It installs/reuses WSL 2 + Docker Desktop, creates/reuses TLS/credentials, runs the broker/web app locally, offers Cloudflare DNS and modem provisioning, and prints the router forwarding plan. No compile or flash occurs. See [home hosting](HOME_HOSTING.md). Detailed options (`-Domain`, `-PublicAddress`, `-SkipModem`, etc.) are available via `tools/home_setup.ps1`.
