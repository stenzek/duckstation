#include "qthostinterface.h"
#include "common/assert.h"
#include "common/audio_stream.h"
#include "common/byte_stream.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/string_util.h"
#include "core/controller.h"
#include "core/game_list.h"
#include "core/gpu.h"
#include "core/system.h"
#include "frontend-common/imgui_styles.h"
#include "frontend-common/ini_settings_interface.h"
#include "frontend-common/opengl_host_display.h"
#include "frontend-common/sdl_audio_stream.h"
#include "frontend-common/sdl_controller_interface.h"
#include "frontend-common/vulkan_host_display.h"
#include "imgui.h"
#include "mainwindow.h"
#include "qtdisplaywidget.h"
#include "qtprogresscallback.h"
#include "qtutils.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QTimer>
#include <QtCore/QTranslator>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <algorithm>
#include <memory>
Log_SetChannel(QtHostInterface);

#ifdef WIN32
#include "frontend-common/d3d11_host_display.h"
#endif

QtHostInterface::QtHostInterface(QObject* parent) : QObject(parent), CommonHostInterface()
{
  qRegisterMetaType<SystemBootParameters>();
}

QtHostInterface::~QtHostInterface()
{
  Assert(!m_display);
}

const char* QtHostInterface::GetFrontendName() const
{
  return "DuckStation Qt Frontend";
}

std::vector<std::pair<QString, QString>> QtHostInterface::getAvailableLanguageList()
{
  return {{QStringLiteral("English"), QStringLiteral("")},
          {QStringLiteral("Deutsch"), QStringLiteral("de")},
          {QStringLiteral("Portuguese (Pt)"), QStringLiteral("pt-pt")},
          {QStringLiteral("Portuguese (Br)"), QStringLiteral("pt-br")}};
}

bool QtHostInterface::Initialize()
{
  createThread();
  if (!m_worker_thread->waitForInit())
    return false;

  installTranslator();
  return true;
}

void QtHostInterface::Shutdown()
{
  stopThread();
}

bool QtHostInterface::initializeOnThread()
{
  if (!CommonHostInterface::Initialize())
    return false;

  // make sure the controllers have been detected
  if (m_controller_interface)
    m_controller_interface->PollEvents();

  // bind buttons/axises
  createBackgroundControllerPollTimer();
  startBackgroundControllerPollTimer();
  updateInputMap();
  return true;
}

void QtHostInterface::shutdownOnThread()
{
  destroyBackgroundControllerPollTimer();
  CommonHostInterface::Shutdown();
}

void QtHostInterface::installTranslator()
{
  m_translator = std::make_unique<QTranslator>();

  std::string language = GetStringSettingValue("Main", "Language", "");
  if (language.empty())
    return;

  const QString path =
    QStringLiteral("%1/translations/duckstation-qt_%3.qm").arg(qApp->applicationDirPath()).arg(language.c_str());
  if (!QFile::exists(path))
  {
    QMessageBox::warning(
      nullptr, QStringLiteral("Translation Error"),
      QStringLiteral("Failed to find translation file for language '%1':\n%2").arg(language.c_str()).arg(path));
    return;
  }

  if (!m_translator->load(path))
  {
    QMessageBox::warning(
      nullptr, QStringLiteral("Translation Error"),
      QStringLiteral("Failed to load translation file for language '%1':\n%2").arg(language.c_str()).arg(path));
    return;
  }

  Log_InfoPrintf("Loaded translation file for language '%s'", language.c_str());
  qApp->installTranslator(m_translator.get());
}

void QtHostInterface::ReportError(const char* message)
{
  HostInterface::ReportError(message);

  const bool was_fullscreen = m_is_fullscreen;
  if (was_fullscreen)
    SetFullscreen(false);

  emit errorReported(QString::fromLocal8Bit(message));

  if (was_fullscreen)
    SetFullscreen(true);
}

void QtHostInterface::ReportMessage(const char* message)
{
  HostInterface::ReportMessage(message);

  emit messageReported(QString::fromLocal8Bit(message));
}

bool QtHostInterface::ConfirmMessage(const char* message)
{
  const bool was_fullscreen = m_is_fullscreen;
  if (was_fullscreen)
    SetFullscreen(false);

  const bool result = messageConfirmed(QString::fromLocal8Bit(message));

  if (was_fullscreen)
    SetFullscreen(true);

  return result;
}

bool QtHostInterface::parseCommandLineParameters(int argc, char* argv[],
                                                 std::unique_ptr<SystemBootParameters>* out_boot_params)
{
  return CommonHostInterface::ParseCommandLineParameters(argc, argv, out_boot_params);
}

std::string QtHostInterface::GetStringSettingValue(const char* section, const char* key,
                                                   const char* default_value /*= ""*/)
{
  std::lock_guard<std::recursive_mutex> guard(m_settings_mutex);
  return m_settings_interface->GetStringValue(section, key, default_value);
}

bool QtHostInterface::GetBoolSettingValue(const char* section, const char* key, bool default_value /* = false */)
{
  std::lock_guard<std::recursive_mutex> guard(m_settings_mutex);
  return m_settings_interface->GetBoolValue(section, key, default_value);
}

int QtHostInterface::GetIntSettingValue(const char* section, const char* key, int default_value /* = 0 */)
{
  std::lock_guard<std::recursive_mutex> guard(m_settings_mutex);
  return m_settings_interface->GetIntValue(section, key, default_value);
}

float QtHostInterface::GetFloatSettingValue(const char* section, const char* key, float default_value /* = 0.0f */)
{
  std::lock_guard<std::recursive_mutex> guard(m_settings_mutex);
  return m_settings_interface->GetFloatValue(section, key, default_value);
}

std::vector<std::string> QtHostInterface::GetSettingStringList(const char* section, const char* key)
{
  std::lock_guard<std::recursive_mutex> guard(m_settings_mutex);
  return m_settings_interface->GetStringList(section, key);
}

void QtHostInterface::SetBoolSettingValue(const char* section, const char* key, bool value)
{
  std::lock_guard<std::recursive_mutex> guard(m_settings_mutex);
  m_settings_interface->SetBoolValue(section, key, value);
  queueSettingsSave();
}

void QtHostInterface::SetIntSettingValue(const char* section, const char* key, int value)
{
  std::lock_guard<std::recursive_mutex> guard(m_settings_mutex);
  m_settings_interface->SetIntValue(section, key, value);
  queueSettingsSave();
}

void QtHostInterface::SetFloatSettingValue(const char* section, const char* key, float value)
{
  std::lock_guard<std::recursive_mutex> guard(m_settings_mutex);
  m_settings_interface->SetFloatValue(section, key, value);
  queueSettingsSave();
}

void QtHostInterface::SetStringSettingValue(const char* section, const char* key, const char* value)
{
  std::lock_guard<std::recursive_mutex> guard(m_settings_mutex);
  m_settings_interface->SetStringValue(section, key, value);
  queueSettingsSave();
}

void QtHostInterface::SetStringListSettingValue(const char* section, const char* key,
                                                const std::vector<std::string>& values)
{
  std::lock_guard<std::recursive_mutex> guard(m_settings_mutex);
  m_settings_interface->SetStringList(section, key, values);
  queueSettingsSave();
}

void QtHostInterface::RemoveSettingValue(const char* section, const char* key)
{
  std::lock_guard<std::recursive_mutex> guard(m_settings_mutex);
  m_settings_interface->DeleteValue(section, key);
  queueSettingsSave();
}

void QtHostInterface::queueSettingsSave()
{
  if (!m_settings_save_timer)
  {
    m_settings_save_timer = std::make_unique<QTimer>();
    connect(m_settings_save_timer.get(), &QTimer::timeout, this, &QtHostInterface::doSaveSettings);
    m_settings_save_timer->setSingleShot(true);
  }
  else
  {
    m_settings_save_timer->stop();
  }

  m_settings_save_timer->start(SETTINGS_SAVE_DELAY);
}

void QtHostInterface::doSaveSettings()
{
  std::lock_guard<std::recursive_mutex> guard(m_settings_mutex);
  m_settings_interface->Save();
  m_settings_save_timer.reset();
}

void QtHostInterface::setDefaultSettings()
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "setDefaultSettings", Qt::QueuedConnection);
    return;
  }

  Settings old_settings(std::move(g_settings));
  {
    std::lock_guard<std::recursive_mutex> guard(m_settings_mutex);
    SetDefaultSettings(*m_settings_interface.get());
    m_settings_interface->Save();

    CommonHostInterface::LoadSettings(*m_settings_interface.get());
  }

  CheckForSettingsChanges(old_settings);
}

void QtHostInterface::applySettings()
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "applySettings", Qt::QueuedConnection);
    return;
  }

  Settings old_settings(std::move(g_settings));
  {
    std::lock_guard<std::recursive_mutex> guard(m_settings_mutex);
    CommonHostInterface::LoadSettings(*m_settings_interface.get());
  }

  CheckForSettingsChanges(old_settings);

  // detect when render-to-main flag changes
  if (!System::IsShutdown())
  {
    const bool render_to_main = m_settings_interface->GetBoolValue("Main", "RenderToMainWindow", true);
    if (m_display && !m_is_fullscreen && render_to_main != m_is_rendering_to_main)
    {
      m_is_rendering_to_main = render_to_main;
      updateDisplayState();
    }
    else
    {
      renderDisplay();
    }
  }
}

void QtHostInterface::refreshGameList(bool invalidate_cache /* = false */, bool invalidate_database /* = false */)
{
  Assert(!isOnWorkerThread());

  std::lock_guard<std::recursive_mutex> lock(m_settings_mutex);
  m_game_list->SetSearchDirectoriesFromSettings(*m_settings_interface.get());

  QtProgressCallback progress(m_main_window);
  m_game_list->Refresh(invalidate_cache, invalidate_database, &progress);
  emit gameListRefreshed();
}

void QtHostInterface::setMainWindow(MainWindow* window)
{
  DebugAssert((!m_main_window && window) || (m_main_window && !window));
  m_main_window = window;
}

void QtHostInterface::bootSystem(const SystemBootParameters& params)
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "bootSystem", Qt::QueuedConnection, Q_ARG(const SystemBootParameters&, params));
    return;
  }

  emit emulationStarting();
  BootSystem(params);
}

void QtHostInterface::resumeSystemFromState(const QString& filename, bool boot_on_failure)
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "resumeSystemFromState", Qt::QueuedConnection, Q_ARG(const QString&, filename),
                              Q_ARG(bool, boot_on_failure));
    return;
  }

  emit emulationStarting();
  if (filename.isEmpty())
    ResumeSystemFromMostRecentState();
  else
    ResumeSystemFromState(filename.toStdString().c_str(), boot_on_failure);
}

void QtHostInterface::resumeSystemFromMostRecentState()
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "resumeSystemFromMostRecentState");
    return;
  }

  emit emulationStarting();
  ResumeSystemFromMostRecentState();
}

void QtHostInterface::onDisplayWindowKeyEvent(int key, bool pressed)
{
  DebugAssert(isOnWorkerThread());
  HandleHostKeyEvent(key, pressed);
}

void QtHostInterface::onDisplayWindowMouseMoveEvent(int x, int y)
{
  // display might be null here if the event happened after shutdown
  DebugAssert(isOnWorkerThread());
  if (!m_display)
    return;

  m_display->SetMousePosition(x, y);

  if (ImGui::GetCurrentContext())
  {
    ImGuiIO& io = ImGui::GetIO();
    io.MousePos[0] = static_cast<float>(x);
    io.MousePos[1] = static_cast<float>(y);
  }
}

void QtHostInterface::onDisplayWindowMouseButtonEvent(int button, bool pressed)
{
  DebugAssert(isOnWorkerThread());

  if (ImGui::GetCurrentContext() && (button > 0 && button <= static_cast<int>(countof(ImGuiIO::MouseDown))))
  {
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDown[button - 1] = pressed;

    if (io.WantCaptureMouse)
    {
      // don't consume input events if it's hitting the UI instead
      return;
    }
  }

  HandleHostMouseEvent(button, pressed);
}

void QtHostInterface::onHostDisplayWindowResized(int width, int height)
{
  // this can be null if it was destroyed and the main thread is late catching up
  if (!m_display)
    return;

  m_display->ResizeRenderWindow(width, height);

  // re-render the display, since otherwise it will be out of date and stretched if paused
  if (!System::IsShutdown())
  {
    g_gpu->UpdateResolutionScale();
    renderDisplay();
  }
}

void QtHostInterface::redrawDisplayWindow()
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "redrawDisplayWindow", Qt::QueuedConnection);
    return;
  }

  if (!m_display || System::IsShutdown())
    return;

  renderDisplay();
}

void QtHostInterface::toggleFullscreen()
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "toggleFullscreen", Qt::QueuedConnection);
    return;
  }

  SetFullscreen(!m_is_fullscreen);
}

bool QtHostInterface::AcquireHostDisplay()
{
  Assert(!m_display);

  m_is_rendering_to_main = m_settings_interface->GetBoolValue("Main", "RenderToMainWindow", true);

  QtDisplayWidget* display_widget =
    createDisplayRequested(m_worker_thread, QString::fromStdString(g_settings.gpu_adapter),
                           g_settings.gpu_use_debug_device, m_is_fullscreen, m_is_rendering_to_main);
  if (!display_widget || !m_display->HasRenderDevice())
  {
    emit destroyDisplayRequested();
    m_display = nullptr;
    return false;
  }

  createImGuiContext(display_widget->devicePixelRatioFromScreen());

  if (!m_display->MakeRenderContextCurrent() ||
      !m_display->InitializeRenderDevice(GetShaderCacheBasePath(), g_settings.gpu_use_debug_device))
  {
    destroyImGuiContext();
    m_display->DestroyRenderDevice();
    emit destroyDisplayRequested();
    m_display.reset();
    return false;
  }

  connectDisplaySignals(display_widget);
  ImGui::NewFrame();
  return true;
}

HostDisplay* QtHostInterface::createHostDisplay()
{
  switch (g_settings.gpu_renderer)
  {
    case GPURenderer::HardwareVulkan:
      m_display = std::make_unique<FrontendCommon::VulkanHostDisplay>();
      break;

    case GPURenderer::HardwareOpenGL:
#ifndef WIN32
    default:
#endif
      m_display = std::make_unique<FrontendCommon::OpenGLHostDisplay>();
      break;

#ifdef WIN32
    case GPURenderer::HardwareD3D11:
    default:
      m_display = std::make_unique<FrontendCommon::D3D11HostDisplay>();
      break;
#endif
  }

  return m_display.get();
}

void QtHostInterface::connectDisplaySignals(QtDisplayWidget* widget)
{
  widget->disconnect(this);

  connect(widget, &QtDisplayWidget::windowResizedEvent, this, &QtHostInterface::onHostDisplayWindowResized);
  connect(widget, &QtDisplayWidget::windowRestoredEvent, this, &QtHostInterface::redrawDisplayWindow);
  connect(widget, &QtDisplayWidget::windowClosedEvent, this, &QtHostInterface::powerOffSystem,
          Qt::BlockingQueuedConnection);
  connect(widget, &QtDisplayWidget::windowKeyEvent, this, &QtHostInterface::onDisplayWindowKeyEvent);
  connect(widget, &QtDisplayWidget::windowMouseMoveEvent, this, &QtHostInterface::onDisplayWindowMouseMoveEvent);
  connect(widget, &QtDisplayWidget::windowMouseButtonEvent, this, &QtHostInterface::onDisplayWindowMouseButtonEvent);
}

void QtHostInterface::updateDisplayState()
{
  if (!m_display)
    return;

  // this expects the context to get moved back to us afterwards
  m_display->DoneRenderContextCurrent();

  QtDisplayWidget* display_widget = updateDisplayRequested(m_worker_thread, m_is_fullscreen, m_is_rendering_to_main);
  if (!display_widget || !m_display->MakeRenderContextCurrent())
    Panic("Failed to make device context current after updating");

  connectDisplaySignals(display_widget);

  if (!System::IsShutdown())
  {
    g_gpu->UpdateResolutionScale();
    redrawDisplayWindow();
  }
  UpdateSpeedLimiterState();
}

void QtHostInterface::ReleaseHostDisplay()
{
  Assert(m_display);

  m_display->DestroyRenderDevice();
  destroyImGuiContext();
  emit destroyDisplayRequested();
  m_display.reset();
  m_is_fullscreen = false;
}

bool QtHostInterface::IsFullscreen() const
{
  return m_is_fullscreen;
}

bool QtHostInterface::SetFullscreen(bool enabled)
{
  if (m_is_fullscreen == enabled)
    return true;

  m_is_fullscreen = enabled;
  updateDisplayState();
  return true;
}

void QtHostInterface::PollAndUpdate()
{
  CommonHostInterface::PollAndUpdate();

  if (m_controller_interface)
    m_controller_interface->PollEvents();
}

void QtHostInterface::RequestExit()
{
  emit exitRequested();
}

std::optional<CommonHostInterface::HostKeyCode> QtHostInterface::GetHostKeyCode(const std::string_view key_code) const
{
  const std::optional<int> code =
    QtUtils::ParseKeyString(QString::fromUtf8(key_code.data(), static_cast<int>(key_code.length())));
  if (!code)
    return std::nullopt;

  return static_cast<s32>(*code);
}

void QtHostInterface::OnSystemCreated()
{
  CommonHostInterface::OnSystemCreated();

  wakeThread();
  stopBackgroundControllerPollTimer();

  emit emulationStarted();
  emit emulationPaused(false);
}

void QtHostInterface::OnSystemPaused(bool paused)
{
  CommonHostInterface::OnSystemPaused(paused);

  emit emulationPaused(paused);

  if (!paused)
  {
    wakeThread();
    stopBackgroundControllerPollTimer();
    emit focusDisplayWidgetRequested();
  }
  else
  {
    startBackgroundControllerPollTimer();
  }
}

void QtHostInterface::OnSystemDestroyed()
{
  CommonHostInterface::OnSystemDestroyed();

  startBackgroundControllerPollTimer();
  emit emulationStopped();
}

void QtHostInterface::OnSystemPerformanceCountersUpdated()
{
  HostInterface::OnSystemPerformanceCountersUpdated();

  emit systemPerformanceCountersUpdated(System::GetEmulationSpeed(), System::GetFPS(), System::GetVPS(),
                                        System::GetAverageFrameTime(), System::GetWorstFrameTime());
}

void QtHostInterface::OnRunningGameChanged()
{
  CommonHostInterface::OnRunningGameChanged();

  if (!System::IsShutdown())
  {
    emit runningGameChanged(QString::fromStdString(System::GetRunningPath()),
                            QString::fromStdString(System::GetRunningCode()),
                            QString::fromStdString(System::GetRunningTitle()));
  }
  else
  {
    emit runningGameChanged(QString(), QString(), QString());
  }
}

void QtHostInterface::OnSystemStateSaved(bool global, s32 slot)
{
  emit stateSaved(QString::fromStdString(System::GetRunningCode()), global, slot);
}

void QtHostInterface::LoadSettings()
{
  m_settings_interface = std::make_unique<INISettingsInterface>(CommonHostInterface::GetSettingsFileName());

  CommonHostInterface::CheckSettings(*m_settings_interface.get());
  CommonHostInterface::LoadSettings(*m_settings_interface.get());
}

void QtHostInterface::SetDefaultSettings(SettingsInterface& si)
{
  CommonHostInterface::SetDefaultSettings(si);

  si.SetBoolValue("Main", "RenderToMainWindow", true);
}

void QtHostInterface::UpdateInputMap()
{
  updateInputMap();
}

void QtHostInterface::updateInputMap()
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "updateInputMap", Qt::QueuedConnection);
    return;
  }

  std::lock_guard<std::recursive_mutex> lock(m_settings_mutex);
  CommonHostInterface::UpdateInputMap(*m_settings_interface.get());
}

void QtHostInterface::applyInputProfile(const QString& profile_path)
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "applyInputProfile", Qt::QueuedConnection, Q_ARG(const QString&, profile_path));
    return;
  }

  std::lock_guard<std::recursive_mutex> lock(m_settings_mutex);
  ApplyInputProfile(profile_path.toUtf8().data(), *m_settings_interface.get());
  emit inputProfileLoaded();
}

void QtHostInterface::saveInputProfile(const QString& profile_name)
{
  std::lock_guard<std::recursive_mutex> lock(m_settings_mutex);
  SaveInputProfile(profile_name.toUtf8().data(), *m_settings_interface.get());
}

QString QtHostInterface::getUserDirectoryRelativePath(const QString& arg) const
{
  QString result = QString::fromStdString(m_user_directory);
  result += FS_OSPATH_SEPERATOR_CHARACTER;
  result += arg;
  return result;
}

QString QtHostInterface::getProgramDirectoryRelativePath(const QString& arg) const
{
  QString result = QString::fromStdString(m_program_directory);
  result += FS_OSPATH_SEPERATOR_CHARACTER;
  result += arg;
  return result;
}

QString QtHostInterface::getProgramDirectory() const
{
  return QString::fromStdString(m_program_directory);
}

void QtHostInterface::powerOffSystem()
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "powerOffSystem", Qt::QueuedConnection);
    return;
  }

  if (System::IsShutdown())
    return;

  if (g_settings.save_state_on_exit)
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

  if (System::IsShutdown())
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

  CommonHostInterface::PauseSystem(paused);
}

void QtHostInterface::changeDisc(const QString& new_disc_filename)
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "changeDisc", Qt::QueuedConnection, Q_ARG(const QString&, new_disc_filename));
    return;
  }

  if (System::IsShutdown())
    return;

  if (!new_disc_filename.isEmpty())
    System::InsertMedia(new_disc_filename.toStdString().c_str());
  else
    System::RemoveMedia();
}

static QString FormatTimestampForSaveStateMenu(u64 timestamp)
{
  const QDateTime qtime(QDateTime::fromSecsSinceEpoch(static_cast<qint64>(timestamp)));
  return qtime.toString(QLocale::system().dateTimeFormat(QLocale::ShortFormat));
}

void QtHostInterface::populateSaveStateMenus(const char* game_code, QMenu* load_menu, QMenu* save_menu)
{
  auto add_slot = [this, game_code, load_menu, save_menu](const char* title, const char* empty_title, bool global,
                                                          s32 slot) {
    std::optional<SaveStateInfo> ssi = GetSaveStateInfo(global ? nullptr : game_code, slot);

    const QString menu_title = ssi.has_value() ?
                                 tr(title).arg(slot).arg(FormatTimestampForSaveStateMenu(ssi->timestamp)) :
                                 tr(empty_title).arg(slot);

    QAction* load_action = load_menu->addAction(menu_title);
    load_action->setEnabled(ssi.has_value());
    if (ssi.has_value())
    {
      const QString path(QString::fromStdString(ssi->path));
      connect(load_action, &QAction::triggered, [this, path]() { loadState(path); });
    }

    QAction* save_action = save_menu->addAction(menu_title);
    connect(save_action, &QAction::triggered, [this, global, slot]() { saveState(global, slot); });
  };

  load_menu->clear();
  save_menu->clear();

  if (game_code && std::strlen(game_code) > 0)
  {
    for (u32 slot = 1; slot <= PER_GAME_SAVE_STATE_SLOTS; slot++)
      add_slot("Game Save %1 (%2)", "Game Save %1 (Empty)", false, static_cast<s32>(slot));

    load_menu->addSeparator();
    save_menu->addSeparator();
  }

  for (u32 slot = 1; slot <= GLOBAL_SAVE_STATE_SLOTS; slot++)
    add_slot("Global Save %1 (%2)", "Global Save %1 (Empty)", true, static_cast<s32>(slot));
}

void QtHostInterface::populateGameListContextMenu(const char* game_code, QWidget* parent_window, QMenu* menu)
{
  QAction* resume_action = menu->addAction(tr("Resume"));
  resume_action->setEnabled(false);

  QMenu* load_state_menu = menu->addMenu(tr("Load State"));
  load_state_menu->setEnabled(false);

  const std::vector<SaveStateInfo> available_states(GetAvailableSaveStates(game_code));
  const QString timestamp_format = QLocale::system().dateTimeFormat(QLocale::ShortFormat);
  for (const SaveStateInfo& ssi : available_states)
  {
    if (ssi.global)
      continue;

    const s32 slot = ssi.slot;
    const QDateTime timestamp(QDateTime::fromSecsSinceEpoch(static_cast<qint64>(ssi.timestamp)));
    const QString timestamp_str(timestamp.toString(timestamp_format));
    const QString path(QString::fromStdString(ssi.path));

    QAction* action;
    if (slot < 0)
    {
      resume_action->setText(tr("Resume (%1)").arg(timestamp_str));
      resume_action->setEnabled(true);
      action = resume_action;
    }
    else
    {
      load_state_menu->setEnabled(true);
      action = load_state_menu->addAction(tr("%1 Save %2 (%3)").arg(tr("Game")).arg(slot).arg(timestamp_str));
    }

    connect(action, &QAction::triggered, [this, path]() { loadState(path); });
  }

  const bool has_any_states = resume_action->isEnabled() || load_state_menu->isEnabled();
  QAction* delete_save_states_action = menu->addAction(tr("Delete Save States..."));
  delete_save_states_action->setEnabled(has_any_states);
  if (has_any_states)
  {
    const std::string game_code_copy(game_code);
    connect(delete_save_states_action, &QAction::triggered, [this, parent_window, game_code_copy] {
      if (QMessageBox::warning(
            parent_window, tr("Confirm Save State Deletion"),
            tr("Are you sure you want to delete all save states for %1?\n\nThe saves will not be recoverable.")
              .arg(game_code_copy.c_str()),
            QMessageBox::Yes, QMessageBox::No) != QMessageBox::Yes)
      {
        return;
      }

      DeleteSaveStates(game_code_copy.c_str(), true);
    });
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

  if (!System::IsShutdown())
    SaveState(global, slot);
}

void QtHostInterface::setAudioOutputVolume(int value)
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "setAudioOutputVolume", Q_ARG(int, value));
    return;
  }

  if (m_audio_stream)
    m_audio_stream->SetOutputVolume(value);

  g_settings.audio_output_volume = value;
}

void QtHostInterface::setAudioOutputMuted(bool muted)
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "setAudioOutputMuted", Q_ARG(bool, muted));
    return;
  }

  if (m_audio_stream)
    m_audio_stream->SetOutputVolume(muted ? 0 : g_settings.audio_output_volume);

  g_settings.audio_output_muted = muted;
}

void QtHostInterface::startDumpingAudio()
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "startDumpingAudio");
    return;
  }

  StartDumpingAudio();
}

void QtHostInterface::stopDumpingAudio()
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "stopDumpingAudio");
    return;
  }

  StopDumpingAudio();
}

void QtHostInterface::saveScreenshot()
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "saveScreenshot");
    return;
  }

  SaveScreenshot(nullptr, true, true);
}

void QtHostInterface::doBackgroundControllerPoll()
{
  PollAndUpdate();
}

void QtHostInterface::createBackgroundControllerPollTimer()
{
  DebugAssert(!m_background_controller_polling_timer);
  m_background_controller_polling_timer = new QTimer(this);
  m_background_controller_polling_timer->setSingleShot(false);
  m_background_controller_polling_timer->setTimerType(Qt::CoarseTimer);
  connect(m_background_controller_polling_timer, &QTimer::timeout, this, &QtHostInterface::doBackgroundControllerPoll);
}

void QtHostInterface::destroyBackgroundControllerPollTimer()
{
  delete m_background_controller_polling_timer;
  m_background_controller_polling_timer = nullptr;
}

void QtHostInterface::startBackgroundControllerPollTimer()
{
  if (m_background_controller_polling_timer->isActive() || !m_controller_interface)
    return;

  m_background_controller_polling_timer->start(BACKGROUND_CONTROLLER_POLLING_INTERVAL);
}

void QtHostInterface::stopBackgroundControllerPollTimer()
{
  if (!m_background_controller_polling_timer->isActive() || !m_controller_interface)
    return;

  m_background_controller_polling_timer->stop();
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
  m_worker_thread->setInitResult(initializeOnThread());

  // TODO: Event which flags the thread as ready
  while (!m_shutdown_flag.load())
  {
    if (!System::IsRunning())
    {
      // wait until we have a system before running
      m_worker_thread_event_loop->exec();
      continue;
    }

    System::RunFrame();
    UpdateControllerRumble();
    if (m_frame_step_request)
    {
      m_frame_step_request = false;
      PauseSystem(true);
    }

    renderDisplay();

    System::UpdatePerformanceCounters();

    if (m_speed_limiter_enabled)
      System::Throttle();

    m_worker_thread_event_loop->processEvents(QEventLoop::AllEvents);
    PollAndUpdate();
  }

  shutdownOnThread();

  delete m_worker_thread_event_loop;
  m_worker_thread_event_loop = nullptr;

  // move back to UI thread
  moveToThread(m_original_thread);
}

void QtHostInterface::renderDisplay()
{
  DrawImGuiWindows();

  m_display->Render();
  ImGui::NewFrame();
}

void QtHostInterface::wakeThread()
{
  if (isOnWorkerThread())
    m_worker_thread_event_loop->quit();
  else
    QMetaObject::invokeMethod(m_worker_thread_event_loop, "quit", Qt::QueuedConnection);
}

void QtHostInterface::createImGuiContext(float framebuffer_scale)
{
  ImGui::CreateContext();

  auto& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.DisplayFramebufferScale.x = framebuffer_scale;
  io.DisplayFramebufferScale.y = framebuffer_scale;
  ImGui::GetStyle().ScaleAllSizes(framebuffer_scale);

  ImGui::StyleColorsDarker();
  ImGui::AddRobotoRegularFont(15.0f * framebuffer_scale);
}

void QtHostInterface::destroyImGuiContext()
{
  ImGui::DestroyContext();
}

QtHostInterface::Thread::Thread(QtHostInterface* parent) : QThread(parent), m_parent(parent) {}

QtHostInterface::Thread::~Thread() = default;

void QtHostInterface::Thread::run()
{
  m_parent->threadEntryPoint();
}

void QtHostInterface::Thread::setInitResult(bool result)
{
  m_init_result.store(result);
  m_init_event.Signal();
}

bool QtHostInterface::Thread::waitForInit()
{
  while (!m_init_event.TryWait(100))
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

  return m_init_result.load();
}
