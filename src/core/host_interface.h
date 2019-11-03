#pragma once
#include "types.h"
#include "settings.h"
#include <memory>

class AudioStream;
class HostDisplay;

class System;

class HostInterface
{
public:
  HostInterface();
  virtual ~HostInterface();

  AudioStream* GetAudioStream() const { return m_audio_stream.get(); }

  bool InitializeSystem(const char* filename, const char* exp1_filename);

  virtual HostDisplay* GetDisplay() const = 0;

  virtual void ReportMessage(const char* message) = 0;

  // Adds OSD messages, duration is in seconds.
  virtual void AddOSDMessage(const char* message, float duration = 2.0f) = 0;

  bool LoadState(const char* filename);
  bool SaveState(const char* filename);

protected:
  std::unique_ptr<AudioStream> m_audio_stream;
  std::unique_ptr<System> m_system;

  Settings m_settings;
};
