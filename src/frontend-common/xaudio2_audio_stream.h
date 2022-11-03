#pragma once
#include "common/windows_headers.h"
#include "util/audio_stream.h"
#include <array>
#include <cstdint>
#include <memory>
#include <wrl/client.h>

// We need to use the Windows 10 headers otherwise this won't compile.
#undef _WIN32_WINNT
#define _WIN32_WINNT _WIN32_WINNT_WIN10
#include <xaudio2.h>

class XAudio2AudioStream final : public AudioStream, private IXAudio2VoiceCallback
{
public:
  XAudio2AudioStream(u32 sample_rate, u32 channels, u32 buffer_ms, AudioStretchMode stretch);
  ~XAudio2AudioStream();

  void SetPaused(bool paused) override;
  void SetOutputVolume(u32 volume) override;

  bool OpenDevice(u32 latency_ms);
  void CloseDevice();
  void EnqueueBuffer();

private:
  enum : u32
  {
    NUM_BUFFERS = 2,
    INTERNAL_BUFFER_SIZE = 512,
  };

  ALWAYS_INLINE bool IsOpen() const { return static_cast<bool>(m_xaudio); }

  // Inherited via IXAudio2VoiceCallback
  void __stdcall OnVoiceProcessingPassStart(UINT32 BytesRequired) override;
  void __stdcall OnVoiceProcessingPassEnd(void) override;
  void __stdcall OnStreamEnd(void) override;
  void __stdcall OnBufferStart(void* pBufferContext) override;
  void __stdcall OnBufferEnd(void* pBufferContext) override;
  void __stdcall OnLoopEnd(void* pBufferContext) override;
  void __stdcall OnVoiceError(void* pBufferContext, HRESULT Error) override;

  Microsoft::WRL::ComPtr<IXAudio2> m_xaudio;
  IXAudio2MasteringVoice* m_mastering_voice = nullptr;
  IXAudio2SourceVoice* m_source_voice = nullptr;

  std::array<std::unique_ptr<SampleType[]>, NUM_BUFFERS> m_enqueue_buffers;
  u32 m_enqueue_buffer_size = 0;
  u32 m_current_buffer = 0;
  bool m_buffer_enqueued = false;

  HMODULE m_xaudio2_library = {};
  bool m_com_initialized_by_us = false;
};
