// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "audio_stream.h"

#include "common/align.h"

#include <array>
#include <atomic>
#include <memory>
#include <optional>

class Error;
class SettingsInterface;

enum class AudioStretchMode : u8
{
  Off,
  Resample,
  TimeStretch,
  Count
};

struct AudioStreamParameters
{
  AudioStretchMode stretch_mode = DEFAULT_STRETCH_MODE;
  bool output_latency_minimal = DEFAULT_OUTPUT_LATENCY_MINIMAL;
  u16 output_latency_ms = DEFAULT_OUTPUT_LATENCY_MS;
  u16 buffer_ms = DEFAULT_BUFFER_MS;

  u16 stretch_sequence_length_ms = DEFAULT_STRETCH_SEQUENCE_LENGTH;
  u16 stretch_seekwindow_ms = DEFAULT_STRETCH_SEEKWINDOW;
  u16 stretch_overlap_ms = DEFAULT_STRETCH_OVERLAP;
  bool stretch_use_quickseek = DEFAULT_STRETCH_USE_QUICKSEEK;
  bool stretch_use_aa_filter = DEFAULT_STRETCH_USE_AA_FILTER;

  static constexpr AudioStretchMode DEFAULT_STRETCH_MODE = AudioStretchMode::TimeStretch;
#ifndef __ANDROID__
  static constexpr u16 DEFAULT_BUFFER_MS = 50;
  static constexpr u16 DEFAULT_OUTPUT_LATENCY_MS = 20;
#else
  static constexpr u16 DEFAULT_BUFFER_MS = 100;
  static constexpr u16 DEFAULT_OUTPUT_LATENCY_MS = 20;
#endif
  static constexpr bool DEFAULT_OUTPUT_LATENCY_MINIMAL = false;

  static constexpr u16 DEFAULT_STRETCH_SEQUENCE_LENGTH = 30;
  static constexpr u16 DEFAULT_STRETCH_SEEKWINDOW = 20;
  static constexpr u16 DEFAULT_STRETCH_OVERLAP = 10;

  static constexpr bool DEFAULT_STRETCH_USE_QUICKSEEK = false;
  static constexpr bool DEFAULT_STRETCH_USE_AA_FILTER = false;

  void Load(const SettingsInterface& si, const char* section);
  void Save(SettingsInterface& si, const char* section) const;
  void Clear(SettingsInterface& si, const char* section);

  bool operator==(const AudioStreamParameters& rhs) const;
  bool operator!=(const AudioStreamParameters& rhs) const;
};

class CoreAudioStream final : private AudioStreamSource
{
public:
  using SampleType = AudioStreamSource::SampleType;

  static constexpr u32 NUM_CHANNELS = 2;
  static constexpr u32 CHUNK_SIZE = 64;

  CoreAudioStream();
  ~CoreAudioStream();

  static u32 GetAlignedBufferSize(u32 size);
  static u32 GetBufferSizeForMS(u32 sample_rate, u32 ms);
  static u32 GetMSForBufferSize(u32 sample_rate, u32 buffer_size);

  static const char* GetStretchModeName(AudioStretchMode mode);
  static const char* GetStretchModeDisplayName(AudioStretchMode mode);
  static std::optional<AudioStretchMode> ParseStretchMode(const char* name);

  ALWAYS_INLINE u32 GetSampleRate() const { return m_sample_rate; }
  ALWAYS_INLINE u32 GetBufferSize() const { return m_buffer_size; }
  ALWAYS_INLINE u32 GetTargetBufferSize() const { return m_target_buffer_size; }
  ALWAYS_INLINE u32 GetOutputVolume() const { return m_volume; }
  ALWAYS_INLINE float GetNominalTempo() const { return m_nominal_rate; }
  ALWAYS_INLINE bool IsPaused() const { return m_paused; }

  u32 GetBufferedFramesRelaxed() const;

  /// Creation/destruction.
  bool Initialize(AudioBackend backend, u32 sample_rate, const AudioStreamParameters& params, const char* driver_name,
                  const char* device_name, Error* error);
  void Destroy();

  /// Temporarily pauses the stream, preventing it from requesting data.
  void SetPaused(bool paused);

  void SetOutputVolume(u32 volume);

  void BeginWrite(SampleType** buffer_ptr, u32* num_frames);
  void EndWrite(u32 num_frames);

  void EmptyBuffer();

  /// Nominal rate is used for both resampling and timestretching, input samples are assumed to be this amount faster
  /// than the sample rate.
  void SetNominalRate(float tempo);

  void SetStretchMode(AudioStretchMode mode);

  /// Wipes out the time stretching buffer, call when reducing target speed.
  void EmptyStretchBuffers();

private:
  static constexpr u32 AVERAGING_BUFFER_SIZE = 256;
  static constexpr u32 STRETCH_RESET_THRESHOLD = 5;

  ALWAYS_INLINE bool IsStretchEnabled() const { return m_parameters.stretch_mode != AudioStretchMode::Off; }

  void AllocateBuffer();
  void DestroyBuffer();

  void InternalWriteFrames(SampleType* samples, u32 num_frames);

  void StretchAllocate();
  void StretchDestroy();
  void StretchWriteBlock(const float* block);
  void StretchUnderrun();
  void StretchOverrun();

  float AddAndGetAverageTempo(float val);
  void UpdateStretchTempo();

  void ReadFrames(SampleType* samples, u32 num_frames) override;

  std::unique_ptr<AudioStream> m_stream;
  u32 m_sample_rate = 0;
  u32 m_volume = 0;
  AudioStreamParameters m_parameters;
  bool m_stretch_inactive = false;
  bool m_filling = false;
  bool m_paused = false;

  u32 m_buffer_size = 0;
  Common::unique_aligned_ptr<s16[]> m_buffer;

  // temporary staging buffer, used for timestretching
  Common::unique_aligned_ptr<s16[]> m_staging_buffer;

  // float buffer, soundtouch only accepts float samples as input
  Common::unique_aligned_ptr<float[]> m_float_buffer;

  std::atomic<u32> m_rpos{0};
  std::atomic<u32> m_wpos{0};

  void* m_soundtouch = nullptr;

  u32 m_target_buffer_size = 0;
  u32 m_stretch_reset = STRETCH_RESET_THRESHOLD;
  u64 m_stretch_reset_time = 0;

  u32 m_stretch_ok_count = 0;
  float m_nominal_rate = 1.0f;
  float m_dynamic_target_usage = 0.0f;

  u32 m_average_position = 0;
  u32 m_average_available = 0;
  u32 m_staging_buffer_pos = 0;

  std::array<float, AVERAGING_BUFFER_SIZE> m_average_fullness = {};
};
