#include "BookOrbitSyncActivity.h"

#include <BookOrbitCapture.h>
#include <ChapterXPathResolver.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <KOReaderDocumentId.h>
#include <Logging.h>
#include <PendingBookmarks.h>
#include <PendingHighlights.h>
#include <PendingReadingSessions.h>
#include <WiFi.h>

#include <algorithm>
#include <ctime>

#include "BookOrbitBookmarkSync.h"
#include "CrossPointSettings.h"
#include "Epub/Section.h"
#include "EpubReaderUtils.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/home/RecentBookProgress.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/WifiUtils.h"

namespace {
constexpr int RESULT_LOCAL_PAGE_Y_OFFSET = 200;

void syncTimeWithNTP() {
  // Route NTP through the HAL, which uses the esp-netif SNTP API and drains the DNS callback
  // under lwIP's core lock. Calling esp_sntp_* directly from this task asserts in sys_untimeout
  // ("Required to lock TCPIP core functionality!") under the v1.5.0 IDF (upstream issue 480).
#ifndef SIMULATOR
  if (!halClock.syncSystemTimeFromNTP()) {
    LOG_DBG("BOSync", "NTP sync unavailable, using fallback");
  }
#endif
}

void wifiOff() {
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
}
}  // namespace

std::string BookOrbitSyncActivity::documentHashForConfig() const {
  return BOOKORBIT.getMatchMethod() == BookOrbitMatchMethod::FILENAME
             ? KOReaderDocumentId::calculateFromFilename(epubPath)
             : KOReaderDocumentId::calculate(epubPath);
}

std::string BookOrbitSyncActivity::syncErrorMessage(BookOrbitClient::Error error) const {
  switch (error) {
    case BookOrbitClient::AUTH_FAILED:
      return tr(STR_KOREADER_SYNC_AUTH_REJECTED);
    case BookOrbitClient::NETWORK_ERROR:
      return tr(STR_KOREADER_SYNC_NETWORK_ERROR);
    case BookOrbitClient::LOW_MEMORY:
      return tr(STR_KOREADER_SYNC_LOW_MEMORY);
    case BookOrbitClient::JSON_ERROR:
      return tr(STR_KOREADER_SYNC_BAD_RESPONSE);
    case BookOrbitClient::NOT_CONFIGURED:
      return tr(STR_NO_CREDENTIALS_MSG);
    default:
      if (BookOrbitClient::lastHttpCode == 404) return tr(STR_KOREADER_SYNC_HTTP_404);
      return tr(STR_KOREADER_SYNC_SERVER_ERROR);
  }
}

void BookOrbitSyncActivity::ensureEpubLoaded() {
  if (!epub) {
    epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
    epub->setupCacheDir();
    if (!epub->load(false, true)) {
      LOG_ERR("BOSync", "Failed to load epub for progress mapping");
      epub.reset();
    }
  }
}

bool BookOrbitSyncActivity::ensureLocalProgressLoaded() {
  if (!localProgressDeferred) return localProgress.valid;
  ensureEpubLoaded();
  if (!epub) return false;

  EpubReaderUtils::Progress progress;
  if (EpubReaderUtils::loadProgress(*epub, progress, "BOSync")) {
    currentSpineIndex = progress.spineIndex;
    currentPage = progress.pageNumber;
    if (progress.hasPageCount) totalPagesInSpine = std::max(1, progress.pageCount);
  }
  if (currentSpineIndex < 0 || currentSpineIndex >= epub->getSpineItemsCount()) currentSpineIndex = 0;

  const CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPagesInSpine};
  const PositionCoordinateSpace space = BOOKORBIT.getMatchMethod() == BookOrbitMatchMethod::FILENAME
                                            ? PositionCoordinateSpace::SourceDocument
                                            : PositionCoordinateSpace::CurrentDocument;
  localProgress = ProgressMapper::toKOReader(epub, localPos, space);
  const int tocIdx = epub->getTocIndexForSpineIndex(currentSpineIndex);
  localChapterName = tocIdx >= 0 ? epub->getTocItem(tocIdx).title : "";
  localProgressDeferred = false;
  return localProgress.valid;
}

void BookOrbitSyncActivity::flushExtensions() {
  BookOrbitCapture::ensureLoaded();  // reload buffers after the network reboot
  const int64_t nowEpoch = static_cast<int64_t>(time(nullptr));

  // Show what the sync is doing before each (slow) network step. Rendered while state is UPLOADING.
  auto showStatus = [this](const char* msg) {
    {
      RenderLock lock(*this);
      state = UPLOADING;
      statusMessage = msg;
    }
    (void)requestUpdateAndWait();
  };

  // Each extension only touches the network when it actually has something buffered to send.
  if (BOOKORBIT.syncSessionsEnabled() && !PENDING_STATS.empty()) {
    showStatus(tr(STR_UPLOADING_SESSIONS));
    if (BookOrbitClient::uploadPageStats(PENDING_STATS.sessions(), nowEpoch) == BookOrbitClient::OK) {
      LOG_INF("BOSync", "Flushed %u session(s)", (unsigned)PENDING_STATS.size());
      PENDING_STATS.clear();
    }
  }

  if (BOOKORBIT.syncHighlightsEnabled() && !PENDING_HIGHLIGHTS.empty()) {
    showStatus(tr(STR_UPLOADING_HIGHLIGHTS));
    ensureEpubLoaded();
    const std::string openHash = epub ? documentHashForConfig() : std::string();
    if (!epub || openHash.empty()) {
      LOG_ERR("BOSync", "No EPUB loaded; cannot resolve highlights this sync");
    } else {
      // Resolve the open book's clips to xpointer ranges here (network-boot mode has the heap the
      // XML parse needs); other books' clips stay buffered until their book is synced.
      std::vector<BookOrbitAnnotation> resolved;
      unsigned unresolved = 0;
      for (const auto& h : PENDING_HIGHLIGHTS.highlights()) {
        if (h.docHash != openHash) continue;
        std::string pos0;
        std::string pos1;
        if (ChapterXPathResolver::findHighlightXPathRange(epub, h.spineIndex, h.paragraphIndex, h.text, pos0, pos1)) {
          BookOrbitAnnotation a;
          a.docHash = h.docHash;
          a.pos0 = std::move(pos0);
          a.pos1 = std::move(pos1);
          a.text = h.text;
          a.chapter = h.chapter;
          a.startEpoch = h.startEpoch;
          resolved.push_back(std::move(a));
        } else {
          unresolved++;
        }
      }
      if (resolved.empty()) {
        LOG_INF("BOSync", "No highlights resolved for this book (%u unresolved)", unresolved);
      } else {
        const BookOrbitClient::Error err = BookOrbitClient::uploadAnnotations(resolved, nowEpoch);
        if (err == BookOrbitClient::OK) {
          LOG_INF("BOSync", "Flushed %u highlight(s) (%u unresolved)", (unsigned)resolved.size(), unresolved);
          PENDING_HIGHLIGHTS.removeForHash(openHash);
        } else {
          LOG_ERR("BOSync", "Highlight upload failed (%u resolved): %s (http=%d)", (unsigned)resolved.size(),
                  BookOrbitClient::errorString(err).c_str(), BookOrbitClient::lastHttpCode);
        }
      }
    }
  }

  if (BOOKORBIT.syncBookmarksEnabled() && !PENDING_BOOKMARKS.empty()) {
    showStatus(tr(STR_UPLOADING_BOOKMARKS));
    ensureEpubLoaded();
    const std::string openHash = epub ? documentHashForConfig() : std::string();
    if (epub && !openHash.empty()) {
      // Full two-way reconciliation for the open book (its EPUB is loaded for xpath conversion).
      const bool ok = BookOrbitBookmarkSync::syncOpenBook(epub, openHash, nowEpoch);
      // Buffered bookmarks for other books can only be pushed one-way (no EPUB to map inbound).
      std::vector<PendingBookmark> others;
      for (const auto& b : PENDING_BOOKMARKS.bookmarks()) {
        if (b.docHash != openHash) others.push_back(b);
      }
      if (ok && !others.empty()) BookOrbitClient::uploadBookmarks(others, nowEpoch);
      if (ok) PENDING_BOOKMARKS.clear();
    } else {
      // No EPUB (non-EPUB book or load failure): fall back to a one-way push of everything.
      if (BookOrbitClient::uploadBookmarks(PENDING_BOOKMARKS.bookmarks(), nowEpoch) == BookOrbitClient::OK) {
        PENDING_BOOKMARKS.clear();
      }
    }
  }
}

void BookOrbitSyncActivity::saveProgressAndReturn(const CrossPointPosition& position) {
  ensureEpubLoaded();
  if (epub) {
    const int pageCount = std::max(position.totalPages, position.pageNumber + 1);
    if (!EpubReaderUtils::saveProgress(*epub, position.spineIndex, position.pageNumber, pageCount)) {
      {
        RenderLock lock(*this);
        state = SYNC_FAILED;
        statusMessage = tr(STR_SAVE_PROGRESS_FAILED);
      }
      requestUpdate(true);
      return;
    }
    RecentBookProgress::saveCachedEpubPercent(*epub, position.spineIndex, position.pageNumber, pageCount);
  }
  epub.reset();
  flushExtensions();
  returnToReader();
}

void BookOrbitSyncActivity::returnToReader() { activityManager.goToReader(epubPath); }

bool BookOrbitSyncActivity::consumeInitialConfirmRelease() {
  if (!lockInitialConfirmRelease) return false;
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      !mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
    lockInitialConfirmRelease = false;
  }
  return true;
}

void BookOrbitSyncActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    returnToReader();
    return;
  }
  sdFontSystem.releaseForNetwork(renderer);
  {
    RenderLock lock(*this);
    state = SYNCING;
    statusMessage = tr(STR_SYNCING_TIME);
  }
  requestUpdate(true);
  syncTimeWithNTP();

  // Extensions-only when progress sync is disabled.
  if (!BOOKORBIT.syncProgressEnabled()) {
    flushExtensions();
    wifiOff();
    {
      RenderLock lock(*this);
      state = SYNC_COMPLETE;
    }
    requestUpdate(true);
    return;
  }

  {
    RenderLock lock(*this);
    statusMessage = tr(STR_CALC_HASH);
  }
  requestUpdate(true);
  performSync();
}

void BookOrbitSyncActivity::performSync() {
  documentHash = documentHashForConfig();
  if (documentHash.empty()) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_HASH_FAILED);
    }
    requestUpdate(true);
    return;
  }

  {
    RenderLock lock(*this);
    statusMessage = tr(STR_FETCH_PROGRESS);
  }
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    wifiOff();
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_SYNC_FAILED_MSG);
    }
    requestUpdate(true);
    return;
  }

  const auto result = BookOrbitClient::getProgress(documentHash, remoteProgress);

  if (!ensureLocalProgressLoaded()) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_SYNC_REOPTIMIZE_REQUIRED);
    }
    requestUpdate(true);
    return;
  }

  if (result == BookOrbitClient::NOT_FOUND) {
    RenderLock lock(*this);
    state = NO_REMOTE_PROGRESS;
    hasRemoteProgress = false;
    requestUpdate(true);
    return;
  }
  if (result != BookOrbitClient::OK) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = syncErrorMessage(result);
    }
    requestUpdate(true);
    return;
  }

  hasRemoteProgress = true;
  const PositionCoordinateSpace space = BOOKORBIT.getMatchMethod() == BookOrbitMatchMethod::FILENAME
                                            ? PositionCoordinateSpace::SourceDocument
                                            : PositionCoordinateSpace::CurrentDocument;
  const KOReaderPosition koPos = {remoteProgress.progress, remoteProgress.percentage};
  remotePosition = ProgressMapper::toCrossPoint(epub, koPos, currentSpineIndex, totalPagesInSpine, space);
  if (!remotePosition.valid) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_SYNC_REOPTIMIZE_REQUIRED);
    }
    requestUpdate(true);
    return;
  }

  // Refine the page from the paragraph LUT, like the KOReader flow does.
  if (remotePosition.hasParagraphIndex) {
    Section tempSection(epub, remotePosition.spineIndex, renderer);
    const auto paragraphPage = tempSection.getPageForParagraphIndex(remotePosition.paragraphIndex);
    if (paragraphPage.has_value()) {
      remotePosition.pageNumber = std::max(remotePosition.pageNumber, static_cast<int>(*paragraphPage));
    }
  }

  BookOrbitCapture::ensureLoaded();
  pendingHighlightCount = BOOKORBIT.syncHighlightsEnabled() ? PENDING_HIGHLIGHTS.size() : 0;
  pendingBookmarkCount = BOOKORBIT.syncBookmarksEnabled() ? PENDING_BOOKMARKS.size() : 0;

  {
    RenderLock lock(*this);
    state = SHOWING_RESULT;
    selectedOption = localProgress.percentage > remoteProgress.percentage ? 1 : 0;
  }
  requestUpdate(true);
}

void BookOrbitSyncActivity::performUpload() {
  {
    RenderLock lock(*this);
    state = UPLOADING;
    statusMessage = tr(STR_UPLOAD_PROGRESS);
  }
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    wifiOff();
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_SYNC_FAILED_MSG);
    }
    requestUpdate(true);
    return;
  }

  if (documentHash.empty()) documentHash = documentHashForConfig();
  epub.reset();

  BookOrbitProgress progress;
  progress.document = documentHash;
  progress.progress = localProgress.xpath;
  progress.percentage = localProgress.percentage;
  progress.device = SETTINGS.getEffectiveDeviceName();

  const auto result = BookOrbitClient::updateProgress(progress);
  if (result != BookOrbitClient::OK) {
    wifiOff();
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = syncErrorMessage(result);
    }
    requestUpdate();
    return;
  }

  // Progress uploaded; flush the extensions while WiFi is still up, then drop the radio.
  flushExtensions();
  wifiOff();
  {
    RenderLock lock(*this);
    state = UPLOAD_COMPLETE;
  }
  requestUpdate(true);
}

void BookOrbitSyncActivity::onEnter() {
  Activity::onEnter();

  if (restartBeforeNetwork) {
    silentRestartToNetwork(NetworkBootTarget::BOOKORBIT_SYNC);
    return;
  }

  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  lockInitialConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);

  BOOKORBIT.ensureLoaded();
  if (!BOOKORBIT.isConfigured()) {
    state = NOT_CONFIGURED;
    requestUpdate();
    return;
  }

  sdFontSystem.releaseLoadedFont(renderer);
  wifiActivated = true;

  if (hasActiveStationWifiConnection()) {
    onWifiSelectionComplete(true);
    return;
  }
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, true, true),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void BookOrbitSyncActivity::onExit() {
  Activity::onExit();
  if (wifiActivated) {
    wifiOff();
    silentRestartToReader();
  }
}

void BookOrbitSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();
  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  const Rect header{screen.x, screen.y + metrics.topPadding, screen.width,
                    TouchHeaderBackButton::height(metrics, mappedInput)};
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, header, tr(STR_BOOKORBIT_SYNC), true);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_BOOKORBIT_SYNC));
  }

  int top = screen.y + screen.height / 2 - 40;

  if (state == NOT_CONFIGURED) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_BOOKORBIT_NOT_CONFIGURED), true,
                              EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, tr(STR_BOOKORBIT_SETUP_HINT), true,
                              EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    renderer.displayBuffer();
    return;
  }

  if (state == SYNCING || state == UPLOADING) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, statusMessage.c_str(), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (state == SHOWING_RESULT) {
    top = screen.y + metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput) + metrics.verticalSpacing;
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_PROGRESS_FOUND), true, EpdFontFamily::BOLD);

    const int remoteTocIndex = epub ? epub->getTocIndexForSpineIndex(remotePosition.spineIndex) : -1;
    const std::string remoteChapter =
        (remoteTocIndex >= 0) ? epub->getTocItem(remoteTocIndex).title
                              : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(remotePosition.spineIndex + 1));
    const std::string localChapter =
        !localChapterName.empty() ? localChapterName
                                  : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(currentSpineIndex + 1));

    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 40, tr(STR_REMOTE_LABEL), true);
    char buf[128];
    snprintf(buf, sizeof(buf), "  %s", remoteChapter.c_str());
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 65, buf);
    snprintf(buf, sizeof(buf), tr(STR_PAGE_OVERALL_FORMAT), remotePosition.pageNumber + 1,
             remoteProgress.percentage * 100);
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 90, buf);
    if (!remoteProgress.device.empty()) {
      snprintf(buf, sizeof(buf), tr(STR_DEVICE_FROM_FORMAT), remoteProgress.device.c_str());
      renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 115, buf);
    }

    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 150, tr(STR_LOCAL_LABEL), true);
    snprintf(buf, sizeof(buf), "  %s", localChapter.c_str());
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 175, buf);
    snprintf(buf, sizeof(buf), tr(STR_PAGE_TOTAL_OVERALL_FORMAT), currentPage + 1, totalPagesInSpine,
             localProgress.percentage * 100);
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + RESULT_LOCAL_PAGE_Y_OFFSET, buf);

    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

    // What the next sync will push, shown only for categories that have buffered items. Drawn in
    // the small font so two lines stay clear of the action buttons even in landscape.
    const int smallLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
    int infoBottom = top + RESULT_LOCAL_PAGE_Y_OFFSET;
    if (pendingHighlightCount > 0) {
      infoBottom += smallLineHeight + 8;
      snprintf(buf, sizeof(buf), tr(STR_PENDING_HIGHLIGHTS_FORMAT), static_cast<int>(pendingHighlightCount));
      renderer.drawText(SMALL_FONT_ID, screen.x + metrics.contentSidePadding, infoBottom, buf);
    }
    if (pendingBookmarkCount > 0) {
      infoBottom += smallLineHeight + 6;
      snprintf(buf, sizeof(buf), tr(STR_PENDING_BOOKMARKS_FORMAT), static_cast<int>(pendingBookmarkCount));
      renderer.drawText(SMALL_FONT_ID, screen.x + metrics.contentSidePadding, infoBottom, buf);
    }

    const int buttonY = infoBottom + lineHeight + 12;
    const int buttonH = 40;
    const int buttonW = screen.width - metrics.contentSidePadding * 2;
    const char* actionLabels[] = {tr(STR_APPLY_REMOTE), tr(STR_UPLOAD_LOCAL)};
    for (int option = 0; option < 2; ++option) {
      const int by = buttonY + option * (buttonH + 8);
      const bool selected = selectedOption == option;
      if (selected) renderer.fillRect(screen.x + metrics.contentSidePadding, by, buttonW, buttonH);
      renderer.drawRect(screen.x + metrics.contentSidePadding, by, buttonW, buttonH, true);
      const int textX = screen.x + metrics.contentSidePadding +
                        (buttonW - renderer.getTextWidth(UI_10_FONT_ID, actionLabels[option])) / 2;
      renderer.drawText(UI_10_FONT_ID, textX, by + (buttonH - lineHeight) / 2, actionLabels[option], !selected);
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    renderer.displayBuffer();
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_NO_REMOTE_MSG), true, EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, tr(STR_UPLOAD_PROMPT));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_UPLOAD), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    renderer.displayBuffer();
    return;
  }

  if (state == UPLOAD_COMPLETE || state == SYNC_COMPLETE) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_UPLOAD_SUCCESS), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DONE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    renderer.displayBuffer();
    return;
  }

  if (state == SYNC_FAILED) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_SYNC_FAILED_MSG), true, EpdFontFamily::BOLD);
    const int messageWidth = screen.width - metrics.contentSidePadding * 2;
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const auto messageLines = renderer.wrappedText(UI_10_FONT_ID, statusMessage.c_str(), messageWidth, 3);
    int messageY = top + 40;
    for (const auto& line : messageLines) {
      UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, messageY, line.c_str());
      messageY += lineHeight + 4;
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    renderer.displayBuffer();
    return;
  }
}

void BookOrbitSyncActivity::loop() {
  if (consumeInitialConfirmRelease()) return;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const Rect header{screen.x, screen.y + metrics.topPadding, screen.width,
                    TouchHeaderBackButton::height(metrics, mappedInput)};
  if (TouchHeaderBackButton::wasTapped(mappedInput, header)) {
    returnToReader();
    return;
  }

  if (state == NOT_CONFIGURED || state == SYNC_FAILED || state == UPLOAD_COMPLETE || state == SYNC_COMPLETE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      returnToReader();
    }
    return;
  }

  if (state == SHOWING_RESULT) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left) ||
        mappedInput.wasReleased(MappedInputManager::Button::Down) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      selectedOption = (selectedOption + 1) % 2;
      requestUpdate();
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (selectedOption == 0) {
        saveProgressAndReturn(remotePosition);
      } else {
        performUpload();
      }
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) returnToReader();
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) performUpload();
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) returnToReader();
    return;
  }
}
