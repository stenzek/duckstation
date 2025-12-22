// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "sound_effect_manager.h"
#include "settings.h"

#include "util/audio_stream.h"
#include "util/wav_reader_writer.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/error.h"
#include "common/gsvector.h"
#include "common/log.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

LOG_CHANNEL(SoundEffectManager);

namespace SoundEffectManager {

static constexpr u32 SAMPLE_RATE = 44100;
static constexpr u32 NUM_CHANNELS = 2;
static constexpr u32 OUTPUT_LATENCY_FRAMES = 2048;
static constexpr u32 BYTES_PER_FRAME = NUM_CHANNELS * sizeof(AudioStream::SampleType);
static constexpr u32 SILENCE_TIMEOUT_SECONDS = 10;
static constexpr u32 SILENCE_TIMEOUT_FRAMES = SAMPLE_RATE * SILENCE_TIMEOUT_SECONDS;

namespace {

class EffectAudioStream final : public AudioStreamSource
{
public:
  void ReadFrames(SampleType* samples, u32 num_frames) override;
};

struct Locals
{
  std::mutex active_sounds_mutex;
  std::deque<WAVReader> active_sounds;
  std::unique_ptr<AudioStream> audio_stream;
  std::vector<AudioStream::SampleType> temp_buffer;
  EffectAudioStream audio_stream_source;
  u32 silence_frames = 0;
  bool stream_started = false;
};

} // namespace

static bool EnqueueWaveFile(const char* path, Error* error);
static void MixFrames(AudioStream::SampleType* dest, const AudioStream::SampleType* src, u32 num_frames);

ALIGN_TO_CACHE_LINE static Locals s_locals;

} // namespace SoundEffectManager

bool SoundEffectManager::IsInitialized()
{
  return (s_locals.audio_stream != nullptr);
}

void SoundEffectManager::EnsureInitialized()
{
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
  if (!s_locals.audio_stream)
    return;

  INFO_COLOR_LOG(StrongGreen, "Closing audio stream");
  std::lock_guard lock(s_locals.active_sounds_mutex);
  decltype(s_locals.active_sounds)().swap(s_locals.active_sounds);
  s_locals.audio_stream.reset();
  s_locals.stream_started = false;
  s_locals.silence_frames = 0;
}

bool SoundEffectManager::EnqueueSoundEffect(std::string_view name)
{
  if (!IsInitialized())
    return false;

  std::string full_path = EmuFolders::GetOverridableResourcePath(name);
  Error error;
  if (!EnqueueWaveFile(full_path.c_str(), &error))
  {
    ERROR_LOG("Failed to play sound effect '{}': {}", name, error.GetDescription());
    return false;
  }

  return true;
}

bool SoundEffectManager::EnqueueWaveFile(const char* path, Error* error)
{
  if (!IsInitialized())
  {
    Error::SetStringView(error, "Effect audio stream is not initialized.");
    return false;
  }

  WAVReader reader;
  if (!reader.Open(path, error))
    return false;

  if (reader.GetSampleRate() != SAMPLE_RATE)
  {
    Error::SetStringFmt(error, "WAV file sample rate {} does not match expected {}", reader.GetSampleRate(),
                        SAMPLE_RATE);
    return false;
  }

  if (reader.GetNumChannels() != NUM_CHANNELS)
  {
    Error::SetStringFmt(error, "WAV file has {} channels, expected {}", reader.GetNumChannels(), NUM_CHANNELS);
    return false;
  }

  std::lock_guard lock(s_locals.active_sounds_mutex);

  if (!s_locals.audio_stream)
  {
    Error::SetStringView(error, "Audio stream not initialized.");
    return false;
  }

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

  s_locals.active_sounds.push_back(std::move(reader));

  // Reset silence counter since we have audio to play
  s_locals.silence_frames = 0;

  return true;
}

void SoundEffectManager::StopAll()
{
  std::lock_guard lock(s_locals.active_sounds_mutex);
  s_locals.active_sounds.clear();
}

bool SoundEffectManager::HasAnyActiveEffects()
{
  std::lock_guard lock(s_locals.active_sounds_mutex);
  return !s_locals.active_sounds.empty();
}

void SoundEffectManager::MixFrames(AudioStream::SampleType* dest, const AudioStream::SampleType* src, u32 num_frames)
{
  static constexpr u32 SAMPLES_PER_VEC = 8;
  const u32 num_samples = num_frames * NUM_CHANNELS;
  const u32 num_samples_aligned = Common::AlignDown(num_samples, SAMPLES_PER_VEC);
  u32 i;
  for (i = 0; i < num_samples_aligned; i += SAMPLES_PER_VEC)
  {
    GSVector4i vsrc = GSVector4i::load<false>(src);
    GSVector4i vdest = GSVector4i::load<false>(dest);
    vdest = vsrc.adds16(vsrc);
    GSVector4i::store<false>(dest, vdest);
    src += SAMPLES_PER_VEC;
    dest += SAMPLES_PER_VEC;
  }

  for (; i < num_samples; i++)
  {
    const s32 mixed = static_cast<s32>(dest[i]) + static_cast<s32>(src[i]);
    dest[i] = static_cast<AudioStream::SampleType>(
      std::clamp(mixed, static_cast<s32>(std::numeric_limits<AudioStream::SampleType>::min()),
                 static_cast<s32>(std::numeric_limits<AudioStream::SampleType>::max())));
  }
}

void SoundEffectManager::EffectAudioStream::ReadFrames(SampleType* samples, u32 num_frames)
{
  const u32 num_samples = num_frames * NUM_CHANNELS;

  std::lock_guard lock(s_locals.active_sounds_mutex);

  if (s_locals.active_sounds.empty())
  {
    // Zero the output buffer first
    std::memset(samples, 0, num_samples * sizeof(SampleType));

    // Track silence frames for timeout
    s_locals.silence_frames += num_frames;

    // Stop the stream if we've exceeded the silence timeout
    if (s_locals.silence_frames >= SILENCE_TIMEOUT_FRAMES)
    {
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

    return;
  }

  // Reset silence counter since we have active sounds
  s_locals.silence_frames = 0;

  // Temporary buffer for reading from each sound
  if (num_frames > s_locals.temp_buffer.size())
    s_locals.temp_buffer.resize(num_frames);

  Error error;
  bool mixed_any = false;
  auto it = s_locals.active_sounds.begin();
  while (it != s_locals.active_sounds.end())
  {
    WAVReader& reader = *it;

    const std::optional<u32> frames =
      reader.ReadFrames(mixed_any ? s_locals.temp_buffer.data() : samples, num_frames, &error);
    if (!frames.has_value())
    {
      ERROR_LOG("Error reading wave file: {}", error.GetDescription());
      it = s_locals.active_sounds.erase(it);
      continue;
    }

    if (frames.value() > 0)
    {
      if (!mixed_any)
      {
        mixed_any = true;

        // first sound, we read directly into the buffer so zero out anything left
        const u32 frames_to_zero = num_frames - frames.value();
        if (frames_to_zero > 0)
          std::memset(&samples[frames.value() * NUM_CHANNELS], 0, frames_to_zero * BYTES_PER_FRAME);
      }
      else
      {
        const u32 frames_to_mix = std::min(num_frames, frames.value());
        MixFrames(samples, s_locals.temp_buffer.data(), frames_to_mix);
      }
    }

    // Check if this sound has finished
    if (frames.value() < num_frames)
      it = s_locals.active_sounds.erase(it);
    else
      ++it;
  }

  if (!mixed_any)
  {
    // No sounds produced any output, zero the buffer
    std::memset(samples, 0, num_samples * sizeof(SampleType));
  }
}
