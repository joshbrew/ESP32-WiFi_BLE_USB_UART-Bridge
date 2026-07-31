# v5.15.44 changes

- Kept DAC1 as the adjustable native ESP32 DAC on GPIO25.
- Converted the former DAC2 channel on GPIO26 into a normal push-pull digital output.
- Added `GPIO26:ON`, `GPIO26:OFF`, `GPIO26:TEST3S`, boot-state, and timeout commands.
- Retained `DAC2:` ON/OFF/test/boot/timeout commands as compatibility aliases.
- Rejects `GPIO26:MV:...` and `DAC2:MV:...` because GPIO26 is digital-only.
- Updated the hosted and standalone consoles to label GPIO26 correctly.
- Updated state JSON, status output, help, validation, README, and command reference.
