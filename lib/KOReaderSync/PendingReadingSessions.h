#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>
#include <vector>

// One reading session captured on the device while a book was open, buffered on
// the SD card until it can be pushed to a BookOrbit / KOReader-plugin server as
// page-stats events. Stored as a compact per-session record rather than per-page
// events to keep the buffer small; the upload client expands each record into the
// page-stat events the server expects.
struct PendingReadingSession {
  std::string docHash;           // KOReader document hash (filename or binary MD5)
  int64_t startEpoch = 0;        // UTC start time in seconds; 0 when no RTC was present at capture
  uint32_t durationSeconds = 0;  // Active reading time (idle already excluded)
  uint16_t startPage = 0;        // Synthetic page at session start (0..totalPages)
  uint16_t endPage = 0;          // Synthetic page at session end (0..totalPages)
  uint16_t totalPages = 1000;    // Synthetic denominator; startPage/endPage encode the progress fraction
};

// Convert a UTC civil date-time to a Unix epoch (seconds). Pure; does not touch
// the RTC. Uses the days-from-civil algorithm so it is valid for any Gregorian date.
int64_t koReaderCivilToEpoch(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute, uint8_t second);

/**
 * Singleton buffer of reading sessions awaiting upload, persisted to
 * /.crosspoint/pending_sessions.json. Sessions accumulate as books are closed and
 * are cleared once a KOReader sync successfully flushes them to the server.
 */
class PendingReadingSessionStore : public PersistableStore<PendingReadingSessionStore> {
 private:
  std::vector<PendingReadingSession> pending;

  PendingReadingSessionStore() = default;
  ~PendingReadingSessionStore() = default;

  friend class PersistableStore<PendingReadingSessionStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/pending_sessions.json"; }

  // Upper bound on buffered sessions. Bounds SD/RAM use and keeps a single upload
  // within the server's per-request limits. Oldest sessions are dropped past this.
  static constexpr size_t MAX_PENDING = 50;

  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  const std::vector<PendingReadingSession>& sessions() const { return pending; }
  bool empty() const { return pending.empty(); }
  size_t size() const { return pending.size(); }

  // Appends a session and persists the buffer. Drops the oldest session when at
  // capacity so the most recent reading is always kept. Returns false on write
  // failure or when the session is missing a hash or has zero duration.
  bool add(const PendingReadingSession& session);

  // Clears all buffered sessions and persists the now-empty buffer.
  void clear();
};

// Helper macro to access the pending sessions store
#define PENDING_STATS PendingReadingSessionStore::getInstance()
