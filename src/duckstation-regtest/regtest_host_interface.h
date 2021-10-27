#pragma once
#include "core/host_interface.h"
#include "regtest_settings_interface.h"

class RegTestHostInterface final : public HostInterface
{
public:
  RegTestHostInterface();
  ~RegTestHostInterface();

  bool Initialize() override;
  void Shutdown() override;

  void ReportError(const char* message) override;
  void ReportMessage(const char* message) override;
  void ReportDebuggerMessage(const char* message) override;
  bool ConfirmMessage(const char* message) override;

  void AddOSDMessage(std::string message, float duration = 2.0f) override;
  void DisplayLoadingScreen(const char* message, int progress_min = -1, int progress_max = -1,
                            int progress_value = -1) override;
  void GetGameInfo(const char* path, CDImage* image, std::string* code, std::string* title) override;

  void OnRunningGameChanged(const std::string& path, CDImage* image, const std::string& game_code,
                            const std::string& game_title);

  std::string GetStringSettingValue(const char* section, const char* key, const char* default_value = "") override;
  bool GetBoolSettingValue(const char* section, const char* key, bool default_value = false) override;
  int GetIntSettingValue(const char* section, const char* key, int default_value = 0) override;
  float GetFloatSettingValue(const char* section, const char* key, float default_value = 0.0f) override;
  std::vector<std::string> GetSettingStringList(const char* section, const char* key) override;

  std::string GetBIOSDirectory() override;

  std::unique_ptr<ByteStream> OpenPackageFile(const char* path, u32 flags) override;

  void OnSystemPerformanceCountersUpdated() override;
  void OnDisplayInvalidated() override;

protected:
  bool AcquireHostDisplay() override;
  void ReleaseHostDisplay() override;

  std::unique_ptr<AudioStream> CreateAudioStream(AudioBackend backend) override;

  void OnSystemCreated() override;
  void OnSystemPaused(bool paused) override;
  void OnSystemDestroyed() override;
  void OnControllerTypeChanged(u32 slot) override;

  void SetMouseMode(bool relative, bool hide_cursor) override;

private:
  void LoadGameSettingsDatabase();
  void InitializeSettings();
  void UpdateSettings();

  RegTestSettingsInterface m_settings_interface;
};
