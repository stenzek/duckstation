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
  XAudio2AudioStream();
  ~XAudio2AudioStream();

  bool Initialize();

  void SetOutputVolume(u32 volume) override;

protected:
  enum : u32
  {
    NUM_BUFFERS = 2
  };

  ALWAYS_INLINE bool IsOpen() const { return static_cast<bool>(m_xaudio); }

  bool OpenDevice() override;
  void PauseDevice(bool paused) override;
  void CloseDevice() override;
  void FramesAvailable() override;

  // Inherited via IXAudio2VoiceCallback
  virtual void __stdcall OnVoiceProcessingPassStart(UINT32 BytesRequired) override;
  virtual void __stdcall OnVoiceProcessingPassEnd(void) override;
  virtual void __stdcall OnStreamEnd(void) override;
  virtual void __stdcall OnBufferStart(void* pBufferContext) override;
  virtual void __stdcall OnBufferEnd(void* pBufferContext) override;
  virtual void __stdcall OnLoopEnd(void* pBufferContext) override;
  virtual void __stdcall OnVoiceError(void* pBufferContext, HRESULT Error) override;

  void EnqueueBuffer();

  Microsoft::WRL::ComPtr<IXAudio2> m_xaudio;
  IXAudio2MasteringVoice* m_mastering_voice = nullptr;
  IXAudio2SourceVoice* m_source_voice = nullptr;

  std::array<std::unique_ptr<SampleType[]>, NUM_BUFFERS> m_buffers;
  u32 m_current_buffer = 0;
  bool m_buffer_enqueued = false;
  bool m_paused = true;

#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
  HMODULE m_xaudio2_library = {};
  bool m_com_initialized_by_us = false;
#endif
};
