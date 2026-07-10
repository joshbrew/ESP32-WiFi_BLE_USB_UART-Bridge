# Addons

`DeviceAddon.h` is the neutral hardware-extension interface used by the base firmware.

The shipped build instantiates `DeviceAddon` directly, so no custom actuator or sensor program owns GPIO pins. To add hardware behavior, create a derived class under this directory and wire one instance into the main sketch. Keep transport, radio, web, and OTA code outside the addon.
