#include "audio_stream.h"
#include "SoundTouch.h"
#include "common/align.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/make_array.h"
#include "common/timer.h"
#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef __APPLE__
#include <stdlib.h> // alloca
#else
#include <malloc.h> // alloca
#endif

#if defined(_M_ARM64)
#include <arm64_neon.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#elif defined(_M_IX86) || defined(_M_AMD64)
#include <emmintrin.h>
#endif

Log_SetChannel(AudioStream);

static constexpr bool LOG_TIMESTRETCH_STATS = false;

AudioStream::AudioStream(u32 sample_rate, u32 channels, u32 buffer_ms, AudioStretchMode stretch)
  : m_sample_rate(sample_rate), m_channels(channels), m_buffer_ms(buffer_ms), m_stretch_mode(stretch)
{
}

AudioStream::~AudioStream()
{
  DestroyBuffer();
}

std::unique_ptr<AudioStream> AudioStream::CreateNullStream(u32 sample_rate, u32 channels, u32 buffer_ms)
{
  std::unique_ptr<AudioStream> stream(new AudioStream(sample_rate, channels, buffer_ms, AudioStretchMode::Off));
  stream->BaseInitialize();
  return stream;
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

static constexpr const auto s_stretch_mode_names = make_array("None", "Resample", "TimeStretch");
static constexpr const auto s_stretch_mode_display_names = make_array("None", "Resampling", "Time Stretching");

const char* AudioStream::GetStretchModeName(AudioStretchMode mode)
{
  return (static_cast<u32>(mode) < s_stretch_mode_names.size()) ? s_stretch_mode_names[static_cast<u32>(mode)] : "";
}

const char* AudioStream::GetStretchModeDisplayName(AudioStretchMode mode)
{
  return (static_cast<u32>(mode) < s_stretch_mode_display_names.size()) ?
           s_stretch_mode_display_names[static_cast<u32>(mode)] :
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

void AudioStream::ReadFrames(s16* bData, u32 nFrames)
{
  const u32 available_frames = GetBufferedFramesRelaxed();
  u32 frames_to_read = nFrames;
  u32 silence_frames = 0;

  if (m_filling)
  {
    u32 toFill = m_buffer_size / ((m_stretch_mode != AudioStretchMode::TimeStretch) ? 32 : 400);
    toFill = GetAlignedBufferSize(toFill);

    if (available_frames < toFill)
    {
      silence_frames = nFrames;
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

    if (m_stretch_mode == AudioStretchMode::TimeStretch)
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
      std::memcpy(bData, &m_buffer[rpos], sizeof(s32) * end);
      rpos += end;
      rpos = (rpos == m_buffer_size) ? 0 : rpos;
    }

    // after wrapping around
    const u32 start = frames_to_read - end;
    if (start > 0)
    {
      std::memcpy(&bData[end * 2], &m_buffer[0], sizeof(s32) * start);
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
        static_cast<u32>(65536.0f * (static_cast<float>(frames_to_read / m_channels) / static_cast<float>(nFrames)));

      s16* resample_ptr = static_cast<s16*>(alloca(sizeof(s16) * frames_to_read));
      std::memcpy(resample_ptr, bData, sizeof(s16) * frames_to_read);

      s16* out_ptr = bData;
      const u32 copy_stride = sizeof(SampleType) * m_channels;
      u32 resample_subpos = 0;
      for (u32 i = 0; i < nFrames; i++)
      {
        std::memcpy(out_ptr, resample_ptr, copy_stride);
        out_ptr += m_channels;

        resample_subpos += increment;
        resample_ptr += (resample_subpos >> 16) * m_channels;
        resample_subpos %= 65536u;
      }

      Log_VerbosePrintf("Audio buffer underflow, resampled %u frames to %u", frames_to_read, nFrames);
    }
    else
    {
      // no data, fall back to silence
      std::memset(bData + frames_to_read, 0, sizeof(s32) * silence_frames);
    }
  }
}

void AudioStream::InternalWriteFrames(s32* bData, u32 nSamples)
{
  const u32 free = m_buffer_size - GetBufferedFramesRelaxed();
  if (free <= nSamples)
  {
    if (m_stretch_mode == AudioStretchMode::TimeStretch)
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
  if ((m_buffer_size - wpos) <= nSamples)
  {
    // needs to be written in two parts
    const u32 end = m_buffer_size - wpos;
    const u32 start = nSamples - end;

    // start is zero when this chunk reaches exactly the end
    std::memcpy(&m_buffer[wpos], bData, end * sizeof(s32));
    if (start > 0)
      std::memcpy(&m_buffer[0], bData + end, start * sizeof(s32));

    wpos = start;
  }
  else
  {
    // no split
    std::memcpy(&m_buffer[wpos], bData, nSamples * sizeof(s32));
    wpos += nSamples;
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
  const u32 multplier =
    (m_stretch_mode == AudioStretchMode::TimeStretch) ? 16 : ((m_stretch_mode == AudioStretchMode::Off) ? 1 : 2);
  m_buffer_size = GetAlignedBufferSize(((m_buffer_ms * multplier) * m_sample_rate) / 1000);
  m_target_buffer_size = GetAlignedBufferSize((m_sample_rate * m_buffer_ms) / 1000u);
  m_buffer = std::unique_ptr<s32[]>(new s32[m_buffer_size]);
  Log_DevPrintf("Allocated buffer of %u frames for buffer of %u ms [stretch %s, target size %u].", m_buffer_size,
                m_buffer_ms, GetStretchModeName(m_stretch_mode), m_target_buffer_size);
}

void AudioStream::DestroyBuffer()
{
  m_buffer.reset();
  m_buffer_size = 0;
  m_wpos.store(0, std::memory_order_release);
  m_rpos.store(0, std::memory_order_release);
}

void AudioStream::EmptyBuffer()
{
  if (m_stretch_mode != AudioStretchMode::Off)
  {
    m_soundtouch->clear();
    if (m_stretch_mode == AudioStretchMode::TimeStretch)
      m_soundtouch->setTempo(m_nominal_rate);
  }

  m_wpos.store(m_rpos.load(std::memory_order_acquire), std::memory_order_release);
}

void AudioStream::SetNominalRate(float tempo)
{
  m_nominal_rate = tempo;
  if (m_stretch_mode == AudioStretchMode::Resample)
    m_soundtouch->setRate(tempo);
}

void AudioStream::UpdateTargetTempo(float tempo)
{
  if (m_stretch_mode != AudioStretchMode::TimeStretch)
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
  if (m_stretch_mode == mode)
    return;

  // can't resize the buffers while paused
  bool paused = m_paused;
  if (!paused)
    SetPaused(true);

  DestroyBuffer();
  StretchDestroy();
  m_stretch_mode = mode;

  AllocateBuffer();
  if (m_stretch_mode != AudioStretchMode::Off)
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
  *buffer_ptr = reinterpret_cast<s16*>(&m_staging_buffer[m_staging_buffer_pos]);
  *num_frames = CHUNK_SIZE - m_staging_buffer_pos;
}

void AudioStream::WriteFrames(const SampleType* frames, u32 num_frames)
{
  Panic("not implemented");
}

void AudioStream::EndWrite(u32 num_frames)
{
  // don't bother committing anything when muted
  if (m_volume == 0)
    return;

  m_staging_buffer_pos += num_frames;
  DebugAssert(m_staging_buffer_pos <= CHUNK_SIZE);
  if (m_staging_buffer_pos < CHUNK_SIZE)
    return;

  m_staging_buffer_pos = 0;

  if (m_stretch_mode != AudioStretchMode::Off)
    StretchWrite();
  else
    InternalWriteFrames(m_staging_buffer.data(), CHUNK_SIZE);
}

static constexpr float S16_TO_FLOAT = 1.0f / 32767.0f;
static constexpr float FLOAT_TO_S16 = 32767.0f;

#if defined(_M_ARM64) || defined(__aarch64__)

static void S16ChunkToFloat(const s32* src, float* dst)
{
  static_assert((AudioStream::CHUNK_SIZE % 4) == 0);
  constexpr u32 iterations = AudioStream::CHUNK_SIZE / 4;

  const float32x4_t S16_TO_FLOAT_V = vdupq_n_f32(S16_TO_FLOAT);

  for (u32 i = 0; i < iterations; i++)
  {
    const int16x8_t sv = vreinterpretq_s16_s32(vld1q_s32(src));
    src += 4;

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

static void FloatChunkToS16(s32* dst, const float* src, uint size)
{
  static_assert((AudioStream::CHUNK_SIZE % 4) == 0);
  constexpr u32 iterations = AudioStream::CHUNK_SIZE / 4;

  const float32x4_t FLOAT_TO_S16_V = vdupq_n_f32(FLOAT_TO_S16);

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
    vst1q_s32(dst, vreinterpretq_s32_s16(iv));
    dst += 4;
  }
}

#elif defined(_M_IX86) || defined(_M_AMD64)

static void S16ChunkToFloat(const s32* src, float* dst)
{
  static_assert((AudioStream::CHUNK_SIZE % 4) == 0);
  constexpr u32 iterations = AudioStream::CHUNK_SIZE / 4;

  const __m128 S16_TO_FLOAT_V = _mm_set1_ps(S16_TO_FLOAT);

  for (u32 i = 0; i < iterations; i++)
  {
    const __m128i sv = _mm_load_si128(reinterpret_cast<const __m128i*>(src));
    src += 4;

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

static void FloatChunkToS16(s32* dst, const float* src, uint size)
{
  static_assert((AudioStream::CHUNK_SIZE % 4) == 0);
  constexpr u32 iterations = AudioStream::CHUNK_SIZE / 4;

  const __m128 FLOAT_TO_S16_V = _mm_set1_ps(FLOAT_TO_S16);

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
    dst += 4;
  }
}

#else

static void S16ChunkToFloat(const s32* src, float* dst)
{
  for (uint i = 0; i < AudioStream::CHUNK_SIZE; ++i)
  {
    *(dst++) = static_cast<float>(static_cast<s16>((u32)*src)) / 32767.0f;
    *(dst++) = static_cast<float>(static_cast<s16>(((u32)*src) >> 16)) / 32767.0f;
    src++;
  }
}

static void FloatChunkToS16(s32* dst, const float* src, uint size)
{
  for (uint i = 0; i < size; ++i)
  {
    const s16 left = static_cast<s16>((*(src++) * 32767.0f));
    const s16 right = static_cast<s16>((*(src++) * 32767.0f));
    *(dst++) = (static_cast<u32>(left) & 0xFFFFu) | (static_cast<u32>(right) << 16);
  }
}
#endif

// Time stretching algorithm based on PCSX2 implementation.

template<class T>
ALWAYS_INLINE static bool IsInRange(const T& val, const T& min, const T& max)
{
  return (min <= val && val <= max);
}

void AudioStream::StretchAllocate()
{
  if (m_stretch_mode == AudioStretchMode::Off)
    return;

  m_soundtouch = std::make_unique<soundtouch::SoundTouch>();
  m_soundtouch->setSampleRate(m_sample_rate);
  m_soundtouch->setChannels(m_channels);

  m_soundtouch->setSetting(SETTING_USE_QUICKSEEK, 0);
  m_soundtouch->setSetting(SETTING_USE_AA_FILTER, 0);

  m_soundtouch->setSetting(SETTING_SEQUENCE_MS, 30);
  m_soundtouch->setSetting(SETTING_SEEKWINDOW_MS, 20);
  m_soundtouch->setSetting(SETTING_OVERLAP_MS, 10);

  if (m_stretch_mode == AudioStretchMode::Resample)
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

void AudioStream::StretchWrite()
{
  S16ChunkToFloat(m_staging_buffer.data(), m_float_buffer.data());

  m_soundtouch->putSamples(m_float_buffer.data(), CHUNK_SIZE);

  int tempProgress;
  while (tempProgress = m_soundtouch->receiveSamples((float*)m_float_buffer.data(), CHUNK_SIZE), tempProgress != 0)
  {
    FloatChunkToS16(m_staging_buffer.data(), m_float_buffer.data(), tempProgress);
    InternalWriteFrames(m_staging_buffer.data(), tempProgress);
  }

  if (m_stretch_mode == AudioStretchMode::TimeStretch)
    UpdateStretchTempo();
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
