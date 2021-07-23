#include "regtest_host_interface.h"
#include "common/assert.h"
#include "common/audio_stream.h"
#include "common/byte_stream.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/string_util.h"
#include "core/system.h"
#include "frontend-common/game_database.h"
#include "frontend-common/game_settings.h"
#include "regtest_host_display.h"
#include "scmversion/scmversion.h"
#include <cstdio>
Log_SetChannel(RegTestHostInterface);

#ifdef _WIN32
#include "frontend-common/d3d11_host_display.h"
#include "frontend-common/d3d12_host_display.h"
#endif

#include "frontend-common/opengl_host_display.h"
#include "frontend-common/vulkan_host_display.h"

static int s_frames_to_run = 60 * 60;
static int s_frame_dump_interval = 0;
static std::shared_ptr<SystemBootParameters> s_boot_parameters;
static std::string s_dump_base_directory;
static std::string s_dump_game_directory;
static GPURenderer s_renderer_to_use = GPURenderer::Software;
static GameSettings::Database s_game_settings_db;
static GameDatabase s_game_database;

RegTestHostInterface::RegTestHostInterface() = default;

RegTestHostInterface::~RegTestHostInterface() = default;

bool RegTestHostInterface::Initialize()
{
  if (!HostInterface::Initialize())
    return false;

  SetUserDirectoryToProgramDirectory();
  s_game_database.Load();
  LoadGameSettingsDatabase();
  InitializeSettings();
  return true;
}

void RegTestHostInterface::Shutdown()
{
  HostInterface::Shutdown();
}

void RegTestHostInterface::ReportError(const char* message)
{
  Log_ErrorPrintf("Error: %s", message);
}

void RegTestHostInterface::ReportMessage(const char* message)
{
  Log_InfoPrintf("Info: %s", message);
}

void RegTestHostInterface::ReportDebuggerMessage(const char* message)
{
  Log_DevPrintf("Debugger: %s", message);
}

bool RegTestHostInterface::ConfirmMessage(const char* message)
{
  Log_InfoPrintf("Confirm: %s", message);
  return false;
}

void RegTestHostInterface::AddOSDMessage(std::string message, float duration /*= 2.0f*/)
{
  Log_InfoPrintf("OSD: %s", message.c_str());
}

void RegTestHostInterface::DisplayLoadingScreen(const char* message, int progress_min /*= -1*/,
                                                int progress_max /*= -1*/, int progress_value /*= -1*/)
{
  Log_InfoPrintf("Loading: %s (%d / %d)", message, progress_value + progress_min, progress_max);
}

void RegTestHostInterface::GetGameInfo(const char* path, CDImage* image, std::string* code, std::string* title)
{
  if (image)
  {
    GameDatabaseEntry database_entry;
    if (s_game_database.GetEntryForDisc(image, &database_entry))
    {
      *code = std::move(database_entry.serial);
      *title = std::move(database_entry.title);
      return;
    }

    *code = System::GetGameCodeForImage(image, true);
  }

  *title = FileSystem::GetFileTitleFromPath(path);
}

void RegTestHostInterface::OnRunningGameChanged(const std::string& path, CDImage* image, const std::string& game_code,
                                                const std::string& game_title)
{
  HostInterface::OnRunningGameChanged(path, image, game_code, game_title);

  Log_InfoPrintf("Game Path: %s", path.c_str());
  Log_InfoPrintf("Game Code: %s", game_code.c_str());
  Log_InfoPrintf("Game Title: %s", game_title.c_str());

  if (!s_dump_base_directory.empty())
  {
    s_dump_game_directory = StringUtil::StdStringFromFormat("%s" FS_OSPATH_SEPARATOR_STR "%s",
                                                            s_dump_base_directory.c_str(), game_title.c_str());

    if (!FileSystem::DirectoryExists(s_dump_game_directory.c_str()))
    {
      Log_InfoPrintf("Creating directory '%s'...", s_dump_game_directory.c_str());
      if (!FileSystem::CreateDirectory(s_dump_game_directory.c_str(), false))
        Panic("Failed to create dump directory.");
    }

    Log_InfoPrintf("Dumping frames to '%s'...", s_dump_game_directory.c_str());
  }

  UpdateSettings();
}

std::string RegTestHostInterface::GetStringSettingValue(const char* section, const char* key,
                                                        const char* default_value /*= ""*/)
{
  return m_settings_interface.GetStringValue(section, key, default_value);
}

bool RegTestHostInterface::GetBoolSettingValue(const char* section, const char* key, bool default_value /*= false*/)
{
  return m_settings_interface.GetBoolValue(section, key, default_value);
}

int RegTestHostInterface::GetIntSettingValue(const char* section, const char* key, int default_value /*= 0*/)
{
  return m_settings_interface.GetIntValue(section, key, default_value);
}

float RegTestHostInterface::GetFloatSettingValue(const char* section, const char* key, float default_value /*= 0.0f*/)
{
  return m_settings_interface.GetFloatValue(section, key, default_value);
}

std::vector<std::string> RegTestHostInterface::GetSettingStringList(const char* section, const char* key)
{
  return m_settings_interface.GetStringList(section, key);
}

void RegTestHostInterface::UpdateSettings()
{
  SettingsInterface& si = m_settings_interface;
  HostInterface::LoadSettings(si);

  const std::string& code = System::GetRunningCode();
  if (!code.empty())
  {
    const GameSettings::Entry* entry = s_game_settings_db.GetEntry(code);
    if (entry)
    {
      Log_InfoPrintf("Applying game settings for '%s'", code.c_str());
      entry->ApplySettings(true);
    }
  }

  HostInterface::FixIncompatibleSettings(true);
}

void RegTestHostInterface::LoadGameSettingsDatabase()
{
  const char* path = "database" FS_OSPATH_SEPARATOR_STR "gamesettings.ini";
  std::unique_ptr<ByteStream> stream = OpenPackageFile(path, BYTESTREAM_OPEN_READ | BYTESTREAM_OPEN_STREAMED);
  if (!stream)
  {
    Log_ErrorPrintf("Failed to open game settings database from '%s'. This could cause compatibility issues.", path);
    return;
  }

  const std::string data(FileSystem::ReadStreamToString(stream.get()));
  if (data.empty() || !s_game_settings_db.Load(data))
  {
    Log_ErrorPrintf("Failed to load game settings database from '%s'. This could cause compatibility issues.", path);
    return;
  }
}

void RegTestHostInterface::InitializeSettings()
{
  SettingsInterface& si = m_settings_interface;
  HostInterface::SetDefaultSettings(si);

  // Set the settings we need for testing.
  si.SetStringValue("GPU", "Renderer", Settings::GetRendererName(s_renderer_to_use));
  si.SetStringValue("Controller1", "Type", Settings::GetControllerTypeName(ControllerType::DigitalController));
  si.SetStringValue("Controller2", "Type", Settings::GetControllerTypeName(ControllerType::None));
  si.SetStringValue("MemoryCards", "Card1Type", Settings::GetMemoryCardTypeName(MemoryCardType::NonPersistent));
  si.SetStringValue("MemoryCards", "Card2Type", Settings::GetMemoryCardTypeName(MemoryCardType::None));
  si.SetStringValue("ControllerPorts", "MultitapMode", Settings::GetMultitapModeName(MultitapMode::Disabled));
  si.SetStringValue("Logging", "LogLevel", Settings::GetLogLevelName(LOGLEVEL_DEV));
  si.SetBoolValue("Logging", "LogToConsole", true);

  HostInterface::LoadSettings(si);
}

std::string RegTestHostInterface::GetBIOSDirectory()
{
  return GetUserDirectoryRelativePath("bios");
}

std::unique_ptr<ByteStream> RegTestHostInterface::OpenPackageFile(const char* path, u32 flags)
{
  std::string full_path(GetProgramDirectoryRelativePath("%s", path));
  return ByteStream_OpenFileStream(full_path.c_str(), flags);
}

bool RegTestHostInterface::AcquireHostDisplay()
{
  switch (g_settings.gpu_renderer)
  {
#ifdef _WIN32
    case GPURenderer::HardwareD3D11:
      m_display = std::make_unique<FrontendCommon::D3D11HostDisplay>();
      break;

    case GPURenderer::HardwareD3D12:
      m_display = std::make_unique<FrontendCommon::D3D12HostDisplay>();
      break;
#endif

    case GPURenderer::HardwareOpenGL:
      m_display = std::make_unique<FrontendCommon::OpenGLHostDisplay>();
      break;

    case GPURenderer::HardwareVulkan:
      m_display = std::make_unique<FrontendCommon::VulkanHostDisplay>();
      break;

    case GPURenderer::Software:
    default:
      m_display = std::make_unique<RegTestHostDisplay>();
      break;
  }

  WindowInfo wi;
  wi.type = WindowInfo::Type::Surfaceless;
  wi.surface_width = 640;
  wi.surface_height = 480;
  if (!m_display->CreateRenderDevice(wi, std::string_view(), false, false))
  {
    Log_ErrorPrintf("Failed to create render device");
    m_display.reset();
    return false;
  }

  if (!m_display->InitializeRenderDevice(std::string_view(), false, false))
  {
    Log_ErrorPrintf("Failed to initialize render device");
    m_display->DestroyRenderDevice();
    m_display.reset();
    return false;
  }

  return true;
}

void RegTestHostInterface::ReleaseHostDisplay()
{
  if (!m_display)
    return;

  m_display->DestroyRenderDevice();
  m_display.reset();
}

std::unique_ptr<AudioStream> RegTestHostInterface::CreateAudioStream(AudioBackend backend)
{
  return AudioStream::CreateNullAudioStream();
}

static void PrintCommandLineVersion()
{
  const bool was_console_enabled = Log::IsConsoleOutputEnabled();
  if (!was_console_enabled)
    Log::SetConsoleOutputParams(true);

  std::fprintf(stderr, "DuckStation Regression Test Runner Version %s (%s)\n", g_scm_tag_str, g_scm_branch_str);
  std::fprintf(stderr, "https://github.com/stenzek/duckstation\n");
  std::fprintf(stderr, "\n");

  if (!was_console_enabled)
    Log::SetConsoleOutputParams(false);
}

static void PrintCommandLineHelp(const char* progname)
{
  const bool was_console_enabled = Log::IsConsoleOutputEnabled();
  if (!was_console_enabled)
    Log::SetConsoleOutputParams(true);

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

  if (!was_console_enabled)
    Log::SetConsoleOutputParams(false);
}

static bool ParseCommandLineArgs(int argc, char* argv[])
{
  s_boot_parameters = std::make_shared<SystemBootParameters>();

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
        s_frame_dump_interval = StringUtil::FromChars<int>(argv[++i]).value_or(0);
        if (s_frames_to_run <= 0)
        {
          Log_ErrorPrintf("Invalid dump interval specified: -1", s_frame_dump_interval);
          return false;
        }

        continue;
      }
      else if (CHECK_ARG_PARAM("-frames"))
      {
        s_frames_to_run = StringUtil::FromChars<int>(argv[++i]).value_or(-1);
        if (s_frames_to_run <= 0)
        {
          Log_ErrorPrintf("Invalid frame count specified: %d", s_frames_to_run);
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

        Log::SetConsoleOutputParams(true, nullptr, level.value());
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

        s_renderer_to_use = renderer.value();
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

    if (!s_boot_parameters->filename.empty())
      s_boot_parameters->filename += ' ';
    s_boot_parameters->filename += argv[i];
  }

  return true;
}

static std::string GetFrameDumpFilename(int frame)
{
  return StringUtil::StdStringFromFormat("%s" FS_OSPATH_SEPARATOR_STR "frame_%05d.png", s_dump_game_directory.c_str(),
                                         frame);
}

int main(int argc, char* argv[])
{
  Log::SetConsoleOutputParams(true, nullptr, LOGLEVEL_VERBOSE);

  if (!ParseCommandLineArgs(argc, argv))
    return -1;

  int result = -1;

  Log_InfoPrintf("Initializing...");
  g_host_interface = new RegTestHostInterface();
  if (!g_host_interface->Initialize())
    goto cleanup;

  if (s_boot_parameters->filename.empty())
  {
    Log_ErrorPrintf("No boot path specified.");
    goto cleanup;
  }

  Log_InfoPrintf("Trying to boot '%s'...", s_boot_parameters->filename.c_str());
  if (!g_host_interface->BootSystem(std::move(s_boot_parameters)))
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

  for (int frame = 1; frame <= s_frames_to_run; frame++)
  {
    System::RunFrame();

    if (s_frame_dump_interval > 0 && (s_frame_dump_interval == 1 || (frame % s_frame_dump_interval) == 0))
    {
      std::string dump_filename(GetFrameDumpFilename(frame));
      g_host_interface->GetDisplay()->WriteDisplayTextureToFile(std::move(dump_filename));
    }

    g_host_interface->GetDisplay()->Render();

    System::UpdatePerformanceCounters();
  }

  Log_InfoPrintf("All done, shutting down system.");
  g_host_interface->DestroySystem();

  Log_InfoPrintf("Exiting with success.");
  result = 0;

cleanup:
  delete g_host_interface;
  return result;
}
