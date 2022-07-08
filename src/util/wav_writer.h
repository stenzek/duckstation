#pragma once
#include "common/types.h"
#include <cstdio>

namespace Common {

class WAVWriter
{
public:
  WAVWriter();
  ~WAVWriter();

  ALWAYS_INLINE u32 GetSampleRate() const { return m_sample_rate; }
  ALWAYS_INLINE u32 GetNumChannels() const { return m_num_channels; }
  ALWAYS_INLINE u32 GetNumFrames() const { return m_num_frames; }
  ALWAYS_INLINE bool IsOpen() const { return (m_file != nullptr); }

  bool Open(const char* filename, u32 sample_rate, u32 num_channels);
  void Close();

  void WriteFrames(const s16* samples, u32 num_frames);

private:
  using SampleType = s16;

  bool WriteHeader();

  std::FILE* m_file = nullptr;
  u32 m_sample_rate = 0;
  u32 m_num_channels = 0;
  u32 m_num_frames = 0;
};

} // namespace Common