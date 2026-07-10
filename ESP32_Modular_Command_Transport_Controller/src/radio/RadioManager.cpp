#include "RadioManager.h"

#include "../util/TextUtil.h"
#include "../web/WebPortal.h"

#if !APP_WIFI_ENABLED
DisabledWiFiClass WiFi;
#endif

namespace {

constexpr const char *RADIO_PREFERENCES_NAMESPACE = "step-radio";
constexpr const char *KEY_TXN_PENDING = "txnpending";
constexpr const char *KEY_TXN_STARTED = "txnstarted";
constexpr const char *KEY_TXN_PROFILE = "txnprofile";
constexpr const char *KEY_LAST_GOOD = "lastgood";
constexpr const char *KEY_TXN_SEQUENCE = "txnseq";

}  // namespace

RadioManager::RadioManager(
  EventBus &events,
  TransportHub &transports,
  WebPortal &webPortal
) : events_(events),
    transports_(transports),
    webPortal_(webPortal),
    config_{},
    activeBootMode_(BootRadioMode::WIFI),
    dnsRunning_(false),
    mdnsRunning_(false),
    wifiRunning_(false),
    wifiStartAttempted_(false),
    wifiStartSucceeded_(false),
    apActive_(false),
    stationAttempted_(false),
    fallbackActive_(false),
    stationConnectedAnnounced_(false),
    stationTimeoutAnnounced_(false),
    stationConnectStartedAtMs_(0),
    bleDormantForWifi_(false),
    bleWebHandoffArmed_(false),
    bleWebHandoffDeadlineMs_(0),
    wifiClientLastSeenAtMs_(0),
    wifiClientPreviouslyConnected_(false),
    coexHttpNoRequestWarned_(false),
    wifiClientAssociatedAtMs_(0),
    lastCoexHttpDiagnosticAtMs_(0),
    lastObservedHttpRequestCount_(0),
    applyPending_(false),
    restartWifiPending_(false),
    applyAtMs_(0),
    lastGoodBootMode_(BootRadioMode::WIFI),
    pendingBootMode_(BootRadioMode::WIFI),
    trialBootActive_(false),
    bootBluetoothInitFailed_(false),
    bootBluetoothFailureReason_(),
    trialStartedAtMs_(0),
    trialValidationStartedAtMs_(0),
    trialHealthySinceMs_(0),
    managedRebootPending_(false),
    managedRebootAtMs_(0),
    managedRebootReason_() {
  setDefaults();
}

void RadioManager::begin() {
  loadSettings(false, CommandSource::INTERNAL, String());
  normalizeBootModeFlags();
  prepareBootTransaction();

  Serial.println(
    "[BOOT][RADIO] active boot profile=" + bootModeString(activeBootMode_)
  );
  Serial.flush();

  if (bootModeUsesWifi(activeBootMode_)) {
    Serial.println("[BOOT][HTTP] reserving portal memory before radio initialization");
    Serial.flush();
    webPortal_.prepare();
  }

  bootBluetoothInitFailed_ = false;
  bootBluetoothFailureReason_ = "";
  const bool coexColdStart = isCombinedWifiBleProfile();

  if (coexColdStart) {
    Serial.println(
      "[BOOT][RADIO][COEX] staged startup=portal reserve, BLE controller, WiFi/HTTP, BLE GATT core=" +
        String(xPortGetCoreID())
    );
    Serial.flush();
    if (!transports_.prepareBleController()) {
      bootBluetoothInitFailed_ = true;
      bootBluetoothFailureReason_ = "BLE controller reservation failed before WiFi startup";
    }
  } else if (!initializeBootBluetooth()) {
    bootBluetoothInitFailed_ = true;
    if (bootBluetoothFailureReason_.length() == 0) {
      bootBluetoothFailureReason_ = "Bluetooth transport did not enter the requested runtime state";
    }
  }

  applyPending_ = false;
  restartWifiPending_ = false;
  applyAtMs_ = 0;

  Serial.println(
    "[BOOT][RADIO] applying WiFi state on Arduino setup core=" +
      String(xPortGetCoreID()) +
      " heap=" + String(ESP.getFreeHeap()) +
      " largest=" + String(ESP.getMaxAllocHeap())
  );
  Serial.flush();

  // On a cold coexistence boot no WiFi interface exists yet. Avoid the normal
  // disconnect/WIFI_OFF cycle and use the same AP and HTTP startup path as the
  // proven WiFi-only profile.
  applyNow(!coexColdStart);

  if (coexColdStart && !bootBluetoothInitFailed_) {
    Serial.println(
      "[BOOT][RADIO][COEX] WiFi/HTTP ready; creating BLE GATT service heap=" +
        String(ESP.getFreeHeap()) +
        " largest=" + String(ESP.getMaxAllocHeap())
    );
    Serial.flush();
    transports_.setBleEnabled(true);
    if (!transports_.isBleInitialized() || !transports_.isBleRunning()) {
      bootBluetoothInitFailed_ = true;
      bootBluetoothFailureReason_ = "BLE GATT startup returned without a running BLE transport";
    }
  }

  if (coexColdStart) {
    Serial.println(
      "[BOOT][RADIO][COEX] staged initialization complete BLE=" +
        TextUtil::boolWord(transports_.isBleRunning()) +
        " WiFi=" + TextUtil::boolWord(wifiRunning_) +
        " web=" + TextUtil::boolWord(webPortal_.isRunning()) +
        " heap=" + String(ESP.getFreeHeap()) +
        " largest=" + String(ESP.getMaxAllocHeap())
    );
    Serial.flush();
  }
}

void RadioManager::service() {
  if (dnsRunning_) {
    dns_.processNextRequest();
  }

  serviceWifiState();

  if (applyPending_ && static_cast<int32_t>(millis() - applyAtMs_) >= 0) {
    const bool restartWifi = restartWifiPending_;
    applyPending_ = false;
    restartWifiPending_ = false;
    applyNow(restartWifi);
  }

  serviceRadioHandoff();
  serviceBootTransaction();

  if (
    managedRebootPending_ &&
    static_cast<int32_t>(millis() - managedRebootAtMs_) >= 0
  ) {
    managedRebootPending_ = false;
    Serial.println("[RADIO][REBOOT] " + managedRebootReason_);
    Serial.flush();
    delay(40);
    ESP.restart();
  }
}

// ADD RADIO OR PERSISTENCE COMMANDS HERE. Update desired config first, validate
// it, then schedule hardware changes through scheduleApply().
bool RadioManager::handleCommand(
  const String &command,
  CommandSource source,
  const String &requestId
) {
  String work = command;
  work.trim();

  if (work.equalsIgnoreCase("RadioStatus")) {
    publishConfigReadback(source, requestId, "status");
    String webStatus = "[WEB] compiled=";
    webStatus += TextUtil::boolWord(AppConfig::ENABLE_WIFI);
    webStatus += " running=";
    webStatus += TextUtil::boolWord(webPortal_.isRunning());
    webStatus += " port=80 dns=";
    webStatus += TextUtil::boolWord(dnsRunning_);
    webStatus += " url=http://";
    webStatus += apActive_ ? WiFi.softAPIP().toString() : ipString();
    webStatus += "/";
    publish(EventLevel::STATUS, source, requestId, webStatus);
    return true;
  }

  BootRadioMode requestedBootMode = BootRadioMode::WIFI;
  bool hasDirectBootMode = false;
  if (work.equalsIgnoreCase("ModeWiFi")) {
    requestedBootMode = BootRadioMode::WIFI;
    hasDirectBootMode = true;
  } else if (work.equalsIgnoreCase("ModeWiFiBLE")) {
    requestedBootMode = BootRadioMode::WIFI_BLE;
    hasDirectBootMode = true;
  } else if (work.equalsIgnoreCase("ModeWiFiBLEP")) {
    requestedBootMode = BootRadioMode::WIFI_BLE_P;
    hasDirectBootMode = true;
  } else if (work.equalsIgnoreCase("ModeBLE")) {
    requestedBootMode = BootRadioMode::BLE;
    hasDirectBootMode = true;
  } else if (work.equalsIgnoreCase("ModeBTSerial")) {
    requestedBootMode = BootRadioMode::SPP;
    hasDirectBootMode = true;
  } else if (work.equalsIgnoreCase("ModeUSB")) {
    requestedBootMode = BootRadioMode::USB;
    hasDirectBootMode = true;
  }

  if (hasDirectBootMode && !bootModeAvailable(requestedBootMode)) {
    error(
      source,
      requestId,
      "radio profile " + bootModeString(requestedBootMode) +
        " is compiled out; edit the build switches in src/config/AppConfig.h and rebuild"
    );
    return true;
  }

  if (hasDirectBootMode || TextUtil::startsWithIgnoreCase(work, "RadioBoot:")) {
    if (!hasDirectBootMode) {
      const int colon = work.indexOf(':');
      if (
        colon < 0 ||
        !parseBootMode(
          work.substring(static_cast<unsigned int>(colon) + 1U),
          requestedBootMode
        )
      ) {
        error(source, requestId, "RadioBoot expects WIFI, WIFI_BLE, WIFI_BLE_P, BLE, SPP, or USB");
        return true;
      }
    }
    beginBootModeSwitch(requestedBootMode, source, requestId);
    return true;
  }
  if (work.equalsIgnoreCase("BLEWebHandoff")) {
    if (!isCombinedWifiBleProfile()) {
      error(source, requestId, "BLE web handoff requires WIFI_BLE or WIFI_BLE_P");
      return true;
    }
    if (!transports_.isBleInitialized() || !transports_.isBleRunning()) {
      error(source, requestId, "BLE transport is not running");
      return true;
    }

    bleWebHandoffArmed_ = true;
    bleWebHandoffDeadlineMs_ = millis() + AppConfig::BLE_WEB_HANDOFF_WINDOW_MS;
    transports_.setBleDormant(false);
    bleDormantForWifi_ = false;
    publish(
      EventLevel::STATUS,
      source,
      requestId,
      "[ACK] BLE browser handoff window open for " +
        String(AppConfig::BLE_WEB_HANDOFF_WINDOW_MS / 1000U) +
        "s name=" + String(AppConfig::BLE_NAME) +
        " service=" + String(AppConfig::BLE_UART_SERVICE_UUID) +
        "; connect BLE now; WiFi and HTTP remain available under native coexistence"
    );
    return true;
  }

  if (work.equalsIgnoreCase("BLEWebCancel")) {
    bleWebHandoffArmed_ = false;
    bleWebHandoffDeadlineMs_ = 0;
    publish(EventLevel::STATUS, source, requestId, "BLE browser handoff window closed");
    return true;
  }

  if (work.equalsIgnoreCase("WebRestart")) {
    if (!bootModeUsesWifi(activeBootMode_)) {
      error(source, requestId, "WiFi is not part of the active boot profile; select WIFI, WIFI_BLE, or WIFI_BLE_P, save, and reboot");
      return true;
    }
    publish(
      EventLevel::STATUS,
      source,
      requestId,
      "[ACK] WiFi interface restart scheduled; HTTP listener remains active and saved configuration is unchanged"
    );
    scheduleApply(true);
    return true;
  }
  if (work.equalsIgnoreCase("ConfigApply")) {
    if (managedRebootPending_) {
      publish(
        EventLevel::STATUS,
        source,
        requestId,
        "radio profile reboot already armed; runtime apply skipped"
      );
      return true;
    }
    String reason;
    if (!validateConfig(reason)) {
      error(source, requestId, reason);
      return true;
    }
    publish(
      EventLevel::STATUS,
      source,
      requestId,
      bootModeNeedsReboot()
        ? "WiFi settings apply scheduled for the active profile; boot profile change remains pending until reboot"
        : "radio configuration apply scheduled"
    );
    publishConfigReadback(source, requestId, "apply request");
    scheduleApply(true);
    return true;
  }
  if (work.equalsIgnoreCase("ConfigSave")) {
    String reason;
    if (!validateConfig(reason)) {
      error(source, requestId, reason);
      return true;
    }

    if (managedRebootPending_) {
      config_.bootMode = pendingBootMode_;
      normalizeBootModeFlags();
      if (saveSettings()) {
        publish(
          EventLevel::STATUS,
          source,
          requestId,
          "[ACK] configuration is saved and verified; the existing radio-mode reboot remains scheduled"
        );
        publishConfigReadback(source, requestId, "saved while reboot pending");
      } else {
        error(source, requestId, "could not verify configuration while the radio-mode reboot is pending");
      }
      return true;
    }

    if (trialBootActive_) {
      config_.bootMode = activeBootMode_;
      normalizeBootModeFlags();
      if (saveSettings()) {
        publish(
          EventLevel::STATUS,
          source,
          requestId,
          "[ACK] configuration saved and verified; the active radio trial remains armed until [BOOT][RADIO][OK]"
        );
        publishConfigReadback(source, requestId, "saved during radio trial");
      } else {
        error(source, requestId, "could not verify configuration during the active radio trial");
      }
      return true;
    }

    if (bootModeNeedsReboot()) {
      beginBootModeSwitch(config_.bootMode, source, requestId);
      return true;
    }
    if (saveSettings()) {
      publish(EventLevel::STATUS, source, requestId, "radio configuration saved to NVS and verified");
      publishConfigReadback(source, requestId, "saved");
    } else {
      error(source, requestId, "could not save radio configuration");
    }
    return true;
  }
  if (work.equalsIgnoreCase("ConfigLoad")) {
    // loadSettings() restores safe defaults when stored data is invalid. Wi-Fi
    // settings can be reapplied now, while a boot-profile change remains pending
    // until reboot so Bluetooth is never torn down and rebuilt in-place.
    loadSettings(true, source, requestId);
    publishConfigReadback(source, requestId, "loaded");
    scheduleApply(true);
    return true;
  }
  if (work.equalsIgnoreCase("ConfigDefaults")) {
    setDefaults();
    publish(EventLevel::STATUS, source, requestId, "volatile radio defaults restored");
    publishConfigReadback(source, requestId, "defaults restored");
    scheduleApply(true);
    return true;
  }
  if (work.equalsIgnoreCase("ConfigErase")) {
    if (eraseSettings()) {
      publish(EventLevel::STATUS, source, requestId, "saved radio configuration erased; volatile settings unchanged");
      publishConfigReadback(source, requestId, "NVS erased");
    } else {
      error(source, requestId, "could not erase radio configuration");
    }
    return true;
  }

  bool enabled = false;
  if (TextUtil::startsWithIgnoreCase(work, "WiFi:")) {
    if (!TextUtil::parseOnOff(work.substring(5), enabled)) {
      error(source, requestId, "WiFi expects ON or OFF");
      return true;
    }
    if (enabled && !AppConfig::ENABLE_WIFI) {
      error(source, requestId, "WiFi is compiled out; uncomment APP_ENABLE_WIFI in src/config/AppConfig.h and rebuild");
      return true;
    }
    config_.wifiEnabled = enabled;
    if (enabled) {
      config_.sppEnabled = false;
    }
    selectBootModeFromFlags();
    publish(EventLevel::STATUS, source, requestId, "wifiDesired=" + TextUtil::boolWord(config_.wifiEnabled));
    publishConfigReadback(source, requestId, "WiFi boot preference updated");
    return true;
  }

  if (TextUtil::startsWithIgnoreCase(work, "BLE:")) {
    if (!TextUtil::parseOnOff(work.substring(4), enabled)) {
      error(source, requestId, "BLE expects ON or OFF");
      return true;
    }
    if (enabled && !AppConfig::ENABLE_BLE) {
      error(source, requestId, "BLE is compiled out; uncomment APP_ENABLE_BLE in src/config/AppConfig.h and rebuild");
      return true;
    }
    config_.bleEnabled = enabled;
    if (enabled) {
      config_.sppEnabled = false;
    }
    selectBootModeFromFlags();
    publish(EventLevel::STATUS, source, requestId, "bleDesired=" + TextUtil::boolWord(config_.bleEnabled));
    publishConfigReadback(source, requestId, "BLE boot preference updated");
    return true;
  }

  if (TextUtil::startsWithIgnoreCase(work, "ClassicBT:") || TextUtil::startsWithIgnoreCase(work, "SPP:")) {
    const int colon = work.indexOf(':');
    if (colon < 0 || !TextUtil::parseOnOff(
        work.substring(static_cast<unsigned int>(colon) + 1U), enabled)) {
      error(source, requestId, "ClassicBT expects ON or OFF");
      return true;
    }
    if (enabled && !AppConfig::ENABLE_CLASSIC_BT_SPP) {
      error(
        source,
        requestId,
        "Classic Bluetooth SPP is compiled out; uncomment APP_ENABLE_CLASSIC_BT_SPP in src/config/AppConfig.h and rebuild"
      );
      return true;
    }
    config_.sppEnabled = enabled;
    if (enabled) {
      config_.wifiEnabled = false;
      config_.bleEnabled = false;
    }
    selectBootModeFromFlags();
    publish(EventLevel::STATUS, source, requestId, "sppDesired=" + TextUtil::boolWord(config_.sppEnabled));
    publishConfigReadback(source, requestId, "Classic Bluetooth boot preference updated");
    return true;
  }

  if (TextUtil::startsWithIgnoreCase(work, "WiFiMode:")) {
    String value = work.substring(9);
    value.trim();
    if (value.equalsIgnoreCase("AP")) {
      config_.wifiRole = WifiRole::AP;
    } else if (value.equalsIgnoreCase("STA")) {
      config_.wifiRole = WifiRole::STA;
    } else if (value.equalsIgnoreCase("APSTA")) {
      config_.wifiRole = WifiRole::AP_STA;
    } else {
      error(source, requestId, "WiFiMode expects AP, STA, or APSTA");
      return true;
    }
    publish(EventLevel::STATUS, source, requestId, "wifiMode=" + roleString());
    publishConfigReadback(source, requestId, "WiFi mode updated");
    return true;
  }

  if (TextUtil::startsWithIgnoreCase(work, "WiFiFallbackAP:")) {
    if (!TextUtil::parseOnOff(work.substring(15), enabled)) {
      error(source, requestId, "WiFiFallbackAP expects ON or OFF");
      return true;
    }
    config_.fallbackAp = enabled;
    publish(EventLevel::STATUS, source, requestId, "wifiFallbackAP=" + TextUtil::boolWord(enabled));
    publishConfigReadback(source, requestId, "fallback AP updated");
    return true;
  }

  if (TextUtil::startsWithIgnoreCase(work, "WiFiTxPower:")) {
    int8_t parsed = 0;
    if (!parseWifiPower(work.substring(12), parsed)) {
      error(source, requestId, "unsupported WiFi TX power; use LOW=11, MAX=19.5, or 19.5,19,18.5,17,15,13,11,8.5,7,5,2,-1 dBm");
      return true;
    }
    config_.wifiTxPowerQuarterDbm = parsed;
    if (wifiRunning_) {
      applyWifiTxPower();
    }
    publish(EventLevel::STATUS, source, requestId, "wifiTxPowerDbm=" + wifiPowerString(parsed));
    publishConfigReadback(source, requestId, "WiFi TX power updated");
    return true;
  }

  if (TextUtil::startsWithIgnoreCase(work, "WiFiStaSSID:")) {
    String value = work.substring(12);
    value.trim();
    if (value.length() > 32) {
      error(source, requestId, "station SSID must be 32 bytes or fewer");
      return true;
    }
    config_.staSsid = value;
    publish(EventLevel::STATUS, source, requestId, "station SSID updated");
    publishConfigReadback(source, requestId, "station SSID updated");
    return true;
  }
  if (TextUtil::startsWithIgnoreCase(work, "WiFiStaPassword:")) {
    const String value = work.substring(16);
    if (value.length() > 0 && (value.length() < 8 || value.length() > 63)) {
      error(source, requestId, "station password must be blank or 8 to 63 characters");
      return true;
    }
    config_.staPassword = value;
    publish(EventLevel::STATUS, source, requestId, "station password updated; plaintext not echoed");
    publishConfigReadback(source, requestId, "station password updated");
    return true;
  }
  if (TextUtil::startsWithIgnoreCase(work, "WiFiApSSID:")) {
    String value = work.substring(11);
    value.trim();
    if (value.length() > 32) {
      error(source, requestId, "access-point SSID must be 32 bytes or fewer");
      return true;
    }
    config_.apSsid = value;
    publish(EventLevel::STATUS, source, requestId, "access-point SSID updated");
    publishConfigReadback(source, requestId, "access-point SSID updated");
    return true;
  }
  if (TextUtil::startsWithIgnoreCase(work, "WiFiApPassword:")) {
    const String value = work.substring(15);
    if (value.length() > 0 && (value.length() < 8 || value.length() > 63)) {
      error(source, requestId, "access-point password must be blank or 8 to 63 characters");
      return true;
    }
    config_.apPassword = value;
    publish(EventLevel::STATUS, source, requestId, "access-point password updated; plaintext not echoed");
    publishConfigReadback(source, requestId, "access-point password updated");
    return true;
  }

  if (work.equalsIgnoreCase("WiFiStaClear")) {
    config_.staSsid = "";
    config_.staPassword = "";
    if (!clearStoredStationCredentials()) {
      error(source, requestId, "station credentials cleared in RAM but NVS could not be updated");
    } else {
      publish(EventLevel::STATUS, source, requestId, "station credentials cleared from RAM and NVS");
      publishConfigReadback(source, requestId, "station credentials cleared");
    }
    scheduleApply(true);
    return true;
  }

  if (work.equalsIgnoreCase("BleAdvertise")) {
    if (!bootModeUsesBle(activeBootMode_)) {
      error(source, requestId, "BLE is not part of the active boot profile; select BLE, WIFI_BLE, or WIFI_BLE_P, save, and reboot");
    } else {
      transports_.restartBleAdvertising();
      bleDormantForWifi_ = false;
      publish(EventLevel::STATUS, source, requestId, "BLE advertising restarted manually");
    }
    return true;
  }

  return false;
}

bool RadioManager::hasUserConnection() const {
  const bool bleConnected = transports_.isBleConnected();
  const bool stationConnected =
    wifiRunning_ && stationAttempted_ && WiFi.status() == WL_CONNECTED;
  const bool accessPointClient =
    wifiRunning_ && apActive_ && WiFi.softAPgetStationNum() > 0;
  return bleConnected || stationConnected || accessPointClient;
}

bool RadioManager::isSeekingConnection() const {
  if (hasUserConnection()) {
    return false;
  }
  const bool bleSeeking = transports_.isBleRunning();
  const bool wifiSeeking =
    wifiRunning_ &&
    (apActive_ || (stationAttempted_ && WiFi.status() != WL_CONNECTED));
  return bleSeeking || wifiSeeking;
}

String RadioManager::statusText() const {
  const bool stationConnected =
    wifiRunning_ && stationAttempted_ && WiFi.status() == WL_CONNECTED;
  String text = "bootDesired=" + bootModeString(config_.bootMode);
  text += " bootActive=" + bootModeString(activeBootMode_);
  text += " rebootRequired=" + TextUtil::boolWord(bootModeNeedsReboot());
  text += " bootTrial=" + TextUtil::boolWord(trialBootActive_);
  text += " lastGood=" + bootModeString(lastGoodBootMode_);
  text += " pendingProfile=" + bootModeString(pendingBootMode_);
  text += " managedReboot=" + TextUtil::boolWord(managedRebootPending_);
  text += " wifiCompiled=" + TextUtil::boolWord(AppConfig::ENABLE_WIFI);
  text += " bleCompiled=" + TextUtil::boolWord(AppConfig::ENABLE_BLE);
  text += " wifiDesired=" + TextUtil::boolWord(config_.wifiEnabled);
  text += " wifiRuntime=" + TextUtil::boolWord(wifiRunning_);
  text += " wifiStartAttempted=" + TextUtil::boolWord(wifiStartAttempted_);
  text += " wifiStartSucceeded=" + TextUtil::boolWord(wifiStartSucceeded_);
  text += " wifiState=" + runtimeState();
  text += " wifiMode=" + roleString();
  text += " apActive=" + TextUtil::boolWord(apActive_);
  text += " apSSID=" + config_.apSsid;
  text += " apPasswordSet=" + TextUtil::boolWord(config_.apPassword.length() > 0);
  text += " apPasswordLength=" + String(config_.apPassword.length());
  text += " apIP=" + String(apActive_ ? WiFi.softAPIP().toString() : "0.0.0.0");
  text += " apClients=" + String(apActive_ ? WiFi.softAPgetStationNum() : 0);
  text += " staCredentials=" + String(hasUsableStationCredentials() ? "usable" : "absent-or-placeholder");
  text += " staAttempted=" + TextUtil::boolWord(stationAttempted_);
  text += " staConnected=" + TextUtil::boolWord(stationConnected);
  text += " staSSID=" + config_.staSsid;
  text += " staPasswordSet=" + TextUtil::boolWord(config_.staPassword.length() > 0);
  text += " staPasswordLength=" + String(config_.staPassword.length());
  text += " staIP=" + String(stationConnected ? WiFi.localIP().toString() : "0.0.0.0");
  text += " wifiIP=" + ipString();
  text += " wifiTxPowerDbm=" + wifiPowerString(config_.wifiTxPowerQuarterDbm);
  text += " webServer=" + TextUtil::boolWord(webPortal_.isRunning());
  text += " dnsServer=" + TextUtil::boolWord(dnsRunning_);
  text += " fallbackAP=" + TextUtil::boolWord(config_.fallbackAp);
  text += " bleDesired=" + TextUtil::boolWord(config_.bleEnabled);
  text += " bleRuntime=" + TextUtil::boolWord(transports_.isBleRunning());
  text += " bleConnected=" + String(transports_.isBleConnected() ? "yes" : "no");
  text += " bleDormant=" + TextUtil::boolWord(transports_.isBleDormant());
  text += " coexPolicy=" + String(isPersistentWifiBleProfile() ? "persistent" : (isCombinedWifiBleProfile() ? "adaptive" : "single"));
  text += " bleWebHandoff=" + TextUtil::boolWord(bleWebHandoffArmed_);
  text += " sppCompiled=" + TextUtil::boolWord(AppConfig::ENABLE_CLASSIC_BT_SPP);
  text += " sppDesired=" + TextUtil::boolWord(config_.sppEnabled);
  text += " sppRuntime=" + TextUtil::boolWord(transports_.isSppRunning());
  text += " sppConnected=" + String(transports_.isSppConnected() ? "yes" : "no");
  return text;
}

String RadioManager::stateJson() const {
  const bool stationConnected =
    wifiRunning_ && stationAttempted_ && WiFi.status() == WL_CONNECTED;
  String json = "{";
  json += "\"bootModeDesired\":\"" + bootModeString(config_.bootMode) + "\"";
  json += ",\"bootModeActive\":\"" + bootModeString(activeBootMode_) + "\"";
  json += ",\"bootModeRebootRequired\":" + TextUtil::jsonBool(bootModeNeedsReboot());
  json += ",\"bootTrialActive\":" + TextUtil::jsonBool(trialBootActive_);
  json += ",\"bootModeLastGood\":\"" + bootModeString(lastGoodBootMode_) + "\"";
  json += ",\"bootModePending\":\"" + bootModeString(pendingBootMode_) + "\"";
  json += ",\"managedRebootPending\":" + TextUtil::jsonBool(managedRebootPending_);
  json += ",\"wifiCompiled\":" + TextUtil::jsonBool(AppConfig::ENABLE_WIFI);
  json += ",\"bleCompiled\":" + TextUtil::jsonBool(AppConfig::ENABLE_BLE);
  json += ",\"wifiDesired\":" + TextUtil::jsonBool(config_.wifiEnabled);
  json += ",\"wifiRunning\":" + TextUtil::jsonBool(wifiRunning_);
  json += ",\"wifiStartAttempted\":" + TextUtil::jsonBool(wifiStartAttempted_);
  json += ",\"wifiStartSucceeded\":" + TextUtil::jsonBool(wifiStartSucceeded_);
  json += ",\"wifiState\":\"" + TextUtil::jsonEscape(runtimeState()) + "\"";
  json += ",\"wifiMode\":\"" + String(
    config_.wifiRole == WifiRole::AP_STA ? "APSTA" :
    config_.wifiRole == WifiRole::STA ? "STA" : "AP"
  ) + "\"";
  json += ",\"wifiRuntimeMode\":\"" + roleString() + "\"";
  json += ",\"fallbackActive\":" + TextUtil::jsonBool(fallbackActive_);
  json += ",\"fallbackApEnabled\":" + TextUtil::jsonBool(config_.fallbackAp);
  json += ",\"ip\":\"" + TextUtil::jsonEscape(ipString()) + "\"";
  json += ",\"apActive\":" + TextUtil::jsonBool(apActive_);
  json += ",\"apIp\":\"" + TextUtil::jsonEscape(apActive_ ? WiFi.softAPIP().toString() : "0.0.0.0") + "\"";
  json += ",\"apClients\":" + String(apActive_ ? WiFi.softAPgetStationNum() : 0);
  json += ",\"stationAttempted\":" + TextUtil::jsonBool(stationAttempted_);
  json += ",\"stationConnected\":" + TextUtil::jsonBool(stationConnected);
  json += ",\"stationCredentialsUsable\":" + TextUtil::jsonBool(hasUsableStationCredentials());
  json += ",\"stationIp\":\"" + TextUtil::jsonEscape(stationConnected ? WiFi.localIP().toString() : "0.0.0.0") + "\"";
  json += ",\"txPowerDbm\":" + wifiPowerString(config_.wifiTxPowerQuarterDbm);
  json += ",\"webServerRunning\":" + TextUtil::jsonBool(webPortal_.isRunning());
  json += ",\"webRequestCount\":" + String(webPortal_.requestCount());
  json += ",\"webLastRequestAtMs\":" + String(webPortal_.lastRequestAtMs());
  json += ",\"freeHeap\":" + String(ESP.getFreeHeap());
  json += ",\"minFreeHeap\":" + String(ESP.getMinFreeHeap());
  json += ",\"largestFreeBlock\":" + String(ESP.getMaxAllocHeap());
  json += ",\"dnsServerRunning\":" + TextUtil::jsonBool(dnsRunning_);
  json += ",\"staSsid\":\"" + TextUtil::jsonEscape(config_.staSsid) + "\"";
  json += ",\"apSsid\":\"" + TextUtil::jsonEscape(config_.apSsid) + "\"";
  json += ",\"staPasswordSet\":" + TextUtil::jsonBool(config_.staPassword.length() > 0);
  json += ",\"apPasswordSet\":" + TextUtil::jsonBool(config_.apPassword.length() > 0);
  json += ",\"rssi\":" + String(stationConnected ? WiFi.RSSI() : 0);
  json += ",\"bleDesired\":" + TextUtil::jsonBool(config_.bleEnabled);
  json += ",\"bleRunning\":" + TextUtil::jsonBool(transports_.isBleRunning());
  json += ",\"bleConnected\":" + TextUtil::jsonBool(transports_.isBleConnected());
  json += ",\"bleDormant\":" + TextUtil::jsonBool(transports_.isBleDormant());
  json += ",\"coexPersistent\":" + TextUtil::jsonBool(isPersistentWifiBleProfile());
  json += ",\"bleWebHandoffArmed\":" + TextUtil::jsonBool(bleWebHandoffArmed_);
  json += ",\"bleWebHandoffRemainingMs\":" + String(
    bleWebHandoffArmed_ && static_cast<int32_t>(bleWebHandoffDeadlineMs_ - millis()) > 0
      ? bleWebHandoffDeadlineMs_ - millis()
      : 0
  );
  json += ",\"sppCompiled\":" + TextUtil::jsonBool(AppConfig::ENABLE_CLASSIC_BT_SPP);
  json += ",\"sppDesired\":" + TextUtil::jsonBool(config_.sppEnabled);
  json += ",\"sppRunning\":" + TextUtil::jsonBool(transports_.isSppRunning());
  json += ",\"sppConnected\":" + TextUtil::jsonBool(transports_.isSppConnected());
  json += ",\"userConnected\":" + TextUtil::jsonBool(hasUserConnection());
  json += "}";
  return json;
}

String RadioManager::webStateJson() const {
  String json;
  json.reserve(420);
  json = "{\"bootModeActive\":\"" + bootModeString(activeBootMode_) + "\"";
  json += ",\"wifiCompiled\":" + TextUtil::jsonBool(AppConfig::ENABLE_WIFI);
  json += ",\"bleCompiled\":" + TextUtil::jsonBool(AppConfig::ENABLE_BLE);
  json += ",\"sppCompiled\":" + TextUtil::jsonBool(AppConfig::ENABLE_CLASSIC_BT_SPP);
  json += ",\"wifiState\":\"" + TextUtil::jsonEscape(runtimeState()) + "\"";
  json += ",\"ip\":\"" + TextUtil::jsonEscape(ipString()) + "\"";
  json += ",\"bleRunning\":" + TextUtil::jsonBool(transports_.isBleRunning());
  json += ",\"bleConnected\":" + TextUtil::jsonBool(transports_.isBleConnected());
  json += ",\"sppRunning\":" + TextUtil::jsonBool(transports_.isSppRunning());
  json += ",\"sppConnected\":" + TextUtil::jsonBool(transports_.isSppConnected());
  json += "}";
  return json;
}

String RadioManager::activeBootModeText() const {
  return bootModeString(activeBootMode_);
}

uint8_t RadioManager::activeBootModeValue() const {
  return static_cast<uint8_t>(activeBootMode_);
}

bool RadioManager::isCombinedWifiBleProfile() const {
  return activeBootMode_ == BootRadioMode::WIFI_BLE ||
    activeBootMode_ == BootRadioMode::WIFI_BLE_P;
}

bool RadioManager::isPersistentWifiBleProfile() const {
  return activeBootMode_ == BootRadioMode::WIFI_BLE_P;
}

bool RadioManager::bootTransactionBusy() const {
  return trialBootActive_ || managedRebootPending_ || applyPending_;
}

bool RadioManager::activeProfileHealthy(String &reason) const {
  return bootProfileHealthy(reason);
}

void RadioManager::setDefaults() {
  config_.bootMode = fallbackBootMode();
  config_.wifiEnabled = false;
  config_.bleEnabled = false;
  config_.sppEnabled = false;
  config_.fallbackAp = true;
  // Default to AP+STA when Wi-Fi is compiled. With no usable credentials,
  // only the setup AP starts and no router association is attempted.
  config_.wifiRole = WifiRole::AP_STA;
  config_.wifiTxPowerQuarterDbm = AppConfig::WIFI_TX_POWER_MAX_QUARTER_DBM;
  config_.staSsid = "";
  config_.staPassword = "";
  config_.apSsid = AppConfig::DEFAULT_WIFI_AP_SSID;
  config_.apPassword = AppConfig::DEFAULT_WIFI_AP_PASSWORD;
  normalizeBootModeFlags();
}

void RadioManager::normalizeBootModeFlags() {
  if (!bootModeAvailable(config_.bootMode)) {
    if (config_.bootMode == BootRadioMode::WIFI_BLE ||
        config_.bootMode == BootRadioMode::WIFI_BLE_P) {
      if (AppConfig::ENABLE_WIFI) {
        config_.bootMode = fallbackBootMode();
      } else if (AppConfig::ENABLE_BLE) {
        config_.bootMode = BootRadioMode::BLE;
      } else {
        config_.bootMode = fallbackBootMode();
      }
    } else {
      config_.bootMode = fallbackBootMode();
    }
  }
  config_.wifiEnabled = bootModeUsesWifi(config_.bootMode);
  config_.bleEnabled = bootModeUsesBle(config_.bootMode);
  config_.sppEnabled = bootModeUsesSpp(config_.bootMode);
}

void RadioManager::selectBootModeFromFlags() {
  config_.wifiEnabled = AppConfig::ENABLE_WIFI && config_.wifiEnabled;
  config_.bleEnabled = AppConfig::ENABLE_BLE && config_.bleEnabled;
  config_.sppEnabled = AppConfig::ENABLE_CLASSIC_BT_SPP && config_.sppEnabled;

  if (config_.sppEnabled) {
    config_.bootMode = BootRadioMode::SPP;
  } else if (config_.wifiEnabled && config_.bleEnabled) {
    config_.bootMode = BootRadioMode::WIFI_BLE;
  } else if (config_.wifiEnabled) {
    config_.bootMode = BootRadioMode::WIFI;
  } else if (config_.bleEnabled) {
    config_.bootMode = BootRadioMode::BLE;
  } else {
    config_.bootMode = BootRadioMode::USB;
  }
  normalizeBootModeFlags();
}

bool RadioManager::parseBootMode(const String &value, BootRadioMode &mode) const {
  String normalized = value;
  normalized.trim();
  normalized.toUpperCase();

  if (normalized == "WIFI") {
    mode = BootRadioMode::WIFI;
    return true;
  }
  if (normalized == "WIFI_BLE") {
    mode = BootRadioMode::WIFI_BLE;
    return true;
  }
  if (normalized == "WIFI_BLE_P") {
    mode = BootRadioMode::WIFI_BLE_P;
    return true;
  }
  if (normalized == "BLE") {
    mode = BootRadioMode::BLE;
    return true;
  }
  if (normalized == "SPP") {
    mode = BootRadioMode::SPP;
    return true;
  }
  if (normalized == "USB") {
    mode = BootRadioMode::USB;
    return true;
  }
  return false;
}

bool RadioManager::bootModeAvailable(BootRadioMode mode) const {
  switch (mode) {
    case BootRadioMode::WIFI:
      return AppConfig::ENABLE_WIFI;
    case BootRadioMode::WIFI_BLE:
    case BootRadioMode::WIFI_BLE_P:
      return AppConfig::ENABLE_WIFI && AppConfig::ENABLE_BLE;
    case BootRadioMode::BLE:
      return AppConfig::ENABLE_BLE;
    case BootRadioMode::SPP:
      return AppConfig::ENABLE_CLASSIC_BT_SPP;
    case BootRadioMode::USB:
      return true;
  }
  return false;
}

RadioManager::BootRadioMode RadioManager::fallbackBootMode() const {
  if (AppConfig::ENABLE_WIFI) {
    return BootRadioMode::WIFI;
  }
  if (AppConfig::ENABLE_BLE) {
    return BootRadioMode::BLE;
  }
  if (AppConfig::ENABLE_CLASSIC_BT_SPP) {
    return BootRadioMode::SPP;
  }
  return BootRadioMode::USB;
}

String RadioManager::bootModeString(BootRadioMode mode) const {
  switch (mode) {
    case BootRadioMode::WIFI_BLE:
      return "WIFI_BLE";
    case BootRadioMode::WIFI_BLE_P:
      return "WIFI_BLE_P";
    case BootRadioMode::BLE:
      return "BLE";
    case BootRadioMode::SPP:
      return "SPP";
    case BootRadioMode::USB:
      return "USB";
    case BootRadioMode::WIFI:
    default:
      return "WIFI";
  }
}

bool RadioManager::bootModeUsesWifi(BootRadioMode mode) const {
  return AppConfig::ENABLE_WIFI &&
    (mode == BootRadioMode::WIFI || mode == BootRadioMode::WIFI_BLE ||
      mode == BootRadioMode::WIFI_BLE_P);
}

bool RadioManager::bootModeUsesBle(BootRadioMode mode) const {
  return AppConfig::ENABLE_BLE &&
    (mode == BootRadioMode::BLE || mode == BootRadioMode::WIFI_BLE ||
      mode == BootRadioMode::WIFI_BLE_P);
}

bool RadioManager::bootModeUsesSpp(BootRadioMode mode) const {
  return AppConfig::ENABLE_CLASSIC_BT_SPP && mode == BootRadioMode::SPP;
}

bool RadioManager::bootModeNeedsReboot() const {
  return config_.bootMode != activeBootMode_;
}

bool RadioManager::initializeBootBluetooth() {
  bootBluetoothFailureReason_ = "";

  if (bootModeUsesBle(activeBootMode_)) {
    Serial.println(
      isCombinedWifiBleProfile()
        ? "[BOOT][RADIO][COEX] initializing BLE before WiFi on Arduino setup core"
        : "[BOOT][RADIO] initializing BLE-only profile on Arduino setup core"
    );
    Serial.flush();
    transports_.setBleEnabled(true);
    if (!transports_.isBleInitialized() || !transports_.isBleRunning()) {
      bootBluetoothFailureReason_ = "BLE initialization returned without a running BLE transport";
      return false;
    }
    return true;
  }

  if (bootModeUsesSpp(activeBootMode_)) {
    Serial.println("[BOOT][RADIO] initializing exclusive Classic BT SPP profile");
    Serial.flush();
    transports_.setSppEnabled(true);
    if (!transports_.isSppRunning()) {
      bootBluetoothFailureReason_ = "Classic Bluetooth SPP initialization returned without a running transport";
      return false;
    }
  }

  return true;
}

void RadioManager::prepareBootTransaction() {
  activeBootMode_ = config_.bootMode;
  lastGoodBootMode_ = fallbackBootMode();
  pendingBootMode_ = config_.bootMode;
  trialBootActive_ = false;
  trialStartedAtMs_ = 0;
  trialValidationStartedAtMs_ = 0;
  trialHealthySinceMs_ = 0;

  Preferences preferences;
  if (!preferences.begin(RADIO_PREFERENCES_NAMESPACE, false)) {
    Serial.println("[BOOT][RADIO][SAFE] NVS unavailable; using saved profile without transaction state");
    Serial.flush();
    return;
  }

  uint8_t lastGoodRaw = preferences.getUChar(KEY_LAST_GOOD, 255);
  if (
    lastGoodRaw > static_cast<uint8_t>(BootRadioMode::WIFI_BLE_P) ||
    !bootModeAvailable(static_cast<BootRadioMode>(lastGoodRaw))
  ) {
    lastGoodRaw = static_cast<uint8_t>(fallbackBootMode());
    preferences.putUChar(KEY_LAST_GOOD, lastGoodRaw);
  }
  lastGoodBootMode_ = static_cast<BootRadioMode>(lastGoodRaw);

  const bool transactionPending = preferences.getBool(KEY_TXN_PENDING, false);
  const bool transactionStarted = preferences.getBool(KEY_TXN_STARTED, false);
  const uint8_t pendingRaw = preferences.getUChar(KEY_TXN_PROFILE, 255);

  if (!transactionPending) {
    preferences.end();
    return;
  }

  if (
    pendingRaw > static_cast<uint8_t>(BootRadioMode::WIFI_BLE_P) ||
    !bootModeAvailable(static_cast<BootRadioMode>(pendingRaw))
  ) {
    preferences.putBool(KEY_TXN_PENDING, false);
    preferences.putBool(KEY_TXN_STARTED, false);
    preferences.end();
    rollbackBootTransaction("pending profile metadata was invalid");
    return;
  }

  pendingBootMode_ = static_cast<BootRadioMode>(pendingRaw);

  if (transactionStarted) {
    preferences.end();
    rollbackBootTransaction(
      "previous trial boot did not reach the OK checkpoint, indicating a panic, watchdog reset, or failed health check"
    );
    return;
  }

  const bool markedStarted =
    preferences.putBool(KEY_TXN_STARTED, true) > 0 &&
    preferences.getBool(KEY_TXN_STARTED, false);
  preferences.end();

  if (!markedStarted) {
    rollbackBootTransaction("could not arm the trial-boot crash marker");
    return;
  }

  config_.bootMode = pendingBootMode_;
  normalizeBootModeFlags();
  activeBootMode_ = pendingBootMode_;
  trialBootActive_ = true;
  trialStartedAtMs_ = millis();

  String memoryReason;
  if (!trialMemoryHealthy(memoryReason)) {
    rollbackBootTransaction("preflight memory check failed: " + memoryReason);
    return;
  }

  Serial.println(
    "[BOOT][RADIO][TRIAL] profile=" + bootModeString(activeBootMode_) +
      " lastGood=" + bootModeString(lastGoodBootMode_) +
      " crashMarker=armed heap=" + String(ESP.getFreeHeap()) +
      " largest=" + String(ESP.getMaxAllocHeap())
  );
  Serial.flush();
}

bool RadioManager::beginBootModeSwitch(
  BootRadioMode mode,
  CommandSource source,
  const String &requestId
) {
  if (!bootModeAvailable(mode)) {
    error(
      source,
      requestId,
      "radio profile " + bootModeString(mode) +
        " is compiled out; edit the build switches in src/config/AppConfig.h and rebuild"
    );
    return false;
  }
  if (trialBootActive_) {
    if (mode == activeBootMode_) {
      publish(
        EventLevel::STATUS,
        source,
        requestId,
        "[ACK] radio profile " + bootModeString(mode) +
          " is already in its boot trial; no second transaction was created"
      );
      return true;
    }
    error(
      source,
      requestId,
      "current radio profile is still in its boot trial; wait for [BOOT][RADIO][OK] before starting another switch"
    );
    return false;
  }
  if (managedRebootPending_) {
    if (mode == pendingBootMode_ || mode == config_.bootMode) {
      publish(
        EventLevel::STATUS,
        source,
        requestId,
        "[ACK] radio profile " + bootModeString(mode) +
          " is already saved and its reboot is scheduled"
      );
      return true;
    }
    error(source, requestId, "a different radio-mode reboot is already scheduled");
    return false;
  }
  if (mode == activeBootMode_ && !bootModeNeedsReboot()) {
    publish(
      EventLevel::STATUS,
      source,
      requestId,
      "[ACK] radio profile " + bootModeString(mode) +
        " is already active and last-known-good; use Reboot to restart the same profile"
    );
    return true;
  }

  const Config previousConfig = config_;
  config_.bootMode = mode;
  normalizeBootModeFlags();

  String reason;
  if (!validateConfig(reason)) {
    config_ = previousConfig;
    error(source, requestId, reason);
    return false;
  }

  if (!writeBootTransaction(mode)) {
    config_ = previousConfig;
    saveSettings();
    clearBootTransaction(activeBootMode_);
    error(source, requestId, "could not save and verify the transactional radio boot profile");
    return false;
  }

  pendingBootMode_ = mode;
  publish(
    EventLevel::STATUS,
    source,
    requestId,
    "[ACK] radio mode saved and verified pending=" + bootModeString(mode) +
      " lastGood=" + bootModeString(activeBootMode_) +
      " rebooting; next boot must report [BOOT][RADIO][OK] or it will automatically roll back"
  );
  publishConfigReadback(source, requestId, "transactional radio switch armed");
  scheduleManagedReboot("activating trial radio profile " + bootModeString(mode));
  return true;
}

bool RadioManager::writeBootTransaction(BootRadioMode pendingMode) {
  if (!saveSettings()) {
    return false;
  }

  Preferences preferences;
  if (!preferences.begin(RADIO_PREFERENCES_NAMESPACE, false)) {
    return false;
  }

  const uint32_t nextSequence = preferences.getUInt(KEY_TXN_SEQUENCE, 0) + 1U;
  bool ok = preferences.putUChar(
    KEY_LAST_GOOD,
    static_cast<uint8_t>(activeBootMode_)
  ) > 0;
  ok = preferences.putUChar(
    KEY_TXN_PROFILE,
    static_cast<uint8_t>(pendingMode)
  ) > 0 && ok;
  ok = preferences.putUInt(KEY_TXN_SEQUENCE, nextSequence) > 0 && ok;
  ok = preferences.putBool(KEY_TXN_STARTED, false) > 0 && ok;
  ok = preferences.putBool(KEY_TXN_PENDING, true) > 0 && ok;

  const bool verified =
    ok &&
    preferences.getBool(KEY_TXN_PENDING, false) &&
    !preferences.getBool(KEY_TXN_STARTED, true) &&
    preferences.getUChar(KEY_TXN_PROFILE, 255) == static_cast<uint8_t>(pendingMode) &&
    preferences.getUChar(KEY_LAST_GOOD, 255) == static_cast<uint8_t>(activeBootMode_) &&
    preferences.getUInt(KEY_TXN_SEQUENCE, 0) == nextSequence;
  preferences.end();
  return verified;
}

bool RadioManager::clearBootTransaction(BootRadioMode knownGoodMode) {
  Preferences preferences;
  if (!preferences.begin(RADIO_PREFERENCES_NAMESPACE, false)) {
    return false;
  }

  bool ok = preferences.putUChar(
    KEY_LAST_GOOD,
    static_cast<uint8_t>(knownGoodMode)
  ) > 0;
  ok = preferences.putUChar(
    KEY_TXN_PROFILE,
    static_cast<uint8_t>(knownGoodMode)
  ) > 0 && ok;
  ok = preferences.putBool(KEY_TXN_STARTED, false) > 0 && ok;
  ok = preferences.putBool(KEY_TXN_PENDING, false) > 0 && ok;

  const bool verified =
    ok &&
    !preferences.getBool(KEY_TXN_PENDING, true) &&
    !preferences.getBool(KEY_TXN_STARTED, true) &&
    preferences.getUChar(KEY_LAST_GOOD, 255) == static_cast<uint8_t>(knownGoodMode);
  preferences.end();
  return verified;
}

bool RadioManager::commitBootTransaction() {
  config_.bootMode = activeBootMode_;
  normalizeBootModeFlags();
  if (!saveSettings() || !clearBootTransaction(activeBootMode_)) {
    return false;
  }

  lastGoodBootMode_ = activeBootMode_;
  pendingBootMode_ = activeBootMode_;
  trialBootActive_ = false;
  trialValidationStartedAtMs_ = 0;
  trialHealthySinceMs_ = 0;

  const String message =
    "[BOOT][RADIO][OK] profile=" + bootModeString(activeBootMode_) +
    " saved as last-known-good heap=" + String(ESP.getFreeHeap()) +
    " largest=" + String(ESP.getMaxAllocHeap());
  Serial.println(message);
  Serial.flush();
  events_.publish(
    EventLevel::STATUS,
    message,
    CommandSource::INTERNAL
  );
  return true;
}

void RadioManager::rollbackBootTransaction(const String &reason) {
  const BootRadioMode rollbackMode = lastGoodBootMode_;
  config_.bootMode = rollbackMode;
  normalizeBootModeFlags();

  bool saved = saveSettings();
  if (!saved) {
    Preferences preferences;
    if (preferences.begin(RADIO_PREFERENCES_NAMESPACE, false)) {
      saved = preferences.putUChar(
        "profile",
        static_cast<uint8_t>(rollbackMode)
      ) > 0;
      preferences.putBool("valid", true);
      preferences.end();
    }
  }
  const bool cleared = clearBootTransaction(rollbackMode);

  activeBootMode_ = rollbackMode;
  pendingBootMode_ = rollbackMode;
  trialBootActive_ = false;
  trialStartedAtMs_ = 0;
  trialValidationStartedAtMs_ = 0;
  trialHealthySinceMs_ = 0;
  bootBluetoothInitFailed_ = false;
  bootBluetoothFailureReason_ = "";

  Serial.println(
    "[BOOT][RADIO][ROLLBACK] reason=" + reason +
      " restored=" + bootModeString(rollbackMode) +
      " saved=" + TextUtil::boolWord(saved) +
      " markerCleared=" + TextUtil::boolWord(cleared)
  );
  Serial.flush();
}

void RadioManager::serviceBootTransaction() {
  if (!trialBootActive_) {
    return;
  }

  if (bootBluetoothInitFailed_) {
    if (!managedRebootPending_) {
      events_.publish(
        EventLevel::ERROR,
        "[BOOT][RADIO][FAIL] profile=" +
          bootModeString(activeBootMode_) + " reason=" +
          bootBluetoothFailureReason_ + "; rebooting to automatic rollback",
        CommandSource::INTERNAL
      );
      scheduleManagedReboot("radio trial failed; rollback marker remains armed");
    }
    return;
  }

  const uint32_t now = millis();
  String readinessReason;
  if (!trialStartupReady(readinessReason)) {
    return;
  }

  if (trialValidationStartedAtMs_ == 0) {
    trialValidationStartedAtMs_ = now;
    const String message =
      "[BOOT][RADIO][VALIDATE] profile=" + bootModeString(activeBootMode_) +
      " startup complete; timeoutMs=" + String(AppConfig::RADIO_TRIAL_TIMEOUT_MS) +
      " heap=" + String(ESP.getFreeHeap()) +
      " largest=" + String(ESP.getMaxAllocHeap());
    Serial.println(message);
    Serial.flush();
    events_.publish(
      EventLevel::STATUS,
      message,
      CommandSource::INTERNAL
    );
  }

  String reason;
  const bool healthy = bootProfileHealthy(reason);

  if (!healthy) {
    trialHealthySinceMs_ = 0;
    if (
      static_cast<uint32_t>(now - trialValidationStartedAtMs_) >=
        AppConfig::RADIO_TRIAL_TIMEOUT_MS &&
      !managedRebootPending_
    ) {
      events_.publish(
        EventLevel::ERROR,
        "[BOOT][RADIO][FAIL] profile=" +
          bootModeString(activeBootMode_) + " reason=" + reason +
          "; rebooting to automatic rollback",
        CommandSource::INTERNAL
      );
      scheduleManagedReboot("radio trial health timeout; rollback marker remains armed");
    }
    return;
  }

  if (trialHealthySinceMs_ == 0) {
    trialHealthySinceMs_ = now;
    const String message =
      "[BOOT][RADIO][CHECK] profile=" + bootModeString(activeBootMode_) +
      " health=pass stabilizingMs=" + String(AppConfig::RADIO_TRIAL_STABILIZE_MS) +
      " heap=" + String(ESP.getFreeHeap()) +
      " largest=" + String(ESP.getMaxAllocHeap());
    Serial.println(message);
    Serial.flush();
    events_.publish(
      EventLevel::STATUS,
      message,
      CommandSource::INTERNAL
    );
    return;
  }

  if (
    static_cast<uint32_t>(now - trialHealthySinceMs_) <
      AppConfig::RADIO_TRIAL_STABILIZE_MS
  ) {
    return;
  }

  if (!commitBootTransaction()) {
    trialHealthySinceMs_ = now;
    events_.publish(
      EventLevel::ERROR,
      "radio trial passed but NVS OK checkpoint could not be verified; crash marker remains armed",
      CommandSource::INTERNAL
    );
  }
}

bool RadioManager::trialStartupReady(String &reason) const {
  if (applyPending_) {
    reason = "waiting for scheduled radio apply";
    return false;
  }
  if (bootModeUsesWifi(activeBootMode_) && !wifiStartAttempted_) {
    reason = "waiting for WiFi startup attempt";
    return false;
  }
  reason = "";
  return true;
}

bool RadioManager::bootProfileHealthy(String &reason) const {
  if (!trialMemoryHealthy(reason)) {
    return false;
  }

  switch (activeBootMode_) {
    case BootRadioMode::WIFI_BLE:
    case BootRadioMode::WIFI_BLE_P:
      if (!transports_.isBleInitialized() || !transports_.isBleRunning()) {
        reason = "BLE transport is not running";
        return false;
      }
      if (!wifiStartSucceeded_) {
        reason = "WiFi startup incomplete: running=" + TextUtil::boolWord(wifiRunning_) +
          " web=" + TextUtil::boolWord(webPortal_.isRunning()) +
          " attempted=" + TextUtil::boolWord(wifiStartAttempted_);
        return false;
      }
      if (activeBootMode_ == BootRadioMode::WIFI_BLE_P && transports_.isBleDormant()) {
        reason = "BLE unexpectedly entered dormancy in persistent coexistence mode";
        return false;
      }
      break;
    case BootRadioMode::BLE:
      if (!transports_.isBleInitialized() || !transports_.isBleRunning()) {
        reason = "BLE transport is not running";
        return false;
      }
      break;
    case BootRadioMode::SPP:
      if (!transports_.isSppRunning()) {
        reason = "Classic Bluetooth SPP transport is not running";
        return false;
      }
      break;
    case BootRadioMode::WIFI:
      if (!wifiStartSucceeded_) {
        reason = "WiFi startup incomplete: running=" + TextUtil::boolWord(wifiRunning_) +
          " web=" + TextUtil::boolWord(webPortal_.isRunning()) +
          " attempted=" + TextUtil::boolWord(wifiStartAttempted_);
        return false;
      }
      break;
    case BootRadioMode::USB:
      break;
  }

  reason = "";
  return true;
}

bool RadioManager::trialMemoryHealthy(String &reason) const {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t largestBlock = ESP.getMaxAllocHeap();
  if (freeHeap < AppConfig::RADIO_TRIAL_MIN_FREE_HEAP_BYTES) {
    reason = "free heap " + String(freeHeap) + " below " +
      String(AppConfig::RADIO_TRIAL_MIN_FREE_HEAP_BYTES);
    return false;
  }
  if (largestBlock < AppConfig::RADIO_TRIAL_MIN_LARGEST_BLOCK_BYTES) {
    reason = "largest heap block " + String(largestBlock) + " below " +
      String(AppConfig::RADIO_TRIAL_MIN_LARGEST_BLOCK_BYTES);
    return false;
  }
  reason = "";
  return true;
}

void RadioManager::scheduleManagedReboot(const String &reason) {
  managedRebootReason_ = reason;
  managedRebootPending_ = true;
  managedRebootAtMs_ = millis() + AppConfig::RADIO_MODE_REBOOT_DELAY_MS;
}

bool RadioManager::loadSettings(
  bool announce,
  CommandSource source,
  const String &requestId
) {
  Preferences preferences;
  if (!preferences.begin(RADIO_PREFERENCES_NAMESPACE, true)) {
    if (announce) {
      error(source, requestId, "could not open radio configuration storage");
    }
    return false;
  }

  bool valid = preferences.getBool("valid", false);
  if (valid) {
    const uint8_t storedBootMode = preferences.getUChar("profile", 255);
    if (storedBootMode <= static_cast<uint8_t>(BootRadioMode::WIFI_BLE_P)) {
      config_.bootMode = static_cast<BootRadioMode>(storedBootMode);
    } else {
      // v5.14 and earlier stored independent radio booleans. Migrate those
      // builds to the first radio profile present in this firmware.
      config_.bootMode = fallbackBootMode();
    }
    config_.fallbackAp = preferences.getBool("fallback", config_.fallbackAp);
    config_.wifiRole = static_cast<WifiRole>(preferences.getUChar("mode", static_cast<uint8_t>(config_.wifiRole)));
    config_.wifiTxPowerQuarterDbm = preferences.getChar("txq", config_.wifiTxPowerQuarterDbm);
    config_.staSsid = preferences.getString("stassid", config_.staSsid);
    config_.staPassword = preferences.getString("stapass", config_.staPassword);
    config_.apSsid = preferences.getString("apssid", config_.apSsid);
    config_.apPassword = preferences.getString("appass", config_.apPassword);

    if (
      config_.wifiRole != WifiRole::AP &&
      config_.wifiRole != WifiRole::STA &&
      config_.wifiRole != WifiRole::AP_STA
    ) {
      config_.wifiRole = WifiRole::AP_STA;
    }
    int8_t validated = 0;
    if (!parseWifiPower(wifiPowerString(config_.wifiTxPowerQuarterDbm), validated)) {
      config_.wifiTxPowerQuarterDbm = AppConfig::WIFI_TX_POWER_MAX_QUARTER_DBM;
    }
    normalizeBootModeFlags();
  }
  preferences.end();

  if (valid) {
    String reason;
    if (!validateConfig(reason)) {
      valid = false;
      setDefaults();
      events_.publish(
        EventLevel::WARNING,
        "saved radio configuration invalid; defaults restored: " + reason,
        CommandSource::INTERNAL
      );
    }
  }

  if (announce) {
    publish(
      valid ? EventLevel::STATUS : EventLevel::WARNING,
      source,
      requestId,
      valid ? "saved radio configuration loaded" : "no saved radio configuration found"
    );
  }
  return valid;
}

bool RadioManager::saveSettings() {
  Preferences preferences;
  if (!preferences.begin(RADIO_PREFERENCES_NAMESPACE, false)) {
    return false;
  }

  normalizeBootModeFlags();
  bool ok = preferences.putUChar("profile", static_cast<uint8_t>(config_.bootMode)) > 0;
  ok = preferences.putBool("wifi", config_.wifiEnabled) > 0 && ok;
  ok = preferences.putBool("ble", config_.bleEnabled) > 0 && ok;
  ok = preferences.putBool("spp", config_.sppEnabled) > 0 && ok;
  ok = preferences.putBool("fallback", config_.fallbackAp) > 0 && ok;
  ok = preferences.putUChar("mode", static_cast<uint8_t>(config_.wifiRole)) > 0 && ok;
  ok = preferences.putChar("txq", config_.wifiTxPowerQuarterDbm) > 0 && ok;
  ok = preferences.putString("stassid", config_.staSsid) == config_.staSsid.length() && ok;
  ok = preferences.putString("stapass", config_.staPassword) == config_.staPassword.length() && ok;
  ok = preferences.putString("apssid", config_.apSsid) == config_.apSsid.length() && ok;
  ok = preferences.putString("appass", config_.apPassword) == config_.apPassword.length() && ok;
  ok = preferences.putBool("valid", true) > 0 && ok;
  preferences.end();
  if (!ok || !preferences.begin(RADIO_PREFERENCES_NAMESPACE, true)) {
    return false;
  }

  const bool verified =
    preferences.getBool("valid", false) &&
    preferences.getUChar("profile", 255) == static_cast<uint8_t>(config_.bootMode) &&
    preferences.getBool("wifi", !config_.wifiEnabled) == config_.wifiEnabled &&
    preferences.getBool("ble", !config_.bleEnabled) == config_.bleEnabled &&
    preferences.getBool("spp", !config_.sppEnabled) == config_.sppEnabled &&
    preferences.getBool("fallback", !config_.fallbackAp) == config_.fallbackAp &&
    preferences.getUChar("mode", 255) == static_cast<uint8_t>(config_.wifiRole) &&
    preferences.getChar("txq", 127) == config_.wifiTxPowerQuarterDbm &&
    preferences.getString("stassid", "") == config_.staSsid &&
    preferences.getString("stapass", "") == config_.staPassword &&
    preferences.getString("apssid", "") == config_.apSsid &&
    preferences.getString("appass", "") == config_.apPassword;
  preferences.end();
  return verified;
}

bool RadioManager::eraseSettings() {
  Preferences preferences;
  if (!preferences.begin(RADIO_PREFERENCES_NAMESPACE, false)) {
    return false;
  }
  const bool ok = preferences.clear();
  preferences.end();
  return ok;
}

bool RadioManager::clearStoredStationCredentials() {
  Preferences preferences;
  if (!preferences.begin(RADIO_PREFERENCES_NAMESPACE, false)) {
    return false;
  }
  // remove() may return false when a key did not exist. Opening and closing the
  // namespace successfully is enough; the desired end state is no stored key.
  preferences.remove("stassid");
  preferences.remove("stapass");
  preferences.end();
  return true;
}

bool RadioManager::isDummyCredential(const String &value) const {
  String normalized = value;
  normalized.trim();
  normalized.toLowerCase();
  static const char *const placeholders[] = {
    "dummy", "dummyssid", "dummy_ssid", "changeme", "change_me",
    "placeholder", "replace_me", "set_me", "example", "example_ssid",
    "example_wifi", "ssid", "yourssid", "your_ssid", "wifi_ssid",
    "your_wifi_ssid", "password", "yourpassword", "your_password",
    "wifi_password", "your_wifi_password", "<ssid>", "<password>",
    "none", "null"
  };
  for (const char *placeholder : placeholders) {
    if (normalized == placeholder) {
      return true;
    }
  }
  return false;
}

bool RadioManager::hasUsableStationCredentials() const {
  String ssid = config_.staSsid;
  ssid.trim();
  if (ssid.length() == 0 || isDummyCredential(ssid)) {
    return false;
  }
  if (config_.staPassword.length() > 0 && isDummyCredential(config_.staPassword)) {
    return false;
  }
  return true;
}

bool RadioManager::validateConfig(String &reason) const {
  if (static_cast<uint8_t>(config_.bootMode) > static_cast<uint8_t>(BootRadioMode::WIFI_BLE_P)) {
    reason = "invalid boot radio profile";
    return false;
  }
  if (!bootModeAvailable(config_.bootMode)) {
    reason = "radio profile " + bootModeString(config_.bootMode) + " is compiled out";
    return false;
  }

  String apSsid = config_.apSsid;
  apSsid.trim();
  String staSsid = config_.staSsid;
  staSsid.trim();

  if (apSsid.length() > 32) {
    reason = "access-point SSID must be 32 bytes or fewer";
    return false;
  }
  if (config_.apPassword.length() > 0 &&
      (config_.apPassword.length() < 8 || config_.apPassword.length() > 63)) {
    reason = "access-point password must be blank or 8 to 63 characters";
    return false;
  }
  if (staSsid.length() > 32) {
    reason = "station SSID must be 32 bytes or fewer";
    return false;
  }
  if (config_.staPassword.length() > 0 &&
      (config_.staPassword.length() < 8 || config_.staPassword.length() > 63)) {
    reason = "station password must be blank or 8 to 63 characters";
    return false;
  }

  reason = "";
  return true;
}

void RadioManager::scheduleApply(bool restartWifi) {
  applyPending_ = true;
  restartWifiPending_ = restartWifiPending_ || restartWifi;
  applyAtMs_ = millis() + AppConfig::RADIO_APPLY_GRACE_MS;
}

void RadioManager::applyNow(bool restartWifi) {
  const bool activeProfileUsesWifi = bootModeUsesWifi(activeBootMode_);

  if (restartWifi || activeProfileUsesWifi != wifiRunning_) {
    String reason;
    if (!validateConfig(reason)) {
      wifiStartAttempted_ = activeProfileUsesWifi;
      wifiStartSucceeded_ = false;
      error(CommandSource::INTERNAL, String(), "WiFi configuration not applied: " + reason);
      if (trialBootActive_) {
        bootBluetoothInitFailed_ = true;
        bootBluetoothFailureReason_ = "radio configuration validation failed: " + reason;
      }
      return;
    }

    wifiStartAttempted_ = false;
    wifiStartSucceeded_ = false;

    const bool wifiInterfaceExists =
      wifiRunning_ || WiFi.getMode() != WIFI_OFF;
    const bool preserveBleDormantForCoexStartup =
      activeBootMode_ == BootRadioMode::WIFI_BLE &&
      !wifiInterfaceExists &&
      transports_.isBleDormant();

    if (wifiInterfaceExists) {
      stopWifi();
    }
    bleDormantForWifi_ = false;
    wifiClientLastSeenAtMs_ = 0;
    if (!preserveBleDormantForCoexStartup) {
      transports_.setBleDormant(false);
    }

    if (activeProfileUsesWifi) {
      Serial.println("[BOOT][WiFi] starting configured WiFi role");
      Serial.flush();
      startWifi();
      Serial.println("[BOOT][WiFi] WiFi startup returned");
      Serial.flush();
    }
  } else if (wifiRunning_) {
    applyWifiTxPower();
  }

  publishConfigReadback(CommandSource::INTERNAL, String(), "applied");
}

void RadioManager::startWifi() {
  wifiStartAttempted_ = true;
  wifiStartSucceeded_ = false;
  WiFi.persistent(false);
  fallbackActive_ = false;
  stationConnectedAnnounced_ = false;
  stationTimeoutAnnounced_ = false;
  stationAttempted_ = false;
  apActive_ = false;

  if (config_.wifiRole == WifiRole::AP) {
    startAccessPoint(false);
  } else if (config_.wifiRole == WifiRole::STA) {
    startStation();
  } else {
    startApSta();
  }

  wifiStartSucceeded_ = wifiRunning_ && webPortal_.isRunning();
  Serial.println(
    "[BOOT][WiFi] startup check running=" + TextUtil::boolWord(wifiRunning_) +
      " web=" + TextUtil::boolWord(webPortal_.isRunning()) +
      " success=" + TextUtil::boolWord(wifiStartSucceeded_) +
      " heap=" + String(ESP.getFreeHeap()) +
      " largest=" + String(ESP.getMaxAllocHeap())
  );
  Serial.flush();
}

bool RadioManager::beginAccessPoint(bool combinedMode) {
  String ssid = config_.apSsid;
  ssid.trim();
  if (ssid.length() == 0) {
    ssid = AppConfig::DEFAULT_WIFI_AP_SSID;
    config_.apSsid = ssid;
  }

  if (config_.apPassword.length() > 0 && config_.apPassword.length() < 8) {
    error(CommandSource::INTERNAL, String(), "WiFi AP password must be blank or at least 8 characters");
    return false;
  }

  const wifi_mode_t mode = combinedMode ? WIFI_AP_STA : WIFI_AP;
  WiFi.mode(mode);
  const char *password = config_.apPassword.length() > 0 ? config_.apPassword.c_str() : nullptr;
  if (!WiFi.softAP(ssid.c_str(), password)) {
    error(CommandSource::INTERNAL, String(), "WiFi access point failed to start");
    WiFi.mode(WIFI_OFF);
    return false;
  }

  // This firmware uses the AP as an interactive control link. Keeping modem
  // sleep disabled avoids long HTTP stalls when BLE is also active.
  WiFi.setSleep(false);

  apActive_ = true;
  wifiRunning_ = true;
  dnsRunning_ = dns_.start(53, "*", WiFi.softAPIP());
  return true;
}

void RadioManager::startAccessPoint(bool fallback) {
  stopMdns();
  if (dnsRunning_) {
    dns_.stop();
    dnsRunning_ = false;
  }

  if (!beginAccessPoint(false)) {
    return;
  }

  fallbackActive_ = fallback;
  stationAttempted_ = false;
  applyWifiTxPower();
  webPortal_.start();
  startMdns();
  Serial.println("[BOOT][HTTP] web console: http://" + WiFi.softAPIP().toString() + "/");
  Serial.println("[BOOT][HTTP] health check: http://" + WiFi.softAPIP().toString() + "/api/ping");
#if APP_HTTP_OTA_ENABLED
  Serial.println("[BOOT][HTTP] OTA update: http://" + WiFi.softAPIP().toString() + "/update");
#endif
  Serial.flush();

  publish(
    fallback ? EventLevel::WARNING : EventLevel::STATUS,
    CommandSource::INTERNAL,
    String(),
    String(fallback ? "station unavailable; fallback " : "") +
      "WiFi AP started ssid=" + config_.apSsid +
      " ip=" + WiFi.softAPIP().toString() +
      " txPowerDbm=" + wifiPowerString(config_.wifiTxPowerQuarterDbm)
  );
}

void RadioManager::startStation() {
  if (!hasUsableStationCredentials()) {
    publish(
      EventLevel::WARNING,
      CommandSource::INTERNAL,
      String(),
      "router connection skipped because station credentials are blank or placeholder values"
    );
    if (config_.fallbackAp) {
      startAccessPoint(true);
    }
    return;
  }

  stopMdns();
  if (dnsRunning_) {
    dns_.stop();
    dnsRunning_ = false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(config_.staSsid.c_str(), config_.staPassword.c_str());
  wifiRunning_ = true;
  apActive_ = false;
  stationAttempted_ = true;
  fallbackActive_ = false;
  stationConnectedAnnounced_ = false;
  stationTimeoutAnnounced_ = false;
  stationConnectStartedAtMs_ = millis();
  applyWifiTxPower();
  webPortal_.start();

  publish(
    EventLevel::STATUS,
    CommandSource::INTERNAL,
    String(),
    "WiFi station connecting ssid=" + config_.staSsid +
      " txPowerDbm=" + wifiPowerString(config_.wifiTxPowerQuarterDbm)
  );
}

void RadioManager::startApSta() {
  stopMdns();
  if (dnsRunning_) {
    dns_.stop();
    dnsRunning_ = false;
  }

  const bool connectStation = hasUsableStationCredentials();
  // Do not enable the STA interface when there are no usable router credentials.
  // WIFI_AP_STA consumes substantially more internal RAM than the AP-only mode.
  if (!beginAccessPoint(connectStation)) {
    return;
  }

  fallbackActive_ = false;
  webPortal_.start();
  startMdns();
  Serial.println("[BOOT][HTTP] web console: http://" + WiFi.softAPIP().toString() + "/");
  Serial.println("[BOOT][HTTP] health check: http://" + WiFi.softAPIP().toString() + "/api/ping");
#if APP_HTTP_OTA_ENABLED
  Serial.println("[BOOT][HTTP] OTA update: http://" + WiFi.softAPIP().toString() + "/update");
#endif
  Serial.flush();

  if (connectStation) {
    WiFi.setAutoReconnect(true);
    WiFi.begin(config_.staSsid.c_str(), config_.staPassword.c_str());
    stationAttempted_ = true;
    stationConnectStartedAtMs_ = millis();
    publish(
      EventLevel::STATUS,
      CommandSource::INTERNAL,
      String(),
      "WiFi AP+STA started apSSID=" + config_.apSsid +
        " apIP=" + WiFi.softAPIP().toString() +
        " routerSSID=" + config_.staSsid
    );
  } else {
    stationAttempted_ = false;
    publish(
      EventLevel::STATUS,
      CommandSource::INTERNAL,
      String(),
      "WiFi AP started ssid=" + config_.apSsid +
        " ip=" + WiFi.softAPIP().toString() +
        "; router connection skipped because credentials are blank or placeholder values"
    );
  }
  applyWifiTxPower();
}

void RadioManager::stopWifi() {
  // Keep the HTTP listener alive across AP/STA interface reconfiguration so
  // ConfigApply cannot tear down port 80.
  stopMdns();
  if (dnsRunning_) {
    dns_.stop();
    dnsRunning_ = false;
  }
  if (wifiRunning_ || WiFi.getMode() != WIFI_OFF) {
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
  wifiRunning_ = false;
  apActive_ = false;
  stationAttempted_ = false;
  fallbackActive_ = false;
  stationConnectedAnnounced_ = false;
  stationTimeoutAnnounced_ = false;
}

void RadioManager::serviceWifiState() {
  if (!wifiRunning_ || !stationAttempted_) {
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (!stationConnectedAnnounced_) {
      stationConnectedAnnounced_ = true;
      stationTimeoutAnnounced_ = false;
      startMdns();
      publish(
        EventLevel::STATUS,
        CommandSource::INTERNAL,
        String(),
        "WiFi station connected ip=" + WiFi.localIP().toString() +
          " rssi=" + String(WiFi.RSSI())
      );
    }
    return;
  }

  if (stationConnectedAnnounced_) {
    stationConnectedAnnounced_ = false;
    stationTimeoutAnnounced_ = false;
    stationConnectStartedAtMs_ = millis();
    publish(
      EventLevel::WARNING,
      CommandSource::INTERNAL,
      String(),
      "WiFi station connection lost; automatic reconnect active"
    );
  }

  const bool timedOut =
    static_cast<uint32_t>(millis() - stationConnectStartedAtMs_) >=
      AppConfig::WIFI_STA_CONNECT_TIMEOUT_MS;
  if (!timedOut) {
    return;
  }

  if (config_.wifiRole == WifiRole::STA && config_.fallbackAp) {
    WiFi.disconnect(true);
    startAccessPoint(true);
    return;
  }

  if (!stationTimeoutAnnounced_) {
    stationTimeoutAnnounced_ = true;
    publish(
      EventLevel::WARNING,
      CommandSource::INTERNAL,
      String(),
      apActive_
        ? "router connection timed out; AP remains available and STA auto-reconnect continues"
        : "router connection timed out; STA auto-reconnect continues"
    );
  }
}

void RadioManager::serviceRadioHandoff() {
  if (!isCombinedWifiBleProfile()) {
    return;
  }

  const uint32_t now = millis();
  const bool bleConnected = transports_.isBleConnected();
  const uint8_t apClients =
    wifiRunning_ && apActive_ ? WiFi.softAPgetStationNum() : 0;
  const bool wifiClientConnected = apClients > 0;

  if (wifiClientConnected) {
    wifiClientLastSeenAtMs_ = now;
  }

  const uint32_t httpRequestCount = webPortal_.requestCount();
  if (wifiClientConnected && !wifiClientPreviouslyConnected_) {
    wifiClientAssociatedAtMs_ = now;
    lastCoexHttpDiagnosticAtMs_ = 0;
    lastObservedHttpRequestCount_ = httpRequestCount;
    coexHttpNoRequestWarned_ = false;
    Serial.printf(
      "[RADIO][COEX][HTTP] WiFi client associated apIp=%s clients=%u webFlag=%s requests=%u free=%u minimum=%u largest=%u bleConnected=%s bleDormant=%s\n",
      WiFi.softAPIP().toString().c_str(),
      static_cast<unsigned>(apClients),
      webPortal_.isRunning() ? "true" : "false",
      static_cast<unsigned>(httpRequestCount),
      static_cast<unsigned>(ESP.getFreeHeap()),
      static_cast<unsigned>(ESP.getMinFreeHeap()),
      static_cast<unsigned>(ESP.getMaxAllocHeap()),
      bleConnected ? "true" : "false",
      transports_.isBleDormant() ? "true" : "false"
    );
    Serial.flush();
  } else if (!wifiClientConnected && wifiClientPreviouslyConnected_) {
    webPortal_.discardClientMetadata();
    Serial.printf(
      "[RADIO][COEX][HTTP] WiFi client released requests=%u free=%u largest=%u\n",
      static_cast<unsigned>(httpRequestCount),
      static_cast<unsigned>(ESP.getFreeHeap()),
      static_cast<unsigned>(ESP.getMaxAllocHeap())
    );
    Serial.flush();
    coexHttpNoRequestWarned_ = false;
  }
  wifiClientPreviouslyConnected_ = wifiClientConnected;

  const bool requestAdvanced = httpRequestCount != lastObservedHttpRequestCount_;
  if (requestAdvanced) {
    lastObservedHttpRequestCount_ = httpRequestCount;
    coexHttpNoRequestWarned_ = false;
  }

#if APP_VERBOSE_COEX_HTTP_DIAGNOSTICS
  if (
    wifiClientConnected &&
    (
      lastCoexHttpDiagnosticAtMs_ == 0 ||
      static_cast<uint32_t>(now - lastCoexHttpDiagnosticAtMs_) >=
        AppConfig::COEX_HTTP_DIAGNOSTIC_INTERVAL_MS
    )
  ) {
    Serial.printf(
      "[RADIO][COEX][HTTP] health apIp=%s clients=%u webFlag=%s requests=%u requestAdvanced=%s lastRequestAgeMs=%u free=%u minimum=%u largest=%u asyncTcpStack=%u asyncTcpCore=%d\n",
      WiFi.softAPIP().toString().c_str(),
      static_cast<unsigned>(apClients),
      webPortal_.isRunning() ? "true" : "false",
      static_cast<unsigned>(httpRequestCount),
      requestAdvanced ? "true" : "false",
      webPortal_.lastRequestAtMs() > 0
        ? static_cast<unsigned>(now - webPortal_.lastRequestAtMs())
        : 0U,
      static_cast<unsigned>(ESP.getFreeHeap()),
      static_cast<unsigned>(ESP.getMinFreeHeap()),
      static_cast<unsigned>(ESP.getMaxAllocHeap()),
      static_cast<unsigned>(CONFIG_ASYNC_TCP_STACK_SIZE),
      static_cast<int>(CONFIG_ASYNC_TCP_RUNNING_CORE)
    );
    Serial.flush();

    if (
      httpRequestCount == 0 &&
      !coexHttpNoRequestWarned_ &&
      static_cast<uint32_t>(now - wifiClientAssociatedAtMs_) >=
        AppConfig::COEX_HTTP_NO_REQUEST_WARNING_MS
    ) {
      coexHttpNoRequestWarned_ = true;
      publish(
        EventLevel::WARNING,
        CommandSource::INTERNAL,
        String(),
        "WiFi station is associated but no HTTP request has reached the portal; try http://" +
          WiFi.softAPIP().toString() + "/api/ping"
      );
    }

    lastCoexHttpDiagnosticAtMs_ = now;
  }
#else
  // Normal builds are event-driven: association, release, command handling,
  // failures, and explicit status commands are logged. A quiet, healthy portal
  // does not need a five-second heap report forever.
  lastCoexHttpDiagnosticAtMs_ = now;
#endif


  if (
    bleWebHandoffArmed_ &&
    static_cast<int32_t>(now - bleWebHandoffDeadlineMs_) >= 0
  ) {
    bleWebHandoffArmed_ = false;
    bleWebHandoffDeadlineMs_ = 0;
    publish(
      EventLevel::WARNING,
      CommandSource::INTERNAL,
      String(),
      "BLE browser connection window expired before a BLE connection completed"
    );
  }

  // Both combined profiles keep the Wi-Fi and BLE stacks initialized. The ESP32
  // coexistence arbiter schedules RF time. WIFI_BLE may pause idle advertising;
  // WIFI_BLE_P deliberately keeps both interfaces awake for persistence tests.
  if (!wifiRunning_) {
    Serial.println("[RADIO][COEX] WiFi unexpectedly offline in combined profile; restarting configured role");
    Serial.flush();
    startWifi();
  }

  if (isPersistentWifiBleProfile()) {
    if (bleConnected) {
      bleWebHandoffArmed_ = false;
      bleWebHandoffDeadlineMs_ = 0;
    }
    if (bleDormantForWifi_ || transports_.isBleDormant()) {
      transports_.setBleDormant(false);
      bleDormantForWifi_ = false;
      publish(
        EventLevel::STATUS,
        CommandSource::INTERNAL,
        String(),
        "radio coexistence=persistent; WiFi remains active and BLE is not intentionally dormant"
      );
    }
    return;
  }

  if (bleConnected) {
    bleWebHandoffArmed_ = false;
    bleWebHandoffDeadlineMs_ = 0;
    if (bleDormantForWifi_ || transports_.isBleDormant()) {
      transports_.setBleDormant(false);
      bleDormantForWifi_ = false;
    }
    return;
  }

  // During a browser BLE connection window, keep advertising awake even when
  // a Wi-Fi station is associated. Wi-Fi remains online throughout the switch.
  if (bleWebHandoffArmed_) {
    if (transports_.isBleDormant()) {
      transports_.setBleDormant(false);
    }
    bleDormantForWifi_ = false;
    return;
  }

  // When Wi-Fi has an associated client and BLE has no connection, stopping
  // advertising reduces background BLE airtime without tearing down either
  // stack. A connected BLE client is never put dormant.
  if (wifiClientConnected) {
    if (!bleDormantForWifi_) {
      transports_.setBleDormant(true);
      bleDormantForWifi_ = transports_.isBleDormant();
      if (bleDormantForWifi_) {
        publish(
          EventLevel::STATUS,
          CommandSource::INTERNAL,
          String(),
          "radio coexistence=WiFi active; idle BLE advertising dormant"
        );
      }
    }
    return;
  }

  if (
    bleDormantForWifi_ &&
    static_cast<uint32_t>(now - wifiClientLastSeenAtMs_) >=
      AppConfig::RADIO_HANDOFF_RELEASE_MS
  ) {
    transports_.setBleDormant(false);
    bleDormantForWifi_ = false;
  }
}

void RadioManager::applyWifiTxPower() {
  if (!wifiRunning_) {
    return;
  }
  if (!WiFi.setTxPower(static_cast<wifi_power_t>(config_.wifiTxPowerQuarterDbm))) {
    error(CommandSource::INTERNAL, String(), "WiFi transmit power setting was rejected");
  }
}

void RadioManager::startMdns() {
#if APP_MDNS_ENABLED
  if (mdnsRunning_) {
    return;
  }
  if (MDNS.begin(AppConfig::MDNS_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    mdnsRunning_ = true;
  }
#endif
}

void RadioManager::stopMdns() {
#if APP_MDNS_ENABLED
  if (!mdnsRunning_) {
    return;
  }
  MDNS.end();
  mdnsRunning_ = false;
#else
  mdnsRunning_ = false;
#endif
}

bool RadioManager::parseWifiPower(const String &value, int8_t &quarterDbm) const {
  String normalized = value;
  normalized.trim();
  normalized.toUpperCase();
  normalized.replace(" ", "");
  normalized.replace("_", "");
  normalized.replace("-", "");

  if (normalized == "LOW" || normalized == "LOWPOWER" || normalized == "ECO") {
    quarterDbm = AppConfig::WIFI_TX_POWER_LOW_QUARTER_DBM;
    return true;
  }
  if (normalized == "MAX" || normalized == "MAXIMUM" || normalized == "FULL") {
    quarterDbm = AppConfig::WIFI_TX_POWER_MAX_QUARTER_DBM;
    return true;
  }

  static const int8_t allowed[] = {
    AppConfig::WIFI_TX_POWER_MAX_QUARTER_DBM,
    76, 74, 68, 60, 52,
    AppConfig::WIFI_TX_POWER_LOW_QUARTER_DBM,
    34, 28, 20, 8, -4
  };
  float requested = 0.0f;
  if (!TextUtil::parseFloat(value, requested)) {
    return false;
  }

  const int requestedQuarter = static_cast<int>(
    requested * 4.0f + (requested >= 0.0f ? 0.5f : -0.5f)
  );
  if (fabsf(requested * 4.0f - static_cast<float>(requestedQuarter)) > 0.001f) {
    return false;
  }
  for (const int8_t candidate : allowed) {
    if (requestedQuarter == candidate) {
      quarterDbm = candidate;
      return true;
    }
  }
  return false;
}

String RadioManager::wifiPowerString(int8_t quarterDbm) const {
  const float dbm = static_cast<float>(quarterDbm) / 4.0f;
  return quarterDbm % 4 == 0 ? String(static_cast<int>(dbm)) : String(dbm, 1);
}

String RadioManager::roleString() const {
  if (fallbackActive_) {
    return "AP-FALLBACK";
  }
  if (config_.wifiRole == WifiRole::AP_STA) {
    return "AP+STA";
  }
  return config_.wifiRole == WifiRole::STA ? "STA" : "AP";
}

String RadioManager::runtimeState() const {
  if (!bootModeUsesWifi(activeBootMode_)) {
    return "disabled-by-boot-profile";
  }
  if (!wifiRunning_) {
    return "stopped";
  }
  const bool stationConnected = stationAttempted_ && WiFi.status() == WL_CONNECTED;
  if (apActive_ && stationConnected) {
    return "ap+sta-connected";
  }
  if (apActive_ && stationAttempted_) {
    return "ap+sta-connecting";
  }
  if (apActive_) {
    return "access-point";
  }
  return stationConnected ? "connected" : stationAttempted_ ? "connecting" : "stopped";
}

String RadioManager::ipString() const {
  if (!wifiRunning_) {
    return "0.0.0.0";
  }
  if (stationAttempted_ && WiFi.status() == WL_CONNECTED) {
    return WiFi.localIP().toString();
  }
  if (apActive_) {
    return WiFi.softAPIP().toString();
  }
  return "0.0.0.0";
}

void RadioManager::publishConfigReadback(
  CommandSource source,
  const String &requestId,
  const String &reason
) {
  const bool stationConnected =
    wifiRunning_ && stationAttempted_ && WiFi.status() == WL_CONNECTED;

  publish(
    EventLevel::STATUS,
    source,
    requestId,
    "[CONFIG] radio " + reason +
      " bootDesired=" + bootModeString(config_.bootMode) +
      " bootActive=" + bootModeString(activeBootMode_) +
      " rebootRequired=" + TextUtil::boolWord(bootModeNeedsReboot()) +
      " wifiDesired=" + TextUtil::boolWord(config_.wifiEnabled) +
      " runtime=" + runtimeState() +
      " mode=" + roleString() +
      " txPowerDbm=" + wifiPowerString(config_.wifiTxPowerQuarterDbm) +
      " web=" + TextUtil::boolWord(webPortal_.isRunning()) +
      " dns=" + TextUtil::boolWord(dnsRunning_) +
      " fallbackAP=" + TextUtil::boolWord(config_.fallbackAp)
  );
  publish(
    EventLevel::STATUS,
    source,
    requestId,
    "[CONFIG] bootSafety trial=" + TextUtil::boolWord(trialBootActive_) +
      " lastGood=" + bootModeString(lastGoodBootMode_) +
      " pending=" + bootModeString(pendingBootMode_) +
      " rebootScheduled=" + TextUtil::boolWord(managedRebootPending_) +
      " freeHeap=" + String(ESP.getFreeHeap()) +
      " largestBlock=" + String(ESP.getMaxAllocHeap())
  );
  publish(
    EventLevel::STATUS,
    source,
    requestId,
    "[CONFIG] AP ssid=" + config_.apSsid +
      " passwordSet=" + TextUtil::boolWord(config_.apPassword.length() > 0) +
      " passwordLength=" + String(config_.apPassword.length()) +
      " active=" + TextUtil::boolWord(apActive_) +
      " ip=" + String(apActive_ ? WiFi.softAPIP().toString() : "0.0.0.0") +
      " clients=" + String(apActive_ ? WiFi.softAPgetStationNum() : 0)
  );
  publish(
    EventLevel::STATUS,
    source,
    requestId,
    "[CONFIG] STA ssid=" + config_.staSsid +
      " passwordSet=" + TextUtil::boolWord(config_.staPassword.length() > 0) +
      " passwordLength=" + String(config_.staPassword.length()) +
      " credentials=" + String(hasUsableStationCredentials() ? "usable" : "absent-or-placeholder") +
      " attempted=" + TextUtil::boolWord(stationAttempted_) +
      " connected=" + TextUtil::boolWord(stationConnected) +
      " ip=" + String(stationConnected ? WiFi.localIP().toString() : "0.0.0.0")
  );
  publish(
    EventLevel::STATUS,
    source,
    requestId,
    "[CONFIG] Bluetooth bleDesired=" + TextUtil::boolWord(config_.bleEnabled) +
      " bleRuntime=" + TextUtil::boolWord(transports_.isBleRunning()) +
      " bleConnected=" + String(transports_.isBleConnected() ? "yes" : "no") +
      " bleDormant=" + TextUtil::boolWord(transports_.isBleDormant()) +
      " sppDesired=" + TextUtil::boolWord(config_.sppEnabled) +
      " sppRuntime=" + TextUtil::boolWord(transports_.isSppRunning()) +
      " sppConnected=" + String(transports_.isSppConnected() ? "yes" : "no")
  );
}

void RadioManager::publish(
  EventLevel level,
  CommandSource source,
  const String &requestId,
  const String &message
) {
  events_.publish(level, message, source, requestId);
}

void RadioManager::error(
  CommandSource source,
  const String &requestId,
  const String &message
) {
  events_.publish(EventLevel::ERROR, message, source, requestId);
}
