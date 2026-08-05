#include "BookOrbitCapture.h"

#include <ChapterXPathResolver.h>
#include <HalClock.h>
#include <KOReaderDocumentId.h>
#include <Logging.h>

#include "BookOrbitConfig.h"
#include "PendingBookmarks.h"
#include "PendingHighlights.h"
#include "PendingReadingSessions.h"

namespace {
bool buffersLoaded = false;

std::string computeDocHash(const std::string& epubPath) {
  return BOOKORBIT.getMatchMethod() == BookOrbitMatchMethod::FILENAME
             ? KOReaderDocumentId::calculateFromFilename(epubPath)
             : KOReaderDocumentId::calculate(epubPath);
}

// UTC epoch from the RTC, or 0 when no RTC (dated at sync time on the server side).
int64_t captureEpoch() {
  uint16_t year = 0;
  uint8_t month = 0, day = 0, hour = 0, minute = 0;
  if (!halClock.getDateTime(year, month, day, hour, minute)) return 0;
  const int64_t epoch = koReaderCivilToEpoch(year, month, day, hour, minute, 0);
  return epoch > 0 ? epoch : 1;
}

uint16_t pctToPseudoPage(float pct) {
  if (pct < 0.0f) pct = 0.0f;
  if (pct > 100.0f) pct = 100.0f;
  return static_cast<uint16_t>(pct * 10.0f + 0.5f);
}
}  // namespace

void BookOrbitCapture::ensureLoaded() {
  BOOKORBIT.ensureLoaded();
  if (buffersLoaded) return;
  buffersLoaded = true;
  PENDING_STATS.loadFromFile();
  PENDING_HIGHLIGHTS.loadFromFile();
  PENDING_BOOKMARKS.loadFromFile();
}

void BookOrbitCapture::captureSession(const std::string& epubPath, uint32_t durationSeconds, float startProgressPercent,
                                      float endProgressPercent) {
  ensureLoaded();
  if (!BOOKORBIT.syncSessionsEnabled() || !BOOKORBIT.isConfigured() || durationSeconds < 60) return;

  PendingReadingSession session;
  session.docHash = computeDocHash(epubPath);
  if (session.docHash.empty()) return;

  const int64_t epoch = captureEpoch();
  if (epoch > 0) {
    const int64_t start = epoch - static_cast<int64_t>(durationSeconds);
    session.startEpoch = start > 0 ? start : 1;
  }
  session.durationSeconds = durationSeconds;
  session.totalPages = 1000;
  if (endProgressPercent <= 0.0f && startProgressPercent > 0.0f) endProgressPercent = startProgressPercent;
  session.startPage = pctToPseudoPage(startProgressPercent);
  session.endPage = pctToPseudoPage(endProgressPercent);
  PENDING_STATS.add(session);
  LOG_DBG("BOCap", "Buffered session: dur=%lus pages=%u->%u", static_cast<unsigned long>(durationSeconds),
          static_cast<unsigned>(session.startPage), static_cast<unsigned>(session.endPage));
}

void BookOrbitCapture::captureClipping(const std::shared_ptr<Epub>& epub, int spineIndex, uint16_t paragraphIndex,
                                       const std::string& text, const std::string& chapter) {
  ensureLoaded();
  if (!BOOKORBIT.isConfigured()) return;
  if (!BOOKORBIT.syncHighlightsEnabled()) {
    LOG_INF("BOCap", "Highlight sync disabled; clipping not buffered");
    return;
  }
  if (!epub || text.empty()) return;

  // Buffer the raw clip only. The clip text -> xpath resolution is deferred to sync time, where
  // the network-boot mode has the contiguous heap the XML parse needs (the reader does not, right
  // after a clip selection on a memory-constrained device).
  PendingHighlight highlight;
  highlight.docHash = computeDocHash(epub->getPath());
  if (highlight.docHash.empty()) return;
  highlight.spineIndex = static_cast<uint16_t>(spineIndex < 0 ? 0 : spineIndex);
  highlight.paragraphIndex = paragraphIndex;
  highlight.text = text;
  highlight.chapter = chapter;
  highlight.startEpoch = captureEpoch();
  PENDING_HIGHLIGHTS.add(highlight);
  LOG_INF("BOCap", "Buffered clipping for BookOrbit (spine=%d, pending=%u)", spineIndex,
          (unsigned)PENDING_HIGHLIGHTS.size());
}

void BookOrbitCapture::captureBookmark(const std::shared_ptr<Epub>& epub, int spineIndex, uint16_t paragraphIndex,
                                       float intraSpineProgress, const std::string& chapter,
                                       const std::string& snippet) {
  ensureLoaded();
  if (!BOOKORBIT.syncBookmarksEnabled() || !BOOKORBIT.isConfigured() || !epub) return;

  std::string pos;
  if (paragraphIndex != UINT16_MAX) {
    pos = ChapterXPathResolver::findXPathForParagraph(epub, spineIndex, paragraphIndex);
  }
  if (pos.empty()) {
    pos = ChapterXPathResolver::findXPathForProgress(epub, spineIndex, intraSpineProgress);
  }
  if (pos.empty()) return;

  PendingBookmark bookmark;
  bookmark.docHash = computeDocHash(epub->getPath());
  if (bookmark.docHash.empty()) return;
  bookmark.pos = std::move(pos);
  bookmark.chapter = chapter;
  bookmark.note = snippet;
  bookmark.startEpoch = captureEpoch();
  PENDING_BOOKMARKS.add(bookmark);
  LOG_DBG("BOCap", "Buffered bookmark: hash=%s pos=%s", bookmark.docHash.c_str(), bookmark.pos.c_str());
}
