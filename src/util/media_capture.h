// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include <ctime>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

enum class GPUTextureFormat : u8;

class Error;
class GPUTexture;

enum class MediaCaptureBackend : u8
{
#ifdef _WIN32
  MediaFoundation,
#endif
#ifndef __ANDROID__
  FFmpeg,
#endif
  MaxCount,
};

enum class MediaCaptureMode : u8
{
  AudioAndVideo,
  AudioOnly,
  VideoOnly,
  MaxCount,
};

class MediaCapture
{
public:
  virtual ~MediaCapture();

  using ContainerName = std::pair<std::string, std::string>; // configname,longname
  using ContainerList = std::vector<ContainerName>;
  using CodecName = std::pair<std::string, std::string>; // configname,longname
  using CodecList = std::vector<CodecName>;

  static std::optional<MediaCaptureBackend> ParseBackendName(std::string_view str);
  static const char* GetBackendName(MediaCaptureBackend backend);
  static const char* GetBackendDisplayName(MediaCaptureBackend backend);

  static ContainerList GetAudioContainerList(MediaCaptureBackend backend);
  static CodecList GetAudioCodecList(MediaCaptureBackend backend, const char* container);
  static ContainerList GetVideoContainerList(MediaCaptureBackend backend);
  static CodecList GetVideoCodecList(MediaCaptureBackend backend, const char* container);

  static void AdjustVideoSize(u32* width, u32* height);

  static std::unique_ptr<MediaCapture> Create(MediaCaptureBackend backend, Error* error);

  virtual bool BeginCapture(MediaCaptureMode mode, float fps, float aspect, u32 width, u32 height,
                            GPUTextureFormat texture_format, u32 sample_rate, std::string path,
                            std::string_view video_codec, u32 video_bitrate, std::string_view video_codec_args,
                            std::string_view audio_codec, u32 audio_bitrate, std::string_view audio_codec_args,
                            Error* error) = 0;
  virtual bool EndCapture(Error* error) = 0;

  // TODO: make non-virtual?
  virtual const std::string& GetPath() const = 0;
  virtual std::string GetNextCapturePath() const = 0;
  virtual MediaCaptureMode GetCaptureMode() const = 0;
  virtual bool IsCapturingAudio() const = 0;
  virtual bool IsCapturingVideo() const = 0;
  virtual u32 GetVideoWidth() const = 0;
  virtual u32 GetVideoHeight() const = 0;
  virtual float GetVideoFPS() const = 0;

  /// Returns the elapsed time in seconds.
  virtual time_t GetElapsedTime() const = 0;

  virtual float GetCaptureThreadUsage() const = 0;
  virtual float GetCaptureThreadTime() const = 0;
  virtual void UpdateCaptureThreadUsage(double pct_divider, double time_divider) = 0;

  virtual GPUTexture* GetRenderTexture() = 0;
  virtual bool DeliverVideoFrame(GPUTexture* stex) = 0;
  virtual bool DeliverAudioFrames(const s16* frames, u32 num_frames) = 0;
};
