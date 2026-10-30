# Addons

`DeviceAddon.h` is the neutral hardware-extension interface used by the base firmware.

The shipped build instantiates `DeviceAddon` directly, so no custom actuator or sensor program owns GPIO pins. To add hardware behavior, create a derived class under this directory and wire one instance into the main sketch. Keep transport, radio, web, and OTA code outside the addon.

For this S3 N16R8 variant, check `AppConfig::isAvailableAddonPin()` before
assigning hardware. Reserve memory GPIO26..37, native USB GPIO19/20, COM
GPIO43/44, boot straps GPIO0/3/45/46, and RGB GPIO38/48. GPIO39..42 overlap
external JTAG. The S3 has no internal DAC or Classic Bluetooth SPP.
