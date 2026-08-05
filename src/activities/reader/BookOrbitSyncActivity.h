#pragma once
#include <BookOrbitClient.h>
#include <BookOrbitConfig.h>
#include <Epub.h>
#include <ProgressMapper.h>

#include <memory>
#include <optional>
#include <string>

#include "activities/Activity.h"

/**
 * One-stop BookOrbit sync: reading progress (interactive apply/upload) plus the enabled
 * extensions (sessions, highlights, bookmarks). Fully self-contained (BookOrbitClient +
 * BookOrbitConfig); the only reused upstream pieces are the store-agnostic ProgressMapper,
 * WifiSelectionActivity, and the network-boot plumbing.
 *
 * Flow: reboot into lightweight network mode -> WiFi -> NTP -> (if progress enabled) fetch
 * remote progress and let the user apply/upload -> flush the enabled extension buffers.
 */
class BookOrbitSyncActivity final : public Activity {
 public:
  static constexpr const char* NAME = "BookOrbitSync";

  // Reader handoff: reboots into network mode, then boot relaunches with the network ctor.
  explicit BookOrbitSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity(NAME, renderer, mappedInput), restartBeforeNetwork(true) {}

  // Network-boot ctor.
  BookOrbitSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string epubPath)
      : Activity(NAME, renderer, mappedInput), epubPath(std::move(epubPath)), localProgressDeferred(true) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == SYNCING || state == UPLOADING; }
  bool isReaderActivity() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }

 private:
  enum State {
    WIFI_SELECTION,
    SYNCING,
    SHOWING_RESULT,
    UPLOADING,
    UPLOAD_COMPLETE,
    SYNC_COMPLETE,
    NO_REMOTE_PROGRESS,
    SYNC_FAILED,
    NOT_CONFIGURED,
  };

  std::shared_ptr<Epub> epub;
  std::string epubPath;
  std::string localChapterName;
  int currentSpineIndex = 0;
  int currentPage = 0;
  int totalPagesInSpine = 1;

  State state = WIFI_SELECTION;
  std::string statusMessage;
  std::string documentHash;

  bool hasRemoteProgress = false;
  BookOrbitProgress remoteProgress;
  CrossPointPosition remotePosition;
  KOReaderPosition localProgress;
  bool localProgressDeferred = false;
  bool restartBeforeNetwork = false;

  int selectedOption = 0;  // 0 = apply remote, 1 = upload local
  bool wifiActivated = false;
  bool lockInitialConfirmRelease = false;

  void onWifiSelectionComplete(bool success);
  void performSync();
  void performUpload();
  void flushExtensions();  // push the enabled buffers (sessions/highlights/bookmarks)
  bool consumeInitialConfirmRelease();
  void ensureEpubLoaded();
  bool ensureLocalProgressLoaded();
  std::string documentHashForConfig() const;
  void saveProgressAndReturn(const CrossPointPosition& position);
  void returnToReader();
  std::string syncErrorMessage(BookOrbitClient::Error error) const;
};
