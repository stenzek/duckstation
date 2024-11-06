// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/types.h"

#include <cstdio>

class Error;

class WAVReader
{
public:
  WAVReader();
  ~WAVReader();

  ALWAYS_INLINE u32 GetSampleRate() const { return m_sample_rate; }
  ALWAYS_INLINE u32 GetNumChannels() const { return m_num_channels; }
  ALWAYS_INLINE u32 GetNumFrames() const { return m_num_frames; }
  ALWAYS_INLINE bool IsOpen() const { return (m_file != nullptr); }

  bool Open(const char* path, Error* error = nullptr);
  void Close();

  bool SeekToFrame(u32 num, Error* error = nullptr);

  bool ReadFrames(s16* samples, u32 num_frames, Error* error = nullptr);

private:
  using SampleType = s16;

  std::FILE* m_file = nullptr;
  s64 m_frames_start = 0;
  u32 m_sample_rate = 0;
  u32 m_num_channels = 0;
  u32 m_num_frames = 0;
};

class WAVWriter
{
public:
  WAVWriter();
  ~WAVWriter();

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
