#pragma once
#include "common/audio_stream.h"
#include <QtCore/QIODevice>
#include <memory>

class QAudioOutput;

class QtAudioStream final : public AudioStream, private QIODevice
{
public:
  QtAudioStream();
  ~QtAudioStream();

  static std::unique_ptr<AudioStream> Create();

protected:
  bool OpenDevice() override;
  void PauseDevice(bool paused) override;
  void CloseDevice() override;
  void BufferAvailable() override;

private:
  bool isSequential() const override;
  qint64 bytesAvailable() const override;
  qint64 readData(char* data, qint64 maxlen) override;
  qint64 writeData(const char* data, qint64 len) override;

  std::unique_ptr<QAudioOutput> m_output;
};
