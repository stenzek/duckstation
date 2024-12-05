// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "audio_stream.h"
#include "host.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/error.h"
#include "common/gsvector.h"
#include "common/log.h"
#include "common/settings_interface.h"
#include "common/timer.h"

#include "soundtouch/SoundTouch.h"
#include "soundtouch/SoundTouchDLL.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

LOG_CHANNEL(AudioStream);

static constexpr bool LOG_TIMESTRETCH_STATS = false;

AudioStream::DeviceInfo::DeviceInfo(std::string name_, std::string display_name_, u32 minimum_latency_)
  : name(std::move(name_)), display_name(std::move(display_name_)), minimum_latency_frames(minimum_latency_)
{
}

AudioStream::DeviceInfo::~DeviceInfo() = default;

void AudioStreamParameters::Load(const SettingsInterface& si, const char* section)
{
  stretch_mode =
    AudioStream::ParseStretchMode(
      si.GetStringValue(section, "StretchMode", AudioStream::GetStretchModeName(DEFAULT_STRETCH_MODE)).c_str())
      .value_or(DEFAULT_STRETCH_MODE);
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
}

void AudioStreamParameters::Save(SettingsInterface& si, const char* section) const
{
  si.SetStringValue(section, "StretchMode", AudioStream::GetStretchModeName(stretch_mode));
  si.SetUIntValue(section, "BufferMS", buffer_ms);
  si.SetUIntValue(section, "OutputLatencyMS", output_latency_ms);
  si.SetBoolValue(section, "OutputLatencyMinimal", output_latency_minimal);

  si.SetUIntValue(section, "StretchSequenceLengthMS", stretch_sequence_length_ms);
  si.SetUIntValue(section, "StretchSeekWindowMS", stretch_seekwindow_ms);
  si.SetUIntValue(section, "StretchOverlapMS", stretch_overlap_ms);
  si.SetBoolValue(section, "StretchUseQuickSeek", stretch_use_quickseek);
  si.SetBoolValue(section, "StretchUseAAFilter", stretch_use_aa_filter);
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
  : m_sample_rate(sample_rate), m_parameters(parameters)
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
  params.stretch_mode = AudioStretchMode::Off;
  params.buffer_ms = static_cast<u16>(buffer_ms);

  std::unique_ptr<AudioStream> stream(new AudioStream(sample_rate, params));
  stream->BaseInitialize();
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
      std::memcpy(samples, &m_buffer[rpos * NUM_CHANNELS], end * NUM_CHANNELS * sizeof(SampleType));
      rpos += end;
      rpos = (rpos == m_buffer_size) ? 0 : rpos;
    }

    // after wrapping around
    const u32 start = frames_to_read - end;
    if (start > 0)
    {
      std::memcpy(&samples[end * NUM_CHANNELS], &m_buffer[0], start * NUM_CHANNELS * sizeof(SampleType));
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
        static_cast<SampleType*>(alloca(frames_to_read * NUM_CHANNELS * sizeof(SampleType)));
      std::memcpy(resample_ptr, samples, frames_to_read * NUM_CHANNELS * sizeof(SampleType));

      SampleType* out_ptr = samples;
      const u32 copy_stride = sizeof(SampleType) * NUM_CHANNELS;
      u32 resample_subpos = 0;
      for (u32 i = 0; i < num_frames; i++)
      {
        std::memcpy(out_ptr, resample_ptr, copy_stride);
        out_ptr += NUM_CHANNELS;

        resample_subpos += increment;
        resample_ptr += (resample_subpos >> 16) * NUM_CHANNELS;
        resample_subpos %= 65536u;
      }

      VERBOSE_LOG("Audio buffer underflow, resampled {} frames to {}", frames_to_read, num_frames);
    }
    else
    {
      // no data, fall back to silence
      std::memset(samples + (frames_to_read * NUM_CHANNELS), 0, silence_frames * NUM_CHANNELS * sizeof(s16));
    }
  }

  if (m_volume != 100)
  {
    u32 num_samples = num_frames * NUM_CHANNELS;

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
    std::memcpy(&m_buffer[wpos * NUM_CHANNELS], data, end * NUM_CHANNELS * sizeof(SampleType));
    if (start > 0)
      std::memcpy(&m_buffer[0], data + end * NUM_CHANNELS, start * NUM_CHANNELS * sizeof(SampleType));

    wpos = start;
  }
  else
  {
    // no split
    std::memcpy(&m_buffer[wpos * NUM_CHANNELS], data, num_frames * NUM_CHANNELS * sizeof(SampleType));
    wpos += num_frames;
  }

  m_wpos.store(wpos, std::memory_order_release);
}

void AudioStream::BaseInitialize()
{
  AllocateBuffer();
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

  m_buffer = Common::make_unique_aligned_for_overwrite<s16[]>(VECTOR_ALIGNMENT, m_buffer_size * NUM_CHANNELS);
  m_staging_buffer = Common::make_unique_aligned_for_overwrite<s16[]>(VECTOR_ALIGNMENT, CHUNK_SIZE * NUM_CHANNELS);
  m_float_buffer = Common::make_unique_aligned_for_overwrite<float[]>(VECTOR_ALIGNMENT, CHUNK_SIZE * NUM_CHANNELS);

  DEV_LOG("Allocated buffer of {} frames for buffer of {} ms [stretch {}, target size {}].", m_buffer_size,
          m_parameters.buffer_ms, GetStretchModeName(m_parameters.stretch_mode), m_target_buffer_size);
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
  *num_frames = CHUNK_SIZE - (m_staging_buffer_pos / NUM_CHANNELS);
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

  m_staging_buffer_pos += num_frames * NUM_CHANNELS;
  DebugAssert(m_staging_buffer_pos <= (CHUNK_SIZE * NUM_CHANNELS));
  if ((m_staging_buffer_pos / NUM_CHANNELS) < CHUNK_SIZE)
    return;

  m_staging_buffer_pos = 0;

  if (!IsStretchEnabled())
  {
    InternalWriteFrames(m_staging_buffer.get(), CHUNK_SIZE);
    return;
  }

  S16ChunkToFloat(m_staging_buffer.get(), m_float_buffer.get(), CHUNK_SIZE * NUM_CHANNELS);
  StretchWriteBlock(m_float_buffer.get());
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
  soundtouch_setChannels(m_soundtouch, NUM_CHANNELS);

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
      FloatChunkToS16(m_staging_buffer.get(), m_float_buffer.get(), tempProgress * NUM_CHANNELS);
      InternalWriteFrames(m_staging_buffer.get(), tempProgress);
    }

    if (m_parameters.stretch_mode == AudioStretchMode::TimeStretch)
      UpdateStretchTempo();
  }
  else
  {
    FloatChunkToS16(m_staging_buffer.get(), block, CHUNK_SIZE * NUM_CHANNELS);
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

    const u64 now = Timer::GetCurrentValue();

    if (Timer::ConvertValueToSeconds(now - last_log_time) > 1.0f)
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
