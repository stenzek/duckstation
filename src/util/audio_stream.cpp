// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "audio_stream.h"
#include "audio_stream_channel_maps.inl"
#include "host.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/error.h"
#include "common/gsvector.h"
#include "common/log.h"
#include "common/settings_interface.h"
#include "common/timer.h"

#include "kiss_fftr.h"
#include "soundtouch/SoundTouch.h"
#include "soundtouch/SoundTouchDLL.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

Log_SetChannel(AudioStream);

static constexpr bool LOG_TIMESTRETCH_STATS = false;

namespace {
struct ExpansionChannelSetup
{
  const ExpandLUT* channel_lut;
  const s8* channel_xsf;
  u8 output_channels;
  bool has_lfe;
};

static constexpr const std::array<ExpansionChannelSetup, static_cast<size_t>(AudioExpansionMode::Count)>
  s_expansion_channel_count = {{
    {CHANNEL_LUT_STEREO, CHANNEL_LUT_STEREO_XSF, static_cast<u8>(2), false},   // Disabled
    {CHANNEL_LUT_STEREO, CHANNEL_LUT_STEREO_XSF, static_cast<u8>(3), true},    // StereoLFE
    {CHANNEL_LUT_4POINT1, CHANNEL_LUT_4POINT1_XSF, static_cast<u8>(4), false}, // Quadraphonic
    {CHANNEL_LUT_4POINT1, CHANNEL_LUT_4POINT1_XSF, static_cast<u8>(5), true},  // QuadraphonicLFE
    {CHANNEL_LUT_5POINT1, CHANNEL_LUT_5POINT1_XSF, static_cast<u8>(6), true},  // Surround51
    {CHANNEL_LUT_7POINT1, CHANNEL_LUT_7POINT1_XSF, static_cast<u8>(8), true},  // Surround71
  }};
} // namespace

AudioStream::DeviceInfo::DeviceInfo(std::string name_, std::string display_name_, u32 minimum_latency_)
  : name(std::move(name_)), display_name(std::move(display_name_)), minimum_latency_frames(minimum_latency_)
{
}

AudioStream::DeviceInfo::~DeviceInfo() = default;

void AudioStreamParameters::Load(SettingsInterface& si, const char* section)
{
  stretch_mode =
    AudioStream::ParseStretchMode(
      si.GetStringValue(section, "StretchMode", AudioStream::GetStretchModeName(DEFAULT_STRETCH_MODE)).c_str())
      .value_or(DEFAULT_STRETCH_MODE);
  expansion_mode =
    AudioStream::ParseExpansionMode(
      si.GetStringValue(section, "ExpansionMode", AudioStream::GetExpansionModeName(DEFAULT_EXPANSION_MODE)).c_str())
      .value_or(DEFAULT_EXPANSION_MODE);
  output_latency_ms = static_cast<u16>(std::min<u32>(
    si.GetUIntValue(section, "OutputLatencyMS", DEFAULT_OUTPUT_LATENCY_MS), std::numeric_limits<u16>::max()));
  output_latency_minimal = si.GetBoolValue(section, "OutputLatencyMinimal", DEFAULT_OUTPUT_LATENCY_MINIMAL);
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
  si.SetBoolValue(section, "OutputLatencyMinimal", output_latency_minimal);

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
  si.DeleteValue(section, "OutputLatencyMinimal");

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

AudioStream::AudioStream(u32 sample_rate, const AudioStreamParameters& parameters)
  : m_sample_rate(sample_rate), m_parameters(parameters),
    m_output_channels(s_expansion_channel_count[static_cast<size_t>(parameters.expansion_mode)].output_channels)
{
}

AudioStream::~AudioStream()
{
  StretchDestroy();
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

std::vector<std::pair<std::string, std::string>> AudioStream::GetDriverNames(AudioBackend backend)
{
  std::vector<std::pair<std::string, std::string>> ret;
  switch (backend)
  {
#ifndef __ANDROID__
    case AudioBackend::Cubeb:
      ret = GetCubebDriverNames();
      break;
#endif

    default:
      break;
  }

  return ret;
}

std::vector<AudioStream::DeviceInfo> AudioStream::GetOutputDevices(AudioBackend backend, const char* driver,
                                                                   u32 sample_rate)
{
  std::vector<AudioStream::DeviceInfo> ret;
  switch (backend)
  {
#ifndef __ANDROID__
    case AudioBackend::Cubeb:
      ret = GetCubebOutputDevices(driver, sample_rate);
      break;
#endif

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
#ifndef __ANDROID__
    case AudioBackend::Cubeb:
      return CreateCubebAudioStream(sample_rate, parameters, driver_name, device_name, error);

    case AudioBackend::SDL:
      return CreateSDLAudioStream(sample_rate, parameters, error);
#else
    case AudioBackend::AAudio:
      return CreateAAudioAudioStream(sample_rate, parameters, error);

    case AudioBackend::OpenSLES:
      return CreateOpenSLESAudioStream(sample_rate, parameters, error);
#endif

    case AudioBackend::Null:
      return CreateNullStream(sample_rate, parameters.buffer_ms);

    default:
      Error::SetStringView(error, "Unknown audio backend.");
      return nullptr;
  }
}

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
  TRANSLATE_DISAMBIG_NOOP("Settings", "Null (No Output)", "AudioBackend"),
#ifndef __ANDROID__
  TRANSLATE_DISAMBIG_NOOP("Settings", "Cubeb", "AudioBackend"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "SDL", "AudioBackend"),
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
  TRANSLATE_DISAMBIG_NOOP("Settings", "Disabled (Stereo)", "AudioExpansionMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Stereo with LFE", "AudioExpansionMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Quadraphonic", "AudioExpansionMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Quadraphonic with LFE", "AudioExpansionMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "5.1 Surround", "AudioExpansionMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "7.1 Surround", "AudioExpansionMode"),
};

const char* AudioStream::GetExpansionModeName(AudioExpansionMode mode)
{
  return (static_cast<size_t>(mode) < s_expansion_mode_names.size()) ?
           s_expansion_mode_names[static_cast<size_t>(mode)] :
           "";
}

const char* AudioStream::GetExpansionModeDisplayName(AudioExpansionMode mode)
{
  return (static_cast<size_t>(mode) < s_expansion_mode_display_names.size()) ?
           Host::TranslateToCString("Settings", s_expansion_mode_display_names[static_cast<size_t>(mode)],
                                    "AudioExpansionMode") :
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
  TRANSLATE_DISAMBIG_NOOP("Settings", "Off (Noisy)", "AudioStretchMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Resampling (Pitch Shift)", "AudioStretchMode"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Time Stretch (Tempo Change, Best Sound)", "AudioStretchMode"),
};

const char* AudioStream::GetStretchModeName(AudioStretchMode mode)
{
  return (static_cast<size_t>(mode) < s_stretch_mode_names.size()) ? s_stretch_mode_names[static_cast<size_t>(mode)] :
                                                                     "";
}

const char* AudioStream::GetStretchModeDisplayName(AudioStretchMode mode)
{
  return (static_cast<size_t>(mode) < s_stretch_mode_display_names.size()) ?
           Host::TranslateToCString("Settings", s_stretch_mode_display_names[static_cast<size_t>(mode)],
                                    "AudioStretchMode") :
           "";
}

std::optional<AudioStretchMode> AudioStream::ParseStretchMode(const char* name)
{
  for (size_t i = 0; i < static_cast<u8>(AudioStretchMode::Count); i++)
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
      VERBOSE_LOG("Underrun compensation done ({} frames buffered)", toFill);
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
      m_sample_reader(samples, &m_buffer[rpos * m_output_channels], end);
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

      VERBOSE_LOG("Audio buffer underflow, resampled {} frames to {}", frames_to_read, num_frames);
    }
    else
    {
      // no data, fall back to silence
      std::memset(samples + (frames_to_read * m_output_channels), 0, silence_frames * m_output_channels * sizeof(s16));
    }
  }

  if (m_volume != 100)
  {
    u32 num_samples = num_frames * m_output_channels;

    const u32 aligned_samples = Common::AlignDownPow2(num_samples, 8);
    num_samples -= aligned_samples;

    const float volume_mult = static_cast<float>(m_volume) / 100.0f;
    const GSVector4 volume_multv = GSVector4(volume_mult);
    const SampleType* const aligned_samples_end = samples + aligned_samples;
    for (; samples != aligned_samples_end; samples += 8)
    {
      GSVector4i iv = GSVector4i::load<false>(samples); // [0, 1, 2, 3, 4, 5, 6, 7]
      GSVector4i iv1 = iv.upl16(iv);                    // [0, 0, 1, 1, 2, 2, 3, 3]
      GSVector4i iv2 = iv.uph16(iv);                    // [4, 4, 5, 5, 6, 6, 7, 7]
      iv1 = iv1.sra32<16>();                            // [0, 1, 2, 3]
      iv2 = iv2.sra32<16>();                            // [4, 5, 6, 7]
      GSVector4 fv1 = GSVector4(iv1);                   // [f0, f1, f2, f3]
      GSVector4 fv2 = GSVector4(iv2);                   // [f4, f5, f6, f7]
      fv1 = fv1 * volume_multv;                         // [f0, f1, f2, f3]
      fv2 = fv2 * volume_multv;                         // [f4, f5, f6, f7]
      iv1 = GSVector4i(fv1);                            // [0, 1, 2, 3]
      iv2 = GSVector4i(fv2);                            // [4, 5, 6, 7]
      iv = iv1.ps32(iv2);                               // [0, 1, 2, 3, 4, 5, 6, 7]
      GSVector4i::store<false>(samples, iv);
    }

    while (num_samples > 0)
    {
      *samples = static_cast<s16>(std::clamp(static_cast<float>(*samples) * volume_mult, -32768.0f, 32767.0f));
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
      DEBUG_LOG("Buffer overrun, chunk dropped");
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
    std::memcpy(&m_buffer[wpos * m_output_channels], data, end * m_output_channels * sizeof(SampleType));
    if (start > 0)
      std::memcpy(&m_buffer[0], data + end * m_output_channels, start * m_output_channels * sizeof(SampleType));

    wpos = start;
  }
  else
  {
    // no split
    std::memcpy(&m_buffer[wpos * m_output_channels], data, num_frames * m_output_channels * sizeof(SampleType));
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

  m_buffer = Common::make_unique_aligned_for_overwrite<s16[]>(VECTOR_ALIGNMENT, m_buffer_size * m_output_channels);
  m_staging_buffer = Common::make_unique_aligned_for_overwrite<s16[]>(VECTOR_ALIGNMENT, CHUNK_SIZE * m_output_channels);
  m_float_buffer = Common::make_unique_aligned_for_overwrite<float[]>(VECTOR_ALIGNMENT, CHUNK_SIZE * m_output_channels);

  DEV_LOG(
    "Allocated buffer of {} frames for buffer of {} ms [expansion {} (block size {}), stretch {}, target size {}].",
    m_buffer_size, m_parameters.buffer_ms, GetExpansionModeName(m_parameters.expansion_mode),
    m_parameters.expand_block_size, GetStretchModeName(m_parameters.stretch_mode), m_target_buffer_size);
}

void AudioStream::DestroyBuffer()
{
  m_staging_buffer.reset();
  m_float_buffer.reset();
  m_buffer.reset();
  m_buffer_size = 0;
  m_wpos.store(0, std::memory_order_release);
  m_rpos.store(0, std::memory_order_release);
}

void AudioStream::EmptyBuffer()
{
  if (IsExpansionEnabled())
    ExpandFlush();

  if (IsStretchEnabled())
  {
    soundtouch_clear(m_soundtouch);
    if (m_parameters.stretch_mode == AudioStretchMode::TimeStretch)
      soundtouch_setTempo(m_soundtouch, m_nominal_rate);
  }

  m_wpos.store(m_rpos.load(std::memory_order_acquire), std::memory_order_release);
}

void AudioStream::SetNominalRate(float tempo)
{
  m_nominal_rate = tempo;
  if (m_parameters.stretch_mode == AudioStretchMode::Resample)
    soundtouch_setRate(m_soundtouch, tempo);
  else if (m_parameters.stretch_mode == AudioStretchMode::TimeStretch && m_stretch_inactive)
    soundtouch_setTempo(m_soundtouch, tempo);
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

static void S16ChunkToFloat(const s16* src, float* dst, u32 num_samples)
{
  constexpr GSVector4 S16_TO_FLOAT_V = GSVector4::cxpr(1.0f / 32767.0f);

  const u32 iterations = (num_samples + 7) / 8;
  for (u32 i = 0; i < iterations; i++)
  {
    const GSVector4i sv = GSVector4i::load<true>(src);
    src += 8;

    GSVector4i iv1 = sv.upl16(sv);  // [0, 0, 1, 1, 2, 2, 3, 3]
    GSVector4i iv2 = sv.uph16(sv);  // [4, 4, 5, 5, 6, 6, 7, 7]
    iv1 = iv1.sra32<16>();          // [0, 1, 2, 3]
    iv2 = iv2.sra32<16>();          // [4, 5, 6, 7]
    GSVector4 fv1 = GSVector4(iv1); // [f0, f1, f2, f3]
    GSVector4 fv2 = GSVector4(iv2); // [f4, f5, f6, f7]
    fv1 = fv1 * S16_TO_FLOAT_V;
    fv2 = fv2 * S16_TO_FLOAT_V;

    GSVector4::store<true>(dst + 0, fv1);
    GSVector4::store<true>(dst + 4, fv2);
    dst += 8;
  }
}

static void FloatChunkToS16(s16* dst, const float* src, u32 num_samples)
{
  const GSVector4 FLOAT_TO_S16_V = GSVector4::cxpr(32767.0f);

  const u32 iterations = (num_samples + 7) / 8;
  for (u32 i = 0; i < iterations; i++)
  {
    GSVector4 fv1 = GSVector4::load<true>(src + 0);
    GSVector4 fv2 = GSVector4::load<true>(src + 4);
    src += 8;

    fv1 = fv1 * FLOAT_TO_S16_V;
    fv2 = fv2 * FLOAT_TO_S16_V;
    GSVector4i iv1 = GSVector4i(fv1);
    GSVector4i iv2 = GSVector4i(fv2);

    const GSVector4i iv = iv1.ps32(iv2);
    GSVector4i::store<true>(dst, iv);
    dst += 8;
  }
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

  if (IsExpansionEnabled())
  {
    // StretchWriteBlock() overwrites the staging buffer on output, so we need to copy into the expand buffer first.
    S16ChunkToFloat(m_staging_buffer.get(),
                    &m_expand_inbuf[m_parameters.expand_block_size + (m_expand_buffer_pos * NUM_INPUT_CHANNELS)],
                    CHUNK_SIZE * NUM_INPUT_CHANNELS);

    // Output the corresponding block.
    if (m_expand_has_block)
      StretchWriteBlock(&m_expand_outbuf[m_expand_buffer_pos * m_output_channels]);

    // Decode the next block if we buffered enough.
    m_expand_buffer_pos += CHUNK_SIZE;
    if (m_expand_buffer_pos == m_parameters.expand_block_size)
    {
      m_expand_buffer_pos = 0;
      m_expand_has_block = true;
      ExpandDecode();
    }
  }
  else
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

  m_soundtouch = soundtouch_createInstance();
  soundtouch_setSampleRate(m_soundtouch, m_sample_rate);
  soundtouch_setChannels(m_soundtouch, m_output_channels);

  soundtouch_setSetting(m_soundtouch, SETTING_USE_QUICKSEEK, m_parameters.stretch_use_quickseek);
  soundtouch_setSetting(m_soundtouch, SETTING_USE_AA_FILTER, m_parameters.stretch_use_aa_filter);

  soundtouch_setSetting(m_soundtouch, SETTING_SEQUENCE_MS, m_parameters.stretch_sequence_length_ms);
  soundtouch_setSetting(m_soundtouch, SETTING_SEEKWINDOW_MS, m_parameters.stretch_seekwindow_ms);
  soundtouch_setSetting(m_soundtouch, SETTING_OVERLAP_MS, m_parameters.stretch_overlap_ms);

  if (m_parameters.stretch_mode == AudioStretchMode::Resample)
    soundtouch_setRate(m_soundtouch, m_nominal_rate);
  else
    soundtouch_setTempo(m_soundtouch, m_nominal_rate);

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
  if (m_soundtouch)
  {
    soundtouch_destroyInstance(m_soundtouch);
    m_soundtouch = nullptr;
  }
}

void AudioStream::StretchWriteBlock(const float* block)
{
  if (IsStretchEnabled())
  {
    soundtouch_putSamples(m_soundtouch, block, CHUNK_SIZE);

    u32 tempProgress;
    while (tempProgress = soundtouch_receiveSamples(m_soundtouch, m_float_buffer.get(), CHUNK_SIZE), tempProgress != 0)
    {
      FloatChunkToS16(m_staging_buffer.get(), m_float_buffer.get(), tempProgress * m_output_channels);
      InternalWriteFrames(m_staging_buffer.get(), tempProgress);
    }

    if (m_parameters.stretch_mode == AudioStretchMode::TimeStretch)
      UpdateStretchTempo();
  }
  else
  {
    FloatChunkToS16(m_staging_buffer.get(), block, CHUNK_SIZE * m_output_channels);
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
    VERBOSE_LOG("___ Stretcher is being reset.");
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
      VERBOSE_LOG("=== Stretcher is now inactive.");
      m_stretch_inactive = true;
    }
  }
  else if (!IsInRange(tempo, 1.0f / INACTIVE_BAD_FACTOR, INACTIVE_BAD_FACTOR))
  {
    VERBOSE_LOG("~~~ Stretcher is now active @ tempo {}.", tempo);
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
      VERBOSE_LOG("buffers: {:4d} ms ({:3.0f}%), tempo: {}, comp: {:2.3f}, iters: {}, reset:{}",
                  (ibuffer_usage * 1000u) / m_sample_rate, 100.0f * buffer_usage / base_target_usage, tempo,
                  m_dynamic_target_usage / base_target_usage, iterations, m_stretch_reset);

      last_log_time = now;
      iterations = 0;
    }

    iterations++;
  }

  soundtouch_setTempo(m_soundtouch, tempo);

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

////////////////////////////////////////////////////////////////////////////////////////////////////
// Channel expander based on FreeSurround: https://hydrogenaud.io/index.php/topic,52235.0.html
// Rewritten with vectorization, performance improvements and integration for DuckStation.
////////////////////////////////////////////////////////////////////////////////////////////////////

static constexpr float EXPAND_PI = 3.141592654f;
static constexpr float EXPAND_EPSILON = 0.000001f;
static constexpr GSVector4 EXPAND_VPI = GSVector4::cxpr64(EXPAND_PI);

ALWAYS_INLINE static GSVector4 vsqr(GSVector4 v)
{
  return v.mul64(v);
}

ALWAYS_INLINE static GSVector4 vsqrt(GSVector4 v)
{
#if 0
  // for diff purposes
  return GSVector4::f64(std::sqrt(v.extract64<0>()), std::sqrt(v.extract64<1>()));
#else
  return v.sqrt64();
#endif
}

ALWAYS_INLINE static GSVector4 vatan2(GSVector4 x, GSVector4 y)
{
  return GSVector4::f64(std::atan2(x.extract64<0>(), y.extract64<0>()), std::atan2(x.extract64<1>(), y.extract64<1>()));
}

ALWAYS_INLINE static GSVector4 vlen(GSVector4 x, GSVector4 y)
{
  // TODO: Replace with dot product
  return vsqrt(x.sqr64().add64(y.sqr64()));
}

ALWAYS_INLINE static GSVector4 vsin(GSVector4 v)
{
  return GSVector4::f64(std::sin(v.extract64<0>()), std::sin(v.extract64<1>()));
}

ALWAYS_INLINE static GSVector4 vcos(GSVector4 v)
{
  return GSVector4::f64(std::cos(v.extract64<0>()), std::cos(v.extract64<1>()));
}

ALWAYS_INLINE static GSVector4 vsign(GSVector4 x)
{
  const GSVector4 zero = GSVector4::zero();
  const GSVector4 zeromask = x.eq64(zero);
  const GSVector4 signbit = GSVector4::cxpr64(1.0) | (x & GSVector4::cxpr64(static_cast<u64>(0x8000000000000000ULL)));
  return signbit.blend32(zero, zeromask);
}

ALWAYS_INLINE static GSVector4 vpow(GSVector4 x, double y)
{
  return GSVector4::f64(std::pow(x.extract64<0>(), y), std::pow(x.extract64<1>(), y));
}

ALWAYS_INLINE_RELEASE static GSVector4 vclamp1(GSVector4 v)
{
  return v.max64(GSVector4::cxpr64(-1.0)).min64(GSVector4::cxpr64(1.0));
}

ALWAYS_INLINE_RELEASE static std::pair<GSVector4, GSVector4> GetPolar(GSVector4 a, GSVector4 p)
{
  return {a.mul64(vcos(p)), a.mul64(vsin(p))};
}

ALWAYS_INLINE_RELEASE static GSVector4 EdgeDistance(const GSVector4& a)
{
  // TODO: Replace with rcp (but probably not on arm, due to precision...)
  const GSVector4 tan_a = GSVector4::f64(std::tan(a.extract64<0>()), std::tan(a.extract64<1>()));
  const GSVector4 v0 = vsqrt(GSVector4::cxpr64(1.0).add64(vsqr(tan_a)));
  const GSVector4 v1 = vsqrt(GSVector4::cxpr64(1.0).add64(vsqr(GSVector4::cxpr64(1.0).div64(tan_a))));
  return v0.min64(v1);
}

ALWAYS_INLINE_RELEASE static void TransformDecode(GSVector4 a, GSVector4 p, GSVector4& x, GSVector4& y)
{
  // TODO: pow() instead?
  // clang-format off
  x = vclamp1(GSVector4::cxpr64(1.0047).mul64(a).add64(GSVector4::cxpr64(0.46804).mul64(a).mul64(p).mul64(p).mul64(p)).sub64(GSVector4::cxpr64(0.2042).mul64(a).mul64(p).mul64(p).mul64(p).mul64(p)).add64(
    GSVector4::cxpr64(0.0080586).mul64(a).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p)).sub64(GSVector4::cxpr64(0.0001526).mul64(a).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p)).sub64(
    GSVector4::cxpr64(0.073512).mul64(a).mul64(a).mul64(a).mul64(p)).sub64(GSVector4::cxpr64(0.2499).mul64(a).mul64(a).mul64(a).mul64(p).mul64(p).mul64(p).mul64(p)).add64(
    GSVector4::cxpr64(0.016932).mul64(a).mul64(a).mul64(a).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p)).sub64(
    GSVector4::cxpr64(0.00027707).mul64(a).mul64(a).mul64(a).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p)).add64(
    GSVector4::cxpr64(0.048105).mul64(a).mul64(a).mul64(a).mul64(a).mul64(a).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p)).sub64(
    GSVector4::cxpr64(0.0065947).mul64(a).mul64(a).mul64(a).mul64(a).mul64(a).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p)).add64(
    GSVector4::cxpr64(0.0016006).mul64(a).mul64(a).mul64(a).mul64(a).mul64(a).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p)).sub64(
    GSVector4::cxpr64(0.0071132).mul64(a).mul64(a).mul64(a).mul64(a).mul64(a).mul64(a).mul64(a).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p)).add64(
    GSVector4::cxpr64(0.0022336).mul64(a).mul64(a).mul64(a).mul64(a).mul64(a).mul64(a).mul64(a).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p)).sub64(
    GSVector4::cxpr64(0.0004804).mul64(a).mul64(a).mul64(a).mul64(a).mul64(a).mul64(a).mul64(a).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p)));
  y = vclamp1(GSVector4::cxpr64(0.98592).sub64(GSVector4::cxpr64(0.62237).mul64(p)).add64(GSVector4::cxpr64(0.077875).mul64(p).mul64(p)).sub64(GSVector4::cxpr64(0.0026929).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p)).add64(GSVector4::cxpr64(0.4971).mul64(a).mul64(a).mul64(p)).sub64(
    GSVector4::cxpr64(0.00032124).mul64(a).mul64(a).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p)).add64(
    GSVector4::cxpr64(9.2491e-006).mul64(a).mul64(a).mul64(a).mul64(a).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p).mul64(p)).add64(
    GSVector4::cxpr64(0.051549).mul64(a).mul64(a).mul64(a).mul64(a).mul64(a).mul64(a).mul64(a).mul64(a)).add64(GSVector4::cxpr64(1.0727e-014).mul64(a).mul64(a).mul64(a).mul64(a).mul64(a).mul64(a).mul64(a).mul64(a).mul64(a).mul64(a)));
  // clang-format on
}

ALWAYS_INLINE_RELEASE static void TransformCircularWrap(GSVector4& x, GSVector4& y, double refangle)
{
  // TODO: Make angle int instead? Precompute the below.
  if (refangle == 90.0)
    return;

  refangle = refangle * EXPAND_PI / 180;
  const double baseangle = 90 * EXPAND_PI / 180;

  // TODO: Move to caller. This one doesn't have clamp.
  GSVector4 ang = vatan2(x, y);
  GSVector4 len = vlen(x, y).div64(EdgeDistance(ang));

  const GSVector4 front = ang.mul64(GSVector4::f64(refangle / baseangle));

  // TODO: Replace div -> mul
  const GSVector4 back = EXPAND_VPI.sub64(GSVector4::f64(refangle - 2.0 * EXPAND_PI)
                                            .mul64(EXPAND_VPI.sub64(ang.abs64()))
                                            .mul64(vsign(ang))
                                            .div64(GSVector4::f64(2.0 * EXPAND_PI - baseangle))
                                            .neg64());
  const GSVector4 cwmask = ang.abs64().lt64(GSVector4::f64(baseangle / 2.0));
  ang = back.blend32(front, cwmask);
  len = len.mul64(EdgeDistance(ang));
  x = vclamp1(vsin(ang).mul64(len));
  y = vclamp1(vcos(ang).mul64(len));
}

ALWAYS_INLINE_RELEASE static void TransformFocus(GSVector4& x, GSVector4& y, double focus)
{
  if (focus == 0.0)
    return;

  const GSVector4 ang = vatan2(x, y);
  GSVector4 len = vclamp1(vlen(x, y).div64(EdgeDistance(ang)));
  if (focus > 0.0)
    len = GSVector4::cxpr64(1.0).sub64(vpow(GSVector4::cxpr64(1.0).sub64(len), 1.0 + focus * 20.0));
  else
    len = vpow(len, 1.0 - focus * 20.0);

  len = len.mul64(EdgeDistance(ang));
  x = vclamp1(vsin(ang).mul64(len));
  y = vclamp1(vcos(ang).mul64(len));
}

ALWAYS_INLINE_RELEASE static std::pair<int, int> MapToGrid(GSVector4& x)
{
  const GSVector4 gp = x.add64(GSVector4::cxpr64(1.0))
                         .mul64(GSVector4::cxpr64(0.5))
                         .mul64(GSVector4::cxpr64(static_cast<double>(EXPAND_GRID_RES - 1)));
  const GSVector4 i = GSVector4::cxpr64(static_cast<double>(EXPAND_GRID_RES - 2)).min64(gp.floor64());
  x = gp.sub64(i);

  const GSVector4i ii = i.f64toi32();
  return {ii.extract32<0>(), ii.extract32<1>()};
}

void AudioStream::ExpandAllocate()
{
  if (m_parameters.expansion_mode == AudioExpansionMode::Disabled)
    return;

  m_expand_buffer_pos = 0;
  m_expand_has_block = false;

  const u32 freqdomain_size = (m_parameters.expand_block_size / 2 + 1);
  m_expand_convbuffer =
    Common::make_unique_aligned_for_overwrite<double[]>(VECTOR_ALIGNMENT, m_parameters.expand_block_size * 2);
  m_expand_freqdomain =
    Common::make_unique_aligned_for_overwrite<std::complex<double>[]>(VECTOR_ALIGNMENT, freqdomain_size * 2);

  m_expand_fft = kiss_fftr_alloc(m_parameters.expand_block_size, 0, 0, 0);
  m_expand_ifft = kiss_fftr_alloc(m_parameters.expand_block_size, 1, 0, 0);

  m_expand_inbuf = Common::make_unique_aligned<float[]>(VECTOR_ALIGNMENT, 3 * m_parameters.expand_block_size);
  m_expand_outbuf = Common::make_unique_aligned<float[]>(
    VECTOR_ALIGNMENT, (m_parameters.expand_block_size + m_parameters.expand_block_size / 2) * m_output_channels);
  m_expand_signal =
    Common::make_unique_aligned<std::complex<double>[]>(VECTOR_ALIGNMENT, freqdomain_size * m_output_channels);

  m_expand_window =
    Common::make_unique_aligned_for_overwrite<double[]>(VECTOR_ALIGNMENT, m_parameters.expand_block_size);
  for (unsigned k = 0; k < m_parameters.expand_block_size; k++)
  {
    m_expand_window[k] = std::sqrt(0.5 * (1 - std::cos(2 * EXPAND_PI * k / m_parameters.expand_block_size)) /
                                   m_parameters.expand_block_size);
  }

  m_expand_lfe_low_cutoff =
    (static_cast<float>(m_parameters.expand_low_cutoff) / m_sample_rate * 2.0f) * (m_parameters.expand_block_size / 2);
  m_expand_lfe_high_cutoff =
    (static_cast<float>(m_parameters.expand_high_cutoff) / m_sample_rate * 2.0f) * (m_parameters.expand_block_size / 2);
}

void AudioStream::ExpandFlush()
{
  std::memset(m_expand_inbuf.get(), 0, sizeof(float) * 3 * m_parameters.expand_block_size);
  std::memset(m_expand_outbuf.get(), 0,
              sizeof(float) * (m_parameters.expand_block_size + m_parameters.expand_block_size / 2) *
                m_output_channels);

  m_expand_buffer_pos = 0;
  m_expand_has_block = false;
}

void AudioStream::ExpandDestroy()
{
  if (m_expand_ifft)
  {
    kiss_fftr_free(m_expand_ifft);
    m_expand_ifft = nullptr;
  }

  if (m_expand_fft)
  {
    kiss_fftr_free(m_expand_fft);
    m_expand_fft = nullptr;
  }

  m_expand_window.reset();
  m_expand_signal = {};
  m_expand_outbuf.reset();
  m_expand_inbuf.reset();

  m_expand_right_fd.reset();
  m_expand_freqdomain.reset();
  m_expand_convbuffer.reset();
}

ALWAYS_INLINE static void StoreCplxVec(std::complex<double>* dst, const GSVector4& real, const GSVector4& imag)
{
  const GSVector4 slow = real.upld(imag);
  const GSVector4 shigh = real.uphd(imag);
  GSVector4::store<true>(&dst[0], slow);
  GSVector4::store<true>(&dst[1], shigh);
}

void AudioStream::ExpandDecode()
{
  for (u32 half = 0; half < 2; half++)
  {
    const float* input = &m_expand_inbuf[half ? m_parameters.expand_block_size : 0];
    const u32 block_size = m_parameters.expand_block_size;
    const u32 freqdomain_size = block_size / 2 + 1;
    DebugAssert((block_size % 4) == 0);
    for (unsigned k = 0; k < block_size; k += 2)
    {
      const GSVector4 ivec = GSVector4::load<true>(&input[k * 2]); // L,R,L,R
      const GSVector4 vwnd = GSVector4::load<true>(&m_expand_window[k]);
      GSVector4::store<true>(&m_expand_convbuffer[k], GSVector4::f32to64(ivec.xzzw()).mul64(vwnd));
      GSVector4::store<true>(&m_expand_convbuffer[block_size + k], GSVector4::f32to64(ivec.ywzw()).mul64(vwnd));
    }
    kiss_fftr(m_expand_fft, &m_expand_convbuffer[0], reinterpret_cast<kiss_fft_cpx*>(&m_expand_freqdomain[0]));
    kiss_fftr(m_expand_fft, &m_expand_convbuffer[block_size],
              reinterpret_cast<kiss_fft_cpx*>(&m_expand_freqdomain[freqdomain_size]));

    // TODO: This should all actually be +1, because otherwise the last element isn't computed.
    const ExpansionChannelSetup& csetup = s_expansion_channel_count[static_cast<size_t>(m_parameters.expansion_mode)];
    const u32 non_lfe_channels = csetup.output_channels - static_cast<u8>(csetup.has_lfe);
    const u32 iterations = m_parameters.expand_block_size / 2u;
    for (u32 f = 0; f < iterations; f += 2)
    {
      GSVector4 lf_real, lf_imag, rf_real, rf_imag;
      {
        GSVector4 lf_low = GSVector4::load<true>(&m_expand_freqdomain[f]);
        GSVector4 lf_high = GSVector4::load<true>(&m_expand_freqdomain[f + 1]);
        lf_real = lf_low.upld(lf_high);
        lf_imag = lf_low.uphd(lf_high);
        GSVector4 rf_low = GSVector4::load<true>(&m_expand_freqdomain[freqdomain_size + f]);
        GSVector4 rf_high = GSVector4::load<true>(&m_expand_freqdomain[freqdomain_size + f + 1]);
        rf_real = rf_low.upld(rf_high);
        rf_imag = rf_low.uphd(rf_high);
      }

      const GSVector4 vampL = vlen(lf_real, lf_imag);
      const GSVector4 vampR = vlen(rf_real, rf_imag);
      const GSVector4 vphaseL = vatan2(lf_imag, lf_real);
      const GSVector4 vphaseR = vatan2(rf_imag, rf_real);
      const GSVector4 vampDiff =
        vclamp1(vampR.sub64(vampL)
                  .div64(vampR.add64(vampL))
                  .blend32(GSVector4::zero(), vampL.add64(vampR).lt64(GSVector4::cxpr64(EXPAND_EPSILON))));
      GSVector4 vphaseDiff = vphaseL.sub64(vphaseR).abs64();
      vphaseDiff = vphaseDiff.blend32(GSVector4::cxpr64(2 * EXPAND_PI).sub64(vphaseDiff), vphaseDiff.gt64(EXPAND_VPI));

      // Decode into soundfield position.
      GSVector4 vx, vy;
      TransformDecode(vampDiff, vphaseDiff, vx, vy);
      TransformCircularWrap(vx, vy, m_parameters.expand_circular_wrap);
      vy = vclamp1(vy.sub64(GSVector4::f64(m_parameters.expand_shift)));
      vy = vclamp1(GSVector4::cxpr64(1.0).sub64(
        GSVector4::cxpr64(1.0).sub64(vy).mul64(GSVector4::f64(m_parameters.expand_depth))));
      TransformFocus(vx, vy, m_parameters.expand_focus);

      // TODO: Replace with * 0.5
      vx = vclamp1(vx.mul64(GSVector4::f64(m_parameters.expand_front_separation)
                              .mul64(GSVector4::cxpr64(1.0).add64(vy))
                              .div64(GSVector4::cxpr64(2.0))
                              .add64(GSVector4::cxpr64(m_parameters.expand_rear_separation)
                                       .mul64(GSVector4::cxpr64(1.0).sub64(vy))
                                       .div64(GSVector4::cxpr64(2.0)))));

      // TODO: Move earlier.
      const GSVector4 vamp_total = vlen(vampL, vampR);
      const GSVector4 vphase_of[] = {vphaseL, vatan2(lf_imag.add64(rf_imag), lf_real.add64(rf_real)), vphaseR};
      const auto [p0, p1] = MapToGrid(vx);
      const auto [q0, q1] = MapToGrid(vy);

      GSVector4 vinv_lfe_level = GSVector4::cxpr64(1.0);
      if (csetup.has_lfe && f < m_expand_lfe_high_cutoff)
      {
        const GSVector4 vlfe_level =
          GSVector4::f64((f < m_expand_lfe_high_cutoff) ?
                           ((f < m_expand_lfe_low_cutoff) ?
                              1 :
                              0.5 * (1 + std::cos(EXPAND_PI * (f - m_expand_lfe_low_cutoff) /
                                                  (m_expand_lfe_high_cutoff - m_expand_lfe_low_cutoff)))) :
                           0.0,
                         ((f + 1) < m_expand_lfe_high_cutoff) ?
                           (((f + 1) < m_expand_lfe_low_cutoff) ?
                              1 :
                              0.5 * (1 + std::cos(EXPAND_PI * ((f + 1) - m_expand_lfe_low_cutoff) /
                                                  (m_expand_lfe_high_cutoff - m_expand_lfe_low_cutoff)))) :
                           0.0);
        vinv_lfe_level = GSVector4::cxpr64(1.0).sub64(vlfe_level);

        const auto& [lfereal, lfeimag] = GetPolar(vamp_total, vphase_of[1]);
        StoreCplxVec(&m_expand_signal[non_lfe_channels * freqdomain_size + f], lfereal.mul64(vlfe_level),
                     lfeimag.mul64(vlfe_level));
      }

      for (u32 c = 0; c < non_lfe_channels; c++)
      {
        const ExpandLUT& a = csetup.channel_lut[c];
        const GSVector4 inv_x = GSVector4::cxpr64(1.0).sub64(vx);
        const GSVector4 inv_y = GSVector4::cxpr64(1.0).sub64(vy);
        const GSVector4 w0 = GSVector4::f32to64(GSVector4(a[q0][p0], a[q1][p1]));
        const GSVector4 w1 = GSVector4::f32to64(GSVector4(a[q0][p0 + 1], a[q1][p1 + 1]));
        const GSVector4 w2 = GSVector4::f32to64(GSVector4(a[q0 + 1][p0], a[q1 + 1][p1]));
        const GSVector4 w3 = GSVector4::f32to64(GSVector4(a[q0 + 1][p0 + 1], a[q1 + 1][p1 + 1]));
        const auto& [sreal, simag] = GetPolar(vamp_total.mul64(inv_x.mul64(inv_y)
                                                                 .mul64(w0)
                                                                 .add64(vx.mul64(inv_y).mul64(w1))
                                                                 .add64(inv_x.mul64(vy).mul64(w2))
                                                                 .add64(vx.mul64(vy).mul64(w3))),
                                              vphase_of[1 + csetup.channel_xsf[c]]);

        // Subtract LFE from other channels.
        StoreCplxVec(&m_expand_signal[c * freqdomain_size + f], sreal.mul64(vinv_lfe_level),
                     simag.mul64(vinv_lfe_level));
      }
    }

    std::memmove(&m_expand_outbuf[0], &m_expand_outbuf[m_output_channels * m_parameters.expand_block_size / 2],
                 m_parameters.expand_block_size * m_output_channels * 4);
    std::memset(&m_expand_outbuf[m_output_channels * m_parameters.expand_block_size], 0,
                m_output_channels * 4 * m_parameters.expand_block_size / 2);

    for (u32 c = 0; c < m_output_channels; c++)
    {
      // We computed the DC value to avoid masking stores, so zero it out.
      std::complex<double>* freqdomain = &m_expand_signal[c * freqdomain_size];
      freqdomain[0] = 0;
      kiss_fftri(m_expand_ifft, reinterpret_cast<kiss_fft_cpx*>(freqdomain), &m_expand_convbuffer[0]);

      // TODO: align, vectorize second half.
      for (u32 k = 0; k < m_parameters.expand_block_size; k += 2)
      {
        const GSVector4 add =
          GSVector4::load<true>(&m_expand_window[k]).mul64(GSVector4::load<true>(&m_expand_convbuffer[k]));
        const size_t idx1 = (m_output_channels * (k + m_parameters.expand_block_size / 2) + c);
        const size_t idx2 = (m_output_channels * ((k + 1) + m_parameters.expand_block_size / 2) + c);
        m_expand_outbuf[idx1] = static_cast<float>(m_expand_outbuf[idx1] + add.extract64<0>());
        m_expand_outbuf[idx2] = static_cast<float>(m_expand_outbuf[idx2] + add.extract64<1>());
      }
    }
  }

  std::memcpy(&m_expand_inbuf[0], &m_expand_inbuf[2 * m_parameters.expand_block_size],
              4 * m_parameters.expand_block_size);
}
