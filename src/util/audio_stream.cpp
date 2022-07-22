#include "audio_stream.h"
#include "assert.h"
#include "common/log.h"
#include "samplerate.h"
#include <algorithm>
#include <cstring>
Log_SetChannel(AudioStream);

AudioStream::AudioStream() = default;

AudioStream::~AudioStream()
{
  DestroyResampler();
}

bool AudioStream::Reconfigure(u32 input_sample_rate /* = DefaultInputSampleRate */,
                              u32 output_sample_rate /* = DefaultOutputSampleRate */, u32 channels /* = 1 */,
                              u32 buffer_size /* = DefaultBufferSize */)
{
  std::unique_lock<std::mutex> buffer_lock(m_buffer_mutex);
  std::unique_lock<std::mutex> resampler_Lock(m_resampler_mutex);

  DestroyResampler();
  if (IsDeviceOpen())
    CloseDevice();

  m_output_sample_rate = output_sample_rate;
  m_channels = channels;
  m_buffer_size = buffer_size;
  m_buffer_filling.store(m_wait_for_buffer_fill);
  m_output_paused = true;

  if (!SetBufferSize(buffer_size))
    return false;

  if (!OpenDevice())
  {
    LockedEmptyBuffers();
    m_buffer_size = 0;
    m_output_sample_rate = 0;
    m_channels = 0;
    return false;
  }

  CreateResampler();
  InternalSetInputSampleRate(input_sample_rate);

  return true;
}

void AudioStream::SetInputSampleRate(u32 sample_rate)
{
  std::unique_lock<std::mutex> buffer_lock(m_buffer_mutex);
  std::unique_lock<std::mutex> resampler_lock(m_resampler_mutex);

  InternalSetInputSampleRate(sample_rate);
}

void AudioStream::SetWaitForBufferFill(bool enabled)
{
  std::unique_lock<std::mutex> buffer_lock(m_buffer_mutex);
  m_wait_for_buffer_fill = enabled;
  if (enabled && m_buffer.IsEmpty())
    m_buffer_filling.store(true);
}

void AudioStream::InternalSetInputSampleRate(u32 sample_rate)
{
  if (m_input_sample_rate == sample_rate)
    return;

  m_input_sample_rate = sample_rate;
  m_resampler_ratio = static_cast<double>(m_output_sample_rate) / static_cast<double>(sample_rate);
  src_set_ratio(static_cast<SRC_STATE*>(m_resampler_state), m_resampler_ratio);
  ResetResampler();
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

  const u32 requested_frames = std::min(*num_frames, m_buffer_size);
  EnsureBuffer(requested_frames * m_channels);

  *buffer_ptr = m_buffer.GetWritePointer();
  *num_frames = std::min(m_buffer_size, m_buffer.GetContiguousSpace() / m_channels);
}

void AudioStream::WriteFrames(const SampleType* frames, u32 num_frames)
{
  Assert(num_frames <= m_buffer_size);
  const u32 num_samples = num_frames * m_channels;
  {
    std::unique_lock<std::mutex> lock(m_buffer_mutex);
    EnsureBuffer(num_samples);
    m_buffer.PushRange(frames, num_samples);
  }

  FramesAvailable();
}

void AudioStream::EndWrite(u32 num_frames)
{
  m_buffer.AdvanceTail(num_frames * m_channels);
  if (m_buffer_filling.load())
  {
    if ((m_buffer.GetSize() / m_channels) >= m_buffer_size)
      m_buffer_filling.store(false);
  }
  m_buffer_mutex.unlock();
  FramesAvailable();
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
  std::unique_lock<std::mutex> buffer_lock(m_buffer_mutex);
  if (!m_buffer_filling.load())
  {
    if (m_input_sample_rate == m_output_sample_rate)
    {
      samples_copied = std::min(m_buffer.GetSize(), total_samples);
      if (samples_copied > 0)
        m_buffer.PopRange(samples, samples_copied);

      ReleaseBufferLock(std::move(buffer_lock));
    }
    else
    {
      if (m_resampled_buffer.GetSize() < total_samples)
        ResampleInput(std::move(buffer_lock));
      else
        ReleaseBufferLock(std::move(buffer_lock));

      samples_copied = std::min(m_resampled_buffer.GetSize(), total_samples);
      if (samples_copied > 0)
        m_resampled_buffer.PopRange(samples, samples_copied);
    }
  }
  else
  {
    ReleaseBufferLock(std::move(buffer_lock));
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

      Log_VerbosePrintf("Audio buffer underflow, resampled %u frames to %u", samples_copied / m_channels, num_frames);
      m_underflow_flag.store(true);
    }
    else
    {
      // read nothing, so zero-fill
      std::memset(samples, 0, sizeof(SampleType) * total_samples);
      Log_VerbosePrintf("Audio buffer underflow with no samples, added %u frames silence", num_frames);
      m_underflow_flag.store(true);
    }

    m_buffer_filling.store(m_wait_for_buffer_fill);
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
  DebugAssert(size <= (m_buffer_size * m_channels));
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
  std::unique_lock<std::mutex> lock(m_buffer_mutex);
  m_buffer.Remove(count);
}

void AudioStream::EmptyBuffers()
{
  std::unique_lock<std::mutex> lock(m_buffer_mutex);
  std::unique_lock<std::mutex> resampler_lock(m_resampler_mutex);
  LockedEmptyBuffers();
}

void AudioStream::LockedEmptyBuffers()
{
  m_buffer.Clear();
  m_underflow_flag.store(false);
  m_buffer_filling.store(m_wait_for_buffer_fill);
  ResetResampler();
}

void AudioStream::CreateResampler()
{
  m_resampler_state = src_new(SRC_SINC_MEDIUM_QUALITY, static_cast<int>(m_channels), nullptr);
  if (!m_resampler_state)
    Panic("Failed to allocate resampler");
}

void AudioStream::DestroyResampler()
{
  if (m_resampler_state)
  {
    src_delete(static_cast<SRC_STATE*>(m_resampler_state));
    m_resampler_state = nullptr;
  }
}

void AudioStream::ResetResampler()
{
  m_resampled_buffer.Clear();
  m_resample_in_buffer.clear();
  m_resample_out_buffer.clear();
  src_reset(static_cast<SRC_STATE*>(m_resampler_state));
}

void AudioStream::ResampleInput(std::unique_lock<std::mutex> buffer_lock)
{
  std::unique_lock<std::mutex> resampler_lock(m_resampler_mutex);

  const u32 input_space_from_output = (m_resampled_buffer.GetSpace() * m_output_sample_rate) / m_input_sample_rate;
  u32 remaining = std::min(m_buffer.GetSize(), input_space_from_output);
  if (m_resample_in_buffer.size() < remaining)
  {
    remaining -= static_cast<u32>(m_resample_in_buffer.size());
    m_resample_in_buffer.reserve(m_resample_in_buffer.size() + remaining);
    while (remaining > 0)
    {
      const u32 read_len = std::min(m_buffer.GetContiguousSize(), remaining);
      const size_t old_pos = m_resample_in_buffer.size();
      m_resample_in_buffer.resize(m_resample_in_buffer.size() + read_len);
      src_short_to_float_array(m_buffer.GetReadPointer(), m_resample_in_buffer.data() + old_pos,
                               static_cast<int>(read_len));
      m_buffer.Remove(read_len);
      remaining -= read_len;
    }
  }

  ReleaseBufferLock(std::move(buffer_lock));

  const u32 potential_output_size =
    (static_cast<u32>(m_resample_in_buffer.size()) * m_input_sample_rate) / m_output_sample_rate;
  const u32 output_size = std::min(potential_output_size, m_resampled_buffer.GetSpace());
  m_resample_out_buffer.resize(output_size);

  SRC_DATA sd = {};
  sd.data_in = m_resample_in_buffer.data();
  sd.data_out = m_resample_out_buffer.data();
  sd.input_frames = static_cast<u32>(m_resample_in_buffer.size()) / m_channels;
  sd.output_frames = output_size / m_channels;
  sd.src_ratio = m_resampler_ratio;

  const int error = src_process(static_cast<SRC_STATE*>(m_resampler_state), &sd);
  if (error)
  {
    Log_ErrorPrintf("Resampler error %d", error);
    m_resample_in_buffer.clear();
    m_resample_out_buffer.clear();
    return;
  }

  m_resample_in_buffer.erase(m_resample_in_buffer.begin(),
                             m_resample_in_buffer.begin() + (static_cast<u32>(sd.input_frames_used) * m_channels));

  const float* write_ptr = m_resample_out_buffer.data();
  remaining = static_cast<u32>(sd.output_frames_gen) * m_channels;
  while (remaining > 0)
  {
    const u32 samples_to_write = std::min(m_resampled_buffer.GetContiguousSpace(), remaining);
    src_float_to_short_array(write_ptr, m_resampled_buffer.GetWritePointer(), static_cast<int>(samples_to_write));
    m_resampled_buffer.AdvanceTail(samples_to_write);
    write_ptr += samples_to_write;
    remaining -= samples_to_write;
  }
  m_resample_out_buffer.erase(m_resample_out_buffer.begin(),
                              m_resample_out_buffer.begin() + (static_cast<u32>(sd.output_frames_gen) * m_channels));
}