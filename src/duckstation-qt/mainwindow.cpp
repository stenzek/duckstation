#include "mainwindow.h"
#include "core/game_list.h"
#include "core/settings.h"
#include "gamelistwidget.h"
#include "qthostinterface.h"
#include "qtsettingsinterface.h"
#include "settingsdialog.h"
#include <QtWidgets/QFileDialog>

static constexpr char DISC_IMAGE_FILTER[] =
  "All File Types (*.bin *.img *.cue *.exe *.psexe);;Single-Track Raw Images (*.bin *.img);;Cue Sheets "
  "(*.cue);;PlayStation Executables (*.exe *.psexe)";

MainWindow::MainWindow(QtHostInterface* host_interface) : QMainWindow(nullptr), m_host_interface(host_interface)
{
  m_ui.setupUi(this);
  setupAdditionalUi();
  connectSignals();

  resize(750, 690);
}

MainWindow::~MainWindow()
{
  m_host_interface->destroyDisplayWidget();
}

void MainWindow::onEmulationStarting()
{
  switchToEmulationView();
  updateEmulationActions(true, false);

  // we need the surface visible..
  QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}

void MainWindow::onEmulationStarted()
{
  updateEmulationActions(false, true);
  m_emulation_running = true;
}

void MainWindow::onEmulationStopped()
{
  updateEmulationActions(false, false);
  switchToGameListView();
  m_emulation_running = false;
}

void MainWindow::onEmulationPaused(bool paused)
{
  m_ui.actionPause->setChecked(paused);
}

void MainWindow::onStartDiscActionTriggered()
{
  QString filename =
    QFileDialog::getOpenFileName(this, tr("Select Disc Image"), QString(), tr(DISC_IMAGE_FILTER), nullptr);
  if (filename.isEmpty())
    return;

  m_host_interface->bootSystem(std::move(filename), QString());
}

void MainWindow::onChangeDiscActionTriggered()
{
  QMenu menu(tr("Change Disc..."), this);
  QAction* from_file = menu.addAction(tr("From File..."));
  QAction* from_game_list = menu.addAction(tr("From Game List"));

  QAction* selected = menu.exec(QCursor::pos());
  if (selected == from_file)
  {
    QString filename =
      QFileDialog::getOpenFileName(this, tr("Select Disc Image"), QString(), tr(DISC_IMAGE_FILTER), nullptr);
    if (filename.isEmpty())
      return;

    m_host_interface->changeDisc(filename);
  }
  else if (selected == from_game_list)
  {
    m_host_interface->pauseSystem(true);
    switchToGameListView();
  }
}

void MainWindow::onStartBiosActionTriggered()
{
  m_host_interface->bootSystem(QString(), QString());
}

void MainWindow::onOpenDirectoryActionTriggered() {}

void MainWindow::onExitActionTriggered() {}

void MainWindow::onFullscreenActionToggled(bool fullscreen) {}

void MainWindow::onGitHubRepositoryActionTriggered() {}

void MainWindow::onIssueTrackerActionTriggered() {}

void MainWindow::onAboutActionTriggered() {}

void MainWindow::setupAdditionalUi()
{
  m_game_list_widget = new GameListWidget(m_ui.mainContainer);
  m_game_list_widget->initialize(m_host_interface);
  m_ui.mainContainer->insertWidget(0, m_game_list_widget);

  QWidget* display_widget = m_host_interface->createDisplayWidget(m_ui.mainContainer);
  m_ui.mainContainer->insertWidget(1, display_widget);

  m_ui.mainContainer->setCurrentIndex(0);
}

void MainWindow::updateEmulationActions(bool starting, bool running)
{
  m_ui.actionStartDisc->setDisabled(starting || running);
  m_ui.actionStartBios->setDisabled(starting || running);
  m_ui.actionOpenDirectory->setDisabled(starting || running);
  m_ui.actionPowerOff->setDisabled(starting || running);

  m_ui.actionPowerOff->setDisabled(starting || !running);
  m_ui.actionReset->setDisabled(starting || !running);
  m_ui.actionPause->setDisabled(starting || !running);
  m_ui.actionChangeDisc->setDisabled(starting || !running);

  m_ui.actionLoadState->setDisabled(starting);
  m_ui.actionSaveState->setDisabled(starting);

  m_ui.actionFullscreen->setDisabled(starting || !running);
}

void MainWindow::switchToGameListView()
{
  m_ui.mainContainer->setCurrentIndex(0);
}

void MainWindow::switchToEmulationView()
{
  m_ui.mainContainer->setCurrentIndex(1);
}

void MainWindow::connectSignals()
{
  updateEmulationActions(false, false);
  onEmulationPaused(false);

  connect(m_ui.actionStartDisc, &QAction::triggered, this, &MainWindow::onStartDiscActionTriggered);
  connect(m_ui.actionStartBios, &QAction::triggered, this, &MainWindow::onStartBiosActionTriggered);
  connect(m_ui.actionChangeDisc, &QAction::triggered, this, &MainWindow::onChangeDiscActionTriggered);
  connect(m_ui.actionOpenDirectory, &QAction::triggered, this, &MainWindow::onOpenDirectoryActionTriggered);
  connect(m_ui.actionPowerOff, &QAction::triggered, m_host_interface, &QtHostInterface::powerOffSystem);
  connect(m_ui.actionReset, &QAction::triggered, m_host_interface, &QtHostInterface::resetSystem);
  connect(m_ui.actionPause, &QAction::toggled, m_host_interface, &QtHostInterface::pauseSystem);
  connect(m_ui.actionExit, &QAction::triggered, this, &MainWindow::onExitActionTriggered);
  connect(m_ui.actionFullscreen, &QAction::toggled, this, &MainWindow::onFullscreenActionToggled);
  connect(m_ui.actionSettings, &QAction::triggered, [this]() { doSettings(SettingsDialog::Category::Count); });
  connect(m_ui.actionGameListSettings, &QAction::triggered,
          [this]() { doSettings(SettingsDialog::Category::GameListSettings); });
  connect(m_ui.actionPortSettings, &QAction::triggered,
          [this]() { doSettings(SettingsDialog::Category::PortSettings); });
  connect(m_ui.actionCPUSettings, &QAction::triggered, [this]() { doSettings(SettingsDialog::Category::CPUSettings); });
  connect(m_ui.actionGPUSettings, &QAction::triggered, [this]() { doSettings(SettingsDialog::Category::GPUSettings); });
  connect(m_ui.actionAudioSettings, &QAction::triggered,
          [this]() { doSettings(SettingsDialog::Category::AudioSettings); });
  connect(m_ui.actionGitHubRepository, &QAction::triggered, this, &MainWindow::onGitHubRepositoryActionTriggered);
  connect(m_ui.actionIssueTracker, &QAction::triggered, this, &MainWindow::onIssueTrackerActionTriggered);
  connect(m_ui.actionAbout, &QAction::triggered, this, &MainWindow::onAboutActionTriggered);

  connect(m_host_interface, &QtHostInterface::emulationStarting, this, &MainWindow::onEmulationStarting);
  connect(m_host_interface, &QtHostInterface::emulationStarted, this, &MainWindow::onEmulationStarted);
  connect(m_host_interface, &QtHostInterface::emulationStopped, this, &MainWindow::onEmulationStopped);
  connect(m_host_interface, &QtHostInterface::emulationPaused, this, &MainWindow::onEmulationPaused);

  connect(m_game_list_widget, &GameListWidget::bootEntryRequested, [this](const GameList::GameListEntry& entry) {
    // if we're not running, boot the system, otherwise swap discs
    QString path = QString::fromStdString(entry.path);
    if (!m_emulation_running)
    {
      m_host_interface->bootSystem(path, QString());
    }
    else
    {
      m_host_interface->changeDisc(path);
      m_host_interface->pauseSystem(false);
      switchToEmulationView();
    }
  });
}

void MainWindow::doSettings(SettingsDialog::Category category)
{
  if (!m_settings_dialog)
    m_settings_dialog = new SettingsDialog(m_host_interface, this);

  if (!m_settings_dialog->isVisible())
  {
    m_settings_dialog->setModal(false);
    m_settings_dialog->show();
  }

  if (category != SettingsDialog::Category::Count)
    m_settings_dialog->setCategory(category);
}