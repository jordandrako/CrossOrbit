#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>
#include <vector>

// One bookmark captured on the device, resolved to a KOReader xpath position and buffered on
// the SD card until it can be pushed to a BookOrbit / KOReader-plugin server via the bookmark
// exchange endpoint (POST {base}/plugin/bookmarks/exchange, one-way: changes only).
struct PendingBookmark {
  std::string docHash;     // KOReader document hash (filename or binary MD5)
  std::string pos;         // KOReader xpath position of the bookmark
  std::string chapter;     // Chapter title (optional)
  std::string note;        // Snippet / note (optional)
  int64_t startEpoch = 0;  // UTC seconds when captured; 0 when no RTC was present (dated at sync)
};

/**
 * Singleton buffer of bookmarks awaiting upload, persisted to
 * /.crosspoint/pending_bookmarks.json. Cleared once a KOReader sync flushes them.
 */
class PendingBookmarkStore : public PersistableStore<PendingBookmarkStore> {
 private:
  std::vector<PendingBookmark> pending;

  PendingBookmarkStore() = default;
  ~PendingBookmarkStore() = default;

  friend class PersistableStore<PendingBookmarkStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/pending_bookmarks.json"; }

  // Upper bound on buffered bookmarks. Bounds SD/RAM use; oldest are dropped past this.
  static constexpr size_t MAX_PENDING = 100;

  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  const std::vector<PendingBookmark>& bookmarks() const { return pending; }
  bool empty() const { return pending.empty(); }
  size_t size() const { return pending.size(); }

  // Appends a bookmark and persists. Drops the oldest when at capacity. Returns false on
  // write failure or when the bookmark is missing its hash or position.
  bool add(const PendingBookmark& bookmark);

  // Clears all buffered bookmarks and persists the now-empty buffer.
  void clear();
};

// Helper macro to access the pending bookmarks store
#define PENDING_BOOKMARKS PendingBookmarkStore::getInstance()
