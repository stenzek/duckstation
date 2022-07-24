#include "mainwindow.h"
#include "aboutdialog.h"
#include "autoupdaterdialog.h"
#include "cheatmanagerdialog.h"
#include "common/assert.h"
#include "common/file_system.h"
#include "common/log.h"
#include "core/host.h"
#include "core/host_display.h"
#include "core/memory_card.h"
#include "core/settings.h"
#include "core/system.h"
#include "debuggerwindow.h"
#include "displaywidget.h"
#include "frontend-common/game_list.h"
#include "gamelistsettingswidget.h"
#include "gamelistwidget.h"
#include "gdbserver.h"
#include "memorycardeditordialog.h"
#include "qthost.h"
#include "qtutils.h"
#include "settingsdialog.h"
#include "settingwidgetbinder.h"
#include "util/cd_image.h"

#ifdef WITH_CHEEVOS
#include "frontend-common/achievements.h"
#endif

#include <QtCore/QDebug>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMimeData>
#include <QtCore/QUrl>
#include <QtGui/QActionGroup>
#include <QtGui/QCursor>
#include <QtGui/QWindowStateChangeEvent>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QStyleFactory>
#include <cmath>

Log_SetChannel(MainWindow);

static constexpr char DISC_IMAGE_FILTER[] = QT_TRANSLATE_NOOP(
  "MainWindow",
  "All File Types (*.bin *.img *.iso *.cue *.chd *.ecm *.mds *.pbp *.exe *.psexe *.ps-exe *.psf *.minipsf "
  "*.m3u);;Single-Track "
  "Raw Images (*.bin *.img *.iso);;Cue Sheets (*.cue);;MAME CHD Images (*.chd);;Error Code Modeler Images "
  "(*.ecm);;Media Descriptor Sidecar Images (*.mds);;PlayStation EBOOTs (*.pbp);;PlayStation Executables (*.exe "
  "*.psexe *.ps-exe);;Portable Sound Format Files (*.psf *.minipsf);;Playlists (*.m3u)");

static const char* DEFAULT_THEME_NAME = "darkfusion";

MainWindow* g_main_window = nullptr;
static QString s_unthemed_style_name;
static bool s_unthemed_style_name_set;

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

bool QtHost::IsSystemPaused()
{
  return s_system_paused;
}

bool QtHost::IsSystemValid()
{
  return s_system_valid;
}

MainWindow::MainWindow() : QMainWindow(nullptr)
{
  Assert(!g_main_window);
  g_main_window = this;

#if !defined(_WIN32) && !defined(__APPLE__)
  s_use_central_widget = DisplayContainer::isRunningOnWayland();
#endif
}

MainWindow::~MainWindow()
{
  Assert(!m_display_widget);
  Assert(!m_debugger_window);

  // we compare here, since recreate destroys the window later
  if (g_main_window == this)
    g_main_window = nullptr;
}

void MainWindow::updateApplicationTheme()
{
  if (!s_unthemed_style_name_set)
  {
    s_unthemed_style_name_set = true;
    s_unthemed_style_name = QApplication::style()->objectName();
  }

  setStyleFromSettings();
  setIconThemeFromSettings();
}

void MainWindow::initialize()
{
  m_ui.setupUi(this);
  setupAdditionalUi();
  connectSignals();

  restoreGeometryFromConfig();
  switchToGameListView();
  updateWindowTitle();

#ifdef WITH_RAINTEGRATION
  if (Achievements::IsUsingRAIntegration())
    Achievements::RAIntegration::MainWindowChanged((void*)winId());
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

bool MainWindow::shouldHideCursorInFullscreen() const
{
  return Host::GetBoolSettingValue("Main", "HideCursorInFullscreen", true);
}

bool MainWindow::createDisplay(bool fullscreen, bool render_to_main)
{
  Log_DevPrintf("createDisplay(%u, %u)", static_cast<u32>(fullscreen), static_cast<u32>(render_to_main));

  const std::string fullscreen_mode(Host::GetBaseStringSettingValue("GPU", "FullscreenMode", ""));
  const bool is_exclusive_fullscreen = (fullscreen && !fullscreen_mode.empty() && g_host_display->SupportsFullscreen());

  createDisplayWidget(fullscreen, render_to_main, is_exclusive_fullscreen);

  std::optional<WindowInfo> wi = m_display_widget->getWindowInfo();
  if (!wi.has_value())
  {
    QMessageBox::critical(this, tr("Error"), tr("Failed to get window info from widget"));
    destroyDisplayWidget(true);
    return false;
  }

  g_emu_thread->connectDisplaySignals(m_display_widget);

  if (!g_host_display->CreateRenderDevice(wi.value(), g_settings.gpu_adapter, g_settings.gpu_use_debug_device,
                                          g_settings.gpu_threaded_presentation))
  {
    QMessageBox::critical(this, tr("Error"), tr("Failed to create host display device context."));
    destroyDisplayWidget(true);
    return false;
  }

  m_display_created = true;

  if (is_exclusive_fullscreen)
    setDisplayFullscreen(fullscreen_mode);

  updateWindowTitle();
  updateWindowState();

  m_display_widget->setFocus();
  m_ui.actionStartFullscreenUI->setEnabled(false);
  m_ui.actionStartFullscreenUI2->setEnabled(false);
  m_ui.actionViewSystemDisplay->setEnabled(true);
  m_ui.actionFullscreen->setEnabled(true);

  m_display_widget->setFocus();
  m_display_widget->setShouldHideCursor(shouldHideMouseCursor());
  m_display_widget->updateRelativeMode(s_system_valid && !s_system_paused);
  m_display_widget->updateCursor(s_system_valid && !s_system_paused);

  g_host_display->DoneRenderContextCurrent();
  return true;
}

bool MainWindow::updateDisplay(bool fullscreen, bool render_to_main, bool surfaceless)
{
  Log_DevPrintf("updateDisplay() fullscreen=%s render_to_main=%s surfaceless=%s", fullscreen ? "true" : "false",
                render_to_main ? "true" : "false", surfaceless ? "true" : "false");

  QWidget* container =
    m_display_container ? static_cast<QWidget*>(m_display_container) : static_cast<QWidget*>(m_display_widget);
  const bool is_fullscreen = isRenderingFullscreen();
  const bool is_rendering_to_main = isRenderingToMain();
  const std::string fullscreen_mode(Host::GetBaseStringSettingValue("GPU", "FullscreenMode", ""));
  const bool is_exclusive_fullscreen = (fullscreen && !fullscreen_mode.empty() && g_host_display->SupportsFullscreen());
  const bool changing_surfaceless = (!m_display_widget != surfaceless);
  if (fullscreen == is_fullscreen && is_rendering_to_main == render_to_main && !changing_surfaceless)
    return true;

  // Skip recreating the surface if we're just transitioning between fullscreen and windowed with render-to-main off.
  // .. except on Wayland, where everything tends to break if you don't recreate.
  const bool has_container = (m_display_container != nullptr);
  const bool needs_container = DisplayContainer::isNeeded(fullscreen, render_to_main);
  if (!is_rendering_to_main && !render_to_main && !is_exclusive_fullscreen && has_container == needs_container &&
      !needs_container && !changing_surfaceless)
  {
    Log_DevPrintf("Toggling to %s without recreating surface", (fullscreen ? "fullscreen" : "windowed"));
    if (g_host_display->IsFullscreen())
      g_host_display->SetFullscreen(false, 0, 0, 0.0f);

    // since we don't destroy the display widget, we need to save it here
    if (!is_fullscreen && !is_rendering_to_main)
      saveDisplayWindowGeometryToConfig();

    if (fullscreen)
    {
      container->showFullScreen();
    }
    else
    {
      restoreDisplayWindowGeometryFromConfig();
      container->showNormal();
    }

    m_display_widget->setFocus();
    m_display_widget->setShouldHideCursor(shouldHideMouseCursor());
    m_display_widget->updateRelativeMode(s_system_valid && !s_system_paused);
    m_display_widget->updateCursor(s_system_valid && !s_system_paused);

    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    return true;
  }

  g_host_display->DestroyRenderSurface();

  destroyDisplayWidget(surfaceless);

  // if we're going to surfaceless, we're done here
  if (surfaceless)
  {
    updateWindowState();
    return true;
  }

  createDisplayWidget(fullscreen, render_to_main, is_exclusive_fullscreen);

  std::optional<WindowInfo> wi = m_display_widget->getWindowInfo();
  if (!wi.has_value())
  {
    QMessageBox::critical(this, tr("Error"), tr("Failed to get new window info from widget"));
    destroyDisplayWidget(true);
    return false;
  }

  g_emu_thread->connectDisplaySignals(m_display_widget);

  if (!g_host_display->ChangeRenderWindow(wi.value()))
    Panic("Failed to recreate surface on new widget.");

  if (is_exclusive_fullscreen)
    setDisplayFullscreen(fullscreen_mode);

  updateWindowTitle();
  updateWindowState();

  m_display_widget->setFocus();
  m_display_widget->setShouldHideCursor(shouldHideMouseCursor());
  m_display_widget->updateRelativeMode(m_relative_mouse_mode && s_system_valid && !s_system_paused);
  m_display_widget->updateCursor(s_system_valid && !s_system_paused);

  QSignalBlocker blocker(m_ui.actionFullscreen);
  m_ui.actionFullscreen->setChecked(fullscreen);
  return true;
}

void MainWindow::createDisplayWidget(bool fullscreen, bool render_to_main, bool is_exclusive_fullscreen)
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
      restoreDisplayWindowGeometryFromConfig();

    if (!is_exclusive_fullscreen)
      container->showFullScreen();
    else
      container->showNormal();
  }
  else if (!render_to_main)
  {
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

  // We need the surface visible.
  QGuiApplication::sync();
}

void MainWindow::setDisplayFullscreen(const std::string& fullscreen_mode)
{
  u32 width, height;
  float refresh_rate;
  bool result = false;

  if (HostDisplay::ParseFullscreenMode(fullscreen_mode, &width, &height, &refresh_rate))
  {
    result = g_host_display->SetFullscreen(true, width, height, refresh_rate);
    if (result)
    {
      Host::AddOSDMessage(Host::TranslateStdString("OSDMessage", "Acquired exclusive fullscreen."), 10.0f);
    }
    else
    {
      Host::AddOSDMessage(Host::TranslateStdString("OSDMessage", "Failed to acquire exclusive fullscreen."), 10.0f);
    }
  }
}

void MainWindow::displaySizeRequested(qint32 width, qint32 height)
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

void MainWindow::destroyDisplay()
{
  // Now we can safely destroy the display window.
  destroyDisplayWidget(true);
  m_display_created = false;

  m_ui.actionViewSystemDisplay->setEnabled(false);
  m_ui.actionFullscreen->setEnabled(false);

  m_ui.actionStartFullscreenUI->setEnabled(true);
  m_ui.actionStartFullscreenUI2->setEnabled(true);
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
    m_display_widget->deleteLater();
    m_display_widget = nullptr;
  }

  if (m_display_container)
  {
    m_display_container->deleteLater();
    m_display_container = nullptr;
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
  m_mouse_cursor_hidden = hide_cursor;

  if (m_display_widget && !s_system_paused)
  {
    m_display_widget->updateRelativeMode(m_relative_mouse_mode);
    m_display_widget->updateCursor(m_mouse_cursor_hidden);
  }
}

void MainWindow::onSystemStarting()
{
  s_system_valid = false;
  s_system_paused = false;

  updateEmulationActions(true, false, Achievements::ChallengeModeActive());
}

void MainWindow::onSystemStarted()
{
  m_was_disc_change_request = false;
  s_system_valid = true;
  updateEmulationActions(false, true, Achievements::ChallengeModeActive());
  updateWindowTitle();
  updateStatusBarWidgetVisibility();
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
  {
    m_display_widget->updateRelativeMode(false);
    m_display_widget->updateCursor(false);
  }
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
    m_display_widget->updateRelativeMode(m_relative_mouse_mode);
    m_display_widget->updateCursor(m_mouse_cursor_hidden);
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
  updateEmulationActions(false, false, Achievements::ChallengeModeActive());
  switchToGameListView();

  if (m_cheat_manager_dialog)
  {
    delete m_cheat_manager_dialog;
    m_cheat_manager_dialog = nullptr;
  }

  if (m_debugger_window)
  {
    delete m_debugger_window;
    m_debugger_window = nullptr;
  }
}

void MainWindow::onRunningGameChanged(const QString& filename, const QString& game_code, const QString& game_title)
{
  m_current_game_title = game_title.toStdString();
  m_current_game_code = game_code.toStdString();

  updateWindowTitle();
  // updateSaveStateMenus(path, serial, crc);
}

void MainWindow::onApplicationStateChanged(Qt::ApplicationState state)
{
  if (!s_system_valid || !g_settings.pause_on_focus_loss)
    return;

  const bool focus_loss = (state != Qt::ApplicationActive);
  if (focus_loss)
  {
    if (!m_was_paused_by_focus_loss && !s_system_paused)
    {
      g_emu_thread->setSystemPaused(true);
      m_was_paused_by_focus_loss = true;
    }
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

void MainWindow::recreate()
{
  if (s_system_valid)
    requestShutdown(false, true, true);

  close();
  g_main_window = nullptr;

  MainWindow* new_main_window = new MainWindow();
  new_main_window->initialize();
  new_main_window->show();
  deleteLater();
}

void MainWindow::populateGameListContextMenu(const GameList::Entry* entry, QWidget* parent_window, QMenu* menu)
{
  QAction* resume_action = menu->addAction(tr("Resume"));
  resume_action->setEnabled(false);

  QMenu* load_state_menu = menu->addMenu(tr("Load State"));
  load_state_menu->setEnabled(false);

  if (!entry->serial.empty())
  {
    std::vector<SaveStateInfo> available_states(System::GetAvailableSaveStates(entry->serial.c_str()));
    const QString timestamp_format = QLocale::system().dateTimeFormat(QLocale::ShortFormat);
    const bool challenge_mode = Achievements::ChallengeModeActive();
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

  QAction* open_memory_cards_action = menu->addAction(tr("Edit Memory Cards..."));
  connect(open_memory_cards_action, &QAction::triggered, [this, entry]() {
    QString paths[2];
    for (u32 i = 0; i < 2; i++)
    {
      MemoryCardType type = g_settings.memory_card_types[i];
      if (entry->serial.empty() && type == MemoryCardType::PerGame)
        type = MemoryCardType::Shared;

      switch (type)
      {
        case MemoryCardType::None:
          continue;
        case MemoryCardType::Shared:
          if (g_settings.memory_card_paths[i].empty())
          {
            paths[i] = QString::fromStdString(g_settings.GetSharedMemoryCardPath(i));
          }
          else
          {
            QFileInfo path(QString::fromStdString(g_settings.memory_card_paths[i]));
            path.makeAbsolute();
            paths[i] = QDir::toNativeSeparators(path.canonicalFilePath());
          }
          break;
        case MemoryCardType::PerGame:
          paths[i] = QString::fromStdString(g_settings.GetGameMemoryCardPath(entry->serial.c_str(), i));
          break;
        case MemoryCardType::PerGameTitle:
          paths[i] = QString::fromStdString(
            g_settings.GetGameMemoryCardPath(MemoryCard::SanitizeGameTitleForFileName(entry->title).c_str(), i));
          break;
        case MemoryCardType::PerGameFileTitle:
        {
          const std::string display_name(FileSystem::GetDisplayNameFromPath(entry->path));
          paths[i] = QString::fromStdString(g_settings.GetGameMemoryCardPath(
            MemoryCard::SanitizeGameTitleForFileName(Path::GetFileTitle(display_name)).c_str(), i));
        }
        break;
        default:
          break;
      }
    }

    g_main_window->openMemoryCardEditor(paths[0], paths[1]);
  });

  const bool has_any_states = resume_action->isEnabled() || load_state_menu->isEnabled();
  QAction* delete_save_states_action = menu->addAction(tr("Delete Save States..."));
  delete_save_states_action->setEnabled(has_any_states);
  if (has_any_states)
  {
    connect(delete_save_states_action, &QAction::triggered, [this, parent_window, entry] {
      if (QMessageBox::warning(
            parent_window, tr("Confirm Save State Deletion"),
            tr("Are you sure you want to delete all save states for %1?\n\nThe saves will not be recoverable.")
              .arg(QString::fromStdString(entry->serial)),
            QMessageBox::Yes, QMessageBox::No) != QMessageBox::Yes)
      {
        return;
      }

      System::DeleteSaveStates(entry->serial.c_str(), true);
    });
  }
}

static QString FormatTimestampForSaveStateMenu(u64 timestamp)
{
  const QDateTime qtime(QDateTime::fromSecsSinceEpoch(static_cast<qint64>(timestamp)));
  return qtime.toString(QLocale::system().dateTimeFormat(QLocale::ShortFormat));
}

void MainWindow::populateLoadStateMenu(const char* game_code, QMenu* menu)
{
  auto add_slot = [this, game_code, menu](const QString& title, const QString& empty_title, bool global, s32 slot) {
    std::optional<SaveStateInfo> ssi = System::GetSaveStateInfo(global ? nullptr : game_code, slot);

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

  connect(menu->addAction(tr("Load From File...")), &QAction::triggered, [this]() {
    const QString path(
      QFileDialog::getOpenFileName(g_main_window, tr("Select Save State File"), QString(), tr("Save States (*.sav)")));
    if (path.isEmpty())
      return;

    g_emu_thread->loadState(path);
  });
  QAction* load_from_state = menu->addAction(tr("Undo Load State"));
  load_from_state->setEnabled(System::CanUndoLoadState());
  connect(load_from_state, &QAction::triggered, g_emu_thread, &EmuThread::undoLoadState);
  menu->addSeparator();

  if (game_code && std::strlen(game_code) > 0)
  {
    for (u32 slot = 1; slot <= System::PER_GAME_SAVE_STATE_SLOTS; slot++)
      add_slot(tr("Game Save %1 (%2)"), tr("Game Save %1 (Empty)"), false, static_cast<s32>(slot));

    menu->addSeparator();
  }

  for (u32 slot = 1; slot <= System::GLOBAL_SAVE_STATE_SLOTS; slot++)
    add_slot(tr("Global Save %1 (%2)"), tr("Global Save %1 (Empty)"), true, static_cast<s32>(slot));
}

void MainWindow::populateSaveStateMenu(const char* game_code, QMenu* menu)
{
  auto add_slot = [this, game_code, menu](const QString& title, const QString& empty_title, bool global, s32 slot) {
    std::optional<SaveStateInfo> ssi = System::GetSaveStateInfo(global ? nullptr : game_code, slot);

    const QString menu_title =
      ssi.has_value() ? title.arg(slot).arg(FormatTimestampForSaveStateMenu(ssi->timestamp)) : empty_title.arg(slot);

    QAction* save_action = menu->addAction(menu_title);
    connect(save_action, &QAction::triggered, [global, slot]() { g_emu_thread->saveState(global, slot); });
  };

  menu->clear();

  connect(menu->addAction(tr("Save To File...")), &QAction::triggered, []() {
    if (!System::IsValid())
      return;

    const QString path(
      QFileDialog::getSaveFileName(g_main_window, tr("Select Save State File"), QString(), tr("Save States (*.sav)")));
    if (path.isEmpty())
      return;

    g_emu_thread->saveState(path);
  });
  menu->addSeparator();

  if (game_code && std::strlen(game_code) > 0)
  {
    for (u32 slot = 1; slot <= System::PER_GAME_SAVE_STATE_SLOTS; slot++)
      add_slot(tr("Game Save %1 (%2)"), tr("Game Save %1 (Empty)"), false, static_cast<s32>(slot));

    menu->addSeparator();
  }

  for (u32 slot = 1; slot <= System::GLOBAL_SAVE_STATE_SLOTS; slot++)
    add_slot(tr("Global Save %1 (%2)"), tr("Global Save %1 (Empty)"), true, static_cast<s32>(slot));
}

void MainWindow::populateChangeDiscSubImageMenu(QMenu* menu, QActionGroup* action_group)
{
  if (!s_system_valid || !System::HasMediaSubImages())
    return;

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

void MainWindow::populateCheatsMenu(QMenu* menu)
{
  if (!s_system_valid)
    return;

  const bool has_cheat_list = System::HasCheatList();

  QMenu* enabled_menu = menu->addMenu(tr("&Enabled Cheats"));
  enabled_menu->setEnabled(false);
  QMenu* apply_menu = menu->addMenu(tr("&Apply Cheats"));
  apply_menu->setEnabled(false);
  if (has_cheat_list)
  {
    CheatList* cl = System::GetCheatList();
    for (const std::string& group : cl->GetCodeGroups())
    {
      QMenu* enabled_submenu = nullptr;
      QMenu* apply_submenu = nullptr;

      for (u32 i = 0; i < cl->GetCodeCount(); i++)
      {
        CheatCode& cc = cl->GetCode(i);
        if (cc.group != group)
          continue;

        QString desc(QString::fromStdString(cc.description));
        if (cc.IsManuallyActivated())
        {
          if (!apply_submenu)
          {
            apply_menu->setEnabled(true);
            apply_submenu = apply_menu->addMenu(QString::fromStdString(group));
          }

          QAction* action = apply_submenu->addAction(desc);
          connect(action, &QAction::triggered, [i]() { g_emu_thread->applyCheat(i); });
        }
        else
        {
          if (!enabled_submenu)
          {
            enabled_menu->setEnabled(true);
            enabled_submenu = enabled_menu->addMenu(QString::fromStdString(group));
          }

          QAction* action = enabled_submenu->addAction(desc);
          action->setCheckable(true);
          action->setChecked(cc.enabled);
          connect(action, &QAction::toggled, [i](bool enabled) { g_emu_thread->setCheatEnabled(i, enabled); });
        }
      }
    }
  }
}

std::optional<bool> MainWindow::promptForResumeState(const std::string& save_state_path)
{
  FILESYSTEM_STAT_DATA sd;
  if (save_state_path.empty() || !FileSystem::StatFile(save_state_path.c_str(), &sd))
    return false;

  QMessageBox msgbox(this);
  msgbox.setIcon(QMessageBox::Question);
  msgbox.setWindowTitle(tr("Load Resume State"));
  msgbox.setText(tr("A resume save state was found for this game, saved at:\n\n%1.\n\nDo you want to load this state, "
                    "or start from a fresh boot?")
                   .arg(QDateTime::fromSecsSinceEpoch(sd.ModificationTime, Qt::UTC).toLocalTime().toString()));

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
  std::shared_ptr<SystemBootParameters> params = std::make_shared<SystemBootParameters>();
  params->filename = std::move(path);
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
  if (!m_was_disc_change_request)
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

  g_emu_thread->changeDisc(path);
  if (reset_system)
    g_emu_thread->resetSystem();
}

void MainWindow::onStartDiscActionTriggered()
{
  std::string path(getDeviceDiscPath(tr("Start Disc")));
  if (path.empty())
    return;

  g_emu_thread->bootSystem(std::make_shared<SystemBootParameters>(std::move(path)));
}

void MainWindow::onStartBIOSActionTriggered()
{
  g_emu_thread->bootSystem(std::make_shared<SystemBootParameters>());
}

void MainWindow::onChangeDiscFromFileActionTriggered()
{
  QString filename =
    QFileDialog::getOpenFileName(this, tr("Select Disc Image"), QString(), tr(DISC_IMAGE_FILTER), nullptr);
  if (filename.isEmpty())
    return;

  g_emu_thread->changeDisc(filename);
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

  g_emu_thread->changeDisc(QString::fromStdString(path));
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
  populateLoadStateMenu(m_current_game_code.c_str(), m_ui.menuLoadState);
}

void MainWindow::onSaveStateMenuAboutToShow()
{
  populateSaveStateMenu(m_current_game_code.c_str(), m_ui.menuSaveState);
}

void MainWindow::onCheatsMenuAboutToShow()
{
  m_ui.menuCheats->clear();
  connect(m_ui.menuCheats->addAction(tr("Cheat Manager")), &QAction::triggered, this,
          &MainWindow::onToolsCheatManagerTriggered);
  m_ui.menuCheats->addSeparator();
  populateCheatsMenu(m_ui.menuCheats);
}

void MainWindow::onRemoveDiscActionTriggered()
{
  g_emu_thread->changeDisc(QString());
}

void MainWindow::onViewToolbarActionToggled(bool checked)
{
  Host::SetBaseBoolSettingValue("UI", "ShowToolbar", checked);
  m_ui.toolBar->setVisible(checked);
}

void MainWindow::onViewLockToolbarActionToggled(bool checked)
{
  Host::SetBaseBoolSettingValue("UI", "LockToolbar", checked);
  m_ui.toolBar->setMovable(!checked);
}

void MainWindow::onViewStatusBarActionToggled(bool checked)
{
  Host::SetBaseBoolSettingValue("UI", "ShowStatusBar", checked);
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

void MainWindow::onViewGamePropertiesActionTriggered()
{
  if (!s_system_valid)
    return;

  const std::string& path = System::GetRunningPath();
  const std::string& serial = System::GetRunningCode();
  if (path.empty() || serial.empty())
    return;

  SettingsDialog::openGamePropertiesDialog(path, serial, System::GetDiscRegion());
}

void MainWindow::onGitHubRepositoryActionTriggered()
{
  QtUtils::OpenURL(this, "https://github.com/stenzek/duckstation/");
}

void MainWindow::onIssueTrackerActionTriggered()
{
  QtUtils::OpenURL(this, "https://github.com/stenzek/duckstation/issues");
}

void MainWindow::onDiscordServerActionTriggered()
{
  QtUtils::OpenURL(this, "https://discord.gg/Buktv3t");
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
  const GameList::Entry* entry = m_game_list_widget->getSelectedEntry();
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

  // we might still be saving a resume state...
  // System::WaitForSaveStateFlush();

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
    QAction* action = menu.addAction(tr("Properties..."));
    connect(action, &QAction::triggered,
            [this, entry]() { SettingsDialog::openGamePropertiesDialog(entry->path, entry->serial, entry->region); });

    connect(menu.addAction(tr("Open Containing Directory...")), &QAction::triggered, [this, entry]() {
      const QFileInfo fi(QString::fromStdString(entry->path));
      QtUtils::OpenURL(this, QUrl::fromLocalFile(fi.absolutePath()));
    });

    connect(menu.addAction(tr("Set Cover Image...")), &QAction::triggered,
            [this, entry]() { setGameListEntryCoverImage(entry); });

    menu.addSeparator();

    if (!s_system_valid)
    {
      populateGameListContextMenu(entry, this, &menu);
      menu.addSeparator();

      connect(menu.addAction(tr("Default Boot")), &QAction::triggered,
              [this, entry]() { g_emu_thread->bootSystem(std::make_shared<SystemBootParameters>(entry->path)); });

      connect(menu.addAction(tr("Fast Boot")), &QAction::triggered, [this, entry]() {
        auto boot_params = std::make_shared<SystemBootParameters>(entry->path);
        boot_params->override_fast_boot = true;
        g_emu_thread->bootSystem(std::move(boot_params));
      });

      connect(menu.addAction(tr("Full Boot")), &QAction::triggered, [this, entry]() {
        auto boot_params = std::make_shared<SystemBootParameters>(entry->path);
        boot_params->override_fast_boot = false;
        g_emu_thread->bootSystem(std::move(boot_params));
      });

      if (m_ui.menuDebug->menuAction()->isVisible() && !Achievements::ChallengeModeActive())
      {
        connect(menu.addAction(tr("Boot and Debug")), &QAction::triggered, [this, entry]() {
          m_open_debugger_on_start = true;

          auto boot_params = std::make_shared<SystemBootParameters>(entry->path);
          boot_params->override_start_paused = true;
          g_emu_thread->bootSystem(std::move(boot_params));
        });
      }
    }
    else
    {
      connect(menu.addAction(tr("Change Disc")), &QAction::triggered, [this, entry]() {
        g_emu_thread->changeDisc(QString::fromStdString(entry->path));
        g_emu_thread->setSystemPaused(false);
        switchToEmulationView();
      });
    }

    menu.addSeparator();
  }

  connect(menu.addAction(tr("Exclude From List")), &QAction::triggered,
          [this, entry]() { getSettingsDialog()->getGameListSettingsWidget()->addExcludedPath(entry->path); });

  connect(menu.addAction(tr("Add Search Directory...")), &QAction::triggered,
          [this]() { getSettingsDialog()->getGameListSettingsWidget()->addSearchDirectory(this); });

  menu.exec(point);
}

void MainWindow::setGameListEntryCoverImage(const GameList::Entry* entry)
{
  QString filename = QFileDialog::getOpenFileName(this, tr("Select Cover Image"), QString(),
                                                  tr("All Cover Image Types (*.jpg *.jpeg *.png)"));
  if (filename.isEmpty())
    return;

  if (!GameList::GetCoverImagePathForEntry(entry).empty())
  {
    if (QMessageBox::question(this, tr("Cover Already Exists"),
                              tr("A cover image for this game already exists, do you wish to replace it?"),
                              QMessageBox::Yes, QMessageBox::No) != QMessageBox::Yes)
    {
      return;
    }
  }

  QString new_filename =
    QString::fromStdString(GameList::GetNewCoverImagePathForEntry(entry, filename.toStdString().c_str()));
  if (new_filename.isEmpty())
    return;

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

  m_game_list_widget->refreshGridCovers();
}

void MainWindow::setupAdditionalUi()
{
  const bool status_bar_visible = Host::GetBaseBoolSettingValue("UI", "ShowStatusBar", true);
  m_ui.actionViewStatusBar->setChecked(status_bar_visible);
  m_ui.statusBar->setVisible(status_bar_visible);

  const bool toolbar_visible = Host::GetBaseBoolSettingValue("UI", "ShowToolbar", true);
  m_ui.actionViewToolbar->setChecked(toolbar_visible);
  m_ui.toolBar->setVisible(toolbar_visible);

  const bool toolbars_locked = Host::GetBaseBoolSettingValue("UI", "LockToolbar", false);
  m_ui.actionViewLockToolbar->setChecked(toolbars_locked);
  m_ui.toolBar->setMovable(!toolbars_locked);
  m_ui.toolBar->setContextMenuPolicy(Qt::PreventContextMenu);

  m_game_list_widget = new GameListWidget(getContentParent());
  m_game_list_widget->initialize();
  m_ui.actionGridViewShowTitles->setChecked(m_game_list_widget->getShowGridCoverTitles());
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

  m_ui.actionGridViewShowTitles->setChecked(m_game_list_widget->getShowGridCoverTitles());

  updateDebugMenuVisibility();

  for (u32 i = 0; i < static_cast<u32>(CPUExecutionMode::Count); i++)
  {
    const CPUExecutionMode mode = static_cast<CPUExecutionMode>(i);
    QAction* action = m_ui.menuCPUExecutionMode->addAction(
      qApp->translate("CPUExecutionMode", Settings::GetCPUExecutionModeDisplayName(mode)));
    action->setCheckable(true);
    connect(action, &QAction::triggered, [this, mode]() {
      Host::SetBaseBoolSettingValue("CPU", "ExecutionMode", Settings::GetCPUExecutionModeName(mode));
      g_emu_thread->applySettings();
      updateDebugMenuCPUExecutionMode();
    });
  }
  updateDebugMenuCPUExecutionMode();

  for (u32 i = 0; i < static_cast<u32>(GPURenderer::Count); i++)
  {
    const GPURenderer renderer = static_cast<GPURenderer>(i);
    QAction* action =
      m_ui.menuRenderer->addAction(qApp->translate("GPURenderer", Settings::GetRendererDisplayName(renderer)));
    action->setCheckable(true);
    connect(action, &QAction::triggered, [this, renderer]() {
      Host::SetBaseStringSettingValue("GPU", "Renderer", Settings::GetRendererName(renderer));
      g_emu_thread->applySettings();
      updateDebugMenuGPURenderer();
    });
  }
  updateDebugMenuGPURenderer();

  for (u32 i = 0; i < static_cast<u32>(DisplayCropMode::Count); i++)
  {
    const DisplayCropMode crop_mode = static_cast<DisplayCropMode>(i);
    QAction* action = m_ui.menuCropMode->addAction(
      qApp->translate("DisplayCropMode", Settings::GetDisplayCropModeDisplayName(crop_mode)));
    action->setCheckable(true);
    connect(action, &QAction::triggered, [this, crop_mode]() {
      Host::SetBaseStringSettingValue("Display", "CropMode", Settings::GetDisplayCropModeName(crop_mode));
      g_emu_thread->applySettings();
      updateDebugMenuCropMode();
    });
  }
  updateDebugMenuCropMode();

  const QString current_language(QString::fromStdString(Host::GetBaseStringSettingValue("Main", "Language", "")));
  QActionGroup* language_group = new QActionGroup(m_ui.menuSettingsLanguage);
  for (const std::pair<QString, QString>& it : QtHost::GetAvailableLanguageList())
  {
    QAction* action = language_group->addAction(it.first);
    action->setCheckable(true);
    action->setChecked(current_language == it.second);

    QString icon_filename(QStringLiteral(":/icons/flags/%1.png").arg(it.second));
    if (!QFile::exists(icon_filename))
    {
      // try without the suffix (e.g. es-es -> es)
      const int pos = it.second.lastIndexOf('-');
      if (pos >= 0)
        icon_filename = QStringLiteral(":/icons/flags/%1.png").arg(it.second.left(pos));
    }
    action->setIcon(QIcon(icon_filename));

    m_ui.menuSettingsLanguage->addAction(action);
    action->setData(it.second);
    connect(action, &QAction::triggered, [this, action]() {
      const QString new_language = action->data().toString();
      Host::SetBaseStringSettingValue("Main", "Language", new_language.toUtf8().constData());
      QtHost::ReinstallTranslator();
      recreate();
    });
  }

  for (u32 scale = 1; scale <= 10; scale++)
  {
    QAction* action = m_ui.menuWindowSize->addAction(tr("%1x Scale").arg(scale));
    connect(action, &QAction::triggered, [scale]() { g_emu_thread->requestDisplaySize(scale); });
  }

#ifdef WITH_RAINTEGRATION
  if (Achievements::IsUsingRAIntegration())
  {
    QMenu* raMenu = new QMenu(QStringLiteral("RAIntegration"), m_ui.menuDebug);
    connect(raMenu, &QMenu::aboutToShow, this, [this, raMenu]() {
      raMenu->clear();

      const auto items = Achievements::RAIntegration::GetMenuItems();
      for (const auto& [id, title] : items)
      {
        if (id == 0)
        {
          raMenu->addSeparator();
          continue;
        }

        QAction* raAction = raMenu->addAction(QString::fromUtf8(title));
        connect(raAction, &QAction::triggered, this,
                [id]() { Host::RunOnCPUThread([id]() { Achievements::RAIntegration::ActivateMenuItem(id); }); });
      }
    });
    m_ui.menuDebug->insertMenu(m_ui.menuCPUExecutionMode->menuAction(), raMenu);
  }
#endif
}

void MainWindow::updateEmulationActions(bool starting, bool running, bool cheevos_challenge_mode)
{
  m_ui.actionStartFile->setDisabled(starting || running);
  m_ui.actionStartDisc->setDisabled(starting || running);
  m_ui.actionStartBios->setDisabled(starting || running);
  m_ui.actionResumeLastState->setDisabled(starting || running || cheevos_challenge_mode);

  m_ui.actionPowerOff->setDisabled(starting || !running);
  m_ui.actionPowerOffWithoutSaving->setDisabled(starting || !running);
  m_ui.actionReset->setDisabled(starting || !running);
  m_ui.actionPause->setDisabled(starting || !running);
  m_ui.actionChangeDisc->setDisabled(starting || !running);
  m_ui.actionCheats->setDisabled(starting || !running || cheevos_challenge_mode);
  m_ui.actionScreenshot->setDisabled(starting || !running);
  m_ui.actionViewSystemDisplay->setEnabled(starting || running);
  m_ui.menuChangeDisc->setDisabled(starting || !running);
  m_ui.menuCheats->setDisabled(starting || !running || cheevos_challenge_mode);
  m_ui.actionCheatManager->setDisabled(starting || !running || cheevos_challenge_mode);
  m_ui.actionCPUDebugger->setDisabled(starting || !running || cheevos_challenge_mode);
  m_ui.actionDumpRAM->setDisabled(starting || !running || cheevos_challenge_mode);
  m_ui.actionDumpVRAM->setDisabled(starting || !running || cheevos_challenge_mode);
  m_ui.actionDumpSPURAM->setDisabled(starting || !running || cheevos_challenge_mode);

  m_ui.actionSaveState->setDisabled(starting || !running);
  m_ui.menuSaveState->setDisabled(starting || !running);
  m_ui.menuWindowSize->setDisabled(starting || !running);

  m_ui.actionViewGameProperties->setDisabled(starting || !running);

  if (starting || running)
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

  if (m_open_debugger_on_start && running)
    openCPUDebugger();
  if ((!starting && !running) || running)
    m_open_debugger_on_start = false;

  if (g_settings.debugging.enable_gdb_server)
  {
    if (starting && !m_gdb_server)
    {
      m_gdb_server = new GDBServer(this, g_settings.debugging.gdb_server_port);
    }
    else if (!running && m_gdb_server)
    {
      delete m_gdb_server;
      m_gdb_server = nullptr;
    }
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
  QString display_title(QString::fromStdString(m_current_game_title) + suffix);

  if (!s_system_valid || m_current_game_title.empty())
    display_title = main_title;
  else if (isRenderingToMain())
    main_title = display_title;

  if (windowTitle() != main_title)
    setWindowTitle(main_title);

  if (m_display_widget && !isRenderingToMain())
  {
    QWidget* container =
      m_display_container ? static_cast<QWidget*>(m_display_container) : static_cast<QWidget*>(m_display_widget);
    if (container->windowTitle() != display_title)
      container->setWindowTitle(display_title);
  }
}

void MainWindow::updateWindowState(bool force_visible)
{
  // Skip all of this when we're closing, since we don't want to make ourselves visible and cancel it.
  if (m_is_closing)
    return;

  const bool hide_window = !isRenderingToMain() && shouldHideMainWindow();
  const bool disable_resize = Host::GetBaseBoolSettingValue("Main", "DisableWindowResize", false);
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
  if (!g_host_display || !m_display_widget)
    return false;

  return getDisplayContainer()->isFullScreen() || g_host_display->IsFullscreen();
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
  return isRenderingFullscreen() && Host::GetBoolSettingValue("Main", "HideMouseCursor", false);
}

bool MainWindow::shouldHideMainWindow() const
{
  return Host::GetBaseBoolSettingValue("Main", "HideMainWindowWhenRunning", false) || isRenderingFullscreen() ||
         QtHost::InNoGUIMode();
}

void MainWindow::switchToGameListView()
{
  if (isShowingGameList())
  {
    m_game_list_widget->setFocus();
    return;
  }

  if (s_system_valid)
  {
    m_was_paused_on_surface_loss = s_system_paused;
    if (!s_system_paused)
      g_emu_thread->setSystemPaused(true);

    // switch to surfaceless. we have to wait until the display widget is gone before we swap over.
    g_emu_thread->setSurfaceless(true);
    while (m_display_widget)
      QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 1);
  }
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
  updateEmulationActions(false, false, Achievements::ChallengeModeActive());

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
  connect(m_ui.actionCheats, &QAction::triggered, [this] { m_ui.menuCheats->exec(QCursor::pos()); });
  connect(m_ui.actionRemoveDisc, &QAction::triggered, this, &MainWindow::onRemoveDiscActionTriggered);
  connect(m_ui.actionAddGameDirectory, &QAction::triggered,
          [this]() { getSettingsDialog()->getGameListSettingsWidget()->addSearchDirectory(this); });
  connect(m_ui.actionPowerOff, &QAction::triggered, this, [this]() { requestShutdown(true, true); });
  connect(m_ui.actionPowerOffWithoutSaving, &QAction::triggered, this, [this]() { requestShutdown(false, false); });
  connect(m_ui.actionReset, &QAction::triggered, g_emu_thread, &EmuThread::resetSystem);
  connect(m_ui.actionPause, &QAction::toggled, [this](bool active) { g_emu_thread->setSystemPaused(active); });
  connect(m_ui.actionScreenshot, &QAction::triggered, g_emu_thread, &EmuThread::saveScreenshot);
  connect(m_ui.actionScanForNewGames, &QAction::triggered, this, [this]() { refreshGameList(false); });
  connect(m_ui.actionRescanAllGames, &QAction::triggered, this, [this]() { refreshGameList(true); });
  connect(m_ui.actionLoadState, &QAction::triggered, this, [this]() { m_ui.menuLoadState->exec(QCursor::pos()); });
  connect(m_ui.actionSaveState, &QAction::triggered, this, [this]() { m_ui.menuSaveState->exec(QCursor::pos()); });
  connect(m_ui.actionExit, &QAction::triggered, this, &MainWindow::close);
  connect(m_ui.actionFullscreen, &QAction::triggered, g_emu_thread, &EmuThread::toggleFullscreen);
  connect(m_ui.actionSettings, &QAction::triggered, [this]() { doSettings(); });
  connect(m_ui.actionGeneralSettings, &QAction::triggered, [this]() { doSettings("General"); });
  connect(m_ui.actionBIOSSettings, &QAction::triggered, [this]() { doSettings("BIOS"); });
  connect(m_ui.actionConsoleSettings, &QAction::triggered, [this]() { doSettings("Console"); });
  connect(m_ui.actionEmulationSettings, &QAction::triggered, [this]() { doSettings("Emulation"); });
  connect(m_ui.actionGameListSettings, &QAction::triggered, [this]() { doSettings("Game List"); });
  connect(m_ui.actionHotkeySettings, &QAction::triggered,
          [this]() { doControllerSettings(ControllerSettingsDialog::Category::HotkeySettings); });
  connect(m_ui.actionControllerSettings, &QAction::triggered,
          [this]() { doControllerSettings(ControllerSettingsDialog::Category::GlobalSettings); });
  connect(m_ui.actionMemoryCardSettings, &QAction::triggered, [this]() { doSettings("Memory Cards"); });
  connect(m_ui.actionDisplaySettings, &QAction::triggered, [this]() { doSettings("Display"); });
  connect(m_ui.actionEnhancementSettings, &QAction::triggered, [this]() { doSettings("Enhancements"); });
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
  connect(m_ui.actionViewGameProperties, &QAction::triggered, this, &MainWindow::onViewGamePropertiesActionTriggered);
  connect(m_ui.actionGitHubRepository, &QAction::triggered, this, &MainWindow::onGitHubRepositoryActionTriggered);
  connect(m_ui.actionIssueTracker, &QAction::triggered, this, &MainWindow::onIssueTrackerActionTriggered);
  connect(m_ui.actionDiscordServer, &QAction::triggered, this, &MainWindow::onDiscordServerActionTriggered);
  connect(m_ui.actionAboutQt, &QAction::triggered, qApp, &QApplication::aboutQt);
  connect(m_ui.actionAbout, &QAction::triggered, this, &MainWindow::onAboutActionTriggered);
  connect(m_ui.actionCheckForUpdates, &QAction::triggered, this, &MainWindow::onCheckForUpdatesActionTriggered);
  connect(m_ui.actionMemory_Card_Editor, &QAction::triggered, this, &MainWindow::onToolsMemoryCardEditorTriggered);
  connect(m_ui.actionCheatManager, &QAction::triggered, this, &MainWindow::onToolsCheatManagerTriggered);
  connect(m_ui.actionCPUDebugger, &QAction::triggered, this, &MainWindow::openCPUDebugger);
  connect(m_ui.actionOpenDataDirectory, &QAction::triggered, this, &MainWindow::onToolsOpenDataDirectoryTriggered);
  connect(m_ui.actionGridViewShowTitles, &QAction::triggered, m_game_list_widget, &GameListWidget::setShowCoverTitles);
  connect(m_ui.actionGridViewZoomIn, &QAction::triggered, m_game_list_widget, [this]() {
    if (isShowingGameList())
      m_game_list_widget->gridZoomIn();
  });
  connect(m_ui.actionGridViewZoomOut, &QAction::triggered, m_game_list_widget, [this]() {
    if (isShowingGameList())
      m_game_list_widget->gridZoomOut();
  });
  connect(m_ui.actionGridViewRefreshCovers, &QAction::triggered, m_game_list_widget,
          &GameListWidget::refreshGridCovers);

  connect(m_ui.actionStartFullscreenUI, &QAction::triggered, g_emu_thread, &EmuThread::startFullscreenUI);
  connect(m_ui.actionStartFullscreenUI2, &QAction::triggered, g_emu_thread, &EmuThread::startFullscreenUI);
  connect(g_emu_thread, &EmuThread::settingsResetToDefault, this, &MainWindow::onSettingsResetToDefault,
          Qt::QueuedConnection);
  connect(g_emu_thread, &EmuThread::errorReported, this, &MainWindow::reportError, Qt::BlockingQueuedConnection);
  connect(g_emu_thread, &EmuThread::messageConfirmed, this, &MainWindow::confirmMessage, Qt::BlockingQueuedConnection);
  connect(g_emu_thread, &EmuThread::createDisplayRequested, this, &MainWindow::createDisplay,
          Qt::BlockingQueuedConnection);
  connect(g_emu_thread, &EmuThread::destroyDisplayRequested, this, &MainWindow::destroyDisplay);
  connect(g_emu_thread, &EmuThread::updateDisplayRequested, this, &MainWindow::updateDisplay,
          Qt::BlockingQueuedConnection);
  connect(g_emu_thread, &EmuThread::displaySizeRequested, this, &MainWindow::displaySizeRequested);
  connect(g_emu_thread, &EmuThread::focusDisplayWidgetRequested, this, &MainWindow::focusDisplayWidget);
  connect(g_emu_thread, &EmuThread::systemStarting, this, &MainWindow::onSystemStarting);
  connect(g_emu_thread, &EmuThread::systemStarted, this, &MainWindow::onSystemStarted);
  connect(g_emu_thread, &EmuThread::systemDestroyed, this, &MainWindow::onSystemDestroyed);
  connect(g_emu_thread, &EmuThread::systemPaused, this, &MainWindow::onSystemPaused);
  connect(g_emu_thread, &EmuThread::systemResumed, this, &MainWindow::onSystemResumed);
  connect(g_emu_thread, &EmuThread::runningGameChanged, this, &MainWindow::onRunningGameChanged);
  connect(g_emu_thread, &EmuThread::mouseModeRequested, this, &MainWindow::onMouseModeRequested);
#ifdef WITH_CHEEVOS
  connect(g_emu_thread, &EmuThread::achievementsChallengeModeChanged, this,
          &MainWindow::onAchievementsChallengeModeChanged);
#endif

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
          [this]() { getSettingsDialog()->getGameListSettingsWidget()->addSearchDirectory(this); });

  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionDisableAllEnhancements, "Main",
                                               "DisableAllEnhancements", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionDisableInterlacing, "GPU", "DisableInterlacing",
                                               true);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionForceNTSCTimings, "GPU", "ForceNTSCTimings", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionDebugDumpCPUtoVRAMCopies, "Debug",
                                               "DumpCPUToVRAMCopies", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionDebugDumpVRAMtoCPUCopies, "Debug",
                                               "DumpVRAMToCPUCopies", false);
  connect(m_ui.actionDumpAudio, &QAction::toggled, [this](bool checked) {
    if (checked)
      g_emu_thread->startDumpingAudio();
    else
      g_emu_thread->stopDumpingAudio();
  });
  connect(m_ui.actionDumpRAM, &QAction::triggered, [this]() {
    const QString filename =
      QFileDialog::getSaveFileName(this, tr("Destination File"), QString(), tr("Binary Files (*.bin)"));
    if (filename.isEmpty())
      return;

    g_emu_thread->dumpRAM(filename);
  });
  connect(m_ui.actionDumpVRAM, &QAction::triggered, [this]() {
    const QString filename = QFileDialog::getSaveFileName(this, tr("Destination File"), QString(),
                                                          tr("Binary Files (*.bin);;PNG Images (*.png)"));
    if (filename.isEmpty())
      return;

    g_emu_thread->dumpVRAM(filename);
  });
  connect(m_ui.actionDumpSPURAM, &QAction::triggered, [this]() {
    const QString filename =
      QFileDialog::getSaveFileName(this, tr("Destination File"), QString(), tr("Binary Files (*.bin)"));
    if (filename.isEmpty())
      return;

    g_emu_thread->dumpSPURAM(filename);
  });
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionDebugShowVRAM, "Debug", "ShowVRAM", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionDebugShowGPUState, "Debug", "ShowGPUState", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionDebugShowCDROMState, "Debug", "ShowCDROMState",
                                               false);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionDebugShowSPUState, "Debug", "ShowSPUState", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionDebugShowTimersState, "Debug", "ShowTimersState",
                                               false);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionDebugShowMDECState, "Debug", "ShowMDECState", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.actionDebugShowDMAState, "Debug", "ShowDMAState", false);

  addThemeToMenu(tr("Default"), QStringLiteral("default"));
  addThemeToMenu(tr("Fusion"), QStringLiteral("fusion"));
  addThemeToMenu(tr("Dark Fusion (Gray)"), QStringLiteral("darkfusion"));
  addThemeToMenu(tr("Dark Fusion (Blue)"), QStringLiteral("darkfusionblue"));
  addThemeToMenu(tr("QDarkStyle"), QStringLiteral("qdarkstyle"));
  updateMenuSelectedTheme();
}

void MainWindow::addThemeToMenu(const QString& name, const QString& key)
{
  QAction* action = m_ui.menuSettingsTheme->addAction(name);
  action->setCheckable(true);
  action->setData(key);
  connect(action, &QAction::toggled, [this, key](bool) { setTheme(key); });
}

void MainWindow::setTheme(const QString& theme)
{
  Host::SetBaseStringSettingValue("UI", "Theme", theme.toUtf8().constData());
  updateApplicationTheme();
  updateMenuSelectedTheme();
  m_game_list_widget->reloadCommonImages();
}

void MainWindow::setStyleFromSettings()
{
  const std::string theme(Host::GetBaseStringSettingValue("UI", "Theme", DEFAULT_THEME_NAME));

  if (theme == "qdarkstyle")
  {
    qApp->setStyle(s_unthemed_style_name);
    qApp->setPalette(QApplication::style()->standardPalette());

    QFile f(QStringLiteral(":qdarkstyle/style.qss"));
    if (f.open(QFile::ReadOnly | QFile::Text))
      qApp->setStyleSheet(f.readAll());
  }
  else if (theme == "fusion")
  {
    qApp->setPalette(QApplication::style()->standardPalette());
    qApp->setStyleSheet(QString());
    qApp->setStyle(QStyleFactory::create("Fusion"));
  }
  else if (theme == "darkfusion")
  {
    // adapted from https://gist.github.com/QuantumCD/6245215
    qApp->setStyle(QStyleFactory::create("Fusion"));

    const QColor lighterGray(75, 75, 75);
    const QColor darkGray(53, 53, 53);
    const QColor gray(128, 128, 128);
    const QColor black(25, 25, 25);
    const QColor blue(198, 238, 255);

    QPalette darkPalette;
    darkPalette.setColor(QPalette::Window, darkGray);
    darkPalette.setColor(QPalette::WindowText, Qt::white);
    darkPalette.setColor(QPalette::Base, black);
    darkPalette.setColor(QPalette::AlternateBase, darkGray);
    darkPalette.setColor(QPalette::ToolTipBase, darkGray);
    darkPalette.setColor(QPalette::ToolTipText, Qt::white);
    darkPalette.setColor(QPalette::Text, Qt::white);
    darkPalette.setColor(QPalette::Button, darkGray);
    darkPalette.setColor(QPalette::ButtonText, Qt::white);
    darkPalette.setColor(QPalette::Link, blue);
    darkPalette.setColor(QPalette::Highlight, lighterGray);
    darkPalette.setColor(QPalette::HighlightedText, Qt::white);
    darkPalette.setColor(QPalette::PlaceholderText, QColor(Qt::white).darker());

    darkPalette.setColor(QPalette::Active, QPalette::Button, gray.darker());
    darkPalette.setColor(QPalette::Disabled, QPalette::ButtonText, gray);
    darkPalette.setColor(QPalette::Disabled, QPalette::WindowText, gray);
    darkPalette.setColor(QPalette::Disabled, QPalette::Text, gray);
    darkPalette.setColor(QPalette::Disabled, QPalette::Light, darkGray);

    qApp->setPalette(darkPalette);

    qApp->setStyleSheet("QToolTip { color: #ffffff; background-color: #2a82da; border: 1px solid white; }");
  }
  else if (theme == "darkfusionblue")
  {
    // adapted from https://gist.github.com/QuantumCD/6245215
    qApp->setStyle(QStyleFactory::create("Fusion"));

    const QColor lighterGray(75, 75, 75);
    const QColor darkGray(53, 53, 53);
    const QColor gray(128, 128, 128);
    const QColor black(25, 25, 25);
    const QColor blue(198, 238, 255);
    const QColor blue2(0, 88, 208);

    QPalette darkPalette;
    darkPalette.setColor(QPalette::Window, darkGray);
    darkPalette.setColor(QPalette::WindowText, Qt::white);
    darkPalette.setColor(QPalette::Base, black);
    darkPalette.setColor(QPalette::AlternateBase, darkGray);
    darkPalette.setColor(QPalette::ToolTipBase, blue2);
    darkPalette.setColor(QPalette::ToolTipText, Qt::white);
    darkPalette.setColor(QPalette::Text, Qt::white);
    darkPalette.setColor(QPalette::Button, darkGray);
    darkPalette.setColor(QPalette::ButtonText, Qt::white);
    darkPalette.setColor(QPalette::Link, blue);
    darkPalette.setColor(QPalette::Highlight, blue2);
    darkPalette.setColor(QPalette::HighlightedText, Qt::white);
    darkPalette.setColor(QPalette::PlaceholderText, QColor(Qt::white).darker());

    darkPalette.setColor(QPalette::Active, QPalette::Button, gray.darker());
    darkPalette.setColor(QPalette::Disabled, QPalette::ButtonText, gray);
    darkPalette.setColor(QPalette::Disabled, QPalette::WindowText, gray);
    darkPalette.setColor(QPalette::Disabled, QPalette::Text, gray);
    darkPalette.setColor(QPalette::Disabled, QPalette::Light, darkGray);

    qApp->setPalette(darkPalette);

    qApp->setStyleSheet("QToolTip { color: #ffffff; background-color: #2a82da; border: 1px solid white; }");
  }
  else
  {
    qApp->setPalette(QApplication::style()->standardPalette());
    qApp->setStyleSheet(QString());
    qApp->setStyle(s_unthemed_style_name);
  }
}

void MainWindow::setIconThemeFromSettings()
{
  const std::string theme(Host::GetBaseStringSettingValue("UI", "Theme", DEFAULT_THEME_NAME));
  QString icon_theme;

  if (theme == "qdarkstyle" || theme == "darkfusion" || theme == "darkfusionblue")
    icon_theme = QStringLiteral("white");
  else
    icon_theme = QStringLiteral("black");

  QIcon::setThemeName(icon_theme);
}

void MainWindow::onSettingsResetToDefault()
{
  if (m_settings_dialog)
  {
    const bool shown = m_settings_dialog->isVisible();

    m_settings_dialog->hide();
    m_settings_dialog->deleteLater();
    m_settings_dialog = new SettingsDialog(this);
    if (shown)
    {
      m_settings_dialog->setModal(false);
      m_settings_dialog->show();
    }
  }

  updateDebugMenuCPUExecutionMode();
  updateDebugMenuGPURenderer();
  updateDebugMenuCropMode();
  updateDebugMenuVisibility();
  updateMenuSelectedTheme();
}

void MainWindow::saveGeometryToConfig()
{
  const QByteArray geometry = saveGeometry();
  const QByteArray geometry_b64 = geometry.toBase64();
  const std::string old_geometry_b64 = Host::GetBaseStringSettingValue("UI", "MainWindowGeometry");
  if (old_geometry_b64 != geometry_b64.constData())
    Host::SetBaseStringSettingValue("UI", "MainWindowGeometry", geometry_b64.constData());
}

void MainWindow::restoreGeometryFromConfig()
{
  const std::string geometry_b64 = Host::GetBaseStringSettingValue("UI", "MainWindowGeometry");
  const QByteArray geometry = QByteArray::fromBase64(QByteArray::fromStdString(geometry_b64));
  if (!geometry.isEmpty())
    restoreGeometry(geometry);
}

void MainWindow::saveDisplayWindowGeometryToConfig()
{
  const QByteArray geometry = getDisplayContainer()->saveGeometry();
  const QByteArray geometry_b64 = geometry.toBase64();
  const std::string old_geometry_b64 = Host::GetBaseStringSettingValue("UI", "DisplayWindowGeometry");
  if (old_geometry_b64 != geometry_b64.constData())
    Host::SetBaseStringSettingValue("UI", "DisplayWindowGeometry", geometry_b64.constData());
}

void MainWindow::restoreDisplayWindowGeometryFromConfig()
{
  const std::string geometry_b64 = Host::GetBaseStringSettingValue("UI", "DisplayWindowGeometry");
  const QByteArray geometry = QByteArray::fromBase64(QByteArray::fromStdString(geometry_b64));
  QWidget* container = getDisplayContainer();
  if (!geometry.isEmpty())
    container->restoreGeometry(geometry);
  else
    container->resize(640, 480);
}

SettingsDialog* MainWindow::getSettingsDialog()
{
  if (!m_settings_dialog)
    m_settings_dialog = new SettingsDialog(this);

  return m_settings_dialog;
}

void MainWindow::doSettings(const char* category /* = nullptr */)
{
  SettingsDialog* dlg = getSettingsDialog();
  if (!dlg->isVisible())
  {
    dlg->setModal(false);
    dlg->show();
  }

  if (category)
    dlg->setCategory(category);
}

ControllerSettingsDialog* MainWindow::getControllerSettingsDialog()
{
  if (!m_controller_settings_dialog)
    m_controller_settings_dialog = new ControllerSettingsDialog(this);

  return m_controller_settings_dialog;
}

void MainWindow::doControllerSettings(
  ControllerSettingsDialog::Category category /*= ControllerSettingsDialog::Category::Count*/)
{
  ControllerSettingsDialog* dlg = getControllerSettingsDialog();
  if (!dlg->isVisible())
  {
    dlg->setModal(false);
    dlg->show();
  }

  if (category != ControllerSettingsDialog::Category::Count)
    dlg->setCategory(category);
}

void MainWindow::updateDebugMenuCPUExecutionMode()
{
  std::optional<CPUExecutionMode> current_mode =
    Settings::ParseCPUExecutionMode(Host::GetBaseStringSettingValue("CPU", "ExecutionMode").c_str());
  if (!current_mode.has_value())
    return;

  const QString current_mode_display_name(
    qApp->translate("CPUExecutionMode", Settings::GetCPUExecutionModeDisplayName(current_mode.value())));
  for (QObject* obj : m_ui.menuCPUExecutionMode->children())
  {
    QAction* action = qobject_cast<QAction*>(obj);
    if (action)
      action->setChecked(action->text() == current_mode_display_name);
  }
}

void MainWindow::updateDebugMenuGPURenderer()
{
  // update the menu with the new selected renderer
  std::optional<GPURenderer> current_renderer =
    Settings::ParseRendererName(Host::GetBaseStringSettingValue("GPU", "Renderer").c_str());
  if (!current_renderer.has_value())
    return;

  const QString current_renderer_display_name(
    qApp->translate("GPURenderer", Settings::GetRendererDisplayName(current_renderer.value())));
  for (QObject* obj : m_ui.menuRenderer->children())
  {
    QAction* action = qobject_cast<QAction*>(obj);
    if (action)
      action->setChecked(action->text() == current_renderer_display_name);
  }
}

void MainWindow::updateDebugMenuCropMode()
{
  std::optional<DisplayCropMode> current_crop_mode =
    Settings::ParseDisplayCropMode(Host::GetBaseStringSettingValue("Display", "CropMode").c_str());
  if (!current_crop_mode.has_value())
    return;

  const QString current_crop_mode_display_name(
    qApp->translate("DisplayCropMode", Settings::GetDisplayCropModeDisplayName(current_crop_mode.value())));
  for (QObject* obj : m_ui.menuCropMode->children())
  {
    QAction* action = qobject_cast<QAction*>(obj);
    if (action)
      action->setChecked(action->text() == current_crop_mode_display_name);
  }
}

void MainWindow::updateMenuSelectedTheme()
{
  QString theme = QString::fromStdString(Host::GetBaseStringSettingValue("UI", "Theme", DEFAULT_THEME_NAME));

  for (QObject* obj : m_ui.menuSettingsTheme->children())
  {
    QAction* action = qobject_cast<QAction*>(obj);
    if (action)
    {
      QVariant action_data(action->data());
      if (action_data.isValid())
      {
        QSignalBlocker blocker(action);
        action->setChecked(action_data == theme);
      }
    }
  }
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
  if (!requestShutdown(true, true, true))
  {
    event->ignore();
    return;
  }

  if (g_emu_thread->isRunningFullscreenUI())
    g_emu_thread->stopFullscreenUI();

  saveGeometryToConfig();
  m_is_closing = true;

  QMainWindow::closeEvent(event);
}

void MainWindow::changeEvent(QEvent* event)
{
  if (static_cast<QWindowStateChangeEvent*>(event)->oldState() & Qt::WindowMinimized)
  {
    // TODO: This should check the render-to-main option.
    if (m_display_widget)
      g_emu_thread->redrawDisplayWindow();
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
      filename = urls.front().toLocalFile();
  }

  return filename;
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
  const std::string filename(getFilenameFromMimeData(event->mimeData()).toStdString());
  if (!System::IsLoadableFilename(filename) && !System::IsSaveStateFilename(filename))
    return;

  event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event)
{
  const QString qfilename(getFilenameFromMimeData(event->mimeData()));
  const std::string filename(qfilename.toStdString());
  if (!System::IsLoadableFilename(filename) && !System::IsSaveStateFilename(filename))
    return;

  event->acceptProposedAction();

  if (System::IsSaveStateFilename(filename))
  {
    g_emu_thread->loadState(qfilename);
    return;
  }

  if (s_system_valid)
    promptForDiscChange(qfilename);
  else
    startFileOrChangeDisc(qfilename);
}

void MainWindow::startupUpdateCheck()
{
  if (!Host::GetBaseBoolSettingValue("AutoUpdater", "CheckAtStartup", true))
    return;

  checkForUpdates(false);
}

void MainWindow::updateDebugMenuVisibility()
{
  const bool visible = Host::GetBaseBoolSettingValue("Main", "ShowDebugMenu", false);
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

void MainWindow::runOnUIThread(const std::function<void()>& func)
{
  func();
}

bool MainWindow::requestShutdown(bool allow_confirm /* = true */, bool allow_save_to_state /* = true */,
                                 bool block_until_done /* = false */)
{
  if (!s_system_valid)
    return true;

  // If we don't have a serial, we can't save state.
  allow_save_to_state &= !m_current_game_code.empty();
  bool save_state = allow_save_to_state && g_settings.save_state_on_exit;

  // Only confirm on UI thread because we need to display a msgbox.
  if (!m_is_closing && allow_confirm && g_settings.confim_power_off)
  {
    SystemLock lock(pauseAndLockSystem());

    QMessageBox msgbox(lock.getDialogParent());
    msgbox.setIcon(QMessageBox::Question);
    msgbox.setWindowTitle(tr("Confirm Shutdown"));
    msgbox.setText("Are you sure you want to shut down the virtual machine?");

    QCheckBox* save_cb = new QCheckBox(tr("Save State For Resume"), &msgbox);
    save_cb->setChecked(save_state);
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
  if (!isRenderingToMain() && isHidden() && !QtHost::InBatchMode())
    updateWindowState(true);

  // Now we can actually shut down the VM.
  g_emu_thread->shutdownSystem(save_state);

  if (block_until_done || m_is_closing || QtHost::InBatchMode())
  {
    // We need to yield here, since the display gets destroyed.
    while (s_system_valid || System::GetState() != System::State::Shutdown)
      QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 1);
  }

  if (!m_is_closing && QtHost::InBatchMode())
  {
    // Closing the window should shut down everything. If we don't set the closing flag here,
    // the VM shutdown may not complete by the time closeEvent() is called, leading to a confirm.
    m_is_closing = true;
    close();
  }

  return true;
}

void MainWindow::requestExit(bool allow_save_to_state /* = true */)
{
  // this is block, because otherwise closeEvent() will also prompt
  if (!requestShutdown(true, allow_save_to_state, true))
    return;

  close();
}

void MainWindow::checkForSettingChanges()
{
#if 0
  // FIXME: Triggers incorrectly
  if (m_display_widget)
    m_display_widget->updateRelativeMode(s_system_valid && !s_system_paused);
#endif

  updateWindowState();
}

void MainWindow::onCheckForUpdatesActionTriggered()
{
  // Wipe out the last version, that way it displays the update if we've previously skipped it.
  Host::DeleteBaseSettingValue("AutoUpdater", "LastVersion");
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
        if (!MemoryCardEditorDialog::createMemoryCard(card_path))
          QMessageBox::critical(this, tr("Memory Card Not Found"),
                                tr("Failed to create memory card '%1'").arg(card_path));
      }
    }
  }

  if (!m_memory_card_editor_dialog)
  {
    m_memory_card_editor_dialog = new MemoryCardEditorDialog(this);
    m_memory_card_editor_dialog->setModal(false);
  }

  m_memory_card_editor_dialog->show();
  m_memory_card_editor_dialog->activateWindow();

  if (!card_a_path.isEmpty())
  {
    if (!m_memory_card_editor_dialog->setCardA(card_a_path))
    {
      QMessageBox::critical(
        this, tr("Memory Card Not Found"),
        tr("Memory card '%1' could not be found. Try starting the game and saving to create it.").arg(card_a_path));
    }
  }
  if (!card_b_path.isEmpty())
  {
    if (!m_memory_card_editor_dialog->setCardB(card_b_path))
    {
      QMessageBox::critical(
        this, tr("Memory Card Not Found"),
        tr("Memory card '%1' could not be found. Try starting the game and saving to create it.").arg(card_b_path));
    }
  }
}

void MainWindow::onAchievementsChallengeModeChanged()
{
#ifdef WITH_CHEEVOS
  const bool active = Achievements::ChallengeModeActive();
  if (active)
  {
    if (m_cheat_manager_dialog)
    {
      m_cheat_manager_dialog->close();
      delete m_cheat_manager_dialog;
      m_cheat_manager_dialog = nullptr;
    }

    if (m_debugger_window)
    {
      m_debugger_window->close();
      delete m_debugger_window;
      m_debugger_window = nullptr;
    }
  }

  updateEmulationActions(false, System::IsValid(), active);
#endif
}

void MainWindow::onToolsMemoryCardEditorTriggered()
{
  openMemoryCardEditor(QString(), QString());
}

void MainWindow::onToolsCheatManagerTriggered()
{
  if (!m_cheat_manager_dialog)
  {
    if (Host::GetBaseBoolSettingValue("UI", "DisplayCheatWarning", true))
    {
      QCheckBox* cb = new QCheckBox(tr("Do not show again"));
      QMessageBox mb(this);
      mb.setWindowTitle(tr("Cheat Manager"));
      mb.setText(
        tr("Using cheats can have unpredictable effects on games, causing crashes, graphical glitches, and corrupted "
           "saves. By using the cheat manager, you agree that it is an unsupported configuration, and we will not "
           "provide you with any assistance when games break.\n\nCheats persist through save states even after being "
           "disabled, please remember to reset/reboot the game after turning off any codes.\n\nAre you sure you want "
           "to continue?"));
      mb.setIcon(QMessageBox::Warning);
      mb.addButton(QMessageBox::Yes);
      mb.addButton(QMessageBox::No);
      mb.setDefaultButton(QMessageBox::No);
      mb.setCheckBox(cb);

      connect(cb, &QCheckBox::stateChanged, [](int state) {
        Host::SetBaseBoolSettingValue("UI", "DisplayCheatWarning", (state != Qt::CheckState::Checked));
      });

      if (mb.exec() == QMessageBox::No)
        return;
    }

    m_cheat_manager_dialog = new CheatManagerDialog(this);
  }

  m_cheat_manager_dialog->setModal(false);
  m_cheat_manager_dialog->show();
}

void MainWindow::openCPUDebugger()
{
  g_emu_thread->setSystemPaused(true, true);
  if (!System::IsValid())
    return;

  Assert(!m_debugger_window);

  m_debugger_window = new DebuggerWindow();
  m_debugger_window->setWindowIcon(windowIcon());
  connect(m_debugger_window, &DebuggerWindow::closed, this, &MainWindow::onCPUDebuggerClosed);
  m_debugger_window->show();

  // the debugger will miss the pause event above (or we were already paused), so fire it now
  m_debugger_window->onEmulationPaused();
}

void MainWindow::onCPUDebuggerClosed()
{
  Assert(m_debugger_window);
  m_debugger_window->deleteLater();
  m_debugger_window = nullptr;
}

void MainWindow::onToolsOpenDataDirectoryTriggered()
{
  QtUtils::OpenURL(this, QUrl::fromLocalFile(QString::fromStdString(EmuFolders::DataRoot)));
}

void MainWindow::checkForUpdates(bool display_message)
{
  if (!AutoUpdaterDialog::isSupported())
  {
    if (display_message)
    {
      QMessageBox mbox(this);
      mbox.setWindowTitle(tr("Updater Error"));
      mbox.setTextFormat(Qt::RichText);

      QString message;
#ifdef _WIN32
      message =
        tr("<p>Sorry, you are trying to update a DuckStation version which is not an official GitHub release. To "
           "prevent incompatibilities, the auto-updater is only enabled on official builds.</p>"
           "<p>To obtain an official build, please follow the instructions under \"Downloading and Running\" at the "
           "link below:</p>"
           "<p><a href=\"https://github.com/stenzek/duckstation/\">https://github.com/stenzek/duckstation/</a></p>");
#else
      message = tr("Automatic updating is not supported on the current platform.");
#endif

      mbox.setText(message);
      mbox.setIcon(QMessageBox::Critical);
      mbox.exec();
    }

    return;
  }

  if (m_auto_updater_dialog)
    return;

  m_auto_updater_dialog = new AutoUpdaterDialog(g_emu_thread, this);
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

MainWindow::SystemLock MainWindow::pauseAndLockSystem()
{
  const bool was_fullscreen = isRenderingFullscreen();
  const bool was_paused = s_system_paused;

  // We use surfaceless rather than switching out of fullscreen, because
  // we're paused, so we're not going to be rendering anyway.
  if (was_fullscreen)
    g_emu_thread->setSurfaceless(true);
  if (!was_paused)
    g_emu_thread->setSystemPaused(true);

  // We want to parent dialogs to the display widget, except if we were fullscreen,
  // since it's going to get destroyed by the surfaceless call above.
  QWidget* dialog_parent = was_fullscreen ? static_cast<QWidget*>(this) : getDisplayContainer();

  return SystemLock(dialog_parent, was_paused, was_fullscreen);
}

MainWindow::SystemLock::SystemLock(QWidget* dialog_parent, bool was_paused, bool was_fullscreen)
  : m_dialog_parent(dialog_parent), m_was_paused(was_paused), m_was_fullscreen(was_fullscreen)
{
}

MainWindow::SystemLock::SystemLock(SystemLock&& lock)
  : m_dialog_parent(lock.m_dialog_parent), m_was_paused(lock.m_was_paused), m_was_fullscreen(lock.m_was_fullscreen)
{
  lock.m_dialog_parent = nullptr;
  lock.m_was_paused = true;
  lock.m_was_fullscreen = false;
}

MainWindow::SystemLock::~SystemLock()
{
  if (m_was_fullscreen)
    g_emu_thread->setSurfaceless(false);
  if (!m_was_paused)
    g_emu_thread->setSystemPaused(false);
}

void MainWindow::SystemLock::cancelResume()
{
  m_was_paused = true;
  m_was_fullscreen = false;
}
