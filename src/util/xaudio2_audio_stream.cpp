// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "util/audio_stream.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/log.h"
#include "common/windows_headers.h"

#include <array>
#include <cstdint>
#include <memory>
#include <wrl/client.h>
#include <xaudio2.h>

Log_SetChannel(XAudio2AudioStream);

namespace {

class XAudio2AudioStream final : public AudioStream, private IXAudio2VoiceCallback
{
public:
  XAudio2AudioStream(u32 sample_rate, const AudioStreamParameters& parameters);
  ~XAudio2AudioStream();

  void SetPaused(bool paused) override;
  void SetOutputVolume(u32 volume) override;

  bool OpenDevice(Error* error);
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
  bool m_com_initialized_by_us = false;
};

} // namespace

XAudio2AudioStream::XAudio2AudioStream(u32 sample_rate, const AudioStreamParameters& parameters)
  : AudioStream(sample_rate, parameters)
{
}

XAudio2AudioStream::~XAudio2AudioStream()
{
  if (IsOpen())
    CloseDevice();

  if (m_com_initialized_by_us)
    CoUninitialize();
}

std::unique_ptr<AudioStream> AudioStream::CreateXAudio2Stream(u32 sample_rate, const AudioStreamParameters& parameters,
                                                              Error* error)
{
  std::unique_ptr<XAudio2AudioStream> stream(std::make_unique<XAudio2AudioStream>(sample_rate, parameters));
  if (!stream->OpenDevice(error))
    stream.reset();
  return stream;
}

bool XAudio2AudioStream::OpenDevice(Error* error)
{
  DebugAssert(!IsOpen());

  if (m_parameters.expansion_mode == AudioExpansionMode::QuadraphonicLFE)
  {
    Log_ErrorPrint("QuadraphonicLFE is not supported by XAudio2.");
    return false;
  }

  static constexpr const std::array<SampleReader, static_cast<size_t>(AudioExpansionMode::Count)> sample_readers = {{
    // Disabled
    &StereoSampleReaderImpl,
    // StereoLFE
    &SampleReaderImpl<AudioExpansionMode::StereoLFE, READ_CHANNEL_FRONT_LEFT, READ_CHANNEL_FRONT_RIGHT,
                      READ_CHANNEL_LFE>,
    // Quadraphonic
    &SampleReaderImpl<AudioExpansionMode::Quadraphonic, READ_CHANNEL_FRONT_LEFT, READ_CHANNEL_FRONT_RIGHT,
                      READ_CHANNEL_REAR_LEFT, READ_CHANNEL_REAR_RIGHT>,
    // QuadraphonicLFE
    nullptr,
    // Surround51
    &SampleReaderImpl<AudioExpansionMode::Surround51, READ_CHANNEL_FRONT_LEFT, READ_CHANNEL_FRONT_RIGHT,
                      READ_CHANNEL_FRONT_CENTER, READ_CHANNEL_LFE, READ_CHANNEL_REAR_LEFT, READ_CHANNEL_REAR_RIGHT>,
    // Surround71
    &SampleReaderImpl<AudioExpansionMode::Surround71, READ_CHANNEL_FRONT_LEFT, READ_CHANNEL_FRONT_RIGHT,
                      READ_CHANNEL_FRONT_CENTER, READ_CHANNEL_LFE, READ_CHANNEL_REAR_LEFT, READ_CHANNEL_REAR_RIGHT,
                      READ_CHANNEL_SIDE_LEFT, READ_CHANNEL_SIDE_RIGHT>,
  }};

  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  m_com_initialized_by_us = SUCCEEDED(hr);
  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE && hr != S_FALSE)
  {
    Error::SetHResult(error, "CoInitializeEx() failed: ", hr);
    return false;
  }

  hr = XAudio2Create(m_xaudio.ReleaseAndGetAddressOf(), 0, XAUDIO2_DEFAULT_PROCESSOR);
  if (FAILED(hr))
  {
    Error::SetHResult(error, "XAudio2Create() failed: ", hr);
    return false;
  }

  hr = m_xaudio->CreateMasteringVoice(&m_mastering_voice, m_output_channels, m_sample_rate, 0, nullptr);
  if (FAILED(hr))
  {
    Error::SetHResult(error, "CreateMasteringVoice() failed: ", hr);
    return false;
  }

  // TODO: CHANNEL LAYOUT
  WAVEFORMATEX wf = {};
  wf.cbSize = sizeof(wf);
  wf.nAvgBytesPerSec = m_sample_rate * m_output_channels * sizeof(s16);
  wf.nBlockAlign = static_cast<WORD>(sizeof(s16) * m_output_channels);
  wf.nChannels = static_cast<WORD>(m_output_channels);
  wf.nSamplesPerSec = m_sample_rate;
  wf.wBitsPerSample = sizeof(s16) * 8;
  wf.wFormatTag = WAVE_FORMAT_PCM;
  hr = m_xaudio->CreateSourceVoice(&m_source_voice, &wf, 0, 1.0f, this);
  if (FAILED(hr))
  {
    Error::SetHResult(error, "CreateMasteringVoice() failed: ", hr);
    return false;
  }

  hr = m_source_voice->SetFrequencyRatio(1.0f);
  if (FAILED(hr))
  {
    Error::SetHResult(error, "SetFrequencyRatio() failed: ", hr);
    return false;
  }

  m_enqueue_buffer_size =
    std::max<u32>(INTERNAL_BUFFER_SIZE, GetBufferSizeForMS(m_sample_rate, (m_parameters.output_latency_ms == 0) ?
                                                                            m_parameters.buffer_ms :
                                                                            m_parameters.output_latency_ms));
  Log_DevPrintf("Allocating %u buffers of %u frames", NUM_BUFFERS, m_enqueue_buffer_size);
  for (u32 i = 0; i < NUM_BUFFERS; i++)
    m_enqueue_buffers[i] = std::make_unique<SampleType[]>(m_enqueue_buffer_size * m_output_channels);

  BaseInitialize(sample_readers[static_cast<size_t>(m_parameters.expansion_mode)]);
  m_volume = 100;
  m_paused = false;

  hr = m_source_voice->Start(0, 0);
  if (FAILED(hr))
  {
    Error::SetHResult(error, "Start() failed: ", hr);
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
    static_cast<UINT32>(0),                                                       // flags
    static_cast<UINT32>(sizeof(s16) * m_output_channels * m_enqueue_buffer_size), // bytes
    reinterpret_cast<const BYTE*>(samples),                                       // data
    0u,
    0u,
    0u,
    0u,
    0u,
    nullptr,
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

void __stdcall XAudio2AudioStream::OnVoiceProcessingPassStart(UINT32 BytesRequired)
{
}

void __stdcall XAudio2AudioStream::OnVoiceProcessingPassEnd(void)
{
}

void __stdcall XAudio2AudioStream::OnStreamEnd(void)
{
}

void __stdcall XAudio2AudioStream::OnBufferStart(void* pBufferContext)
{
}

void __stdcall XAudio2AudioStream::OnBufferEnd(void* pBufferContext)
{
  EnqueueBuffer();
}

void __stdcall XAudio2AudioStream::OnLoopEnd(void* pBufferContext)
{
}

void __stdcall XAudio2AudioStream::OnVoiceError(void* pBufferContext, HRESULT Error)
{
}
