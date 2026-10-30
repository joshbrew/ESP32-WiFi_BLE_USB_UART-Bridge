# Hardware addon guide

The base firmware owns transport, radio, web, OTA, event routing, and status indicators. User hardware belongs in a class derived from `DeviceAddon`.

Create `src/addons/MyAddon.h`:

```cpp
#ifndef ESP32_MODULAR_CONTROLLER_MYADDON_H
#define ESP32_MODULAR_CONTROLLER_MYADDON_H

#include "DeviceAddon.h"

class MyAddon : public DeviceAddon {
 public:
  explicit MyAddon(EventBus &events) : events_(events) {}

  const char *name() const override { return "my-addon"; }

  void begin() override {
    // Configure addon-owned hardware here.
  }

  void service() override {
    // Keep this bounded and nonblocking.
  }

  bool handleCommand(
    const String &command,
    CommandSource source,
    const String &requestId
  ) override {
    if (!command.equalsIgnoreCase("MyCommand")) {
      return false;
    }
    events_.publish(EventLevel::STATUS, "my addon command complete", source, requestId);
    return true;
  }

  bool stopAll(CommandSource source, const String &requestId) override {
    (void)source;
    (void)requestId;
    // Release outputs and cancel queued work.
    return true;
  }

 private:
  EventBus &events_;
};

#endif  // ESP32_MODULAR_CONTROLLER_MYADDON_H
```

Then include it in the sketch and replace:

```cpp
DeviceAddon deviceAddon;
```

with:

```cpp
MyAddon deviceAddon(events);
```

For long-running work, use a dedicated task, timer, or state machine. Do not block `service()` or directly write to transport sockets. Publish output through `EventBus` so source routing and queue limits remain consistent.

For this S3 N16R8 variant, check `AppConfig::isAvailableAddonPin()` before
assigning hardware. Reserve memory GPIO26..37, native USB GPIO19/20, COM
GPIO43/44, boot straps GPIO0/3/45/46, and RGB GPIO38/48. GPIO39..42 overlap
external JTAG. The S3 has no internal DAC or Classic Bluetooth SPP.
