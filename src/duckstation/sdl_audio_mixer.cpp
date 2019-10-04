#include "sdl_audio_mixer.h"
#include "YBaseLib/Timer.h"
#include <SDL_audio.h>

using namespace Audio;

inline SDL_AudioFormat GetSDLAudioFormat(SampleFormat format)
{
  switch (format)
  {
    case SampleFormat::Signed8:
      return AUDIO_S8;

    case SampleFormat::Unsigned8:
      return AUDIO_U8;

    case SampleFormat::Signed16:
      return AUDIO_S16SYS;

    case SampleFormat::Unsigned16:
      return AUDIO_U16SYS;

    case SampleFormat::Signed32:
      return AUDIO_S32SYS;

    case SampleFormat::Float32:
      return AUDIO_F32;
  }

  Panic("Unhandled format");
  return AUDIO_U8;
}

SDLAudioMixer::SDLAudioMixer(SDL_AudioDeviceID device_id, float output_sample_rate)
  : Mixer(output_sample_rate), m_device_id(device_id)
{
}

SDLAudioMixer::~SDLAudioMixer()
{
  SDL_CloseAudioDevice(m_device_id);
}

std::unique_ptr<SDLAudioMixer> SDLAudioMixer::Create()
{
  auto mixer = std::make_unique<SDLAudioMixer>(0, 44100.0f);
  SDL_AudioSpec spec = {44100,          AUDIO_F32,  static_cast<Uint8>(NumOutputChannels), 0, 4096, 0, 0,
                        RenderCallback, mixer.get()};
  SDL_AudioSpec obtained_spec;
  SDL_AudioDeviceID device_id = SDL_OpenAudioDevice(nullptr, 0, &spec, &obtained_spec, 0);
  if (device_id == 0)
    return nullptr;

  mixer->m_device_id = device_id;

  SDL_PauseAudioDevice(device_id, SDL_FALSE);

  return mixer;
}

void SDLAudioMixer::RenderSamples(Audio::OutputFormatType* buf, size_t num_samples)
{
  CheckRenderBufferSize(num_samples);
  std::fill_n(buf, num_samples * NumOutputChannels, 0.0f);

  for (auto& channel : m_channels)
  {
    channel->ReadSamples(m_render_buffer.data(), num_samples);

    // Don't bother mixing it if we're muted.
    if (m_muted)
      continue;

    // If the format is the same, we can just copy it as-is..
    if (channel->GetChannels() == 1)
    {
      // Mono -> stereo
      for (ssize_t idx = ssize_t(num_samples) - 1; idx >= 0; idx--)
      {
        float sample = m_render_buffer[idx];
        m_render_buffer[idx * 2 + 0] = sample;
        m_render_buffer[idx * 2 + 1] = sample;
      }
    }
    else if (channel->GetChannels() != NumOutputChannels)
    {
      SDL_AudioCVT cvt;
      int err = SDL_BuildAudioCVT(&cvt, AUDIO_F32, Truncate8(channel->GetChannels()), int(m_output_sample_rate),
                                  AUDIO_F32, Truncate8(NumOutputChannels), int(m_output_sample_rate));
      if (err != 1)
        Panic("Failed to set up audio conversion");

      cvt.len = int(channel->GetChannels() * sizeof(float));
      cvt.buf = reinterpret_cast<Uint8*>(m_render_buffer.data());
      err = SDL_ConvertAudio(&cvt);
      if (err != 0)
        Panic("Failed to convert audio");
    }

    // Mix channels together.
    const Audio::OutputFormatType* mix_src = reinterpret_cast<const Audio::OutputFormatType*>(m_render_buffer.data());
    Audio::OutputFormatType* mix_dst = buf;
    for (size_t i = 0; i < num_samples * NumOutputChannels; i++)
    {
      // TODO: Saturation/clamping here
      *(mix_dst++) += *(mix_src++);
    }
  }

#if 0
  static FILE* fp = nullptr;
  if (!fp)
    fp = fopen("D:\\mixed.raw", "wb");
  if (fp)
  {
    fwrite(buf, sizeof(float), num_samples * NumOutputChannels, fp);
    fflush(fp);
  }
#endif
}

void SDLAudioMixer::RenderCallback(void* userdata, Uint8* stream, int len)
{
  SDLAudioMixer* mixer = static_cast<SDLAudioMixer*>(userdata);
  Audio::OutputFormatType* buf = reinterpret_cast<Audio::OutputFormatType*>(stream);
  size_t num_samples = size_t(len) / NumOutputChannels / sizeof(Audio::OutputFormatType);
  if (num_samples > 0)
    mixer->RenderSamples(buf, num_samples);
}
