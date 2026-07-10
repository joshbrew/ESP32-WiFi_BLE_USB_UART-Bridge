#ifndef ESP32_MODULAR_CONTROLLER_DEVICEADDON_H
#define ESP32_MODULAR_CONTROLLER_DEVICEADDON_H

#include <Arduino.h>
#include <Preferences.h>

#include <cstring>

#include "../core/AppTypes.h"
#include "../core/EventBus.h"

class CommandDispatcher;

// Compile-time selected hardware feature pack. The transport, command, radio,
// web, logging, send, and self-test framework remains the sketch base.
//
// The defaults make small Arduino-style addons cheap to write. A typical sensor
// addon only overrides name(), begin(), service(), and handleCommand(). Override
// the remaining hooks only when the hardware has outputs, long-running work,
// startup tests, custom web state, or persistent settings that the full
// self-test must preserve.
class DeviceAddon {
 public:
  virtual ~DeviceAddon() = default;

  virtual const char *name() const { return "none"; }
  virtual void begin() {}
  virtual void service() {}

  virtual bool canAcceptCommand(const String &command) const {
    (void)command;
    return true;
  }

  virtual bool handleCommand(
    const String &command,
    CommandSource source,
    const String &requestId
  ) {
    (void)command;
    (void)source;
    (void)requestId;
    return false;
  }

  virtual bool stopAll(CommandSource source, const String &requestId) {
    (void)source;
    (void)requestId;
    return true;
  }

  virtual bool isBusy() const { return false; }
  virtual bool hasActiveOutput() const { return false; }
  virtual bool hasTimedOperationActive() const { return false; }

  virtual void appendStateJson(String &json, bool compact) const {
    (void)compact;
    json += ",\"addon\":{\"name\":\"" + String(name()) + "\",\"active\":false}";
  }

  virtual void publishHelp(
    EventBus &events,
    CommandSource source,
    const String &requestId
  ) const {
    events.publish(
      EventLevel::STATUS,
      "Addon: none selected; choose an addon in src/config/AppConfig.h",
      source,
      requestId
    );
  }

  virtual void publishStatus(
    EventBus &events,
    CommandSource source,
    const String &requestId,
    bool configuration
  ) const {
    events.publish(
      EventLevel::STATUS,
      String(configuration ? "[CONFIG]" : "[STATUS]") + " addon=" + name(),
      source,
      requestId
    );
  }

  virtual void queueStartupTests(CommandDispatcher &dispatcher) const {
    (void)dispatcher;
  }

  virtual bool captureSelfTestState(
    Preferences &snapshot,
    String &defaultNamespaces,
    String &reason
  ) {
    (void)defaultNamespaces;
    const size_t nameLength = std::strlen(name());
    const bool ok = snapshot.putString("addon", name()) == nameLength;
    reason = ok ? "addon marker captured" : "could not capture addon marker";
    return ok;
  }

  virtual bool loadSelfTestState(Preferences &snapshot, String &reason) {
    const bool ok = snapshot.getString("addon", "") == name();
    reason = ok ? "addon marker loaded" : "self-test snapshot belongs to a different addon";
    return ok;
  }

  virtual bool restorePersistentSelfTestState(
    Preferences &snapshot,
    String &reason
  ) {
    (void)snapshot;
    reason = "addon has no persistent state to restore";
    return true;
  }

  virtual bool verifyPersistentSelfTestState(
    Preferences &snapshot,
    String &reason
  ) const {
    const bool ok = snapshot.getString("addon", "") == name();
    reason = ok ? "addon marker matches" : "addon marker mismatch";
    return ok;
  }

  virtual uint16_t runtimeRestoreCommandCount() const { return 0; }

  virtual String runtimeRestoreCommand(uint16_t index) const {
    (void)index;
    return String();
  }
};

#endif  // ESP32_MODULAR_CONTROLLER_DEVICEADDON_H
