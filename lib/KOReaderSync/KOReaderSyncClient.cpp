#include "KOReaderSyncClient.h"

#include <ArduinoJson.h>
#ifdef SIMULATOR
#include <ArduinoJsonStringCompat.h>
#endif
#include <I18n.h>
#include <Logging.h>
#ifdef SIMULATOR
#include <SecureHttpClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#else
#include <SecureHttpClient.h>
#include <base64.h>
#endif

#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>

#include <AppVersion.h>

#include "KOReaderCredentialStore.h"
#include "PendingReadingSessions.h"

#ifndef SIMULATOR
// wolfSSL is built with DEBUG_WOLFSSL, whose Arduino backend expects the app to
// provide this print hook (the stock definition lives in <wolfssl.h>, which no
// translation unit here includes). Route it through the firmware logger, same
// as upstream CrossPoint's HttpDownloader.cpp.
extern "C" void wolfSSL_Arduino_Serial_Print(const char* const msg) { LOG_DBG("WOLFSSL", "%s", msg); }
#endif

int KOReaderSyncClient::lastHttpCode = 0;
int KOReaderSyncClient::lastTransportError = 0;

namespace {
constexpr char DEVICE_ID[] = "crossink-device";

std::string formatHttpStatusMessage(int httpCode) {
  char buffer[96];
  snprintf(buffer, sizeof(buffer), tr(STR_KOREADER_SYNC_HTTP_STATUS_FORMAT), httpCode);
  return std::string(buffer);
}

std::string networkErrorMessage() {
#ifdef SIMULATOR
  switch (KOReaderSyncClient::lastTransportError) {
    case HTTPC_ERROR_CONNECTION_REFUSED:
    case HTTPC_ERROR_NOT_CONNECTED:
    case HTTPC_ERROR_NO_HTTP_SERVER:
      return tr(STR_KOREADER_SYNC_NETWORK_REFUSED);
    case HTTPC_ERROR_CONNECTION_LOST:
    case HTTPC_ERROR_READ_TIMEOUT:
      return tr(STR_KOREADER_SYNC_NETWORK_TIMEOUT);
    default:
      return tr(STR_KOREADER_SYNC_NETWORK_ERROR);
  }
#else
  // SecureHttpClient reports transport failures as a bare -1 (no granular
  // error codes), so there is nothing finer to translate on device.
  return tr(STR_KOREADER_SYNC_NETWORK_ERROR);
#endif
}

const char* classifyJsonBody(const char* body) {
  if (!body || body[0] == '\0') return "empty response";

  const char* cursor = body;
  while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') {
    cursor++;
  }

  if (*cursor == '\0') return "blank response";
  if (*cursor == '<') return "HTML response";
  if (*cursor != '{' && *cursor != '[') return "non-JSON response";
  return "malformed JSON";
}

void logJsonParseFailure(const char* context, DeserializationError error, const char* body) {
  char preview[97];
  size_t i = 0;
  if (body) {
    for (; i < sizeof(preview) - 1 && body[i] != '\0'; i++) {
      const char c = body[i];
      preview[i] = (c == '\r' || c == '\n' || c == '\t') ? ' ' : c;
    }
  }
  preview[i] = '\0';

  LOG_ERR("KOSync", "%s JSON parse failed: %s (%s, preview=\"%s\")", context, error.c_str(), classifyJsonBody(body),
          preview);
}

KOReaderSyncClient::Error validateAuthResponse(const char* body) {
  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, body ? body : "");
  if (error) {
    logJsonParseFailure("Auth", error, body);
    return KOReaderSyncClient::JSON_ERROR;
  }

  if (!doc.is<JsonObject>()) {
    LOG_ERR("KOSync", "Auth response was not a JSON object");
    return KOReaderSyncClient::INVALID_AUTH_RESPONSE;
  }

  const char* authorized = doc["authorized"] | nullptr;
  if (authorized && std::strcmp(authorized, "OK") != 0) {
    LOG_ERR("KOSync", "Auth response explicitly denied authorization");
    return KOReaderSyncClient::INVALID_AUTH_RESPONSE;
  }

  return KOReaderSyncClient::OK;
}

// KOSync's TLS-1.3 servers can't be reached through the precompiled system
// mbedTLS (TLS 1.3 is stubbed out), so requests run over wolfSSL via
// SecureHttpClient. The handshake still needs working heap; gate on it. wolfSSL's
// footprint is smaller than mbedTLS's old ~48KB peak, but keep conservative
// floors for total free heap and the largest contiguous block.
constexpr uint32_t MIN_FREE_HEAP_FOR_TLS = 35000;
constexpr uint32_t MIN_MAX_ALLOC_HEAP_FOR_TLS = 20000;

#ifdef SIMULATOR
void addAuthHeaders(HTTPClient& http) {
  http.addHeader("Accept", "application/vnd.koreader.v1+json");
  http.addHeader("x-auth-user", KOREADER_STORE.getUsername().c_str());
  http.addHeader("x-auth-key", KOREADER_STORE.getMd5Password().c_str());
  http.setAuthorization(KOREADER_STORE.getUsername().c_str(), KOREADER_STORE.getPassword().c_str());
}

bool isHttpsUrl(const std::string& url) { return url.rfind("https://", 0) == 0; }
#else
// Apply the shared KOSync auth headers after begin(). x-auth-* is the native
// KOSync scheme; Basic auth is added for Calibre-Web-Automated compatibility.
void applyAuthHeaders(freeink::SecureHttpClient& http) {
  http.addHeader("Accept", "application/vnd.koreader.v1+json");
  http.addHeader("x-auth-user", KOREADER_STORE.getUsername());
  http.addHeader("x-auth-key", KOREADER_STORE.getMd5Password());
  const std::string credentials = KOREADER_STORE.getUsername() + ":" + KOREADER_STORE.getPassword();
  const String encoded = base64::encode(credentials.c_str());
  http.addHeader("Authorization", std::string("Basic ") + encoded.c_str());
}
#endif

// True when free heap is too low to risk a TLS handshake.
bool insufficientHeap() {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAllocHeap = ESP.getMaxAllocHeap();
  if (freeHeap < MIN_FREE_HEAP_FOR_TLS || maxAllocHeap < MIN_MAX_ALLOC_HEAP_FOR_TLS) {
    LOG_ERR("KOSync", "Insufficient heap for TLS handshake: %u bytes free (need %u), %u max alloc (need %u)", freeHeap,
            MIN_FREE_HEAP_FOR_TLS, maxAllocHeap, MIN_MAX_ALLOC_HEAP_FOR_TLS);
    return true;
  }
  return false;
}
}  // namespace

KOReaderSyncClient::Error KOReaderSyncClient::authenticate() {
  lastHttpCode = 0;
  lastTransportError = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  const std::string url = KOREADER_STORE.getBaseUrl() + "/users/auth";
  LOG_DBG("KOSync", "Authenticating: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

#ifdef SIMULATOR
  HTTPClient http;
  std::unique_ptr<WiFiClientSecure> secureClient;
  WiFiClient plainClient;

  if (isHttpsUrl(url)) {
    secureClient.reset(new WiFiClientSecure);
    secureClient->setInsecure();
    http.begin(*secureClient, url.c_str());
  } else {
    http.begin(plainClient, url.c_str());
  }
  addAuthHeaders(http);

  const int httpCode = http.GET();
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;

  LOG_DBG("KOSync", "Auth response: %d", httpCode);

  if (httpCode == 200) {
    String responseBody = http.getString();
    http.end();
    return validateAuthResponse(responseBody.c_str());
  }

  http.end();

  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode < 0) return NETWORK_ERROR;
  return SERVER_ERROR;
#else
  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR("KOSync", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  applyAuthHeaders(http);
  const int httpCode = http.GET();
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;

  LOG_DBG("KOSync", "Auth response: %d", httpCode);

  if (httpCode <= 0) {
    http.end();
    return NETWORK_ERROR;
  }
  if (httpCode == 200) {
    const Error result = validateAuthResponse(http.getString().c_str());
    http.end();
    return result;
  }
  http.end();
  if (httpCode == 401) return AUTH_FAILED;
  return SERVER_ERROR;
#endif
}

KOReaderSyncClient::Error KOReaderSyncClient::createUser() {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  const std::string url = KOREADER_STORE.getBaseUrl() + "/users/create";
  LOG_DBG("KOSync", "Creating account: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

  JsonDocument doc;
  doc["username"] = KOREADER_STORE.getUsername();
  doc["password"] = KOREADER_STORE.getMd5Password();
  std::string body;
  serializeJson(doc, body);

  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR("KOSync", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  http.addHeader("Accept", "application/vnd.koreader.v1+json");
  http.addHeader("Content-Type", "application/json");
  const int httpCode = http.sendRequest("POST", body);
  http.end();
  lastHttpCode = httpCode;

  LOG_DBG("KOSync", "Create user response: %d", httpCode);

  if (httpCode <= 0) return NETWORK_ERROR;
  if (httpCode == 200 || httpCode == 201) return OK;
  if (httpCode == 402) return USER_EXISTS;
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::getProgress(const std::string& documentHash,
                                                          KOReaderProgress& outProgress) {
  lastHttpCode = 0;
  lastTransportError = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  const std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/progress/" + documentHash;
  LOG_DBG("KOSync", "Getting progress: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

#ifdef SIMULATOR
  HTTPClient http;
  std::unique_ptr<WiFiClientSecure> secureClient;
  WiFiClient plainClient;

  if (isHttpsUrl(url)) {
    secureClient.reset(new WiFiClientSecure);
    secureClient->setInsecure();
    http.begin(*secureClient, url.c_str());
  } else {
    http.begin(plainClient, url.c_str());
  }
  addAuthHeaders(http);

  const int httpCode = http.GET();
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;

  if (httpCode == 200) {
    String responseBody = http.getString();
    http.end();

    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, responseBody);

    if (error) {
      logJsonParseFailure("Get progress", error, responseBody.c_str());
      return JSON_ERROR;
    }

    outProgress.document = documentHash;
    outProgress.progress = doc["progress"].as<std::string>();
    outProgress.percentage = doc["percentage"].as<float>();
    outProgress.device = doc["device"].as<std::string>();
    outProgress.deviceId = doc["device_id"].as<std::string>();
    outProgress.timestamp = doc["timestamp"].as<int64_t>();

    LOG_DBG("KOSync", "Got progress: %.2f%% at %s", outProgress.percentage * 100, outProgress.progress.c_str());
    return OK;
  }

  http.end();
  LOG_DBG("KOSync", "Get progress response: %d", httpCode);

  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode == 404) return NOT_FOUND;
  if (httpCode < 0) return NETWORK_ERROR;
  return SERVER_ERROR;
#else
  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR("KOSync", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  applyAuthHeaders(http);
  const int httpCode = http.GET();
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;

  LOG_DBG("KOSync", "Get progress response: %d", httpCode);

  if (httpCode <= 0) {
    http.end();
    return NETWORK_ERROR;
  }

  if (httpCode == 200) {
    const std::string& body = http.getString();
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, body);

    if (error) {
      logJsonParseFailure("Get progress", error, body.c_str());
      http.end();
      return JSON_ERROR;
    }

    outProgress.document = documentHash;
    outProgress.progress = doc["progress"].as<std::string>();
    outProgress.percentage = doc["percentage"].as<float>();
    outProgress.device = doc["device"].as<std::string>();
    outProgress.deviceId = doc["device_id"].as<std::string>();
    outProgress.timestamp = doc["timestamp"].as<int64_t>();

    outProgress.position.reset();
    if (KOREADER_STORE.usesCrossPointSyncServer()) {
      const JsonObjectConst pos = doc["position"].as<JsonObjectConst>();
      if (!pos.isNull()) {
        KOReaderRichPosition rich;
        rich.pctQ = pos["pctQ"].as<uint32_t>();
        rich.spineIndex = pos["spine"].as<uint16_t>();
        rich.pageNumber = pos["page"].as<uint16_t>();
        const uint16_t pages = pos["pages"].as<uint16_t>();
        rich.totalPages = pages > 0 ? pages : 1;
        const uint16_t para = pos["para"].as<uint16_t>();
        if (para > 0) rich.paragraphIndex = para;
        rich.xpath = pos["xpath"].as<const char*>() ? pos["xpath"].as<const char*>() : "";
        LOG_DBG("KOSync", "Got rich position: spine=%u page=%u/%u para=%u", rich.spineIndex, rich.pageNumber,
                rich.totalPages, para);
        outProgress.position = std::move(rich);
      }
    }

    LOG_DBG("KOSync", "Got progress: %.2f%% at %s", outProgress.percentage * 100, outProgress.progress.c_str());
    return OK;
  }

  http.end();
  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode == 404) return NOT_FOUND;
  return SERVER_ERROR;
#endif
}

KOReaderSyncClient::Error KOReaderSyncClient::updateProgress(const KOReaderProgress& progress) {
  lastHttpCode = 0;
  lastTransportError = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  const std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/progress";
  LOG_DBG("KOSync", "Updating progress: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

  // Build JSON body
  JsonDocument doc;
  doc["document"] = progress.document;
  if (progress.metadata.has_value()) {
    auto meta = doc["metadata"].to<JsonObject>();
    meta["filename"] = progress.metadata->filename;
    meta["title"] = progress.metadata->title;
    meta["authors"] = progress.metadata->authors;
  }
  doc["progress"] = progress.progress;
  doc["percentage"] = progress.percentage;
  doc["device"] = progress.device;
  doc["device_id"] = DEVICE_ID;
  if (progress.position.has_value() && KOREADER_STORE.usesCrossPointSyncServer()) {
    // CrossPoint-specific extension: do not send it to third-party KOSync servers.
    const auto& p = *progress.position;
    auto pos = doc["position"].to<JsonObject>();
    pos["pctQ"] = p.pctQ;
    pos["spine"] = p.spineIndex;
    pos["page"] = p.pageNumber;
    pos["pages"] = p.totalPages;
    if (p.paragraphIndex.has_value()) pos["para"] = *p.paragraphIndex;
    // Server rejects the whole position object if xpath exceeds 120 bytes.
    if (!p.xpath.empty() && p.xpath.size() <= 120) pos["xpath"] = p.xpath;
  }

  std::string body;
  serializeJson(doc, body);

#ifdef SIMULATOR
  HTTPClient http;
  std::unique_ptr<WiFiClientSecure> secureClient;
  WiFiClient plainClient;

  if (isHttpsUrl(url)) {
    secureClient.reset(new WiFiClientSecure);
    secureClient->setInsecure();
    http.begin(*secureClient, url.c_str());
  } else {
    http.begin(plainClient, url.c_str());
  }
  addAuthHeaders(http);
  http.addHeader("Content-Type", "application/json");

  const int httpCode = http.PUT(body.c_str());
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;
  http.end();

  LOG_DBG("KOSync", "Update progress response: %d", httpCode);

  if (httpCode == 200 || httpCode == 202) return OK;
  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode < 0) return NETWORK_ERROR;
  return SERVER_ERROR;
#else
  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR("KOSync", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  applyAuthHeaders(http);
  http.addHeader("Content-Type", "application/json");
  const int httpCode = http.sendRequest("PUT", body);
  http.end();
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;

  LOG_DBG("KOSync", "Update progress response: %d", httpCode);

  if (httpCode <= 0) return NETWORK_ERROR;
  if (httpCode == 200 || httpCode == 202) return OK;
  if (httpCode == 401) return AUTH_FAILED;
  return SERVER_ERROR;
#endif
}

namespace {
constexpr int PAGE_STATS_CHUNK_SECONDS = 1500;         // Split a session into events no longer than this
constexpr int PAGE_STATS_MAX_EVENTS_PER_SESSION = 30;  // Cap events per session (marathon-read guard)
constexpr size_t PAGE_STATS_MAX_EVENTS = 500;          // Server accepts at most 500 events per request
constexpr size_t PAGE_STATS_MAX_BOOKS = 50;            // Server accepts at most 50 books per request

std::string clampStr(const std::string& value, size_t maxLen) {
  return value.size() <= maxLen ? value : value.substr(0, maxLen);
}

// Builds the page-stats JSON request body from buffered sessions. Sessions are
// grouped by document hash and each is expanded into contiguous events (so the
// server clusters them into one session). Sessions captured without an RTC
// (startEpoch == 0) are dated ending at nowEpoch. Returns the number of events
// written; 0 means there is nothing worth sending.
size_t buildPageStatsBody(const std::vector<PendingReadingSession>& sessions, const std::string& deviceModel,
                          int64_t nowEpoch, std::string& outBody) {
  JsonDocument doc;
  doc["deviceId"] = DEVICE_ID;
  doc["deviceModel"] = clampStr(deviceModel.empty() ? std::string("CrossOrbit") : deviceModel, 100);
  doc["pluginVersion"] = clampStr(std::string(CROSSINK_VERSION), 20);
  JsonArray books = doc["books"].to<JsonArray>();

  std::vector<std::string> hashes;
  std::vector<int64_t> floorEpoch;   // next event for this hash must start after this
  std::vector<JsonArray> eventArrays;
  size_t totalEvents = 0;

  for (const auto& s : sessions) {
    if (s.docHash.empty() || s.durationSeconds == 0) continue;
    if (totalEvents >= PAGE_STATS_MAX_EVENTS) break;

    int bookIndex = -1;
    for (size_t j = 0; j < hashes.size(); j++) {
      if (hashes[j] == s.docHash) {
        bookIndex = static_cast<int>(j);
        break;
      }
    }
    if (bookIndex < 0) {
      if (hashes.size() >= PAGE_STATS_MAX_BOOKS) continue;
      JsonObject bookObj = books.add<JsonObject>();
      bookObj["hash"] = s.docHash;
      hashes.push_back(s.docHash);
      floorEpoch.push_back(0);
      eventArrays.push_back(bookObj["events"].to<JsonArray>());
      bookIndex = static_cast<int>(hashes.size()) - 1;
    }

    int64_t start = s.startEpoch > 0 ? s.startEpoch : (nowEpoch - static_cast<int64_t>(s.durationSeconds));
    if (start < 1) start = 1;
    // Keep events for the same book strictly ordered; overlapping/synthetic
    // sessions are pushed just past the previous one so they cluster cleanly.
    if (start <= floorEpoch[bookIndex]) start = floorEpoch[bookIndex] + 1;

    const uint32_t duration = s.durationSeconds;
    int chunks = static_cast<int>((duration + PAGE_STATS_CHUNK_SECONDS - 1) / PAGE_STATS_CHUNK_SECONDS);
    if (chunks < 1) chunks = 1;
    if (chunks > PAGE_STATS_MAX_EVENTS_PER_SESSION) chunks = PAGE_STATS_MAX_EVENTS_PER_SESSION;
    const uint32_t baseDuration = duration / static_cast<uint32_t>(chunks);
    const uint32_t remainder = duration % static_cast<uint32_t>(chunks);

    const int32_t pageSpan = static_cast<int32_t>(s.endPage) - static_cast<int32_t>(s.startPage);
    int64_t eventStart = start;
    uint32_t cumulative = 0;
    JsonArray& events = eventArrays[bookIndex];
    for (int k = 0; k < chunks; k++) {
      if (totalEvents >= PAGE_STATS_MAX_EVENTS) break;
      const uint32_t chunkDuration = baseDuration + (static_cast<uint32_t>(k) < remainder ? 1u : 0u);
      cumulative += chunkDuration;

      int32_t page = static_cast<int32_t>(s.startPage);
      if (duration > 0) {
        page += static_cast<int32_t>((static_cast<int64_t>(pageSpan) * cumulative) / duration);
      }
      if (page < 0) page = 0;
      if (page > s.totalPages) page = s.totalPages;

      JsonObject event = events.add<JsonObject>();
      event["page"] = page;
      event["startTime"] = eventStart;
      event["durationSeconds"] = chunkDuration;
      event["totalPages"] = s.totalPages == 0 ? 1 : s.totalPages;

      eventStart += chunkDuration;
      totalEvents++;
    }
    floorEpoch[bookIndex] = eventStart;
  }

  if (totalEvents == 0) return 0;
  serializeJson(doc, outBody);
  return totalEvents;
}
}  // namespace

KOReaderSyncClient::Error KOReaderSyncClient::uploadPageStats(const std::vector<PendingReadingSession>& sessions,
                                                              const std::string& deviceModel, int64_t nowEpoch) {
  lastHttpCode = 0;
  lastTransportError = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }
  if (sessions.empty()) return OK;

  std::string body;
  const size_t eventCount = buildPageStatsBody(sessions, deviceModel, nowEpoch, body);
  if (eventCount == 0) return OK;

  const std::string url = KOREADER_STORE.getBaseUrl() + "/plugin/page-stats";
  LOG_DBG("KOSync", "Uploading page stats: %s events=%u (heap: %u)", url.c_str(), (unsigned)eventCount,
          (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

#ifdef SIMULATOR
  HTTPClient http;
  std::unique_ptr<WiFiClientSecure> secureClient;
  WiFiClient plainClient;

  if (isHttpsUrl(url)) {
    secureClient.reset(new WiFiClientSecure);
    secureClient->setInsecure();
    http.begin(*secureClient, url.c_str());
  } else {
    http.begin(plainClient, url.c_str());
  }
  addAuthHeaders(http);
  http.addHeader("Content-Type", "application/json");

  const int httpCode = http.POST(body.c_str());
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;
  http.end();

  LOG_DBG("KOSync", "Page stats response: %d", httpCode);

  if (httpCode == 200 || httpCode == 202) return OK;
  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode < 0) return NETWORK_ERROR;
  return SERVER_ERROR;
#else
  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR("KOSync", "Bad URL: %s", url.c_str());
    return NETWORK_ERROR;
  }
  applyAuthHeaders(http);
  http.addHeader("Content-Type", "application/json");
  const int httpCode = http.sendRequest("POST", body);
  http.end();
  lastHttpCode = httpCode;
  lastTransportError = (httpCode < 0) ? httpCode : 0;

  LOG_DBG("KOSync", "Page stats response: %d", httpCode);

  if (httpCode <= 0) return NETWORK_ERROR;
  if (httpCode == 200 || httpCode == 202) return OK;
  if (httpCode == 401) return AUTH_FAILED;
  return SERVER_ERROR;
#endif
}

std::string KOReaderSyncClient::errorString(Error error) {
  switch (error) {
    case OK:
      return "Success";
    case NO_CREDENTIALS:
      return tr(STR_NO_CREDENTIALS_MSG);
    case NETWORK_ERROR:
      return networkErrorMessage();
    case AUTH_FAILED:
      return tr(STR_KOREADER_SYNC_AUTH_REJECTED);
    case SERVER_ERROR:
      if (lastHttpCode == 404) return tr(STR_KOREADER_SYNC_HTTP_404);
      if (lastHttpCode > 0) return formatHttpStatusMessage(lastHttpCode);
      return tr(STR_KOREADER_SYNC_SERVER_ERROR);
    case JSON_ERROR:
      return tr(STR_KOREADER_SYNC_BAD_RESPONSE);
    case NOT_FOUND:
      return tr(STR_NO_REMOTE_MSG);
    case INVALID_AUTH_RESPONSE:
      return tr(STR_KOREADER_SYNC_BAD_RESPONSE);
    case LOW_MEMORY:
      return tr(STR_KOREADER_SYNC_LOW_MEMORY);
    default:
      return tr(STR_UNKNOWN_ERROR);
  }
}
