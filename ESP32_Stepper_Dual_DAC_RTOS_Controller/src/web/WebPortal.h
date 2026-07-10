#ifndef ESP32_STEPPER_DUAL_DAC_WEBPORTAL_H
#define ESP32_STEPPER_DUAL_DAC_WEBPORTAL_H

#include <Arduino.h>

#include "../config/AppConfig.h"
#include "../core/AppTypes.h"
#include "../core/EventBus.h"

#if APP_WIFI_ENABLED
// Dependency order matters here. ESPAsyncWebServer 3.11.x requires the matching
// ESP32Async AsyncTCP API where AsyncServer::status() is const.
#include <AsyncTCP.h>

#ifndef ASYNCTCP_FORK_ESP32Async
#error Remove old/duplicate AsyncTCP libraries and install "Async TCP" by ESP32Async, version 3.4.10 or newer. Run tools/install_async_libraries.cmd on Windows.
#endif

#ifndef ASYNCTCP_VERSION_NUM
#error AsyncTCP is too old to report its version. Install "Async TCP" by ESP32Async, version 3.4.10 or newer.
#elif ASYNCTCP_VERSION_NUM < ASYNCTCP_VERSION_VAL(3, 4, 10)
#error AsyncTCP is too old for this ESPAsyncWebServer build. Install "Async TCP" by ESP32Async, version 3.4.10 or newer.
#endif

#include <ESPAsyncWebServer.h>

#ifndef ASYNCWEBSERVER_FORK_ESP32Async
#error Install the maintained "ESP Async WebServer" by ESP32Async, version 3.11.2 or newer.
#endif

#ifndef ASYNCWEBSERVER_VERSION_NUM
#error ESPAsyncWebServer is too old to report its version. Install the ESP32Async version 3.11.2 or newer.
#elif ASYNCWEBSERVER_VERSION_NUM < ASYNCWEBSERVER_VERSION_VAL(3, 11, 2)
#error ESPAsyncWebServer is too old. Install the ESP32Async version 3.11.2 or newer.
#endif

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#endif

// EDIT HERE for HTTP routes, retry/idempotency policy, CORS, or browser API behavior.
// The hosted portal is generated as one self-contained PROGMEM page from
// web/index.html, web/app.css, and web/app.js.
// web/standalone_console.html is intentionally never embedded or served.
// Keep command execution behind BatchCommandSubmitter so HTTP cannot bypass queues.
#if APP_WIFI_ENABLED
class WebPortal {
 public:
  explicit WebPortal(EventBus &events);

  void configure(
    BatchCommandSubmitter submitter,
    void *submitContext,
    StateProvider stateProvider,
    void *stateContext
  );

  void prepare();
  void start();
  // AsyncTCP handles sockets independently. This service call only refreshes the
  // thread-safe state snapshot returned by /api/state.
  void service();
  bool isRunning() const;
  uint32_t requestCount() const;
  uint32_t lastRequestAtMs() const;
  bool hasRecentClient(uint32_t maxAgeMs = 10000) const;
  void discardClientMetadata();

 private:
  static constexpr uint8_t REQUEST_HISTORY_SIZE = AppConfig::HTTP_REQUEST_HISTORY_SIZE;
  static constexpr uint32_t REQUEST_HISTORY_TTL_MS = AppConfig::HTTP_REQUEST_HISTORY_TTL_MS;

  struct RequestRecord {
    bool used;
    bool completed;
    char id[AppConfig::REQUEST_ID_MAX_BYTES + 1];
    uint16_t acceptedLines;
    uint32_t bodyHash;
    uint32_t latestEventId;
    uint32_t createdAtMs;
  };

  struct ActiveRequestRecord {
    bool used;
    AsyncWebServerRequest *request;
    char *ownedBody;
    size_t bodyCapacity;
    size_t bodyLength;
    bool bodyComplete;
    size_t reservedBytes;
    uint32_t startedAtMs;
    const char *routeLabel;
  };

  enum class RequestDecision : uint8_t {
    RESERVED,
    DUPLICATE,
    CONFLICT,
    IN_PROGRESS,
    HISTORY_BUSY
  };

  void installRoutes();
  bool admitRequest(
    AsyncWebServerRequest *request,
    size_t reservedBytes,
    const char *routeLabel
  );
  bool beginCommandBody(AsyncWebServerRequest *request, size_t totalBytes);
  bool appendCommandBody(
    AsyncWebServerRequest *request,
    const uint8_t *data,
    size_t length,
    size_t index,
    size_t totalBytes
  );
  bool takeCommandBody(AsyncWebServerRequest *request, String &body);
  bool isRequestActive(AsyncWebServerRequest *request) const;
  void releaseRequest(AsyncWebServerRequest *request);
  void rejectForPressure(AsyncWebServerRequest *request);
  void clearRequestHistoryUnlocked();
  void noteRequest(AsyncWebServerRequest *request, const char *routeLabel, bool verbose);
  void logRequest(AsyncWebServerRequest *request, const char *routeLabel = nullptr);
  void sendAsset(
    AsyncWebServerRequest *request,
    const char *contentType,
    const uint8_t *content,
    size_t length,
    const char *contentEncoding = nullptr
  );
  void sendJson(AsyncWebServerRequest *request, int statusCode, String json);
  void sendText(
    AsyncWebServerRequest *request,
    int statusCode,
    const char *contentType,
    const String &body
  );
  void sendEmpty(AsyncWebServerRequest *request, int statusCode);
  void handleState(AsyncWebServerRequest *request);
  void handleEvents(AsyncWebServerRequest *request);
  void handleCommand(AsyncWebServerRequest *request);
  void handleCommandBody(
    AsyncWebServerRequest *request,
    uint8_t *data,
    size_t length,
    size_t index,
    size_t totalBytes
  );
#if APP_HTTP_OTA_ENABLED
  void handleOtaUpload(
    AsyncWebServerRequest *request,
    const String &filename,
    size_t index,
    uint8_t *data,
    size_t length,
    bool final
  );
  void handleOtaComplete(AsyncWebServerRequest *request);
  void setOtaError(const String &message);
  void resetOtaState();
#endif
  void handleNotFound(AsyncWebServerRequest *request);
  void refreshStateCache(bool force);

  bool validateRequestId(const String &requestId, String &reason) const;
  RequestDecision inspectOrReserveRequest(
    const String &requestId,
    uint32_t bodyHash,
    RequestRecord &snapshot
  );
  void completeRequest(
    const String &requestId,
    uint16_t acceptedLines,
    uint32_t latestEventId
  );
  void removeReservation(const String &requestId);
  RequestRecord *findRequestUnlocked(const String &requestId, uint32_t now);
  RequestRecord *findWritableRecordUnlocked(uint32_t now);
  void expireRecordUnlocked(RequestRecord &record, uint32_t now);

  uint32_t hashBody(const String &body) const;
  String commandResponseJson(
    bool accepted,
    bool duplicate,
    bool retryable,
    const String &requestId,
    uint16_t acceptedLines,
    uint32_t latestEventId,
    const String &error
  ) const;

  EventBus &events_;
  AsyncWebServer server_;
  BatchCommandSubmitter submitter_;
  void *submitContext_;
  StateProvider stateProvider_;
  void *stateContext_;
  bool routesInstalled_;
  bool prepared_;
  bool running_;
  volatile uint32_t requestCount_;
  volatile uint32_t lastRequestAtMs_;

  SemaphoreHandle_t sharedMutex_;
  String cachedStateJson_;
  uint32_t stateCachedAtMs_;
  RequestRecord requestHistory_[REQUEST_HISTORY_SIZE];
  uint8_t requestHistoryHead_;
  mutable portMUX_TYPE activeRequestMux_;
  ActiveRequestRecord activeRequests_[AppConfig::HTTP_MAX_ACTIVE_REQUESTS];
  volatile uint8_t activeRequestCount_;
  volatile size_t activeReservedBytes_;
  volatile uint32_t pressureRejectCount_;
#if APP_HTTP_OTA_ENABLED
  bool otaUploadActive_;
  bool otaUploadSuccess_;
  bool otaImageValidated_;
  bool otaRebootPending_;
  uint32_t otaRebootAtMs_;
  size_t otaBytesWritten_;
  size_t otaExpectedBytes_;
  AsyncWebServerRequest *otaRequest_;
  String otaLastError_;
#endif
};
#else
class WebPortal {
 public:
  explicit WebPortal(EventBus &events);

  void configure(
    BatchCommandSubmitter submitter,
    void *submitContext,
    StateProvider stateProvider,
    void *stateContext
  );
  void prepare();
  void start();
  void service();
  bool isRunning() const;
  uint32_t requestCount() const;
  uint32_t lastRequestAtMs() const;
  bool hasRecentClient(uint32_t maxAgeMs = 10000) const;
  void discardClientMetadata();

 private:
  EventBus &events_;
};
#endif

#endif  // ESP32_STEPPER_DUAL_DAC_WEBPORTAL_H
