#include "xaudio2_audio_stream.h"
#include "common/assert.h"
#include "common/log.h"
#include "common_host.h"
#include <VersionHelpers.h>
#include <xaudio2.h>
Log_SetChannel(XAudio2AudioStream);

#if !WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
#pragma comment(lib, "xaudio2.lib")
#endif

XAudio2AudioStream::XAudio2AudioStream(u32 sample_rate, u32 channels, u32 buffer_ms, AudioStretchMode stretch)
  : AudioStream(sample_rate, channels, buffer_ms, stretch)
{
}

XAudio2AudioStream::~XAudio2AudioStream()
{
  if (IsOpen())
    CloseDevice();

#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
  if (m_xaudio2_library)
    FreeLibrary(m_xaudio2_library);

  if (m_com_initialized_by_us)
    CoUninitialize();
#endif
}

std::unique_ptr<AudioStream> CommonHost::CreateXAudio2Stream(u32 sample_rate, u32 channels, u32 buffer_ms,
                                                             u32 latency_ms, AudioStretchMode stretch)
{
  std::unique_ptr<XAudio2AudioStream> stream(
    std::make_unique<XAudio2AudioStream>(sample_rate, channels, buffer_ms, stretch));
  if (!stream->OpenDevice(latency_ms))
    stream.reset();
  return stream;
}

bool XAudio2AudioStream::OpenDevice(u32 latency_ms)
{
  DebugAssert(!IsOpen());

#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
  m_xaudio2_library = LoadLibraryW(XAUDIO2_DLL_W);
  if (!m_xaudio2_library)
  {
    Log_ErrorPrintf("Failed to load '%s', make sure you're using Windows 10", XAUDIO2_DLL_A);
    return false;
  }
#endif

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

  hr = m_xaudio->CreateMasteringVoice(&m_mastering_voice, m_channels, m_sample_rate, 0, nullptr);
  if (FAILED(hr))
  {
    Log_ErrorPrintf("CreateMasteringVoice() failed: %08X", hr);
    return false;
  }

  WAVEFORMATEX wf = {};
  wf.cbSize = sizeof(wf);
  wf.nAvgBytesPerSec = m_sample_rate * m_channels * sizeof(s16);
  wf.nBlockAlign = static_cast<WORD>(sizeof(s16) * m_channels);
  wf.nChannels = static_cast<WORD>(m_channels);
  wf.nSamplesPerSec = m_sample_rate;
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

  m_enqueue_buffer_size = std::max<u32>(INTERNAL_BUFFER_SIZE, GetBufferSizeForMS(m_sample_rate, latency_ms));
  Log_DevPrintf("Allocating %u buffers of %u frames", NUM_BUFFERS, m_enqueue_buffer_size);
  for (u32 i = 0; i < NUM_BUFFERS; i++)
    m_enqueue_buffers[i] = std::make_unique<SampleType[]>(m_enqueue_buffer_size * m_channels);

  BaseInitialize();
  m_volume = 100;
  m_paused = false;

  hr = m_source_voice->Start(0, 0);
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Start() failed: %08X", hr);
    return false;
  }

  EnqueueBuffer();
  return true;
}

void XAudio2AudioStream::SetPaused(bool paused)
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

  if (!m_buffer_enqueued)
    EnqueueBuffer();
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
  m_enqueue_buffers = {};
  m_current_buffer = 0;
  m_paused = true;
}

void XAudio2AudioStream::EnqueueBuffer()
{
  SampleType* samples = m_enqueue_buffers[m_current_buffer].get();
  ReadFrames(samples, m_enqueue_buffer_size);

  const XAUDIO2_BUFFER buf = {
    static_cast<UINT32>(0),                                                // flags
    static_cast<UINT32>(sizeof(s16) * m_channels * m_enqueue_buffer_size), // bytes
    reinterpret_cast<const BYTE*>(samples)                                 // data
  };

  HRESULT hr = m_source_voice->SubmitSourceBuffer(&buf, nullptr);
  if (FAILED(hr))
    Log_ErrorPrintf("SubmitSourceBuffer() failed: %08X", hr);

  m_current_buffer = (m_current_buffer + 1) % NUM_BUFFERS;
}

void XAudio2AudioStream::SetOutputVolume(u32 volume)
{
  HRESULT hr = m_mastering_voice->SetVolume(static_cast<float>(m_volume) / 100.0f);
  if (FAILED(hr))
  {
    Log_ErrorPrintf("SetVolume() failed: %08X", hr);
    return;
  }

  m_volume = volume;
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
