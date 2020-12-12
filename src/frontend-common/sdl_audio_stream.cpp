#include "sdl_audio_stream.h"
#include "common/assert.h"
#include "common/log.h"
#include "sdl_initializer.h"
#include <SDL.h>
Log_SetChannel(SDLAudioStream);

SDLAudioStream::SDLAudioStream() = default;

SDLAudioStream::~SDLAudioStream()
{
  if (IsOpen())
    SDLAudioStream::CloseDevice();
}

std::unique_ptr<SDLAudioStream> SDLAudioStream::Create()
{
  return std::make_unique<SDLAudioStream>();
}

bool SDLAudioStream::OpenDevice()
{
  DebugAssert(!IsOpen());

  FrontendCommon::EnsureSDLInitialized();

  if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
  {
    Log_ErrorPrintf("SDL_InitSubSystem(SDL_INIT_AUDIO) failed");
    return false;
  }

  SDL_AudioSpec spec = {};
  spec.freq = m_output_sample_rate;
  spec.channels = static_cast<Uint8>(m_channels);
  spec.format = AUDIO_S16;
  spec.samples = static_cast<Uint16>(m_buffer_size);
  spec.callback = AudioCallback;
  spec.userdata = static_cast<void*>(this);

  SDL_AudioSpec obtained_spec = {};

#ifdef SDL_AUDIO_ALLOW_SAMPLES_CHANGE
  const u32 allowed_change_flags = SDL_AUDIO_ALLOW_SAMPLES_CHANGE;
#else
  const u32 allowed_change_flags = 0;
#endif

  m_device_id = SDL_OpenAudioDevice(nullptr, 0, &spec, &obtained_spec, allowed_change_flags);
  if (m_device_id == 0)
  {
    Log_ErrorPrintf("SDL_OpenAudioDevice() failed: %s", SDL_GetError());
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return false;
  }

  if (obtained_spec.samples > spec.samples)
  {
    Log_WarningPrintf("Requested buffer size %u, got buffer size %u. Adjusting to compensate.", spec.samples,
                      obtained_spec.samples);

    if (!SetBufferSize(obtained_spec.samples))
    {
      Log_ErrorPrintf("Failed to set new buffer size of %u", obtained_spec.samples);
      CloseDevice();
      return false;
    }
  }

  return true;
}

void SDLAudioStream::PauseDevice(bool paused)
{
  SDL_PauseAudioDevice(m_device_id, paused ? 1 : 0);
}

void SDLAudioStream::CloseDevice()
{
  SDL_CloseAudioDevice(m_device_id);
  SDL_QuitSubSystem(SDL_INIT_AUDIO);
  m_device_id = 0;
}

void SDLAudioStream::AudioCallback(void* userdata, uint8_t* stream, int len)
{
  SDLAudioStream* const this_ptr = static_cast<SDLAudioStream*>(userdata);
  const u32 num_frames = len / sizeof(SampleType) / this_ptr->m_channels;

  this_ptr->ReadFrames(reinterpret_cast<SampleType*>(stream), num_frames, true);
}

void SDLAudioStream::FramesAvailable() {}
