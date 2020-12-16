#include "common_host_interface.h"
#include "common/assert.h"
#include "common/audio_stream.h"
#include "common/byte_stream.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/string_util.h"
#include "controller_interface.h"
#include "core/cdrom.h"
#include "core/cheats.h"
#include "core/cpu_code_cache.h"
#include "core/dma.h"
#include "core/gpu.h"
#include "core/host_display.h"
#include "core/mdec.h"
#include "core/pgxp.h"
#include "core/save_state_version.h"
#include "core/spu.h"
#include "core/system.h"
#include "core/timers.h"
#include "cubeb_audio_stream.h"
#include "game_list.h"
#include "icon.h"
#include "imgui.h"
#include "ini_settings_interface.h"
#include "save_state_selector_ui.h"
#include "scmversion/scmversion.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#ifdef WITH_SDL2
#include "sdl_audio_stream.h"
#endif

#ifdef WITH_DISCORD_PRESENCE
#include "discord_rpc.h"
#endif

#ifdef WIN32
#include "common/windows_headers.h"
#include <KnownFolders.h>
#include <ShlObj.h>
#include <mmsystem.h>
#endif

Log_SetChannel(CommonHostInterface);

CommonHostInterface::CommonHostInterface() = default;

CommonHostInterface::~CommonHostInterface() = default;

bool CommonHostInterface::Initialize()
{
  if (!HostInterface::Initialize())
    return false;

  SetUserDirectory();
  InitializeUserDirectory();

  // Change to the user directory so that all default/relative paths in the config are after this.
  if (!FileSystem::SetWorkingDirectory(m_user_directory.c_str()))
    Log_ErrorPrintf("Failed to set working directory to '%s'", m_user_directory.c_str());

  LoadSettings();
  UpdateLogSettings(g_settings.log_level, g_settings.log_filter.empty() ? nullptr : g_settings.log_filter.c_str(),
                    g_settings.log_to_console, g_settings.log_to_debug, g_settings.log_to_window,
                    g_settings.log_to_file);

  m_game_list = std::make_unique<GameList>();
  m_game_list->SetCacheFilename(GetUserDirectoryRelativePath("cache/gamelist.cache"));
  m_game_list->SetUserDatabaseFilename(GetUserDirectoryRelativePath("redump.dat"));
  m_game_list->SetUserCompatibilityListFilename(GetUserDirectoryRelativePath("compatibility.xml"));
  m_game_list->SetUserGameSettingsFilename(GetUserDirectoryRelativePath("gamesettings.ini"));

  m_save_state_selector_ui = std::make_unique<FrontendCommon::SaveStateSelectorUI>(this);

  RegisterGeneralHotkeys();
  RegisterGraphicsHotkeys();
  RegisterSaveStateHotkeys();
  RegisterAudioHotkeys();

  UpdateControllerInterface();
  return true;
}

void CommonHostInterface::Shutdown()
{
  HostInterface::Shutdown();

#ifdef WITH_DISCORD_PRESENCE
  ShutdownDiscordPresence();
#endif

  if (m_controller_interface)
  {
    m_controller_interface->Shutdown();
    m_controller_interface.reset();
  }
}

void CommonHostInterface::InitializeUserDirectory()
{
  std::fprintf(stdout, "User directory: \"%s\"\n", m_user_directory.c_str());

  if (m_user_directory.empty())
    Panic("Cannot continue without user directory set.");

  if (!FileSystem::DirectoryExists(m_user_directory.c_str()))
  {
    std::fprintf(stderr, "User directory \"%s\" does not exist, creating.\n", m_user_directory.c_str());
    if (!FileSystem::CreateDirectory(m_user_directory.c_str(), true))
      std::fprintf(stderr, "Failed to create user directory \"%s\".\n", m_user_directory.c_str());
  }

  bool result = true;

  result &= FileSystem::CreateDirectory(GetUserDirectoryRelativePath("bios").c_str(), false);
  result &= FileSystem::CreateDirectory(GetUserDirectoryRelativePath("cache").c_str(), false);
  result &= FileSystem::CreateDirectory(GetUserDirectoryRelativePath("cheats").c_str(), false);
  result &= FileSystem::CreateDirectory(GetUserDirectoryRelativePath("covers").c_str(), false);
  result &= FileSystem::CreateDirectory(GetUserDirectoryRelativePath("dump").c_str(), false);
  result &= FileSystem::CreateDirectory(GetUserDirectoryRelativePath("dump/audio").c_str(), false);
  result &= FileSystem::CreateDirectory(GetUserDirectoryRelativePath("inputprofiles").c_str(), false);
  result &= FileSystem::CreateDirectory(GetUserDirectoryRelativePath("savestates").c_str(), false);
  result &= FileSystem::CreateDirectory(GetUserDirectoryRelativePath("screenshots").c_str(), false);
  result &= FileSystem::CreateDirectory(GetUserDirectoryRelativePath("shaders").c_str(), false);
  result &= FileSystem::CreateDirectory(GetUserDirectoryRelativePath("memcards").c_str(), false);

  if (!result)
    ReportError("Failed to create one or more user directories. This may cause issues at runtime.");
}

bool CommonHostInterface::BootSystem(const SystemBootParameters& parameters)
{
  if (!HostInterface::BootSystem(parameters))
  {
    // if in batch mode, exit immediately if booting failed
    if (m_batch_mode)
      RequestExit();

    return false;
  }

  // enter fullscreen if requested in the parameters
  if (!g_settings.start_paused && ((parameters.override_fullscreen.has_value() && *parameters.override_fullscreen) ||
                                   (!parameters.override_fullscreen.has_value() && g_settings.start_fullscreen)))
  {
    SetFullscreen(true);
  }

  if (g_settings.audio_dump_on_boot)
    StartDumpingAudio();

  UpdateSpeedLimiterState();
  return true;
}

void CommonHostInterface::DestroySystem()
{
  SetTimerResolutionIncreased(false);
  m_save_state_selector_ui->Close();
  m_display->SetPostProcessingChain({});

  HostInterface::DestroySystem();
}

void CommonHostInterface::PowerOffSystem()
{
  if (System::IsShutdown())
    return;

  if (g_settings.save_state_on_exit)
    SaveResumeSaveState();

  HostInterface::PowerOffSystem();

  if (m_batch_mode)
    RequestExit();
}

static void PrintCommandLineVersion(const char* frontend_name)
{
  const bool was_console_enabled = Log::IsConsoleOutputEnabled();
  if (!was_console_enabled)
    Log::SetConsoleOutputParams(true);

  std::fprintf(stderr, "%s Version %s (%s)\n", frontend_name, g_scm_tag_str, g_scm_branch_str);
  std::fprintf(stderr, "https://github.com/stenzek/duckstation\n");
  std::fprintf(stderr, "\n");

  if (!was_console_enabled)
    Log::SetConsoleOutputParams(false);
}

static void PrintCommandLineHelp(const char* progname, const char* frontend_name)
{
  const bool was_console_enabled = Log::IsConsoleOutputEnabled();
  if (!was_console_enabled)
    Log::SetConsoleOutputParams(true);

  PrintCommandLineVersion(frontend_name);
  std::fprintf(stderr, "Usage: %s [parameters] [--] [boot filename]\n", progname);
  std::fprintf(stderr, "\n");
  std::fprintf(stderr, "  -help: Displays this information and exits.\n");
  std::fprintf(stderr, "  -version: Displays version information and exits.\n");
  std::fprintf(stderr, "  -batch: Enables batch mode (exits after powering off).\n");
  std::fprintf(stderr, "  -fastboot: Force fast boot for provided filename.\n");
  std::fprintf(stderr, "  -slowboot: Force slow boot for provided filename.\n");
  std::fprintf(stderr, "  -resume: Load resume save state. If a boot filename is provided,\n"
                       "    that game's resume state will be loaded, otherwise the most\n"
                       "    recent resume save state will be loaded.\n");
  std::fprintf(stderr, "  -state <index>: Loads specified save state by index. If a boot\n"
                       "    filename is provided, a per-game state will be loaded, otherwise\n"
                       "    a global state will be loaded.\n");
  std::fprintf(stderr, "  -statefile <filename>: Loads state from the specified filename.\n"
                       "    No boot filename is required with this option.\n");
  std::fprintf(stderr, "  -fullscreen: Enters fullscreen mode immediately after starting.\n");
  std::fprintf(stderr, "  -nofullscreen: Prevents fullscreen mode from triggering if enabled.\n");
  std::fprintf(stderr, "  -portable: Forces \"portable mode\", data in same directory.\n");
  std::fprintf(stderr, "  --: Signals that no more arguments will follow and the remaining\n"
                       "    parameters make up the filename. Use when the filename contains\n"
                       "    spaces or starts with a dash.\n");
  std::fprintf(stderr, "\n");

  if (!was_console_enabled)
    Log::SetConsoleOutputParams(false);
}

bool CommonHostInterface::ParseCommandLineParameters(int argc, char* argv[],
                                                     std::unique_ptr<SystemBootParameters>* out_boot_params)
{
  std::optional<bool> force_fast_boot;
  std::optional<bool> force_fullscreen;
  std::optional<s32> state_index;
  std::string state_filename;
  std::string boot_filename;
  bool no_more_args = false;

  for (int i = 1; i < argc; i++)
  {
    if (!no_more_args)
    {
#define CHECK_ARG(str) !std::strcmp(argv[i], str)
#define CHECK_ARG_PARAM(str) (!std::strcmp(argv[i], str) && ((i + 1) < argc))

      if (CHECK_ARG("-help"))
      {
        PrintCommandLineHelp(argv[0], GetFrontendName());
        return false;
      }
      else if (CHECK_ARG("-version"))
      {
        PrintCommandLineVersion(GetFrontendName());
        return false;
      }
      else if (CHECK_ARG("-batch"))
      {
        Log_InfoPrintf("Enabling batch mode.");
        m_batch_mode = true;
        continue;
      }
      else if (CHECK_ARG("-fastboot"))
      {
        Log_InfoPrintf("Forcing fast boot.");
        force_fast_boot = true;
        continue;
      }
      else if (CHECK_ARG("-slowboot"))
      {
        Log_InfoPrintf("Forcing slow boot.");
        force_fast_boot = false;
        continue;
      }
      else if (CHECK_ARG("-resume"))
      {
        state_index = -1;
        continue;
      }
      else if (CHECK_ARG_PARAM("-state"))
      {
        state_index = std::atoi(argv[++i]);
        continue;
      }
      else if (CHECK_ARG_PARAM("-statefile"))
      {
        state_filename = argv[++i];
        continue;
      }
      else if (CHECK_ARG("-fullscreen"))
      {
        Log_InfoPrintf("Going fullscreen after booting.");
        force_fullscreen = true;
        continue;
      }
      else if (CHECK_ARG("-nofullscreen"))
      {
        Log_InfoPrintf("Preventing fullscreen after booting.");
        force_fullscreen = false;
        continue;
      }
      else if (CHECK_ARG("-portable"))
      {
        Log_InfoPrintf("Using portable mode.");
        SetUserDirectoryToProgramDirectory();
        continue;
      }
      else if (CHECK_ARG_PARAM("-resume"))
      {
        state_index = -1;
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

    if (!boot_filename.empty())
      boot_filename += ' ';
    boot_filename += argv[i];
  }

  if (state_index.has_value() || !boot_filename.empty() || !state_filename.empty())
  {
    // init user directory early since we need it for save states
    SetUserDirectory();

    if (state_index.has_value() && state_filename.empty())
    {
      // if a save state is provided, whether a boot filename was provided determines per-game/local
      if (boot_filename.empty())
      {
        // loading a global state. if this is -1, we're loading the most recent resume state
        if (*state_index < 0)
          state_filename = GetMostRecentResumeSaveStatePath();
        else
          state_filename = GetGlobalSaveStateFileName(*state_index);

        if (state_filename.empty() || !FileSystem::FileExists(state_filename.c_str()))
        {
          Log_ErrorPrintf("Could not find file for global save state %d", *state_index);
          return false;
        }
      }
      else
      {
        // find the game id, and get its save state path
        std::string game_code = System::GetGameCodeForPath(boot_filename.c_str());
        if (game_code.empty())
        {
          Log_WarningPrintf("Could not identify game code for '%s', cannot load save state %d.", boot_filename.c_str(),
                            *state_index);
        }
        else
        {
          state_filename = GetGameSaveStateFileName(game_code.c_str(), *state_index);
          if (state_filename.empty() || !FileSystem::FileExists(state_filename.c_str()))
          {
            if (state_index >= 0) // Do not exit if -resume is specified, but resume save state does not exist
            {
              Log_ErrorPrintf("Could not find file for game '%s' save state %d", game_code.c_str(), *state_index);
              return false;
            }
            else
            {
              state_filename.clear();
            }
          }
        }
      }
    }

    std::unique_ptr<SystemBootParameters> boot_params = std::make_unique<SystemBootParameters>();
    boot_params->filename = std::move(boot_filename);
    boot_params->override_fast_boot = std::move(force_fast_boot);
    boot_params->override_fullscreen = std::move(force_fullscreen);

    if (!state_filename.empty())
    {
      std::unique_ptr<ByteStream> state_stream =
        FileSystem::OpenFile(state_filename.c_str(), BYTESTREAM_OPEN_READ | BYTESTREAM_OPEN_STREAMED);
      if (!state_stream)
      {
        Log_ErrorPrintf("Failed to open save state file '%s'", state_filename.c_str());
        return false;
      }

      boot_params->state_stream = std::move(state_stream);
    }

    *out_boot_params = std::move(boot_params);
  }

  return true;
}

void CommonHostInterface::PollAndUpdate()
{
#ifdef WITH_DISCORD_PRESENCE
  PollDiscordPresence();
#endif
}

bool CommonHostInterface::IsFullscreen() const
{
  return false;
}

bool CommonHostInterface::SetFullscreen(bool enabled)
{
  return false;
}

bool CommonHostInterface::CreateHostDisplayResources()
{
  m_logo_texture =
    m_display->CreateTexture(APP_ICON_WIDTH, APP_ICON_HEIGHT, APP_ICON_DATA, sizeof(u32) * APP_ICON_WIDTH, false);
  if (!m_logo_texture)
    Log_WarningPrintf("Failed to create logo texture");

  return true;
}

void CommonHostInterface::ReleaseHostDisplayResources()
{
  m_logo_texture.reset();
}

std::unique_ptr<AudioStream> CommonHostInterface::CreateAudioStream(AudioBackend backend)
{
  switch (backend)
  {
    case AudioBackend::Null:
      return AudioStream::CreateNullAudioStream();

    case AudioBackend::Cubeb:
      return CubebAudioStream::Create();

#ifdef WITH_SDL2
    case AudioBackend::SDL:
      return SDLAudioStream::Create();
#endif

    default:
      return nullptr;
  }
}

s32 CommonHostInterface::GetAudioOutputVolume() const
{
  return g_settings.GetAudioOutputVolume(!m_speed_limiter_enabled);
}

void CommonHostInterface::UpdateControllerInterface()
{
  const std::string backend_str = GetStringSettingValue(
    "Main", "ControllerBackend", ControllerInterface::GetBackendName(ControllerInterface::GetDefaultBackend()));
  const std::optional<ControllerInterface::Backend> new_backend =
    ControllerInterface::ParseBackendName(backend_str.c_str());
  const ControllerInterface::Backend current_backend =
    (m_controller_interface ? m_controller_interface->GetBackend() : ControllerInterface::Backend::None);
  if (new_backend == current_backend)
    return;

  if (m_controller_interface)
  {
    ClearInputMap();
    m_controller_interface->Shutdown();
    m_controller_interface.reset();
  }

  if (!new_backend.has_value())
  {
    Log_ErrorPrintf("Invalid controller interface type: '%s'", backend_str.c_str());
    return;
  }

  if (new_backend == ControllerInterface::Backend::None)
  {
    Log_WarningPrintf("No controller interface created, controller bindings are not possible.");
    return;
  }

  m_controller_interface = ControllerInterface::Create(new_backend.value());
  if (!m_controller_interface || !m_controller_interface->Initialize(this))
  {
    Log_WarningPrintf("Failed to initialize controller interface, bindings are not possible.");
    if (m_controller_interface)
    {
      m_controller_interface->Shutdown();
      m_controller_interface.reset();
    }
  }
}

bool CommonHostInterface::LoadState(bool global, s32 slot)
{
  if (!global && (System::IsShutdown() || System::GetRunningCode().empty()))
  {
    ReportFormattedError("Can't save per-game state without a running game code.");
    return false;
  }

  std::string save_path =
    global ? GetGlobalSaveStateFileName(slot) : GetGameSaveStateFileName(System::GetRunningCode().c_str(), slot);
  return LoadState(save_path.c_str());
}

bool CommonHostInterface::SaveState(bool global, s32 slot)
{
  const std::string& code = System::GetRunningCode();
  if (!global && code.empty())
  {
    ReportFormattedError("Can't save per-game state without a running game code.");
    return false;
  }

  std::string save_path = global ? GetGlobalSaveStateFileName(slot) : GetGameSaveStateFileName(code.c_str(), slot);
  if (!SaveState(save_path.c_str()))
    return false;

  OnSystemStateSaved(global, slot);
  return true;
}

bool CommonHostInterface::ResumeSystemFromState(const char* filename, bool boot_on_failure)
{
  SystemBootParameters boot_params;
  boot_params.filename = filename;
  if (!BootSystem(boot_params))
    return false;

  const bool global = System::GetRunningCode().empty();
  if (global)
  {
    ReportFormattedError("Cannot resume system with undetectable game code from '%s'.", filename);
    if (!boot_on_failure)
    {
      DestroySystem();
      return true;
    }
  }
  else
  {
    const std::string path = GetGameSaveStateFileName(System::GetRunningCode().c_str(), -1);
    if (FileSystem::FileExists(path.c_str()))
    {
      if (!LoadState(path.c_str()) && !boot_on_failure)
      {
        DestroySystem();
        return false;
      }
    }
    else if (!boot_on_failure)
    {
      ReportFormattedError("Resume save state not found for '%s' ('%s').", System::GetRunningCode().c_str(),
                           System::GetRunningTitle().c_str());
      DestroySystem();
      return false;
    }
  }

  return true;
}

bool CommonHostInterface::ResumeSystemFromMostRecentState()
{
  const std::string path = GetMostRecentResumeSaveStatePath();
  if (path.empty())
  {
    ReportError("No resume save state found.");
    return false;
  }

  return LoadState(path.c_str());
}

void CommonHostInterface::UpdateSpeedLimiterState()
{
  const float target_speed = m_fast_forward_enabled ? g_settings.fast_forward_speed : g_settings.emulation_speed;
  m_speed_limiter_enabled = (target_speed != 0.0f);

  const bool is_non_standard_speed = (std::abs(target_speed - 1.0f) > 0.05f);
  const bool audio_sync_enabled =
    !System::IsRunning() || (m_speed_limiter_enabled && g_settings.audio_sync_enabled && !is_non_standard_speed);
  const bool video_sync_enabled =
    !System::IsRunning() || (m_speed_limiter_enabled && g_settings.video_sync_enabled && !is_non_standard_speed);
  const float max_display_fps = m_speed_limiter_enabled ? 0.0f : g_settings.display_max_fps;
  Log_InfoPrintf("Syncing to %s%s", audio_sync_enabled ? "audio" : "",
                 (audio_sync_enabled && video_sync_enabled) ? " and video" : (video_sync_enabled ? "video" : ""));
  Log_InfoPrintf("Max display fps: %f", max_display_fps);

  if (m_audio_stream)
  {
    m_audio_stream->SetOutputVolume(GetAudioOutputVolume());
    m_audio_stream->SetSync(audio_sync_enabled);
    if (audio_sync_enabled)
      m_audio_stream->EmptyBuffers();
  }

  if (m_display)
  {
    m_display->SetDisplayMaxFPS(max_display_fps);
    m_display->SetVSync(video_sync_enabled);
  }

  if (g_settings.increase_timer_resolution)
    SetTimerResolutionIncreased(m_speed_limiter_enabled);

  if (System::IsValid())
  {
    System::SetTargetSpeed(m_speed_limiter_enabled ? target_speed : 1.0f);
    System::ResetPerformanceCounters();
  }
}

void CommonHostInterface::RecreateSystem()
{
  const bool was_paused = System::IsPaused();
  HostInterface::RecreateSystem();
  if (was_paused)
    PauseSystem(true);
}

void CommonHostInterface::UpdateLogSettings(LOGLEVEL level, const char* filter, bool log_to_console, bool log_to_debug,
                                            bool log_to_window, bool log_to_file)
{
  Log::SetFilterLevel(level);
  Log::SetConsoleOutputParams(g_settings.log_to_console, filter, level);
  Log::SetDebugOutputParams(g_settings.log_to_debug, filter, level);

  if (log_to_file)
  {
    Log::SetFileOutputParams(g_settings.log_to_file, GetUserDirectoryRelativePath("duckstation.log").c_str(), true,
                             filter, level);
  }
  else
  {
    Log::SetFileOutputParams(false, nullptr);
  }
}

void CommonHostInterface::SetUserDirectory()
{
  if (!m_user_directory.empty())
    return;

  std::fprintf(stdout, "Program directory \"%s\"\n", m_program_directory.c_str());

  if (FileSystem::FileExists(
        StringUtil::StdStringFromFormat("%s" FS_OSPATH_SEPARATOR_STR "%s", m_program_directory.c_str(), "portable.txt")
          .c_str()) ||
      FileSystem::FileExists(
        StringUtil::StdStringFromFormat("%s" FS_OSPATH_SEPARATOR_STR "%s", m_program_directory.c_str(), "settings.ini")
          .c_str()))
  {
    std::fprintf(stdout, "portable.txt or old settings.ini found, using program directory as user directory.\n");
    m_user_directory = m_program_directory;
  }
  else
  {
#ifdef WIN32
    // On Windows, use My Documents\DuckStation.
    PWSTR documents_directory;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, NULL, &documents_directory)))
    {
      const std::string documents_directory_str(StringUtil::WideStringToUTF8String(documents_directory));
      if (!documents_directory_str.empty())
      {
        m_user_directory = StringUtil::StdStringFromFormat("%s" FS_OSPATH_SEPARATOR_STR "%s",
                                                           documents_directory_str.c_str(), "DuckStation");
      }
      CoTaskMemFree(documents_directory);
    }
#elif __linux__
    // On Linux, use .local/share/duckstation as a user directory by default.
    const char* xdg_data_home = getenv("XDG_DATA_HOME");
    if (xdg_data_home && xdg_data_home[0] == '/')
    {
      m_user_directory = StringUtil::StdStringFromFormat("%s/duckstation", xdg_data_home);
    }
    else
    {
      const char* home_path = getenv("HOME");
      if (home_path)
        m_user_directory = StringUtil::StdStringFromFormat("%s/.local/share/duckstation", home_path);
    }
#elif __APPLE__
    // On macOS, default to ~/Library/Application Support/DuckStation.
    const char* home_path = getenv("HOME");
    if (home_path)
      m_user_directory = StringUtil::StdStringFromFormat("%s/Library/Application Support/DuckStation", home_path);
#endif

    if (m_user_directory.empty())
    {
      std::fprintf(stderr, "User directory path could not be determined, falling back to program directory.");
      m_user_directory = m_program_directory;
    }
  }
}

void CommonHostInterface::OnSystemCreated()
{
  HostInterface::OnSystemCreated();

  if (g_settings.display_post_processing && !m_display->SetPostProcessingChain(g_settings.display_post_process_chain))
    AddOSDMessage(TranslateStdString("OSDMessage", "Failed to load post processing shader chain."), 20.0f);
}

void CommonHostInterface::OnSystemPaused(bool paused)
{
  ReportFormattedMessage("System %s.", paused ? "paused" : "resumed");

  if (paused)
  {
    if (IsFullscreen())
      SetFullscreen(false);

    StopControllerRumble();
  }

  UpdateSpeedLimiterState();
}

void CommonHostInterface::OnSystemDestroyed()
{
  HostInterface::OnSystemDestroyed();

  StopControllerRumble();
}

void CommonHostInterface::OnRunningGameChanged()
{
  HostInterface::OnRunningGameChanged();

  if (!System::IsShutdown())
  {
    System::SetCheatList(nullptr);
    if (g_settings.auto_load_cheats)
      LoadCheatListFromGameTitle();
  }

#ifdef WITH_DISCORD_PRESENCE
  UpdateDiscordPresence();
#endif
}

void CommonHostInterface::OnControllerTypeChanged(u32 slot)
{
  HostInterface::OnControllerTypeChanged(slot);

  UpdateInputMap();
}

void CommonHostInterface::DrawImGuiWindows()
{
  if (System::IsValid())
  {
    DrawDebugWindows();
    DrawFPSWindow();
  }

  DrawOSDMessages();

  if (m_save_state_selector_ui->IsOpen())
    m_save_state_selector_ui->Draw();
}

void CommonHostInterface::DrawFPSWindow()
{
  if (!(g_settings.display_show_fps | g_settings.display_show_vps | g_settings.display_show_speed |
        g_settings.display_show_resolution))
  {
    return;
  }

  const ImVec2 window_size =
    ImVec2(175.0f * ImGui::GetIO().DisplayFramebufferScale.x, 48.0f * ImGui::GetIO().DisplayFramebufferScale.y);
  ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - window_size.x, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(window_size);

  if (!ImGui::Begin("FPSWindow", nullptr,
                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse |
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMouseInputs |
                      ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav))
  {
    ImGui::End();
    return;
  }

  bool first = true;
  if (g_settings.display_show_fps)
  {
    ImGui::Text("%.2f", System::GetFPS());
    first = false;
  }
  if (g_settings.display_show_vps)
  {
    if (first)
    {
      first = false;
    }
    else
    {
      ImGui::SameLine();
      ImGui::Text("/");
      ImGui::SameLine();
    }

    ImGui::Text("%.2f", System::GetVPS());
  }
  if (g_settings.display_show_speed)
  {
    if (first)
    {
      first = false;
    }
    else
    {
      ImGui::SameLine();
      ImGui::Text("/");
      ImGui::SameLine();
    }

    const float speed = System::GetEmulationSpeed();
    const u32 rounded_speed = static_cast<u32>(std::round(speed));
    if (speed < 90.0f)
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%u%%", rounded_speed);
    else if (speed < 110.0f)
      ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "%u%%", rounded_speed);
    else
      ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%u%%", rounded_speed);
  }

  if (g_settings.display_show_resolution)
  {
    const auto [effective_width, effective_height] = g_gpu->GetEffectiveDisplayResolution();
    const bool interlaced = g_gpu->IsInterlacedDisplayEnabled();
    ImGui::Text("%ux%u (%s)", effective_width, effective_height, interlaced ? "interlaced" : "progressive");
  }

  ImGui::End();
}

void CommonHostInterface::AddOSDMessage(std::string message, float duration /*= 2.0f*/)
{
  OSDMessage msg;
  msg.text = std::move(message);
  msg.duration = duration;

  std::unique_lock<std::mutex> lock(m_osd_messages_lock);
  m_osd_messages.push_back(std::move(msg));
}

void CommonHostInterface::ClearOSDMessages()
{
  std::unique_lock<std::mutex> lock(m_osd_messages_lock);
  m_osd_messages.clear();
}

void CommonHostInterface::DrawOSDMessages()
{
  constexpr ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs |
                                            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav |
                                            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing;

  std::unique_lock<std::mutex> lock(m_osd_messages_lock);
  if (m_osd_messages.empty())
    return;

  const float scale = ImGui::GetIO().DisplayFramebufferScale.x;

  auto iter = m_osd_messages.begin();
  float position_x = 10.0f * scale;
  float position_y = (10.0f + (static_cast<float>(m_display->GetDisplayTopMargin()))) * scale;
  u32 index = 0;
  while (iter != m_osd_messages.end())
  {
    const OSDMessage& msg = *iter;
    const double time = msg.time.GetTimeSeconds();
    const float time_remaining = static_cast<float>(msg.duration - time);
    if (time_remaining <= 0.0f)
    {
      iter = m_osd_messages.erase(iter);
      continue;
    }

    if (!g_settings.display_show_osd_messages)
    {
      ++iter;
      continue;
    }

    const float opacity = std::min(time_remaining, 1.0f);
    ImGui::SetNextWindowPos(ImVec2(position_x, position_y));
    ImGui::SetNextWindowSize(ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, opacity);

    char buf[64];
    std::snprintf(buf, sizeof(buf), "osd_%u", index++);

    if (ImGui::Begin(buf, nullptr, window_flags))
    {
      ImGui::TextUnformatted(msg.text.c_str());
      position_y += ImGui::GetWindowSize().y + (4.0f * scale);
    }

    ImGui::End();
    ImGui::PopStyleVar();
    ++iter;
  }
}

void CommonHostInterface::DrawDebugWindows()
{
  if (g_settings.debugging.show_gpu_state)
    g_gpu->DrawDebugStateWindow();
  if (g_settings.debugging.show_cdrom_state)
    g_cdrom.DrawDebugWindow();
  if (g_settings.debugging.show_timers_state)
    g_timers.DrawDebugStateWindow();
  if (g_settings.debugging.show_spu_state)
    g_spu.DrawDebugStateWindow();
  if (g_settings.debugging.show_mdec_state)
    g_mdec.DrawDebugStateWindow();
  if (g_settings.debugging.show_dma_state)
    g_dma.DrawDebugStateWindow();
}

void CommonHostInterface::DoFrameStep()
{
  if (System::IsShutdown())
    return;

  m_frame_step_request = true;
  PauseSystem(false);
}

void CommonHostInterface::DoToggleCheats()
{
  if (System::IsShutdown())
    return;

  CheatList* cl = System::GetCheatList();
  if (!cl)
  {
    AddOSDMessage(TranslateStdString("OSDMessage", "No cheats are loaded."), 10.0f);
    return;
  }

  cl->SetMasterEnable(!cl->GetMasterEnable());
  AddFormattedOSDMessage(10.0f,
                         cl->GetMasterEnable() ? TranslateString("OSDMessage", "%u cheats are now active.") :
                                                 TranslateString("OSDMessage", "%u cheats are now inactive."),
                         cl->GetEnabledCodeCount());
}

std::optional<CommonHostInterface::HostKeyCode>
CommonHostInterface::GetHostKeyCode(const std::string_view key_code) const
{
  return std::nullopt;
}

void CommonHostInterface::RegisterHotkey(String category, String name, String display_name, InputButtonHandler handler)
{
  m_hotkeys.push_back(HotkeyInfo{std::move(category), std::move(name), std::move(display_name), std::move(handler)});
}

bool CommonHostInterface::HandleHostKeyEvent(HostKeyCode key, bool pressed)
{
  const auto iter = m_keyboard_input_handlers.find(key);
  if (iter == m_keyboard_input_handlers.end())
    return false;

  iter->second(pressed);
  return true;
}

bool CommonHostInterface::HandleHostMouseEvent(HostMouseButton button, bool pressed)
{
  const auto iter = m_mouse_input_handlers.find(button);
  if (iter == m_mouse_input_handlers.end())
    return false;

  iter->second(pressed);
  return true;
}

void CommonHostInterface::UpdateInputMap(SettingsInterface& si)
{
  ClearInputMap();

  if (!UpdateControllerInputMapFromGameSettings())
    UpdateControllerInputMap(si);

  UpdateHotkeyInputMap(si);
}

void CommonHostInterface::ClearInputMap()
{
  m_keyboard_input_handlers.clear();
  m_mouse_input_handlers.clear();
  m_controller_vibration_motors.clear();
  if (m_controller_interface)
    m_controller_interface->ClearBindings();
}

void CommonHostInterface::AddControllerRumble(u32 controller_index, u32 num_motors, ControllerRumbleCallback callback)
{
  ControllerRumbleState rumble;
  rumble.controller_index = 0;
  rumble.num_motors = std::min<u32>(num_motors, ControllerRumbleState::MAX_MOTORS);
  rumble.last_strength.fill(0.0f);
  rumble.update_callback = std::move(callback);
  m_controller_vibration_motors.push_back(std::move(rumble));
}

void CommonHostInterface::UpdateControllerRumble()
{
  for (ControllerRumbleState& rumble : m_controller_vibration_motors)
  {
    Controller* controller = System::GetController(rumble.controller_index);
    if (!controller)
      continue;

    bool changed = false;
    for (u32 i = 0; i < rumble.num_motors; i++)
    {
      const float strength = controller->GetVibrationMotorStrength(i);
      changed |= (strength != rumble.last_strength[i]);
      rumble.last_strength[i] = strength;
    }

    if (changed)
      rumble.update_callback(rumble.last_strength.data(), rumble.num_motors);
  }
}

void CommonHostInterface::StopControllerRumble()
{
  for (ControllerRumbleState& rumble : m_controller_vibration_motors)
  {
    bool changed = false;
    for (u32 i = 0; i < rumble.num_motors; i++)
    {
      changed |= (rumble.last_strength[i] != 0.0f);
      rumble.last_strength[i] = 0.0f;
    }

    if (changed)
      rumble.update_callback(rumble.last_strength.data(), rumble.num_motors);
  }
}

static bool SplitBinding(const std::string& binding, std::string_view* device, std::string_view* sub_binding)
{
  const std::string::size_type slash_pos = binding.find('/');
  if (slash_pos == std::string::npos)
  {
    Log_WarningPrintf("Malformed binding: '%s'", binding.c_str());
    return false;
  }

  *device = std::string_view(binding).substr(0, slash_pos);
  *sub_binding = std::string_view(binding).substr(slash_pos + 1);
  return true;
}

void CommonHostInterface::UpdateControllerInputMap(SettingsInterface& si)
{
  StopControllerRumble();
  m_controller_vibration_motors.clear();

  for (u32 controller_index = 0; controller_index < 2; controller_index++)
  {
    const ControllerType ctype = g_settings.controller_types[controller_index];
    if (ctype == ControllerType::None)
      continue;

    const auto category = TinyString::FromFormat("Controller%u", controller_index + 1);
    const auto button_names = Controller::GetButtonNames(ctype);
    for (const auto& it : button_names)
    {
      const std::string& button_name = it.first;
      const s32 button_code = it.second;

      const std::vector<std::string> bindings =
        si.GetStringList(category, TinyString::FromFormat("Button%s", button_name.c_str()));
      for (const std::string& binding : bindings)
      {
        std::string_view device, button;
        if (!SplitBinding(binding, &device, &button))
          continue;

        AddButtonToInputMap(binding, device, button, [controller_index, button_code](bool pressed) {
          if (System::IsShutdown())
            return;

          Controller* controller = System::GetController(controller_index);
          if (controller)
            controller->SetButtonState(button_code, pressed);
        });
      }
    }

    const auto axis_names = Controller::GetAxisNames(ctype);
    for (const auto& it : axis_names)
    {
      const std::string& axis_name = std::get<std::string>(it);
      const s32 axis_code = std::get<s32>(it);
      const auto axis_type = std::get<Controller::AxisType>(it);

      const std::vector<std::string> bindings =
        si.GetStringList(category, TinyString::FromFormat("Axis%s", axis_name.c_str()));
      for (const std::string& binding : bindings)
      {
        std::string_view device, axis;
        if (!SplitBinding(binding, &device, &axis))
          continue;

        AddAxisToInputMap(binding, device, axis, axis_type, [controller_index, axis_code](float value) {
          if (System::IsShutdown())
            return;

          Controller* controller = System::GetController(controller_index);
          if (controller)
            controller->SetAxisState(axis_code, value);
        });
      }
    }

    const u32 num_motors = Controller::GetVibrationMotorCount(ctype);
    if (num_motors > 0)
    {
      const std::vector<std::string> bindings = si.GetStringList(category, TinyString::FromFormat("Rumble"));
      for (const std::string& binding : bindings)
        AddRumbleToInputMap(binding, controller_index, num_motors);
    }

    if (m_controller_interface)
    {
      const float deadzone_size = si.GetFloatValue(category, "Deadzone", 0.25f);
      m_controller_interface->SetControllerDeadzone(controller_index, deadzone_size);
    }
  }
}

void CommonHostInterface::UpdateHotkeyInputMap(SettingsInterface& si)
{
  for (const HotkeyInfo& hi : m_hotkeys)
  {
    const std::vector<std::string> bindings = si.GetStringList("Hotkeys", hi.name);
    for (const std::string& binding : bindings)
    {
      std::string_view device, button;
      if (!SplitBinding(binding, &device, &button))
        continue;

      AddButtonToInputMap(binding, device, button, hi.handler);
    }
  }
}

bool CommonHostInterface::AddButtonToInputMap(const std::string& binding, const std::string_view& device,
                                              const std::string_view& button, InputButtonHandler handler)
{
  if (device == "Keyboard")
  {
    std::optional<int> key_id = GetHostKeyCode(button);
    if (!key_id.has_value())
    {
      Log_WarningPrintf("Unknown keyboard key in binding '%s'", binding.c_str());
      return false;
    }

    m_keyboard_input_handlers.emplace(key_id.value(), std::move(handler));
    return true;
  }

  if (device == "Mouse")
  {
    if (StringUtil::StartsWith(button, "Button"))
    {
      const std::optional<s32> button_index = StringUtil::FromChars<s32>(button.substr(6));
      if (!button_index.has_value())
      {
        Log_WarningPrintf("Invalid button in mouse binding '%s'", binding.c_str());
        return false;
      }

      m_mouse_input_handlers.emplace(static_cast<HostMouseButton>(button_index.value()), std::move(handler));
      return true;
    }

    Log_WarningPrintf("Malformed mouse binding '%s'", binding.c_str());
    return false;
  }

  if (StringUtil::StartsWith(device, "Controller"))
  {
    if (!m_controller_interface)
    {
      Log_ErrorPrintf("No controller interface set, cannot bind '%s'", binding.c_str());
      return false;
    }

    const std::optional<int> controller_index = StringUtil::FromChars<int>(device.substr(10));
    if (!controller_index || *controller_index < 0)
    {
      Log_WarningPrintf("Invalid controller index in button binding '%s'", binding.c_str());
      return false;
    }

    if (StringUtil::StartsWith(button, "Button"))
    {
      const std::optional<int> button_index = StringUtil::FromChars<int>(button.substr(6));
      if (!button_index ||
          !m_controller_interface->BindControllerButton(*controller_index, *button_index, std::move(handler)))
      {
        Log_WarningPrintf("Failed to bind controller button '%s' to button", binding.c_str());
        return false;
      }

      return true;
    }
    else if (StringUtil::StartsWith(button, "+Axis") || StringUtil::StartsWith(button, "-Axis"))
    {
      const std::optional<int> axis_index = StringUtil::FromChars<int>(button.substr(5));
      const bool positive = (button[0] == '+');
      if (!axis_index || !m_controller_interface->BindControllerAxisToButton(*controller_index, *axis_index, positive,
                                                                             std::move(handler)))
      {
        Log_WarningPrintf("Failed to bind controller axis '%s' to button", binding.c_str());
        return false;
      }

      return true;
    }
    else if (StringUtil::StartsWith(button, "Hat"))
    {
      const std::optional<int> hat_index = StringUtil::FromChars<int>(button.substr(3));
      const std::optional<std::string_view> hat_direction = [](const auto& button) {
        std::optional<std::string_view> result;

        const size_t pos = button.find(' ');
        if (pos != button.npos)
        {
          result = button.substr(pos + 1);
        }
        return result;
      }(button);

      if (!hat_index || !hat_direction ||
          !m_controller_interface->BindControllerHatToButton(*controller_index, *hat_index, *hat_direction,
                                                             std::move(handler)))
      {
        Log_WarningPrintf("Failed to bind controller hat '%s' to button", binding.c_str());
        return false;
      }

      return true;
    }

    Log_WarningPrintf("Malformed controller binding '%s' in button", binding.c_str());
    return false;
  }

  Log_WarningPrintf("Unknown input device in button binding '%s'", binding.c_str());
  return false;
}

bool CommonHostInterface::AddAxisToInputMap(const std::string& binding, const std::string_view& device,
                                            const std::string_view& axis, Controller::AxisType axis_type,
                                            InputAxisHandler handler)
{
  if (axis_type == Controller::AxisType::Half)
  {
    if (device == "Keyboard")
    {
      std::optional<int> key_id = GetHostKeyCode(axis);
      if (!key_id.has_value())
      {
        Log_WarningPrintf("Unknown keyboard key in binding '%s'", binding.c_str());
        return false;
      }

      m_keyboard_input_handlers.emplace(key_id.value(),
                                        [cb = std::move(handler)](bool pressed) { cb(pressed ? 1.0f : -1.0f); });
      return true;
    }

    if (device == "Mouse")
    {
      if (StringUtil::StartsWith(axis, "Button"))
      {
        const std::optional<s32> button_index = StringUtil::FromChars<s32>(axis.substr(6));
        if (!button_index.has_value())
        {
          Log_WarningPrintf("Invalid button in mouse binding '%s'", binding.c_str());
          return false;
        }

        m_mouse_input_handlers.emplace(static_cast<HostMouseButton>(button_index.value()),
                                       [cb = std::move(handler)](bool pressed) { cb(pressed ? 1.0f : -1.0f); });
        return true;
      }

      Log_WarningPrintf("Malformed mouse binding '%s'", binding.c_str());
      return false;
    }
  }

  if (StringUtil::StartsWith(device, "Controller"))
  {
    if (!m_controller_interface)
    {
      Log_ErrorPrintf("No controller interface set, cannot bind '%s'", binding.c_str());
      return false;
    }

    const std::optional<int> controller_index = StringUtil::FromChars<int>(device.substr(10));
    if (!controller_index || *controller_index < 0)
    {
      Log_WarningPrintf("Invalid controller index in axis binding '%s'", binding.c_str());
      return false;
    }

    if (StringUtil::StartsWith(axis, "Axis") || StringUtil::StartsWith(axis, "+Axis") ||
        StringUtil::StartsWith(axis, "-Axis"))
    {
      const std::optional<int> axis_index =
        StringUtil::FromChars<int>(axis.substr(axis[0] == '+' || axis[0] == '-' ? 5 : 4));
      if (axis_index)
      {
        ControllerInterface::AxisSide axis_side = ControllerInterface::AxisSide::Full;
        if (axis[0] == '+')
          axis_side = ControllerInterface::AxisSide::Positive;
        else if (axis[0] == '-')
          axis_side = ControllerInterface::AxisSide::Negative;

        const bool inverted = StringUtil::EndsWith(axis, "-");
        if (!inverted)
        {
          if (m_controller_interface->BindControllerAxis(*controller_index, *axis_index, axis_side, std::move(handler)))
          {
            return true;
          }
        }
        else
        {
          if (m_controller_interface->BindControllerAxis(*controller_index, *axis_index, axis_side,
                                                         [cb = std::move(handler)](float value) { cb(-value); }))
          {
            return true;
          }
        }
      }
      Log_WarningPrintf("Failed to bind controller axis '%s' to axis", binding.c_str());
      return false;
    }
    else if (StringUtil::StartsWith(axis, "Button") && axis_type == Controller::AxisType::Half)
    {
      const std::optional<int> button_index = StringUtil::FromChars<int>(axis.substr(6));
      if (!button_index ||
          !m_controller_interface->BindControllerButtonToAxis(*controller_index, *button_index, std::move(handler)))
      {
        Log_WarningPrintf("Failed to bind controller button '%s' to axis", binding.c_str());
        return false;
      }

      return true;
    }

    Log_WarningPrintf("Malformed controller binding '%s' in button", binding.c_str());
    return false;
  }

  Log_WarningPrintf("Unknown input device in axis binding '%s'", binding.c_str());
  return false;
}

bool CommonHostInterface::AddRumbleToInputMap(const std::string& binding, u32 controller_index, u32 num_motors)
{
  if (StringUtil::StartsWith(binding, "Controller"))
  {
    if (!m_controller_interface)
    {
      Log_ErrorPrintf("No controller interface set, cannot bind '%s'", binding.c_str());
      return false;
    }

    const std::optional<int> host_controller_index = StringUtil::FromChars<int>(binding.substr(10));
    if (!host_controller_index || *host_controller_index < 0)
    {
      Log_WarningPrintf("Invalid controller index in rumble binding '%s'", binding.c_str());
      return false;
    }

    AddControllerRumble(controller_index, num_motors,
                        std::bind(&ControllerInterface::SetControllerRumbleStrength, m_controller_interface.get(),
                                  host_controller_index.value(), std::placeholders::_1, std::placeholders::_2));

    return true;
  }

  Log_WarningPrintf("Unknown input device in rumble binding '%s'", binding.c_str());
  return false;
}

void CommonHostInterface::RegisterGeneralHotkeys()
{
  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "General")), StaticString("FastForward"),
                 TRANSLATABLE("Hotkeys", "Fast Forward"), [this](bool pressed) {
                   m_fast_forward_enabled = pressed;
                   UpdateSpeedLimiterState();
                 });

  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "General")), StaticString("ToggleFastForward"),
                 StaticString(TRANSLATABLE("Hotkeys", "Toggle Fast Forward")), [this](bool pressed) {
                   if (pressed)
                   {
                     m_fast_forward_enabled = !m_fast_forward_enabled;
                     UpdateSpeedLimiterState();
                     AddOSDMessage(m_fast_forward_enabled ?
                                     TranslateStdString("OSDMessage", "Fast forwarding...") :
                                     TranslateStdString("OSDMessage", "Stopped fast forwarding."),
                                   2.0f);
                   }
                 });

  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "General")), StaticString("ToggleFullscreen"),
                 StaticString(TRANSLATABLE("Hotkeys", "Toggle Fullscreen")), [this](bool pressed) {
                   if (pressed)
                     SetFullscreen(!IsFullscreen());
                 });

  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "General")), StaticString("TogglePause"),
                 StaticString(TRANSLATABLE("Hotkeys", "Toggle Pause")), [this](bool pressed) {
                   if (pressed && System::IsValid())
                     PauseSystem(!System::IsPaused());
                 });

  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "General")), StaticString("ToggleCheats"),
                 StaticString(TRANSLATABLE("Hotkeys", "Toggle Cheats")), [this](bool pressed) {
                   if (pressed)
                     DoToggleCheats();
                 });

  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "General")), StaticString("PowerOff"),
                 StaticString(TRANSLATABLE("Hotkeys", "Power Off System")), [this](bool pressed) {
                   if (pressed && System::IsValid())
                   {
                     if (g_settings.confim_power_off && !m_batch_mode)
                     {
                       SmallString confirmation_message(
                         TranslateString("CommonHostInterface", "Are you sure you want to stop emulation?"));
                       if (g_settings.save_state_on_exit)
                       {
                         confirmation_message.AppendString("\n\n");
                         confirmation_message.AppendString(
                           TranslateString("CommonHostInterface", "The current state will be saved."));
                       }

                       if (!ConfirmMessage(confirmation_message))
                       {
                         System::ResetPerformanceCounters();
                         return;
                       }
                     }

                     PowerOffSystem();
                   }
                 });

  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "General")), StaticString("Reset"),
                 StaticString(TRANSLATABLE("Hotkeys", "Reset System")), [this](bool pressed) {
                   if (pressed && System::IsValid())
                     ResetSystem();
                 });

  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "General")), StaticString("Screenshot"),
                 StaticString(TRANSLATABLE("Hotkeys", "Save Screenshot")), [this](bool pressed) {
                   if (pressed && System::IsValid())
                     SaveScreenshot();
                 });

  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "General")), StaticString("FrameStep"),
                 StaticString(TRANSLATABLE("Hotkeys", "Frame Step")), [this](bool pressed) {
                   if (pressed)
                     DoFrameStep();
                 });
}

void CommonHostInterface::RegisterGraphicsHotkeys()
{
  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Graphics")), StaticString("ToggleSoftwareRendering"),
                 StaticString(TRANSLATABLE("Hotkeys", "Toggle Software Rendering")), [this](bool pressed) {
                   if (pressed)
                     ToggleSoftwareRendering();
                 });

  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Graphics")), StaticString("TogglePGXP"),
                 StaticString(TRANSLATABLE("Hotkeys", "Toggle PGXP")), [this](bool pressed) {
                   if (pressed)
                   {
                     g_settings.gpu_pgxp_enable = !g_settings.gpu_pgxp_enable;
                     g_gpu->UpdateSettings();
                     AddOSDMessage(g_settings.gpu_pgxp_enable ?
                                     TranslateStdString("OSDMessage", "PGXP is now enabled.") :
                                     TranslateStdString("OSDMessage", "PGXP is now disabled"),
                                   5.0f);

                     if (g_settings.gpu_pgxp_enable)
                       PGXP::Initialize();
                     else
                       PGXP::Shutdown();

                     // we need to recompile all blocks if pgxp is toggled on/off
                     if (g_settings.IsUsingCodeCache())
                       CPU::CodeCache::Flush();
                   }
                 });

  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Graphics")), StaticString("IncreaseResolutionScale"),
                 StaticString(TRANSLATABLE("Hotkeys", "Increase Resolution Scale")), [this](bool pressed) {
                   if (pressed)
                     ModifyResolutionScale(1);
                 });

  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Graphics")), StaticString("DecreaseResolutionScale"),
                 StaticString(TRANSLATABLE("Hotkeys", "Decrease Resolution Scale")), [this](bool pressed) {
                   if (pressed)
                     ModifyResolutionScale(-1);
                 });

  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Graphics")), StaticString("TogglePostProcessing"),
                 StaticString(TRANSLATABLE("Hotkeys", "Toggle Post-Processing")), [this](bool pressed) {
                   if (pressed)
                     TogglePostProcessing();
                 });

  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Graphics")), StaticString("ReloadPostProcessingShaders"),
                 StaticString(TRANSLATABLE("Hotkeys", "Reload Post Processing Shaders")), [this](bool pressed) {
                   if (pressed)
                     ReloadPostProcessingShaders();
                 });
}

void CommonHostInterface::RegisterSaveStateHotkeys()
{
  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Save States")), StaticString("LoadSelectedSaveState"),
                 StaticString(TRANSLATABLE("Hotkeys", "Load From Selected Slot")), [this](bool pressed) {
                   if (pressed)
                     m_save_state_selector_ui->LoadCurrentSlot();
                 });
  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Save States")), StaticString("SaveSelectedSaveState"),
                 StaticString(TRANSLATABLE("Hotkeys", "Save To Selected Slot")), [this](bool pressed) {
                   if (pressed)
                     m_save_state_selector_ui->SaveCurrentSlot();
                 });
  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Save States")), StaticString("SelectPreviousSaveStateSlot"),
                 StaticString(TRANSLATABLE("Hotkeys", "Select Previous Save Slot")), [this](bool pressed) {
                   if (pressed)
                     m_save_state_selector_ui->SelectPreviousSlot();
                 });
  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Save States")), StaticString("SelectNextSaveStateSlot"),
                 StaticString(TRANSLATABLE("Hotkeys", "Select Next Save Slot")), [this](bool pressed) {
                   if (pressed)
                     m_save_state_selector_ui->SelectNextSlot();
                 });

  for (u32 slot = 1; slot <= PER_GAME_SAVE_STATE_SLOTS; slot++)
  {
    RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Save States")),
                   TinyString::FromFormat("LoadGameState%u", slot), TinyString::FromFormat("Load Game State %u", slot),
                   [this, slot](bool pressed) {
                     if (pressed)
                       LoadState(false, slot);
                   });
    RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Save States")),
                   TinyString::FromFormat("SaveGameState%u", slot), TinyString::FromFormat("Save Game State %u", slot),
                   [this, slot](bool pressed) {
                     if (pressed)
                       SaveState(false, slot);
                   });
  }

  for (u32 slot = 1; slot <= GLOBAL_SAVE_STATE_SLOTS; slot++)
  {
    RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Save States")),
                   TinyString::FromFormat("LoadGlobalState%u", slot),
                   TinyString::FromFormat("Load Global State %u", slot), [this, slot](bool pressed) {
                     if (pressed)
                       LoadState(true, slot);
                   });
    RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Save States")),
                   TinyString::FromFormat("SaveGlobalState%u", slot),
                   TinyString::FromFormat("Save Global State %u", slot), [this, slot](bool pressed) {
                     if (pressed)
                       SaveState(true, slot);
                   });
  }

  // Dummy strings for translation because we construct them in a loop.
  (void)TRANSLATABLE("Hotkeys", "Load Game State 1");
  (void)TRANSLATABLE("Hotkeys", "Load Game State 2");
  (void)TRANSLATABLE("Hotkeys", "Load Game State 3");
  (void)TRANSLATABLE("Hotkeys", "Load Game State 4");
  (void)TRANSLATABLE("Hotkeys", "Load Game State 5");
  (void)TRANSLATABLE("Hotkeys", "Load Game State 6");
  (void)TRANSLATABLE("Hotkeys", "Load Game State 7");
  (void)TRANSLATABLE("Hotkeys", "Load Game State 8");
  (void)TRANSLATABLE("Hotkeys", "Load Game State 9");
  (void)TRANSLATABLE("Hotkeys", "Load Game State 10");
  (void)TRANSLATABLE("Hotkeys", "Save Game State 1");
  (void)TRANSLATABLE("Hotkeys", "Save Game State 2");
  (void)TRANSLATABLE("Hotkeys", "Save Game State 3");
  (void)TRANSLATABLE("Hotkeys", "Save Game State 4");
  (void)TRANSLATABLE("Hotkeys", "Save Game State 5");
  (void)TRANSLATABLE("Hotkeys", "Save Game State 6");
  (void)TRANSLATABLE("Hotkeys", "Save Game State 7");
  (void)TRANSLATABLE("Hotkeys", "Save Game State 8");
  (void)TRANSLATABLE("Hotkeys", "Save Game State 9");
  (void)TRANSLATABLE("Hotkeys", "Save Game State 10");
  (void)TRANSLATABLE("Hotkeys", "Load Global State 1");
  (void)TRANSLATABLE("Hotkeys", "Load Global State 2");
  (void)TRANSLATABLE("Hotkeys", "Load Global State 3");
  (void)TRANSLATABLE("Hotkeys", "Load Global State 4");
  (void)TRANSLATABLE("Hotkeys", "Load Global State 5");
  (void)TRANSLATABLE("Hotkeys", "Load Global State 6");
  (void)TRANSLATABLE("Hotkeys", "Load Global State 7");
  (void)TRANSLATABLE("Hotkeys", "Load Global State 8");
  (void)TRANSLATABLE("Hotkeys", "Load Global State 9");
  (void)TRANSLATABLE("Hotkeys", "Load Global State 10");
  (void)TRANSLATABLE("Hotkeys", "Save Global State 1");
  (void)TRANSLATABLE("Hotkeys", "Save Global State 2");
  (void)TRANSLATABLE("Hotkeys", "Save Global State 3");
  (void)TRANSLATABLE("Hotkeys", "Save Global State 4");
  (void)TRANSLATABLE("Hotkeys", "Save Global State 5");
  (void)TRANSLATABLE("Hotkeys", "Save Global State 6");
  (void)TRANSLATABLE("Hotkeys", "Save Global State 7");
  (void)TRANSLATABLE("Hotkeys", "Save Global State 8");
  (void)TRANSLATABLE("Hotkeys", "Save Global State 9");
  (void)TRANSLATABLE("Hotkeys", "Save Global State 10");
}

void CommonHostInterface::RegisterAudioHotkeys()
{
  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Audio")), StaticString("AudioMute"),
                 StaticString(TRANSLATABLE("Hotkeys", "Toggle Mute")), [this](bool pressed) {
                   if (pressed && System::IsValid())
                   {
                     g_settings.audio_output_muted = !g_settings.audio_output_muted;
                     const s32 volume = GetAudioOutputVolume();
                     m_audio_stream->SetOutputVolume(volume);
                     if (g_settings.audio_output_muted)
                       AddOSDMessage(TranslateStdString("OSDMessage", "Volume: Muted"), 2.0f);
                     else
                       AddFormattedOSDMessage(2.0f, TranslateString("OSDMessage", "Volume: %d%%"), volume);
                   }
                 });
  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Audio")), StaticString("AudioCDAudioMute"),
                 StaticString(TRANSLATABLE("Hotkeys", "Toggle CD Audio Mute")), [this](bool pressed) {
                   if (pressed && System::IsValid())
                   {
                     g_settings.cdrom_mute_cd_audio = !g_settings.cdrom_mute_cd_audio;
                     AddOSDMessage(g_settings.cdrom_mute_cd_audio ?
                                     TranslateStdString("OSDMessage", "CD Audio Muted.") :
                                     TranslateStdString("OSDMessage", "CD Audio Unmuted."),
                                   2.0f);
                   }
                 });
  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Audio")), StaticString("AudioVolumeUp"),
                 StaticString(TRANSLATABLE("Hotkeys", "Volume Up")), [this](bool pressed) {
                   if (pressed && System::IsValid())
                   {
                     g_settings.audio_output_muted = false;

                     const s32 volume = std::min<s32>(GetAudioOutputVolume() + 10, 100);
                     g_settings.audio_output_volume = volume;
                     g_settings.audio_fast_forward_volume = volume;
                     m_audio_stream->SetOutputVolume(volume);
                     AddFormattedOSDMessage(2.0f, TranslateString("OSDMessage", "Volume: %d%%"), volume);
                   }
                 });
  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "Audio")), StaticString("AudioVolumeDown"),
                 StaticString(TRANSLATABLE("Hotkeys", "Volume Down")), [this](bool pressed) {
                   if (pressed && System::IsValid())
                   {
                     g_settings.audio_output_muted = false;

                     const s32 volume = std::max<s32>(GetAudioOutputVolume() - 10, 0);
                     g_settings.audio_output_volume = volume;
                     g_settings.audio_fast_forward_volume = volume;
                     m_audio_stream->SetOutputVolume(volume);
                     AddFormattedOSDMessage(2.0f, TranslateString("OSDMessage", "Volume: %d%%"), volume);
                   }
                 });
}

std::string CommonHostInterface::GetSavePathForInputProfile(const char* name) const
{
  return GetUserDirectoryRelativePath("inputprofiles/%s.ini", name);
}

CommonHostInterface::InputProfileList CommonHostInterface::GetInputProfileList() const
{
  InputProfileList profiles;

  const std::string user_dir(GetUserDirectoryRelativePath("inputprofiles"));
  const std::string program_dir(GetProgramDirectoryRelativePath("inputprofiles"));

  FindInputProfiles(user_dir, &profiles);
  if (user_dir != program_dir)
    FindInputProfiles(program_dir, &profiles);

  return profiles;
}

void CommonHostInterface::FindInputProfiles(const std::string& base_path, InputProfileList* out_list) const
{
  FileSystem::FindResultsArray results;
  FileSystem::FindFiles(base_path.c_str(), "*.ini", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_RELATIVE_PATHS, &results);

  out_list->reserve(out_list->size() + results.size());
  for (auto& it : results)
  {
    if (it.FileName.size() < 4)
      continue;

    std::string name(it.FileName.substr(0, it.FileName.length() - 4));

    // skip duplicates, we prefer the user directory
    if (std::any_of(out_list->begin(), out_list->end(),
                    [&name](const InputProfileEntry& e) { return (e.name == name); }))
    {
      continue;
    }

    std::string filename(
      StringUtil::StdStringFromFormat("%s" FS_OSPATH_SEPARATOR_STR "%s", base_path.c_str(), it.FileName.c_str()));
    out_list->push_back(InputProfileEntry{std::move(name), std::move(filename)});
  }
}

std::string CommonHostInterface::GetInputProfilePath(const char* name) const
{
  std::string path = GetUserDirectoryRelativePath("inputprofiles" FS_OSPATH_SEPARATOR_STR "%s.ini", name);
  if (FileSystem::FileExists(path.c_str()))
    return path;

  path = GetProgramDirectoryRelativePath("inputprofiles" FS_OSPATH_SEPARATOR_STR "%s.ini", name);
  if (FileSystem::FileExists(path.c_str()))
    return path;

  return {};
}

void CommonHostInterface::ClearAllControllerBindings(SettingsInterface& si)
{
  for (u32 controller_index = 1; controller_index <= NUM_CONTROLLER_AND_CARD_PORTS; controller_index++)
  {
    const ControllerType ctype = g_settings.controller_types[controller_index - 1];
    if (ctype == ControllerType::None)
      continue;

    const auto section_name = TinyString::FromFormat("Controller%u", controller_index);

    si.DeleteValue(section_name, "Type");

    for (const auto& button : Controller::GetButtonNames(ctype))
      si.DeleteValue(section_name, button.first.c_str());

    for (const auto& axis : Controller::GetAxisNames(ctype))
      si.DeleteValue(section_name, std::get<std::string>(axis).c_str());

    if (Controller::GetVibrationMotorCount(ctype) > 0)
      si.DeleteValue(section_name, "Rumble");
  }
}

void CommonHostInterface::ApplyInputProfile(const char* profile_path, SettingsInterface& si)
{
  // clear bindings for all controllers
  ClearAllControllerBindings(si);

  INISettingsInterface profile(profile_path);

  for (u32 controller_index = 1; controller_index <= NUM_CONTROLLER_AND_CARD_PORTS; controller_index++)
  {
    const auto section_name = TinyString::FromFormat("Controller%u", controller_index);
    const std::string ctype_str = profile.GetStringValue(section_name, "Type");
    if (ctype_str.empty())
      continue;

    std::optional<ControllerType> ctype = Settings::ParseControllerTypeName(ctype_str.c_str());
    if (!ctype)
    {
      Log_ErrorPrintf("Invalid controller type in profile: '%s'", ctype_str.c_str());
      return;
    }

    g_settings.controller_types[controller_index - 1] = *ctype;
    HostInterface::OnControllerTypeChanged(controller_index - 1);

    si.SetStringValue(section_name, "Type", Settings::GetControllerTypeName(*ctype));

    for (const auto& button : Controller::GetButtonNames(*ctype))
    {
      const auto key_name = TinyString::FromFormat("Button%s", button.first.c_str());
      si.DeleteValue(section_name, key_name);
      const std::vector<std::string> bindings = profile.GetStringList(section_name, key_name);
      for (const std::string& binding : bindings)
        si.AddToStringList(section_name, key_name, binding.c_str());
    }

    for (const auto& axis : Controller::GetAxisNames(*ctype))
    {
      const auto key_name = TinyString::FromFormat("Axis%s", std::get<std::string>(axis).c_str());
      si.DeleteValue(section_name, std::get<std::string>(axis).c_str());
      const std::vector<std::string> bindings = profile.GetStringList(section_name, key_name);
      for (const std::string& binding : bindings)
        si.AddToStringList(section_name, key_name, binding.c_str());
    }

    si.DeleteValue(section_name, "Rumble");
    const std::string rumble_value = profile.GetStringValue(section_name, "Rumble");
    if (!rumble_value.empty())
      si.SetStringValue(section_name, "Rumble", rumble_value.c_str());

    Controller::SettingList settings = Controller::GetSettings(*ctype);
    for (const SettingInfo& ssi : settings)
    {
      const std::string value = profile.GetStringValue(section_name, ssi.key, "");
      if (!value.empty())
        si.SetStringValue(section_name, ssi.key, value.c_str());
    }
  }

  if (System::IsValid())
    System::UpdateControllers();

  UpdateInputMap(si);

  ReportFormattedMessage(TranslateString("OSDMessage", "Loaded input profile from '%s'"), profile_path);
}

bool CommonHostInterface::SaveInputProfile(const char* profile_path, SettingsInterface& si)
{
  if (FileSystem::FileExists(profile_path))
    Log_WarningPrintf("Existing input profile at '%s' will be overwritten", profile_path);
  else
    Log_WarningPrintf("Input profile at '%s' does not exist, new input profile will be created", profile_path);

  INISettingsInterface profile(profile_path);
  profile.Clear();

  for (u32 controller_index = 1; controller_index <= NUM_CONTROLLER_AND_CARD_PORTS; controller_index++)
  {
    const ControllerType ctype = g_settings.controller_types[controller_index - 1];
    if (ctype == ControllerType::None)
      continue;

    const auto section_name = TinyString::FromFormat("Controller%u", controller_index);

    profile.SetStringValue(section_name, "Type", Settings::GetControllerTypeName(ctype));

    for (const auto& button : Controller::GetButtonNames(ctype))
    {
      const auto key_name = TinyString::FromFormat("Button%s", button.first.c_str());
      const std::vector<std::string> bindings = si.GetStringList(section_name, key_name);
      for (const std::string& binding : bindings)
        profile.AddToStringList(section_name, key_name, binding.c_str());
    }

    for (const auto& axis : Controller::GetAxisNames(ctype))
    {
      const auto key_name = TinyString::FromFormat("Axis%s", std::get<std::string>(axis).c_str());
      const std::vector<std::string> bindings = si.GetStringList(section_name, key_name);
      for (const std::string& binding : bindings)
        profile.AddToStringList(section_name, key_name, binding.c_str());
    }

    const std::string rumble_value = si.GetStringValue(section_name, "Rumble");
    if (!rumble_value.empty())
      profile.SetStringValue(section_name, "Rumble", rumble_value.c_str());

    Controller::SettingList settings = Controller::GetSettings(ctype);
    for (const SettingInfo& ssi : settings)
    {
      const std::string value = si.GetStringValue(section_name, ssi.key, "");
      if (!value.empty())
        profile.SetStringValue(section_name, ssi.key, value.c_str());
    }
  }

  if (!profile.Save())
  {
    Log_ErrorPrintf("Failed to save input profile to '%s'", profile_path);
    return false;
  }

  Log_InfoPrintf("Input profile saved to '%s'", profile_path);
  return true;
}

std::string CommonHostInterface::GetSettingsFileName() const
{
  return GetUserDirectoryRelativePath("settings.ini");
}

std::string CommonHostInterface::GetGameSaveStateFileName(const char* game_code, s32 slot) const
{
  if (slot < 0)
    return GetUserDirectoryRelativePath("savestates" FS_OSPATH_SEPARATOR_STR "%s_resume.sav", game_code);
  else
    return GetUserDirectoryRelativePath("savestates" FS_OSPATH_SEPARATOR_STR "%s_%d.sav", game_code, slot);
}

std::string CommonHostInterface::GetGlobalSaveStateFileName(s32 slot) const
{
  if (slot < 0)
    return GetUserDirectoryRelativePath("savestates" FS_OSPATH_SEPARATOR_STR "resume.sav");
  else
    return GetUserDirectoryRelativePath("savestates" FS_OSPATH_SEPARATOR_STR "savestate_%d.sav", slot);
}

std::vector<CommonHostInterface::SaveStateInfo> CommonHostInterface::GetAvailableSaveStates(const char* game_code) const
{
  std::vector<SaveStateInfo> si;
  std::string path;

  auto add_path = [&si](std::string path, s32 slot, bool global) {
    FILESYSTEM_STAT_DATA sd;
    if (!FileSystem::StatFile(path.c_str(), &sd))
      return;

    si.push_back(SaveStateInfo{std::move(path), sd.ModificationTime.AsUnixTimestamp(), static_cast<s32>(slot), global});
  };

  if (game_code && std::strlen(game_code) > 0)
  {
    add_path(GetGameSaveStateFileName(game_code, -1), -1, false);
    for (s32 i = 1; i <= PER_GAME_SAVE_STATE_SLOTS; i++)
      add_path(GetGameSaveStateFileName(game_code, i), i, false);
  }

  for (s32 i = 1; i <= GLOBAL_SAVE_STATE_SLOTS; i++)
    add_path(GetGlobalSaveStateFileName(i), i, true);

  return si;
}

std::optional<CommonHostInterface::SaveStateInfo> CommonHostInterface::GetSaveStateInfo(const char* game_code, s32 slot)
{
  const bool global = (!game_code || game_code[0] == 0);
  std::string path = global ? GetGlobalSaveStateFileName(slot) : GetGameSaveStateFileName(game_code, slot);

  FILESYSTEM_STAT_DATA sd;
  if (!FileSystem::StatFile(path.c_str(), &sd))
    return std::nullopt;

  return SaveStateInfo{std::move(path), sd.ModificationTime.AsUnixTimestamp(), slot, global};
}

std::optional<CommonHostInterface::ExtendedSaveStateInfo>
CommonHostInterface::GetExtendedSaveStateInfo(const char* game_code, s32 slot)
{
  const bool global = (!game_code || game_code[0] == 0);
  std::string path = global ? GetGlobalSaveStateFileName(slot) : GetGameSaveStateFileName(game_code, slot);

  FILESYSTEM_STAT_DATA sd;
  if (!FileSystem::StatFile(path.c_str(), &sd))
    return std::nullopt;

  std::unique_ptr<ByteStream> stream =
    FileSystem::OpenFile(path.c_str(), BYTESTREAM_OPEN_READ | BYTESTREAM_OPEN_SEEKABLE);
  if (!stream)
    return std::nullopt;

  SAVE_STATE_HEADER header;
  if (!stream->Read(&header, sizeof(header)) || header.magic != SAVE_STATE_MAGIC)
    return std::nullopt;

  ExtendedSaveStateInfo ssi;
  ssi.path = std::move(path);
  ssi.timestamp = sd.ModificationTime.AsUnixTimestamp();
  ssi.slot = slot;
  ssi.global = global;

  if (header.version < SAVE_STATE_MINIMUM_VERSION || header.version > SAVE_STATE_VERSION)
  {
    ssi.title = StringUtil::StdStringFromFormat(
      TranslateString("CommonHostInterface", "Invalid version %u (%s version %u)"), header.version,
      header.version > SAVE_STATE_VERSION ? "maximum" : "minimum",
      header.version > SAVE_STATE_VERSION ? SAVE_STATE_VERSION : SAVE_STATE_MINIMUM_VERSION);
    return ssi;
  }

  header.title[sizeof(header.title) - 1] = 0;
  ssi.title = header.title;
  header.game_code[sizeof(header.game_code) - 1] = 0;
  ssi.game_code = header.game_code;

  if (header.screenshot_width > 0 && header.screenshot_height > 0 && header.screenshot_size > 0 &&
      (static_cast<u64>(header.offset_to_screenshot) + static_cast<u64>(header.screenshot_size)) <= stream->GetSize())
  {
    stream->SeekAbsolute(header.offset_to_screenshot);
    ssi.screenshot_data.resize((header.screenshot_size + 3u) / 4u);
    if (stream->Read2(ssi.screenshot_data.data(), header.screenshot_size))
    {
      ssi.screenshot_width = header.screenshot_width;
      ssi.screenshot_height = header.screenshot_height;
    }
    else
    {
      decltype(ssi.screenshot_data)().swap(ssi.screenshot_data);
    }
  }

  return ssi;
}

void CommonHostInterface::DeleteSaveStates(const char* game_code, bool resume)
{
  const std::vector<SaveStateInfo> states(GetAvailableSaveStates(game_code));
  for (const SaveStateInfo& si : states)
  {
    if (si.global || (!resume && si.slot < 0))
      continue;

    Log_InfoPrintf("Removing save state at '%s'", si.path.c_str());
    if (!FileSystem::DeleteFile(si.path.c_str()))
      Log_ErrorPrintf("Failed to delete save state file '%s'", si.path.c_str());
  }
}

std::string CommonHostInterface::GetMostRecentResumeSaveStatePath() const
{
  std::vector<FILESYSTEM_FIND_DATA> files;
  if (!FileSystem::FindFiles(GetUserDirectoryRelativePath("savestates").c_str(), "*resume.sav", FILESYSTEM_FIND_FILES,
                             &files) ||
      files.empty())
  {
    return {};
  }

  FILESYSTEM_FIND_DATA* most_recent = &files[0];
  for (FILESYSTEM_FIND_DATA& file : files)
  {
    if (file.ModificationTime > most_recent->ModificationTime)
      most_recent = &file;
  }

  return std::move(most_recent->FileName);
}

bool CommonHostInterface::CheckSettings(SettingsInterface& si)
{
  const int settings_version = si.GetIntValue("Main", "SettingsVersion", -1);
  if (settings_version == SETTINGS_VERSION)
    return true;

  Log_ErrorPrintf("Settings version %d does not match expected version %d, resetting", settings_version,
                  SETTINGS_VERSION);

  si.Clear();
  si.SetIntValue("Main", "SettingsVersion", SETTINGS_VERSION);
  SetDefaultSettings(si);
  return false;
}

void CommonHostInterface::SetDefaultSettings(SettingsInterface& si)
{
  HostInterface::SetDefaultSettings(si);

  si.SetStringValue("Controller1", "ButtonUp", "Keyboard/W");
  si.SetStringValue("Controller1", "ButtonDown", "Keyboard/S");
  si.SetStringValue("Controller1", "ButtonLeft", "Keyboard/A");
  si.SetStringValue("Controller1", "ButtonRight", "Keyboard/D");
  si.SetStringValue("Controller1", "ButtonSelect", "Keyboard/Backspace");
  si.SetStringValue("Controller1", "ButtonStart", "Keyboard/Return");
  si.SetStringValue("Controller1", "ButtonTriangle", "Keyboard/Keypad+8");
  si.SetStringValue("Controller1", "ButtonCross", "Keyboard/Keypad+2");
  si.SetStringValue("Controller1", "ButtonSquare", "Keyboard/Keypad+4");
  si.SetStringValue("Controller1", "ButtonCircle", "Keyboard/Keypad+6");
  si.SetStringValue("Controller1", "ButtonL1", "Keyboard/Q");
  si.SetStringValue("Controller1", "ButtonL2", "Keyboard/1");
  si.SetStringValue("Controller1", "ButtonR1", "Keyboard/E");
  si.SetStringValue("Controller1", "ButtonR2", "Keyboard/3");
  si.SetStringValue("Hotkeys", "FastForward", "Keyboard/Tab");
  si.SetStringValue("Hotkeys", "TogglePause", "Keyboard/Pause");
  si.SetStringValue("Hotkeys", "ToggleFullscreen", "Keyboard/Alt+Return");
  si.SetStringValue("Hotkeys", "PowerOff", "Keyboard/Escape");
  si.SetStringValue("Hotkeys", "LoadSelectedSaveState", "Keyboard/F1");
  si.SetStringValue("Hotkeys", "SaveSelectedSaveState", "Keyboard/F2");
  si.SetStringValue("Hotkeys", "SelectPreviousSaveStateSlot", "Keyboard/F3");
  si.SetStringValue("Hotkeys", "SelectNextSaveStateSlot", "Keyboard/F4");
  si.SetStringValue("Hotkeys", "Screenshot", "Keyboard/F10");
  si.SetStringValue("Hotkeys", "IncreaseResolutionScale", "Keyboard/PageUp");
  si.SetStringValue("Hotkeys", "DecreaseResolutionScale", "Keyboard/PageDown");
  si.SetStringValue("Hotkeys", "ToggleSoftwareRendering", "Keyboard/End");

  si.SetStringValue("Main", "ControllerBackend",
                    ControllerInterface::GetBackendName(ControllerInterface::GetDefaultBackend()));

#ifdef WITH_DISCORD_PRESENCE
  si.SetBoolValue("Main", "EnableDiscordPresence", false);
#endif
}

void CommonHostInterface::LoadSettings(SettingsInterface& si)
{
  HostInterface::LoadSettings(si);

#ifdef WITH_DISCORD_PRESENCE
  SetDiscordPresenceEnabled(si.GetBoolValue("Main", "EnableDiscordPresence", false));
#endif
}

void CommonHostInterface::SaveSettings(SettingsInterface& si)
{
  HostInterface::SaveSettings(si);
}

void CommonHostInterface::CheckForSettingsChanges(const Settings& old_settings)
{
  HostInterface::CheckForSettingsChanges(old_settings);

  UpdateControllerInterface();

  if (System::IsValid())
  {
    if (g_settings.audio_backend != old_settings.audio_backend ||
        g_settings.audio_buffer_size != old_settings.audio_buffer_size ||
        g_settings.video_sync_enabled != old_settings.video_sync_enabled ||
        g_settings.audio_sync_enabled != old_settings.audio_sync_enabled ||
        g_settings.increase_timer_resolution != old_settings.increase_timer_resolution ||
        g_settings.emulation_speed != old_settings.emulation_speed ||
        g_settings.fast_forward_speed != old_settings.fast_forward_speed ||
        g_settings.display_max_fps != old_settings.display_max_fps)
    {
      UpdateSpeedLimiterState();
    }

    if (g_settings.display_post_processing != old_settings.display_post_processing ||
        g_settings.display_post_process_chain != old_settings.display_post_process_chain)
    {
      if (g_settings.display_post_processing)
      {
        if (!m_display->SetPostProcessingChain(g_settings.display_post_process_chain))
          AddOSDMessage(TranslateStdString("OSDMessage", "Failed to load post processing shader chain."), 20.0f);
      }
      else
      {
        m_display->SetPostProcessingChain({});
      }
    }
  }

  if (g_settings.log_level != old_settings.log_level || g_settings.log_filter != old_settings.log_filter ||
      g_settings.log_to_console != old_settings.log_to_console ||
      g_settings.log_to_window != old_settings.log_to_window || g_settings.log_to_file != old_settings.log_to_file)
  {
    UpdateLogSettings(g_settings.log_level, g_settings.log_filter.empty() ? nullptr : g_settings.log_filter.c_str(),
                      g_settings.log_to_console, g_settings.log_to_debug, g_settings.log_to_window,
                      g_settings.log_to_file);
  }

  UpdateInputMap();
}

void CommonHostInterface::SetTimerResolutionIncreased(bool enabled)
{
  if (m_timer_resolution_increased == enabled)
    return;

  m_timer_resolution_increased = enabled;

#ifdef WIN32
  if (enabled)
    timeBeginPeriod(1);
  else
    timeEndPeriod(1);
#endif
}

void CommonHostInterface::DisplayLoadingScreen(const char* message, int progress_min /*= -1*/,
                                               int progress_max /*= -1*/, int progress_value /*= -1*/)
{
  const auto& io = ImGui::GetIO();
  const float scale = io.DisplayFramebufferScale.x;
  const float width = (400.0f * scale);
  const bool has_progress = (progress_min < progress_max);

  // eat the last imgui frame, it might've been partially rendered by the caller.
  // ImGui::EndFrame();
  // ImGui::NewFrame();

  const float logo_width = static_cast<float>(APP_ICON_WIDTH) * scale;
  const float logo_height = static_cast<float>(APP_ICON_HEIGHT) * scale;

  ImGui::SetNextWindowSize(ImVec2(logo_width, logo_height), ImGuiCond_Always);
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, (io.DisplaySize.y * 0.5f) - (50.0f * scale)),
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  if (ImGui::Begin("LoadingScreenLogo", nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoBackground))
  {
    if (m_logo_texture)
      ImGui::Image(m_logo_texture->GetHandle(), ImVec2(logo_width, logo_height));
  }
  ImGui::End();

  ImGui::SetNextWindowSize(ImVec2(width, (has_progress ? 50.0f : 30.0f) * scale), ImGuiCond_Always);
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, (io.DisplaySize.y * 0.5f) + (100.0f * scale)),
                          ImGuiCond_Always, ImVec2(0.5f, 0.0f));
  if (ImGui::Begin("LoadingScreen", nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing))
  {
    if (has_progress)
    {
      ImGui::Text("%s: %d/%d", message, progress_value, progress_max);
      ImGui::ProgressBar(static_cast<float>(progress_value) / static_cast<float>(progress_max - progress_min),
                         ImVec2(-1.0f, 0.0f), "");
      Log_InfoPrintf("%s: %d/%d", message, progress_value, progress_max);
    }
    else
    {
      const ImVec2 text_size(ImGui::CalcTextSize(message));
      ImGui::SetCursorPosX((width - text_size.x) / 2.0f);
      ImGui::TextUnformatted(message);
      Log_InfoPrintf("%s", message);
    }
  }
  ImGui::End();

  m_display->Render();
  ImGui::NewFrame();
}

void CommonHostInterface::GetGameInfo(const char* path, CDImage* image, std::string* code, std::string* title)
{
  const GameListEntry* list_entry = m_game_list->GetEntryForPath(path);
  if (list_entry)
  {
    *code = list_entry->code;
    *title = list_entry->title;
  }
  else
  {
    if (image)
      *code = System::GetGameCodeForImage(image);

    const GameListDatabaseEntry* db_entry = (!code->empty()) ? m_game_list->GetDatabaseEntryForCode(*code) : nullptr;
    if (db_entry)
      *title = db_entry->title;
    else
      *title = System::GetTitleForPath(path);
  }
}

bool CommonHostInterface::SaveResumeSaveState()
{
  if (System::IsShutdown())
    return false;

  const bool global = System::GetRunningCode().empty();
  return SaveState(global, -1);
}

bool CommonHostInterface::IsDumpingAudio() const
{
  return g_spu.IsDumpingAudio();
}

bool CommonHostInterface::StartDumpingAudio(const char* filename)
{
  if (System::IsShutdown())
    return false;

  std::string auto_filename;
  if (!filename)
  {
    const auto& code = System::GetRunningCode();
    if (code.empty())
    {
      auto_filename = GetUserDirectoryRelativePath("dump/audio/%s.wav", GetTimestampStringForFileName().GetCharArray());
    }
    else
    {
      auto_filename = GetUserDirectoryRelativePath("dump/audio/%s_%s.wav", code.c_str(),
                                                   GetTimestampStringForFileName().GetCharArray());
    }

    filename = auto_filename.c_str();
  }

  if (g_spu.StartDumpingAudio(filename))
  {
    AddFormattedOSDMessage(5.0f, TranslateString("OSDMessage", "Started dumping audio to '%s'."), filename);
    return true;
  }
  else
  {
    AddFormattedOSDMessage(10.0f, TranslateString("OSDMessage", "Failed to start dumping audio to '%s'."), filename);
    return false;
  }
}

void CommonHostInterface::StopDumpingAudio()
{
  if (System::IsShutdown() || !g_spu.StopDumpingAudio())
    return;

  AddOSDMessage(TranslateStdString("OSDMessage", "Stopped dumping audio."), 5.0f);
}

bool CommonHostInterface::SaveScreenshot(const char* filename /* = nullptr */, bool full_resolution /* = true */,
                                         bool apply_aspect_ratio /* = true */, bool compress_on_thread /* = true */)
{
  if (System::IsShutdown())
    return false;

  std::string auto_filename;
  if (!filename)
  {
    const auto& code = System::GetRunningCode();
    const char* extension = "png";
    if (code.empty())
    {
      auto_filename = GetUserDirectoryRelativePath("screenshots" FS_OSPATH_SEPARATOR_STR "%s.%s",
                                                   GetTimestampStringForFileName().GetCharArray(), extension);
    }
    else
    {
      auto_filename = GetUserDirectoryRelativePath("screenshots" FS_OSPATH_SEPARATOR_STR "%s_%s.%s", code.c_str(),
                                                   GetTimestampStringForFileName().GetCharArray(), extension);
    }

    filename = auto_filename.c_str();
  }

  if (FileSystem::FileExists(filename))
  {
    AddFormattedOSDMessage(10.0f, TranslateString("OSDMessage", "Screenshot file '%s' already exists."), filename);
    return false;
  }

  const bool screenshot_saved =
    m_display->WriteDisplayTextureToFile(filename, full_resolution, apply_aspect_ratio, compress_on_thread);
  if (!screenshot_saved)
  {
    AddFormattedOSDMessage(10.0f, TranslateString("OSDMessage", "Failed to save screenshot to '%s'"), filename);
    return false;
  }

  AddFormattedOSDMessage(5.0f, TranslateString("OSDMessage", "Screenshot saved to '%s'."), filename);
  return true;
}

void CommonHostInterface::ApplyGameSettings(bool display_osd_messages)
{
  // this gets called while booting, so can't use valid
  if (System::IsShutdown() || System::GetRunningCode().empty() || !g_settings.apply_game_settings)
    return;

  const GameSettings::Entry* gs = m_game_list->GetGameSettings(System::GetRunningPath(), System::GetRunningCode());
  if (gs)
    gs->ApplySettings(display_osd_messages);
}

bool CommonHostInterface::UpdateControllerInputMapFromGameSettings()
{
  // this gets called while booting, so can't use valid
  if (System::IsShutdown() || System::GetRunningCode().empty() || !g_settings.apply_game_settings)
    return false;

  const GameSettings::Entry* gs = m_game_list->GetGameSettings(System::GetRunningPath(), System::GetRunningCode());
  if (!gs || gs->input_profile_name.empty())
    return false;

  std::string path = GetInputProfilePath(gs->input_profile_name.c_str());
  if (path.empty())
  {
    AddFormattedOSDMessage(10.0f, TranslateString("OSDMessage", "Input profile '%s' cannot be found."),
                           gs->input_profile_name.c_str());
    return false;
  }

  if (System::GetState() == System::State::Starting)
  {
    AddFormattedOSDMessage(5.0f, TranslateString("OSDMessage", "Using input profile '%s'."),
                           gs->input_profile_name.c_str());
  }

  INISettingsInterface si(std::move(path));
  UpdateControllerInputMap(si);
  return true;
}

std::string CommonHostInterface::GetCheatFileName() const
{
  const std::string& title = System::GetRunningTitle();
  if (title.empty())
    return {};

  return GetUserDirectoryRelativePath("cheats/%s.cht", title.c_str());
}

bool CommonHostInterface::LoadCheatList(const char* filename)
{
  if (System::IsShutdown())
    return false;

  std::unique_ptr<CheatList> cl = std::make_unique<CheatList>();
  if (!cl->LoadFromFile(filename, CheatList::Format::Autodetect))
  {
    AddFormattedOSDMessage(15.0f, TranslateString("OSDMessage", "Failed to load cheats from '%s'."), filename);
    return false;
  }

  AddFormattedOSDMessage(10.0f, TranslateString("OSDMessage", "Loaded %u cheats from list. %u cheats are enabled."),
                         cl->GetCodeCount(), cl->GetEnabledCodeCount());
  System::SetCheatList(std::move(cl));
  return true;
}

bool CommonHostInterface::LoadCheatListFromGameTitle()
{
  const std::string filename(GetCheatFileName());
  if (filename.empty() || !FileSystem::FileExists(filename.c_str()))
    return false;

  return LoadCheatList(filename.c_str());
}

bool CommonHostInterface::LoadCheatListFromDatabase()
{
  if (System::GetRunningCode().empty())
    return false;

  std::unique_ptr<CheatList> cl = std::make_unique<CheatList>();
  if (!cl->LoadFromPackage(System::GetRunningCode()))
    return false;

  AddFormattedOSDMessage(10.0f, TranslateString("OSDMessage", "Loaded %u cheats from database."), cl->GetCodeCount());
  System::SetCheatList(std::move(cl));
  return true;
}

bool CommonHostInterface::SaveCheatList()
{
  if (!System::IsValid() || !System::HasCheatList())
    return false;

  const std::string filename(GetCheatFileName());
  if (filename.empty())
    return false;

  if (!System::GetCheatList()->SaveToPCSXRFile(filename.c_str()))
  {
    AddFormattedOSDMessage(15.0f, TranslateString("OSDMessage", "Failed to save cheat list to '%s'"), filename.c_str());
  }

  return true;
}

bool CommonHostInterface::SaveCheatList(const char* filename)
{
  if (!System::IsValid() || !System::HasCheatList())
    return false;

  if (!System::GetCheatList()->SaveToPCSXRFile(filename))
    return false;

  AddFormattedOSDMessage(5.0f, TranslateString("OSDMessage", "Saved %u cheats to '%s'."),
                         System::GetCheatList()->GetCodeCount(), filename);
  return true;
}

void CommonHostInterface::SetCheatCodeState(u32 index, bool enabled, bool save_to_file)
{
  if (!System::IsValid() || !System::HasCheatList())
    return;

  CheatList* cl = System::GetCheatList();
  if (index >= cl->GetCodeCount())
    return;

  CheatCode& cc = cl->GetCode(index);
  if (cc.enabled == enabled)
    return;

  cc.enabled = enabled;

  if (enabled)
  {
    AddFormattedOSDMessage(5.0f, TranslateString("OSDMessage", "Cheat '%s' enabled."), cc.description.c_str());
  }
  else
  {
    AddFormattedOSDMessage(5.0f, TranslateString("OSDMessage", "Cheat '%s' disabled."), cc.description.c_str());
  }

  if (save_to_file)
    SaveCheatList();
}

void CommonHostInterface::ApplyCheatCode(u32 index)
{
  if (!System::HasCheatList() || index >= System::GetCheatList()->GetCodeCount())
    return;

  const CheatCode& cc = System::GetCheatList()->GetCode(index);
  if (!cc.enabled)
  {
    cc.Apply();
    AddFormattedOSDMessage(5.0f, TranslateString("OSDMessage", "Applied cheat '%s'."), cc.description.c_str());
  }
  else
  {
    AddFormattedOSDMessage(5.0f, TranslateString("OSDMessage", "Cheat '%s' is already enabled."),
                           cc.description.c_str());
  }
}

void CommonHostInterface::TogglePostProcessing()
{
  if (!m_display)
    return;

  g_settings.display_post_processing = !g_settings.display_post_processing;
  if (g_settings.display_post_processing)
  {
    AddOSDMessage(TranslateStdString("OSDMessage", "Post-processing is now enabled."), 10.0f);

    if (!m_display->SetPostProcessingChain(g_settings.display_post_process_chain))
      AddOSDMessage(TranslateStdString("OSDMessage", "Failed to load post processing shader chain."), 20.0f);
  }
  else
  {
    AddOSDMessage(TranslateStdString("OSDMessage", "Post-processing is now disabled."), 10.0f);
    m_display->SetPostProcessingChain({});
  }
}

void CommonHostInterface::ReloadPostProcessingShaders()
{
  if (!m_display || !g_settings.display_post_processing)
    return;

  if (!m_display->SetPostProcessingChain(g_settings.display_post_process_chain))
    AddOSDMessage(TranslateStdString("OSDMessage", "Failed to load post-processing shader chain."), 20.0f);
  else
    AddOSDMessage(TranslateStdString("OSDMessage", "Post-processing shaders reloaded."), 10.0f);
}

bool CommonHostInterface::ParseFullscreenMode(const std::string_view& mode, u32* width, u32* height,
                                              float* refresh_rate)
{
  if (!mode.empty())
  {
    std::string_view::size_type sep1 = mode.find('x');
    if (sep1 != std::string_view::npos)
    {
      std::optional<u32> owidth = StringUtil::FromChars<u32>(mode.substr(0, sep1));
      sep1++;

      while (sep1 < mode.length() && std::isspace(mode[sep1]))
        sep1++;

      if (owidth.has_value() && sep1 < mode.length())
      {
        std::string_view::size_type sep2 = mode.find('@', sep1);
        if (sep2 != std::string_view::npos)
        {
          std::optional<u32> oheight = StringUtil::FromChars<u32>(mode.substr(sep1, sep2 - sep1));
          sep2++;

          while (sep2 < mode.length() && std::isspace(mode[sep2]))
            sep2++;

          if (oheight.has_value() && sep2 < mode.length())
          {
            std::optional<float> orefresh_rate = StringUtil::FromChars<float>(mode.substr(sep2));
            if (orefresh_rate.has_value())
            {
              *width = owidth.value();
              *height = oheight.value();
              *refresh_rate = orefresh_rate.value();
              return true;
            }
          }
        }
      }
    }
  }

  *width = 0;
  *height = 0;
  *refresh_rate = 0;
  return false;
}

bool CommonHostInterface::RequestRenderWindowSize(s32 new_window_width, s32 new_window_height)
{
  return false;
}

bool CommonHostInterface::RequestRenderWindowScale(float scale)
{
  if (!System::IsValid() || scale == 0)
    return false;

  const float y_scale =
    (static_cast<float>(m_display->GetDisplayWidth()) / static_cast<float>(m_display->GetDisplayHeight())) /
    m_display->GetDisplayAspectRatio();

  const u32 requested_width =
    std::max<u32>(static_cast<u32>(std::ceil(static_cast<float>(m_display->GetDisplayWidth()) * scale)), 1);
  const u32 requested_height =
    std::max<u32>(static_cast<u32>(std::ceil(static_cast<float>(m_display->GetDisplayHeight()) * y_scale * scale)), 1);

  return RequestRenderWindowSize(static_cast<s32>(requested_width), static_cast<s32>(requested_height));
}

std::unique_ptr<ByteStream> CommonHostInterface::OpenPackageFile(const char* path, u32 flags)
{
  const u32 allowed_flags = (BYTESTREAM_OPEN_READ | BYTESTREAM_OPEN_SEEKABLE | BYTESTREAM_OPEN_STREAMED);
  const std::string full_path(
    StringUtil::StdStringFromFormat("%s" FS_OSPATH_SEPARATOR_STR "%s", m_program_directory.c_str(), path));
  const u32 real_flags = (flags & allowed_flags) | BYTESTREAM_OPEN_READ;
  Log_DevPrintf("Requesting package file '%s'", path);
  return FileSystem::OpenFile(full_path.c_str(), real_flags);
}

#ifdef WITH_DISCORD_PRESENCE

void CommonHostInterface::SetDiscordPresenceEnabled(bool enabled)
{
  if (m_discord_presence_enabled == enabled)
    return;

  m_discord_presence_enabled = enabled;
  if (enabled)
    InitializeDiscordPresence();
  else
    ShutdownDiscordPresence();
}

void CommonHostInterface::InitializeDiscordPresence()
{
  if (m_discord_presence_active)
    return;

  DiscordEventHandlers handlers = {};
  Discord_Initialize("705325712680288296", &handlers, 0, nullptr);
  m_discord_presence_active = true;

  UpdateDiscordPresence();
}

void CommonHostInterface::ShutdownDiscordPresence()
{
  if (!m_discord_presence_active)
    return;

  Discord_ClearPresence();
  Discord_Shutdown();
  m_discord_presence_active = false;
}

void CommonHostInterface::UpdateDiscordPresence()
{
  if (!m_discord_presence_active)
    return;

  // https://discord.com/developers/docs/rich-presence/how-to#updating-presence-update-presence-payload-fields
  DiscordRichPresence rp = {};
  rp.largeImageKey = "duckstation_logo";
  rp.largeImageText = "DuckStation PS1/PSX Emulator";
  rp.startTimestamp = std::time(nullptr);

  SmallString details_string;
  if (!System::IsShutdown())
  {
    details_string.AppendFormattedString("%s (%s)", System::GetRunningTitle().c_str(),
                                         System::GetRunningCode().c_str());
  }
  else
  {
    details_string.AppendString("No Game Running");
  }

  rp.details = details_string;

  Discord_UpdatePresence(&rp);
}

void CommonHostInterface::PollDiscordPresence()
{
  if (!m_discord_presence_active)
    return;

  Discord_RunCallbacks();
}

#endif
