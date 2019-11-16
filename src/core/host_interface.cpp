#include "host_interface.h"
#include "YBaseLib/ByteStream.h"
#include "YBaseLib/Log.h"
#include "bios.h"
#include "common/audio_stream.h"
#include "host_display.h"
#include "system.h"
#include <filesystem>
Log_SetChannel(HostInterface);

HostInterface::HostInterface()
{
  m_settings.SetDefaults();
}

HostInterface::~HostInterface() = default;

bool HostInterface::CreateSystem()
{
  m_system = System::Create(this);

  // Pull in any invalid settings which have been reset.
  m_settings = m_system->GetSettings();
  m_paused = true;
  UpdateAudioVisualSync();
  return true;
}

bool HostInterface::BootSystem(const char* filename, const char* state_filename)
{
  if (!m_system->Boot(filename))
    return false;

  m_paused = m_settings.start_paused;
  ConnectControllers();
  UpdateAudioVisualSync();
  return true;
}

void HostInterface::DestroySystem()
{
  m_system.reset();
  m_paused = false;
  UpdateAudioVisualSync();
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

void HostInterface::UpdateAudioVisualSync()
{
  const bool speed_limiter_enabled = m_settings.speed_limiter_enabled && !m_speed_limiter_temp_disabled;
  const bool audio_sync_enabled = speed_limiter_enabled;
  const bool vsync_enabled = !m_system || m_paused || (speed_limiter_enabled && m_settings.gpu_vsync);
  Log_InfoPrintf("Syncing to %s%s", audio_sync_enabled ? "audio" : "",
                 (speed_limiter_enabled && vsync_enabled) ? " and video" : "");

  m_audio_stream->SetSync(false);
  m_display->SetVSync(vsync_enabled);
}
