#ifndef ESP32_STEPPER_DUAL_DAC_RADIOMANAGER_H
#define ESP32_STEPPER_DUAL_DAC_RADIOMANAGER_H

#include <Arduino.h>

#include "../config/AppConfig.h"
#include <Preferences.h>

#if APP_WIFI_ENABLED
#include <DNSServer.h>
#if APP_MDNS_ENABLED
#include <ESPmDNS.h>
#endif
#include <WiFi.h>
#else
enum wifi_mode_t : uint8_t {
  WIFI_OFF = 0,
  WIFI_STA = 1,
  WIFI_AP = 2,
  WIFI_AP_STA = 3
};
using wifi_power_t = int8_t;
constexpr int WL_CONNECTED = 3;

class DisabledIpAddress {
 public:
  String toString() const { return "0.0.0.0"; }
};

class DisabledWiFiClass {
 public:
  void persistent(bool enabled) { (void)enabled; }
  bool mode(wifi_mode_t modeValue) { (void)modeValue; return false; }
  wifi_mode_t getMode() const { return WIFI_OFF; }
  bool softAP(const char *ssid, const char *password) {
    (void)ssid;
    (void)password;
    return false;
  }
  DisabledIpAddress softAPIP() const { return DisabledIpAddress(); }
  uint8_t softAPgetStationNum() const { return 0; }
  bool softAPdisconnect(bool powerOff) { (void)powerOff; return true; }
  void disconnect(bool eraseCredentials) { (void)eraseCredentials; }
  void setAutoReconnect(bool enabled) { (void)enabled; }
  void begin(const char *ssid, const char *password) {
    (void)ssid;
    (void)password;
  }
  int status() const { return 0; }
  DisabledIpAddress localIP() const { return DisabledIpAddress(); }
  int32_t RSSI() const { return 0; }
  bool setTxPower(wifi_power_t power) { (void)power; return false; }
  bool setSleep(bool enabled) { (void)enabled; return false; }
};

extern DisabledWiFiClass WiFi;

class DNSServer {
 public:
  bool start(uint16_t port, const char *domain, const DisabledIpAddress &address) {
    (void)port;
    (void)domain;
    (void)address;
    return false;
  }
  void stop() {}
  void processNextRequest() {}
};
#endif

#include "../core/AppTypes.h"
#include "../core/EventBus.h"
#include "../transports/TransportHub.h"

class WebPortal;

// EDIT HERE for Wi-Fi modes, credentials, TX power, persistence, or Bluetooth
// policy. The boot radio profile owns which heavyweight stack is initialized.
// WIFI_BLE pauses idle advertising while Wi-Fi is active. WIFI_BLE_P keeps
// Wi-Fi and BLE persistent for coexistence testing. Neither mode tears down
// Bluetooth and tries to rebuild it in the same boot.
class RadioManager {
 public:
  enum class WifiRole : uint8_t {
    AP = 0,
    STA = 1,
    AP_STA = 2
  };

  enum class BootRadioMode : uint8_t {
    WIFI = 0,
    WIFI_BLE = 1,
    BLE = 2,
    SPP = 3,
    USB = 4,
    WIFI_BLE_P = 5
  };

  RadioManager(
    EventBus &events,
    TransportHub &transports,
    WebPortal &webPortal
  );

  void begin();
  void service();
  bool handleCommand(
    const String &command,
    CommandSource source,
    const String &requestId
  );

  bool hasUserConnection() const;
  bool isSeekingConnection() const;
  String statusText() const;
  String stateJson() const;
  String webStateJson() const;
  String activeBootModeText() const;
  uint8_t activeBootModeValue() const;
  bool isCombinedWifiBleProfile() const;
  bool isPersistentWifiBleProfile() const;
  bool bootTransactionBusy() const;
  bool activeProfileHealthy(String &reason) const;

 private:
  struct Config {
    BootRadioMode bootMode;
    bool wifiEnabled;
    bool bleEnabled;
    bool sppEnabled;
    bool fallbackAp;
    WifiRole wifiRole;
    int8_t wifiTxPowerQuarterDbm;
    String staSsid;
    String staPassword;
    String apSsid;
    String apPassword;
  };

  void setDefaults();
  void normalizeBootModeFlags();
  void selectBootModeFromFlags();
  bool parseBootMode(const String &value, BootRadioMode &mode) const;
  bool bootModeAvailable(BootRadioMode mode) const;
  BootRadioMode fallbackBootMode() const;
  String bootModeString(BootRadioMode mode) const;
  bool bootModeUsesWifi(BootRadioMode mode) const;
  bool bootModeUsesBle(BootRadioMode mode) const;
  bool bootModeUsesSpp(BootRadioMode mode) const;
  bool bootModeNeedsReboot() const;
  bool initializeBootBluetooth();

  void prepareBootTransaction();
  bool beginBootModeSwitch(
    BootRadioMode mode,
    CommandSource source,
    const String &requestId
  );
  bool writeBootTransaction(BootRadioMode pendingMode);
  bool clearBootTransaction(BootRadioMode knownGoodMode);
  bool commitBootTransaction();
  void rollbackBootTransaction(const String &reason);
  void serviceBootTransaction();
  bool trialStartupReady(String &reason) const;
  bool bootProfileHealthy(String &reason) const;
  bool trialMemoryHealthy(String &reason) const;
  void scheduleManagedReboot(const String &reason);

  bool loadSettings(bool announce, CommandSource source, const String &requestId);
  bool saveSettings();
  bool eraseSettings();
  bool clearStoredStationCredentials();
  bool validateConfig(String &reason) const;
  bool hasUsableStationCredentials() const;
  bool isDummyCredential(const String &value) const;

  void scheduleApply(bool restartWifi);
  void applyNow(bool restartWifi);
  void startWifi();
  void startAccessPoint(bool fallback);
  void startStation();
  void startApSta();
  bool beginAccessPoint(bool combinedMode);
  void stopWifi();
  void serviceWifiState();
  void serviceRadioHandoff();
  void applyWifiTxPower();
  void startMdns();
  void stopMdns();

  bool parseWifiPower(const String &value, int8_t &quarterDbm) const;
  String wifiPowerString(int8_t quarterDbm) const;
  String roleString() const;
  String runtimeState() const;
  String ipString() const;
  void publishConfigReadback(
    CommandSource source,
    const String &requestId,
    const String &reason
  );

  void publish(
    EventLevel level,
    CommandSource source,
    const String &requestId,
    const String &message
  );
  void error(CommandSource source, const String &requestId, const String &message);

  EventBus &events_;
  TransportHub &transports_;
  WebPortal &webPortal_;
  Config config_;
  BootRadioMode activeBootMode_;

  DNSServer dns_;
  bool dnsRunning_;
  bool mdnsRunning_;
  bool wifiRunning_;
  bool wifiStartAttempted_;
  bool wifiStartSucceeded_;
  bool apActive_;
  bool stationAttempted_;
  bool fallbackActive_;
  bool stationConnectedAnnounced_;
  bool stationTimeoutAnnounced_;
  uint32_t stationConnectStartedAtMs_;

  bool bleDormantForWifi_;
  bool bleWebHandoffArmed_;
  uint32_t bleWebHandoffDeadlineMs_;
  uint32_t wifiClientLastSeenAtMs_;
  bool wifiClientPreviouslyConnected_;
  bool coexHttpNoRequestWarned_;
  uint32_t wifiClientAssociatedAtMs_;
  uint32_t lastCoexHttpDiagnosticAtMs_;
  uint32_t lastObservedHttpRequestCount_;

  bool applyPending_;
  bool restartWifiPending_;
  uint32_t applyAtMs_;

  BootRadioMode lastGoodBootMode_;
  BootRadioMode pendingBootMode_;
  bool trialBootActive_;
  bool bootBluetoothInitFailed_;
  String bootBluetoothFailureReason_;
  uint32_t trialStartedAtMs_;
  uint32_t trialValidationStartedAtMs_;
  uint32_t trialHealthySinceMs_;

  bool managedRebootPending_;
  uint32_t managedRebootAtMs_;
  String managedRebootReason_;
};

#endif  // ESP32_STEPPER_DUAL_DAC_RADIOMANAGER_H
