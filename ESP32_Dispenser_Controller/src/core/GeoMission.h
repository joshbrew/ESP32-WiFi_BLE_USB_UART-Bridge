#ifndef DRONE_GEL_CONTROLLER_GEOMISSION_H
#define DRONE_GEL_CONTROLLER_GEOMISSION_H

#include <Arduino.h>
#include "../config/AppConfig.h"
#include "RoutineEngine.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#if APP_WIFI_ENABLED
#include <WiFiUdp.h>
#endif

class GeoMission {
 public:
  GeoMission(EventBus &events, DeviceAddon &addon, RoutineEngine &routines);
  void begin();
  bool handleCommand(const String &command, CommandSource source, const String &requestId);
  void service();
  void stop(const String &reason);
  bool isActive() const { return active_; }
  bool consumeSafetyStop();
  String stateJson(bool compact) const;
  // Called by HTTP tasks: overwrite one latest sample; never execute outputs.
  bool submitPosition(const String &body);
 private:
  struct InputFix { double latitude, longitude; float accuracy; uint32_t ageMs, receivedAt; bool valid; };
  struct Point { double latitude, longitude; float tolerance; char routine[16]; };
  struct Plan { uint32_t magic; uint16_t count; uint8_t source; Point points[AppConfig::GEO_MAX_POINTS]; uint32_t checksum; };
  QueueHandle_t positionQueue_ = nullptr;
  void resetPosition();
  void pollMavlink();
  void position(double latitude, double longitude, float accuracy, uint32_t ageMs);
  bool fresh() const;
  bool parsePosition(const String &body, InputFix &fix) const;
  bool sendTestPosition(const String &body);
  bool load();
  bool save();
  bool validate(const Plan &plan) const;
  uint32_t checksum(const Plan &plan) const;
  double distance(const Point &point) const;
  void report(EventLevel level, const String &text, CommandSource source = CommandSource::INTERNAL,
              const String &requestId = String()) const;
  EventBus &events_;
  DeviceAddon &addon_;
  RoutineEngine &routines_;
  Plan plan_{};
  bool active_ = false, runningRoutine_ = false, stopRequested_ = false, saved_ = false;
  bool havePosition_ = false, haveBootTime_ = false;
  uint16_t next_ = 0;
  double latitude_ = 0, longitude_ = 0;
  float accuracy_ = -1;
  uint32_t positionAt_ = 0, ageAtReceipt_ = 0, gpsAt_ = 0, lastBootMs_ = 0;
  uint8_t fixType_ = 0;
  float mavlinkAccuracy_ = -1;
  String result_ = "idle";
#if APP_WIFI_ENABLED
  WiFiUDP udp_;
  bool udpReady_ = false;
#endif
};
#endif  // DRONE_GEL_CONTROLLER_GEOMISSION_H
