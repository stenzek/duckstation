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

// TODO: Resampling and channels/format conversion support

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

struct CachedEffect
{
  std::vector<AudioStream::SampleType> samples;
  u32 num_frames = 0;
};
using CachedEffectPtr = std::shared_ptr<CachedEffect>;

struct PlayingCachedEffect
{
  CachedEffectPtr effect;
  u32 current_frame = 0;
};

using ActiveSoundEntry = std::variant<WAVReader, PlayingCachedEffect>;

struct Locals
{
  std::mutex state_mutex;
  std::deque<ActiveSoundEntry> active_sounds;
  std::unique_ptr<AudioStream> audio_stream;
  std::vector<AudioStream::SampleType> temp_buffer;
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

/// Mixes multiple active sounds into the destination buffer.
static void MixFrames(AudioStream::SampleType* dest, const AudioStream::SampleType* src, u32 num_frames);

/// Stops the stream if there are no active sounds.
static void StopStreamIfInactive();

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
    if (effect && (*effect)->num_frames > 0 && EnsureStreamStarted())
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

  if (parsed->sample_rate != SAMPLE_RATE)
  {
    Error::SetStringFmt(error, "WAV file sample rate {} does not match expected {}", parsed->sample_rate, SAMPLE_RATE);
    return false;
  }

  if (parsed->num_channels != NUM_CHANNELS)
  {
    Error::SetStringFmt(error, "WAV file has {} channels, expected {}", parsed->num_channels, NUM_CHANNELS);
    return false;
  }

  effect->num_frames = parsed->num_frames;
  effect->samples.resize(parsed->num_frames * parsed->num_channels);
  if (parsed->num_frames > 0)
  {
    std::memcpy(effect->samples.data(), parsed->sample_data, parsed->num_frames * parsed->bytes_per_frame);
    DEV_LOG("Loaded effect '{}' with {} frames.", resource_name, effect->num_frames);
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

    std::lock_guard lock(s_locals.state_mutex);
    if (!LockedIsInitialized())
      return;

    if (EnsureStreamStarted())
    {
      s_locals.active_sounds.emplace_back(std::move(reader));
      DEBUG_LOG("{} effects active", s_locals.active_sounds.size());
    }
  });
}

bool SoundEffectManager::OpenFileForStreaming(const char* path, WAVReader* reader, Error* error)
{
  if (!reader->Open(path, error))
    return false;

  if (reader->GetSampleRate() != SAMPLE_RATE)
  {
    Error::SetStringFmt(error, "WAV file sample rate {} does not match expected {}", reader->GetSampleRate(),
                        SAMPLE_RATE);
    return false;
  }

  if (reader->GetNumChannels() != NUM_CHANNELS)
  {
    Error::SetStringFmt(error, "WAV file has {} channels, expected {}", reader->GetNumChannels(), NUM_CHANNELS);
    return false;
  }

  std::lock_guard lock(s_locals.state_mutex);

  if (!s_locals.audio_stream)
  {
    Error::SetStringView(error, "Audio stream not initialized.");
    return false;
  }

  DEV_LOG("Streaming WAV file '{}': {} frames", Path::GetFileName(path), reader->GetNumFrames());
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
  {
    PlayingCachedEffect& reader = std::get<PlayingCachedEffect>(entry);
    const u32 frames_available = reader.effect->num_frames - reader.current_frame;
    if (frames_available == 0)
      return 0;

    frames_read = std::min(frames_available, num_frames);
    const AudioStream::SampleType* src_ptr = reader.effect->samples.data() + (reader.current_frame * NUM_CHANNELS);
    reader.current_frame += frames_read;
    if (mix)
      MixFrames(samples, src_ptr, frames_read);
    else
      std::memcpy(samples, src_ptr, frames_read * BYTES_PER_FRAME);
  }
  else
  {
    DebugAssert(std::holds_alternative<WAVReader>(entry));
    WAVReader& reader = std::get<WAVReader>(entry);

    const u32 num_samples = num_frames * NUM_CHANNELS;
    if (!mix && num_samples > s_locals.temp_buffer.size())
      s_locals.temp_buffer.resize(num_samples);

    Error error;
    const std::optional<u32> frames =
      reader.ReadFrames(mix ? s_locals.temp_buffer.data() : samples, num_frames, &error);
    if (!frames.has_value())
    {
      ERROR_LOG("Error reading wave file: {}", error.GetDescription());
      return 0;
    }

    frames_read = frames.value();
    if (frames_read == 0)
    {
      // reached end of file
      return 0;
    }

    if (mix)
      MixFrames(samples, s_locals.temp_buffer.data(), frames_read);
  }

  if (!mix)
  {
    // first sound, we read directly into the buffer so zero out anything left
    const u32 frames_to_zero = num_frames - frames_read;
    if (frames_to_zero > 0)
      std::memset(&samples[frames_read * NUM_CHANNELS], 0, frames_to_zero * BYTES_PER_FRAME);
  }

  return frames_read;
}

void SoundEffectManager::MixFrames(AudioStream::SampleType* dest, const AudioStream::SampleType* src, u32 num_frames)
{
  static constexpr u32 SAMPLES_PER_VEC = 8;
  const u32 num_samples = num_frames * NUM_CHANNELS;
  const u32 num_samples_aligned = Common::AlignDown(num_samples, SAMPLES_PER_VEC);
  u32 i = 0;
  for (; i < num_samples_aligned; i += SAMPLES_PER_VEC)
  {
    GSVector4i vsrc = GSVector4i::load<false>(src);
    GSVector4i vdest = GSVector4i::load<false>(dest);
    vdest = vdest.adds16(vsrc);
    GSVector4i::store<false>(dest, vdest);
    src += SAMPLES_PER_VEC;
    dest += SAMPLES_PER_VEC;
  }

  for (; i < num_samples; i++)
  {
    const s32 mixed = static_cast<s32>(*dest) + static_cast<s32>(*(src++));
    *(dest++) = static_cast<AudioStream::SampleType>(
      std::clamp(mixed, static_cast<s32>(std::numeric_limits<AudioStream::SampleType>::min()),
                 static_cast<s32>(std::numeric_limits<AudioStream::SampleType>::max())));
  }
}
