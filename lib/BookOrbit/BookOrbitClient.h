#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct PendingReadingSession;
struct PendingBookmark;

// A clipping resolved to a KOReader xpointer range, ready to upload as an annotation. Resolution
// (clip text -> pos0/pos1) happens at sync time, so the client takes the resolved form.
struct BookOrbitAnnotation {
  std::string docHash;
  std::string pos0;
  std::string pos1;
  std::string text;
  std::string chapter;
  int64_t startEpoch = 0;
};

// Reading progress as stored on the BookOrbit / KOReader sync server.
struct BookOrbitProgress {
  std::string document;     // Document hash
  std::string progress;     // KOReader xpath progress string
  float percentage = 0.0f;  // 0.0 - 1.0
  std::string device;       // Device name that last wrote it
  int64_t timestamp = 0;    // Unix timestamp of the last update
};

// ---- Two-way bookmark exchange (POST /plugin/bookmarks/exchange[-ack]) ----

// A new or renamed dogear to push to the server (device -> server).
struct BookmarkChange {
  std::string datetime;  // "YYYY-MM-DD HH:MM:SS"
  std::string pos;       // KOReader xpointer
  int pageno = -1;       // -1 to omit
  std::string chapter;
  std::string note;
};

// One local bookmark identity. Sent so the server can detect device-side deletions when the
// full set is enumerated (keysComplete). identity = md5("datetime|pos").
struct BookmarkKey {
  std::string key;       // 32-char lowercase hex md5
  std::string datetime;  // "YYYY-MM-DD HH:MM:SS"
};

// A server bookmark the device does not have yet (server -> device).
struct BookmarkAdd {
  int64_t serverId = 0;
  std::string pos;  // KOReader xpointer
  int pageno = -1;
  std::string title;
};

// A server bookmark that was tombstoned and should be removed on the device.
struct BookmarkDelete {
  int64_t serverId = 0;
  std::string key;       // device identity of the row (may be empty)
  std::string datetime;  // may be empty
};

struct BookmarkExchangeResult {
  std::vector<BookmarkAdd> adds;
  std::vector<BookmarkDelete> deletes;
  bool more = false;  // server has more push-down for this book; exchange again
};

// Confirms the device applied a pushed-down add, so the server links it to this device.
struct BookmarkAckApplied {
  int64_t serverId = 0;
  std::string key;       // md5("datetime|pos") the device assigned
  std::string datetime;  // "YYYY-MM-DD HH:MM:SS"
  std::string pos;       // canonical xpointer the device stored
};

/**
 * HTTP client for a BookOrbit instance, fully independent of the upstream KOReaderSyncClient
 * so it never has to be reconciled on an upstream pull. All endpoints are derived from
 * BookOrbitConfig's single URL: {url}/api/v1/koreader (+ /plugin/... for the extensions).
 *
 * Auth is the KOReader plugin scheme (x-auth-user / x-auth-key) plus HTTP Basic, sourced from
 * the BookOrbit login. All calls return OK / an error code; none throw.
 */
class BookOrbitClient {
 public:
  enum Error {
    OK = 0,
    NOT_CONFIGURED,
    NETWORK_ERROR,
    AUTH_FAILED,
    SERVER_ERROR,
    JSON_ERROR,
    NOT_FOUND,
    LOW_MEMORY,
  };

  // Validate credentials (GET /users/auth).
  static Error authenticate();

  // Fetch reading progress for a document (GET /syncs/progress/:hash).
  // Returns NOT_FOUND when the server has no progress for the document.
  static Error getProgress(const std::string& documentHash, BookOrbitProgress& out);

  // Push reading progress (PUT /syncs/progress).
  static Error updateProgress(const BookOrbitProgress& progress);

  // Push buffered reading sessions as page-stats (POST /plugin/page-stats).
  static Error uploadPageStats(const std::vector<PendingReadingSession>& sessions, int64_t nowEpoch);

  // Push resolved clippings as highlights (POST /plugin/annotations).
  static Error uploadAnnotations(const std::vector<BookOrbitAnnotation>& annotations, int64_t nowEpoch);

  // Push buffered bookmarks (POST /plugin/bookmarks/exchange, one-way changes only). Used for
  // books other than the currently-open one, which get the full two-way exchange below instead.
  static Error uploadBookmarks(const std::vector<PendingBookmark>& bookmarks, int64_t nowEpoch);

  // Two-way bookmark exchange for one book (POST /plugin/bookmarks/exchange). `changes` are the
  // new/renamed dogears to push; `keys` is the device's full identity set for the book, used for
  // deletion detection only when `keysComplete` is true. Fills `out` with the server's push-down
  // (bookmarks to add/remove on the device) and a `more` flag when the push-down is paginated.
  static Error exchangeBookmarks(const std::string& hash, const std::vector<BookmarkChange>& changes,
                                 const std::vector<BookmarkKey>& keys, bool keysComplete, int64_t nowEpoch,
                                 BookmarkExchangeResult& out);

  // Confirm applied push-down (POST /plugin/bookmarks/exchange-ack) so the server links applied
  // adds to this device and drops this device's links for applied deletes.
  static Error ackBookmarks(const std::string& hash, const std::vector<BookmarkAckApplied>& applied,
                            const std::vector<int64_t>& deletedServerIds, int64_t nowEpoch);

  // Human-readable message for the last error (for the sync UI).
  static std::string errorString(Error error);

  static int lastHttpCode;
};
