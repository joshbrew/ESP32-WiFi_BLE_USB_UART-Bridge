#include "GeoMission.h"
#include <Preferences.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#if APP_WIFI_ENABLED
#include <WiFi.h>
#endif
#include "MavlinkPosition.h"
#include "../util/TextUtil.h"

namespace {
constexpr uint32_t MAGIC = 0x47454F32; // Wider point counters and expanded capacity.
uint32_t recordChecksum(const void *record, size_t length) {
  const auto *bytes = static_cast<const uint8_t *>(record); uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < length; ++i) { hash ^= bytes[i]; hash *= 16777619UL; } return hash;
}
constexpr uint32_t FIX_TIMEOUT_MS = 3000;
bool number(const String &text, double &value) {
  String input = text; input.trim();
  if (!input.length() || input.length() > 32) return false;
  for (size_t at = 0; at < input.length(); ++at) {
    const char c = input[at];
    if ((c < '0' || c > '9') && c != '.' && c != '-' && c != '+' && c != 'e' && c != 'E') return false;
  }
  char *end = nullptr;
  value = strtod(input.c_str(), &end);
  return end != input.c_str() && *end == '\0' && isfinite(value);
}
bool coordinates(double latitude, double longitude) {
  return isfinite(latitude) && isfinite(longitude) && latitude >= -90 && latitude <= 90 && longitude >= -180 && longitude <= 180;
}
}

GeoMission::GeoMission(EventBus &events, DeviceAddon &addon, RoutineEngine &routines)
  : events_(events), addon_(addon), routines_(routines) {}
void GeoMission::begin() {
  plan_.magic = MAGIC; saved_ = load();
  positionQueue_ = xQueueCreate(1, sizeof(InputFix));
}
void GeoMission::resetPosition() {
  havePosition_ = false; haveBootTime_ = false; fixType_ = 0;
  if (positionQueue_) xQueueReset(positionQueue_);
#if APP_WIFI_ENABLED
  udp_.stop(); udpReady_ = false;
#endif
}
bool GeoMission::submitPosition(const String &body) {
  if (!positionQueue_ || body.length() > 128) return false;
  InputFix fix{}; fix.receivedAt = millis();
  String fields[4]; unsigned int start = 0;
  bool shape = true;
  for (uint8_t i = 0; i < 4; ++i) {
    const int comma = body.indexOf(',', start);
    if ((i < 3 && comma < 0) || (i == 3 && comma >= 0)) { shape = false; break; }
    fields[i] = comma < 0 ? body.substring(start) : body.substring(start, comma);
    fields[i].trim(); start = comma + 1;
  }
  double accuracy = -1;
  fix.valid = shape && number(fields[0], fix.latitude) && number(fields[1], fix.longitude) &&
    coordinates(fix.latitude, fix.longitude) && number(fields[2], accuracy) && (accuracy == -1 || accuracy >= 0) && accuracy <= 100000 &&
    TextUtil::parseUnsigned32(fields[3], fix.ageMs) && fix.ageMs <= FIX_TIMEOUT_MS;
  fix.accuracy = accuracy;
  // Invalid samples invalidate the previous fix on the controller task too.
  return xQueueOverwrite(positionQueue_, &fix) == pdPASS && fix.valid;
}
void GeoMission::report(EventLevel level, const String &text, CommandSource source, const String &requestId) const {
  events_.publish(level, "[GEO] " + text, source, requestId);
}
bool GeoMission::fresh() const {
  const uint32_t elapsed = millis() - positionAt_;
  return havePosition_ && elapsed <= FIX_TIMEOUT_MS && ageAtReceipt_ <= FIX_TIMEOUT_MS - elapsed;
}
void GeoMission::position(double latitude, double longitude, float accuracy, uint32_t ageMs) {
  if (!coordinates(latitude, longitude) || !isfinite(accuracy) || accuracy < -1 || ageMs > FIX_TIMEOUT_MS) {
    havePosition_ = false;
    return;
  }
  latitude_ = latitude; longitude_ = longitude; accuracy_ = accuracy;
  ageAtReceipt_ = ageMs; positionAt_ = millis(); havePosition_ = true;
}
double GeoMission::distance(const Point &point) const {
  constexpr float radians = 0.017453292519943295f;
  // Subtract full-precision coordinates first, then use float trig. This keeps
  // sub-meter resolution inside our 1 km maximum trigger radius without linking
  // the ESP32's much larger double-precision trigonometric implementations.
  const float dLat = static_cast<float>(point.latitude - latitude_) * radians;
  double longitudeDelta = point.longitude - longitude_;
  if (longitudeDelta > 180) longitudeDelta -= 360;
  else if (longitudeDelta < -180) longitudeDelta += 360;
  const float dLon = static_cast<float>(longitudeDelta) * radians;
  const float latSin = sinf(dLat / 2), lonSin = sinf(dLon / 2);
  // Complementary angles also preserve precision near either geographic pole.
  const float latCos = sinf(static_cast<float>(90 - fabs(latitude_)) * radians);
  const float pointCos = sinf(static_cast<float>(90 - fabs(point.latitude)) * radians);
  const float a = latSin * latSin + latCos * pointCos * lonSin * lonSin;
  return 12742000.0f * atan2f(sqrtf(fmaxf(0.0f, a)), sqrtf(fmaxf(0.0f, 1 - a)));
}
void GeoMission::stop(const String &reason) {
  const bool wasActive = active_;
  active_ = false; runningRoutine_ = false;
  result_ = reason;
  if (wasActive) { stopRequested_ = true; report(EventLevel::WARNING, reason); }
}
bool GeoMission::consumeSafetyStop() { const bool requested = stopRequested_; stopRequested_ = false; return requested; }

bool GeoMission::handleCommand(const String &command, CommandSource source, const String &requestId) {
  String work = command; work.trim();
  if (!TextUtil::startsWithIgnoreCase(work, "Geo")) return false;
  if (work.equalsIgnoreCase("GeoStatus")) { report(EventLevel::STATUS, stateJson(false), source, requestId); return true; }
  if (work.equalsIgnoreCase("GeoStart")) {
#if APP_DRONE_DISPENSER_ADDON_ENABLED
    for (uint16_t i = 0; i < plan_.count; ++i) {
      if (!routines_.hasSaved(plan_.points[i].routine)) {
        report(EventLevel::ERROR, "point " + String(i + 1) + " needs a saved routine: " + plan_.points[i].routine, source, requestId);
        return true;
      }
    }
    if (active_ || !plan_.count || !saved_ || !fresh() || routines_.isActive() || addon_.hasActiveOutput()) {
      report(EventLevel::ERROR, "start needs a saved plan, fresh position, and idle output", source, requestId);
    } else {
      active_ = true; runningRoutine_ = false; next_ = 0; result_ = "waiting for coordinate";
      report(EventLevel::WARNING, "sequence enabled; each reached point arms and runs its saved routine", source, requestId);
    }
#else
    report(EventLevel::ERROR, "coordinate sequences require the dispenser hardware profile", source, requestId);
#endif
    return true;
  }
  if (work.equalsIgnoreCase("GeoList")) {
    for (uint16_t i = 0; i < plan_.count; ++i) {
      const Point &point = plan_.points[i];
      report(EventLevel::STATUS, "point=" + String(i + 1) + " " + String(point.latitude, 7) + "," +
        String(point.longitude, 7) + "," + String(point.tolerance, 2) + "," + point.routine, source, requestId);
    }
    return true;
  }
  if (TextUtil::startsWithIgnoreCase(work, "GeoPosition:")) {
    if (plan_.source != 1) report(EventLevel::ERROR, "select GeoSource:API first", source, requestId);
    else if (!submitPosition(work.substring(12))) report(EventLevel::ERROR, "invalid or stale position; use latitude,longitude,accuracyMeters,ageMs", source, requestId);
    return true;
  }
  if (active_) { report(EventLevel::ERROR, "stop the coordinate sequence before editing", source, requestId); return true; }
  if (work.equalsIgnoreCase("GeoResetPosition")) { resetPosition(); return true; }
  if (TextUtil::startsWithIgnoreCase(work, "GeoSource:")) {
    const String value = work.substring(10);
    if (!value.equalsIgnoreCase("API") && !value.equalsIgnoreCase("MAVLINK")) {
      report(EventLevel::ERROR, "use GeoSource:API or GeoSource:MAVLINK", source, requestId);
    } else { plan_.source = value.equalsIgnoreCase("API") ? 1 : 0; saved_ = false; resetPosition(); }
    return true;
  }
  if (work.equalsIgnoreCase("GeoClear")) { plan_ = {}; plan_.magic = MAGIC; saved_ = false; next_ = 0; resetPosition(); return true; }
  if (work.equalsIgnoreCase("GeoLoad")) { saved_ = load(); resetPosition(); report(saved_ ? EventLevel::STATUS : EventLevel::ERROR, saved_ ? "plan loaded" : "no valid saved plan", source, requestId); return true; }
  if (work.equalsIgnoreCase("GeoSave")) { saved_ = save(); report(saved_ ? EventLevel::STATUS : EventLevel::ERROR, saved_ ? "plan saved" : "plan save failed", source, requestId); return true; }
  if (TextUtil::startsWithIgnoreCase(work, "GeoAdd:")) {
    String fields[4]; unsigned int start = 7;
    for (uint8_t i = 0; i < 4; ++i) {
      const int comma = work.indexOf(',', start);
      if ((i < 3 && comma < 0) || (i == 3 && comma >= 0)) { report(EventLevel::ERROR, "use GeoAdd:latitude,longitude,toleranceMeters,routineName", source, requestId); return true; }
      fields[i] = comma < 0 ? work.substring(start) : work.substring(start, comma); fields[i].trim(); start = comma + 1;
    }
    double lat, lon, tolerance;
    bool validName = fields[3].length() && fields[3].length() <= 15;
    for (size_t i = 0; i < fields[3].length(); ++i) {
      const char c = fields[3][i];
      validName = validName && (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_');
    }
    if (plan_.count >= AppConfig::GEO_MAX_POINTS || !number(fields[0], lat) || !number(fields[1], lon) || !coordinates(lat, lon) ||
        !number(fields[2], tolerance) || tolerance < 0.1 || tolerance > 1000 || !validName) {
      report(EventLevel::ERROR, "invalid point; max " + String(AppConfig::GEO_MAX_POINTS) + ", radius 0.1-1000 m, valid routine name required", source, requestId);
    } else {
      Point &point = plan_.points[plan_.count++]; point.latitude = lat; point.longitude = lon; point.tolerance = tolerance;
      fields[3].toCharArray(point.routine, sizeof(point.routine)); saved_ = false;
    }
    return true;
  }
  report(EventLevel::ERROR, "unknown Geo command", source, requestId); return true;
}

void GeoMission::pollMavlink() {
#if APP_WIFI_ENABLED
  if (plan_.source != 0) { if (udpReady_) udp_.stop(); udpReady_ = false; return; }
  if (WiFi.status() != WL_CONNECTED) { udp_.stop(); udpReady_ = false; return; }
  if (!udpReady_) { udpReady_ = udp_.begin(AppConfig::GEO_MAVLINK_UDP_PORT); if (!udpReady_) return; }
  // Bounded datagrams; the configured UDP peer must send whole MAVLink frames.
  for (uint8_t packet = 0; packet < 4; ++packet) {
    const int size = udp_.parsePacket(); if (size <= 0) break;
    uint8_t bytes[1024];
    if (size > static_cast<int>(sizeof(bytes))) { udp_.clear(); continue; }
    const int length = udp_.read(bytes, sizeof(bytes)); if (length <= 0) continue;
    size_t offset = 0; MavlinkPosition::Message message{};
    while (MavlinkPosition::next(bytes, length, offset, message)) {
      if (message.system != AppConfig::GEO_MAVLINK_SYSTEM_ID || message.component != AppConfig::GEO_MAVLINK_COMPONENT_ID) continue;
      if (message.id == 24) {
        fixType_ = message.fixType; gpsAt_ = millis();
        mavlinkAccuracy_ = message.accuracyMm ? message.accuracyMm / 1000.0f : -1;
        if (fixType_ < 3 || fixType_ > 6) havePosition_ = false;
      } else if (fixType_ >= 3 && fixType_ <= 6 && millis() - gpsAt_ <= FIX_TIMEOUT_MS) {
        if (haveBootTime_ && static_cast<int32_t>(message.bootMs - lastBootMs_) <= 0) continue;
        lastBootMs_ = message.bootMs; haveBootTime_ = true;
        position(message.latitudeE7 / 1e7, message.longitudeE7 / 1e7, mavlinkAccuracy_, 0);
      }
    }
  }
#endif
}
void GeoMission::service() {
  InputFix fix{};
  if (positionQueue_ && xQueueReceive(positionQueue_, &fix, 0) == pdPASS && plan_.source == 1) {
    const uint32_t queueAge = millis() - fix.receivedAt;
    if (!fix.valid || queueAge > FIX_TIMEOUT_MS || fix.ageMs > FIX_TIMEOUT_MS - queueAge) havePosition_ = false;
    else position(fix.latitude, fix.longitude, fix.accuracy, fix.ageMs + queueAge);
  }
  pollMavlink();
  if (!fresh()) havePosition_ = false;
  if (!active_) return;
  if (!fresh()) { stop("position stale; sequence stopped"); return; }
  if (runningRoutine_) {
    if (routines_.isActive()) return;
    if (!routines_.lastRunSucceeded()) { stop("routine failed or stopped; sequence stopped"); return; }
    runningRoutine_ = false; ++next_;
    if (next_ >= plan_.count) { active_ = false; result_ = "sequence complete"; report(EventLevel::STATUS, result_); return; }
  }
  const Point &point = plan_.points[next_];
  if (accuracy_ >= 0 && accuracy_ > point.tolerance) return;
  if (distance(point) > point.tolerance) return;
  // An explicit GeoStart grants per-point arming, through the normal addon's
  // interlock/fault checks. Waiting between points does not energize output.
  addon_.handleCommand("Arm", CommandSource::INTERNAL, "geo-arm");
  if (!routines_.runNamed(point.routine, CommandSource::INTERNAL, "geo-run")) {
    stop("point routine could not start"); return;
  }
  runningRoutine_ = true; result_ = "running coordinate " + String(next_ + 1);
  report(EventLevel::WARNING, result_ + " routine=" + point.routine);
}
uint32_t GeoMission::checksum(const Plan &plan) const {
  return recordChecksum(&plan, offsetof(Plan, checksum));
}
bool GeoMission::validate(const Plan &plan) const {
  if (plan.magic != MAGIC || plan.count > AppConfig::GEO_MAX_POINTS || plan.source > 1 || plan.checksum != checksum(plan)) return false;
  for (uint16_t i = 0; i < plan.count; ++i) {
    const Point &point = plan.points[i];
    if (!coordinates(point.latitude, point.longitude) || !isfinite(point.tolerance) || point.tolerance < 0.1 || point.tolerance > 1000 || point.routine[15] != '\0' || !point.routine[0]) return false;
  } return true;
}
bool GeoMission::load() {
  Preferences preferences; if (!preferences.begin("drone-geo", true)) return false;
  // Load into persistent RAM; the expanded plan must never live on a task stack.
  const size_t length = preferences.getBytesLength("plan");
  bool ok = length == sizeof(plan_) && preferences.getBytes("plan", &plan_, sizeof(plan_)) == sizeof(plan_);
  struct LegacyPlan { uint32_t magic; uint8_t count, source; Point points[12]; uint32_t checksum; };
  if (length == sizeof(LegacyPlan)) {
    LegacyPlan old{};
    if (preferences.getBytes("plan", &old, sizeof(old)) == sizeof(old) && old.magic == 0x47454F31 &&
        old.count <= 12 && old.source <= 1 && old.checksum == recordChecksum(&old, offsetof(LegacyPlan, checksum))) {
      memset(&plan_, 0, sizeof(plan_)); plan_.magic = MAGIC; plan_.count = old.count; plan_.source = old.source;
      memcpy(plan_.points, old.points, sizeof(old.points)); plan_.checksum = checksum(plan_); ok = true;
    }
  }
  preferences.end(); ok = ok && validate(plan_);
  if (!ok) { memset(&plan_, 0, sizeof(plan_)); plan_.magic = MAGIC; }
  return ok;
}
bool GeoMission::save() {
  plan_.checksum = checksum(plan_); if (!validate(plan_)) return false;
  Preferences preferences; if (!preferences.begin("drone-geo", false)) return false;
  const bool written = preferences.putBytes("plan", &plan_, sizeof(plan_)) == sizeof(plan_);
  auto *readback = written ? static_cast<Plan *>(malloc(sizeof(Plan))) : nullptr;
  const bool verified = readback && preferences.getBytes("plan", readback, sizeof(Plan)) == sizeof(Plan) &&
    validate(*readback) && memcmp(readback, &plan_, sizeof(plan_)) == 0;
  free(readback);
  preferences.end(); return verified;
}
String GeoMission::stateJson(bool compact) const {
  char fields[256];
  snprintf(fields, sizeof(fields), "{\"active\":%s,\"saved\":%s,\"source\":\"%s\",\"next\":%u,\"count\":%u,\"fresh\":%s,\"running\":%s,\"capacity\":%u",
    active_ ? "true" : "false", saved_ ? "true" : "false", plan_.source == 1 ? "API" : "MAVLINK",
    static_cast<unsigned>(next_), static_cast<unsigned>(plan_.count), fresh() ? "true" : "false", runningRoutine_ ? "true" : "false", static_cast<unsigned>(AppConfig::GEO_MAX_POINTS));
  String json(fields);
  if (!compact) {
    snprintf(fields, sizeof(fields), ",\"latitude\":%.7f,\"longitude\":%.7f,\"accuracy\":%.2f,\"result\":\"", latitude_, longitude_, accuracy_);
    json += fields; json += TextUtil::jsonEscape(result_); json += '"';
  }
  if (next_ < plan_.count && fresh()) {
    snprintf(fields, sizeof(fields), ",\"distance\":%.1f", distance(plan_.points[next_])); json += fields;
  }
  json += "}"; return json;
}
