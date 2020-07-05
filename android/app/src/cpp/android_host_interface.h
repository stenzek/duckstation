#pragma once
#include "common/event.h"
#include "frontend-common/common_host_interface.h"
#include "frontend-common/ini_settings_interface.h"
#include <array>
#include <atomic>
#include <functional>
#include <jni.h>
#include <memory>
#include <thread>

struct ANativeWindow;

class Controller;

class AndroidHostInterface final : public CommonHostInterface
{
public:
  AndroidHostInterface(jobject java_object);
  ~AndroidHostInterface() override;

  bool Initialize() override;
  void Shutdown() override;

  const char* GetFrontendName() const override;
  void RequestExit() override;

  void ReportError(const char* message) override;
  void ReportMessage(const char* message) override;

  std::string GetSettingValue(const char* section, const char* key, const char* default_value = "") override;

  bool IsEmulationThreadRunning() const { return m_emulation_thread.joinable(); }
  bool StartEmulationThread(ANativeWindow* initial_surface, SystemBootParameters boot_params);
  void RunOnEmulationThread(std::function<void()> function, bool blocking = false);
  void StopEmulationThread();

  void SurfaceChanged(ANativeWindow* surface, int format, int width, int height);

  void SetControllerType(u32 index, std::string_view type_name);
  void SetControllerButtonState(u32 index, s32 button_code, bool pressed);

protected:
  void SetUserDirectory() override;
  void LoadSettings() override;
  void UpdateInputMap() override;

  bool AcquireHostDisplay() override;
  void ReleaseHostDisplay() override;
  std::unique_ptr<AudioStream> CreateAudioStream(AudioBackend backend) override;

private:
  void EmulationThreadEntryPoint(ANativeWindow* initial_surface, SystemBootParameters boot_params);

  void CreateImGuiContext();
  void DestroyImGuiContext();

  jobject m_java_object = {};

  std::unique_ptr<INISettingsInterface> m_settings_interface;

  ANativeWindow* m_surface = nullptr;

  std::mutex m_callback_mutex;
  std::deque<std::function<void()>> m_callback_queue;

  std::thread m_emulation_thread;
  std::atomic_bool m_emulation_thread_stop_request{false};
  std::atomic_bool m_emulation_thread_start_result{false};
  Common::Event m_emulation_thread_started;
};
