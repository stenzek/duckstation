// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/align.h"
#include "common/types.h"

#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class Error;
class SettingsInterface;

namespace soundtouch {
class SoundTouch;
}

enum class AudioBackend : u8
{
  Null,
#ifndef __ANDROID__
  Cubeb,
  SDL,
#else
  AAudio,
  OpenSLES,
#endif
  Count
};

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

class AudioStream
{
public:
  using SampleType = s16;

  static constexpr u32 NUM_CHANNELS = 2;
  static constexpr u32 CHUNK_SIZE = 64;

#ifndef __ANDROID__
  static constexpr AudioBackend DEFAULT_BACKEND = AudioBackend::Cubeb;
#else
  static constexpr AudioBackend DEFAULT_BACKEND = AudioBackend::AAudio;
#endif

  struct DeviceInfo
  {
    std::string name;
    std::string display_name;
    u32 minimum_latency_frames;

    DeviceInfo(std::string name_, std::string display_name_, u32 minimum_latency_);
    ~DeviceInfo();
  };

public:
  virtual ~AudioStream();

  static u32 GetAlignedBufferSize(u32 size);
  static u32 GetBufferSizeForMS(u32 sample_rate, u32 ms);
  static u32 GetMSForBufferSize(u32 sample_rate, u32 buffer_size);

  static std::optional<AudioBackend> ParseBackendName(const char* str);
  static const char* GetBackendName(AudioBackend backend);
  static const char* GetBackendDisplayName(AudioBackend backend);

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

  /// Temporarily pauses the stream, preventing it from requesting data.
  virtual void SetPaused(bool paused);

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

  static std::vector<std::pair<std::string, std::string>> GetDriverNames(AudioBackend backend);
  static std::vector<DeviceInfo> GetOutputDevices(AudioBackend backend, const char* driver, u32 sample_rate);
  static std::unique_ptr<AudioStream> CreateStream(AudioBackend backend, u32 sample_rate,
                                                   const AudioStreamParameters& parameters, const char* driver_name,
                                                   const char* device_name, Error* error = nullptr);
  static std::unique_ptr<AudioStream> CreateNullStream(u32 sample_rate, u32 buffer_ms);

protected:
  AudioStream(u32 sample_rate, const AudioStreamParameters& parameters);
  void BaseInitialize();

  void ReadFrames(SampleType* samples, u32 num_frames);

  u32 m_sample_rate = 0;
  u32 m_volume = 100;
  AudioStreamParameters m_parameters;
  bool m_stretch_inactive = false;
  bool m_filling = false;
  bool m_paused = false;

private:
  static constexpr u32 AVERAGING_BUFFER_SIZE = 256;
  static constexpr u32 AVERAGING_WINDOW = 50;
  static constexpr u32 STRETCH_RESET_THRESHOLD = 5;
  static constexpr u32 TARGET_IPS = 691;

#ifndef __ANDROID__
  static std::vector<std::pair<std::string, std::string>> GetCubebDriverNames();
  static std::vector<DeviceInfo> GetCubebOutputDevices(const char* driver, u32 sample_rate);
  static std::unique_ptr<AudioStream> CreateCubebAudioStream(u32 sample_rate, const AudioStreamParameters& parameters,
                                                             const char* driver_name, const char* device_name,
                                                             Error* error);
  static std::unique_ptr<AudioStream> CreateSDLAudioStream(u32 sample_rate, const AudioStreamParameters& parameters,
                                                           Error* error);
#else
  static std::unique_ptr<AudioStream> CreateAAudioAudioStream(u32 sample_rate, const AudioStreamParameters& parameters,
                                                              Error* error);
  static std::unique_ptr<AudioStream> CreateOpenSLESAudioStream(u32 sample_rate,
                                                                const AudioStreamParameters& parameters, Error* error);
#endif

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

  u32 m_buffer_size = 0;
  Common::unique_aligned_ptr<s16[]> m_buffer;

  std::atomic<u32> m_rpos{0};
  std::atomic<u32> m_wpos{0};

  void* m_soundtouch = nullptr;

  u32 m_target_buffer_size = 0;
  u32 m_stretch_reset = STRETCH_RESET_THRESHOLD;

  u32 m_stretch_ok_count = 0;
  float m_nominal_rate = 1.0f;
  float m_dynamic_target_usage = 0.0f;

  u32 m_average_position = 0;
  u32 m_average_available = 0;
  u32 m_staging_buffer_pos = 0;

  std::array<float, AVERAGING_BUFFER_SIZE> m_average_fullness = {};

  // temporary staging buffer, used for timestretching
  Common::unique_aligned_ptr<s16[]> m_staging_buffer;

  // float buffer, soundtouch only accepts float samples as input
  Common::unique_aligned_ptr<float[]> m_float_buffer;
};
