#pragma once
#include "common/audio.h"
#include <SDL_audio.h>

class SDLAudioMixer : public Audio::Mixer
{
public:
  SDLAudioMixer(SDL_AudioDeviceID device_id, float output_sample_rate);
  virtual ~SDLAudioMixer();

  static std::unique_ptr<SDLAudioMixer> Create();

protected:
  void RenderSamples(Audio::OutputFormatType* buf, size_t num_samples);
  static void RenderCallback(void* userdata, Uint8* stream, int len);

private:
  SDL_AudioDeviceID m_device_id;
};
