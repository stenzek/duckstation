// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/types.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

class Error;

enum class AudioBackend : u8
{
  Null,
#ifndef __ANDROID__
  Cubeb,
  SDL,
#else
  AAudio,
  OpenSLES,
#endif
  Count
};

class AudioStreamSource
{
public:
  using SampleType = s16;

  virtual void ReadFrames(SampleType* samples, u32 num_frames) = 0;
};

class AudioStream
{
public:
  using SampleType = AudioStreamSource::SampleType;

#ifndef __ANDROID__
  static constexpr AudioBackend DEFAULT_BACKEND = AudioBackend::Cubeb;
#else
  static constexpr AudioBackend DEFAULT_BACKEND = AudioBackend::AAudio;
#endif

  struct DeviceInfo
  {
    std::string name;
    std::string display_name;
    u32 minimum_latency_frames;

    DeviceInfo(std::string name_, std::string display_name_, u32 minimum_latency_);
    ~DeviceInfo();
  };

  virtual ~AudioStream();

  static std::optional<AudioBackend> ParseBackendName(const char* str);
  static const char* GetBackendName(AudioBackend backend);
  static const char* GetBackendDisplayName(AudioBackend backend);

  static u32 FramesToMS(u32 sample_rate, u32 frames);

  /// Returns a list of available driver names for the specified backend.
  static std::vector<std::pair<std::string, std::string>> GetDriverNames(AudioBackend backend);

  /// Returns a list of available output devices for the specified backend and driver.
  static std::vector<DeviceInfo> GetOutputDevices(AudioBackend backend, std::string_view driver, u32 sample_rate);

  /// Creates an audio stream with the specified parameters.
  static std::unique_ptr<AudioStream> CreateStream(AudioBackend backend, u32 sample_rate, u32 channels,
                                                   u32 output_latency_frames, bool output_latency_minimal,
                                                   std::string_view driver_name, std::string_view device_name,
                                                   AudioStreamSource* source, bool auto_start, Error* error);

  /// Starts the stream, allowing it to request data.
  virtual bool Start(Error* error) = 0;

  /// Temporarily pauses the stream, preventing it from requesting data.
  virtual bool Stop(Error* error) = 0;

protected:
  AudioStream();

private:
#ifndef __ANDROID__
  static std::vector<std::pair<std::string, std::string>> GetCubebDriverNames();
  static std::vector<DeviceInfo> GetCubebOutputDevices(std::string_view driver, u32 sample_rate);
  static std::unique_ptr<AudioStream> CreateCubebAudioStream(u32 sample_rate, u32 channels, u32 output_latency_frames,
                                                             bool output_latency_minimal, std::string_view driver_name,
                                                             std::string_view device_name, AudioStreamSource* source,
                                                             bool auto_start, Error* error);
  static std::unique_ptr<AudioStream> CreateSDLAudioStream(u32 sample_rate, u32 channels, u32 output_latency_frames,
                                                           bool output_latency_minimal, AudioStreamSource* source,
                                                           bool auto_start, Error* error);
#else
  static std::unique_ptr<AudioStream> CreateAAudioAudioStream(u32 sample_rate, u32 output_latency_frames,
                                                              bool output_latency_minimal, AudioStreamSource* source,
                                                              bool auto_start, Error* error);
  static std::unique_ptr<AudioStream> CreateOpenSLESAudioStream(u32 sample_rate, u32 output_latency_frames,
                                                                bool output_latency_minimal, AudioStreamSource* source,
                                                                bool auto_start, Error* error);
#endif
};
