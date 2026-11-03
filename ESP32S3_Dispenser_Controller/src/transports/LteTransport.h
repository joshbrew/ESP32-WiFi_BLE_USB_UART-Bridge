#ifndef ESP32S3_LTE_TRANSPORT_H
#define ESP32S3_LTE_TRANSPORT_H

#include <Arduino.h>
#include <atomic>
#include "../config/AppConfig.h"
#include "../core/AppTypes.h"
#include "../core/EventBus.h"

// The attach/MQTT workers hand off exclusive modem ownership. Only
// serviceControl invokes application callbacks; AT/TLS calls stay off controls.
class LteTransport {
 public:
  explicit LteTransport(EventBus &events) : events_(events) {}
  bool begin(BatchCommandSubmitter submitter, void *submitContext,
             StateProvider provider, void *stateContext);
  void serviceControl();
  bool isConnected() const { return connected_.load(); }
  bool isSeeking() const { return APP_LTE_ENABLED && !connected_.load(); }
 private:
  struct Impl;
  Impl *impl_ = nullptr;
  bool started_ = false;
  EventBus &events_;
  std::atomic<bool> connected_{false};
};
#endif  // ESP32S3_LTE_TRANSPORT_H
