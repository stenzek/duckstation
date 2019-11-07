#include "host_interface.h"
#include "YBaseLib/ByteStream.h"
#include "YBaseLib/Log.h"
#include "common/audio_stream.h"
#include "host_display.h"
#include "system.h"
Log_SetChannel(HostInterface);

HostInterface::HostInterface()
{
  m_settings.SetDefaults();
}

HostInterface::~HostInterface() = default;

bool HostInterface::InitializeSystem(const char* filename, const char* exp1_filename)
{
  m_system = std::make_unique<System>(this);
  if (!m_system->Initialize())
  {
    m_system.reset();
    return false;
  }

  m_system->Reset();

  if (filename)
  {
    const StaticString filename_str(filename);
    if (filename_str.EndsWith(".psexe", false) || filename_str.EndsWith(".exe", false))
    {
      Log_InfoPrintf("Sideloading EXE file '%s'", filename);
      if (!m_system->LoadEXE(filename))
      {
        Log_ErrorPrintf("Failed to load EXE file '%s'", filename);
        return false;
      }
    }
    else
    {
      Log_InfoPrintf("Inserting CDROM from image file '%s'", filename);
      if (!m_system->InsertMedia(filename))
      {
        Log_ErrorPrintf("Failed to insert media '%s'", filename);
        return false;
      }
    }
  }

  if (exp1_filename)
    m_system->SetExpansionROM(exp1_filename);

  // Resume execution.
  m_settings = m_system->GetSettings();
  return true;
}

void HostInterface::ShutdownSystem()
{
  m_system.reset();
  m_paused = false;
  UpdateAudioVisualSync();
}

bool HostInterface::LoadState(const char* filename)
{
  ByteStream* stream;
  if (!ByteStream_OpenFileStream(filename, BYTESTREAM_OPEN_READ | BYTESTREAM_OPEN_STREAMED, &stream))
    return false;

  ReportMessage(SmallString::FromFormat("Loading state from %s...", filename));

  const bool result = m_system->LoadState(stream);
  if (!result)
  {
    ReportMessage(SmallString::FromFormat("Loading state from %s failed. Resetting.", filename));
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
    ReportMessage(SmallString::FromFormat("Saving state to %s failed.", filename));
    stream->Discard();
  }
  else
  {
    ReportMessage(SmallString::FromFormat("State saved to %s.", filename));
    stream->Commit();
  }

  stream->Release();
  return result;
}

void HostInterface::UpdateAudioVisualSync()
{
  const bool speed_limiter_enabled = m_settings.speed_limiter_enabled && !m_speed_limiter_temp_disabled;
  const bool audio_sync_enabled = speed_limiter_enabled;
  const bool vsync_enabled = !m_system || (speed_limiter_enabled && m_settings.gpu_vsync);
  Log_InfoPrintf("Syncing to %s%s", audio_sync_enabled ? "audio" : "",
                 (speed_limiter_enabled && vsync_enabled) ? " and video" : "");

  m_audio_stream->SetSync(false);
  m_display->SetVSync(vsync_enabled);
}
