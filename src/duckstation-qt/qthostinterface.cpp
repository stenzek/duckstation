#include "qthostinterface.h"
#include "YBaseLib/Log.h"
#include "YBaseLib/String.h"
#include "common/null_audio_stream.h"
#include "core/controller.h"
#include "core/game_list.h"
#include "core/gpu.h"
#include "core/system.h"
#include "qtaudiostream.h"
#include "qtsettingsinterface.h"
#include "qtutils.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtWidgets/QMessageBox>
#include <memory>
Log_SetChannel(QtHostInterface);

QtHostInterface::QtHostInterface(QObject* parent)
  : QObject(parent), m_qsettings("duckstation-qt.ini", QSettings::IniFormat)
{
  checkSettings();
  createGameList();
  doUpdateInputMap();
  createAudioStream();
  createThread();
}

QtHostInterface::~QtHostInterface()
{
  Assert(!m_display_window);
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

  // default input settings for Qt
  m_settings.controller_types[0] = ControllerType::DigitalController;
  m_qsettings.setValue(QStringLiteral("Controller1/ButtonUp"), QStringLiteral("Keyboard/W"));
  m_qsettings.setValue(QStringLiteral("Controller1/ButtonDown"), QStringLiteral("Keyboard/S"));
  m_qsettings.setValue(QStringLiteral("Controller1/ButtonLeft"), QStringLiteral("Keyboard/A"));
  m_qsettings.setValue(QStringLiteral("Controller1/ButtonRight"), QStringLiteral("Keyboard/D"));
  m_qsettings.setValue(QStringLiteral("Controller1/ButtonSelect"), QStringLiteral("Keyboard/Backspace"));
  m_qsettings.setValue(QStringLiteral("Controller1/ButtonStart"), QStringLiteral("Keyboard/Return"));
  m_qsettings.setValue(QStringLiteral("Controller1/ButtonTriangle"), QStringLiteral("Keyboard/8"));
  m_qsettings.setValue(QStringLiteral("Controller1/ButtonCross"), QStringLiteral("Keyboard/2"));
  m_qsettings.setValue(QStringLiteral("Controller1/ButtonSquare"), QStringLiteral("Keyboard/4"));
  m_qsettings.setValue(QStringLiteral("Controller1/ButtonCircle"), QStringLiteral("Keyboard/6"));
  m_qsettings.setValue(QStringLiteral("Controller1/ButtonL1"), QStringLiteral("Keyboard/Q"));
  m_qsettings.setValue(QStringLiteral("Controller1/ButtonL2"), QStringLiteral("Keyboard/1"));
  m_qsettings.setValue(QStringLiteral("Controller1/ButtonR1"), QStringLiteral("Keyboard/E"));
  m_qsettings.setValue(QStringLiteral("Controller1/ButtonR2"), QStringLiteral("Keyboard/3"));

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
  m_display_window = new OpenGLDisplayWindow(this, nullptr);
  connect(m_display_window, &QtDisplayWindow::windowResizedEvent, this, &QtHostInterface::onDisplayWindowResized);

  m_display.release();
  m_display = std::unique_ptr<HostDisplay>(m_display_window->getHostDisplayInterface());
  return QWidget::createWindowContainer(m_display_window, parent);
}

void QtHostInterface::destroyDisplayWidget()
{
  m_display.release();
  delete m_display_window;
  m_display_window = nullptr;
}

void QtHostInterface::bootSystem(QString initial_filename, QString initial_save_state_filename)
{
  emit emulationStarting();

  if (!m_display_window->createDeviceContext(m_worker_thread))
  {
    emit emulationStopped();
    return;
  }

  QMetaObject::invokeMethod(this, "doBootSystem", Qt::QueuedConnection, Q_ARG(QString, initial_filename),
                            Q_ARG(QString, initial_save_state_filename));
}

void QtHostInterface::handleKeyEvent(int key, bool pressed)
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "doHandleKeyEvent", Qt::QueuedConnection, Q_ARG(int, key), Q_ARG(bool, pressed));
    return;
  }

  doHandleKeyEvent(key, pressed);
}

void QtHostInterface::doHandleKeyEvent(int key, bool pressed)
{
  const auto iter = m_keyboard_input_handlers.find(key);
  if (iter == m_keyboard_input_handlers.end())
    return;

  iter->second(pressed);
}

void QtHostInterface::onDisplayWindowResized(int width, int height)
{
  m_display_window->onWindowResized(width, height);
}

void QtHostInterface::updateInputMap()
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "doUpdateInputMap", Qt::QueuedConnection);
    return;
  }

  doUpdateInputMap();
}

void QtHostInterface::doUpdateInputMap()
{
  m_keyboard_input_handlers.clear();

  updateControllerInputMap();
  updateHotkeyInputMap();
}

void QtHostInterface::updateControllerInputMap()
{
  for (u32 controller_index = 0; controller_index < 2; controller_index++)
  {
    const ControllerType ctype = m_settings.controller_types[controller_index];
    if (ctype == ControllerType::None)
      continue;

    const auto button_names = Controller::GetButtonNames(ctype);
    for (const auto& [button_name, button_code] : button_names)
    {
      QVariant var = m_qsettings.value(
        QStringLiteral("Controller%1/Button%2").arg(controller_index + 1).arg(QString::fromStdString(button_name)));
      if (!var.isValid())
        continue;

      addButtonToInputMap(var.toString(), [this, controller_index, button_code](bool pressed) {
        if (!m_system)
          return;

        Controller* controller = m_system->GetController(controller_index);
        if (controller)
          controller->SetButtonState(button_code, pressed);
      });
    }
  }
}

std::vector<QtHostInterface::HotkeyInfo> QtHostInterface::getHotkeyList() const
{
  std::vector<HotkeyInfo> hotkeys = {
    {QStringLiteral("FastForward"), QStringLiteral("Toggle Fast Forward"), QStringLiteral("General")},
    {QStringLiteral("Fullscreen"), QStringLiteral("Toggle Fullscreen"), QStringLiteral("General")},
    {QStringLiteral("Pause"), QStringLiteral("Toggle Pause"), QStringLiteral("General")}};

  for (u32 i = 1; i <= NUM_SAVE_STATE_HOTKEYS; i++)
  {
    hotkeys.push_back(
      {QStringLiteral("LoadState%1").arg(i), QStringLiteral("Load State %1").arg(i), QStringLiteral("Save States")});
  }
  for (u32 i = 1; i <= NUM_SAVE_STATE_HOTKEYS; i++)
  {
    hotkeys.push_back(
      {QStringLiteral("SaveState%1").arg(i), QStringLiteral("Save State %1").arg(i), QStringLiteral("Save States")});
  }

  return hotkeys;
}

void QtHostInterface::updateHotkeyInputMap()
{
  auto hk = [this](const QString& hotkey_name, InputButtonHandler handler) {
    QVariant var = m_qsettings.value(QStringLiteral("Hotkeys/%1").arg(hotkey_name));
    if (!var.isValid())
      return;

    addButtonToInputMap(var.toString(), std::move(handler));
  };

  hk(QStringLiteral("FastForward"), [this](bool pressed) {
    m_speed_limiter_temp_disabled = pressed;
    HostInterface::UpdateSpeedLimiterState();
  });

  hk(QStringLiteral("Fullscreen"), [this](bool pressed) {
    if (!pressed)
      emit toggleFullscreenRequested();
  });

  hk(QStringLiteral("Pause"), [this](bool pressed) {
    if (!pressed)
      pauseSystem(!m_paused);
  });

  for (u32 i = 1; i <= NUM_SAVE_STATE_HOTKEYS; i++)
  {
    hk(QStringLiteral("LoadState%1").arg(i), [this, i](bool pressed) {
      if (!pressed)
        HostInterface::LoadState(TinyString::FromFormat("savestate_%u.bin", i));
    });

    hk(QStringLiteral("SaveState%1").arg(i), [this, i](bool pressed) {
      if (!pressed)
        HostInterface::SaveState(TinyString::FromFormat("savestate_%u.bin", i));
    });
  }
}

void QtHostInterface::addButtonToInputMap(const QString& binding, InputButtonHandler handler)
{
  const QString device = binding.section('/', 0, 0);
  const QString button = binding.section('/', 1, 1);
  if (device == QStringLiteral("Keyboard"))
  {
    std::optional<int> key_id = QtUtils::ParseKeyString(button);
    if (!key_id.has_value())
    {
      qWarning() << "Unknown keyboard key " << button;
      return;
    }

    m_keyboard_input_handlers.emplace(key_id.value(), std::move(handler));
  }
  else
  {
    qWarning() << "Unknown input device: " << device;
    return;
  }
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
  m_audio_stream->PauseOutput(true);
  m_audio_stream->EmptyBuffers();
  m_display_window->destroyDeviceContext();

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
  m_audio_stream->PauseOutput(paused);
  emit emulationPaused(paused);
}

void QtHostInterface::changeDisc(QString new_disc_filename) {}

void QtHostInterface::doBootSystem(QString initial_filename, QString initial_save_state_filename)
{
  if (!m_display_window->initializeDeviceContext())
  {
    emit emulationStopped();
    return;
  }

  std::string initial_filename_str = initial_filename.toStdString();
  std::string initial_save_state_filename_str = initial_save_state_filename.toStdString();
  if (!CreateSystem() ||
      !BootSystem(initial_filename_str.empty() ? nullptr : initial_filename_str.c_str(),
                  initial_save_state_filename_str.empty() ? nullptr : initial_save_state_filename_str.c_str()))
  {
    m_display_window->destroyDeviceContext();
    emit emulationStopped();
    return;
  }

  m_audio_stream->PauseOutput(false);
  emit emulationStarted();
}

void QtHostInterface::createAudioStream()
{
  // Qt at least on Windows seems to want a buffer size of at least 8KB.
  m_audio_stream = QtAudioStream::Create();
  if (!m_audio_stream->Reconfigure(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, AUDIO_BUFFER_SIZE, 4))
  {
    qWarning() << "Failed to configure audio stream, falling back to null output";

    // fall back to null output
    m_audio_stream.reset();
    m_audio_stream = NullAudioStream::Create();
    m_audio_stream->Reconfigure(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, AUDIO_BUFFER_SIZE, 4);
  }
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
      if (m_system)
      {
        DrawDebugWindows();
        m_system->GetGPU()->ResetGraphicsAPIState();
      }

      DrawFPSWindow();
      DrawOSDMessages();

      m_display->Render();

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
