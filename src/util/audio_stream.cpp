// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "audio_stream.h"
#include "translation.h"

#include "common/error.h"

AudioStream::AudioStream() = default;

AudioStream::~AudioStream() = default;

AudioStream::DeviceInfo::DeviceInfo(std::string name_, std::string display_name_, u32 minimum_latency_)
  : name(std::move(name_)), display_name(std::move(display_name_)), minimum_latency_frames(minimum_latency_)
{
}

AudioStream::DeviceInfo::~DeviceInfo() = default;

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

u32 AudioStream::FramesToMS(u32 sample_rate, u32 frames)
{
  return (frames * 1000) / sample_rate;
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

std::vector<AudioStream::DeviceInfo> AudioStream::GetOutputDevices(AudioBackend backend, std::string_view driver,
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

std::unique_ptr<AudioStream> AudioStream::CreateStream(AudioBackend backend, u32 sample_rate, u32 channels,
                                                       u32 output_latency_frames, bool output_latency_minimal,
                                                       std::string_view driver_name, std::string_view device_name,
                                                       AudioStreamSource* source, bool auto_start, Error* error)
{
  switch (backend)
  {
#ifndef __ANDROID__
    case AudioBackend::Cubeb:
      return CreateCubebAudioStream(sample_rate, channels, output_latency_frames, output_latency_minimal, driver_name,
                                    device_name, source, auto_start, error);

    case AudioBackend::SDL:
      return CreateSDLAudioStream(sample_rate, channels, output_latency_frames, output_latency_minimal, source,
                                  auto_start, error);
#else
    case AudioBackend::AAudio:
      return CreateAAudioAudioStream(sample_rate, channels, output_latency_frames, output_latency_minimal, source,
                                     auto_start, error);

    case AudioBackend::OpenSLES:
      return CreateOpenSLESAudioStream(sample_rate, channels, output_latency_frames, output_latency_minimal, source,
                                       auto_start, error);
#endif

    default:
      Error::SetStringView(error, "Unknown audio backend.");
      return nullptr;
  }
}
