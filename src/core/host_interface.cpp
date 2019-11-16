#include "host_interface.h"
#include "YBaseLib/ByteStream.h"
#include "YBaseLib/Log.h"
#include "YBaseLib/Timer.h"
#include "bios.h"
#include "common/audio_stream.h"
#include "host_display.h"
#include "system.h"
#include <filesystem>
Log_SetChannel(HostInterface);

#ifdef _WIN32
#include "YBaseLib/Windows/WindowsHeaders.h"
#else
#include <time.h>
#endif

HostInterface::HostInterface()
{
  m_settings.SetDefaults();
  m_last_throttle_time = Y_TimerGetValue();
}

HostInterface::~HostInterface() = default;

bool HostInterface::CreateSystem()
{
  m_system = System::Create(this);

  // Pull in any invalid settings which have been reset.
  m_settings = m_system->GetSettings();
  m_paused = true;
  UpdateSpeedLimiterState();
  return true;
}

bool HostInterface::BootSystem(const char* filename, const char* state_filename)
{
  if (!m_system->Boot(filename))
    return false;

  m_paused = m_settings.start_paused;
  ConnectControllers();
  UpdateSpeedLimiterState();
  return true;
}

void HostInterface::DestroySystem()
{
  m_system.reset();
  m_paused = false;
  UpdateSpeedLimiterState();
}

void HostInterface::ReportError(const char* message)
{
  Log_ErrorPrint(message);
}

void HostInterface::ReportMessage(const char* message)
{
  Log_InfoPrintf(message);
}

std::optional<std::vector<u8>> HostInterface::GetBIOSImage(ConsoleRegion region)
{
  // Try the other default filenames in the directory of the configured BIOS.
#define TRY_FILENAME(filename)                                                                                         \
  do                                                                                                                   \
  {                                                                                                                    \
    std::string try_filename = filename;                                                                               \
    std::optional<BIOS::Image> found_image = BIOS::LoadImageFromFile(try_filename);                                    \
    BIOS::Hash found_hash = BIOS::GetHash(*found_image);                                                               \
    Log_DevPrintf("Hash for BIOS '%s': %s", try_filename.c_str(), found_hash.ToString().c_str());                      \
    if (BIOS::IsValidHashForRegion(region, found_hash))                                                                \
    {                                                                                                                  \
      Log_InfoPrintf("Using BIOS from '%s'", try_filename.c_str());                                                    \
      return found_image;                                                                                              \
    }                                                                                                                  \
  } while (0)

#define RELATIVE_PATH(filename) std::filesystem::path(m_settings.bios_path).replace_filename(filename).string()

  // Try the configured image.
  TRY_FILENAME(m_settings.bios_path);

  // Try searching in the same folder for other region's images.
  switch (region)
  {
    case ConsoleRegion::NTSC_J:
      TRY_FILENAME(RELATIVE_PATH("scph1000.bin"));
      TRY_FILENAME(RELATIVE_PATH("scph5500.bin"));
      break;

    case ConsoleRegion::NTSC_U:
      TRY_FILENAME(RELATIVE_PATH("scph1001.bin"));
      TRY_FILENAME(RELATIVE_PATH("scph5501.bin"));
      break;

    case ConsoleRegion::PAL:
      TRY_FILENAME(RELATIVE_PATH("scph1002.bin"));
      TRY_FILENAME(RELATIVE_PATH("scph5502.bin"));
      break;

    default:
      break;
  }

#undef RELATIVE_PATH
#undef TRY_FILENAME

  // Fall back to the default image.
  Log_WarningPrintf("No suitable BIOS image for region %s could be located, using configured image '%s'. This may "
                    "result in instability.",
                    Settings::GetConsoleRegionName(region), m_settings.bios_path.c_str());
  return BIOS::LoadImageFromFile(m_settings.bios_path);
}

void HostInterface::ConnectControllers() {}

void HostInterface::Throttle()
{
  // Allow variance of up to 40ms either way.
  constexpr s64 MAX_VARIANCE_TIME = INT64_C(40000000);

  // Don't sleep for <1ms or >=period.
  constexpr s64 MINIMUM_SLEEP_TIME = INT64_C(1000000);

  // Use unsigned for defined overflow/wrap-around.
  const u64 time = static_cast<u64>(m_throttle_timer.GetTimeNanoseconds());
  const s64 sleep_time = static_cast<s64>(m_last_throttle_time - time);
  if (std::abs(sleep_time) >= MAX_VARIANCE_TIME)
  {
#ifdef Y_BUILD_CONFIG_RELEASE
    // Don't display the slow messages in debug, it'll always be slow...
    // Limit how often the messages are displayed.
    if (m_speed_lost_time_timestamp.GetTimeSeconds() >= 1.0f)
    {
      Log_WarningPrintf("System too %s, lost %.2f ms", sleep_time < 0 ? "slow" : "fast",
                        static_cast<double>(std::abs(sleep_time) - MAX_VARIANCE_TIME) / 1000000.0);
      m_speed_lost_time_timestamp.Reset();
    }
#endif
    m_last_throttle_time = time - MAX_VARIANCE_TIME;
  }
  else if (sleep_time >= MINIMUM_SLEEP_TIME && sleep_time <= m_throttle_period)
  {
#ifdef WIN32
    Sleep(static_cast<u32>(sleep_time / 1000000));
#else
    const struct timespec ts = {0, static_cast<long>(sleep_time)};
    nanosleep(&ts, nullptr);
#endif
  }

  m_last_throttle_time += m_throttle_period;
}

bool HostInterface::LoadState(const char* filename)
{
  ByteStream* stream;
  if (!ByteStream_OpenFileStream(filename, BYTESTREAM_OPEN_READ | BYTESTREAM_OPEN_STREAMED, &stream))
    return false;

  AddOSDMessage(SmallString::FromFormat("Loading state from %s...", filename));

  const bool result = m_system->LoadState(stream);
  if (!result)
  {
    ReportError(SmallString::FromFormat("Loading state from %s failed. Resetting.", filename));
    m_system->Reset();
  }

  stream->Release();
  return result;
}

bool HostInterface::SaveState(const char* filename)
{
  ByteStream* stream;
  if (!ByteStream_OpenFileStream(filename,
                                 BYTESTREAM_OPEN_CREATE | BYTESTREAM_OPEN_WRITE | BYTESTREAM_OPEN_TRUNCATE |
                                   BYTESTREAM_OPEN_ATOMIC_UPDATE | BYTESTREAM_OPEN_STREAMED,
                                 &stream))
  {
    return false;
  }

  const bool result = m_system->SaveState(stream);
  if (!result)
  {
    ReportError(SmallString::FromFormat("Saving state to %s failed.", filename));
    stream->Discard();
  }
  else
  {
    AddOSDMessage(SmallString::FromFormat("State saved to %s.", filename));
    stream->Commit();
  }

  stream->Release();
  return result;
}

void HostInterface::UpdateSpeedLimiterState()
{
  m_speed_limiter_enabled = m_settings.speed_limiter_enabled && !m_speed_limiter_temp_disabled;

  const bool audio_sync_enabled = !m_system || m_paused || m_speed_limiter_enabled;
  const bool vsync_enabled = !m_system || m_paused || (m_speed_limiter_enabled && m_settings.gpu_vsync);
  Log_InfoPrintf("Syncing to %s%s", audio_sync_enabled ? "audio" : "",
                 (m_speed_limiter_enabled && vsync_enabled) ? " and video" : "");

  m_audio_stream->SetSync(audio_sync_enabled);
  m_display->SetVSync(vsync_enabled);
}
