#include "qtaudiostream.h"
#include <QtCore/QDebug>
#include <QtMultimedia/QAudioOutput>

QtAudioStream::QtAudioStream()
{
  QIODevice::open(QIODevice::ReadOnly);
}

QtAudioStream::~QtAudioStream() = default;

std::unique_ptr<AudioStream> QtAudioStream::Create()
{
  return std::make_unique<QtAudioStream>();
}

bool QtAudioStream::OpenDevice()
{
  QAudioFormat format;
  format.setSampleRate(m_output_sample_rate);
  format.setChannelCount(m_channels);
  format.setSampleSize(sizeof(SampleType) * 8);
  format.setCodec("audio/pcm");
  format.setByteOrder(QAudioFormat::LittleEndian);
  format.setSampleType(QAudioFormat::SignedInt);

  QAudioDeviceInfo adi = QAudioDeviceInfo::defaultOutputDevice();
  if (!adi.isFormatSupported(format))
  {
    qWarning() << "Audio format not supported by device";
    return false;
  }

  m_output = std::make_unique<QAudioOutput>(format);
  m_output->setBufferSize(sizeof(SampleType) * m_channels * m_buffer_size);
  return true;
}

void QtAudioStream::PauseDevice(bool paused)
{
  if (paused)
  {
    m_output->stop();
    return;
  }

  m_output->start(this);
}

void QtAudioStream::CloseDevice()
{
  m_output.reset();
}

void QtAudioStream::BufferAvailable() {}

bool QtAudioStream::isSequential() const
{
  return true;
}

qint64 QtAudioStream::bytesAvailable() const
{
  return GetSamplesAvailable() * m_channels * sizeof(SampleType);
}

qint64 QtAudioStream::readData(char* data, qint64 maxlen)
{
  const u32 num_samples = static_cast<u32>(maxlen) / sizeof(SampleType) / m_channels;
  const u32 read_samples = ReadSamples(reinterpret_cast<SampleType*>(data), num_samples);
  const u32 silence_samples = num_samples - read_samples;
  if (silence_samples > 0)
  {
    std::memset(reinterpret_cast<SampleType*>(data) + (read_samples * m_channels), 0,
                silence_samples * m_channels * sizeof(SampleType));
  }

  return num_samples * m_channels * sizeof(SampleType);
}

qint64 QtAudioStream::writeData(const char* data, qint64 len)
{
  return 0;
}
