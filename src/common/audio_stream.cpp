#include "audio_stream.h"
#include "assert.h"
#include "log.h"
#include <algorithm>
#include <cstring>
Log_SetChannel(AudioStream);

AudioStream::AudioStream() = default;

AudioStream::~AudioStream() = default;

bool AudioStream::Reconfigure(u32 output_sample_rate /*= DefaultOutputSampleRate*/, u32 channels /*= 1*/,
                              u32 buffer_size /*= DefaultBufferSize*/)
{
  if (IsDeviceOpen())
    CloseDevice();

  m_output_sample_rate = output_sample_rate;
  m_channels = channels;
  m_buffer_size = buffer_size;
  m_output_paused = true;

  if (!SetBufferSize(buffer_size))
    return false;

  if (!OpenDevice())
  {
    EmptyBuffers();
    m_buffer_size = 0;
    m_output_sample_rate = 0;
    m_channels = 0;
    return false;
  }

  return true;
}

void AudioStream::SetOutputVolume(u32 volume)
{
  std::unique_lock<std::mutex> lock(m_buffer_mutex);
  m_output_volume = volume;
}

void AudioStream::PauseOutput(bool paused)
{
  if (m_output_paused == paused)
    return;

  PauseDevice(paused);
  m_output_paused = paused;

  // Empty buffers on pause.
  if (paused)
    EmptyBuffers();
}

void AudioStream::Shutdown()
{
  if (!IsDeviceOpen())
    return;

  CloseDevice();
  EmptyBuffers();
  m_buffer_size = 0;
  m_output_sample_rate = 0;
  m_channels = 0;
  m_output_paused = true;
}

void AudioStream::BeginWrite(SampleType** buffer_ptr, u32* num_frames)
{
  m_buffer_mutex.lock();

  EnsureBuffer(*num_frames * m_channels);

  *buffer_ptr = m_buffer.GetWritePointer();
  *num_frames = m_buffer.GetContiguousSpace() / m_channels;
}

void AudioStream::WriteFrames(const SampleType* frames, u32 num_frames)
{
  const u32 num_samples = num_frames * m_channels;
  std::unique_lock<std::mutex> lock(m_buffer_mutex);

  EnsureBuffer(num_samples);
  m_buffer.PushRange(frames, num_samples);
  FramesAvailable();
}

void AudioStream::EndWrite(u32 num_frames)
{
  m_buffer.AdvanceTail(num_frames * m_channels);
  FramesAvailable();

  m_buffer_mutex.unlock();
}

float AudioStream::GetMaxLatency(u32 sample_rate, u32 buffer_size)
{
  return (static_cast<float>(buffer_size) / static_cast<float>(sample_rate));
}

bool AudioStream::SetBufferSize(u32 buffer_size)
{
  const u32 buffer_size_in_samples = buffer_size * m_channels;
  const u32 max_samples = buffer_size_in_samples * 2u;
  if (max_samples > m_buffer.GetCapacity())
    return false;

  m_buffer_size = buffer_size;
  m_max_samples = max_samples;
  return true;
}

u32 AudioStream::GetSamplesAvailable() const
{
  // TODO: Use atomic loads
  u32 available_samples;
  {
    std::unique_lock<std::mutex> lock(m_buffer_mutex);
    available_samples = m_buffer.GetSize();
  }

  return available_samples / m_channels;
}

u32 AudioStream::GetSamplesAvailableLocked() const
{
  return m_buffer.GetSize() / m_channels;
}

void AudioStream::ReadFrames(SampleType* samples, u32 num_frames, bool apply_volume)
{
  const u32 total_samples = num_frames * m_channels;
  u32 samples_copied = 0;
  {
    std::unique_lock<std::mutex> lock(m_buffer_mutex);
    samples_copied = std::min(m_buffer.GetSize(), total_samples);
    if (samples_copied > 0)
      m_buffer.PopRange(samples, samples_copied);

    m_buffer_draining_cv.notify_one();
  }

  if (samples_copied < total_samples)
  {
    if (samples_copied > 0)
    {
      m_resample_buffer.resize(samples_copied);
      std::memcpy(m_resample_buffer.data(), samples, sizeof(SampleType) * samples_copied);

      // super basic resampler - spread the input samples evenly across the output samples. will sound like ass and have
      // aliasing, but better than popping by inserting silence.
      const u32 increment =
        static_cast<u32>(65536.0f * (static_cast<float>(samples_copied / m_channels) / static_cast<float>(num_frames)));

      SampleType* out_ptr = samples;
      const SampleType* resample_ptr = m_resample_buffer.data();
      const u32 copy_stride = sizeof(SampleType) * m_channels;
      u32 resample_subpos = 0;
      for (u32 i = 0; i < num_frames; i++)
      {
        std::memcpy(out_ptr, resample_ptr, copy_stride);
        out_ptr += m_channels;

        resample_subpos += increment;
        resample_ptr += (resample_subpos >> 16) * m_channels;
        resample_subpos %= 65536u;
      }

      Log_DevPrintf("Audio buffer underflow, resampled %u frames to %u", samples_copied / m_channels, num_frames);
    }
    else
    {
      // read nothing, so zero-fill
      std::memset(samples, 0, sizeof(SampleType) * total_samples);
      Log_DevPrintf("Audio buffer underflow with no samples, added %u frames silence", num_frames);
    }
  }

  if (apply_volume && m_output_volume != FullVolume)
  {
    SampleType* current_ptr = samples;
    const SampleType* end_ptr = samples + (num_frames * m_channels);
    while (current_ptr != end_ptr)
    {
      *current_ptr = ApplyVolume(*current_ptr, m_output_volume);
      current_ptr++;
    }
  }
}

void AudioStream::EnsureBuffer(u32 size)
{
  if (GetBufferSpace() >= size)
    return;

  if (m_sync)
  {
    std::unique_lock<std::mutex> lock(m_buffer_mutex, std::adopt_lock);
    m_buffer_draining_cv.wait(lock, [this, size]() { return GetBufferSpace() >= size; });
    lock.release();
  }
  else
  {
    m_buffer.Remove(size);
  }
}

void AudioStream::DropFrames(u32 count)
{
  m_buffer.Remove(count);
}

void AudioStream::EmptyBuffers()
{
  std::unique_lock<std::mutex> lock(m_buffer_mutex);
  m_buffer.Clear();
}
