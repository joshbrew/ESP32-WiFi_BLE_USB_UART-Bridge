# Flash existing binaries without recompiling

On Windows, double-click **Flash Firmware.cmd** in the repository root, or run `controller.cmd flash`. Select the firmware variant; the tool suggests a detected USB COM port (or lists candidates) and asks you to confirm it before flashing. It finds an existing matching build and invokes the ESP32 flashing tool directly, without compiling.

The [command-line guide](CONTROLLER_CLI.md) covers `setup`, `doctor`, `ports`, `compile` and `flash`, including automatic CLI/package installation when needed. An existing ESP32 package's `esptool.exe` can be reused. Close Serial Monitor before flashing. `-Port` still requires confirmation; metadata alone does not establish the exact Feather/ESP32 board model.

## Build once, flash repeatedly

1. When ready to build, select the exact board/settings from the variant's README. For N16R8 use **ESP32S3 Dev Module, 16 MB flash and OPI PSRAM**.
2. Use **Sketch > Export Compiled Binary** once, or keep an existing completed build folder. Exporting itself compiles; the flashing script never does.
3. Double-click the launcher for later uploads, or run it from PowerShell:

```powershell
& ".\Flash Firmware.cmd" -Variant s3-dispenser -Port COM24
```

Replace COM24 with the port for your board. The selectable variants are:

| Variant | Sketch |
|---|---|
| `s3-dispenser` | `ESP32S3_Dispenser_Controller` |
| `s3-transport` | `ESP32S3_Modular_Command_Transport_Controller` |
| `lolin32-dispenser` | `ESP32_Dispenser_Controller` |
| `lolin32-transport` | `ESP32_Modular_Command_Transport_Controller` |

The Raspberry Pi application uses its own update workflow; this serial flasher is for the Arduino variants.

## Choose or pin a binary set

Pass the **folder containing the files**, rather than a single application binary:

```powershell
& ".\Flash Firmware.cmd" -Variant s3-dispenser -BuildDirectory "D:\Firmware\my-s3-build" -Port COM24
```

For the S3 dispenser, keep these matching files together:

```text
ESP32S3_Dispenser_Controller.ino.bin
ESP32S3_Dispenser_Controller.ino.bootloader.bin
ESP32S3_Dispenser_Controller.ino.partitions.bin
boot_app0.bin
build.options.json             (keep when available)
```

Other variants use their own sketch name in the same filenames. Arduino exports sometimes omit `boot_app0.bin`; the script can find it in the installed ESP32 core, preferring the core recorded in `build.options.json`. Keep the complete set from the same completed build. The partition file determines the application and OTA-data offsets. Factory-only partition layouts omit the OTA initialization write.

Automatic selection checks these locations:

1. `precompiled/<variant>/` if you have saved a set there.
2. The newest matching build among the sketch's `build/` export folders and Arduino IDE 2's `%LOCALAPPDATA%\arduino\sketches\` cache.

Explicit `-BuildDirectory` overrides automatic selection. Temporary Arduino caches may disappear; pin a build in `precompiled/s3-dispenser/`, for example, if you want repeatable uploads later. Files inside `precompiled/` are ignored by Git.

The script prints the binary's date, saved board settings, original sketch location when available, and write offsets. It reports when repository source files are newer than the binary. Later source/UI changes are included only after a new build. No newly compiled firmware is supplied by adding this script.

## Preview and faster repeat flashing

```powershell
# Print the plan and command without opening a serial port:
& ".\Flash Firmware.cmd" -Variant s3-dispenser -BuildDirectory "D:\Firmware\my-s3-build" -DryRun

# Skip writing images that already match flash (esptool 5.3+):
& ".\Flash Firmware.cmd" -Variant s3-dispenser -Port COM24 -SkipUnchanged

# Reduce transfer speed if the cable/bridge is unreliable:
& ".\Flash Firmware.cmd" -Variant s3-dispenser -Port COM24 -Baud 460800
```

Default transfer speed is 921600 baud, with compressed transfer. `-SkipUnchanged` checks the board's flash contents; it does not blindly trust a previous file. The script supports older esptool command spelling for ordinary flashing. A custom tool can be selected with `-EsptoolPath "C:\path\to\esptool.exe"` using the PowerShell script directly.

## Board settings and stored data

The bootloader address is **0x0 for ESP32-S3** and **0x1000 for classic ESP32**. The script checks both image chip IDs, partition boundaries, image size and write overlap before invoking esptool. For S3 N16R8, a known build-options record with QSPI PSRAM or a different flash size is rejected with a settings message. If export metadata is absent, verify the original build settings yourself; flash headers cannot identify the compiled PSRAM/USB settings.

Flashing keeps the images' compiled flash settings. It cannot change CPU frequency, PSRAM mode, USB CDC, pins or feature flags after compilation. It writes the selected bootloader, partition table, OTA initialization and application without a whole-chip erase. NVS and filesystem regions outside those writes remain untouched with an unchanged partition layout; changing layouts can change how saved data is interpreted. A padded merged image is not used.

The S3 board's **USB** and **COM** connectors can both upload. Serial Monitor still follows the USB CDC setting compiled into the binary. If native USB does not appear, hold **BOOT**, tap **RESET**, release **BOOT**, and select the new port. Flashing restarts the controller, as Arduino Upload does.

Reference: [Espressif esptool flashing commands](https://docs.espressif.com/projects/esptool/en/latest/esp32/esptool/basic-commands.html).

## Script checks

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tests\flash_precompiled.tests.ps1
```

The tests use synthetic image headers/partition tables and a non-executable fake tool. They cover all four variants, chip and N16R8 settings, partition sizes/overlaps, paths with spaces/quotes, cache selection, older/newer tool arguments, core OTA-data fallback, stale-source reporting and dry-run behavior. They never open a serial port or compile/flash firmware. Existing real S3 build folders were also checked in dry-run mode.
