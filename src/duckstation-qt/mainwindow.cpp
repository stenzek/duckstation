// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "mainwindow.h"
#include "aboutdialog.h"
#include "achievementlogindialog.h"
#include "autoupdaterdialog.h"
#include "controllersettingswindow.h"
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
#include "qtprogresscallback.h"
#include "qtutils.h"
#include "qtwindowinfo.h"
#include "selectdiscdialog.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"

#include "core/achievements.h"
#include "core/cheats.h"
#include "core/core.h"
#include "core/game_list.h"
#include "core/host.h"
#include "core/memory_card.h"
#include "core/settings.h"
#include "core/system.h"

#include "util/cd_image.h"
#include "util/gpu_device.h"
#include "util/translation.h"

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
#include <QtGui/QGuiApplication>
#include <QtGui/QShortcut>
#include <QtGui/QWindowStateChangeEvent>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QStyleFactory>
#include <cmath>

#include "moc_mainwindow.cpp"

using namespace Qt::StringLiterals;

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
  {"Reset", &Ui::MainWindow::actionRestartGame},
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
namespace {
struct MainWindowLocals
{
  QString current_game_title;
  QString current_game_serial;
  QString current_game_path;
  QIcon current_game_icon;
  std::optional<std::time_t> undo_state_timestamp;
  std::atomic_uint32_t system_locked{false};
  bool system_starting = false;
  bool system_valid = false;
  bool system_paused = false;
  bool achievements_hardcore_mode = false;
  bool fullscreen_ui_started = false;

#ifdef _WIN32
  bool disable_window_rounded_corners = false;
#endif
};
} // namespace

ALIGN_TO_CACHE_LINE static MainWindowLocals s_locals;

bool QtHost::IsSystemPaused()
{
  return s_locals.system_paused;
}

bool QtHost::IsSystemValid()
{
  return s_locals.system_valid;
}

bool QtHost::IsSystemValidOrStarting()
{
  return (s_locals.system_starting || s_locals.system_valid);
}

bool QtHost::IsFullscreenUIStarted()
{
  return s_locals.fullscreen_ui_started;
}

const QString& QtHost::GetCurrentGameTitle()
{
  return s_locals.current_game_title;
}

const QString& QtHost::GetCurrentGameSerial()
{
  return s_locals.current_game_serial;
}

const QString& QtHost::GetCurrentGamePath()
{
  return s_locals.current_game_path;
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
}

void MainWindow::initialize()
{
  m_ui.setupUi(this);
  setupAdditionalUi();
  updateToolbarActions();
  updateToolbarIconStyle();
  updateToolbarArea();
  updateEmulationActions();
  updateDisplayRelatedActions();
  updateWindowTitle();
  connectSignals();

  switchToGameListView();
}

QMenu* MainWindow::createPopupMenu()
{
  return nullptr;
}

void MainWindow::reportError(const QString& title, const QString& message)
{
  QtUtils::AsyncMessageBox(this, QMessageBox::Critical, title, message);
}

void MainWindow::onStatusMessage(const QString& message)
{
  // display as OSD message if fullscreen
  if (isRenderingFullscreen())
    Host::AddOSDMessage(OSDMessageType::Info, message.toStdString());
  else
    m_ui.statusBar->showMessage(message);
}

std::optional<WindowInfo> MainWindow::acquireRenderWindow(RenderAPI render_api, bool fullscreen,
                                                          bool exclusive_fullscreen, Error* error)
{
  const bool render_to_main =
    canRenderToMainWindow() && !fullscreen && (s_locals.system_locked.load(std::memory_order_relaxed) == 0);

  DEV_LOG("acquireRenderWindow() fullscreen={} exclusive_fullscreen={}, render_to_main={}", fullscreen,
          exclusive_fullscreen, render_to_main);

  QWidget* container =
    m_display_container ? static_cast<QWidget*>(m_display_container) : static_cast<QWidget*>(m_display_widget);
  const bool is_fullscreen = isRenderingFullscreen();
  const bool is_rendering_to_main = isRenderingToMain();

  // Always update exclusive fullscreen state, it controls main window visibility
  m_exclusive_fullscreen_requested = exclusive_fullscreen;

  // Skip recreating the surface if we're just transitioning between fullscreen and windowed with render-to-main off.
  // We also need to unparent the display widget from the container when switching to fullscreen on Wayland.
  const bool needs_container = (QtHost::IsDisplayWidgetContainerNeeded() && !fullscreen && !is_rendering_to_main);
  if (container && !is_rendering_to_main && !render_to_main && (m_display_container != nullptr) == needs_container)
  {
    DEV_LOG("Toggling to {} without recreating surface", (fullscreen ? "fullscreen" : "windowed"));
    m_exclusive_fullscreen_requested = exclusive_fullscreen;

    // in case it gets a new native handle
    m_display_widget->clearWindowInfo();

    // ensure it's resizable when changing size, we'll fix it up later in updateWindowState()
    QtUtils::SetWindowResizeable(container, true);

    // since we don't destroy the display widget, we need to save it here
    if (!is_fullscreen && !is_rendering_to_main)
      saveRenderWindowGeometryToConfig();

    if (fullscreen)
    {
      container->showFullScreen();
    }
    else
    {
      container->showNormal();
      restoreRenderWindowGeometryFromConfig();
    }

// See note below.
#if !defined(_WIN32) && !defined(__APPLE__)
    QGuiApplication::sync();
    QGuiApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
#endif

    updateDisplayRelatedActions();
    updateDisplayWidgetCursor();
    m_display_widget->setFocus();
    updateWindowState();

    return m_display_widget->getWindowInfo(render_api, error);
  }

  destroyDisplayWidget();

  createDisplayWidget(fullscreen, render_to_main);

  // We want to avoid nested event loops as much as possible because it's problematic on MacOS.
  // I removed the sync/processEvents() here for this reason, except of course fucking Linux throws
  // a wrench in the plan. On Windows and MacOS, calling show() and showFullscreen() will send resize
  // events with the correct size before returning. On Linux with X11 and Wankland, it doesn't.
  // So we have to force a processEvents() here to ensure the display widget is the correct size,
  // otherwise we'll see a glitched frame at the windowed size when starting fullscreen. Linux is
  // the odd one out again, as usual. Note: QGuiApplication::sync() is supposed to pump events
  // before and after syncing, but it seems this alone is not sufficient for getting the resize.
#if !defined(_WIN32) && !defined(__APPLE__)
  QGuiApplication::sync();
  QGuiApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
#endif

  const std::optional<WindowInfo>& wi = m_display_widget->getWindowInfo(render_api, error);
  if (!wi.has_value())
  {
    destroyDisplayWidget();
    return std::nullopt;
  }

  g_core_thread->connectRenderWindowSignals(m_display_widget);

  updateWindowTitle();
  updateWindowState();
  updateLogWidget();

  return wi;
}

bool MainWindow::canRenderToMainWindow() const
{
  return !Core::GetBoolSettingValue("Main", "RenderToSeparateWindow", false) && !QtHost::InNoGUIMode();
}

bool MainWindow::useMainWindowGeometryForRenderWindow() const
{
  // nogui _or_ main window mode, since we want to use it for temporary unfullscreens
  return !Core::GetBoolSettingValue("Main", "RenderToSeparateWindow", false) || QtHost::InNoGUIMode();
}

bool MainWindow::wantsLogWidget() const
{
  return (wantsDisplayWidget() && Core::GetBoolSettingValue("Main", "RenderToSeparateWindow", false) &&
          !Core::GetBaseBoolSettingValue("Main", "HideMainWindowWhenRunning", false) &&
          Core::GetBoolSettingValue("Main", "DisplayLogInMainWindow", false));
}

bool MainWindow::wantsDisplayWidget() const
{
  // big picture or system created
  return (QtHost::IsSystemValidOrStarting() || s_locals.fullscreen_ui_started);
}

bool MainWindow::hasDisplayWidget() const
{
  return (m_display_widget != nullptr);
}

void MainWindow::createDisplayWidget(bool fullscreen, bool render_to_main)
{
  // If we're rendering to main and were hidden (e.g. coming back from fullscreen),
  // make sure we're visible before trying to add ourselves. Otherwise Wayland breaks.
  if (!fullscreen && render_to_main && !isVisible())
    setVisible(true);

  QWidget* container;
  if (!fullscreen && !render_to_main && QtHost::IsDisplayWidgetContainerNeeded())
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
    if (isVisible() && canRenderToMainWindow())
      container->move(pos());
    else
      restoreRenderWindowGeometryFromConfig();

    container->showFullScreen();
  }
  else if (!render_to_main)
  {
    restoreRenderWindowGeometryFromConfig();
    container->showNormal();

#ifdef _WIN32
    if (s_locals.disable_window_rounded_corners)
      QtUtils::SetWindowRoundedCornerState(container, false);
#endif
  }
  else
  {
    AssertMsg(m_ui.mainContainer->indexOf(m_display_widget) < 0, "Has no display widget");
    m_ui.mainContainer->addWidget(m_display_widget);
    m_ui.mainContainer->setCurrentWidget(m_display_widget);
    m_ui.actionViewSystemDisplay->setChecked(true);
  }

  updateDisplayRelatedActions();
  updateShortcutActions();
  updateDisplayWidgetCursor();

  if (!render_to_main)
    QtUtils::ShowOrRaiseWindow(m_display_widget->window());
  m_display_widget->setFocus();
}

void MainWindow::displayResizeRequested(qint32 width, qint32 height)
{
  if (!m_display_widget || isRenderingFullscreen())
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
  destroyDisplayWidget();
  updateWindowTitle();
  updateWindowState();
  updateLogWidget();
}

void MainWindow::destroyDisplayWidget()
{
  if (m_display_widget)
  {
    m_display_widget->clearWindowInfo();

    if (!isRenderingFullscreen() && !isRenderingToMain())
      saveRenderWindowGeometryToConfig();

    if (m_display_container)
      m_display_container->removeDisplayWidget();

    if (isRenderingToMain())
    {
      AssertMsg(m_ui.mainContainer->indexOf(m_display_widget) >= 0, "Display widget in stack");
      m_ui.mainContainer->removeWidget(m_display_widget);
      m_ui.mainContainer->setCurrentWidget(m_game_list_widget);
      if (m_game_list_widget->isShowingGameGrid())
        m_ui.actionViewGameGrid->setChecked(true);
      else
        m_ui.actionViewGameList->setChecked(true);
      m_game_list_widget->setFocus();
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

  updateDisplayRelatedActions();
  updateShortcutActions();
}

void MainWindow::updateDisplayWidgetCursor()
{
  // may be temporarily surfaceless
  if (!m_display_widget)
    return;

  m_display_widget->updateRelativeMode(s_locals.system_valid && !s_locals.system_paused && m_relative_mouse_mode);
  m_display_widget->updateCursor(s_locals.system_valid && !s_locals.system_paused && shouldHideMouseCursor());
}

void MainWindow::updateDisplayRelatedActions()
{
  const bool fullscreen = isRenderingFullscreen();

  // rendering to main, or switched to gamelist/grid
  m_ui.actionViewSystemDisplay->setEnabled(isRenderingToMain());
  m_ui.actionViewSystemLog->setEnabled(m_log_widget != nullptr);
  m_ui.menuWindowSize->setEnabled(s_locals.system_valid && !s_locals.system_starting && m_display_widget &&
                                  !fullscreen);
  m_ui.actionFullscreen->setEnabled(m_display_widget && !s_locals.system_starting);
  m_ui.actionFullscreen->setChecked(fullscreen);

  updateGameListRelatedActions();
}

void MainWindow::updateGameListRelatedActions()
{
  const bool running = !isShowingGameList();
  const bool disable = (s_locals.system_starting || (running && isRenderingToMain()));

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
  m_ui.actionPreferAchievementGameIcons->setDisabled(disable || !game_list);
  m_ui.actionChangeGameListBackground->setDisabled(disable);
  m_ui.actionClearGameListBackground->setDisabled(disable || !has_background);
}

void MainWindow::updateLogWidget()
{
  const bool has_log_widget = (m_log_widget != nullptr);
  const bool wants_log_widget = wantsLogWidget();
  if (has_log_widget == wants_log_widget)
    return;

  m_ui.actionViewSystemLog->setEnabled(wants_log_widget);

  if (has_log_widget && !wants_log_widget)
  {
    // avoid focusing log widget
    if (!isRenderingToMain())
      switchToGameListView(false);

    DEV_COLOR_LOG(StrongMagenta, "Removing main window log widget");
    m_ui.mainContainer->removeWidget(m_log_widget);
    QtUtils::SafeDeleteWidget(m_log_widget);
  }
  else if (!has_log_widget && wants_log_widget)
  {
    DEV_COLOR_LOG(StrongMagenta, "Creating main window log widget");
    m_log_widget = new LogWidget(this);
    m_ui.mainContainer->addWidget(m_log_widget);
    DebugAssert(m_ui.mainContainer->indexOf(m_log_widget) >= 0);
    switchToEmulationView();
  }
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
  s_locals.system_starting = true;
  s_locals.system_valid = false;
  s_locals.system_paused = false;

  updateLogWidget();
  switchToEmulationView();
  updateEmulationActions();
  updateDisplayRelatedActions();
}

void MainWindow::onSystemStarted()
{
  m_was_disc_change_request = false;
  s_locals.system_starting = false;
  s_locals.system_valid = true;

  updateEmulationActions();
  updateDisplayRelatedActions();
  updateWindowTitle();
  updateStatusBarWidgetVisibility();
  updateDisplayWidgetCursor();
}

void MainWindow::onSystemPaused()
{
  m_ui.actionPause->setChecked(true);
  s_locals.system_paused = true;
  updateStatusBarWidgetVisibility();
  m_ui.statusBar->showMessage(tr("Paused"));
  updateDisplayWidgetCursor();
}

void MainWindow::onSystemResumed()
{
  m_ui.actionPause->setChecked(false);
  s_locals.system_paused = false;
  m_was_disc_change_request = false;
  m_ui.statusBar->clearMessage();
  updateStatusBarWidgetVisibility();
  updateDisplayWidgetCursor();
  if (m_display_widget)
    m_display_widget->setFocus();
}

void MainWindow::onSystemStopping()
{
  m_ui.actionPause->setChecked(false);
  s_locals.system_starting = false;
  s_locals.system_valid = false;
  s_locals.system_paused = false;
  s_locals.undo_state_timestamp.reset();

  updateEmulationActions();
  updateDisplayRelatedActions();
  updateStatusBarWidgetVisibility();
}

void MainWindow::onSystemDestroyed()
{
  Assert(!QtHost::IsSystemValidOrStarting());
  updateLogWidget();

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
  s_locals.current_game_path = path;
  s_locals.current_game_title = game_title;
  s_locals.current_game_serial = game_serial;
  s_locals.current_game_icon = getIconForGame(path);

  updateWindowTitle();
  updateEmulationActions();
}

void MainWindow::onSystemUndoStateAvailabilityChanged(bool available, quint64 timestamp)
{
  if (!available)
    s_locals.undo_state_timestamp.reset();
  else
    s_locals.undo_state_timestamp = timestamp;
}

void MainWindow::onMediaCaptureStarted()
{
  m_ui.actionMediaCapture->setChecked(true);
}

void MainWindow::onMediaCaptureStopped()
{
  m_ui.actionMediaCapture->setChecked(false);
}

void MainWindow::onStartFileActionTriggered()
{
  QString filename = QDir::toNativeSeparators(
    QFileDialog::getOpenFileName(this, tr("Select Disc Image"), QString(), tr(DISC_IMAGE_FILTER), nullptr));
  if (filename.isEmpty())
    return;

  startFileOrChangeDisc(filename);
}

void MainWindow::openSelectDiscDialog(const QString& title, std::function<void(std::string)> callback)
{
  std::vector<std::pair<std::string, std::string>> devices = CDImage::GetDeviceList();
  if (devices.empty())
  {
    QtUtils::AsyncMessageBox(
      this, QMessageBox::Critical, title,
      tr("Could not find any CD-ROM devices. Please ensure you have a CD-ROM drive connected and "
         "sufficient permissions to access it."));
    return;
  }

  // if there's only one, select it automatically
  if (devices.size() == 1)
  {
    callback(std::move(devices.front().first));
    return;
  }

  QStringList input_options;
  for (const auto& [path, name] : devices)
    input_options.append(tr("%1 (%2)").arg(QString::fromStdString(name)).arg(QString::fromStdString(path)));

  QInputDialog* input_dialog = new QInputDialog(this);
  input_dialog->setWindowTitle(title);
  input_dialog->setLabelText(tr("Select disc drive:"));
  input_dialog->setInputMode(QInputDialog::TextInput);
  input_dialog->setOptions(QInputDialog::UseListViewForComboBoxItems);
  input_dialog->setComboBoxEditable(false);
  input_dialog->setComboBoxItems(std::move(input_options));
  input_dialog->connect(input_dialog, &QInputDialog::accepted, this,
                        [input_dialog, callback = std::move(callback), devices = std::move(devices)]() mutable {
                          const qsizetype selected_index =
                            input_dialog->comboBoxItems().indexOf(input_dialog->textValue());
                          if (selected_index < 0 || static_cast<u32>(selected_index) >= devices.size())
                            return;

                          callback(std::move(devices[selected_index].first));
                        });
  input_dialog->open();
}

void MainWindow::quit()
{
  // Make sure VM is gone. It really should be if we're here.
  if (QtHost::IsSystemValidOrStarting())
  {
    g_core_thread->shutdownSystem(false, false);
    QtUtils::ProcessEventsWithSleep(QEventLoop::ExcludeUserInputEvents, &QtHost::IsSystemValidOrStarting);
  }

  // Big picture might still be active.
  if (s_locals.fullscreen_ui_started)
    g_core_thread->stopFullscreenUI();

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
  int controller_settings_window_row = 0;
  if (m_settings_window && m_settings_window->isVisible())
  {
    settings_window_pos = m_settings_window->pos();
    settings_window_row = m_settings_window->getCategoryRow();
  }
  if (m_controller_settings_window && m_controller_settings_window->isVisible())
  {
    controller_settings_window_pos = m_controller_settings_window->pos();
    controller_settings_window_row = m_controller_settings_window->getCategoryRow();
  }

  // Remove subwindows before switching to surfaceless, because otherwise e.g. the debugger can cause funkyness.
  destroySubWindows();

  // Ensure the main window is visible, otherwise last-window-closed terminates the application.
  if (!isVisible())
    show();

  // We need to close input sources, because e.g. DInput uses our window handle.
  Host::RunOnCoreThread(&InputManager::CloseSources, true);

  // Ensure we don't get a display widget creation sent to us.
  const bool was_display_created = hasDisplayWidget();
  QObject::disconnect(g_core_thread, nullptr, this, nullptr);

  // Create new window.
  g_main_window = nullptr;
  MainWindow* new_main_window = new MainWindow();
  DebugAssert(g_main_window == new_main_window);
  new_main_window->setGeometry(geometry());

  // Recreate log window as well. Then make sure we're still on top.
  LogWindow::updateSettings(true);

  // Qt+XCB will ignore the raise request of the settings window if we raise the main window.
  // So skip that if we're going to be re-opening the settings window.
  if (!settings_window_pos.has_value())
    QtUtils::ShowOrRaiseWindow(new_main_window);
  else
    new_main_window->show();

  LogWindow::deferredShow();

  if (was_display_created)
  {
    g_core_thread->recreateRenderWindow();
    QtUtils::ProcessEventsWithSleep(QEventLoop::ExcludeUserInputEvents,
                                    []() { return !g_main_window->hasDisplayWidget(); });
    g_main_window->updateEmulationActions();
    g_main_window->onFullscreenUIStartedOrStopped(s_locals.fullscreen_ui_started);
  }

  // New window ready to go, close out the old one.
  destroyDisplayWidget();
  close();
  deleteLater();

  // Reload the sources we just closed.
  Host::RunOnCoreThread(&System::ReloadInputSources);

  if (controller_settings_window_pos.has_value())
  {
    ControllerSettingsWindow* dlg = g_main_window->getControllerSettingsWindow();
    dlg->move(controller_settings_window_pos.value());
    dlg->setCategoryRow(controller_settings_window_row);
    dlg->show();
  }
  if (settings_window_pos.has_value())
  {
    SettingsWindow* dlg = g_main_window->getSettingsWindow();
    dlg->move(settings_window_pos.value());
    dlg->setCategoryRow(settings_window_row);
    QtUtils::ShowOrRaiseWindow(dlg);
  }
  else
  {
    QtUtils::RaiseWindow(new_main_window);
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

        action->setDisabled(s_locals.achievements_hardcore_mode);
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
      paths[i] = QString::fromStdString(System::GetGameMemoryCardPath(entry->title, entry->serial, entry->path, i));

    g_main_window->openMemoryCardEditor(paths[0], paths[1]);
  });

  if (!entry->IsDiscSet())
  {
    const bool has_any_states = resume_action->isEnabled() || load_state_menu->isEnabled();
    QAction* delete_save_states_action = menu->addAction(tr("Delete Save States"));
    delete_save_states_action->setEnabled(has_any_states);
    if (has_any_states)
    {
      connect(delete_save_states_action, &QAction::triggered, [parent_window, serial = entry->serial]() mutable {
        QMessageBox* const msgbox = QtUtils::NewMessageBox(
          parent_window, QMessageBox::Warning, tr("Confirm Save State Deletion"),
          tr("Are you sure you want to delete all save states for %1?\n\nThe saves will not be recoverable.")
            .arg(QString::fromStdString(serial)),
          QMessageBox::Yes | QMessageBox::No);
        connect(msgbox, &QMessageBox::accepted,
                [serial = std::move(serial)]() { System::DeleteSaveStates(serial, true); });
        msgbox->open();
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
      connect(load_action, &QAction::triggered, this, [path]() { g_core_thread->loadState(path); });
    }
  };

  menu->clear();

  connect(menu->addAction(tr("Load From File...")), &QAction::triggered, []() {
    const QString path = QDir::toNativeSeparators(
      QFileDialog::getOpenFileName(g_main_window, tr("Select Save State File"), QString(), tr("Save States (*.sav)")));
    if (path.isEmpty())
      return;

    g_core_thread->loadState(path);
  });
  QAction* load_from_state =
    menu->addAction(s_locals.undo_state_timestamp.has_value() ?
                      tr("Undo Load State (%1)")
                        .arg(QtHost::FormatNumber(Host::NumberFormatType::ShortDateTime,
                                                  static_cast<s64>(s_locals.undo_state_timestamp.value()))) :
                      tr("Undo Load State"));
  load_from_state->setEnabled(s_locals.undo_state_timestamp.has_value());
  connect(load_from_state, &QAction::triggered, g_core_thread, &CoreThread::undoLoadState);
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
            [global = serial.empty(), slot]() { g_core_thread->saveState(global, slot); });
  };

  menu->clear();

  connect(menu->addAction(tr("Save To File...")), &QAction::triggered, []() {
    if (!System::IsValid())
      return;

    const QString path = QDir::toNativeSeparators(
      QFileDialog::getSaveFileName(g_main_window, tr("Select Save State File"), QString(), tr("Save States (*.sav)")));
    if (path.isEmpty())
      return;

    g_core_thread->saveState(QDir::toNativeSeparators(path));
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

  Host::RunOnCoreThread([menu = m_ui.menuCheats]() {
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
          Host::RunOnCoreThread([name = name.toStdString()]() {
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

  return openResumeStateDialog(std::move(save_state_path));
}

bool MainWindow::openResumeStateDialog(std::string save_state_path)
{
  if (s_locals.system_valid)
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

  connect(load, &QPushButton::clicked, [this, dlg, path = ssi->media_path, save_state_path]() mutable {
    startFile(std::move(path), std::move(save_state_path), std::nullopt);
    dlg->accept();
  });
  connect(boot, &QPushButton::clicked, [this, dlg, path = ssi->media_path]() mutable {
    startFile(std::move(path), std::nullopt, std::nullopt);
    dlg->accept();
  });
  connect(delboot, &QPushButton::clicked,
          [this, dlg, path = ssi->media_path, save_state_path = std::move(save_state_path)]() mutable {
            if (!FileSystem::DeleteFile(save_state_path.c_str()))
            {
              QtUtils::MessageBoxCritical(
                this, tr("Error"),
                tr("Failed to delete save state file '%1'.").arg(QString::fromStdString(save_state_path)));
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

  g_core_thread->bootSystem(std::move(params));
}

void MainWindow::startFileOrChangeDisc(const QString& qpath)
{
  if (s_locals.system_valid)
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
  if (m_was_disc_change_request)
  {
    switchToEmulationView();
    g_core_thread->changeDisc(path, false, true);
    return;
  }

  SystemLock lock(pauseAndLockSystem());

  QMessageBox* const mb =
    QtUtils::NewMessageBox(lock.getDialogParent(), QMessageBox::Question, tr("Confirm Disc Change"),
                           tr("Do you want to swap discs or boot the new image via system restart?"),
                           QMessageBox::NoButton, QMessageBox::NoButton);

  /*const QAbstractButton* const swap_button = */ mb->addButton(tr("Swap Disc"), QMessageBox::YesRole);
  const QAbstractButton* const restart_button = mb->addButton(tr("Restart"), QMessageBox::NoRole);
  const QAbstractButton* const cancel_button =
    mb->addButton(qApp->translate("QPlatformTheme", "Cancel"), QMessageBox::RejectRole);

  connect(mb, &QMessageBox::finished, this, [this, mb, restart_button, cancel_button, path, lock = std::move(lock)]() {
    const QAbstractButton* const clicked_button = mb->clickedButton();
    if (!clicked_button || clicked_button == cancel_button)
      return;

    const bool restart_system = (clicked_button == restart_button);
    switchToEmulationView();

    g_core_thread->changeDisc(path, restart_system, true);
  });

  mb->open();
}

void MainWindow::onStartDiscActionTriggered()
{
  openSelectDiscDialog(tr("Start Disc"), [](std::string path) mutable {
    g_core_thread->bootSystem(g_main_window->getSystemBootParameters(std::move(path)));
  });
}

void MainWindow::onStartBIOSActionTriggered()
{
  g_core_thread->bootSystem(getSystemBootParameters(std::string()));
}

void MainWindow::onResumeLastStateActionTriggered()
{
  std::string state_path = System::GetMostRecentResumeSaveStatePath();
  if (state_path.empty())
  {
    reportError(tr("Error"), tr("No resume save state found."));
    return;
  }

  openResumeStateDialog(std::move(state_path));
}

void MainWindow::onChangeDiscFromFileActionTriggered()
{
  QString filename = QDir::toNativeSeparators(
    QFileDialog::getOpenFileName(this, tr("Select Disc Image"), QString(), tr(DISC_IMAGE_FILTER), nullptr));
  if (filename.isEmpty())
    return;

  g_core_thread->changeDisc(filename, false, true);
}

void MainWindow::onChangeDiscFromGameListActionTriggered()
{
  m_was_disc_change_request = true;
  switchToGameListView();
}

void MainWindow::onChangeDiscFromDeviceActionTriggered()
{
  openSelectDiscDialog(tr("Change Disc"),
                       [](std::string path) { g_core_thread->changeDisc(QString::fromStdString(path), false, true); });
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

  if (!s_locals.system_valid)
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
      connect(action, &QAction::triggered, [i]() { g_core_thread->changeDiscFromPlaylist(i); });
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
      action->setChecked(path == s_locals.current_game_path);
      connect(action, &QAction::triggered,
              [path = std::move(path)]() { g_core_thread->changeDisc(path, false, true); });
    }
  }
}

void MainWindow::onLoadStateMenuAboutToShow()
{
  populateLoadStateMenu(s_locals.current_game_serial.toStdString(), m_ui.menuLoadState);
}

void MainWindow::onSaveStateMenuAboutToShow()
{
  populateSaveStateMenu(s_locals.current_game_serial.toStdString(), m_ui.menuSaveState);
}

void MainWindow::onStartFullscreenUITriggered()
{
  if (m_display_widget)
    g_core_thread->stopFullscreenUI();
  else
    g_core_thread->startFullscreenUI();
}

void MainWindow::onFullscreenUIStartedOrStopped(bool running)
{
  s_locals.fullscreen_ui_started = running;
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

void MainWindow::onRestartGameActionTriggered()
{
  g_core_thread->resetSystem(true);
}

void MainWindow::onPauseActionTriggered(bool checked)
{
  if (s_locals.system_paused == checked)
    return;

  if (checked && s_locals.achievements_hardcore_mode)
  {
    // Need to check restrictions.
    Host::RunOnCoreThread([]() {
      if (System::CanPauseSystem(true))
      {
        g_core_thread->setSystemPaused(true);
      }
      else
      {
        // Restore action state.
        Host::RunOnUIThread([]() { g_main_window->m_ui.actionPause->setChecked(false); });
      }
    });
  }
  else
  {
    g_core_thread->setSystemPaused(checked);
  }
}

void MainWindow::onRemoveDiscActionTriggered()
{
  g_core_thread->changeDisc(QString(), false, true);
}

void MainWindow::onScanForNewGamesTriggered()
{
  refreshGameList(false);
}

void MainWindow::onViewToolbarActionTriggered(bool checked)
{
  Core::SetBaseBoolSettingValue("UI", "ShowToolbar", checked);
  Host::CommitBaseSettingChanges();
  m_ui.toolBar->setVisible(checked);
  updateToolbarIconStyle();
}

void MainWindow::onViewToolbarLockActionTriggered(bool checked)
{
  Core::SetBaseBoolSettingValue("UI", "LockToolbar", checked);
  Host::CommitBaseSettingChanges();
  m_ui.toolBar->setMovable(!checked);

  // ensure synced
  m_ui.actionViewLockToolbar->setChecked(checked);
}

void MainWindow::onViewToolbarSmallIconsActionTriggered(bool checked)
{
  Core::SetBaseBoolSettingValue("UI", "ToolbarSmallIcons", checked);
  Host::CommitBaseSettingChanges();
  updateToolbarIconStyle();

  // ensure synced
  m_ui.actionViewSmallToolbarIcons->setChecked(checked);
}

void MainWindow::onViewToolbarLabelsActionTriggered(bool checked)
{
  Core::SetBaseBoolSettingValue("UI", "ToolbarLabels", checked);
  Host::CommitBaseSettingChanges();
  updateToolbarIconStyle();

  // ensure synced
  m_ui.actionViewToolbarLabels->setChecked(checked);
}

void MainWindow::onViewToolbarLabelsBesideIconsActionTriggered(bool checked)
{
  Core::SetBaseBoolSettingValue("UI", "ToolbarLabelsBesideIcons", checked);
  Host::CommitBaseSettingChanges();
  updateToolbarIconStyle();

  // ensure synced
  m_ui.actionViewToolbarLabelsBesideIcons->setChecked(checked);
}

void MainWindow::onViewStatusBarActionTriggered(bool checked)
{
  Core::SetBaseBoolSettingValue("UI", "ShowStatusBar", checked);
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

  if (s_locals.system_valid)
  {
    // change disc on double click
    if (!entry->IsDisc())
    {
      QtUtils::AsyncMessageBox(this, QMessageBox::Critical, tr("Error"), tr("You must select a disc to change discs."));
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
            QtUtils::CenterWindowRelativeToParent(ib, this);
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

      if (!s_locals.system_valid)
      {
        populateGameListContextMenu(entry, this, menu);
        menu->addSeparator();

        menu->addAction(tr("Default Boot"), [this, qpath]() mutable {
          g_core_thread->bootSystem(getSystemBootParameters(qpath.toStdString()));
        });

        menu->addAction(tr("Fast Boot"), [this, qpath]() mutable {
          std::shared_ptr<SystemBootParameters> boot_params = getSystemBootParameters(qpath.toStdString());
          boot_params->override_fast_boot = true;
          g_core_thread->bootSystem(std::move(boot_params));
        });

        menu->addAction(tr("Full Boot"), [this, qpath]() mutable {
          std::shared_ptr<SystemBootParameters> boot_params = getSystemBootParameters(qpath.toStdString());
          boot_params->override_fast_boot = false;
          g_core_thread->bootSystem(std::move(boot_params));
        });

        if (m_ui.menuDebug->menuAction()->isVisible())
        {
          menu->addAction(tr("Boot and Debug"), [this, qpath]() mutable {
            openCPUDebugger();

            std::shared_ptr<SystemBootParameters> boot_params = getSystemBootParameters(qpath.toStdString());
            boot_params->override_start_paused = true;
            boot_params->disable_achievements_hardcore_mode = true;
            g_core_thread->bootSystem(std::move(boot_params));
          });
        }
      }
      else
      {
        menu->addAction(tr("Change Disc"), [this, qpath]() {
          g_core_thread->changeDisc(qpath, false, true);
          g_core_thread->setSystemPaused(false);
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
      QtUtils::AsyncMessageBox(this, QMessageBox::Critical, tr("Copy Error"),
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
    QtUtils::AsyncMessageBox(this, QMessageBox::Critical, tr("Copy Error"),
                             tr("Failed to remove existing cover '%1'").arg(new_filename));
    return;
  }
  if (!QFile::copy(filename, new_filename))
  {
    QtUtils::AsyncMessageBox(this, QMessageBox::Critical, tr("Copy Error"),
                             tr("Failed to copy '%1' to '%2'").arg(filename).arg(new_filename));
    return;
  }
  if (!old_filename.isEmpty() && old_filename != new_filename && !QFile::remove(old_filename))
  {
    QtUtils::AsyncMessageBox(this, QMessageBox::Critical, tr("Copy Error"),
                             tr("Failed to remove '%1'").arg(old_filename));
    return;
  }

  m_game_list_widget->getModel()->invalidateColumnForPath(entry->path, GameListModel::Column_Cover);
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
  const bool status_bar_visible = Core::GetBaseBoolSettingValue("UI", "ShowStatusBar", true);
  m_ui.actionViewStatusBar->setChecked(status_bar_visible);
  m_ui.statusBar->setVisible(status_bar_visible);

  const bool toolbar_visible = Core::GetBaseBoolSettingValue("UI", "ShowToolbar", false);
  m_ui.actionViewToolbar->setChecked(toolbar_visible);
  m_ui.toolBar->setVisible(toolbar_visible);

  const bool toolbars_locked = Core::GetBaseBoolSettingValue("UI", "LockToolbar", false);
  m_ui.actionViewLockToolbar->setChecked(toolbars_locked);
  m_ui.toolBar->setMovable(!toolbars_locked);

  m_ui.actionViewSmallToolbarIcons->setChecked(Core::GetBaseBoolSettingValue("UI", "ToolbarSmallIcons", false));
  m_ui.actionViewToolbarLabels->setChecked(Core::GetBaseBoolSettingValue("UI", "ToolbarLabels", true));
  m_ui.actionViewToolbarLabelsBesideIcons->setChecked(
    Core::GetBaseBoolSettingValue("UI", "ToolbarLabelsBesideIcons", false));

  // mutually exclusive actions
  QActionGroup* group = new QActionGroup(this);
  group->addAction(m_ui.actionViewGameList);
  group->addAction(m_ui.actionViewGameGrid);
  group->addAction(m_ui.actionViewSystemDisplay);
  group->addAction(m_ui.actionViewSystemLog);

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
    ascending_action->setObjectName("SortAscending"_L1);
    m_ui.menuSortBy->addAction(ascending_action);
    connect(ascending_action, &QAction::triggered, this, &MainWindow::onViewSortOrderActionTriggered);

    QAction* const descending_action = new QAction(tr("&Descending"), order_group);
    descending_action->setIcon(QIcon::fromTheme(QIcon::ThemeIcon::GoDown));
    descending_action->setCheckable(true);
    descending_action->setChecked(current_sort_order == Qt::DescendingOrder);
    descending_action->setObjectName("SortDescending"_L1);
    m_ui.menuSortBy->addAction(descending_action);
    connect(descending_action, &QAction::triggered, this, &MainWindow::onViewSortOrderActionTriggered);
  }

  for (u32 scale = 1; scale <= 10; scale++)
  {
    QAction* const action = m_ui.menuWindowSize->addAction(tr("%1x Scale").arg(scale));
    connect(action, &QAction::triggered,
            [scale = static_cast<float>(scale)]() { g_core_thread->requestDisplaySize(scale); });
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

#ifdef _WIN32
  s_locals.disable_window_rounded_corners = Core::GetBaseBoolSettingValue("Main", "DisableWindowRoundedCorners", false);
  if (s_locals.disable_window_rounded_corners)
    QtUtils::SetWindowRoundedCornerState(this, false);
#endif

  QtUtils::StyleChildMenus(this);
}

void MainWindow::onGameListSortIndicatorOrderChanged(int column, Qt::SortOrder order)
{
  // yuck, allocations
  for (QAction* const action : m_ui.menuSortBy->actions())
  {
    bool activate = false;

    if (action->objectName() == "SortAscending"_L1)
      activate = (order == Qt::AscendingOrder);
    else if (action->objectName() == "SortDescending"_L1)
      activate = (order == Qt::DescendingOrder);
    else
      activate = (action->data() == column);

    if (activate)
      action->setChecked(true);
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
    Core::GetBaseStringSettingValue("UI", "ToolbarButtons", DEFAULT_TOOLBAR_ACTIONS);
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
    if (action == m_ui.actionCloseGame && !s_locals.system_valid)
      action = m_ui.actionResumeLastState;

    m_ui.toolBar->addAction(action);
    any_items_before_separator = true;
  }
}

void MainWindow::updateToolbarIconStyle()
{
  const bool show_toolbar = Core::GetBaseBoolSettingValue("UI", "ShowToolbar", false);
  const bool show_labels = Core::GetBaseBoolSettingValue("UI", "ToolbarLabels", true);
  const bool small_icons = Core::GetBaseBoolSettingValue("UI", "ToolbarSmallIcons", false);
  const bool labels_beside_icons = Core::GetBaseBoolSettingValue("UI", "ToolbarLabelsBesideIcons", false);

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
  const TinyString cfg_name = Core::GetBaseTinyStringSettingValue("UI", "ToolbarArea", "Top");
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
  if (Core::GetBaseBoolSettingValue("UI", "ShowToolbar", false))
    m_ui.toolBar->show();
}

void MainWindow::onToolbarContextMenuRequested(const QPoint& pos)
{
  {
    const bool show_labels = Core::GetBaseBoolSettingValue("UI", "ToolbarLabels", true);

    const std::string active_buttons_str =
      Core::GetBaseStringSettingValue("UI", "ToolbarButtons", DEFAULT_TOOLBAR_ACTIONS);
    std::vector<std::string_view> active_buttons = StringUtil::SplitString(active_buttons_str, ',');

    QMenu* const menu = QtUtils::NewPopupMenu(this);

    QAction* action = menu->addAction(tr("Lock Toolbar"));
    action->setCheckable(true);
    action->setChecked(!m_ui.toolBar->isMovable());
    connect(action, &QAction::triggered, this, &MainWindow::onViewToolbarLockActionTriggered);

    action = menu->addAction(tr("Small Icons"));
    action->setCheckable(true);
    action->setChecked(Core::GetBaseBoolSettingValue("UI", "ToolbarSmallIcons", false));
    connect(action, &QAction::triggered, this, &MainWindow::onViewToolbarSmallIconsActionTriggered);

    action = menu->addAction(tr("Show Labels"));
    action->setCheckable(true);
    action->setChecked(Core::GetBaseBoolSettingValue("UI", "ToolbarLabels", true));
    connect(action, &QAction::triggered, this, &MainWindow::onViewToolbarLabelsActionTriggered);

    action = menu->addAction(tr("Labels Beside Icons"));
    action->setCheckable(true);
    action->setChecked(Core::GetBaseBoolSettingValue("UI", "ToolbarLabelsBesideIcons", false));
    action->setEnabled(show_labels);
    connect(action, &QAction::triggered, this, &MainWindow::onViewToolbarLabelsBesideIconsActionTriggered);

    QMenu* const position_menu = menu->addMenu(tr("Position"));
    QtUtils::StylePopupMenu(position_menu);
    for (const auto& [area, name] : s_toolbar_areas)
    {
      QAction* const position_action = position_menu->addAction(tr(name), [this, name]() {
        Core::SetBaseStringSettingValue("UI", "ToolbarArea", name);
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
      connect(menu_action, &QAction::triggered, this, [this, name](bool checked) {
        const std::string active_buttons_str =
          Core::GetBaseStringSettingValue("UI", "ToolbarButtons", DEFAULT_TOOLBAR_ACTIONS);
        std::vector<std::string_view> active_buttons = StringUtil::SplitString(active_buttons_str, ',');
        if (checked ? StringUtil::AddToStringList(active_buttons, name) :
                      StringUtil::RemoveFromStringList(active_buttons, name))
        {
          Core::SetBaseStringSettingValue("UI", "ToolbarButtons", StringUtil::JoinString(active_buttons, ',').c_str());
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
      Core::SetBaseStringSettingValue("UI", "ToolbarArea", name);
      Host::CommitBaseSettingChanges();
      break;
    }
  }
}

void MainWindow::updateEmulationActions()
{
  const bool starting = s_locals.system_starting;
  const bool starting_or_running = (starting || s_locals.system_valid);
  const bool starting_or_not_running = (starting || !s_locals.system_valid);
  const bool achievements_hardcore_mode = s_locals.achievements_hardcore_mode;
  m_ui.actionStartFile->setDisabled(starting_or_running);
  m_ui.actionStartDisc->setDisabled(starting_or_running);
  m_ui.actionStartBios->setDisabled(starting_or_running);
  m_ui.actionResumeLastState->setDisabled(starting_or_running || achievements_hardcore_mode);
  m_ui.actionStartFullscreenUI->setDisabled(starting_or_running);
  m_ui.actionStartFullscreenUI2->setDisabled(starting_or_running);

  m_ui.actionCloseGame->setDisabled(starting_or_not_running);
  m_ui.actionCloseGameWithoutSaving->setDisabled(starting_or_not_running);
  m_ui.actionRestartGame->setDisabled(starting_or_not_running);
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

  updateShortcutActions();

  if (starting_or_running)
  {
    if (m_ui.toolBar->widgetForAction(m_ui.actionResumeLastState))
    {
      m_ui.toolBar->insertAction(m_ui.actionResumeLastState, m_ui.actionCloseGame);
      m_ui.toolBar->removeAction(m_ui.actionResumeLastState);
    }

    m_ui.actionViewGameProperties->setEnabled(
      !s_locals.current_game_path.isEmpty() && !s_locals.current_game_serial.isEmpty() &&
      GameList::CanEditGameSettingsForPath(s_locals.current_game_path.toStdString(),
                                           s_locals.current_game_serial.toStdString()));
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

void MainWindow::updateShortcutActions()
{
  const bool starting_or_running = s_locals.system_starting || s_locals.system_valid;
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

  Update(m_status_renderer_widget, s_locals.system_valid && !s_locals.system_paused, 0);
  Update(m_status_resolution_widget, s_locals.system_valid && !s_locals.system_paused, 0);
  Update(m_status_fps_widget, s_locals.system_valid && !s_locals.system_paused, 0);
  Update(m_status_vps_widget, s_locals.system_valid && !s_locals.system_paused, 0);
}

void MainWindow::updateWindowTitle()
{
  QString suffix(QtHost::GetAppConfigSuffix());
  QString main_title(QtHost::GetAppNameAndVersion() + suffix);
  QString display_title(s_locals.current_game_title + suffix);

  if (!s_locals.system_valid || s_locals.current_game_title.isEmpty())
    display_title = main_title;
  else if (isRenderingToMain())
    main_title = display_title;

  if (windowTitle() != main_title)
    setWindowTitle(main_title);
  setWindowIcon(s_locals.current_game_icon.isNull() ? QtHost::GetAppIcon() : s_locals.current_game_icon);

  if (m_display_widget && !isRenderingToMain())
  {
    QWidget* container =
      m_display_container ? static_cast<QWidget*>(m_display_container) : static_cast<QWidget*>(m_display_widget);
    if (container->windowTitle() != display_title)
      container->setWindowTitle(display_title);
    container->setWindowIcon(s_locals.current_game_icon.isNull() ? QtHost::GetAppIcon() : s_locals.current_game_icon);
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
  const bool resizeable = (!Core::GetBoolSettingValue("Main", "DisableWindowResize", false) || !wantsDisplayWidget() ||
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
  // avoid frequent updates by normalizing value first
  const int maximum = (total != 0) ? 100 : 0;
  const int value = (total != 0) ? ((current * 100) / total) : 0;
  if (m_status_progress_widget->maximum() != maximum)
    m_status_progress_widget->setMaximum(maximum);
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
  return (m_ui.mainContainer->currentWidget() == m_game_list_widget);
}

bool MainWindow::isRenderingFullscreen() const
{
  return m_display_widget && (m_exclusive_fullscreen_requested || getDisplayContainer()->isFullScreen());
}

bool MainWindow::isRenderingToMain() const
{
  return (m_display_widget && m_ui.mainContainer->indexOf(m_display_widget) >= 0);
}

bool MainWindow::shouldHideMouseCursor() const
{
  return m_hide_mouse_cursor ||
         (isRenderingFullscreen() && Core::GetBoolSettingValue("Main", "HideCursorInFullscreen", true));
}

bool MainWindow::shouldHideMainWindow() const
{
  // CanRenderToMain check is for temporary unfullscreens.
  return (!isRenderingToMain() && wantsDisplayWidget() &&
          ((Core::GetBoolSettingValue("Main", "RenderToSeparateWindow", false) &&
            Core::GetBoolSettingValue("Main", "HideMainWindowWhenRunning", false)) ||
           (canRenderToMainWindow() &&
            (isRenderingFullscreen() || s_locals.system_locked.load(std::memory_order_relaxed))))) ||
         QtHost::InNoGUIMode();
}

void MainWindow::switchToGameListView(bool pause_system /* = true */)
{
  // Normally, we'd never end up here. But on MacOS, the global menu is accessible while fullscreen.
  if (canRenderToMainWindow() && isRenderingFullscreen())
  {
    g_core_thread->setFullscreenWithCompletionHandler(false, []() { g_main_window->switchToGameListView(); });
    return;
  }

  if (isShowingGameList())
    return;

  m_was_paused_on_game_list_switch = s_locals.system_paused;
  if (!s_locals.system_paused && pause_system)
    g_core_thread->setSystemPaused(true);

  m_ui.mainContainer->setCurrentWidget(m_game_list_widget);
  if (m_game_list_widget->isShowingGameGrid())
    m_ui.actionViewGameGrid->setChecked(true);
  else
    m_ui.actionViewGameList->setChecked(true);
  m_game_list_widget->setFocus();

  updateShortcutActions();
}

void MainWindow::switchToEmulationView()
{
  if (isRenderingToMain())
  {
    if (!m_display_widget || m_ui.mainContainer->currentWidget() == m_display_widget)
      return;

    m_ui.mainContainer->setCurrentWidget(m_display_widget);
    m_ui.actionViewSystemDisplay->setChecked(true);

    // size of the widget might have changed, let it check itself
    m_display_widget->checkForSizeChange(false);
    m_display_widget->setFocus();
  }
  else
  {
    if (!m_log_widget || m_ui.mainContainer->currentWidget() == m_log_widget)
      return;

    m_ui.mainContainer->setCurrentWidget(m_log_widget);
    m_ui.actionViewSystemLog->setChecked(true);

    m_log_widget->setFocus();
  }

  // resume if we weren't paused at switch time
  if (s_locals.system_paused && !m_was_paused_on_game_list_switch)
    g_core_thread->setSystemPaused(false);

  updateShortcutActions();
}

void MainWindow::connectSignals()
{
  connect(m_ui.toolBar, &QToolBar::customContextMenuRequested, this, &MainWindow::onToolbarContextMenuRequested);
  connect(m_ui.toolBar, &QToolBar::topLevelChanged, this, &MainWindow::onToolbarTopLevelChanged);

  connect(m_ui.actionStartFile, &QAction::triggered, this, &MainWindow::onStartFileActionTriggered);
  connect(m_ui.actionStartDisc, &QAction::triggered, this, &MainWindow::onStartDiscActionTriggered);
  connect(m_ui.actionStartBios, &QAction::triggered, this, &MainWindow::onStartBIOSActionTriggered);
  connect(m_ui.actionResumeLastState, &QAction::triggered, this, &MainWindow::onResumeLastStateActionTriggered);
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
  connect(m_ui.actionRestartGame, &QAction::triggered, this, &MainWindow::onRestartGameActionTriggered);
  connect(m_ui.actionPause, &QAction::triggered, this, &MainWindow::onPauseActionTriggered);
  connect(m_ui.actionScreenshot, &QAction::triggered, g_core_thread, &CoreThread::saveScreenshot);
  connect(m_ui.actionScanForNewGames, &QAction::triggered, this, &MainWindow::onScanForNewGamesTriggered);
  connect(m_ui.actionRescanAllGames, &QAction::triggered, this, [this]() { refreshGameList(true); });
  connect(m_ui.actionLoadState, &QAction::triggered, this, [this]() { m_ui.menuLoadState->popup(QCursor::pos()); });
  connect(m_ui.actionSaveState, &QAction::triggered, this, [this]() { m_ui.menuSaveState->popup(QCursor::pos()); });
  connect(m_ui.actionExit, &QAction::triggered, this, &MainWindow::close);
  connect(m_ui.actionFullscreen, &QAction::triggered, g_core_thread, &CoreThread::toggleFullscreen);
  connect(m_ui.actionSettings, &QAction::triggered, [this]() { doSettings(); });
  connect(m_ui.actionSettings2, &QAction::triggered, this, &MainWindow::onSettingsTriggeredFromToolbar);
  connect(m_ui.actionInterfaceSettings, &QAction::triggered, [this]() { doSettings("Interface"); });
  connect(m_ui.actionBIOSSettings, &QAction::triggered, [this]() { doSettings("BIOS"); });
  connect(m_ui.actionConsoleSettings, &QAction::triggered, [this]() { doSettings("Console"); });
  connect(m_ui.actionEmulationSettings, &QAction::triggered, [this]() { doSettings("Emulation"); });
  connect(m_ui.actionGameListSettings, &QAction::triggered, [this]() { doSettings("Game List"); });
  connect(m_ui.actionHotkeySettings, &QAction::triggered,
          [this]() { doControllerSettings(ControllerSettingsWindow::CATEGORY_HOTKEY_SETTINGS); });
  connect(m_ui.actionControllerSettings, &QAction::triggered,
          [this]() { doControllerSettings(ControllerSettingsWindow::CATEGORY_GLOBAL_SETTINGS); });
  connect(m_ui.actionMemoryCardSettings, &QAction::triggered, [this]() { doSettings("Memory Cards"); });
  connect(m_ui.actionGraphicsSettings, &QAction::triggered, [this]() { doSettings("Graphics"); });
  connect(m_ui.actionOSDSettings, &QAction::triggered, [this]() { doSettings("On-Screen Display"); });
  connect(m_ui.actionPostProcessingSettings, &QAction::triggered, [this]() { doSettings("Post-Processing"); });
  connect(m_ui.actionAudioSettings, &QAction::triggered, [this]() { doSettings("Audio"); });
  connect(m_ui.actionAchievementSettings, &QAction::triggered, [this]() { doSettings("Achievements"); });
  connect(m_ui.actionFolderSettings, &QAction::triggered, [this]() { doSettings("Folders"); });
  connect(m_ui.actionCaptureSettings, &QAction::triggered, [this]() { doSettings("Capture"); });
  connect(m_ui.actionAdvancedSettings, &QAction::triggered, [this]() { doSettings("Advanced"); });
  connect(m_ui.actionControllerProfiles, &QAction::triggered, this, &MainWindow::onSettingsControllerProfilesTriggered);
  connect(m_ui.actionViewToolbar, &QAction::triggered, this, &MainWindow::onViewToolbarActionTriggered);
  connect(m_ui.actionViewLockToolbar, &QAction::triggered, this, &MainWindow::onViewToolbarLockActionTriggered);
  connect(m_ui.actionViewSmallToolbarIcons, &QAction::triggered, this,
          &MainWindow::onViewToolbarSmallIconsActionTriggered);
  connect(m_ui.actionViewToolbarLabels, &QAction::triggered, this, &MainWindow::onViewToolbarLabelsActionTriggered);
  connect(m_ui.actionViewToolbarLabelsBesideIcons, &QAction::triggered, this,
          &MainWindow::onViewToolbarLabelsBesideIconsActionTriggered);
  connect(m_ui.actionViewStatusBar, &QAction::triggered, this, &MainWindow::onViewStatusBarActionTriggered);
  connect(m_ui.actionViewGameList, &QAction::triggered, this, &MainWindow::onViewGameListActionTriggered);
  connect(m_ui.actionViewGameGrid, &QAction::triggered, this, &MainWindow::onViewGameGridActionTriggered);
  connect(m_ui.actionViewSystemDisplay, &QAction::triggered, this, &MainWindow::onViewSystemDisplayTriggered);
  connect(m_ui.actionViewSystemLog, &QAction::triggered, this, &MainWindow::onViewSystemDisplayTriggered);
  connect(m_ui.actionViewGameProperties, &QAction::triggered, this, [this]() { openGamePropertiesForCurrentGame(); });
  connect(m_ui.actionGitHubRepository, &QAction::triggered, this, &MainWindow::onGitHubRepositoryActionTriggered);
  connect(m_ui.actionDiscordServer, &QAction::triggered, this, &MainWindow::onDiscordServerActionTriggered);
  connect(m_ui.actionViewThirdPartyNotices, &QAction::triggered, this,
          [this]() { AboutDialog::openThirdPartyNotices(this); });
  connect(m_ui.actionAboutQt, &QAction::triggered, qApp, &QApplication::aboutQt);
  connect(m_ui.actionAbout, &QAction::triggered, this, &MainWindow::onAboutActionTriggered);
  connect(m_ui.actionCheckForUpdates, &QAction::triggered, this, [this]() { checkForUpdates(true, true); });
  connect(m_ui.actionMemoryCardEditor, &QAction::triggered, this, &MainWindow::onToolsMemoryCardEditorTriggered);
  connect(m_ui.actionMemoryEditor, &QAction::triggered, this, &MainWindow::onToolsMemoryEditorTriggered);
  connect(m_ui.actionMemoryScanner, &QAction::triggered, this, &MainWindow::onToolsMemoryScannerTriggered);
  connect(m_ui.actionISOBrowser, &QAction::triggered, this, &MainWindow::onToolsISOBrowserTriggered);
  connect(m_ui.actionControllerTest, &QAction::triggered, g_core_thread, &CoreThread::startControllerTest);
  connect(m_ui.actionCoverDownloader, &QAction::triggered, this, &MainWindow::onToolsCoverDownloaderTriggered);
  connect(m_ui.actionToolsDownloadAchievementGameIcons, &QAction::triggered, this,
          &MainWindow::onToolsDownloadAchievementGameIconsTriggered);
  connect(m_ui.actionToolsRefreshAchievementProgress, &QAction::triggered, g_main_window,
          &MainWindow::refreshAchievementProgress);
  connect(m_ui.actionMediaCapture, &QAction::triggered, this, &MainWindow::onToolsMediaCaptureTriggered);
  connect(m_ui.actionCaptureGPUFrame, &QAction::triggered, g_core_thread, &CoreThread::captureGPUFrameDump);
  connect(m_ui.actionCPUDebugger, &QAction::triggered, this, &MainWindow::openCPUDebugger);
  connect(m_ui.actionOpenDataDirectory, &QAction::triggered, this, &MainWindow::onToolsOpenDataDirectoryTriggered);
  connect(m_ui.actionOpenTextureDirectory, &QAction::triggered, this,
          &MainWindow::onToolsOpenTextureDirectoryTriggered);
  connect(m_ui.actionReloadTextureReplacements, &QAction::triggered, g_core_thread,
          &CoreThread::reloadTextureReplacements);
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
  connect(m_ui.actionGridViewRefreshCovers, &QAction::triggered, this,
          [this]() { m_game_list_widget->getModel()->invalidateColumn(GameListModel::Column_Cover); });
  connect(m_ui.actionChangeGameListBackground, &QAction::triggered, this,
          &MainWindow::onViewChangeGameListBackgroundTriggered);
  connect(m_ui.actionClearGameListBackground, &QAction::triggered, this,
          &MainWindow::onViewClearGameListBackgroundTriggered);

  connect(g_core_thread, &CoreThread::settingsResetToDefault, this, &MainWindow::onSettingsResetToDefault,
          Qt::QueuedConnection);
  connect(g_core_thread, &CoreThread::errorReported, this, &MainWindow::reportError);
  connect(g_core_thread, &CoreThread::statusMessage, this, &MainWindow::onStatusMessage);
  connect(g_core_thread, &CoreThread::onAcquireRenderWindowRequested, this, &MainWindow::acquireRenderWindow,
          Qt::BlockingQueuedConnection);
  connect(g_core_thread, &CoreThread::onReleaseRenderWindowRequested, this, &MainWindow::releaseRenderWindow);
  connect(g_core_thread, &CoreThread::onResizeRenderWindowRequested, this, &MainWindow::displayResizeRequested,
          Qt::BlockingQueuedConnection);
  connect(g_core_thread, &CoreThread::systemStarting, this, &MainWindow::onSystemStarting);
  connect(g_core_thread, &CoreThread::systemStarted, this, &MainWindow::onSystemStarted);
  connect(g_core_thread, &CoreThread::systemStopping, this, &MainWindow::onSystemStopping);
  connect(g_core_thread, &CoreThread::systemDestroyed, this, &MainWindow::onSystemDestroyed);
  connect(g_core_thread, &CoreThread::systemPaused, this, &MainWindow::onSystemPaused);
  connect(g_core_thread, &CoreThread::systemResumed, this, &MainWindow::onSystemResumed);
  connect(g_core_thread, &CoreThread::systemGameChanged, this, &MainWindow::onSystemGameChanged);
  connect(g_core_thread, &CoreThread::systemUndoStateAvailabilityChanged, this,
          &MainWindow::onSystemUndoStateAvailabilityChanged);
  connect(g_core_thread, &CoreThread::mediaCaptureStarted, this, &MainWindow::onMediaCaptureStarted);
  connect(g_core_thread, &CoreThread::mediaCaptureStopped, this, &MainWindow::onMediaCaptureStopped);
  connect(g_core_thread, &CoreThread::mouseModeRequested, this, &MainWindow::onMouseModeRequested);
  connect(g_core_thread, &CoreThread::fullscreenUIStartedOrStopped, this, &MainWindow::onFullscreenUIStartedOrStopped);
  connect(g_core_thread, &CoreThread::achievementsLoginRequested, this, &MainWindow::onAchievementsLoginRequested);
  connect(g_core_thread, &CoreThread::achievementsLoginSuccess, this, &MainWindow::onAchievementsLoginSuccess);
  connect(g_core_thread, &CoreThread::achievementsActiveChanged, this, &MainWindow::onAchievementsActiveChanged);
  connect(g_core_thread, &CoreThread::achievementsHardcoreModeChanged, this,
          &MainWindow::onAchievementsHardcoreModeChanged);
  connect(g_core_thread, &CoreThread::onCreateAuxiliaryRenderWindow, this, &MainWindow::onCreateAuxiliaryRenderWindow,
          Qt::BlockingQueuedConnection);
  connect(g_core_thread, &CoreThread::onDestroyAuxiliaryRenderWindow, this, &MainWindow::onDestroyAuxiliaryRenderWindow,
          Qt::BlockingQueuedConnection);
  connect(this, &MainWindow::themeChanged, g_core_thread, &CoreThread::updateFullscreenUITheme);

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

    g_core_thread->dumpRAM(filename);
  });
  connect(m_ui.actionDumpVRAM, &QAction::triggered, [this]() {
    const QString filename = QDir::toNativeSeparators(QFileDialog::getSaveFileName(
      this, tr("Destination File"), QString(), tr("Binary Files (*.bin);;PNG Images (*.png)")));
    if (filename.isEmpty())
      return;

    g_core_thread->dumpVRAM(filename);
  });
  connect(m_ui.actionDumpSPURAM, &QAction::triggered, [this]() {
    const QString filename = QDir::toNativeSeparators(
      QFileDialog::getSaveFileName(this, tr("Destination File"), QString(), tr("Binary Files (*.bin)")));
    if (filename.isEmpty())
      return;

    g_core_thread->dumpSPURAM(filename);
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
      doControllerSettings(0);
  }

  updateDebugMenuVisibility();
}

void MainWindow::saveRenderWindowGeometryToConfig()
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

void MainWindow::restoreRenderWindowGeometryFromConfig()
{
  QWidget* const container = getDisplayContainer();
  DebugAssert(m_display_widget);

  // just sync it with the main window if we're not using nogui modem, config will be stale
  if (canRenderToMainWindow())
  {
    container->setGeometry(geometry());
    return;
  }

  // we don't want the temporary windowed window to be positioned on a different monitor, so use the main window
  // coordinates... unless you're on wayland, too fucking bad, broken by design.
  const bool use_main_window_pos = useMainWindowGeometryForRenderWindow();
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
    m_settings_window = new SettingsWindow();

  return m_settings_window;
}

void MainWindow::doSettings(const char* category /* = nullptr */)
{
  SettingsWindow* window = getSettingsWindow();
  QtUtils::ShowOrRaiseWindow(window, this, true);
  if (category)
    window->setCategory(category);
}

void MainWindow::openGamePropertiesForCurrentGame(const char* category /* = nullptr */)
{
  if (!s_locals.system_valid)
    return;

  auto lock = GameList::GetLock();
  const std::string game_path = s_locals.current_game_path.toStdString();
  const GameList::Entry* entry = GameList::GetEntryForPath(game_path);
  if (entry && entry->disc_set_member && !entry->dbentry->IsFirstDiscInSet() &&
      !System::ShouldUseSeparateDiscSettingsForSerial(entry->serial))
  {
    // show for first disc instead
    entry = GameList::GetFirstDiscSetMember(entry->dbentry->disc_set);
  }

  // playlists will always contain the first disc's serial, so use the current game instead
  if (entry && entry->type != GameList::EntryType::Playlist)
    SettingsWindow::openGamePropertiesDialog(entry, category);
  else
    g_core_thread->openGamePropertiesForCurrentGame(category ? QString::fromUtf8(category) : QString());
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

void MainWindow::doControllerSettings(u32 category /* = 0 */)
{
  ControllerSettingsWindow* window = getControllerSettingsWindow();
  QtUtils::ShowOrRaiseWindow(window, this, true);
  window->setCategory(category);
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
  if (s_locals.system_valid)
    m_settings_toolbar_menu->popup(QCursor::pos());
  else
    doSettings();
}

void MainWindow::onSettingsControllerProfilesTriggered()
{
  if (!m_input_profile_editor_window)
    m_input_profile_editor_window = new ControllerSettingsWindow(nullptr, true);

  QtUtils::ShowOrRaiseWindow(m_input_profile_editor_window, this);
}

void MainWindow::openInputProfileEditor(const std::string_view name)
{
  if (!m_input_profile_editor_window)
    m_input_profile_editor_window = new ControllerSettingsWindow(nullptr, true);

  QtUtils::ShowOrRaiseWindow(m_input_profile_editor_window, this);
  m_input_profile_editor_window->switchProfile(name);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
  // If there's no VM, we can just exit as normal.
  // When recreating, g_main_window will be the new window at this point.
  if (!QtHost::IsSystemValidOrStarting() || g_main_window != this)
  {
    QtUtils::SaveWindowGeometry(this);

    if (s_locals.fullscreen_ui_started && g_main_window == this)
      g_core_thread->stopFullscreenUI();

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
      g_core_thread->redrawRenderWindow();
  }

  if (event->type() == QEvent::StyleChange)
  {
    QtHost::UpdateThemeOnStyleChange();
    QtUtils::StyleChildMenus(this);
    emit themeChanged(QtHost::IsDarkApplicationTheme());
  }

  QMainWindow::changeEvent(event);
}

static QString getPathFromMimeData(const QMimeData* md)
{
  QString path;
  if (md->hasUrls())
  {
    // only one url accepted
    const QList<QUrl> urls(md->urls());
    if (urls.size() == 1)
      path = QDir::toNativeSeparators(urls.front().toLocalFile());
  }

  return path;
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
  const std::string path = getPathFromMimeData(event->mimeData()).toStdString();
  if (!System::IsLoadablePath(path) && !System::IsSaveStatePath(path))
    return;

  event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event)
{
  const QString qpath(getPathFromMimeData(event->mimeData()));
  const std::string path(qpath.toStdString());
  if (!System::IsLoadablePath(path) && !System::IsSaveStatePath(path))
    return;

  event->acceptProposedAction();

  if (System::IsSaveStatePath(path))
  {
    g_core_thread->loadState(qpath);
    return;
  }
  else if (System::IsExePath(path) || System::IsPsfPath(path) || System::IsGPUDumpPath(path))
  {
    g_core_thread->bootOrSwitchNonDisc(qpath);
    return;
  }

  if (s_locals.system_valid)
    promptForDiscChange(qpath);
  else
    startFileOrChangeDisc(qpath);
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
  if (!Core::GetBaseBoolSettingValue("AutoUpdater", "CheckAtStartup", true))
    return;

  checkForUpdates(false, false);
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
    if (exit_fullscreen_ui && s_locals.fullscreen_ui_started)
      g_core_thread->stopFullscreenUI();

    if (quit_afterwards)
      quit();

    return;
  }

  // If we don't have a serial, we can't save state.
  allow_save_to_state &= !s_locals.current_game_serial.isEmpty();
  save_state &= allow_save_to_state;

  // Only confirm on UI thread because we need to display a msgbox.
  if (!m_is_closing && s_locals.system_valid && allow_confirm &&
      Core::GetBoolSettingValue("Main", "ConfirmPowerOff", true))
  {
    // Hardcore mode restrictions.
    if (check_pause && !s_locals.system_paused && s_locals.achievements_hardcore_mode && allow_confirm)
    {
      Host::RunOnCoreThread(
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

    QMessageBox* const msgbox = QtUtils::NewMessageBox(
      lock.getDialogParent(), QMessageBox::Question, quit_afterwards ? tr("Confirm Exit") : tr("Confirm Close"),
      quit_afterwards ? tr("Are you sure you want to exit the application?") :
                        tr("Are you sure you want to close the current game?"),
      QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);

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
  if (s_locals.system_starting)
    System::CancelPendingStartup();

  // Stop fullscreen UI from reopening if requested.
  if (exit_fullscreen_ui && s_locals.fullscreen_ui_started)
    g_core_thread->stopFullscreenUI();

  // Now we can actually shut down the VM.
  g_core_thread->shutdownSystem(save_state, check_safety);
}

void MainWindow::requestExit(bool allow_confirm /* = true */)
{
  // this is block, because otherwise closeEvent() will also prompt
  requestShutdown(allow_confirm, true, g_settings.save_state_on_exit, true, true, true, true);
}

void MainWindow::checkForSettingChanges()
{
#ifdef _WIN32
  if (const bool disable_window_rounded_corners =
        Core::GetBaseBoolSettingValue("Main", "DisableWindowRoundedCorners", false);
      disable_window_rounded_corners != s_locals.disable_window_rounded_corners)
  {
    s_locals.disable_window_rounded_corners = disable_window_rounded_corners;
    QtUtils::SetWindowRoundedCornerState(this, !s_locals.disable_window_rounded_corners);

    if (QWidget* container = getDisplayContainer(); container && !container->parent() && !container->isFullScreen())
      QtUtils::SetWindowRoundedCornerState(container, !s_locals.disable_window_rounded_corners);
  }
#endif

  // don't change state if temporary unfullscreened
  if (m_display_widget && !QtHost::IsSystemLocked() && !isRenderingFullscreen())
  {
    if (canRenderToMainWindow() != isRenderingToMain())
      g_core_thread->recreateRenderWindow();
    else
      updateLogWidget();
  }
  else
  {
    // log widget update must be deferred because updateRenderWindow() is asynchronous
    updateLogWidget();
  }

  LogWindow::updateSettings(false);

  // don't refresh window state while setup wizard is running, i.e. no game and hidden
  if (isVisible() || s_locals.system_valid || s_locals.system_starting)
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
          QtUtils::AsyncMessageBox(this, QMessageBox::Critical, tr("Memory Card Not Found"),
                                   tr("Failed to create memory card '%1': %2")
                                     .arg(card_path)
                                     .arg(QString::fromStdString(error.GetDescription())));
          return;
        }
      }
    }
  }

  // don't open the editor if no cards exist and we requested one
  if (!any_cards_exist && (!card_a_path.isEmpty() || !card_b_path.isEmpty()))
    return;

  if (!m_memory_card_editor_window)
    m_memory_card_editor_window = new MemoryCardEditorWindow();

  QtUtils::ShowOrRaiseWindow(m_memory_card_editor_window, this);

  if (!card_a_path.isEmpty())
  {
    if (!m_memory_card_editor_window->setCardA(card_a_path))
    {
      QtUtils::AsyncMessageBox(
        m_memory_card_editor_window, QMessageBox::Critical, tr("Memory Card Not Found"),
        tr("Memory card '%1' could not be found. Try starting the game and saving to create it.").arg(card_a_path));
    }
  }
  if (!card_b_path.isEmpty())
  {
    if (!m_memory_card_editor_window->setCardB(card_b_path))
    {
      QtUtils::AsyncMessageBox(
        m_memory_card_editor_window, QMessageBox::Critical, tr("Memory Card Not Found"),
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
  if (!Core::GetBaseBoolSettingValue("GameListTableView", "TriedShowingAchievementsColumn", false))
  {
    Core::SetBaseBoolSettingValue("GameListTableView", "TriedShowingAchievementsColumn", true);
    m_game_list_widget->getListView()->setAndSaveColumnHidden(GameListModel::Column_Achievements, false);
  }
}

void MainWindow::onAchievementsActiveChanged(bool active)
{
  m_ui.actionToolsRefreshAchievementProgress->setEnabled(active);
  m_ui.actionToolsDownloadAchievementGameIcons->setEnabled(active);
}

void MainWindow::onAchievementsHardcoreModeChanged(bool enabled)
{
  if (enabled)
  {
    QtUtils::CloseAndDeleteWindow(m_debugger_window);
    QtUtils::CloseAndDeleteWindow(m_memory_editor_window);
    QtUtils::CloseAndDeleteWindow(m_memory_scanner_window);
  }

  s_locals.achievements_hardcore_mode = enabled;
  updateEmulationActions();
}

bool MainWindow::onCreateAuxiliaryRenderWindow(RenderAPI render_api, qint32 x, qint32 y, quint32 width, quint32 height,
                                               const QString& title, const QString& icon_name,
                                               Host::AuxiliaryRenderWindowUserData userdata,
                                               Host::AuxiliaryRenderWindowHandle* handle, WindowInfo* wi, Error* error)
{
  AuxiliaryDisplayWidget* widget = AuxiliaryDisplayWidget::create(x, y, width, height, title, icon_name, userdata);
  if (!widget)
    return false;

#ifdef _WIN32
  if (s_locals.disable_window_rounded_corners)
    QtUtils::SetWindowRoundedCornerState(widget, false);
#endif

  const std::optional<WindowInfo>& owi = widget->getWindowInfo(render_api, error);
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
  if (!m_cover_download_window)
  {
    m_cover_download_window = new CoverDownloadWindow();
    connect(m_cover_download_window, &CoverDownloadWindow::closed, this, [this]() {
      m_cover_download_window->deleteLater();
      m_cover_download_window = nullptr;
    });
  }

  QtUtils::ShowOrRaiseWindow(m_cover_download_window, this, true);
}

void MainWindow::onToolsDownloadAchievementGameIconsTriggered()
{
  QtAsyncTaskWithProgressDialog::create(
    this, TRANSLATE_STR("GameListWidget", "Download Game Icons"),
    TRANSLATE_STR("GameListWidget", "Downloading game icons..."), false, true, 0, 0, 0.0f, true,
    [](ProgressCallback* progress) {
      Error error;
      const bool result = Achievements::DownloadGameIcons(progress, &error);
      return [error = std::move(error), result]() {
        if (!result)
          g_main_window->reportError(tr("Error"), QString::fromStdString(error.GetDescription()));

        g_main_window->m_game_list_widget->getModel()->invalidateColumn(GameListModel::Column_Icon);
      };
    });
}

void MainWindow::refreshAchievementProgress()
{
  QtAsyncTaskWithProgressDialog::create(
    this, TRANSLATE_STR("MainWindow", "Refresh Achievement Progress"), {}, false, true, 0, 0, 0.0f, true,
    [](ProgressCallback* progress) {
      Error error;
      const bool result = Achievements::RefreshAllProgressDatabase(progress, &error);
      return [error = std::move(error), result]() {
        if (!result)
          g_main_window->reportError(tr("Error"), QString::fromStdString(error.GetDescription()));

        g_main_window->m_ui.statusBar->showMessage(tr("RA: Updated achievement progress database."));
        g_main_window->m_game_list_widget->getModel()->invalidateColumn(GameListModel::Column_Achievements);
      };
    });
}

void MainWindow::onToolsMediaCaptureTriggered(bool checked)
{
  if (!s_locals.system_valid)
  {
    // leave it for later, we'll fill in the boot params
    return;
  }

  if (!checked)
  {
    Host::RunOnCoreThread(&System::StopMediaCapture);
    return;
  }

  const std::string container =
    Core::GetStringSettingValue("MediaCapture", "Container", Settings::DEFAULT_MEDIA_CAPTURE_CONTAINER);
  const QString qcontainer = QString::fromStdString(container);
  const QString filter(tr("%1 Files (*.%2)").arg(qcontainer.toUpper()).arg(qcontainer));

  QString path =
    QString::fromStdString(System::GetNewMediaCapturePath(QtHost::GetCurrentGameTitle().toStdString(), container));
  path = QDir::toNativeSeparators(QFileDialog::getSaveFileName(this, tr("Media Capture"), path, filter));
  if (path.isEmpty())
  {
    m_ui.actionMediaCapture->setChecked(false);
    return;
  }

  Host::RunOnCoreThread([path = path.toStdString()]() { System::StartMediaCapture(path); });
}

void MainWindow::onToolsMemoryEditorTriggered()
{
  if (s_locals.achievements_hardcore_mode)
    return;

  QtUtils::ShowOrRaiseWindow(getMemoryEditorWindow(), this, true);
}

void MainWindow::onToolsMemoryScannerTriggered()
{
  if (s_locals.achievements_hardcore_mode)
    return;

  if (!m_memory_scanner_window)
  {
    m_memory_scanner_window = new MemoryScannerWindow();
    connect(m_memory_scanner_window, &MemoryScannerWindow::closed, this, [this]() {
      m_memory_scanner_window->deleteLater();
      m_memory_scanner_window = nullptr;
    });
  }

  QtUtils::ShowOrRaiseWindow(m_memory_scanner_window, this, true);
}

void MainWindow::onToolsISOBrowserTriggered()
{
  ISOBrowserWindow* ib = new ISOBrowserWindow();
  ib->setAttribute(Qt::WA_DeleteOnClose);
  ib->show();
  QtUtils::CenterWindowRelativeToParent(ib, this);
}

void MainWindow::openCPUDebugger()
{
  if (s_locals.achievements_hardcore_mode)
    return;

  if (!m_debugger_window)
  {
    m_debugger_window = new DebuggerWindow();
    connect(m_debugger_window, &DebuggerWindow::closed, this, [this]() {
      m_debugger_window->deleteLater();
      m_debugger_window = nullptr;
    });
  }

  QtUtils::ShowOrRaiseWindow(m_debugger_window, this, true);
}

void MainWindow::onToolsOpenDataDirectoryTriggered()
{
  QtUtils::OpenURL(this, QUrl::fromLocalFile(QString::fromStdString(EmuFolders::DataRoot)));
}

void MainWindow::onToolsOpenTextureDirectoryTriggered()
{
  QString dir = QString::fromStdString(EmuFolders::Textures);
  if (s_locals.system_valid && !s_locals.current_game_serial.isEmpty())
    dir = QStringLiteral("%1" FS_OSPATH_SEPARATOR_STR "%2").arg(dir).arg(s_locals.current_game_serial);

  QtUtils::OpenURL(this, QUrl::fromLocalFile(dir));
}

AutoUpdaterDialog* MainWindow::createAutoUpdaterDialog(QWidget* parent, bool display_message)
{
  DebugAssert(parent);

  // The user could click Check for Updates while an update check is in progress.
  // Don't show an incomplete dialog in this case.
  if (m_auto_updater_dialog)
  {
    // Only raise it if it is waiting.
    if (m_auto_updater_dialog && m_auto_updater_dialog->areUpdatesAvailable())
      showAutoUpdaterWindow();

    return nullptr;
  }

  Error error;
  m_auto_updater_dialog = AutoUpdaterDialog::create(parent, &error);
  if (!m_auto_updater_dialog)
  {
    if (display_message)
    {
      QtUtils::AsyncMessageBox(
        parent, QMessageBox::Critical, tr("Error"),
        tr("Failed to create auto updater: %1").arg(QString::fromStdString(error.GetDescription())));
    }

    return nullptr;
  }

  // display status message indicating check is in progress
  // technically this could conflict with the game list refresh, but this is only for manual update checks.
  // by that point the game list refresh should have completed anyway
  if (display_message && parent == this)
  {
    setProgressBar(0, 0);
    m_ui.statusBar->showMessage(tr("Checking for updates..."));

    connect(m_auto_updater_dialog, &AutoUpdaterDialog::updateCheckCompleted, this, [this](bool) {
      clearProgressBar();
      m_ui.statusBar->clearMessage();
    });
  }

  connect(m_auto_updater_dialog, &AutoUpdaterDialog::closed, this, [this]() {
    if (!m_auto_updater_dialog)
      return;

    m_auto_updater_dialog->deleteLater();
    m_auto_updater_dialog = nullptr;
  });

  connect(m_auto_updater_dialog, &AutoUpdaterDialog::updateCheckCompleted, this, [this](bool update_available) {
    if (!m_auto_updater_dialog)
      return;

    if (update_available)
    {
      showAutoUpdaterWindow();
    }
    else
    {
      // NOTE: Looks terrifying, but it uses deleteLater() so it's okay.
      m_auto_updater_dialog->disconnect(m_auto_updater_dialog, &AutoUpdaterDialog::closed, this, nullptr);
      QtUtils::CloseAndDeleteWindow(m_auto_updater_dialog);
    }
  });

  return m_auto_updater_dialog;
}

void MainWindow::checkForUpdates(bool display_message, bool ignore_skipped_updates)
{
  AutoUpdaterDialog* const dialog = createAutoUpdaterDialog(this, display_message);
  if (!dialog)
    return;

  m_auto_updater_dialog->queueUpdateCheck(display_message, ignore_skipped_updates);
}

void MainWindow::showAutoUpdaterWindow()
{
  if (isRenderingFullscreen())
  {
    // Gotta get out of fullscreen first.
    g_core_thread->setFullscreenWithCompletionHandler(false, []() { g_main_window->showAutoUpdaterWindow(); });
    return;
  }

  if (m_auto_updater_dialog->isVisible())
  {
    QtUtils::ShowOrRaiseWindow(m_auto_updater_dialog);
    return;
  }

  // If the main window cannot be shown (e.g. render to separate), then don't make it window-modal.
  // Otherwise it ends as a sheet on MacOS, and leaves an empty main window behind.
  if (!isVisible())
    m_auto_updater_dialog->show();
  else
    m_auto_updater_dialog->open();
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
  const bool was_fullscreen = isRenderingFullscreen();
#else
  const bool was_fullscreen = false;
#endif
  const bool was_paused = !s_locals.system_valid || s_locals.system_paused;

  // Have to do this early to avoid making the main window visible.
  s_locals.system_locked.fetch_add(1, std::memory_order_release);

  // We need to switch out of exclusive fullscreen before we can display our popup.
  // However, we do not want to switch back to render-to-main, the window might have generated this event.
  if (was_fullscreen)
  {
    g_core_thread->setFullscreen(false);

    // Container could change... thanks Wayland.
    QtUtils::ProcessEventsWithSleep(QEventLoop::ExcludeUserInputEvents, [this]() { return isRenderingFullscreen(); });
  }

  if (!was_paused)
  {
    g_core_thread->setSystemPaused(true);

    // Need to wait for the pause to go through, and make the main window visible if needed.
    QtUtils::ProcessEventsWithSleep(QEventLoop::ExcludeUserInputEvents, []() { return !s_locals.system_paused; });
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

  DebugAssert(s_locals.system_locked.load(std::memory_order_relaxed) > 0);
  s_locals.system_locked.fetch_sub(1, std::memory_order_release);
  if (m_was_fullscreen)
    g_core_thread->setFullscreen(true);
  if (!m_was_paused)
    g_core_thread->setSystemPaused(false);
}

void MainWindow::SystemLock::cancelResume()
{
  m_was_paused = true;
  m_was_fullscreen = false;
}

bool QtHost::IsSystemLocked()
{
  return (s_locals.system_locked.load(std::memory_order_acquire) > 0);
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
