#include "PendingHighlights.h"

#include <utility>

void PendingHighlightStore::toJson(JsonDocument& doc) const {
  doc["v"] = 2;
  JsonArray arr = doc["highlights"].to<JsonArray>();
  for (const auto& h : pending) {
    JsonObject o = arr.add<JsonObject>();
    o["h"] = h.docHash;
    o["si"] = h.spineIndex;
    o["pi"] = h.paragraphIndex;
    o["t"] = h.text;
    o["c"] = h.chapter;
    o["s"] = h.startEpoch;
  }
}

bool PendingHighlightStore::fromJson(JsonVariantConst doc) {
  pending.clear();
  JsonArrayConst arr = doc["highlights"].as<JsonArrayConst>();
  for (JsonVariantConst v : arr) {
    if (pending.size() >= MAX_PENDING) break;
    PendingHighlight h;
    h.docHash = v["h"] | "";
    h.text = v["t"] | "";
    if (h.docHash.empty() || h.text.empty()) continue;  // pre-v2 records (no text) are dropped
    h.spineIndex = static_cast<uint16_t>(v["si"] | 0);
    h.paragraphIndex = static_cast<uint16_t>(v["pi"] | 0);
    h.chapter = v["c"] | "";
    h.startEpoch = v["s"] | static_cast<int64_t>(0);
    pending.push_back(std::move(h));
  }
  return true;
}

bool PendingHighlightStore::add(const PendingHighlight& highlight) {
  if (highlight.docHash.empty() || highlight.text.empty()) return false;
  if (pending.size() >= MAX_PENDING) {
    pending.erase(pending.begin());
  }
  pending.push_back(highlight);
  if (pending.back().text.size() > TEXT_MAX) {
    pending.back().text.resize(TEXT_MAX);
  }
  return saveToFile();
}

void PendingHighlightStore::clear() {
  pending.clear();
  saveToFile();
}

void PendingHighlightStore::removeForHash(const std::string& docHash) {
  const size_t before = pending.size();
  std::erase_if(pending, [&](const PendingHighlight& h) { return h.docHash == docHash; });
  if (pending.size() != before) saveToFile();
}
