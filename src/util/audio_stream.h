#pragma once
#include "common/types.h"
#include <array>
#include <atomic>
#include <memory>
#include <optional>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324) // warning C4324: structure was padded due to alignment specifier
#endif

namespace soundtouch {
class SoundTouch;
}

enum class AudioStretchMode : u8
{
  Off,
  Resample,
  TimeStretch,
  Count
};

class AudioStream
{
public:
  using SampleType = s16;

  enum : u32
  {
    CHUNK_SIZE = 64,
    MAX_CHANNELS = 2
  };

public:
  virtual ~AudioStream();

  static u32 GetAlignedBufferSize(u32 size);
  static u32 GetBufferSizeForMS(u32 sample_rate, u32 ms);
  static u32 GetMSForBufferSize(u32 sample_rate, u32 buffer_size);

  static const char* GetStretchModeName(AudioStretchMode mode);
  static const char* GetStretchModeDisplayName(AudioStretchMode mode);
  static std::optional<AudioStretchMode> ParseStretchMode(const char* name);

  ALWAYS_INLINE u32 GetSampleRate() const { return m_sample_rate; }
  ALWAYS_INLINE u32 GetChannels() const { return m_channels; }
  ALWAYS_INLINE u32 GetBufferSize() const { return m_buffer_size; }
  ALWAYS_INLINE u32 GetTargetBufferSize() const { return m_target_buffer_size; }
  ALWAYS_INLINE u32 GetOutputVolume() const { return m_volume; }
  ALWAYS_INLINE float GetNominalTempo() const { return m_nominal_rate; }
  ALWAYS_INLINE bool IsPaused() const { return m_paused; }

  u32 GetBufferedFramesRelaxed() const;

  /// Temporarily pauses the stream, preventing it from requesting data.
  virtual void SetPaused(bool paused);

  virtual void SetOutputVolume(u32 volume);

  void BeginWrite(SampleType** buffer_ptr, u32* num_frames);
  void WriteFrames(const SampleType* frames, u32 num_frames);
  void EndWrite(u32 num_frames);

  void EmptyBuffer();

  /// Nominal rate is used for both resampling and timestretching, input samples are assumed to be this amount faster
  /// than the sample rate.
  void SetNominalRate(float tempo);
  void UpdateTargetTempo(float tempo);

  void SetStretchMode(AudioStretchMode mode);

  static std::unique_ptr<AudioStream> CreateNullStream(u32 sample_rate, u32 channels, u32 buffer_ms);

protected:
  AudioStream(u32 sample_rate, u32 channels, u32 buffer_ms, AudioStretchMode stretch);
  void BaseInitialize();

  void ReadFrames(s16* bData, u32 nSamples);

  u32 m_sample_rate = 0;
  u32 m_channels = 0;
  u32 m_buffer_ms = 0;
  u32 m_volume = 0;

  AudioStretchMode m_stretch_mode = AudioStretchMode::Off;
  bool m_stretch_inactive = false;
  bool m_filling = false;
  bool m_paused = false;

private:
  enum : u32
  {
    AVERAGING_BUFFER_SIZE = 256,
    AVERAGING_WINDOW = 50,
    STRETCH_RESET_THRESHOLD = 5,
    TARGET_IPS = 691,
  };

  void AllocateBuffer();
  void DestroyBuffer();

  void InternalWriteFrames(s32* bData, u32 nFrames);

  void StretchAllocate();
  void StretchDestroy();
  void StretchWrite();
  void StretchUnderrun();
  void StretchOverrun();

  float AddAndGetAverageTempo(float val);
  void UpdateStretchTempo();

  u32 m_buffer_size = 0;
  std::unique_ptr<s32[]> m_buffer;

  std::atomic<u32> m_rpos{0};
  std::atomic<u32> m_wpos{0};

  std::unique_ptr<soundtouch::SoundTouch> m_soundtouch;

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
  alignas(16) std::array<s32, CHUNK_SIZE> m_staging_buffer;

  // float buffer, soundtouch only accepts float samples as input
  alignas(16) std::array<float, CHUNK_SIZE * MAX_CHANNELS> m_float_buffer;
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif
