#pragma once

#include <memory>
#include <mutex>
#include <vector>

#include "YBaseLib/CircularBuffer.h"
#include "YBaseLib/Mutex.h"
#include "YBaseLib/MutexLock.h"
#include "YBaseLib/String.h"
#include "types.h"

namespace Audio {

class Channel;
class Mixer;

enum class SampleFormat
{
  Signed8,
  Unsigned8,
  Signed16,
  Unsigned16,
  Signed32,
  Float32
};

// Constants for the maximums we support.
constexpr size_t NumOutputChannels = 2;

// We buffer one second of data either way.
constexpr float InputBufferLengthInSeconds = 1.0f;
constexpr float OutputBufferLengthInSeconds = 1.0f;

// Audio render frequency. We render the elapsed simulated time worth of audio at this interval.
// Currently it is every 50ms, or 20hz. For audio channels, it's recommended to render at twice this
// frequency in order to ensure that there is always data ready. This could also be mitigated by buffering.
constexpr u32 MixFrequency = 20;
constexpr SimulationTime MixInterval = SimulationTime(1000000000) / SimulationTime(MixFrequency);

// We buffer 10ms of input before rendering any samples, that way we don't get buffer underruns.
constexpr SimulationTime ChannelDelayTimeInSimTime = SimulationTime(10000000);

// Output format type.
constexpr SampleFormat OutputFormat = SampleFormat::Float32;
using OutputFormatType = float;

// Get the number of bytes for each element of a sample format.
size_t GetBytesPerSample(SampleFormat format);

// Base audio class, handles mixing/resampling
class Mixer
{
public:
  Mixer(float output_sample_rate);
  virtual ~Mixer();

  // Disable all outputs.
  // This prevents any samples being written to the device, but still consumes samples.
  bool IsMuted() const { return m_muted; }
  void SetMuted(bool muted) { m_muted = muted; }

  // Adds a channel to the audio mixer.
  // This pointer is owned by the audio class.
  Channel* CreateChannel(const char* name, float sample_rate, SampleFormat format, size_t channels);

  // Drops a channel from the audio mixer.
  void RemoveChannel(Channel* channel);

  // Looks up channel by name. Shouldn't really be needed.
  Channel* GetChannelByName(const char* name);

  // Clears all buffers. Use when changing speed limiter state, or loading state.
  void ClearBuffers();

protected:
  void CheckRenderBufferSize(size_t num_samples);

  float m_output_sample_rate;
  float m_output_sample_carry = 0.0f;
  bool m_muted = false;

  // Input channels.
  std::vector<std::unique_ptr<Channel>> m_channels;

  // Output buffer.
  std::vector<OutputFormatType> m_render_buffer;
  std::unique_ptr<CircularBuffer> m_output_buffer;
};

class AudioBuffer
{
public:
  AudioBuffer(size_t size);

  size_t GetBufferUsed() const;
  size_t GetContiguousBufferSpace() const;

  void Clear();

  bool Read(void* dst, size_t len);

  bool GetWritePointer(void** ptr, size_t* len);

  void MoveWritePointer(size_t len);

  bool GetReadPointer(const void** ppReadPointer, size_t* pByteCount) const;

  void MoveReadPointer(size_t byteCount);

private:
  std::vector<byte> m_buffer;
  size_t m_used = 0;
};

// A channel, or source of audio for the mixer.
class Channel
{
public:
  Channel(const char* name, float output_sample_rate, float input_sample_rate, SampleFormat format, size_t channels);
  ~Channel();

  const String& GetName() const { return m_name; }
  float GetSampleRate() const { return m_input_sample_rate; }
  SampleFormat GetFormat() const { return m_format; }
  size_t GetChannels() const { return m_channels; }

  // When the channel is disabled, adding samples will have no effect, and it won't affect the output.
  bool IsEnabled() const { return m_enabled; }
  void SetEnabled(bool enabled) { m_enabled = enabled; }

  // This sample_count is the number of samples per channel, so two-channel will be half of the total values.
  size_t GetFreeInputSamples();
  void* ReserveInputSamples(size_t sample_count);
  void CommitInputSamples(size_t sample_count);

  // Resamples at most num_output_samples, the actual number can be lower if there isn't enough input data.
  bool ResampleInput(size_t num_output_samples);

  // Render n output samples. If not enough input data is in the buffer, set to zero.
  void ReadSamples(float* destination, size_t num_samples);

  // Changes the frequency of the input data. Flushes the resample buffer.
  void ChangeSampleRate(float new_sample_rate);

  // Clears the buffer. Use when loading state or changing speed limiter.
  void ClearBuffer();

private:
  void InternalClearBuffer();

  String m_name;
  float m_input_sample_rate;
  float m_output_sample_rate;
  SampleFormat m_format;
  size_t m_channels;
  bool m_enabled;

  Mutex m_lock;
  // std::unique_ptr<CircularBuffer> m_input_buffer;
  size_t m_input_sample_size;
  size_t m_input_frame_size;
  size_t m_output_frame_size;
  AudioBuffer m_input_buffer;
  AudioBuffer m_output_buffer;
  std::vector<float> m_resample_buffer;
  double m_resample_ratio;
  void* m_resampler_state;
};

// Null audio sink/mixer
class NullMixer : public Mixer
{
public:
  NullMixer();
  virtual ~NullMixer();

  static std::unique_ptr<Mixer> Create();

protected:
  void RenderSamples(size_t output_samples);
};

} // namespace Audio
