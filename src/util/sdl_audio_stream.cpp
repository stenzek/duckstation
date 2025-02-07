// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "audio_stream.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/log.h"

#include <SDL3/SDL.h>

LOG_CHANNEL(SDL);

namespace {

class SDLAudioStream final : public AudioStream
{
public:
  SDLAudioStream(u32 sample_rate, const AudioStreamParameters& parameters);
  ~SDLAudioStream();

  void SetPaused(bool paused) override;

  bool OpenDevice(Error* error);
  void CloseDevice();

protected:
  static void AudioCallback(void* userdata, SDL_AudioStream* stream, int additional_amount, int total_amount);

  SDL_AudioStream* m_sdl_stream = nullptr;
};
} // namespace

static bool InitializeSDLAudio(Error* error)
{
  static bool initialized = false;
  if (initialized)
    return true;

  // May as well keep it alive until the process exits.
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
  {
    Error::SetStringFmt(error, "SDL_InitSubSystem(SDL_INIT_AUDIO) failed: {}", SDL_GetError());
    return false;
  }

  std::atexit([]() { SDL_QuitSubSystem(SDL_INIT_AUDIO); });

  initialized = true;
  return true;
}

SDLAudioStream::SDLAudioStream(u32 sample_rate, const AudioStreamParameters& parameters)
  : AudioStream(sample_rate, parameters)
{
}

SDLAudioStream::~SDLAudioStream()
{
  SDLAudioStream::CloseDevice();
}

std::unique_ptr<AudioStream> AudioStream::CreateSDLAudioStream(u32 sample_rate, const AudioStreamParameters& parameters,
                                                               Error* error)
{
  if (!InitializeSDLAudio(error))
    return {};

  std::unique_ptr<SDLAudioStream> stream = std::make_unique<SDLAudioStream>(sample_rate, parameters);
  if (!stream->OpenDevice(error))
    stream.reset();

  return stream;
}

bool SDLAudioStream::OpenDevice(Error* error)
{
  DebugAssert(!m_sdl_stream);

  const SDL_AudioSpec spec = {
    .format = SDL_AUDIO_S16LE, .channels = NUM_CHANNELS, .freq = static_cast<int>(m_sample_rate)};

  m_sdl_stream =
    SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, AudioCallback, static_cast<void*>(this));
  if (!m_sdl_stream)
  {
    Error::SetStringFmt(error, "SDL_OpenAudioDeviceStream() failed: {}", SDL_GetError());
    return false;
  }

  BaseInitialize();
  SDL_ResumeAudioDevice(SDL_GetAudioStreamDevice(m_sdl_stream));

  return true;
}

void SDLAudioStream::SetPaused(bool paused)
{
  if (m_paused == paused)
    return;

  paused ? SDL_PauseAudioStreamDevice(m_sdl_stream) : SDL_ResumeAudioStreamDevice(m_sdl_stream);
  m_paused = paused;
}

void SDLAudioStream::CloseDevice()
{
  if (m_sdl_stream)
  {
    SDL_DestroyAudioStream(m_sdl_stream);
    m_sdl_stream = nullptr;
  }
}

void SDLAudioStream::AudioCallback(void* userdata, SDL_AudioStream* stream, int additional_amount, int total_amount)
{
  if (additional_amount == 0)
    return;

  u8* data = SDL_stack_alloc(u8, additional_amount);
  if (data)
  {
    SDLAudioStream* const this_ptr = static_cast<SDLAudioStream*>(userdata);
    const u32 num_frames = static_cast<u32>(additional_amount) / (sizeof(SampleType) * NUM_CHANNELS);
    this_ptr->ReadFrames(reinterpret_cast<SampleType*>(data), num_frames);
    SDL_PutAudioStreamData(stream, data, additional_amount);
    SDL_stack_free(data);
  }
}
