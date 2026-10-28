// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "audio_stream.h"
#include "translation.h"

#include "common/align.h"
#include "common/error.h"
#include "common/gsvector.h"

#include <algorithm>

AudioStream::AudioStream() = default;

AudioStream::~AudioStream() = default;

AudioStream::DeviceInfo::DeviceInfo(std::string name_, std::string display_name_, u32 minimum_latency_)
  : name(std::move(name_)), display_name(std::move(display_name_)), minimum_latency_frames(minimum_latency_)
{
}

AudioStream::DeviceInfo::~DeviceInfo() = default;

static constexpr const std::array s_backend_names = {
  // clang-format off
  "Null",
  "Cubeb",
  "SDL",
#ifdef _WIN32
  "XAudio2",
#endif
  // clang-format on
};
static constexpr const std::array s_backend_display_names = {
  // clang-format off
  TRANSLATE_DISAMBIG_NOOP("Settings", "Null (No Output)", "AudioBackend"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "Cubeb", "AudioBackend"),
  TRANSLATE_DISAMBIG_NOOP("Settings", "SDL", "AudioBackend"),
#ifdef _WIN32
  TRANSLATE_DISAMBIG_NOOP("Settings", "XAudio2", "AudioBackend"),
#endif
  // clang-format on
};

std::optional<AudioBackend> AudioStream::ParseBackendName(std::string_view str)
{
  int index = 0;
  for (const char* name : s_backend_names)
  {
    if (str == name)
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
  return Host::TranslateToCString("Settings", s_backend_display_names[static_cast<int>(backend)], "AudioBackend");
}

u32 AudioStream::FramesToMS(u32 sample_rate, u32 frames)
{
  return (frames * 1000) / sample_rate;
}

void AudioStream::ApplyVolume(SampleType* samples, u32 num_samples, u32 volume)
{
  const u32 aligned_samples = Common::AlignDownPow2(num_samples, 8);
  num_samples -= aligned_samples;

  const float volume_mult = static_cast<float>(volume) / 100.0f;
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

std::vector<std::pair<std::string, std::string>> AudioStream::GetDriverNames(AudioBackend backend)
{
  std::vector<std::pair<std::string, std::string>> ret;
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

std::vector<AudioStream::DeviceInfo> AudioStream::GetOutputDevices(AudioBackend backend, std::string_view driver,
                                                                   u32 sample_rate)
{
  std::vector<AudioStream::DeviceInfo> ret;
  switch (backend)
  {
    case AudioBackend::Cubeb:
      ret = GetCubebOutputDevices(driver, sample_rate);
      break;
#ifdef _WIN32
    case AudioBackend::XAudio2:
      ret = GetXAudio2OutputDevices(sample_rate);
      break;
#endif

    default:
      break;
  }

  return ret;
}

std::unique_ptr<AudioStream> AudioStream::CreateStream(AudioBackend backend, u32 sample_rate, u32 channels,
                                                       u32 output_latency_frames, bool output_latency_minimal,
                                                       std::string_view driver_name, std::string_view device_name,
                                                       AudioStreamSource* source, bool auto_start, Error* error)
{
  switch (backend)
  {
    case AudioBackend::Cubeb:
      return CreateCubebAudioStream(sample_rate, channels, output_latency_frames, output_latency_minimal, driver_name,
                                    device_name, source, auto_start, error);

    case AudioBackend::SDL:
      return CreateSDLAudioStream(sample_rate, channels, output_latency_frames, output_latency_minimal, source,
                                  auto_start, error);
#ifdef _WIN32
    case AudioBackend::XAudio2:
      return CreateXAudio2AudioStream(sample_rate, channels, output_latency_frames, output_latency_minimal, device_name,
                                      source, auto_start, error);
#endif

    default:
      Error::SetStringView(error, "Unknown audio backend.");
      return nullptr;
  }
}
