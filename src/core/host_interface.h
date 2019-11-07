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

  /// Access to host display.
  ALWAYS_INLINE HostDisplay* GetDisplay() const { return m_display.get(); }

  /// Access to host audio stream.
  AudioStream* GetAudioStream() const { return m_audio_stream.get(); }

  /// Returns a settings object which can be modified.
  Settings& GetSettings() { return m_settings; }


  bool InitializeSystem(const char* filename, const char* exp1_filename);
  void ShutdownSystem();


  virtual void ReportMessage(const char* message) = 0;

  // Adds OSD messages, duration is in seconds.
  virtual void AddOSDMessage(const char* message, float duration = 2.0f) = 0;

  bool LoadState(const char* filename);
  bool SaveState(const char* filename);

protected:
  void UpdateAudioVisualSync();

  std::unique_ptr<HostDisplay> m_display;
  std::unique_ptr<AudioStream> m_audio_stream;
  std::unique_ptr<System> m_system;
  Settings m_settings;

  bool m_paused = false;
  bool m_speed_limiter_temp_disabled = false;
};
