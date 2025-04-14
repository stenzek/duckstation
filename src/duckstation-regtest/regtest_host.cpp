// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "core/achievements.h"
#include "core/bus.h"
#include "core/controller.h"
#include "core/fullscreen_ui.h"
#include "core/game_list.h"
#include "core/gpu.h"
#include "core/gpu_backend.h"
#include "core/gpu_presenter.h"
#include "core/gpu_thread.h"
#include "core/host.h"
#include "core/spu.h"
#include "core/system.h"
#include "core/system_private.h"

#include "scmversion/scmversion.h"

#include "util/cd_image.h"
#include "util/gpu_device.h"
#include "util/imgui_fullscreen.h"
#include "util/imgui_manager.h"
#include "util/input_manager.h"
#include "util/platform_misc.h"

#include "common/assert.h"
#include "common/crash_handler.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/memory_settings_interface.h"
#include "common/path.h"
#include "common/sha256_digest.h"
#include "common/string_util.h"
#include "common/threading.h"
#include "common/timer.h"

#include "fmt/format.h"

#include <csignal>
#include <cstdio>

LOG_CHANNEL(Host);

namespace RegTestHost {

static bool ParseCommandLineParameters(int argc, char* argv[], std::optional<SystemBootParameters>& autoboot);
static void PrintCommandLineVersion();
static void PrintCommandLineHelp(const char* progname);
static bool InitializeConfig();
static void InitializeEarlyConsole();
static void HookSignals();
static bool SetFolders();
static bool SetNewDataRoot(const std::string& filename);
static void DumpSystemStateHashes();
static std::string GetFrameDumpPath(u32 frame);
static void ProcessCPUThreadEvents();
static void GPUThreadEntryPoint();

struct RegTestHostState
{
  ALIGN_TO_CACHE_LINE std::mutex cpu_thread_events_mutex;
  std::condition_variable cpu_thread_event_done;
  std::deque<std::pair<std::function<void()>, bool>> cpu_thread_events;
  u32 blocking_cpu_events_pending = 0;
};

static RegTestHostState s_state;

} // namespace RegTestHost

static std::unique_ptr<MemorySettingsInterface> s_base_settings_interface;
static Threading::Thread s_gpu_thread;

static u32 s_frames_to_run = 60 * 60;
static u32 s_frames_remaining = 0;
static u32 s_frame_dump_interval = 0;
static std::string s_dump_base_directory;

bool RegTestHost::SetFolders()
{
  std::string program_path(FileSystem::GetProgramPath());
  DEV_LOG("Program Path: {}", program_path);

  EmuFolders::AppRoot = Path::Canonicalize(Path::GetDirectory(program_path));
  EmuFolders::DataRoot = Host::Internal::ComputeDataDirectory();
  EmuFolders::Resources = Path::Combine(EmuFolders::AppRoot, "resources");

  DEV_LOG("AppRoot Directory: {}", EmuFolders::AppRoot);
  DEV_LOG("DataRoot Directory: {}", EmuFolders::DataRoot);
  DEV_LOG("Resources Directory: {}", EmuFolders::Resources);

  // Write crash dumps to the data directory, since that'll be accessible for certain.
  CrashHandler::SetWriteDirectory(EmuFolders::DataRoot);

  // the resources directory should exist, bail out if not
  if (!FileSystem::DirectoryExists(EmuFolders::Resources.c_str()))
  {
    ERROR_LOG("Resources directory is missing, your installation is incomplete.");
    return false;
  }

  if (EmuFolders::DataRoot.empty() || !FileSystem::EnsureDirectoryExists(EmuFolders::DataRoot.c_str(), false))
  {
    ERROR_LOG("Failed to create data directory '{}'", EmuFolders::DataRoot);
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
  g_settings.Load(si, si);
  g_settings.Save(si, false);
  si.SetStringValue("GPU", "Renderer", Settings::GetRendererName(GPURenderer::Software));
  si.SetBoolValue("GPU", "DisableShaderCache", true);
  si.SetStringValue("Pad1", "Type", Controller::GetControllerInfo(ControllerType::AnalogController).name);
  si.SetStringValue("Pad2", "Type", Controller::GetControllerInfo(ControllerType::None).name);
  si.SetStringValue("MemoryCards", "Card1Type", Settings::GetMemoryCardTypeName(MemoryCardType::NonPersistent));
  si.SetStringValue("MemoryCards", "Card2Type", Settings::GetMemoryCardTypeName(MemoryCardType::None));
  si.SetStringValue("ControllerPorts", "MultitapMode", Settings::GetMultitapModeName(MultitapMode::Disabled));
  si.SetStringValue("Audio", "Backend", AudioStream::GetBackendName(AudioBackend::Null));
  si.SetBoolValue("Logging", "LogToConsole", false);
  si.SetBoolValue("Logging", "LogToFile", false);
  si.SetStringValue("Logging", "LogLevel", Settings::GetLogLevelName(Log::Level::Info));
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

void Host::ReportFatalError(std::string_view title, std::string_view message)
{
  ERROR_LOG("ReportFatalError: {}", message);
  abort();
}

void Host::ReportErrorAsync(std::string_view title, std::string_view message)
{
  if (!title.empty() && !message.empty())
    ERROR_LOG("ReportErrorAsync: {}: {}", title, message);
  else if (!message.empty())
    ERROR_LOG("ReportErrorAsync: {}", message);
}

bool Host::ConfirmMessage(std::string_view title, std::string_view message)
{
  if (!title.empty() && !message.empty())
    ERROR_LOG("ConfirmMessage: {}: {}", title, message);
  else if (!message.empty())
    ERROR_LOG("ConfirmMessage: {}", message);

  return true;
}

void Host::ConfirmMessageAsync(std::string_view title, std::string_view message, ConfirmMessageAsyncCallback callback,
                               std::string_view yes_text, std::string_view no_text)
{
  if (!title.empty() && !message.empty())
    ERROR_LOG("ConfirmMessage: {}: {}", title, message);
  else if (!message.empty())
    ERROR_LOG("ConfirmMessage: {}", message);

  callback(true);
}

void Host::ReportDebuggerMessage(std::string_view message)
{
  ERROR_LOG("ReportDebuggerMessage: {}", message);
}

std::span<const std::pair<const char*, const char*>> Host::GetAvailableLanguageList()
{
  return {};
}

bool Host::ChangeLanguage(const char* new_language)
{
  return false;
}

s32 Host::Internal::GetTranslatedStringImpl(std::string_view context, std::string_view msg,
                                            std::string_view disambiguation, char* tbuf, size_t tbuf_space)
{
  if (msg.size() > tbuf_space)
    return -1;
  else if (msg.empty())
    return 0;

  std::memcpy(tbuf, msg.data(), msg.size());
  return static_cast<s32>(msg.size());
}

std::string Host::TranslatePluralToString(const char* context, const char* msg, const char* disambiguation, int count)
{
  TinyString count_str = TinyString::from_format("{}", count);

  std::string ret(msg);
  for (;;)
  {
    std::string::size_type pos = ret.find("%n");
    if (pos == std::string::npos)
      break;

    ret.replace(pos, pos + 2, count_str.view());
  }

  return ret;
}

SmallString Host::TranslatePluralToSmallString(const char* context, const char* msg, const char* disambiguation,
                                               int count)
{
  SmallString ret(msg);
  ret.replace("%n", TinyString::from_format("{}", count));
  return ret;
}

void Host::LoadSettings(const SettingsInterface& si, std::unique_lock<std::mutex>& lock)
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

std::optional<DynamicHeapArray<u8>> Host::ReadResourceFile(std::string_view filename, bool allow_override, Error* error)
{
  const std::string path(Path::Combine(EmuFolders::Resources, filename));
  return FileSystem::ReadBinaryFile(path.c_str(), error);
}

std::optional<std::string> Host::ReadResourceFileToString(std::string_view filename, bool allow_override, Error* error)
{
  const std::string path(Path::Combine(EmuFolders::Resources, filename));
  return FileSystem::ReadFileToString(path.c_str(), error);
}

std::optional<std::time_t> Host::GetResourceFileTimestamp(std::string_view filename, bool allow_override)
{
  const std::string path(Path::Combine(EmuFolders::Resources, filename));
  FILESYSTEM_STAT_DATA sd;
  if (!FileSystem::StatFile(path.c_str(), &sd))
  {
    ERROR_LOG("Failed to stat resource file '{}'", filename);
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

void Host::OnSystemAbnormalShutdown(const std::string_view reason)
{
  // Already logged in core.
}

void Host::OnGPUThreadRunIdleChanged(bool is_active)
{
  //
}

void Host::OnPerformanceCountersUpdated(const GPUBackend* gpu_backend)
{
  //
}

void Host::OnGameChanged(const std::string& disc_path, const std::string& game_serial, const std::string& game_name,
                         GameHash hash)
{
  INFO_LOG("Disc Path: {}", disc_path);
  INFO_LOG("Game Serial: {}", game_serial);
  INFO_LOG("Game Name: {}", game_name);
}

void Host::OnMediaCaptureStarted()
{
  //
}

void Host::OnMediaCaptureStopped()
{
  //
}

void Host::PumpMessagesOnCPUThread()
{
  RegTestHost::ProcessCPUThreadEvents();

  s_frames_remaining--;
  if (s_frames_remaining == 0)
  {
    RegTestHost::DumpSystemStateHashes();
    System::ShutdownSystem(false);
  }
}

void Host::RunOnCPUThread(std::function<void()> function, bool block /* = false */)
{
  using namespace RegTestHost;

  std::unique_lock lock(s_state.cpu_thread_events_mutex);
  s_state.cpu_thread_events.emplace_back(std::move(function), block);
  s_state.blocking_cpu_events_pending += BoolToUInt32(block);
  if (block)
    s_state.cpu_thread_event_done.wait(lock, []() { return s_state.blocking_cpu_events_pending == 0; });
}

void RegTestHost::ProcessCPUThreadEvents()
{
  std::unique_lock lock(s_state.cpu_thread_events_mutex);

  for (;;)
  {
    if (s_state.cpu_thread_events.empty())
      break;

    auto event = std::move(s_state.cpu_thread_events.front());
    s_state.cpu_thread_events.pop_front();
    lock.unlock();
    event.first();
    lock.lock();

    if (event.second)
    {
      s_state.blocking_cpu_events_pending--;
      s_state.cpu_thread_event_done.notify_one();
    }
  }
}

void Host::RunOnUIThread(std::function<void()> function, bool block /* = false */)
{
  RunOnCPUThread(std::move(function), block);
}

void Host::RequestResizeHostDisplay(s32 width, s32 height)
{
  //
}

void Host::RequestResetSettings(bool system, bool controller)
{
  //
}

void Host::RequestExitApplication(bool save_state_if_running)
{
  //
}

void Host::RequestExitBigPicture()
{
  //
}

void Host::RequestSystemShutdown(bool allow_confirm, bool save_state, bool check_memcard_busy)
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

std::optional<WindowInfo> Host::AcquireRenderWindow(RenderAPI render_api, bool fullscreen, bool exclusive_fullscreen,
                                                    Error* error)
{
  return WindowInfo();
}

void Host::ReleaseRenderWindow()
{
  //
}

void Host::BeginTextInput()
{
  //
}

void Host::EndTextInput()
{
  //
}

bool Host::CreateAuxiliaryRenderWindow(s32 x, s32 y, u32 width, u32 height, std::string_view title,
                                       std::string_view icon_name, AuxiliaryRenderWindowUserData userdata,
                                       AuxiliaryRenderWindowHandle* handle, WindowInfo* wi, Error* error)
{
  return false;
}

void Host::DestroyAuxiliaryRenderWindow(AuxiliaryRenderWindowHandle handle, s32* pos_x /* = nullptr */,
                                        s32* pos_y /* = nullptr */, u32* width /* = nullptr */,
                                        u32* height /* = nullptr */)
{
}

void Host::FrameDoneOnGPUThread(GPUBackend* gpu_backend, u32 frame_number)
{
  const GPUPresenter& presenter = gpu_backend->GetPresenter();
  if (s_frame_dump_interval == 0 || (frame_number % s_frame_dump_interval) != 0 || !presenter.HasDisplayTexture())
    return;

  // Need to take a copy of the display texture.
  GPUTexture* const read_texture = presenter.GetDisplayTexture();
  const u32 read_x = static_cast<u32>(presenter.GetDisplayTextureViewX());
  const u32 read_y = static_cast<u32>(presenter.GetDisplayTextureViewY());
  const u32 read_width = static_cast<u32>(presenter.GetDisplayTextureViewWidth());
  const u32 read_height = static_cast<u32>(presenter.GetDisplayTextureViewHeight());
  const ImageFormat read_format = GPUTexture::GetImageFormatForTextureFormat(read_texture->GetFormat());
  if (read_format == ImageFormat::None)
    return;

  Image image(read_width, read_height, read_format);
  std::unique_ptr<GPUDownloadTexture> dltex;
  if (g_gpu_device->GetFeatures().memory_import)
  {
    dltex = g_gpu_device->CreateDownloadTexture(read_width, read_height, read_texture->GetFormat(), image.GetPixels(),
                                                image.GetStorageSize(), image.GetPitch());
  }
  if (!dltex)
  {
    if (!(dltex = g_gpu_device->CreateDownloadTexture(read_width, read_height, read_texture->GetFormat())))
    {
      ERROR_LOG("Failed to create {}x{} {} download texture", read_width, read_height,
                GPUTexture::GetFormatName(read_texture->GetFormat()));
      return;
    }
  }

  dltex->CopyFromTexture(0, 0, read_texture, read_x, read_y, read_width, read_height, 0, 0, !dltex->IsImported());
  if (!dltex->ReadTexels(0, 0, read_width, read_height, image.GetPixels(), image.GetPitch()))
  {
    ERROR_LOG("Failed to read {}x{} download texture", read_width, read_height);
    gpu_backend->RestoreDeviceContext();
    return;
  }

  // no more GPU calls
  gpu_backend->RestoreDeviceContext();

  Error error;
  const std::string path = RegTestHost::GetFrameDumpPath(frame_number);
  auto fp = FileSystem::OpenManagedCFile(path.c_str(), "wb", &error);
  if (!fp)
  {
    ERROR_LOG("Can't open file '{}': {}", Path::GetFileName(path), error.GetDescription());
    return;
  }

  System::QueueAsyncTask([path = std::move(path), fp = fp.release(), image = std::move(image)]() mutable {
    Error error;

    if (image.GetFormat() != ImageFormat::RGBA8)
    {
      std::optional<Image> convert_image = image.ConvertToRGBA8(&error);
      if (!convert_image.has_value())
      {
        ERROR_LOG("Failed to convert {} screenshot to RGBA8: {}", Image::GetFormatName(image.GetFormat()),
                  error.GetDescription());
        image.Invalidate();
      }
      else
      {
        image = std::move(convert_image.value());
      }
    }

    bool result = false;
    if (image.IsValid())
    {
      image.SetAllPixelsOpaque();

      result = image.SaveToFile(path.c_str(), fp, Image::DEFAULT_SAVE_QUALITY, &error);
      if (!result)
        ERROR_LOG("Failed to save screenshot to '{}': '{}'", Path::GetFileName(path), error.GetDescription());
    }

    std::fclose(fp);
    return result;
  });
}

void Host::OpenURL(std::string_view url)
{
  //
}

std::string Host::GetClipboardText()
{
  return std::string();
}

bool Host::CopyTextToClipboard(std::string_view text)
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

#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION

void Host::OnRAIntegrationMenuChanged()
{
  // noop
}

#endif

void Host::OnCoverDownloaderOpenRequested()
{
  // noop
}

const char* Host::GetDefaultFullscreenUITheme()
{
  return "";
}

bool Host::ShouldPreferHostFileSelector()
{
  return false;
}

void Host::OpenHostFileSelectorAsync(std::string_view title, bool select_directory, FileSelectorCallback callback,
                                     FileSelectorFilters filters /* = FileSelectorFilters() */,
                                     std::string_view initial_directory /* = std::string_view() */)
{
  callback(std::string());
}

std::optional<u32> InputManager::ConvertHostKeyboardStringToCode(std::string_view str)
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

void Host::AddFixedInputBindings(const SettingsInterface& si)
{
  // noop
}

void Host::OnInputDeviceConnected(InputBindingKey key, std::string_view identifier, std::string_view device_name)
{
  // noop
}

void Host::OnInputDeviceDisconnected(InputBindingKey key, std::string_view identifier)
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

void Host::OnGameListEntriesChanged(std::span<const u32> changed_indices)
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

#ifndef _WIN32
  // Ignore SIGCHLD by default on Linux, since we kick off aplay asynchronously.
  struct sigaction sa_chld = {};
  sigemptyset(&sa_chld.sa_mask);
  sa_chld.sa_handler = SIG_IGN;
  sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP | SA_NOCLDWAIT;
  sigaction(SIGCHLD, &sa_chld, nullptr);
#endif
}

void RegTestHost::GPUThreadEntryPoint()
{
  Threading::SetNameOfCurrentThread("CPU Thread");
  GPUThread::Internal::GPUThreadEntryPoint();
}

void RegTestHost::DumpSystemStateHashes()
{
  Error error;

  // don't save full state on gpu dump, it's not going to be complete...
  if (!System::IsReplayingGPUDump())
  {
    DynamicHeapArray<u8> state_data(System::GetMaxSaveStateSize());
    size_t state_data_size;
    if (!System::SaveStateDataToBuffer(state_data, &state_data_size, &error))
    {
      ERROR_LOG("Failed to save system state: {}", error.GetDescription());
      return;
    }

    INFO_LOG("Save State Hash: {}",
             SHA256Digest::DigestToString(SHA256Digest::GetDigest(state_data.cspan(0, state_data_size))));
    INFO_LOG("RAM Hash: {}",
             SHA256Digest::DigestToString(SHA256Digest::GetDigest(std::span<const u8>(Bus::g_ram, Bus::g_ram_size))));
    INFO_LOG("SPU RAM Hash: {}", SHA256Digest::DigestToString(SHA256Digest::GetDigest(SPU::GetRAM())));
  }

  INFO_LOG("VRAM Hash: {}", SHA256Digest::DigestToString(SHA256Digest::GetDigest(
                              std::span<const u8>(reinterpret_cast<const u8*>(g_vram), VRAM_SIZE))));
}

void RegTestHost::InitializeEarlyConsole()
{
  const bool was_console_enabled = Log::IsConsoleOutputEnabled();
  if (!was_console_enabled)
  {
    Log::SetConsoleOutputParams(true);
    Log::SetLogLevel(Log::Level::Info);
  }
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
  std::fprintf(stderr, "  -console: Enables console logging output.\n");
  std::fprintf(stderr, "  -pgxp: Enables PGXP.\n");
  std::fprintf(stderr, "  -pgxp-cpu: Forces PGXP CPU mode.\n");
  std::fprintf(stderr, "  -renderer <renderer>: Sets the graphics renderer. Default to software.\n");
  std::fprintf(stderr, "  -upscale <multiplier>: Enables upscaled rendering at the specified multiplier.\n");
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
          ERROR_LOG("Invalid dump directory specified.");
          return false;
        }

        continue;
      }
      else if (CHECK_ARG_PARAM("-dumpinterval"))
      {
        s_frame_dump_interval = StringUtil::FromChars<u32>(argv[++i]).value_or(0);
        if (s_frame_dump_interval <= 0)
        {
          ERROR_LOG("Invalid dump interval specified: {}", argv[i]);
          return false;
        }

        continue;
      }
      else if (CHECK_ARG_PARAM("-frames"))
      {
        s_frames_to_run = StringUtil::FromChars<u32>(argv[++i]).value_or(0);
        if (s_frames_to_run == 0)
        {
          ERROR_LOG("Invalid frame count specified: {}", argv[i]);
          return false;
        }

        continue;
      }
      else if (CHECK_ARG_PARAM("-log"))
      {
        std::optional<Log::Level> level = Settings::ParseLogLevelName(argv[++i]);
        if (!level.has_value())
        {
          ERROR_LOG("Invalid log level specified.");
          return false;
        }

        Log::SetLogLevel(level.value());
        s_base_settings_interface->SetStringValue("Logging", "LogLevel", Settings::GetLogLevelName(level.value()));
        continue;
      }
      else if (CHECK_ARG("-console"))
      {
        Log::SetConsoleOutputParams(true);
        s_base_settings_interface->SetBoolValue("Logging", "LogToConsole", true);
        continue;
      }
      else if (CHECK_ARG_PARAM("-renderer"))
      {
        std::optional<GPURenderer> renderer = Settings::ParseRendererName(argv[++i]);
        if (!renderer.has_value())
        {
          ERROR_LOG("Invalid renderer specified.");
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
          ERROR_LOG("Invalid upscale value.");
          return false;
        }

        INFO_LOG("Setting upscale to {}.", upscale);
        s_base_settings_interface->SetIntValue("GPU", "ResolutionScale", static_cast<s32>(upscale));
        continue;
      }
      else if (CHECK_ARG_PARAM("-cpu"))
      {
        const std::optional<CPUExecutionMode> cpu = Settings::ParseCPUExecutionMode(argv[++i]);
        if (!cpu.has_value())
        {
          ERROR_LOG("Invalid CPU execution mode.");
          return false;
        }

        INFO_LOG("Setting CPU execution mode to {}.", Settings::GetCPUExecutionModeName(cpu.value()));
        s_base_settings_interface->SetStringValue("CPU", "ExecutionMode",
                                                  Settings::GetCPUExecutionModeName(cpu.value()));
        continue;
      }
      else if (CHECK_ARG("-pgxp"))
      {
        INFO_LOG("Enabling PGXP.");
        s_base_settings_interface->SetBoolValue("GPU", "PGXPEnable", true);
        continue;
      }
      else if (CHECK_ARG("-pgxp-cpu"))
      {
        INFO_LOG("Enabling PGXP CPU mode.");
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
        ERROR_LOG("Unknown parameter: '{}'", argv[i]);
        return false;
      }

#undef CHECK_ARG
#undef CHECK_ARG_PARAM
    }

    if (autoboot && !autoboot->path.empty())
      autoboot->path += ' ';
    AutoBoot(autoboot)->path += argv[i];
  }

  return true;
}

bool RegTestHost::SetNewDataRoot(const std::string& filename)
{
  if (!s_dump_base_directory.empty())
  {
    std::string game_subdir = Path::SanitizeFileName(Path::GetFileTitle(filename));
    INFO_LOG("Writing to subdirectory '{}'", game_subdir);

    std::string dump_directory = Path::Combine(s_dump_base_directory, game_subdir);
    if (!FileSystem::DirectoryExists(dump_directory.c_str()))
    {
      INFO_LOG("Creating directory '{}'...", dump_directory);
      if (!FileSystem::CreateDirectory(dump_directory.c_str(), false))
        Panic("Failed to create dump directory.");
    }

    // Switch to file logging.
    INFO_LOG("Dumping frames to '{}'...", dump_directory);
    EmuFolders::DataRoot = std::move(dump_directory);
    s_base_settings_interface->SetBoolValue("Logging", "LogToFile", true);
    s_base_settings_interface->SetStringValue("Logging", "LogLevel", Settings::GetLogLevelName(Log::Level::Dev));
    Settings::UpdateLogConfig(*s_base_settings_interface);
  }

  return true;
}

std::string RegTestHost::GetFrameDumpPath(u32 frame)
{
  return Path::Combine(EmuFolders::DataRoot, fmt::format("frame_{:05d}.png", frame));
}

int main(int argc, char* argv[])
{
  CrashHandler::Install(&Bus::CleanupMemoryMap);

  Error startup_error;
  if (!System::PerformEarlyHardwareChecks(&startup_error) || !System::ProcessStartup(&startup_error))
  {
    ERROR_LOG("CPUThreadInitialize() failed: {}", startup_error.GetDescription());
    return EXIT_FAILURE;
  }

  RegTestHost::InitializeEarlyConsole();

  if (!RegTestHost::InitializeConfig())
    return EXIT_FAILURE;

  std::optional<SystemBootParameters> autoboot;
  if (!RegTestHost::ParseCommandLineParameters(argc, argv, autoboot))
    return EXIT_FAILURE;

  if (!autoboot || autoboot->path.empty())
  {
    ERROR_LOG("No boot path specified.");
    return EXIT_FAILURE;
  }

  if (!RegTestHost::SetNewDataRoot(autoboot->path))
    return EXIT_FAILURE;

  // Only one async worker.
  if (!System::CPUThreadInitialize(&startup_error, 1))
  {
    ERROR_LOG("CPUThreadInitialize() failed: {}", startup_error.GetDescription());
    return EXIT_FAILURE;
  }

  RegTestHost::HookSignals();
  s_gpu_thread.Start(&RegTestHost::GPUThreadEntryPoint);

  Error error;
  int result = -1;
  INFO_LOG("Trying to boot '{}'...", autoboot->path);
  if (!System::BootSystem(std::move(autoboot.value()), &error))
  {
    ERROR_LOG("Failed to boot system: {}", error.GetDescription());
    goto cleanup;
  }

  if (System::IsReplayingGPUDump() && !s_dump_base_directory.empty())
  {
    INFO_LOG("Replaying GPU dump, dumping all frames.");
    s_frame_dump_interval = 1;
    s_frames_to_run = static_cast<u32>(System::GetGPUDumpFrameCount());
  }

  if (s_frame_dump_interval > 0)
  {
    if (s_dump_base_directory.empty())
    {
      ERROR_LOG("Dump directory not specified.");
      goto cleanup;
    }

    INFO_LOG("Dumping every {}th frame to '{}'.", s_frame_dump_interval, s_dump_base_directory);
  }

  INFO_LOG("Running for {} frames...", s_frames_to_run);
  s_frames_remaining = s_frames_to_run;

  {
    const Timer::Value start_time = Timer::GetCurrentValue();

    System::Execute();

    const Timer::Value elapsed_time = Timer::GetCurrentValue() - start_time;
    const double elapsed_time_ms = Timer::ConvertValueToMilliseconds(elapsed_time);
    INFO_LOG("Total execution time: {:.2f}ms, average frame time {:.2f}ms, {:.2f} FPS", elapsed_time_ms,
             elapsed_time_ms / static_cast<double>(s_frames_to_run),
             static_cast<double>(s_frames_to_run) / elapsed_time_ms * 1000.0);
  }

  INFO_LOG("Exiting with success.");
  result = 0;

cleanup:
  if (s_gpu_thread.Joinable())
  {
    GPUThread::Internal::RequestShutdown();
    s_gpu_thread.Join();
  }

  RegTestHost::ProcessCPUThreadEvents();
  System::CPUThreadShutdown();
  System::ProcessShutdown();
  return result;
}
