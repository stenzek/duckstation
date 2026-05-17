// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "audio_stream.h"
#include "translation.h"

#include "common/assert.h"
#include "common/dynamic_library.h"
#include "common/error.h"
#include "common/heap_array.h"
#include "common/log.h"
#include "common/string_util.h"

#include <array>
#include <atomic>
#include <vector>

#include "common/windows_headers.h"

#include <mmdeviceapi.h> // must be included before functiondiscoverykeys_devpkey.h

#include <devpkey.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propsys.h>
#include <wrl/client.h>
#include <xaudio2.h>

LOG_CHANNEL(AudioStream);

namespace {

static constexpr u32 NUM_BUFFERS = 2;

class XAudio2AudioStream final : public AudioStream, private IXAudio2VoiceCallback
{
public:
  XAudio2AudioStream(AudioStreamSource* source, u32 channels);
  ~XAudio2AudioStream() override;

  bool Initialize(u32 sample_rate, u32 channels, u32 output_latency_frames, bool output_latency_minimal,
                  std::string_view device_name, bool auto_start, Error* error);

  bool Start(Error* error) override;
  bool Stop(Error* error) override;

private:
  s16* GetBufferPointer(u32 buffer_index);

  // IXAudio2VoiceCallback — only OnBufferEnd needs a body
  void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32 BytesRequired) override {}
  void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
  void STDMETHODCALLTYPE OnStreamEnd() override {}
  void STDMETHODCALLTYPE OnBufferStart(void* pBufferContext) override {}
  void STDMETHODCALLTYPE OnBufferEnd(void* pBufferContext) override;
  void STDMETHODCALLTYPE OnLoopEnd(void* pBufferContext) override {}
  void STDMETHODCALLTYPE OnVoiceError(void* pBufferContext, HRESULT Error) override {}

  AudioStreamSource* m_source;
  u32 m_buffer_frames = 0;
  u32 m_channels;

  DynamicLibrary m_library;

  Microsoft::WRL::ComPtr<IXAudio2> m_xaudio2;
  IXAudio2MasteringVoice* m_mastering_voice = nullptr;
  IXAudio2SourceVoice* m_source_voice = nullptr;

  DynamicHeapArray<s16> m_buffer;
  std::atomic_bool m_shutting_down{false};
};

} // namespace

XAudio2AudioStream::XAudio2AudioStream(AudioStreamSource* source, u32 channels) : m_source(source), m_channels(channels)
{
}

XAudio2AudioStream::~XAudio2AudioStream()
{
  if (m_source_voice)
  {
    m_shutting_down.store(true, std::memory_order_release);

    m_source_voice->Stop(0);
    m_source_voice->FlushSourceBuffers();
    m_source_voice->DestroyVoice();
    m_source_voice = nullptr;
  }

  if (m_mastering_voice)
  {
    m_mastering_voice->DestroyVoice();
    m_mastering_voice = nullptr;
  }

  m_xaudio2.Reset();
}

bool XAudio2AudioStream::Initialize(u32 sample_rate, u32 channels, u32 output_latency_frames,
                                    bool output_latency_minimal, std::string_view device_name, bool auto_start,
                                    Error* error)
{
  if (!m_library.IsOpen() && !m_library.Open(XAUDIO2_DLL_A, error))
    return false;

  HRESULT(WINAPI * pXAudio2Create)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR);
  if (!m_library.GetSymbol("XAudio2Create", &pXAudio2Create))
  {
    Error::SetWin32(error, "Failed to get XAudio2Create function: ", GetLastError());
    return false;
  }

  HRESULT hr = pXAudio2Create(m_xaudio2.GetAddressOf(), 0, XAUDIO2_DEFAULT_PROCESSOR);
  if (FAILED(hr))
  {
    Error::SetHResult(error, "XAudio2Create() failed: ", hr);
    return false;
  }

  // Convert optional device name from UTF-8 to wide for CreateMasteringVoice.
  std::wstring device_id;
  if (!device_name.empty())
    device_id = StringUtil::UTF8StringToWideString(device_name);

  hr = m_xaudio2->CreateMasteringVoice(&m_mastering_voice, channels, sample_rate, 0,
                                       device_id.empty() ? nullptr : device_id.c_str());
  if (FAILED(hr))
  {
    if (!device_name.empty())
    {
      // Try with the default device if a specific one was requested, in case the requested device
      // is invalid or unavailable.
      ERROR_LOG("IXAudio2::CreateMasteringVoice() for specific device failed: {:08X}", static_cast<unsigned>(hr));
      hr = m_xaudio2->CreateMasteringVoice(&m_mastering_voice, channels, sample_rate, 0, nullptr);
      if (SUCCEEDED(hr))
      {
        WARNING_LOG("IXAudio2::CreateMasteringVoice() succeeded with default device after specific device failed, "
                    "ignoring requested device '{}'",
                    device_name);
      }
    }

    if (FAILED(hr))
    {
      Error::SetHResult(error, "IXAudio2::CreateMasteringVoice() failed: ", hr);
      return false;
    }
  }

  WAVEFORMATEX wfx = {};
  wfx.wFormatTag = WAVE_FORMAT_PCM;
  wfx.nChannels = static_cast<WORD>(channels);
  wfx.nSamplesPerSec = sample_rate;
  wfx.wBitsPerSample = 16;
  wfx.nBlockAlign = static_cast<WORD>((channels * 16) / 8);
  wfx.nAvgBytesPerSec = sample_rate * wfx.nBlockAlign;
  wfx.cbSize = 0;

  hr = m_xaudio2->CreateSourceVoice(&m_source_voice, &wfx, 0, 1.0f, this);
  if (FAILED(hr))
  {
    Error::SetHResult(error, "IXAudio2::CreateSourceVoice() failed: ", hr);
    return false;
  }

  m_buffer_frames = output_latency_frames;
  m_buffer.resize(NUM_BUFFERS * m_buffer_frames * channels);

  // Pre-fill with a single frame buffers and enqueue them so playback can begin immediately.
  for (u32 i = 0; i < NUM_BUFFERS; i++)
  {
    XAUDIO2_BUFFER buffer = {};
    buffer.AudioBytes = sizeof(s16) * channels;
    buffer.pAudioData = reinterpret_cast<const BYTE*>(GetBufferPointer(i));
    buffer.pContext = reinterpret_cast<void*>(static_cast<uintptr_t>(i));

    hr = m_source_voice->SubmitSourceBuffer(&buffer);
    if (FAILED(hr))
    {
      Error::SetHResult(error, "IXAudio2SourceVoice::SubmitSourceBuffer() failed: ", hr);
      return false;
    }
  }

  if (auto_start)
  {
    hr = m_source_voice->Start(0);
    if (FAILED(hr))
    {
      Error::SetHResult(error, "IXAudio2SourceVoice::Start() failed: ", hr);
      return false;
    }
  }

  INFO_LOG("XAudio2 stream initialized: {}hz, {} channels, {} frames/buffer ({} ms)", sample_rate, channels,
           output_latency_frames, FramesToMS(sample_rate, output_latency_frames));
  return true;
}

s16* XAudio2AudioStream::GetBufferPointer(u32 buffer_index)
{
  DebugAssert(((buffer_index * (m_channels * m_buffer_frames)) + m_buffer_frames) <= m_buffer.size());
  return &m_buffer[buffer_index * (m_channels * m_buffer_frames)];
}

void XAudio2AudioStream::OnBufferEnd(void* pBufferContext)
{
  if (m_shutting_down.load(std::memory_order_acquire))
    return;
  const u32 buffer_idx = static_cast<u32>(reinterpret_cast<uintptr_t>(pBufferContext));
  s16* const buffer_ptr = GetBufferPointer(buffer_idx);
  m_source->ReadFrames(buffer_ptr, m_buffer_frames);

  XAUDIO2_BUFFER buffer = {};
  buffer.AudioBytes = static_cast<UINT32>(m_buffer_frames * m_channels * sizeof(s16));
  buffer.pAudioData = reinterpret_cast<const BYTE*>(buffer_ptr);
  buffer.pContext = pBufferContext;
  m_source_voice->SubmitSourceBuffer(&buffer);
}

bool XAudio2AudioStream::Start(Error* error)
{
  const HRESULT hr = m_source_voice->Start(0);
  if (FAILED(hr))
  {
    Error::SetHResult(error, "IXAudio2SourceVoice::Start() failed: ", hr);
    return false;
  }

  return true;
}

bool XAudio2AudioStream::Stop(Error* error)
{
  const HRESULT hr = m_source_voice->Stop(0);
  if (FAILED(hr))
  {
    Error::SetHResult(error, "IXAudio2SourceVoice::Stop() failed: ", hr);
    return false;
  }

  return true;
}

std::vector<AudioStream::DeviceInfo> AudioStream::GetXAudio2OutputDevices(u32 sample_rate)
{
  std::vector<AudioStream::DeviceInfo> ret;
  ret.emplace_back(std::string(), TRANSLATE_STR("AudioStream", "Default"), 0);

  Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(enumerator.GetAddressOf()));
  if (FAILED(hr))
  {
    WARNING_LOG("CoCreateInstance(IMMDeviceEnumerator) failed: {:08X}", static_cast<unsigned>(hr));
    return ret;
  }

  Microsoft::WRL::ComPtr<IMMDeviceCollection> devices;
  hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, devices.GetAddressOf());
  if (FAILED(hr))
  {
    WARNING_LOG("IMMDeviceEnumerator::EnumAudioEndpoints() failed: {:08X}", static_cast<unsigned>(hr));
    return ret;
  }

  UINT count = 0;
  devices->GetCount(&count);

  for (UINT i = 0; i < count; i++)
  {
    Microsoft::WRL::ComPtr<IMMDevice> device;
    if (FAILED(devices->Item(i, device.GetAddressOf())))
      continue;

    // The WASAPI device ID is what XAudio2 expects for CreateMasteringVoice.
    LPWSTR device_id_wide = nullptr;
    if (FAILED(device->GetId(&device_id_wide)))
      continue;

    std::string device_id = StringUtil::WideStringToUTF8String(device_id_wide);
    CoTaskMemFree(device_id_wide);

    std::string friendly_name;
    Microsoft::WRL::ComPtr<IPropertyStore> props;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, props.GetAddressOf())))
    {
      PROPVARIANT pv;
      PropVariantInit(&pv);
      if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
        friendly_name = StringUtil::WideStringToUTF8String(pv.pwszVal);
      PropVariantClear(&pv);
    }

    ret.emplace_back(std::move(device_id), friendly_name.empty() ? ret.back().name : std::move(friendly_name), 0);
  }

  return ret;
}

std::unique_ptr<AudioStream>
AudioStream::CreateXAudio2AudioStream(u32 sample_rate, u32 channels, u32 output_latency_frames,
                                      bool output_latency_minimal, std::string_view device_name,
                                      AudioStreamSource* source, bool auto_start, Error* error)
{
  std::unique_ptr<XAudio2AudioStream> stream = std::make_unique<XAudio2AudioStream>(source, channels);
  if (!stream->Initialize(sample_rate, channels, output_latency_frames, output_latency_minimal, device_name, auto_start,
                          error))
  {
    stream.reset();
  }

  return stream;
}
