#pragma once
#include "cubeb/cubeb.h"
#include "util/audio_stream.h"
#include <cstdint>

class CubebAudioStream final : public AudioStream
{
public:
  CubebAudioStream();
  ~CubebAudioStream();

  static std::unique_ptr<AudioStream> Create();

protected:
  bool IsOpen() const { return m_cubeb_stream != nullptr; }

  bool OpenDevice() override;
  void PauseDevice(bool paused) override;
  void CloseDevice() override;
  void FramesAvailable() override;
  void SetOutputVolume(u32 volume) override;

  void DestroyContext();

  static long DataCallback(cubeb_stream* stm, void* user_ptr, const void* input_buffer, void* output_buffer,
                           long nframes);
  static void StateCallback(cubeb_stream* stream, void* user_ptr, cubeb_state state);

  cubeb* m_cubeb_context = nullptr;
  cubeb_stream* m_cubeb_stream = nullptr;
  bool m_paused = true;

#ifdef _WIN32
  bool m_com_initialized_by_us = false;
#endif
};
