# Local precompiled firmware

Save complete, matching binary sets in one of these folders:

```text
precompiled/s3-dispenser/
precompiled/s3-transport/
precompiled/lolin32-dispenser/
precompiled/lolin32-transport/
```

The root **Flash Firmware.cmd** launcher prefers a saved set here over Arduino's temporary cache. Keep the original sketch filenames, bootloader, partition table, `boot_app0.bin` and build metadata together. These local build folders are ignored by Git.

No binaries have been built or copied here by this change. See [the flashing guide](../docs/FLASHING_PRECOMPILED.md) for export steps, dry runs and command-line options.
