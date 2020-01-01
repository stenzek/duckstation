#include "qthostinterface.h"
#include "YBaseLib/Log.h"
#include "common/null_audio_stream.h"
#include "core/game_list.h"
#include "core/gpu.h"
#include "core/system.h"
#include "qtsettingsinterface.h"
#include <QtCore/QCoreApplication>
#include <QtWidgets/QMessageBox>
#include <memory>
Log_SetChannel(QtHostInterface);

QtHostInterface::QtHostInterface(QObject* parent)
  : QObject(parent), m_qsettings("duckstation-qt.ini", QSettings::IniFormat)
{
  checkSettings();
  createGameList();
  createThread();
}

QtHostInterface::~QtHostInterface()
{
  Assert(!m_opengl_display_window);
  stopThread();
}

void QtHostInterface::ReportError(const char* message)
{
  // QMessageBox::critical(nullptr, tr("DuckStation Error"), message, QMessageBox::Ok);
}

void QtHostInterface::ReportMessage(const char* message)
{
  // QMessageBox::information(nullptr, tr("DuckStation Information"), message, QMessageBox::Ok);
}

void QtHostInterface::setDefaultSettings()
{
  m_settings.SetDefaults();
  updateQSettings();
}

void QtHostInterface::updateQSettings()
{
  QtSettingsInterface si(m_qsettings);
  m_settings.Save(si);
  // m_qsettings.sync();
}

void QtHostInterface::applySettings()
{
  QtSettingsInterface si(m_qsettings);
  m_settings.Load(si);
}

void QtHostInterface::checkSettings()
{
  const QSettings::Status settings_status = m_qsettings.status();
  if (settings_status != QSettings::NoError)
    m_qsettings.clear();

  const QString settings_version_key = QStringLiteral("General/SettingsVersion");
  const int expected_version = 1;
  const QVariant settings_version_var = m_qsettings.value(settings_version_key);
  bool settings_version_okay;
  int settings_version = settings_version_var.toInt(&settings_version_okay);
  if (!settings_version_okay)
    settings_version = 0;
  if (settings_version != expected_version)
  {
    Log_WarningPrintf("Settings version %d does not match expected version %d, resetting", settings_version,
                      expected_version);
    m_qsettings.clear();
    m_qsettings.setValue(settings_version_key, expected_version);
    setDefaultSettings();
  }

  applySettings();
}

void QtHostInterface::createGameList()
{
  m_game_list = std::make_unique<GameList>();
  updateGameListDatabase(false);
  refreshGameList(false);
}

void QtHostInterface::updateGameListDatabase(bool refresh_list /*= true*/)
{
  m_game_list->ClearDatabase();

  const QString redump_dat_path = m_qsettings.value("GameList/RedumpDatabasePath").toString();
  if (!redump_dat_path.isEmpty())
    m_game_list->ParseRedumpDatabase(redump_dat_path.toStdString().c_str());

  if (refresh_list)
    refreshGameList(true);
}

void QtHostInterface::refreshGameList(bool invalidate_cache /*= false*/)
{
  QtSettingsInterface si(m_qsettings);
  m_game_list->SetDirectoriesFromSettings(si);
  m_game_list->RescanAllDirectories();
  emit gameListRefreshed();
}

QWidget* QtHostInterface::createDisplayWidget(QWidget* parent)
{
  m_opengl_display_window = new OpenGLDisplayWindow(nullptr);
  m_display.release();
  m_display = std::unique_ptr<HostDisplay>(static_cast<HostDisplay*>(m_opengl_display_window));
  return QWidget::createWindowContainer(m_opengl_display_window, parent);
}

void QtHostInterface::destroyDisplayWidget()
{
  m_display.release();
  delete m_opengl_display_window;
  m_opengl_display_window = nullptr;
}

void QtHostInterface::bootSystem(QString initial_filename, QString initial_save_state_filename)
{
  emit emulationStarting();

  if (!m_opengl_display_window->createGLContext(m_worker_thread))
  {
    emit emulationStopped();
    return;
  }

  QMetaObject::invokeMethod(this, "doBootSystem", Qt::QueuedConnection, Q_ARG(QString, initial_filename),
                            Q_ARG(QString, initial_save_state_filename));
}

void QtHostInterface::powerOffSystem()
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "doPowerOffSystem", Qt::QueuedConnection);
    return;
  }

  if (!m_system)
  {
    Log_ErrorPrintf("powerOffSystem() called without system");
    return;
  }

  m_system.reset();
  m_opengl_display_window->destroyGLContext();

  emit emulationStopped();
}

void QtHostInterface::resetSystem()
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "resetSystem", Qt::QueuedConnection);
    return;
  }

  if (!m_system)
  {
    Log_ErrorPrintf("resetSystem() called without system");
    return;
  }

  HostInterface::ResetSystem();
}

void QtHostInterface::pauseSystem(bool paused)
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "pauseSystem", Qt::QueuedConnection, Q_ARG(bool, paused));
    return;
  }

  m_paused = paused;
  emit emulationPaused(paused);
}

void QtHostInterface::changeDisc(QString new_disc_filename) {}

void QtHostInterface::doBootSystem(QString initial_filename, QString initial_save_state_filename)
{
  if (!m_opengl_display_window->initializeGLContext())
  {
    emit emulationStopped();
    return;
  }

  m_audio_stream = NullAudioStream::Create();
  m_audio_stream->Reconfigure();

  std::string initial_filename_str = initial_filename.toStdString();
  std::string initial_save_state_filename_str = initial_save_state_filename.toStdString();
  if (!CreateSystem() ||
      !BootSystem(initial_filename_str.empty() ? nullptr : initial_filename_str.c_str(),
                  initial_save_state_filename_str.empty() ? nullptr : initial_save_state_filename_str.c_str()))
  {
    m_opengl_display_window->destroyGLContext();
    emit emulationStopped();
    return;
  }

  emit emulationStarted();
}

void QtHostInterface::createThread()
{
  m_original_thread = QThread::currentThread();
  m_worker_thread = new Thread(this);
  m_worker_thread->start();
  moveToThread(m_worker_thread);
}

void QtHostInterface::stopThread()
{
  Assert(!isOnWorkerThread());

  QMetaObject::invokeMethod(this, "doStopThread", Qt::QueuedConnection);
  m_worker_thread->wait();
}

void QtHostInterface::doStopThread()
{
  m_shutdown_flag.store(true);
}

void QtHostInterface::threadEntryPoint()
{
  while (!m_shutdown_flag.load())
  {
    if (!m_system)
    {
      // wait until we have a system before running
      QCoreApplication::processEvents(QEventLoop::AllEvents, 1000);
      continue;
    }

    // execute the system, polling events inbetween frames
    // simulate the system if not paused
    if (m_system && !m_paused)
      m_system->RunFrame();

    // rendering
    {
      // DrawImGui();

      if (m_system)
        m_system->GetGPU()->ResetGraphicsAPIState();

      // ImGui::Render();
      m_display->Render();

      // ImGui::NewFrame();

      if (m_system)
      {
        m_system->GetGPU()->RestoreGraphicsAPIState();

        if (m_speed_limiter_enabled)
          Throttle();
      }

      UpdatePerformanceCounters();
    }

    QCoreApplication::processEvents(QEventLoop::AllEvents, m_paused ? 16 : 0);
  }

  m_system.reset();

  // move back to UI thread
  moveToThread(m_original_thread);
}

QtHostInterface::Thread::Thread(QtHostInterface* parent) : QThread(parent), m_parent(parent) {}

QtHostInterface::Thread::~Thread() = default;

void QtHostInterface::Thread::run()
{
  m_parent->threadEntryPoint();
}
