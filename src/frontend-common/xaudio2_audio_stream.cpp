#include "xaudio2_audio_stream.h"
#include "common/assert.h"
#include "common/log.h"
#include <VersionHelpers.h>
#include <xaudio2.h>
Log_SetChannel(XAudio2AudioStream);

#if !WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
#pragma comment(lib, "xaudio2.lib")
#endif

XAudio2AudioStream::XAudio2AudioStream() = default;

XAudio2AudioStream::~XAudio2AudioStream()
{
  if (IsOpen())
    XAudio2AudioStream::CloseDevice();

#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
  if (m_xaudio2_library)
    FreeLibrary(m_xaudio2_library);

  if (m_com_initialized_by_us)
    CoUninitialize();
#endif
}

bool XAudio2AudioStream::Initialize()
{
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
  m_xaudio2_library = LoadLibraryW(XAUDIO2_DLL_W);
  if (!m_xaudio2_library)
  {
    Log_ErrorPrintf("Failed to load '%s', make sure you're using Windows 10", XAUDIO2_DLL_A);
    return false;
  }
#endif

  return true;
}

bool XAudio2AudioStream::OpenDevice()
{
  DebugAssert(!IsOpen());

  HRESULT hr;
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
  using PFNXAUDIO2CREATE =
    HRESULT(STDAPICALLTYPE*)(IXAudio2 * *ppXAudio2, UINT32 Flags, XAUDIO2_PROCESSOR XAudio2Processor);
  PFNXAUDIO2CREATE xaudio2_create =
    reinterpret_cast<PFNXAUDIO2CREATE>(GetProcAddress(m_xaudio2_library, "XAudio2Create"));
  if (!xaudio2_create)
    return false;

  hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  m_com_initialized_by_us = SUCCEEDED(hr);
  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE && hr != S_FALSE)
  {
    Log_ErrorPrintf("Failed to initialize COM");
    return false;
  }

  hr = xaudio2_create(m_xaudio.ReleaseAndGetAddressOf(), 0, XAUDIO2_DEFAULT_PROCESSOR);
#else
  hr = XAudio2Create(m_xaudio.ReleaseAndGetAddressOf(), 0, XAUDIO2_DEFAULT_PROCESSOR);
#endif
  if (FAILED(hr))
  {
    Log_ErrorPrintf("XAudio2Create() failed: %08X", hr);
    return false;
  }

  hr = m_xaudio->CreateMasteringVoice(&m_mastering_voice, m_channels, m_output_sample_rate, 0, nullptr);
  if (FAILED(hr))
  {
    Log_ErrorPrintf("CreateMasteringVoice() failed: %08X", hr);
    return false;
  }

  WAVEFORMATEX wf = {};
  wf.cbSize = sizeof(wf);
  wf.nAvgBytesPerSec = m_output_sample_rate * m_channels * sizeof(s16);
  wf.nBlockAlign = static_cast<WORD>(sizeof(s16) * m_channels);
  wf.nChannels = static_cast<WORD>(m_channels);
  wf.nSamplesPerSec = m_output_sample_rate;
  wf.wBitsPerSample = sizeof(s16) * 8;
  wf.wFormatTag = WAVE_FORMAT_PCM;
  hr = m_xaudio->CreateSourceVoice(&m_source_voice, &wf, 0, 1.0f, this);
  if (FAILED(hr))
  {
    Log_ErrorPrintf("CreateMasteringVoice() failed: %08X", hr);
    return false;
  }

  hr = m_source_voice->SetFrequencyRatio(1.0f);
  if (FAILED(hr))
  {
    Log_ErrorPrintf("SetFrequencyRatio() failed: %08X", hr);
    return false;
  }

  for (u32 i = 0; i < NUM_BUFFERS; i++)
    m_buffers[i] = std::make_unique<SampleType[]>(m_buffer_size * m_channels);

  return true;
}

void XAudio2AudioStream::PauseDevice(bool paused)
{
  if (m_paused == paused)
    return;

  if (paused)
  {
    HRESULT hr = m_source_voice->Stop(0, 0);
    if (FAILED(hr))
      Log_ErrorPrintf("Stop() failed: %08X", hr);
  }
  else
  {
    HRESULT hr = m_source_voice->Start(0, 0);
    if (FAILED(hr))
      Log_ErrorPrintf("Start() failed: %08X", hr);
  }

  m_paused = paused;
}

void XAudio2AudioStream::CloseDevice()
{
  HRESULT hr;
  if (!m_paused)
  {
    hr = m_source_voice->Stop(0, 0);
    if (FAILED(hr))
      Log_ErrorPrintf("Stop() failed: %08X", hr);
  }

  m_source_voice = nullptr;
  m_mastering_voice = nullptr;
  m_xaudio.Reset();
  m_buffers = {};
  m_current_buffer = 0;
  m_paused = true;
}

void XAudio2AudioStream::FramesAvailable()
{
  if (!m_buffer_enqueued)
  {
    m_buffer_enqueued = true;
    EnqueueBuffer();
  }
}

void XAudio2AudioStream::EnqueueBuffer()
{
  SampleType* samples = m_buffers[m_current_buffer].get();
  ReadFrames(samples, m_buffer_size, false);

  const XAUDIO2_BUFFER buf = {
    static_cast<UINT32>(0),                                        // flags
    static_cast<UINT32>(sizeof(s16) * m_channels * m_buffer_size), // bytes
    reinterpret_cast<const BYTE*>(samples)                         // data
  };

  HRESULT hr = m_source_voice->SubmitSourceBuffer(&buf, nullptr);
  if (FAILED(hr))
    Log_ErrorPrintf("SubmitSourceBuffer() failed: %08X", hr);

  m_current_buffer = (m_current_buffer + 1) % NUM_BUFFERS;
}

void XAudio2AudioStream::SetOutputVolume(u32 volume)
{
  AudioStream::SetOutputVolume(volume);
  HRESULT hr = m_mastering_voice->SetVolume(static_cast<float>(m_output_volume) / 100.0f);
  if (FAILED(hr))
    Log_ErrorPrintf("SetVolume() failed: %08X", hr);
}

void __stdcall XAudio2AudioStream::OnVoiceProcessingPassStart(UINT32 BytesRequired) {}

void __stdcall XAudio2AudioStream::OnVoiceProcessingPassEnd(void) {}

void __stdcall XAudio2AudioStream::OnStreamEnd(void) {}

void __stdcall XAudio2AudioStream::OnBufferStart(void* pBufferContext) {}

void __stdcall XAudio2AudioStream::OnBufferEnd(void* pBufferContext)
{
  EnqueueBuffer();
}

void __stdcall XAudio2AudioStream::OnLoopEnd(void* pBufferContext) {}

void __stdcall XAudio2AudioStream::OnVoiceError(void* pBufferContext, HRESULT Error) {}

namespace FrontendCommon {

std::unique_ptr<AudioStream> CreateXAudio2AudioStream()
{
  std::unique_ptr<XAudio2AudioStream> stream = std::make_unique<XAudio2AudioStream>();
  if (!stream->Initialize())
    return {};

  return stream;
}

} // namespace FrontendCommon