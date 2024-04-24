// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "audio_stream.h"
#include "host.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/error.h"
#include "common/intrin.h"
#include "common/log.h"
#include "common/settings_interface.h"
#include "common/small_string.h"
#include "common/timer.h"

#include "SoundTouch.h"

#ifndef __ANDROID__
#include "freesurround_decoder.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

Log_SetChannel(AudioStream);

static constexpr bool LOG_TIMESTRETCH_STATS = false;

static constexpr const std::array<std::pair<u8, u8>, static_cast<size_t>(AudioExpansionMode::Count)>
  s_expansion_channel_count = {{
    {u8(2), u8(2)}, // Disabled
    {u8(3), u8(3)}, // StereoLFE
    {u8(5), u8(4)}, // Quadraphonic
    {u8(5), u8(5)}, // QuadraphonicLFE
    {u8(6), u8(6)}, // Surround51
    {u8(8), u8(8)}, // Surround71
  }};

AudioStream::DeviceInfo::DeviceInfo(std::string name_, std::string display_name_, u32 minimum_latency_)
  : name(std::move(name_)), display_name(std::move(display_name_)), minimum_latency_frames(minimum_latency_)
{
}

AudioStream::DeviceInfo::~DeviceInfo() = default;

AudioStream::AudioStream(u32 sample_rate, const AudioStreamParameters& parameters)
  : m_sample_rate(sample_rate), m_parameters(parameters),
    m_internal_channels(s_expansion_channel_count[static_cast<size_t>(parameters.expansion_mode)].first),
    m_output_channels(s_expansion_channel_count[static_cast<size_t>(parameters.expansion_mode)].second)
{
}

AudioStream::~AudioStream()
{
  DestroyBuffer();
}

std::unique_ptr<AudioStream> AudioStream::CreateNullStream(u32 sample_rate, u32 buffer_ms)
{
  // no point stretching with no output
  AudioStreamParameters params;
  params.expansion_mode = AudioExpansionMode::Disabled;
  params.stretch_mode = AudioStretchMode::Off;
  params.buffer_ms = static_cast<u16>(buffer_ms);

  std::unique_ptr<AudioStream> stream(new AudioStream(sample_rate, params));
  stream->BaseInitialize(&StereoSampleReaderImpl);
  return stream;
}

#ifndef __ANDROID__

std::vector<std::string> AudioStream::GetDriverNames(AudioBackend backend)
{
  std::vector<std::string> ret;
  switch (backend)
  {
    case AudioBackend::Cubeb:
      ret = GetCubebDriverNames();
      break;

    default:
      break;
  }

  return ret;
}

std::vector<AudioStream::DeviceInfo> AudioStream::GetOutputDevices(AudioBackend backend, const char* driver)
{
  std::vector<AudioStream::DeviceInfo> ret;
  switch (backend)
  {
    case AudioBackend::Cubeb:
      ret = GetCubebOutputDevices(driver);
      break;

    default:
      break;
  }

  return ret;
}

std::unique_ptr<AudioStream> AudioStream::CreateStream(AudioBackend backend, u32 sample_rate,
                                                       const AudioStreamParameters& parameters, const char* driver_name,
                                                       const char* device_name, Error* error /* = nullptr */)
{
  switch (backend)
  {
    case AudioBackend::Cubeb:
      return CreateCubebAudioStream(sample_rate, parameters, driver_name, device_name, error);

    case AudioBackend::SDL:
      return CreateSDLAudioStream(sample_rate, parameters, error);

    case AudioBackend::Null:
      return CreateNullStream(sample_rate, parameters.buffer_ms);

    default:
      Error::SetStringView(error, "Unknown audio backend.");
      return nullptr;
  }
}

#endif

u32 AudioStream::GetAlignedBufferSize(u32 size)
{
  static_assert(Common::IsPow2(CHUNK_SIZE));
  return Common::AlignUpPow2(size, CHUNK_SIZE);
}

u32 AudioStream::GetBufferSizeForMS(u32 sample_rate, u32 ms)
{
  return GetAlignedBufferSize((ms * sample_rate) / 1000u);
}

u32 AudioStream::GetMSForBufferSize(u32 sample_rate, u32 buffer_size)
{
  buffer_size = GetAlignedBufferSize(buffer_size);
  return (buffer_size * 1000u) / sample_rate;
}

static constexpr const std::array s_backend_names = {
  "Null",
#ifndef __ANDROID__
  "Cubeb",
  "SDL",
#else
  "AAudio",
  "OpenSLES",
#endif
};
static constexpr const std::array s_backend_display_names = {
  TRANSLATE_NOOP("AudioStream", "Null (No Output)"),
#ifndef __ANDROID__
  TRANSLATE_NOOP("AudioStream", "Cubeb"),
  TRANSLATE_NOOP("AudioStream", "SDL"),
#else
  "AAudio",
  "OpenSL ES",
#endif
};

std::optional<AudioBackend> AudioStream::ParseBackendName(const char* str)
{
  int index = 0;
  for (const char* name : s_backend_names)
  {
    if (std::strcmp(name, str) == 0)
      return static_cast<AudioBackend>(index);

    index++;
  }

  return std::nullopt;
}

const char* AudioStream::GetBackendName(AudioBackend backend)
{
  return s_backend_names[static_cast<int>(backend)];
}

const char* AudioStream::GetBackendDisplayName(AudioBackend backend)
{
  return Host::TranslateToCString("AudioStream", s_backend_display_names[static_cast<int>(backend)]);
}

static constexpr const std::array s_expansion_mode_names = {
  "Disabled", "StereoLFE", "Quadraphonic", "QuadraphonicLFE", "Surround51", "Surround71",
};
static constexpr const std::array s_expansion_mode_display_names = {
  TRANSLATE_NOOP("AudioStream", "Disabled (Stereo)"), TRANSLATE_NOOP("AudioStream", "Stereo with LFE"),
  TRANSLATE_NOOP("AudioStream", "Quadraphonic"),      TRANSLATE_NOOP("AudioStream", "Quadraphonic with LFE"),
  TRANSLATE_NOOP("AudioStream", "5.1 Surround"),      TRANSLATE_NOOP("AudioStream", "7.1 Surround"),
};

const char* AudioStream::GetExpansionModeName(AudioExpansionMode mode)
{
  return (static_cast<u32>(mode) < s_expansion_mode_names.size()) ? s_expansion_mode_names[static_cast<u32>(mode)] : "";
}

const char* AudioStream::GetExpansionModeDisplayName(AudioExpansionMode mode)
{
  return (static_cast<u32>(mode) < s_expansion_mode_display_names.size()) ?
           Host::TranslateToCString("AudioStream", s_expansion_mode_display_names[static_cast<u32>(mode)]) :
           "";
}

std::optional<AudioExpansionMode> AudioStream::ParseExpansionMode(const char* name)
{
  for (u8 i = 0; i < static_cast<u8>(AudioExpansionMode::Count); i++)
  {
    if (std::strcmp(name, s_expansion_mode_names[i]) == 0)
      return static_cast<AudioExpansionMode>(i);
  }

  return std::nullopt;
}

static constexpr const std::array s_stretch_mode_names = {
  "None",
  "Resample",
  "TimeStretch",
};
static constexpr const std::array s_stretch_mode_display_names = {
  TRANSLATE_NOOP("AudioStream", "Off (Noisy)"),
  TRANSLATE_NOOP("AudioStream", "Resampling (Pitch Shift)"),
  TRANSLATE_NOOP("AudioStream", "Time Stretch (Tempo Change, Best Sound)"),
};

const char* AudioStream::GetStretchModeName(AudioStretchMode mode)
{
  return (static_cast<u32>(mode) < s_stretch_mode_names.size()) ? s_stretch_mode_names[static_cast<u32>(mode)] : "";
}

const char* AudioStream::GetStretchModeDisplayName(AudioStretchMode mode)
{
  return (static_cast<u32>(mode) < s_stretch_mode_display_names.size()) ?
           Host::TranslateToCString("AudioStream", s_stretch_mode_display_names[static_cast<u32>(mode)]) :
           "";
}

std::optional<AudioStretchMode> AudioStream::ParseStretchMode(const char* name)
{
  for (u8 i = 0; i < static_cast<u8>(AudioStretchMode::Count); i++)
  {
    if (std::strcmp(name, s_stretch_mode_names[i]) == 0)
      return static_cast<AudioStretchMode>(i);
  }

  return std::nullopt;
}

u32 AudioStream::GetBufferedFramesRelaxed() const
{
  const u32 rpos = m_rpos.load(std::memory_order_relaxed);
  const u32 wpos = m_wpos.load(std::memory_order_relaxed);
  return (wpos + m_buffer_size - rpos) % m_buffer_size;
}

void AudioStream::ReadFrames(SampleType* samples, u32 num_frames)
{
  const u32 available_frames = GetBufferedFramesRelaxed();
  u32 frames_to_read = num_frames;
  u32 silence_frames = 0;

  if (m_filling)
  {
    u32 toFill = m_buffer_size / ((m_parameters.stretch_mode != AudioStretchMode::TimeStretch) ? 32 : 400);
    toFill = GetAlignedBufferSize(toFill);

    if (available_frames < toFill)
    {
      silence_frames = num_frames;
      frames_to_read = 0;
    }
    else
    {
      m_filling = false;
      Log_VerbosePrintf("Underrun compensation done (%d frames buffered)", toFill);
    }
  }

  if (available_frames < frames_to_read)
  {
    silence_frames = frames_to_read - available_frames;
    frames_to_read = available_frames;
    m_filling = true;

    if (m_parameters.stretch_mode == AudioStretchMode::TimeStretch)
      StretchUnderrun();
  }

  if (frames_to_read > 0)
  {
    u32 rpos = m_rpos.load(std::memory_order_acquire);

    u32 end = m_buffer_size - rpos;
    if (end > frames_to_read)
      end = frames_to_read;

    // towards the end of the buffer
    if (end > 0)
    {
      m_sample_reader(samples, &m_buffer[rpos * m_internal_channels], end);
      rpos += end;
      rpos = (rpos == m_buffer_size) ? 0 : rpos;
    }

    // after wrapping around
    const u32 start = frames_to_read - end;
    if (start > 0)
    {
      m_sample_reader(&samples[end * m_output_channels], &m_buffer[0], start);
      rpos = start;
    }

    m_rpos.store(rpos, std::memory_order_release);
  }

  if (silence_frames > 0)
  {
    if (frames_to_read > 0)
    {
      // super basic resampler - spread the input samples evenly across the output samples. will sound like ass and have
      // aliasing, but better than popping by inserting silence.
      const u32 increment =
        static_cast<u32>(65536.0f * (static_cast<float>(frames_to_read) / static_cast<float>(num_frames)));

      SampleType* resample_ptr =
        static_cast<SampleType*>(alloca(frames_to_read * m_output_channels * sizeof(SampleType)));
      std::memcpy(resample_ptr, samples, frames_to_read * m_output_channels * sizeof(SampleType));

      SampleType* out_ptr = samples;
      const u32 copy_stride = sizeof(SampleType) * m_output_channels;
      u32 resample_subpos = 0;
      for (u32 i = 0; i < num_frames; i++)
      {
        std::memcpy(out_ptr, resample_ptr, copy_stride);
        out_ptr += m_output_channels;

        resample_subpos += increment;
        resample_ptr += (resample_subpos >> 16) * m_output_channels;
        resample_subpos %= 65536u;
      }

      Log_VerboseFmt("Audio buffer underflow, resampled {} frames to {}", frames_to_read, num_frames);
    }
    else
    {
      // no data, fall back to silence
      std::memset(samples + (frames_to_read * m_output_channels), 0, silence_frames * m_output_channels * sizeof(s16));
    }
  }

  if (m_volume != 100)
  {
    const s32 volume_mult = static_cast<s32>((static_cast<float>(m_volume) / 100.0f) * 32768.0f);

    u32 num_samples = num_frames * m_output_channels;
    while (num_samples > 0)
    {
      *samples = static_cast<s16>((static_cast<s32>(*samples) * volume_mult) >> 15);
      samples++;
      num_samples--;
    }
  }
}

void AudioStream::StereoSampleReaderImpl(SampleType* dest, const SampleType* src, u32 num_frames)
{
  std::memcpy(dest, src, num_frames * 2 * sizeof(SampleType));
}

void AudioStream::InternalWriteFrames(s16* data, u32 num_frames)
{
  const u32 free = m_buffer_size - GetBufferedFramesRelaxed();
  if (free <= num_frames)
  {
    if (m_parameters.stretch_mode == AudioStretchMode::TimeStretch)
    {
      StretchOverrun();
    }
    else
    {
      Log_DebugPrintf("Buffer overrun, chunk dropped");
      return;
    }
  }

  u32 wpos = m_wpos.load(std::memory_order_acquire);

  // wrapping around the end of the buffer?
  if ((m_buffer_size - wpos) <= num_frames)
  {
    // needs to be written in two parts
    const u32 end = m_buffer_size - wpos;
    const u32 start = num_frames - end;

    // start is zero when this chunk reaches exactly the end
    std::memcpy(&m_buffer[wpos * m_internal_channels], data, end * m_internal_channels * sizeof(SampleType));
    if (start > 0)
      std::memcpy(&m_buffer[0], data + end * m_internal_channels, start * m_internal_channels * sizeof(SampleType));

    wpos = start;
  }
  else
  {
    // no split
    std::memcpy(&m_buffer[wpos * m_internal_channels], data, num_frames * m_internal_channels * sizeof(SampleType));
    wpos += num_frames;
  }

  m_wpos.store(wpos, std::memory_order_release);
}

void AudioStream::BaseInitialize(SampleReader sample_reader)
{
  m_sample_reader = sample_reader;

  AllocateBuffer();
  ExpandAllocate();
  StretchAllocate();
}

void AudioStream::AllocateBuffer()
{
  // use a larger buffer when time stretching, since we need more input
  // TODO: do we really? it's more the output...
  const u32 multiplier = (m_parameters.stretch_mode == AudioStretchMode::TimeStretch) ?
                           16 :
                           ((m_parameters.stretch_mode == AudioStretchMode::Off) ? 1 : 2);
  m_buffer_size = GetAlignedBufferSize(((m_parameters.buffer_ms * multiplier) * m_sample_rate) / 1000);
  m_target_buffer_size = GetAlignedBufferSize((m_sample_rate * m_parameters.buffer_ms) / 1000u);

  m_buffer = std::make_unique<s16[]>(m_buffer_size * m_internal_channels);
  m_staging_buffer = std::make_unique<s16[]>(CHUNK_SIZE * m_internal_channels);
  m_float_buffer = std::make_unique<float[]>(CHUNK_SIZE * m_internal_channels);

  if (IsExpansionEnabled())
    m_expand_buffer = std::make_unique<float[]>(m_parameters.expand_block_size * NUM_INPUT_CHANNELS);

  Log_DevFmt(
    "Allocated buffer of {} frames for buffer of {} ms [expansion {} (block size {}), stretch {}, target size {}].",
    m_buffer_size, m_parameters.buffer_ms, GetExpansionModeName(m_parameters.expansion_mode),
    m_parameters.expand_block_size, GetStretchModeName(m_parameters.stretch_mode), m_target_buffer_size);
}

void AudioStream::DestroyBuffer()
{
  m_expand_buffer.reset();
  m_staging_buffer.reset();
  m_float_buffer.reset();
  m_buffer.reset();
  m_buffer_size = 0;
  m_wpos.store(0, std::memory_order_release);
  m_rpos.store(0, std::memory_order_release);
}

void AudioStream::EmptyBuffer()
{
#ifndef __ANDROID__
  if (IsExpansionEnabled())
  {
    m_expander->Flush();
    m_expand_output_buffer = nullptr;
    m_expand_buffer_pos = 0;
  }
#endif

  if (IsStretchEnabled())
  {
    m_soundtouch->clear();
    if (m_parameters.stretch_mode == AudioStretchMode::TimeStretch)
      m_soundtouch->setTempo(m_nominal_rate);
  }

  m_wpos.store(m_rpos.load(std::memory_order_acquire), std::memory_order_release);
}

void AudioStream::SetNominalRate(float tempo)
{
  m_nominal_rate = tempo;
  if (m_parameters.stretch_mode == AudioStretchMode::Resample)
    m_soundtouch->setRate(tempo);
}

void AudioStream::UpdateTargetTempo(float tempo)
{
  if (m_parameters.stretch_mode != AudioStretchMode::TimeStretch)
    return;

  // undo sqrt()
  if (tempo)
    tempo *= tempo;

  m_average_position = AVERAGING_WINDOW;
  m_average_available = AVERAGING_WINDOW;
  std::fill_n(m_average_fullness.data(), AVERAGING_WINDOW, tempo);
  m_soundtouch->setTempo(tempo);
  m_stretch_reset = 0;
  m_stretch_inactive = false;
  m_stretch_ok_count = 0;
  m_dynamic_target_usage = static_cast<float>(m_target_buffer_size) * m_nominal_rate;
}

void AudioStream::SetStretchMode(AudioStretchMode mode)
{
  if (m_parameters.stretch_mode == mode)
    return;

  // can't resize the buffers while paused
  bool paused = m_paused;
  if (!paused)
    SetPaused(true);

  DestroyBuffer();
  StretchDestroy();
  m_parameters.stretch_mode = mode;

  AllocateBuffer();
  if (m_parameters.stretch_mode != AudioStretchMode::Off)
    StretchAllocate();

  if (!paused)
    SetPaused(false);
}

void AudioStream::SetPaused(bool paused)
{
  m_paused = paused;
}

void AudioStream::SetOutputVolume(u32 volume)
{
  m_volume = volume;
}

void AudioStream::BeginWrite(SampleType** buffer_ptr, u32* num_frames)
{
  // TODO: Write directly to buffer when not using stretching.
  *buffer_ptr = &m_staging_buffer[m_staging_buffer_pos];
  *num_frames = CHUNK_SIZE - (m_staging_buffer_pos / NUM_INPUT_CHANNELS);
}

void AudioStream::WriteFrames(const SampleType* frames, u32 num_frames)
{
  Panic("not implemented");
}

static constexpr float S16_TO_FLOAT = 1.0f / 32767.0f;
static constexpr float FLOAT_TO_S16 = 32767.0f;

#if defined(CPU_ARCH_NEON)

static void S16ChunkToFloat(const s16* src, float* dst, u32 num_samples)
{
  const float32x4_t S16_TO_FLOAT_V = vdupq_n_f32(S16_TO_FLOAT);

  const u32 iterations = (num_samples + 7) / 8;
  for (u32 i = 0; i < iterations; i++)
  {
    const int16x8_t sv = vreinterpretq_s16_s32(vld1q_s16(src));
    src += 8;

    int32x4_t iv1 = vreinterpretq_s32_s16(vzip1q_s16(sv, sv)); // [0, 0, 1, 1, 2, 2, 3, 3]
    int32x4_t iv2 = vreinterpretq_s32_s16(vzip2q_s16(sv, sv)); // [4, 4, 5, 5, 6, 6, 7, 7]
    iv1 = vshrq_n_s32(iv1, 16);                                // [0, 1, 2, 3]
    iv2 = vshrq_n_s32(iv2, 16);                                // [4, 5, 6, 7]
    float32x4_t fv1 = vcvtq_f32_s32(iv1);                      // [f0, f1, f2, f3]
    float32x4_t fv2 = vcvtq_f32_s32(iv2);                      // [f4, f5, f6, f7]
    fv1 = vmulq_f32(fv1, S16_TO_FLOAT_V);
    fv2 = vmulq_f32(fv2, S16_TO_FLOAT_V);

    vst1q_f32(dst + 0, fv1);
    vst1q_f32(dst + 4, fv2);
    dst += 8;
  }
}

static void FloatChunkToS16(s16* dst, const float* src, u32 num_samples)
{
  const float32x4_t FLOAT_TO_S16_V = vdupq_n_f32(FLOAT_TO_S16);

  const u32 iterations = (num_samples + 7) / 8;
  for (u32 i = 0; i < iterations; i++)
  {
    float32x4_t fv1 = vld1q_f32(src + 0);
    float32x4_t fv2 = vld1q_f32(src + 4);
    src += 8;

    fv1 = vmulq_f32(fv1, FLOAT_TO_S16_V);
    fv2 = vmulq_f32(fv2, FLOAT_TO_S16_V);
    int32x4_t iv1 = vcvtq_s32_f32(fv1);
    int32x4_t iv2 = vcvtq_s32_f32(fv2);

    int16x8_t iv = vcombine_s16(vqmovn_s32(iv1), vqmovn_s32(iv2));
    vst1q_s16(dst, iv);
    dst += 8;
  }
}

#elif defined(CPU_ARCH_SSE)

static void S16ChunkToFloat(const s16* src, float* dst, u32 num_samples)
{
  const __m128 S16_TO_FLOAT_V = _mm_set1_ps(S16_TO_FLOAT);

  const u32 iterations = (num_samples + 7) / 8;
  for (u32 i = 0; i < iterations; i++)
  {
    const __m128i sv = _mm_load_si128(reinterpret_cast<const __m128i*>(src));
    src += 8;

    __m128i iv1 = _mm_unpacklo_epi16(sv, sv); // [0, 0, 1, 1, 2, 2, 3, 3]
    __m128i iv2 = _mm_unpackhi_epi16(sv, sv); // [4, 4, 5, 5, 6, 6, 7, 7]
    iv1 = _mm_srai_epi32(iv1, 16);            // [0, 1, 2, 3]
    iv2 = _mm_srai_epi32(iv2, 16);            // [4, 5, 6, 7]
    __m128 fv1 = _mm_cvtepi32_ps(iv1);        // [f0, f1, f2, f3]
    __m128 fv2 = _mm_cvtepi32_ps(iv2);        // [f4, f5, f6, f7]
    fv1 = _mm_mul_ps(fv1, S16_TO_FLOAT_V);
    fv2 = _mm_mul_ps(fv2, S16_TO_FLOAT_V);

    _mm_store_ps(dst + 0, fv1);
    _mm_store_ps(dst + 4, fv2);
    dst += 8;
  }
}

static void FloatChunkToS16(s16* dst, const float* src, u32 num_samples)
{
  const __m128 FLOAT_TO_S16_V = _mm_set1_ps(FLOAT_TO_S16);

  const u32 iterations = (num_samples + 7) / 8;
  for (u32 i = 0; i < iterations; i++)
  {
    __m128 fv1 = _mm_load_ps(src + 0);
    __m128 fv2 = _mm_load_ps(src + 4);
    src += 8;

    fv1 = _mm_mul_ps(fv1, FLOAT_TO_S16_V);
    fv2 = _mm_mul_ps(fv2, FLOAT_TO_S16_V);
    __m128i iv1 = _mm_cvtps_epi32(fv1);
    __m128i iv2 = _mm_cvtps_epi32(fv2);

    __m128i iv = _mm_packs_epi32(iv1, iv2);
    _mm_store_si128(reinterpret_cast<__m128i*>(dst), iv);
    dst += 8;
  }
}

#else

static void S16ChunkToFloat(const s16* src, float* dst, u32 num_samples)
{
  for (u32 i = 0; i < num_samples; ++i)
    *(dst++) = static_cast<float>(*(src++)) / 32767.0f;
}

static void FloatChunkToS16(s16* dst, const float* src, u32 num_samples)
{
  for (u32 i = 0; i < num_samples; ++i)
    *(dst++) = static_cast<s16>((*(src++) * 32767.0f));
}
#endif

void AudioStream::ExpandAllocate()
{
  DebugAssert(!m_expander);
  if (m_parameters.expansion_mode == AudioExpansionMode::Disabled)
    return;

#ifndef __ANDROID__
  static constexpr std::array<std::pair<FreeSurroundDecoder::ChannelSetup, bool>,
                              static_cast<size_t>(AudioExpansionMode::Count)>
    channel_setup_mapping = {{
      {FreeSurroundDecoder::ChannelSetup::Stereo, false},     // Disabled
      {FreeSurroundDecoder::ChannelSetup::Stereo, true},      // StereoLFE
      {FreeSurroundDecoder::ChannelSetup::Surround41, false}, // Quadraphonic
      {FreeSurroundDecoder::ChannelSetup::Surround41, true},  // QuadraphonicLFE
      {FreeSurroundDecoder::ChannelSetup::Surround51, true},  // Surround51
      {FreeSurroundDecoder::ChannelSetup::Surround71, true},  // Surround71
    }};

  const auto [fs_setup, fs_lfe] = channel_setup_mapping[static_cast<size_t>(m_parameters.expansion_mode)];

  m_expander = std::make_unique<FreeSurroundDecoder>(fs_setup, m_parameters.expand_block_size);
  m_expander->SetBassRedirection(fs_lfe);
  m_expander->SetCircularWrap(m_parameters.expand_circular_wrap);
  m_expander->SetShift(m_parameters.expand_shift);
  m_expander->SetDepth(m_parameters.expand_depth);
  m_expander->SetFocus(m_parameters.expand_focus);
  m_expander->SetCenterImage(m_parameters.expand_center_image);
  m_expander->SetFrontSeparation(m_parameters.expand_front_separation);
  m_expander->SetRearSeparation(m_parameters.expand_rear_separation);
  m_expander->SetLowCutoff(static_cast<float>(m_parameters.expand_low_cutoff) / m_sample_rate * 2);
  m_expander->SetHighCutoff(static_cast<float>(m_parameters.expand_high_cutoff) / m_sample_rate * 2);
#else
  Panic("Attempting to use expansion on Android.");
#endif
}

void AudioStream::EndWrite(u32 num_frames)
{
  // don't bother committing anything when muted
  if (m_volume == 0)
    return;

  m_staging_buffer_pos += num_frames * NUM_INPUT_CHANNELS;
  DebugAssert(m_staging_buffer_pos <= (CHUNK_SIZE * NUM_INPUT_CHANNELS));
  if ((m_staging_buffer_pos / NUM_INPUT_CHANNELS) < CHUNK_SIZE)
    return;

  m_staging_buffer_pos = 0;

  if (!IsExpansionEnabled() && !IsStretchEnabled())
  {
    InternalWriteFrames(m_staging_buffer.get(), CHUNK_SIZE);
    return;
  }

#ifndef __ANDROID__
  if (IsExpansionEnabled())
  {
    // StretchWriteBlock() overwrites the staging buffer on output, so we need to copy into the expand buffer first.
    S16ChunkToFloat(m_staging_buffer.get(), m_expand_buffer.get() + m_expand_buffer_pos * NUM_INPUT_CHANNELS,
                    CHUNK_SIZE * NUM_INPUT_CHANNELS);

    // Output the corresponding block.
    if (m_expand_output_buffer)
      StretchWriteBlock(m_expand_output_buffer + m_expand_buffer_pos * m_internal_channels);

    // Decode the next block if we buffered enough.
    m_expand_buffer_pos += CHUNK_SIZE;
    if (m_expand_buffer_pos == m_parameters.expand_block_size)
    {
      m_expand_buffer_pos = 0;
      m_expand_output_buffer = m_expander->Decode(m_expand_buffer.get());
    }
  }
  else
#endif
  {
    S16ChunkToFloat(m_staging_buffer.get(), m_float_buffer.get(), CHUNK_SIZE * NUM_INPUT_CHANNELS);
    StretchWriteBlock(m_float_buffer.get());
  }
}

// Time stretching algorithm based on PCSX2 implementation.

template<class T>
ALWAYS_INLINE static bool IsInRange(const T& val, const T& min, const T& max)
{
  return (min <= val && val <= max);
}

void AudioStream::StretchAllocate()
{
  if (m_parameters.stretch_mode == AudioStretchMode::Off)
    return;

  m_soundtouch = std::make_unique<soundtouch::SoundTouch>();
  m_soundtouch->setSampleRate(m_sample_rate);
  m_soundtouch->setChannels(m_internal_channels);

  m_soundtouch->setSetting(SETTING_USE_QUICKSEEK, m_parameters.stretch_use_quickseek);
  m_soundtouch->setSetting(SETTING_USE_AA_FILTER, m_parameters.stretch_use_aa_filter);

  m_soundtouch->setSetting(SETTING_SEQUENCE_MS, m_parameters.stretch_sequence_length_ms);
  m_soundtouch->setSetting(SETTING_SEEKWINDOW_MS, m_parameters.stretch_seekwindow_ms);
  m_soundtouch->setSetting(SETTING_OVERLAP_MS, m_parameters.stretch_overlap_ms);

  if (m_parameters.stretch_mode == AudioStretchMode::Resample)
    m_soundtouch->setRate(m_nominal_rate);
  else
    m_soundtouch->setTempo(m_nominal_rate);

  m_stretch_reset = STRETCH_RESET_THRESHOLD;
  m_stretch_inactive = false;
  m_stretch_ok_count = 0;
  m_dynamic_target_usage = 0.0f;
  m_average_position = 0;
  m_average_available = 0;

  m_staging_buffer_pos = 0;
}

void AudioStream::StretchDestroy()
{
  m_soundtouch.reset();
}

void AudioStream::StretchWriteBlock(const float* block)
{
  if (IsStretchEnabled())
  {
    m_soundtouch->putSamples(block, CHUNK_SIZE);

    u32 tempProgress;
    while (tempProgress = m_soundtouch->receiveSamples(m_float_buffer.get(), CHUNK_SIZE), tempProgress != 0)
    {
      FloatChunkToS16(m_staging_buffer.get(), m_float_buffer.get(), tempProgress * m_internal_channels);
      InternalWriteFrames(m_staging_buffer.get(), tempProgress);
    }

    if (m_parameters.stretch_mode == AudioStretchMode::TimeStretch)
      UpdateStretchTempo();
  }
  else
  {
    FloatChunkToS16(m_staging_buffer.get(), block, CHUNK_SIZE * m_internal_channels);
    InternalWriteFrames(m_staging_buffer.get(), CHUNK_SIZE);
  }
}

float AudioStream::AddAndGetAverageTempo(float val)
{
  if (m_stretch_reset >= STRETCH_RESET_THRESHOLD)
    m_average_available = 0;
  if (m_average_available < AVERAGING_BUFFER_SIZE)
    m_average_available++;

  m_average_fullness[m_average_position] = val;
  m_average_position = (m_average_position + 1U) % AVERAGING_BUFFER_SIZE;

  const u32 actual_window = std::min<u32>(m_average_available, AVERAGING_WINDOW);
  const u32 first_index = (m_average_position - actual_window + AVERAGING_BUFFER_SIZE) % AVERAGING_BUFFER_SIZE;

  float sum = 0;
  for (u32 i = first_index; i < first_index + actual_window; i++)
    sum += m_average_fullness[i % AVERAGING_BUFFER_SIZE];
  sum = sum / actual_window;

  return (sum != 0.0f) ? sum : 1.0f;
}

void AudioStream::UpdateStretchTempo()
{
  static constexpr float MIN_TEMPO = 0.05f;
  static constexpr float MAX_TEMPO = 50.0f;

  // Which range we will run in 1:1 mode for.
  static constexpr float INACTIVE_GOOD_FACTOR = 1.04f;
  static constexpr float INACTIVE_BAD_FACTOR = 1.2f;
  static constexpr u32 INACTIVE_MIN_OK_COUNT = 50;
  static constexpr u32 COMPENSATION_DIVIDER = 100;

  float base_target_usage = static_cast<float>(m_target_buffer_size) * m_nominal_rate;

  // state vars
  if (m_stretch_reset >= STRETCH_RESET_THRESHOLD)
  {
    Log_VerbosePrintf("___ Stretcher is being reset.");
    m_stretch_inactive = false;
    m_stretch_ok_count = 0;
    m_dynamic_target_usage = base_target_usage;
  }

  const u32 ibuffer_usage = GetBufferedFramesRelaxed();
  float buffer_usage = static_cast<float>(ibuffer_usage);
  float tempo = buffer_usage / m_dynamic_target_usage;
  tempo = AddAndGetAverageTempo(tempo);

  // Dampening when we get close to target.
  if (tempo < 2.0f)
    tempo = std::sqrt(tempo);

  tempo = std::clamp(tempo, MIN_TEMPO, MAX_TEMPO);

  if (tempo < 1.0f)
    base_target_usage /= std::sqrt(tempo);

  m_dynamic_target_usage +=
    static_cast<float>(base_target_usage / tempo - m_dynamic_target_usage) / static_cast<float>(COMPENSATION_DIVIDER);
  if (IsInRange(tempo, 0.9f, 1.1f) &&
      IsInRange(m_dynamic_target_usage, base_target_usage * 0.9f, base_target_usage * 1.1f))
  {
    m_dynamic_target_usage = base_target_usage;
  }

  if (!m_stretch_inactive)
  {
    if (IsInRange(tempo, 1.0f / INACTIVE_GOOD_FACTOR, INACTIVE_GOOD_FACTOR))
      m_stretch_ok_count++;
    else
      m_stretch_ok_count = 0;

    if (m_stretch_ok_count >= INACTIVE_MIN_OK_COUNT)
    {
      Log_VerbosePrintf("=== Stretcher is now inactive.");
      m_stretch_inactive = true;
    }
  }
  else if (!IsInRange(tempo, 1.0f / INACTIVE_BAD_FACTOR, INACTIVE_BAD_FACTOR))
  {
    Log_VerbosePrintf("~~~ Stretcher is now active @ tempo %f.", tempo);
    m_stretch_inactive = false;
    m_stretch_ok_count = 0;
  }

  if (m_stretch_inactive)
    tempo = m_nominal_rate;

  if constexpr (LOG_TIMESTRETCH_STATS)
  {
    static int iterations = 0;
    static u64 last_log_time = 0;

    const u64 now = Common::Timer::GetCurrentValue();

    if (Common::Timer::ConvertValueToSeconds(now - last_log_time) > 1.0f)
    {
      Log_VerbosePrintf("buffers: %4u ms (%3.0f%%), tempo: %f, comp: %2.3f, iters: %d, reset:%d",
                        (ibuffer_usage * 1000u) / m_sample_rate, 100.0f * buffer_usage / base_target_usage, tempo,
                        m_dynamic_target_usage / base_target_usage, iterations, m_stretch_reset);

      last_log_time = now;
      iterations = 0;
    }

    iterations++;
  }

  m_soundtouch->setTempo(tempo);

  if (m_stretch_reset >= STRETCH_RESET_THRESHOLD)
    m_stretch_reset = 0;
}

void AudioStream::StretchUnderrun()
{
  // Didn't produce enough frames in time.
  m_stretch_reset++;
}

void AudioStream::StretchOverrun()
{
  // Produced more frames than can fit in the buffer.
  m_stretch_reset++;

  // Drop two packets to give the time stretcher a bit more time to slow things down.
  const u32 discard = CHUNK_SIZE * 2;
  m_rpos.store((m_rpos.load(std::memory_order_acquire) + discard) % m_buffer_size, std::memory_order_release);
}

void AudioStreamParameters::Load(SettingsInterface& si, const char* section)
{
  stretch_mode =
    AudioStream::ParseStretchMode(
      si.GetStringValue(section, "StretchMode", AudioStream::GetStretchModeName(DEFAULT_STRETCH_MODE)).c_str())
      .value_or(DEFAULT_STRETCH_MODE);
#ifndef __ANDROID__
  expansion_mode =
    AudioStream::ParseExpansionMode(
      si.GetStringValue(section, "ExpansionMode", AudioStream::GetExpansionModeName(DEFAULT_EXPANSION_MODE)).c_str())
      .value_or(DEFAULT_EXPANSION_MODE);
#else
  expansion_mode = AudioExpansionMode::Disabled;
#endif
  output_latency_ms = static_cast<u16>(std::min<u32>(
    si.GetUIntValue(section, "OutputLatencyMS", DEFAULT_OUTPUT_LATENCY_MS), std::numeric_limits<u16>::max()));
  buffer_ms = static_cast<u16>(
    std::min<u32>(si.GetUIntValue(section, "BufferMS", DEFAULT_BUFFER_MS), std::numeric_limits<u16>::max()));

  stretch_sequence_length_ms =
    static_cast<u16>(std::min<u32>(si.GetUIntValue(section, "StretchSequenceLengthMS", DEFAULT_STRETCH_SEQUENCE_LENGTH),
                                   std::numeric_limits<u16>::max()));
  stretch_seekwindow_ms = static_cast<u16>(std::min<u32>(
    si.GetUIntValue(section, "StretchSeekWindowMS", DEFAULT_STRETCH_SEEKWINDOW), std::numeric_limits<u16>::max()));
  stretch_overlap_ms = static_cast<u16>(std::min<u32>(
    si.GetUIntValue(section, "StretchOverlapMS", DEFAULT_STRETCH_OVERLAP), std::numeric_limits<u16>::max()));
  stretch_use_quickseek = si.GetBoolValue(section, "StretchUseQuickSeek", DEFAULT_STRETCH_USE_QUICKSEEK);
  stretch_use_aa_filter = si.GetBoolValue(section, "StretchUseAAFilter", DEFAULT_STRETCH_USE_AA_FILTER);

  expand_block_size = static_cast<u16>(std::min<u32>(
    si.GetUIntValue(section, "ExpandBlockSize", DEFAULT_EXPAND_BLOCK_SIZE), std::numeric_limits<u16>::max()));
  expand_block_size = std::clamp<u16>(
    Common::IsPow2(expand_block_size) ? expand_block_size : Common::NextPow2(expand_block_size), 128, 8192);
  expand_circular_wrap =
    std::clamp(si.GetFloatValue(section, "ExpandCircularWrap", DEFAULT_EXPAND_CIRCULAR_WRAP), 0.0f, 360.0f);
  expand_shift = std::clamp(si.GetFloatValue(section, "ExpandShift", DEFAULT_EXPAND_SHIFT), -1.0f, 1.0f);
  expand_depth = std::clamp(si.GetFloatValue(section, "ExpandDepth", DEFAULT_EXPAND_DEPTH), 0.0f, 5.0f);
  expand_focus = std::clamp(si.GetFloatValue(section, "ExpandFocus", DEFAULT_EXPAND_FOCUS), -1.0f, 1.0f);
  expand_center_image =
    std::clamp(si.GetFloatValue(section, "ExpandCenterImage", DEFAULT_EXPAND_CENTER_IMAGE), 0.0f, 1.0f);
  expand_front_separation =
    std::clamp(si.GetFloatValue(section, "ExpandFrontSeparation", DEFAULT_EXPAND_FRONT_SEPARATION), 0.0f, 10.0f);
  expand_rear_separation =
    std::clamp(si.GetFloatValue(section, "ExpandRearSeparation", DEFAULT_EXPAND_REAR_SEPARATION), 0.0f, 10.0f);
  expand_low_cutoff =
    static_cast<u8>(std::min<u32>(si.GetUIntValue(section, "ExpandLowCutoff", DEFAULT_EXPAND_LOW_CUTOFF), 100));
  expand_high_cutoff =
    static_cast<u8>(std::min<u32>(si.GetUIntValue(section, "ExpandHighCutoff", DEFAULT_EXPAND_HIGH_CUTOFF), 100));
}

void AudioStreamParameters::Save(SettingsInterface& si, const char* section) const
{
  si.SetStringValue(section, "StretchMode", AudioStream::GetStretchModeName(stretch_mode));
  si.SetStringValue(section, "ExpansionMode", AudioStream::GetExpansionModeName(expansion_mode));
  si.SetUIntValue(section, "BufferMS", buffer_ms);
  si.SetUIntValue(section, "OutputLatencyMS", output_latency_ms);

  si.SetUIntValue(section, "StretchSequenceLengthMS", stretch_sequence_length_ms);
  si.SetUIntValue(section, "StretchSeekWindowMS", stretch_seekwindow_ms);
  si.SetUIntValue(section, "StretchOverlapMS", stretch_overlap_ms);
  si.SetBoolValue(section, "StretchUseQuickSeek", stretch_use_quickseek);
  si.SetBoolValue(section, "StretchUseAAFilter", stretch_use_aa_filter);

  si.SetUIntValue(section, "ExpandBlockSize", expand_block_size);
  si.SetFloatValue(section, "ExpandCircularWrap", expand_circular_wrap);
  si.SetFloatValue(section, "ExpandShift", expand_shift);
  si.SetFloatValue(section, "ExpandDepth", expand_depth);
  si.SetFloatValue(section, "ExpandFocus", expand_focus);
  si.SetFloatValue(section, "ExpandCenterImage", expand_center_image);
  si.SetFloatValue(section, "ExpandFrontSeparation", expand_front_separation);
  si.SetFloatValue(section, "ExpandRearSeparation", expand_rear_separation);
  si.SetUIntValue(section, "ExpandLowCutoff", expand_low_cutoff);
  si.SetUIntValue(section, "ExpandHighCutoff", expand_high_cutoff);
}

void AudioStreamParameters::Clear(SettingsInterface& si, const char* section)
{
  si.DeleteValue(section, "StretchMode");
  si.DeleteValue(section, "ExpansionMode");
  si.DeleteValue(section, "BufferMS");
  si.DeleteValue(section, "OutputLatencyMS");

  si.DeleteValue(section, "StretchSequenceLengthMS");
  si.DeleteValue(section, "StretchSeekWindowMS");
  si.DeleteValue(section, "StretchOverlapMS");
  si.DeleteValue(section, "StretchUseQuickSeek");
  si.DeleteValue(section, "StretchUseAAFilter");

  si.DeleteValue(section, "ExpandBlockSize");
  si.DeleteValue(section, "ExpandCircularWrap");
  si.DeleteValue(section, "ExpandShift");
  si.DeleteValue(section, "ExpandDepth");
  si.DeleteValue(section, "ExpandFocus");
  si.DeleteValue(section, "ExpandCenterImage");
  si.DeleteValue(section, "ExpandFrontSeparation");
  si.DeleteValue(section, "ExpandRearSeparation");
  si.DeleteValue(section, "ExpandLowCutoff");
  si.DeleteValue(section, "ExpandHighCutoff");
}

bool AudioStreamParameters::operator!=(const AudioStreamParameters& rhs) const
{
  return (std::memcmp(this, &rhs, sizeof(*this)) != 0);
}

bool AudioStreamParameters::operator==(const AudioStreamParameters& rhs) const
{
  return (std::memcmp(this, &rhs, sizeof(*this)) == 0);
}
