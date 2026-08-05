#include "BookOrbitClient.h"

#include <AppVersion.h>
#include <ArduinoJson.h>
#include <Logging.h>
#ifdef SIMULATOR
#include <ArduinoJsonStringCompat.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#else
#include <SecureHttpClient.h>
#include <base64.h>
#endif

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "BookOrbitConfig.h"
#include "PendingBookmarks.h"
#include "PendingReadingSessions.h"

int BookOrbitClient::lastHttpCode = 0;

namespace {
constexpr char DEVICE_ID[] = "crossorbit-device";
constexpr char DEVICE_MODEL[] = "CrossOrbit";

// wolfSSL's debug print hook is provided once by the upstream KOReaderSyncClient.cpp, which is
// always linked; do not redefine it here (that would be a duplicate symbol).

// TLS handshakes need working heap; gate on total free and the largest contiguous block.
constexpr uint32_t MIN_FREE_HEAP_FOR_TLS = 35000;
constexpr uint32_t MIN_MAX_ALLOC_HEAP_FOR_TLS = 20000;

bool insufficientHeap() {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAllocHeap = ESP.getMaxAllocHeap();
  if (freeHeap < MIN_FREE_HEAP_FOR_TLS || maxAllocHeap < MIN_MAX_ALLOC_HEAP_FOR_TLS) {
    LOG_ERR("BOClient", "Insufficient heap for TLS: free=%u maxAlloc=%u", freeHeap, maxAllocHeap);
    return true;
  }
  return false;
}

std::string clampStr(const std::string& value, size_t maxLen) {
  return value.size() <= maxLen ? value : value.substr(0, maxLen);
}

void addDeviceFields(JsonDocument& doc, int64_t nowEpoch) {
  doc["deviceId"] = DEVICE_ID;
  doc["deviceModel"] = DEVICE_MODEL;
  doc["pluginVersion"] = clampStr(std::string(CROSSINK_VERSION), 20);
  char deviceTime[24];
  koReaderEpochToDatetime(nowEpoch, deviceTime, sizeof(deviceTime));
  doc["deviceTime"] = deviceTime;
}

#ifdef SIMULATOR
bool isHttpsUrl(const std::string& url) { return url.rfind("https://", 0) == 0; }

void addAuthHeaders(HTTPClient& http) {
  http.addHeader("Accept", "application/vnd.koreader.v1+json");
  http.addHeader("x-auth-user", BOOKORBIT.getUsername().c_str());
  http.addHeader("x-auth-key", BOOKORBIT.getMd5Password().c_str());
  http.setAuthorization(BOOKORBIT.getUsername().c_str(), BOOKORBIT.getPassword().c_str());
}

// Perform an HTTP request in the simulator. Returns the HTTP status (<0 on transport error).
int simRequest(const std::string& url, const char* method, const std::string* body, std::string* outBody) {
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
  if (body) http.addHeader("Content-Type", "application/json");

  int code;
  if (strcmp(method, "GET") == 0) {
    code = http.GET();
  } else if (strcmp(method, "PUT") == 0) {
    code = http.PUT(body ? body->c_str() : "");
  } else {
    code = http.POST(body ? body->c_str() : "");
  }
  if (outBody && code >= 200 && code < 300) *outBody = http.getString().c_str();
  http.end();
  return code;
}
#else
void applyAuthHeaders(freeink::SecureHttpClient& http) {
  http.addHeader("Accept", "application/vnd.koreader.v1+json");
  http.addHeader("x-auth-user", BOOKORBIT.getUsername());
  http.addHeader("x-auth-key", BOOKORBIT.getMd5Password());
  const std::string credentials = BOOKORBIT.getUsername() + ":" + BOOKORBIT.getPassword();
  const String encoded = base64::encode(credentials.c_str());
  http.addHeader("Authorization", std::string("Basic ") + encoded.c_str());
}

// Perform an HTTP request on device. Returns the HTTP status (<=0 on transport error).
int deviceRequest(const std::string& url, const char* method, const std::string* body, std::string* outBody) {
  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR("BOClient", "Bad URL: %s", url.c_str());
    return -1;
  }
  applyAuthHeaders(http);
  if (body) http.addHeader("Content-Type", "application/json");

  const int code = (strcmp(method, "GET") == 0) ? http.GET() : http.sendRequest(method, *body);
  if (outBody && code >= 200 && code < 300) *outBody = http.getString();
  http.end();
  return code;
}
#endif

// Single entry point for all requests. url is the full endpoint; body is null for GET.
int performRequest(const std::string& url, const char* method, const std::string* body, std::string* outBody) {
#ifdef SIMULATOR
  return simRequest(url, method, body, outBody);
#else
  return deviceRequest(url, method, body, outBody);
#endif
}

BookOrbitClient::Error mapStatus(int httpCode) {
  BookOrbitClient::lastHttpCode = httpCode;
  if (httpCode <= 0) return BookOrbitClient::NETWORK_ERROR;
  // POST routes return 201 Created (NestJS default), PUT/GET return 200; accept any 2xx.
  if (httpCode >= 200 && httpCode < 300) return BookOrbitClient::OK;
  if (httpCode == 401) return BookOrbitClient::AUTH_FAILED;
  if (httpCode == 404) return BookOrbitClient::NOT_FOUND;
  return BookOrbitClient::SERVER_ERROR;
}

// ---- payload builders (see BookOrbit KOReader plugin DTOs) ----

constexpr int PAGE_STATS_CHUNK_SECONDS = 1500;
constexpr int PAGE_STATS_MAX_EVENTS_PER_SESSION = 30;
constexpr size_t PAGE_STATS_MAX_EVENTS = 500;
constexpr size_t GROUP_MAX_BOOKS = 50;
constexpr size_t ANNOTATIONS_MAX_TOTAL = 100;
constexpr size_t BOOKMARKS_MAX_PER_BOOK = 50;    // server BOOKMARK_EXCHANGE_MAX_CHANGES
constexpr size_t BOOKMARK_EXCHANGE_MAX_KEYS = 500;  // server BOOKMARK_EXCHANGE_MAX_KEYS

int findOrAddBook(JsonArray& books, std::vector<std::string>& hashes, std::vector<JsonArray>& childArrays,
                  const std::string& hash, const char* childKey, size_t maxBooks, bool bookmarkShape) {
  for (size_t j = 0; j < hashes.size(); j++) {
    if (hashes[j] == hash) return static_cast<int>(j);
  }
  if (hashes.size() >= maxBooks) return -1;
  JsonObject bookObj = books.add<JsonObject>();
  bookObj["hash"] = hash;
  if (bookmarkShape) {
    bookObj["keysComplete"] = false;  // one-way push: never delete server-side bookmarks
    bookObj["keys"].to<JsonArray>();
  }
  hashes.push_back(hash);
  childArrays.push_back(bookObj[childKey].to<JsonArray>());
  return static_cast<int>(hashes.size()) - 1;
}

size_t buildPageStatsBody(const std::vector<PendingReadingSession>& sessions, int64_t nowEpoch, std::string& outBody) {
  JsonDocument doc;
  addDeviceFields(doc, nowEpoch);
  JsonArray books = doc["books"].to<JsonArray>();
  std::vector<std::string> hashes;
  std::vector<int64_t> floorEpoch;
  std::vector<JsonArray> eventArrays;
  size_t totalEvents = 0;

  for (const auto& s : sessions) {
    if (s.docHash.empty() || s.durationSeconds == 0 || totalEvents >= PAGE_STATS_MAX_EVENTS) continue;
    const int bookIndex = findOrAddBook(books, hashes, eventArrays, s.docHash, "events", GROUP_MAX_BOOKS, false);
    if (bookIndex < 0) continue;
    if (static_cast<size_t>(bookIndex) >= floorEpoch.size()) floorEpoch.resize(hashes.size(), 0);

    int64_t start = s.startEpoch > 0 ? s.startEpoch : (nowEpoch - static_cast<int64_t>(s.durationSeconds));
    if (start < 1) start = 1;
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
    for (int k = 0; k < chunks && totalEvents < PAGE_STATS_MAX_EVENTS; k++) {
      const uint32_t chunkDuration = baseDuration + (static_cast<uint32_t>(k) < remainder ? 1u : 0u);
      cumulative += chunkDuration;
      int32_t page = static_cast<int32_t>(s.startPage);
      if (duration > 0) page += static_cast<int32_t>((static_cast<int64_t>(pageSpan) * cumulative) / duration);
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

size_t buildAnnotationsBody(const std::vector<BookOrbitAnnotation>& highlights, int64_t nowEpoch,
                            std::string& outBody) {
  JsonDocument doc;
  addDeviceFields(doc, nowEpoch);
  JsonArray books = doc["books"].to<JsonArray>();
  std::vector<std::string> hashes;
  std::vector<JsonArray> annotationArrays;
  size_t total = 0;

  for (const auto& h : highlights) {
    if (h.docHash.empty() || h.pos0.empty() || h.pos1.empty() || total >= ANNOTATIONS_MAX_TOTAL) continue;
    const int bookIndex =
        findOrAddBook(books, hashes, annotationArrays, h.docHash, "annotations", GROUP_MAX_BOOKS, false);
    if (bookIndex < 0) continue;

    char datetime[24];
    koReaderEpochToDatetime(h.startEpoch > 0 ? h.startEpoch : nowEpoch, datetime, sizeof(datetime));
    JsonObject annotation = annotationArrays[bookIndex].add<JsonObject>();
    annotation["datetime"] = datetime;
    annotation["drawer"] = "lighten";
    annotation["posFormat"] = "xpointer";
    annotation["pos0"] = h.pos0;
    annotation["pos1"] = h.pos1;
    if (!h.text.empty()) annotation["text"] = h.text;
    if (!h.chapter.empty()) annotation["chapter"] = h.chapter;
    total++;
  }

  if (total == 0) return 0;
  serializeJson(doc, outBody);
  return total;
}

size_t buildBookmarksBody(const std::vector<PendingBookmark>& bookmarks, int64_t nowEpoch, std::string& outBody) {
  JsonDocument doc;
  addDeviceFields(doc, nowEpoch);
  JsonArray books = doc["books"].to<JsonArray>();
  std::vector<std::string> hashes;
  std::vector<JsonArray> changeArrays;
  size_t total = 0;

  for (const auto& b : bookmarks) {
    if (b.docHash.empty() || b.pos.empty()) continue;
    const int bookIndex = findOrAddBook(books, hashes, changeArrays, b.docHash, "changes", GROUP_MAX_BOOKS, true);
    if (bookIndex < 0) continue;
    if (changeArrays[bookIndex].size() >= BOOKMARKS_MAX_PER_BOOK) continue;

    char datetime[24];
    koReaderEpochToDatetime(b.startEpoch > 0 ? b.startEpoch : nowEpoch, datetime, sizeof(datetime));
    JsonObject change = changeArrays[bookIndex].add<JsonObject>();
    change["datetime"] = datetime;
    change["pos"] = b.pos;
    if (!b.chapter.empty()) change["chapter"] = b.chapter;
    if (!b.note.empty()) change["note"] = b.note;
    total++;
  }

  if (total == 0) return 0;
  serializeJson(doc, outBody);
  return total;
}
}  // namespace

BookOrbitClient::Error BookOrbitClient::authenticate() {
  lastHttpCode = 0;
  if (!BOOKORBIT.isConfigured()) return NOT_CONFIGURED;
  if (insufficientHeap()) return LOW_MEMORY;

  std::string responseBody;
  const std::string url = BOOKORBIT.getKoreaderBaseUrl() + "/users/auth";
  const int code = performRequest(url, "GET", nullptr, &responseBody);
  LOG_DBG("BOClient", "Auth response: %d", code);
  return mapStatus(code);
}

BookOrbitClient::Error BookOrbitClient::getProgress(const std::string& documentHash, BookOrbitProgress& out) {
  lastHttpCode = 0;
  if (!BOOKORBIT.isConfigured()) return NOT_CONFIGURED;
  if (insufficientHeap()) return LOW_MEMORY;

  std::string responseBody;
  const std::string url = BOOKORBIT.getKoreaderBaseUrl() + "/syncs/progress/" + documentHash;
  const int code = performRequest(url, "GET", nullptr, &responseBody);
  const Error status = mapStatus(code);
  if (status != OK) return status;

  JsonDocument doc;
  if (deserializeJson(doc, responseBody)) {
    LOG_ERR("BOClient", "Get progress JSON parse failed");
    return JSON_ERROR;
  }
  // BookOrbit returns {} when it has no progress for this document.
  const char* progress = doc["progress"] | "";
  if (progress[0] == '\0' && !doc["percentage"].is<float>()) return NOT_FOUND;

  out.document = documentHash;
  out.progress = progress;
  out.percentage = doc["percentage"] | 0.0f;
  out.device = doc["device"] | "";
  out.timestamp = doc["timestamp"] | static_cast<int64_t>(0);
  return OK;
}

BookOrbitClient::Error BookOrbitClient::updateProgress(const BookOrbitProgress& progress) {
  lastHttpCode = 0;
  if (!BOOKORBIT.isConfigured()) return NOT_CONFIGURED;
  if (insufficientHeap()) return LOW_MEMORY;

  JsonDocument doc;
  doc["document"] = progress.document;
  doc["progress"] = progress.progress;
  doc["percentage"] = progress.percentage;
  doc["device"] = progress.device.empty() ? std::string(DEVICE_MODEL) : progress.device;
  doc["device_id"] = DEVICE_ID;
  std::string body;
  serializeJson(doc, body);

  const std::string url = BOOKORBIT.getKoreaderBaseUrl() + "/syncs/progress";
  const int code = performRequest(url, "PUT", &body, nullptr);
  LOG_DBG("BOClient", "Update progress response: %d", code);
  return mapStatus(code);
}

BookOrbitClient::Error BookOrbitClient::uploadPageStats(const std::vector<PendingReadingSession>& sessions,
                                                        int64_t nowEpoch) {
  lastHttpCode = 0;
  if (!BOOKORBIT.isConfigured()) return NOT_CONFIGURED;
  if (sessions.empty()) return OK;

  std::string body;
  if (buildPageStatsBody(sessions, nowEpoch, body) == 0) return OK;
  if (insufficientHeap()) return LOW_MEMORY;

  const std::string url = BOOKORBIT.getKoreaderBaseUrl() + "/plugin/page-stats";
  const int code = performRequest(url, "POST", &body, nullptr);
  LOG_DBG("BOClient", "Page stats response: %d", code);
  return mapStatus(code);
}

BookOrbitClient::Error BookOrbitClient::uploadAnnotations(const std::vector<BookOrbitAnnotation>& annotations,
                                                          int64_t nowEpoch) {
  lastHttpCode = 0;
  if (!BOOKORBIT.isConfigured()) return NOT_CONFIGURED;
  if (annotations.empty()) return OK;

  std::string body;
  if (buildAnnotationsBody(annotations, nowEpoch, body) == 0) return OK;
  if (insufficientHeap()) return LOW_MEMORY;

  const std::string url = BOOKORBIT.getKoreaderBaseUrl() + "/plugin/annotations";
  const int code = performRequest(url, "POST", &body, nullptr);
  LOG_DBG("BOClient", "Annotations response: %d", code);
  return mapStatus(code);
}

BookOrbitClient::Error BookOrbitClient::uploadBookmarks(const std::vector<PendingBookmark>& bookmarks,
                                                        int64_t nowEpoch) {
  lastHttpCode = 0;
  if (!BOOKORBIT.isConfigured()) return NOT_CONFIGURED;
  if (bookmarks.empty()) return OK;

  std::string body;
  if (buildBookmarksBody(bookmarks, nowEpoch, body) == 0) return OK;
  if (insufficientHeap()) return LOW_MEMORY;

  const std::string url = BOOKORBIT.getKoreaderBaseUrl() + "/plugin/bookmarks/exchange";
  const int code = performRequest(url, "POST", &body, nullptr);
  LOG_DBG("BOClient", "Bookmarks response: %d", code);
  return mapStatus(code);
}

BookOrbitClient::Error BookOrbitClient::exchangeBookmarks(const std::string& hash,
                                                          const std::vector<BookmarkChange>& changes,
                                                          const std::vector<BookmarkKey>& keys, bool keysComplete,
                                                          int64_t nowEpoch, BookmarkExchangeResult& out) {
  lastHttpCode = 0;
  out.adds.clear();
  out.deletes.clear();
  out.more = false;
  if (!BOOKORBIT.isConfigured()) return NOT_CONFIGURED;
  if (hash.empty()) return OK;
  if (insufficientHeap()) return LOW_MEMORY;

  JsonDocument doc;
  addDeviceFields(doc, nowEpoch);
  JsonArray books = doc["books"].to<JsonArray>();
  JsonObject book = books.add<JsonObject>();
  book["hash"] = hash;
  book["keysComplete"] = keysComplete;

  JsonArray keysArr = book["keys"].to<JsonArray>();
  for (const auto& k : keys) {
    if (keysArr.size() >= BOOKMARK_EXCHANGE_MAX_KEYS) break;
    if (k.key.empty() || k.datetime.empty()) continue;
    JsonObject ko = keysArr.add<JsonObject>();
    ko["k"] = k.key;
    ko["dt"] = k.datetime;
  }

  JsonArray changesArr = book["changes"].to<JsonArray>();
  for (const auto& c : changes) {
    if (changesArr.size() >= BOOKMARKS_MAX_PER_BOOK) break;
    if (c.pos.empty() || c.datetime.empty()) continue;
    JsonObject co = changesArr.add<JsonObject>();
    co["datetime"] = c.datetime;
    co["pos"] = c.pos;
    if (c.pageno >= 0) co["pageno"] = c.pageno;
    if (!c.chapter.empty()) co["chapter"] = c.chapter;
    if (!c.note.empty()) co["note"] = c.note;
  }

  std::string body;
  serializeJson(doc, body);

  std::string resp;
  const std::string url = BOOKORBIT.getKoreaderBaseUrl() + "/plugin/bookmarks/exchange";
  const int code = performRequest(url, "POST", &body, &resp);
  const Error status = mapStatus(code);
  if (status != OK) return status;

  JsonDocument rdoc;
  if (deserializeJson(rdoc, resp)) {
    LOG_ERR("BOClient", "Bookmark exchange JSON parse failed");
    return JSON_ERROR;
  }
  for (JsonObjectConst r : rdoc["results"].as<JsonArrayConst>()) {
    const char* rHash = r["hash"] | "";
    if (hash != rHash) continue;
    if (r["more"] | false) out.more = true;
    JsonObjectConst toApply = r["toApply"];
    for (JsonObjectConst a : toApply["add"].as<JsonArrayConst>()) {
      BookmarkAdd add;
      add.serverId = a["serverId"] | static_cast<int64_t>(0);
      add.pos = a["pos"] | "";
      add.pageno = a["pageno"] | -1;
      add.title = a["title"] | "";
      if (add.serverId > 0 && !add.pos.empty()) out.adds.push_back(std::move(add));
    }
    for (JsonObjectConst d : toApply["delete"].as<JsonArrayConst>()) {
      BookmarkDelete del;
      del.serverId = d["serverId"] | static_cast<int64_t>(0);
      del.key = d["key"] | "";
      del.datetime = d["datetime"] | "";
      if (del.serverId > 0) out.deletes.push_back(std::move(del));
    }
  }
  LOG_DBG("BOClient", "Bookmark exchange: +%u -%u more=%d", (unsigned)out.adds.size(), (unsigned)out.deletes.size(),
          (int)out.more);
  return OK;
}

BookOrbitClient::Error BookOrbitClient::ackBookmarks(const std::string& hash,
                                                     const std::vector<BookmarkAckApplied>& applied,
                                                     const std::vector<int64_t>& deletedServerIds, int64_t nowEpoch) {
  lastHttpCode = 0;
  if (!BOOKORBIT.isConfigured()) return NOT_CONFIGURED;
  if (hash.empty() || (applied.empty() && deletedServerIds.empty())) return OK;
  if (insufficientHeap()) return LOW_MEMORY;

  JsonDocument doc;
  addDeviceFields(doc, nowEpoch);
  JsonArray books = doc["books"].to<JsonArray>();
  JsonObject book = books.add<JsonObject>();
  book["hash"] = hash;

  JsonArray appliedArr = book["applied"].to<JsonArray>();
  for (const auto& a : applied) {
    if (a.serverId <= 0) continue;
    JsonObject ao = appliedArr.add<JsonObject>();
    ao["serverId"] = a.serverId;
    ao["status"] = "applied";
    if (!a.key.empty()) ao["key"] = a.key;
    if (!a.datetime.empty()) ao["datetime"] = a.datetime;
    if (!a.pos.empty()) ao["pos"] = a.pos;
  }

  JsonArray deletedArr = book["deleted"].to<JsonArray>();
  for (int64_t id : deletedServerIds) {
    if (id <= 0) continue;
    JsonObject dobj = deletedArr.add<JsonObject>();
    dobj["serverId"] = id;
    dobj["status"] = "applied";
  }

  std::string body;
  serializeJson(doc, body);

  const std::string url = BOOKORBIT.getKoreaderBaseUrl() + "/plugin/bookmarks/exchange-ack";
  const int code = performRequest(url, "POST", &body, nullptr);
  LOG_DBG("BOClient", "Bookmark ack response: %d", code);
  return mapStatus(code);
}

std::string BookOrbitClient::errorString(Error error) {
  switch (error) {
    case OK:
      return "Success";
    case NOT_CONFIGURED:
      return "BookOrbit is not configured. Set the URL and login in BookOrbit settings.";
    case NETWORK_ERROR:
      return "Network error. Check WiFi and the BookOrbit URL.";
    case AUTH_FAILED:
      return "Login rejected. Check your BookOrbit username and password.";
    case SERVER_ERROR:
      if (lastHttpCode == 404) return "Endpoint not found. Check the BookOrbit URL.";
      return "Server error (" + std::to_string(lastHttpCode) + ").";
    case JSON_ERROR:
      return "Unexpected response from the server.";
    case NOT_FOUND:
      return "No remote progress found.";
    case LOW_MEMORY:
      return "Not enough memory to sync right now.";
    default:
      return "Unknown error.";
  }
}
