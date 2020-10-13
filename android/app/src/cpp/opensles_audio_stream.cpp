#include "opensles_audio_stream.h"
#include "common/assert.h"
#include "common/log.h"
#include <cmath>
Log_SetChannel(OpenSLESAudioStream);

// Based off Dolphin's OpenSLESStream class.

OpenSLESAudioStream::OpenSLESAudioStream() = default;

OpenSLESAudioStream::~OpenSLESAudioStream()
{
  if (IsOpen())
    OpenSLESAudioStream::CloseDevice();
}

std::unique_ptr<AudioStream> OpenSLESAudioStream::Create()
{
  return std::make_unique<OpenSLESAudioStream>();
}

bool OpenSLESAudioStream::OpenDevice()
{
  DebugAssert(!IsOpen());

  SLresult res = slCreateEngine(&m_engine, 0, nullptr, 0, nullptr, nullptr);
  if (res != SL_RESULT_SUCCESS)
  {
    Log_ErrorPrintf("slCreateEngine failed: %d", res);
    return false;
  }

  res = (*m_engine)->Realize(m_engine, SL_BOOLEAN_FALSE);
  if (res == SL_RESULT_SUCCESS)
    res = (*m_engine)->GetInterface(m_engine, SL_IID_ENGINE, &m_engine_engine);
  if (res == SL_RESULT_SUCCESS)
    res = (*m_engine_engine)->CreateOutputMix(m_engine_engine, &m_output_mix, 0, 0, 0);
  if (res == SL_RESULT_SUCCESS)
    res = (*m_output_mix)->Realize(m_output_mix, SL_BOOLEAN_FALSE);
  if (res != SL_RESULT_SUCCESS)
  {
    Log_ErrorPrintf("Failed to create engine/output mix");
    CloseDevice();
    return false;
  }

  SLDataLocator_AndroidSimpleBufferQueue dloc_bq{SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, NUM_BUFFERS};
  SLDataFormat_PCM format = {SL_DATAFORMAT_PCM,
                             m_channels,
                             m_output_sample_rate * 1000u,
                             SL_PCMSAMPLEFORMAT_FIXED_16,
                             SL_PCMSAMPLEFORMAT_FIXED_16,
                             SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT,
                             SL_BYTEORDER_LITTLEENDIAN};
  SLDataSource dsrc{&dloc_bq, &format};
  SLDataLocator_OutputMix dloc_outputmix{SL_DATALOCATOR_OUTPUTMIX, m_output_mix};
  SLDataSink dsink{&dloc_outputmix, nullptr};

  const std::array<SLInterfaceID, 2> ap_interfaces = {{SL_IID_BUFFERQUEUE, SL_IID_VOLUME}};
  const std::array<SLboolean, 2> ap_interfaces_req = {{true, true}};
  res = (*m_engine_engine)
          ->CreateAudioPlayer(m_engine_engine, &m_player, &dsrc, &dsink, static_cast<u32>(ap_interfaces.size()),
                              ap_interfaces.data(), ap_interfaces_req.data());
  if (res != SL_RESULT_SUCCESS)
  {
    Log_ErrorPrintf("Failed to create audio player: %d", res);
    CloseDevice();
    return false;
  }

  res = (*m_player)->Realize(m_player, SL_BOOLEAN_FALSE);
  if (res == SL_RESULT_SUCCESS)
    res = (*m_player)->GetInterface(m_player, SL_IID_PLAY, &m_play_interface);
  if (res == SL_RESULT_SUCCESS)
    res = (*m_player)->GetInterface(m_player, SL_IID_BUFFERQUEUE, &m_buffer_queue_interface);
  if (res == SL_RESULT_SUCCESS)
    res = (*m_player)->GetInterface(m_player, SL_IID_VOLUME, &m_volume_interface);
  if (res != SL_RESULT_SUCCESS)
  {
    Log_ErrorPrintf("Failed to get player interfaces: %d", res);
    CloseDevice();
    return false;
  }

  res = (*m_buffer_queue_interface)->RegisterCallback(m_buffer_queue_interface, BufferCallback, this);
  if (res != SL_RESULT_SUCCESS)
  {
    Log_ErrorPrintf("Failed to register callback: %d", res);
    CloseDevice();
    return false;
  }

  for (u32 i = 0; i < NUM_BUFFERS; i++)
    m_buffers[i] = std::make_unique<SampleType[]>(m_buffer_size * m_channels);

  return true;
}

void OpenSLESAudioStream::PauseDevice(bool paused)
{
  if (m_paused == paused)
    return;

  (*m_play_interface)->SetPlayState(m_play_interface, paused ? SL_PLAYSTATE_PAUSED : SL_PLAYSTATE_PLAYING);

  if (!paused && !m_buffer_enqueued)
  {
    m_buffer_enqueued = true;
    EnqueueBuffer();
  }

  m_paused = paused;
}

void OpenSLESAudioStream::CloseDevice()
{
  m_buffers = {};
  m_current_buffer = 0;
  m_paused = true;
  m_buffer_enqueued = false;

  if (m_player)
  {
    (*m_player)->Destroy(m_player);
    m_volume_interface = {};
    m_buffer_queue_interface = {};
    m_play_interface = {};
    m_player = {};
  }
  if (m_output_mix)
  {
    (*m_output_mix)->Destroy(m_output_mix);
    m_output_mix = {};
  }
  (*m_engine)->Destroy(m_engine);
  m_engine_engine = {};
  m_engine = {};
}

void OpenSLESAudioStream::SetOutputVolume(u32 volume)
{
  const SLmillibel attenuation = (volume == 0) ?
                                   SL_MILLIBEL_MIN :
                                   static_cast<SLmillibel>(2000.0f * std::log10(static_cast<float>(volume) / 100.0f));
  (*m_volume_interface)->SetVolumeLevel(m_volume_interface, attenuation);
}

void OpenSLESAudioStream::EnqueueBuffer()
{
  SampleType* samples = m_buffers[m_current_buffer].get();
  ReadFrames(samples, m_buffer_size, false);

  SLresult res = (*m_buffer_queue_interface)
                   ->Enqueue(m_buffer_queue_interface, samples, m_buffer_size * m_channels * sizeof(SampleType));
  if (res != SL_RESULT_SUCCESS)
    Log_ErrorPrintf("Enqueue buffer failed: %d", res);

  m_current_buffer = (m_current_buffer + 1) % NUM_BUFFERS;
}

void OpenSLESAudioStream::BufferCallback(SLAndroidSimpleBufferQueueItf buffer_queue, void* context)
{
  OpenSLESAudioStream* const this_ptr = static_cast<OpenSLESAudioStream*>(context);
  this_ptr->EnqueueBuffer();
}

void OpenSLESAudioStream::FramesAvailable() {}
