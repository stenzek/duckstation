// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/types.h"

#include <cstdio>
#include <optional>
#include <vector>

class Error;

class WAVReader
{
public:
  enum Format : u8
  {
    InvalidFormat = 0,
    PCMFormat = 1,
    FloatFormat = 3,
  };

  WAVReader();
  WAVReader(WAVReader&& move);
  WAVReader(const WAVReader&) = delete;
  ~WAVReader();

  WAVReader& operator=(WAVReader&& move);
  WAVReader& operator=(const WAVReader&) = delete;

  ALWAYS_INLINE Format GetFormat() const { return m_format; }
  ALWAYS_INLINE u32 GetSampleRate() const { return m_sample_rate; }
  ALWAYS_INLINE u32 GetNumChannels() const { return m_num_channels; }
  ALWAYS_INLINE u32 GetNumFrames() const { return m_num_frames; }
  ALWAYS_INLINE u32 GetBitsPerSample() const { return m_bits_per_sample; }
  ALWAYS_INLINE u32 GetBytesPerFrame() const { return m_bytes_per_frame; }
  ALWAYS_INLINE u64 GetFramesStartOffset() const { return m_frames_start; }
  ALWAYS_INLINE bool IsOpen() const { return (m_file != nullptr); }

  bool Open(const char* path, Error* error = nullptr);
  void Close();

  std::FILE* TakeFile();
  u64 GetFileSize();

  u32 GetRemainingFrames() const;

  bool SeekToFrame(u32 num, Error* error = nullptr);

  std::optional<u32> ReadFrames(void* samples, u32 num_frames, Error* error = nullptr);

  struct MemoryParseResult
  {
    Format format;
    u8 bits_per_sample;
    u8 num_channels;
    u8 bytes_per_frame;
    u32 sample_rate;
    u32 num_frames;
    const void* sample_data;
  };

  static std::optional<MemoryParseResult> ParseMemory(const void* data, size_t size, Error* error = nullptr);

private:
  using SampleType = s16;

  std::FILE* m_file = nullptr;
  s64 m_frames_start = 0;
  Format m_format = PCMFormat;
  u8 m_bits_per_sample = 0;
  u8 m_num_channels = 0;
  u8 m_bytes_per_frame = 0;
  u32 m_sample_rate = 0;
  u32 m_num_frames = 0;
  u32 m_current_frame = 0;
};

class WAVWriter
{
public:
  WAVWriter();
  WAVWriter(WAVWriter&& move);
  WAVWriter(const WAVWriter&) = delete;
  ~WAVWriter();

  WAVWriter& operator=(WAVWriter&& move);
  WAVWriter& operator=(const WAVWriter&) = delete;

  ALWAYS_INLINE u32 GetSampleRate() const { return m_sample_rate; }
  ALWAYS_INLINE u32 GetNumChannels() const { return m_num_channels; }
  ALWAYS_INLINE u32 GetNumFrames() const { return m_num_frames; }
  ALWAYS_INLINE bool IsOpen() const { return (m_file != nullptr); }

  bool Open(const char* path, u32 sample_rate, u32 num_channels, Error* error = nullptr);
  bool Close(Error* error);

  bool WriteFrames(const s16* samples, u32 num_frames, Error* error = nullptr);

private:
  using SampleType = s16;

  bool WriteHeader(Error* error);

  std::FILE* m_file = nullptr;
  u32 m_sample_rate = 0;
  u32 m_num_channels = 0;
  u32 m_num_frames = 0;
};
