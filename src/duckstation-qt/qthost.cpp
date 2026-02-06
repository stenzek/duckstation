// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "qthost.h"
#include "autoupdaterdialog.h"
#include "displaywidget.h"
#include "logwindow.h"
#include "mainwindow.h"
#include "qtprogresscallback.h"
#include "qtutils.h"
#include "qtwindowinfo.h"
#include "settingswindow.h"
#include "setupwizarddialog.h"

#include "core/achievements.h"
#include "core/bus.h"
#include "core/cheats.h"
#include "core/controller.h"
#include "core/core.h"
#include "core/core_private.h"
#include "core/fullscreenui.h"
#include "core/fullscreenui_widgets.h"
#include "core/game_database.h"
#include "core/game_list.h"
#include "core/gdb_server.h"
#include "core/gpu.h"
#include "core/gpu_backend.h"
#include "core/gpu_hw_texture_cache.h"
#include "core/host.h"
#include "core/imgui_overlays.h"
#include "core/memory_card.h"
#include "core/performance_counters.h"
#include "core/spu.h"
#include "core/system.h"
#include "core/system_private.h"
#include "core/video_presenter.h"
#include "core/video_thread.h"

#include "common/assert.h"
#include "common/crash_handler.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/minizip_helpers.h"
#include "common/path.h"
#include "common/scoped_guard.h"
#include "common/string_util.h"
#include "common/task_queue.h"
#include "common/threading.h"

#include "util/audio_stream.h"
#include "util/cd_image.h"
#include "util/http_downloader.h"
#include "util/imgui_manager.h"
#include "util/ini_settings_interface.h"
#include "util/input_manager.h"
#include "util/postprocessing.h"
#include "util/translation.h"

#include "scmversion/scmversion.h"

#include "fmt/format.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QTimer>
#include <QtCore/QTranslator>
#include <QtCore/QtLogging>
#include <QtGui/QClipboard>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QMessageBox>
#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>

#ifdef _WIN32
#include <objbase.h> // CoInitializeEx
#endif

#include "moc_qthost.cpp"

using namespace Qt::Literals::StringLiterals;

LOG_CHANNEL(Host);

#if 0
// Mac application menu strings
QT_TRANSLATE_NOOP("MAC_APPLICATION_MENU", "Services")
QT_TRANSLATE_NOOP("MAC_APPLICATION_MENU", "Hide %1")
QT_TRANSLATE_NOOP("MAC_APPLICATION_MENU", "Hide Others")
QT_TRANSLATE_NOOP("MAC_APPLICATION_MENU", "Show All")
QT_TRANSLATE_NOOP("MAC_APPLICATION_MENU", "Preferences...")
QT_TRANSLATE_NOOP("MAC_APPLICATION_MENU", "Quit %1")
QT_TRANSLATE_NOOP("MAC_APPLICATION_MENU", "About %1")
#endif

static constexpr u32 SETTINGS_SAVE_DELAY = 1000;

/// Use two async worker threads, should be enough for most tasks.
static constexpr u32 NUM_ASYNC_WORKER_THREADS = 2;

/// Interval at which the controllers are polled when the system is not active.
static constexpr int BACKGROUND_CONTROLLER_POLLING_INTERVAL_WITH_DEVICES = 100;
static constexpr int BACKGROUND_CONTROLLER_POLLING_INTERVAL_WITHOUT_DEVICES = 1000;

/// Poll at half the vsync rate for FSUI to reduce the chance of getting a press+release in the same frame.
static constexpr int FULLSCREEN_UI_CONTROLLER_POLLING_INTERVAL = 8;

/// Poll at 1ms when running GDB server. We can get rid of this once we move networking to its own thread.
static constexpr int GDB_SERVER_POLLING_INTERVAL = 1;

//////////////////////////////////////////////////////////////////////////
// Local function declarations
//////////////////////////////////////////////////////////////////////////
namespace QtHost {
static bool VeryEarlyProcessStartup();
static bool PerformEarlyHardwareChecks();
static bool EarlyProcessStartup();
static void ProcessShutdown();
static void MessageOutputHandler(QtMsgType type, const QMessageLogContext& context, const QString& msg);
static void RegisterTypes();
static bool InitializeFoldersAndConfig(Error* error);
static QString GetAppIconPath();
static bool LoadResources(Error* error);
static void SaveSettings();
static bool RunSetupWizard();
static void UpdateFontOrder(std::string_view language);
static void UpdateApplicationLocale(std::string_view language);
static std::string_view GetSystemLanguage();
static void InitializeEarlyConsole();
static void HookSignals();
static void PrintCommandLineVersion();
static void PrintCommandLineHelp(const char* progname);
static bool ParseCommandLineParametersAndInitializeConfig(QApplication& app,
                                                          std::shared_ptr<SystemBootParameters>& boot_params);

#ifdef __linux__
static void AdjustQtEnvironmentVariables();
static bool IsRunningOnWayland();
static void ApplyWaylandWorkarounds();
static bool ParseDesktopFileExecPath(const std::string& desktop_file_path, std::string* out_exec_path);
static bool CreateDesktopFile(const std::string& app_path, const std::string& desktop_file_path, Error* error);
static void CheckDesktopFile();
#endif

namespace {
struct State
{
  std::unique_ptr<QTimer> settings_save_timer;
  std::vector<QTranslator*> translators;
  QIcon app_icon;
  QLocale app_locale;
  std::once_flag roboto_font_once_flag;
  QStringList roboto_font_families;
  std::once_flag fixed_font_once_flag;
  QStringList fixed_font_families;
  QFont fixed_font;
  bool batch_mode = false;
  bool nogui_mode = false;
  bool start_fullscreen_ui = false;
  bool start_fullscreen_ui_fullscreen = false;
  bool run_setup_wizard = false;
  bool cleanup_after_update = false;

#ifdef __linux__
  bool wayland_workarounds = false;
#endif
};
} // namespace

ALIGN_TO_CACHE_LINE static State s_state;
ALIGN_TO_CACHE_LINE static TaskQueue s_async_task_queue;

} // namespace QtHost

CoreThread* g_core_thread;

CoreThread::CoreThread()
  : QThread(), m_ui_thread(QThread::currentThread()),
    m_input_device_list_model(std::make_unique<InputDeviceListModel>())
{
  // owned by itself
  moveToThread(this);
}

CoreThread::~CoreThread() = default;

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
  qRegisterMetaType<InputDeviceListModel::Device>("InputDeviceListModel::Device");
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

bool QtHost::VeryEarlyProcessStartup()
{
  CrashHandler::Install(&Bus::CleanupMemoryMap);

#if defined(_WIN32)
  // Ensure COM is initialized before Qt gets a chance to do it, since this could change in the future.
  HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  if (FAILED(hr)) [[unlikely]]
  {
    MessageBoxA(nullptr, fmt::format("CoInitializeEx failed: 0x{:08X}", hr).c_str(), "Error", MB_ICONERROR);
    return false;
  }
#elif defined(__linux__)
  AdjustQtEnvironmentVariables();
#endif

  QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
  return true;
}

bool QtHost::EarlyProcessStartup()
{
  // Redirect Qt errors.
  qInstallMessageHandler(MessageOutputHandler);

  // Set application details.
  // This is critical for Linux to show the correct application name in the task switcher, since it appears
  // to uses the application name to search for desktop files with the corresponding StartupWMClass.
  QApplication::setApplicationName("duckstation-qt"_L1);
  QApplication::setApplicationVersion(QString::fromUtf8(g_scm_version_str));
  QApplication::setOrganizationName("Stenzek"_L1);
  QApplication::setOrganizationDomain("duckstation.org"_L1);
  QApplication::setDesktopFileName("org.duckstation.DuckStation"_L1);

  Error error;
  if (!System::ProcessStartup(&error)) [[unlikely]]
  {
    QMessageBox::critical(nullptr, QStringLiteral("Process Startup Failed"),
                          QString::fromStdString(error.GetDescription()));
    return false;
  }

  // allow us to override standard qt icons as well
  QStringList icon_theme_search_paths = QIcon::themeSearchPaths();
  if (!icon_theme_search_paths.contains(":/icons"_L1))
    icon_theme_search_paths.emplace_back(":/icons"_L1);
  icon_theme_search_paths.emplace_back(":/standard-icons"_L1);
  QIcon::setThemeSearchPaths(icon_theme_search_paths);
  return true;
}

void QtHost::ProcessShutdown()
{
  System::ProcessShutdown();

  // Ensure log is flushed.
  Log::SetFileOutputParams(false, nullptr);

  // Clean up CoInitializeEx() from VeryEarlyProcessStartup().
#ifdef _WIN32
  CoUninitialize();
#endif
}

void QtHost::MessageOutputHandler(QtMsgType type, const QMessageLogContext& context, const QString& msg)
{
  const char* function = context.function ? context.function : "<unknown>";
  const std::string smsg = msg.toStdString();
  switch (type)
  {
    case QtDebugMsg:
      GENERIC_LOG(Log::Channel::Host, Log::Level::Debug, Log::Color::Default, "qDebug({}): {}", function, smsg);
      break;
    case QtInfoMsg:
      GENERIC_LOG(Log::Channel::Host, Log::Level::Info, Log::Color::Default, "qInfo({}): {}", function, smsg);
      break;
    case QtWarningMsg:
      GENERIC_LOG(Log::Channel::Host, Log::Level::Warning, Log::Color::Default, "qWarning({}): {}", function, smsg);
      break;
    case QtCriticalMsg:
      GENERIC_LOG(Log::Channel::Host, Log::Level::Error, Log::Color::Default, "qCritical({}): {}", function, smsg);
      break;
    case QtFatalMsg:
      GENERIC_LOG(Log::Channel::Host, Log::Level::Error, Log::Color::Default, "qFatal({}): {}", function, smsg);
      Y_OnPanicReached(smsg.c_str(), function, context.file ? context.file : "<unknown>", context.line);
    default:
      GENERIC_LOG(Log::Channel::Host, Log::Level::Error, Log::Color::Default, "<unknown>({}): {}", function, smsg);
      break;
  }
}

bool QtHost::InBatchMode()
{
  return s_state.batch_mode;
}

bool QtHost::InNoGUIMode()
{
  return s_state.nogui_mode;
}

bool QtHost::IsDisplayWidgetContainerNeeded()
{
#ifdef __linux__
  return s_state.wayland_workarounds;
#else
  return false;
#endif
}

#ifdef __linux__

void QtHost::AdjustQtEnvironmentVariables()
{
  const char* desktop = std::getenv("XDG_SESSION_DESKTOP");
  if (!desktop)
    return;

  std::fprintf(stderr, "XDG_SESSION_DESKTOP=%s\n", desktop);

  if (std::strcmp(desktop, "KDE") == 0 || std::strcmp(desktop, "GNOME") == 0)
  {
    const char* platform_theme = std::getenv("QT_QPA_PLATFORMTHEME");
    if (platform_theme)
    {
      std::fprintf(stderr, "QT_QPA_PLATFORMTHEME=%s, not overridding\n", platform_theme);
    }
    else
    {
      std::fputs("Enabling xdg-desktop-portal platform theme.\n", stderr);
      setenv("QT_QPA_PLATFORMTHEME", "xdgdesktopportal", true);
    }
  }
}

bool QtHost::IsRunningOnWayland()
{
  const QString platform_name = QGuiApplication::platformName();
  return (platform_name == "wayland"_L1);
}

void QtHost::ApplyWaylandWorkarounds()
{
  if (!IsRunningOnWayland())
  {
    std::fputs("Wayland not detected, not applying workarounds.\n", stderr);
    return;
  }

  if (const char* desktop = std::getenv("XDG_SESSION_DESKTOP"); desktop && std::strcmp(desktop, "KDE") == 0)
  {
    std::fputs("Wayland with KDE detected, not applying Wayland workarounds.\n", stderr);
  }
  else
  {
    std::fputs("Wayland with non-KDE desktop detected, applying Wayland workarounds.\n"
               "Don't complain when things break.\n",
               stderr);

    // When rendering fullscreen or to a separate window on Wayland, because we take over the surface we need
    // to wrap the widget in a container because GNOME is stupid and refuses to ever support server-side
    // decorations. There's no sign of this ever changing. Fuck Wayland.
    s_state.wayland_workarounds = true;

    // On Wayland, turning any window into a native window causes DPI scaling to break, as well as window
    // updates, creating a complete mess of a window. Setting this attribute isn't ideal, since you'd think
    // that setting WA_DontCreateNativeAncestors on the widget would be sufficient, but apparently not.
    QGuiApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings, true);
  }
}

bool QtHost::ParseDesktopFileExecPath(const std::string& desktop_file_path, std::string* out_exec_path)
{
  std::optional<std::string> contents = FileSystem::ReadFileToString(desktop_file_path.c_str(), nullptr);
  if (!contents.has_value())
    return false;

  // Parse line by line looking for Exec=
  std::istringstream stream(contents.value());
  std::string line_str;

  while (std::getline(stream, line_str))
  {
    const std::string_view line = StringUtil::StripWhitespace(line_str);
    if (StringUtil::StartsWithNoCase(line, "Exec="))
    {
      std::string_view exec_value = line.substr(5); // Skip "Exec="

      // The Exec line may have arguments after the path, extract just the path.
      // Handle quoted paths.
      if (!exec_value.empty() && exec_value[0] == '"')
      {
        const size_t end_quote = exec_value.find('"', 1);
        if (end_quote != std::string::npos)
          *out_exec_path = exec_value.substr(1, end_quote - 1);
        else
          *out_exec_path = exec_value.substr(1);
      }
      else
      {
        // Unquoted path - take until first space or end
        const size_t space_pos = exec_value.find(' ');
        if (space_pos != std::string::npos)
          *out_exec_path = exec_value.substr(0, space_pos);
        else
          *out_exec_path = exec_value;
      }

      return true;
    }
  }

  return false;
}

bool QtHost::CreateDesktopFile(const std::string& app_path, const std::string& desktop_file_path, Error* error)
{
  // Ensure the applications directory exists.
  const std::string applications_directory = std::string(Path::GetDirectory(desktop_file_path));
  if (!FileSystem::EnsureDirectoryExists(applications_directory.c_str(), false, error))
    return false;

  // Because the icon is inside the AppImage, we have to copy it as well.
  // This should resolve to $XDG_DATA_DIR/icons or ~/.local/share/icons.
  const std::string icon_file_name = Path::ReplaceExtension(Path::GetFileName(desktop_file_path), "png");
  const std::string icons_directory = Path::Canonicalize(Path::BuildRelativePath(desktop_file_path, "../icons"));
  const std::string icon_path = Path::Combine(icons_directory, icon_file_name);
  if (!FileSystem::FileExists(icon_path.c_str()))
  {
    if (!FileSystem::EnsureDirectoryExists(icons_directory.c_str(), false, error) ||
        !FileSystem::CopyFilePath(GetAppIconPath().toUtf8().constData(), icon_path.c_str(), false, error))
    {
      Error::AddPrefix(error, "Failed to copy application icon: ");
      return false;
    }
  }

  // Create the .desktop file content.
  std::string desktop_content = fmt::format(R"([Desktop Entry]
Type=Application
Name=DuckStation
GenericName=PlayStation 1 Emulator
Comment=Fast PlayStation 1 emulator
Icon={}
Exec="{}" %f
Terminal=false
Categories=Game;Emulator;Qt
StartupNotify=true
StartupWMClass=duckstation-qt
)",
                                            icon_path, app_path, desktop_file_path);

  if (!FileSystem::WriteStringToFile(desktop_file_path.c_str(), desktop_content, error))
    return false;

  // Make the desktop file executable (required by some desktop environments).
  if (!FileSystem::SetPathExecutable(desktop_file_path.c_str(), true, error))
  {
    FileSystem::DeleteFile(desktop_file_path.c_str());
    return false;
  }

  INFO_LOG("Created desktop file at: {}", desktop_file_path);
  return true;
}

void QtHost::CheckDesktopFile()
{
  static constexpr const char* DESKTOP_FILE_NAME = "applications/org.duckstation.DuckStation.desktop";
  static constexpr const char* CONFIG_SECTION = "Main";
  static constexpr const char* CONFIG_KEY = "NoDesktopFile";

  // AppImage sets the APPIMAGE environment variable to the actual executable path.
  // Without this, we'd get the path to the mounted filesystem, which changes on each run.
  const char* appimage_path = std::getenv("APPIMAGE");
  if (!appimage_path || !FileSystem::FileExists(appimage_path))
  {
    ERROR_LOG("Not running from AppImage, skipping desktop file check.");
    return;
  }

  std::string application_path = Path::RealPath(appimage_path);
  if (application_path.empty())
    return;

  std::string desktop_file_path;
  if (const char* xdg_data_home = std::getenv("XDG_DATA_HOME"); xdg_data_home && xdg_data_home[0] != '\0')
  {
    desktop_file_path = Path::Combine(xdg_data_home, DESKTOP_FILE_NAME);
  }
  else
  {
    const char* home = std::getenv("HOME");
    if (!home)
      return;

    desktop_file_path = fmt::format("{}/.local/share/{}", home, DESKTOP_FILE_NAME);
  }

  const auto msgbox_title = "DuckStation"_L1;

  if (!FileSystem::FileExists(desktop_file_path.c_str()))
  {
    // Desktop file doesn't exist - ask if user wants to create it.
    // Only prompt once per installation by storing a flag in settings.
    if (Core::GetBaseBoolSettingValue(CONFIG_SECTION, CONFIG_KEY, false))
      return;

    bool accepted, ignore_future;
    {
      const std::unique_ptr<QMessageBox> msgbox(QtUtils::NewMessageBox(
        nullptr, QMessageBox::Question, msgbox_title,
        qApp
          ->translate("QtHost",
                      "Would you like to create a launcher shortcut for DuckStation?\n\n"
                      "This will add DuckStation to your application menu, allowing you to launch it more easily.\n\n"
                      "The shortcut will be created at:\n%1")
          .arg(QString::fromStdString(desktop_file_path)),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::NoButton, false));
      QCheckBox* const ignore_cb = new QCheckBox(qApp->translate("QtHost", "Don't ask again"), msgbox.get());
      msgbox->setCheckBox(ignore_cb);
      msgbox->setWindowIcon(GetAppIcon());

      accepted = (msgbox->exec() == QMessageBox::Yes);
      ignore_future = ignore_cb->isChecked();
    }

    if (!accepted)
    {
      if (ignore_future)
      {
        Core::SetBaseBoolSettingValue(CONFIG_SECTION, CONFIG_KEY, true);
        QtHost::SaveSettings();
      }

      return;
    }

    Error error;
    if (!CreateDesktopFile(application_path, desktop_file_path, &error))
    {
      QMessageBox::critical(g_main_window, msgbox_title,
                            qApp->translate("QtHost", "Failed to create launcher shortcut shortcut:\n%1")
                              .arg(QString::fromStdString(error.GetDescription())));
    }
    else
    {
      QMessageBox::information(g_main_window, msgbox_title,
                               qApp->translate("QtHost", "Launcher shortcut created successfully.\n\n"
                                                         "You can find DuckStation in your application menu."));
    }
  }
  else
  {
    // Desktop file exists - check if the path matches.
    std::string existing_exec_path;
    if (!ParseDesktopFileExecPath(desktop_file_path, &existing_exec_path))
    {
      WARNING_LOG("Failed to parse existing desktop file: {}", desktop_file_path);
      return;
    }

    // Normalize paths for comparison.
    const std::string normalized_existing = Path::RealPath(existing_exec_path);
    if (application_path == normalized_existing)
      return;

    INFO_LOG("Desktop file path mismatch: current='{}', existing='{}'", application_path, normalized_existing);

    const QMessageBox::StandardButton result = QMessageBox::question(
      g_main_window, "DuckStation"_L1,
      qApp
        ->translate("QtHost", "The existing launcher shortcut points to a different location:\n\n"
                              "Current: %1\n"
                              "Shortcut: %2\n\n"
                              "Would you like to update the shortcut to point to the current location?")
        .arg(QString::fromStdString(application_path))
        .arg(QString::fromStdString(existing_exec_path)),
      QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);

    if (result == QMessageBox::Yes)
    {
      Error error;

      // Delete the old desktop file first.
      if (!FileSystem::DeleteFile(desktop_file_path.c_str(), &error))
      {
        QMessageBox::critical(g_main_window, msgbox_title,
                              qApp->translate("QtHost", "Failed to remove old launcher shortcut:\n%1")
                                .arg(QString::fromStdString(error.GetDescription())));
        return;
      }

      // Create the new desktop file.
      if (!CreateDesktopFile(application_path, desktop_file_path, &error))
      {
        QMessageBox::critical(g_main_window, msgbox_title,
                              qApp->translate("QtHost", "Failed to create updated launcher shortcut:\n%1")
                                .arg(QString::fromStdString(error.GetDescription())));
      }
      else
      {
        QMessageBox::information(g_main_window, msgbox_title,
                                 qApp->translate("QtHost", "Launcher shortcut updated successfully."));
      }
    }
  }
}

#endif // __linux__

QString QtHost::GetAppNameAndVersion()
{
  return QStringLiteral("DuckStation %1").arg(QLatin1StringView(g_scm_version_str));
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

bool QtHost::SaveGameSettings(SettingsInterface* sif, bool delete_if_empty)
{
  INISettingsInterface* ini = static_cast<INISettingsInterface*>(sif);
  Error error;

  // if there's no keys, just toss the whole thing out
  if (delete_if_empty && ini->IsEmpty())
  {
    INFO_LOG("Removing empty gamesettings ini {}", Path::GetFileName(ini->GetPath()));

    // grab the settings lock while we're writing the file, that way the CPU thread doesn't try
    // to read it at the same time.
    const auto lock = Core::GetSettingsLock();

    if (FileSystem::FileExists(ini->GetPath().c_str()) && !FileSystem::DeleteFile(ini->GetPath().c_str(), &error))
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
  const auto lock = Core::GetSettingsLock();

  if (!ini->Save(&error))
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
  return s_state.app_icon;
}

QString QtHost::GetAppIconPath()
{
  return GetResourceQPath("images/duck.png", true);
}

QPixmap QtHost::GetAppLogo()
{
  QPixmap pm(GetAppIconPath());
  pm.setDevicePixelRatio(qApp->devicePixelRatio());
  return pm;
}

void QtHost::DownloadFile(QWidget* parent, std::string url, std::string path,
                          std::function<void(bool result, std::string path, const Error& error)> completion_callback)
{
  INFO_LOG("Download from {}, saving to {}.", url, path);

  const std::string::size_type url_file_part_pos = url.rfind('/');
  const std::string status_text =
    fmt::format(TRANSLATE_FS("QtHost", "Downloading {}..."),
                std::string_view(url).substr((url_file_part_pos >= 0) ? (url_file_part_pos + 1) : 0));

  QtAsyncTaskWithProgressDialog::create(
    parent, TRANSLATE_SV("QtHost", "File Download"), status_text, false, true, 0, 0, 0.0f, true,
    [url = std::move(url), path = std::move(path),
     completion_callback = std::move(completion_callback)](ProgressCallback* const progress) mutable {
      Error error;
      std::unique_ptr<HTTPDownloader> http = HTTPDownloader::Create(Core::GetHTTPUserAgent(), &error);
      bool result;
      if ((result = static_cast<bool>(http)))
      {
        http->CreateRequest(
          std::move(url),
          [&result, &error, &path](s32 status_code, const Error& http_error, const std::string&,
                                   std::vector<u8> hdata) {
            if (status_code != HTTPDownloader::HTTP_STATUS_OK)
            {
              error.SetString(http_error.GetDescription());
              return;
            }
            else if (hdata.empty())
            {
              error.SetStringView(TRANSLATE_SV("QtHost", "Download failed: Data is empty."));
              return;
            }

            result = FileSystem::WriteBinaryFile(path.c_str(), hdata, &error);
          },
          progress);

        // Block until completion.
        http->WaitForAllRequests();
      }

      QtAsyncTaskWithProgressDialog::CompletionCallback ret;
      if (completion_callback)
      {
        ret = [path = std::move(path), completion_callback = std::move(completion_callback), error = std::move(error),
               result]() mutable { completion_callback(result, std::move(path), error); };
      }
      else
      {
        ret = []() {};
      }

      return ret;
    });
}

bool QtHost::InitializeFoldersAndConfig(Error* error)
{
  // Path to the resources directory relative to the application binary.
// On Windows/Linux, these are in the binary directory.
// On macOS, this is in the bundle resources directory.
#ifndef __APPLE__
  static constexpr const char* RESOURCES_RELATIVE_PATH = "resources";
#else
  static constexpr const char* RESOURCES_RELATIVE_PATH = "../Resources";
#endif

  if (!Core::SetCriticalFolders(RESOURCES_RELATIVE_PATH, error))
    return false;

  Error config_error;
  if (!Core::InitializeBaseSettingsLayer(Core::GetBaseSettingsPath(), &config_error))
  {
    if (QMessageBox::question(
          nullptr, "DuckStation"_L1,
          "Failed to load configuration. The error was:\n\n%1\n\nThe settings file may be corrupted. Do you want to "
          "delete the settings file and try again? Note that any currently-configured settings will be lost."_L1.arg(
            QString::fromStdString(config_error.GetDescription()))) == QMessageBox::Yes)
    {
      if (!FileSystem::DeleteFile(Core::GetBaseSettingsPath().c_str(), &config_error))
      {
        QMessageBox::critical(nullptr, QStringLiteral("DuckStation"),
                              QStringLiteral("Failed to delete settings file:\n\n%1")
                                .arg(QString::fromStdString(config_error.GetDescription())));
      }
    }

    // Try again after deleting.
    if (!Core::InitializeBaseSettingsLayer(Core::GetBaseSettingsPath(), &config_error))
    {
      Error::SetStringFmt(error,
                          "Failed to load configuration. The error was:\n\n{}\n\nPlease ensure that the data directory "
                          "is writable. The data directory is located at:\n\n{}\n\nYou can also try portable mode by "
                          "creating portable.txt in the same directory you installed DuckStation into.",
                          config_error.GetDescription(), EmuFolders::DataRoot);
      return false;
    }
  }

  // Very old installations pre-setup-wizard won't have the "SetupWizardIncomplete" key.
  // Instead, we rely on "SettingsVersion" there as a signal that setup has been completed.
  if (!Core::ContainsBaseSettingValue("Main", "SetupWizardIncomplete") &&
      !Core::ContainsBaseSettingValue("Main", "SettingsVersion"))
  {
    // Flag for running the setup wizard if this is our first run. We want to run it next time if they don't finish it.
    Core::SetBaseBoolSettingValue("Main", "SetupWizardIncomplete", true);
  }

  // Setup wizard was incomplete last time?
  s_state.run_setup_wizard =
    s_state.run_setup_wizard || Core::GetBaseBoolSettingValue("Main", "SetupWizardIncomplete", false);

  UpdateApplicationLanguage(nullptr);
  return true;
}

bool QtHost::LoadResources(Error* error)
{
  // the resources directory should exist, bail out if not
  const std::string rcc_path = Path::Combine(EmuFolders::Resources, "duckstation-qt.rcc");
  if (!FileSystem::FileExists(rcc_path.c_str()) || !QResource::registerResource(QString::fromStdString(rcc_path)))
  {
    Error::SetStringFmt(error,
                        "{} could not be loaded. Your installation is not complete. Please delete and re-download the "
                        "application from https://www.duckstation.org/.",
                        Path::GetFileName(rcc_path));
    return false;
  }

  s_state.app_icon = QIcon(GetAppIconPath());
  return true;
}

void Host::LoadSettings(const SettingsInterface& si, std::unique_lock<std::mutex>& lock)
{
}

void Host::CheckForSettingsChanges(const Settings& old_settings)
{
  // NOTE: emu thread, push to UI thread
  if (g_main_window)
    QMetaObject::invokeMethod(g_main_window, &MainWindow::checkForSettingChanges, Qt::QueuedConnection);
}

void CoreThread::setDefaultSettings(bool host, bool system, bool controller)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &CoreThread::setDefaultSettings, Qt::QueuedConnection, host, system, controller);
    return;
  }

  Core::SetDefaultSettings(host, system, controller);
}

void Host::SetDefaultSettings(SettingsInterface& si)
{
#if defined(_WIN32)
  si.SetBoolValue("Main", "DisableWindowRoundedCorners", false);
#elif defined(__APPLE__)
  si.SetBoolValue("Main", "UseFractionalWindowScale", false);
#endif

  si.SetBoolValue("Main", "DisableWindowResize", false);
  si.SetBoolValue("Main", "HideCursorInFullscreen", false);
  si.SetBoolValue("Main", "RenderToSeparateWindow", false);
  si.SetBoolValue("Main", "HideMainWindowWhenRunning", false);

  // TODO: We could include stuff like game list here, but meh...
}

void Host::OnSettingsResetToDefault(bool host, bool system, bool controller)
{
  g_core_thread->applySettings(false);

  if (system)
    emit g_core_thread->settingsResetToDefault(host, system, controller);
}

void Host::RequestResizeHostDisplay(s32 new_window_width, s32 new_window_height)
{
  emit g_core_thread->onResizeRenderWindowRequested(new_window_width, new_window_height);
}

void CoreThread::applySettings(bool display_osd_messages /* = false */)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &CoreThread::applySettings, Qt::QueuedConnection, display_osd_messages);
    return;
  }

  System::ApplySettings(display_osd_messages);
}

void CoreThread::reloadGameSettings(bool display_osd_messages /* = false */)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &CoreThread::reloadGameSettings, Qt::QueuedConnection, display_osd_messages);
    return;
  }

  System::ReloadGameSettings(display_osd_messages);
}

void CoreThread::reloadInputProfile(bool display_osd_messages /*= false*/)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &CoreThread::reloadInputProfile, Qt::QueuedConnection, display_osd_messages);
    return;
  }

  System::ReloadInputProfile(display_osd_messages);
}

void CoreThread::updateEmuFolders()
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &CoreThread::updateEmuFolders, Qt::QueuedConnection);
    return;
  }

  EmuFolders::Update();
}

void CoreThread::updateControllerSettings()
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &CoreThread::updateControllerSettings, Qt::QueuedConnection);
    return;
  }

  if (!System::IsValid())
    return;

  System::UpdateControllerSettings();
}

void CoreThread::startFullscreenUI()
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &CoreThread::startFullscreenUI, Qt::QueuedConnection);
    return;
  }

  if (System::IsValid() || VideoThread::IsFullscreenUIRequested())
    return;

  // we want settings loaded so we choose the correct renderer
  // this also sorts out input sources.
  System::LoadSettings(false);

  // borrow the game start fullscreen flag
  const bool start_fullscreen =
    (QtHost::s_state.start_fullscreen_ui_fullscreen || Core::GetBaseBoolSettingValue("Main", "StartFullscreen", false));

  m_is_fullscreen_ui_started = true;
  emit fullscreenUIStartedOrStopped(true);

  Error error;
  if (!VideoThread::StartFullscreenUI(start_fullscreen, &error))
  {
    Host::ReportErrorAsync("Error", error.GetDescription());
    m_is_fullscreen_ui_started = false;
    emit fullscreenUIStartedOrStopped(false);
    return;
  }
}

void CoreThread::stopFullscreenUI()
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &CoreThread::stopFullscreenUI, Qt::QueuedConnection);

    // if we still have a system, don't wait
    if (QtHost::IsSystemValid())
      return;

    // wait until the host display is gone
    QtUtils::ProcessEventsWithSleep(QEventLoop::ExcludeUserInputEvents, []() {
      return QtHost::IsFullscreenUIStarted() || g_main_window->hasDisplayWidget();
    });
    return;
  }

  if (m_is_fullscreen_ui_started)
  {
    m_is_fullscreen_ui_started = false;
    emit fullscreenUIStartedOrStopped(false);
    VideoThread::StopFullscreenUI();
  }
}

void CoreThread::exitFullscreenUI()
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &CoreThread::exitFullscreenUI, Qt::QueuedConnection);
    return;
  }

  const bool was_in_nogui_mode = std::exchange(QtHost::s_state.nogui_mode, false);

  stopFullscreenUI();

  if (was_in_nogui_mode)
  {
    Host::RunOnUIThread([]() {
      // Restore the geometry of the main window, since the display window may have been moved.
      QtUtils::RestoreWindowGeometry(g_main_window);

      // if we were in nogui mode, the game list won't have been populated yet. do it now.
      g_main_window->refreshGameList(false);
    });
  }
}

void CoreThread::bootSystem(std::shared_ptr<SystemBootParameters> params)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &CoreThread::bootSystem, Qt::QueuedConnection, std::move(params));
    return;
  }

  // Just in case of rapid clicking games before it gets the chance to start.
  if (System::IsValidOrInitializing())
    return;

  Error error;
  if (!System::BootSystem(std::move(*params), &error))
  {
    emit errorReported(tr("Error"),
                       tr("Failed to boot system: %1").arg(QString::fromStdString(error.GetDescription())));
  }
}

void CoreThread::bootOrSwitchNonDisc(const QString& path)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &CoreThread::bootOrSwitchNonDisc, Qt::QueuedConnection, path);
    return;
  }

  std::string path_str = path.toStdString();

  // No system -> just boot it.
  if (!System::IsValid())
  {
    bootSystem(std::make_shared<SystemBootParameters>(std::move(path_str)));
    return;
  }

  const bool is_gpu_dump = System::IsGPUDumpPath(path_str);
  if (is_gpu_dump != System::IsReplayingGPUDump())
  {
    emit errorReported(tr("Error"), tr("Cannot change GPU dump state without restarting the system."));
    return;
  }

  if (is_gpu_dump)
  {
    System::ChangeGPUDump(std::move(path_str));
  }
  else
  {
    // Change override and reset.
    System::ChangeExeOverrideAndReset(std::move(path_str));
    System::ResetSystem();
  }
}

void CoreThread::bootOrLoadState(std::string path)
{
  DebugAssert(isCurrentThread());

  if (System::IsValid())
  {
    Error error;
    if (!System::LoadState(path.c_str(), &error, true, false).value_or(true))
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

void CoreThread::onRenderWindowKeyEvent(int key, bool pressed)
{
  DebugAssert(isCurrentThread());

  InputManager::InvokeEvents(InputManager::MakeHostKeyboardKey(key), static_cast<float>(pressed),
                             GenericInputBinding::Unknown);
}

void CoreThread::onRenderWindowTextEntered(const QString& text)
{
  DebugAssert(isCurrentThread());

  ImGuiManager::AddTextInput(text.toStdString());
}

void CoreThread::onRenderWindowMouseMoveAbsoluteEvent(float x, float y)
{
  InputManager::UpdatePointerAbsolutePosition(0, x, y);
}

void CoreThread::onRenderWindowMouseMoveRelativeEvent(float dx, float dy)
{
  if (dx != 0.0f)
    InputManager::UpdatePointerPositionRelativeDelta(0, InputPointerAxis::X, dx);
  if (dy != 0.0f)
    InputManager::UpdatePointerPositionRelativeDelta(0, InputPointerAxis::Y, dy);
}

void CoreThread::onRenderWindowMouseButtonEvent(int button, bool pressed)
{
  DebugAssert(isCurrentThread());

  InputManager::InvokeEvents(InputManager::MakePointerButtonKey(0, button), static_cast<float>(pressed),
                             GenericInputBinding::Unknown);
}

void CoreThread::onRenderWindowMouseWheelEvent(float dx, float dy)
{
  DebugAssert(isCurrentThread());

  if (dx != 0.0f)
    InputManager::UpdatePointerWheelRelativeDelta(0, InputPointerAxis::WheelX, dx);

  if (dy != 0.0f)
    InputManager::UpdatePointerWheelRelativeDelta(0, InputPointerAxis::WheelY, dy);
}

void CoreThread::onRenderWindowResized(int width, int height, float scale, float refresh_rate)
{
  VideoThread::ResizeRenderWindow(width, height, scale, refresh_rate);
}

void CoreThread::redrawRenderWindow()
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &CoreThread::redrawRenderWindow, Qt::QueuedConnection);
    return;
  }

  if (System::IsShutdown())
    return;

  VideoThread::PresentCurrentFrame();
}

void CoreThread::toggleFullscreen()
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &CoreThread::toggleFullscreen, Qt::QueuedConnection);
    return;
  }

  VideoThread::SetFullscreen(!VideoThread::IsFullscreen());
}

void CoreThread::setFullscreen(bool fullscreen)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &CoreThread::setFullscreen, Qt::QueuedConnection, fullscreen);
    return;
  }

  VideoThread::SetFullscreen(fullscreen);
}

void CoreThread::setFullscreenWithCompletionHandler(bool fullscreen, std::function<void()> completion_handler)
{
  if (!isCurrentThread())
  {
    DebugAssert(QThread::isMainThread());
    QMetaObject::invokeMethod(this, &CoreThread::setFullscreenWithCompletionHandler, Qt::QueuedConnection, fullscreen,
                              [completion_handler = std::move(completion_handler)]() mutable {
                                Host::RunOnUIThread(std::move(completion_handler));
                              });
    return;
  }

  VideoThread::SetFullscreenWithCompletionHandler(fullscreen, std::move(completion_handler));
}

void CoreThread::recreateRenderWindow()
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &CoreThread::recreateRenderWindow, Qt::QueuedConnection);
    return;
  }

  VideoThread::RecreateRenderWindow();
}

void CoreThread::requestDisplaySize(float scale)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &CoreThread::requestDisplaySize, Qt::QueuedConnection, scale);
    return;
  }

  if (!System::IsValid())
    return;

  System::RequestDisplaySize(scale);
}

std::optional<WindowInfo> CoreThread::acquireRenderWindow(RenderAPI render_api, bool fullscreen,
                                                          bool exclusive_fullscreen, Error* error)
{
  return emit onAcquireRenderWindowRequested(render_api, fullscreen, exclusive_fullscreen, error);
}

void CoreThread::releaseRenderWindow()
{
  emit onReleaseRenderWindowRequested();
}

void CoreThread::connectRenderWindowSignals(DisplayWidget* widget)
{
  widget->disconnect(this);

  connect(widget, &DisplayWidget::windowResizedEvent, this, &CoreThread::onRenderWindowResized);
  connect(widget, &DisplayWidget::windowRestoredEvent, this, &CoreThread::redrawRenderWindow);
  connect(widget, &DisplayWidget::windowKeyEvent, this, &CoreThread::onRenderWindowKeyEvent);
  connect(widget, &DisplayWidget::windowTextEntered, this, &CoreThread::onRenderWindowTextEntered);
  connect(widget, &DisplayWidget::windowMouseMoveAbsoluteEvent, this,
          &CoreThread::onRenderWindowMouseMoveAbsoluteEvent);
  connect(widget, &DisplayWidget::windowMouseMoveRelativeEvent, this,
          &CoreThread::onRenderWindowMouseMoveRelativeEvent);
  connect(widget, &DisplayWidget::windowMouseButtonEvent, this, &CoreThread::onRenderWindowMouseButtonEvent);
  connect(widget, &DisplayWidget::windowMouseWheelEvent, this, &CoreThread::onRenderWindowMouseWheelEvent);
}

void Host::OnSystemStarting()
{
  emit g_core_thread->systemStarting();
}

void Host::OnSystemStarted()
{
  g_core_thread->stopBackgroundControllerPollTimer();
  g_core_thread->wakeThread();

  emit g_core_thread->systemStarted();
}

void Host::OnSystemPaused()
{
  emit g_core_thread->systemPaused();
  g_core_thread->startBackgroundControllerPollTimer();
}

void Host::OnSystemResumed()
{
  emit g_core_thread->systemResumed();
  g_core_thread->wakeThread();

  g_core_thread->stopBackgroundControllerPollTimer();
}

void Host::OnSystemStopping()
{
  emit g_core_thread->systemStopping();
}

void Host::OnSystemDestroyed()
{
  g_core_thread->resetPerformanceCounters();
  g_core_thread->startBackgroundControllerPollTimer();
  emit g_core_thread->systemDestroyed();
}

void Host::OnSystemAbnormalShutdown(const std::string_view reason)
{
  Host::ReportErrorAsync(
    TRANSLATE_SV("QtHost", "Error"),
    fmt::format(
      TRANSLATE_FS("QtHost",
                   "Unfortunately, the virtual machine has abnormally shut down and cannot be recovered. Please use "
                   "the available support options for further assistance, and provide information about what you were "
                   "doing when the error occurred, as well as the details below:\n\n{}"),
      reason));
}

void Host::OnVideoThreadRunIdleChanged(bool is_active)
{
  g_core_thread->setVideoThreadRunIdle(is_active);
}

void CoreThread::reloadInputBindings()
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &CoreThread::reloadInputBindings, Qt::QueuedConnection);
    return;
  }

  System::ReloadInputBindings();
}

void CoreThread::confirmActionWithSafetyCheck(const QString& action, bool check_achievements,
                                              bool cancel_resume_on_accept, std::function<void(bool)> callback) const
{
  DebugAssert(isCurrentThread());
  if (!System::IsValid())
  {
    callback(true);
    return;
  }

  const bool saving = System::IsSavingMemoryCards();
  const u32 pending_unlock_count = Achievements::GetPendingUnlockCount();
  if (!saving && pending_unlock_count == 0)
  {
    callback(true);
    return;
  }

  Host::RunOnUIThread(
    [callback = std::move(callback), action, saving, pending_unlock_count, cancel_resume_on_accept]() mutable {
      auto lock = g_main_window->pauseAndLockSystem();

      bool result;
      if (saving)
      {
        result = (QtUtils::MessageBoxIcon(
                    lock.getDialogParent(), QMessageBox::Warning, tr("Memory Card Busy"),
                    tr("WARNING: Your game is still saving to the memory card. Continuing to %1 may "
                       "IRREVERSIBLY DESTROY YOUR MEMORY CARD. We recommend resuming your game and waiting 5 "
                       "seconds for it to finish saving.\n\nDo you want to %1 anyway?")
                      .arg(action),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::No);
      }
      else
      {
        result = (QtUtils::MessageBoxIcon(
                    lock.getDialogParent(), QMessageBox::Warning, tr("Achievement Unlocks Unconfirmed"),
                    tr("%1 achievement unlocks have not been confirmed by the server. Continuing to %2 will result in "
                       "loss of these unlocks. Once network connectivity has been re-established, these unlocks will "
                       "be confirmed automatically.\n\nDo you want to %2 anyway?")
                      .arg(pending_unlock_count)
                      .arg(action),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::No);
      }

      if (cancel_resume_on_accept && !QtHost::IsFullscreenUIStarted())
        lock.cancelResume();

      Host::RunOnCoreThread([result, callback = std::move(callback)]() { callback(result); });
    });
}

void CoreThread::shutdownSystem(bool save_state, bool check_safety)
{
  if (!isCurrentThread())
  {
    System::CancelPendingStartup();
    QMetaObject::invokeMethod(this, &CoreThread::shutdownSystem, Qt::QueuedConnection, save_state, check_safety);
    return;
  }

  if (check_safety && (System::IsSavingMemoryCards() || Achievements::GetPendingUnlockCount() > 0))
  {
    confirmActionWithSafetyCheck(tr("shut down"), true, true, [save_state](bool result) {
      if (result)
        g_core_thread->shutdownSystem(save_state, false);
      else
        g_core_thread->setSystemPaused(false);
    });
    return;
  }

  System::ShutdownSystem(save_state);
}

void CoreThread::resetSystem(bool check_memcard_busy)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &CoreThread::resetSystem, Qt::QueuedConnection, check_memcard_busy);
    return;
  }

  if (check_memcard_busy && System::IsSavingMemoryCards())
  {
    confirmActionWithSafetyCheck(tr("reset"), false, false, [](bool result) {
      if (result)
        g_core_thread->resetSystem(false);
    });
    return;
  }

  System::ResetSystem();
}

void CoreThread::setSystemPaused(bool paused)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &CoreThread::setSystemPaused, Qt::QueuedConnection, paused);
    return;
  }

  System::PauseSystem(paused);
}

void CoreThread::changeDisc(const QString& new_disc_filename, bool reset_system, bool check_memcard_busy)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &CoreThread::changeDisc, Qt::QueuedConnection, new_disc_filename, reset_system,
                              check_memcard_busy);
    return;
  }

  if (check_memcard_busy && System::IsSavingMemoryCards())
  {
    confirmActionWithSafetyCheck(tr("change disc"), false, false, [new_disc_filename, reset_system](bool result) {
      if (result)
        g_core_thread->changeDisc(new_disc_filename, reset_system, false);
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

void CoreThread::changeDiscFromPlaylist(quint32 index)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &CoreThread::changeDiscFromPlaylist, Qt::QueuedConnection, index);
    return;
  }

  if (System::IsShutdown())
    return;

  if (!System::SwitchMediaSubImage(index))
    errorReported(tr("Error"), tr("Failed to switch to subimage %1").arg(index));
}

void CoreThread::reloadCheats(bool reload_files, bool reload_enabled_list, bool verbose, bool verbose_if_changed)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &CoreThread::reloadCheats, Qt::QueuedConnection, reload_files, reload_enabled_list,
                              verbose, verbose_if_changed);
    return;
  }

  if (System::IsValid())
  {
    // If the reloaded list is being enabled, we also need to reload the gameini file.
    if (reload_enabled_list)
      System::ReloadGameSettings(verbose);
    Cheats::ReloadCheats(reload_files, reload_enabled_list, verbose, verbose_if_changed, verbose);
  }
}

void CoreThread::applyCheat(const QString& name)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &CoreThread::applyCheat, Qt::QueuedConnection, name);
    return;
  }

  if (System::IsValid())
    Cheats::ApplyManualCode(name.toStdString());
}

void CoreThread::reloadPostProcessingShaders()
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &CoreThread::reloadPostProcessingShaders, Qt::QueuedConnection);
    return;
  }

  if (System::IsValid())
    VideoPresenter::ReloadPostProcessingSettings(true, true, true);
}

void CoreThread::updatePostProcessingSettings(bool display, bool internal, bool force_reload)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &CoreThread::updatePostProcessingSettings, Qt::QueuedConnection, display, internal,
                              force_reload);
    return;
  }

  if (System::IsValid())
    VideoPresenter::ReloadPostProcessingSettings(display, internal, force_reload);
}

void CoreThread::reloadTextureReplacements()
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &CoreThread::reloadTextureReplacements, Qt::QueuedConnection);
    return;
  }

  if (System::IsValid())
    VideoThread::RunOnThread([]() { GPUTextureCache::ReloadTextureReplacements(true, true); });
}

void CoreThread::captureGPUFrameDump()
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &CoreThread::captureGPUFrameDump, Qt::QueuedConnection);
    return;
  }

  if (System::IsValid())
    System::StartRecordingGPUDump();
}

void CoreThread::startControllerTest()
{
  static constexpr const char* PADTEST_URL =
    "https://github.com/stenzek/duckstation/raw/refs/heads/master/extras/padtest/padtest.psexe";

  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &CoreThread::startControllerTest, Qt::QueuedConnection);
    return;
  }

  if (System::IsValid())
    return;

  std::string path = Path::Combine(EmuFolders::UserResources, "padtest.psexe");
  if (FileSystem::FileExists(path.c_str()))
  {
    bootSystem(std::make_shared<SystemBootParameters>(std::move(path)));
    return;
  }

  Host::RunOnUIThread([path = std::move(path)]() mutable {
    if (QtHost::IsSystemValid())
      return;

    QMessageBox* const msgbox =
      QtUtils::NewMessageBox(g_main_window, QMessageBox::Question, tr("Confirm Download"),
                             tr("Your DuckStation installation does not have the padtest application "
                                "available.\n\nThis file is approximately 206KB, do you want to download it now?"),
                             QMessageBox::Yes | QMessageBox::No);
    msgbox->connect(msgbox, &QMessageBox::accepted, g_main_window, [path = std::move(path)]() mutable {
      QtHost::DownloadFile(
        g_main_window, PADTEST_URL, std::move(path), [](bool result, std::string path, const Error& error) {
          if (!result)
          {
            QtUtils::MessageBoxCritical(
              g_main_window, tr("Error"),
              tr("Failed to download padtest application: %1").arg(QString::fromStdString(error.GetDescription())));
            return;
          }

          g_core_thread->bootSystem(std::make_shared<SystemBootParameters>(std::move(path)));
        });
    });
    msgbox->open();
  });
}

void CoreThread::openGamePropertiesForCurrentGame(const QString& category)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &CoreThread::openGamePropertiesForCurrentGame, Qt::QueuedConnection, category);
    return;
  }

  Error error;
  GameList::Entry dynamic_entry;
  if (System::PopulateGameListEntryFromCurrentGame(&dynamic_entry, &error))
  {
    Host::RunOnUIThread([dynamic_entry = std::move(dynamic_entry), category]() {
      SettingsWindow::openGamePropertiesDialog(&dynamic_entry,
                                               category.isEmpty() ? nullptr : category.toUtf8().constData());
    });
  }
  else
  {
    emit errorReported(tr("Error"), QString::fromStdString(error.GetDescription()));
  }
}

void CoreThread::runOnCoreThread(const std::function<void()>& callback)
{
  callback();
}

void Host::RunOnCoreThread(std::function<void()> function, bool block /* = false */)
{
  const bool self = g_core_thread->isCurrentThread();

  QMetaObject::invokeMethod(g_core_thread, &CoreThread::runOnCoreThread,
                            (block && !self) ? Qt::BlockingQueuedConnection : Qt::QueuedConnection,
                            std::move(function));
}

void Host::RunOnUIThread(std::function<void()> function, bool block /* = false*/)
{
  // main window always exists, so it's fine to attach it to that.
  QMetaObject::invokeMethod(g_main_window, &MainWindow::runOnUIThread,
                            block ? Qt::BlockingQueuedConnection : Qt::QueuedConnection, std::move(function));
}

void Host::QueueAsyncTask(std::function<void()> function)
{
  QtHost::s_async_task_queue.SubmitTask(std::move(function));
}

void Host::WaitForAllAsyncTasks()
{
  QtHost::s_async_task_queue.WaitForAll();
}

QtAsyncTask::QtAsyncTask(WorkCallback callback)
{
  m_callback = std::move(callback);
}

QtAsyncTask::~QtAsyncTask() = default;

void QtAsyncTask::create(QObject* owner, WorkCallback callback)
{
  // NOTE: Must get connected before queuing, because otherwise you risk a race.
  QtAsyncTask* task = new QtAsyncTask(std::move(callback));
  connect(task, &QtAsyncTask::completed, owner, [task]() { std::get<CompletionCallback>(task->m_callback)(); });
  Host::QueueAsyncTask([task]() {
    task->m_callback = std::get<WorkCallback>(task->m_callback)();
    Host::RunOnUIThread([task]() {
      emit task->completed(task);
      delete task;
    });
  });
}

void Host::RefreshGameListAsync(bool invalidate_cache)
{
  QMetaObject::invokeMethod(g_main_window, &MainWindow::refreshGameList, Qt::QueuedConnection, invalidate_cache);
}

void Host::CancelGameListRefresh()
{
  QMetaObject::invokeMethod(g_main_window, &MainWindow::cancelGameListRefresh, Qt::BlockingQueuedConnection);
}

void Host::OnGameListEntriesChanged(std::span<const u32> changed_indices)
{
  QList<int> changed_rows;
  changed_rows.reserve(changed_indices.size());
  for (const u32 row : changed_indices)
    changed_rows.push_back(static_cast<int>(row));
  emit g_core_thread->gameListRowsChanged(changed_rows);
}

void CoreThread::loadState(const QString& path)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, static_cast<void (CoreThread::*)(const QString&)>(&CoreThread::loadState),
                              Qt::QueuedConnection, path);
    return;
  }

  bootOrLoadState(path.toStdString());
}

void CoreThread::loadState(bool global, qint32 slot)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, static_cast<void (CoreThread::*)(bool, qint32)>(&CoreThread::loadState),
                              Qt::QueuedConnection, global, slot);
    return;
  }

  if (System::IsValid())
  {
    System::LoadStateFromSlot(global, slot);
    return;
  }

  bootOrLoadState(global ? System::GetGlobalSaveStatePath(slot) :
                           System::GetGameSaveStatePath(System::GetGameSerial(), slot));
}

void CoreThread::saveState(const QString& path)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, static_cast<void (CoreThread::*)(const QString&)>(&CoreThread::saveState),
                              Qt::QueuedConnection, path);
    return;
  }

  if (!System::IsValid())
    return;

  Error error;
  if (System::SaveState(path.toStdString(), &error, g_settings.create_save_state_backups, false))
    emit statusMessage(tr("State saved to %1.").arg(QFileInfo(path).fileName()));
  else
    emit errorReported(tr("Error"), tr("Failed to save state: %1").arg(QString::fromStdString(error.GetDescription())));
}

void CoreThread::saveState(bool global, qint32 slot)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, static_cast<void (CoreThread::*)(bool, qint32)>(&CoreThread::saveState),
                              Qt::QueuedConnection, global, slot);
    return;
  }

  System::SaveStateToSlot(global, slot);
}

void CoreThread::undoLoadState()
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &CoreThread::undoLoadState, Qt::QueuedConnection);
    return;
  }

  System::UndoLoadState();
}

void CoreThread::setAudioOutputVolume(int volume, int fast_forward_volume)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &CoreThread::setAudioOutputVolume, Qt::QueuedConnection, volume,
                              fast_forward_volume);
    return;
  }

  g_settings.audio_output_volume = static_cast<u8>(volume);
  g_settings.audio_fast_forward_volume = static_cast<u8>(fast_forward_volume);
  System::UpdateVolume();
}

void CoreThread::setAudioOutputMuted(bool muted)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &CoreThread::setAudioOutputMuted, Qt::QueuedConnection, muted);
    return;
  }

  g_settings.audio_output_muted = muted;
  System::UpdateVolume();
}

void CoreThread::singleStepCPU()
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &CoreThread::singleStepCPU, Qt::BlockingQueuedConnection);
    return;
  }

  if (!System::IsValid())
    return;

  System::SingleStepCPU();
}

void CoreThread::dumpRAM(const QString& path)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &CoreThread::dumpRAM, Qt::QueuedConnection, path);
    return;
  }

  Error error;
  const std::string path_str = path.toStdString();
  if (System::DumpRAM(path_str.c_str(), &error))
  {
    emit statusMessage(QStringLiteral("RAM dumped to %1.").arg(path));
  }
  else
  {
    emit errorReported(
      QStringLiteral("Error"),
      QStringLiteral("Failed to dump RAM to %1: %2").arg(path).arg(QString::fromStdString(error.GetDescription())));
  }
}

void CoreThread::dumpVRAM(const QString& path)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &CoreThread::dumpVRAM, Qt::QueuedConnection, path);
    return;
  }

  Error error;
  const std::string path_str = path.toStdString();
  if (System::DumpVRAM(path_str.c_str(), &error))
  {
    emit statusMessage(QStringLiteral("VRAM dumped to %1.").arg(path));
  }
  else
  {
    emit errorReported(
      QStringLiteral("Error"),
      QStringLiteral("Failed to dump VRAM to %1: %2").arg(path).arg(QString::fromStdString(error.GetDescription())));
  }
}

void CoreThread::dumpSPURAM(const QString& path)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &CoreThread::dumpSPURAM, Qt::QueuedConnection, path);
    return;
  }

  Error error;
  const std::string path_str = path.toStdString();
  if (System::DumpSPURAM(path_str.c_str(), &error))
  {
    emit statusMessage(QStringLiteral("SPU RAM dumped to %1.").arg(path));
  }
  else
  {
    emit errorReported(
      QStringLiteral("Error"),
      QStringLiteral("Failed to dump SPU RAM to %1: %2").arg(path).arg(QString::fromStdString(error.GetDescription())));
  }
}

void CoreThread::saveScreenshot()
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &CoreThread::saveScreenshot, Qt::QueuedConnection);
    return;
  }

  System::SaveScreenshot();
}

void CoreThread::applicationStateChanged(Qt::ApplicationState state)
{
  const bool background = (state != Qt::ApplicationActive);
  InputManager::OnApplicationBackgroundStateChanged(background);

  if (!System::IsValid())
    return;

  if (background)
  {
    if (g_settings.pause_on_focus_loss && !m_was_paused_by_focus_loss && !System::IsPaused())
    {
      setSystemPaused(true);
      m_was_paused_by_focus_loss = true;
    }

    // Clear the state of all keyboard binds.
    // That way, if we had a key held down, and lost focus, the bind won't be stuck enabled because we never
    // got the key release message, because it happened in another window which "stole" the event.
    InputManager::ClearBindStateFromSource(InputManager::MakeHostKeyboardKey(0));
  }
  else
  {
    if (m_was_paused_by_focus_loss)
    {
      if (System::IsPaused())
        setSystemPaused(false);
      m_was_paused_by_focus_loss = false;
    }
  }
}

void Host::OnAchievementsLoginRequested(Achievements::LoginRequestReason reason)
{
  emit g_core_thread->achievementsLoginRequested(reason);
}

void Host::OnAchievementsLoginSuccess(const char* username, u32 points, u32 sc_points, u32 unread_messages)
{
  emit g_core_thread->achievementsLoginSuccess(QString::fromUtf8(username), points, sc_points, unread_messages);
}

void Host::OnAchievementsActiveChanged(bool active)
{
  emit g_core_thread->achievementsActiveChanged(active);
}

void Host::OnAchievementsHardcoreModeChanged(bool enabled)
{
  emit g_core_thread->achievementsHardcoreModeChanged(enabled);
}

void Host::BeginTextInput()
{
  DEV_LOG("Host::BeginTextInput()");

  // NOTE: Called on GPU thread.
  QInputMethod* method = qApp->inputMethod();
  if (method)
    QMetaObject::invokeMethod(method, &QInputMethod::show, Qt::QueuedConnection);
}

void Host::EndTextInput()
{
  DEV_LOG("Host::EndTextInput()");

  QInputMethod* method = qApp->inputMethod();
  if (method)
    QMetaObject::invokeMethod(method, &QInputMethod::hide, Qt::QueuedConnection);
}

bool Host::CreateAuxiliaryRenderWindow(s32 x, s32 y, u32 width, u32 height, std::string_view title,
                                       std::string_view icon_name, AuxiliaryRenderWindowUserData userdata,
                                       AuxiliaryRenderWindowHandle* handle, WindowInfo* wi, Error* error)
{
  return emit g_core_thread->onCreateAuxiliaryRenderWindow(
    g_gpu_device->GetRenderAPI(), x, y, width, height, QtUtils::StringViewToQString(title),
    QtUtils::StringViewToQString(icon_name), userdata, handle, wi, error);
}

void Host::DestroyAuxiliaryRenderWindow(AuxiliaryRenderWindowHandle handle, s32* pos_x, s32* pos_y, u32* width,
                                        u32* height)
{
  QPoint pos;
  QSize size;
  emit g_core_thread->onDestroyAuxiliaryRenderWindow(handle, &pos, &size);

  if (pos_x)
    *pos_x = pos.x();
  if (pos_y)
    *pos_y = pos.y();
  if (width)
    *width = size.width();
  if (height)
    *height = size.height();

  // eat all pending events, to make sure we're not going to write input events back to a dead pointer
  if (g_core_thread->isCurrentThread())
    g_core_thread->getEventLoop()->processEvents(QEventLoop::AllEvents);
}

void CoreThread::queueAuxiliaryRenderWindowInputEvent(Host::AuxiliaryRenderWindowUserData userdata,
                                                      Host::AuxiliaryRenderWindowEvent event,
                                                      Host::AuxiliaryRenderWindowEventParam param1,
                                                      Host::AuxiliaryRenderWindowEventParam param2,
                                                      Host::AuxiliaryRenderWindowEventParam param3)
{
  DebugAssert(QThread::isMainThread());
  QMetaObject::invokeMethod(this, &CoreThread::processAuxiliaryRenderWindowInputEvent, Qt::QueuedConnection, userdata,
                            static_cast<quint32>(event), static_cast<quint32>(param1.uint_param),
                            static_cast<quint32>(param2.uint_param), static_cast<quint32>(param3.uint_param));
}

void CoreThread::processAuxiliaryRenderWindowInputEvent(void* userdata, quint32 event, quint32 param1, quint32 param2,
                                                        quint32 param3)
{
  DebugAssert(isCurrentThread());
  VideoThread::RunOnThread([userdata, event, param1, param2, param3]() {
    ImGuiManager::ProcessAuxiliaryRenderWindowInputEvent(userdata, static_cast<Host::AuxiliaryRenderWindowEvent>(event),
                                                         Host::AuxiliaryRenderWindowEventParam{.uint_param = param1},
                                                         Host::AuxiliaryRenderWindowEventParam{.uint_param = param2},
                                                         Host::AuxiliaryRenderWindowEventParam{.uint_param = param3});
  });
}

void CoreThread::doBackgroundControllerPoll()
{
  System::IdlePollUpdate();
}

void CoreThread::createBackgroundControllerPollTimer()
{
  DebugAssert(!m_background_controller_polling_timer);
  m_background_controller_polling_timer = new QTimer(this);
  m_background_controller_polling_timer->setSingleShot(false);
  m_background_controller_polling_timer->setTimerType(Qt::CoarseTimer);
  connect(m_background_controller_polling_timer, &QTimer::timeout, this, &CoreThread::doBackgroundControllerPoll);
}

void CoreThread::destroyBackgroundControllerPollTimer()
{
  delete m_background_controller_polling_timer;
  m_background_controller_polling_timer = nullptr;
}

void CoreThread::startBackgroundControllerPollTimer()
{
  if (m_background_controller_polling_timer->isActive())
  {
    updateBackgroundControllerPollInterval();
    return;
  }

  const int interval = getBackgroundControllerPollInterval();
  DEV_LOG("Starting background controller polling timer with interval {} ms", interval);
  m_background_controller_polling_timer->start(interval);
}

void CoreThread::stopBackgroundControllerPollTimer()
{
  if (!m_background_controller_polling_timer->isActive())
    return;

  DEV_LOG("Stopping background controller polling timer");
  m_background_controller_polling_timer->stop();
}

void CoreThread::updateBackgroundControllerPollInterval()
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &CoreThread::updateBackgroundControllerPollInterval, Qt::QueuedConnection);
    return;
  }

  if (!m_background_controller_polling_timer || !m_background_controller_polling_timer->isActive())
    return;

  const int current_interval = m_background_controller_polling_timer->interval();
  const int new_interval = getBackgroundControllerPollInterval();
  if (current_interval != new_interval)
  {
    WARNING_LOG("Changed background polling interval from {} ms to {} ms", current_interval, new_interval);
    m_background_controller_polling_timer->setInterval(new_interval);
  }
}

int CoreThread::getBackgroundControllerPollInterval() const
{
  if (GDBServer::HasAnyClients())
    return GDB_SERVER_POLLING_INTERVAL;
  else if (m_video_thread_run_idle)
    return FULLSCREEN_UI_CONTROLLER_POLLING_INTERVAL;
  else if (InputManager::GetPollableDeviceCount() > 0)
    return BACKGROUND_CONTROLLER_POLLING_INTERVAL_WITH_DEVICES;
  else
    return BACKGROUND_CONTROLLER_POLLING_INTERVAL_WITHOUT_DEVICES;
}

void CoreThread::setVideoThreadRunIdle(bool active)
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &CoreThread::setVideoThreadRunIdle, Qt::QueuedConnection, active);
    return;
  }

  m_video_thread_run_idle = active;

  // break out of the event loop if we're not executing a system
  if (active && !g_settings.gpu_use_thread && !System::IsRunning())
    m_event_loop->quit();

  // adjust the timer speed to pick up controller input faster
  if (!m_background_controller_polling_timer->isActive())
    return;

  g_core_thread->updateBackgroundControllerPollInterval();
}

void CoreThread::updateFullscreenUITheme()
{
  if (!isCurrentThread())
  {
    QMetaObject::invokeMethod(this, &CoreThread::updateFullscreenUITheme, Qt::QueuedConnection);
    return;
  }

  // don't bother if nothing is running
  if (VideoThread::IsFullscreenUIRequested() || VideoThread::IsGPUBackendRequested())
    VideoThread::RunOnThread(&FullscreenUI::UpdateTheme);
}

void CoreThread::start()
{
  AssertMsg(!g_core_thread->isRunning(), "Emu thread is not started");

  g_core_thread->QThread::start();
  g_core_thread->m_started_semaphore.acquire();
}

void CoreThread::stop()
{
  AssertMsg(g_core_thread, "Emu thread exists");
  AssertMsg(!g_core_thread->isCurrentThread(), "Not called on the emu thread");

  QMetaObject::invokeMethod(g_core_thread, &CoreThread::stopInThread, Qt::QueuedConnection);
  QtUtils::ProcessEventsWithSleep(QEventLoop::ExcludeUserInputEvents, []() { return (g_core_thread->isRunning()); });

  // Ensure settings are saved.
  if (QtHost::s_state.settings_save_timer)
  {
    QtHost::s_state.settings_save_timer.reset();
    QtHost::SaveSettings();
  }
}

void CoreThread::stopInThread()
{
  stopFullscreenUI();

  m_shutdown_flag = true;
  m_event_loop->quit();
}

void CoreThread::run()
{
  m_event_loop = new QEventLoop();
  m_started_semaphore.release();

  // input source setup must happen on emu thread
  {
    Error startup_error;
    if (!System::CoreThreadInitialize(&startup_error))
    {
      moveToThread(m_ui_thread);
      Host::ReportFatalError("Fatal Startup Error", startup_error.GetDescription());
      return;
    }
  }

  // start up worker threads
  // TODO: Replace this with QThreads
  QtHost::s_async_task_queue.SetWorkerCount(NUM_ASYNC_WORKER_THREADS);

  // connections
  connect(qApp, &QGuiApplication::applicationStateChanged, this, &CoreThread::applicationStateChanged);

  // enumerate all devices, even those which were added early
  m_input_device_list_model->enumerateDevices();

  // start background input polling
  createBackgroundControllerPollTimer();
  startBackgroundControllerPollTimer();

  // kick off GPU thread
  Threading::Thread video_thread(&CoreThread::videoThreadEntryPoint);

  // main loop
  while (!m_shutdown_flag)
  {
    if (System::IsRunning())
    {
      System::Execute();
    }
    else if (!VideoThread::IsUsingThread() && VideoThread::IsRunningIdle())
    {
      m_event_loop->processEvents(QEventLoop::AllEvents);

      // have to double-check the condition after processing events, because the events could shut us down
      if (!VideoThread::IsUsingThread() && VideoThread::IsRunningIdle())
        VideoThread::Internal::DoRunIdle();
    }
    else
    {
      m_event_loop->exec();
    }
  }

  if (System::IsValid())
    System::ShutdownSystem(false);

  destroyBackgroundControllerPollTimer();

  // tell GPU thread to exit
  VideoThread::Internal::RequestShutdown();
  video_thread.Join();

  // join worker threads
  QtHost::s_async_task_queue.SetWorkerCount(0);

  // and tidy up everything left
  System::CoreThreadShutdown();

  // move back to UI thread
  moveToThread(m_ui_thread);
  delete m_event_loop;
  m_event_loop = nullptr;
}

void CoreThread::videoThreadEntryPoint()
{
  Threading::SetNameOfCurrentThread("Video Thread");
  VideoThread::Internal::VideoThreadEntryPoint();
}

void Host::FrameDoneOnVideoThread(GPUBackend* gpu_backend, u32 frame_number)
{
}

void CoreThread::wakeThread()
{
  if (isCurrentThread())
    m_event_loop->quit();
  else
    QMetaObject::invokeMethod(m_event_loop, &QEventLoop::quit, Qt::QueuedConnection);
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
  QThread* const ui_thread = qApp->thread();
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
    QMetaObject::invokeMethod(timer, static_cast<void (QTimer::*)(int)>(&QTimer::start), Qt::QueuedConnection,
                              static_cast<int>(0));

    // should never return
    while (ui_thread->isRunning())
      Timer::NanoSleep(1000000000ULL);
  }

  std::abort();
}

void Host::ReportErrorAsync(std::string_view title, std::string_view message)
{
  if (!title.empty() && !message.empty())
    ERROR_LOG("ReportErrorAsync: {}: {}", title, message);
  else if (!message.empty())
    ERROR_LOG("ReportErrorAsync: {}", message);

  QMetaObject::invokeMethod(g_main_window, &MainWindow::reportError, Qt::QueuedConnection,
                            title.empty() ? QString() : QString::fromUtf8(title.data(), title.size()),
                            message.empty() ? QString() : QString::fromUtf8(message.data(), message.size()));
}

void Host::ReportStatusMessage(std::string_view message)
{
  if (message.empty())
    return;

  emit g_core_thread->statusMessage(QtUtils::StringViewToQString(message));
}

void Host::ConfirmMessageAsync(std::string_view title, std::string_view message, ConfirmMessageAsyncCallback callback,
                               std::string_view yes_text, std::string_view no_text)
{
  INFO_LOG("ConfirmMessageAsync({}, {})", title, message);

  // Default button titles.
  if (yes_text.empty())
    yes_text = TRANSLATE_SV("QtHost", "Yes");
  if (no_text.empty())
    no_text = TRANSLATE_SV("QtHost", "No");

  // Ensure it always comes from the CPU thread.
  if (!g_core_thread->isCurrentThread())
  {
    Host::RunOnCoreThread([title = std::string(title), message = std::string(message), callback = std::move(callback),
                           yes_text = std::string(yes_text), no_text = std::string(no_text)]() mutable {
      ConfirmMessageAsync(title, message, std::move(callback));
    });
    return;
  }

  // Pause system while dialog is up.
  const bool needs_pause = System::IsValid() && !System::IsPaused();
  if (needs_pause)
    System::PauseSystem(true);

  // Use FSUI if we're ingame.
  if (System::IsValid() || g_core_thread->isFullscreenUIStarted())
  {
    VideoThread::RunOnThread([title = std::string(title), message = std::string(message),
                              callback = std::move(callback), yes_text = std::string(yes_text),
                              no_text = std::string(no_text), needs_pause]() mutable {
      // Need to reset run idle state _again_ after displaying.
      auto final_callback = [callback = std::move(callback), needs_pause](bool result) {
        FullscreenUI::UpdateRunIdleState();
        if (needs_pause)
        {
          Host::RunOnCoreThread([]() {
            if (System::IsValid())
              System::PauseSystem(false);
          });
        }
        callback(result);
      };

      FullscreenUI::OpenConfirmMessageDialog(ICON_EMOJI_QUESTION_MARK, std::move(title), std::move(message),
                                             std::move(final_callback), fmt::format(ICON_FA_CHECK " {}", yes_text),
                                             fmt::format(ICON_FA_XMARK " {}", no_text));
      FullscreenUI::UpdateRunIdleState();
    });
  }
  else
  {
    // Otherwise, use the desktop UI.
    Host::RunOnUIThread([title = QtUtils::StringViewToQString(title), message = QtUtils::StringViewToQString(message),
                         callback = std::move(callback), yes_text = QtUtils::StringViewToQString(yes_text),
                         no_text = QtUtils::StringViewToQString(no_text), needs_pause]() mutable {
      auto lock = g_main_window->pauseAndLockSystem();

      QWidget* const dialog_parent = lock.getDialogParent();
      QMessageBox* const msgbox =
        QtUtils::NewMessageBox(dialog_parent, QMessageBox::Question, title, message, QMessageBox::NoButton);

      QPushButton* const yes_button = msgbox->addButton(yes_text, QMessageBox::AcceptRole);
      msgbox->addButton(no_text, QMessageBox::RejectRole);

      QObject::connect(msgbox, &QMessageBox::finished, dialog_parent,
                       [msgbox, yes_button, callback = std::move(callback), lock = std::move(lock), needs_pause]() {
                         const bool result = (msgbox->clickedButton() == yes_button);
                         callback(result);

                         if (needs_pause)
                         {
                           Host::RunOnCoreThread([]() {
                             if (System::IsValid())
                               System::PauseSystem(false);
                           });
                         }
                       });
      msgbox->open();
    });
  }
}

void Host::OpenURL(std::string_view url)
{
  Host::RunOnUIThread([url = QtUtils::StringViewToQString(url)]() { QtUtils::OpenURL(g_main_window, QUrl(url)); });
}

std::string Host::GetClipboardText()
{
  // Hope this doesn't deadlock...
  std::string ret;
  Host::RunOnUIThread(
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
  Host::RunOnUIThread([text = QtUtils::StringViewToQString(text)]() {
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (clipboard)
      clipboard->setText(text);
  });
  return true;
}

QString QtHost::FormatNumber(Host::NumberFormatType type, s64 value)
{
  QString ret;

  if (type >= Host::NumberFormatType::ShortDate && type <= Host::NumberFormatType::LongDateTime)
  {
    QString format;
    switch (type)
    {
      case Host::NumberFormatType::ShortDate:
      case Host::NumberFormatType::LongDate:
      {
        format = s_state.app_locale.dateFormat((type == Host::NumberFormatType::LongDate) ? QLocale::LongFormat :
                                                                                            QLocale::ShortFormat);
      }
      break;

      case Host::NumberFormatType::ShortTime:
      case Host::NumberFormatType::LongTime:
      {
        format = s_state.app_locale.timeFormat((type == Host::NumberFormatType::LongTime) ? QLocale::LongFormat :
                                                                                            QLocale::ShortFormat);
      }
      break;

      case Host::NumberFormatType::ShortDateTime:
      case Host::NumberFormatType::LongDateTime:
      {
        format = s_state.app_locale.dateTimeFormat(
          (type == Host::NumberFormatType::LongDateTime) ? QLocale::LongFormat : QLocale::ShortFormat);

        // Remove time zone specifiers 't', 'tt', 'ttt', 'tttt'.
        format.remove(QRegularExpression("\\s*t+\\s*"));
      }
      break;

        DefaultCaseIsUnreachable();
    }

    ret = QDateTime::fromSecsSinceEpoch(value, QTimeZone::utc()).toLocalTime().toString(format);
  }
  else
  {
    ret = s_state.app_locale.toString(value);
  }

  return ret;
}

std::string Host::FormatNumber(NumberFormatType type, s64 value)
{
  return QtHost::FormatNumber(type, value).toStdString();
}

QString QtHost::FormatNumber(Host::NumberFormatType type, double value)
{
  QString ret;

  switch (type)
  {
    case Host::NumberFormatType::Number:
    default:
      ret = s_state.app_locale.toString(value);
      break;
  }

  return ret;
}

std::string Host::FormatNumber(NumberFormatType type, double value)
{
  return QtHost::FormatNumber(type, value).toStdString();
}

void QtHost::UpdateApplicationLanguage(QWidget* dialog_parent)
{
  for (QTranslator* translator : s_state.translators)
  {
    qApp->removeTranslator(translator);
    translator->deleteLater();
  }
  s_state.translators.clear();

  // Fixup automatic language.
  std::string language = Core::GetBaseStringSettingValue("Main", "Language", "");
  if (language.empty())
    language = GetSystemLanguage();
  QString qlanguage = QString::fromStdString(language);

  // install the base qt translation first
#ifndef __APPLE__
  const QString base_dir = QStringLiteral("%1/translations").arg(qApp->applicationDirPath());
#else
  const QString base_dir = QStringLiteral("%1/../Resources/translations").arg(qApp->applicationDirPath());
#endif

  // Qt base uses underscores instead of hyphens.
  const QString qtbase_language = QString(qlanguage).replace(QChar('-'), QChar('_'));
  QString base_path(QStringLiteral("%1/qt_%2.qm").arg(base_dir).arg(qtbase_language));
  bool has_base_ts = QFile::exists(base_path);
  if (!has_base_ts)
  {
    // Try without the country suffix.
    const qsizetype index = qlanguage.lastIndexOf('-');
    if (index > 0)
    {
      base_path = QStringLiteral("%1/qt_%2.qm").arg(base_dir).arg(qlanguage.left(index));
      has_base_ts = QFile::exists(base_path);
    }
  }
  if (has_base_ts)
  {
    QTranslator* base_translator = new QTranslator(qApp);
    if (!base_translator->load(base_path))
    {
      QtUtils::MessageBoxWarning(
        dialog_parent, QStringLiteral("Translation Error"),
        QStringLiteral("Failed to load base translation file for '%1':\n%2").arg(qlanguage).arg(base_path));
      delete base_translator;
    }
    else
    {
      s_state.translators.push_back(base_translator);
      qApp->installTranslator(base_translator);
    }
  }

  const QString path = QStringLiteral("%1/duckstation-qt_%3.qm").arg(base_dir).arg(qlanguage);
  if (!QFile::exists(path))
  {
    QtUtils::MessageBoxWarning(
      dialog_parent, QStringLiteral("Translation Error"),
      QStringLiteral("Failed to find translation file for language '%1':\n%2").arg(qlanguage).arg(path));
    return;
  }

  QTranslator* translator = new QTranslator(qApp);
  if (!translator->load(path))
  {
    QtUtils::MessageBoxWarning(
      dialog_parent, QStringLiteral("Translation Error"),
      QStringLiteral("Failed to load translation file for language '%1':\n%2").arg(qlanguage).arg(path));
    delete translator;
    return;
  }

  INFO_LOG("Loaded translation file for language {}", qlanguage.toUtf8().constData());
  qApp->installTranslator(translator);
  s_state.translators.push_back(translator);

  // We end up here both on language change, and on startup.
  UpdateFontOrder(language);
  UpdateApplicationLocale(language);
}

s32 Host::Internal::GetTranslatedStringImpl(std::string_view context, std::string_view msg,
                                            std::string_view disambiguation, char* tbuf, size_t tbuf_space)
{
  // This is really awful. Thankfully we're caching the results...
  const std::string temp_context(context);
  const std::string temp_msg(msg);
  const std::string temp_disambiguation(disambiguation);
  const QString translated_msg = qApp->translate(temp_context.c_str(), temp_msg.c_str(),
                                                 disambiguation.empty() ? nullptr : temp_disambiguation.c_str());
  const QByteArray translated_utf8 = translated_msg.toUtf8();
  const size_t translated_size = translated_utf8.size();
  if (translated_size > tbuf_space)
    return -1;
  else if (translated_size > 0)
    std::memcpy(tbuf, translated_utf8.constData(), translated_size);

  return static_cast<s32>(translated_size);
}

std::string Host::TranslatePluralToString(const char* context, const char* msg, const char* disambiguation, int count)
{
  return qApp->translate(context, msg, disambiguation, count).toStdString();
}

SmallString Host::TranslatePluralToSmallString(const char* context, const char* msg, const char* disambiguation,
                                               int count)
{
  const QString qstr = qApp->translate(context, msg, disambiguation, count);
  SmallString ret;

#ifdef _WIN32
  // Cheeky way to avoid heap allocations.
  static_assert(sizeof(*qstr.utf16()) == sizeof(wchar_t));
  ret.assign(std::wstring_view(reinterpret_cast<const wchar_t*>(qstr.utf16()), qstr.length()));
#else
  const QByteArray utf8 = qstr.toUtf8();
  ret.assign(utf8.constData(), utf8.length());
#endif

  return ret;
}

std::span<const std::pair<const char*, const char*>> Host::GetAvailableLanguageList()
{
  static constexpr const std::pair<const char*, const char*> languages[] = {
    {QT_TRANSLATE_NOOP("QtHost", "System Language"), ""},

#define TRANSLATION_LIST_ENTRY(name, code, locale_code, qlanguage, qcountry) {name " [" code "]", code},
#include "qttranslations.inl"
#undef TRANSLATION_LIST_ENTRY
  };

  return languages;
}

const char* Host::GetLanguageName(std::string_view language_code)
{
  for (const auto& [name, code] : GetAvailableLanguageList())
  {
    if (language_code == code)
      return Host::TranslateToCString("QtHost", name);
  }

  return TRANSLATE("QtHost", "Unknown");
}

std::string_view QtHost::GetSystemLanguage()
{
  std::string locname = QLocale::system().name(QLocale::TagSeparator::Dash).toStdString();

  // Does this match any of our translations?
  for (const auto& [lname, lcode] : Host::GetAvailableLanguageList())
  {
    if (locname == lcode)
      return lcode;
  }

  // Check for a partial match, e.g. "zh" for "zh-CN".
  if (const std::string::size_type pos = locname.find('-'); pos != std::string::npos)
  {
    const std::string_view plocname = std::string_view(locname).substr(0, pos);
    for (const auto& [lname, lcode] : Host::GetAvailableLanguageList())
    {
      // Only some languages have a country code, so we need to check both.
      const std::string_view lcodev(lcode);
      if (lcodev == plocname)
      {
        return lcode;
      }
      else if (const std::string_view::size_type lpos = lcodev.find('-'); lpos != std::string::npos)
      {
        if (lcodev.substr(0, lpos) == plocname)
          return lcode;
      }
    }
  }

  // Fallback to English.
  return "en";
}

bool Host::ChangeLanguage(const char* new_language)
{
  Host::RunOnUIThread([new_language = std::string(new_language)]() {
    Core::SetBaseStringSettingValue("Main", "Language", new_language.c_str());
    Host::CommitBaseSettingChanges();
    QtHost::UpdateApplicationLanguage(g_main_window);
    g_main_window->recreate();
  });
  return true;
}

void QtHost::UpdateFontOrder(std::string_view language)
{
  // Why is this a thing? Because we want all glyphs to be available, but don't want to conflict
  // between codepoints shared between Chinese and Japanese. Therefore we prioritize the language
  // that the user has selected.
  ImGuiManager::TextFontOrder font_order;
#define TF(name) ImGuiManager::TextFont::name
  if (language == "ja")
    font_order = {TF(Default), TF(Japanese), TF(Chinese), TF(Korean)};
  else if (language == "ko")
    font_order = {TF(Default), TF(Korean), TF(Japanese), TF(Chinese)};
  else if (language == "zh-CN")
    font_order = {TF(Default), TF(Chinese), TF(Japanese), TF(Korean)};
  else
    font_order = ImGuiManager::GetDefaultTextFontOrder();
#undef TF

  if (g_core_thread)
  {
    Host::RunOnCoreThread([font_order]() mutable {
      VideoThread::RunOnThread([font_order]() mutable { ImGuiManager::SetTextFontOrder(font_order); });
      Host::ClearTranslationCache();
    });
  }
  else
  {
    // Startup, safe to set directly.
    ImGuiManager::SetTextFontOrder(font_order);
    Host::ClearTranslationCache();
  }
}

const QLocale& QtHost::GetApplicationLocale()
{
  return s_state.app_locale;
}

void QtHost::UpdateApplicationLocale(std::string_view language)
{
  static constexpr const std::tuple<const char*, QLocale::Language, QLocale::Country> lookup[] = {
#define TRANSLATION_LIST_ENTRY(name, code, locale_code, qlanguage, qcountry) {code, qlanguage, qcountry},
#include "qttranslations.inl"
#undef TRANSLATION_LIST_ENTRY
  };

  // If the system locale is using the same language, then use the system locale.
  // Otherwise we'll be using that ugly US date format in Straya mate.
  s_state.app_locale = QLocale::system();
  const QString system_locale_name = s_state.app_locale.name(QLocale::TagSeparator::Dash);
  if (system_locale_name.startsWith(QLatin1StringView(language), Qt::CaseInsensitive))
  {
    INFO_LOG("Using system locale for {}.", language);
    return;
  }

  for (const auto& [code, qlanguage, qcountry] : lookup)
  {
    if (language == code)
    {
      s_state.app_locale = QLocale(qlanguage, qcountry);
      return;
    }
  }
}

InputDeviceListModel::InputDeviceListModel(QObject* parent) : QAbstractListModel(parent)
{
}

InputDeviceListModel::~InputDeviceListModel() = default;

QIcon InputDeviceListModel::getIconForKey(const InputBindingKey& key)
{
  if (key.source_type == InputSourceType::Keyboard)
    return QIcon::fromTheme("keyboard-line"_L1);
  else if (key.source_type == InputSourceType::Pointer)
    return QIcon::fromTheme("mouse-line"_L1);
  else
    return QIcon::fromTheme("controller-line"_L1);
}

QString InputDeviceListModel::getDeviceName(const InputBindingKey& key)
{
  QString ret;
  for (const InputDeviceListModel::Device& device : m_devices)
  {
    if (device.key.source_type == key.source_type && device.key.source_index == key.source_index)
    {
      ret = device.display_name;
      break;
    }
  }

  return ret;
}

bool InputDeviceListModel::hasEffectsOfType(InputBindingInfo::Type type)
{
  return std::ranges::any_of(m_effects, [type](const auto& eff) { return eff.first == type; });
}

int InputDeviceListModel::rowCount(const QModelIndex& parent /*= QModelIndex()*/) const
{
  return static_cast<int>(m_devices.size());
}

QVariant InputDeviceListModel::data(const QModelIndex& index, int role /*= Qt::DisplayRole*/) const
{
  const int row = index.row();
  if (index.column() != 0 || row < 0 || static_cast<qsizetype>(row) >= m_devices.size())
    return QVariant();

  if (role == Qt::DisplayRole)
  {
    const auto& dev = m_devices[static_cast<qsizetype>(row)];
    const InputBindingKey key = dev.key;

    // don't display device names for implicit keyboard/mouse
    if (key.source_type == InputSourceType::Keyboard ||
        (key.source_type == InputSourceType::Pointer && !InputManager::IsUsingRawInput()))
    {
      return dev.display_name;
    }
    else
    {
      return QStringLiteral("%1\n%2").arg(dev.identifier).arg(dev.display_name);
    }
  }
  else if (role == Qt::DecorationRole)
  {
    const auto& dev = m_devices[static_cast<qsizetype>(row)];
    return getIconForKey(dev.key);
  }
  else
  {
    return QVariant();
  }
}

void InputDeviceListModel::enumerateDevices()
{
  DebugAssert(g_core_thread->isCurrentThread());

  const InputManager::DeviceList devices = InputManager::EnumerateDevices();
  const InputManager::DeviceEffectList effects = InputManager::EnumerateDeviceEffects();

  DeviceList new_devices;
  new_devices.reserve(devices.size());
  for (const auto& [key, identifier, device_name] : devices)
    new_devices.emplace_back(key, QString::fromStdString(identifier), QString::fromStdString(device_name));

  EffectList new_effects;
  new_effects.reserve(effects.size());
  for (const auto& [type, key] : effects)
    new_effects.emplace_back(type, key);

  QMetaObject::invokeMethod(this, &InputDeviceListModel::resetLists, Qt::QueuedConnection, new_devices, new_effects);
}

void InputDeviceListModel::resetLists(const DeviceList& devices, const EffectList& effects)
{
  beginResetModel();

  m_devices = devices;
  m_effects = effects;

  endResetModel();
}

void InputDeviceListModel::onDeviceConnected(const InputBindingKey& key, const QString& identifier,
                                             const QString& device_name, const EffectList& effects)
{
  for (const auto& it : m_devices)
  {
    if (it.identifier == identifier)
      return;
  }

  const int index = static_cast<int>(m_devices.size());
  beginInsertRows(QModelIndex(), index, index);
  m_devices.emplace_back(key, identifier, device_name);
  endInsertRows();

  m_effects.append(effects);
}

void InputDeviceListModel::onDeviceDisconnected(const InputBindingKey& key, const QString& identifier)
{
  for (qsizetype i = 0; i < m_devices.size(); i++)
  {
    if (m_devices[i].identifier == identifier)
    {
      const int index = static_cast<int>(i);
      beginRemoveRows(QModelIndex(), index, index);
      m_devices.remove(i);
      endRemoveRows();

      // remove effects too
      const QString effect_prefix = QStringLiteral("%1/").arg(identifier);
      for (qsizetype j = 0; j < m_effects.size();)
      {
        if (m_effects[j].second.source_type == key.source_type && m_effects[j].second.source_index == key.source_index)
          m_effects.remove(j);
        else
          j++;
      }

      return;
    }
  }
}

void Host::OnInputDeviceConnected(InputBindingKey key, std::string_view identifier, std::string_view device_name)
{
  // get the effects for this device to append to the list
  InputDeviceListModel::EffectList qeffect_list;
  const InputManager::DeviceEffectList effect_list = InputManager::EnumerateDeviceEffects(std::nullopt, key);
  if (!effect_list.empty())
  {
    qeffect_list.reserve(effect_list.size());
    for (const auto& [eff_type, eff_key] : effect_list)
      qeffect_list.emplace_back(eff_type, eff_key);
  }

  QMetaObject::invokeMethod(g_core_thread->getInputDeviceListModel(), &InputDeviceListModel::onDeviceConnected,
                            Qt::QueuedConnection, key, QtUtils::StringViewToQString(identifier),
                            QtUtils::StringViewToQString(device_name), qeffect_list);
  g_core_thread->updateBackgroundControllerPollInterval();
}

void Host::OnInputDeviceDisconnected(InputBindingKey key, std::string_view identifier)
{
  QMetaObject::invokeMethod(g_core_thread->getInputDeviceListModel(), &InputDeviceListModel::onDeviceDisconnected,
                            Qt::QueuedConnection, key, QtUtils::StringViewToQString(identifier));
  g_core_thread->updateBackgroundControllerPollInterval();
}

void Host::AddFixedInputBindings(const SettingsInterface& si)
{
}

std::string QtHost::GetResourcePath(std::string_view filename, bool allow_override)
{
  return allow_override ? EmuFolders::GetOverridableResourcePath(filename) :
                          Path::Combine(EmuFolders::Resources, filename);
}

QString QtHost::GetResourceQPath(std::string_view name, bool allow_override)
{
  return QString::fromStdString(GetResourcePath(name, allow_override));
}

const QStringList& QtHost::GetRobotoFontFamilies()
{
  std::call_once(s_state.roboto_font_once_flag, []() {
    const int font_id = QFontDatabase::addApplicationFont(
      QString::fromStdString(Path::Combine(EmuFolders::Resources, "fonts/Roboto-VariableFont_wdth,wght.ttf")));
    if (font_id < 0)
    {
      ERROR_LOG("Failed to load Roboto font.");
      return;
    }

    s_state.roboto_font_families = QFontDatabase::applicationFontFamilies(font_id);
    if (s_state.roboto_font_families.isEmpty())
    {
      ERROR_LOG("Failed to get Roboto font family.");
      return;
    }
  });

  return s_state.roboto_font_families;
}

const QFont& QtHost::GetFixedFont()
{
  std::call_once(s_state.fixed_font_once_flag, []() {
    const int font_id = QFontDatabase::addApplicationFont(
      QString::fromStdString(Path::Combine(EmuFolders::Resources, "fonts/GoogleSansCode-VariableFont_wght.ttf")));
    if (font_id < 0)
    {
      ERROR_LOG("Failed to load fixed-width font.");
      return;
    }

    const QStringList families = QFontDatabase::applicationFontFamilies(font_id);
    if (families.isEmpty())
    {
      ERROR_LOG("Failed to get fixed-width font family.");
      return;
    }

    s_state.fixed_font.setFamilies(families);
    s_state.fixed_font.setWeight(QFont::Medium);
    s_state.fixed_font.setPixelSize(12);
    s_state.fixed_font.setHintingPreference(QFont::PreferNoHinting);
  });

  return s_state.fixed_font;
}

bool Host::ResourceFileExists(std::string_view filename, bool allow_override)
{
  const std::string path = QtHost::GetResourcePath(filename, allow_override);
  return FileSystem::FileExists(path.c_str());
}

std::optional<DynamicHeapArray<u8>> Host::ReadResourceFile(std::string_view filename, bool allow_override, Error* error)
{
  const std::string path = QtHost::GetResourcePath(filename, allow_override);
  const std::optional<DynamicHeapArray<u8>> ret = FileSystem::ReadBinaryFile(path.c_str(), error);
  if (!ret.has_value())
    Error::AddPrefixFmt(error, "Failed to read resource file '{}': ", filename);
  return ret;
}

std::optional<std::string> Host::ReadResourceFileToString(std::string_view filename, bool allow_override, Error* error)
{
  const std::string path = QtHost::GetResourcePath(filename, allow_override);
  const std::optional<std::string> ret = FileSystem::ReadFileToString(path.c_str(), error);
  if (!ret.has_value())
    Error::AddPrefixFmt(error, "Failed to read resource file '{}': ", filename);
  return ret;
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
  QtHost::QueueSettingsSave();
}

std::optional<WindowInfo> Host::AcquireRenderWindow(RenderAPI render_api, bool fullscreen, bool exclusive_fullscreen,
                                                    Error* error)
{
  return g_core_thread->acquireRenderWindow(render_api, fullscreen, exclusive_fullscreen, error);
}

WindowInfoType Host::GetRenderWindowInfoType()
{
  return QtUtils::GetWindowInfoType();
}

void Host::ReleaseRenderWindow()
{
  g_core_thread->releaseRenderWindow();
}

bool Host::CanChangeFullscreenMode(bool new_fullscreen_state)
{
  // Don't mess with fullscreen while locked.
  return (!new_fullscreen_state || !QtHost::IsSystemLocked());
}

void CoreThread::updatePerformanceCounters(const GPUBackend* gpu_backend)
{
  const RenderAPI render_api = g_gpu_device->GetRenderAPI();
  const bool hardware_renderer = GPUBackend::IsUsingHardwareBackend();
  u32 render_width = 0;
  u32 render_height = 0;

  if (gpu_backend)
  {
    const u32 render_scale = gpu_backend->GetResolutionScale();
    std::tie(render_width, render_height) = g_gpu.GetFullDisplayResolution();
    render_width *= render_scale;
    render_height *= render_scale;
  }

  if (render_api != m_last_render_api || hardware_renderer != m_last_hardware_renderer)
  {
    const QString renderer_str = hardware_renderer ? QString::fromUtf8(GPUDevice::RenderAPIToString(render_api)) :
                                                     qApp->translate("GPURenderer", "Software");
    QMetaObject::invokeMethod(g_main_window->getStatusRendererWidget(), &QLabel::setText, Qt::QueuedConnection,
                              renderer_str);
    m_last_render_api = render_api;
    m_last_hardware_renderer = hardware_renderer;
  }
  if (render_width != m_last_render_width || render_height != m_last_render_height)
  {
    const QString text =
      (render_width != 0 && render_height != 0) ? tr("%1x%2").arg(render_width).arg(render_height) : tr("No Image");
    QMetaObject::invokeMethod(g_main_window->getStatusResolutionWidget(), &QLabel::setText, Qt::QueuedConnection, text);
    m_last_render_width = render_width;
    m_last_render_height = render_height;
  }

  const float gfps = PerformanceCounters::GetFPS();
  if (gfps != m_last_game_fps)
  {
    QMetaObject::invokeMethod(g_main_window->getStatusFPSWidget(), &QLabel::setText, Qt::QueuedConnection,
                              tr("Game: %1 FPS").arg(gfps, 0, 'f', 0));
    m_last_game_fps = gfps;
  }

  const float speed = PerformanceCounters::GetEmulationSpeed();
  const float vfps = PerformanceCounters::GetVPS();
  if (speed != m_last_speed || vfps != m_last_video_fps)
  {
    QMetaObject::invokeMethod(g_main_window->getStatusVPSWidget(), &QLabel::setText, Qt::QueuedConnection,
                              tr("Video: %1 FPS (%2%)").arg(vfps, 0, 'f', 0).arg(speed, 0, 'f', 0));
    m_last_speed = speed;
    m_last_video_fps = vfps;
  }
}

void CoreThread::resetPerformanceCounters()
{
  m_last_speed = std::numeric_limits<float>::infinity();
  m_last_game_fps = std::numeric_limits<float>::infinity();
  m_last_video_fps = std::numeric_limits<float>::infinity();
  m_last_render_width = std::numeric_limits<u32>::max();
  m_last_render_height = std::numeric_limits<u32>::max();
  m_last_render_api = RenderAPI::None;
  m_last_hardware_renderer = false;

  const QString blank;
  QMetaObject::invokeMethod(g_main_window->getStatusRendererWidget(), &QLabel::setText, Qt::QueuedConnection, blank);
  QMetaObject::invokeMethod(g_main_window->getStatusResolutionWidget(), &QLabel::setText, Qt::QueuedConnection, blank);
  QMetaObject::invokeMethod(g_main_window->getStatusFPSWidget(), &QLabel::setText, Qt::QueuedConnection, blank);
  QMetaObject::invokeMethod(g_main_window->getStatusVPSWidget(), &QLabel::setText, Qt::QueuedConnection, blank);
}

void Host::OnPerformanceCountersUpdated(const GPUBackend* gpu_backend)
{
  g_core_thread->updatePerformanceCounters(gpu_backend);
}

void Host::OnSystemGameChanged(const std::string& disc_path, const std::string& game_serial,
                               const std::string& game_name, GameHash hash)
{
  emit g_core_thread->systemGameChanged(QString::fromStdString(disc_path), QString::fromStdString(game_serial),
                                        QString::fromStdString(game_name));
}

void Host::OnSystemUndoStateAvailabilityChanged(bool available, u64 timestamp)
{
  emit g_core_thread->systemUndoStateAvailabilityChanged(available, timestamp);
}

void Host::OnMediaCaptureStarted()
{
  emit g_core_thread->mediaCaptureStarted();
}

void Host::OnMediaCaptureStopped()
{
  emit g_core_thread->mediaCaptureStopped();
}

void Host::SetMouseMode(bool relative, bool hide_cursor)
{
  emit g_core_thread->mouseModeRequested(relative, hide_cursor);
}

void Host::PumpMessagesOnCoreThread()
{
  g_core_thread->getEventLoop()->processEvents(QEventLoop::AllEvents);
}

void QtHost::SaveSettings()
{
  AssertMsg(!g_core_thread->isCurrentThread(), "Saving should happen on the UI thread.");

  {
    Error error;
    const auto lock = Core::GetSettingsLock();
    if (!Core::SaveBaseSettingsLayer(&error))
    {
      ERROR_LOG("Failed to save settings: {}", error.GetDescription());
      QtUtils::AsyncMessageBox(
        g_main_window, QMessageBox::Critical, QStringLiteral("DuckStation"),
        QStringLiteral("Failed to save settings: %1").arg(QString::fromStdString(error.GetDescription())));
    }
  }

  if (s_state.settings_save_timer)
  {
    s_state.settings_save_timer->deleteLater();
    s_state.settings_save_timer.release();
  }
}

void QtHost::QueueSettingsSave()
{
  if (!QThread::isMainThread())
  {
    Host::RunOnUIThread(QueueSettingsSave);
    return;
  }

  if (s_state.settings_save_timer)
    return;

  s_state.settings_save_timer = std::make_unique<QTimer>();
  s_state.settings_save_timer->connect(s_state.settings_save_timer.get(), &QTimer::timeout, SaveSettings);
  s_state.settings_save_timer->setSingleShot(true);
  s_state.settings_save_timer->start(SETTINGS_SAVE_DELAY);
}

bool QtHost::ShouldShowDebugOptions()
{
  return Core::GetBaseBoolSettingValue("Main", "ShowDebugMenu", false);
}

void Host::RequestSystemShutdown(bool allow_confirm, bool save_state, bool check_memcard_busy)
{
  if (!System::IsValid())
    return;

  QMetaObject::invokeMethod(g_main_window, &MainWindow::requestShutdown, Qt::QueuedConnection, allow_confirm, true,
                            save_state, check_memcard_busy, true, false, false);
}

void Host::RequestExitApplication(bool allow_confirm)
{
  QMetaObject::invokeMethod(g_main_window, &MainWindow::requestExit, Qt::QueuedConnection, allow_confirm);
}

void Host::RequestExitBigPicture()
{
  g_core_thread->exitFullscreenUI();
}

std::optional<WindowInfo> Host::GetTopLevelWindowInfo()
{
  std::optional<WindowInfo> ret;
  if (QThread::isMainThread())
    ret = g_main_window->getWindowInfo();
  else
    QMetaObject::invokeMethod(g_main_window, &MainWindow::getWindowInfo, Qt::BlockingQueuedConnection, &ret);
  return ret;
}

static void SignalHandler(int signal)
{
  // First try the normal (graceful) shutdown/exit.
  static bool graceful_shutdown_attempted = false;
  if (!graceful_shutdown_attempted && g_main_window)
  {
    std::fprintf(stderr, "Received CTRL+C, attempting graceful shutdown. Press CTRL+C again to force.\n");
    graceful_shutdown_attempted = true;

    // This could be a bit risky invoking from a signal handler... hopefully it's okay.
    QMetaObject::invokeMethod(g_main_window, &MainWindow::requestExit, Qt::QueuedConnection, true);
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

#ifndef _WIN32
  // Ignore SIGCHLD by default on Linux, since we kick off aplay asynchronously.
  struct sigaction sa_chld = {};
  sigemptyset(&sa_chld.sa_mask);
  sa_chld.sa_handler = SIG_IGN;
  sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP | SA_NOCLDWAIT;
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
  std::fprintf(stderr, "  -earlyconsole: Creates console as early as possible, for logging.\n");
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
  bool starting_bios = false;

  bool no_more_args = false;

  for (qsizetype i = 1; i < args.size(); i++)
  {
    if (!no_more_args)
    {
#define CHECK_ARG(str) (args[i] == str##_L1)
#define CHECK_ARG_PARAM(str) (args[i] == str##_L1 && ((i + 1) < args.size()))

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
        s_state.batch_mode = true;
        continue;
      }
      else if (CHECK_ARG("-nogui"))
      {
        INFO_LOG("Command Line: Using NoGUI mode.");
        s_state.nogui_mode = true;
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
        s_state.start_fullscreen_ui_fullscreen = true;
        continue;
      }
      else if (CHECK_ARG("-nofullscreen"))
      {
        INFO_LOG("Command Line: Not using fullscreen.");
        AutoBoot(autoboot)->override_fullscreen = false;
        continue;
      }
      else if (CHECK_ARG("-bigpicture"))
      {
        INFO_LOG("Command Line: Starting big picture mode.");
        s_state.start_fullscreen_ui = true;
        continue;
      }
      else if (CHECK_ARG("-setupwizard"))
      {
        s_state.run_setup_wizard = true;
        continue;
      }
      else if (CHECK_ARG("-earlyconsole"))
      {
        InitializeEarlyConsole();
        continue;
      }
      else if (CHECK_ARG("-updatecleanup"))
      {
        s_state.cleanup_after_update = true;
        continue;
      }
      else if (CHECK_ARG("--"))
      {
        no_more_args = true;
        continue;
      }
      else if (args[i][0] == QChar('-'))
      {
        QMessageBox::critical(nullptr, "DuckStation"_L1, QString("Unknown parameter: %1"_L1).arg(args[i]));
        return false;
      }

#undef CHECK_ARG
#undef CHECK_ARG_PARAM
    }

    if (autoboot && !autoboot->path.empty())
      autoboot->path += ' ';
    AutoBoot(autoboot)->path += args[i].toStdString();
  }

  // To do anything useful, we need the config initialized.
  Error error;
  if (!InitializeFoldersAndConfig(&error) || !LoadResources(&error))
  {
    // NOTE: No point translating this, because no config means the language won't be loaded anyway.
    QMessageBox::critical(nullptr, "DuckStation"_L1, QString::fromStdString(error.GetDescription()));
    return false;
  }

  // Check the file we're starting actually exists.
  if (autoboot && !autoboot->path.empty() && !CDImage::IsDeviceName(autoboot->path.c_str()))
  {
    autoboot->path = Path::RealPath(autoboot->path);

    if (!FileSystem::FileExists(autoboot->path.c_str()))
    {
      QMessageBox::critical(
        nullptr, qApp->translate("QtHost", "Error"),
        qApp->translate("QtHost", "File '%1' does not exist.").arg(QString::fromStdString(autoboot->path)));
      return false;
    }
  }

  if (state_index.has_value())
  {
    AutoBoot(autoboot);

    if (autoboot->path.empty())
    {
      // loading global state, -1 means resume the last game
      if (state_index.value() < 0)
        autoboot->save_state = System::GetMostRecentResumeSaveStatePath();
      else
        autoboot->save_state = System::GetGlobalSaveStatePath(state_index.value());
    }
    else
    {
      // loading game state
      const std::string game_serial = GameDatabase::GetSerialForPath(autoboot->path.c_str());
      autoboot->save_state = System::GetGameSaveStatePath(game_serial, state_index.value());
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
  if (autoboot && autoboot->path.empty() && autoboot->save_state.empty() && !starting_bios)
    autoboot.reset();

  // nogui implies batch mode if not running big picture mode
  s_state.batch_mode = (s_state.batch_mode || (s_state.nogui_mode && !s_state.start_fullscreen_ui));

  // if we don't have autoboot, we definitely don't want batch mode (because that'll skip
  // scanning the game list).
  if (s_state.batch_mode && !autoboot && !s_state.start_fullscreen_ui)
  {
    QMessageBox::critical(
      nullptr, qApp->translate("QtHost", "Error"),
      s_state.nogui_mode ?
        qApp->translate("QtHost", "Cannot use no-gui mode, because no boot filename was specified.") :
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
  Core::SetBaseBoolSettingValue("Main", "SetupWizardIncomplete", false);
  Host::CommitBaseSettingChanges();
  return true;
}

int main(int argc, char* argv[])
{
  if (!QtHost::VeryEarlyProcessStartup())
    return EXIT_FAILURE;

  QApplication app(argc, argv);
  if (!QtHost::PerformEarlyHardwareChecks())
    return EXIT_FAILURE;

#ifdef __linux__
  // Normally we'd have this shitfuckery in VeryEarlyProcessStartup(), but QApplication needs to be
  // created before the platform plugin is loaded. This is only here because GNOME plus Wankland
  // and their implementation of it is fucking terrible.
  QtHost::ApplyWaylandWorkarounds();
#endif

  // Type registration has to happen after hardware checks, clang emits ptest instructions otherwise.
  QtHost::RegisterTypes();

  std::shared_ptr<SystemBootParameters> autoboot;
  if (!QtHost::ParseCommandLineParametersAndInitializeConfig(app, autoboot))
    return EXIT_FAILURE;

  if (!QtHost::EarlyProcessStartup())
    return EXIT_FAILURE;

  // Remove any previous-version remnants.
  if (QtHost::s_state.cleanup_after_update)
    AutoUpdaterDialog::cleanupAfterUpdate();

  // Set theme before creating any windows.
  QtHost::UpdateApplicationTheme();

  // Build warning.
  AutoUpdaterDialog::warnAboutUnofficialBuild();

  // Create core thread object, but don't start it yet. That way the main window can connect to it,
  // and ensures that no signals are lost. Then we create and connect the main window.
  g_core_thread = new CoreThread();

  // Make sure the main window is the first window created.
  new MainWindow();

  // Start logging early, but don't show the window yet.
  // We want to catch the early messages, but if the window is shown the Windows taskbar will use
  // the log window icon as the application icon because it's the first window shown.
  LogWindow::updateSettings(true);

  // Now we can actually start the CPU thread.
  QtHost::HookSignals();
  g_core_thread->start();

  // Optionally run setup wizard.
  int result;
  if (QtHost::s_state.run_setup_wizard && !QtHost::RunSetupWizard())
  {
    result = EXIT_FAILURE;
    goto shutdown_and_exit;
  }

#ifdef __linux__
  // Create desktop file if it does not exist.
  QtHost::CheckDesktopFile();

  // I hate this so much. Turns out not only is window raising non-functional on GNOME for what I'm guessing
  // is purely political reasons, it's also very unreliable on KDE too. Sometimes raising works, other times
  // it doesn't. Deferring the request doesn't seem to help either. Can't be arsed to debug, just force all
  // Wayland down the reverse path of showing the log window first.
  if (QtHost::IsRunningOnWayland())
  {
    LogWindow::deferredShow();
    QApplication::sync();
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
  }
#endif

  // When running in batch mode, ensure game list is loaded, but don't scan for any new files.
  if (!QtHost::s_state.batch_mode)
    g_main_window->refreshGameList(false);

  // Don't bother showing the window in no-gui mode.
  if (!QtHost::s_state.nogui_mode)
    QtUtils::ShowOrRaiseWindow(g_main_window, nullptr, true);

  // Initialize big picture mode if requested.
  if (QtHost::s_state.start_fullscreen_ui)
    g_core_thread->startFullscreenUI();
  else
    QtHost::s_state.start_fullscreen_ui_fullscreen = false;

  // Always kick off update check. It'll take over if the user is booting a game fullscreen.
  g_main_window->startupUpdateCheck();

  if (autoboot)
    g_core_thread->bootSystem(std::move(autoboot));

  // Bring the log window up last, so that its icon does not take precedence.
  if (LogWindow::deferredShow())
  {
    // But ensure the main window still has focus.
    QtUtils::RaiseWindow(g_main_window);
  }

  // This doesn't return until we exit.
  result = app.exec();

shutdown_and_exit:
  if (g_main_window)
    g_main_window->close();

  // Shutting down.
  g_core_thread->stop();
  delete g_core_thread;
  g_core_thread = nullptr;

  // Close main window.
  delete g_main_window;
  Assert(!g_main_window);

  QtHost::ProcessShutdown();

  return result;
}
