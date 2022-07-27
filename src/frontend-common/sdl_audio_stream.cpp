#include "sdl_audio_stream.h"
#include "common/assert.h"
#include "common/log.h"
#include "common_host.h"
#include "sdl_initializer.h"
#include <SDL.h>
Log_SetChannel(SDLAudioStream);

SDLAudioStream::SDLAudioStream(u32 sample_rate, u32 channels, u32 buffer_ms, AudioStretchMode stretch)
  : AudioStream(sample_rate, channels, buffer_ms, stretch)
{
}

SDLAudioStream::~SDLAudioStream()
{
  if (IsOpen())
    SDLAudioStream::CloseDevice();
}

std::unique_ptr<AudioStream> CommonHost::CreateSDLAudioStream(u32 sample_rate, u32 channels, u32 buffer_ms,
                                                              u32 latency_ms, AudioStretchMode stretch)
{
  std::unique_ptr<SDLAudioStream> stream(std::make_unique<SDLAudioStream>(sample_rate, channels, buffer_ms, stretch));
  if (!stream->OpenDevice(latency_ms))
    stream.reset();
  return stream;
}

bool SDLAudioStream::OpenDevice(u32 latency_ms)
{
  DebugAssert(!IsOpen());

  FrontendCommon::EnsureSDLInitialized();

  if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
  {
    Log_ErrorPrintf("SDL_InitSubSystem(SDL_INIT_AUDIO) failed");
    return false;
  }

  SDL_AudioSpec spec = {};
  spec.freq = m_sample_rate;
  spec.channels = static_cast<Uint8>(m_channels);
  spec.format = AUDIO_S16;
  spec.samples = static_cast<Uint16>(GetBufferSizeForMS(m_sample_rate, (latency_ms == 0) ? m_buffer_ms : latency_ms));
  spec.callback = AudioCallback;
  spec.userdata = static_cast<void*>(this);

  SDL_AudioSpec obtained_spec = {};
  m_device_id = SDL_OpenAudioDevice(nullptr, 0, &spec, &obtained_spec, SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
  if (m_device_id == 0)
  {
    Log_ErrorPrintf("SDL_OpenAudioDevice() failed: %s", SDL_GetError());
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return false;
  }

  Log_DevPrintf("Requested %u frame buffer, got %u frame buffer", spec.samples, obtained_spec.samples);

  BaseInitialize();
  m_volume = 100;
  m_paused = false;
  SDL_PauseAudioDevice(m_device_id, 0);

  return true;
}

void SDLAudioStream::SetPaused(bool paused)
{
  if (m_paused == paused)
    return;

  SDL_PauseAudioDevice(m_device_id, paused ? 1 : 0);
  m_paused = paused;
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

  this_ptr->ReadFrames(reinterpret_cast<SampleType*>(stream), num_frames);
}

void SDLAudioStream::SetOutputVolume(u32 volume)
{
  if (m_volume == volume)
    return;

  Panic("Fixme");
}
