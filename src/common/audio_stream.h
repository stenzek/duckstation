#pragma once
#include "fifo_queue.h"
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

  enum : u32
  {
    DefaultOutputSampleRate = 44100,
    DefaultBufferSize = 2048,
    MaxSamples = 32768,
    FullVolume = 100
  };

  AudioStream();
  virtual ~AudioStream();

  u32 GetOutputSampleRate() const { return m_output_sample_rate; }
  u32 GetChannels() const { return m_channels; }
  u32 GetBufferSize() const { return m_buffer_size; }
  s32 GetOutputVolume() const { return m_output_volume; }
  bool IsSyncing() const { return m_sync; }

  bool Reconfigure(u32 output_sample_rate = DefaultOutputSampleRate, u32 channels = 1,
                   u32 buffer_size = DefaultBufferSize);
  void SetSync(bool enable) { m_sync = enable; }

  virtual void SetOutputVolume(u32 volume);

  void PauseOutput(bool paused);
  void EmptyBuffers();

  void Shutdown();

  void BeginWrite(SampleType** buffer_ptr, u32* num_frames);
  void WriteFrames(const SampleType* frames, u32 num_frames);
  void EndWrite(u32 num_frames);

  static std::unique_ptr<AudioStream> CreateNullAudioStream();

  // Latency computation - returns values in seconds
  static float GetMaxLatency(u32 sample_rate, u32 buffer_size);

protected:
  virtual bool OpenDevice() = 0;
  virtual void PauseDevice(bool paused) = 0;
  virtual void CloseDevice() = 0;
  virtual void FramesAvailable() = 0;

  ALWAYS_INLINE static SampleType ApplyVolume(SampleType sample, u32 volume)
  {
    return s16((s32(sample) * s32(volume)) / 100);
  }

  bool SetBufferSize(u32 buffer_size);
  bool IsDeviceOpen() const { return (m_output_sample_rate > 0); }

  u32 GetSamplesAvailable() const;
  u32 GetSamplesAvailableLocked() const;
  void ReadFrames(SampleType* samples, u32 num_frames, bool apply_volume);
  void DropFrames(u32 count);

  u32 m_output_sample_rate = 0;
  u32 m_channels = 0;
  u32 m_buffer_size = 0;

  // volume, 0-100
  u32 m_output_volume = FullVolume;

private:
  ALWAYS_INLINE u32 GetBufferSpace() const { return (m_max_samples - m_buffer.GetSize()); }
  void EnsureBuffer(u32 size);

  HeapFIFOQueue<SampleType, MaxSamples> m_buffer;
  mutable std::mutex m_buffer_mutex;
  std::condition_variable m_buffer_draining_cv;
  std::vector<SampleType> m_resample_buffer;
  u32 m_max_samples = 0;

  bool m_output_paused = true;
  bool m_sync = true;
};