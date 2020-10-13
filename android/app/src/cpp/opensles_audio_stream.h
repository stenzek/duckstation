#pragma once
#include "common/audio_stream.h"
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <array>
#include <memory>

class OpenSLESAudioStream final : public AudioStream
{
public:
  OpenSLESAudioStream();
  ~OpenSLESAudioStream();

  static std::unique_ptr<AudioStream> Create();

  void SetOutputVolume(u32 volume) override;

protected:
  enum : u32
  {
    NUM_BUFFERS = 2
  };

  ALWAYS_INLINE bool IsOpen() const { return (m_engine != nullptr); }

  bool OpenDevice() override;
  void PauseDevice(bool paused) override;
  void CloseDevice() override;
  void FramesAvailable() override;

  void EnqueueBuffer();

  static void BufferCallback(SLAndroidSimpleBufferQueueItf buffer_queue, void* context);

  SLObjectItf m_engine{};
  SLEngineItf m_engine_engine{};
  SLObjectItf m_output_mix{};

  SLObjectItf m_player{};
  SLPlayItf m_play_interface{};
  SLAndroidSimpleBufferQueueItf m_buffer_queue_interface{};
  SLVolumeItf m_volume_interface{};

  std::array<std::unique_ptr<SampleType[]>, NUM_BUFFERS> m_buffers;
  u32 m_current_buffer = 0;
  bool m_paused = true;
  bool m_buffer_enqueued = false;
};
