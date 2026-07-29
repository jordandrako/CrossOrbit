#include "PendingReadingSessions.h"

#include <utility>

int64_t koReaderCivilToEpoch(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute,
                             uint8_t second) {
  int y = static_cast<int>(year);
  const unsigned m = month;
  y -= (m <= 2);
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + day - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  const int64_t days = era * 146097 + static_cast<int64_t>(doe) - 719468;
  return days * 86400 + static_cast<int64_t>(hour) * 3600 + static_cast<int64_t>(minute) * 60 +
         static_cast<int64_t>(second);
}

void PendingReadingSessionStore::toJson(JsonDocument& doc) const {
  doc["v"] = 1;
  JsonArray arr = doc["sessions"].to<JsonArray>();
  for (const auto& s : pending) {
    JsonObject o = arr.add<JsonObject>();
    o["h"] = s.docHash;
    o["s"] = s.startEpoch;
    o["d"] = s.durationSeconds;
    o["sp"] = s.startPage;
    o["ep"] = s.endPage;
    o["tp"] = s.totalPages;
  }
}

bool PendingReadingSessionStore::fromJson(JsonVariantConst doc) {
  pending.clear();
  JsonArrayConst arr = doc["sessions"].as<JsonArrayConst>();
  for (JsonVariantConst v : arr) {
    if (pending.size() >= MAX_PENDING) break;
    PendingReadingSession s;
    s.docHash = v["h"] | "";
    if (s.docHash.empty()) continue;
    s.startEpoch = v["s"] | static_cast<int64_t>(0);
    s.durationSeconds = v["d"] | static_cast<uint32_t>(0);
    s.startPage = v["sp"] | static_cast<uint16_t>(0);
    s.endPage = v["ep"] | static_cast<uint16_t>(0);
    s.totalPages = v["tp"] | static_cast<uint16_t>(1000);
    if (s.totalPages == 0) s.totalPages = 1000;
    pending.push_back(std::move(s));
  }
  return true;
}

bool PendingReadingSessionStore::add(const PendingReadingSession& session) {
  if (session.docHash.empty() || session.durationSeconds == 0) return false;
  if (pending.size() >= MAX_PENDING) {
    pending.erase(pending.begin());
  }
  pending.push_back(session);
  return saveToFile();
}

void PendingReadingSessionStore::clear() {
  pending.clear();
  saveToFile();
}
