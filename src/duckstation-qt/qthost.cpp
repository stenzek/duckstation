// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "qthost.h"
#include "autoupdaterdialog.h"
#include "displaywidget.h"
#include "logwindow.h"
#include "mainwindow.h"
#include "qtprogresscallback.h"
#include "qtutils.h"
#include "setupwizarddialog.h"

#include "core/achievements.h"
#include "core/bus.h"
#include "core/cheats.h"
#include "core/controller.h"
#include "core/fullscreen_ui.h"
#include "core/game_database.h"
#include "core/game_list.h"
#include "core/gdb_server.h"
#include "core/gpu.h"
#include "core/gpu_backend.h"
#include "core/gpu_hw_texture_cache.h"
#include "core/gpu_thread.h"
#include "core/host.h"
#include "core/imgui_overlays.h"
#include "core/memory_card.h"
#include "core/performance_counters.h"
#include "core/spu.h"
#include "core/system.h"
#include "core/system_private.h"

#include "common/assert.h"
#include "common/crash_handler.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/minizip_helpers.h"
#include "common/path.h"
#include "common/scoped_guard.h"
#include "common/string_util.h"
#include "common/threading.h"

#include "util/audio_stream.h"
#include "util/http_downloader.h"
#include "util/imgui_fullscreen.h"
#include "util/imgui_manager.h"
#include "util/ini_settings_interface.h"
#include "util/input_manager.h"
#include "util/platform_misc.h"
#include "util/postprocessing.h"

#include "scmversion/scmversion.h"

#include "fmt/format.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QTimer>
#include <QtGui/QClipboard>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>

LOG_CHANNEL(Host);

static constexpr u32 SETTINGS_VERSION = 3;
static constexpr u32 SETTINGS_SAVE_DELAY = 1000;

/// Interval at which the controllers are polled when the system is not active.
static constexpr u32 BACKGROUND_CONTROLLER_POLLING_INTERVAL = 100;

/// Poll at half the vsync rate for FSUI to reduce the chance of getting a press+release in the same frame.
static constexpr u32 FULLSCREEN_UI_CONTROLLER_POLLING_INTERVAL = 8;

/// Poll at 1ms when running GDB server. We can get rid of this once we move networking to its own thread.
static constexpr u32 GDB_SERVER_POLLING_INTERVAL = 1;

//////////////////////////////////////////////////////////////////////////
// Local function declarations
//////////////////////////////////////////////////////////////////////////
namespace QtHost {

namespace {

class GPUThread : public QThread
{
public:
  GPUThread(QObject* parent = nullptr);
  ~GPUThread() override;

  void run() override;
};

} // namespace

static bool PerformEarlyHardwareChecks();
static bool EarlyProcessStartup();
static void RegisterTypes();
static bool InitializeConfig(std::string settings_filename);
static void SetAppRoot();
static void SetResourcesDirectory();
static bool SetDataDirectory();
static bool SetCriticalFolders();
static void SetDefaultSettings(SettingsInterface& si, bool system, bool controller);
static void MigrateSettings();
static void SaveSettings();
static bool RunSetupWizard();
static std::optional<bool> DownloadFile(QWidget* parent, const QString& title, std::string url, std::vector<u8>* data);
static void InitializeEarlyConsole();
static void HookSignals();
static void PrintCommandLineVersion();
static void PrintCommandLineHelp(const char* progname);
static bool ParseCommandLineParametersAndInitializeConfig(QApplication& app,
                                                          std::shared_ptr<SystemBootParameters>& boot_params);
} // namespace QtHost

static std::unique_ptr<INISettingsInterface> s_base_settings_interface;
static std::unique_ptr<QTimer> s_settings_save_timer;
static bool s_batch_mode = false;
static bool s_nogui_mode = false;
static bool s_start_fullscreen_ui = false;
static bool s_start_fullscreen_ui_fullscreen = false;
static bool s_run_setup_wizard = false;
static bool s_cleanup_after_update = false;

EmuThread* g_emu_thread = nullptr;

EmuThread::EmuThread(QThread* ui_thread) : QThread(), m_ui_thread(ui_thread)
{
}

EmuThread::~EmuThread() = default;

void QtHost::RegisterTypes()
{
  // Register any standard types we need elsewhere
  qRegisterMetaType<std::optional<WindowInfo>>("std::optional<WindowInfo>()");
  qRegisterMetaType<std::optional<bool>>();
  qRegisterMetaType<std::function<void()>>("std::function<void()>");
  qRegisterMetaType<std::shared_ptr<SystemBootParameters>>();
  qRegisterMetaType<const GameList::Entry*>();
  qRegisterMetaType<RenderAPI>("RenderAPI");
  qRegisterMetaType<GPURenderer>("GPURenderer");
  qRegisterMetaType<InputBindingKey>("InputBindingKey");
  qRegisterMetaType<std::string>("std::string");
  qRegisterMetaType<std::vector<std::pair<std::string, std::string>>>(
    "std::vector<std::pair<std::string, std::string>>");
}

bool QtHost::PerformEarlyHardwareChecks()
{
  Error error;
  const bool okay = System::PerformEarlyHardwareChecks(&error);
  if (okay && !error.IsValid()) [[likely]]
    return true;

  if (okay)
  {
    QMessageBox::warning(nullptr, QStringLiteral("Hardware Check Warning"),
                         QString::fromStdString(error.GetDescription()));
  }
  else
  {
    QMessageBox::critical(nullptr, QStringLiteral("Hardware Check Failed"),
                          QString::fromStdString(error.GetDescription()));
  }

  return okay;
}

bool QtHost::EarlyProcessStartup()
{
  // Config-based RAIntegration switch must happen before the main window is displayed.
#ifdef ENABLE_RAINTEGRATION
  if (!Achievements::IsUsingRAIntegration() && Host::GetBaseBoolSettingValue("Cheevos", "UseRAIntegration", false))
    Achievements::SwitchToRAIntegration();
#endif

  Error error;
  if (System::ProcessStartup(&error)) [[likely]]
    return true;

  QMessageBox::critical(nullptr, QStringLiteral("Process Startup Failed"),
                        QString::fromStdString(error.GetDescription()));
  return false;
}

bool QtHost::InBatchMode()
{
  return s_batch_mode;
}

bool QtHost::InNoGUIMode()
{
  return s_nogui_mode;
}

QString QtHost::GetAppNameAndVersion()
{
  return QStringLiteral("DuckStation %1").arg(QLatin1StringView(g_scm_tag_str));
}

QString QtHost::GetAppConfigSuffix()
{
#if defined(_DEVEL)
  return QStringLiteral(" [Devel]");
#elif defined(_DEBUGFAST)
  return QStringLiteral(" [DebugFast]");
#elif defined(_DEBUG)
  return QStringLiteral(" [Debug]");
#else
  return QString();
#endif
}

QString QtHost::GetResourcesBasePath()
{
  return QString::fromStdString(EmuFolders::Resources);
}

INISettingsInterface* QtHost::GetBaseSettingsInterface()
{
  return s_base_settings_interface.get();
}

bool QtHost::SaveGameSettings(SettingsInterface* sif, bool delete_if_empty)
{
  INISettingsInterface* ini = static_cast<INISettingsInterface*>(sif);
  Error error;

  // if there's no keys, just toss the whole thing out
  if (delete_if_empty && ini->IsEmpty())
  {
    INFO_LOG("Removing empty gamesettings ini {}", Path::GetFileName(ini->GetFileName()));

    // grab the settings lock while we're writing the file, that way the CPU thread doesn't try
    // to read it at the same time.
    const auto lock = Host::GetSettingsLock();

    if (FileSystem::FileExists(ini->GetFileName().c_str()) &&
        !FileSystem::DeleteFile(ini->GetFileName().c_str(), &error))
    {
      Host::ReportErrorAsync(
        TRANSLATE_SV("QtHost", "Error"),
        fmt::format(TRANSLATE_FS("QtHost", "An error occurred while deleting empty game settings:\n{}"),
                    error.GetDescription()));
      return false;
    }

    return true;
  }

  // clean unused sections, stops the file being bloated
  sif->RemoveEmptySections();

  // see above
  const auto lock = Host::GetSettingsLock();

  if (!sif->Save(&error))
  {
    Host::ReportErrorAsync(
      TRANSLATE_SV("QtHost", "Error"),
      fmt::format(TRANSLATE_FS("QtHost", "An error occurred while saving game settings:\n{}"), error.GetDescription()));
    return false;
  }

  return true;
}

const QIcon& QtHost::GetAppIcon()
{
  static QIcon icon = QIcon(QStringLiteral(":/icons/duck.png"));
  return icon;
}

std::optional<bool> QtHost::DownloadFile(QWidget* parent, const QString& title, std::string url, std::vector<u8>* data)
{
  static constexpr u32 HTTP_POLL_INTERVAL = 10;

  Error error;
  std::unique_ptr<HTTPDownloader> http = HTTPDownloader::Create(Host::GetHTTPUserAgent(), &error);
  if (!http)
  {
    QMessageBox::critical(parent, qApp->translate("QtHost", "Error"),
                          qApp->translate("QtHost", "Failed to create HTTPDownloader:\n%1")
                            .arg(QString::fromStdString(error.GetDescription())));
    return false;
  }

  std::optional<bool> download_result;
  const std::string::size_type url_file_part_pos = url.rfind('/');
  QtModalProgressCallback progress(parent);
  progress.GetDialog().setLabelText(qApp->translate("QtHost", "Downloading %1...")
                                      .arg(QtUtils::StringViewToQString(std::string_view(url).substr(
                                        (url_file_part_pos >= 0) ? (url_file_part_pos + 1) : 0))));
  progress.GetDialog().setWindowTitle(title);
  progress.GetDialog().setWindowIcon(GetAppIcon());
  progress.SetCancellable(true);

  http->CreateRequest(
    std::move(url),
    [parent, data, &download_result](s32 status_code, const Error& error, const std::string&, std::vector<u8> hdata) {
      if (status_code == HTTPDownloader::HTTP_STATUS_CANCELLED)
        return;

      if (status_code != HTTPDownloader::HTTP_STATUS_OK)
      {
        QMessageBox::critical(parent, qApp->translate("QtHost", "Error"),
                              qApp->translate("QtHost", "Download failed with HTTP status code %1:\n%2")
                                .arg(status_code)
                                .arg(QString::fromStdString(error.GetDescription())));
        download_result = false;
        return;
      }

      if (hdata.empty())
      {
        QMessageBox::critical(parent, qApp->translate("QtHost", "Error"),
                              qApp->translate("QtHost", "Download failed: Data is empty.").arg(status_code));

        download_result = false;
        return;
      }

      *data = std::move(hdata);
      download_result = true;
    },
    &progress);

  // Block until completion.
  QtUtils::ProcessEventsWithSleep(
    QEventLoop::AllEvents,
    [http = http.get()]() {
      http->PollRequests();
      return http->HasAnyRequests();
    },
    HTTP_POLL_INTERVAL);

  return download_result;
}

bool QtHost::DownloadFile(QWidget* parent, const QString& title, std::string url, const char* path)
{
  INFO_LOG("Download from {}, saving to {}.", url, path);

  std::vector<u8> data;
  if (!DownloadFile(parent, title, std::move(url), &data).value_or(false) || data.empty())
    return false;

  // Directory may not exist. Create it.
  const std::string directory(Path::GetDirectory(path));
  if ((!directory.empty() && !FileSystem::DirectoryExists(directory.c_str()) &&
       !FileSystem::CreateDirectory(directory.c_str(), true)) ||
      !FileSystem::WriteBinaryFile(path, data.data(), data.size()))
  {
    QMessageBox::critical(parent, qApp->translate("QtHost", "Error"),
                          qApp->translate("QtHost", "Failed to write '%1'.").arg(QString::fromUtf8(path)));
    return false;
  }

  return true;
}

bool QtHost::DownloadFileFromZip(QWidget* parent, const QString& title, std::string url, const char* zip_filename,
                                 const char* output_path)
{
  INFO_LOG("Download {} from {}, saving to {}.", zip_filename, url, output_path);

  std::vector<u8> data;
  if (!DownloadFile(parent, title, std::move(url), &data).value_or(false) || data.empty())
    return false;

  const unzFile zf = MinizipHelpers::OpenUnzMemoryFile(data.data(), data.size());
  if (!zf)
  {
    QMessageBox::critical(parent, qApp->translate("QtHost", "Error"),
                          qApp->translate("QtHost", "Failed to open downloaded zip file."));
    return false;
  }

  const ScopedGuard zf_guard = [&zf]() { unzClose(zf); };

  if (unzLocateFile(zf, zip_filename, 0) != UNZ_OK || unzOpenCurrentFile(zf) != UNZ_OK)
  {
    QMessageBox::critical(
      parent, qApp->translate("QtHost", "Error"),
      qApp->translate("QtHost", "Failed to locate '%1' in zip.").arg(QString::fromUtf8(zip_filename)));
    return false;
  }

  // Directory may not exist. Create it.
  Error error;
  FileSystem::ManagedCFilePtr output_file;
  const std::string directory(Path::GetDirectory(output_path));
  if ((!directory.empty() && !FileSystem::DirectoryExists(directory.c_str()) &&
       !FileSystem::CreateDirectory(directory.c_str(), true)) ||
      !(output_file = FileSystem::OpenManagedCFile(output_path, "wb", &error)))
  {
    QMessageBox::critical(parent, qApp->translate("QtHost", "Error"),
                          qApp->translate("QtHost", "Failed to open '%1': %2.")
                            .arg(QString::fromUtf8(output_path))
                            .arg(QString::fromStdString(error.GetDescription())));
    return false;
  }

  static constexpr size_t CHUNK_SIZE = 4096;
  char chunk[CHUNK_SIZE];
  for (;;)
  {
    int size = unzReadCurrentFile(zf, chunk, CHUNK_SIZE);
    if (size < 0)
    {
      QMessageBox::critical(
        parent, qApp->translate("QtHost", "Error"),
        qApp->translate("QtHost", "Failed to read '%1' from zip.").arg(QString::fromUtf8(zip_filename)));
      output_file.reset();
      FileSystem::DeleteFile(output_path);
      return false;
    }
    else if (size == 0)
    {
      break;
    }

    if (std::fwrite(chunk, size, 1, output_file.get()) != 1)
    {
      QMessageBox::critical(parent, qApp->translate("QtHost", "Error"),
                            qApp->translate("QtHost", "Failed to write to '%1'.").arg(QString::fromUtf8(output_path)));

      output_file.reset();
      FileSystem::DeleteFile(output_path);
      return false;
    }
  }

  return true;
}

bool QtHost::InitializeConfig(std::string settings_filename)
{
  if (!SetCriticalFolders())
    return false;

  if (settings_filename.empty())
    settings_filename = Path::Combine(EmuFolders::DataRoot, "settings.ini");

  const bool settings_exists = FileSystem::FileExists(settings_filename.c_str());
  INFO_LOG("Loading config from {}.", settings_filename);
  s_base_settings_interface = std::make_unique<INISettingsInterface>(std::move(settings_filename));
  Host::Internal::SetBaseSettingsLayer(s_base_settings_interface.get());

  uint settings_version;
  if (!settings_exists || !s_base_settings_interface->Load() ||
      !s_base_settings_interface->GetUIntValue("Main", "SettingsVersion", &settings_version) ||
      settings_version != SETTINGS_VERSION)
  {
    if (s_base_settings_interface->ContainsValue("Main", "SettingsVersion"))
    {
      // NOTE: No point translating this, because there's no config loaded, so no language loaded.
      Host::ReportErrorAsync("Error", fmt::format("Settings version {} does not match expected version {}, resetting.",
                                                  settings_version, SETTINGS_VERSION));
    }

    s_base_settings_interface->SetUIntValue("Main", "SettingsVersion", SETTINGS_VERSION);
    SetDefaultSettings(*s_base_settings_interface, true, true);

    // Flag for running the setup wizard if this is our first run. We want to run it next time if they don't finish it.
    s_base_settings_interface->SetBoolValue("Main", "SetupWizardIncomplete", true);

    // Make sure we can actually save the config, and the user doesn't have some permission issue.
    Error error;
    if (!s_base_settings_interface->Save(&error))
    {
      QMessageBox::critical(
        nullptr, QStringLiteral("DuckStation"),
        QStringLiteral(
          "Failed to save configuration to\n\n%1\n\nThe error was: %2\n\nPlease ensure this directory is writable. You "
          "can also try portable mode by creating portable.txt in the same directory you installed DuckStation into.")
          .arg(QString::fromStdString(s_base_settings_interface->GetFileName()))
          .arg(QString::fromStdString(error.GetDescription())));
      return false;
    }
  }

  // Setup wizard was incomplete last time?
  s_run_setup_wizard =
    s_run_setup_wizard || s_base_settings_interface->GetBoolValue("Main", "SetupWizardIncomplete", false);

  EmuFolders::LoadConfig(*s_base_settings_interface.get());
  EmuFolders::EnsureFoldersExist();
  MigrateSettings();

  // We need to create the console window early, otherwise it appears in front of the main window.
  if (!Log::IsConsoleOutputEnabled() && s_base_settings_interface->GetBoolValue("Logging", "LogToConsole", false))
    Log::SetConsoleOutputParams(true, s_base_settings_interface->GetBoolValue("Logging", "LogTimestamps", true));

  UpdateApplicationLanguage(nullptr);
  return true;
}

bool QtHost::SetCriticalFolders()
{
  SetAppRoot();
  SetResourcesDirectory();
  if (!SetDataDirectory())
    return false;

  // logging of directories in case something goes wrong super early
  DEV_LOG("AppRoot Directory: {}", EmuFolders::AppRoot);
  DEV_LOG("DataRoot Directory: {}", EmuFolders::DataRoot);
  DEV_LOG("Resources Directory: {}", EmuFolders::Resources);

  // Write crash dumps to the data directory, since that'll be accessible for certain.
  CrashHandler::SetWriteDirectory(EmuFolders::DataRoot);

  // the resources directory should exist, bail out if not
  const std::string rcc_path = Path::Combine(EmuFolders::Resources, "duckstation-qt.rcc");
  if (!FileSystem::FileExists(rcc_path.c_str()) || !QResource::registerResource(QString::fromStdString(rcc_path)) ||
      !FileSystem::DirectoryExists(EmuFolders::Resources.c_str()))
  {
    QMessageBox::critical(nullptr, QStringLiteral("Error"),
                          QStringLiteral("Resources are missing, your installation is incomplete."));
    return false;
  }

  return true;
}

void QtHost::SetAppRoot()
{
  const std::string program_path = FileSystem::GetProgramPath();
  INFO_LOG("Program Path: {}", program_path.c_str());

  EmuFolders::AppRoot = Path::Canonicalize(Path::GetDirectory(program_path));
}

void QtHost::SetResourcesDirectory()
{
#ifndef __APPLE__
  // On Windows/Linux, these are in the binary directory.
  EmuFolders::Resources = Path::Combine(EmuFolders::AppRoot, "resources");
#else
  // On macOS, this is in the bundle resources directory.
  EmuFolders::Resources = Path::Canonicalize(Path::Combine(EmuFolders::AppRoot, "../Resources"));
#endif
}

bool QtHost::SetDataDirectory()
{
  // Already set, e.g. by -portable.
  if (EmuFolders::DataRoot.empty())
    EmuFolders::DataRoot = Host::Internal::ComputeDataDirectory();

  // make sure it exists
  if (!FileSystem::DirectoryExists(EmuFolders::DataRoot.c_str()))
  {
    // we're in trouble if we fail to create this directory... but try to hobble on with portable
    Error error;
    if (!FileSystem::EnsureDirectoryExists(EmuFolders::DataRoot.c_str(), false, &error))
    {
      // no point translating, config isn't loaded
      QMessageBox::critical(
        nullptr, QStringLiteral("DuckStation"),
        QStringLiteral("Failed to create data directory at path\n\n%1\n\nThe error was: %2\nPlease ensure this "
                       "directory is writable. You can also try portable mode by creating portable.txt in the same "
                       "directory you installed DuckStation into.")
          .arg(QString::fromStdString(EmuFolders::DataRoot))
          .arg(QString::fromStdString(error.GetDescription())));
      return false;
    }
  }

  return true;
}

void Host::LoadSettings(const SettingsInterface& si, std::unique_lock<std::mutex>& lock)
{
}

void EmuThread::checkForSettingsChanges(const Settings& old_settings)
{
  if (g_main_window)
    QMetaObject::invokeMethod(g_main_window, &MainWindow::checkForSettingChanges, Qt::QueuedConnection);

  // don't mess with fullscreen while locked
  if (!QtHost::IsSystemLocked())
  {
    const bool render_to_main = shouldRenderToMain();
    if (m_is_rendering_to_main != render_to_main && !m_is_fullscreen)
    {
      m_is_rendering_to_main = render_to_main;
      if (g_gpu_device)
        GPUThread::UpdateDisplayWindow(m_is_fullscreen);
    }
  }
}

void Host::CheckForSettingsChanges(const Settings& old_settings)
{
  g_emu_thread->checkForSettingsChanges(old_settings);
}

void EmuThread::setDefaultSettings(bool system /* = true */, bool controller /* = true */)
{
  if (isCurrentThread())
  {
    QMetaObject::invokeMethod(this, "setDefaultSettings", Qt::QueuedConnection);
    return;
  }

  auto lock = Host::GetSettingsLock();
  QtHost::SetDefaultSettings(*s_base_settings_interface, system, controller);
  QtHost::QueueSettingsSave();

  applySettings(false);

  if (system)
    emit settingsResetToDefault(system, controller);
}

void QtHost::SetDefaultSettings(SettingsInterface& si, bool system, bool controller)
{
  if (system)
  {
    System::SetDefaultSettings(si);
    EmuFolders::SetDefaults();
    EmuFolders::Save(si);
  }

  if (controller)
  {
    InputManager::SetDefaultSourceConfig(si);
    Settings::SetDefaultControllerConfig(si);
    Settings::SetDefaultHotkeyConfig(si);
  }
}

void QtHost::MigrateSettings()
{
  SmallString value;
  if (s_base_settings_interface->GetStringValue("Display", "SyncMode", &value))
  {
    s_base_settings_interface->SetBoolValue("Display", "VSync", (value == "VSync" || value == "VSyncRelaxed"));
    s_base_settings_interface->SetBoolValue(
      "Display", "OptimalFramePacing",
      (value == "VRR" || s_base_settings_interface->GetBoolValue("Display", "DisplayAllFrames", false)));
    s_base_settings_interface->DeleteValue("Display", "SyncMode");
    s_base_settings_interface->DeleteValue("Display", "DisplayAllFrames");
    s_base_settings_interface->Save();
  }
}

bool EmuThread::shouldRenderToMain() const
{
  return !Host::GetBoolSettingValue("Main", "RenderToSeparateWindow", false) && !QtHost::InNoGUIMode();
}

void Host::RequestResizeHostDisplay(s32 new_window_width, s32 new_window_height)
{
  if (g_emu_thread->isFullscreen())
    return;

  emit g_emu_thread->onResizeRenderWindowRequested(new_window_width, new_window_height);
}

void EmuThread::applySettings(bool display_osd_messages /* = false */)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, "applySettings", Qt::QueuedConnection, Q_ARG(bool, display_osd_messages));
    return;
  }

  System::ApplySettings(display_osd_messages);
}

void EmuThread::reloadGameSettings(bool display_osd_messages /* = false */)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, "reloadGameSettings", Qt::QueuedConnection, Q_ARG(bool, display_osd_messages));
    return;
  }

  System::ReloadGameSettings(display_osd_messages);
}

void EmuThread::reloadInputProfile(bool display_osd_messages /*= false*/)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, "reloadInputProfile", Qt::QueuedConnection, Q_ARG(bool, display_osd_messages));
    return;
  }

  System::ReloadInputProfile(display_osd_messages);
}

void EmuThread::updateEmuFolders()
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &EmuThread::updateEmuFolders, Qt::QueuedConnection);
    return;
  }

  EmuFolders::Update();
}

void EmuThread::updateControllerSettings()
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &EmuThread::updateControllerSettings, Qt::QueuedConnection);
    return;
  }

  if (!System::IsValid())
    return;

  System::UpdateControllerSettings();
}

void EmuThread::startFullscreenUI()
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &EmuThread::startFullscreenUI, Qt::QueuedConnection);
    return;
  }

  if (System::IsValid() || m_is_fullscreen_ui_started)
    return;

  // we want settings loaded so we choose the correct renderer
  // this also sorts out input sources.
  System::LoadSettings(false);
  m_is_rendering_to_main = shouldRenderToMain();

  // borrow the game start fullscreen flag
  const bool start_fullscreen =
    (s_start_fullscreen_ui_fullscreen || Host::GetBaseBoolSettingValue("Main", "StartFullscreen", false));

  Error error;
  if (!GPUThread::StartFullscreenUI(start_fullscreen, &error))
  {
    Host::ReportErrorAsync("Error", error.GetDescription());
    return;
  }
}

void EmuThread::stopFullscreenUI()
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &EmuThread::stopFullscreenUI, Qt::QueuedConnection);

    // wait until the host display is gone
    QtUtils::ProcessEventsWithSleep(QEventLoop::ExcludeUserInputEvents,
                                    []() { return (!QtHost::IsSystemValid() && g_gpu_device); });
    return;
  }

  if (m_is_fullscreen_ui_started)
    GPUThread::StopFullscreenUI();
}

void EmuThread::bootSystem(std::shared_ptr<SystemBootParameters> params)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, "bootSystem", Qt::QueuedConnection,
                              Q_ARG(std::shared_ptr<SystemBootParameters>, std::move(params)));
    return;
  }

  // Just in case of rapid clicking games before it gets the chance to start.
  if (System::IsValidOrInitializing())
    return;

  m_is_rendering_to_main = shouldRenderToMain();

  Error error;
  if (!System::BootSystem(std::move(*params), &error))
  {
    emit errorReported(tr("Error"),
                       tr("Failed to boot system: %1").arg(QString::fromStdString(error.GetDescription())));
  }
}

void EmuThread::bootOrLoadState(std::string path)
{
  DebugAssert(isCurrentThread());

  if (System::IsValid())
  {
    Error error;
    if (!System::LoadState(path.c_str(), &error, true))
    {
      emit errorReported(tr("Error"),
                         tr("Failed to load state: %1").arg(QString::fromStdString(error.GetDescription())));
    }
  }
  else
  {
    std::shared_ptr<SystemBootParameters> params = std::make_shared<SystemBootParameters>();
    params->save_state = std::move(path);
    bootSystem(std::move(params));
  }
}

void EmuThread::resumeSystemFromMostRecentState()
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &EmuThread::resumeSystemFromMostRecentState, Qt::QueuedConnection);
    return;
  }

  // shouldn't be doing this with a system running
  if (System::IsValid())
    return;

  std::string state_filename(System::GetMostRecentResumeSaveStatePath());
  if (state_filename.empty())
  {
    emit errorReported(tr("Error"), tr("No resume save state found."));
    return;
  }

  bootOrLoadState(std::move(state_filename));
}

void EmuThread::onDisplayWindowKeyEvent(int key, bool pressed)
{
  DebugAssert(isCurrentThread());

  InputManager::InvokeEvents(InputManager::MakeHostKeyboardKey(key), static_cast<float>(pressed),
                             GenericInputBinding::Unknown);
}

void EmuThread::onDisplayWindowTextEntered(const QString& text)
{
  DebugAssert(isCurrentThread());

  ImGuiManager::AddTextInput(text.toStdString());
}

void EmuThread::onDisplayWindowMouseButtonEvent(int button, bool pressed)
{
  DebugAssert(isCurrentThread());

  InputManager::InvokeEvents(InputManager::MakePointerButtonKey(0, button), static_cast<float>(pressed),
                             GenericInputBinding::Unknown);
}

void EmuThread::onDisplayWindowMouseWheelEvent(const QPoint& delta_angle)
{
  DebugAssert(isCurrentThread());

  const float dx = std::clamp(static_cast<float>(delta_angle.x()) / QtUtils::MOUSE_WHEEL_DELTA, -1.0f, 1.0f);
  if (dx != 0.0f)
    InputManager::UpdatePointerRelativeDelta(0, InputPointerAxis::WheelX, dx);

  const float dy = std::clamp(static_cast<float>(delta_angle.y()) / QtUtils::MOUSE_WHEEL_DELTA, -1.0f, 1.0f);
  if (dy != 0.0f)
    InputManager::UpdatePointerRelativeDelta(0, InputPointerAxis::WheelY, dy);
}

void EmuThread::onDisplayWindowResized(int width, int height, float scale)
{
  GPUThread::ResizeDisplayWindow(width, height, scale);
}

void EmuThread::redrawDisplayWindow()
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, "redrawDisplayWindow", Qt::QueuedConnection);
    return;
  }

  if (System::IsShutdown())
    return;

  GPUThread::PresentCurrentFrame();
}

void EmuThread::toggleFullscreen()
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, "toggleFullscreen", Qt::QueuedConnection);
    return;
  }

  setFullscreen(!m_is_fullscreen, true);
}

void EmuThread::setFullscreen(bool fullscreen, bool allow_render_to_main)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, "setFullscreen", Qt::QueuedConnection, Q_ARG(bool, fullscreen),
                              Q_ARG(bool, allow_render_to_main));
    return;
  }

  if (!g_gpu_device || m_is_fullscreen == fullscreen)
    return;

  m_is_fullscreen = fullscreen;
  m_is_rendering_to_main = allow_render_to_main && shouldRenderToMain();
  GPUThread::UpdateDisplayWindow(fullscreen);
}

bool Host::IsFullscreen()
{
  return g_emu_thread->isFullscreen();
}

void Host::SetFullscreen(bool enabled)
{
  // don't mess with fullscreen while locked
  if (QtHost::IsSystemLocked())
    return;

  g_emu_thread->setFullscreen(enabled, true);
}

void EmuThread::setSurfaceless(bool surfaceless)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, "setSurfaceless", Qt::QueuedConnection, Q_ARG(bool, surfaceless));
    return;
  }

  if (!g_gpu_device || m_is_surfaceless == surfaceless)
    return;

  m_is_surfaceless = surfaceless;
  GPUThread::UpdateDisplayWindow(false);
}

void EmuThread::requestDisplaySize(float scale)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, "requestDisplaySize", Qt::QueuedConnection, Q_ARG(float, scale));
    return;
  }

  if (!System::IsValid())
    return;

  System::RequestDisplaySize(scale);
}

std::optional<WindowInfo> EmuThread::acquireRenderWindow(RenderAPI render_api, bool fullscreen,
                                                         bool exclusive_fullscreen, Error* error)
{
  DebugAssert(g_gpu_device);

  m_is_fullscreen = fullscreen;

  const bool window_fullscreen = m_is_fullscreen && !exclusive_fullscreen;
  const bool render_to_main = !fullscreen && m_is_rendering_to_main;
  const bool use_main_window_pos = shouldRenderToMain();

  return emit onAcquireRenderWindowRequested(render_api, window_fullscreen, render_to_main, m_is_surfaceless,
                                             use_main_window_pos, error);
}

void EmuThread::releaseRenderWindow()
{
  emit onReleaseRenderWindowRequested();
}

void EmuThread::connectDisplaySignals(DisplayWidget* widget)
{
  widget->disconnect(this);

  connect(widget, &DisplayWidget::windowResizedEvent, this, &EmuThread::onDisplayWindowResized);
  connect(widget, &DisplayWidget::windowRestoredEvent, this, &EmuThread::redrawDisplayWindow);
  connect(widget, &DisplayWidget::windowKeyEvent, this, &EmuThread::onDisplayWindowKeyEvent);
  connect(widget, &DisplayWidget::windowTextEntered, this, &EmuThread::onDisplayWindowTextEntered);
  connect(widget, &DisplayWidget::windowMouseButtonEvent, this, &EmuThread::onDisplayWindowMouseButtonEvent);
  connect(widget, &DisplayWidget::windowMouseWheelEvent, this, &EmuThread::onDisplayWindowMouseWheelEvent);
}

void Host::OnSystemStarting()
{
  emit g_emu_thread->systemStarting();
}

void Host::OnSystemStarted()
{
  g_emu_thread->stopBackgroundControllerPollTimer();
  g_emu_thread->wakeThread();

  emit g_emu_thread->systemStarted();
}

void Host::OnSystemPaused()
{
  emit g_emu_thread->systemPaused();
  g_emu_thread->startBackgroundControllerPollTimer();
}

void Host::OnSystemResumed()
{
  // if we were surfaceless (view->game list, system->unpause), get our display widget back
  if (g_emu_thread->isSurfaceless())
    g_emu_thread->setSurfaceless(false);

  emit g_emu_thread->systemResumed();
  g_emu_thread->wakeThread();

  g_emu_thread->stopBackgroundControllerPollTimer();
}

void Host::OnSystemDestroyed()
{
  g_emu_thread->resetPerformanceCounters();
  g_emu_thread->startBackgroundControllerPollTimer();
  emit g_emu_thread->systemDestroyed();
}

void Host::OnFullscreenUIStartedOrStopped(bool started)
{
  g_emu_thread->setFullscreenUIStarted(started);
}

void Host::OnGPUThreadRunIdleChanged(bool is_active)
{
  g_emu_thread->setGPUThreadRunIdle(is_active);
}

void EmuThread::reloadInputSources()
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &EmuThread::reloadInputSources, Qt::QueuedConnection);
    return;
  }

  System::ReloadInputSources();
}

void EmuThread::reloadInputBindings()
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &EmuThread::reloadInputBindings, Qt::QueuedConnection);
    return;
  }

  System::ReloadInputBindings();
}

void EmuThread::reloadInputDevices()
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &EmuThread::reloadInputDevices, Qt::QueuedConnection);
    return;
  }

  InputManager::ReloadDevices();
}

void EmuThread::closeInputSources()
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &EmuThread::reloadInputDevices, Qt::BlockingQueuedConnection);
    return;
  }

  InputManager::CloseSources();
}

void EmuThread::enumerateInputDevices()
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &EmuThread::enumerateInputDevices, Qt::QueuedConnection);
    return;
  }

  onInputDevicesEnumerated(InputManager::EnumerateDevices());
}

void EmuThread::enumerateVibrationMotors()
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &EmuThread::enumerateVibrationMotors, Qt::QueuedConnection);
    return;
  }

  const std::vector<InputBindingKey> motors(InputManager::EnumerateMotors());
  QList<InputBindingKey> qmotors;
  qmotors.reserve(motors.size());
  for (InputBindingKey key : motors)
    qmotors.push_back(key);

  onVibrationMotorsEnumerated(qmotors);
}

void EmuThread::confirmActionIfMemoryCardBusy(const QString& action, bool cancel_resume_on_accept,
                                              std::function<void(bool)> callback) const
{
  DebugAssert(isCurrentThread());

  if (!System::IsValid() || !System::IsSavingMemoryCards())
  {
    callback(true);
    return;
  }

  QtHost::RunOnUIThread([action, cancel_resume_on_accept, callback = std::move(callback)]() mutable {
    auto lock = g_main_window->pauseAndLockSystem();

    const bool result =
      (QMessageBox::question(lock.getDialogParent(), tr("Memory Card Busy"),
                             tr("WARNING: Your game is still saving to the memory card. Continuing to %1 may "
                                "IRREVERSIBLY DESTROY YOUR MEMORY CARD. We recommend resuming your game and waiting 5 "
                                "seconds for it to finish saving.\n\nDo you want to %1 anyway?")
                               .arg(action)) != QMessageBox::No);

    if (cancel_resume_on_accept)
      lock.cancelResume();

    Host::RunOnCPUThread([result, callback = std::move(callback)]() { callback(result); });
  });
}

void EmuThread::shutdownSystem(bool save_state, bool check_memcard_busy)
{
  if (!isCurrentThread())
  {
    System::CancelPendingStartup();
    QMetaObject::invokeMethod(this, "shutdownSystem", Qt::QueuedConnection, Q_ARG(bool, save_state),
                              Q_ARG(bool, check_memcard_busy));
    return;
  }

  if (check_memcard_busy && System::IsSavingMemoryCards())
  {
    confirmActionIfMemoryCardBusy(tr("shut down"), true, [save_state](bool result) {
      if (result)
        g_emu_thread->shutdownSystem(save_state, false);
      else
        g_emu_thread->setSystemPaused(false);
    });
    return;
  }

  System::ShutdownSystem(save_state);
}

void EmuThread::resetSystem(bool check_memcard_busy)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, "resetSystem", Qt::QueuedConnection, Q_ARG(bool, check_memcard_busy));
    return;
  }

  if (check_memcard_busy && System::IsSavingMemoryCards())
  {
    confirmActionIfMemoryCardBusy(tr("reset"), false, [](bool result) {
      if (result)
        g_emu_thread->resetSystem(false);
    });
    return;
  }

  System::ResetSystem();
}

void EmuThread::setSystemPaused(bool paused, bool wait_until_paused /* = false */)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, "setSystemPaused",
                              wait_until_paused ? Qt::BlockingQueuedConnection : Qt::QueuedConnection,
                              Q_ARG(bool, paused), Q_ARG(bool, wait_until_paused));
    return;
  }

  System::PauseSystem(paused);
}

void EmuThread::changeDisc(const QString& new_disc_filename, bool reset_system, bool check_memcard_busy)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, "changeDisc", Qt::QueuedConnection, Q_ARG(const QString&, new_disc_filename),
                              Q_ARG(bool, reset_system), Q_ARG(bool, check_memcard_busy));
    return;
  }

  if (check_memcard_busy && System::IsSavingMemoryCards())
  {
    confirmActionIfMemoryCardBusy(tr("change disc"), false, [new_disc_filename, reset_system](bool result) {
      if (result)
        g_emu_thread->changeDisc(new_disc_filename, reset_system, false);
    });
    return;
  }

  if (System::IsShutdown())
    return;

  if (!new_disc_filename.isEmpty())
    System::InsertMedia(new_disc_filename.toStdString().c_str());
  else
    System::RemoveMedia();

  if (reset_system)
    System::ResetSystem();
}

void EmuThread::changeDiscFromPlaylist(quint32 index)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, "changeDiscFromPlaylist", Qt::QueuedConnection, Q_ARG(quint32, index));
    return;
  }

  if (System::IsShutdown())
    return;

  if (!System::SwitchMediaSubImage(index))
    errorReported(tr("Error"), tr("Failed to switch to subimage %1").arg(index));
}

void EmuThread::reloadCheats(bool reload_files, bool reload_enabled_list, bool verbose, bool verbose_if_changed)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, "reloadCheats", Qt::QueuedConnection, Q_ARG(bool, reload_files),
                              Q_ARG(bool, reload_enabled_list), Q_ARG(bool, verbose), Q_ARG(bool, verbose_if_changed));
    return;
  }

  if (System::IsValid())
  {
    // If the reloaded list is being enabled, we also need to reload the gameini file.
    if (reload_enabled_list)
      System::ReloadGameSettings(verbose);
    Cheats::ReloadCheats(reload_files, reload_enabled_list, verbose, verbose_if_changed);
  }
}

void EmuThread::applyCheat(const QString& name)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, "applyCheat", Qt::QueuedConnection, Q_ARG(const QString&, name));
    return;
  }

  if (System::IsValid())
    Cheats::ApplyManualCode(name.toStdString());
}

void EmuThread::reloadPostProcessingShaders()
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, "reloadPostProcessingShaders", Qt::QueuedConnection);
    return;
  }

  if (System::IsValid())
  {
    GPUThread::RunOnThread([]() {
      if (GPUThread::HasGPUBackend())
        PostProcessing::ReloadShaders();
    });
  }
}

void EmuThread::updatePostProcessingSettings()
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, "updatePostProcessingSettings", Qt::QueuedConnection);
    return;
  }

  if (System::IsValid())
  {
    GPUThread::RunOnThread([]() {
      if (GPUThread::HasGPUBackend())
        PostProcessing::UpdateSettings();
    });
  }
}

void EmuThread::clearInputBindStateFromSource(InputBindingKey key)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, "clearInputBindStateFromSource", Qt::QueuedConnection, Q_ARG(InputBindingKey, key));
    return;
  }

  InputManager::ClearBindStateFromSource(key);
}

void EmuThread::reloadTextureReplacements()
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, "reloadTextureReplacements", Qt::QueuedConnection);
    return;
  }

  if (System::IsValid())
    GPUThread::RunOnThread([]() { GPUTextureCache::ReloadTextureReplacements(true); });
}

void EmuThread::captureGPUFrameDump()
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, "captureGPUFrameDump", Qt::QueuedConnection);
    return;
  }

  if (System::IsValid())
    System::StartRecordingGPUDump();
}

void EmuThread::runOnEmuThread(std::function<void()> callback)
{
  callback();
}

void Host::RunOnCPUThread(std::function<void()> function, bool block /* = false */)
{
  const bool self = g_emu_thread->isCurrentThread();

  QMetaObject::invokeMethod(g_emu_thread, "runOnEmuThread",
                            (block && !self) ? Qt::BlockingQueuedConnection : Qt::QueuedConnection,
                            Q_ARG(std::function<void()>, std::move(function)));
}

void QtHost::RunOnUIThread(const std::function<void()>& func, bool block /*= false*/)
{
  // main window always exists, so it's fine to attach it to that.
  QMetaObject::invokeMethod(g_main_window, "runOnUIThread", block ? Qt::BlockingQueuedConnection : Qt::QueuedConnection,
                            Q_ARG(const std::function<void()>&, func));
}

void Host::RefreshGameListAsync(bool invalidate_cache)
{
  QMetaObject::invokeMethod(g_main_window, "refreshGameList", Qt::QueuedConnection, Q_ARG(bool, invalidate_cache));
}

void Host::CancelGameListRefresh()
{
  QMetaObject::invokeMethod(g_main_window, "cancelGameListRefresh", Qt::BlockingQueuedConnection);
}

void EmuThread::loadState(const QString& filename)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, "loadState", Qt::QueuedConnection, Q_ARG(const QString&, filename));
    return;
  }

  bootOrLoadState(filename.toStdString());
}

void EmuThread::loadState(bool global, qint32 slot)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, "loadState", Qt::QueuedConnection, Q_ARG(bool, global), Q_ARG(qint32, slot));
    return;
  }

  // shouldn't even get here if we don't have a running game
  if (!global && System::GetGameSerial().empty())
    return;

  bootOrLoadState(global ? System::GetGlobalSaveStateFileName(slot) :
                           System::GetGameSaveStateFileName(System::GetGameSerial(), slot));
}

void EmuThread::saveState(const QString& filename, bool block_until_done /* = false */)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, "saveState", block_until_done ? Qt::BlockingQueuedConnection : Qt::QueuedConnection,
                              Q_ARG(const QString&, filename), Q_ARG(bool, block_until_done));
    return;
  }

  if (!System::IsValid())
    return;

  Error error;
  if (!System::SaveState(filename.toStdString(), &error, g_settings.create_save_state_backups, false))
    emit errorReported(tr("Error"), tr("Failed to save state: %1").arg(QString::fromStdString(error.GetDescription())));
}

void EmuThread::saveState(bool global, qint32 slot, bool block_until_done /* = false */)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, "saveState", block_until_done ? Qt::BlockingQueuedConnection : Qt::QueuedConnection,
                              Q_ARG(bool, global), Q_ARG(qint32, slot), Q_ARG(bool, block_until_done));
    return;
  }

  if (!global && System::GetGameSerial().empty())
    return;

  Error error;
  if (!System::SaveState((global ? System::GetGlobalSaveStateFileName(slot) :
                                   System::GetGameSaveStateFileName(System::GetGameSerial(), slot))
                           .c_str(),
                         &error, g_settings.create_save_state_backups, false))
  {
    emit errorReported(tr("Error"), tr("Failed to save state: %1").arg(QString::fromStdString(error.GetDescription())));
  }
}

void EmuThread::undoLoadState()
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, "undoLoadState", Qt::QueuedConnection);
    return;
  }

  System::UndoLoadState();
}

void EmuThread::setAudioOutputVolume(int volume, int fast_forward_volume)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, "setAudioOutputVolume", Qt::QueuedConnection, Q_ARG(int, volume),
                              Q_ARG(int, fast_forward_volume));
    return;
  }

  g_settings.audio_output_volume = volume;
  g_settings.audio_fast_forward_volume = fast_forward_volume;
  System::UpdateVolume();
}

void EmuThread::setAudioOutputMuted(bool muted)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, "setAudioOutputMuted", Qt::QueuedConnection, Q_ARG(bool, muted));
    return;
  }

  g_settings.audio_output_muted = muted;
  System::UpdateVolume();
}

void EmuThread::startDumpingAudio()
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, "startDumpingAudio", Qt::QueuedConnection);
    return;
  }

  // System::StartDumpingAudio();
}

void EmuThread::stopDumpingAudio()
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, "stopDumpingAudio", Qt::QueuedConnection);
    return;
  }

  // System::StopDumpingAudio();
}

void EmuThread::singleStepCPU()
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, "singleStepCPU", Qt::BlockingQueuedConnection);
    return;
  }

  if (!System::IsValid())
    return;

  System::SingleStepCPU();
}

void EmuThread::dumpRAM(const QString& filename)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, "dumpRAM", Qt::QueuedConnection, Q_ARG(const QString&, filename));
    return;
  }

  const std::string filename_str = filename.toStdString();
  if (System::DumpRAM(filename_str.c_str()))
    Host::AddOSDMessage(fmt::format("RAM dumped to '{}'", filename_str), 10.0f);
  else
    Host::ReportErrorAsync("Error", fmt::format("Failed to dump RAM to '{}'", filename_str));
}

void EmuThread::dumpVRAM(const QString& filename)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, "dumpVRAM", Qt::QueuedConnection, Q_ARG(const QString&, filename));
    return;
  }

  const std::string filename_str = filename.toStdString();
  if (System::DumpVRAM(filename_str.c_str()))
    Host::AddOSDMessage(fmt::format("VRAM dumped to '{}'", filename_str), 10.0f);
  else
    Host::ReportErrorAsync("Error", fmt::format("Failed to dump VRAM to '{}'", filename_str));
}

void EmuThread::dumpSPURAM(const QString& filename)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, "dumpSPURAM", Qt::QueuedConnection, Q_ARG(const QString&, filename));
    return;
  }

  const std::string filename_str = filename.toStdString();
  if (System::DumpSPURAM(filename_str.c_str()))
    Host::AddOSDMessage(fmt::format("SPU RAM dumped to '{}'", filename_str), 10.0f);
  else
    Host::ReportErrorAsync("Error", fmt::format("Failed to dump SPU RAM to '{}'", filename_str));
}

void EmuThread::saveScreenshot()
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, "saveScreenshot", Qt::QueuedConnection);
    return;
  }

  System::SaveScreenshot();
}

void Host::OnAchievementsLoginRequested(Achievements::LoginRequestReason reason)
{
  emit g_emu_thread->achievementsLoginRequested(reason);
}

void Host::OnAchievementsLoginSuccess(const char* username, u32 points, u32 sc_points, u32 unread_messages)
{
  const QString message = qApp->translate("QtHost", "RA: Logged in as %1 (%2, %3 softcore). %4 unread messages.")
                            .arg(QString::fromUtf8(username))
                            .arg(points)
                            .arg(sc_points)
                            .arg(unread_messages);

  emit g_emu_thread->statusMessage(message);
}

void Host::OnAchievementsRefreshed()
{
  u32 game_id = 0;

  QString game_info;

  if (Achievements::HasActiveGame())
  {
    game_id = Achievements::GetGameID();

    game_info = qApp->translate("EmuThread", "Game: %1 (%2)\n")
                  .arg(QString::fromStdString(Achievements::GetGameTitle()))
                  .arg(game_id);

    const std::string& rich_presence_string = Achievements::GetRichPresenceString();
    if (Achievements::HasRichPresence() && !rich_presence_string.empty())
      game_info.append(QString::fromStdString(StringUtil::Ellipsise(rich_presence_string, 128)));
    else
      game_info.append(qApp->translate("EmuThread", "Rich presence inactive or unsupported."));
  }
  else
  {
    game_info = qApp->translate("EmuThread", "Game not loaded or no RetroAchievements available.");
  }

  emit g_emu_thread->achievementsRefreshed(game_id, game_info);
}

void Host::OnAchievementsHardcoreModeChanged(bool enabled)
{
  emit g_emu_thread->achievementsChallengeModeChanged(enabled);
}

void Host::OnCoverDownloaderOpenRequested()
{
  emit g_emu_thread->onCoverDownloaderOpenRequested();
}

bool Host::ShouldPreferHostFileSelector()
{
#ifdef __linux__
  // If running inside a flatpak, we want to use native selectors/portals.
  return (std::getenv("container") != nullptr);
#else
  return false;
#endif
}

void Host::OpenHostFileSelectorAsync(std::string_view title, bool select_directory, FileSelectorCallback callback,
                                     FileSelectorFilters filters /* = FileSelectorFilters() */,
                                     std::string_view initial_directory /* = std::string_view() */)
{
  const bool from_cpu_thread = g_emu_thread->isCurrentThread();

  QString filters_str;
  if (!filters.empty())
  {
    filters_str.append(QStringLiteral("All File Types (%1)")
                         .arg(QString::fromStdString(StringUtil::JoinString(filters.begin(), filters.end(), " "))));
    for (const std::string& filter : filters)
    {
      filters_str.append(
        QStringLiteral(";;%1 Files (%2)")
          .arg(
            QtUtils::StringViewToQString(std::string_view(filter).substr(filter.starts_with("*.") ? 2 : 0)).toUpper())
          .arg(QString::fromStdString(filter)));
    }
  }

  QtHost::RunOnUIThread([title = QtUtils::StringViewToQString(title), select_directory, callback = std::move(callback),
                         filters_str = std::move(filters_str),
                         initial_directory = QtUtils::StringViewToQString(initial_directory),
                         from_cpu_thread]() mutable {
    auto lock = g_main_window->pauseAndLockSystem();

    QString path;

    if (select_directory)
      path = QFileDialog::getExistingDirectory(lock.getDialogParent(), title, initial_directory);
    else
      path = QFileDialog::getOpenFileName(lock.getDialogParent(), title, initial_directory, filters_str);

    if (!path.isEmpty())
      path = QDir::toNativeSeparators(path);

    if (from_cpu_thread)
      Host::RunOnCPUThread([callback = std::move(callback), path = path.toStdString()]() { callback(path); });
    else
      callback(path.toStdString());
  });
}

bool Host::CreateAuxiliaryRenderWindow(s32 x, s32 y, u32 width, u32 height, std::string_view title,
                                       std::string_view icon_name, AuxiliaryRenderWindowUserData userdata,
                                       AuxiliaryRenderWindowHandle* handle, WindowInfo* wi, Error* error)
{
  return emit g_emu_thread->onCreateAuxiliaryRenderWindow(
    g_gpu_device->GetRenderAPI(), x, y, width, height, QtUtils::StringViewToQString(title),
    QtUtils::StringViewToQString(icon_name), userdata, handle, wi, error);
}

void Host::DestroyAuxiliaryRenderWindow(AuxiliaryRenderWindowHandle handle, s32* pos_x, s32* pos_y, u32* width,
                                        u32* height)
{
  QPoint pos;
  QSize size;
  emit g_emu_thread->onDestroyAuxiliaryRenderWindow(handle, &pos, &size);

  if (pos_x)
    *pos_x = pos.x();
  if (pos_y)
    *pos_y = pos.y();
  if (width)
    *width = size.width();
  if (height)
    *height = size.height();

  // eat all pending events, to make sure we're not going to write input events back to a dead pointer
  if (g_emu_thread->isCurrentThread())
    g_emu_thread->getEventLoop()->processEvents(QEventLoop::AllEvents);
}

void EmuThread::queueAuxiliaryRenderWindowInputEvent(Host::AuxiliaryRenderWindowUserData userdata,
                                                     Host::AuxiliaryRenderWindowEvent event,
                                                     Host::AuxiliaryRenderWindowEventParam param1,
                                                     Host::AuxiliaryRenderWindowEventParam param2,
                                                     Host::AuxiliaryRenderWindowEventParam param3)
{
  DebugAssert(QThread::isMainThread());
  QMetaObject::invokeMethod(this, "processAuxiliaryRenderWindowInputEvent", Qt::QueuedConnection,
                            Q_ARG(void*, userdata), Q_ARG(quint32, static_cast<quint32>(event)),
                            Q_ARG(quint32, param1.uint_param), Q_ARG(quint32, param2.uint_param),
                            Q_ARG(quint32, param3.uint_param));
}

void EmuThread::processAuxiliaryRenderWindowInputEvent(void* userdata, quint32 event, quint32 param1, quint32 param2,
                                                       quint32 param3)
{
  DebugAssert(isCurrentThread());
  GPUThread::RunOnThread([userdata, event, param1, param2, param3]() {
    ImGuiManager::ProcessAuxiliaryRenderWindowInputEvent(userdata, static_cast<Host::AuxiliaryRenderWindowEvent>(event),
                                                         Host::AuxiliaryRenderWindowEventParam{.uint_param = param1},
                                                         Host::AuxiliaryRenderWindowEventParam{.uint_param = param2},
                                                         Host::AuxiliaryRenderWindowEventParam{.uint_param = param3});
  });
}

void EmuThread::doBackgroundControllerPoll()
{
  System::IdlePollUpdate();
}

void EmuThread::createBackgroundControllerPollTimer()
{
  DebugAssert(!m_background_controller_polling_timer);
  m_background_controller_polling_timer = new QTimer(this);
  m_background_controller_polling_timer->setSingleShot(false);
  m_background_controller_polling_timer->setTimerType(Qt::CoarseTimer);
  connect(m_background_controller_polling_timer, &QTimer::timeout, this, &EmuThread::doBackgroundControllerPoll);
}

void EmuThread::destroyBackgroundControllerPollTimer()
{
  delete m_background_controller_polling_timer;
  m_background_controller_polling_timer = nullptr;
}

void EmuThread::startBackgroundControllerPollTimer()
{
  if (m_background_controller_polling_timer->isActive())
    return;

  u32 poll_interval = BACKGROUND_CONTROLLER_POLLING_INTERVAL;
  if (m_gpu_thread_run_idle)
    poll_interval = FULLSCREEN_UI_CONTROLLER_POLLING_INTERVAL;
  if (GDBServer::HasAnyClients())
    poll_interval = GDB_SERVER_POLLING_INTERVAL;

  m_background_controller_polling_timer->start(poll_interval);
}

void EmuThread::stopBackgroundControllerPollTimer()
{
  if (!m_background_controller_polling_timer->isActive())
    return;

  m_background_controller_polling_timer->stop();
}

void EmuThread::setGPUThreadRunIdle(bool active)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, "setGPUThreadRunIdle", Qt::QueuedConnection, Q_ARG(bool, active));
    return;
  }

  m_gpu_thread_run_idle = active;

  // adjust the timer speed to pick up controller input faster
  if (!m_background_controller_polling_timer->isActive())
    return;

  g_emu_thread->stopBackgroundControllerPollTimer();
  g_emu_thread->startBackgroundControllerPollTimer();
}

void EmuThread::setFullscreenUIStarted(bool started)
{
  if (m_is_fullscreen_ui_started == started)
    return;

  m_is_fullscreen_ui_started = started;
  emit fullscreenUIStartedOrStopped(started);
}

void EmuThread::start()
{
  AssertMsg(!g_emu_thread, "Emu thread does not exist");

  g_emu_thread = new EmuThread(QThread::currentThread());
  g_emu_thread->QThread::start();
  g_emu_thread->m_started_semaphore.acquire();
  g_emu_thread->moveToThread(g_emu_thread);
}

void EmuThread::stop()
{
  AssertMsg(g_emu_thread, "Emu thread exists");
  AssertMsg(!g_emu_thread->isCurrentThread(), "Not called on the emu thread");

  QMetaObject::invokeMethod(g_emu_thread, &EmuThread::stopInThread, Qt::QueuedConnection);
  QtUtils::ProcessEventsWithSleep(QEventLoop::ExcludeUserInputEvents, []() { return (g_emu_thread->isRunning()); });
}

void EmuThread::stopInThread()
{
  stopFullscreenUI();

  m_shutdown_flag = true;
  m_event_loop->quit();
}

void EmuThread::run()
{
  Threading::SetNameOfCurrentThread("CPU Thread");

  m_event_loop = new QEventLoop();
  m_started_semaphore.release();

  // input source setup must happen on emu thread
  {
    Error startup_error;
    if (!System::CPUThreadInitialize(&startup_error))
    {
      moveToThread(m_ui_thread);
      Host::ReportFatalError("Fatal Startup Error", startup_error.GetDescription());
      return;
    }
  }

  {
    // kick off GPU thread
    QtHost::GPUThread gpu_thread;
    gpu_thread.start();

    // bind buttons/axises
    createBackgroundControllerPollTimer();
    startBackgroundControllerPollTimer();

    // main loop
    while (!m_shutdown_flag)
    {
      if (System::IsRunning())
        System::Execute();
      else
        m_event_loop->exec();
    }

    if (System::IsValid())
      System::ShutdownSystem(false);

    destroyBackgroundControllerPollTimer();

    // tell GPU thread to exit
    GPUThread::Internal::RequestShutdown();

    // and tidy up everything left
    System::CPUThreadShutdown();
  }

  // move back to UI thread
  moveToThread(m_ui_thread);
}

QtHost::GPUThread::GPUThread(QObject* parent) : QThread(parent)
{
}

QtHost::GPUThread::~GPUThread() = default;

void QtHost::GPUThread::run()
{
  Threading::SetNameOfCurrentThread("GPU Thread");
  ::GPUThread::Internal::GPUThreadEntryPoint();
}

void Host::FrameDoneOnGPUThread(GPUBackend* gpu_backend, u32 frame_number)
{
}

void EmuThread::wakeThread()
{
  if (isCurrentThread())
    m_event_loop->quit();
  else
    QMetaObject::invokeMethod(m_event_loop, "quit", Qt::QueuedConnection);
}

void Host::ReportFatalError(std::string_view title, std::string_view message)
{
  auto cb = [title = QtUtils::StringViewToQString(title), message = QtUtils::StringViewToQString(message)]() {
    QMessageBox::critical(g_main_window && g_main_window->isVisible() ? g_main_window : nullptr, title, message);
#ifndef __APPLE__
    std::quick_exit(EXIT_FAILURE);
#else
    _exit(EXIT_FAILURE);
#endif
  };

  // https://stackoverflow.com/questions/34135624/how-to-properly-execute-gui-operations-in-qt-main-thread
  QTimer* timer = new QTimer();
  QThread* ui_thread = qApp->thread();
  if (QThread::currentThread() == ui_thread)
  {
    // On UI thread, we can do it straight away.
    cb();
  }
  else
  {
    timer->moveToThread(ui_thread);
    timer->setSingleShot(true);
    QObject::connect(timer, &QTimer::timeout, std::move(cb));
    QMetaObject::invokeMethod(timer, "start", Qt::QueuedConnection, Q_ARG(int, 0));
  }
}

void Host::ReportErrorAsync(std::string_view title, std::string_view message)
{
  if (!title.empty() && !message.empty())
    ERROR_LOG("ReportErrorAsync: {}: {}", title, message);
  else if (!message.empty())
    ERROR_LOG("ReportErrorAsync: {}", message);

  QMetaObject::invokeMethod(
    g_main_window, "reportError", Qt::QueuedConnection,
    Q_ARG(const QString&, title.empty() ? QString() : QString::fromUtf8(title.data(), title.size())),
    Q_ARG(const QString&, message.empty() ? QString() : QString::fromUtf8(message.data(), message.size())));
}

bool Host::ConfirmMessage(std::string_view title, std::string_view message)
{
  auto lock = g_emu_thread->pauseAndLockSystem();

  return emit g_emu_thread->messageConfirmed(QString::fromUtf8(title.data(), title.size()),
                                             QString::fromUtf8(message.data(), message.size()));
}

void Host::ConfirmMessageAsync(std::string_view title, std::string_view message, ConfirmMessageAsyncCallback callback)
{
  QtHost::RunOnUIThread([title = QtUtils::StringViewToQString(title), message = QtUtils::StringViewToQString(message),
                         callback = std::move(callback)]() mutable {
    auto lock = g_main_window->pauseAndLockSystem();

    const bool result = (QMessageBox::question(lock.getDialogParent(), title, message) != QMessageBox::No);

    callback(result);
  });
}

void Host::OpenURL(std::string_view url)
{
  QtHost::RunOnUIThread([url = QtUtils::StringViewToQString(url)]() { QtUtils::OpenURL(g_main_window, QUrl(url)); });
}

std::string Host::GetClipboardText()
{
  // Hope this doesn't deadlock...
  std::string ret;
  QtHost::RunOnUIThread(
    [&ret]() {
      QClipboard* clipboard = QGuiApplication::clipboard();
      if (clipboard)
        ret = clipboard->text().toStdString();
    },
    true);
  return ret;
}

bool Host::CopyTextToClipboard(std::string_view text)
{
  QtHost::RunOnUIThread([text = QtUtils::StringViewToQString(text)]() {
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (clipboard)
      clipboard->setText(text);
  });
  return true;
}

void Host::ReportDebuggerMessage(std::string_view message)
{
  emit g_emu_thread->debuggerMessageReported(QString::fromUtf8(message));
}

void Host::AddFixedInputBindings(const SettingsInterface& si)
{
}

void Host::OnInputDeviceConnected(std::string_view identifier, std::string_view device_name)
{
  emit g_emu_thread->onInputDeviceConnected(std::string(identifier), std::string(device_name));

  if (System::IsValid() || GPUThread::IsFullscreenUIRequested())
  {
    Host::AddIconOSDMessage(fmt::format("ControllerConnected{}", identifier), ICON_FA_GAMEPAD,
                            fmt::format(TRANSLATE_FS("QtHost", "Controller {} connected."), identifier),
                            Host::OSD_INFO_DURATION);
  }
}

void Host::OnInputDeviceDisconnected(InputBindingKey key, std::string_view identifier)
{
  emit g_emu_thread->onInputDeviceDisconnected(std::string(identifier));

  if (g_settings.pause_on_controller_disconnection && System::GetState() == System::State::Running &&
      InputManager::HasAnyBindingsForSource(key))
  {
    std::string message =
      fmt::format(TRANSLATE_FS("QtHost", "System paused because controller {} was disconnected."), identifier);
    Host::RunOnCPUThread([message = QString::fromStdString(message)]() {
      System::PauseSystem(true);

      // has to be done after pause, otherwise pause message takes precedence
      emit g_emu_thread->statusMessage(message);
    });
    Host::AddIconOSDMessage(fmt::format("ControllerConnected{}", identifier), ICON_FA_GAMEPAD, std::move(message),
                            Host::OSD_WARNING_DURATION);
  }
  else if (System::IsValid() || GPUThread::IsFullscreenUIRequested())
  {
    Host::AddIconOSDMessage(fmt::format("ControllerConnected{}", identifier), ICON_FA_GAMEPAD,
                            fmt::format(TRANSLATE_FS("QtHost", "Controller {} disconnected."), identifier),
                            Host::OSD_INFO_DURATION);
  }
}

std::string QtHost::GetResourcePath(std::string_view filename, bool allow_override)
{
  return allow_override ? EmuFolders::GetOverridableResourcePath(filename) :
                          Path::Combine(EmuFolders::Resources, filename);
}

bool Host::ResourceFileExists(std::string_view filename, bool allow_override)
{
  const std::string path = QtHost::GetResourcePath(filename, allow_override);
  return FileSystem::FileExists(path.c_str());
}

std::optional<DynamicHeapArray<u8>> Host::ReadResourceFile(std::string_view filename, bool allow_override, Error* error)
{
  const std::string path = QtHost::GetResourcePath(filename, allow_override);
  return FileSystem::ReadBinaryFile(path.c_str(), error);
}

std::optional<std::string> Host::ReadResourceFileToString(std::string_view filename, bool allow_override, Error* error)
{
  const std::string path = QtHost::GetResourcePath(filename, allow_override);
  return FileSystem::ReadFileToString(path.c_str(), error);
}

std::optional<std::time_t> Host::GetResourceFileTimestamp(std::string_view filename, bool allow_override)
{
  const std::string path = QtHost::GetResourcePath(filename, allow_override);

  FILESYSTEM_STAT_DATA sd;
  if (!FileSystem::StatFile(path.c_str(), &sd))
  {
    ERROR_LOG("Failed to stat resource file '{}'", filename);
    return std::nullopt;
  }

  return sd.ModificationTime;
}

void Host::CommitBaseSettingChanges()
{
  if (g_emu_thread->isCurrentThread())
    QtHost::RunOnUIThread([]() { QtHost::QueueSettingsSave(); });
  else
    QtHost::QueueSettingsSave();
}

std::optional<WindowInfo> Host::AcquireRenderWindow(RenderAPI render_api, bool fullscreen, bool exclusive_fullscreen,
                                                    Error* error)
{
  return g_emu_thread->acquireRenderWindow(render_api, fullscreen, exclusive_fullscreen, error);
}

void Host::ReleaseRenderWindow()
{
  g_emu_thread->releaseRenderWindow();
}

void EmuThread::updatePerformanceCounters(const GPUBackend* gpu_backend)
{
  const RenderAPI render_api = g_gpu_device->GetRenderAPI();
  const bool hardware_renderer = GPUBackend::IsUsingHardwareBackend();
  u32 render_width = 0;
  u32 render_height = 0;

  if (gpu_backend)
  {
    const u32 render_scale = gpu_backend->GetResolutionScale();
    std::tie(render_width, render_height) = g_gpu->GetFullDisplayResolution();
    render_width *= render_scale;
    render_height *= render_scale;
  }

  if (render_api != m_last_render_api || hardware_renderer != m_last_hardware_renderer)
  {
    const QString renderer_str = hardware_renderer ? QString::fromUtf8(GPUDevice::RenderAPIToString(render_api)) :
                                                     qApp->translate("GPURenderer", "Software");
    QMetaObject::invokeMethod(g_main_window->getStatusRendererWidget(), "setText", Qt::QueuedConnection,
                              Q_ARG(const QString&, renderer_str));
    m_last_render_api = render_api;
    m_last_hardware_renderer = hardware_renderer;
  }
  if (render_width != m_last_render_width || render_height != m_last_render_height)
  {
    QMetaObject::invokeMethod(g_main_window->getStatusResolutionWidget(), "setText", Qt::QueuedConnection,
                              Q_ARG(const QString&, tr("%1x%2").arg(render_width).arg(render_height)));
    m_last_render_width = render_width;
    m_last_render_height = render_height;
  }

  const float gfps = PerformanceCounters::GetFPS();
  if (gfps != m_last_game_fps)
  {
    QMetaObject::invokeMethod(g_main_window->getStatusFPSWidget(), "setText", Qt::QueuedConnection,
                              Q_ARG(const QString&, tr("Game: %1 FPS").arg(gfps, 0, 'f', 0)));
    m_last_game_fps = gfps;
  }

  const float speed = PerformanceCounters::GetEmulationSpeed();
  const float vfps = PerformanceCounters::GetVPS();
  if (speed != m_last_speed || vfps != m_last_video_fps)
  {
    QMetaObject::invokeMethod(
      g_main_window->getStatusVPSWidget(), "setText", Qt::QueuedConnection,
      Q_ARG(const QString&, tr("Video: %1 FPS (%2%)").arg(vfps, 0, 'f', 0).arg(speed, 0, 'f', 0)));
    m_last_speed = speed;
    m_last_video_fps = vfps;
  }
}

void EmuThread::resetPerformanceCounters()
{
  m_last_speed = std::numeric_limits<float>::infinity();
  m_last_game_fps = std::numeric_limits<float>::infinity();
  m_last_video_fps = std::numeric_limits<float>::infinity();
  m_last_render_width = std::numeric_limits<u32>::max();
  m_last_render_height = std::numeric_limits<u32>::max();
  m_last_render_api = RenderAPI::None;
  m_last_hardware_renderer = false;

  QString blank;
  QMetaObject::invokeMethod(g_main_window->getStatusRendererWidget(), "setText", Qt::QueuedConnection,
                            Q_ARG(const QString&, blank));
  QMetaObject::invokeMethod(g_main_window->getStatusResolutionWidget(), "setText", Qt::QueuedConnection,
                            Q_ARG(const QString&, blank));
  QMetaObject::invokeMethod(g_main_window->getStatusFPSWidget(), "setText", Qt::QueuedConnection,
                            Q_ARG(const QString&, blank));
  QMetaObject::invokeMethod(g_main_window->getStatusVPSWidget(), "setText", Qt::QueuedConnection,
                            Q_ARG(const QString&, blank));
}

void Host::OnPerformanceCountersUpdated(const GPUBackend* gpu_backend)
{
  g_emu_thread->updatePerformanceCounters(gpu_backend);
}

void Host::OnGameChanged(const std::string& disc_path, const std::string& game_serial, const std::string& game_name)
{
  emit g_emu_thread->runningGameChanged(QString::fromStdString(disc_path), QString::fromStdString(game_serial),
                                        QString::fromStdString(game_name));
}

void Host::OnMediaCaptureStarted()
{
  emit g_emu_thread->mediaCaptureStarted();
}

void Host::OnMediaCaptureStopped()
{
  emit g_emu_thread->mediaCaptureStopped();
}

void Host::SetMouseMode(bool relative, bool hide_cursor)
{
  emit g_emu_thread->mouseModeRequested(relative, hide_cursor);
}

void Host::PumpMessagesOnCPUThread()
{
  g_emu_thread->getEventLoop()->processEvents(QEventLoop::AllEvents);
}

void QtHost::SaveSettings()
{
  AssertMsg(!g_emu_thread->isCurrentThread(), "Saving should happen on the UI thread.");

  {
    Error error;
    auto lock = Host::GetSettingsLock();
    if (!s_base_settings_interface->Save(&error))
      ERROR_LOG("Failed to save settings: {}", error.GetDescription());
  }

  if (s_settings_save_timer)
  {
    s_settings_save_timer->deleteLater();
    s_settings_save_timer.release();
  }
}

void QtHost::QueueSettingsSave()
{
  if (g_emu_thread->isCurrentThread())
  {
    QtHost::RunOnUIThread(QueueSettingsSave);
    return;
  }

  if (s_settings_save_timer)
    return;

  s_settings_save_timer = std::make_unique<QTimer>();
  s_settings_save_timer->connect(s_settings_save_timer.get(), &QTimer::timeout, SaveSettings);
  s_settings_save_timer->setSingleShot(true);
  s_settings_save_timer->start(SETTINGS_SAVE_DELAY);
}

bool QtHost::ShouldShowDebugOptions()
{
  return Host::GetBaseBoolSettingValue("Main", "ShowDebugMenu", false);
}

void Host::RequestSystemShutdown(bool allow_confirm, bool save_state)
{
  if (!System::IsValid())
    return;

  QMetaObject::invokeMethod(g_main_window, "requestShutdown", Qt::QueuedConnection, Q_ARG(bool, allow_confirm),
                            Q_ARG(bool, true), Q_ARG(bool, save_state));
}

void Host::RequestExitApplication(bool allow_confirm)
{
  QMetaObject::invokeMethod(g_main_window, "requestExit", Qt::QueuedConnection, Q_ARG(bool, allow_confirm));
}

void Host::RequestExitBigPicture()
{
  g_emu_thread->stopFullscreenUI();
}

std::optional<WindowInfo> Host::GetTopLevelWindowInfo()
{
  std::optional<WindowInfo> ret;
  QMetaObject::invokeMethod(g_main_window, &MainWindow::getWindowInfo, Qt::BlockingQueuedConnection, &ret);
  return ret;
}

EmuThread::SystemLock EmuThread::pauseAndLockSystem()
{
  const bool was_fullscreen = QtHost::IsSystemValid() && isFullscreen();
  const bool was_paused = QtHost::IsSystemPaused();

  // We use surfaceless rather than switching out of fullscreen, because
  // we're paused, so we're not going to be rendering anyway.
  if (was_fullscreen)
    setSurfaceless(true);
  if (!was_paused)
    setSystemPaused(true);

  return SystemLock(was_paused, was_fullscreen);
}

EmuThread::SystemLock::SystemLock(bool was_paused, bool was_fullscreen)
  : m_was_paused(was_paused), m_was_fullscreen(was_fullscreen)
{
}

EmuThread::SystemLock::SystemLock(SystemLock&& lock)
  : m_was_paused(lock.m_was_paused), m_was_fullscreen(lock.m_was_fullscreen)
{
  lock.m_was_paused = true;
  lock.m_was_fullscreen = false;
}

EmuThread::SystemLock::~SystemLock()
{
  if (m_was_fullscreen)
    g_emu_thread->setSurfaceless(false);
  if (!m_was_paused)
    g_emu_thread->setSystemPaused(false);
}

void EmuThread::SystemLock::cancelResume()
{
  m_was_paused = true;
  m_was_fullscreen = false;
}

BEGIN_HOTKEY_LIST(g_host_hotkeys)
END_HOTKEY_LIST()

static void SignalHandler(int signal)
{
  // First try the normal (graceful) shutdown/exit.
  static bool graceful_shutdown_attempted = false;
  if (!graceful_shutdown_attempted && g_main_window)
  {
    std::fprintf(stderr, "Received CTRL+C, attempting graceful shutdown. Press CTRL+C again to force.\n");
    graceful_shutdown_attempted = true;

    // This could be a bit risky invoking from a signal handler... hopefully it's okay.
    QMetaObject::invokeMethod(g_main_window, "requestExit", Qt::QueuedConnection, Q_ARG(bool, true));
    return;
  }

  std::signal(signal, SIG_DFL);

  // MacOS is missing std::quick_exit() despite it being C++11...
#ifndef __APPLE__
  std::quick_exit(1);
#else
  _Exit(1);
#endif
}

void QtHost::HookSignals()
{
  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);

#ifdef __linux__
  // Ignore SIGCHLD by default on Linux, since we kick off aplay asynchronously.
  struct sigaction sa_chld = {};
  sigemptyset(&sa_chld.sa_mask);
  sa_chld.sa_flags = SA_SIGINFO | SA_RESTART | SA_NOCLDSTOP | SA_NOCLDWAIT;
  sigaction(SIGCHLD, &sa_chld, nullptr);
#endif
}

void QtHost::InitializeEarlyConsole()
{
  const bool was_console_enabled = Log::IsConsoleOutputEnabled();
  if (!was_console_enabled)
    Log::SetConsoleOutputParams(true);
  Log::SetLogLevel(Log::Level::Dev);
}

void QtHost::PrintCommandLineVersion()
{
  InitializeEarlyConsole();

  std::fprintf(stderr, "DuckStation Version %s (%s)\n", g_scm_tag_str, g_scm_branch_str);
  std::fprintf(stderr, "https://github.com/stenzek/duckstation\n");
  std::fprintf(stderr, "\n");
}

void QtHost::PrintCommandLineHelp(const char* progname)
{
  InitializeEarlyConsole();

  PrintCommandLineVersion();
  std::fprintf(stderr, "Usage: %s [parameters] [--] [boot filename]\n", progname);
  std::fprintf(stderr, "\n");
  std::fprintf(stderr, "  -help: Displays this information and exits.\n");
  std::fprintf(stderr, "  -version: Displays version information and exits.\n");
  std::fprintf(stderr, "  -batch: Enables batch mode (exits after powering off).\n");
  std::fprintf(stderr, "  -fastboot: Force fast boot for provided filename.\n");
  std::fprintf(stderr, "  -slowboot: Force slow boot for provided filename.\n");
  std::fprintf(stderr, "  -bios: Boot into the BIOS shell.\n");
  std::fprintf(stderr, "  -resume: Load resume save state. If a boot filename is provided,\n"
                       "    that game's resume state will be loaded, otherwise the most\n"
                       "    recent resume save state will be loaded.\n");
  std::fprintf(stderr, "  -state <index>: Loads specified save state by index. If a boot\n"
                       "    filename is provided, a per-game state will be loaded, otherwise\n"
                       "    a global state will be loaded.\n");
  std::fprintf(stderr, "  -statefile <filename>: Loads state from the specified filename.\n"
                       "    No boot filename is required with this option.\n");
  std::fprintf(stderr, "  -exe <filename>: Boot the specified exe instead of loading from disc.\n");
  std::fprintf(stderr, "  -fullscreen: Enters fullscreen mode immediately after starting.\n");
  std::fprintf(stderr, "  -nofullscreen: Prevents fullscreen mode from triggering if enabled.\n");
  std::fprintf(stderr, "  -nogui: Disables main window from being shown, exits on shutdown.\n");
  std::fprintf(stderr, "  -bigpicture: Automatically starts big picture UI.\n");
  std::fprintf(stderr, "  -portable: Forces \"portable mode\", data in same directory.\n");
  std::fprintf(stderr, "  -settings <filename>: Loads a custom settings configuration from the\n"
                       "    specified filename. Default settings applied if file not found.\n");
  std::fprintf(stderr, "  -earlyconsole: Creates console as early as possible, for logging.\n");
#ifdef ENABLE_RAINTEGRATION
  std::fprintf(stderr, "  -raintegration: Use RAIntegration instead of built-in achievement support.\n");
#endif
  std::fprintf(stderr, "  --: Signals that no more arguments will follow and the remaining\n"
                       "    parameters make up the filename. Use when the filename contains\n"
                       "    spaces or starts with a dash.\n");
  std::fprintf(stderr, "\n");
}

std::shared_ptr<SystemBootParameters>& AutoBoot(std::shared_ptr<SystemBootParameters>& autoboot)
{
  if (!autoboot)
    autoboot = std::make_shared<SystemBootParameters>();

  return autoboot;
}

bool QtHost::ParseCommandLineParametersAndInitializeConfig(QApplication& app,
                                                           std::shared_ptr<SystemBootParameters>& autoboot)
{
  const QStringList args(app.arguments());
  std::optional<s32> state_index;
  std::string settings_filename;
  bool starting_bios = false;

  bool no_more_args = false;

  for (qsizetype i = 1; i < args.size(); i++)
  {
    if (!no_more_args)
    {
#define CHECK_ARG(str) (args[i] == QStringLiteral(str))
#define CHECK_ARG_PARAM(str) (args[i] == QStringLiteral(str) && ((i + 1) < args.size()))

      if (CHECK_ARG("-help"))
      {
        PrintCommandLineHelp(args[0].toUtf8().constData());
        return false;
      }
      else if (CHECK_ARG("-version"))
      {
        PrintCommandLineVersion();
        return false;
      }
      else if (CHECK_ARG("-batch"))
      {
        INFO_LOG("Command Line: Using batch mode.");
        s_batch_mode = true;
        continue;
      }
      else if (CHECK_ARG("-nogui"))
      {
        INFO_LOG("Command Line: Using NoGUI mode.");
        s_nogui_mode = true;
        s_batch_mode = true;
        continue;
      }
      else if (CHECK_ARG("-bios"))
      {
        INFO_LOG("Command Line: Starting BIOS.");
        AutoBoot(autoboot);
        starting_bios = true;
        continue;
      }
      else if (CHECK_ARG("-fastboot"))
      {
        INFO_LOG("Command Line: Forcing fast boot.");
        AutoBoot(autoboot)->override_fast_boot = true;
        continue;
      }
      else if (CHECK_ARG("-slowboot"))
      {
        INFO_LOG("Command Line: Forcing slow boot.");
        AutoBoot(autoboot)->override_fast_boot = false;
        continue;
      }
      else if (CHECK_ARG("-resume"))
      {
        state_index = -1;
        INFO_LOG("Command Line: Loading resume state.");
        continue;
      }
      else if (CHECK_ARG_PARAM("-state"))
      {
        state_index = args[++i].toInt();
        INFO_LOG("Command Line: Loading state index: {}", state_index.value());
        continue;
      }
      else if (CHECK_ARG_PARAM("-statefile"))
      {
        AutoBoot(autoboot)->save_state = args[++i].toStdString();
        INFO_LOG("Command Line: Loading state file: '{}'", autoboot->save_state);
        continue;
      }
      else if (CHECK_ARG_PARAM("-exe"))
      {
        AutoBoot(autoboot)->override_exe = args[++i].toStdString();
        INFO_LOG("Command Line: Overriding EXE file: '{}'", autoboot->override_exe);
        continue;
      }
      else if (CHECK_ARG("-fullscreen"))
      {
        INFO_LOG("Command Line: Using fullscreen.");
        AutoBoot(autoboot)->override_fullscreen = true;
        s_start_fullscreen_ui_fullscreen = true;
        continue;
      }
      else if (CHECK_ARG("-nofullscreen"))
      {
        INFO_LOG("Command Line: Not using fullscreen.");
        AutoBoot(autoboot)->override_fullscreen = false;
        continue;
      }
      else if (CHECK_ARG("-portable"))
      {
        INFO_LOG("Command Line: Using portable mode.");
        EmuFolders::DataRoot = EmuFolders::AppRoot;
        continue;
      }
      else if (CHECK_ARG_PARAM("-settings"))
      {
        settings_filename = args[++i].toStdString();
        INFO_LOG("Command Line: Overriding settings filename: {}", settings_filename);
        continue;
      }
      else if (CHECK_ARG("-bigpicture"))
      {
        INFO_LOG("Command Line: Starting big picture mode.");
        s_start_fullscreen_ui = true;
        continue;
      }
      else if (CHECK_ARG("-setupwizard"))
      {
        s_run_setup_wizard = true;
        continue;
      }
      else if (CHECK_ARG("-earlyconsole"))
      {
        InitializeEarlyConsole();
        continue;
      }
      else if (CHECK_ARG("-updatecleanup"))
      {
        s_cleanup_after_update = AutoUpdaterDialog::isSupported();
        continue;
      }
#ifdef ENABLE_RAINTEGRATION
      else if (CHECK_ARG("-raintegration"))
      {
        Achievements::SwitchToRAIntegration();
        continue;
      }
#endif
      else if (CHECK_ARG("--"))
      {
        no_more_args = true;
        continue;
      }
      else if (args[i][0] == QChar('-'))
      {
        QMessageBox::critical(nullptr, QStringLiteral("Error"), QStringLiteral("Unknown parameter: %1").arg(args[i]));
        return false;
      }

#undef CHECK_ARG
#undef CHECK_ARG_PARAM
    }

    if (autoboot && !autoboot->filename.empty())
      autoboot->filename += ' ';
    AutoBoot(autoboot)->filename += args[i].toStdString();
  }

  // To do anything useful, we need the config initialized.
  if (!InitializeConfig(std::move(settings_filename)))
  {
    // NOTE: No point translating this, because no config means the language won't be loaded anyway.
    QMessageBox::critical(nullptr, QStringLiteral("Error"), QStringLiteral("Failed to initialize config."));
    return false;
  }

  // Check the file we're starting actually exists.

  if (autoboot && !autoboot->filename.empty() && !FileSystem::FileExists(autoboot->filename.c_str()))
  {
    QMessageBox::critical(
      nullptr, qApp->translate("QtHost", "Error"),
      qApp->translate("QtHost", "File '%1' does not exist.").arg(QString::fromStdString(autoboot->filename)));
    return false;
  }

  if (state_index.has_value())
  {
    AutoBoot(autoboot);

    if (autoboot->filename.empty())
    {
      // loading global state, -1 means resume the last game
      if (state_index.value() < 0)
        autoboot->save_state = System::GetMostRecentResumeSaveStatePath();
      else
        autoboot->save_state = System::GetGlobalSaveStateFileName(state_index.value());
    }
    else
    {
      // loading game state
      const std::string game_serial(GameDatabase::GetSerialForPath(autoboot->filename.c_str()));
      autoboot->save_state = System::GetGameSaveStateFileName(game_serial, state_index.value());
    }

    if (autoboot->save_state.empty() || !FileSystem::FileExists(autoboot->save_state.c_str()))
    {
      QMessageBox::critical(nullptr, qApp->translate("QtHost", "Error"),
                            qApp->translate("QtHost", "The specified save state does not exist."));
      return false;
    }
  }

  // check autoboot parameters, if we set something like fullscreen without a bios
  // or disc, we don't want to actually start.
  if (autoboot && autoboot->filename.empty() && autoboot->save_state.empty() && !starting_bios)
    autoboot.reset();

  // if we don't have autoboot, we definitely don't want batch mode (because that'll skip
  // scanning the game list).
  if (s_batch_mode && !autoboot && !s_start_fullscreen_ui)
  {
    QMessageBox::critical(
      nullptr, qApp->translate("QtHost", "Error"),
      s_nogui_mode ? qApp->translate("QtHost", "Cannot use no-gui mode, because no boot filename was specified.") :
                     qApp->translate("QtHost", "Cannot use batch mode, because no boot filename was specified."));
    return false;
  }

  return true;
}

bool QtHost::RunSetupWizard()
{
  SetupWizardDialog dialog;
  if (dialog.exec() == QDialog::Rejected)
    return false;

  // Remove the flag.
  Host::SetBaseBoolSettingValue("Main", "SetupWizardIncomplete", false);
  Host::CommitBaseSettingChanges();
  return true;
}

int main(int argc, char* argv[])
{
  CrashHandler::Install(&Bus::CleanupMemoryMap);

  QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

  QApplication app(argc, argv);
  if (!QtHost::PerformEarlyHardwareChecks())
    return EXIT_FAILURE;

  // Type registration has to happen after hardware checks, clang emits ptest instructions otherwise.
  QtHost::RegisterTypes();

  std::shared_ptr<SystemBootParameters> autoboot;
  if (!QtHost::ParseCommandLineParametersAndInitializeConfig(app, autoboot))
    return EXIT_FAILURE;

  if (!AutoUpdaterDialog::warnAboutUnofficialBuild())
    return EXIT_FAILURE;

  if (!QtHost::EarlyProcessStartup())
    return EXIT_FAILURE;

  // Remove any previous-version remanants.
  if (s_cleanup_after_update)
    AutoUpdaterDialog::cleanupAfterUpdate();

  // Set theme before creating any windows.
  QtHost::UpdateApplicationTheme();

  // Start logging early.
  LogWindow::updateSettings();

  // Start up the CPU thread.
  QtHost::HookSignals();
  EmuThread::start();

  // Optionally run setup wizard.
  MainWindow* main_window;
  int result;
  if (s_run_setup_wizard && !QtHost::RunSetupWizard())
  {
    result = EXIT_FAILURE;
    goto shutdown_and_exit;
  }

  // Create all window objects, the emuthread might still be starting up at this point.
  main_window = new MainWindow();

  // When running in batch mode, ensure game list is loaded, but don't scan for any new files.
  if (!s_batch_mode)
    main_window->refreshGameList(false);
  else
    GameList::Refresh(false, true);

  // Don't bother showing the window in no-gui mode.
  if (!s_nogui_mode)
    main_window->show();

  // Initialize big picture mode if requested.
  if (s_start_fullscreen_ui)
    g_emu_thread->startFullscreenUI();
  else
    s_start_fullscreen_ui_fullscreen = false;

  // Skip the update check if we're booting a game directly.
  if (autoboot)
    g_emu_thread->bootSystem(std::move(autoboot));
  else if (!s_nogui_mode)
    main_window->startupUpdateCheck();

  // This doesn't return until we exit.
  result = app.exec();

shutdown_and_exit:
  // Shutting down.
  EmuThread::stop();

  // Close main window.
  if (g_main_window)
  {
    g_main_window->close();
    delete g_main_window;
    Assert(!g_main_window);
  }

  // Ensure settings are saved.
  if (s_settings_save_timer)
  {
    s_settings_save_timer.reset();
    QtHost::SaveSettings();
  }

  // Ensure log is flushed.
  Log::SetFileOutputParams(false, nullptr);

  System::ProcessShutdown();

  return result;
}
