// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "sound_effect_manager.h"
#include "host.h"
#include "settings.h"

#include "util/audio_stream.h"
#include "util/wav_reader_writer.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/error.h"
#include "common/gsvector.h"
#include "common/heap_array.h"
#include "common/log.h"
#include "common/lru_cache.h"
#include "common/path.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <variant>
#include <vector>

LOG_CHANNEL(SoundEffectManager);

typedef struct SpeexResamplerState_ SpeexResamplerState;

namespace SoundEffectManager {

static constexpr u32 SAMPLE_RATE = 44100;
static constexpr u32 NUM_CHANNELS = 2;
static constexpr u32 OUTPUT_LATENCY_FRAMES = 2048;
static constexpr u32 BYTES_PER_FRAME = NUM_CHANNELS * sizeof(AudioStream::SampleType);
static constexpr u32 SILENCE_TIMEOUT_SECONDS = 10;
static constexpr u32 SILENCE_TIMEOUT_FRAMES = SAMPLE_RATE * SILENCE_TIMEOUT_SECONDS;
static constexpr u32 MAX_CACHE_SIZE = 32;

namespace {

class EffectAudioStream final : public AudioStreamSource
{
public:
  void ReadFrames(SampleType* samples, u32 num_frames) override;
};

using FrameArray = DynamicHeapArray<AudioStream::SampleType>;

struct CachedEffect
{
  FrameArray frames;
};
using CachedEffectPtr = std::shared_ptr<CachedEffect>;

struct PlayingCachedEffect
{
  CachedEffectPtr effect;
  u32 current_frame = 0;
};

struct SpeexResamplerStateDeleter
{
  void operator()(SpeexResamplerState* state) const;
};
using SpeexResamplerStatePtr = std::unique_ptr<SpeexResamplerState, SpeexResamplerStateDeleter>;

struct ResampledStreamedEffect
{
  static constexpr u32 INPUT_BUFFER_SIZE =
    (4096 - sizeof(WAVReader) - sizeof(SpeexResamplerStatePtr) - sizeof(u32) - sizeof(u32)) / NUM_CHANNELS /
    sizeof(u16);

  ResampledStreamedEffect(WAVReader&& reader_, SpeexResamplerStatePtr&& resampler_state_);

  WAVReader reader;
  SpeexResamplerStatePtr resampler_state;
  u32 input_buffer_size = 0;
  u32 input_buffer_pos = 0;
  std::array<AudioStream::SampleType, INPUT_BUFFER_SIZE * NUM_CHANNELS> input_buffer;
};
static_assert(sizeof(ResampledStreamedEffect) == 4096);
using PlayingResampledEffect = std::unique_ptr<ResampledStreamedEffect>;

using PlayingStreamedEffect = WAVReader;
using ActiveSoundEntry = std::variant<PlayingStreamedEffect, PlayingCachedEffect, PlayingResampledEffect>;

struct Locals
{
  std::mutex state_mutex;
  std::deque<ActiveSoundEntry> active_sounds;
  std::unique_ptr<AudioStream> audio_stream;
  DynamicHeapArray<AudioStream::SampleType> temp_buffer;
  LRUCache<std::string, CachedEffectPtr> effect_cache{MAX_CACHE_SIZE};
  EffectAudioStream audio_stream_source;
  u32 silence_frames = 0;
  bool stream_started = false;
  bool stream_stop_pending = false;
};

} // namespace

/// Returns true if a stream has been created.
static bool LockedIsInitialized();

/// Loads a WAV file into a cached effect.
static bool LoadCachedEffect(const std::string& resource_name, const CachedEffectPtr& effect, Error* error);

/// Looks up a cached effect, loading it if necessary.
static const CachedEffectPtr* LookupOrLoadCachedEffect(std::string resource_name, std::unique_lock<std::mutex>& lock);

/// Opens a WAV file for streaming, checking that it matches the correct format.
static bool OpenFileForStreaming(const char* path, WAVReader* reader, Error* error);

/// Ensures the audio stream has been started.
static bool EnsureStreamStarted();

/// Reads frames from an active sound entry.
static u32 ReadEntryFrames(ActiveSoundEntry& entry, AudioStream::SampleType* samples, u32 num_frames, bool mix);
static u32 ReadEntryFrames(PlayingCachedEffect& effect, AudioStream::SampleType* samples, u32 num_frames, bool mix);
static u32 ReadEntryFrames(PlayingStreamedEffect& effect, AudioStream::SampleType* samples, u32 num_frames, bool mix);
static u32 ReadEntryFrames(PlayingResampledEffect& effect, AudioStream::SampleType* samples, u32 num_frames, bool mix);

/// Mixes multiple active sounds into the destination buffer.
static void MixFrames(AudioStream::SampleType* dest, const AudioStream::SampleType* src, u32 num_frames);

/// Stops the stream if there are no active sounds.
static void StopStreamIfInactive();

static void ConvertToStereo(std::span<const AudioStream::SampleType> src, std::span<AudioStream::SampleType> dst,
                            u32 in_channels);

static bool ConvertFrames(CachedEffect& effect, u32 in_sample_rate, u32 in_channels, u32 in_frames, Error* error);

static PlayingResampledEffect CreateResampledStreamedEffect(WAVReader&& reader, Error* error);

ALIGN_TO_CACHE_LINE static Locals s_locals;

} // namespace SoundEffectManager

bool SoundEffectManager::IsInitialized()
{
  std::lock_guard lock(s_locals.state_mutex);
  return LockedIsInitialized();
}

bool SoundEffectManager::LockedIsInitialized()
{
  return (s_locals.audio_stream != nullptr);
}

void SoundEffectManager::EnsureInitialized()
{
  std::lock_guard lock(s_locals.state_mutex);
  if (s_locals.audio_stream)
    return;

  Error error;
  s_locals.audio_stream = AudioStream::CreateStream(
    g_settings.audio_backend, SAMPLE_RATE, NUM_CHANNELS, OUTPUT_LATENCY_FRAMES, false, g_settings.audio_driver,
    g_settings.audio_output_device, &s_locals.audio_stream_source, false, &error);
  if (!s_locals.audio_stream)
  {
    ERROR_LOG("Failed to create stream: {}", error.GetDescription());
    return;
  }

  INFO_COLOR_LOG(StrongGreen, "Created audio stream");
}

void SoundEffectManager::Shutdown()
{
  std::lock_guard lock(s_locals.state_mutex);
  if (!s_locals.audio_stream)
    return;

  INFO_COLOR_LOG(StrongGreen, "Closing audio stream");
  decltype(s_locals.active_sounds)().swap(s_locals.active_sounds);
  s_locals.audio_stream.reset();
  s_locals.stream_started = false;
  s_locals.silence_frames = 0;
}

void SoundEffectManager::EnqueueSoundEffect(std::string_view name)
{
#if 0
  // for testing streaming
  if constexpr (true)
  {
    StreamSoundEffect(EmuFolders::GetOverridableResourcePath(name));
    return;
  }
#endif

  std::lock_guard lock(s_locals.state_mutex);
  if (!LockedIsInitialized())
    return;

  Host::QueueAsyncTask([name = std::string(name)]() mutable {
    std::unique_lock lock(s_locals.state_mutex);
    if (!LockedIsInitialized())
      return;

    const CachedEffectPtr* effect = LookupOrLoadCachedEffect(std::move(name), lock);
    if (effect && !(*effect)->frames.empty() && EnsureStreamStarted())
    {
      s_locals.active_sounds.emplace_back(PlayingCachedEffect{*effect, 0u});
      DEBUG_LOG("{} effects active", s_locals.active_sounds.size());
    }
  });
}

bool SoundEffectManager::LoadCachedEffect(const std::string& resource_name, const CachedEffectPtr& effect, Error* error)
{
  std::optional<DynamicHeapArray<u8>> resource_data = Host::ReadResourceFile(resource_name, true, error);
  if (!resource_data.has_value())
    return false;

  const std::optional<WAVReader::MemoryParseResult> parsed =
    WAVReader::ParseMemory(resource_data->data(), resource_data->size(), error);
  if (!parsed.has_value())
    return false;

  effect->frames.resize(parsed->num_frames * parsed->num_channels);
  if (parsed->num_frames > 0)
  {
    std::memcpy(effect->frames.data(), parsed->sample_data, parsed->num_frames * parsed->bytes_per_frame);

    SpeexResamplerStatePtr resampler_state;
    if (parsed->sample_rate != SAMPLE_RATE || parsed->num_channels != NUM_CHANNELS)
    {
      if (!ConvertFrames(*effect, parsed->sample_rate, parsed->num_channels, parsed->num_frames, error))
        return false;
    }

    DEV_LOG("Loaded effect '{}' with {} frames.", resource_name, effect->frames.size() / NUM_CHANNELS);
  }
  else
  {
    WARNING_LOG("Effect '{}' has zero frames.", resource_name);
  }

  return true;
}

const SoundEffectManager::CachedEffectPtr*
SoundEffectManager::LookupOrLoadCachedEffect(std::string resource_name, std::unique_lock<std::mutex>& lock)
{
  const CachedEffectPtr* cached_effect = s_locals.effect_cache.Lookup(resource_name);
  if (cached_effect)
    return cached_effect;

  lock.unlock();

  Error error;
  CachedEffectPtr new_effect = std::make_shared<CachedEffect>();
  if (!LoadCachedEffect(resource_name, new_effect, &error))
    ERROR_LOG("Failed to load cached effect '{}': {}", resource_name, error.GetDescription());

  lock.lock();

  // could get shutdown while the lock was released...
  if (!LockedIsInitialized())
    return nullptr;

  // still insert it in the cache even if it failed, that way we don't try to load it again
  return s_locals.effect_cache.Insert(std::move(resource_name), std::move(new_effect));
}

void SoundEffectManager::StreamSoundEffect(std::string path)
{
  std::lock_guard lock(s_locals.state_mutex);
  if (!LockedIsInitialized())
    return;

  Host::QueueAsyncTask([path = std::move(path)]() {
    WAVReader reader;
    Error error;
    if (!OpenFileForStreaming(path.c_str(), &reader, &error))
    {
      ERROR_LOG("Failed to open sound effect '{}': {}", Path::GetFileName(path), error.GetDescription());
      return;
    }

    PlayingResampledEffect resampled;
    if (reader.GetSampleRate() != SAMPLE_RATE || reader.GetNumChannels() != NUM_CHANNELS)
    {
      resampled = CreateResampledStreamedEffect(std::move(reader), &error);
      if (!resampled)
      {
        ERROR_LOG("Failed to open sound effect '{}': {}", Path::GetFileName(path), error.GetDescription());
        return;
      }
    }

    std::lock_guard lock(s_locals.state_mutex);
    if (!LockedIsInitialized())
      return;

    if (EnsureStreamStarted())
    {
      if (resampled)
        s_locals.active_sounds.emplace_back(std::move(resampled));
      else
        s_locals.active_sounds.emplace_back(std::move(reader));

      DEBUG_LOG("{} effects active", s_locals.active_sounds.size());
    }
  });
}

bool SoundEffectManager::OpenFileForStreaming(const char* path, WAVReader* reader, Error* error)
{
  if (!reader->Open(path, error))
    return false;

  DEV_LOG("Streaming WAV file '{}': {} frames @ {}hz, {} channels", Path::GetFileName(path), reader->GetNumFrames(),
          reader->GetSampleRate(), reader->GetNumChannels());
  return true;
}

bool SoundEffectManager::EnsureStreamStarted()
{
  // Start the stream if it was stopped
  if (!s_locals.stream_started)
  {
    DebugAssert(s_locals.active_sounds.empty());

    Error start_error;
    if (s_locals.audio_stream->Start(&start_error))
    {
      s_locals.stream_started = true;
      VERBOSE_LOG("Started audio stream for sound effect playback.");
    }
    else
    {
      ERROR_LOG("Failed to start sound effect stream: {}", start_error.GetDescription());
      return false;
    }
  }

  // Reset silence counter since we have audio to play
  s_locals.silence_frames = 0;
  return true;
}

void SoundEffectManager::StopAll()
{
  std::lock_guard lock(s_locals.state_mutex);
  s_locals.active_sounds.clear();
}

bool SoundEffectManager::HasAnyActiveEffects()
{
  std::lock_guard lock(s_locals.state_mutex);
  return !s_locals.active_sounds.empty();
}

void SoundEffectManager::EffectAudioStream::ReadFrames(SampleType* samples, u32 num_frames)
{
  std::lock_guard lock(s_locals.state_mutex);

  // extremely unlikely: we could end up here after stopping on the core thread due to the mutex
  if (!s_locals.stream_started) [[unlikely]]
  {
    std::memset(samples, 0, num_frames * NUM_CHANNELS * sizeof(SampleType));
    return;
  }

  bool mixed_any = false;
  auto it = s_locals.active_sounds.begin();
  while (it != s_locals.active_sounds.end())
  {
    const u32 frames_read = ReadEntryFrames(*it, samples, num_frames, mixed_any);

    // Mixed?
    mixed_any = mixed_any || (frames_read > 0);

    // Check if this sound has finished
    if (frames_read < num_frames)
    {
      it = s_locals.active_sounds.erase(it);
      DEBUG_LOG("{} effects active", s_locals.active_sounds.size());
    }
    else
    {
      ++it;
    }
  }

  if (mixed_any)
  {
    // Reset silence counter since we have active sounds
    s_locals.silence_frames = 0;
  }
  else
  {
    // Track silence frames for timeout
    s_locals.silence_frames += num_frames;

    // Stop the stream if we've exceeded the silence timeout
    // Have to do this on the main thread, because pulseaudio doesn't allow you to stop from the callback
    if (s_locals.silence_frames >= SILENCE_TIMEOUT_FRAMES && !s_locals.stream_stop_pending)
    {
      s_locals.stream_stop_pending = true;
      Host::RunOnCoreThread(&SoundEffectManager::StopStreamIfInactive);
    }

    // No samples
    std::memset(samples, 0, num_frames * NUM_CHANNELS * sizeof(SampleType));
  }
}

void SoundEffectManager::StopStreamIfInactive()
{
  std::lock_guard lock(s_locals.state_mutex);
  s_locals.stream_stop_pending = false;

  if (s_locals.silence_frames < SILENCE_TIMEOUT_FRAMES || !s_locals.active_sounds.empty())
  {
    DEBUG_LOG("Cancelling stream stop, activity detected.");
    return;
  }

  Error stop_error;
  if (s_locals.audio_stream->Stop(&stop_error))
  {
    s_locals.stream_started = false;
    VERBOSE_LOG("Stopped effect audio stream due to inactivity.");
  }
  else
  {
    ERROR_LOG("Failed to stop effect audio stream: {}", stop_error.GetDescription());
  }
}

u32 SoundEffectManager::ReadEntryFrames(ActiveSoundEntry& entry, AudioStream::SampleType* samples, u32 num_frames,
                                        bool mix)
{
  u32 frames_read;
  if (std::holds_alternative<PlayingCachedEffect>(entry))
    frames_read = ReadEntryFrames(std::get<PlayingCachedEffect>(entry), samples, num_frames, mix);
  else if (std::holds_alternative<PlayingResampledEffect>(entry))
    frames_read = ReadEntryFrames(std::get<PlayingResampledEffect>(entry), samples, num_frames, mix);
  else
    frames_read = ReadEntryFrames(std::get<PlayingStreamedEffect>(entry), samples, num_frames, mix);

  if (!mix)
  {
    // first sound, we read directly into the buffer so zero out anything left
    const u32 frames_to_zero = num_frames - frames_read;
    if (frames_to_zero > 0)
      std::memset(&samples[frames_read * NUM_CHANNELS], 0, frames_to_zero * BYTES_PER_FRAME);
  }

  return frames_read;
}

u32 SoundEffectManager::ReadEntryFrames(PlayingCachedEffect& effect, AudioStream::SampleType* samples, u32 num_frames,
                                        bool mix)
{
  const u32 frames_available = static_cast<u32>(effect.effect->frames.size() / NUM_CHANNELS) - effect.current_frame;
  if (frames_available == 0)
    return 0;

  const u32 frames_read = std::min(frames_available, num_frames);
  const AudioStream::SampleType* src_ptr = effect.effect->frames.data() + (effect.current_frame * NUM_CHANNELS);
  effect.current_frame += frames_read;
  if (mix)
    MixFrames(samples, src_ptr, frames_read);
  else
    std::memcpy(samples, src_ptr, frames_read * BYTES_PER_FRAME);

  return frames_read;
}

u32 SoundEffectManager::ReadEntryFrames(PlayingStreamedEffect& effect, AudioStream::SampleType* samples, u32 num_frames,
                                        bool mix)
{
  const u32 num_samples = num_frames * NUM_CHANNELS;
  if (mix && num_samples > s_locals.temp_buffer.size())
    s_locals.temp_buffer.resize(num_samples);

  Error error;
  const std::optional<u32> frames = effect.ReadFrames(mix ? s_locals.temp_buffer.data() : samples, num_frames, &error);
  if (!frames.has_value())
  {
    ERROR_LOG("Error reading wave file: {}", error.GetDescription());
    return 0;
  }

  if (frames.value() == 0)
  {
    // reached end of file
    return 0;
  }

  if (mix)
    MixFrames(samples, s_locals.temp_buffer.data(), frames.value());

  return frames.value();
}

void SoundEffectManager::MixFrames(AudioStream::SampleType* dest, const AudioStream::SampleType* src, u32 num_frames)
{
  const u32 num_samples = num_frames * NUM_CHANNELS;
  u32 i = 0;

#ifdef CPU_ARCH_SIMD
  static constexpr u32 SAMPLES_PER_VEC = 8;
  const u32 num_samples_aligned = Common::AlignDown(num_samples, SAMPLES_PER_VEC);
  for (; i < num_samples_aligned; i += SAMPLES_PER_VEC)
  {
    GSVector4i vsrc = GSVector4i::load<false>(src);
    GSVector4i vdest = GSVector4i::load<false>(dest);
    vdest = vdest.adds16(vsrc);
    GSVector4i::store<false>(dest, vdest);
    src += SAMPLES_PER_VEC;
    dest += SAMPLES_PER_VEC;
  }
#endif

  for (; i < num_samples; i++)
  {
    const s32 mixed = static_cast<s32>(*dest) + static_cast<s32>(*(src++));
    *(dest++) = static_cast<AudioStream::SampleType>(
      std::clamp(mixed, static_cast<s32>(std::numeric_limits<AudioStream::SampleType>::min()),
                 static_cast<s32>(std::numeric_limits<AudioStream::SampleType>::max())));
  }
}

void SoundEffectManager::ConvertToStereo(std::span<const AudioStream::SampleType> src,
                                         std::span<AudioStream::SampleType> dst, u32 in_channels)
{
  const u32 num_frames = static_cast<u32>(src.size() / in_channels);
  DebugAssert(num_frames == static_cast<u32>(dst.size() / NUM_CHANNELS));
  DebugAssert(in_channels > 0);

  if (in_channels == 1)
  {
    // Upmix mono -> Stereo, optimized
    const AudioStream::SampleType* src_ptr = src.data();
    AudioStream::SampleType* dst_ptr = dst.data();
    u32 i = 0;

#ifdef CPU_ARCH_SIMD
    const u32 aligned_frames = Common::AlignDownPow2(num_frames, 8);
    for (; i < aligned_frames; i += 8)
    {
      const GSVector4i vsrc = GSVector4i::load<false>(src_ptr);
      const GSVector4i low = vsrc.upl16(vsrc);
      const GSVector4i high = vsrc.uph16(vsrc);
      GSVector4i::store<false>(dst_ptr, low);
      GSVector4i::store<false>(dst_ptr + 8, high);
      src_ptr += 8;
      dst_ptr += 16;
    }
#endif

    for (; i < num_frames; i++)
    {
      const AudioStream::SampleType sample = *(src_ptr++);
      *(dst_ptr++) = sample;
      *(dst_ptr++) = sample;
    }
  }
  else
  {
    // Downmix case, drop the other channels. Not ideal, but who's using surround wavs...
    const AudioStream::SampleType* src_ptr = src.data();
    AudioStream::SampleType* dst_ptr = dst.data();
    const u32 skip = in_channels - 2;
    for (u32 frame = 0; frame < num_frames; frame++)
    {
      *(dst_ptr++) = *(src_ptr++);
      *(dst_ptr++) = *(src_ptr++);
      src_ptr += skip;
    }
  }
}

// Defined in here to avoid polluting the command line.
#define OUTSIDE_SPEEX
#define FLOATING_POINT
#define EXPORT
#define RANDOM_PREFIX speex
#include "speex/speex_resampler.h"

void SoundEffectManager::SpeexResamplerStateDeleter::operator()(SpeexResamplerState* state) const
{
  speex_resampler_destroy(state);
}

bool SoundEffectManager::ConvertFrames(CachedEffect& effect, u32 in_sample_rate, u32 in_channels, u32 in_frames,
                                       Error* error)
{
  DynamicHeapArray<AudioStream::SampleType> temp_frames;

  if (in_channels != NUM_CHANNELS)
  {
    temp_frames.resize(in_frames * NUM_CHANNELS);
    ConvertToStereo(effect.frames, temp_frames, in_channels);
    effect.frames.swap(temp_frames);
  }

  if (in_sample_rate != SAMPLE_RATE)
  {
    const auto num_resampled_frames = [](u32 num_frames, u32 in_sample_rate) {
      return static_cast<u32>(((static_cast<u64>(num_frames) * SAMPLE_RATE) + (in_sample_rate - 1)) / in_sample_rate);
    };

    // since this is on a worker thread, use max quality
    int errcode;
    const SpeexResamplerStatePtr resampler_state(
      speex_resampler_init(NUM_CHANNELS, in_sample_rate, SAMPLE_RATE, SPEEX_RESAMPLER_QUALITY_MAX, &errcode));
    if (!resampler_state)
    {
      Error::SetStringFmt(error, "speex_resampler_init() failed: {} ({})", speex_resampler_strerror(errcode), errcode);
      return false;
    }

    // reserve a bit extra for the last part of the resample
    if (const u32 min_buffer_size = num_resampled_frames(in_frames + 2048u, in_sample_rate) * NUM_CHANNELS;
        temp_frames.size() < min_buffer_size)
    {
      temp_frames.resize(min_buffer_size);
    }

    u32 input_frames_count = 0;
    u32 output_frame_count = 0;
    for (;;)
    {
      const u32 expected_output_frames =
        num_resampled_frames(std::max(in_frames - input_frames_count, 1024u), in_sample_rate);
      if (const u32 min_buffer_size = (output_frame_count + expected_output_frames) * NUM_CHANNELS;
          temp_frames.size() < min_buffer_size)
      {
        temp_frames.resize(min_buffer_size);
      }

      unsigned int frames_processed = in_frames - input_frames_count;
      unsigned int frames_generated = (static_cast<u32>(temp_frames.size() / NUM_CHANNELS) - output_frame_count);
      const int ret = speex_resampler_process_interleaved_int(
        resampler_state.get(), (frames_processed > 0) ? &effect.frames[input_frames_count * NUM_CHANNELS] : nullptr,
        &frames_processed, &temp_frames[output_frame_count * NUM_CHANNELS], &frames_generated);
      if (ret != RESAMPLER_ERR_SUCCESS)
      {
        Error::SetStringFmt(error, "speex_resampler_process_interleaved_int() failed: {} ({})",
                            speex_resampler_strerror(ret), ret);
        return false;
      }

      input_frames_count += frames_processed;
      output_frame_count += frames_generated;
      if (frames_generated == 0)
        break;
    }

    temp_frames.resize(output_frame_count * NUM_CHANNELS);
    effect.frames.swap(temp_frames);
  }

  return true;
}

SoundEffectManager::ResampledStreamedEffect::ResampledStreamedEffect(WAVReader&& reader_,
                                                                     SpeexResamplerStatePtr&& resampler_state_)
  : reader(std::move(reader_)), resampler_state(std::move(resampler_state_))
{
}

SoundEffectManager::PlayingResampledEffect SoundEffectManager::CreateResampledStreamedEffect(WAVReader&& reader,
                                                                                             Error* error)
{
  SpeexResamplerStatePtr resampler;
  if (reader.GetSampleRate() != SAMPLE_RATE)
  {
    int errcode;
    resampler = SpeexResamplerStatePtr(speex_resampler_init(NUM_CHANNELS, reader.GetSampleRate(), SAMPLE_RATE,
                                                            SPEEX_RESAMPLER_QUALITY_DESKTOP, &errcode));
    if (!resampler)
    {
      Error::SetStringFmt(error, "speex_resampler_init() failed: {} ({})", speex_resampler_strerror(errcode), errcode);
      return {};
    }
  }

  return std::make_unique<ResampledStreamedEffect>(std::move(reader), std::move(resampler));
}

u32 SoundEffectManager::ReadEntryFrames(PlayingResampledEffect& effect, AudioStream::SampleType* samples,
                                        u32 num_frames, bool mix)
{
  u32 frames_read = 0;
  do
  {
    // fill input buffer if needed
    if (effect->input_buffer_pos == effect->input_buffer_size && effect->reader.GetRemainingFrames() > 0)
    {
      const bool needs_upmix = (effect->reader.GetNumChannels() != NUM_CHANNELS);
      if (needs_upmix)
      {
        const u32 required_buffer_space = ResampledStreamedEffect::INPUT_BUFFER_SIZE * effect->reader.GetNumChannels();
        if (required_buffer_space > s_locals.temp_buffer.size())
          s_locals.temp_buffer.resize(required_buffer_space);
      }

      Error error;
      const std::optional<u32> frames =
        effect->reader.ReadFrames(needs_upmix ? s_locals.temp_buffer.data() : effect->input_buffer.data(),
                                  ResampledStreamedEffect::INPUT_BUFFER_SIZE, &error);
      if (!frames.has_value())
      {
        ERROR_LOG("Error reading wave file: {}", error.GetDescription());
        break;
      }

      if (needs_upmix && frames.value() > 0)
      {
        ConvertToStereo(s_locals.temp_buffer.cspan(0, frames.value() * effect->reader.GetNumChannels()),
                        std::span(effect->input_buffer).subspan(0, frames.value() * NUM_CHANNELS),
                        effect->reader.GetNumChannels());
      }

      effect->input_buffer_pos = 0;
      effect->input_buffer_size = frames.value();
    }

    const u32 input_frames_available = effect->input_buffer_size - effect->input_buffer_pos;
    const u32 output_frames_requested = num_frames - frames_read;
    const AudioStream::SampleType* const src_ptr = &effect->input_buffer[effect->input_buffer_pos * NUM_CHANNELS];

    // not resampling? just mix
    if (!effect->resampler_state)
    {
      const u32 frames_to_copy = std::min(input_frames_available, output_frames_requested);
      if (frames_to_copy == 0)
        break;

      if (mix)
        MixFrames(samples, src_ptr, frames_to_copy);
      else
        std::memcpy(samples, src_ptr, frames_to_copy * BYTES_PER_FRAME);

      DebugAssert((effect->input_buffer_pos + frames_to_copy) <= effect->input_buffer_size);
      effect->input_buffer_pos += frames_to_copy;
      samples += frames_to_copy * NUM_CHANNELS;
      frames_read += frames_to_copy;
      continue;
    }

    unsigned int frames_processed = input_frames_available;
    unsigned int frames_generated = output_frames_requested;
    if (mix && (frames_generated * NUM_CHANNELS) > s_locals.temp_buffer.size())
      s_locals.temp_buffer.resize(frames_generated * NUM_CHANNELS);

    const int ret = speex_resampler_process_interleaved_int(
      effect->resampler_state.get(), (frames_processed > 0) ? src_ptr : nullptr, &frames_processed,
      mix ? s_locals.temp_buffer.data() : samples, &frames_generated);
    if (ret != RESAMPLER_ERR_SUCCESS)
    {
      ERROR_LOG("speex_resampler_process_interleaved_int() failed: {} ({})", speex_resampler_strerror(ret), ret);
      return 0;
    }

    DebugAssert((effect->input_buffer_pos + frames_processed) <= effect->input_buffer_size);
    effect->input_buffer_pos += frames_processed;

    // end of file?
    if (frames_generated == 0)
      break;

    if (mix)
      MixFrames(samples, s_locals.temp_buffer.data(), frames_generated);

    frames_read += frames_generated;
    samples += frames_generated * NUM_CHANNELS;
  } while (frames_read < num_frames);

  return frames_read;
}
