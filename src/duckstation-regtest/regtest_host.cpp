// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "core/achievements.h"
#include "core/fullscreen_ui.h"
#include "core/game_list.h"
#include "core/gpu.h"
#include "core/host.h"
#include "core/system.h"

#include "scmversion/scmversion.h"

#include "util/gpu_device.h"
#include "util/imgui_manager.h"
#include "util/input_manager.h"
#include "util/platform_misc.h"

#include "common/assert.h"
#include "common/crash_handler.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/memory_settings_interface.h"
#include "common/path.h"
#include "common/string_util.h"

#include <csignal>
#include <cstdio>

Log_SetChannel(RegTestHost);

namespace RegTestHost {
static bool ParseCommandLineParameters(int argc, char* argv[], std::optional<SystemBootParameters>& autoboot);
static void PrintCommandLineVersion();
static void PrintCommandLineHelp(const char* progname);
static bool InitializeConfig();
static void InitializeEarlyConsole();
static void HookSignals();
static bool SetFolders();
static std::string GetFrameDumpFilename(u32 frame);
} // namespace RegTestHost

static std::unique_ptr<MemorySettingsInterface> s_base_settings_interface;

static u32 s_frames_to_run = 60 * 60;
static u32 s_frame_dump_interval = 0;
static std::string s_dump_base_directory;
static std::string s_dump_game_directory;

bool RegTestHost::SetFolders()
{
  std::string program_path(FileSystem::GetProgramPath());
  Log_InfoPrintf("Program Path: %s", program_path.c_str());

  EmuFolders::AppRoot = Path::Canonicalize(Path::GetDirectory(program_path));
  EmuFolders::DataRoot = EmuFolders::AppRoot;

#ifdef __APPLE__
  static constexpr char MAC_DATA_DIR[] = "Library/Application Support/DuckStation";
  const char* home_dir = getenv("HOME");
  if (home_dir)
    EmuFolders::DataRoot = Path::Combine(home_dir, MAC_DATA_DIR);
#endif

  // On Windows/Linux, these are in the binary directory.
  EmuFolders::Resources = Path::Combine(EmuFolders::AppRoot, "resources");

  Log_DevPrintf("AppRoot Directory: %s", EmuFolders::AppRoot.c_str());
  Log_DevPrintf("DataRoot Directory: %s", EmuFolders::DataRoot.c_str());
  Log_DevPrintf("Resources Directory: %s", EmuFolders::Resources.c_str());

  // Write crash dumps to the data directory, since that'll be accessible for certain.
  CrashHandler::SetWriteDirectory(EmuFolders::DataRoot);

  // the resources directory should exist, bail out if not
  if (!FileSystem::DirectoryExists(EmuFolders::Resources.c_str()))
  {
    Log_ErrorPrint("Resources directory is missing, your installation is incomplete.");
    return false;
  }

  return true;
}

bool RegTestHost::InitializeConfig()
{
  SetFolders();

  s_base_settings_interface = std::make_unique<MemorySettingsInterface>();
  Host::Internal::SetBaseSettingsLayer(s_base_settings_interface.get());

  // default settings for runner
  SettingsInterface& si = *s_base_settings_interface.get();
  g_settings.Save(si);
  si.SetStringValue("GPU", "Renderer", Settings::GetRendererName(GPURenderer::Software));
  si.SetBoolValue("GPU", "DisableShaderCache", true);
  si.SetStringValue("Pad1", "Type", Settings::GetControllerTypeName(ControllerType::AnalogController));
  si.SetStringValue("Pad2", "Type", Settings::GetControllerTypeName(ControllerType::None));
  si.SetStringValue("MemoryCards", "Card1Type", Settings::GetMemoryCardTypeName(MemoryCardType::NonPersistent));
  si.SetStringValue("MemoryCards", "Card2Type", Settings::GetMemoryCardTypeName(MemoryCardType::None));
  si.SetStringValue("ControllerPorts", "MultitapMode", Settings::GetMultitapModeName(MultitapMode::Disabled));
  si.SetStringValue("Audio", "Backend", Settings::GetAudioBackendName(AudioBackend::Null));
  si.SetBoolValue("Logging", "LogToConsole", true);
  si.SetBoolValue("Main", "ApplyGameSettings", false); // don't want game settings interfering
  si.SetBoolValue("BIOS", "PatchFastBoot", true);      // no point validating the bios intro..
  si.SetFloatValue("Main", "EmulationSpeed", 0.0f);

  // disable all sources
  for (u32 i = 0; i < static_cast<u32>(InputSourceType::Count); i++)
    si.SetBoolValue("InputSources", InputManager::InputSourceToString(static_cast<InputSourceType>(i)), false);

  EmuFolders::LoadConfig(*s_base_settings_interface.get());
  EmuFolders::EnsureFoldersExist();

  return true;
}

void Host::ReportErrorAsync(const std::string_view& title, const std::string_view& message)
{
  if (!title.empty() && !message.empty())
  {
    Log_ErrorPrintf("ReportErrorAsync: %.*s: %.*s", static_cast<int>(title.size()), title.data(),
                    static_cast<int>(message.size()), message.data());
  }
  else if (!message.empty())
  {
    Log_ErrorPrintf("ReportErrorAsync: %.*s", static_cast<int>(message.size()), message.data());
  }
}

bool Host::ConfirmMessage(const std::string_view& title, const std::string_view& message)
{
  if (!title.empty() && !message.empty())
  {
    Log_ErrorPrintf("ConfirmMessage: %.*s: %.*s", static_cast<int>(title.size()), title.data(),
                    static_cast<int>(message.size()), message.data());
  }
  else if (!message.empty())
  {
    Log_ErrorPrintf("ConfirmMessage: %.*s", static_cast<int>(message.size()), message.data());
  }

  return true;
}

void Host::ReportDebuggerMessage(const std::string_view& message)
{
  Log_ErrorPrintf("ReportDebuggerMessage: %.*s", static_cast<int>(message.size()), message.data());
}

std::span<const std::pair<const char*, const char*>> Host::GetAvailableLanguageList()
{
  return {};
}

bool Host::ChangeLanguage(const char* new_language)
{
  return false;
}

s32 Host::Internal::GetTranslatedStringImpl(const std::string_view& context, const std::string_view& msg, char* tbuf,
                                            size_t tbuf_space)
{
  if (msg.size() > tbuf_space)
    return -1;
  else if (msg.empty())
    return 0;

  std::memcpy(tbuf, msg.data(), msg.size());
  return static_cast<s32>(msg.size());
}

void Host::LoadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock)
{
}

void Host::CheckForSettingsChanges(const Settings& old_settings)
{
}

void Host::CommitBaseSettingChanges()
{
  // noop, in memory
}

bool Host::ResourceFileExists(std::string_view filename, bool allow_override)
{
  const std::string path(Path::Combine(EmuFolders::Resources, filename));
  return FileSystem::FileExists(path.c_str());
}

std::optional<std::vector<u8>> Host::ReadResourceFile(std::string_view filename, bool allow_override)
{
  const std::string path(Path::Combine(EmuFolders::Resources, filename));
  std::optional<std::vector<u8>> ret(FileSystem::ReadBinaryFile(path.c_str()));
  if (!ret.has_value())
    Log_ErrorPrintf("Failed to read resource file '%s'", filename);
  return ret;
}

std::optional<std::string> Host::ReadResourceFileToString(std::string_view filename, bool allow_override)
{
  const std::string path(Path::Combine(EmuFolders::Resources, filename));
  std::optional<std::string> ret(FileSystem::ReadFileToString(path.c_str()));
  if (!ret.has_value())
    Log_ErrorPrintf("Failed to read resource file to string '%s'", filename);
  return ret;
}

std::optional<std::time_t> Host::GetResourceFileTimestamp(std::string_view filename, bool allow_override)
{
  const std::string path(Path::Combine(EmuFolders::Resources, filename));
  FILESYSTEM_STAT_DATA sd;
  if (!FileSystem::StatFile(path.c_str(), &sd))
  {
    Log_ErrorPrintf("Failed to stat resource file '%s'", filename);
    return std::nullopt;
  }

  return sd.ModificationTime;
}

void Host::OnSystemStarting()
{
  //
}

void Host::OnSystemStarted()
{
  //
}

void Host::OnSystemDestroyed()
{
  //
}

void Host::OnSystemPaused()
{
  //
}

void Host::OnSystemResumed()
{
  //
}

void Host::OnIdleStateChanged()
{
  //
}

void Host::OnPerformanceCountersUpdated()
{
  //
}

void Host::OnGameChanged(const std::string& disc_path, const std::string& game_serial, const std::string& game_name)
{
  Log_InfoPrintf("Disc Path: %s", disc_path.c_str());
  Log_InfoPrintf("Game Serial: %s", game_serial.c_str());
  Log_InfoPrintf("Game Name: %s", game_name.c_str());

  if (!s_dump_base_directory.empty())
  {
    s_dump_game_directory = Path::Combine(s_dump_base_directory, game_name);
    if (!FileSystem::DirectoryExists(s_dump_game_directory.c_str()))
    {
      Log_InfoPrintf("Creating directory '%s'...", s_dump_game_directory.c_str());
      if (!FileSystem::CreateDirectory(s_dump_game_directory.c_str(), false))
        Panic("Failed to create dump directory.");
    }

    Log_InfoPrintf("Dumping frames to '%s'...", s_dump_game_directory.c_str());
  }
}

void Host::PumpMessagesOnCPUThread()
{
  s_frames_to_run--;
  if (s_frames_to_run == 0)
    System::ShutdownSystem(false);
}

void Host::RunOnCPUThread(std::function<void()> function, bool block /* = false */)
{
  // only one thread in this version...
  function();
}

void Host::RequestResizeHostDisplay(s32 width, s32 height)
{
  //
}

void Host::RequestExit(bool save_state_if_running)
{
  //
}

void Host::RequestSystemShutdown(bool allow_confirm, bool save_state)
{
  //
}

bool Host::IsFullscreen()
{
  return false;
}

void Host::SetFullscreen(bool enabled)
{
  //
}

std::optional<WindowInfo> Host::AcquireRenderWindow(bool recreate_window)
{
  WindowInfo wi;
  wi.SetSurfaceless();
  return wi;
}

void Host::ReleaseRenderWindow()
{
  //
}

void Host::BeginPresentFrame()
{
  const u32 frame = System::GetFrameNumber();
  if (s_frame_dump_interval > 0 && (s_frame_dump_interval == 1 || (frame % s_frame_dump_interval) == 0))
  {
    std::string dump_filename(RegTestHost::GetFrameDumpFilename(frame));
    g_gpu->WriteDisplayTextureToFile(std::move(dump_filename));
  }
}

void Host::OpenURL(const std::string_view& url)
{
  //
}

bool Host::CopyTextToClipboard(const std::string_view& text)
{
  return false;
}

void Host::SetMouseMode(bool relative, bool hide_cursor)
{
  //
}

void Host::OnAchievementsLoginRequested(Achievements::LoginRequestReason reason)
{
  // noop
}

void Host::OnAchievementsLoginSuccess(const char* username, u32 points, u32 sc_points, u32 unread_messages)
{
  // noop
}

void Host::OnAchievementsRefreshed()
{
  // noop
}

void Host::OnAchievementsHardcoreModeChanged(bool enabled)
{
  // noop
}

void Host::OnCoverDownloaderOpenRequested()
{
  // noop
}

std::optional<u32> InputManager::ConvertHostKeyboardStringToCode(const std::string_view& str)
{
  return std::nullopt;
}

std::optional<std::string> InputManager::ConvertHostKeyboardCodeToString(u32 code)
{
  return std::nullopt;
}

const char* InputManager::ConvertHostKeyboardCodeToIcon(u32 code)
{
  return nullptr;
}

void Host::AddFixedInputBindings(SettingsInterface& si)
{
  // noop
}

void Host::OnInputDeviceConnected(const std::string_view& identifier, const std::string_view& device_name)
{
  // noop
}

void Host::OnInputDeviceDisconnected(const std::string_view& identifier)
{
  // noop
}

std::optional<WindowInfo> Host::GetTopLevelWindowInfo()
{
  return std::nullopt;
}

void Host::RefreshGameListAsync(bool invalidate_cache)
{
  // noop
}

void Host::CancelGameListRefresh()
{
  // noop
}

BEGIN_HOTKEY_LIST(g_host_hotkeys)
END_HOTKEY_LIST()

static void SignalHandler(int signal)
{
  std::signal(signal, SIG_DFL);

  // MacOS is missing std::quick_exit() despite it being C++11...
#ifndef __APPLE__
  std::quick_exit(1);
#else
  _Exit(1);
#endif
}

void RegTestHost::HookSignals()
{
  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);
}

void RegTestHost::InitializeEarlyConsole()
{
  const bool was_console_enabled = Log::IsConsoleOutputEnabled();
  if (!was_console_enabled)
    Log::SetConsoleOutputParams(true);
}

void RegTestHost::PrintCommandLineVersion()
{
  InitializeEarlyConsole();
  std::fprintf(stderr, "DuckStation Regression Test Runner Version %s (%s)\n", g_scm_tag_str, g_scm_branch_str);
  std::fprintf(stderr, "https://github.com/stenzek/duckstation\n");
  std::fprintf(stderr, "\n");
}

void RegTestHost::PrintCommandLineHelp(const char* progname)
{
  InitializeEarlyConsole();
  PrintCommandLineVersion();
  std::fprintf(stderr, "Usage: %s [parameters] [--] [boot filename]\n", progname);
  std::fprintf(stderr, "\n");
  std::fprintf(stderr, "  -help: Displays this information and exits.\n");
  std::fprintf(stderr, "  -version: Displays version information and exits.\n");
  std::fprintf(stderr, "  -dumpdir: Set frame dump base directory (will be dumped to basedir/gametitle).\n");
  std::fprintf(stderr, "  -dumpinterval: Dumps every N frames.\n");
  std::fprintf(stderr, "  -frames: Sets the number of frames to execute.\n");
  std::fprintf(stderr, "  -log <level>: Sets the log level. Defaults to verbose.\n");
  std::fprintf(stderr, "  -renderer <renderer>: Sets the graphics renderer. Default to software.\n");
  std::fprintf(stderr, "  --: Signals that no more arguments will follow and the remaining\n"
                       "    parameters make up the filename. Use when the filename contains\n"
                       "    spaces or starts with a dash.\n");
  std::fprintf(stderr, "\n");
}

static std::optional<SystemBootParameters>& AutoBoot(std::optional<SystemBootParameters>& autoboot)
{
  if (!autoboot)
    autoboot.emplace();

  return autoboot;
}

bool RegTestHost::ParseCommandLineParameters(int argc, char* argv[], std::optional<SystemBootParameters>& autoboot)
{
  bool no_more_args = false;
  for (int i = 1; i < argc; i++)
  {
    if (!no_more_args)
    {
#define CHECK_ARG(str) !std::strcmp(argv[i], str)
#define CHECK_ARG_PARAM(str) (!std::strcmp(argv[i], str) && ((i + 1) < argc))

      if (CHECK_ARG("-help"))
      {
        PrintCommandLineHelp(argv[0]);
        return false;
      }
      else if (CHECK_ARG("-version"))
      {
        PrintCommandLineVersion();
        return false;
      }
      else if (CHECK_ARG_PARAM("-dumpdir"))
      {
        s_dump_base_directory = argv[++i];
        if (s_dump_base_directory.empty())
        {
          Log_ErrorPrintf("Invalid dump directory specified.");
          return false;
        }

        continue;
      }
      else if (CHECK_ARG_PARAM("-dumpinterval"))
      {
        s_frame_dump_interval = StringUtil::FromChars<u32>(argv[++i]).value_or(0);
        if (s_frames_to_run <= 0)
        {
          Log_ErrorPrintf("Invalid dump interval specified: %s", argv[i]);
          return false;
        }

        continue;
      }
      else if (CHECK_ARG_PARAM("-frames"))
      {
        s_frames_to_run = StringUtil::FromChars<u32>(argv[++i]).value_or(0);
        if (s_frames_to_run == 0)
        {
          Log_ErrorPrintf("Invalid frame count specified: %s", argv[i]);
          return false;
        }

        continue;
      }
      else if (CHECK_ARG_PARAM("-log"))
      {
        std::optional<LOGLEVEL> level = Settings::ParseLogLevelName(argv[++i]);
        if (!level.has_value())
        {
          Log_ErrorPrintf("Invalid log level specified.");
          return false;
        }

        Log::SetConsoleOutputParams(true, level.value());
        s_base_settings_interface->SetStringValue("Logging", "LogLevel", Settings::GetLogLevelName(level.value()));
        continue;
      }
      else if (CHECK_ARG_PARAM("-renderer"))
      {
        std::optional<GPURenderer> renderer = Settings::ParseRendererName(argv[++i]);
        if (!renderer.has_value())
        {
          Log_ErrorPrintf("Invalid renderer specified.");
          return false;
        }

        s_base_settings_interface->SetStringValue("GPU", "Renderer", Settings::GetRendererName(renderer.value()));
        continue;
      }
      else if (CHECK_ARG_PARAM("-upscale"))
      {
        const u32 upscale = StringUtil::FromChars<u32>(argv[++i]).value_or(0);
        if (upscale == 0)
        {
          Log_ErrorPrint("Invalid upscale value.");
          return false;
        }

        Log_InfoFmt("Setting upscale to {}.", upscale);
        s_base_settings_interface->SetIntValue("GPU", "ResolutionScale", static_cast<s32>(upscale));
        continue;
      }
      else if (CHECK_ARG_PARAM("-cpu"))
      {
        const std::optional<CPUExecutionMode> cpu = Settings::ParseCPUExecutionMode(argv[++i]);
        if (!cpu.has_value())
        {
          Log_ErrorPrint("Invalid CPU execution mode.");
          return false;
        }

        Log_InfoFmt("Setting CPU execution mode to {}.", Settings::GetCPUExecutionModeName(cpu.value()));
        s_base_settings_interface->SetStringValue("CPU", "ExecutionMode",
                                                  Settings::GetCPUExecutionModeName(cpu.value()));
        continue;
      }
      else if (CHECK_ARG("-pgxp"))
      {
        Log_InfoPrint("Enabling PGXP.");
        s_base_settings_interface->SetBoolValue("GPU", "PGXPEnable", true);
        continue;
      }
      else if (CHECK_ARG("-pgxp-cpu"))
      {
        Log_InfoPrint("Enabling PGXP CPU mode.");
        s_base_settings_interface->SetBoolValue("GPU", "PGXPEnable", true);
        s_base_settings_interface->SetBoolValue("GPU", "PGXPCPU", true);
        continue;
      }
      else if (CHECK_ARG("--"))
      {
        no_more_args = true;
        continue;
      }
      else if (argv[i][0] == '-')
      {
        Log_ErrorPrintf("Unknown parameter: '%s'", argv[i]);
        return false;
      }

#undef CHECK_ARG
#undef CHECK_ARG_PARAM
    }

    if (autoboot && !autoboot->filename.empty())
      autoboot->filename += ' ';
    AutoBoot(autoboot)->filename += argv[i];
  }

  return true;
}

std::string RegTestHost::GetFrameDumpFilename(u32 frame)
{
  return Path::Combine(s_dump_game_directory, fmt::format("frame_{:05d}.png", frame));
}

int main(int argc, char* argv[])
{
  RegTestHost::InitializeEarlyConsole();

  if (!RegTestHost::InitializeConfig())
    return EXIT_FAILURE;

  std::optional<SystemBootParameters> autoboot;
  if (!RegTestHost::ParseCommandLineParameters(argc, argv, autoboot))
    return EXIT_FAILURE;

  if (!autoboot || autoboot->filename.empty())
  {
    Log_ErrorPrintf("No boot path specified.");
    return EXIT_FAILURE;
  }

  System::Internal::ProcessStartup();
  RegTestHost::HookSignals();

  int result = -1;
  Log_InfoPrintf("Trying to boot '%s'...", autoboot->filename.c_str());
  if (!System::BootSystem(std::move(autoboot.value())))
  {
    Log_ErrorPrintf("Failed to boot system.");
    goto cleanup;
  }

  if (s_frame_dump_interval > 0)
  {
    if (s_dump_base_directory.empty())
    {
      Log_ErrorPrint("Dump directory not specified.");
      goto cleanup;
    }

    Log_InfoPrintf("Dumping every %dth frame to '%s'.", s_frame_dump_interval, s_dump_base_directory.c_str());
  }

  Log_InfoPrintf("Running for %d frames...", s_frames_to_run);
  System::Execute();

  Log_InfoPrintf("Exiting with success.");
  result = 0;

cleanup:
  System::Internal::ProcessShutdown();
  return result;
}
