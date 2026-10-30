#include "WebPortal.h"

#if APP_WIFI_ENABLED
#include "../util/TextUtil.h"
#include "WebAssets.h"

#include <utility>
#include <stdlib.h>
#include <string.h>

#if APP_HTTP_OTA_ENABLED
#include <Update.h>
#endif

WebPortal::WebPortal(EventBus &events)
  : events_(events),
    server_(80),
    submitter_(nullptr),
    submitContext_(nullptr),
    stateProvider_(nullptr),
    stateContext_(nullptr),
    routesInstalled_(false),
    prepared_(false),
    running_(false),
    requestCount_(0),
    lastRequestAtMs_(0),
    sharedMutex_(nullptr),
    cachedStateJson_("{\"ok\":false,\"error\":\"state not ready\"}"),
    stateCachedAtMs_(0),
    requestHistory_{},
    requestHistoryHead_(0),
    activeRequestMux_(portMUX_INITIALIZER_UNLOCKED),
    activeRequests_{},
    activeRequestCount_(0),
    activeReservedBytes_(0),
    pressureRejectCount_(0)
#if APP_HTTP_OTA_ENABLED
    , otaUploadActive_(false),
    otaUploadSuccess_(false),
    otaImageValidated_(false),
    otaRebootPending_(false),
    otaRebootAtMs_(0),
    otaBytesWritten_(0),
    otaExpectedBytes_(0),
    otaRequest_(nullptr),
    otaLastError_()
#endif
{}

void WebPortal::configure(
  BatchCommandSubmitter submitter,
  void *submitContext,
  StateProvider stateProvider,
  void *stateContext
) {
  submitter_ = submitter;
  submitContext_ = submitContext;
  stateProvider_ = stateProvider;
  stateContext_ = stateContext;
}

void WebPortal::prepare() {
  if (prepared_) {
    return;
  }
  if (sharedMutex_ == nullptr) {
    // Reserve the portal's fixed runtime objects before a coexistence profile
    // initializes Bluedroid. This avoids asking the fragmented post-BLE heap for
    // the mutex, route closures, and state-cache capacity all at once.
    sharedMutex_ = xSemaphoreCreateMutex();
  }
  if (sharedMutex_ == nullptr) {
    events_.publish(
      EventLevel::ERROR,
      "async web server could not allocate its synchronization mutex",
      CommandSource::INTERNAL
    );
    return;
  }

  installRoutes();
  cachedStateJson_.reserve(AppConfig::WEB_STATE_JSON_BUDGET_BYTES);
  refreshStateCache(true);
  prepared_ = true;
  events_.publish(
    EventLevel::STATUS,
    "HTTP portal memory and routes prepared",
    CommandSource::INTERNAL
  );
}

void WebPortal::start() {
  if (running_) {
    events_.publish(
      EventLevel::STATUS,
      "HTTP listener already active; WiFi interface rebound without restarting port 80",
      CommandSource::INTERNAL
    );
    return;
  }

  prepare();
  if (!prepared_) {
    return;
  }

#if APP_VERBOSE_COEX_HTTP_DIAGNOSTICS
  const uint32_t heapBefore = ESP.getFreeHeap();
  const uint32_t largestBefore = ESP.getMaxAllocHeap();
  Serial.printf(
    "[BOOT][HTTP][MEM] before listener free=%u min=%u largest=%u tcpStack=%u core=%d\n",
    static_cast<unsigned>(heapBefore),
    static_cast<unsigned>(ESP.getMinFreeHeap()),
    static_cast<unsigned>(largestBefore),
    static_cast<unsigned>(CONFIG_ASYNC_TCP_STACK_SIZE),
    static_cast<int>(CONFIG_ASYNC_TCP_RUNNING_CORE)
  );
#endif

  server_.begin();
  delay(20);
  running_ = true;

#if APP_VERBOSE_COEX_HTTP_DIAGNOSTICS
  const uint32_t heapAfter = ESP.getFreeHeap();
  Serial.printf(
    "[BOOT][HTTP][MEM] after listener free=%u min=%u largest=%u heapDelta=%d\n",
    static_cast<unsigned>(heapAfter),
    static_cast<unsigned>(ESP.getMinFreeHeap()),
    static_cast<unsigned>(ESP.getMaxAllocHeap()),
    static_cast<int32_t>(heapAfter) - static_cast<int32_t>(heapBefore)
  );
  Serial.flush();
#endif

  events_.publish(
    EventLevel::STATUS,
    "async embedded web console listening on port 80",
    CommandSource::INTERNAL
  );
}

void WebPortal::service() {
  if (running_) {
    refreshStateCache(false);
  }
#if APP_HTTP_OTA_ENABLED
  if (otaRebootPending_ && static_cast<int32_t>(millis() - otaRebootAtMs_) >= 0) {
    Serial.println("[OTA] validated image selected; rebooting");
    Serial.flush();
    ESP.restart();
  }
#endif
}

bool WebPortal::isRunning() const {
  return running_;
}

uint32_t WebPortal::requestCount() const {
  return requestCount_;
}

uint32_t WebPortal::lastRequestAtMs() const {
  return lastRequestAtMs_;
}

bool WebPortal::hasRecentClient(uint32_t maxAgeMs) const {
  const uint32_t last = lastRequestAtMs_;
  return running_ && requestCount_ > 0 && last != 0 &&
    static_cast<uint32_t>(millis() - last) <= maxAgeMs;
}

void WebPortal::discardClientMetadata() {
  lastRequestAtMs_ = 0;
  if (sharedMutex_ == nullptr) {
    return;
  }
  if (xSemaphoreTake(sharedMutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
    clearRequestHistoryUnlocked();
    xSemaphoreGive(sharedMutex_);
  }
}

bool WebPortal::admitRequest(
  AsyncWebServerRequest *request,
  size_t reservedBytes,
  const char *routeLabel
) {
  if (request == nullptr || request->client() == nullptr) {
    return false;
  }

  request->client()->setRxTimeout(AppConfig::HTTP_CLIENT_RX_TIMEOUT_SECONDS);
  request->client()->setAckTimeout(AppConfig::HTTP_CLIENT_ACK_TIMEOUT_MS);
  request->client()->setNoDelay(true);

#if APP_HTTP_OTA_ENABLED
  if (otaUploadActive_ && otaRequest_ != request) {
    request->abort();
    return false;
  }
#endif

  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t largestBlock = ESP.getMaxAllocHeap();
  if (
    reservedBytes > AppConfig::HTTP_MAX_ACTIVE_RESERVED_BYTES ||
    freeHeap < AppConfig::HTTP_REMAINING_FREE_HEAP_BYTES + reservedBytes ||
    largestBlock < AppConfig::HTTP_MIN_LARGEST_BLOCK_BYTES
  ) {
    rejectForPressure(request);
    return false;
  }

  bool admitted = false;
  portENTER_CRITICAL(&activeRequestMux_);
  if (
    activeRequestCount_ < AppConfig::HTTP_MAX_ACTIVE_REQUESTS &&
    activeReservedBytes_ + reservedBytes <= AppConfig::HTTP_MAX_ACTIVE_RESERVED_BYTES
  ) {
    for (ActiveRequestRecord &record : activeRequests_) {
      if (record.used) {
        continue;
      }
      record.used = true;
      record.request = request;
      record.ownedBody = nullptr;
      record.bodyCapacity = 0;
      record.bodyLength = 0;
      record.bodyComplete = false;
      record.reservedBytes = reservedBytes;
      record.startedAtMs = millis();
      record.routeLabel = routeLabel;
      activeRequestCount_++;
      activeReservedBytes_ += reservedBytes;
      admitted = true;
      break;
    }
  }
  portEXIT_CRITICAL(&activeRequestMux_);

  if (!admitted) {
    rejectForPressure(request);
    return false;
  }

  request->onDisconnect([this, request]() { releaseRequest(request); });
  return true;
}

bool WebPortal::beginCommandBody(
  AsyncWebServerRequest *request,
  size_t totalBytes
) {
  if (
    request == nullptr ||
    totalBytes == 0 ||
    totalBytes > AppConfig::HTTP_COMMAND_MAX_BYTES
  ) {
    rejectForPressure(request);
    return false;
  }

  const size_t bodyCapacity = totalBytes + 1;
  const size_t reservation = AppConfig::HTTP_JSON_RESPONSE_BUDGET_BYTES + bodyCapacity;
  if (!admitRequest(request, reservation, "command")) {
    return false;
  }

  char *body = static_cast<char *>(malloc(bodyCapacity));
  if (body == nullptr) {
    rejectForPressure(request);
    return false;
  }
  body[0] = '\0';

  bool attached = false;
  portENTER_CRITICAL(&activeRequestMux_);
  for (ActiveRequestRecord &record : activeRequests_) {
    if (record.used && record.request == request && record.ownedBody == nullptr) {
      record.ownedBody = body;
      record.bodyCapacity = bodyCapacity;
      record.bodyLength = 0;
      record.bodyComplete = false;
      attached = true;
      break;
    }
  }
  portEXIT_CRITICAL(&activeRequestMux_);

  if (!attached) {
    free(body);
    rejectForPressure(request);
    return false;
  }
  return true;
}

bool WebPortal::appendCommandBody(
  AsyncWebServerRequest *request,
  const uint8_t *data,
  size_t length,
  size_t index,
  size_t totalBytes
) {
  if (request == nullptr || data == nullptr || length == 0) {
    return false;
  }

  bool accepted = false;
  portENTER_CRITICAL(&activeRequestMux_);
  for (ActiveRequestRecord &record : activeRequests_) {
    if (!record.used || record.request != request || record.ownedBody == nullptr) {
      continue;
    }
    if (
      totalBytes + 1 != record.bodyCapacity ||
      index != record.bodyLength ||
      index > totalBytes ||
      length > totalBytes - index ||
      index + length >= record.bodyCapacity
    ) {
      break;
    }
    memcpy(record.ownedBody + index, data, length);
    record.bodyLength += length;
    record.ownedBody[record.bodyLength] = '\0';
    record.bodyComplete = record.bodyLength == totalBytes;
    accepted = true;
    break;
  }
  portEXIT_CRITICAL(&activeRequestMux_);

  if (!accepted) {
    rejectForPressure(request);
  }
  return accepted;
}

bool WebPortal::takeCommandBody(
  AsyncWebServerRequest *request,
  String &body
) {
  char *ownedBody = nullptr;
  size_t bodyCapacity = 0;
  size_t bodyLength = 0;

  portENTER_CRITICAL(&activeRequestMux_);
  for (ActiveRequestRecord &record : activeRequests_) {
    if (
      !record.used ||
      record.request != request ||
      record.ownedBody == nullptr ||
      !record.bodyComplete
    ) {
      continue;
    }
    ownedBody = record.ownedBody;
    bodyCapacity = record.bodyCapacity;
    bodyLength = record.bodyLength;
    record.ownedBody = nullptr;
    record.bodyCapacity = 0;
    record.bodyLength = 0;
    record.bodyComplete = false;
    if (record.reservedBytes >= bodyCapacity) {
      record.reservedBytes -= bodyCapacity;
    }
    if (activeReservedBytes_ >= bodyCapacity) {
      activeReservedBytes_ -= bodyCapacity;
    } else {
      activeReservedBytes_ = 0;
    }
    break;
  }
  portEXIT_CRITICAL(&activeRequestMux_);

  if (ownedBody == nullptr) {
    return false;
  }
  body.reserve(bodyLength + 1);
  body = ownedBody;
  free(ownedBody);
  return body.length() == bodyLength;
}

bool WebPortal::isRequestActive(AsyncWebServerRequest *request) const {
  bool active = false;
  portENTER_CRITICAL(&activeRequestMux_);
  for (const ActiveRequestRecord &record : activeRequests_) {
    if (record.used && record.request == request) {
      active = true;
      break;
    }
  }
  portEXIT_CRITICAL(&activeRequestMux_);
  return active;
}

void WebPortal::releaseRequest(AsyncWebServerRequest *request) {
  char *ownedBody = nullptr;
#if APP_HTTP_OTA_ENABLED
  const bool interruptedOta = otaUploadActive_ && otaRequest_ == request;
#endif
#if APP_VERBOSE_COEX_HTTP_DIAGNOSTICS
  const char *routeLabel = nullptr;
  uint32_t startedAtMs = 0;
#endif
  portENTER_CRITICAL(&activeRequestMux_);
  for (ActiveRequestRecord &record : activeRequests_) {
    if (!record.used || record.request != request) {
      continue;
    }
    ownedBody = record.ownedBody;
#if APP_VERBOSE_COEX_HTTP_DIAGNOSTICS
    routeLabel = record.routeLabel;
    startedAtMs = record.startedAtMs;
#endif
    if (activeRequestCount_ > 0) {
      activeRequestCount_--;
    }
    if (activeReservedBytes_ >= record.reservedBytes) {
      activeReservedBytes_ -= record.reservedBytes;
    } else {
      activeReservedBytes_ = 0;
    }
    record = ActiveRequestRecord{};
    break;
  }
  portEXIT_CRITICAL(&activeRequestMux_);
  free(ownedBody);
#if APP_HTTP_OTA_ENABLED
  if (interruptedOta) {
    Update.abort();
    otaUploadActive_ = false;
    otaUploadSuccess_ = false;
    otaImageValidated_ = false;
    otaRebootPending_ = false;
    otaRequest_ = nullptr;
    otaLastError_ = "OTA client disconnected before the image completed";
    events_.publish(EventLevel::ERROR, "[OTA] " + otaLastError_, CommandSource::INTERNAL);
    Serial.println("[OTA][ERROR] " + otaLastError_);
    Serial.flush();
  }
#endif
#if APP_VERBOSE_COEX_HTTP_DIAGNOSTICS
  if (startedAtMs != 0) {
    Serial.printf(
      "[HTTP][RELEASE] route=%s ageMs=%u active=%u bytes=%u free=%u largest=%u\n",
      routeLabel != nullptr ? routeLabel : "?",
      static_cast<unsigned>(millis() - startedAtMs),
      static_cast<unsigned>(activeRequestCount_),
      static_cast<unsigned>(activeReservedBytes_),
      static_cast<unsigned>(ESP.getFreeHeap()),
      static_cast<unsigned>(ESP.getMaxAllocHeap())
    );
    Serial.flush();
  }
#endif
}

void WebPortal::rejectForPressure(AsyncWebServerRequest *request) {
  pressureRejectCount_++;
  if (request == nullptr || request->client() == nullptr) {
    return;
  }
#if APP_VERBOSE_COEX_HTTP_DIAGNOSTICS
  Serial.printf(
    "[HTTP][DROP] active=%u bytes=%u free=%u largest=%u rejects=%u\n",
    static_cast<unsigned>(activeRequestCount_),
    static_cast<unsigned>(activeReservedBytes_),
    static_cast<unsigned>(ESP.getFreeHeap()),
    static_cast<unsigned>(ESP.getMaxAllocHeap()),
    static_cast<unsigned>(pressureRejectCount_)
  );
  Serial.flush();
#endif
  // Do not allocate a 503 response when the admission guard is already under
  // pressure. Aborting the newest socket lets AsyncWebServer discard its parsed
  // request/body immediately; the browser's serialized retry loop backs off.
  request->abort();
}

void WebPortal::clearRequestHistoryUnlocked() {
  for (RequestRecord &record : requestHistory_) {
    record = RequestRecord{};
  }
  requestHistoryHead_ = 0;
}

// ADD HTTP ROUTES HERE. Command routes must keep request-ID deduplication and
// submit through the unified dispatcher rather than calling controllers directly.
void WebPortal::installRoutes() {
  if (routesInstalled_) {
    return;
  }

  // CORS headers are attached directly to API responses. Keeping the static
  // control page independent of global middleware avoids library-version-specific
  // middleware behavior from preventing the root page from being served.

  server_.on("/", HTTP_GET, [this](AsyncWebServerRequest *request) {
    if (!admitRequest(request, 768, "root")) return;
    logRequest(request, "root");
    sendAsset(
      request,
      WebAssets::PORTAL_HTML_CONTENT_TYPE,
      WebAssets::PORTAL_HTML_GZIP,
      WebAssets::PORTAL_HTML_GZIP_LENGTH,
      WebAssets::PORTAL_HTML_CONTENT_ENCODING
    );
  });
  server_.on("/api/ping", HTTP_GET, [this](AsyncWebServerRequest *request) {
    if (!admitRequest(request, 128, "ping")) return;
    logRequest(request, "ping");
    sendJson(request, 200, "{\"ok\":true}");
  });
  server_.on("/api/state", HTTP_GET, [this](AsyncWebServerRequest *request) {
    if (!admitRequest(request, AppConfig::WEB_STATE_JSON_BUDGET_BYTES, "state")) return;
    noteRequest(request, "state", false);
    handleState(request);
  });
  server_.on("/api/events", HTTP_GET, [this](AsyncWebServerRequest *request) {
    if (!admitRequest(request, AppConfig::WEB_EVENT_JSON_BUDGET_BYTES, "events")) return;
    noteRequest(request, "events", false);
    handleEvents(request);
  });
  server_.on(
    "/api/command",
    HTTP_POST,
    [this](AsyncWebServerRequest *request) {
      if (!isRequestActive(request)) {
        if (!admitRequest(request, AppConfig::HTTP_JSON_RESPONSE_BUDGET_BYTES, "command-empty")) return;
      }
      logRequest(request, "command");
      handleCommand(request);
    },
    nullptr,
    [this](
      AsyncWebServerRequest *request,
      uint8_t *data,
      size_t length,
      size_t index,
      size_t totalBytes
    ) {
      handleCommandBody(request, data, length, index, totalBytes);
    }
  );
#if APP_HTTP_OTA_ENABLED
  server_.on(
    "/api/ota",
    HTTP_POST,
    [this](AsyncWebServerRequest *request) { handleOtaComplete(request); },
    [this](
      AsyncWebServerRequest *request,
      const String &filename,
      size_t index,
      uint8_t *data,
      size_t length,
      bool final
    ) {
      handleOtaUpload(request, filename, index, data, length, final);
    }
  );
#endif
  server_.on("/favicon.ico", HTTP_ANY, [this](AsyncWebServerRequest *request) {
    if (!admitRequest(request, 64, "favicon")) return;
    noteRequest(request, "favicon", false);
    sendEmpty(request, 204);
  });

  // Return each operating system's expected connectivity-check response. This
  // prevents Android, iOS, Windows, Chrome, and Firefox from opening a captive
  // sign-in mini-browser for this password-protected local control network.
  server_.on("/generate_204", HTTP_ANY, [this](AsyncWebServerRequest *request) {
    if (!admitRequest(request, 64, "generate_204")) return;
    noteRequest(request, "generate_204", false);
    sendEmpty(request, 204);
  });
  server_.on("/gen_204", HTTP_ANY, [this](AsyncWebServerRequest *request) {
    if (!admitRequest(request, 64, "gen_204")) return;
    noteRequest(request, "gen_204", false);
    sendEmpty(request, 204);
  });
  server_.on("/hotspot-detect.html", HTTP_ANY, [this](AsyncWebServerRequest *request) {
    if (!admitRequest(request, 256, "hotspot-detect")) return;
    noteRequest(request, "hotspot-detect", false);
    sendText(
      request,
      200,
      "text/html; charset=utf-8",
      "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>"
    );
  });
  server_.on("/library/test/success.html", HTTP_ANY, [this](AsyncWebServerRequest *request) {
    if (!admitRequest(request, 256, "apple-success")) return;
    noteRequest(request, "apple-success", false);
    sendText(
      request,
      200,
      "text/html; charset=utf-8",
      "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>"
    );
  });
  server_.on("/connecttest.txt", HTTP_ANY, [this](AsyncWebServerRequest *request) {
    if (!admitRequest(request, 128, "connecttest")) return;
    noteRequest(request, "connecttest", false);
    sendText(request, 200, "text/plain; charset=utf-8", "Microsoft Connect Test");
  });
  server_.on("/ncsi.txt", HTTP_ANY, [this](AsyncWebServerRequest *request) {
    if (!admitRequest(request, 128, "ncsi")) return;
    noteRequest(request, "ncsi", false);
    sendText(request, 200, "text/plain; charset=utf-8", "Microsoft NCSI");
  });
  server_.on("/canonical.html", HTTP_ANY, [this](AsyncWebServerRequest *request) {
    if (!admitRequest(request, 128, "canonical")) return;
    noteRequest(request, "canonical", false);
    sendText(request, 200, "text/plain; charset=utf-8", "success\n");
  });
  server_.on("/success.txt", HTTP_ANY, [this](AsyncWebServerRequest *request) {
    if (!admitRequest(request, 128, "success")) return;
    noteRequest(request, "success", false);
    sendText(request, 200, "text/plain; charset=utf-8", "success\n");
  });
  server_.onNotFound([this](AsyncWebServerRequest *request) {
    if (!admitRequest(request, 768, "not-found")) return;
    handleNotFound(request);
  });

  routesInstalled_ = true;
}

void WebPortal::noteRequest(
  AsyncWebServerRequest *request,
  const char *routeLabel,
  bool verbose
) {
  requestCount_++;
  lastRequestAtMs_ = millis();

#if APP_VERBOSE_COEX_HTTP_DIAGNOSTICS
  if (!verbose) {
    return;
  }

  IPAddress remoteIp;
  bool hasRemote = false;
  if (request != nullptr && request->client() != nullptr) {
    remoteIp = request->client()->remoteIP();
    hasRemote = true;
  }

  Serial.printf(
    "[HTTP][REQUEST] %s %s route=%s client=%s%u.%u.%u.%u free=%u largest=%u stack=%u count=%u\n",
    request != nullptr ? request->methodToString() : "?",
    request != nullptr ? request->url().c_str() : "?",
    routeLabel != nullptr ? routeLabel : "?",
    hasRemote ? "" : "unknown/",
    hasRemote ? remoteIp[0] : 0,
    hasRemote ? remoteIp[1] : 0,
    hasRemote ? remoteIp[2] : 0,
    hasRemote ? remoteIp[3] : 0,
    static_cast<unsigned>(ESP.getFreeHeap()),
    static_cast<unsigned>(ESP.getMaxAllocHeap()),
    static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
    static_cast<unsigned>(requestCount_)
  );
  Serial.flush();
#else
  (void)request;
  (void)routeLabel;
  (void)verbose;
#endif
}

void WebPortal::logRequest(
  AsyncWebServerRequest *request,
  const char *routeLabel
) {
  noteRequest(request, routeLabel, AppConfig::VERBOSE_COEX_HTTP_DIAGNOSTICS);
}

void WebPortal::sendAsset(
  AsyncWebServerRequest *request,
  const char *contentType,
  const uint8_t *content,
  size_t length,
  const char *contentEncoding
) {
#if APP_VERBOSE_COEX_HTTP_DIAGNOSTICS
  const uint32_t startedAtMs = millis();
  const uint32_t freeBefore = ESP.getFreeHeap();
  const uint32_t largestBefore = ESP.getMaxAllocHeap();
#endif

  // The fixed content length lets the browser know exactly when the response is
  // complete. RESPONSE_TRY_AGAIN forces a flush between bounded PROGMEM reads,
  // preventing AsyncTCP from retaining a full TCP window of portal data.
  AsyncWebServerResponse *response = request->beginResponse(
    contentType,
    length,
    [content, length, yieldBeforeNextChunk = false](
      uint8_t *buffer,
      size_t maxLength,
      size_t index
    ) mutable -> size_t {
      if (index >= length) {
        return 0;
      }

      if (yieldBeforeNextChunk) {
        yieldBeforeNextChunk = false;
        return RESPONSE_TRY_AGAIN;
      }

      size_t copyLength = length - index;
      if (copyLength > maxLength) {
        copyLength = maxLength;
      }
      if (copyLength > AppConfig::HTTP_ASSET_CHUNK_BYTES) {
        copyLength = AppConfig::HTTP_ASSET_CHUNK_BYTES;
      }

      memcpy_P(buffer, content + index, copyLength);
      yieldBeforeNextChunk = true;
      return copyLength;
    }
  );

  if (response == nullptr) {
#if APP_VERBOSE_COEX_HTTP_DIAGNOSTICS
    Serial.printf(
      "[HTTP][ASSET][FAIL] bytes=%u free=%u largest=%u\n",
      static_cast<unsigned>(length),
      static_cast<unsigned>(ESP.getFreeHeap()),
      static_cast<unsigned>(ESP.getMaxAllocHeap())
    );
    Serial.flush();
#endif
    releaseRequest(request);
    if (request->client() != nullptr) {
      request->abort();
    }
    return;
  }

  if (contentEncoding != nullptr && contentEncoding[0] != '\0') {
    response->addHeader("Content-Encoding", contentEncoding);
    response->addHeader("Vary", "Accept-Encoding");
  }
  response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  response->addHeader("Pragma", "no-cache");
  response->addHeader("Expires", "0");
  response->addHeader("X-Content-Type-Options", "nosniff");
  response->addHeader("Connection", "close");

#if APP_VERBOSE_COEX_HTTP_DIAGNOSTICS
  Serial.printf(
    "[HTTP][ASSET] start bytes=%u chunk=%u free=%u largest=%u\n",
    static_cast<unsigned>(length),
    static_cast<unsigned>(AppConfig::HTTP_ASSET_CHUNK_BYTES),
    static_cast<unsigned>(freeBefore),
    static_cast<unsigned>(largestBefore)
  );
  Serial.flush();
#endif
  request->send(response);
}

void WebPortal::sendJson(
  AsyncWebServerRequest *request,
  int statusCode,
  String json
) {
  const size_t length = json.length();
  if (
    ESP.getFreeHeap() < AppConfig::HTTP_REMAINING_FREE_HEAP_BYTES + length ||
    ESP.getMaxAllocHeap() < AppConfig::HTTP_MIN_LARGEST_BLOCK_BYTES
  ) {
    rejectForPressure(request);
    return;
  }
  if (length > AppConfig::HTTP_JSON_RESPONSE_BUDGET_BYTES) {
    request->send(507, "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"response budget exceeded\"}");
    return;
  }

  // AsyncBasicResponse owns its String content and is deleted with the request.
  // This is smaller and safer for bounded JSON than AsyncCallbackResponse, which
  // also allocates a two-MSS intermediate send buffer.
  AsyncWebServerResponse *response = request->beginResponse(
    statusCode,
    "application/json; charset=utf-8",
    json
  );
  json = String();
  if (response == nullptr) {
    releaseRequest(request);
    if (request->client() != nullptr) {
      request->abort();
    }
    return;
  }

  response->addHeader("Cache-Control", "no-store");
  response->addHeader("X-Content-Type-Options", "nosniff");
  response->addHeader("Access-Control-Allow-Origin", "*");
  response->addHeader("Connection", "close");
  request->send(response);
}

void WebPortal::sendText(
  AsyncWebServerRequest *request,
  int statusCode,
  const char *contentType,
  const String &body
) {
  AsyncWebServerResponse *response = request->beginResponse(statusCode, contentType, body);
  if (response == nullptr) {
    releaseRequest(request);
    if (request->client() != nullptr) {
      request->abort();
    }
    return;
  }
  response->addHeader("Cache-Control", "no-store");
  response->addHeader("X-Content-Type-Options", "nosniff");
  response->addHeader("Connection", "close");
  request->send(response);
}

void WebPortal::sendEmpty(AsyncWebServerRequest *request, int statusCode) {
  AsyncWebServerResponse *response = request->beginResponse(statusCode, "text/plain", "");
  if (response == nullptr) {
    releaseRequest(request);
    if (request->client() != nullptr) {
      request->abort();
    }
    return;
  }
  response->addHeader("Cache-Control", "no-store");
  response->addHeader("Access-Control-Allow-Origin", "*");
  response->addHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  response->addHeader("Access-Control-Allow-Headers", "Content-Type,X-Request-ID,X-Firmware-Size,Cache-Control");
  response->addHeader("Access-Control-Max-Age", "600");
  response->addHeader("Access-Control-Allow-Private-Network", "true");
  response->addHeader("Connection", "close");
  request->send(response);
}

void WebPortal::handleState(AsyncWebServerRequest *request) {
  if (sharedMutex_ == nullptr) {
    sendJson(request, 503, "{\"ok\":false,\"error\":\"state cache unavailable\"}");
    return;
  }

  String snapshot;
  if (xSemaphoreTake(sharedMutex_, pdMS_TO_TICKS(100)) != pdTRUE) {
    sendJson(request, 503, "{\"ok\":false,\"error\":\"state cache busy\"}");
    return;
  }
  snapshot = cachedStateJson_;
  xSemaphoreGive(sharedMutex_);
  sendJson(request, 200, std::move(snapshot));
}

void WebPortal::handleEvents(AsyncWebServerRequest *request) {
  uint32_t cursor = events_.latestId();
  if (request->hasParam("since")) {
    uint32_t parsed = 0;
    if (TextUtil::parseUnsigned32(request->getParam("since")->value(), parsed)) {
      cursor = parsed;
    }
  }

  uint8_t limit = AppConfig::WEB_EVENT_DEFAULT_LIMIT;
  if (request->hasParam("limit")) {
    uint32_t parsed = 0;
    if (TextUtil::parseUnsigned32(request->getParam("limit")->value(), parsed)) {
      limit = static_cast<uint8_t>(min<uint32_t>(max<uint32_t>(parsed, 1U), AppConfig::WEB_EVENT_MAX_LIMIT));
    }
  }

  bool anyGap = false;
  bool first = true;
  uint8_t emitted = 0;
  String json;
  json.reserve(AppConfig::WEB_EVENT_JSON_BUDGET_BYTES);
  json = "{\"ok\":true,\"events\":[";

  EventRecord record{};
  while (emitted < limit) {
    const uint32_t cursorBefore = cursor;
    bool gap = false;
    if (!events_.nextAfter(cursor, record, gap)) {
      anyGap = anyGap || gap;
      break;
    }
    anyGap = anyGap || gap;
    if ((record.targetMask & TRANSPORT_MASK_WIFI) == 0) {
      continue;
    }

    String text(record.text);
    if (!record.rawPayload && text.length() > 120) {
      text.remove(120);
    }

    String item;
    item.reserve(220);
    if (record.rawPayload) {
      item = "{\"q\":1,\"t\":\"" + TextUtil::jsonEscape(text) + "\"}";
    } else {
      item = "{\"l\":\"" + String(eventLevelName(record.level));
      item += "\",\"s\":\"" + String(commandSourceName(record.source)) + "\"";
      if (record.requestId[0] != '\0') {
        item += ",\"r\":\"" + TextUtil::jsonEscape(String(record.requestId)) + "\"";
      }
      item += ",\"t\":\"" + TextUtil::jsonEscape(text) + "\"}";
    }

    const size_t separatorBytes = first ? 0 : 1;
    if (json.length() + separatorBytes + item.length() + 64 > AppConfig::WEB_EVENT_JSON_BUDGET_BYTES) {
      cursor = cursorBefore;
      break;
    }
    if (!first) {
      json += ',';
    }
    first = false;
    json += item;
    emitted++;
  }

  json += "],\"cursor\":" + String(cursor);
  json += ",\"gap\":" + TextUtil::jsonBool(anyGap);
  json += ",\"more\":" + TextUtil::jsonBool(cursor < events_.latestId());
  json += "}";
  sendJson(request, 200, std::move(json));
}

void WebPortal::handleCommandBody(
  AsyncWebServerRequest *request,
  uint8_t *data,
  size_t length,
  size_t index,
  size_t totalBytes
) {
  if (index == 0 && !beginCommandBody(request, totalBytes)) {
    return;
  }
  appendCommandBody(request, data, length, index, totalBytes);
}

void WebPortal::handleCommand(AsyncWebServerRequest *request) {
  if (submitter_ == nullptr) {
    sendJson(request, 503, commandResponseJson(
      false, false, true, String(), 0, events_.latestId(), "command dispatcher unavailable"
    ));
    return;
  }

  String body;
  if (!takeCommandBody(request, body)) {
    sendJson(request, 400, commandResponseJson(
      false, false, false, String(), 0, events_.latestId(),
      "missing, incomplete, or rejected command body"
    ));
    return;
  }
  body.replace("\r", "");
  body.trim();

  if (body.length() == 0 || body.length() > AppConfig::HTTP_COMMAND_MAX_BYTES) {
    sendJson(request, 400, commandResponseJson(
      false,
      false,
      false,
      String(),
      0,
      events_.latestId(),
      body.length() == 0 ? "empty command form field" : "command body too large"
    ));
    return;
  }

  String suppliedRequestId = request->header("X-Request-ID");
  suppliedRequestId.trim();
  String requestIdError;
  if (!validateRequestId(suppliedRequestId, requestIdError)) {
    sendJson(request, 400, commandResponseJson(
      false, false, false, String(), 0, events_.latestId(), requestIdError
    ));
    return;
  }

  const String requestId = suppliedRequestId;
#if APP_VERBOSE_COEX_HTTP_DIAGNOSTICS
  Serial.printf("[HTTP][COMMAND] id=%s bytes=%u\n", requestId.c_str(), static_cast<unsigned>(body.length()));
  Serial.flush();
#endif

  const uint32_t bodyHash = hashBody(body);
  RequestRecord existing{};
  const RequestDecision decision = inspectOrReserveRequest(requestId, bodyHash, existing);

  if (decision == RequestDecision::CONFLICT) {
    sendJson(request, 409, commandResponseJson(
      false,
      false,
      false,
      requestId,
      0,
      events_.latestId(),
      "request ID was already used for a different command body"
    ));
    return;
  }
  if (decision == RequestDecision::DUPLICATE) {
    sendJson(request, 200, commandResponseJson(
      true,
      true,
      false,
      requestId,
      existing.acceptedLines,
      existing.latestEventId,
      String()
    ));
    return;
  }
  if (decision == RequestDecision::IN_PROGRESS) {
    sendJson(request, 503, commandResponseJson(
      false,
      false,
      true,
      requestId,
      0,
      events_.latestId(),
      "matching request is still being accepted"
    ));
    return;
  }
  if (decision == RequestDecision::HISTORY_BUSY) {
    sendJson(request, 503, commandResponseJson(
      false,
      false,
      true,
      requestId,
      0,
      events_.latestId(),
      "request history is temporarily full"
    ));
    return;
  }

  uint16_t acceptedLines = 0;
  const bool accepted = submitter_(
    submitContext_,
    CommandSource::WIFI,
    body,
    requestId,
    acceptedLines
  );

  if (!accepted) {
    removeReservation(requestId);
    sendJson(request, 503, commandResponseJson(
      false,
      false,
      true,
      requestId,
      acceptedLines,
      events_.latestId(),
      "command queue busy or command rejected"
    ));
    return;
  }

  const uint32_t latestEventId = events_.latestId();
  completeRequest(requestId, acceptedLines, latestEventId);
#if APP_VERBOSE_COEX_HTTP_DIAGNOSTICS
  Serial.printf("[HTTP][COMMAND] accepted id=%s lines=%u event=%u\n", requestId.c_str(), static_cast<unsigned>(acceptedLines), static_cast<unsigned>(latestEventId));
  Serial.flush();
#endif
  sendJson(request, 202, commandResponseJson(
    true,
    false,
    false,
    requestId,
    acceptedLines,
    latestEventId,
    String()
  ));
}

#if APP_HTTP_OTA_ENABLED
void WebPortal::resetOtaState() {
  otaUploadActive_ = false;
  otaUploadSuccess_ = false;
  otaImageValidated_ = false;
  otaRebootPending_ = false;
  otaRebootAtMs_ = 0;
  otaBytesWritten_ = 0;
  otaExpectedBytes_ = 0;
  otaRequest_ = nullptr;
  otaLastError_ = "";
}

void WebPortal::setOtaError(const String &message) {
  if (otaUploadActive_) {
    Update.abort();
  }
  otaLastError_ = message;
  otaUploadSuccess_ = false;
  otaImageValidated_ = false;
  otaUploadActive_ = false;
  otaRebootPending_ = false;
  otaRequest_ = nullptr;
  events_.publish(EventLevel::ERROR, "[OTA] " + message, CommandSource::INTERNAL);
  Serial.println("[OTA][ERROR] " + message);
  Update.printError(Serial);
  Serial.flush();
}

void WebPortal::handleOtaUpload(
  AsyncWebServerRequest *request,
  const String &filename,
  size_t index,
  uint8_t *data,
  size_t length,
  bool final
) {
  if (index == 0) {
    if (otaUploadActive_ && otaRequest_ != request) {
      otaLastError_ = "another OTA upload is already active";
      request->abort();
      return;
    }
    if (
      !isRequestActive(request) &&
      !admitRequest(request, AppConfig::OTA_REQUEST_RESERVATION_BYTES, "ota")
    ) {
      otaUploadSuccess_ = false;
      otaLastError_ = "controller busy before OTA upload";
      return;
    }

    resetOtaState();
    otaRequest_ = request;

    String lowerFilename = filename;
    lowerFilename.toLowerCase();
    if (!lowerFilename.endsWith(".bin")) {
      setOtaError("firmware upload must be an ESP32 application .bin");
      return;
    }

    const String contentType = request->contentType();
    if (!contentType.startsWith("multipart/form-data")) {
      setOtaError("OTA upload must use multipart/form-data");
      return;
    }

    uint32_t suppliedSize = 0;
    if (
      !TextUtil::parseUnsigned32(request->header("X-Firmware-Size"), suppliedSize) ||
      suppliedSize < AppConfig::OTA_MIN_IMAGE_BYTES
    ) {
      setOtaError("missing or invalid X-Firmware-Size header");
      return;
    }

    const size_t availableSketchBytes = ESP.getFreeSketchSpace();
    if (suppliedSize > availableSketchBytes) {
      setOtaError(
        "firmware image is " + String(suppliedSize) +
        " bytes but the inactive app partition has " + String(availableSketchBytes)
      );
      return;
    }

    uint16_t acceptedLines = 0;
    if (submitter_ != nullptr) {
      submitter_(submitContext_, CommandSource::WIFI, "StopAll", "ota-stop", acceptedLines);
    }

    otaExpectedBytes_ = suppliedSize;
    if (!Update.begin(otaExpectedBytes_, U_FLASH)) {
      setOtaError("could not open the inactive OTA app partition");
      return;
    }

    otaUploadActive_ = true;
    events_.publish(
      EventLevel::STATUS,
      "[OTA] multipart upload started file=" + filename +
        " imageBytes=" + String(otaExpectedBytes_),
      CommandSource::INTERNAL
    );
  }

  if (!otaUploadActive_ || otaRequest_ != request) {
    return;
  }

  if (index != otaBytesWritten_) {
    setOtaError(
      "out-of-order OTA chunk at byte " + String(index) +
      ", expected " + String(otaBytesWritten_)
    );
    return;
  }

  if (length > 0) {
    if (!otaImageValidated_) {
      if (index != 0 || data[0] != AppConfig::OTA_IMAGE_MAGIC) {
        setOtaError("uploaded file does not have an ESP32 application image header");
        return;
      }
      otaImageValidated_ = true;
    }

    if (otaBytesWritten_ + length > otaExpectedBytes_) {
      setOtaError("OTA upload exceeded the declared firmware size");
      return;
    }

    const size_t written = Update.write(data, length);
    otaBytesWritten_ += written;
    if (written != length) {
      setOtaError(
        "flash write failed at byte " + String(otaBytesWritten_) +
        " of " + String(otaExpectedBytes_)
      );
      return;
    }
  }

  if (!final) {
    return;
  }

  if (!otaImageValidated_ || otaBytesWritten_ != otaExpectedBytes_) {
    setOtaError(
      "incomplete OTA image: wrote " + String(otaBytesWritten_) +
      " of " + String(otaExpectedBytes_) + " bytes"
    );
    return;
  }

  if (!Update.end(false)) {
    setOtaError("uploaded image validation or finalization failed");
    return;
  }

  otaUploadActive_ = false;
  otaUploadSuccess_ = true;
  events_.publish(
    EventLevel::STATUS,
    "[OTA] image validated bytes=" + String(otaBytesWritten_) +
      "; waiting for HTTP completion response",
    CommandSource::INTERNAL
  );
}

void WebPortal::handleOtaComplete(AsyncWebServerRequest *request) {
  if (!isRequestActive(request) && !admitRequest(request, 512, "ota-complete")) {
    return;
  }

  const bool success = otaRequest_ == request && otaUploadSuccess_ &&
    otaImageValidated_ && otaBytesWritten_ == otaExpectedBytes_ &&
    otaExpectedBytes_ > 0;
  otaRequest_ = nullptr;
  if (success) {
    otaRebootPending_ = true;
    otaRebootAtMs_ = millis() + AppConfig::OTA_REBOOT_DELAY_MS;
  } else if (otaLastError_.length() == 0) {
    otaLastError_ = "no complete multipart firmware image was received";
  }

  String json;
  json.reserve(240);
  json = "{\"ok\":" + TextUtil::jsonBool(success);
  json += ",\"bytes\":" + String(otaBytesWritten_);
  json += ",\"expectedBytes\":" + String(otaExpectedBytes_);
  json += ",\"rebooting\":" + TextUtil::jsonBool(success);
  if (otaLastError_.length() > 0) {
    json += ",\"error\":\"" + TextUtil::jsonEscape(otaLastError_) + "\"";
  }
  json += "}";

  sendJson(request, success ? 200 : 400, std::move(json));
  if (success) {
    events_.publish(
      EventLevel::STATUS,
      "[OTA] completion response queued; reboot scheduled",
      CommandSource::INTERNAL
    );
  }
}
#endif

void WebPortal::handleNotFound(AsyncWebServerRequest *request) {
  logRequest(request, "not-found-fallback");
  if (request->method() == HTTP_OPTIONS && request->url().startsWith("/api/")) {
    sendEmpty(request, 204);
    return;
  }
  if (request->url() == "/standalone_console.html") {
    sendJson(
      request,
      404,
      "{\"ok\":false,\"error\":\"standalone console is intentionally not embedded\"}"
    );
    return;
  }
  if (request->url().startsWith("/api/")) {
    sendJson(request, 404, "{\"ok\":false,\"error\":\"API route not found\"}");
    return;
  }
  sendAsset(
    request,
    WebAssets::PORTAL_HTML_CONTENT_TYPE,
    WebAssets::PORTAL_HTML_GZIP,
    WebAssets::PORTAL_HTML_GZIP_LENGTH,
    WebAssets::PORTAL_HTML_CONTENT_ENCODING
  );
}

void WebPortal::refreshStateCache(bool force) {
  if (stateProvider_ == nullptr || sharedMutex_ == nullptr) {
    return;
  }

  const uint32_t now = millis();
  if (
    !force &&
    static_cast<uint32_t>(now - stateCachedAtMs_) < AppConfig::WEB_STATE_CACHE_INTERVAL_MS
  ) {
    return;
  }

  // Build outside the mutex. This keeps AsyncTCP callbacks from waiting while
  // controller snapshots and JSON strings are assembled on the control task.
  String next = stateProvider_(stateContext_);
#if APP_VERBOSE_COEX_HTTP_DIAGNOSTICS
  if (next.length() > AppConfig::WEB_STATE_JSON_BUDGET_BYTES) {
    Serial.printf(
      "[HTTP][STATE] bytes=%u budget=%u free=%u largest=%u\n",
      static_cast<unsigned>(next.length()),
      static_cast<unsigned>(AppConfig::WEB_STATE_JSON_BUDGET_BYTES),
      static_cast<unsigned>(ESP.getFreeHeap()),
      static_cast<unsigned>(ESP.getMaxAllocHeap())
    );
    Serial.flush();
  }
#endif
  if (xSemaphoreTake(sharedMutex_, pdMS_TO_TICKS(100)) != pdTRUE) {
    return;
  }
  cachedStateJson_ = std::move(next);
  stateCachedAtMs_ = now;
  xSemaphoreGive(sharedMutex_);
}

bool WebPortal::validateRequestId(const String &requestId, String &reason) const {
  if (requestId.length() == 0) {
    reason = "X-Request-ID header is required";
    return false;
  }
  if (requestId.length() > AppConfig::REQUEST_ID_MAX_BYTES) {
    reason = "X-Request-ID exceeds " + String(AppConfig::REQUEST_ID_MAX_BYTES) + " bytes";
    return false;
  }
  for (size_t index = 0; index < requestId.length(); index++) {
    const char value = requestId[index];
    const bool allowed =
      (value >= 'a' && value <= 'z') ||
      (value >= 'A' && value <= 'Z') ||
      (value >= '0' && value <= '9') ||
      value == '-' || value == '_' || value == '.';
    if (!allowed) {
      reason = "X-Request-ID may contain only letters, digits, dash, underscore, and period";
      return false;
    }
  }
  reason = "";
  return true;
}

WebPortal::RequestDecision WebPortal::inspectOrReserveRequest(
  const String &requestId,
  uint32_t bodyHash,
  RequestRecord &snapshot
) {
  if (sharedMutex_ == nullptr || xSemaphoreTake(sharedMutex_, pdMS_TO_TICKS(100)) != pdTRUE) {
    return RequestDecision::HISTORY_BUSY;
  }

  const uint32_t now = millis();
  RequestRecord *existing = findRequestUnlocked(requestId, now);
  if (existing != nullptr) {
    snapshot = *existing;
    const RequestDecision decision = existing->bodyHash != bodyHash
      ? RequestDecision::CONFLICT
      : existing->completed
        ? RequestDecision::DUPLICATE
        : RequestDecision::IN_PROGRESS;
    xSemaphoreGive(sharedMutex_);
    return decision;
  }

  RequestRecord *slot = findWritableRecordUnlocked(now);
  if (slot == nullptr) {
    xSemaphoreGive(sharedMutex_);
    return RequestDecision::HISTORY_BUSY;
  }

  slot->used = true;
  slot->completed = false;
  strncpy(slot->id, requestId.c_str(), sizeof(slot->id) - 1);
  slot->id[sizeof(slot->id) - 1] = '\0';
  slot->acceptedLines = 0;
  slot->bodyHash = bodyHash;
  slot->latestEventId = events_.latestId();
  slot->createdAtMs = now;
  snapshot = *slot;
  xSemaphoreGive(sharedMutex_);
  return RequestDecision::RESERVED;
}

void WebPortal::completeRequest(
  const String &requestId,
  uint16_t acceptedLines,
  uint32_t latestEventId
) {
  if (sharedMutex_ == nullptr || xSemaphoreTake(sharedMutex_, portMAX_DELAY) != pdTRUE) {
    return;
  }
  RequestRecord *record = findRequestUnlocked(requestId, millis());
  if (record != nullptr) {
    record->acceptedLines = acceptedLines;
    record->latestEventId = latestEventId;
    record->completed = true;
  }
  xSemaphoreGive(sharedMutex_);
}

void WebPortal::removeReservation(const String &requestId) {
  if (sharedMutex_ == nullptr || xSemaphoreTake(sharedMutex_, portMAX_DELAY) != pdTRUE) {
    return;
  }
  RequestRecord *record = findRequestUnlocked(requestId, millis());
  if (record != nullptr && !record->completed) {
    record->used = false;
    record->id[0] = '\0';
  }
  xSemaphoreGive(sharedMutex_);
}

WebPortal::RequestRecord *WebPortal::findRequestUnlocked(
  const String &requestId,
  uint32_t now
) {
  for (RequestRecord &record : requestHistory_) {
    expireRecordUnlocked(record, now);
    if (record.used && requestId.equals(record.id)) {
      return &record;
    }
  }
  return nullptr;
}

WebPortal::RequestRecord *WebPortal::findWritableRecordUnlocked(uint32_t now) {
  for (RequestRecord &record : requestHistory_) {
    expireRecordUnlocked(record, now);
    if (!record.used) {
      return &record;
    }
  }

  for (uint8_t offset = 0; offset < REQUEST_HISTORY_SIZE; offset++) {
    const uint8_t index = static_cast<uint8_t>(
      (requestHistoryHead_ + offset) % REQUEST_HISTORY_SIZE
    );
    RequestRecord &record = requestHistory_[index];
    if (record.completed) {
      requestHistoryHead_ = static_cast<uint8_t>((index + 1) % REQUEST_HISTORY_SIZE);
      return &record;
    }
  }
  return nullptr;
}

void WebPortal::expireRecordUnlocked(RequestRecord &record, uint32_t now) {
  if (
    record.used &&
    static_cast<uint32_t>(now - record.createdAtMs) > REQUEST_HISTORY_TTL_MS
  ) {
    record.used = false;
    record.completed = false;
    record.id[0] = '\0';
  }
}

uint32_t WebPortal::hashBody(const String &body) const {
  // FNV-1a is compact and deterministic. It is not an authentication primitive;
  // it only prevents an accidental request-ID collision from executing or hiding
  // a different command batch.
  uint32_t hash = 2166136261UL;
  for (size_t index = 0; index < body.length(); index++) {
    hash ^= static_cast<uint8_t>(body[index]);
    hash *= 16777619UL;
  }
  return hash;
}

String WebPortal::commandResponseJson(
  bool accepted,
  bool duplicate,
  bool retryable,
  const String &requestId,
  uint16_t acceptedLines,
  uint32_t latestEventId,
  const String &error
) const {
  String json = "{";
  json += "\"accepted\":" + TextUtil::jsonBool(accepted);
  json += ",\"duplicate\":" + TextUtil::jsonBool(duplicate);
  json += ",\"retryable\":" + TextUtil::jsonBool(retryable);
  json += ",\"requestId\":\"" + TextUtil::jsonEscape(requestId) + "\"";
  json += ",\"acceptedLines\":" + String(acceptedLines);
  json += ",\"latestEventId\":" + String(latestEventId);
  if (error.length() > 0) {
    json += ",\"error\":\"" + TextUtil::jsonEscape(error) + "\"";
  }
  json += "}";
  return json;
}
#else
WebPortal::WebPortal(EventBus &events) : events_(events) {}

void WebPortal::configure(
  BatchCommandSubmitter submitter,
  void *submitContext,
  StateProvider stateProvider,
  void *stateContext
) {
  (void)submitter;
  (void)submitContext;
  (void)stateProvider;
  (void)stateContext;
}

void WebPortal::prepare() {}
void WebPortal::start() {}
void WebPortal::service() {}
bool WebPortal::isRunning() const { return false; }
uint32_t WebPortal::requestCount() const { return 0; }
uint32_t WebPortal::lastRequestAtMs() const { return 0; }
bool WebPortal::hasRecentClient(uint32_t maxAgeMs) const {
  (void)maxAgeMs;
  return false;
}
void WebPortal::discardClientMetadata() {}
#endif
