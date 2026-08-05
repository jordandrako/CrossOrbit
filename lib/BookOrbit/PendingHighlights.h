#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>
#include <vector>

// One clipping captured on the device, buffered on the SD card until a BookOrbit sync resolves it
// to KOReader-style highlight positions and pushes it as an annotation (POST {base}/plugin/
// annotations). Resolution is deferred to sync time (network-boot mode) because the word->xpath
// XML parse needs more contiguous heap than the reader has right after a clip selection.
struct PendingHighlight {
  std::string docHash;          // KOReader document hash (filename or binary MD5)
  uint16_t spineIndex = 0;      // spine item the clip is in
  uint16_t paragraphIndex = 0;  // 1-based paragraph hint (UINT16_MAX if unknown); only a hint
  std::string text;             // clipped text; matched against the source to place the highlight
  std::string chapter;          // Chapter title (optional context)
  int64_t startEpoch = 0;       // UTC seconds when captured; 0 when no RTC was present (dated at sync)
};

/**
 * Singleton buffer of highlights awaiting upload, persisted to
 * /.crosspoint/pending_highlights.json. Cleared once a KOReader sync flushes them.
 */
class PendingHighlightStore : public PersistableStore<PendingHighlightStore> {
 private:
  std::vector<PendingHighlight> pending;

  PendingHighlightStore() = default;
  ~PendingHighlightStore() = default;

  friend class PersistableStore<PendingHighlightStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/pending_highlights.json"; }

  // Upper bound on buffered highlights. Bounds SD/RAM use; oldest are dropped past this.
  static constexpr size_t MAX_PENDING = 100;

  // BookOrbit caps the annotation text; keep the buffered copy small too.
  static constexpr size_t TEXT_MAX = 1000;

  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  const std::vector<PendingHighlight>& highlights() const { return pending; }
  bool empty() const { return pending.empty(); }
  size_t size() const { return pending.size(); }

  // Appends a highlight and persists. Drops the oldest when at capacity. Returns false on
  // write failure or when the highlight is missing its hash or text.
  bool add(const PendingHighlight& highlight);

  // Clears all buffered highlights and persists the now-empty buffer.
  void clear();

  // Removes every buffered highlight for one document hash and persists. Used after a sync
  // resolves and uploads the open book's highlights, leaving other books' clips buffered.
  void removeForHash(const std::string& docHash);
};

// Helper macro to access the pending highlights store
#define PENDING_HIGHLIGHTS PendingHighlightStore::getInstance()
