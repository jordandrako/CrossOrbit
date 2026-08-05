#pragma once
#include <Epub.h>

#include <cstdint>
#include <memory>
#include <string>

/**
 * Two-way bookmark sync for the currently-open book against a BookOrbit server.
 *
 * Kept in its own translation unit (not the upstream BookmarkStore, not the sync activity) so
 * BookOrbit-specific behaviour stays sequestered for easy upstream pulls. It reuses the
 * store-agnostic ChapterXPathResolver / ProgressMapper for position conversion and drives the
 * BookOrbitClient exchange/ack endpoints.
 */
namespace BookOrbitBookmarkSync {

/**
 * Reconcile the open book's local bookmarks with the server: push new/renamed dogears, apply
 * inbound adds and tombstone deletes, and ack so the server links what the device applied.
 *
 * `epub` must be loaded and `hash` is the book's KOReader document hash. Loads and saves the
 * book's local bookmark store internally. Returns true when at least one exchange round
 * completed without a transport error (so the caller may clear this book's pending buffer).
 */
bool syncOpenBook(const std::shared_ptr<Epub>& epub, const std::string& hash, int64_t nowEpoch);

}  // namespace BookOrbitBookmarkSync
