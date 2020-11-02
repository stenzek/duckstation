#include "mainwindow.h"
#include "aboutdialog.h"
#include "autoupdaterdialog.h"
#include "cheatmanagerdialog.h"
#include "common/assert.h"
#include "core/host_display.h"
#include "core/settings.h"
#include "core/system.h"
#include "frontend-common/game_list.h"
#include "gamelistsettingswidget.h"
#include "gamelistwidget.h"
#include "gamepropertiesdialog.h"
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
#include <QtCore/QUrl>
#include <QtGui/QCursor>
#include <QtGui/QWindowStateChangeEvent>
#include <QtWidgets/QActionGroup>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QStyleFactory>
#include <cmath>

static constexpr char DISC_IMAGE_FILTER[] = QT_TRANSLATE_NOOP(
  "MainWindow",
  "All File Types (*.bin *.img *.cue *.chd *.exe *.psexe *.psf *.m3u);;Single-Track Raw Images (*.bin *.img);;Cue "
  "Sheets (*.cue);;MAME CHD Images (*.chd);;PlayStation Executables (*.exe *.psexe);;Portable Sound Format Files "
  "(*.psf);;Playlists (*.m3u)");

ALWAYS_INLINE static QString getWindowTitle()
{
  return QStringLiteral("DuckStation %1 (%2)").arg(g_scm_tag_str).arg(g_scm_branch_str);
}

MainWindow::MainWindow(QtHostInterface* host_interface)
  : QMainWindow(nullptr), m_unthemed_style_name(QApplication::style()->objectName()), m_host_interface(host_interface)
{
  m_host_interface->setMainWindow(this);

  m_ui.setupUi(this);
  setupAdditionalUi();
  connectSignals();
  updateTheme();

  resize(800, 700);

  restoreStateFromConfig();
  switchToGameListView();
}

MainWindow::~MainWindow()
{
  Assert(!m_display_widget);
  m_host_interface->setMainWindow(nullptr);
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

QtDisplayWidget* MainWindow::createDisplay(QThread* worker_thread, const QString& adapter_name, bool use_debug_device,
                                           bool fullscreen, bool render_to_main)
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

  m_display_widget = new QtDisplayWidget((!fullscreen && render_to_main) ? m_ui.mainContainer : nullptr);
  m_display_widget->setWindowTitle(windowTitle());
  m_display_widget->setWindowIcon(windowIcon());

  if (fullscreen)
  {
    if (!is_exclusive_fullscreen)
      m_display_widget->showFullScreen();
    else
      m_display_widget->showNormal();

    m_display_widget->setCursor(Qt::BlankCursor);
  }
  else if (!render_to_main)
  {
    restoreDisplayWindowGeometryFromConfig();
    m_display_widget->showNormal();
  }
  else
  {
    m_ui.mainContainer->insertWidget(1, m_display_widget);
    switchToEmulationView();
  }

  // we need the surface visible.. this might be able to be replaced with something else
  QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

  std::optional<WindowInfo> wi = m_display_widget->getWindowInfo();
  if (!wi.has_value())
  {
    reportError(QStringLiteral("Failed to get window info from widget"));
    destroyDisplayWidget();
    delete m_host_display;
    m_host_display = nullptr;
    return nullptr;
  }

  if (!m_host_display->CreateRenderDevice(wi.value(), adapter_name.toStdString(), use_debug_device))
  {
    reportError(tr("Failed to create host display device context."));
    destroyDisplayWidget();
    delete m_host_display;
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

  m_host_display->DestroyRenderSurface();

  destroyDisplayWidget();
  m_display_widget = new QtDisplayWidget((!fullscreen && render_to_main) ? m_ui.mainContainer : nullptr);
  m_display_widget->setWindowTitle(windowTitle());
  m_display_widget->setWindowIcon(windowIcon());

  if (fullscreen)
  {
    if (!is_exclusive_fullscreen)
      m_display_widget->showFullScreen();
    else
      m_display_widget->showNormal();
    m_display_widget->setCursor(Qt::BlankCursor);
  }
  else if (!render_to_main)
  {
    restoreDisplayWindowGeometryFromConfig();
    m_display_widget->showNormal();
  }
  else
  {
    m_ui.mainContainer->insertWidget(1, m_display_widget);
    switchToEmulationView();
  }

  // we need the surface visible.. this might be able to be replaced with something else
  QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

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
    if (!result)
    {
      m_host_interface->AddOSDMessage(
        m_host_interface->TranslateStdString("OSDMessage", "Failed to acquire exclusive fullscreen."), 20.0f);
    }
  }
}

void MainWindow::destroyDisplay()
{
  DebugAssert(m_host_display && m_display_widget);
  m_host_display = nullptr;
  destroyDisplayWidget();
}

void MainWindow::destroyDisplayWidget()
{
  if (!m_display_widget)
    return;

  if (m_display_widget->parent())
  {
    switchToGameListView();
    m_ui.mainContainer->removeWidget(m_display_widget);
  }
  else if (!m_display_widget->isFullScreen())
  {
    saveDisplayWindowGeometryToConfig();
  }

  delete m_display_widget;
  m_display_widget = nullptr;
}

void MainWindow::focusDisplayWidget()
{
  if (m_ui.mainContainer->currentIndex() != 1)
    return;

  m_display_widget->setFocus();
}

void MainWindow::onEmulationStarting()
{
  m_emulation_running = true;
  updateEmulationActions(true, false);

  // ensure it gets updated, since the boot can take a while
  QGuiApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}

void MainWindow::onEmulationStarted()
{
  updateEmulationActions(false, true);
}

void MainWindow::onEmulationStopped()
{
  m_emulation_running = false;
  updateEmulationActions(false, false);
  switchToGameListView();

  if (m_cheat_manager_dialog)
  {
    delete m_cheat_manager_dialog;
    m_cheat_manager_dialog = nullptr;
  }
}

void MainWindow::onEmulationPaused(bool paused)
{
  QSignalBlocker blocker(m_ui.actionPause);
  m_ui.actionPause->setChecked(paused);
}

void MainWindow::onStateSaved(const QString& game_code, bool global, qint32 slot)
{
  // don't bother updating for the resume state since we're powering off anyway
  if (slot < 0)
    return;

  m_host_interface->populateSaveStateMenus(game_code.toStdString().c_str(), m_ui.menuLoadState, m_ui.menuSaveState);
}

void MainWindow::onSystemPerformanceCountersUpdated(float speed, float fps, float vps, float average_frame_time,
                                                    float worst_frame_time)
{
  m_status_speed_widget->setText(QStringLiteral("%1%").arg(speed, 0, 'f', 0));
  m_status_fps_widget->setText(
    QStringLiteral("FPS: %1/%2").arg(std::round(fps), 0, 'f', 0).arg(std::round(vps), 0, 'f', 0));
  m_status_frame_time_widget->setText(
    QStringLiteral("%1ms average, %2ms worst").arg(average_frame_time, 0, 'f', 2).arg(worst_frame_time, 0, 'f', 2));
}

void MainWindow::onRunningGameChanged(const QString& filename, const QString& game_code, const QString& game_title)
{
  m_host_interface->populateSaveStateMenus(game_code.toStdString().c_str(), m_ui.menuLoadState, m_ui.menuSaveState);
  if (game_title.isEmpty())
    setWindowTitle(getWindowTitle());
  else
    setWindowTitle(game_title);

  if (m_display_widget)
    m_display_widget->setWindowTitle(windowTitle());
}

void MainWindow::onStartDiscActionTriggered()
{
  QString filename =
    QFileDialog::getOpenFileName(this, tr("Select Disc Image"), QString(), tr(DISC_IMAGE_FILTER), nullptr);
  if (filename.isEmpty())
    return;

  m_host_interface->bootSystem(std::make_shared<const SystemBootParameters>(filename.toStdString()));
}

void MainWindow::onStartBIOSActionTriggered()
{
  m_host_interface->bootSystem(std::make_shared<const SystemBootParameters>());
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

void MainWindow::onChangeDiscFromPlaylistMenuAboutToShow()
{
  m_host_interface->populatePlaylistEntryMenu(m_ui.menuChangeDiscFromPlaylist);
}

void MainWindow::onChangeDiscFromPlaylistMenuAboutToHide()
{
  m_ui.menuChangeDiscFromPlaylist->clear();
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
  const GameListEntry* entry;

  if (m_emulation_running)
  {
    const std::string& path = System::GetRunningPath();
    if (path.empty())
      return;

    entry = m_host_interface->getGameList()->GetEntryForPath(path.c_str());
  }
  else
  {
    entry = m_game_list_widget->getSelectedEntry();
  }

  if (!entry)
    return;

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
    m_host_interface->populateSaveStateMenus("", m_ui.menuLoadState, m_ui.menuSaveState);
    return;
  }

  m_ui.statusBar->showMessage(QString::fromStdString(entry->path));
  m_host_interface->populateSaveStateMenus(entry->code.c_str(), m_ui.menuLoadState, m_ui.menuSaveState);
}

void MainWindow::onGameListEntryDoubleClicked(const GameListEntry* entry)
{
  // if we're not running, boot the system, otherwise swap discs
  QString path = QString::fromStdString(entry->path);
  if (!m_emulation_running)
  {
    if (!entry->code.empty() && m_host_interface->GetBoolSettingValue("Main", "SaveStateOnExit", true))
    {
      m_host_interface->resumeSystemFromState(path, true);
    }
    else
    {
      m_host_interface->bootSystem(std::make_shared<const SystemBootParameters>(path.toStdString()));
    }
  }
  else
  {
    m_host_interface->changeDisc(path);
    m_host_interface->pauseSystem(false);
    switchToEmulationView();
  }
}

void MainWindow::onGameListContextMenuRequested(const QPoint& point, const GameListEntry* entry)
{
  QMenu menu;

  // Hopefully this pointer doesn't disappear... it shouldn't.
  if (entry)
  {
    connect(menu.addAction(tr("Properties...")), &QAction::triggered,
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
      if (!entry->code.empty())
      {
        m_host_interface->populateGameListContextMenu(entry->code.c_str(), this, &menu);
        menu.addSeparator();
      }

      connect(menu.addAction(tr("Default Boot")), &QAction::triggered, [this, entry]() {
        m_host_interface->bootSystem(std::make_shared<const SystemBootParameters>(entry->path));
      });

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
  setWindowTitle(getWindowTitle());

  const bool status_bar_visible = m_host_interface->GetBoolSettingValue("UI", "ShowStatusBar", true);
  m_ui.actionViewStatusBar->setChecked(status_bar_visible);
  m_ui.statusBar->setVisible(status_bar_visible);

  m_game_list_widget = new GameListWidget(m_ui.mainContainer);
  m_game_list_widget->initialize(m_host_interface);
  m_ui.mainContainer->insertWidget(0, m_game_list_widget);
  m_ui.mainContainer->setCurrentIndex(0);

  m_status_speed_widget = new QLabel(m_ui.statusBar);
  m_status_speed_widget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  m_status_speed_widget->setFixedSize(40, 16);
  m_status_speed_widget->hide();

  m_status_fps_widget = new QLabel(m_ui.statusBar);
  m_status_fps_widget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  m_status_fps_widget->setFixedSize(80, 16);
  m_status_fps_widget->hide();

  m_status_frame_time_widget = new QLabel(m_ui.statusBar);
  m_status_frame_time_widget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  m_status_frame_time_widget->setFixedSize(190, 16);
  m_status_frame_time_widget->hide();

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
    m_ui.menuSettingsLanguage->addAction(action);
    action->setData(it.second);
    connect(action, &QAction::triggered, [this, action]() {
      const QString new_language = action->data().toString();
      m_host_interface->SetStringSettingValue("Main", "Language", new_language.toUtf8().constData());
      QMessageBox::information(this, tr("DuckStation"),
                               tr("Language changed. Please restart the application to apply."));
    });
  }
}

void MainWindow::updateEmulationActions(bool starting, bool running)
{
  m_ui.actionStartDisc->setDisabled(starting || running);
  m_ui.actionStartBios->setDisabled(starting || running);
  m_ui.actionResumeLastState->setDisabled(starting || running);

  m_ui.actionPowerOff->setDisabled(starting || !running);
  m_ui.actionReset->setDisabled(starting || !running);
  m_ui.actionPause->setDisabled(starting || !running);
  m_ui.actionChangeDisc->setDisabled(starting || !running);
  m_ui.actionCheats->setDisabled(starting || !running);
  m_ui.actionScreenshot->setDisabled(starting || !running);
  m_ui.actionViewSystemDisplay->setEnabled(starting || running);
  m_ui.menuChangeDisc->setDisabled(starting || !running);
  m_ui.menuCheats->setDisabled(starting || !running);
  m_ui.actionCheatManager->setDisabled(starting || !running);

  m_ui.actionSaveState->setDisabled(starting || !running);
  m_ui.menuSaveState->setDisabled(starting || !running);

  m_ui.actionFullscreen->setDisabled(starting || !running);

  if (running && m_status_speed_widget->isHidden())
  {
    m_status_speed_widget->show();
    m_status_fps_widget->show();
    m_status_frame_time_widget->show();
    m_ui.statusBar->addPermanentWidget(m_status_speed_widget);
    m_ui.statusBar->addPermanentWidget(m_status_fps_widget);
    m_ui.statusBar->addPermanentWidget(m_status_frame_time_widget);
  }
  else if (!running && m_status_speed_widget->isVisible())
  {
    m_ui.statusBar->removeWidget(m_status_speed_widget);
    m_ui.statusBar->removeWidget(m_status_fps_widget);
    m_ui.statusBar->removeWidget(m_status_frame_time_widget);
    m_status_speed_widget->hide();
    m_status_fps_widget->hide();
    m_status_frame_time_widget->hide();
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

void MainWindow::connectSignals()
{
  updateEmulationActions(false, false);
  onEmulationPaused(false);

  connect(m_ui.actionStartDisc, &QAction::triggered, this, &MainWindow::onStartDiscActionTriggered);
  connect(m_ui.actionStartBios, &QAction::triggered, this, &MainWindow::onStartBIOSActionTriggered);
  connect(m_ui.actionResumeLastState, &QAction::triggered, m_host_interface,
          &QtHostInterface::resumeSystemFromMostRecentState);
  connect(m_ui.actionChangeDisc, &QAction::triggered, [this] { m_ui.menuChangeDisc->exec(QCursor::pos()); });
  connect(m_ui.actionChangeDiscFromFile, &QAction::triggered, this, &MainWindow::onChangeDiscFromFileActionTriggered);
  connect(m_ui.actionChangeDiscFromGameList, &QAction::triggered, this,
          &MainWindow::onChangeDiscFromGameListActionTriggered);
  connect(m_ui.menuChangeDiscFromPlaylist, &QMenu::aboutToShow, this,
          &MainWindow::onChangeDiscFromPlaylistMenuAboutToShow);
  connect(m_ui.menuChangeDiscFromPlaylist, &QMenu::aboutToHide, this,
          &MainWindow::onChangeDiscFromPlaylistMenuAboutToHide);
  connect(m_ui.menuCheats, &QMenu::aboutToShow, this, &MainWindow::onCheatsMenuAboutToShow);
  connect(m_ui.actionCheats, &QAction::triggered, [this] { m_ui.menuCheats->exec(QCursor::pos()); });
  connect(m_ui.actionRemoveDisc, &QAction::triggered, this, &MainWindow::onRemoveDiscActionTriggered);
  connect(m_ui.actionAddGameDirectory, &QAction::triggered,
          [this]() { getSettingsDialog()->getGameListSettingsWidget()->addSearchDirectory(this); });
  connect(m_ui.actionPowerOff, &QAction::triggered, m_host_interface, &QtHostInterface::powerOffSystem);
  connect(m_ui.actionReset, &QAction::triggered, m_host_interface, &QtHostInterface::resetSystem);
  connect(m_ui.actionPause, &QAction::toggled, m_host_interface, &QtHostInterface::pauseSystem);
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
  connect(m_ui.actionAdvancedSettings, &QAction::triggered,
          [this]() { doSettings(SettingsDialog::Category::AdvancedSettings); });
  connect(m_ui.actionViewToolbar, &QAction::toggled, this, &MainWindow::onViewToolbarActionToggled);
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
  connect(m_host_interface, &QtHostInterface::focusDisplayWidgetRequested, this, &MainWindow::focusDisplayWidget);
  connect(m_host_interface, &QtHostInterface::emulationStarting, this, &MainWindow::onEmulationStarting);
  connect(m_host_interface, &QtHostInterface::emulationStarted, this, &MainWindow::onEmulationStarted);
  connect(m_host_interface, &QtHostInterface::emulationStopped, this, &MainWindow::onEmulationStopped);
  connect(m_host_interface, &QtHostInterface::emulationPaused, this, &MainWindow::onEmulationPaused);
  connect(m_host_interface, &QtHostInterface::stateSaved, this, &MainWindow::onStateSaved);
  connect(m_host_interface, &QtHostInterface::systemPerformanceCountersUpdated, this,
          &MainWindow::onSystemPerformanceCountersUpdated);
  connect(m_host_interface, &QtHostInterface::runningGameChanged, this, &MainWindow::onRunningGameChanged);
  connect(m_host_interface, &QtHostInterface::exitRequested, this, &MainWindow::close);

  // These need to be queued connections to stop crashing due to menus opening/closing and switching focus.
  connect(m_game_list_widget, &GameListWidget::entrySelected, this, &MainWindow::onGameListEntrySelected,
          Qt::QueuedConnection);
  connect(m_game_list_widget, &GameListWidget::entryDoubleClicked, this, &MainWindow::onGameListEntryDoubleClicked,
          Qt::QueuedConnection);
  connect(m_game_list_widget, &GameListWidget::entryContextMenuRequested, this,
          &MainWindow::onGameListContextMenuRequested);

  m_host_interface->populateSaveStateMenus(nullptr, m_ui.menuLoadState, m_ui.menuSaveState);

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
    const QString filename = QFileDialog::getSaveFileName(this, tr("Destination File"));
    if (filename.isEmpty())
      return;

    m_host_interface->dumpRAM(filename);
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

  addThemeToMenu(tr("Default"), QStringLiteral("default"));
  addThemeToMenu(tr("Fusion"), QStringLiteral("fusion"));
  addThemeToMenu(tr("Dark Fusion (Gray)"), QStringLiteral("darkfusion"));
  addThemeToMenu(tr("Dark Fusion (Blue)"), QStringLiteral("darkfusionblue"));
  addThemeToMenu(tr("QDarkStyle"), QStringLiteral("qdarkstyle"));
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
  updateTheme();
}

void MainWindow::updateTheme()
{
  QString theme = QString::fromStdString(m_host_interface->GetStringSettingValue("UI", "Theme", "default"));
  if (theme == QStringLiteral("qdarkstyle"))
  {
    qApp->setStyle(m_unthemed_style_name);
    qApp->setPalette(QApplication::style()->standardPalette());

    QFile f(QStringLiteral(":qdarkstyle/style.qss"));
    if (f.open(QFile::ReadOnly | QFile::Text))
      qApp->setStyleSheet(f.readAll());
  }
  else if (theme == QStringLiteral("fusion"))
  {
    qApp->setPalette(QApplication::style()->standardPalette());
    qApp->setStyleSheet(QString());
    qApp->setStyle(QStyleFactory::create("Fusion"));
  }
  else if (theme == QStringLiteral("darkfusion"))
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
  else if (theme == QStringLiteral("darkfusionblue"))
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
  const QByteArray geometry = m_display_widget->saveGeometry();
  const QByteArray geometry_b64 = geometry.toBase64();
  const std::string old_geometry_b64 = m_host_interface->GetStringSettingValue("UI", "DisplayWindowGeometry");
  if (old_geometry_b64 != geometry_b64.constData())
    m_host_interface->SetStringSettingValue("UI", "DisplayWindowGeometry", geometry_b64.constData());
}

void MainWindow::restoreDisplayWindowGeometryFromConfig()
{
  const std::string geometry_b64 = m_host_interface->GetStringSettingValue("UI", "DisplayWindowGeometry");
  const QByteArray geometry = QByteArray::fromBase64(QByteArray::fromStdString(geometry_b64));
  if (!geometry.isEmpty())
    m_display_widget->restoreGeometry(geometry);
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

void MainWindow::onToolsMemoryCardEditorTriggered()
{
  if (!m_memory_card_editor_dialog)
    m_memory_card_editor_dialog = new MemoryCardEditorDialog(this);

  m_memory_card_editor_dialog->setModal(false);
  m_memory_card_editor_dialog->show();
}

void MainWindow::onToolsCheatManagerTriggered()
{
  if (!m_cheat_manager_dialog)
    m_cheat_manager_dialog = new CheatManagerDialog(this);

  m_cheat_manager_dialog->setModal(false);
  m_cheat_manager_dialog->show();
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
#ifdef WIN32
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
