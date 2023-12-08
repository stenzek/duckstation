// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "mainwindow.h"
#include "aboutdialog.h"
#include "achievementlogindialog.h"
#include "autoupdaterdialog.h"
#include "coverdownloaddialog.h"
#include "debuggerwindow.h"
#include "displaywidget.h"
#include "gamelistmodel.h"
#include "gamelistsettingswidget.h"
#include "gamelistwidget.h"
#include "interfacesettingswidget.h"
#include "isobrowserwindow.h"
#include "logwindow.h"
#include "memorycardeditorwindow.h"
#include "memoryscannerwindow.h"
#include "qthost.h"
#include "qtutils.h"
#include "selectdiscdialog.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"

#include "core/achievements.h"
#include "core/cheats.h"
#include "core/game_list.h"
#include "core/host.h"
#include "core/memory_card.h"
#include "core/settings.h"
#include "core/system.h"

#include "util/cd_image.h"
#include "util/gpu_device.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"

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
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QStyleFactory>
#include <cmath>

#ifdef _WIN32
#include "common/windows_headers.h"
#include <Dbt.h>
#include <VersionHelpers.h>
#endif

LOG_CHANNEL(Host);

static constexpr char DISC_IMAGE_FILTER[] = QT_TRANSLATE_NOOP(
  "MainWindow",
  "All File Types (*.bin *.img *.iso *.cue *.chd *.cpe *.ecm *.mds *.pbp *.elf *.exe *.psexe *.ps-exe *.psx *.psf "
  "*.minipsf *.m3u *.psxgpu);;Single-Track Raw Images (*.bin *.img *.iso);;Cue Sheets (*.cue);;MAME CHD Images "
  "(*.chd);;Error Code Modeler Images (*.ecm);;Media Descriptor Sidecar Images (*.mds);;PlayStation EBOOTs (*.pbp "
  "*.PBP);;PlayStation Executables (*.cpe *.elf *.exe *.psexe *.ps-exe, *.psx);;Portable Sound Format Files (*.psf "
  "*.minipsf);;Playlists (*.m3u);;PSX GPU Dumps (*.psxgpu)");

MainWindow* g_main_window = nullptr;

#if defined(_WIN32) || defined(__APPLE__)
static const bool s_use_central_widget = false;
#else
// Qt Wayland is broken. Any sort of stacked widget usage fails to update,
// leading to broken window resizes, no display rendering, etc. So, we mess
// with the central widget instead. Which we can't do on xorg, because it
// breaks window resizing there...
static bool s_use_central_widget = false;
#endif

// UI thread VM validity.
static bool s_system_valid = false;
static bool s_system_paused = false;
static bool s_fullscreen_ui_started = false;
static std::atomic_uint32_t s_system_locked{false};
static QString s_current_game_title;
static QString s_current_game_serial;
static QString s_current_game_path;
static QIcon s_current_game_icon;

bool QtHost::IsSystemPaused()
{
  return s_system_paused;
}

bool QtHost::IsSystemValid()
{
  return s_system_valid;
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

#if !defined(_WIN32) && !defined(__APPLE__)
  s_use_central_widget = DisplayContainer::isRunningOnWayland();
#endif

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
  connectSignals();

  restoreStateFromConfig();
  switchToGameListView();
  updateWindowTitle();

#ifdef ENABLE_RAINTEGRATION
  if (Achievements::IsUsingRAIntegration())
    Achievements::RAIntegration::MainWindowChanged((void*)winId());
#endif

#ifdef _WIN32
  registerForDeviceNotifications();
#endif
}

void MainWindow::reportError(const QString& title, const QString& message)
{
  QMessageBox::critical(this, title, message, QMessageBox::Ok);
}

bool MainWindow::confirmMessage(const QString& title, const QString& message)
{
  SystemLock lock(pauseAndLockSystem());

  return (QMessageBox::question(this, title, message) == QMessageBox::Yes);
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

std::optional<WindowInfo> MainWindow::acquireRenderWindow(RenderAPI render_api, bool fullscreen, bool render_to_main,
                                                          bool surfaceless, bool use_main_window_pos, Error* error)
{
  DEV_LOG("acquireRenderWindow() fullscreen={} render_to_main={} surfaceless={} use_main_window_pos={}",
          fullscreen ? "true" : "false", render_to_main ? "true" : "false", surfaceless ? "true" : "false",
          use_main_window_pos ? "true" : "false");

  QWidget* container =
    m_display_container ? static_cast<QWidget*>(m_display_container) : static_cast<QWidget*>(m_display_widget);
  const bool is_fullscreen = isRenderingFullscreen();
  const bool is_rendering_to_main = isRenderingToMain();
  const bool changing_surfaceless = (!m_display_widget != surfaceless);

  // Skip recreating the surface if we're just transitioning between fullscreen and windowed with render-to-main off.
  // .. except on Wayland, where everything tends to break if you don't recreate.
  const bool has_container = (m_display_container != nullptr);
  const bool needs_container = DisplayContainer::isNeeded(fullscreen, render_to_main);
  if (m_display_created && !is_rendering_to_main && !render_to_main && has_container == needs_container &&
      !needs_container && !changing_surfaceless)
  {
    DEV_LOG("Toggling to {} without recreating surface", (fullscreen ? "fullscreen" : "windowed"));

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
      if (use_main_window_pos)
        container->setGeometry(geometry());
      else
        restoreDisplayWindowGeometryFromConfig();
    }

    updateDisplayWidgetCursor();
    m_display_widget->setFocus();
    updateWindowState();

    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    return m_display_widget->getWindowInfo(render_api, error);
  }

  destroyDisplayWidget(surfaceless);
  m_display_created = true;

  // if we're going to surfaceless, we're done here
  if (surfaceless)
    return WindowInfo();

  createDisplayWidget(fullscreen, render_to_main, use_main_window_pos);

  std::optional<WindowInfo> wi = m_display_widget->getWindowInfo(render_api, error);
  if (!wi.has_value())
  {
    QMessageBox::critical(this, tr("Error"), tr("Failed to get window info from widget"));
    destroyDisplayWidget(true);
    return std::nullopt;
  }

  g_emu_thread->connectDisplaySignals(m_display_widget);

  updateWindowTitle();
  updateWindowState();

  updateDisplayWidgetCursor();
  updateDisplayRelatedActions(true, render_to_main, fullscreen);
  QtUtils::ShowOrRaiseWindow(QtUtils::GetRootWidget(m_display_widget));
  m_display_widget->setFocus();

  return wi;
}

void MainWindow::createDisplayWidget(bool fullscreen, bool render_to_main, bool use_main_window_pos)
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
    m_display_widget = new DisplayWidget((!fullscreen && render_to_main) ? getContentParent() : nullptr);
    container = m_display_widget;
  }

  if (fullscreen || !render_to_main)
  {
    container->setWindowTitle(windowTitle());
    container->setWindowIcon(windowIcon());
  }

  if (fullscreen)
  {
    // Don't risk doing this on Wayland, it really doesn't like window state changes,
    // and positioning has no effect anyway.
    if (!s_use_central_widget)
    {
      if (isVisible() && g_emu_thread->shouldRenderToMain())
        container->move(pos());
      else
        restoreDisplayWindowGeometryFromConfig();
    }

    container->showFullScreen();
  }
  else if (!render_to_main)
  {
    // See lameland comment above.
    if (use_main_window_pos && !s_use_central_widget)
      container->setGeometry(geometry());
    else
      restoreDisplayWindowGeometryFromConfig();
    container->showNormal();
  }
  else if (s_use_central_widget)
  {
    m_game_list_widget->setVisible(false);
    takeCentralWidget();
    m_game_list_widget->setParent(this); // takeCentralWidget() removes parent
    setCentralWidget(m_display_widget);
    m_display_widget->setFocus();
    update();
  }
  else
  {
    AssertMsg(m_ui.mainContainer->count() == 1, "Has no display widget");
    m_ui.mainContainer->addWidget(container);
    m_ui.mainContainer->setCurrentIndex(1);
  }

  updateDisplayRelatedActions(true, render_to_main, fullscreen);

  // We need the surface visible.
  QGuiApplication::sync();
}

void MainWindow::displayResizeRequested(qint32 width, qint32 height)
{
  if (!m_display_widget)
    return;

  // unapply the pixel scaling factor for hidpi
  const float dpr = devicePixelRatioF();
  width = static_cast<qint32>(std::max(static_cast<int>(std::lroundf(static_cast<float>(width) / dpr)), 1));
  height = static_cast<qint32>(std::max(static_cast<int>(std::lroundf(static_cast<float>(height) / dpr)), 1));

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
  m_display_created = false;

  updateDisplayRelatedActions(false, false, false);

  m_ui.actionViewSystemDisplay->setEnabled(false);
  m_ui.actionFullscreen->setEnabled(false);
}

void MainWindow::destroyDisplayWidget(bool show_game_list)
{
  if (!m_display_widget)
    return;

  if (!isRenderingFullscreen() && !isRenderingToMain())
    saveDisplayWindowGeometryToConfig();

  if (m_display_container)
    m_display_container->removeDisplayWidget();

  if (isRenderingToMain())
  {
    if (s_use_central_widget)
    {
      AssertMsg(centralWidget() == m_display_widget, "Display widget is currently central");
      takeCentralWidget();
      if (show_game_list)
      {
        m_game_list_widget->setVisible(true);
        setCentralWidget(m_game_list_widget);
        m_game_list_widget->resizeTableViewColumnsToFit();
      }
    }
    else
    {
      AssertMsg(m_ui.mainContainer->indexOf(m_display_widget) == 1, "Display widget in stack");
      m_ui.mainContainer->removeWidget(m_display_widget);
      if (show_game_list)
      {
        m_ui.mainContainer->setCurrentIndex(0);
        m_game_list_widget->resizeTableViewColumnsToFit();
      }
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

void MainWindow::updateDisplayWidgetCursor()
{
  m_display_widget->updateRelativeMode(s_system_valid && !s_system_paused && m_relative_mouse_mode);
  m_display_widget->updateCursor(s_system_valid && !s_system_paused && shouldHideMouseCursor());
}

void MainWindow::updateDisplayRelatedActions(bool has_surface, bool render_to_main, bool fullscreen)
{
  // rendering to main, or switched to gamelist/grid
  m_ui.actionViewSystemDisplay->setEnabled((has_surface && render_to_main) || (!has_surface && g_gpu_device));
  m_ui.menuWindowSize->setEnabled(has_surface && !fullscreen);
  m_ui.actionFullscreen->setEnabled(has_surface);

  {
    QSignalBlocker blocker(m_ui.actionFullscreen);
    m_ui.actionFullscreen->setChecked(fullscreen);
  }
}

void MainWindow::focusDisplayWidget()
{
  if (!m_display_widget || centralWidget() != m_display_widget)
    return;

  m_display_widget->setFocus();
}

QWidget* MainWindow::getContentParent()
{
  return s_use_central_widget ? static_cast<QWidget*>(this) : static_cast<QWidget*>(m_ui.mainContainer);
}

QWidget* MainWindow::getDisplayContainer() const
{
  return (m_display_container ? static_cast<QWidget*>(m_display_container) : static_cast<QWidget*>(m_display_widget));
}

void MainWindow::onMouseModeRequested(bool relative_mode, bool hide_cursor)
{
  m_relative_mouse_mode = relative_mode;
  m_hide_mouse_cursor = hide_cursor;
  if (m_display_widget)
    updateDisplayWidgetCursor();
}

void MainWindow::onSystemStarting()
{
  s_system_valid = false;
  s_system_paused = false;

  updateEmulationActions(true, false, Achievements::IsHardcoreModeActive());
}

void MainWindow::onSystemStarted()
{
  m_was_disc_change_request = false;
  s_system_valid = true;
  updateEmulationActions(false, true, Achievements::IsHardcoreModeActive());
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
  if (m_display_widget)
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
  if (m_display_widget)
  {
    updateDisplayWidgetCursor();
    m_display_widget->setFocus();
  }
}

void MainWindow::onSystemDestroyed()
{
  // update UI
  {
    QSignalBlocker sb(m_ui.actionPause);
    m_ui.actionPause->setChecked(false);
  }

  s_system_valid = false;
  s_system_paused = false;

  // If we're closing or in batch mode, quit the whole application now.
  if (m_is_closing || QtHost::InBatchMode())
  {
    destroySubWindows();
    quit();
    return;
  }

  updateEmulationActions(false, false, Achievements::IsHardcoreModeActive());
  if (m_display_widget)
    updateDisplayWidgetCursor();
  else
    switchToGameListView();

  // reload played time
  if (m_game_list_widget->isShowingGameList())
    m_game_list_widget->refresh(false);
}

void MainWindow::onRunningGameChanged(const QString& filename, const QString& game_serial, const QString& game_title)
{
  s_current_game_path = filename;
  s_current_game_title = game_title;
  s_current_game_serial = game_serial;
  s_current_game_icon = m_game_list_widget->getModel()->getIconForGame(filename);

  updateWindowTitle();
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
    QMessageBox::critical(this, title,
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
  if (input_dialog.exec() == 0)
    return ret;

  const int selected_index = input_dialog.comboBoxItems().indexOf(input_dialog.textValue());
  if (selected_index < 0 || static_cast<u32>(selected_index) >= devices.size())
    return ret;

  ret = std::move(devices[selected_index].first);
  return ret;
}

void MainWindow::quit()
{
  // Make sure VM is gone. It really should be if we're here.
  if (s_system_valid)
  {
    g_emu_thread->shutdownSystem(false, true);
    QtUtils::ProcessEventsWithSleep(QEventLoop::ExcludeUserInputEvents, []() { return s_system_valid; });
  }

  // Big picture might still be active.
  if (m_display_created)
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

  const bool was_display_created = m_display_created;
  if (was_display_created)
  {
    g_emu_thread->setSurfaceless(true);
    QtUtils::ProcessEventsWithSleep(QEventLoop::ExcludeUserInputEvents,
                                    [this]() { return (m_display_widget || !g_emu_thread->isSurfaceless()); });
    m_display_created = false;
  }

  // We need to close input sources, because e.g. DInput uses our window handle.
  g_emu_thread->closeInputSources();

  close();
  g_main_window = nullptr;

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
    g_main_window->updateEmulationActions(false, System::IsValid(), Achievements::IsHardcoreModeActive());
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
}

void MainWindow::destroySubWindows()
{
  QtUtils::CloseAndDeleteWindow(m_memory_scanner_window);
  QtUtils::CloseAndDeleteWindow(m_debugger_window);
  QtUtils::CloseAndDeleteWindow(m_memory_card_editor_window);
  QtUtils::CloseAndDeleteWindow(m_controller_settings_window);
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

    if (!entry->serial.empty())
    {
      std::vector<SaveStateInfo> available_states(System::GetAvailableSaveStates(entry->serial));
      const QString timestamp_format = QLocale::system().dateTimeFormat(QLocale::ShortFormat);
      const bool challenge_mode = Achievements::IsHardcoreModeActive();
      for (SaveStateInfo& ssi : available_states)
      {
        if (ssi.global)
          continue;

        const s32 slot = ssi.slot;
        const QDateTime timestamp(QDateTime::fromSecsSinceEpoch(static_cast<qint64>(ssi.timestamp)));
        const QString timestamp_str(timestamp.toString(timestamp_format));

        QAction* action;
        if (slot < 0)
        {
          resume_action->setText(tr("Resume (%1)").arg(timestamp_str));
          resume_action->setEnabled(!challenge_mode);
          action = resume_action;
        }
        else
        {
          load_state_menu->setEnabled(true);
          action = load_state_menu->addAction(tr("Game Save %1 (%2)").arg(slot).arg(timestamp_str));
        }

        action->setDisabled(challenge_mode);
        connect(action, &QAction::triggered,
                [this, entry, path = std::move(ssi.path)]() { startFile(entry->path, std::move(path), std::nullopt); });
      }
    }
  }

  QAction* open_memory_cards_action = menu->addAction(tr("Edit Memory Cards..."));
  connect(open_memory_cards_action, &QAction::triggered, [entry]() {
    QString paths[2];
    for (u32 i = 0; i < 2; i++)
      paths[i] = QString::fromStdString(System::GetGameMemoryCardPath(entry->serial, entry->path, i));

    g_main_window->openMemoryCardEditor(paths[0], paths[1]);
  });

  if (!entry->IsDiscSet())
  {
    const bool has_any_states = resume_action->isEnabled() || load_state_menu->isEnabled();
    QAction* delete_save_states_action = menu->addAction(tr("Delete Save States..."));
    delete_save_states_action->setEnabled(has_any_states);
    if (has_any_states)
    {
      connect(delete_save_states_action, &QAction::triggered, [parent_window, entry] {
        if (QMessageBox::warning(
              parent_window, tr("Confirm Save State Deletion"),
              tr("Are you sure you want to delete all save states for %1?\n\nThe saves will not be recoverable.")
                .arg(QString::fromStdString(entry->serial)),
              QMessageBox::Yes, QMessageBox::No) != QMessageBox::Yes)
        {
          return;
        }

        System::DeleteSaveStates(entry->serial, true);
      });
    }
  }
}

static QString FormatTimestampForSaveStateMenu(u64 timestamp)
{
  const QDateTime qtime(QDateTime::fromSecsSinceEpoch(static_cast<qint64>(timestamp)));
  return qtime.toString(QLocale::system().dateTimeFormat(QLocale::ShortFormat));
}

void MainWindow::populateLoadStateMenu(std::string_view game_serial, QMenu* menu)
{
  auto add_slot = [this, menu](const QString& title, const QString& empty_title, const std::string_view& serial,
                               s32 slot) {
    std::optional<SaveStateInfo> ssi = System::GetSaveStateInfo(serial, slot);

    const QString menu_title =
      ssi.has_value() ? title.arg(slot).arg(FormatTimestampForSaveStateMenu(ssi->timestamp)) : empty_title.arg(slot);

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
  QAction* load_from_state = menu->addAction(tr("Undo Load State"));
  load_from_state->setEnabled(System::CanUndoLoadState());
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

    const QString menu_title =
      ssi.has_value() ? title.arg(slot).arg(FormatTimestampForSaveStateMenu(ssi->timestamp)) : empty_title.arg(slot);

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

void MainWindow::populateChangeDiscSubImageMenu(QMenu* menu, QActionGroup* action_group)
{
  if (!s_system_valid)
    return;

  if (System::HasMediaSubImages())
  {
    const u32 count = System::GetMediaSubImageCount();
    const u32 current = System::GetMediaSubImageIndex();
    for (u32 i = 0; i < count; i++)
    {
      QAction* action = action_group->addAction(QString::fromStdString(System::GetMediaSubImageTitle(i)));
      action->setCheckable(true);
      action->setChecked(i == current);
      connect(action, &QAction::triggered, [i]() { g_emu_thread->changeDiscFromPlaylist(i); });
      menu->addAction(action);
    }
  }
  else if (const GameDatabase::Entry* entry = System::GetGameDatabaseEntry(); entry && !entry->disc_set_serials.empty())
  {
    auto lock = GameList::GetLock();
    for (const auto& [title, glentry] : GameList::GetMatchingEntriesForSerial(entry->disc_set_serials))
    {
      QAction* action = action_group->addAction(QString::fromStdString(title));
      QString path = QString::fromStdString(glentry->path);
      action->setCheckable(true);
      action->setChecked(path == s_current_game_path);
      connect(action, &QAction::triggered, [path = std::move(path)]() { g_emu_thread->changeDisc(path, false, true); });
      menu->addAction(action);
    }
  }
}

void MainWindow::onCheatsActionTriggered()
{
  m_ui.menuCheats->exec(QCursor::pos());
}

void MainWindow::onCheatsMenuAboutToShow()
{
  m_ui.menuCheats->clear();
  connect(m_ui.menuCheats->addAction(tr("Select Cheats...")), &QAction::triggered, this,
          [this]() { openGamePropertiesForCurrentGame("Cheats"); });
  m_ui.menuCheats->addSeparator();
  populateCheatsMenu(m_ui.menuCheats);
}

void MainWindow::populateCheatsMenu(QMenu* menu)
{
  Host::RunOnCPUThread([menu]() {
    if (!System::IsValid())
      return;

    QStringList names;
    Cheats::EnumerateManualCodes([&names](const std::string& name) {
      names.append(QString::fromStdString(name));
      return true;
    });
    if (Cheats::AreCheatsEnabled() && names.empty())
      return;

    QtHost::RunOnUIThread([menu, names = std::move(names)]() {
      if (names.empty())
      {
        QAction* action = menu->addAction(tr("Cheats are not enabled."));
        action->setEnabled(false);
        return;
      }

      QMenu* apply_submenu = menu->addMenu(tr("&Apply Cheat"));
      for (const QString& name : names)
      {
        const QAction* action = apply_submenu->addAction(name);
        connect(action, &QAction::triggered, apply_submenu, [action]() {
          Host::RunOnCPUThread([name = action->text().toStdString()]() {
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
  if (!entry || entry->type != GameList::EntryType::DiscSet)
    return entry;

  // disc set... need to figure out the disc we want
  SelectDiscDialog dlg(entry->path, this);

  lock.unlock();
  const int res = dlg.exec();
  lock.lock();

  return res ? GameList::GetEntryForPath(dlg.getSelectedDiscPath()) : nullptr;
}

std::shared_ptr<SystemBootParameters> MainWindow::getSystemBootParameters(std::string file)
{
  std::shared_ptr<SystemBootParameters> ret = std::make_shared<SystemBootParameters>(std::move(file));
  ret->start_media_capture = m_ui.actionMediaCapture->isChecked();
  return ret;
}

std::optional<bool> MainWindow::promptForResumeState(const std::string& save_state_path)
{
  FILESYSTEM_STAT_DATA sd;
  if (save_state_path.empty() || !FileSystem::StatFile(save_state_path.c_str(), &sd))
    return false;

  QMessageBox msgbox(this);
  msgbox.setIcon(QMessageBox::Question);
  msgbox.setWindowTitle(tr("Load Resume State"));
  msgbox.setWindowModality(Qt::WindowModal);
  msgbox.setText(tr("A resume save state was found for this game, saved at:\n\n%1.\n\nDo you want to load this state, "
                    "or start from a fresh boot?")
                   .arg(QDateTime::fromSecsSinceEpoch(sd.ModificationTime, QTimeZone::utc()).toLocalTime().toString()));

  QPushButton* load = msgbox.addButton(tr("Load State"), QMessageBox::AcceptRole);
  QPushButton* boot = msgbox.addButton(tr("Fresh Boot"), QMessageBox::RejectRole);
  QPushButton* delboot = msgbox.addButton(tr("Delete And Boot"), QMessageBox::RejectRole);
  msgbox.addButton(QMessageBox::Cancel);
  msgbox.setDefaultButton(load);
  msgbox.exec();

  QAbstractButton* clicked = msgbox.clickedButton();
  if (load == clicked)
  {
    return true;
  }
  else if (boot == clicked)
  {
    return false;
  }
  else if (delboot == clicked)
  {
    if (!FileSystem::DeleteFile(save_state_path.c_str()))
    {
      QMessageBox::critical(this, tr("Error"),
                            tr("Failed to delete save state file '%1'.").arg(QString::fromStdString(save_state_path)));
    }

    return false;
  }

  return std::nullopt;
}

void MainWindow::startFile(std::string path, std::optional<std::string> save_path, std::optional<bool> fast_boot)
{
  std::shared_ptr<SystemBootParameters> params = getSystemBootParameters(std::move(path));
  params->override_fast_boot = fast_boot;
  if (save_path.has_value())
    params->save_state = std::move(save_path.value());

  g_emu_thread->bootSystem(std::move(params));
}

void MainWindow::startFileOrChangeDisc(const QString& path)
{
  if (s_system_valid)
  {
    // this is a disc change
    promptForDiscChange(path);
    return;
  }

  // try to find the serial for the game
  std::string path_str(path.toStdString());
  std::string serial(GameDatabase::GetSerialForPath(path_str.c_str()));
  std::optional<std::string> save_path;
  if (!serial.empty())
  {
    std::string resume_path(System::GetGameSaveStateFileName(serial.c_str(), -1));
    std::optional<bool> resume = promptForResumeState(resume_path);
    if (!resume.has_value())
    {
      // cancelled
      return;
    }
    else if (resume.value())
      save_path = std::move(resume_path);
  }

  // only resume if the option is enabled, and we have one for this game
  startFile(std::move(path_str), std::move(save_path), std::nullopt);
}

void MainWindow::promptForDiscChange(const QString& path)
{
  SystemLock lock(pauseAndLockSystem());

  bool reset_system = false;
  if (!m_was_disc_change_request && !System::IsGPUDumpPath(path.toStdString()))
  {
    QMessageBox mb(QMessageBox::Question, tr("Confirm Disc Change"),
                   tr("Do you want to swap discs or boot the new image (via system reset)?"), QMessageBox::NoButton,
                   this);
    /*const QAbstractButton* const swap_button = */ mb.addButton(tr("Swap Disc"), QMessageBox::YesRole);
    const QAbstractButton* const reset_button = mb.addButton(tr("Reset"), QMessageBox::NoRole);
    const QAbstractButton* const cancel_button = mb.addButton(tr("Cancel"), QMessageBox::RejectRole);
    mb.exec();

    const QAbstractButton* const clicked_button = mb.clickedButton();
    if (!clicked_button || clicked_button == cancel_button)
      return;

    reset_system = (clicked_button == reset_button);
  }

  switchToEmulationView();

  g_emu_thread->changeDisc(path, reset_system, true);
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
  populateChangeDiscSubImageMenu(m_ui.menuChangeDisc, m_ui.actionGroupChangeDiscSubImages);
}

void MainWindow::onChangeDiscMenuAboutToHide()
{
  for (QAction* action : m_ui.actionGroupChangeDiscSubImages->actions())
  {
    m_ui.actionGroupChangeDiscSubImages->removeAction(action);
    m_ui.menuChangeDisc->removeAction(action);
    action->deleteLater();
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
}

void MainWindow::onViewLockToolbarActionToggled(bool checked)
{
  Host::SetBaseBoolSettingValue("UI", "LockToolbar", checked);
  Host::CommitBaseSettingChanges();
  m_ui.toolBar->setMovable(!checked);
}

void MainWindow::onViewStatusBarActionToggled(bool checked)
{
  Host::SetBaseBoolSettingValue("UI", "ShowStatusBar", checked);
  Host::CommitBaseSettingChanges();
  m_ui.statusBar->setVisible(checked);
}

void MainWindow::onViewGameListActionTriggered()
{
  switchToGameListView();
  m_game_list_widget->showGameList();
}

void MainWindow::onViewGameGridActionTriggered()
{
  switchToGameListView();
  m_game_list_widget->showGameGrid();
}

void MainWindow::onViewSystemDisplayTriggered()
{
  if (m_display_created)
    switchToEmulationView();
}

void MainWindow::onViewGameGridZoomInActionTriggered()
{
  if (isShowingGameList())
    m_game_list_widget->gridZoomIn();
}

void MainWindow::onViewGameGridZoomOutActionTriggered()
{
  if (isShowingGameList())
    m_game_list_widget->gridZoomOut();
}

void MainWindow::onGitHubRepositoryActionTriggered()
{
  QtUtils::OpenURL(this, "https://github.com/stenzek/duckstation/");
}

void MainWindow::onIssueTrackerActionTriggered()
{
  QtUtils::OpenURL(this, "https://www.duckstation.org/issues.html");
}

void MainWindow::onDiscordServerActionTriggered()
{
  QtUtils::OpenURL(this, "https://www.duckstation.org/discord.html");
}

void MainWindow::onAboutActionTriggered()
{
  AboutDialog about(this);
  about.exec();
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
      QMessageBox::critical(this, tr("Error"), tr("You must select a disc to change discs."));
      return;
    }

    promptForDiscChange(QString::fromStdString(entry->path));
    return;
  }

  std::optional<std::string> save_path;
  if (!entry->serial.empty())
  {
    std::string resume_path(System::GetGameSaveStateFileName(entry->serial.c_str(), -1));
    std::optional<bool> resume = promptForResumeState(resume_path);
    if (!resume.has_value())
    {
      // cancelled
      return;
    }
    else if (resume.value())
      save_path = std::move(resume_path);
  }

  // only resume if the option is enabled, and we have one for this game
  startFile(entry->path, std::move(save_path), std::nullopt);
}

void MainWindow::onGameListEntryContextMenuRequested(const QPoint& point)
{
  auto lock = GameList::GetLock();
  const GameList::Entry* entry = m_game_list_widget->getSelectedEntry();

  QMenu menu;

  // Hopefully this pointer doesn't disappear... it shouldn't.
  if (entry)
  {
    if (!entry->IsDiscSet())
    {
      connect(menu.addAction(tr("Properties...")), &QAction::triggered, [entry]() {
        SettingsWindow::openGamePropertiesDialog(entry->path, entry->title, entry->serial, entry->hash, entry->region);
      });

      connect(menu.addAction(tr("Open Containing Directory...")), &QAction::triggered, [this, entry]() {
        const QFileInfo fi(QString::fromStdString(entry->path));
        QtUtils::OpenURL(this, QUrl::fromLocalFile(fi.absolutePath()));
      });

      if (entry->IsDisc())
      {
        connect(menu.addAction(tr("Browse ISO...")), &QAction::triggered, [this, entry]() {
          ISOBrowserWindow* ib = ISOBrowserWindow::createAndOpenFile(this, QString::fromStdString(entry->path));
          if (ib)
          {
            ib->setAttribute(Qt::WA_DeleteOnClose);
            ib->show();
          }
        });
      }

      connect(menu.addAction(tr("Set Cover Image...")), &QAction::triggered,
              [this, entry]() { setGameListEntryCoverImage(entry); });

      menu.addSeparator();

      if (!s_system_valid)
      {
        populateGameListContextMenu(entry, this, &menu);
        menu.addSeparator();

        connect(menu.addAction(tr("Default Boot")), &QAction::triggered,
                [this, entry]() { g_emu_thread->bootSystem(getSystemBootParameters(entry->path)); });

        connect(menu.addAction(tr("Fast Boot")), &QAction::triggered, [this, entry]() {
          std::shared_ptr<SystemBootParameters> boot_params = getSystemBootParameters(entry->path);
          boot_params->override_fast_boot = true;
          g_emu_thread->bootSystem(std::move(boot_params));
        });

        connect(menu.addAction(tr("Full Boot")), &QAction::triggered, [this, entry]() {
          std::shared_ptr<SystemBootParameters> boot_params = getSystemBootParameters(entry->path);
          boot_params->override_fast_boot = false;
          g_emu_thread->bootSystem(std::move(boot_params));
        });

        if (m_ui.menuDebug->menuAction()->isVisible() && !Achievements::IsHardcoreModeActive())
        {
          connect(menu.addAction(tr("Boot and Debug")), &QAction::triggered, [this, entry]() {
            openCPUDebugger();

            std::shared_ptr<SystemBootParameters> boot_params = getSystemBootParameters(entry->path);
            boot_params->override_start_paused = true;
            g_emu_thread->bootSystem(std::move(boot_params));
          });
        }
      }
      else
      {
        connect(menu.addAction(tr("Change Disc")), &QAction::triggered, [this, entry]() {
          g_emu_thread->changeDisc(QString::fromStdString(entry->path), false, true);
          g_emu_thread->setSystemPaused(false);
          switchToEmulationView();
        });
      }

      menu.addSeparator();

      connect(menu.addAction(tr("Exclude From List")), &QAction::triggered,
              [this, entry]() { getSettingsWindow()->getGameListSettingsWidget()->addExcludedPath(entry->path); });

      connect(menu.addAction(tr("Reset Play Time")), &QAction::triggered,
              [this, entry]() { clearGameListEntryPlayTime(entry); });
    }
    else
    {
      connect(menu.addAction(tr("Properties...")), &QAction::triggered, [disc_set_name = entry->path]() {
        // resolve path first
        auto lock = GameList::GetLock();
        const GameList::Entry* first_disc = GameList::GetFirstDiscSetMember(disc_set_name);
        if (first_disc)
        {
          SettingsWindow::openGamePropertiesDialog(first_disc->path, first_disc->title, first_disc->serial,
                                                   first_disc->hash, first_disc->region);
        }
      });

      connect(menu.addAction(tr("Set Cover Image...")), &QAction::triggered,
              [this, entry]() { setGameListEntryCoverImage(entry); });

      menu.addSeparator();

      populateGameListContextMenu(entry, this, &menu);

      menu.addSeparator();

      connect(menu.addAction(tr("Select Disc")), &QAction::triggered, this, &MainWindow::onGameListEntryActivated);

      menu.addSeparator();

      connect(menu.addAction(tr("Exclude From List")), &QAction::triggered,
              [this, entry]() { getSettingsWindow()->getGameListSettingsWidget()->addExcludedPath(entry->path); });
    }
  }

  menu.addSeparator();

  connect(menu.addAction(tr("Add Search Directory...")), &QAction::triggered,
          [this]() { getSettingsWindow()->getGameListSettingsWidget()->addSearchDirectory(this); });

  menu.exec(point);
}

void MainWindow::setGameListEntryCoverImage(const GameList::Entry* entry)
{
  const QString filename = QDir::toNativeSeparators(QFileDialog::getOpenFileName(
    this, tr("Select Cover Image"), QString(), tr("All Cover Image Types (*.jpg *.jpeg *.png *.webp)")));
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
      QMessageBox::critical(this, tr("Copy Error"), tr("You must select a different file to the current cover image."));
      return;
    }

    if (QMessageBox::question(this, tr("Cover Already Exists"),
                              tr("A cover image for this game already exists, do you wish to replace it?"),
                              QMessageBox::Yes, QMessageBox::No) != QMessageBox::Yes)
    {
      return;
    }
  }

  if (QFile::exists(new_filename) && !QFile::remove(new_filename))
  {
    QMessageBox::critical(this, tr("Copy Error"), tr("Failed to remove existing cover '%1'").arg(new_filename));
    return;
  }
  if (!QFile::copy(filename, new_filename))
  {
    QMessageBox::critical(this, tr("Copy Error"), tr("Failed to copy '%1' to '%2'").arg(filename).arg(new_filename));
    return;
  }
  if (!old_filename.isEmpty() && old_filename != new_filename && !QFile::remove(old_filename))
  {
    QMessageBox::critical(this, tr("Copy Error"), tr("Failed to remove '%1'").arg(old_filename));
    return;
  }
  m_game_list_widget->refreshGridCovers();
}

void MainWindow::clearGameListEntryPlayTime(const GameList::Entry* entry)
{
  if (QMessageBox::question(
        this, tr("Confirm Reset"),
        tr("Are you sure you want to reset the play time for '%1'?\n\nThis action cannot be undone.")
          .arg(QString::fromStdString(entry->title))) != QMessageBox::Yes)
  {
    return;
  }

  GameList::ClearPlayedTimeForSerial(entry->serial);
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
  m_ui.toolBar->setContextMenuPolicy(Qt::PreventContextMenu);

  m_game_list_widget = new GameListWidget(getContentParent());
  m_game_list_widget->initialize();
  m_ui.actionGridViewShowTitles->setChecked(m_game_list_widget->isShowingGridCoverTitles());
  m_ui.actionMergeDiscSets->setChecked(m_game_list_widget->isMergingDiscSets());
  m_ui.actionShowGameIcons->setChecked(m_game_list_widget->isShowingGameIcons());
  if (s_use_central_widget)
  {
    m_ui.mainContainer = nullptr; // setCentralWidget() will delete this
    setCentralWidget(m_game_list_widget);
  }
  else
  {
    m_ui.mainContainer->addWidget(m_game_list_widget);
  }

  m_status_progress_widget = new QProgressBar(m_ui.statusBar);
  m_status_progress_widget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  m_status_progress_widget->setFixedSize(140, 16);
  m_status_progress_widget->setMinimum(0);
  m_status_progress_widget->setMaximum(100);
  m_status_progress_widget->hide();

  m_status_renderer_widget = new QLabel(m_ui.statusBar);
  m_status_renderer_widget->setFixedHeight(16);
  m_status_renderer_widget->setFixedSize(65, 16);
  m_status_renderer_widget->hide();

  m_status_resolution_widget = new QLabel(m_ui.statusBar);
  m_status_resolution_widget->setFixedHeight(16);
  m_status_resolution_widget->setFixedSize(70, 16);
  m_status_resolution_widget->hide();

  m_status_fps_widget = new QLabel(m_ui.statusBar);
  m_status_fps_widget->setFixedSize(85, 16);
  m_status_fps_widget->hide();

  m_status_vps_widget = new QLabel(m_ui.statusBar);
  m_status_vps_widget->setFixedSize(125, 16);
  m_status_vps_widget->hide();

  m_settings_toolbar_menu = new QMenu(m_ui.toolBar);
  m_settings_toolbar_menu->addAction(m_ui.actionSettings);
  m_settings_toolbar_menu->addAction(m_ui.actionViewGameProperties);

  m_ui.actionGridViewShowTitles->setChecked(m_game_list_widget->isShowingGridCoverTitles());

  for (u32 scale = 1; scale <= 10; scale++)
  {
    QAction* action = m_ui.menuWindowSize->addAction(tr("%1x Scale").arg(scale));
    connect(action, &QAction::triggered, [scale]() { g_emu_thread->requestDisplaySize(scale); });
  }

  updateDebugMenuVisibility();

  m_shortcuts.open_file =
    new QShortcut(Qt::ControlModifier | Qt::Key_O, this, this, &MainWindow::onStartFileActionTriggered);
  m_shortcuts.game_list_refresh = new QShortcut(Qt::Key_F5, this, this, &MainWindow::onScanForNewGamesTriggered);
  m_shortcuts.game_list_search = new QShortcut(this);
  m_shortcuts.game_list_search->setKeys({Qt::ControlModifier | Qt::Key_F, Qt::Key_F3});
  connect(m_shortcuts.game_list_search, &QShortcut::activated, m_game_list_widget, &GameListWidget::focusSearchWidget);
  m_shortcuts.game_grid_zoom_in =
    new QShortcut(Qt::ControlModifier | Qt::Key_Plus, this, this, &MainWindow::onViewGameGridZoomInActionTriggered);
  m_shortcuts.game_grid_zoom_out =
    new QShortcut(Qt::ControlModifier | Qt::Key_Minus, this, this, &MainWindow::onViewGameGridZoomOutActionTriggered);

#ifdef ENABLE_RAINTEGRATION
  if (Achievements::IsUsingRAIntegration())
  {
    QMenu* raMenu = new QMenu(QStringLiteral("&RAIntegration"));
    m_ui.menuBar->insertMenu(m_ui.menuDebug->menuAction(), raMenu);
    connect(raMenu, &QMenu::aboutToShow, this, [this, raMenu]() {
      raMenu->clear();

      const auto items = Achievements::RAIntegration::GetMenuItems();
      for (const auto& [id, title, checked] : items)
      {
        if (id == 0)
        {
          raMenu->addSeparator();
          continue;
        }

        QAction* raAction = raMenu->addAction(QString::fromUtf8(title));
        if (checked)
        {
          raAction->setCheckable(true);
          raAction->setChecked(checked);
        }

        connect(raAction, &QAction::triggered, this,
                [id = id]() { Host::RunOnCPUThread([id]() { Achievements::RAIntegration::ActivateMenuItem(id); }); });
      }
    });
  }
#endif
}

void MainWindow::updateEmulationActions(bool starting, bool running, bool cheevos_challenge_mode)
{
  const bool starting_or_running = (starting || running);
  const bool starting_or_not_running = (starting || !running);
  m_ui.actionStartFile->setDisabled(starting_or_running);
  m_ui.actionStartDisc->setDisabled(starting_or_running);
  m_ui.actionStartBios->setDisabled(starting_or_running);
  m_ui.actionResumeLastState->setDisabled(starting_or_running || cheevos_challenge_mode);
  m_ui.actionStartFullscreenUI->setDisabled(starting_or_running);
  m_ui.actionStartFullscreenUI2->setDisabled(starting_or_running);

  m_ui.actionPowerOff->setDisabled(starting_or_not_running);
  m_ui.actionPowerOffWithoutSaving->setDisabled(starting_or_not_running);
  m_ui.actionReset->setDisabled(starting_or_not_running);
  m_ui.actionPause->setDisabled(starting_or_not_running);
  m_ui.actionChangeDisc->setDisabled(starting_or_not_running);
  m_ui.actionCheatsToolbar->setDisabled(starting_or_not_running || cheevos_challenge_mode);
  m_ui.actionScreenshot->setDisabled(starting_or_not_running);
  m_ui.menuChangeDisc->setDisabled(starting_or_not_running);
  m_ui.menuCheats->setDisabled(starting_or_not_running || cheevos_challenge_mode);
  m_ui.actionCPUDebugger->setDisabled(cheevos_challenge_mode);
  m_ui.actionMemoryScanner->setDisabled(cheevos_challenge_mode);
  m_ui.actionReloadTextureReplacements->setDisabled(starting_or_not_running);
  m_ui.actionDumpRAM->setDisabled(starting_or_not_running || cheevos_challenge_mode);
  m_ui.actionDumpVRAM->setDisabled(starting_or_not_running || cheevos_challenge_mode);
  m_ui.actionDumpSPURAM->setDisabled(starting_or_not_running || cheevos_challenge_mode);

  m_ui.actionSaveState->setDisabled(starting_or_not_running);
  m_ui.menuSaveState->setDisabled(starting_or_not_running);
  m_ui.menuWindowSize->setDisabled(starting_or_not_running);

  m_ui.actionViewGameProperties->setDisabled(starting_or_not_running);

  m_shortcuts.open_file->setEnabled(!starting_or_running);
  m_shortcuts.game_list_refresh->setEnabled(!starting_or_running);
  m_shortcuts.game_list_search->setEnabled(!starting_or_running);
  m_shortcuts.game_grid_zoom_in->setEnabled(!starting_or_running);
  m_shortcuts.game_grid_zoom_out->setEnabled(!starting_or_running);

  if (starting_or_running)
  {
    if (!m_ui.toolBar->actions().contains(m_ui.actionPowerOff))
    {
      m_ui.toolBar->insertAction(m_ui.actionResumeLastState, m_ui.actionPowerOff);
      m_ui.toolBar->removeAction(m_ui.actionResumeLastState);
    }
  }
  else
  {
    if (!m_ui.toolBar->actions().contains(m_ui.actionResumeLastState))
    {
      m_ui.toolBar->insertAction(m_ui.actionPowerOff, m_ui.actionResumeLastState);
      m_ui.toolBar->removeAction(m_ui.actionPowerOff);
    }

    m_ui.actionViewGameProperties->setEnabled(false);
  }

  m_ui.statusBar->clearMessage();
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

void MainWindow::updateWindowState(bool force_visible)
{
  // Skip all of this when we're closing, since we don't want to make ourselves visible and cancel it.
  if (m_is_closing)
    return;

  const bool hide_window = !isRenderingToMain() && shouldHideMainWindow();
  const bool disable_resize = Host::GetBoolSettingValue("Main", "DisableWindowResize", false);
  const bool has_window = s_system_valid || m_display_widget;

  // Need to test both valid and display widget because of startup (vm invalid while window is created).
  const bool visible = force_visible || !hide_window || !has_window;
  if (isVisible() != visible)
    setVisible(visible);

  // No point changing realizability if we're not visible.
  const bool resizeable = force_visible || !disable_resize || !has_window;
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
  if (s_use_central_widget)
    return (centralWidget() == m_game_list_widget);
  else
    return (m_ui.mainContainer->currentIndex() == 0);
}

bool MainWindow::isRenderingFullscreen() const
{
  if (!g_gpu_device || !m_display_widget)
    return false;

  return getDisplayContainer()->isFullScreen();
}

bool MainWindow::isRenderingToMain() const
{
  if (s_use_central_widget)
    return (m_display_widget && centralWidget() == m_display_widget);
  else
    return (m_display_widget && m_ui.mainContainer->indexOf(m_display_widget) == 1);
}

bool MainWindow::shouldHideMouseCursor() const
{
  return m_hide_mouse_cursor ||
         (isRenderingFullscreen() && Host::GetBoolSettingValue("Main", "HideCursorInFullscreen", true));
}

bool MainWindow::shouldHideMainWindow() const
{
  return Host::GetBoolSettingValue("Main", "HideMainWindowWhenRunning", false) ||
         (g_emu_thread->shouldRenderToMain() && !isRenderingToMain()) || QtHost::InNoGUIMode();
}

void MainWindow::switchToGameListView()
{
  if (!isShowingGameList())
  {
    if (m_display_created)
    {
      m_was_paused_on_surface_loss = s_system_paused;
      if (!s_system_paused)
        g_emu_thread->setSystemPaused(true);

      // switch to surfaceless. we have to wait until the display widget is gone before we swap over.
      g_emu_thread->setSurfaceless(true);
      QtUtils::ProcessEventsWithSleep(QEventLoop::ExcludeUserInputEvents,
                                      [this]() { return static_cast<bool>(m_display_widget); });
    }
  }

  m_game_list_widget->setFocus();
}

void MainWindow::switchToEmulationView()
{
  if (!m_display_created || !isShowingGameList())
    return;

  // we're no longer surfaceless! this will call back to UpdateDisplay(), which will swap the widget out.
  g_emu_thread->setSurfaceless(false);

  // resume if we weren't paused at switch time
  if (s_system_paused && !m_was_paused_on_surface_loss)
    g_emu_thread->setSystemPaused(false);

  if (m_display_widget)
    m_display_widget->setFocus();
}

void MainWindow::connectSignals()
{
  updateEmulationActions(false, false, Achievements::IsHardcoreModeActive());

  connect(qApp, &QGuiApplication::applicationStateChanged, this, &MainWindow::onApplicationStateChanged);

  connect(m_ui.actionStartFile, &QAction::triggered, this, &MainWindow::onStartFileActionTriggered);
  connect(m_ui.actionStartDisc, &QAction::triggered, this, &MainWindow::onStartDiscActionTriggered);
  connect(m_ui.actionStartBios, &QAction::triggered, this, &MainWindow::onStartBIOSActionTriggered);
  connect(m_ui.actionResumeLastState, &QAction::triggered, g_emu_thread, &EmuThread::resumeSystemFromMostRecentState);
  connect(m_ui.actionChangeDisc, &QAction::triggered, [this] { m_ui.menuChangeDisc->exec(QCursor::pos()); });
  connect(m_ui.actionChangeDiscFromFile, &QAction::triggered, this, &MainWindow::onChangeDiscFromFileActionTriggered);
  connect(m_ui.actionChangeDiscFromDevice, &QAction::triggered, this,
          &MainWindow::onChangeDiscFromDeviceActionTriggered);
  connect(m_ui.actionChangeDiscFromGameList, &QAction::triggered, this,
          &MainWindow::onChangeDiscFromGameListActionTriggered);
  connect(m_ui.menuChangeDisc, &QMenu::aboutToShow, this, &MainWindow::onChangeDiscMenuAboutToShow);
  connect(m_ui.menuChangeDisc, &QMenu::aboutToHide, this, &MainWindow::onChangeDiscMenuAboutToHide);
  connect(m_ui.menuLoadState, &QMenu::aboutToShow, this, &MainWindow::onLoadStateMenuAboutToShow);
  connect(m_ui.menuSaveState, &QMenu::aboutToShow, this, &MainWindow::onSaveStateMenuAboutToShow);
  connect(m_ui.menuCheats, &QMenu::aboutToShow, this, &MainWindow::onCheatsMenuAboutToShow);
  connect(m_ui.actionCheatsToolbar, &QAction::triggered, this, &MainWindow::onCheatsActionTriggered);
  connect(m_ui.actionStartFullscreenUI, &QAction::triggered, this, &MainWindow::onStartFullscreenUITriggered);
  connect(m_ui.actionStartFullscreenUI2, &QAction::triggered, this, &MainWindow::onStartFullscreenUITriggered);
  connect(m_ui.actionRemoveDisc, &QAction::triggered, this, &MainWindow::onRemoveDiscActionTriggered);
  connect(m_ui.actionAddGameDirectory, &QAction::triggered,
          [this]() { getSettingsWindow()->getGameListSettingsWidget()->addSearchDirectory(this); });
  connect(m_ui.actionPowerOff, &QAction::triggered, this,
          [this]() { requestShutdown(true, true, g_settings.save_state_on_exit); });
  connect(m_ui.actionPowerOffWithoutSaving, &QAction::triggered, this,
          [this]() { requestShutdown(false, false, false); });
  connect(m_ui.actionReset, &QAction::triggered, this, []() { g_emu_thread->resetSystem(true); });
  connect(m_ui.actionPause, &QAction::toggled, this, [](bool active) { g_emu_thread->setSystemPaused(active); });
  connect(m_ui.actionScreenshot, &QAction::triggered, g_emu_thread, &EmuThread::saveScreenshot);
  connect(m_ui.actionScanForNewGames, &QAction::triggered, this, &MainWindow::onScanForNewGamesTriggered);
  connect(m_ui.actionRescanAllGames, &QAction::triggered, this, [this]() { refreshGameList(true); });
  connect(m_ui.actionLoadState, &QAction::triggered, this, [this]() { m_ui.menuLoadState->exec(QCursor::pos()); });
  connect(m_ui.actionSaveState, &QAction::triggered, this, [this]() { m_ui.menuSaveState->exec(QCursor::pos()); });
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
  connect(m_ui.actionViewToolbar, &QAction::toggled, this, &MainWindow::onViewToolbarActionToggled);
  connect(m_ui.actionViewLockToolbar, &QAction::toggled, this, &MainWindow::onViewLockToolbarActionToggled);
  connect(m_ui.actionViewStatusBar, &QAction::toggled, this, &MainWindow::onViewStatusBarActionToggled);
  connect(m_ui.actionViewGameList, &QAction::triggered, this, &MainWindow::onViewGameListActionTriggered);
  connect(m_ui.actionViewGameGrid, &QAction::triggered, this, &MainWindow::onViewGameGridActionTriggered);
  connect(m_ui.actionViewSystemDisplay, &QAction::triggered, this, &MainWindow::onViewSystemDisplayTriggered);
  connect(m_ui.actionViewGameProperties, &QAction::triggered, this, [this]() { openGamePropertiesForCurrentGame(); });
  connect(m_ui.actionGitHubRepository, &QAction::triggered, this, &MainWindow::onGitHubRepositoryActionTriggered);
  connect(m_ui.actionDiscordServer, &QAction::triggered, this, &MainWindow::onDiscordServerActionTriggered);
  connect(m_ui.actionViewThirdPartyNotices, &QAction::triggered, this,
          [this]() { AboutDialog::showThirdPartyNotices(this); });
  connect(m_ui.actionAboutQt, &QAction::triggered, qApp, &QApplication::aboutQt);
  connect(m_ui.actionAbout, &QAction::triggered, this, &MainWindow::onAboutActionTriggered);
  connect(m_ui.actionCheckForUpdates, &QAction::triggered, this, &MainWindow::onCheckForUpdatesActionTriggered);
  connect(m_ui.actionMemoryCardEditor, &QAction::triggered, this, &MainWindow::onToolsMemoryCardEditorTriggered);
  connect(m_ui.actionMemoryScanner, &QAction::triggered, this, &MainWindow::onToolsMemoryScannerTriggered);
  connect(m_ui.actionCoverDownloader, &QAction::triggered, this, &MainWindow::onToolsCoverDownloaderTriggered);
  connect(m_ui.actionMediaCapture, &QAction::toggled, this, &MainWindow::onToolsMediaCaptureToggled);
  connect(m_ui.actionCaptureGPUFrame, &QAction::triggered, g_emu_thread, &EmuThread::captureGPUFrameDump);
  connect(m_ui.actionCPUDebugger, &QAction::triggered, this, &MainWindow::openCPUDebugger);
  connect(m_ui.actionOpenDataDirectory, &QAction::triggered, this, &MainWindow::onToolsOpenDataDirectoryTriggered);
  connect(m_ui.actionOpenTextureDirectory, &QAction::triggered, this,
          &MainWindow::onToolsOpenTextureDirectoryTriggered);
  connect(m_ui.actionReloadTextureReplacements, &QAction::triggered, g_emu_thread,
          &EmuThread::reloadTextureReplacements);
  connect(m_ui.actionMergeDiscSets, &QAction::triggered, m_game_list_widget, &GameListWidget::setMergeDiscSets);
  connect(m_ui.actionShowGameIcons, &QAction::triggered, m_game_list_widget, &GameListWidget::setShowGameIcons);
  connect(m_ui.actionGridViewShowTitles, &QAction::triggered, m_game_list_widget, &GameListWidget::setShowCoverTitles);
  connect(m_ui.actionGridViewZoomIn, &QAction::triggered, this, &MainWindow::onViewGameGridZoomInActionTriggered);
  connect(m_ui.actionGridViewZoomOut, &QAction::triggered, this, &MainWindow::onViewGameGridZoomOutActionTriggered);
  connect(m_ui.actionGridViewRefreshCovers, &QAction::triggered, m_game_list_widget,
          &GameListWidget::refreshGridCovers);

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
  connect(g_emu_thread, &EmuThread::systemDestroyed, this, &MainWindow::onSystemDestroyed);
  connect(g_emu_thread, &EmuThread::systemPaused, this, &MainWindow::onSystemPaused);
  connect(g_emu_thread, &EmuThread::systemResumed, this, &MainWindow::onSystemResumed);
  connect(g_emu_thread, &EmuThread::runningGameChanged, this, &MainWindow::onRunningGameChanged);
  connect(g_emu_thread, &EmuThread::mediaCaptureStarted, this, &MainWindow::onMediaCaptureStarted);
  connect(g_emu_thread, &EmuThread::mediaCaptureStopped, this, &MainWindow::onMediaCaptureStopped);
  connect(g_emu_thread, &EmuThread::mouseModeRequested, this, &MainWindow::onMouseModeRequested);
  connect(g_emu_thread, &EmuThread::fullscreenUIStartedOrStopped, this, &MainWindow::onFullscreenUIStartedOrStopped);
  connect(g_emu_thread, &EmuThread::achievementsLoginRequested, this, &MainWindow::onAchievementsLoginRequested);
  connect(g_emu_thread, &EmuThread::achievementsChallengeModeChanged, this,
          &MainWindow::onAchievementsChallengeModeChanged);
  connect(g_emu_thread, &EmuThread::onCoverDownloaderOpenRequested, this, &MainWindow::onToolsCoverDownloaderTriggered);
  connect(g_emu_thread, &EmuThread::onCreateAuxiliaryRenderWindow, this, &MainWindow::onCreateAuxiliaryRenderWindow,
          Qt::BlockingQueuedConnection);
  connect(g_emu_thread, &EmuThread::onDestroyAuxiliaryRenderWindow, this, &MainWindow::onDestroyAuxiliaryRenderWindow,
          Qt::BlockingQueuedConnection);

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
                                             Settings::DEFAULT_LOG_LEVEL, Log::Level::MaxCount);
  connect(m_ui.menuLogChannels, &QMenu::aboutToShow, this, &MainWindow::onDebugLogChannelsMenuAboutToShow);

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
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionDebugShowGPUState, "DebugWindows", "GPU", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionDebugShowCDROMState, "DebugWindows", "CDROM", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionDebugShowSPUState, "DebugWindows", "SPU", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionDebugShowTimersState, "DebugWindows", "Timers",
                                               false);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionDebugShowMDECState, "DebugWindows", "MDEC", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionDebugShowDMAState, "DebugWindows", "DMA", false);
}

void MainWindow::updateTheme()
{
  QtHost::UpdateApplicationTheme();
  reloadThemeSpecificImages();
}

void MainWindow::reloadThemeSpecificImages()
{
  m_game_list_widget->reloadThemeSpecificImages();
}

void MainWindow::onSettingsThemeChanged()
{
#ifdef _WIN32
  const QString old_style_name = qApp->style()->name();
#endif

  updateTheme();

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

void MainWindow::saveStateToConfig()
{
  if (!isVisible() || ((windowState() & Qt::WindowFullScreen) != Qt::WindowNoState))
    return;

  bool changed = QtUtils::SaveWindowGeometry("MainWindow", this, false);

  const QByteArray state(saveState());
  const QByteArray state_b64(state.toBase64());
  const std::string old_state_b64(Host::GetBaseStringSettingValue("UI", "MainWindowState"));
  if (old_state_b64 != state_b64.constData())
  {
    Host::SetBaseStringSettingValue("UI", "MainWindowState", state_b64.constData());
    changed = true;
  }

  if (changed)
    Host::CommitBaseSettingChanges();
}

void MainWindow::restoreStateFromConfig()
{
  QtUtils::RestoreWindowGeometry("MainWindow", this);

  {
    const std::string state_b64 = Host::GetBaseStringSettingValue("UI", "MainWindowState");
    const QByteArray state = QByteArray::fromBase64(QByteArray::fromStdString(state_b64));
    if (!state.isEmpty())
    {
      restoreState(state);

      // make sure we're not loading a dodgy config which had fullscreen set...
      setWindowState(windowState() & ~(Qt::WindowFullScreen | Qt::WindowActive));
    }

    {
      QSignalBlocker sb(m_ui.actionViewToolbar);
      m_ui.actionViewToolbar->setChecked(!m_ui.toolBar->isHidden());
    }
    {
      QSignalBlocker sb(m_ui.actionViewStatusBar);
      m_ui.actionViewStatusBar->setChecked(!m_ui.statusBar->isHidden());
    }
  }
}

void MainWindow::saveDisplayWindowGeometryToConfig()
{
  QWidget* const container = getDisplayContainer();
  if (container->windowState() & Qt::WindowFullScreen)
  {
    // if we somehow ended up here, don't save the fullscreen state to the config
    return;
  }

  QtUtils::SaveWindowGeometry("DisplayWindow", container);
}

void MainWindow::restoreDisplayWindowGeometryFromConfig()
{
  QWidget* const container = getDisplayContainer();
  if (!QtUtils::RestoreWindowGeometry("DisplayWindow", container))
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

  Host::RunOnCPUThread([category]() {
    const std::string& path = System::GetDiscPath();
    const std::string& serial = System::GetGameSerial();
    if (path.empty() || serial.empty())
      return;

    QtHost::RunOnUIThread([title = std::string(System::GetGameTitle()), path = std::string(path),
                           serial = std::string(serial), hash = System::GetGameHash(), region = System::GetDiscRegion(),
                           category]() {
      SettingsWindow::openGamePropertiesDialog(path, title, std::move(serial), hash, region, category);
    });
  });
}

ControllerSettingsWindow* MainWindow::getControllerSettingsWindow()
{
  if (!m_controller_settings_window)
    m_controller_settings_window = new ControllerSettingsWindow();

  return m_controller_settings_window;
}

void MainWindow::doControllerSettings(
  ControllerSettingsWindow::Category category /*= ControllerSettingsDialog::Category::Count*/)
{
  ControllerSettingsWindow* dlg = getControllerSettingsWindow();
  QtUtils::ShowOrRaiseWindow(dlg);
  if (category != ControllerSettingsWindow::Category::Count)
    dlg->setCategory(category);
}

void MainWindow::openInputProfileEditor(const std::string_view name)
{
  ControllerSettingsWindow* dlg = getControllerSettingsWindow();
  QtUtils::ShowOrRaiseWindow(dlg);
  dlg->switchProfile(name);
}

void MainWindow::showEvent(QShowEvent* event)
{
  QMainWindow::showEvent(event);

  // This is a bit silly, but for some reason resizing *before* the window is shown
  // gives the incorrect sizes for columns, if you set the style before setting up
  // the rest of the window... so, instead, let's just force it to be resized on show.
  if (isShowingGameList())
    m_game_list_widget->resizeTableViewColumnsToFit();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
  // If there's no VM, we can just exit as normal.
  if (!s_system_valid || !m_display_created)
  {
    saveStateToConfig();
    if (m_display_created)
      g_emu_thread->stopFullscreenUI();
    destroySubWindows();
    QMainWindow::closeEvent(event);
    return;
  }

  // But if there is, we have to cancel the action, regardless of whether we ended exiting
  // or not. The window still needs to be visible while GS is shutting down.
  event->ignore();

  // Exit cancelled?
  if (!requestShutdown(true, true, g_settings.save_state_on_exit))
    return;

  // Application will be exited in VM stopped handler.
  saveStateToConfig();
  m_is_closing = true;
}

void MainWindow::changeEvent(QEvent* event)
{
  if (static_cast<QWindowStateChangeEvent*>(event)->oldState() & Qt::WindowMinimized)
  {
    // TODO: This should check the render-to-main option.
    if (m_display_widget)
      g_emu_thread->redrawDisplayWindow();
  }

  if (event->type() == QEvent::StyleChange)
  {
    QtHost::SetIconThemeFromStyle();
    reloadThemeSpecificImages();
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
  m_game_list_widget->refreshModel();
}

void MainWindow::cancelGameListRefresh()
{
  m_game_list_widget->cancelRefresh();
}

void MainWindow::runOnUIThread(const std::function<void()>& func)
{
  func();
}

bool MainWindow::requestShutdown(bool allow_confirm /* = true */, bool allow_save_to_state /* = true */,
                                 bool save_state /* = true */)
{
  if (!s_system_valid)
    return true;

  // If we don't have a serial, we can't save state.
  allow_save_to_state &= !s_current_game_serial.isEmpty();
  save_state &= allow_save_to_state;

  // Only confirm on UI thread because we need to display a msgbox.
  if (!m_is_closing && allow_confirm && Host::GetBoolSettingValue("Main", "ConfirmPowerOff", true))
  {
    SystemLock lock(pauseAndLockSystem());

    QMessageBox msgbox(lock.getDialogParent());
    msgbox.setIcon(QMessageBox::Question);
    msgbox.setWindowTitle(tr("Confirm Shutdown"));
    msgbox.setWindowModality(Qt::WindowModal);
    msgbox.setText(tr("Are you sure you want to shut down the virtual machine?"));

    QCheckBox* save_cb = new QCheckBox(tr("Save State For Resume"), &msgbox);
    save_cb->setChecked(allow_save_to_state && save_state);
    save_cb->setEnabled(allow_save_to_state);
    msgbox.setCheckBox(save_cb);
    msgbox.addButton(QMessageBox::Yes);
    msgbox.addButton(QMessageBox::No);
    msgbox.setDefaultButton(QMessageBox::Yes);
    if (msgbox.exec() != QMessageBox::Yes)
      return false;

    save_state = save_cb->isChecked();

    // Don't switch back to fullscreen when we're shutting down anyway.
    lock.cancelResume();
  }

  // This is a little bit annoying. Qt will close everything down if we don't have at least one window visible,
  // but we might not be visible because the user is using render-to-separate and hide. We don't want to always
  // reshow the main window during display updates, because otherwise fullscreen transitions and renderer switches
  // would briefly show and then hide the main window. So instead, we do it on shutdown, here. Except if we're in
  // batch mode, when we're going to exit anyway.
  if (!isRenderingToMain() && isHidden() && !QtHost::InBatchMode() && !s_fullscreen_ui_started)
    updateWindowState(true);

  // Now we can actually shut down the VM.
  g_emu_thread->shutdownSystem(save_state, true);
  return true;
}

void MainWindow::requestExit(bool allow_confirm /* = true */)
{
  // this is block, because otherwise closeEvent() will also prompt
  if (!requestShutdown(allow_confirm, true, g_settings.save_state_on_exit))
    return;

  // VM stopped signal won't have fired yet, so queue an exit if we still have one.
  // Otherwise, immediately exit, because there's no VM to exit us later.
  if (s_system_valid)
    m_is_closing = true;
  else
    quit();
}

void MainWindow::checkForSettingChanges()
{
  LogWindow::updateSettings();
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
  for (const QString& card_path : {card_a_path, card_b_path})
  {
    if (!card_path.isEmpty() && !QFile::exists(card_path))
    {
      if (QMessageBox::question(
            this, tr("Memory Card Not Found"),
            tr("Memory card '%1' does not exist. Do you want to create an empty memory card?").arg(card_path),
            QMessageBox::Yes, QMessageBox::No) == QMessageBox::Yes)
      {
        Error error;
        if (!MemoryCardEditorWindow::createMemoryCard(card_path, &error))
        {
          QMessageBox::critical(this, tr("Memory Card Not Found"),
                                tr("Failed to create memory card '%1': %2")
                                  .arg(card_path)
                                  .arg(QString::fromStdString(error.GetDescription())));
        }
      }
    }
  }

  if (!m_memory_card_editor_window)
    m_memory_card_editor_window = new MemoryCardEditorWindow();

  QtUtils::ShowOrRaiseWindow(m_memory_card_editor_window);

  if (!card_a_path.isEmpty())
  {
    if (!m_memory_card_editor_window->setCardA(card_a_path))
    {
      QMessageBox::critical(
        this, tr("Memory Card Not Found"),
        tr("Memory card '%1' could not be found. Try starting the game and saving to create it.").arg(card_a_path));
    }
  }
  if (!card_b_path.isEmpty())
  {
    if (!m_memory_card_editor_window->setCardB(card_b_path))
    {
      QMessageBox::critical(
        this, tr("Memory Card Not Found"),
        tr("Memory card '%1' could not be found. Try starting the game and saving to create it.").arg(card_b_path));
    }
  }
}

void MainWindow::onAchievementsLoginRequested(Achievements::LoginRequestReason reason)
{
  const auto lock = pauseAndLockSystem();

  AchievementLoginDialog dlg(lock.getDialogParent(), reason);
  dlg.exec();
}

void MainWindow::onAchievementsChallengeModeChanged(bool enabled)
{
  if (enabled)
  {
    QtUtils::CloseAndDeleteWindow(m_debugger_window);
    QtUtils::CloseAndDeleteWindow(m_memory_scanner_window);
  }

  updateEmulationActions(false, System::IsValid(), enabled);
}

bool MainWindow::onCreateAuxiliaryRenderWindow(RenderAPI render_api, qint32 x, qint32 y, quint32 width, quint32 height,
                                               const QString& title, const QString& icon_name,
                                               Host::AuxiliaryRenderWindowUserData userdata,
                                               Host::AuxiliaryRenderWindowHandle* handle, WindowInfo* wi, Error* error)
{
  AuxiliaryDisplayWidget* widget = AuxiliaryDisplayWidget::create(x, y, width, height, title, icon_name, userdata);
  if (!widget)
    return false;

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
  SystemLock lock(pauseAndLockSystem());
  CoverDownloadDialog dlg(lock.getDialogParent());
  connect(&dlg, &CoverDownloadDialog::coverRefreshRequested, m_game_list_widget, &GameListWidget::refreshGridCovers);
  dlg.exec();
}

void MainWindow::onToolsMediaCaptureToggled(bool checked)
{
  if (!QtHost::IsSystemValid())
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
    const QSignalBlocker sb(m_ui.actionMediaCapture);
    m_ui.actionMediaCapture->setChecked(false);
    return;
  }

  Host::RunOnCPUThread([path = path.toStdString()]() { System::StartMediaCapture(path); });
}

void MainWindow::onToolsMemoryScannerTriggered()
{
  if (Achievements::IsHardcoreModeActive())
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

void MainWindow::openCPUDebugger()
{
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

void MainWindow::onSettingsTriggeredFromToolbar()
{
  if (s_system_valid)
    m_settings_toolbar_menu->exec(QCursor::pos());
  else
    doSettings();
}

void MainWindow::checkForUpdates(bool display_message)
{
  if (!AutoUpdaterDialog::isSupported())
  {
    if (display_message)
    {
      QMessageBox mbox(this);
      mbox.setWindowTitle(tr("Updater Error"));
      mbox.setWindowModality(Qt::WindowModal);
      mbox.setTextFormat(Qt::RichText);

      QString message;
      if (!AutoUpdaterDialog::isOfficialBuild())
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
    return;

  m_auto_updater_dialog = new AutoUpdaterDialog(this);
  connect(m_auto_updater_dialog, &AutoUpdaterDialog::updateCheckCompleted, this, &MainWindow::onUpdateCheckComplete);
  m_auto_updater_dialog->queueUpdateCheck(display_message);
}

void* MainWindow::getNativeWindowId()
{
  return (void*)winId();
}

void MainWindow::onUpdateCheckComplete()
{
  if (!m_auto_updater_dialog)
    return;

  m_auto_updater_dialog->deleteLater();
  m_auto_updater_dialog = nullptr;
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
  const bool was_fullscreen = g_emu_thread->isFullscreen() && !s_use_central_widget;
#else
  const bool was_fullscreen = false;
#endif
  const bool was_paused = !s_system_valid || s_system_paused;

  // We need to switch out of exclusive fullscreen before we can display our popup.
  // However, we do not want to switch back to render-to-main, the window might have generated this event.
  if (was_fullscreen)
  {
    g_emu_thread->setFullscreen(false, false);

    // Container could change... thanks Wayland.
    QtUtils::ProcessEventsWithSleep(QEventLoop::ExcludeUserInputEvents, [this]() {
      QWidget* container;
      return (s_system_valid &&
              (g_emu_thread->isFullscreen() || !(container = getDisplayContainer()) || container->isFullScreen()));
    });
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
  QWidget* dialog_parent = s_system_valid ? getDisplayContainer() : this;

  return SystemLock(dialog_parent, was_paused, was_fullscreen);
}

MainWindow::SystemLock::SystemLock(QWidget* dialog_parent, bool was_paused, bool was_fullscreen)
  : m_dialog_parent(dialog_parent), m_was_paused(was_paused), m_was_fullscreen(was_fullscreen)
{
  s_system_locked.fetch_add(1, std::memory_order_release);
}

MainWindow::SystemLock::SystemLock(SystemLock&& lock)
  : m_dialog_parent(lock.m_dialog_parent), m_was_paused(lock.m_was_paused), m_was_fullscreen(lock.m_was_fullscreen)
{
  s_system_locked.fetch_add(1, std::memory_order_release);
  lock.m_dialog_parent = nullptr;
  lock.m_was_paused = true;
  lock.m_was_fullscreen = false;
}

MainWindow::SystemLock::~SystemLock()
{
  DebugAssert(s_system_locked.load(std::memory_order_relaxed) > 0);
  s_system_locked.fetch_sub(1, std::memory_order_release);
  if (m_was_fullscreen)
    g_emu_thread->setFullscreen(true, true);
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
