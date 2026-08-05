#include "PendingBookmarks.h"

#include <utility>

void PendingBookmarkStore::toJson(JsonDocument& doc) const {
  doc["v"] = 1;
  JsonArray arr = doc["bookmarks"].to<JsonArray>();
  for (const auto& b : pending) {
    JsonObject o = arr.add<JsonObject>();
    o["h"] = b.docHash;
    o["p"] = b.pos;
    o["c"] = b.chapter;
    o["n"] = b.note;
    o["s"] = b.startEpoch;
  }
}

bool PendingBookmarkStore::fromJson(JsonVariantConst doc) {
  pending.clear();
  JsonArrayConst arr = doc["bookmarks"].as<JsonArrayConst>();
  for (JsonVariantConst v : arr) {
    if (pending.size() >= MAX_PENDING) break;
    PendingBookmark b;
    b.docHash = v["h"] | "";
    b.pos = v["p"] | "";
    if (b.docHash.empty() || b.pos.empty()) continue;
    b.chapter = v["c"] | "";
    b.note = v["n"] | "";
    b.startEpoch = v["s"] | static_cast<int64_t>(0);
    pending.push_back(std::move(b));
  }
  return true;
}

bool PendingBookmarkStore::add(const PendingBookmark& bookmark) {
  if (bookmark.docHash.empty() || bookmark.pos.empty()) return false;
  if (pending.size() >= MAX_PENDING) {
    pending.erase(pending.begin());
  }
  pending.push_back(bookmark);
  return saveToFile();
}

void PendingBookmarkStore::clear() {
  pending.clear();
  saveToFile();
}
