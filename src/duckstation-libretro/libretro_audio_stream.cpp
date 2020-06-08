#include "libretro_audio_stream.h"
#include "libretro_host_interface.h"

LibretroAudioStream::LibretroAudioStream() = default;

LibretroAudioStream::~LibretroAudioStream() = default;

bool LibretroAudioStream::OpenDevice()
{
  m_output_buffer.resize(m_buffer_size * m_channels);
  return true;
}

void LibretroAudioStream::PauseDevice(bool paused) {}

void LibretroAudioStream::CloseDevice() {}

void LibretroAudioStream::FramesAvailable()
{
  const u32 num_frames = GetSamplesAvailable();
  ReadFrames(m_output_buffer.data(), num_frames, false);
  g_retro_audio_sample_batch_callback(m_output_buffer.data(), num_frames);
}
