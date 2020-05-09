#pragma once
#include "types.h"
#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

// Uses signed 16-bits samples.

class AudioStream
{
public:
  using SampleType = s16;

  enum
  {
    DefaultOutputSampleRate = 44100,
    DefaultBufferSize = 2048,
    DefaultBufferCount = 3,
  };

  AudioStream();
  virtual ~AudioStream();

  u32 GetOutputSampleRate() const { return m_output_sample_rate; }
  u32 GetChannels() const { return m_channels; }
  u32 GetBufferSize() const { return m_buffer_size; }
  u32 GetBufferCount() const { return static_cast<u32>(m_buffers.size()); }
  s32 GetOutputVolume() const { return m_output_volume; }
  bool IsSyncing() const { return m_sync; }

  bool Reconfigure(u32 output_sample_rate = DefaultOutputSampleRate, u32 channels = 1,
                   u32 buffer_size = DefaultBufferSize, u32 buffer_count = DefaultBufferCount);
  void SetSync(bool enable) { m_sync = enable; }
  void SetOutputVolume(s32 volume);

  void PauseOutput(bool paused);
  void EmptyBuffers();

  void Shutdown();

  void BeginWrite(SampleType** buffer_ptr, u32* num_frames);
  void WriteFrames(const SampleType* frames, u32 num_frames);
  void EndWrite(u32 num_frames);

  static std::unique_ptr<AudioStream> CreateNullAudioStream();

  static std::unique_ptr<AudioStream> CreateCubebAudioStream();

  // Latency computation - returns values in seconds
  static float GetMinLatency(u32 sample_rate, u32 buffer_size, u32 buffer_count);
  static float GetMaxLatency(u32 sample_rate, u32 buffer_size, u32 buffer_count);

protected:
  virtual bool OpenDevice() = 0;
  virtual void PauseDevice(bool paused) = 0;
  virtual void CloseDevice() = 0;
  virtual void BufferAvailable() = 0;

  ALWAYS_INLINE static SampleType ApplyVolume(SampleType sample, s32 volume)
  {
    return s16((s32(sample) * volume) / 100);
  }

  bool IsDeviceOpen() const { return (m_output_sample_rate > 0); }

  u32 GetSamplesAvailable() const;
  u32 ReadSamples(SampleType* samples, u32 num_samples);

  void DropBuffer();

  u32 m_output_sample_rate = 0;
  u32 m_channels = 0;
  u32 m_buffer_size = 0;

private:
  struct Buffer
  {
    std::vector<SampleType> data;
    u32 write_position;
    u32 read_position;
  };

  void AllocateBuffers(u32 buffer_count);
  void EnsureBuffer();

  std::vector<Buffer> m_buffers;
  mutable std::mutex m_buffer_mutex;

  // For input.
  u32 m_first_free_buffer = 0;
  u32 m_num_free_buffers = 0;

  // For output.
  u32 m_num_available_buffers = 0;
  u32 m_first_available_buffer = 0;

  // TODO: Switch to semaphore
  std::condition_variable m_buffer_available_cv;

  // volume, 0-100
  s32 m_output_volume = 100;

  bool m_output_paused = true;
  bool m_sync = true;
};