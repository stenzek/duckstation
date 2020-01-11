#include "cubeb_audio_stream.h"
#include "common/assert.h"
#include "common/log.h"
Log_SetChannel(CubebAudioStream);

CubebAudioStream::CubebAudioStream() = default;

CubebAudioStream::~CubebAudioStream()
{
  if (IsOpen())
    CubebAudioStream::CloseDevice();
}

bool CubebAudioStream::OpenDevice()
{
  Assert(!IsOpen());

  int rv = cubeb_init(&m_cubeb_context, "DuckStation", nullptr);
  if (rv != CUBEB_OK)
  {
    Log_ErrorPrintf("Could not initialize cubeb context: %d", rv);
    return false;
  }

  cubeb_stream_params params = {};
  params.format = CUBEB_SAMPLE_S16LE;
  params.rate = m_output_sample_rate;
  params.channels = m_channels;
  params.layout = CUBEB_LAYOUT_UNDEFINED;
  params.prefs = CUBEB_STREAM_PREF_NONE;

  u32 latency_frames = 0;
  rv = cubeb_get_min_latency(m_cubeb_context, &params, &latency_frames);
  if (rv != CUBEB_OK)
  {
    Log_ErrorPrintf("Could not get minimum latency: %d", rv);
    cubeb_destroy(m_cubeb_context);
    m_cubeb_context = nullptr;
    return false;
  }

  Log_InfoPrintf("Minimum latency in frames: %u", latency_frames);
  if (latency_frames > m_buffer_size)
    Log_WarningPrintf("Minimum latency is above buffer size: %u vs %u", latency_frames, m_buffer_size);
  else
    latency_frames = m_buffer_size;

  char stream_name[32];
  std::snprintf(stream_name, sizeof(stream_name), "AudioStream_%p", this);

  rv = cubeb_stream_init(m_cubeb_context, &m_cubeb_stream, stream_name, nullptr, nullptr, nullptr, &params,
                         latency_frames, DataCallback, StateCallback, this);
  if (rv != CUBEB_OK)
  {
    Log_ErrorPrintf("Could not create stream: %d", rv);
    cubeb_destroy(m_cubeb_context);
    m_cubeb_context = nullptr;
    return false;
  }

  return true;
}

void CubebAudioStream::PauseDevice(bool paused)
{
  if (paused == m_paused)
    return;

  int rv = paused ? cubeb_stream_stop(m_cubeb_stream) : cubeb_stream_start(m_cubeb_stream);
  if (rv != CUBEB_OK)
  {
    Log_ErrorPrintf("cubeb_stream_%s failed: %d", paused ? "stop" : "start", rv);
    return;
  }
}

void CubebAudioStream::CloseDevice()
{
  Assert(IsOpen());

  if (!m_paused)
    cubeb_stream_stop(m_cubeb_stream);

  cubeb_stream_destroy(m_cubeb_stream);
  m_cubeb_stream = nullptr;

  cubeb_destroy(m_cubeb_context);
  m_cubeb_context = nullptr;
}

long CubebAudioStream::DataCallback(cubeb_stream* stm, void* user_ptr, const void* input_buffer, void* output_buffer,
                                    long nframes)
{
  CubebAudioStream* const this_ptr = static_cast<CubebAudioStream*>(user_ptr);

  const u32 read_frames =
    this_ptr->ReadSamples(reinterpret_cast<SampleType*>(output_buffer), static_cast<u32>(nframes));
  const u32 silence_frames = static_cast<u32>(nframes) - read_frames;
  if (silence_frames > 0)
  {
    std::memset(reinterpret_cast<SampleType*>(output_buffer) + (read_frames * this_ptr->m_channels), 0,
                silence_frames * this_ptr->m_channels * sizeof(SampleType));
  }

  return nframes;
}

void CubebAudioStream::StateCallback(cubeb_stream* stream, void* user_ptr, cubeb_state state)
{
  CubebAudioStream* const this_ptr = static_cast<CubebAudioStream*>(user_ptr);

  this_ptr->m_paused = (state != CUBEB_STATE_STARTED);
}

void CubebAudioStream::BufferAvailable() {}

std::unique_ptr<AudioStream> AudioStream::CreateCubebAudioStream()
{
  return std::make_unique<CubebAudioStream>();
}
