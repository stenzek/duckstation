// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "core_audio_stream.h"
#include "translation.h"

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

void AudioStreamParameters::Load(const SettingsInterface& si, const char* section)
{
  stretch_mode =
    CoreAudioStream::ParseStretchMode(
      si.GetStringViewValue(section, "StretchMode", CoreAudioStream::GetStretchModeName(DEFAULT_STRETCH_MODE)))
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
  si.SetStringValue(section, "StretchMode", CoreAudioStream::GetStretchModeName(stretch_mode));
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

CoreAudioStream::CoreAudioStream() = default;

CoreAudioStream::~CoreAudioStream()
{
  Destroy();
}

bool CoreAudioStream::Initialize(AudioBackend backend, u32 sample_rate, const AudioStreamParameters& params,
                                 std::string_view driver_name, std::string_view device_name,
                                 Error* error /* = nullptr */)
{
  Destroy();

  m_sample_rate = sample_rate;
  m_volume.store(100, std::memory_order_relaxed);
  m_parameters = params;
  m_filling = false;
  m_paused = false;

  if (backend == AudioBackend::Null)
  {
    // no point stretching with no output
    m_parameters = AudioStreamParameters();
    m_parameters.stretch_mode = AudioStretchMode::Off;
    m_parameters.buffer_ms = params.buffer_ms;

    // always paused to avoid output
    m_paused = true;
  }

  AllocateBuffer();
  StretchAllocate();

  const u32 output_latency_frames =
    GetBufferSizeForMS(sample_rate, (params.output_latency_ms != 0) ? params.output_latency_ms : params.buffer_ms);
  if (backend != AudioBackend::Null && !(m_stream = AudioStream::CreateStream(
                                           backend, sample_rate, NUM_CHANNELS, output_latency_frames,
                                           params.output_latency_minimal, driver_name, device_name, this, true, error)))
  {
    Destroy();
    return false;
  }

  return true;
}

void CoreAudioStream::Destroy()
{
  m_stream.reset();
  StretchDestroy();
  DestroyBuffer();
  m_sample_rate = 0;
  m_parameters = AudioStreamParameters();
  m_volume.store(0, std::memory_order_relaxed);
  m_filling = false;
  m_paused = true;
}

u32 CoreAudioStream::GetAlignedBufferSize(u32 size)
{
  static_assert(Common::IsPow2(CHUNK_SIZE));
  return Common::AlignUpPow2(size, CHUNK_SIZE);
}

u32 CoreAudioStream::GetBufferSizeForMS(u32 sample_rate, u32 ms)
{
  return GetAlignedBufferSize((ms * sample_rate) / 1000u);
}

u32 CoreAudioStream::GetMSForBufferSize(u32 sample_rate, u32 buffer_size)
{
  buffer_size = GetAlignedBufferSize(buffer_size);
  return (buffer_size * 1000u) / sample_rate;
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

const char* CoreAudioStream::GetStretchModeName(AudioStretchMode mode)
{
  return (static_cast<size_t>(mode) < s_stretch_mode_names.size()) ? s_stretch_mode_names[static_cast<size_t>(mode)] :
                                                                     "";
}

const char* CoreAudioStream::GetStretchModeDisplayName(AudioStretchMode mode)
{
  return (static_cast<size_t>(mode) < s_stretch_mode_display_names.size()) ?
           Host::TranslateToCString("Settings", s_stretch_mode_display_names[static_cast<size_t>(mode)],
                                    "AudioStretchMode") :
           "";
}

std::optional<AudioStretchMode> CoreAudioStream::ParseStretchMode(std::string_view name)
{
  for (size_t i = 0; i < static_cast<u8>(AudioStretchMode::Count); i++)
  {
    if (name == s_stretch_mode_names[i])
      return static_cast<AudioStretchMode>(i);
  }

  return std::nullopt;
}

u32 CoreAudioStream::GetBufferedFramesRelaxed() const
{
  const u32 rpos = m_rpos.load(std::memory_order_relaxed);
  const u32 wpos = m_wpos.load(std::memory_order_relaxed);
  u32 buffered_frames = (wpos + m_buffer_size - rpos) % m_buffer_size;

  // when stretching, the buffer holds input, which plays back at the nominal rate
  if (IsStretchEnabled())
  {
    buffered_frames =
      static_cast<u32>(static_cast<float>(buffered_frames) / m_nominal_rate.load(std::memory_order_relaxed)) +
      m_stretch_buffered_frames.load(std::memory_order_relaxed);
  }

  return buffered_frames;
}

u32 CoreAudioStream::GetTargetSize() const
{
  // Needs to cover at least two callbacks, otherwise the stretcher holds it below what the next callback needs.
  return std::max(m_target_buffer_size + m_extra_target_frames, m_callback_frames.load(std::memory_order_relaxed) * 2);
}

// Fade in/out around underruns, otherwise there'll be a pop.
static constexpr u32 FADE_FRAMES = 128;

static void FadeFrames(CoreAudioStream::SampleType* samples, u32 num_frames, bool fade_in)
{
  for (u32 i = 0; i < num_frames; i++)
  {
    const float gain = static_cast<float>(fade_in ? i : (num_frames - i)) / static_cast<float>(num_frames);
    for (u32 j = 0; j < CoreAudioStream::NUM_CHANNELS; j++, samples++)
      *samples = static_cast<CoreAudioStream::SampleType>(static_cast<float>(*samples) * gain);
  }
}

u32 CoreAudioStream::ReadBufferFrames(SampleType* samples, u32 rpos, u32 num_frames) const
{
  u32 end = m_buffer_size - rpos;
  if (end > num_frames)
    end = num_frames;

  // towards the end of the buffer
  if (end > 0)
  {
    std::memcpy(samples, &m_buffer[rpos * NUM_CHANNELS], end * NUM_CHANNELS * sizeof(SampleType));
    rpos += end;
    rpos = (rpos == m_buffer_size) ? 0 : rpos;
  }

  // after wrapping around
  const u32 start = num_frames - end;
  if (start > 0)
  {
    std::memcpy(&samples[end * NUM_CHANNELS], &m_buffer[0], start * NUM_CHANNELS * sizeof(SampleType));
    rpos = start;
  }

  return rpos;
}

void CoreAudioStream::ReadFrames(SampleType* samples, u32 num_frames)
{
  m_callback_frames.store(num_frames, std::memory_order_relaxed);

  // set by the writer
  const u32 volume = m_volume.load(std::memory_order_relaxed);

  if (IsStretchEnabled())
  {
    StretchReadFrames(samples, num_frames);
    if (volume != 100)
      AudioStream::ApplyVolume(samples, num_frames * NUM_CHANNELS, volume);
    return;
  }

  // acquire the write position, so we see the frames written before it
  u32 rpos = m_rpos.load(std::memory_order_relaxed);
  const u32 available_frames = (m_wpos.load(std::memory_order_acquire) + m_buffer_size - rpos) % m_buffer_size;
  u32 frames_to_read = num_frames;
  u32 silence_frames = 0;
  bool fade_in = false;

  if (m_filling)
  {
    // Need at least a full callback's worth, otherwise we'll immediately underrun again.
    const u32 toFill = std::min(GetAlignedBufferSize(std::max(num_frames, GetTargetSize() / 2)), m_buffer_size / 2);

    if (available_frames < toFill)
    {
      silence_frames = num_frames;
      frames_to_read = 0;
    }
    else
    {
      m_filling = false;
      fade_in = true;
      VERBOSE_LOG("Underrun compensation done ({} frames buffered)", toFill);
    }
  }

  if (available_frames < frames_to_read)
  {
    silence_frames = frames_to_read - available_frames;
    frames_to_read = available_frames;
    m_filling = true;
    VERBOSE_LOG("Audio buffer underflow, {} of {} frames available", available_frames, num_frames);
  }

  if (frames_to_read > 0)
    m_rpos.store(ReadBufferFrames(samples, rpos, frames_to_read), std::memory_order_release);

  if (fade_in)
    FadeFrames(samples, std::min(frames_to_read, FADE_FRAMES), true);

  if (silence_frames > 0)
  {
    const u32 fade_frames = std::min(frames_to_read, FADE_FRAMES);
    FadeFrames(samples + (frames_to_read - fade_frames) * NUM_CHANNELS, fade_frames, false);
    std::memset(samples + (frames_to_read * NUM_CHANNELS), 0, silence_frames * NUM_CHANNELS * sizeof(SampleType));
  }

  if (volume != 100)
    AudioStream::ApplyVolume(samples, num_frames * NUM_CHANNELS, volume);
}

void CoreAudioStream::AllocateBuffer()
{
  // Stretcher can produce a large amount of samples from few samples when running slow, so allocate a larger buffer.
  // In most cases it's not going to be used, but better to have a larger buffer and not need it than overrun.
  // The buffer holds stretcher input, which also arrives faster than real time when running fast.
  const u32 multiplier = (m_parameters.stretch_mode == AudioStretchMode::Off) ? 1 : 16;
  m_buffer_size = GetAlignedBufferSize(((m_parameters.buffer_ms * multiplier) * m_sample_rate) / 1000);
  m_target_buffer_size = GetAlignedBufferSize((m_sample_rate * m_parameters.buffer_ms) / 1000u);

  m_buffer = Common::make_unique_aligned_for_overwrite<s16[]>(VECTOR_ALIGNMENT, m_buffer_size * NUM_CHANNELS);
  m_float_buffer = Common::make_unique_aligned_for_overwrite<float[]>(VECTOR_ALIGNMENT, CHUNK_SIZE * NUM_CHANNELS);

  DEV_LOG("Allocated buffer of {} frames for buffer of {} ms [stretch {}, target size {}].", m_buffer_size,
          m_parameters.buffer_ms, GetStretchModeName(m_parameters.stretch_mode), m_target_buffer_size);
}

void CoreAudioStream::DestroyBuffer()
{
  m_float_buffer.reset();
  m_buffer.reset();
  m_buffer_size = 0;
  m_wpos.store(0, std::memory_order_release);
  m_rpos.store(0, std::memory_order_release);
}

void CoreAudioStream::SetNominalRate(float tempo)
{
  // applied by the audio thread on the next chunk
  m_nominal_rate.store(tempo, std::memory_order_relaxed);
}

void CoreAudioStream::SetPaused(bool paused)
{
  // force state to always be paused if we're a null output
  if (m_paused == paused || !m_stream)
    return;

  Error error;
  if (!(paused ? m_stream->Stop(&error) : m_stream->Start(&error)))
    ERROR_LOG("Failed to {} stream: {}", paused ? "pause" : "restart", error.GetDescription());
  else
    m_paused = paused;
}

void CoreAudioStream::SetOutputVolume(u32 volume)
{
  m_volume.store(volume, std::memory_order_relaxed);
}

bool CoreAudioStream::BeginWrite(SampleType** buffer_ptr, u32* num_frames)
{
  // don't bother committing anything when muted
  if (m_volume.load(std::memory_order_relaxed) == 0 || m_paused)
    return false;

  // acquire the read position, so the reader is done with the frames we're about to overwrite
  const u32 wpos = m_wpos.load(std::memory_order_relaxed);
  const u32 free = m_buffer_size - ((wpos + m_buffer_size - m_rpos.load(std::memory_order_acquire)) % m_buffer_size);

  // leave one frame free, otherwise a full buffer looks empty. the read position belongs to the audio thread, so the
  // caller drops what doesn't fit.
  *buffer_ptr = &m_buffer[wpos * NUM_CHANNELS];
  *num_frames = std::min(m_buffer_size - wpos, free - 1);

  // stretcher should take care of this, only triggers with stretch off
  if (*num_frames == 0) [[unlikely]]
    DEBUG_LOG("Buffer overrun, dropping frames");

  return (*num_frames > 0);
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

void CoreAudioStream::EndWrite(u32 num_frames)
{
  // release the write position, so the reader sees the frames written before it
  u32 wpos = m_wpos.load(std::memory_order_relaxed);
  DebugAssert((wpos + num_frames) <= m_buffer_size);
  wpos += num_frames;
  wpos = (wpos == m_buffer_size) ? 0 : wpos;
  m_wpos.store(wpos, std::memory_order_release);
}

// Time stretching algorithm based on PCSX2 implementation.

template<class T>
ALWAYS_INLINE static bool IsInRange(const T& val, const T& min, const T& max)
{
  return (min <= val && val <= max);
}

void CoreAudioStream::StretchAllocate()
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
    soundtouch_setRate(m_soundtouch, m_nominal_rate.load(std::memory_order_relaxed));
  else
    soundtouch_setTempo(m_soundtouch, m_nominal_rate.load(std::memory_order_relaxed));

  m_stretch_reset = STRETCH_RESET_THRESHOLD;
  m_stretch_reset_time = Timer::GetCurrentValue();
  m_frames_since_underrun = EXTRA_TARGET_DECAY_SECONDS * m_sample_rate;
  m_underrun_gap_frames = std::numeric_limits<u32>::max() / 2; // initial fill isn't a gap
}

void CoreAudioStream::StretchDestroy()
{
  if (m_soundtouch)
  {
    soundtouch_destroyInstance(m_soundtouch);
    m_soundtouch = nullptr;
  }

  m_stretch_reset = 0;
  m_stretch_reset_time = 0;
  m_extra_target_frames = 0;
  m_stretch_reset_request.store(false, std::memory_order_relaxed);
  m_stretch_buffered_frames.store(0, std::memory_order_relaxed);
  m_stretch_inactive = false;
  m_stretch_ok_count = 0;
  m_dynamic_target_usage = 0.0f;
  m_average_tempo = 1.0f;
}

void CoreAudioStream::StretchReadFrames(SampleType* samples, u32 num_frames)
{
  // acquire the write position, so we see the frames written before it
  u32 rpos = m_rpos.load(std::memory_order_relaxed);
  u32 buffered_frames = (m_wpos.load(std::memory_order_acquire) + m_buffer_size - rpos) % m_buffer_size;
  const float nominal_rate = m_nominal_rate.load(std::memory_order_relaxed);

  // Reset the tempo when slowing down, handled below once the queued input has been processed.
  const bool reset_requested = m_stretch_reset_request.exchange(false, std::memory_order_relaxed);

  // Soundtouch holds about tempo * sequence length of input, which is a lot when fast forwarding. Process it at the new
  // speed now, along with the queued input, so that it's dropped below instead of turning into a backlog. The tempo
  // can't change until then, otherwise soundtouch would hold onto the input again.
  if (reset_requested)
  {
    if (m_parameters.stretch_mode == AudioStretchMode::Resample)
    {
      soundtouch_setRate(m_soundtouch, nominal_rate);
      soundtouch_setTempo(m_soundtouch, 1.0f);
    }
    else
    {
      soundtouch_setTempo(m_soundtouch, nominal_rate);
    }

    // If there's enough queued input to refill the target, start over from the newest, instead of processing all of it
    // only for most of it to be dropped.
    const u32 refill_frames =
      GetAlignedBufferSize(static_cast<u32>(static_cast<float>(GetTargetSize() + num_frames) * nominal_rate) +
                           static_cast<u32>(std::max(soundtouch_getSetting(m_soundtouch, SETTING_INITIAL_LATENCY), 0)));
    if (buffered_frames >= refill_frames)
    {
      const u32 discard_frames = Common::AlignDownPow2(buffered_frames - refill_frames, CHUNK_SIZE);
      DEV_LOG("Restarting stretcher, dropping {} frames queued at previous speed", discard_frames);
      soundtouch_clear(m_soundtouch);
      rpos = (rpos + discard_frames) % m_buffer_size;
      buffered_frames -= discard_frames;
      m_rpos.store(rpos, std::memory_order_release);
    }
    else
    {
      soundtouch_putSamples(m_soundtouch, m_float_buffer.get(), 0);
    }
  }

  // If we've fallen far behind, e.g. the audio device stalled, drop the oldest input instead of stretching through it.
  // The target covers at least two callbacks, so this leaves room for normal input between callbacks when running fast.
  // Input arrives at the actual speed, which is above the nominal rate when fast forwarding without a speed limit.
  // Not capped to the buffer size, if the buffer fills the writer drops instead.
  static constexpr u32 MAX_INPUT_TARGET_MULTIPLIER = 8;
  const float input_rate =
    nominal_rate *
    ((m_parameters.stretch_mode == AudioStretchMode::TimeStretch) ? std::max(m_average_tempo, 1.0f) : 1.0f);
  u32 max_input_frames =
    static_cast<u32>(static_cast<float>(GetTargetSize() * MAX_INPUT_TARGET_MULTIPLIER) * std::max(input_rate, 1.0f));

  // If input is arriving faster than the stretcher can go, only take what it can play back, otherwise output builds up.
  // The average is clamped to the maximum, so this is when it's been at the maximum for a while. Not on reset, since
  // both are from the previous speed, and the input kept above is needed to refill.
  const bool tempo_limited = (!reset_requested && m_parameters.stretch_mode == AudioStretchMode::TimeStretch &&
                              m_average_tempo > 1.0f && m_average_tempo >= m_max_tempo);
  if (tempo_limited)
  {
    max_input_frames =
      std::min(max_input_frames, static_cast<u32>(static_cast<float>(num_frames) * nominal_rate * m_max_tempo));
  }

  if (buffered_frames > max_input_frames)
  {
    const u32 discard_frames = Common::AlignUpPow2(buffered_frames - max_input_frames, CHUNK_SIZE);
    if (!tempo_limited)
      DEV_LOG("Stretch input overrun, dropping {} frames", discard_frames);
    rpos = (rpos + discard_frames) % m_buffer_size;
    buffered_frames -= discard_frames;
    m_rpos.store(rpos, std::memory_order_release);
  }

  // Run all input through the stretcher, its output buffer is what the tempo is controlled on.
  if (buffered_frames >= CHUNK_SIZE)
  {
    alignas(VECTOR_ALIGNMENT) SampleType chunk[CHUNK_SIZE * NUM_CHANNELS];
    do
    {
      rpos = ReadBufferFrames(chunk, rpos, CHUNK_SIZE);
      buffered_frames -= CHUNK_SIZE;
      S16ChunkToFloat(chunk, m_float_buffer.get(), CHUNK_SIZE * NUM_CHANNELS);
      soundtouch_putSamples(m_soundtouch, m_float_buffer.get(), CHUNK_SIZE);
      if (!reset_requested)
        UpdateStretchTempo(nominal_rate);
    } while (buffered_frames >= CHUNK_SIZE);

    m_rpos.store(rpos, std::memory_order_release);
  }

  // Stretcher can produce a large amount of samples from few samples when running slow, cap it to the buffer size.
  u32 available_frames = soundtouch_numSamples(m_soundtouch);

  // Input queued at the previous higher speed produces much more output at the new speed, and the output stalling
  // leaves a backlog. Resampling can only adjust the rate slightly, so it can't catch up on either. Drop it down to the
  // target, so playback continues without a gap or a backlog. Not clearing soundtouch, since its output buffer is what
  // we're playing from. Resampling uses the average buffer usage, so that bursts of input don't trigger it.
  static constexpr float MAX_RESAMPLE_AVERAGE_USAGE = 2.0f;
  if (reset_requested ||
      (m_parameters.stretch_mode == AudioStretchMode::Resample && m_average_tempo > MAX_RESAMPLE_AVERAGE_USAGE))
  {
    // keep the target after this callback has read its frames
    const u32 keep_frames = GetTargetSize() + num_frames;
    if (available_frames > keep_frames)
    {
      DEV_LOG("Dropping {} frames queued at previous speed", available_frames - keep_frames);
      soundtouch_receiveSamples(m_soundtouch, nullptr, available_frames - keep_frames);
      available_frames = keep_frames;
    }

    // restart the tempo control from here, not the backlog it saw while processing
    m_stretch_reset = STRETCH_RESET_THRESHOLD;
    m_average_tempo = 1.0f;
  }

  if (available_frames > m_buffer_size)
  {
    if (m_parameters.stretch_mode == AudioStretchMode::TimeStretch)
      StretchOverrun();

    DEBUG_LOG("Stretch buffer overrun, dropping {} frames", available_frames - m_buffer_size);
    soundtouch_receiveSamples(m_soundtouch, nullptr, available_frames - m_buffer_size);
    available_frames = m_buffer_size;
  }

  bool fade_in = false;
  if (m_filling)
  {
    // Need at least a full callback's worth, otherwise we'll immediately underrun again.
    const u32 to_fill = std::min(GetAlignedBufferSize(std::max(num_frames, GetTargetSize() / 2)), m_buffer_size / 2);
    if (available_frames < to_fill)
    {
      std::memset(samples, 0, num_frames * NUM_CHANNELS * sizeof(SampleType));
      m_underrun_gap_frames += num_frames;
      return;
    }

    m_filling = false;
    fade_in = true;
    VERBOSE_LOG("Underrun compensation done ({} frames buffered)", available_frames);

    // Buffer more if short gaps keep happening, the frame timing is too uneven for the buffer. Not for single or long
    // stalls, buffering more wouldn't have helped there. Underruns in quick succession are recovering from the same
    // event, so only count the first. Capped to twice the configured size.
    if (m_parameters.stretch_mode == AudioStretchMode::TimeStretch && m_underrun_gap_frames <= GetTargetSize() &&
        m_frames_since_underrun >= m_sample_rate)
    {
      const u32 max_extra_frames = std::min(m_target_buffer_size, m_buffer_size / 4);
      if (m_frames_since_underrun < EXTRA_TARGET_UNDERRUN_SECONDS * m_sample_rate &&
          m_extra_target_frames < max_extra_frames)
      {
        m_extra_target_frames =
          std::min(m_extra_target_frames + GetAlignedBufferSize(m_target_buffer_size / 4), max_extra_frames);
        DEV_LOG("Repeated underruns, increasing stretch target to {} frames", GetTargetSize());
      }

      m_frames_since_underrun = 0;
    }

    m_underrun_gap_frames = 0;
  }

  // for latency display
  m_stretch_buffered_frames.store(available_frames - std::min(available_frames, num_frames), std::memory_order_relaxed);

  u32 frames_read = 0;
  while (frames_read < num_frames)
  {
    const u32 count =
      soundtouch_receiveSamples(m_soundtouch, m_float_buffer.get(), std::min(num_frames - frames_read, CHUNK_SIZE));
    if (count == 0)
      break;

    // conversion writes whole vectors, so go through an aligned buffer
    alignas(VECTOR_ALIGNMENT) SampleType chunk[CHUNK_SIZE * NUM_CHANNELS];
    FloatChunkToS16(chunk, m_float_buffer.get(), count * NUM_CHANNELS);
    std::memcpy(&samples[frames_read * NUM_CHANNELS], chunk, count * NUM_CHANNELS * sizeof(SampleType));
    frames_read += count;
  }

  if (fade_in)
    FadeFrames(samples, std::min(frames_read, FADE_FRAMES), true);

  if (frames_read < num_frames)
  {
    VERBOSE_LOG("Audio buffer underflow, {} of {} frames available", frames_read, num_frames);
    m_filling = true;
    m_underrun_gap_frames += num_frames - frames_read;

    const u32 fade_frames = std::min(frames_read, FADE_FRAMES);
    FadeFrames(samples + (frames_read - fade_frames) * NUM_CHANNELS, fade_frames, false);
    std::memset(samples + (frames_read * NUM_CHANNELS), 0,
                (num_frames - frames_read) * NUM_CHANNELS * sizeof(SampleType));
  }
  else
  {
    // Drop the extra buffering back down slowly once the timing has been even for a while.
    const u32 decay_frames = EXTRA_TARGET_DECAY_SECONDS * m_sample_rate;
    m_frames_since_underrun = std::min(m_frames_since_underrun + num_frames, decay_frames);
    if (m_extra_target_frames > 0 && m_frames_since_underrun == decay_frames)
    {
      m_extra_target_frames -= std::min(m_extra_target_frames, GetAlignedBufferSize(m_target_buffer_size / 8));
      m_frames_since_underrun = EXTRA_TARGET_UNDERRUN_SECONDS * m_sample_rate;
      DEV_LOG("Reducing stretch target to {} frames", GetTargetSize());
    }
  }
}

void CoreAudioStream::UpdateStretchTempo(float nominal_rate)
{
  static constexpr float MIN_TEMPO = 0.05f;
  static constexpr float MAX_TEMPO = 500.0f;

  // Hysteresis thresholds to prevent stretcher from constantly toggling on/off.
  // i.e. this is the range we will run in 1:1 mode for.
  static constexpr float INACTIVE_GOOD_FACTOR = 1.04f;
  static constexpr float INACTIVE_BAD_FACTOR = 1.2f;

  // Require sustained good performance before deactivating.
  static constexpr u32 INACTIVE_MIN_OK_COUNT = 50;
  static constexpr u32 COMPENSATION_DIVIDER = 100;

  // Same lag and noise reduction as a 50-chunk moving average.
  static constexpr float AVERAGING_FACTOR = 1.0f / 26.0f;

  // Resampling only nudges the tempo to compensate for clock drift, small enough to not be audible.
  // Not the rate, as SoundTouch reorders its pipeline each time the rate crosses 1.0, causing clicks.
  static constexpr float MAX_RESAMPLE_ADJUSTMENT = 0.005f;

  // Output buffered in soundtouch, which takes the place of the buffer.
  const u32 ibuffer_usage = soundtouch_numSamples(m_soundtouch);
  float buffer_usage = static_cast<float>(ibuffer_usage);

  if (m_parameters.stretch_mode == AudioStretchMode::Resample)
  {
    m_average_tempo += (buffer_usage / static_cast<float>(GetTargetSize()) - m_average_tempo) * AVERAGING_FACTOR;
    soundtouch_setRate(m_soundtouch, nominal_rate);
    soundtouch_setTempo(m_soundtouch, 1.0f + std::clamp(m_average_tempo - 1.0f, -1.0f, 1.0f) * MAX_RESAMPLE_ADJUSTMENT);
    return;
  }

  // Controls how aggressively we adjust the dynamic target. We want to keep the same target size regardless
  // of the target speed, but need additional buffering when intentionally running below 100%.
  // The callback minimum doesn't scale with speed, and the target has to stay within the buffer.
  float base_target_usage = std::min(std::max(static_cast<float>(m_target_buffer_size) / std::min(nominal_rate, 1.0f),
                                              static_cast<float>(GetTargetSize())),
                                     static_cast<float>(m_buffer_size / 2));

  // tempo = current_buffer / target_buffer, as a multiplier of the nominal rate.
  float tempo = buffer_usage / m_dynamic_target_usage;

  // Prevents the system from getting stuck in a bad state due to accumulated errors.
  if (m_stretch_reset >= STRETCH_RESET_THRESHOLD)
  {
    VERBOSE_LOG("___ Stretcher is being reset.");
    m_stretch_inactive = false;
    m_stretch_ok_count = 0;
    m_dynamic_target_usage = base_target_usage;
    m_average_tempo = 1.0f;
    m_stretch_reset = 0;
    tempo = 1.0f;
  }
  else if (m_stretch_reset > 0)
  {
    // Back off resets if enough time has passed. That way a very occasional lag/overflow
    // doesn't cascade into unnecessary tempo adjustment.
    const u64 now = Timer::GetCurrentValue();
    if (Timer::ConvertValueToSeconds(now - m_stretch_reset_time) >= 2.0f)
    {
      m_stretch_reset--;
      m_stretch_reset_time = now;
    }
  }

  // Soundtouch holds about tempo * output sequence length of input, keep that within the buffer. Otherwise it grows
  // without bound when the controller overshoots, e.g. when starting fast forward, and slows down as it grows.
  m_max_tempo = std::clamp(
    static_cast<float>(m_buffer_size) /
      (static_cast<float>(std::max(soundtouch_getSetting(m_soundtouch, SETTING_NOMINAL_OUTPUT_SEQUENCE), 1)) *
       nominal_rate),
    1.0f, MAX_TEMPO);

  // Apply temporal smoothing to prevent rapid tempo changes that cause artifacts.
  m_average_tempo = std::min(m_average_tempo + (tempo - m_average_tempo) * AVERAGING_FACTOR, m_max_tempo);
  tempo = m_average_tempo;

  // Apply non-linear dampening when close to target to reduce oscillation.
  if (tempo < 2.0f)
    tempo = std::sqrt(tempo);

  tempo = std::clamp(tempo, MIN_TEMPO, m_max_tempo);

  // Running slower than nominal means input arrives less often, so buffer more. The target has to grow faster than the
  // tempo drops, as the buffer settles at around target * tempo. Only below 90%, as it dips slightly in normal use too.
  if (tempo < 1.0f)
    base_target_usage /= std::sqrt(tempo) * std::min(tempo / 0.9f, 1.0f);

  // Gradually adjust our dynamic target toward what would give us the desired tempo.
  m_dynamic_target_usage +=
    static_cast<float>(base_target_usage / tempo - m_dynamic_target_usage) / static_cast<float>(COMPENSATION_DIVIDER);

  // Snap back to baseline if we're very close.
  if (IsInRange(tempo, 0.9f, 1.1f) &&
      IsInRange(m_dynamic_target_usage, base_target_usage * 0.9f, base_target_usage * 1.1f))
  {
    m_dynamic_target_usage = base_target_usage;
  }

  // Are we changing the active state?
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

  // If we're inactive, we don't want to change the tempo.
  if (m_stretch_inactive)
    tempo = 1.0f;

  if constexpr (LOG_TIMESTRETCH_STATS)
  {
    static float min_tempo = 0.0f;
    static float max_tempo = 0.0f;
    static float acc_tempo = 0.0f;
    static u32 acc_cnt = 0;
    acc_tempo += tempo;
    acc_cnt++;
    min_tempo = std::min(min_tempo, tempo);
    max_tempo = std::max(max_tempo, tempo);

    static int iterations = 0;
    static u64 last_log_time = 0;

    const u64 now = Timer::GetCurrentValue();

    if (Timer::ConvertValueToSeconds(now - last_log_time) > 1.0f)
    {
      const float avg_tempo = (acc_cnt > 0) ? (acc_tempo / static_cast<float>(acc_cnt)) : 0.0f;

      VERBOSE_LOG("{:3d} ms ({:3.0f}%), tempo: avg={:.2f} min={:.2f} max={:.2f}, comp: {:2.3f}, iters: {}, reset:{}",
                  (ibuffer_usage * 1000u) / m_sample_rate, 100.0f * buffer_usage / base_target_usage, avg_tempo,
                  min_tempo, max_tempo, m_dynamic_target_usage / base_target_usage, iterations, m_stretch_reset);

      last_log_time = now;
      iterations = 0;

      min_tempo = std::numeric_limits<float>::max();
      max_tempo = std::numeric_limits<float>::lowest();
      acc_tempo = 0.0f;
      acc_cnt = 0;
    }

    iterations++;
  }

  soundtouch_setTempo(m_soundtouch, nominal_rate * tempo);
}

void CoreAudioStream::StretchOverrun()
{
  // Produced more frames than can fit in the buffer.
  m_stretch_reset++;
  if (m_stretch_reset < STRETCH_RESET_THRESHOLD)
    m_stretch_reset_time = Timer::GetCurrentValue();
}

void CoreAudioStream::EmptyStretchBuffers()
{
  if (!IsStretchEnabled())
    return;

  // handled on the audio thread
  m_stretch_reset_request.store(true, std::memory_order_relaxed);
}
