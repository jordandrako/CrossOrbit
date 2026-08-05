#include "PendingReadingSessions.h"

#include <cstdio>
#include <utility>

int64_t koReaderCivilToEpoch(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute, uint8_t second) {
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

void koReaderEpochToDatetime(int64_t epoch, char* buf, size_t bufLen) {
  if (!buf || bufLen == 0) return;
  if (epoch < 0) epoch = 0;

  int64_t days = epoch / 86400;
  int64_t secOfDay = epoch % 86400;
  const int hour = static_cast<int>(secOfDay / 3600);
  const int minute = static_cast<int>((secOfDay % 3600) / 60);
  const int second = static_cast<int>(secOfDay % 60);

  // civil_from_days (Howard Hinnant): inverse of the days-from-civil used above.
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(days - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t y = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  const int year = static_cast<int>(y + (m <= 2 ? 1 : 0));

  snprintf(buf, bufLen, "%04d-%02u-%02u %02d:%02d:%02d", year, m, d, hour, minute, second);
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
