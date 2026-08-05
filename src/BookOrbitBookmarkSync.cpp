#include "BookOrbitBookmarkSync.h"

#include <BookOrbitClient.h>
#include <BookOrbitConfig.h>
#include <ChapterXPathResolver.h>
#include <Logging.h>
#include <MD5Builder.h>
#include <ProgressMapper.h>

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

#include "BookmarkStore.h"

namespace {

// Local bookmarks carry no creation time (Bookmark::timestamp is reserved and always 0), so every
// device identity uses one stable synthetic datetime. The identity that matters is md5(dt|pos),
// and pos is unique per page, so a constant datetime keeps keys stable and reproducible across
// syncs without touching the on-disk bookmark format.
constexpr char SENTINEL_DATETIME[] = "2000-01-01 00:00:00";
constexpr int MAX_EXCHANGE_ROUNDS = 5;
constexpr size_t MAX_KEYS = 500;  // matches the server's per-book key cap

std::string md5Hex(const std::string& value) {
  MD5Builder md5;
  md5.begin();
  md5.add(value.c_str());
  md5.calculate();
  return std::string(md5.toString().c_str());
}

// Canonical KOReader xpointer for a stored position, using the same resolver the reader uses to
// capture a bookmark. Keeping device-origin and pulled bookmarks on one derivation is what makes
// their identities stable round to round (the server xpointer round-trips lossily otherwise).
std::string canonicalPos(const std::shared_ptr<Epub>& epub, int spineIndex, uint16_t paragraphIndex, float progress) {
  std::string pos;
  if (paragraphIndex != UINT16_MAX) {
    pos = ChapterXPathResolver::findXPathForParagraph(epub, spineIndex, paragraphIndex);
  }
  if (pos.empty()) {
    pos = ChapterXPathResolver::findXPathForProgress(epub, spineIndex, progress);
  }
  return pos;
}

std::string keyForPos(const std::string& pos) {
  return pos.empty() ? std::string() : md5Hex(std::string(SENTINEL_DATETIME) + "|" + pos);
}

// Turn one server push-down add into a local bookmark plus the ack that links it back.
void applyAdd(const std::shared_ptr<Epub>& epub, PositionCoordinateSpace space, const BookmarkAdd& add,
              BookmarkStore& store, std::vector<BookmarkAckApplied>& applied) {
  KOReaderPosition ko;
  ko.xpath = add.pos;
  ko.percentage = 0.0f;
  ko.valid = true;

  const CrossPointPosition cp = ProgressMapper::toCrossPoint(epub, ko, -1, 0, space);
  if (cp.spineIndex < 0) {
    LOG_DBG("BOBmk", "Skipping inbound bookmark: unmappable position");
    return;
  }
  const int pageCount = std::max(1, cp.totalPages);
  const float progress = static_cast<float>(cp.pageNumber) / static_cast<float>(pageCount);
  const uint16_t paragraphIndex = cp.hasParagraphIndex ? cp.paragraphIndex : UINT16_MAX;

  // Chapter label from the local TOC; the server's title becomes the snippet shown in the list.
  const int tocIdx = epub->getTocIndexForSpineIndex(cp.spineIndex);
  const std::string chapter = tocIdx >= 0 ? epub->getTocItem(tocIdx).title : std::string();

  store.addBookmark(static_cast<uint16_t>(cp.spineIndex), progress, pageCount,
                    chapter.empty() ? nullptr : chapter.c_str(), paragraphIndex,
                    add.title.empty() ? nullptr : add.title.c_str());

  // Ack with the device's own canonical identity so the next round enumerates the same key the
  // server now holds for this bookmark. Fall back to the raw server pos if canonicalization fails.
  std::string pos = canonicalPos(epub, cp.spineIndex, paragraphIndex, progress);
  if (pos.empty()) pos = add.pos;

  BookmarkAckApplied ack;
  ack.serverId = add.serverId;
  ack.datetime = SENTINEL_DATETIME;
  ack.pos = pos;
  ack.key = keyForPos(pos);
  applied.push_back(std::move(ack));
}

}  // namespace

bool BookOrbitBookmarkSync::syncOpenBook(const std::shared_ptr<Epub>& epub, const std::string& hash, int64_t nowEpoch) {
  if (!epub || hash.empty()) return false;

  BookmarkStore& store = BookmarkStore::getInstance();
  if (!store.loadForBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), "epub")) {
    LOG_ERR("BOBmk", "Could not load bookmark store for two-way sync");
    return false;
  }

  const PositionCoordinateSpace space = BOOKORBIT.getMatchMethod() == BookOrbitMatchMethod::FILENAME
                                            ? PositionCoordinateSpace::SourceDocument
                                            : PositionCoordinateSpace::CurrentDocument;

  bool anyRoundOk = false;
  for (int round = 0; round < MAX_EXCHANGE_ROUNDS; round++) {
    // Snapshot the current local set into changes + keys. Both use the same canonical pos, so a
    // dogear's identity is identical whether it originated here or was pulled from the server.
    const auto& bookmarks = store.getBookmarks();
    std::vector<BookmarkChange> changes;
    std::vector<BookmarkKey> keys;
    std::vector<std::pair<std::string, size_t>> keyToIndex;  // key -> store index, for inbound deletes
    changes.reserve(bookmarks.size());
    keys.reserve(bookmarks.size());
    keyToIndex.reserve(bookmarks.size());

    bool allResolved = true;
    for (size_t i = 0; i < bookmarks.size(); i++) {
      const Bookmark& bm = bookmarks[i];
      const std::string pos = canonicalPos(epub, bm.spineIndex, bm.paragraphIndex, bm.progress);
      if (pos.empty()) {
        // A local bookmark we cannot place has no identity to send; excluding it means the key
        // set is no longer provably complete, so deletion detection must be disabled this round.
        allResolved = false;
        continue;
      }
      const std::string key = keyForPos(pos);

      keys.push_back({key, SENTINEL_DATETIME});
      keyToIndex.emplace_back(key, i);

      BookmarkChange change;
      change.datetime = SENTINEL_DATETIME;
      change.pos = pos;
      change.chapter = bm.chapterTitle;
      change.note = bm.snippet;
      changes.push_back(std::move(change));
    }

    // Claim a complete key set only when every local bookmark resolved and the set fits the
    // server cap; otherwise the server would read the gap as device-side deletions and tombstone
    // the missing rows.
    const bool keysComplete = allResolved && keys.size() <= MAX_KEYS;

    BookmarkExchangeResult result;
    const BookOrbitClient::Error err =
        BookOrbitClient::exchangeBookmarks(hash, changes, keys, keysComplete, nowEpoch, result);
    if (err != BookOrbitClient::OK) {
      LOG_ERR("BOBmk", "Bookmark exchange failed: %s", BookOrbitClient::errorString(err).c_str());
      break;
    }
    anyRoundOk = true;

    std::vector<BookmarkAckApplied> applied;
    std::vector<int64_t> deletedIds;

    // Inbound tombstones: remove local bookmarks whose identity the server deleted. Collect the
    // indices first (they are valid against the pre-mutation snapshot), then erase high-to-low so
    // earlier indices stay correct.
    std::vector<size_t> removeIdxs;
    for (const auto& del : result.deletes) {
      deletedIds.push_back(del.serverId);
      for (const auto& kv : keyToIndex) {
        if (kv.first == del.key) {
          removeIdxs.push_back(kv.second);
          break;
        }
      }
    }
    std::sort(removeIdxs.begin(), removeIdxs.end(), std::greater<size_t>());
    removeIdxs.erase(std::unique(removeIdxs.begin(), removeIdxs.end()), removeIdxs.end());
    for (size_t idx : removeIdxs) store.removeBookmarkAt(idx);

    // Inbound adds: create local bookmarks the server has that this device lacks.
    for (const auto& add : result.adds) applyAdd(epub, space, add, store, applied);

    if (!applied.empty() || !deletedIds.empty()) {
      const BookOrbitClient::Error ackErr = BookOrbitClient::ackBookmarks(hash, applied, deletedIds, nowEpoch);
      if (ackErr != BookOrbitClient::OK) {
        LOG_ERR("BOBmk", "Bookmark ack failed: %s", BookOrbitClient::errorString(ackErr).c_str());
      }
    }

    store.saveToFile();
    LOG_INF("BOBmk", "Bookmark round %d: applied +%u -%u", round, (unsigned)applied.size(),
            (unsigned)removeIdxs.size());
    if (!result.more) break;
  }

  store.unload();
  return anyRoundOk;
}
