#pragma once
#include "common/audio_stream.h"
#include "cubeb/cubeb.h"
#include <cstdint>

class CubebAudioStream final : public AudioStream
{
public:
  CubebAudioStream();
  ~CubebAudioStream();

protected:
  bool IsOpen() const { return m_cubeb_stream != nullptr; }

  bool OpenDevice() override;
  void PauseDevice(bool paused) override;
  void CloseDevice() override;
  void BufferAvailable() override;

  static long DataCallback(cubeb_stream* stm, void* user_ptr, const void* input_buffer, void* output_buffer,
                           long nframes);
  static void StateCallback(cubeb_stream* stream, void* user_ptr, cubeb_state state);

  cubeb* m_cubeb_context = nullptr;
  cubeb_stream* m_cubeb_stream = nullptr;
  bool m_paused = true;
};
