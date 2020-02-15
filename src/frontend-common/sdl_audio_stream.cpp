#include "sdl_audio_stream.h"
#include "sdl_initializer.h"
#include "common/assert.h"
#include "common/log.h"
#include <SDL.h>
Log_SetChannel(SDLAudioStream);

SDLAudioStream::SDLAudioStream() = default;

SDLAudioStream::~SDLAudioStream()
{
  if (m_is_open)
    SDLAudioStream::CloseDevice();
}

std::unique_ptr<SDLAudioStream> SDLAudioStream::Create()
{
  return std::make_unique<SDLAudioStream>();
}

bool SDLAudioStream::OpenDevice()
{
  DebugAssert(!m_is_open);

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

  if (SDL_OpenAudio(&spec, nullptr) < 0)
  {
    Log_ErrorPrintf("SDL_OpenAudio failed");
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return false;
  }

  m_is_open = true;
  return true;
}

void SDLAudioStream::PauseDevice(bool paused)
{
  SDL_PauseAudio(paused ? 1 : 0);
}

void SDLAudioStream::CloseDevice()
{
  DebugAssert(m_is_open);
  SDL_CloseAudio();
  SDL_QuitSubSystem(SDL_INIT_AUDIO);
  m_is_open = false;
}

void SDLAudioStream::AudioCallback(void* userdata, uint8_t* stream, int len)
{
  SDLAudioStream* const this_ptr = static_cast<SDLAudioStream*>(userdata);
  const u32 num_samples = len / sizeof(SampleType) / this_ptr->m_channels;
  const u32 read_samples = this_ptr->ReadSamples(reinterpret_cast<SampleType*>(stream), num_samples);
  const u32 silence_samples = num_samples - read_samples;
  if (silence_samples > 0)
  {
    std::memset(reinterpret_cast<SampleType*>(stream) + (read_samples * this_ptr->m_channels), 0,
                silence_samples * this_ptr->m_channels * sizeof(SampleType));
  }
}

void SDLAudioStream::BufferAvailable() {}
