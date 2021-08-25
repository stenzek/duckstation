#include "mainwindow.h"
#include "aboutdialog.h"
#include "autoupdaterdialog.h"
#include "cheatmanagerdialog.h"
#include "common/assert.h"
#include "common/cd_image.h"
#include "core/host_display.h"
#include "core/settings.h"
#include "core/system.h"
#include "debuggerwindow.h"
#include "frontend-common/game_list.h"
#include "gamelistsettingswidget.h"
#include "gamelistwidget.h"
#include "gamepropertiesdialog.h"
#include "gdbserver.h"
#include "memorycardeditordialog.h"
#include "qtdisplaywidget.h"
#include "qthostinterface.h"
#include "qtutils.h"
#include "scmversion/scmversion.h"
#include "settingsdialog.h"
#include "settingwidgetbinder.h"
#include <QtCore/QDebug>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMimeData>
#include <QtCore/QUrl>
#include <QtGui/QCursor>
#include <QtGui/QWindowStateChangeEvent>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QStyleFactory>
#include <cmath>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QtGui/QActionGroup>
#else
#include <QtWidgets/QActionGroup>
#endif

static constexpr char DISC_IMAGE_FILTER[] = QT_TRANSLATE_NOOP(
  "MainWindow",
  "All File Types (*.bin *.img *.iso *.cue *.chd *.ecm *.mds *.pbp *.exe *.psexe *.psf *.minipsf *.m3u);;Single-Track "
  "Raw Images (*.bin *.img *.iso);;Cue Sheets (*.cue);;MAME CHD Images (*.chd);;Error Code Modeler Images "
  "(*.ecm);;Media Descriptor Sidecar Images (*.mds);;PlayStation EBOOTs (*.pbp);;PlayStation Executables (*.exe "
  "*.psexe);;Portable Sound Format Files (*.psf *.minipsf);;Playlists (*.m3u)");

static const char* DEFAULT_THEME_NAME = "darkfusion";

ALWAYS_INLINE static QString getWindowTitle(const QString& game_title)
{
  if (game_title.isEmpty())
  {
#if defined(_DEBUG)
    return QStringLiteral("DuckStation [Debug] %1 (%2)").arg(g_scm_tag_str).arg(g_scm_branch_str);
#elif defined(_DEBUGFAST)
    return QStringLiteral("DuckStation [DebugFast] %1 (%2)").arg(g_scm_tag_str).arg(g_scm_branch_str);
#else
    return QStringLiteral("DuckStation %1 (%2)").arg(g_scm_tag_str).arg(g_scm_branch_str);
#endif
  }

#if defined(_DEBUG)
  return QStringLiteral("%1 [Debug]").arg(game_title);
#elif defined(_DEBUGFAST)
  return QStringLiteral("%2 [DebugFast]").arg(game_title);
#else
  return game_title;
#endif
}

MainWindow::MainWindow(QtHostInterface* host_interface)
  : QMainWindow(nullptr), m_unthemed_style_name(QApplication::style()->objectName()), m_host_interface(host_interface)
{
  m_host_interface->setMainWindow(this);

  // force creation of native window
  winId();
}

MainWindow::~MainWindow()
{
  Assert(!m_display_widget);
  if (m_host_interface->getMainWindow() == this)
    m_host_interface->setMainWindow(nullptr);

  Assert(!m_debugger_window);
}

void MainWindow::initializeAndShow()
{
  setIconThemeFromSettings();

  m_ui.setupUi(this);
  setupAdditionalUi();
  setStyleFromSettings();
  connectSignals();

  restoreStateFromConfig();
  switchToGameListView();

  show();
}

void MainWindow::reportError(const QString& message)
{
  QMessageBox::critical(this, tr("DuckStation"), message, QMessageBox::Ok);
  focusDisplayWidget();
}

void MainWindow::reportMessage(const QString& message)
{
  m_ui.statusBar->showMessage(message, 2000);
}

bool MainWindow::confirmMessage(const QString& message)
{
  const int result = QMessageBox::question(this, tr("DuckStation"), message);
  focusDisplayWidget();

  return (result == QMessageBox::Yes);
}

bool MainWindow::shouldHideCursorInFullscreen() const
{
  return g_host_interface->GetBoolSettingValue("Main", "HideCursorInFullscreen", true);
}

QtDisplayWidget* MainWindow::createDisplay(QThread* worker_thread, bool fullscreen, bool render_to_main)
{
  Assert(!m_host_display && !m_display_widget);
  Assert(!fullscreen || !render_to_main);

  m_host_display = m_host_interface->createHostDisplay();
  if (!m_host_display)
  {
    reportError(tr("Failed to create host display."));
    return nullptr;
  }

  const std::string fullscreen_mode = m_host_interface->GetStringSettingValue("GPU", "FullscreenMode", "");
  const bool is_exclusive_fullscreen = (fullscreen && !fullscreen_mode.empty() && m_host_display->SupportsFullscreen());

  QWidget* container;
  if (QtDisplayContainer::IsNeeded(fullscreen, render_to_main))
  {
    m_display_container = new QtDisplayContainer();
    m_display_widget = new QtDisplayWidget(m_display_container);
    m_display_container->setDisplayWidget(m_display_widget);
    container = m_display_container;
  }
  else
  {
    m_display_widget = new QtDisplayWidget((!fullscreen && render_to_main) ? m_ui.mainContainer : nullptr);
    container = m_display_widget;
  }

  container->setWindowTitle(windowTitle());
  container->setWindowIcon(windowIcon());

  if (fullscreen)
  {
    if (!is_exclusive_fullscreen)
      container->showFullScreen();
    else
      container->showNormal();

    updateMouseMode(System::IsPaused());
  }
  else if (!render_to_main)
  {
    restoreDisplayWindowGeometryFromConfig();
    container->showNormal();
  }
  else
  {
    m_ui.mainContainer->insertWidget(1, m_display_widget);
    switchToEmulationView();
  }

  // we need the surface visible.. this might be able to be replaced with something else
  QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

  m_host_interface->connectDisplaySignals(m_display_widget);

  std::optional<WindowInfo> wi = m_display_widget->getWindowInfo();
  if (!wi.has_value())
  {
    reportError(QStringLiteral("Failed to get window info from widget"));
    destroyDisplayWidget();
    m_host_display = nullptr;
    return nullptr;
  }

  if (!m_host_display->CreateRenderDevice(wi.value(), g_settings.gpu_adapter, g_settings.gpu_use_debug_device,
                                          g_settings.gpu_threaded_presentation))
  {
    reportError(tr("Failed to create host display device context."));
    destroyDisplayWidget();
    m_host_display = nullptr;
    return nullptr;
  }

  if (is_exclusive_fullscreen)
    setDisplayFullscreen(fullscreen_mode);

  m_host_display->DoneRenderContextCurrent();
  return m_display_widget;
}

QtDisplayWidget* MainWindow::updateDisplay(QThread* worker_thread, bool fullscreen, bool render_to_main)
{
  const bool is_fullscreen = m_display_widget->isFullScreen();
  const bool is_rendering_to_main = (!is_fullscreen && m_display_widget->parent());
  const std::string fullscreen_mode = m_host_interface->GetStringSettingValue("GPU", "FullscreenMode", "");
  const bool is_exclusive_fullscreen = (fullscreen && !fullscreen_mode.empty() && m_host_display->SupportsFullscreen());
  if (fullscreen == is_fullscreen && is_rendering_to_main == render_to_main)
    return m_display_widget;

  // Skip recreating the surface if we're just transitioning between fullscreen and windowed with render-to-main off.
  const bool has_container = (m_display_container != nullptr);
  const bool needs_container = QtDisplayContainer::IsNeeded(fullscreen, render_to_main);
  if (!is_rendering_to_main && !render_to_main && !is_exclusive_fullscreen && has_container == needs_container)
  {
    qDebug() << "Toggling to" << (fullscreen ? "fullscreen" : "windowed") << "without recreating surface";
    if (m_host_display && m_host_display->IsFullscreen())
      m_host_display->SetFullscreen(false, 0, 0, 0.0f);

    if (fullscreen)
    {
      m_display_widget->showFullScreen();
    }
    else
    {
      restoreDisplayWindowGeometryFromConfig();
      m_display_widget->showNormal();
    }

    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    updateMouseMode(System::IsPaused());
    return m_display_widget;
  }

  m_host_display->DestroyRenderSurface();

  destroyDisplayWidget();

  QWidget* container;
  if (QtDisplayContainer::IsNeeded(fullscreen, render_to_main))
  {
    m_display_container = new QtDisplayContainer();
    m_display_widget = new QtDisplayWidget(m_display_container);
    m_display_container->setDisplayWidget(m_display_widget);
    container = m_display_container;
  }
  else
  {
    m_display_widget = new QtDisplayWidget((!fullscreen && render_to_main) ? m_ui.mainContainer : nullptr);
    container = m_display_widget;
  }

  container->setWindowTitle(windowTitle());
  container->setWindowIcon(windowIcon());

  if (fullscreen)
  {
    if (!is_exclusive_fullscreen)
      container->showFullScreen();
    else
      container->showNormal();

    updateMouseMode(System::IsPaused());
  }
  else if (!render_to_main)
  {
    restoreDisplayWindowGeometryFromConfig();
    container->showNormal();
  }
  else
  {
    m_ui.mainContainer->insertWidget(1, m_display_widget);
    switchToEmulationView();
  }

  // we need the surface visible.. this might be able to be replaced with something else
  QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

  m_host_interface->connectDisplaySignals(m_display_widget);

  std::optional<WindowInfo> wi = m_display_widget->getWindowInfo();
  if (!wi.has_value())
  {
    reportError(QStringLiteral("Failed to get new window info from widget"));
    destroyDisplayWidget();
    return nullptr;
  }

  if (!m_host_display->ChangeRenderWindow(wi.value()))
    Panic("Failed to recreate surface on new widget.");

  if (is_exclusive_fullscreen)
    setDisplayFullscreen(fullscreen_mode);

  m_display_widget->setFocus();

  QSignalBlocker blocker(m_ui.actionFullscreen);
  m_ui.actionFullscreen->setChecked(fullscreen);
  return m_display_widget;
}

void MainWindow::setDisplayFullscreen(const std::string& fullscreen_mode)
{
  u32 width, height;
  float refresh_rate;
  bool result = false;

  if (CommonHostInterface::ParseFullscreenMode(fullscreen_mode, &width, &height, &refresh_rate))
  {
    result = m_host_display->SetFullscreen(true, width, height, refresh_rate);
    if (result)
    {
      m_host_interface->AddOSDMessage(
        m_host_interface->TranslateStdString("OSDMessage", "Acquired exclusive fullscreen."), 10.0f);
    }
    else
    {
      m_host_interface->AddOSDMessage(
        m_host_interface->TranslateStdString("OSDMessage", "Failed to acquire exclusive fullscreen."), 10.0f);
    }
  }
}

void MainWindow::displaySizeRequested(qint32 width, qint32 height)
{
  if (!m_display_widget)
    return;

  if (m_display_container || !m_display_widget->parent())
  {
    // no parent - rendering to separate window. easy.
    getDisplayContainer()->resize(QSize(std::max<qint32>(width, 1), std::max<qint32>(height, 1)));
    return;
  }

  // we are rendering to the main window. we have to add in the extra height from the toolbar/status bar.
  const s32 extra_height = this->height() - m_display_widget->height();
  resize(QSize(std::max<qint32>(width, 1), std::max<qint32>(height + extra_height, 1)));
}

void MainWindow::destroyDisplay()
{
  if (!m_host_display)
    return;

  DebugAssert(m_host_display && m_display_widget);
  m_host_display = nullptr;
  destroyDisplayWidget();
}

void MainWindow::destroyDisplayWidget()
{
  if (!m_display_widget)
    return;

  if (m_display_container || (!m_display_widget->parent() && !m_display_widget->isFullScreen()))
    saveDisplayWindowGeometryToConfig();

  if (m_display_container)
    m_display_container->removeDisplayWidget();

  if (m_display_widget->parent())
  {
    switchToGameListView();
    m_ui.mainContainer->removeWidget(m_display_widget);
  }

  delete m_display_widget;
  m_display_widget = nullptr;

  delete m_display_container;
  m_display_container = nullptr;
}

void MainWindow::focusDisplayWidget()
{
  if (m_ui.mainContainer->currentIndex() != 1)
    return;

  m_display_widget->setFocus();
}

void MainWindow::onMouseModeRequested(bool relative_mode, bool hide_cursor)
{
  m_relative_mouse_mode = relative_mode;
  m_mouse_cursor_hidden = hide_cursor;
  updateMouseMode(System::IsPaused());
}

void MainWindow::updateMouseMode(bool paused)
{
  if (!m_display_widget)
    return;

  if (paused)
  {
    m_display_widget->unsetCursor();
    m_display_widget->setRelativeMode(false);
    return;
  }

  const bool hide_mouse = m_mouse_cursor_hidden || (m_display_widget->isFullScreen() && shouldHideCursorInFullscreen());
  if (hide_mouse)
    m_display_widget->setCursor(Qt::BlankCursor);
  else
    m_display_widget->unsetCursor();

  m_display_widget->setRelativeMode(m_relative_mouse_mode);
}

void MainWindow::onEmulationStarting()
{
  m_emulation_running = true;
  updateEmulationActions(true, false, m_host_interface->IsCheevosChallengeModeActive());

  // ensure it gets updated, since the boot can take a while
  QGuiApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}

void MainWindow::onEmulationStarted()
{
  updateEmulationActions(false, true, m_host_interface->IsCheevosChallengeModeActive());
}

void MainWindow::onEmulationStopped()
{
  m_emulation_running = false;
  updateEmulationActions(false, false, m_host_interface->IsCheevosChallengeModeActive());
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

void MainWindow::onEmulationPaused(bool paused)
{
  QSignalBlocker blocker(m_ui.actionPause);
  m_ui.actionPause->setChecked(paused);
  updateMouseMode(paused);
}

void MainWindow::onSystemPerformanceCountersUpdated(float speed, float fps, float vps, float average_frame_time,
                                                    float worst_frame_time, GPURenderer renderer, quint32 render_width,
                                                    quint32 render_height, bool render_interlaced)
{
  m_status_speed_widget->setText(QStringLiteral("%1%").arg(speed, 0, 'f', 0));
  m_status_fps_widget->setText(
    QStringLiteral("FPS: %1/%2").arg(std::round(fps), 0, 'f', 0).arg(std::round(vps), 0, 'f', 0));
  m_status_frame_time_widget->setText(
    QStringLiteral("%1ms average, %2ms worst").arg(average_frame_time, 0, 'f', 2).arg(worst_frame_time, 0, 'f', 2));
  m_status_renderer_widget->setText(QString::fromUtf8(Settings::GetRendererName(renderer)));
  m_status_resolution_widget->setText(
    (render_interlaced ? QStringLiteral("%1x%2 (Interlaced)") : QStringLiteral("%1x%2 (Progressive)"))
      .arg(render_width)
      .arg(render_height));
}

void MainWindow::onRunningGameChanged(const QString& filename, const QString& game_code, const QString& game_title)
{
  setWindowTitle(getWindowTitle(game_title));

  if (m_display_widget)
    m_display_widget->setWindowTitle(windowTitle());

  m_running_game_code = game_code.toStdString();
}

void MainWindow::onApplicationStateChanged(Qt::ApplicationState state)
{
  if (!m_emulation_running || !g_settings.pause_on_focus_loss)
    return;

  const bool focus_loss = (state != Qt::ApplicationActive);
  if (focus_loss)
  {
    if (!m_was_paused_by_focus_loss && !System::IsPaused())
    {
      m_host_interface->pauseSystem(true);
      m_was_paused_by_focus_loss = true;
      updateMouseMode(true);
    }
  }
  else
  {
    if (m_was_paused_by_focus_loss)
    {
      if (System::IsPaused())
        m_host_interface->pauseSystem(false);
      m_was_paused_by_focus_loss = false;
      updateMouseMode(false);
    }
  }
}

void MainWindow::onStartFileActionTriggered()
{
  QString filename = QDir::toNativeSeparators(
    QFileDialog::getOpenFileName(this, tr("Select Disc Image"), QString(), tr(DISC_IMAGE_FILTER), nullptr));
  if (filename.isEmpty())
    return;

  m_host_interface->bootSystem(std::make_shared<SystemBootParameters>(filename.toStdString()));
}

std::string MainWindow::getDeviceDiscPath(const QString& title)
{
  std::string ret;

  const auto devices = CDImage::GetDeviceList();
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
  if (m_emulation_running)
    m_host_interface->synchronousPowerOffSystem();

  close();
  m_host_interface->setMainWindow(nullptr);

  MainWindow* new_main_window = new MainWindow(m_host_interface);
  new_main_window->initializeAndShow();
  deleteLater();
}

void MainWindow::onStartDiscActionTriggered()
{
  std::string path(getDeviceDiscPath(tr("Start Disc")));
  if (path.empty())
    return;

  m_host_interface->bootSystem(std::make_shared<SystemBootParameters>(std::move(path)));
}

void MainWindow::onStartBIOSActionTriggered()
{
  m_host_interface->bootSystem(std::make_shared<SystemBootParameters>());
}

void MainWindow::onChangeDiscFromFileActionTriggered()
{
  QString filename =
    QFileDialog::getOpenFileName(this, tr("Select Disc Image"), QString(), tr(DISC_IMAGE_FILTER), nullptr);
  if (filename.isEmpty())
    return;

  m_host_interface->changeDisc(filename);
}

void MainWindow::onChangeDiscFromGameListActionTriggered()
{
  m_host_interface->pauseSystem(true);
  switchToGameListView();
}

void MainWindow::onChangeDiscFromDeviceActionTriggered()
{
  std::string path(getDeviceDiscPath(tr("Change Disc")));
  if (path.empty())
    return;

  m_host_interface->changeDisc(QString::fromStdString(path));
}

void MainWindow::onChangeDiscMenuAboutToShow()
{
  m_host_interface->populateChangeDiscSubImageMenu(m_ui.menuChangeDisc, m_ui.actionGroupChangeDiscSubImages);
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
  m_host_interface->populateLoadStateMenu(m_running_game_code.c_str(), m_ui.menuLoadState);
}

void MainWindow::onSaveStateMenuAboutToShow()
{
  m_host_interface->populateSaveStateMenu(m_running_game_code.c_str(), m_ui.menuSaveState);
}

void MainWindow::onCheatsMenuAboutToShow()
{
  m_ui.menuCheats->clear();
  connect(m_ui.menuCheats->addAction(tr("Cheat Manager")), &QAction::triggered, this,
          &MainWindow::onToolsCheatManagerTriggered);
  m_ui.menuCheats->addSeparator();
  m_host_interface->populateCheatsMenu(m_ui.menuCheats);
}

void MainWindow::onRemoveDiscActionTriggered()
{
  m_host_interface->changeDisc(QString());
}

void MainWindow::onViewToolbarActionToggled(bool checked)
{
  m_ui.toolBar->setVisible(checked);
  saveStateToConfig();
}

void MainWindow::onViewLockToolbarActionToggled(bool checked)
{
  m_host_interface->SetBoolSettingValue("UI", "LockToolbar", checked);
  m_ui.toolBar->setMovable(!checked);
}

void MainWindow::onViewStatusBarActionToggled(bool checked)
{
  m_host_interface->SetBoolSettingValue("UI", "ShowStatusBar", checked);
  m_ui.statusBar->setVisible(checked);
}

void MainWindow::onViewGameListActionTriggered()
{
  if (m_emulation_running)
    m_host_interface->pauseSystem(true);
  switchToGameListView();
  m_game_list_widget->showGameList();
}

void MainWindow::onViewGameGridActionTriggered()
{
  if (m_emulation_running)
    m_host_interface->pauseSystem(true);
  switchToGameListView();
  m_game_list_widget->showGameGrid();
}

void MainWindow::onViewSystemDisplayTriggered()
{
  if (m_emulation_running)
  {
    switchToEmulationView();
    m_host_interface->pauseSystem(false);
  }
}

void MainWindow::onViewGamePropertiesActionTriggered()
{
  if (!m_emulation_running)
    return;

  const std::string& path = System::GetRunningPath();
  if (path.empty())
    return;

  ensureGameListLoaded();

  const GameListEntry* entry = m_host_interface->getGameList()->GetEntryForPath(path.c_str());
  if (!entry)
  {
    QMessageBox::critical(this, tr("DuckStation"),
                          tr("Could not find a game list entry for the currently running file. Please make sure this "
                             "file is in a location scanned by the game list."));
    return;
  }

  GamePropertiesDialog::showForEntry(m_host_interface, entry, this);
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

void MainWindow::onGameListEntrySelected(const GameListEntry* entry)
{
  if (!entry)
  {
    m_ui.statusBar->clearMessage();
    return;
  }

  m_ui.statusBar->showMessage(QString::fromStdString(entry->path));
}

void MainWindow::onGameListEntryDoubleClicked(const GameListEntry* entry)
{
  startGameOrChangeDiscs(entry->path);
}

void MainWindow::onGameListContextMenuRequested(const QPoint& point, const GameListEntry* entry)
{
  QMenu menu;

  // Hopefully this pointer doesn't disappear... it shouldn't.
  if (entry)
  {
    QAction* action = menu.addAction(tr("Properties..."));
    connect(action, &QAction::triggered,
            [this, entry]() { GamePropertiesDialog::showForEntry(m_host_interface, entry, this); });

    connect(menu.addAction(tr("Open Containing Directory...")), &QAction::triggered, [this, entry]() {
      const QFileInfo fi(QString::fromStdString(entry->path));
      QtUtils::OpenURL(this, QUrl::fromLocalFile(fi.absolutePath()));
    });

    connect(menu.addAction(tr("Set Cover Image...")), &QAction::triggered,
            [this, entry]() { onGameListSetCoverImageRequested(entry); });

    menu.addSeparator();

    if (!m_emulation_running)
    {
      m_host_interface->populateGameListContextMenu(entry, this, &menu);
      menu.addSeparator();

      connect(menu.addAction(tr("Default Boot")), &QAction::triggered,
              [this, entry]() { m_host_interface->bootSystem(std::make_shared<SystemBootParameters>(entry->path)); });

      connect(menu.addAction(tr("Fast Boot")), &QAction::triggered, [this, entry]() {
        auto boot_params = std::make_shared<SystemBootParameters>(entry->path);
        boot_params->override_fast_boot = true;
        m_host_interface->bootSystem(std::move(boot_params));
      });

      connect(menu.addAction(tr("Full Boot")), &QAction::triggered, [this, entry]() {
        auto boot_params = std::make_shared<SystemBootParameters>(entry->path);
        boot_params->override_fast_boot = false;
        m_host_interface->bootSystem(std::move(boot_params));
      });

      if (m_ui.menuDebug->menuAction()->isVisible() && !m_host_interface->IsCheevosChallengeModeActive())
      {
        connect(menu.addAction(tr("Boot and Debug")), &QAction::triggered, [this, entry]() {
          m_open_debugger_on_start = true;

          auto boot_params = std::make_shared<SystemBootParameters>(entry->path);
          boot_params->override_start_paused = true;
          m_host_interface->bootSystem(std::move(boot_params));
        });
      }
    }
    else
    {
      connect(menu.addAction(tr("Change Disc")), &QAction::triggered, [this, entry]() {
        m_host_interface->changeDisc(QString::fromStdString(entry->path));
        m_host_interface->pauseSystem(false);
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

void MainWindow::onGameListSetCoverImageRequested(const GameListEntry* entry)
{
  QString filename = QFileDialog::getOpenFileName(this, tr("Select Cover Image"), QString(),
                                                  tr("All Cover Image Types (*.jpg *.jpeg *.png)"));
  if (filename.isEmpty())
    return;

  if (!m_host_interface->getGameList()->GetCoverImagePathForEntry(entry).empty())
  {
    if (QMessageBox::question(this, tr("Cover Already Exists"),
                              tr("A cover image for this game already exists, do you wish to replace it?"),
                              QMessageBox::Yes, QMessageBox::No) != QMessageBox::Yes)
    {
      return;
    }
  }

  QString new_filename = QString::fromStdString(
    m_host_interface->getGameList()->GetNewCoverImagePathForEntry(entry, filename.toStdString().c_str()));
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
  setWindowTitle(getWindowTitle(QString()));

  const bool status_bar_visible = m_host_interface->GetBoolSettingValue("UI", "ShowStatusBar", true);
  m_ui.actionViewStatusBar->setChecked(status_bar_visible);
  m_ui.statusBar->setVisible(status_bar_visible);

  const bool toolbars_locked = m_host_interface->GetBoolSettingValue("UI", "LockToolbar", false);
  m_ui.actionViewLockToolbar->setChecked(toolbars_locked);
  m_ui.toolBar->setMovable(!toolbars_locked);
  m_ui.toolBar->setContextMenuPolicy(Qt::PreventContextMenu);

  m_game_list_widget = new GameListWidget(m_ui.mainContainer);
  m_game_list_widget->initialize(m_host_interface);
  m_ui.mainContainer->insertWidget(0, m_game_list_widget);
  m_ui.mainContainer->setCurrentIndex(0);

  m_status_speed_widget = new QLabel(m_ui.statusBar);
  m_status_speed_widget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  m_status_speed_widget->setFixedSize(50, 16);
  m_status_speed_widget->hide();

  m_status_fps_widget = new QLabel(m_ui.statusBar);
  m_status_fps_widget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  m_status_fps_widget->setFixedSize(80, 16);
  m_status_fps_widget->hide();

  m_status_frame_time_widget = new QLabel(m_ui.statusBar);
  m_status_frame_time_widget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  m_status_frame_time_widget->setFixedSize(190, 16);
  m_status_frame_time_widget->hide();

  m_status_renderer_widget = new QLabel(m_ui.statusBar);
  m_status_renderer_widget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  m_status_renderer_widget->setFixedSize(50, 16);
  m_status_renderer_widget->hide();

  m_status_resolution_widget = new QLabel(m_ui.statusBar);
  m_status_resolution_widget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  m_status_resolution_widget->setFixedSize(140, 16);
  m_status_resolution_widget->hide();

  m_ui.actionGridViewShowTitles->setChecked(m_game_list_widget->getShowGridCoverTitles());

  updateDebugMenuVisibility();

  for (u32 i = 0; i < static_cast<u32>(CPUExecutionMode::Count); i++)
  {
    const CPUExecutionMode mode = static_cast<CPUExecutionMode>(i);
    QAction* action = m_ui.menuCPUExecutionMode->addAction(
      qApp->translate("CPUExecutionMode", Settings::GetCPUExecutionModeDisplayName(mode)));
    action->setCheckable(true);
    connect(action, &QAction::triggered, [this, mode]() {
      m_host_interface->SetStringSettingValue("CPU", "ExecutionMode", Settings::GetCPUExecutionModeName(mode));
      m_host_interface->applySettings();
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
      m_host_interface->SetStringSettingValue("GPU", "Renderer", Settings::GetRendererName(renderer));
      m_host_interface->applySettings();
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
      m_host_interface->SetStringSettingValue("Display", "CropMode", Settings::GetDisplayCropModeName(crop_mode));
      m_host_interface->applySettings();
      updateDebugMenuCropMode();
    });
  }
  updateDebugMenuCropMode();

  const QString current_language(
    QString::fromStdString(m_host_interface->GetStringSettingValue("Main", "Language", "")));
  QActionGroup* language_group = new QActionGroup(m_ui.menuSettingsLanguage);
  for (const std::pair<QString, QString>& it : m_host_interface->getAvailableLanguageList())
  {
    QAction* action = language_group->addAction(it.first);
    action->setCheckable(true);
    action->setChecked(current_language == it.second);
    action->setIcon(QIcon(QStringLiteral(":/icons/flags/%1.png").arg(it.second)));
    m_ui.menuSettingsLanguage->addAction(action);
    action->setData(it.second);
    connect(action, &QAction::triggered, [this, action]() {
      const QString new_language = action->data().toString();
      m_host_interface->SetStringSettingValue("Main", "Language", new_language.toUtf8().constData());
      m_host_interface->reinstallTranslator();
      recreate();
    });
  }

  for (u32 scale = 1; scale <= 10; scale++)
  {
    QAction* action = m_ui.menuWindowSize->addAction(tr("%1x Scale").arg(scale));
    connect(action, &QAction::triggered,
            [scale]() { QtHostInterface::GetInstance()->requestRenderWindowScale(scale); });
  }
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

  m_ui.actionFullscreen->setDisabled(starting || !running);
  m_ui.actionViewGameProperties->setDisabled(starting || !running);

  m_ui.actionLoadState->setDisabled(cheevos_challenge_mode);
  m_ui.menuLoadState->setDisabled(cheevos_challenge_mode);

  if (running && m_status_speed_widget->isHidden())
  {
    m_status_speed_widget->show();
    m_status_fps_widget->show();
    m_status_frame_time_widget->show();
    m_status_renderer_widget->show();
    m_status_resolution_widget->show();
    m_ui.statusBar->addPermanentWidget(m_status_speed_widget);
    m_ui.statusBar->addPermanentWidget(m_status_fps_widget);
    m_ui.statusBar->addPermanentWidget(m_status_resolution_widget);
    m_ui.statusBar->addPermanentWidget(m_status_renderer_widget);
    m_ui.statusBar->addPermanentWidget(m_status_frame_time_widget);
  }
  else if (!running && m_status_speed_widget->isVisible())
  {
    m_ui.statusBar->removeWidget(m_status_renderer_widget);
    m_ui.statusBar->removeWidget(m_status_resolution_widget);
    m_ui.statusBar->removeWidget(m_status_speed_widget);
    m_ui.statusBar->removeWidget(m_status_fps_widget);
    m_ui.statusBar->removeWidget(m_status_frame_time_widget);
    m_status_speed_widget->hide();
    m_status_fps_widget->hide();
    m_status_frame_time_widget->hide();
    m_status_renderer_widget->hide();
    m_status_resolution_widget->hide();
  }

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

bool MainWindow::isShowingGameList() const
{
  return m_ui.mainContainer->currentIndex() == 0;
}

void MainWindow::switchToGameListView()
{
  m_ui.mainContainer->setCurrentIndex(0);
}

void MainWindow::switchToEmulationView()
{
  if (m_display_widget->parent())
    m_ui.mainContainer->setCurrentIndex(1);
  m_display_widget->setFocus();
}

void MainWindow::startGameOrChangeDiscs(const std::string& path)
{
  // if we're not running, boot the system, otherwise swap discs
  if (!m_emulation_running)
  {
    if (m_host_interface->CanResumeSystemFromFile(path.c_str()))
      m_host_interface->resumeSystemFromState(QString::fromStdString(path), true);
    else
      m_host_interface->bootSystem(std::make_shared<SystemBootParameters>(path));
  }
  else
  {
    m_host_interface->changeDisc(QString::fromStdString(path));
    m_host_interface->pauseSystem(false);
    switchToEmulationView();
  }
}

void MainWindow::connectSignals()
{
  updateEmulationActions(false, false, m_host_interface->IsCheevosChallengeModeActive());
  onEmulationPaused(false);

  connect(qApp, &QGuiApplication::applicationStateChanged, this, &MainWindow::onApplicationStateChanged);

  connect(m_ui.actionStartFile, &QAction::triggered, this, &MainWindow::onStartFileActionTriggered);
  connect(m_ui.actionStartDisc, &QAction::triggered, this, &MainWindow::onStartDiscActionTriggered);
  connect(m_ui.actionStartBios, &QAction::triggered, this, &MainWindow::onStartBIOSActionTriggered);
  connect(m_ui.actionResumeLastState, &QAction::triggered, m_host_interface,
          &QtHostInterface::resumeSystemFromMostRecentState);
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
  connect(m_ui.actionPowerOff, &QAction::triggered, m_host_interface, &QtHostInterface::powerOffSystem);
  connect(m_ui.actionPowerOffWithoutSaving, &QAction::triggered, m_host_interface,
          &QtHostInterface::powerOffSystemWithoutSaving);
  connect(m_ui.actionReset, &QAction::triggered, m_host_interface, &QtHostInterface::resetSystem);
  connect(m_ui.actionPause, &QAction::toggled, [this](bool active) { m_host_interface->pauseSystem(active); });
  connect(m_ui.actionScreenshot, &QAction::triggered, m_host_interface, &QtHostInterface::saveScreenshot);
  connect(m_ui.actionScanForNewGames, &QAction::triggered, this,
          [this]() { m_host_interface->refreshGameList(false, false); });
  connect(m_ui.actionRescanAllGames, &QAction::triggered, this,
          [this]() { m_host_interface->refreshGameList(true, false); });
  connect(m_ui.actionLoadState, &QAction::triggered, this, [this]() { m_ui.menuLoadState->exec(QCursor::pos()); });
  connect(m_ui.actionSaveState, &QAction::triggered, this, [this]() { m_ui.menuSaveState->exec(QCursor::pos()); });
  connect(m_ui.actionExit, &QAction::triggered, this, &MainWindow::close);
  connect(m_ui.actionFullscreen, &QAction::triggered, m_host_interface, &QtHostInterface::toggleFullscreen);
  connect(m_ui.actionSettings, &QAction::triggered, [this]() { doSettings(SettingsDialog::Category::Count); });
  connect(m_ui.actionGeneralSettings, &QAction::triggered,
          [this]() { doSettings(SettingsDialog::Category::GeneralSettings); });
  connect(m_ui.actionBIOSSettings, &QAction::triggered,
          [this]() { doSettings(SettingsDialog::Category::BIOSSettings); });
  connect(m_ui.actionConsoleSettings, &QAction::triggered,
          [this]() { doSettings(SettingsDialog::Category::ConsoleSettings); });
  connect(m_ui.actionEmulationSettings, &QAction::triggered,
          [this]() { doSettings(SettingsDialog::Category::EmulationSettings); });
  connect(m_ui.actionGameListSettings, &QAction::triggered,
          [this]() { doSettings(SettingsDialog::Category::GameListSettings); });
  connect(m_ui.actionHotkeySettings, &QAction::triggered,
          [this]() { doSettings(SettingsDialog::Category::HotkeySettings); });
  connect(m_ui.actionControllerSettings, &QAction::triggered,
          [this]() { doSettings(SettingsDialog::Category::ControllerSettings); });
  connect(m_ui.actionMemoryCardSettings, &QAction::triggered,
          [this]() { doSettings(SettingsDialog::Category::MemoryCardSettings); });
  connect(m_ui.actionDisplaySettings, &QAction::triggered,
          [this]() { doSettings(SettingsDialog::Category::DisplaySettings); });
  connect(m_ui.actionEnhancementSettings, &QAction::triggered,
          [this]() { doSettings(SettingsDialog::Category::EnhancementSettings); });
  connect(m_ui.actionPostProcessingSettings, &QAction::triggered,
          [this]() { doSettings(SettingsDialog::Category::PostProcessingSettings); });
  connect(m_ui.actionAudioSettings, &QAction::triggered,
          [this]() { doSettings(SettingsDialog::Category::AudioSettings); });
  connect(m_ui.actionAchievementSettings, &QAction::triggered,
          [this]() { doSettings(SettingsDialog::Category::AchievementSettings); });
  connect(m_ui.actionAdvancedSettings, &QAction::triggered,
          [this]() { doSettings(SettingsDialog::Category::AdvancedSettings); });
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

  connect(m_host_interface, &QtHostInterface::settingsResetToDefault, this, &MainWindow::onSettingsResetToDefault);
  connect(m_host_interface, &QtHostInterface::errorReported, this, &MainWindow::reportError,
          Qt::BlockingQueuedConnection);
  connect(m_host_interface, &QtHostInterface::messageReported, this, &MainWindow::reportMessage);
  connect(m_host_interface, &QtHostInterface::messageConfirmed, this, &MainWindow::confirmMessage,
          Qt::BlockingQueuedConnection);
  connect(m_host_interface, &QtHostInterface::createDisplayRequested, this, &MainWindow::createDisplay,
          Qt::BlockingQueuedConnection);
  connect(m_host_interface, &QtHostInterface::destroyDisplayRequested, this, &MainWindow::destroyDisplay);
  connect(m_host_interface, &QtHostInterface::updateDisplayRequested, this, &MainWindow::updateDisplay,
          Qt::BlockingQueuedConnection);
  connect(m_host_interface, &QtHostInterface::displaySizeRequested, this, &MainWindow::displaySizeRequested);
  connect(m_host_interface, &QtHostInterface::focusDisplayWidgetRequested, this, &MainWindow::focusDisplayWidget);
  connect(m_host_interface, &QtHostInterface::emulationStarting, this, &MainWindow::onEmulationStarting);
  connect(m_host_interface, &QtHostInterface::emulationStarted, this, &MainWindow::onEmulationStarted);
  connect(m_host_interface, &QtHostInterface::emulationStopped, this, &MainWindow::onEmulationStopped);
  connect(m_host_interface, &QtHostInterface::emulationPaused, this, &MainWindow::onEmulationPaused);
  connect(m_host_interface, &QtHostInterface::systemPerformanceCountersUpdated, this,
          &MainWindow::onSystemPerformanceCountersUpdated);
  connect(m_host_interface, &QtHostInterface::runningGameChanged, this, &MainWindow::onRunningGameChanged);
  connect(m_host_interface, &QtHostInterface::exitRequested, this, &MainWindow::close);
  connect(m_host_interface, &QtHostInterface::mouseModeRequested, this, &MainWindow::onMouseModeRequested);

  // These need to be queued connections to stop crashing due to menus opening/closing and switching focus.
  connect(m_game_list_widget, &GameListWidget::entrySelected, this, &MainWindow::onGameListEntrySelected,
          Qt::QueuedConnection);
  connect(m_game_list_widget, &GameListWidget::entryDoubleClicked, this, &MainWindow::onGameListEntryDoubleClicked,
          Qt::QueuedConnection);
  connect(m_game_list_widget, &GameListWidget::entryContextMenuRequested, this,
          &MainWindow::onGameListContextMenuRequested);

  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.actionDisableAllEnhancements, "Main",
                                               "DisableAllEnhancements");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.actionDisableInterlacing, "GPU",
                                               "DisableInterlacing");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.actionForceNTSCTimings, "GPU",
                                               "ForceNTSCTimings");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.actionDebugDumpCPUtoVRAMCopies, "Debug",
                                               "DumpCPUToVRAMCopies");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.actionDebugDumpVRAMtoCPUCopies, "Debug",
                                               "DumpVRAMToCPUCopies");
  connect(m_ui.actionDumpAudio, &QAction::toggled, [this](bool checked) {
    if (checked)
      m_host_interface->startDumpingAudio();
    else
      m_host_interface->stopDumpingAudio();
  });
  connect(m_ui.actionDumpRAM, &QAction::triggered, [this]() {
    const QString filename =
      QFileDialog::getSaveFileName(this, tr("Destination File"), QString(), tr("Binary Files (*.bin)"));
    if (filename.isEmpty())
      return;

    m_host_interface->dumpRAM(filename);
  });
  connect(m_ui.actionDumpVRAM, &QAction::triggered, [this]() {
    const QString filename = QFileDialog::getSaveFileName(this, tr("Destination File"), QString(),
                                                          tr("Binary Files (*.bin);;PNG Images (*.png)"));
    if (filename.isEmpty())
      return;

    m_host_interface->dumpVRAM(filename);
  });
  connect(m_ui.actionDumpSPURAM, &QAction::triggered, [this]() {
    const QString filename =
      QFileDialog::getSaveFileName(this, tr("Destination File"), QString(), tr("Binary Files (*.bin)"));
    if (filename.isEmpty())
      return;

    m_host_interface->dumpSPURAM(filename);
  });
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.actionDebugShowVRAM, "Debug", "ShowVRAM");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.actionDebugShowGPUState, "Debug", "ShowGPUState");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.actionDebugShowCDROMState, "Debug",
                                               "ShowCDROMState");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.actionDebugShowSPUState, "Debug", "ShowSPUState");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.actionDebugShowTimersState, "Debug",
                                               "ShowTimersState");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.actionDebugShowMDECState, "Debug",
                                               "ShowMDECState");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.actionDebugShowDMAState, "Debug", "ShowDMAState");

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
  m_host_interface->SetStringSettingValue("UI", "Theme", theme.toUtf8().constData());
  setStyleFromSettings();
  setIconThemeFromSettings();
  updateMenuSelectedTheme();
  recreate();
}

void MainWindow::setStyleFromSettings()
{
  const std::string theme(m_host_interface->GetStringSettingValue("UI", "Theme", DEFAULT_THEME_NAME));

  if (theme == "qdarkstyle")
  {
    qApp->setStyle(m_unthemed_style_name);
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
    qApp->setStyle(m_unthemed_style_name);
  }
}

void MainWindow::setIconThemeFromSettings()
{
  const std::string theme(m_host_interface->GetStringSettingValue("UI", "Theme", DEFAULT_THEME_NAME));
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
    m_settings_dialog = new SettingsDialog(m_host_interface, this);
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

void MainWindow::saveStateToConfig()
{
  {
    const QByteArray geometry = saveGeometry();
    const QByteArray geometry_b64 = geometry.toBase64();
    const std::string old_geometry_b64 = m_host_interface->GetStringSettingValue("UI", "MainWindowGeometry");
    if (old_geometry_b64 != geometry_b64.constData())
      m_host_interface->SetStringSettingValue("UI", "MainWindowGeometry", geometry_b64.constData());
  }

  {
    const QByteArray state = saveState();
    const QByteArray state_b64 = state.toBase64();
    const std::string old_state_b64 = m_host_interface->GetStringSettingValue("UI", "MainWindowState");
    if (old_state_b64 != state_b64.constData())
      m_host_interface->SetStringSettingValue("UI", "MainWindowState", state_b64.constData());
  }
}

void MainWindow::restoreStateFromConfig()
{
  {
    const std::string geometry_b64 = m_host_interface->GetStringSettingValue("UI", "MainWindowGeometry");
    const QByteArray geometry = QByteArray::fromBase64(QByteArray::fromStdString(geometry_b64));
    if (!geometry.isEmpty())
      restoreGeometry(geometry);
  }

  {
    const std::string state_b64 = m_host_interface->GetStringSettingValue("UI", "MainWindowState");
    const QByteArray state = QByteArray::fromBase64(QByteArray::fromStdString(state_b64));
    if (!state.isEmpty())
      restoreState(state);

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
  const QByteArray geometry = getDisplayContainer()->saveGeometry();
  const QByteArray geometry_b64 = geometry.toBase64();
  const std::string old_geometry_b64 = m_host_interface->GetStringSettingValue("UI", "DisplayWindowGeometry");
  if (old_geometry_b64 != geometry_b64.constData())
    m_host_interface->SetStringSettingValue("UI", "DisplayWindowGeometry", geometry_b64.constData());
}

void MainWindow::restoreDisplayWindowGeometryFromConfig()
{
  const std::string geometry_b64 = m_host_interface->GetStringSettingValue("UI", "DisplayWindowGeometry");
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
    m_settings_dialog = new SettingsDialog(m_host_interface, this);

  return m_settings_dialog;
}

void MainWindow::doSettings(SettingsDialog::Category category)
{
  SettingsDialog* dlg = getSettingsDialog();
  if (!dlg->isVisible())
  {
    dlg->setModal(false);
    dlg->show();
  }

  if (category != SettingsDialog::Category::Count)
    dlg->setCategory(category);
}

void MainWindow::updateDebugMenuCPUExecutionMode()
{
  std::optional<CPUExecutionMode> current_mode =
    Settings::ParseCPUExecutionMode(m_host_interface->GetStringSettingValue("CPU", "ExecutionMode").c_str());
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
    Settings::ParseRendererName(m_host_interface->GetStringSettingValue("GPU", "Renderer").c_str());
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
    Settings::ParseDisplayCropMode(m_host_interface->GetStringSettingValue("Display", "CropMode").c_str());
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
  QString theme = QString::fromStdString(m_host_interface->GetStringSettingValue("UI", "Theme", DEFAULT_THEME_NAME));

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

void MainWindow::ensureGameListLoaded()
{
  if (m_host_interface->getGameList()->IsGameListLoaded())
    return;

  const bool was_running = System::IsRunning();
  if (m_emulation_running)
    m_host_interface->pauseSystem(true, true);

  m_host_interface->refreshGameList();

  if (!was_running)
    m_host_interface->pauseSystem(false, false);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
  m_host_interface->synchronousPowerOffSystem();
  saveStateToConfig();
  QMainWindow::closeEvent(event);
}

void MainWindow::changeEvent(QEvent* event)
{
  if (static_cast<QWindowStateChangeEvent*>(event)->oldState() & Qt::WindowMinimized)
  {
    // TODO: This should check the render-to-main option.
    if (m_display_widget)
      m_host_interface->redrawDisplayWindow();
  }

  QMainWindow::changeEvent(event);
}

static std::string getFilenameFromMimeData(const QMimeData* md)
{
  std::string filename;
  if (md->hasUrls())
  {
    // only one url accepted
    const QList<QUrl> urls(md->urls());
    if (urls.size() == 1)
      filename = urls.front().toLocalFile().toStdString();
  }

  return filename;
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
  const std::string filename(getFilenameFromMimeData(event->mimeData()));
  if (!System::IsLoadableFilename(filename.c_str()))
    return;

  event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event)
{
  const std::string filename(getFilenameFromMimeData(event->mimeData()));
  if (!System::IsLoadableFilename(filename.c_str()))
    return;

  event->acceptProposedAction();
  startGameOrChangeDiscs(filename);
}

void MainWindow::startupUpdateCheck()
{
  if (!m_host_interface->GetBoolSettingValue("AutoUpdater", "CheckAtStartup", true))
    return;

  checkForUpdates(false);
}

void MainWindow::updateDebugMenuVisibility()
{
  const bool visible = m_host_interface->GetBoolSettingValue("Main", "ShowDebugMenu", false);
  m_ui.menuDebug->menuAction()->setVisible(visible);
}

void MainWindow::onCheckForUpdatesActionTriggered()
{
  // Wipe out the last version, that way it displays the update if we've previously skipped it.
  m_host_interface->RemoveSettingValue("AutoUpdater", "LastVersion");
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

void MainWindow::onAchievementsChallengeModeToggled(bool enabled)
{
  if (enabled)
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

  updateEmulationActions(false, System::IsValid(), enabled);
}

void MainWindow::onToolsMemoryCardEditorTriggered()
{
  openMemoryCardEditor(QString(), QString());
}

void MainWindow::onToolsCheatManagerTriggered()
{
  if (!m_cheat_manager_dialog)
  {
    if (m_host_interface->GetBoolSettingValue("UI", "DisplayCheatWarning", true))
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
        QtHostInterface::GetInstance()->SetBoolSettingValue("UI", "DisplayCheatWarning",
                                                            (state != Qt::CheckState::Checked));
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
  m_host_interface->pauseSystem(true, true);
  if (!System::IsValid())
    return;

  Assert(!m_debugger_window);

  m_debugger_window = new DebuggerWindow();
  m_debugger_window->setWindowIcon(windowIcon());
  connect(m_debugger_window, &DebuggerWindow::closed, this, &MainWindow::onCPUDebuggerClosed);
  m_debugger_window->show();

  // the debugger will miss the pause event above (or we were already paused), so fire it now
  m_debugger_window->onEmulationPaused(true);
}

void MainWindow::onCPUDebuggerClosed()
{
  Assert(m_debugger_window);
  m_debugger_window->deleteLater();
  m_debugger_window = nullptr;
}

void MainWindow::onToolsOpenDataDirectoryTriggered()
{
  QtUtils::OpenURL(this, QUrl::fromLocalFile(m_host_interface->getUserDirectoryRelativePath(QString())));
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

  m_auto_updater_dialog = new AutoUpdaterDialog(m_host_interface, this);
  connect(m_auto_updater_dialog, &AutoUpdaterDialog::updateCheckCompleted, this, &MainWindow::onUpdateCheckComplete);
  m_auto_updater_dialog->queueUpdateCheck(display_message);
}

void MainWindow::onUpdateCheckComplete()
{
  if (!m_auto_updater_dialog)
    return;

  m_auto_updater_dialog->deleteLater();
  m_auto_updater_dialog = nullptr;
}
