#pragma once
#include "util/audio_stream.h"
#include <cstdint>

struct cubeb;
struct cubeb_stream;

class CubebAudioStream : public AudioStream
{
public:
  CubebAudioStream(u32 sample_rate, u32 channels, u32 buffer_ms, AudioStretchMode stretch);
  ~CubebAudioStream();

  void SetPaused(bool paused) override;
  void SetOutputVolume(u32 volume) override;

  bool Initialize(u32 latency_ms);

private:
  static void LogCallback(const char* fmt, ...);
  static long DataCallback(cubeb_stream* stm, void* user_ptr, const void* input_buffer, void* output_buffer,
                           long nframes);

  void DestroyContextAndStream();

  cubeb* m_context = nullptr;
  cubeb_stream* stream = nullptr;

#ifdef _WIN32
  bool m_com_initialized_by_us = false;
#endif
};
