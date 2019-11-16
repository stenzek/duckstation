#pragma once
#include "types.h"
#include "settings.h"
#include <memory>
#include <optional>
#include <vector>

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

  bool CreateSystem();
  bool BootSystem(const char* filename, const char* state_filename);
  void DestroySystem();

  virtual void ReportError(const char* message);
  virtual void ReportMessage(const char* message);

  // Adds OSD messages, duration is in seconds.
  virtual void AddOSDMessage(const char* message, float duration = 2.0f) = 0;

  /// Loads the BIOS image for the specified region.
  virtual std::optional<std::vector<u8>> GetBIOSImage(ConsoleRegion region);

  bool LoadState(const char* filename);
  bool SaveState(const char* filename);

protected:
  /// Connects controllers. TODO: Clean this up later...
  virtual void ConnectControllers();

  void UpdateAudioVisualSync();

  std::unique_ptr<HostDisplay> m_display;
  std::unique_ptr<AudioStream> m_audio_stream;
  std::unique_ptr<System> m_system;
  Settings m_settings;

  bool m_paused = false;
  bool m_speed_limiter_temp_disabled = false;
};
