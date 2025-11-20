// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "mainwindow.h"
#include "aboutdialog.h"
#include "achievementlogindialog.h"
#include "autoupdaterwindow.h"
#include "coverdownloadwindow.h"
#include "debuggerwindow.h"
#include "displaywidget.h"
#include "gamelistsettingswidget.h"
#include "gamelistwidget.h"
#include "interfacesettingswidget.h"
#include "isobrowserwindow.h"
#include "logwindow.h"
#include "memorycardeditorwindow.h"
#include "memoryeditorwindow.h"
#include "memoryscannerwindow.h"
#include "qthost.h"
#include "qtutils.h"
#include "selectdiscdialog.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"

#include "core/cheats.h"
#include "core/game_list.h"
#include "core/host.h"
#include "core/memory_card.h"
#include "core/settings.h"
#include "core/system.h"

#include "util/cd_image.h"
#include "util/gpu_device.h"
#include "util/platform_misc.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/string_util.h"

#include <QtCore/QDebug>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMimeData>
#include <QtCore/QUrl>
#include <QtGui/QActionGroup>
#include <QtGui/QCursor>
#include <QtGui/QShortcut>
#include <QtGui/QWindowStateChangeEvent>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QStyleFactory>
#include <cmath>

#include "moc_mainwindow.cpp"

#ifdef _WIN32
#include "common/windows_headers.h"
#include <Dbt.h>
#include <VersionHelpers.h>
#endif

LOG_CHANNEL(Host);

static constexpr std::array<std::pair<Qt::ToolBarArea, const char*>, 4> s_toolbar_areas = {{
  {Qt::TopToolBarArea, QT_TRANSLATE_NOOP("MainWindow", "Top")},
  {Qt::BottomToolBarArea, QT_TRANSLATE_NOOP("MainWindow", "Bottom")},
  {Qt::LeftToolBarArea, QT_TRANSLATE_NOOP("MainWindow", "Left")},
  {Qt::RightToolBarArea, QT_TRANSLATE_NOOP("MainWindow", "Right")},
}};

static constexpr std::pair<const char*, QAction * Ui::MainWindow::*> s_toolbar_actions[] = {
  {"StartFile", &Ui::MainWindow::actionStartFile},
  {"StartBIOS", &Ui::MainWindow::actionStartBios},
  {"StartDisc", &Ui::MainWindow::actionStartDisc},
  {"FullscreenUI", &Ui::MainWindow::actionStartFullscreenUI2},
  {nullptr, nullptr},
  {"PowerOff", &Ui::MainWindow::actionCloseGame},
  {"PowerOffWithoutSaving", &Ui::MainWindow::actionCloseGameWithoutSaving},
  {"Reset", &Ui::MainWindow::actionResetGame},
  {"Pause", &Ui::MainWindow::actionPause},
  {"ChangeDisc", &Ui::MainWindow::actionChangeDisc},
  {"Cheats", &Ui::MainWindow::actionCheatsToolbar},
  {"Screenshot", &Ui::MainWindow::actionScreenshot},
  {"MediaCapture", &Ui::MainWindow::actionMediaCapture},
  {nullptr, nullptr},
  {"LoadState", &Ui::MainWindow::actionLoadState},
  {"SaveState", &Ui::MainWindow::actionSaveState},
  {nullptr, nullptr},
  {"MemoryScanner", &Ui::MainWindow::actionMemoryScanner},
  {"MemoryEditor", &Ui::MainWindow::actionMemoryEditor},
  {nullptr, nullptr},
  {"Fullscreen", &Ui::MainWindow::actionFullscreen},
  {"Settings", &Ui::MainWindow::actionSettings2},
  {"ControllerSettings", &Ui::MainWindow::actionControllerSettings},
  {"ControllerPresets", &Ui::MainWindow::actionControllerProfiles},
};

static constexpr const char* DEFAULT_TOOLBAR_ACTIONS =
  "StartFile,StartBIOS,FullscreenUI,PowerOff,Reset,Pause,ChangeDisc,Screenshot,LoadState,SaveState,Fullscreen,Settings,"
  "ControllerSettings";

static constexpr char DISC_IMAGE_FILTER[] = QT_TRANSLATE_NOOP(
  "MainWindow",
  "All File Types (*.bin *.img *.iso *.cue *.chd *.cpe *.ecm *.mds *.pbp *.elf *.exe *.psexe *.ps-exe *.psx *.psf "
  "*.minipsf *.m3u *.psxgpu);;Single-Track Raw Images (*.bin *.img *.iso);;Cue Sheets (*.cue);;MAME CHD Images "
  "(*.chd);;Error Code Modeler Images (*.ecm);;Media Descriptor Sidecar Images (*.mds);;PlayStation EBOOTs (*.pbp "
  "*.PBP);;PlayStation Executables (*.cpe *.elf *.exe *.psexe *.ps-exe, *.psx);;Portable Sound Format Files (*.psf "
  "*.minipsf);;Playlists (*.m3u);;PSX GPU Dumps (*.psxgpu *.psxgpu.zst *.psxgpu.xz)");

static constexpr char IMAGE_FILTER[] = QT_TRANSLATE_NOOP("MainWindow", "Images (*.jpg *.jpeg *.png *.webp)");

MainWindow* g_main_window = nullptr;

// UI thread VM validity.
static bool s_disable_window_rounded_corners = false;
static bool s_system_starting = false;
static bool s_system_valid = false;
static bool s_system_paused = false;
static bool s_achievements_hardcore_mode = false;
static bool s_fullscreen_ui_started = false;
static std::atomic_uint32_t s_system_locked{false};
static QString s_current_game_title;
static QString s_current_game_serial;
static QString s_current_game_path;
static QIcon s_current_game_icon;
static std::optional<std::time_t> s_undo_state_timestamp;

bool QtHost::IsSystemPaused()
{
  return s_system_paused;
}

bool QtHost::IsSystemValid()
{
  return s_system_valid;
}

bool QtHost::IsSystemValidOrStarting()
{
  return (s_system_starting || s_system_valid);
}

bool QtHost::IsFullscreenUIStarted()
{
  return s_fullscreen_ui_started;
}

const QString& QtHost::GetCurrentGameTitle()
{
  return s_current_game_title;
}

const QString& QtHost::GetCurrentGameSerial()
{
  return s_current_game_serial;
}

const QString& QtHost::GetCurrentGamePath()
{
  return s_current_game_path;
}

MainWindow::MainWindow() : QMainWindow(nullptr)
{
  Assert(!g_main_window);
  g_main_window = this;
  initialize();
}

MainWindow::~MainWindow()
{
  Assert(!m_display_widget);
  Assert(!m_debugger_window);
  cancelGameListRefresh();

  // we compare here, since recreate destroys the window later
  if (g_main_window == this)
    g_main_window = nullptr;

#ifdef _WIN32
  unregisterForDeviceNotifications();
#endif
}

void MainWindow::initialize()
{
  m_ui.setupUi(this);
  setupAdditionalUi();
  updateToolbarActions();
  updateToolbarIconStyle();
  updateToolbarArea();
  updateEmulationActions(false, false, false);
  updateDisplayRelatedActions(false, false);
  updateWindowTitle();
  connectSignals();

  switchToGameListView();

  QtUtils::RestoreWindowGeometry("MainWindow", this);

#ifdef _WIN32
  registerForDeviceNotifications();
#endif
}

QMenu* MainWindow::createPopupMenu()
{
  return nullptr;
}

void MainWindow::reportError(const QString& title, const QString& message)
{
  QtUtils::MessageBoxCritical(this, title, message);
}

bool MainWindow::confirmMessage(const QString& title, const QString& message)
{
  SystemLock lock(pauseAndLockSystem());

  return (QtUtils::MessageBoxQuestion(this, title, message) == QMessageBox::Yes);
}

void MainWindow::onStatusMessage(const QString& message)
{
  m_ui.statusBar->showMessage(message);
}

void MainWindow::registerForDeviceNotifications()
{
#ifdef _WIN32
  // We use these notifications to detect when a controller is connected or disconnected.
  DEV_BROADCAST_DEVICEINTERFACE_W filter = {
    sizeof(DEV_BROADCAST_DEVICEINTERFACE_W), DBT_DEVTYP_DEVICEINTERFACE, 0u, {}, {}};
  m_device_notification_handle = RegisterDeviceNotificationW(
    (HANDLE)winId(), &filter, DEVICE_NOTIFY_WINDOW_HANDLE | DEVICE_NOTIFY_ALL_INTERFACE_CLASSES);
#endif
}

void MainWindow::unregisterForDeviceNotifications()
{
#ifdef _WIN32
  if (!m_device_notification_handle)
    return;

  UnregisterDeviceNotification(static_cast<HDEVNOTIFY>(m_device_notification_handle));
  m_device_notification_handle = nullptr;
#endif
}

#ifdef _WIN32

bool MainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
  static constexpr const char win_type[] = "windows_generic_MSG";
  if (eventType == QByteArray(win_type, sizeof(win_type) - 1))
  {
    const MSG* msg = static_cast<const MSG*>(message);
    if (msg->message == WM_DEVICECHANGE && msg->wParam == DBT_DEVNODES_CHANGED)
    {
      g_emu_thread->reloadInputDevices();
      *result = 1;
      return true;
    }
  }

  return QMainWindow::nativeEvent(eventType, message, result);
}

#endif

std::optional<WindowInfo> MainWindow::acquireRenderWindow(RenderAPI render_api, bool fullscreen,
                                                          bool exclusive_fullscreen, bool surfaceless, Error* error)
{
  const bool render_to_main =
    QtHost::CanRenderToMainWindow() && !fullscreen && (s_system_locked.load(std::memory_order_relaxed) == 0);

  DEV_LOG("acquireRenderWindow() fullscreen={} exclusive_fullscreen={}, render_to_main={}, surfaceless={} ", fullscreen,
          exclusive_fullscreen, render_to_main, surfaceless);

  QWidget* container =
    m_display_container ? static_cast<QWidget*>(m_display_container) : static_cast<QWidget*>(m_display_widget);
  const bool is_fullscreen = isRenderingFullscreen();
  const bool is_rendering_to_main = isRenderingToMain();
  const bool changing_surfaceless = (!m_display_widget != surfaceless);

  // Always update exclusive fullscreen state, it controls main window visibility
  m_exclusive_fullscreen_requested = !surfaceless && exclusive_fullscreen;

  // Skip recreating the surface if we're just transitioning between fullscreen and windowed with render-to-main off.
  // .. except on Wayland, where everything tends to break if you don't recreate.
  // Container can also be null if we're messing with settings while surfaceless.
  const bool has_container = (m_display_container != nullptr);
  const bool needs_container = DisplayContainer::isNeeded(fullscreen, render_to_main);
  if (container && !is_rendering_to_main && !render_to_main && !has_container && !needs_container &&
      !changing_surfaceless)
  {
    DEV_LOG("Toggling to {} without recreating surface", (fullscreen ? "fullscreen" : "windowed"));
    m_exclusive_fullscreen_requested = exclusive_fullscreen;

    // ensure it's resizable when changing size, we'll fix it up later in updateWindowState()
    QtUtils::SetWindowResizeable(container, true);

    // since we don't destroy the display widget, we need to save it here
    if (!is_fullscreen && !is_rendering_to_main)
      saveDisplayWindowGeometryToConfig();

    if (fullscreen)
    {
      container->showFullScreen();
    }
    else
    {
      container->showNormal();
      restoreDisplayWindowGeometryFromConfig();
    }

    updateDisplayRelatedActions(!surfaceless, fullscreen);
    updateDisplayWidgetCursor();
    m_display_widget->setFocus();
    updateWindowState();

    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    return m_display_widget->getWindowInfo(render_api, error);
  }

  destroyDisplayWidget(surfaceless);

  std::optional<WindowInfo> wi;
  if (!surfaceless)
  {
    createDisplayWidget(fullscreen, render_to_main);

    wi = m_display_widget->getWindowInfo(render_api, error);
    if (!wi.has_value())
    {
      QtUtils::MessageBoxCritical(this, tr("Error"), tr("Failed to get window info from widget"));
      destroyDisplayWidget(true);
      return std::nullopt;
    }

    g_emu_thread->connectDisplaySignals(m_display_widget);
  }
  else
  {
    wi = WindowInfo();
  }

  updateWindowTitle();
  updateWindowState();

  return wi;
}

bool MainWindow::wantsDisplayWidget() const
{
  // big picture or system created
  return (QtHost::IsSystemValidOrStarting() || s_fullscreen_ui_started);
}

bool MainWindow::hasDisplayWidget() const
{
  return m_display_widget != nullptr;
}

void MainWindow::createDisplayWidget(bool fullscreen, bool render_to_main)
{
  // If we're rendering to main and were hidden (e.g. coming back from fullscreen),
  // make sure we're visible before trying to add ourselves. Otherwise Wayland breaks.
  if (!fullscreen && render_to_main && !isVisible())
  {
    setVisible(true);
    QGuiApplication::sync();
  }

  QWidget* container;
  if (DisplayContainer::isNeeded(fullscreen, render_to_main))
  {
    m_display_container = new DisplayContainer();
    m_display_widget = new DisplayWidget(m_display_container);
    m_display_container->setDisplayWidget(m_display_widget);
    container = m_display_container;
  }
  else
  {
    m_display_widget = new DisplayWidget((!fullscreen && render_to_main) ? m_ui.mainContainer : nullptr);
    container = m_display_widget;
  }

  if (fullscreen || !render_to_main)
  {
    container->setWindowTitle(windowTitle());
    container->setWindowIcon(windowIcon());
  }

  if (fullscreen)
  {
    if (isVisible() && QtHost::CanRenderToMainWindow())
      container->move(pos());
    else
      restoreDisplayWindowGeometryFromConfig();

    container->showFullScreen();
  }
  else if (!render_to_main)
  {
    restoreDisplayWindowGeometryFromConfig();
    container->showNormal();

    if (s_disable_window_rounded_corners)
      PlatformMisc::SetWindowRoundedCornerState(reinterpret_cast<void*>(container->winId()), false);
  }
  else
  {
    AssertMsg(m_ui.mainContainer->count() == 1, "Has no display widget");
    m_ui.mainContainer->addWidget(container);
    m_ui.mainContainer->setCurrentIndex(1);
    m_ui.actionViewSystemDisplay->setChecked(true);
  }

  updateDisplayRelatedActions(true, fullscreen);
  updateShortcutActions(false);
  updateDisplayWidgetCursor();

  // We need the surface visible.
  QGuiApplication::sync();

  if (!render_to_main)
    QtUtils::ShowOrRaiseWindow(QtUtils::GetRootWidget(m_display_widget));
  m_display_widget->setFocus();
}

void MainWindow::exitFullscreen(bool wait_for_completion)
{
  if (!isRenderingFullscreen())
    return;

  g_emu_thread->setFullscreen(false);
  if (wait_for_completion)
    QtUtils::ProcessEventsWithSleep(QEventLoop::ExcludeUserInputEvents, [this]() { return isRenderingFullscreen(); });
}

void MainWindow::displayResizeRequested(qint32 width, qint32 height)
{
  if (!m_display_widget)
    return;

  // unapply the pixel scaling factor for hidpi
  const qreal dpr = devicePixelRatioF();
  width = static_cast<qint32>(std::max(static_cast<int>(std::lround(static_cast<qreal>(width) / dpr)), 1));
  height = static_cast<qint32>(std::max(static_cast<int>(std::lround(static_cast<qreal>(height) / dpr)), 1));

  if (m_display_container || !m_display_widget->parent())
  {
    // no parent - rendering to separate window. easy.
    QtUtils::ResizePotentiallyFixedSizeWindow(getDisplayContainer(), width, height);
    return;
  }

  // we are rendering to the main window. we have to add in the extra height from the toolbar/status bar.
  const s32 extra_height = this->height() - m_display_widget->height();
  QtUtils::ResizePotentiallyFixedSizeWindow(this, width, height + extra_height);
}

void MainWindow::releaseRenderWindow()
{
  // Now we can safely destroy the display window.
  destroyDisplayWidget(true);
  updateWindowTitle();
  updateWindowState();
}

void MainWindow::destroyDisplayWidget(bool show_game_list)
{
  if (m_display_widget)
  {
    if (!isRenderingFullscreen() && !isRenderingToMain())
      saveDisplayWindowGeometryToConfig();

    if (m_display_container)
      m_display_container->removeDisplayWidget();

    if (isRenderingToMain())
    {
      AssertMsg(m_ui.mainContainer->indexOf(m_display_widget) == 1, "Display widget in stack");
      m_ui.mainContainer->removeWidget(m_display_widget);
      if (show_game_list)
      {
        m_ui.mainContainer->setCurrentIndex(0);
        if (m_game_list_widget->isShowingGameGrid())
          m_ui.actionViewGameGrid->setChecked(true);
        else
          m_ui.actionViewGameList->setChecked(true);
      }
    }

    if (m_display_widget)
    {
      m_display_widget->destroy();
      m_display_widget = nullptr;
    }

    if (m_display_container)
    {
      m_display_container->deleteLater();
      m_display_container = nullptr;
    }
  }

  m_exclusive_fullscreen_requested = false;

  updateDisplayRelatedActions(false, false);
  updateShortcutActions(false);
}

void MainWindow::updateDisplayWidgetCursor()
{
  // may be temporarily surfaceless
  if (!m_display_widget)
    return;

  m_display_widget->updateRelativeMode(s_system_valid && !s_system_paused && m_relative_mouse_mode);
  m_display_widget->updateCursor(s_system_valid && !s_system_paused && shouldHideMouseCursor());
}

void MainWindow::updateDisplayRelatedActions(bool has_surface, bool fullscreen)
{
  // rendering to main, or switched to gamelist/grid
  m_ui.actionViewSystemDisplay->setEnabled(wantsDisplayWidget() && QtHost::CanRenderToMainWindow() &&
                                           !s_system_starting);
  m_ui.menuWindowSize->setEnabled(s_system_valid && !s_system_starting && has_surface && !fullscreen);
  m_ui.actionFullscreen->setEnabled(has_surface && !s_system_starting);
  m_ui.actionFullscreen->setChecked(fullscreen);

  updateGameListRelatedActions();
}

void MainWindow::updateGameListRelatedActions()
{
  const bool running = !isShowingGameList();
  const bool disable = (s_system_starting || (running && isRenderingToMain()));

  const bool game_grid = m_game_list_widget->isShowingGameGrid();
  const bool game_list = m_game_list_widget->isShowingGameList();
  const bool has_background = m_game_list_widget->hasBackground();

  m_ui.menuSortBy->setDisabled(disable);
  m_ui.actionMergeDiscSets->setDisabled(disable);
  m_ui.actionShowLocalizedTitles->setDisabled(disable);
  m_ui.actionShowGameIcons->setDisabled(disable || !game_list);
  m_ui.actionAnimateGameIcons->setDisabled(disable || !game_list || !m_ui.actionShowGameIcons->isChecked());
  m_ui.actionGridViewShowTitles->setDisabled(disable || !game_grid);
  m_ui.actionViewZoomIn->setDisabled(disable);
  m_ui.actionViewZoomOut->setDisabled(disable);
  m_ui.actionGridViewRefreshCovers->setDisabled(disable || !game_grid);
  m_ui.actionChangeGameListBackground->setDisabled(disable);
  m_ui.actionClearGameListBackground->setDisabled(disable || !has_background);
}

void MainWindow::focusDisplayWidget()
{
  if (!m_display_widget || centralWidget() != m_display_widget)
    return;

  m_display_widget->setFocus();
}

QWidget* MainWindow::getDisplayContainer() const
{
  return (m_display_container ? static_cast<QWidget*>(m_display_container) : static_cast<QWidget*>(m_display_widget));
}

void MainWindow::onMouseModeRequested(bool relative_mode, bool hide_cursor)
{
  m_relative_mouse_mode = relative_mode;
  m_hide_mouse_cursor = hide_cursor;
  updateDisplayWidgetCursor();
}

void MainWindow::onSystemStarting()
{
  s_system_starting = true;
  s_system_valid = false;
  s_system_paused = false;

  switchToEmulationView();
  updateEmulationActions(true, false, s_achievements_hardcore_mode);
  updateDisplayRelatedActions(m_display_widget != nullptr, isRenderingFullscreen());
}

void MainWindow::onSystemStarted()
{
  m_was_disc_change_request = false;
  s_system_starting = false;
  s_system_valid = true;

  updateEmulationActions(false, true, s_achievements_hardcore_mode);
  updateDisplayRelatedActions(m_display_widget != nullptr, isRenderingFullscreen());
  updateWindowTitle();
  updateStatusBarWidgetVisibility();
  updateDisplayWidgetCursor();
}

void MainWindow::onSystemPaused()
{
  // update UI
  {
    QSignalBlocker sb(m_ui.actionPause);
    m_ui.actionPause->setChecked(true);
  }

  s_system_paused = true;
  updateStatusBarWidgetVisibility();
  m_ui.statusBar->showMessage(tr("Paused"));
  updateDisplayWidgetCursor();
}

void MainWindow::onSystemResumed()
{
  // update UI
  {
    QSignalBlocker sb(m_ui.actionPause);
    m_ui.actionPause->setChecked(false);
  }

  s_system_paused = false;
  m_was_disc_change_request = false;
  m_ui.statusBar->clearMessage();
  updateStatusBarWidgetVisibility();
  updateDisplayWidgetCursor();
  if (m_display_widget)
    m_display_widget->setFocus();
}

void MainWindow::onSystemStopping()
{
  // update UI
  {
    QSignalBlocker sb(m_ui.actionPause);
    m_ui.actionPause->setChecked(false);
  }

  s_system_starting = false;
  s_system_valid = false;
  s_system_paused = false;
  s_undo_state_timestamp.reset();

  updateEmulationActions(false, false, s_achievements_hardcore_mode);
  updateDisplayRelatedActions(m_display_widget != nullptr, isRenderingFullscreen());
  updateStatusBarWidgetVisibility();
}

void MainWindow::onSystemDestroyed()
{
  Assert(!QtHost::IsSystemValidOrStarting());

  // If we're closing or in batch mode, quit the whole application now.
  if (m_is_closing || QtHost::InBatchMode())
  {
    destroySubWindows();
    quit();
    return;
  }

  if (m_display_widget)
    updateDisplayWidgetCursor();
  else
    switchToGameListView();
}

void MainWindow::onSystemGameChanged(const QString& path, const QString& game_serial, const QString& game_title)
{
  s_current_game_path = path;
  s_current_game_title = game_title;
  s_current_game_serial = game_serial;
  s_current_game_icon = getIconForGame(path);

  updateWindowTitle();
}

void MainWindow::onSystemUndoStateAvailabilityChanged(bool available, quint64 timestamp)
{
  if (!available)
    s_undo_state_timestamp.reset();
  else
    s_undo_state_timestamp = timestamp;
}

void MainWindow::onMediaCaptureStarted()
{
  QSignalBlocker sb(m_ui.actionMediaCapture);
  m_ui.actionMediaCapture->setChecked(true);
}

void MainWindow::onMediaCaptureStopped()
{
  QSignalBlocker sb(m_ui.actionMediaCapture);
  m_ui.actionMediaCapture->setChecked(false);
}

void MainWindow::onApplicationStateChanged(Qt::ApplicationState state)
{
  if (!s_system_valid)
    return;

  const bool focus_loss = (state != Qt::ApplicationActive);
  if (focus_loss)
  {
    if (g_settings.pause_on_focus_loss && !m_was_paused_by_focus_loss && !s_system_paused)
    {
      g_emu_thread->setSystemPaused(true);
      m_was_paused_by_focus_loss = true;
    }

    // Clear the state of all keyboard binds.
    // That way, if we had a key held down, and lost focus, the bind won't be stuck enabled because we never
    // got the key release message, because it happened in another window which "stole" the event.
    g_emu_thread->clearInputBindStateFromSource(InputManager::MakeHostKeyboardKey(0));
  }
  else
  {
    if (m_was_paused_by_focus_loss)
    {
      if (s_system_paused)
        g_emu_thread->setSystemPaused(false);
      m_was_paused_by_focus_loss = false;
    }
  }
}

void MainWindow::onStartFileActionTriggered()
{
  QString filename = QDir::toNativeSeparators(
    QFileDialog::getOpenFileName(this, tr("Select Disc Image"), QString(), tr(DISC_IMAGE_FILTER), nullptr));
  if (filename.isEmpty())
    return;

  startFileOrChangeDisc(filename);
}

std::string MainWindow::getDeviceDiscPath(const QString& title)
{
  std::string ret;

  auto devices = CDImage::GetDeviceList();
  if (devices.empty())
  {
    QtUtils::MessageBoxCritical(
      this, title,
      tr("Could not find any CD-ROM devices. Please ensure you have a CD-ROM drive connected and "
         "sufficient permissions to access it."));
    return ret;
  }

  // if there's only one, select it automatically
  if (devices.size() == 1)
  {
    ret = std::move(devices.front().first);
    return ret;
  }

  QStringList input_options;
  for (const auto& [path, name] : devices)
    input_options.append(tr("%1 (%2)").arg(QString::fromStdString(name)).arg(QString::fromStdString(path)));

  QInputDialog input_dialog(this);
  input_dialog.setWindowTitle(title);
  input_dialog.setLabelText(tr("Select disc drive:"));
  input_dialog.setInputMode(QInputDialog::TextInput);
  input_dialog.setOptions(QInputDialog::UseListViewForComboBoxItems);
  input_dialog.setComboBoxEditable(false);
  input_dialog.setComboBoxItems(std::move(input_options));
  if (input_dialog.exec() == QDialog::Rejected)
    return ret;

  const qsizetype selected_index = input_dialog.comboBoxItems().indexOf(input_dialog.textValue());
  if (selected_index < 0 || static_cast<u32>(selected_index) >= devices.size())
    return ret;

  ret = std::move(devices[selected_index].first);
  return ret;
}

void MainWindow::quit()
{
  // Make sure VM is gone. It really should be if we're here.
  if (QtHost::IsSystemValidOrStarting())
  {
    g_emu_thread->shutdownSystem(false, false);
    QtUtils::ProcessEventsWithSleep(QEventLoop::ExcludeUserInputEvents, &QtHost::IsSystemValidOrStarting);
  }

  // Big picture might still be active.
  if (s_fullscreen_ui_started)
    g_emu_thread->stopFullscreenUI();

  // Ensure subwindows are removed before quitting. That way the log window cancelling
  // the close event won't cancel the quit process.
  destroySubWindows();
  QGuiApplication::quit();
}

void MainWindow::recreate()
{
  std::optional<QPoint> settings_window_pos;
  int settings_window_row = 0;
  std::optional<QPoint> controller_settings_window_pos;
  ControllerSettingsWindow::Category controller_settings_window_row =
    ControllerSettingsWindow::Category::GlobalSettings;
  if (m_settings_window && m_settings_window->isVisible())
  {
    settings_window_pos = m_settings_window->pos();
    settings_window_row = m_settings_window->getCategoryRow();
  }
  if (m_controller_settings_window && m_controller_settings_window->isVisible())
  {
    controller_settings_window_pos = m_controller_settings_window->pos();
    controller_settings_window_row = m_controller_settings_window->getCurrentCategory();
  }

  // Remove subwindows before switching to surfaceless, because otherwise e.g. the debugger can cause funkyness.
  destroySubWindows();

  const bool was_display_created = wantsDisplayWidget();
  const bool was_fullscreen = (was_display_created && g_emu_thread->isFullscreen());
  if (was_display_created)
  {
    // Ensure the main window is visible, otherwise last-window-closed terminates the application.
    if (!isVisible())
      show();

    g_emu_thread->setSurfaceless(true);
    QtUtils::ProcessEventsWithSleep(QEventLoop::ExcludeUserInputEvents,
                                    [this]() { return (m_display_widget || !g_emu_thread->isSurfaceless()); });
  }

  // We need to close input sources, because e.g. DInput uses our window handle.
  g_emu_thread->closeInputSources();

  g_main_window = nullptr;
  close();

  MainWindow* new_main_window = new MainWindow();
  DebugAssert(g_main_window == new_main_window);
  new_main_window->show();
  deleteLater();

  // Recreate log window as well. Then make sure we're still on top.
  LogWindow::updateSettings();

  // Qt+XCB will ignore the raise request of the settings window if we raise the main window.
  // So skip that if we're going to be re-opening the settings window.
  if (!settings_window_pos.has_value())
    QtUtils::ShowOrRaiseWindow(new_main_window);

  // Reload the sources we just closed.
  g_emu_thread->reloadInputSources();

  if (was_display_created)
  {
    g_emu_thread->setSurfaceless(false);
    if (was_fullscreen)
      g_emu_thread->setFullscreen(true);
    g_main_window->updateEmulationActions(false, s_system_valid, s_achievements_hardcore_mode);
    g_main_window->onFullscreenUIStartedOrStopped(s_fullscreen_ui_started);
  }

  if (controller_settings_window_pos.has_value())
  {
    ControllerSettingsWindow* dlg = g_main_window->getControllerSettingsWindow();
    dlg->move(controller_settings_window_pos.value());
    dlg->setCategory(controller_settings_window_row);
    dlg->show();
  }
  if (settings_window_pos.has_value())
  {
    SettingsWindow* dlg = g_main_window->getSettingsWindow();
    dlg->move(settings_window_pos.value());
    dlg->setCategoryRow(settings_window_row);
    QtUtils::ShowOrRaiseWindow(dlg);
  }

  notifyRAIntegrationOfWindowChange();
}

void MainWindow::destroySubWindows()
{
  QtUtils::CloseAndDeleteWindow(m_cover_download_window);
  QtUtils::CloseAndDeleteWindow(m_memory_editor_window);
  QtUtils::CloseAndDeleteWindow(m_memory_scanner_window);
  QtUtils::CloseAndDeleteWindow(m_debugger_window);
  QtUtils::CloseAndDeleteWindow(m_memory_card_editor_window);
  QtUtils::CloseAndDeleteWindow(m_auto_updater_dialog);
  QtUtils::CloseAndDeleteWindow(m_controller_settings_window);
  QtUtils::CloseAndDeleteWindow(m_input_profile_editor_window);
  QtUtils::CloseAndDeleteWindow(m_settings_window);

  SettingsWindow::closeGamePropertiesDialogs();

  LogWindow::destroy();
}

void MainWindow::populateGameListContextMenu(const GameList::Entry* entry, QWidget* parent_window, QMenu* menu)
{
  QAction* resume_action = nullptr;
  QMenu* load_state_menu = nullptr;

  if (!entry->IsDiscSet())
  {
    resume_action = menu->addAction(tr("Resume"));
    resume_action->setEnabled(false);

    load_state_menu = menu->addMenu(tr("Load State"));
    load_state_menu->setEnabled(false);
    QtUtils::StylePopupMenu(load_state_menu);

    if (!entry->serial.empty())
    {
      std::vector<SaveStateInfo> available_states(System::GetAvailableSaveStates(entry->serial));
      const QString timestamp_format = QtHost::GetApplicationLocale().dateTimeFormat(QLocale::ShortFormat);
      for (SaveStateInfo& ssi : available_states)
      {
        if (ssi.global)
          continue;

        const s32 slot = ssi.slot;
        const QString timestamp_str =
          QtHost::FormatNumber(Host::NumberFormatType::ShortDateTime, static_cast<s64>(ssi.timestamp));

        QAction* action;
        if (slot < 0)
        {
          resume_action->setText(tr("Resume (%1)").arg(timestamp_str));
          action = resume_action;
        }
        else
        {
          load_state_menu->setEnabled(true);
          action = load_state_menu->addAction(tr("Game Save %1 (%2)").arg(slot).arg(timestamp_str));
        }

        action->setDisabled(s_achievements_hardcore_mode);
        connect(action, &QAction::triggered, [this, game_path = entry->path, path = std::move(ssi.path)]() mutable {
          startFile(std::move(game_path), std::move(path), std::nullopt);
        });
      }
    }
  }

  QAction* open_memory_cards_action = menu->addAction(tr("Edit Memory Cards..."));
  connect(open_memory_cards_action, &QAction::triggered, [path = entry->path]() {
    const auto lock = GameList::GetLock();
    const GameList::Entry* entry = GameList::GetEntryForPath(path);
    if (!entry)
      return;

    QString paths[2];
    for (u32 i = 0; i < 2; i++)
      paths[i] = QString::fromStdString(System::GetGameMemoryCardPath(entry->serial, entry->path, i));

    g_main_window->openMemoryCardEditor(paths[0], paths[1]);
  });

  if (!entry->IsDiscSet())
  {
    const bool has_any_states = resume_action->isEnabled() || load_state_menu->isEnabled();
    QAction* delete_save_states_action = menu->addAction(tr("Delete Save States"));
    delete_save_states_action->setEnabled(has_any_states);
    if (has_any_states)
    {
      connect(delete_save_states_action, &QAction::triggered, [parent_window, serial = entry->serial] {
        if (QtUtils::MessageBoxWarning(
              parent_window, tr("Confirm Save State Deletion"),
              tr("Are you sure you want to delete all save states for %1?\n\nThe saves will not be recoverable.")
                .arg(QString::fromStdString(serial)),
              QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
        {
          return;
        }

        System::DeleteSaveStates(serial, true);
      });
    }
  }
}

void MainWindow::populateLoadStateMenu(std::string_view game_serial, QMenu* menu)
{
  auto add_slot = [this, menu](const QString& title, const QString& empty_title, const std::string_view& serial,
                               s32 slot) {
    std::optional<SaveStateInfo> ssi = System::GetSaveStateInfo(serial, slot);

    const QString menu_title = ssi.has_value() ?
                                 title.arg(slot).arg(QtHost::FormatNumber(Host::NumberFormatType::ShortDateTime,
                                                                          static_cast<s64>(ssi->timestamp))) :
                                 empty_title.arg(slot);

    QAction* load_action = menu->addAction(menu_title);
    load_action->setEnabled(ssi.has_value());
    if (ssi.has_value())
    {
      const QString path(QString::fromStdString(ssi->path));
      connect(load_action, &QAction::triggered, this, [path]() { g_emu_thread->loadState(path); });
    }
  };

  menu->clear();

  connect(menu->addAction(tr("Load From File...")), &QAction::triggered, []() {
    const QString path = QDir::toNativeSeparators(
      QFileDialog::getOpenFileName(g_main_window, tr("Select Save State File"), QString(), tr("Save States (*.sav)")));
    if (path.isEmpty())
      return;

    g_emu_thread->loadState(path);
  });
  QAction* load_from_state =
    menu->addAction(s_undo_state_timestamp.has_value() ?
                      tr("Undo Load State (%1)")
                        .arg(QtHost::FormatNumber(Host::NumberFormatType::ShortDateTime,
                                                  static_cast<s64>(s_undo_state_timestamp.value()))) :
                      tr("Undo Load State"));
  load_from_state->setEnabled(s_undo_state_timestamp.has_value());
  connect(load_from_state, &QAction::triggered, g_emu_thread, &EmuThread::undoLoadState);
  menu->addSeparator();

  if (!game_serial.empty())
  {
    for (u32 slot = 1; slot <= System::PER_GAME_SAVE_STATE_SLOTS; slot++)
      add_slot(tr("Game Save %1 (%2)"), tr("Game Save %1 (Empty)"), game_serial, static_cast<s32>(slot));

    menu->addSeparator();
  }

  std::string_view empty_serial;
  for (u32 slot = 1; slot <= System::GLOBAL_SAVE_STATE_SLOTS; slot++)
    add_slot(tr("Global Save %1 (%2)"), tr("Global Save %1 (Empty)"), empty_serial, static_cast<s32>(slot));
}

void MainWindow::populateSaveStateMenu(std::string_view game_serial, QMenu* menu)
{
  auto add_slot = [menu](const QString& title, const QString& empty_title, const std::string_view& serial, s32 slot) {
    std::optional<SaveStateInfo> ssi = System::GetSaveStateInfo(serial, slot);

    const QString menu_title = ssi.has_value() ?
                                 title.arg(slot).arg(QtHost::FormatNumber(Host::NumberFormatType::ShortDateTime,
                                                                          static_cast<s64>(ssi->timestamp))) :
                                 empty_title.arg(slot);

    QAction* save_action = menu->addAction(menu_title);
    connect(save_action, &QAction::triggered,
            [global = serial.empty(), slot]() { g_emu_thread->saveState(global, slot); });
  };

  menu->clear();

  connect(menu->addAction(tr("Save To File...")), &QAction::triggered, []() {
    if (!System::IsValid())
      return;

    const QString path = QDir::toNativeSeparators(
      QFileDialog::getSaveFileName(g_main_window, tr("Select Save State File"), QString(), tr("Save States (*.sav)")));
    if (path.isEmpty())
      return;

    g_emu_thread->saveState(QDir::toNativeSeparators(path));
  });
  menu->addSeparator();

  if (!game_serial.empty())
  {
    for (u32 slot = 1; slot <= System::PER_GAME_SAVE_STATE_SLOTS; slot++)
      add_slot(tr("Game Save %1 (%2)"), tr("Game Save %1 (Empty)"), game_serial, static_cast<s32>(slot));

    menu->addSeparator();
  }

  std::string_view empty_serial;
  for (u32 slot = 1; slot <= System::GLOBAL_SAVE_STATE_SLOTS; slot++)
    add_slot(tr("Global Save %1 (%2)"), tr("Global Save %1 (Empty)"), empty_serial, static_cast<s32>(slot));
}

void MainWindow::onCheatsMenuAboutToShow()
{
  m_ui.menuCheats->clear();
  connect(m_ui.menuCheats->addAction(tr("Select Cheats...")), &QAction::triggered, this,
          [this]() { openGamePropertiesForCurrentGame("Cheats"); });
  m_ui.menuCheats->addSeparator();

  Host::RunOnCPUThread([menu = m_ui.menuCheats]() {
    if (!System::IsValid())
      return;

    QStringList names;
    Cheats::EnumerateManualCodes([&names](const std::string& name) {
      names.append(QString::fromStdString(name));
      return true;
    });
    if (Cheats::AreCheatsEnabled() && names.empty())
      return;

    Host::RunOnUIThread([menu, names = std::move(names)]() {
      if (names.empty())
      {
        QAction* action = menu->addAction(tr("Cheats are not enabled."));
        action->setEnabled(false);
        return;
      }

      QMenu* const apply_submenu = menu->addMenu(tr("&Apply Cheat"));
      QtUtils::StylePopupMenu(apply_submenu);
      for (const QString& name : names)
      {
        apply_submenu->addAction(name, [name]() {
          Host::RunOnCPUThread([name = name.toStdString()]() {
            if (System::IsValid())
              Cheats::ApplyManualCode(name);
          });
        });
      }
    });
  });
}

const GameList::Entry* MainWindow::resolveDiscSetEntry(const GameList::Entry* entry,
                                                       std::unique_lock<std::recursive_mutex>& lock)
{
  if (!entry || !entry->IsDiscSet())
    return entry;

  // disc set... need to figure out the disc we want
  SelectDiscDialog dlg(entry->GetDiscSetEntry(), m_game_list_widget->getModel()->getShowLocalizedTitles(), this);

  lock.unlock();
  const int res = dlg.exec();
  lock.lock();

  return res == QDialog::Accepted ? GameList::GetEntryForPath(dlg.getSelectedDiscPath()) : nullptr;
}

std::shared_ptr<SystemBootParameters> MainWindow::getSystemBootParameters(std::string file)
{
  std::shared_ptr<SystemBootParameters> ret = std::make_shared<SystemBootParameters>(std::move(file));
  ret->start_media_capture = m_ui.actionMediaCapture->isChecked();
  return ret;
}

bool MainWindow::openResumeStateDialog(const std::string& path, const std::string& serial)
{
  System::FlushSaveStates();

  std::string save_state_path = System::GetGameSaveStatePath(serial, -1);
  if (save_state_path.empty() || !FileSystem::FileExists(save_state_path.c_str()))
    return false;

  std::optional<ExtendedSaveStateInfo> ssi = System::GetExtendedSaveStateInfo(save_state_path.c_str());
  if (!ssi.has_value())
    return false;

  QDialog* const dlg = new QDialog(this);
  dlg->setWindowTitle(tr("Load Resume State"));
  dlg->setAttribute(Qt::WA_DeleteOnClose);

  QVBoxLayout* const main_layout = new QVBoxLayout(dlg);
  main_layout->setSpacing(10);

  QHBoxLayout* const heading_layout = new QHBoxLayout();
  heading_layout->setSpacing(10);
  QLabel* const heading_icon = new QLabel(dlg);
  heading_icon->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxQuestion).pixmap(32, 32));
  heading_layout->addWidget(heading_icon, 0, Qt::AlignTop);

  QLabel* const heading_text = new QLabel(dlg);
  heading_text->setText(
    tr("<strong>Resume Game</strong><br>Do you want to load this state, or start from a fresh boot?"));
  heading_layout->addWidget(heading_text, 1, Qt::AlignTop);
  main_layout->addLayout(heading_layout);

  QLabel* const timestamp_label = new QLabel(dlg);
  timestamp_label->setText(
    tr("Save was created on %1.")
      .arg(QtHost::FormatNumber(Host::NumberFormatType::LongDateTime, static_cast<s64>(ssi->timestamp))));
  timestamp_label->setWordWrap(true);
  timestamp_label->setAlignment(Qt::AlignHCenter);
  main_layout->addWidget(timestamp_label);

  if (ssi->screenshot.IsValid())
  {
    std::optional<Image> image;
    if (ssi->screenshot.GetFormat() != ImageFormat::RGBA8)
      image = ssi->screenshot.ConvertToRGBA8();
    else
      image = std::move(ssi->screenshot);

    if (image.has_value())
    {
      // Convert the screenshot to a QImage, then to a QPixmap, then scale it down to something reasonable.
      const QPixmap pixmap =
        QPixmap::fromImage(QImage(static_cast<uchar*>(image->GetPixels()), static_cast<int>(image->GetWidth()),
                                  static_cast<int>(image->GetHeight()), QImage::Format_RGBA8888))
          .scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation);
      QLabel* const image_label = new QLabel(dlg);
      image_label->setPixmap(pixmap);
      image_label->setAlignment(Qt::AlignHCenter);
      main_layout->addWidget(image_label);
    }
  }

  QDialogButtonBox* const bbox = new QDialogButtonBox(Qt::Horizontal, dlg);

  QPushButton* const load = bbox->addButton(tr("Load State"), QDialogButtonBox::AcceptRole);
  QPushButton* const boot = bbox->addButton(tr("Fresh Boot"), QDialogButtonBox::RejectRole);
  QPushButton* const delboot = bbox->addButton(tr("Delete And Boot"), QDialogButtonBox::RejectRole);
  QPushButton* const cancel = bbox->addButton(QDialogButtonBox::Cancel);
  load->setDefault(true);

  connect(load, &QPushButton::clicked, [this, dlg, path, save_state_path]() mutable {
    startFile(std::move(path), std::move(save_state_path), std::nullopt);
    dlg->accept();
  });
  connect(boot, &QPushButton::clicked, [this, dlg, path]() mutable {
    startFile(std::move(path), std::nullopt, std::nullopt);
    dlg->accept();
  });
  connect(delboot, &QPushButton::clicked, [this, dlg, path, save_state_path]() mutable {
    if (!FileSystem::DeleteFile(save_state_path.c_str()))
    {
      QtUtils::MessageBoxCritical(
        this, tr("Error"), tr("Failed to delete save state file '%1'.").arg(QString::fromStdString(save_state_path)));
    }
    startFile(std::move(path), std::nullopt, std::nullopt);
    dlg->accept();
  });
  connect(cancel, &QPushButton::clicked, dlg, &QDialog::reject);

  main_layout->addWidget(bbox);

  dlg->open();
  return true;
}

void MainWindow::startFile(std::string path, std::optional<std::string> save_path, std::optional<bool> fast_boot)
{
  std::shared_ptr<SystemBootParameters> params = getSystemBootParameters(std::move(path));
  params->override_fast_boot = fast_boot;
  if (save_path.has_value())
    params->save_state = std::move(save_path.value());

  g_emu_thread->bootSystem(std::move(params));
}

void MainWindow::startFileOrChangeDisc(const QString& qpath)
{
  if (s_system_valid)
  {
    // this is a disc change
    promptForDiscChange(qpath);
    return;
  }

  std::string path = qpath.toStdString();
  {
    const auto lock = GameList::GetLock();
    const GameList::Entry* entry = GameList::GetEntryForPath(path);
    if (entry && !entry->serial.empty() && openResumeStateDialog(entry->path, entry->serial))
      return;
  }

  startFile(std::move(path), std::nullopt, std::nullopt);
}

void MainWindow::promptForDiscChange(const QString& path)
{
  if (m_was_disc_change_request || System::IsGPUDumpPath(path.toStdString()))
  {
    switchToEmulationView();
    g_emu_thread->changeDisc(path, false, true);
    return;
  }

  SystemLock lock(pauseAndLockSystem());

  QMessageBox* const mb =
    QtUtils::NewMessageBox(QMessageBox::Question, tr("Confirm Disc Change"),
                           tr("Do you want to swap discs or boot the new image (via system reset)?"),
                           QMessageBox::NoButton, QMessageBox::NoButton, lock.getDialogParent());

  /*const QAbstractButton* const swap_button = */ mb->addButton(tr("Swap Disc"), QMessageBox::YesRole);
  const QAbstractButton* const reset_button = mb->addButton(tr("Reset"), QMessageBox::NoRole);
  const QAbstractButton* const cancel_button = mb->addButton(tr("Cancel"), QMessageBox::RejectRole);

  connect(mb, &QMessageBox::finished, this, [this, mb, reset_button, cancel_button, path, lock = std::move(lock)]() {
    const QAbstractButton* const clicked_button = mb->clickedButton();
    if (!clicked_button || clicked_button == cancel_button)
      return;

    const bool reset_system = (clicked_button == reset_button);
    switchToEmulationView();

    g_emu_thread->changeDisc(path, reset_system, true);
  });

  mb->open();
}

void MainWindow::onStartDiscActionTriggered()
{
  std::string path(getDeviceDiscPath(tr("Start Disc")));
  if (path.empty())
    return;

  g_emu_thread->bootSystem(getSystemBootParameters(std::move(path)));
}

void MainWindow::onStartBIOSActionTriggered()
{
  g_emu_thread->bootSystem(getSystemBootParameters(std::string()));
}

void MainWindow::onChangeDiscFromFileActionTriggered()
{
  QString filename = QDir::toNativeSeparators(
    QFileDialog::getOpenFileName(this, tr("Select Disc Image"), QString(), tr(DISC_IMAGE_FILTER), nullptr));
  if (filename.isEmpty())
    return;

  g_emu_thread->changeDisc(filename, false, true);
}

void MainWindow::onChangeDiscFromGameListActionTriggered()
{
  m_was_disc_change_request = true;
  switchToGameListView();
}

void MainWindow::onChangeDiscFromDeviceActionTriggered()
{
  std::string path(getDeviceDiscPath(tr("Change Disc")));
  if (path.empty())
    return;

  g_emu_thread->changeDisc(QString::fromStdString(path), false, true);
}

void MainWindow::onChangeDiscMenuAboutToShow()
{
  // clean up temporary menu items, they're owned by the QMenu so they get deleted here. the main QActions do not
  m_ui.menuChangeDisc->clear();

  m_ui.menuChangeDisc->addAction(m_ui.actionChangeDiscFromFile);
  m_ui.menuChangeDisc->addAction(m_ui.actionChangeDiscFromDevice);
  m_ui.menuChangeDisc->addAction(m_ui.actionChangeDiscFromGameList);
  m_ui.menuChangeDisc->addAction(m_ui.actionRemoveDisc);
  m_ui.menuChangeDisc->addSeparator();

  if (!s_system_valid)
    return;

  // NOTE: This is terrible and a race condition. But nobody should be using m3u files anyway.
  if (System::HasMediaSubImages())
  {
    const u32 count = System::GetMediaSubImageCount();
    const u32 current = System::GetMediaSubImageIndex();
    for (u32 i = 0; i < count; i++)
    {
      QAction* action = m_ui.menuChangeDisc->addAction(QString::fromStdString(System::GetMediaSubImageTitle(i)));
      action->setCheckable(true);
      action->setChecked(i == current);
      connect(action, &QAction::triggered, [i]() { g_emu_thread->changeDiscFromPlaylist(i); });
    }
  }
  else if (const GameDatabase::Entry* entry = System::GetGameDatabaseEntry(); entry && entry->disc_set)
  {
    auto lock = GameList::GetLock();
    for (const auto& [title, glentry] :
         GameList::GetEntriesInDiscSet(entry->disc_set, m_game_list_widget->getModel()->getShowLocalizedTitles()))
    {
      QAction* action = m_ui.menuChangeDisc->addAction(QtUtils::StringViewToQString(title));
      QString path = QString::fromStdString(glentry->path);
      action->setCheckable(true);
      action->setChecked(path == s_current_game_path);
      connect(action, &QAction::triggered, [path = std::move(path)]() { g_emu_thread->changeDisc(path, false, true); });
    }
  }
}

void MainWindow::onLoadStateMenuAboutToShow()
{
  populateLoadStateMenu(s_current_game_serial.toStdString(), m_ui.menuLoadState);
}

void MainWindow::onSaveStateMenuAboutToShow()
{
  populateSaveStateMenu(s_current_game_serial.toStdString(), m_ui.menuSaveState);
}

void MainWindow::onStartFullscreenUITriggered()
{
  if (m_display_widget)
    g_emu_thread->stopFullscreenUI();
  else
    g_emu_thread->startFullscreenUI();
}

void MainWindow::onFullscreenUIStartedOrStopped(bool running)
{
  s_fullscreen_ui_started = running;
  m_ui.actionStartFullscreenUI->setText(running ? tr("Stop Big Picture Mode") : tr("Start Big Picture Mode"));
  m_ui.actionStartFullscreenUI2->setText(running ? tr("Exit Big Picture") : tr("Big Picture"));
}

void MainWindow::onCloseGameActionTriggered()
{
  requestShutdown(true, true, g_settings.save_state_on_exit, true, true, false, false);
}

void MainWindow::onCloseGameWithoutSavingActionTriggered()
{
  requestShutdown(false, false, false, true, false, false, false);
}

void MainWindow::onResetGameActionTriggered()
{
  g_emu_thread->resetSystem(true);
}

void MainWindow::onPauseActionToggled(bool checked)
{
  if (s_system_paused == checked)
    return;

  if (checked && s_achievements_hardcore_mode)
  {
    // Need to check restrictions.
    Host::RunOnCPUThread([]() {
      if (System::CanPauseSystem(true))
      {
        g_emu_thread->setSystemPaused(true);
      }
      else
      {
        // Restore action state.
        Host::RunOnUIThread([]() {
          QSignalBlocker sb(g_main_window->m_ui.actionPause);
          g_main_window->m_ui.actionPause->setChecked(false);
        });
      }
    });
  }
  else
  {
    g_emu_thread->setSystemPaused(checked);
  }
}

void MainWindow::onRemoveDiscActionTriggered()
{
  g_emu_thread->changeDisc(QString(), false, true);
}

void MainWindow::onScanForNewGamesTriggered()
{
  refreshGameList(false);
}

void MainWindow::onViewToolbarActionToggled(bool checked)
{
  Host::SetBaseBoolSettingValue("UI", "ShowToolbar", checked);
  Host::CommitBaseSettingChanges();
  m_ui.toolBar->setVisible(checked);
  updateToolbarIconStyle();
}

void MainWindow::onViewToolbarLockActionToggled(bool checked)
{
  Host::SetBaseBoolSettingValue("UI", "LockToolbar", checked);
  Host::CommitBaseSettingChanges();
  m_ui.toolBar->setMovable(!checked);

  // ensure synced
  const QSignalBlocker sb(m_ui.actionViewLockToolbar);
  m_ui.actionViewLockToolbar->setChecked(checked);
}

void MainWindow::onViewToolbarSmallIconsActionToggled(bool checked)
{
  Host::SetBaseBoolSettingValue("UI", "ToolbarSmallIcons", checked);
  Host::CommitBaseSettingChanges();
  updateToolbarIconStyle();

  // ensure synced
  const QSignalBlocker sb(m_ui.actionViewSmallToolbarIcons);
  m_ui.actionViewSmallToolbarIcons->setChecked(checked);
}

void MainWindow::onViewToolbarLabelsActionToggled(bool checked)
{
  Host::SetBaseBoolSettingValue("UI", "ToolbarLabels", checked);
  Host::CommitBaseSettingChanges();
  updateToolbarIconStyle();

  // ensure synced
  const QSignalBlocker sb(m_ui.actionViewToolbarLabels);
  m_ui.actionViewToolbarLabels->setChecked(checked);
}

void MainWindow::onViewToolbarLabelsBesideIconsActionToggled(bool checked)
{
  Host::SetBaseBoolSettingValue("UI", "ToolbarLabelsBesideIcons", checked);
  Host::CommitBaseSettingChanges();
  updateToolbarIconStyle();

  // ensure synced
  const QSignalBlocker sb(m_ui.actionViewToolbarLabelsBesideIcons);
  m_ui.actionViewToolbarLabelsBesideIcons->setChecked(checked);
}

void MainWindow::onViewStatusBarActionToggled(bool checked)
{
  Host::SetBaseBoolSettingValue("UI", "ShowStatusBar", checked);
  Host::CommitBaseSettingChanges();
  m_ui.statusBar->setVisible(checked);
}

void MainWindow::onViewGameListActionTriggered()
{
  m_game_list_widget->showGameList();
  updateGameListRelatedActions();
  switchToGameListView();
}

void MainWindow::onViewGameGridActionTriggered()
{
  m_game_list_widget->showGameGrid();
  updateGameListRelatedActions();
  switchToGameListView();
}

void MainWindow::onViewSystemDisplayTriggered()
{
  switchToEmulationView();
}

void MainWindow::onViewZoomInActionTriggered()
{
  if (!isShowingGameList())
    return;

  m_game_list_widget->zoomIn();
}

void MainWindow::onViewZoomOutActionTriggered()
{
  if (!isShowingGameList())
    return;

  m_game_list_widget->zoomOut();
}

void MainWindow::onGitHubRepositoryActionTriggered()
{
  QtUtils::OpenURL(this, "https://github.com/stenzek/duckstation/");
}

void MainWindow::onDiscordServerActionTriggered()
{
  QtUtils::OpenURL(this, "https://www.duckstation.org/discord.html");
}

void MainWindow::onAboutActionTriggered()
{
  QDialog* const about = new AboutDialog(this);
  about->setAttribute(Qt::WA_DeleteOnClose);
  about->open();
}

void MainWindow::onGameListRefreshProgress(const QString& status, int current, int total)
{
  m_ui.statusBar->showMessage(status);
  setProgressBar(current, total);
}

void MainWindow::onGameListRefreshComplete()
{
  m_ui.statusBar->clearMessage();
  clearProgressBar();
}

void MainWindow::onGameListSelectionChanged()
{
  auto lock = GameList::GetLock();
  const GameList::Entry* entry = m_game_list_widget->getSelectedEntry();
  if (!entry)
    return;

  m_ui.statusBar->showMessage(QString::fromStdString(entry->path));
}

void MainWindow::onGameListEntryActivated()
{
  auto lock = GameList::GetLock();
  const GameList::Entry* entry = resolveDiscSetEntry(m_game_list_widget->getSelectedEntry(), lock);
  if (!entry)
    return;

  if (s_system_valid)
  {
    // change disc on double click
    if (!entry->IsDisc())
    {
      lock.unlock();
      QtUtils::MessageBoxCritical(this, tr("Error"), tr("You must select a disc to change discs."));
      return;
    }

    promptForDiscChange(QString::fromStdString(entry->path));
    return;
  }

  if (!entry->serial.empty() && openResumeStateDialog(entry->path, entry->serial))
    return;

  // only resume if the option is enabled, and we have one for this game
  startFile(entry->path, std::nullopt, std::nullopt);
}

void MainWindow::onGameListEntryContextMenuRequested(const QPoint& point)
{
  QMenu* const menu = QtUtils::NewPopupMenu(this);

  const auto lock = GameList::GetLock();
  const GameList::Entry* entry = m_game_list_widget->getSelectedEntry();

  if (entry)
  {
    const QString qpath = QString::fromStdString(entry->path); // QString is CoW

    if (!entry->IsDiscSet())
    {
      menu->addAction(tr("Properties..."), [qpath]() {
        const auto lock = GameList::GetLock();
        const GameList::Entry* entry = GameList::GetEntryForPath(qpath.toStdString());
        if (!entry || !g_main_window)
          return;

        SettingsWindow::openGamePropertiesDialog(entry);
      });

      menu->addAction(tr("Open Containing Directory..."), [this, qpath]() {
        const QFileInfo fi(qpath);
        QtUtils::OpenURL(this, QUrl::fromLocalFile(fi.absolutePath()));
      });

      if (entry->IsDisc())
      {
        menu->addAction(tr("Browse ISO..."), [this, qpath]() {
          ISOBrowserWindow* ib = ISOBrowserWindow::createAndOpenFile(this, qpath);
          if (ib)
          {
            ib->setAttribute(Qt::WA_DeleteOnClose);
            ib->show();
          }
        });
      }

      menu->addAction(tr("Set Cover Image..."), [this, qpath]() {
        const auto lock = GameList::GetLock();
        const GameList::Entry* entry = GameList::GetEntryForPath(qpath.toStdString());
        if (entry)
          setGameListEntryCoverImage(entry);
      });

      menu->addSeparator();

      if (!s_system_valid)
      {
        populateGameListContextMenu(entry, this, menu);
        menu->addSeparator();

        menu->addAction(tr("Default Boot"), [this, qpath]() mutable {
          g_emu_thread->bootSystem(getSystemBootParameters(qpath.toStdString()));
        });

        menu->addAction(tr("Fast Boot"), [this, qpath]() mutable {
          std::shared_ptr<SystemBootParameters> boot_params = getSystemBootParameters(qpath.toStdString());
          boot_params->override_fast_boot = true;
          g_emu_thread->bootSystem(std::move(boot_params));
        });

        menu->addAction(tr("Full Boot"), [this, qpath]() mutable {
          std::shared_ptr<SystemBootParameters> boot_params = getSystemBootParameters(qpath.toStdString());
          boot_params->override_fast_boot = false;
          g_emu_thread->bootSystem(std::move(boot_params));
        });

        if (m_ui.menuDebug->menuAction()->isVisible())
        {
          menu->addAction(tr("Boot and Debug"), [this, qpath]() mutable {
            openCPUDebugger();

            std::shared_ptr<SystemBootParameters> boot_params = getSystemBootParameters(qpath.toStdString());
            boot_params->override_start_paused = true;
            boot_params->disable_achievements_hardcore_mode = true;
            g_emu_thread->bootSystem(std::move(boot_params));
          });
        }
      }
      else
      {
        menu->addAction(tr("Change Disc"), [this, qpath]() {
          g_emu_thread->changeDisc(qpath, false, true);
          g_emu_thread->setSystemPaused(false);
          switchToEmulationView();
        });
      }
    }
    else
    {
      menu->addAction(tr("Properties..."), [dsentry = entry->GetDiscSetEntry()]() {
        // resolve path first
        auto lock = GameList::GetLock();
        const GameList::Entry* first_disc = GameList::GetFirstDiscSetMember(dsentry);
        if (first_disc && g_main_window)
          SettingsWindow::openGamePropertiesDialog(first_disc);
      });

      menu->addAction(tr("Set Cover Image..."), [this, qpath]() {
        const auto lock = GameList::GetLock();
        const GameList::Entry* entry = GameList::GetEntryForPath(qpath.toStdString());
        if (!entry)
          return;

        setGameListEntryCoverImage(entry);
      });

      menu->addSeparator();

      populateGameListContextMenu(entry, this, menu);

      menu->addSeparator();

      menu->addAction(tr("Select Disc..."), this, &MainWindow::onGameListEntryActivated);
    }

    menu->addSeparator();

    menu->addAction(tr("Exclude From List"),
                    [this, qpath]() { getSettingsWindow()->getGameListSettingsWidget()->addExcludedPath(qpath); });

    menu->addAction(tr("Reset Play Time"), [this, qpath]() {
      const auto lock = GameList::GetLock();
      const GameList::Entry* entry = GameList::GetEntryForPath(qpath.toStdString());
      if (!entry)
        return;

      clearGameListEntryPlayTime(entry);
    });
  }

  menu->addSeparator();

  menu->addAction(tr("Add Search Directory..."),
                  [this]() { getSettingsWindow()->getGameListSettingsWidget()->addSearchDirectory(this); });

  menu->popup(point);
}

void MainWindow::setGameListEntryCoverImage(const GameList::Entry* entry)
{
  const QString filename =
    QDir::toNativeSeparators(QFileDialog::getOpenFileName(this, tr("Select Cover Image"), QString(), tr(IMAGE_FILTER)));
  if (filename.isEmpty())
    return;

  const QString old_filename = QString::fromStdString(GameList::GetCoverImagePathForEntry(entry));
  const QString new_filename =
    QString::fromStdString(GameList::GetNewCoverImagePathForEntry(entry, filename.toUtf8().constData(), false));
  if (new_filename.isEmpty())
    return;

  if (!old_filename.isEmpty())
  {
    if (QFileInfo(old_filename) == QFileInfo(filename))
    {
      QtUtils::MessageBoxCritical(this, tr("Copy Error"),
                                  tr("You must select a different file to the current cover image."));
      return;
    }

    if (QtUtils::MessageBoxQuestion(this, tr("Cover Already Exists"),
                                    tr("A cover image for this game already exists, do you wish to replace it?"),
                                    QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
    {
      return;
    }
  }

  if (QFile::exists(new_filename) && !QFile::remove(new_filename))
  {
    QtUtils::MessageBoxCritical(this, tr("Copy Error"), tr("Failed to remove existing cover '%1'").arg(new_filename));
    return;
  }
  if (!QFile::copy(filename, new_filename))
  {
    QtUtils::MessageBoxCritical(this, tr("Copy Error"),
                                tr("Failed to copy '%1' to '%2'").arg(filename).arg(new_filename));
    return;
  }
  if (!old_filename.isEmpty() && old_filename != new_filename && !QFile::remove(old_filename))
  {
    QtUtils::MessageBoxCritical(this, tr("Copy Error"), tr("Failed to remove '%1'").arg(old_filename));
    return;
  }
  m_game_list_widget->refreshGridCovers();
}

void MainWindow::clearGameListEntryPlayTime(const GameList::Entry* entry)
{
  if (QtUtils::MessageBoxQuestion(
        this, tr("Confirm Reset"),
        tr("Are you sure you want to reset the play time for '%1'?\n\nThis action cannot be undone.")
          .arg(QtUtils::StringViewToQString(
            entry->GetDisplayTitle(m_game_list_widget->getModel()->getShowLocalizedTitles())))) != QMessageBox::Yes)
  {
    return;
  }

  GameList::ClearPlayedTimeForEntry(entry);
  m_game_list_widget->refresh(false);
}

void MainWindow::setupAdditionalUi()
{
  const bool status_bar_visible = Host::GetBaseBoolSettingValue("UI", "ShowStatusBar", true);
  m_ui.actionViewStatusBar->setChecked(status_bar_visible);
  m_ui.statusBar->setVisible(status_bar_visible);

  const bool toolbar_visible = Host::GetBaseBoolSettingValue("UI", "ShowToolbar", false);
  m_ui.actionViewToolbar->setChecked(toolbar_visible);
  m_ui.toolBar->setVisible(toolbar_visible);

  const bool toolbars_locked = Host::GetBaseBoolSettingValue("UI", "LockToolbar", false);
  m_ui.actionViewLockToolbar->setChecked(toolbars_locked);
  m_ui.toolBar->setMovable(!toolbars_locked);

  m_ui.actionViewSmallToolbarIcons->setChecked(Host::GetBaseBoolSettingValue("UI", "ToolbarSmallIcons", false));
  m_ui.actionViewToolbarLabels->setChecked(Host::GetBaseBoolSettingValue("UI", "ToolbarLabels", true));
  m_ui.actionViewToolbarLabelsBesideIcons->setChecked(
    Host::GetBaseBoolSettingValue("UI", "ToolbarLabelsBesideIcons", false));

  // mutually exclusive actions
  QActionGroup* group = new QActionGroup(this);
  group->addAction(m_ui.actionViewGameList);
  group->addAction(m_ui.actionViewGameGrid);
  group->addAction(m_ui.actionViewSystemDisplay);

  m_game_list_widget =
    new GameListWidget(m_ui.mainContainer, m_ui.actionViewGameList, m_ui.actionViewGameGrid, m_ui.actionMergeDiscSets,
                       m_ui.actionShowGameIcons, m_ui.actionAnimateGameIcons, m_ui.actionPreferAchievementGameIcons,
                       m_ui.actionGridViewShowTitles, m_ui.actionShowLocalizedTitles);
  m_ui.mainContainer->addWidget(m_game_list_widget);

  m_status_progress_widget = new QProgressBar(m_ui.statusBar);
  m_status_progress_widget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  m_status_progress_widget->setFixedSize(140, 16);
  m_status_progress_widget->setMinimum(0);
  m_status_progress_widget->setMaximum(100);
  m_status_progress_widget->hide();

  m_status_renderer_widget = new QLabel(m_ui.statusBar);
  m_status_renderer_widget->setFixedHeight(16);
  m_status_renderer_widget->setFixedSize(80, 16);
  m_status_renderer_widget->hide();

  m_status_resolution_widget = new QLabel(m_ui.statusBar);
  m_status_resolution_widget->setFixedHeight(16);
  m_status_resolution_widget->setFixedSize(80, 16);
  m_status_resolution_widget->hide();

  m_status_fps_widget = new QLabel(m_ui.statusBar);
  m_status_fps_widget->setFixedSize(100, 16);
  m_status_fps_widget->hide();

  m_status_vps_widget = new QLabel(m_ui.statusBar);
  m_status_vps_widget->setFixedSize(150, 16);
  m_status_vps_widget->hide();

  m_ui.actionAbout->setIcon(QtHost::GetAppIcon());

  m_settings_toolbar_menu = new QMenu(m_ui.toolBar);
  QtUtils::StylePopupMenu(m_settings_toolbar_menu);
  m_settings_toolbar_menu->addAction(m_ui.actionSettings);
  m_settings_toolbar_menu->addAction(m_ui.actionViewGameProperties);

  // View > Sort By
  {
    const int current_sort_column = m_game_list_widget->getListView()->horizontalHeader()->sortIndicatorSection();
    const Qt::SortOrder current_sort_order =
      m_game_list_widget->getListView()->horizontalHeader()->sortIndicatorOrder();

    QActionGroup* const column_group = new QActionGroup(m_ui.menuSortBy);

    for (int i = 0; i <= GameListModel::Column_LastVisible; i++)
    {
      QAction* const action = new QAction(
        m_game_list_widget->getModel()->headerData(i, Qt::Horizontal, Qt::DisplayRole).toString(), column_group);
      action->setCheckable(true);
      action->setChecked(current_sort_column == i);
      action->setData(i);
      m_ui.menuSortBy->addAction(action);
      connect(action, &QAction::triggered, this, &MainWindow::onViewSortByActionTriggered);
    }

    m_ui.menuSortBy->addSeparator();

    QActionGroup* const order_group = new QActionGroup(m_ui.menuSortBy);

    QAction* const ascending_action = new QAction(tr("&Ascending"), order_group);
    ascending_action->setIcon(QIcon::fromTheme(QIcon::ThemeIcon::GoUp));
    ascending_action->setCheckable(true);
    ascending_action->setChecked(current_sort_order == Qt::AscendingOrder);
    ascending_action->setObjectName(QStringLiteral("SortAscending"));
    m_ui.menuSortBy->addAction(ascending_action);
    connect(ascending_action, &QAction::triggered, this, &MainWindow::onViewSortOrderActionTriggered);

    QAction* const descending_action = new QAction(tr("&Descending"), order_group);
    descending_action->setIcon(QIcon::fromTheme(QIcon::ThemeIcon::GoDown));
    descending_action->setCheckable(true);
    descending_action->setChecked(current_sort_order == Qt::DescendingOrder);
    descending_action->setObjectName(QStringLiteral("SortDescending"));
    m_ui.menuSortBy->addAction(descending_action);
    connect(descending_action, &QAction::triggered, this, &MainWindow::onViewSortOrderActionTriggered);
  }

  for (u32 scale = 1; scale <= 10; scale++)
  {
    QAction* const action = m_ui.menuWindowSize->addAction(tr("%1x Scale").arg(scale));
    connect(action, &QAction::triggered,
            [scale = static_cast<float>(scale)]() { g_emu_thread->requestDisplaySize(scale); });
  }

  updateDebugMenuVisibility();

  m_shortcuts.open_file = new QShortcut(QKeySequence::Open, this, this, &MainWindow::onStartFileActionTriggered);
  m_shortcuts.game_list_refresh =
    new QShortcut(QKeySequence::Refresh, this, this, &MainWindow::onScanForNewGamesTriggered);
  m_shortcuts.game_list_search = new QShortcut(this);
  m_shortcuts.game_list_search->setKeys(QKeySequence::keyBindings(QKeySequence::Find) +
                                        QKeySequence::keyBindings(QKeySequence::FindNext));
  connect(m_shortcuts.game_list_search, &QShortcut::activated, m_game_list_widget, &GameListWidget::focusSearchWidget);
  m_shortcuts.game_list_zoom_in =
    new QShortcut(QKeySequence::ZoomIn, this, this, &MainWindow::onViewZoomInActionTriggered);
  m_shortcuts.game_list_zoom_out =
    new QShortcut(QKeySequence::ZoomOut, this, this, &MainWindow::onViewZoomOutActionTriggered);
  m_shortcuts.settings = new QShortcut(QKeySequence::Preferences, this, [this] { doSettings(); });

  s_disable_window_rounded_corners = Host::GetBaseBoolSettingValue("Main", "DisableWindowRoundedCorners", false);
  if (s_disable_window_rounded_corners)
    PlatformMisc::SetWindowRoundedCornerState(reinterpret_cast<void*>(winId()), false);

  QtUtils::StyleChildMenus(this);
}

void MainWindow::onGameListSortIndicatorOrderChanged(int column, Qt::SortOrder order)
{
  // yuck, allocations
  for (QAction* const action : m_ui.menuSortBy->actions())
  {
    bool activate = false;

    if (action->objectName() == QStringLiteral("SortAscending"))
      activate = (order == Qt::AscendingOrder);
    else if (action->objectName() == QStringLiteral("SortDescending"))
      activate = (order == Qt::DescendingOrder);
    else
      activate = (action->data() == column);

    if (activate)
    {
      const QSignalBlocker sb(m_ui.menuSortBy);
      action->setChecked(true);
    }
  }
}

void MainWindow::onViewSortByActionTriggered()
{
  const QAction* const action = qobject_cast<const QAction*>(sender());
  if (!action)
    return;

  QHeaderView* const hh = m_game_list_widget->getListView()->horizontalHeader();
  hh->setSortIndicator(action->data().toInt(), hh->sortIndicatorOrder());
}

void MainWindow::onViewSortOrderActionTriggered()
{
  const QAction* const action = qobject_cast<const QAction*>(sender());
  if (!action)
    return;

  const Qt::SortOrder order =
    (action->objectName() == QStringLiteral("SortAscending")) ? Qt::AscendingOrder : Qt::DescendingOrder;

  QHeaderView* const hh = m_game_list_widget->getListView()->horizontalHeader();
  hh->setSortIndicator(hh->sortIndicatorSection(), order);
}

void MainWindow::updateToolbarActions()
{
  const std::string active_buttons_str =
    Host::GetBaseStringSettingValue("UI", "ToolbarButtons", DEFAULT_TOOLBAR_ACTIONS);
  const std::vector<std::string_view> active_buttons = StringUtil::SplitString(active_buttons_str, ',');

  m_ui.toolBar->clear();

  bool any_items_before_separator = false;
  for (const auto& [name, action_ptr] : s_toolbar_actions)
  {
    if (!name)
    {
      // separator, but don't insert empty space between them
      if (any_items_before_separator)
      {
        any_items_before_separator = false;
        m_ui.toolBar->addSeparator();
      }

      continue;
    }

    // enabled?
    if (!StringUtil::IsInStringList(active_buttons, name))
      continue;

    // only one of resume/poweroff should be present depending on system state
    QAction* action = (m_ui.*action_ptr);
    if (action == m_ui.actionCloseGame && !s_system_valid)
      action = m_ui.actionResumeLastState;

    m_ui.toolBar->addAction(action);
    any_items_before_separator = true;
  }
}

void MainWindow::updateToolbarIconStyle()
{
  const bool show_toolbar = Host::GetBaseBoolSettingValue("UI", "ShowToolbar", false);
  const bool show_labels = Host::GetBaseBoolSettingValue("UI", "ToolbarLabels", true);
  const bool small_icons = Host::GetBaseBoolSettingValue("UI", "ToolbarSmallIcons", false);
  const bool labels_beside_icons = Host::GetBaseBoolSettingValue("UI", "ToolbarLabelsBesideIcons", false);

  Qt::ToolButtonStyle style;
  if (!show_labels)
    style = Qt::ToolButtonIconOnly;
  else if (labels_beside_icons)
    style = Qt::ToolButtonTextBesideIcon;
  else
    style = Qt::ToolButtonTextUnderIcon;
  if (m_ui.toolBar->toolButtonStyle() != style)
    m_ui.toolBar->setToolButtonStyle(style);

  const QSize icon_size = QSize(small_icons ? 16 : 32, small_icons ? 16 : 32);
  if (m_ui.toolBar->iconSize() != icon_size)
    m_ui.toolBar->setIconSize(icon_size);

  m_ui.actionViewLockToolbar->setEnabled(show_toolbar);
  m_ui.actionViewSmallToolbarIcons->setEnabled(show_toolbar);
  m_ui.actionViewToolbarLabels->setEnabled(show_toolbar);
  m_ui.actionViewToolbarLabelsBesideIcons->setEnabled(show_toolbar && show_labels);
}

void MainWindow::updateToolbarArea()
{
  const TinyString cfg_name = Host::GetBaseTinyStringSettingValue("UI", "ToolbarArea", "Top");
  Qt::ToolBarArea cfg_area = Qt::TopToolBarArea;
  for (const auto& [area, name] : s_toolbar_areas)
  {
    if (cfg_name == name)
    {
      cfg_area = area;
      break;
    }
  }

  if (toolBarArea(m_ui.toolBar) == cfg_area)
    return;

  removeToolBar(m_ui.toolBar);
  addToolBar(cfg_area, m_ui.toolBar);

  // need to explicitly make it visible again
  if (Host::GetBaseBoolSettingValue("UI", "ShowToolbar", false))
    m_ui.toolBar->show();
}

void MainWindow::onToolbarContextMenuRequested(const QPoint& pos)
{
  {
    const bool show_labels = Host::GetBaseBoolSettingValue("UI", "ToolbarLabels", true);

    const std::string active_buttons_str =
      Host::GetBaseStringSettingValue("UI", "ToolbarButtons", DEFAULT_TOOLBAR_ACTIONS);
    std::vector<std::string_view> active_buttons = StringUtil::SplitString(active_buttons_str, ',');

    QMenu* const menu = QtUtils::NewPopupMenu(this);

    QAction* action = menu->addAction(tr("Lock Toolbar"));
    action->setCheckable(true);
    action->setChecked(!m_ui.toolBar->isMovable());
    connect(action, &QAction::toggled, this, &MainWindow::onViewToolbarLockActionToggled);

    action = menu->addAction(tr("Small Icons"));
    action->setCheckable(true);
    action->setChecked(Host::GetBaseBoolSettingValue("UI", "ToolbarSmallIcons", false));
    connect(action, &QAction::toggled, this, &MainWindow::onViewToolbarSmallIconsActionToggled);

    action = menu->addAction(tr("Show Labels"));
    action->setCheckable(true);
    action->setChecked(Host::GetBaseBoolSettingValue("UI", "ToolbarLabels", true));
    connect(action, &QAction::toggled, this, &MainWindow::onViewToolbarLabelsActionToggled);

    action = menu->addAction(tr("Labels Beside Icons"));
    action->setCheckable(true);
    action->setChecked(Host::GetBaseBoolSettingValue("UI", "ToolbarLabelsBesideIcons", false));
    action->setEnabled(show_labels);
    connect(action, &QAction::toggled, this, &MainWindow::onViewToolbarLabelsBesideIconsActionToggled);

    QMenu* const position_menu = menu->addMenu(tr("Position"));
    QtUtils::StylePopupMenu(position_menu);
    for (const auto& [area, name] : s_toolbar_areas)
    {
      QAction* const position_action = position_menu->addAction(tr(name), [this, name]() {
        Host::SetBaseStringSettingValue("UI", "ToolbarArea", name);
        Host::CommitBaseSettingChanges();
        updateToolbarArea();
      });
      position_action->setCheckable(true);
      position_action->setChecked(toolBarArea(m_ui.toolBar) == area);
    }

    menu->addSeparator();

    for (const auto& [name, action_ptr] : s_toolbar_actions)
    {
      if (!name)
      {
        menu->addSeparator();
        continue;
      }

      QAction* const menu_action = menu->addAction((m_ui.*action_ptr)->iconText());
      menu_action->setCheckable(true);
      menu_action->setChecked(StringUtil::IsInStringList(active_buttons, name));
      connect(menu_action, &QAction::toggled, this, [this, name](bool checked) {
        const std::string active_buttons_str =
          Host::GetBaseStringSettingValue("UI", "ToolbarButtons", DEFAULT_TOOLBAR_ACTIONS);
        std::vector<std::string_view> active_buttons = StringUtil::SplitString(active_buttons_str, ',');
        if (checked ? StringUtil::AddToStringList(active_buttons, name) :
                      StringUtil::RemoveFromStringList(active_buttons, name))
        {
          Host::SetBaseStringSettingValue("UI", "ToolbarButtons", StringUtil::JoinString(active_buttons, ',').c_str());
          Host::CommitBaseSettingChanges();
          updateToolbarActions();
        }
      });
    }

    menu->popup(m_ui.toolBar->mapToGlobal(pos));
  }
}

void MainWindow::onToolbarTopLevelChanged(bool top_level)
{
  // ignore while floating
  if (top_level)
    return;

  // update config
  const Qt::ToolBarArea current_area = toolBarArea(m_ui.toolBar);
  for (const auto& [area, name] : s_toolbar_areas)
  {
    if (current_area == area)
    {
      Host::SetBaseStringSettingValue("UI", "ToolbarArea", name);
      Host::CommitBaseSettingChanges();
      break;
    }
  }
}

void MainWindow::updateEmulationActions(bool starting, bool running, bool achievements_hardcore_mode)
{
  const bool starting_or_running = (starting || running);
  const bool starting_or_not_running = (starting || !running);
  m_ui.actionStartFile->setDisabled(starting_or_running);
  m_ui.actionStartDisc->setDisabled(starting_or_running);
  m_ui.actionStartBios->setDisabled(starting_or_running);
  m_ui.actionResumeLastState->setDisabled(starting_or_running || achievements_hardcore_mode);
  m_ui.actionStartFullscreenUI->setDisabled(starting_or_running);
  m_ui.actionStartFullscreenUI2->setDisabled(starting_or_running);

  m_ui.actionCloseGame->setDisabled(starting_or_not_running);
  m_ui.actionCloseGameWithoutSaving->setDisabled(starting_or_not_running);
  m_ui.actionResetGame->setDisabled(starting_or_not_running);
  m_ui.actionPause->setDisabled(starting_or_not_running);
  m_ui.actionChangeDisc->setDisabled(starting_or_not_running);
  m_ui.actionCheatsToolbar->setDisabled(starting_or_not_running || achievements_hardcore_mode);
  m_ui.actionScreenshot->setDisabled(starting_or_not_running);
  m_ui.menuChangeDisc->menuAction()->setDisabled(starting_or_not_running);
  m_ui.menuChangeDisc->setDisabled(starting_or_not_running);
  m_ui.menuCheats->menuAction()->setDisabled(starting_or_not_running || achievements_hardcore_mode);
  m_ui.menuCheats->setDisabled(starting_or_not_running || achievements_hardcore_mode);
  m_ui.actionCPUDebugger->setDisabled(achievements_hardcore_mode);
  m_ui.actionMemoryEditor->setDisabled(achievements_hardcore_mode);
  m_ui.actionMemoryScanner->setDisabled(achievements_hardcore_mode);
  m_ui.actionFreeCamera->setDisabled(achievements_hardcore_mode);
  m_ui.actionReloadTextureReplacements->setDisabled(starting_or_not_running);
  m_ui.actionDumpRAM->setDisabled(starting_or_not_running || achievements_hardcore_mode);
  m_ui.actionDumpVRAM->setDisabled(starting_or_not_running || achievements_hardcore_mode);
  m_ui.actionDumpSPURAM->setDisabled(starting_or_not_running || achievements_hardcore_mode);
  m_ui.actionCaptureGPUFrame->setDisabled(starting_or_not_running);

  m_ui.actionLoadState->setDisabled(starting);
  m_ui.menuLoadState->menuAction()->setDisabled(starting);
  m_ui.menuLoadState->setDisabled(starting);
  m_ui.actionSaveState->setDisabled(starting_or_not_running);
  m_ui.menuSaveState->menuAction()->setDisabled(starting_or_not_running);
  m_ui.menuSaveState->setDisabled(starting_or_not_running);
  m_ui.menuWindowSize->setDisabled(starting_or_not_running);
  m_ui.actionViewGameList->setDisabled(starting);
  m_ui.actionViewGameGrid->setDisabled(starting);

  m_ui.actionViewGameProperties->setDisabled(starting_or_not_running);

  m_ui.actionControllerTest->setDisabled(starting_or_running);

  m_game_list_widget->setDisabled(starting);

  updateShortcutActions(starting);

  if (starting_or_running)
  {
    if (m_ui.toolBar->widgetForAction(m_ui.actionResumeLastState))
    {
      m_ui.toolBar->insertAction(m_ui.actionResumeLastState, m_ui.actionCloseGame);
      m_ui.toolBar->removeAction(m_ui.actionResumeLastState);
    }
  }
  else
  {
    if (m_ui.toolBar->widgetForAction(m_ui.actionCloseGame))
    {
      m_ui.toolBar->insertAction(m_ui.actionCloseGame, m_ui.actionResumeLastState);
      m_ui.toolBar->removeAction(m_ui.actionCloseGame);
    }

    m_ui.actionViewGameProperties->setEnabled(false);
  }

  m_ui.statusBar->clearMessage();
}

void MainWindow::updateShortcutActions(bool starting)
{
  const bool starting_or_running = starting || s_system_valid;
  const bool is_showing_game_list = isShowingGameList();

  m_shortcuts.open_file->setEnabled(!starting_or_running);
  m_shortcuts.game_list_refresh->setEnabled(is_showing_game_list);
  m_shortcuts.game_list_search->setEnabled(is_showing_game_list);
  m_shortcuts.game_list_zoom_in->setEnabled(is_showing_game_list);
  m_shortcuts.game_list_zoom_out->setEnabled(is_showing_game_list);
}

void MainWindow::updateStatusBarWidgetVisibility()
{
  auto Update = [this](QWidget* widget, bool visible, int stretch) {
    if (widget->isVisible())
    {
      m_ui.statusBar->removeWidget(widget);
      widget->hide();
    }

    if (visible)
    {
      m_ui.statusBar->addPermanentWidget(widget, stretch);
      widget->show();
    }
  };

  Update(m_status_renderer_widget, s_system_valid && !s_system_paused, 0);
  Update(m_status_resolution_widget, s_system_valid && !s_system_paused, 0);
  Update(m_status_fps_widget, s_system_valid && !s_system_paused, 0);
  Update(m_status_vps_widget, s_system_valid && !s_system_paused, 0);
}

void MainWindow::updateWindowTitle()
{
  QString suffix(QtHost::GetAppConfigSuffix());
  QString main_title(QtHost::GetAppNameAndVersion() + suffix);
  QString display_title(s_current_game_title + suffix);

  if (!s_system_valid || s_current_game_title.isEmpty())
    display_title = main_title;
  else if (isRenderingToMain())
    main_title = display_title;

  if (windowTitle() != main_title)
    setWindowTitle(main_title);
  setWindowIcon(s_current_game_icon.isNull() ? QtHost::GetAppIcon() : s_current_game_icon);

  if (m_display_widget && !isRenderingToMain())
  {
    QWidget* container =
      m_display_container ? static_cast<QWidget*>(m_display_container) : static_cast<QWidget*>(m_display_widget);
    if (container->windowTitle() != display_title)
      container->setWindowTitle(display_title);
    container->setWindowIcon(s_current_game_icon.isNull() ? QtHost::GetAppIcon() : s_current_game_icon);
  }

  if (g_log_window)
    g_log_window->updateWindowTitle();
}

void MainWindow::updateWindowState()
{
  // Skip all of this when we're closing, since we don't want to make ourselves visible and cancel it.
  if (m_is_closing)
    return;

  const bool visible = !shouldHideMainWindow();
  const bool resizeable = (!Host::GetBoolSettingValue("Main", "DisableWindowResize", false) || !wantsDisplayWidget() ||
                           isRenderingFullscreen());

  if (isVisible() != visible)
    setVisible(visible);

  // No point changing realizability if we're not visible.
  if (visible)
    QtUtils::SetWindowResizeable(this, resizeable);

  // Update the display widget too if rendering separately.
  if (m_display_widget && !isRenderingToMain())
    QtUtils::SetWindowResizeable(getDisplayContainer(), resizeable);
}

void MainWindow::setProgressBar(int current, int total)
{
  const int value = (total != 0) ? ((current * 100) / total) : 0;
  if (m_status_progress_widget->value() != value)
    m_status_progress_widget->setValue(value);

  if (m_status_progress_widget->isVisible())
    return;

  m_status_progress_widget->show();
  m_ui.statusBar->addPermanentWidget(m_status_progress_widget);
}

void MainWindow::clearProgressBar()
{
  if (!m_status_progress_widget->isVisible())
    return;

  m_status_progress_widget->hide();
  m_ui.statusBar->removeWidget(m_status_progress_widget);
}

bool MainWindow::isShowingGameList() const
{
  return (m_ui.mainContainer->currentIndex() == 0);
}

bool MainWindow::isRenderingFullscreen() const
{
  return m_display_widget && (m_exclusive_fullscreen_requested || getDisplayContainer()->isFullScreen());
}

bool MainWindow::isRenderingToMain() const
{
  return (m_display_widget && m_ui.mainContainer->indexOf(m_display_widget) == 1);
}

bool MainWindow::shouldHideMouseCursor() const
{
  return m_hide_mouse_cursor ||
         (isRenderingFullscreen() && Host::GetBoolSettingValue("Main", "HideCursorInFullscreen", true));
}

bool MainWindow::shouldHideMainWindow() const
{
  // CanRenderToMain check is for temporary unfullscreens.
  return (!isRenderingToMain() && wantsDisplayWidget() &&
          ((Host::GetBoolSettingValue("Main", "RenderToSeparateWindow", false) &&
            Host::GetBoolSettingValue("Main", "HideMainWindowWhenRunning", false)) ||
           (QtHost::CanRenderToMainWindow() &&
            (isRenderingFullscreen() || s_system_locked.load(std::memory_order_relaxed))))) ||
         QtHost::InNoGUIMode();
}

void MainWindow::switchToGameListView()
{
  if (QtHost::CanRenderToMainWindow())
    // Normally, we'd never end up here. But on MacOS, the global menu is accessible while fullscreen.
    exitFullscreen(true);

  if (!isShowingGameList())
  {
    if (wantsDisplayWidget())
    {
      m_was_paused_on_surface_loss = s_system_paused;
      if (!s_system_paused)
        g_emu_thread->setSystemPaused(true);

      // switch to surfaceless. we have to wait until the display widget is gone before we swap over.
      g_emu_thread->setSurfaceless(true);
      QtUtils::ProcessEventsWithSleep(QEventLoop::ExcludeUserInputEvents,
                                      [this]() { return static_cast<bool>(m_display_widget); });
    }

    updateShortcutActions(false);
  }

  m_game_list_widget->setFocus();
}

void MainWindow::switchToEmulationView()
{
  if (!wantsDisplayWidget() || !isShowingGameList())
    return;

  // we're no longer surfaceless! this will call back to acquireRenderWindow(), which will swap the widget out.
  g_emu_thread->setSurfaceless(false);

  // resume if we weren't paused at switch time
  if (s_system_paused && !m_was_paused_on_surface_loss)
    g_emu_thread->setSystemPaused(false);

  updateShortcutActions(false);

  if (m_display_widget)
  {
    if (!isRenderingToMain())
      QtUtils::ShowOrRaiseWindow(QtUtils::GetRootWidget(m_display_widget));
    m_display_widget->setFocus();
  }
}

void MainWindow::connectSignals()
{
  connect(qApp, &QGuiApplication::applicationStateChanged, this, &MainWindow::onApplicationStateChanged);
  connect(m_ui.toolBar, &QToolBar::customContextMenuRequested, this, &MainWindow::onToolbarContextMenuRequested);
  connect(m_ui.toolBar, &QToolBar::topLevelChanged, this, &MainWindow::onToolbarTopLevelChanged);

  connect(m_ui.actionStartFile, &QAction::triggered, this, &MainWindow::onStartFileActionTriggered);
  connect(m_ui.actionStartDisc, &QAction::triggered, this, &MainWindow::onStartDiscActionTriggered);
  connect(m_ui.actionStartBios, &QAction::triggered, this, &MainWindow::onStartBIOSActionTriggered);
  connect(m_ui.actionResumeLastState, &QAction::triggered, g_emu_thread, &EmuThread::resumeSystemFromMostRecentState);
  connect(m_ui.actionChangeDisc, &QAction::triggered, [this] { m_ui.menuChangeDisc->popup(QCursor::pos()); });
  connect(m_ui.actionChangeDiscFromFile, &QAction::triggered, this, &MainWindow::onChangeDiscFromFileActionTriggered);
  connect(m_ui.actionChangeDiscFromDevice, &QAction::triggered, this,
          &MainWindow::onChangeDiscFromDeviceActionTriggered);
  connect(m_ui.actionChangeDiscFromGameList, &QAction::triggered, this,
          &MainWindow::onChangeDiscFromGameListActionTriggered);
  connect(m_ui.menuChangeDisc, &QMenu::aboutToShow, this, &MainWindow::onChangeDiscMenuAboutToShow);
  connect(m_ui.menuLoadState, &QMenu::aboutToShow, this, &MainWindow::onLoadStateMenuAboutToShow);
  connect(m_ui.menuSaveState, &QMenu::aboutToShow, this, &MainWindow::onSaveStateMenuAboutToShow);
  connect(m_ui.menuCheats, &QMenu::aboutToShow, this, &MainWindow::onCheatsMenuAboutToShow);
  connect(m_ui.actionCheatsToolbar, &QAction::triggered, [this] { m_ui.menuCheats->popup(QCursor::pos()); });
  connect(m_ui.actionStartFullscreenUI, &QAction::triggered, this, &MainWindow::onStartFullscreenUITriggered);
  connect(m_ui.actionStartFullscreenUI2, &QAction::triggered, this, &MainWindow::onStartFullscreenUITriggered);
  connect(m_ui.actionRemoveDisc, &QAction::triggered, this, &MainWindow::onRemoveDiscActionTriggered);
  connect(m_ui.actionAddGameDirectory, &QAction::triggered,
          [this]() { getSettingsWindow()->getGameListSettingsWidget()->addSearchDirectory(this); });
  connect(m_ui.actionCloseGame, &QAction::triggered, this, &MainWindow::onCloseGameActionTriggered);
  connect(m_ui.actionCloseGameWithoutSaving, &QAction::triggered, this,
          &MainWindow::onCloseGameWithoutSavingActionTriggered);
  connect(m_ui.actionResetGame, &QAction::triggered, this, &MainWindow::onResetGameActionTriggered);
  connect(m_ui.actionPause, &QAction::toggled, this, &MainWindow::onPauseActionToggled);
  connect(m_ui.actionScreenshot, &QAction::triggered, g_emu_thread, &EmuThread::saveScreenshot);
  connect(m_ui.actionScanForNewGames, &QAction::triggered, this, &MainWindow::onScanForNewGamesTriggered);
  connect(m_ui.actionRescanAllGames, &QAction::triggered, this, [this]() { refreshGameList(true); });
  connect(m_ui.actionLoadState, &QAction::triggered, this, [this]() { m_ui.menuLoadState->popup(QCursor::pos()); });
  connect(m_ui.actionSaveState, &QAction::triggered, this, [this]() { m_ui.menuSaveState->popup(QCursor::pos()); });
  connect(m_ui.actionExit, &QAction::triggered, this, &MainWindow::close);
  connect(m_ui.actionFullscreen, &QAction::triggered, g_emu_thread, &EmuThread::toggleFullscreen);
  connect(m_ui.actionSettings, &QAction::triggered, [this]() { doSettings(); });
  connect(m_ui.actionSettings2, &QAction::triggered, this, &MainWindow::onSettingsTriggeredFromToolbar);
  connect(m_ui.actionInterfaceSettings, &QAction::triggered, [this]() { doSettings("Interface"); });
  connect(m_ui.actionBIOSSettings, &QAction::triggered, [this]() { doSettings("BIOS"); });
  connect(m_ui.actionConsoleSettings, &QAction::triggered, [this]() { doSettings("Console"); });
  connect(m_ui.actionEmulationSettings, &QAction::triggered, [this]() { doSettings("Emulation"); });
  connect(m_ui.actionGameListSettings, &QAction::triggered, [this]() { doSettings("Game List"); });
  connect(m_ui.actionHotkeySettings, &QAction::triggered,
          [this]() { doControllerSettings(ControllerSettingsWindow::Category::HotkeySettings); });
  connect(m_ui.actionControllerSettings, &QAction::triggered,
          [this]() { doControllerSettings(ControllerSettingsWindow::Category::GlobalSettings); });
  connect(m_ui.actionMemoryCardSettings, &QAction::triggered, [this]() { doSettings("Memory Cards"); });
  connect(m_ui.actionGraphicsSettings, &QAction::triggered, [this]() { doSettings("Graphics"); });
  connect(m_ui.actionPostProcessingSettings, &QAction::triggered, [this]() { doSettings("Post-Processing"); });
  connect(m_ui.actionAudioSettings, &QAction::triggered, [this]() { doSettings("Audio"); });
  connect(m_ui.actionAchievementSettings, &QAction::triggered, [this]() { doSettings("Achievements"); });
  connect(m_ui.actionFolderSettings, &QAction::triggered, [this]() { doSettings("Folders"); });
  connect(m_ui.actionAdvancedSettings, &QAction::triggered, [this]() { doSettings("Advanced"); });
  connect(m_ui.actionControllerProfiles, &QAction::triggered, this, &MainWindow::onSettingsControllerProfilesTriggered);
  connect(m_ui.actionViewToolbar, &QAction::toggled, this, &MainWindow::onViewToolbarActionToggled);
  connect(m_ui.actionViewLockToolbar, &QAction::toggled, this, &MainWindow::onViewToolbarLockActionToggled);
  connect(m_ui.actionViewSmallToolbarIcons, &QAction::toggled, this, &MainWindow::onViewToolbarSmallIconsActionToggled);
  connect(m_ui.actionViewToolbarLabels, &QAction::toggled, this, &MainWindow::onViewToolbarLabelsActionToggled);
  connect(m_ui.actionViewToolbarLabelsBesideIcons, &QAction::toggled, this,
          &MainWindow::onViewToolbarLabelsBesideIconsActionToggled);
  connect(m_ui.actionViewStatusBar, &QAction::toggled, this, &MainWindow::onViewStatusBarActionToggled);
  connect(m_ui.actionViewGameList, &QAction::triggered, this, &MainWindow::onViewGameListActionTriggered);
  connect(m_ui.actionViewGameGrid, &QAction::triggered, this, &MainWindow::onViewGameGridActionTriggered);
  connect(m_ui.actionViewSystemDisplay, &QAction::triggered, this, &MainWindow::onViewSystemDisplayTriggered);
  connect(m_ui.actionViewGameProperties, &QAction::triggered, this, [this]() { openGamePropertiesForCurrentGame(); });
  connect(m_ui.actionGitHubRepository, &QAction::triggered, this, &MainWindow::onGitHubRepositoryActionTriggered);
  connect(m_ui.actionDiscordServer, &QAction::triggered, this, &MainWindow::onDiscordServerActionTriggered);
  connect(m_ui.actionViewThirdPartyNotices, &QAction::triggered, this,
          [this]() { AboutDialog::openThirdPartyNotices(this); });
  connect(m_ui.actionAboutQt, &QAction::triggered, qApp, &QApplication::aboutQt);
  connect(m_ui.actionAbout, &QAction::triggered, this, &MainWindow::onAboutActionTriggered);
  connect(m_ui.actionCheckForUpdates, &QAction::triggered, this, &MainWindow::onCheckForUpdatesActionTriggered);
  connect(m_ui.actionMemoryCardEditor, &QAction::triggered, this, &MainWindow::onToolsMemoryCardEditorTriggered);
  connect(m_ui.actionMemoryEditor, &QAction::triggered, this, &MainWindow::onToolsMemoryEditorTriggered);
  connect(m_ui.actionMemoryScanner, &QAction::triggered, this, &MainWindow::onToolsMemoryScannerTriggered);
  connect(m_ui.actionISOBrowser, &QAction::triggered, this, &MainWindow::onToolsISOBrowserTriggered);
  connect(m_ui.actionCoverDownloader, &QAction::triggered, this, &MainWindow::onToolsCoverDownloaderTriggered);
  connect(m_ui.actionControllerTest, &QAction::triggered, g_emu_thread, &EmuThread::startControllerTest);
  connect(m_ui.actionMediaCapture, &QAction::toggled, this, &MainWindow::onToolsMediaCaptureToggled);
  connect(m_ui.actionCaptureGPUFrame, &QAction::triggered, g_emu_thread, &EmuThread::captureGPUFrameDump);
  connect(m_ui.actionCPUDebugger, &QAction::triggered, this, &MainWindow::openCPUDebugger);
  connect(m_ui.actionOpenDataDirectory, &QAction::triggered, this, &MainWindow::onToolsOpenDataDirectoryTriggered);
  connect(m_ui.actionOpenTextureDirectory, &QAction::triggered, this,
          &MainWindow::onToolsOpenTextureDirectoryTriggered);
  connect(m_ui.actionReloadTextureReplacements, &QAction::triggered, g_emu_thread,
          &EmuThread::reloadTextureReplacements);
  connect(m_ui.actionMergeDiscSets, &QAction::triggered, m_game_list_widget, &GameListWidget::setMergeDiscSets);
  connect(m_ui.actionShowLocalizedTitles, &QAction::triggered, m_game_list_widget,
          &GameListWidget::setShowLocalizedTitles);
  connect(m_ui.actionShowGameIcons, &QAction::triggered, m_game_list_widget, &GameListWidget::setShowGameIcons);
  connect(m_ui.actionAnimateGameIcons, &QAction::triggered, m_game_list_widget, &GameListWidget::setAnimateGameIcons);
  connect(m_ui.actionPreferAchievementGameIcons, &QAction::triggered, m_game_list_widget,
          &GameListWidget::setPreferAchievementGameIcons);
  connect(m_ui.actionGridViewShowTitles, &QAction::triggered, m_game_list_widget, &GameListWidget::setShowCoverTitles);
  connect(m_ui.actionGridViewShowTitles, &QAction::triggered, m_game_list_widget, &GameListWidget::setShowCoverTitles);
  connect(m_ui.actionViewZoomIn, &QAction::triggered, this, &MainWindow::onViewZoomInActionTriggered);
  connect(m_ui.actionViewZoomOut, &QAction::triggered, this, &MainWindow::onViewZoomOutActionTriggered);
  connect(m_ui.actionGridViewRefreshCovers, &QAction::triggered, m_game_list_widget,
          &GameListWidget::refreshGridCovers);
  connect(m_ui.actionViewRefreshAchievementProgress, &QAction::triggered, g_emu_thread,
          &EmuThread::refreshAchievementsAllProgress);
  connect(m_ui.actionChangeGameListBackground, &QAction::triggered, this,
          &MainWindow::onViewChangeGameListBackgroundTriggered);
  connect(m_ui.actionClearGameListBackground, &QAction::triggered, this,
          &MainWindow::onViewClearGameListBackgroundTriggered);

  connect(g_emu_thread, &EmuThread::settingsResetToDefault, this, &MainWindow::onSettingsResetToDefault,
          Qt::QueuedConnection);
  connect(g_emu_thread, &EmuThread::errorReported, this, &MainWindow::reportError, Qt::BlockingQueuedConnection);
  connect(g_emu_thread, &EmuThread::messageConfirmed, this, &MainWindow::confirmMessage, Qt::BlockingQueuedConnection);
  connect(g_emu_thread, &EmuThread::statusMessage, this, &MainWindow::onStatusMessage);
  connect(g_emu_thread, &EmuThread::onAcquireRenderWindowRequested, this, &MainWindow::acquireRenderWindow,
          Qt::BlockingQueuedConnection);
  connect(g_emu_thread, &EmuThread::onReleaseRenderWindowRequested, this, &MainWindow::releaseRenderWindow);
  connect(g_emu_thread, &EmuThread::onResizeRenderWindowRequested, this, &MainWindow::displayResizeRequested,
          Qt::BlockingQueuedConnection);
  connect(g_emu_thread, &EmuThread::focusDisplayWidgetRequested, this, &MainWindow::focusDisplayWidget);
  connect(g_emu_thread, &EmuThread::systemStarting, this, &MainWindow::onSystemStarting);
  connect(g_emu_thread, &EmuThread::systemStarted, this, &MainWindow::onSystemStarted);
  connect(g_emu_thread, &EmuThread::systemStopping, this, &MainWindow::onSystemStopping);
  connect(g_emu_thread, &EmuThread::systemDestroyed, this, &MainWindow::onSystemDestroyed);
  connect(g_emu_thread, &EmuThread::systemPaused, this, &MainWindow::onSystemPaused);
  connect(g_emu_thread, &EmuThread::systemResumed, this, &MainWindow::onSystemResumed);
  connect(g_emu_thread, &EmuThread::systemGameChanged, this, &MainWindow::onSystemGameChanged);
  connect(g_emu_thread, &EmuThread::systemUndoStateAvailabilityChanged, this,
          &MainWindow::onSystemUndoStateAvailabilityChanged);
  connect(g_emu_thread, &EmuThread::mediaCaptureStarted, this, &MainWindow::onMediaCaptureStarted);
  connect(g_emu_thread, &EmuThread::mediaCaptureStopped, this, &MainWindow::onMediaCaptureStopped);
  connect(g_emu_thread, &EmuThread::mouseModeRequested, this, &MainWindow::onMouseModeRequested);
  connect(g_emu_thread, &EmuThread::fullscreenUIStartedOrStopped, this, &MainWindow::onFullscreenUIStartedOrStopped);
  connect(g_emu_thread, &EmuThread::achievementsLoginRequested, this, &MainWindow::onAchievementsLoginRequested);
  connect(g_emu_thread, &EmuThread::achievementsLoginSuccess, this, &MainWindow::onAchievementsLoginSuccess);
  connect(g_emu_thread, &EmuThread::achievementsActiveChanged, this, &MainWindow::onAchievementsActiveChanged);
  connect(g_emu_thread, &EmuThread::achievementsHardcoreModeChanged, this,
          &MainWindow::onAchievementsHardcoreModeChanged);
  connect(g_emu_thread, &EmuThread::achievementsAllProgressRefreshed, this,
          &MainWindow::onAchievementsAllProgressRefreshed);
  connect(g_emu_thread, &EmuThread::onCoverDownloaderOpenRequested, this, &MainWindow::onToolsCoverDownloaderTriggered);
  connect(g_emu_thread, &EmuThread::onCreateAuxiliaryRenderWindow, this, &MainWindow::onCreateAuxiliaryRenderWindow,
          Qt::BlockingQueuedConnection);
  connect(g_emu_thread, &EmuThread::onDestroyAuxiliaryRenderWindow, this, &MainWindow::onDestroyAuxiliaryRenderWindow,
          Qt::BlockingQueuedConnection);
  connect(this, &MainWindow::themeChanged, g_emu_thread, &EmuThread::updateFullscreenUITheme);

  // These need to be queued connections to stop crashing due to menus opening/closing and switching focus.
  connect(m_game_list_widget, &GameListWidget::refreshProgress, this, &MainWindow::onGameListRefreshProgress);
  connect(m_game_list_widget, &GameListWidget::refreshComplete, this, &MainWindow::onGameListRefreshComplete);
  connect(m_game_list_widget, &GameListWidget::selectionChanged, this, &MainWindow::onGameListSelectionChanged,
          Qt::QueuedConnection);
  connect(m_game_list_widget, &GameListWidget::entryActivated, this, &MainWindow::onGameListEntryActivated,
          Qt::QueuedConnection);
  connect(m_game_list_widget, &GameListWidget::entryContextMenuRequested, this,
          &MainWindow::onGameListEntryContextMenuRequested, Qt::QueuedConnection);
  connect(m_game_list_widget, &GameListWidget::addGameDirectoryRequested, this,
          [this]() { getSettingsWindow()->getGameListSettingsWidget()->addSearchDirectory(this); });
  connect(m_game_list_widget->getListView()->horizontalHeader(), &QHeaderView::sortIndicatorChanged, this,
          &MainWindow::onGameListSortIndicatorOrderChanged);

  SettingWidgetBinder::BindMenuToEnumSetting(m_ui.menuCPUExecutionMode, "CPU", "ExecutionMode",
                                             &Settings::ParseCPUExecutionMode, &Settings::GetCPUExecutionModeName,
                                             &Settings::GetCPUExecutionModeDisplayName,
                                             Settings::DEFAULT_CPU_EXECUTION_MODE, CPUExecutionMode::Count);
  SettingWidgetBinder::BindMenuToEnumSetting(m_ui.menuRenderer, "GPU", "Renderer", &Settings::ParseRendererName,
                                             &Settings::GetRendererName, &Settings::GetRendererDisplayName,
                                             Settings::DEFAULT_GPU_RENDERER, GPURenderer::Count);
  SettingWidgetBinder::BindMenuToEnumSetting(
    m_ui.menuCropMode, "Display", "CropMode", &Settings::ParseDisplayCropMode, &Settings::GetDisplayCropModeName,
    &Settings::GetDisplayCropModeDisplayName, Settings::DEFAULT_DISPLAY_CROP_MODE, DisplayCropMode::MaxCount);
  SettingWidgetBinder::BindMenuToEnumSetting(m_ui.menuLogLevel, "Logging", "LogLevel", &Settings::ParseLogLevelName,
                                             &Settings::GetLogLevelName, &Settings::GetLogLevelDisplayName,
                                             Log::DEFAULT_LOG_LEVEL, Log::Level::MaxCount);
  connect(m_ui.menuLogChannels, &QMenu::aboutToShow, this, &MainWindow::onDebugLogChannelsMenuAboutToShow);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionLogToSystemConsole, "Logging", "LogToConsole",
                                               false);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionLogToFile, "Logging", "LogToFile", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionLogToWindow, "Logging", "LogToWindow", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionLogTimestamps, "Logging", "LogTimestamps", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionLogFileTimestamps, "Logging", "LogFileTimestamps",
                                               false);

  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionEnableSafeMode, "Main", "DisableAllEnhancements",
                                               false);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionDebugDumpCPUtoVRAMCopies, "Debug",
                                               "DumpCPUToVRAMCopies", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionDebugDumpVRAMtoCPUCopies, "Debug",
                                               "DumpVRAMToCPUCopies", false);
  connect(m_ui.actionDumpRAM, &QAction::triggered, [this]() {
    const QString filename = QDir::toNativeSeparators(
      QFileDialog::getSaveFileName(this, tr("Destination File"), QString(), tr("Binary Files (*.bin)")));
    if (filename.isEmpty())
      return;

    g_emu_thread->dumpRAM(filename);
  });
  connect(m_ui.actionDumpVRAM, &QAction::triggered, [this]() {
    const QString filename = QDir::toNativeSeparators(QFileDialog::getSaveFileName(
      this, tr("Destination File"), QString(), tr("Binary Files (*.bin);;PNG Images (*.png)")));
    if (filename.isEmpty())
      return;

    g_emu_thread->dumpVRAM(filename);
  });
  connect(m_ui.actionDumpSPURAM, &QAction::triggered, [this]() {
    const QString filename = QDir::toNativeSeparators(
      QFileDialog::getSaveFileName(this, tr("Destination File"), QString(), tr("Binary Files (*.bin)")));
    if (filename.isEmpty())
      return;

    g_emu_thread->dumpSPURAM(filename);
  });
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionDebugShowVRAM, "Debug", "ShowVRAM", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionFreeCamera, "DebugWindows", "Freecam", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionDebugShowGPUState, "DebugWindows", "GPU", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionDebugShowCDROMState, "DebugWindows", "CDROM", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionDebugShowSPUState, "DebugWindows", "SPU", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionDebugShowTimersState, "DebugWindows", "Timers",
                                               false);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionDebugShowMDECState, "DebugWindows", "MDEC", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionDebugShowDMAState, "DebugWindows", "DMA", false);

  // Set status tip to the same as tooltip for accessibility.
  for (QAction* action : findChildren<QAction*>())
  {
    if (action->menu())
      continue;

    if (const QString tip = action->toolTip(); !tip.isEmpty())
      action->setStatusTip(tip);
  }
}

void MainWindow::onSettingsThemeChanged()
{
#ifdef _WIN32
  const QString old_style_name = qApp->style()->name();
#endif

  QtHost::UpdateApplicationTheme();

#ifdef _WIN32
  // Work around a bug where the background colour of menus is broken when changing to/from the windowsvista theme.
  const QString new_style_name = qApp->style()->name();
  if ((old_style_name == QStringLiteral("windowsvista")) != (new_style_name == QStringLiteral("windowsvista")))
    recreate();
#endif
}

void MainWindow::onSettingsResetToDefault(bool system, bool controller)
{
  if (system && m_settings_window)
  {
    const bool had_settings_window = m_settings_window->isVisible();
    m_settings_window->close();
    m_settings_window->deleteLater();
    m_settings_window = nullptr;

    if (had_settings_window)
      doSettings();
  }

  if (controller && m_controller_settings_window)
  {
    const bool had_controller_settings_window = m_controller_settings_window->isVisible();
    m_controller_settings_window->close();
    m_controller_settings_window->deleteLater();
    m_controller_settings_window = nullptr;

    if (had_controller_settings_window)
      doControllerSettings(ControllerSettingsWindow::Category::GlobalSettings);
  }

  updateDebugMenuVisibility();
}

void MainWindow::saveDisplayWindowGeometryToConfig()
{
  QWidget* const container = getDisplayContainer();
  if (container->windowState() & Qt::WindowFullScreen)
  {
    // if we somehow ended up here, don't save the fullscreen state to the config
    return;
  }

  const char* key = m_display_widget->windowPositionKey();
  if (key)
    QtUtils::SaveWindowGeometry(key, container);
}

void MainWindow::restoreDisplayWindowGeometryFromConfig()
{
  QWidget* const container = getDisplayContainer();
  DebugAssert(m_display_widget);

  // just sync it with the main window if we're not using nogui modem, config will be stale
  if (QtHost::CanRenderToMainWindow())
  {
    container->setGeometry(geometry());
    return;
  }

  // we don't want the temporary windowed window to be positioned on a different monitor, so use the main window
  // coordinates... unless you're on wayland, too fucking bad, broken by design.
  const bool use_main_window_pos = QtHost::UseMainWindowGeometryForDisplayWindow();
  m_display_widget->setWindowPositionKey(use_main_window_pos ? "MainWindow" : "DisplayWindow");

  if (!QtUtils::RestoreWindowGeometry(m_display_widget->windowPositionKey(), container))
  {
    // default size
    container->resize(640, 480);
  }
}

SettingsWindow* MainWindow::getSettingsWindow()
{
  if (!m_settings_window)
  {
    m_settings_window = new SettingsWindow();
    connect(m_settings_window->getInterfaceSettingsWidget(), &InterfaceSettingsWidget::themeChanged, this,
            &MainWindow::onSettingsThemeChanged);
  }

  return m_settings_window;
}

void MainWindow::doSettings(const char* category /* = nullptr */)
{
  SettingsWindow* dlg = getSettingsWindow();
  QtUtils::ShowOrRaiseWindow(dlg);
  if (category)
    dlg->setCategory(category);
}

void MainWindow::openGamePropertiesForCurrentGame(const char* category /* = nullptr */)
{
  if (!s_system_valid)
    return;

  auto lock = GameList::GetLock();
  const GameList::Entry* entry = GameList::GetEntryForPath(s_current_game_path.toStdString());
  if (entry && entry->disc_set_member && !entry->dbentry->IsFirstDiscInSet() &&
      !System::ShouldUseSeparateDiscSettingsForSerial(entry->serial))
  {
    // show for first disc instead
    entry = GameList::GetFirstDiscSetMember(entry->dbentry->disc_set);
  }
  if (!entry)
  {
    lock.unlock();
    QtUtils::MessageBoxCritical(this, tr("Error"), tr("Game properties is only available for scanned games."));
    return;
  }

  SettingsWindow::openGamePropertiesDialog(entry, category);
}

ControllerSettingsWindow* MainWindow::getControllerSettingsWindow()
{
  if (!m_controller_settings_window)
    m_controller_settings_window = new ControllerSettingsWindow();

  return m_controller_settings_window;
}

MemoryEditorWindow* MainWindow::getMemoryEditorWindow()
{
  if (!m_memory_editor_window)
  {
    m_memory_editor_window = new MemoryEditorWindow();
    connect(m_memory_editor_window, &MemoryEditorWindow::closed, this, [this]() {
      m_memory_editor_window->deleteLater();
      m_memory_editor_window = nullptr;
    });
  }

  return m_memory_editor_window;
}

void MainWindow::doControllerSettings(
  ControllerSettingsWindow::Category category /*= ControllerSettingsDialog::Category::Count*/)
{
  ControllerSettingsWindow* dlg = getControllerSettingsWindow();
  QtUtils::ShowOrRaiseWindow(dlg);
  if (category != ControllerSettingsWindow::Category::Count)
    dlg->setCategory(category);
}

void MainWindow::onViewChangeGameListBackgroundTriggered()
{
  const QString path = QDir::toNativeSeparators(
    QFileDialog::getOpenFileName(this, tr("Select Background Image"), QString(), tr(IMAGE_FILTER)));
  if (path.isEmpty())
    return;

  m_game_list_widget->setBackgroundPath(QDir::toNativeSeparators(path).toStdString());
  m_ui.actionClearGameListBackground->setEnabled(true);
}

void MainWindow::onViewClearGameListBackgroundTriggered()
{
  m_game_list_widget->setBackgroundPath({});
  m_ui.actionClearGameListBackground->setEnabled(false);
}

void MainWindow::onSettingsTriggeredFromToolbar()
{
  if (s_system_valid)
    m_settings_toolbar_menu->popup(QCursor::pos());
  else
    doSettings();
}

void MainWindow::onSettingsControllerProfilesTriggered()
{
  if (!m_input_profile_editor_window)
    m_input_profile_editor_window = new ControllerSettingsWindow(nullptr, true);

  QtUtils::ShowOrRaiseWindow(m_input_profile_editor_window);
}

void MainWindow::openInputProfileEditor(const std::string_view name)
{
  if (!m_input_profile_editor_window)
    m_input_profile_editor_window = new ControllerSettingsWindow(nullptr, true);

  QtUtils::ShowOrRaiseWindow(m_input_profile_editor_window);
  m_input_profile_editor_window->switchProfile(name);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
  // If there's no VM, we can just exit as normal.
  // When recreating, g_main_window will be null at this point.
  if (!QtHost::IsSystemValidOrStarting() || !g_main_window)
  {
    QtUtils::SaveWindowGeometry("MainWindow", this);

    // surfaceless for language change
    if (s_fullscreen_ui_started && g_main_window)
      g_emu_thread->stopFullscreenUI();

    destroySubWindows();
    QMainWindow::closeEvent(event);
    return;
  }

  // But if there is, we have to cancel the action, regardless of whether we ended exiting
  // or not. The window still needs to be visible while the system shuts down.
  event->ignore();

  requestShutdown(true, true, g_settings.save_state_on_exit, true, true, true, true);
}

void MainWindow::changeEvent(QEvent* event)
{
  if (event->type() == QEvent::WindowStateChange &&
      static_cast<QWindowStateChangeEvent*>(event)->oldState() & Qt::WindowMinimized)
  {
    // TODO: This should check the render-to-main option.
    if (isRenderingToMain())
      g_emu_thread->redrawDisplayWindow();
  }

  if (event->type() == QEvent::StyleChange)
  {
    QtHost::UpdateThemeOnStyleChange();
    QtUtils::StyleChildMenus(this);
    emit themeChanged(QtHost::IsDarkApplicationTheme());
  }

  QMainWindow::changeEvent(event);
}

static QString getFilenameFromMimeData(const QMimeData* md)
{
  QString filename;
  if (md->hasUrls())
  {
    // only one url accepted
    const QList<QUrl> urls(md->urls());
    if (urls.size() == 1)
      filename = QDir::toNativeSeparators(urls.front().toLocalFile());
  }

  return filename;
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
  const std::string filename(getFilenameFromMimeData(event->mimeData()).toStdString());
  if (!System::IsLoadablePath(filename) && !System::IsSaveStatePath(filename))
    return;

  event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event)
{
  const QString qfilename(getFilenameFromMimeData(event->mimeData()));
  const std::string filename(qfilename.toStdString());
  if (!System::IsLoadablePath(filename) && !System::IsSaveStatePath(filename))
    return;

  event->acceptProposedAction();

  if (System::IsSaveStatePath(filename))
  {
    g_emu_thread->loadState(qfilename);
    return;
  }

  if (s_system_valid)
    promptForDiscChange(qfilename);
  else
    startFileOrChangeDisc(qfilename);
}

void MainWindow::moveEvent(QMoveEvent* event)
{
  QMainWindow::moveEvent(event);

  if (g_log_window && g_log_window->isAttachedToMainWindow())
    g_log_window->reattachToMainWindow();
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
  QMainWindow::resizeEvent(event);

  if (g_log_window && g_log_window->isAttachedToMainWindow())
    g_log_window->reattachToMainWindow();
}

void MainWindow::startupUpdateCheck()
{
  if (!Host::GetBaseBoolSettingValue("AutoUpdater", "CheckAtStartup", true))
    return;

  checkForUpdates(false);
}

void MainWindow::updateDebugMenuVisibility()
{
  const bool visible = QtHost::ShouldShowDebugOptions();
  m_ui.menuDebug->menuAction()->setVisible(visible);
}

void MainWindow::refreshGameList(bool invalidate_cache)
{
  m_game_list_widget->refresh(invalidate_cache);
}

void MainWindow::refreshGameListModel()
{
  m_game_list_widget->getModel()->refresh();
}

void MainWindow::cancelGameListRefresh()
{
  m_game_list_widget->cancelRefresh();
}

QIcon MainWindow::getIconForGame(const QString& path)
{
  return m_game_list_widget->getModel()->getIconForGame(path);
}

void MainWindow::runOnUIThread(const std::function<void()>& func)
{
  func();
}

void MainWindow::requestShutdown(bool allow_confirm, bool allow_save_to_state, bool save_state, bool check_safety,
                                 bool check_pause, bool exit_fullscreen_ui, bool quit_afterwards)
{
  if (!QtHost::IsSystemValidOrStarting())
  {
    if (exit_fullscreen_ui && s_fullscreen_ui_started)
      g_emu_thread->stopFullscreenUI();

    if (quit_afterwards)
      quit();

    return;
  }

  // If we don't have a serial, we can't save state.
  allow_save_to_state &= !s_current_game_serial.isEmpty();
  save_state &= allow_save_to_state;

  // Only confirm on UI thread because we need to display a msgbox.
  if (!m_is_closing && s_system_valid && allow_confirm && Host::GetBoolSettingValue("Main", "ConfirmPowerOff", true))
  {
    // Hardcore mode restrictions.
    if (check_pause && !s_system_paused && s_achievements_hardcore_mode && allow_confirm)
    {
      Host::RunOnCPUThread(
        [allow_confirm, allow_save_to_state, save_state, check_safety, exit_fullscreen_ui, quit_afterwards]() {
          if (!System::CanPauseSystem(true))
            return;

          Host::RunOnUIThread(
            [allow_confirm, allow_save_to_state, save_state, check_safety, exit_fullscreen_ui, quit_afterwards]() {
              g_main_window->requestShutdown(allow_confirm, allow_save_to_state, save_state, check_safety, false,
                                             exit_fullscreen_ui, quit_afterwards);
            });
        });

      return;
    }

    SystemLock lock(pauseAndLockSystem());

    QMessageBox* msgbox =
      QtUtils::NewMessageBox(QMessageBox::Question, quit_afterwards ? tr("Confirm Exit") : tr("Confirm Close"),
                             quit_afterwards ? tr("Are you sure you want to exit the application?") :
                                               tr("Are you sure you want to close the current game?"),
                             QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes, lock.getDialogParent());

    QCheckBox* const save_cb = new QCheckBox(tr("Save State For Resume"), msgbox);
    save_cb->setChecked(allow_save_to_state && save_state);
    save_cb->setEnabled(allow_save_to_state);
    msgbox->setCheckBox(save_cb);
    connect(msgbox, &QMessageBox::finished, this,
            [this, lock = std::move(lock), save_cb, allow_save_to_state, check_safety, check_pause, exit_fullscreen_ui,
             quit_afterwards](int result) mutable {
              if (result != QMessageBox::Yes)
                return;

              // Don't switch back to fullscreen when we're shutting down anyway.
              if (!QtHost::IsFullscreenUIStarted())
                lock.cancelResume();

              const bool save_state = save_cb->isChecked();
              requestShutdown(false, allow_save_to_state, save_state, check_safety, check_pause, exit_fullscreen_ui,
                              quit_afterwards);
            });
    msgbox->open();
    return;
  }

  // If we're running in batch mode, don't show the main window after shutting down.
  if (quit_afterwards || QtHost::InBatchMode())
  {
    INFO_LOG("Setting pending main window close flag.");
    m_is_closing = true;
  }

  // If we're still starting, shut down first. Otherwise the FSUI shutdown will block until it finishes.
  if (s_system_starting)
    System::CancelPendingStartup();

  // Stop fullscreen UI from reopening if requested.
  if (exit_fullscreen_ui && s_fullscreen_ui_started)
    g_emu_thread->stopFullscreenUI();

  // Now we can actually shut down the VM.
  g_emu_thread->shutdownSystem(save_state, check_safety);
}

void MainWindow::requestExit(bool allow_confirm /* = true */)
{
  // this is block, because otherwise closeEvent() will also prompt
  requestShutdown(allow_confirm, true, g_settings.save_state_on_exit, true, true, true, true);
}

void MainWindow::checkForSettingChanges()
{
  if (const bool disable_window_rounded_corners =
        Host::GetBaseBoolSettingValue("Main", "DisableWindowRoundedCorners", false);
      disable_window_rounded_corners != s_disable_window_rounded_corners)
  {
    s_disable_window_rounded_corners = disable_window_rounded_corners;
    PlatformMisc::SetWindowRoundedCornerState(reinterpret_cast<void*>(winId()), !s_disable_window_rounded_corners);

    if (QWidget* container = getDisplayContainer(); container && !container->parent() && !container->isFullScreen())
    {
      PlatformMisc::SetWindowRoundedCornerState(reinterpret_cast<void*>(container->winId()),
                                                !s_disable_window_rounded_corners);
    }
  }

  // don't change state if temporary unfullscreened
  if (m_display_widget && !QtHost::IsSystemLocked() && !isRenderingFullscreen())
  {
    if (QtHost::CanRenderToMainWindow() != isRenderingToMain())
      g_emu_thread->updateDisplayWindow();
  }

  LogWindow::updateSettings();

  // don't refresh window state while setup wizard is running, i.e. no game and hidden
  if (isVisible() || s_system_valid || s_system_starting)
    updateWindowState();
}

std::optional<WindowInfo> MainWindow::getWindowInfo()
{
  if (!m_display_widget || isRenderingToMain())
    return QtUtils::GetWindowInfoForWidget(this, RenderAPI::None);
  else if (QWidget* widget = getDisplayContainer())
    return QtUtils::GetWindowInfoForWidget(widget, RenderAPI::None);
  else
    return std::nullopt;
}

void MainWindow::onCheckForUpdatesActionTriggered()
{
  // Wipe out the last version, that way it displays the update if we've previously skipped it.
  Host::DeleteBaseSettingValue("AutoUpdater", "LastVersion");
  Host::CommitBaseSettingChanges();
  checkForUpdates(true);
}

void MainWindow::openMemoryCardEditor(const QString& card_a_path, const QString& card_b_path)
{
  bool any_cards_exist = false;

  for (const QString& card_path : {card_a_path, card_b_path})
  {
    if (!card_path.isEmpty())
    {
      if (QFile::exists(card_path))
      {
        any_cards_exist = true;
      }
      else if (QtUtils::MessageBoxQuestion(
                 this, tr("Memory Card Not Found"),
                 tr("Memory card '%1' does not exist. Do you want to create an empty memory card?").arg(card_path),
                 QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes)
      {
        Error error;
        if (MemoryCardEditorWindow::createMemoryCard(card_path, &error))
        {
          any_cards_exist = true;
        }
        else
        {
          QtUtils::MessageBoxCritical(this, tr("Memory Card Not Found"),
                                      tr("Failed to create memory card '%1': %2")
                                        .arg(card_path)
                                        .arg(QString::fromStdString(error.GetDescription())));
        }
      }
    }
  }

  // don't open the editor if no cards exist and we requested one
  if (!any_cards_exist && (!card_a_path.isEmpty() || !card_b_path.isEmpty()))
    return;

  if (!m_memory_card_editor_window)
    m_memory_card_editor_window = new MemoryCardEditorWindow();

  QtUtils::ShowOrRaiseWindow(m_memory_card_editor_window);

  if (!card_a_path.isEmpty())
  {
    if (!m_memory_card_editor_window->setCardA(card_a_path))
    {
      QtUtils::MessageBoxCritical(
        this, tr("Memory Card Not Found"),
        tr("Memory card '%1' could not be found. Try starting the game and saving to create it.").arg(card_a_path));
    }
  }
  if (!card_b_path.isEmpty())
  {
    if (!m_memory_card_editor_window->setCardB(card_b_path))
    {
      QtUtils::MessageBoxCritical(
        this, tr("Memory Card Not Found"),
        tr("Memory card '%1' could not be found. Try starting the game and saving to create it.").arg(card_b_path));
    }
  }
}

void MainWindow::onAchievementsLoginRequested(Achievements::LoginRequestReason reason)
{
  auto lock = pauseAndLockSystem();

  AchievementLoginDialog* dlg = new AchievementLoginDialog(lock.getDialogParent(), reason);
  connect(dlg, &AchievementLoginDialog::finished, this, [lock = std::move(lock)]() {});
  dlg->open();
}

void MainWindow::onAchievementsLoginSuccess(const QString& username, quint32 points, quint32 sc_points,
                                            quint32 unread_messages)
{
  m_ui.statusBar->showMessage(tr("RA: Logged in as %1 (%2, %3 softcore). %4 unread messages.")
                                .arg(username)
                                .arg(points)
                                .arg(sc_points)
                                .arg(unread_messages));

  // Automatically show the achievements column after first login. If the user has manually hidden it,
  // it will not be automatically shown again.
  if (!Host::GetBaseBoolSettingValue("GameListTableView", "TriedShowingAchievementsColumn", false))
  {
    Host::SetBaseBoolSettingValue("GameListTableView", "TriedShowingAchievementsColumn", true);
    m_game_list_widget->getListView()->setAndSaveColumnHidden(GameListModel::Column_Achievements, false);
  }
}

void MainWindow::onAchievementsActiveChanged(bool active)
{
  m_ui.actionViewRefreshAchievementProgress->setEnabled(active);
}

void MainWindow::onAchievementsHardcoreModeChanged(bool enabled)
{
  if (enabled)
  {
    QtUtils::CloseAndDeleteWindow(m_debugger_window);
    QtUtils::CloseAndDeleteWindow(m_memory_editor_window);
    QtUtils::CloseAndDeleteWindow(m_memory_scanner_window);
  }

  s_achievements_hardcore_mode = enabled;
  updateEmulationActions(s_system_starting, s_system_valid, enabled);
}

void MainWindow::onAchievementsAllProgressRefreshed()
{
  m_ui.statusBar->showMessage(tr("RA: Updated achievement progress database."));
}

bool MainWindow::onCreateAuxiliaryRenderWindow(RenderAPI render_api, qint32 x, qint32 y, quint32 width, quint32 height,
                                               const QString& title, const QString& icon_name,
                                               Host::AuxiliaryRenderWindowUserData userdata,
                                               Host::AuxiliaryRenderWindowHandle* handle, WindowInfo* wi, Error* error)
{
  AuxiliaryDisplayWidget* widget = AuxiliaryDisplayWidget::create(x, y, width, height, title, icon_name, userdata);
  if (!widget)
    return false;

  if (s_disable_window_rounded_corners)
    PlatformMisc::SetWindowRoundedCornerState(reinterpret_cast<void*>(widget->winId()), false);

  const std::optional<WindowInfo> owi = QtUtils::GetWindowInfoForWidget(widget, render_api, error);
  if (!owi.has_value())
  {
    widget->destroy();
    return false;
  }

  *handle = widget;
  *wi = owi.value();
  return true;
}

void MainWindow::onDestroyAuxiliaryRenderWindow(Host::AuxiliaryRenderWindowHandle handle, QPoint* pos, QSize* size)
{
  AuxiliaryDisplayWidget* widget = static_cast<AuxiliaryDisplayWidget*>(handle);
  DebugAssert(widget);

  *pos = widget->pos();
  *size = widget->size();
  widget->destroy();
}

void MainWindow::onToolsMemoryCardEditorTriggered()
{
  openMemoryCardEditor(QString(), QString());
}

void MainWindow::onToolsCoverDownloaderTriggered()
{
  // This can be invoked via big picture, so exit fullscreen.
  // Wait for the fullscreen request to actually go through, otherwise the downloader appears behind the main window.
  exitFullscreen(true);

  if (!m_cover_download_window)
  {
    m_cover_download_window = new CoverDownloadWindow();
    connect(m_cover_download_window, &CoverDownloadWindow::coverRefreshRequested, m_game_list_widget,
            &GameListWidget::refreshGridCovers);
    connect(m_cover_download_window, &CoverDownloadWindow::closed, this, [this]() {
      m_cover_download_window->deleteLater();
      m_cover_download_window = nullptr;
    });
  }

  QtUtils::ShowOrRaiseWindow(m_cover_download_window);
}

void MainWindow::onToolsMediaCaptureToggled(bool checked)
{
  if (!s_system_valid)
  {
    // leave it for later, we'll fill in the boot params
    return;
  }

  if (!checked)
  {
    Host::RunOnCPUThread(&System::StopMediaCapture);
    return;
  }

  const std::string container =
    Host::GetStringSettingValue("MediaCapture", "Container", Settings::DEFAULT_MEDIA_CAPTURE_CONTAINER);
  const QString qcontainer = QString::fromStdString(container);
  const QString filter(tr("%1 Files (*.%2)").arg(qcontainer.toUpper()).arg(qcontainer));

  QString path =
    QString::fromStdString(System::GetNewMediaCapturePath(QtHost::GetCurrentGameTitle().toStdString(), container));
  path = QDir::toNativeSeparators(QFileDialog::getSaveFileName(this, tr("Media Capture"), path, filter));
  if (path.isEmpty())
  {
    // uncheck it again
    QSignalBlocker sb(m_ui.actionMediaCapture);
    m_ui.actionMediaCapture->setChecked(false);
    return;
  }

  Host::RunOnCPUThread([path = path.toStdString()]() { System::StartMediaCapture(path); });
}

void MainWindow::onToolsMemoryEditorTriggered()
{
  if (s_achievements_hardcore_mode)
    return;

  QtUtils::ShowOrRaiseWindow(getMemoryEditorWindow());
}

void MainWindow::onToolsMemoryScannerTriggered()
{
  if (s_achievements_hardcore_mode)
    return;

  if (!m_memory_scanner_window)
  {
    m_memory_scanner_window = new MemoryScannerWindow();
    connect(m_memory_scanner_window, &MemoryScannerWindow::closed, this, [this]() {
      m_memory_scanner_window->deleteLater();
      m_memory_scanner_window = nullptr;
    });
  }

  QtUtils::ShowOrRaiseWindow(m_memory_scanner_window);
}

void MainWindow::onToolsISOBrowserTriggered()
{
  ISOBrowserWindow* ib = new ISOBrowserWindow();
  ib->setAttribute(Qt::WA_DeleteOnClose);
  ib->show();
}

void MainWindow::openCPUDebugger()
{
  if (s_achievements_hardcore_mode)
    return;

  if (!m_debugger_window)
  {
    m_debugger_window = new DebuggerWindow();
    connect(m_debugger_window, &DebuggerWindow::closed, this, [this]() {
      m_debugger_window->deleteLater();
      m_debugger_window = nullptr;
    });
  }

  QtUtils::ShowOrRaiseWindow(m_debugger_window);
}

void MainWindow::onToolsOpenDataDirectoryTriggered()
{
  QtUtils::OpenURL(this, QUrl::fromLocalFile(QString::fromStdString(EmuFolders::DataRoot)));
}

void MainWindow::onToolsOpenTextureDirectoryTriggered()
{
  QString dir = QString::fromStdString(EmuFolders::Textures);
  if (s_system_valid && !s_current_game_serial.isEmpty())
    dir = QStringLiteral("%1" FS_OSPATH_SEPARATOR_STR "%2").arg(dir).arg(s_current_game_serial);

  QtUtils::OpenURL(this, QUrl::fromLocalFile(dir));
}

void MainWindow::checkForUpdates(bool display_message)
{
  if (!AutoUpdaterWindow::isSupported())
  {
    if (display_message)
    {
      QMessageBox mbox(this);
      mbox.setWindowTitle(tr("Updater Error"));
      mbox.setTextFormat(Qt::RichText);

      QString message;
      if (!AutoUpdaterWindow::isOfficialBuild())
      {
        message =
          tr("<p>Sorry, you are trying to update a DuckStation version which is not an official GitHub release. To "
             "prevent incompatibilities, the auto-updater is only enabled on official builds.</p>"
             "<p>Please download an official release from from <a "
             "href=\"https://www.duckstation.org/\">duckstation.org</a>.</p>");
      }
      else
      {
        message = tr("Automatic updating is not supported on the current platform.");
      }

      mbox.setText(message);
      mbox.setIcon(QMessageBox::Critical);
      mbox.exec();
    }

    return;
  }

  if (m_auto_updater_dialog)
  {
    QtUtils::ShowOrRaiseWindow(m_auto_updater_dialog);
    return;
  }

  m_auto_updater_dialog = new AutoUpdaterWindow();
  connect(m_auto_updater_dialog, &AutoUpdaterWindow::updateCheckCompleted, this,
          [this] { QtUtils::CloseAndDeleteWindow(m_auto_updater_dialog); });
  m_auto_updater_dialog->queueUpdateCheck(display_message);
}

void* MainWindow::getNativeWindowId()
{
  return (void*)winId();
}

void MainWindow::onDebugLogChannelsMenuAboutToShow()
{
  m_ui.menuLogChannels->clear();
  LogWindow::populateFilterMenu(m_ui.menuLogChannels);
}

MainWindow::SystemLock MainWindow::pauseAndLockSystem()
{
  // To switch out of fullscreen when displaying a popup, or not to?
  // For Windows, with driver's direct scanout, what renders behind tends to be hit and miss.
  // We can't draw anything over exclusive fullscreen, so get out of it in that case.
  // Wayland's a pain as usual, we need to recreate the window, which means there'll be a brief
  // period when there's no window, and Qt might shut us down. So avoid it there.
  // On MacOS, it forces a workspace switch, which is kinda jarring.

#ifndef __APPLE__
  const bool was_fullscreen = g_emu_thread->isFullscreen();
#else
  const bool was_fullscreen = false;
#endif
  const bool was_paused = !s_system_valid || s_system_paused;

  // Have to do this early to avoid making the main window visible.
  s_system_locked.fetch_add(1, std::memory_order_release);

  // We need to switch out of exclusive fullscreen before we can display our popup.
  // However, we do not want to switch back to render-to-main, the window might have generated this event.
  if (was_fullscreen)
  {
    g_emu_thread->setFullscreen(false);

    // Container could change... thanks Wayland.
    QtUtils::ProcessEventsWithSleep(QEventLoop::ExcludeUserInputEvents, [this]() { return isRenderingFullscreen(); });
  }

  if (!was_paused)
  {
    g_emu_thread->setSystemPaused(true);

    // Need to wait for the pause to go through, and make the main window visible if needed.
    QtUtils::ProcessEventsWithSleep(QEventLoop::ExcludeUserInputEvents, []() { return !s_system_paused; });

    // Ensure it's visible before we try to create any dialogs parented to us.
    QApplication::sync();
  }

  // Now we'll either have a borderless window, or a regular window (if we were exclusive fullscreen).
  QWidget* dialog_parent = getDisplayContainer();
  if (!dialog_parent || dialog_parent->parent())
    dialog_parent = this;

  return SystemLock(dialog_parent, was_paused, was_fullscreen);
}

MainWindow::SystemLock::SystemLock(QWidget* dialog_parent, bool was_paused, bool was_fullscreen)
  : m_dialog_parent(dialog_parent), m_was_paused(was_paused), m_was_fullscreen(was_fullscreen), m_valid(true)
{
}

MainWindow::SystemLock::SystemLock(SystemLock&& lock)
  : m_dialog_parent(lock.m_dialog_parent), m_was_paused(lock.m_was_paused), m_was_fullscreen(lock.m_was_fullscreen),
    m_valid(true)
{
  Assert(lock.m_valid);
  lock.m_dialog_parent = nullptr;
  lock.m_was_paused = true;
  lock.m_was_fullscreen = false;
  lock.m_valid = false;
}

MainWindow::SystemLock::~SystemLock()
{
  if (!m_valid)
    return;

  DebugAssert(s_system_locked.load(std::memory_order_relaxed) > 0);
  s_system_locked.fetch_sub(1, std::memory_order_release);
  if (m_was_fullscreen)
    g_emu_thread->setFullscreen(true);
  if (!m_was_paused)
    g_emu_thread->setSystemPaused(false);
}

void MainWindow::SystemLock::cancelResume()
{
  m_was_paused = true;
  m_was_fullscreen = false;
}

bool QtHost::IsSystemLocked()
{
  return (s_system_locked.load(std::memory_order_acquire) > 0);
}

#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION

#include "core/achievements.h"
#include "core/achievements_private.h"

#include "rc_client_raintegration.h"

void MainWindow::onRAIntegrationMenuChanged()
{
  const auto lock = Achievements::GetLock();

  if (!Achievements::IsUsingRAIntegration())
  {
    if (m_raintegration_menu)
    {
      m_ui.menuBar->removeAction(m_raintegration_menu->menuAction());
      m_raintegration_menu->deleteLater();
      m_raintegration_menu = nullptr;
    }

    return;
  }

  if (!m_raintegration_menu)
  {
    m_raintegration_menu = new QMenu(QStringLiteral("&RAIntegration"));
    QtUtils::StylePopupMenu(m_raintegration_menu);
    m_ui.menuBar->insertMenu(m_ui.menuDebug->menuAction(), m_raintegration_menu);
  }

  m_raintegration_menu->clear();

  const rc_client_raintegration_menu_t* menu = rc_client_raintegration_get_menu(Achievements::GetClient());
  if (!menu)
    return;

  for (const rc_client_raintegration_menu_item_t& item :
       std::span<const rc_client_raintegration_menu_item_t>(menu->items, menu->num_items))
  {
    if (item.id == 0)
    {
      m_raintegration_menu->addSeparator();
      continue;
    }

    QAction* action = m_raintegration_menu->addAction(QString::fromUtf8(item.label));
    action->setEnabled(item.enabled != 0);
    action->setCheckable(item.checked != 0);
    action->setChecked(item.checked != 0);
    connect(action, &QAction::triggered, this, [id = item.id]() {
      const auto lock = Achievements::GetLock();
      if (!Achievements::IsUsingRAIntegration())
        return;

      rc_client_raintegration_activate_menu_item(Achievements::GetClient(), id);
    });
  }
}

void MainWindow::notifyRAIntegrationOfWindowChange()
{
  HWND hwnd = static_cast<HWND>((void*)winId());

  {
    const auto lock = Achievements::GetLock();
    if (!Achievements::IsUsingRAIntegration())
      return;

    rc_client_raintegration_update_main_window_handle(Achievements::GetClient(), hwnd);
  }

  onRAIntegrationMenuChanged();
}

void Host::OnRAIntegrationMenuChanged()
{
  QMetaObject::invokeMethod(g_main_window, &MainWindow::onRAIntegrationMenuChanged, Qt::QueuedConnection);
}

#else // RC_CLIENT_SUPPORTS_RAINTEGRATION

void MainWindow::onRAIntegrationMenuChanged()
{
  // has to be stubbed out because otherwise moc won't find it
}

void MainWindow::notifyRAIntegrationOfWindowChange()
{
}

#endif // RC_CLIENT_SUPPORTS_RAINTEGRATION
