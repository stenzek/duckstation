#include "qthostinterface.h"
#include "common/assert.h"
#include "common/audio_stream.h"
#include "common/byte_stream.h"
#include "common/log.h"
#include "common/string_util.h"
#include "core/controller.h"
#include "core/game_list.h"
#include "core/gpu.h"
#include "core/system.h"
#include "frontend-common/sdl_audio_stream.h"
#include "frontend-common/sdl_controller_interface.h"
#include "qtsettingsinterface.h"
#include "qtutils.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QEventLoop>
#include <QtCore/QTimer>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <memory>
Log_SetChannel(QtHostInterface);

#ifdef WIN32
#include "d3d11displaywindow.h"
#endif

QtHostInterface::QtHostInterface(QObject* parent)
  : QObject(parent), HostInterface(), m_qsettings(QString::fromStdString(GetSettingsFileName()), QSettings::IniFormat)
{
  checkSettings();
  refreshGameList();
  createThread();
}

QtHostInterface::~QtHostInterface()
{
  Assert(!m_display_window);
  stopThread();
}

void QtHostInterface::ReportError(const char* message)
{
  HostInterface::ReportError(message);

  emit errorReported(QString::fromLocal8Bit(message));
}

void QtHostInterface::ReportMessage(const char* message)
{
  HostInterface::ReportMessage(message);

  emit messageReported(QString::fromLocal8Bit(message));
}

bool QtHostInterface::ConfirmMessage(const char* message)
{
  return messageConfirmed(QString::fromLocal8Bit(message));
}

void QtHostInterface::setDefaultSettings()
{
  HostInterface::UpdateSettings([this]() { HostInterface::SetDefaultSettings(); });

  // default input settings for Qt
  std::lock_guard<std::mutex> guard(m_qsettings_mutex);
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

  updateQSettingsFromCoreSettings();
}

QVariant QtHostInterface::getSettingValue(const QString& name, const QVariant& default_value)
{
  std::lock_guard<std::mutex> guard(m_qsettings_mutex);
  return m_qsettings.value(name, default_value);
}

void QtHostInterface::putSettingValue(const QString& name, const QVariant& value)
{
  std::lock_guard<std::mutex> guard(m_qsettings_mutex);
  m_qsettings.setValue(name, value);
}

void QtHostInterface::removeSettingValue(const QString& name)
{
  std::lock_guard<std::mutex> guard(m_qsettings_mutex);
  m_qsettings.remove(name);
}

void QtHostInterface::updateQSettingsFromCoreSettings()
{
  QtSettingsInterface si(m_qsettings);
  m_settings.Save(si);
}

void QtHostInterface::applySettings()
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "applySettings", Qt::QueuedConnection);
    return;
  }

  UpdateSettings([this]() {
    std::lock_guard<std::mutex> guard(m_qsettings_mutex);
    QtSettingsInterface si(m_qsettings);
    m_settings.Load(si);
  });
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

  // initial setting init - we don't do this locked since the thread hasn't been created yet
  QtSettingsInterface si(m_qsettings);
  m_settings.Load(si);
}

void QtHostInterface::refreshGameList(bool invalidate_cache /* = false */, bool invalidate_database /* = false */)
{
  std::lock_guard<std::mutex> lock(m_qsettings_mutex);
  QtSettingsInterface si(m_qsettings);
  m_game_list->SetSearchDirectoriesFromSettings(si);
  m_game_list->Refresh(invalidate_cache, invalidate_database);
  emit gameListRefreshed();
}

QtDisplayWindow* QtHostInterface::createDisplayWindow()
{
  Assert(!m_display_window);

#ifdef WIN32
  if (m_settings.gpu_renderer == GPURenderer::HardwareOpenGL)
    m_display_window = new OpenGLDisplayWindow(this, nullptr);
  else
    m_display_window = new D3D11DisplayWindow(this, nullptr);
#else
  m_display_window = new OpenGLDisplayWindow(this, nullptr);
#endif
  connect(m_display_window, &QtDisplayWindow::windowResizedEvent, this, &QtHostInterface::onDisplayWindowResized);
  return m_display_window;
}

void QtHostInterface::bootSystemFromFile(const QString& filename)
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "bootSystemFromFile", Qt::QueuedConnection, Q_ARG(const QString&, filename));
    return;
  }

  HostInterface::BootSystemFromFile(filename.toStdString().c_str());
}

void QtHostInterface::resumeSystemFromState(const QString& filename, bool boot_on_failure)
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "resumeSystemFromState", Qt::QueuedConnection, Q_ARG(const QString&, filename),
                              Q_ARG(bool, boot_on_failure));
    return;
  }

  if (filename.isEmpty())
    HostInterface::ResumeSystemFromMostRecentState();
  else
    HostInterface::ResumeSystemFromState(filename.toStdString().c_str(), boot_on_failure);
}

void QtHostInterface::bootSystemFromBIOS()
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "bootSystemFromBIOS", Qt::QueuedConnection);
    return;
  }

  HostInterface::BootSystemFromBIOS();
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
  // this can be null if it was destroyed and the main thread is late catching up
  if (m_display_window)
    m_display_window->onWindowResized(width, height);
}

bool QtHostInterface::AcquireHostDisplay()
{
  DebugAssert(!m_display_window);

  emit createDisplayWindowRequested(m_worker_thread, m_settings.gpu_use_debug_device);
  if (!m_display_window->hasDeviceContext())
  {
    m_display_window = nullptr;
    emit destroyDisplayWindowRequested();
    return false;
  }

  if (!m_display_window->initializeDeviceContext(m_settings.gpu_use_debug_device))
  {
    m_display_window->destroyDeviceContext();
    m_display_window = nullptr;
    emit destroyDisplayWindowRequested();
    return false;
  }

  m_display = m_display_window->getHostDisplayInterface();
  return true;
}

void QtHostInterface::ReleaseHostDisplay()
{
  DebugAssert(m_display_window && m_display == m_display_window->getHostDisplayInterface());
  m_display = nullptr;
  m_display_window->destroyDeviceContext();
  m_display_window = nullptr;
  emit destroyDisplayWindowRequested();
}

std::unique_ptr<AudioStream> QtHostInterface::CreateAudioStream(AudioBackend backend)
{
  switch (backend)
  {
    case AudioBackend::Null:
      return AudioStream::CreateNullAudioStream();

    case AudioBackend::Cubeb:
      return AudioStream::CreateCubebAudioStream();

    case AudioBackend::SDL:
      return SDLAudioStream::Create();

    default:
      return nullptr;
  }
}

void QtHostInterface::OnSystemCreated()
{
  HostInterface::OnSystemCreated();

  wakeThread();
  destroyBackgroundControllerPollTimer();

  emit emulationStarted();
}

void QtHostInterface::OnSystemPaused(bool paused)
{
  HostInterface::OnSystemPaused(paused);

  emit emulationPaused(paused);

  if (!paused)
  {
    wakeThread();
    emit focusDisplayWidgetRequested();
  }
}

void QtHostInterface::OnSystemDestroyed()
{
  HostInterface::OnSystemDestroyed();

  if (m_background_controller_polling_enable_count > 0)
    createBackgroundControllerPollTimer();

  emit emulationStopped();
}

void QtHostInterface::OnSystemPerformanceCountersUpdated()
{
  HostInterface::OnSystemPerformanceCountersUpdated();

  DebugAssert(m_system);
  emit systemPerformanceCountersUpdated(m_system->GetEmulationSpeed(), m_system->GetFPS(), m_system->GetVPS(),
                                        m_system->GetAverageFrameTime(), m_system->GetWorstFrameTime());
}

void QtHostInterface::OnRunningGameChanged()
{
  HostInterface::OnRunningGameChanged();

  if (m_system)
  {
    emit runningGameChanged(QString::fromStdString(m_system->GetRunningPath()),
                            QString::fromStdString(m_system->GetRunningCode()),
                            QString::fromStdString(m_system->GetRunningTitle()));
  }
  else
  {
    emit runningGameChanged(QString(), QString(), QString());
  }
}

void QtHostInterface::OnControllerTypeChanged(u32 slot)
{
  HostInterface::OnControllerTypeChanged(slot);

  updateInputMap();
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
  g_sdl_controller_interface.ClearControllerBindings();

  std::lock_guard<std::mutex> lock(m_qsettings_mutex);
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
    for (const auto& it : button_names)
    {
      const std::string& button_name = it.first;
      const s32 button_code = it.second;

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

    const auto axis_names = Controller::GetAxisNames(ctype);
    for (const auto& it : axis_names)
    {
      const std::string& axis_name = it.first;
      const s32 axis_code = it.second;

      QVariant var = m_qsettings.value(
        QStringLiteral("Controller%1/Axis%2").arg(controller_index + 1).arg(QString::fromStdString(axis_name)));
      if (!var.isValid())
        continue;

      addAxisToInputMap(var.toString(), [this, controller_index, axis_code](float value) {
        if (!m_system)
          return;

        Controller* controller = m_system->GetController(controller_index);
        if (controller)
          controller->SetAxisState(axis_code, value);
      });
    }
  }
}

std::vector<QtHostInterface::HotkeyInfo> QtHostInterface::getHotkeyList() const
{
  std::vector<HotkeyInfo> hotkeys = {
    {QStringLiteral("FastForward"), QStringLiteral("Toggle Fast Forward"), QStringLiteral("General")},
    {QStringLiteral("Fullscreen"), QStringLiteral("Toggle Fullscreen"), QStringLiteral("General")},
    {QStringLiteral("Pause"), QStringLiteral("Toggle Pause"), QStringLiteral("General")},
    {QStringLiteral("ToggleSoftwareRendering"), QStringLiteral("Toggle Software Rendering"),
     QStringLiteral("Graphics")},
    {QStringLiteral("IncreaseResolutionScale"), QStringLiteral("Increase Resolution Scale"),
     QStringLiteral("Graphics")},
    {QStringLiteral("DecreaseResolutionScale"), QStringLiteral("Decrease Resolution Scale"),
     QStringLiteral("Graphics")}};

  for (u32 global_i = 0; global_i < 2; global_i++)
  {
    const bool global = ConvertToBoolUnchecked(global_i);
    const u32 count = global ? GLOBAL_SAVE_STATE_SLOTS : PER_GAME_SAVE_STATE_SLOTS;
    for (u32 i = 1; i <= count; i++)
    {
      hotkeys.push_back({QStringLiteral("Load%1State%2").arg(global ? "Global" : "Game").arg(i),
                         QStringLiteral("Load %1 State %2").arg(global ? tr("Global") : tr("Game")).arg(i),
                         QStringLiteral("Save States")});
    }
    for (u32 slot = 1; slot <= count; slot++)
    {
      hotkeys.push_back({QStringLiteral("Save%1State%2").arg(global ? "Global" : "Game").arg(slot),
                         QStringLiteral("Save %1 State %2").arg(global ? tr("Global") : tr("Game")).arg(slot),
                         QStringLiteral("Save States")});
    }
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

  hk(QStringLiteral("ToggleSoftwareRendering"), [this](bool pressed) {
    if (!pressed)
      ToggleSoftwareRendering();
  });

  hk(QStringLiteral("IncreaseResolutionScale"), [this](bool pressed) {
    if (!pressed)
      ModifyResolutionScale(1);
  });

  hk(QStringLiteral("DecreaseResolutionScale"), [this](bool pressed) {
    if (!pressed)
      ModifyResolutionScale(-1);
  });

  for (u32 global_i = 0; global_i < 2; global_i++)
  {
    const bool global = ConvertToBoolUnchecked(global_i);
    const u32 count = global ? GLOBAL_SAVE_STATE_SLOTS : PER_GAME_SAVE_STATE_SLOTS;
    for (u32 slot = 1; slot <= count; slot++)
    {
      hk(QStringLiteral("Load%1State%2").arg(global ? "Global" : "Game").arg(slot), [this, global, slot](bool pressed) {
        if (!pressed)
          loadState(global, slot);
      });
      hk(QStringLiteral("Save%1State%2").arg(global ? "Global" : "Game").arg(slot), [this, global, slot](bool pressed) {
        if (!pressed)
          saveState(global, slot);
      });
    }
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
  else if (device.startsWith(QStringLiteral("Controller")))
  {
    bool controller_index_okay;
    const int controller_index = device.mid(10).toInt(&controller_index_okay);
    if (!controller_index_okay || controller_index < 0)
    {
      qWarning() << "Malformed controller binding: " << binding;
      return;
    }

    if (button.startsWith(QStringLiteral("Button")))
    {
      bool button_index_okay;
      const int button_index = button.mid(6).toInt(&button_index_okay);
      if (!button_index_okay ||
          !g_sdl_controller_interface.BindControllerButton(controller_index, button_index, std::move(handler)))
      {
        qWarning() << "Failed to bind " << binding;
      }
    }
    else if (button.startsWith(QStringLiteral("+Axis")) || button.startsWith(QStringLiteral("-Axis")))
    {
      bool axis_index_okay;
      const int axis_index = button.mid(5).toInt(&axis_index_okay);
      const bool positive = (button[0] == '+');
      if (!axis_index_okay || !g_sdl_controller_interface.BindControllerAxisToButton(controller_index, axis_index,
                                                                                     positive, std::move(handler)))
      {
        qWarning() << "Failed to bind " << binding;
      }
    }
  }
  else
  {
    qWarning() << "Unknown input device: " << binding;
    return;
  }
}

void QtHostInterface::addAxisToInputMap(const QString& binding, InputAxisHandler handler)
{
  const QString device = binding.section('/', 0, 0);
  const QString axis = binding.section('/', 1, 1);
  if (device.startsWith(QStringLiteral("Controller")))
  {
    bool controller_index_okay;
    const int controller_index = device.mid(10).toInt(&controller_index_okay);
    if (!controller_index_okay || controller_index < 0)
    {
      qWarning() << "Malformed controller binding: " << binding;
      return;
    }

    if (axis.startsWith(QStringLiteral("Axis")))
    {
      bool axis_index_okay;
      const int axis_index = axis.mid(4).toInt(&axis_index_okay);
      if (!axis_index_okay ||
          !g_sdl_controller_interface.BindControllerAxis(controller_index, axis_index, std::move(handler)))
      {
        qWarning() << "Failed to bind " << binding;
      }
    }
  }
  else
  {
    qWarning() << "Unknown input device: " << binding;
    return;
  }
}

void QtHostInterface::powerOffSystem()
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "powerOffSystem", Qt::QueuedConnection);
    return;
  }

  if (!m_system)
    return;

  if (m_settings.save_state_on_exit)
    SaveResumeSaveState();

  DestroySystem();
}

void QtHostInterface::synchronousPowerOffSystem()
{
  if (!isOnWorkerThread())
    QMetaObject::invokeMethod(this, "powerOffSystem", Qt::BlockingQueuedConnection);
  else
    powerOffSystem();
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
  if (!paused)
    wakeThread();
  emit emulationPaused(paused);
}

void QtHostInterface::changeDisc(const QString& new_disc_filename)
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "changeDisc", Qt::QueuedConnection, Q_ARG(const QString&, new_disc_filename));
    return;
  }

  if (!m_system)
    return;

  m_system->InsertMedia(new_disc_filename.toStdString().c_str());
}

void QtHostInterface::populateSaveStateMenus(const char* game_code, QMenu* load_menu, QMenu* save_menu)
{
  const std::vector<SaveStateInfo> available_states(GetAvailableSaveStates(game_code));

  load_menu->clear();
  if (!available_states.empty())
  {
    bool last_global = available_states.front().global;
    s32 last_slot = available_states.front().slot;
    for (const SaveStateInfo& ssi : available_states)
    {
      const s32 slot = ssi.slot;
      const bool global = ssi.global;
      const QDateTime timestamp(QDateTime::fromSecsSinceEpoch(static_cast<qint64>(ssi.timestamp)));
      const QString timestamp_str(timestamp.toString(Qt::SystemLocaleShortDate));
      const QString path(QString::fromStdString(ssi.path));

      QString title;
      if (slot < 0)
        title = tr("Resume Save (%1)").arg(timestamp_str);
      else
        title = tr("%1 Save %2 (%3)").arg(global ? tr("Global") : tr("Game")).arg(slot).arg(timestamp_str);

      if (global != last_global || last_slot < 0)
        load_menu->addSeparator();

      last_global = global;
      last_slot = slot;

      QAction* action = load_menu->addAction(title);
      connect(action, &QAction::triggered, [this, path]() { loadState(path); });
    }
  }

  save_menu->clear();
  if (game_code && std::strlen(game_code) > 0)
  {
    for (s32 i = 1; i <= PER_GAME_SAVE_STATE_SLOTS; i++)
    {
      QAction* action = save_menu->addAction(tr("Game Save %1").arg(i));
      connect(action, &QAction::triggered, [this, i]() { saveState(false, i); });
    }

    save_menu->addSeparator();
  }

  for (s32 i = 1; i <= GLOBAL_SAVE_STATE_SLOTS; i++)
  {
    QAction* action = save_menu->addAction(tr("Global Save %1").arg(i));
    connect(action, &QAction::triggered, [this, i]() { saveState(true, i); });
  }
}

void QtHostInterface::loadState(const QString& filename)
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "loadState", Qt::QueuedConnection, Q_ARG(const QString&, filename));
    return;
  }

  LoadState(filename.toStdString().c_str());
}

void QtHostInterface::loadState(bool global, qint32 slot)
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "loadState", Qt::QueuedConnection, Q_ARG(bool, global), Q_ARG(qint32, slot));
    return;
  }

  LoadState(global, slot);
}

void QtHostInterface::saveState(bool global, qint32 slot, bool block_until_done /* = false */)
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "saveState", block_until_done ? Qt::BlockingQueuedConnection : Qt::QueuedConnection,
                              Q_ARG(bool, global), Q_ARG(qint32, slot), Q_ARG(bool, block_until_done));
    return;
  }

  if (m_system)
  {
    SaveState(global, slot);
    emit stateSaved(QString::fromStdString(m_system->GetRunningCode()), global, slot);
  }
}

void QtHostInterface::enableBackgroundControllerPolling()
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "enableBackgroundControllerPolling", Qt::BlockingQueuedConnection);
    return;
  }

  if (m_background_controller_polling_enable_count++ > 0)
    return;

  if (!m_system)
  {
    createBackgroundControllerPollTimer();

    // drain the event queue so we don't get events late
    g_sdl_controller_interface.PumpSDLEvents();
  }
}

void QtHostInterface::disableBackgroundControllerPolling()
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "disableBackgroundControllerPolling");
    return;
  }

  Assert(m_background_controller_polling_enable_count > 0);
  if (--m_background_controller_polling_enable_count > 0)
    return;

  if (!m_system)
    destroyBackgroundControllerPollTimer();
}

void QtHostInterface::doBackgroundControllerPoll()
{
  g_sdl_controller_interface.PumpSDLEvents();
}

void QtHostInterface::createBackgroundControllerPollTimer()
{
  DebugAssert(!m_background_controller_polling_timer);
  m_background_controller_polling_timer = new QTimer(this);
  m_background_controller_polling_timer->setSingleShot(false);
  m_background_controller_polling_timer->setTimerType(Qt::VeryCoarseTimer);
  connect(m_background_controller_polling_timer, &QTimer::timeout, this, &QtHostInterface::doBackgroundControllerPoll);
  m_background_controller_polling_timer->start(BACKGROUND_CONTROLLER_POLLING_INTERVAL);
}

void QtHostInterface::destroyBackgroundControllerPollTimer()
{
  delete m_background_controller_polling_timer;
  m_background_controller_polling_timer = nullptr;
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
  m_worker_thread_event_loop->quit();
}

void QtHostInterface::threadEntryPoint()
{
  m_worker_thread_event_loop = new QEventLoop();

  // set up controller interface and immediate poll to pick up the controller attached events
  g_sdl_controller_interface.Initialize(this);
  g_sdl_controller_interface.PumpSDLEvents();

  doUpdateInputMap();

  // TODO: Event which flags the thread as ready
  while (!m_shutdown_flag.load())
  {
    if (!m_system || m_paused)
    {
      // wait until we have a system before running
      m_worker_thread_event_loop->exec();
      continue;
    }

    m_system->RunFrame();

    m_system->GetGPU()->ResetGraphicsAPIState();

    DrawDebugWindows();
    DrawOSDMessages();

    m_display->Render();

    m_system->GetGPU()->RestoreGraphicsAPIState();

    if (m_speed_limiter_enabled)
      m_system->Throttle();

    m_worker_thread_event_loop->processEvents(QEventLoop::AllEvents);
    g_sdl_controller_interface.PumpSDLEvents();
  }

  m_system.reset();
  m_audio_stream.reset();

  g_sdl_controller_interface.Shutdown();

  delete m_worker_thread_event_loop;
  m_worker_thread_event_loop = nullptr;

  // move back to UI thread
  moveToThread(m_original_thread);
}

void QtHostInterface::wakeThread()
{
  if (isOnWorkerThread())
    m_worker_thread_event_loop->quit();
  else
    QMetaObject::invokeMethod(m_worker_thread_event_loop, "quit", Qt::QueuedConnection);
}

QtHostInterface::Thread::Thread(QtHostInterface* parent) : QThread(parent), m_parent(parent) {}

QtHostInterface::Thread::~Thread() = default;

void QtHostInterface::Thread::run()
{
  m_parent->threadEntryPoint();
}
