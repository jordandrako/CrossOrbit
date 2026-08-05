#include "BookOrbitSettingsActivity.h"

#include <BookOrbitConfig.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
constexpr int MENU_ITEMS = 8;
const StrId menuNames[MENU_ITEMS] = {
    StrId::STR_BOOKORBIT_SERVER_URL,     StrId::STR_BOOKORBIT_USERNAME,       StrId::STR_BOOKORBIT_PASSWORD,
    StrId::STR_BOOKORBIT_MATCH_METHOD,   StrId::STR_BOOKORBIT_SYNC_PROGRESS,  StrId::STR_BOOKORBIT_SYNC_SESSIONS,
    StrId::STR_BOOKORBIT_SYNC_HIGHLIGHTS, StrId::STR_BOOKORBIT_SYNC_BOOKMARKS};
constexpr fui::ActionId ACTION_ROW = 1;
}  // namespace

BookOrbitSettingsActivity::BookOrbitSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("BookOrbitSettings", renderer, mappedInput),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void BookOrbitSettingsActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<BookOrbitSettingsActivity*>(user);
  if (event.value < 0 || event.value >= MENU_ITEMS) return;
  self->selectedIndex = static_cast<size_t>(event.value);
  // Activation opens a keyboard/sub-activity or repaints a new value; a
  // lingering flash would gray an unrelated row.
  self->app.clearTapFlash();
  self->handleSelection();
}

void BookOrbitSettingsActivity::onEnter() {
  Activity::onEnter();

  BOOKORBIT.ensureLoaded();
  selectedIndex = 0;
  uiReady = false;
  visibleRows = 1;
  topIndex = 0;
  app.setTheme(uiThemeTokens(uiTarget));
  app.on(ACTION_ROW, &BookOrbitSettingsActivity::onRowEvent, this);
  app.setScreen(&BookOrbitSettingsActivity::listScreen, this);
  requestUpdate();
}

void BookOrbitSettingsActivity::onExit() { Activity::onExit(); }

void BookOrbitSettingsActivity::loop() {
  auto activateSelected = [this] { handleSelection(); };

  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    finishAfterBackPress();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finishAfterBackPress();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelected();
    return;
  }

  // Touch goes through the FreeInkApp: render() registered the row hit rects;
  // route the snapshot and let onRowEvent dispatch.
  if (uiReady) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchReleased) {
      const auto event = app.route(snap);
      if (app.invalidated()) requestUpdate();
      if (event) return;  // dispatched to onRowEvent
    }
  }

  // Handle navigation
  buttonNavigator.onNext([this] {
    selectedIndex = (selectedIndex + 1) % MENU_ITEMS;
    topIndex = followListSelection(static_cast<int>(selectedIndex), topIndex, visibleRows, MENU_ITEMS);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = (selectedIndex + MENU_ITEMS - 1) % MENU_ITEMS;
    topIndex = followListSelection(static_cast<int>(selectedIndex), topIndex, visibleRows, MENU_ITEMS);
    requestUpdate();
  });
}

void BookOrbitSettingsActivity::handleSelection() {
  if (selectedIndex == 0) {
    // Server URL - prefill with https:// if empty to save typing
    const std::string currentUrl = BOOKORBIT.getServerUrl();
    const std::string prefillUrl = currentUrl.empty() ? "https://" : currentUrl;
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_BOOKORBIT_SERVER_URL),
                                                                   prefillUrl, 128, InputType::Url),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               const std::string urlToSave =
                                   (kb.text == "https://" || kb.text == "http://") ? "" : kb.text;
                               BOOKORBIT.setServerUrl(urlToSave);
                               BOOKORBIT.saveToFile();
                             }
                           });
  } else if (selectedIndex == 1) {
    // Username
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_BOOKORBIT_USERNAME),
                                                                   BOOKORBIT.getUsername(), 64, InputType::Text),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               BOOKORBIT.setCredentials(kb.text, BOOKORBIT.getPassword());
                               BOOKORBIT.saveToFile();
                             }
                           });
  } else if (selectedIndex == 2) {
    // Password
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_BOOKORBIT_PASSWORD),
                                                BOOKORBIT.getPassword(), 64, InputType::Password),
        [this](const ActivityResult& result) {
          if (!result.isCancelled) {
            const auto& kb = std::get<KeyboardResult>(result.data);
            BOOKORBIT.setCredentials(BOOKORBIT.getUsername(), kb.text);
            BOOKORBIT.saveToFile();
          }
        });
  } else if (selectedIndex == 3) {
    // Document Matching - toggle between Filename and Binary
    const auto current = BOOKORBIT.getMatchMethod();
    const auto newMethod =
        (current == BookOrbitMatchMethod::FILENAME) ? BookOrbitMatchMethod::BINARY : BookOrbitMatchMethod::FILENAME;
    BOOKORBIT.setMatchMethod(newMethod);
    BOOKORBIT.saveToFile();
    requestUpdate();
  } else if (selectedIndex == 4) {
    BOOKORBIT.setSyncProgress(!BOOKORBIT.syncProgressEnabled());
    BOOKORBIT.saveToFile();
    requestUpdate();
  } else if (selectedIndex == 5) {
    BOOKORBIT.setSyncSessions(!BOOKORBIT.syncSessionsEnabled());
    BOOKORBIT.saveToFile();
    requestUpdate();
  } else if (selectedIndex == 6) {
    BOOKORBIT.setSyncHighlights(!BOOKORBIT.syncHighlightsEnabled());
    BOOKORBIT.saveToFile();
    requestUpdate();
  } else if (selectedIndex == 7) {
    BOOKORBIT.setSyncBookmarks(!BOOKORBIT.syncBookmarksEnabled());
    BOOKORBIT.saveToFile();
    requestUpdate();
  }
}

void BookOrbitSettingsActivity::listScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<BookOrbitSettingsActivity*>(user)->buildListScreen(screen);
}

void BookOrbitSettingsActivity::buildListScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput)), 0,
                  static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // Per-render owned value strings; items point into them for the draw only.
  std::vector<std::string> values(MENU_ITEMS);
  for (int i = 0; i < MENU_ITEMS; i++) {
    if (i == 0) {
      values[i] = BOOKORBIT.getServerUrl();
      if (values[i].empty()) values[i] = tr(STR_NOT_SET);
    } else if (i == 1) {
      const auto& username = BOOKORBIT.getUsername();
      values[i] = username.empty() ? tr(STR_NOT_SET) : username;
    } else if (i == 2) {
      values[i] = BOOKORBIT.getPassword().empty() ? tr(STR_NOT_SET) : "******";
    } else if (i == 3) {
      values[i] = BOOKORBIT.getMatchMethod() == BookOrbitMatchMethod::FILENAME ? tr(STR_FILENAME) : tr(STR_BINARY);
    } else if (i == 4) {
      values[i] = BOOKORBIT.syncProgressEnabled() ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    } else if (i == 5) {
      values[i] = BOOKORBIT.syncSessionsEnabled() ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    } else if (i == 6) {
      values[i] = BOOKORBIT.syncHighlightsEnabled() ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    } else {
      values[i] = BOOKORBIT.syncBookmarksEnabled() ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    }
  }

  std::vector<fui::ListItem> items;
  items.reserve(MENU_ITEMS);
  for (int i = 0; i < MENU_ITEMS; i++) {
    fui::ListItem item;
    item.label = I18N.get(menuNames[i]);
    if (!values[i].empty()) item.value = values[i].c_str();
    item.actionValue = static_cast<int16_t>(i);
    items.push_back(item);
  }

  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.selectedIndex = static_cast<int16_t>(selectedIndex);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the value and the row edge
  const auto rows = configureUiList(props, screen.theme(), screen.body());
  visibleRows = rows > 0 ? rows : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, MENU_ITEMS);  // clamp to range
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);
}

void BookOrbitSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  (void)pageWidth;

  // Header via GUI.drawHeader (already FreeInkUI-themed) for the battery
  // indicator; the rest of the screen renders through the app.
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, header, tr(STR_BOOKORBIT_SYNC), false);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_BOOKORBIT_SYNC));
  }

  uiReady = false;
  app.render();
  uiReady = true;

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
