#include "audio.h"
#include "samplerate.h"
#include <cmath>
#include <cstring>

namespace Audio {

size_t GetBytesPerSample(SampleFormat format)
{
  switch (format)
  {
    case SampleFormat::Signed8:
    case SampleFormat::Unsigned8:
      return sizeof(u8);

    case SampleFormat::Signed16:
    case SampleFormat::Unsigned16:
      return sizeof(u16);

    case SampleFormat::Signed32:
      return sizeof(s32);

    case SampleFormat::Float32:
      return sizeof(float);
  }

  Panic("Unhandled format");
  return 1;
}

Mixer::Mixer(float output_sample_rate) : m_output_sample_rate(output_sample_rate)
{
  // Render/mix buffers are allocated on-demand.
  m_output_buffer = std::make_unique<CircularBuffer>(size_t(output_sample_rate * OutputBufferLengthInSeconds) *
                                                     NumOutputChannels * sizeof(OutputFormatType));
}

Mixer::~Mixer() {}

Channel* Mixer::CreateChannel(const char* name, float sample_rate, SampleFormat format, size_t channels)
{
  Assert(!GetChannelByName(name));

  std::unique_ptr<Channel> channel =
    std::make_unique<Channel>(name, m_output_sample_rate, sample_rate, format, channels);
  m_channels.push_back(std::move(channel));
  return m_channels.back().get();
}

void Mixer::RemoveChannel(Channel* channel)
{
  for (auto iter = m_channels.begin(); iter != m_channels.end(); iter++)
  {
    if (iter->get() == channel)
    {
      m_channels.erase(iter);
      return;
    }
  }

  Panic("Removing unknown channel.");
}

Channel* Mixer::GetChannelByName(const char* name)
{
  for (auto& channel : m_channels)
  {
    if (channel->GetName().Compare(name))
      return channel.get();
  }

  return nullptr;
}

void Mixer::ClearBuffers()
{
  for (const auto& channel : m_channels)
    channel->ClearBuffer();
}

void Mixer::CheckRenderBufferSize(size_t num_samples)
{
  size_t buffer_size = num_samples * NumOutputChannels * sizeof(OutputFormatType);
  if (m_render_buffer.size() < buffer_size)
    m_render_buffer.resize(buffer_size);
}

AudioBuffer::AudioBuffer(size_t size) : m_buffer(size) {}

size_t AudioBuffer::GetBufferUsed() const
{
  return m_used;
}

size_t AudioBuffer::GetContiguousBufferSpace() const
{
  return m_buffer.size() - m_used;
}

void AudioBuffer::Clear()
{
  m_used = 0;
}

bool AudioBuffer::Read(void* dst, size_t len)
{
  if (len > m_used)
    return false;

  std::memcpy(dst, m_buffer.data(), len);
  m_used -= len;
  if (m_used > 0)
    std::memmove(m_buffer.data(), m_buffer.data() + len, m_used);

  return true;
}

bool AudioBuffer::GetWritePointer(void** ptr, size_t* len)
{
  size_t free = GetContiguousBufferSpace();
  if (*len > free)
    return false;

  *len = free;
  *ptr = m_buffer.data() + m_used;
  return true;
}

void AudioBuffer::MoveWritePointer(size_t len)
{
  DebugAssert(m_used + len <= m_buffer.size());
  m_used += len;
}

bool AudioBuffer::GetReadPointer(const void** ppReadPointer, size_t* pByteCount) const
{
  if (m_used == 0)
    return false;

  *ppReadPointer = m_buffer.data();
  *pByteCount = m_used;
  return true;
}

void AudioBuffer::MoveReadPointer(size_t byteCount)
{
  DebugAssert(byteCount <= m_used);
  m_used -= byteCount;
  if (m_used > 0)
    std::memmove(m_buffer.data(), m_buffer.data() + byteCount, m_used);
}

Channel::Channel(const char* name, float output_sample_rate, float input_sample_rate, SampleFormat format,
                 size_t channels)
  : m_name(name), m_input_sample_rate(input_sample_rate), m_output_sample_rate(output_sample_rate), m_format(format),
    m_channels(channels), m_enabled(true), m_input_sample_size(GetBytesPerSample(format)),
    m_input_frame_size(GetBytesPerSample(format) * channels), m_output_frame_size(sizeof(float) * channels),
    m_input_buffer(u32(float(InputBufferLengthInSeconds* input_sample_rate)) * channels * m_input_sample_size),
    m_output_buffer(u32(float(InputBufferLengthInSeconds* output_sample_rate)) * channels * sizeof(OutputFormatType)),
    m_resample_buffer(u32(float(InputBufferLengthInSeconds* output_sample_rate)) * channels),
    m_resample_ratio(double(output_sample_rate) / double(input_sample_rate)),
    m_resampler_state(src_new(SRC_SINC_FASTEST, int(channels), nullptr))
{
  Assert(m_resampler_state != nullptr);
}

Channel::~Channel()
{
  src_delete(reinterpret_cast<SRC_STATE*>(m_resampler_state));
}

size_t Channel::GetFreeInputSamples()
{
  MutexLock lock(m_lock);
  return m_input_buffer.GetContiguousBufferSpace() / m_input_frame_size;
}

void* Channel::ReserveInputSamples(size_t sample_count)
{
  void* write_ptr;
  size_t byte_count = sample_count * m_input_frame_size;

  m_lock.Lock();

  // When the speed limiter is off, we can easily exceed the audio buffer length.
  // In this case, just destroy the oldest samples, wrapping around.
  while (!m_input_buffer.GetWritePointer(&write_ptr, &byte_count))
  {
    size_t bytes_to_remove = byte_count - m_input_buffer.GetContiguousBufferSpace();
    m_input_buffer.MoveReadPointer(bytes_to_remove);
  }

  return write_ptr;
}

void Channel::CommitInputSamples(size_t sample_count)
{
  size_t byte_count = sample_count * m_input_frame_size;
  m_input_buffer.MoveWritePointer(byte_count);

  m_lock.Unlock();
}

void Channel::ReadSamples(float* destination, size_t num_samples)
{
  MutexLock lock(m_lock);

  while (num_samples > 0)
  {
    // Can we use what we have buffered?
    size_t currently_buffered = m_output_buffer.GetBufferUsed() / m_output_frame_size;
    if (currently_buffered > 0)
    {
      size_t to_read = std::min(num_samples, currently_buffered);
      m_output_buffer.Read(destination, to_read * m_output_frame_size);
      destination += to_read;
      num_samples -= to_read;
      if (num_samples == 0)
        break;
    }

    // Resample num_samples samples
    if (m_input_buffer.GetBufferUsed() > 0)
    {
      if (ResampleInput(num_samples))
        continue;
    }

    // If we hit here, it's because we're out of input data.
    std::memset(destination, 0, num_samples * m_output_frame_size);
    break;
  }
}

void Channel::ChangeSampleRate(float new_sample_rate)
{
  MutexLock lock(m_lock);
  InternalClearBuffer();

  // Calculate the new ratio.
  m_input_sample_rate = new_sample_rate;
  m_resample_ratio = double(m_output_sample_rate) / double(new_sample_rate);
}

void Channel::ClearBuffer()
{
  MutexLock lock(m_lock);
  InternalClearBuffer();
}

void Channel::InternalClearBuffer()
{
  m_input_buffer.Clear();
  src_reset(reinterpret_cast<SRC_STATE*>(m_resampler_state));
  m_output_buffer.Clear();
}

bool Channel::ResampleInput(size_t num_output_samples)
{
  const void* in_buf;
  size_t in_bufsize;
  size_t in_num_frames;
  size_t in_num_samples;
  if (!m_input_buffer.GetReadPointer(&in_buf, &in_bufsize))
    return false;

  in_num_frames = in_bufsize / m_input_frame_size;
  in_num_samples = in_num_frames * m_channels;
  if (in_num_frames == 0)
    return false;

  // Cap output samples at buffer size.
  num_output_samples = std::min(num_output_samples, m_output_buffer.GetContiguousBufferSpace() / m_output_frame_size);
  Assert((num_output_samples * m_channels) < m_resample_buffer.size());

  // Only use as many input samples as needed.
  void* out_buf;
  size_t out_bufsize = num_output_samples * m_output_frame_size;
  if (!m_output_buffer.GetWritePointer(&out_buf, &out_bufsize))
    return false;

  // Set up resampling.
  SRC_DATA resample_data;
  resample_data.data_out = reinterpret_cast<float*>(out_buf);
  resample_data.output_frames = static_cast<long>(num_output_samples);
  resample_data.input_frames_used = 0;
  resample_data.output_frames_gen = 0;
  resample_data.end_of_input = 0;
  resample_data.src_ratio = m_resample_ratio;

  // Convert from whatever format the input is in to float.
  switch (m_format)
  {
    case SampleFormat::Signed8:
    {
      const s8* in_samples_typed = reinterpret_cast<const s8*>(in_buf);
      for (size_t i = 0; i < in_num_samples; i++)
        m_resample_buffer[i] = float(in_samples_typed[i]) / float(0x80);

      resample_data.input_frames = long(in_num_frames);
      resample_data.data_in = m_resample_buffer.data();
    }
    break;
    case SampleFormat::Unsigned8:
    {
      const s8* in_samples_typed = reinterpret_cast<const s8*>(in_buf);
      for (size_t i = 0; i < in_num_samples; i++)
        m_resample_buffer[i] = float(int(in_samples_typed[i]) - 128) / float(0x80);

      resample_data.input_frames = long(in_num_frames);
      resample_data.data_in = m_resample_buffer.data();
    }
    break;
    case SampleFormat::Signed16:
      src_short_to_float_array(reinterpret_cast<const short*>(in_buf), m_resample_buffer.data(), int(in_num_samples));
      resample_data.input_frames = long(in_num_frames);
      resample_data.data_in = m_resample_buffer.data();
      break;

    case SampleFormat::Unsigned16:
    {
      const u16* in_samples_typed = reinterpret_cast<const u16*>(in_buf);
      for (size_t i = 0; i < in_num_samples; i++)
        m_resample_buffer[i] = float(int(in_samples_typed[i]) - 32768) / float(0x8000);

      resample_data.input_frames = long(in_num_frames);
      resample_data.data_in = m_resample_buffer.data();
    }
    break;

    case SampleFormat::Signed32:
      src_int_to_float_array(reinterpret_cast<const int*>(in_buf), m_resample_buffer.data(), int(in_num_samples));
      resample_data.input_frames = long(in_num_frames);
      resample_data.data_in = m_resample_buffer.data();
      break;

    case SampleFormat::Float32:
    default:
      resample_data.input_frames = long(in_num_frames);
      resample_data.data_in = reinterpret_cast<const float*>(in_buf);
      break;
  }

  // Actually perform the resampling.
  int process_result = src_process(reinterpret_cast<SRC_STATE*>(m_resampler_state), &resample_data);
  Assert(process_result == 0);

  // Update buffer pointers.
  m_input_buffer.MoveReadPointer(size_t(resample_data.input_frames_used) * m_input_frame_size);
  m_output_buffer.MoveWritePointer(size_t(resample_data.output_frames_gen) * m_output_frame_size);
  return true;
}

NullMixer::NullMixer() : Mixer(44100) {}

NullMixer::~NullMixer() {}

std::unique_ptr<Mixer> NullMixer::Create()
{
  return std::make_unique<NullMixer>();
}

void NullMixer::RenderSamples(size_t output_samples)
{
  CheckRenderBufferSize(output_samples);

  // Consume everything from the input buffers.
  for (auto& channel : m_channels)
    channel->ReadSamples(m_render_buffer.data(), output_samples);
}

} // namespace Audio
