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
  static constexpr u16 DEFAULT_BUFFER_MS = 50;
  static constexpr u16 DEFAULT_OUTPUT_LATENCY_MS = 20;
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
  friend class CoreAudioStreamTest;

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
  static std::optional<AudioStretchMode> ParseStretchMode(std::string_view name);

  ALWAYS_INLINE u32 GetSampleRate() const { return m_sample_rate; }
  ALWAYS_INLINE u32 GetBufferSize() const { return m_buffer_size; }
  ALWAYS_INLINE u32 GetTargetBufferSize() const { return m_target_buffer_size; }
  ALWAYS_INLINE u32 GetOutputVolume() const { return m_volume.load(std::memory_order_relaxed); }
  ALWAYS_INLINE float GetNominalTempo() const { return m_nominal_rate.load(std::memory_order_relaxed); }
  ALWAYS_INLINE bool IsPaused() const { return m_paused; }

  u32 GetBufferedFramesRelaxed() const;

  /// Creation/destruction.
  bool Initialize(AudioBackend backend, u32 sample_rate, const AudioStreamParameters& params,
                  std::string_view driver_name, std::string_view device_name, Error* error);
  void Destroy();

  /// Temporarily pauses the stream, preventing it from requesting data.
  void SetPaused(bool paused);

  void SetOutputVolume(u32 volume);

  /// Returns contiguous space in the buffer to write to. If there's no space (the buffer is full, or output is
  /// muted/paused), return false.
  bool BeginWrite(SampleType** buffer_ptr, u32* num_frames);
  void EndWrite(u32 num_frames);

  /// Nominal rate is used for both resampling and timestretching, input samples are assumed to be this amount faster
  /// than the sample rate.
  void SetNominalRate(float tempo);

  /// Resets the time stretcher tempo, call when reducing target speed.
  void EmptyStretchBuffers();

private:
  static constexpr u32 STRETCH_RESET_THRESHOLD = 5;
  static constexpr u32 EXTRA_TARGET_UNDERRUN_SECONDS = 30;
  static constexpr u32 EXTRA_TARGET_DECAY_SECONDS = 60;

  ALWAYS_INLINE bool IsStretchEnabled() const { return m_parameters.stretch_mode != AudioStretchMode::Off; }

  void AllocateBuffer();
  void DestroyBuffer();

  u32 GetTargetSize() const;

  u32 ReadBufferFrames(SampleType* samples, u32 rpos, u32 num_frames) const;

  void StretchAllocate();
  void StretchDestroy();
  void StretchReadFrames(SampleType* samples, u32 num_frames);
  void StretchOverrun();

  void UpdateStretchTempo(float nominal_rate);

  void ReadFrames(SampleType* samples, u32 num_frames) override;

  std::unique_ptr<AudioStream> m_stream;
  u32 m_sample_rate = 0;
  std::atomic<u32> m_volume{0};
  AudioStreamParameters m_parameters;
  bool m_stretch_inactive = false;
  bool m_filling = false;
  bool m_paused = false;

  u32 m_buffer_size = 0;
  Common::unique_aligned_ptr<s16[]> m_buffer;

  // float buffer, soundtouch only accepts float samples as input, used on the audio thread
  Common::unique_aligned_ptr<float[]> m_float_buffer;

  std::atomic<u32> m_rpos{0};
  std::atomic<u32> m_wpos{0};

  // stretching runs on the audio thread, these are set by the writer
  std::atomic<float> m_nominal_rate{1.0f};
  std::atomic<bool> m_stretch_reset_request{false};

  // output buffered in the stretcher, set by the audio thread
  std::atomic<u32> m_stretch_buffered_frames{0};

  // last callback size, set by the audio thread
  std::atomic<u32> m_callback_frames{0};

  void* m_soundtouch = nullptr;

  u32 m_target_buffer_size = 0;
  u32 m_stretch_reset = 0;
  u64 m_stretch_reset_time = 0;

  // raised when underrunning repeatedly, i.e. the frame timing is too uneven for the buffer
  u32 m_extra_target_frames = 0;
  u32 m_frames_since_underrun = 0;
  u32 m_underrun_gap_frames = 0;

  u32 m_stretch_ok_count = 0;
  float m_dynamic_target_usage = 0.0f;
  float m_average_tempo = 1.0f;
  float m_max_tempo = 1.0f;
};
