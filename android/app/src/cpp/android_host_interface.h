#pragma once
#include "android_settings_interface.h"
#include "common/byte_stream.h"
#include "common/event.h"
#include "common/progress_callback.h"
#include "core/host_display.h"
#include "frontend-common/common_host_interface.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <jni.h>
#include <memory>
#include <string>
#include <thread>

struct ANativeWindow;

class Controller;

class AndroidHostInterface final : public CommonHostInterface
{
public:
  AndroidHostInterface(jobject java_object, jobject context_object, std::string user_directory);
  ~AndroidHostInterface() override;

  bool Initialize() override;
  void Shutdown() override;

  const char* GetFrontendName() const override;
  void RequestExit() override;

  void ReportError(const char* message) override;
  void ReportMessage(const char* message) override;

  std::string GetStringSettingValue(const char* section, const char* key, const char* default_value = "") override;
  bool GetBoolSettingValue(const char* section, const char* key, bool default_value = false) override;
  int GetIntSettingValue(const char* section, const char* key, int default_value = 0) override;
  float GetFloatSettingValue(const char* section, const char* key, float default_value = 0.0f) override;
  std::unique_ptr<ByteStream> OpenPackageFile(const char* path, u32 flags) override;
  bool GetMainDisplayRefreshRate(float* refresh_rate) override;

  bool IsEmulationThreadRunning() const { return m_emulation_thread_running.load(); }
  bool IsEmulationThreadPaused() const;
  void RunOnEmulationThread(std::function<void()> function, bool blocking = false);
  void PauseEmulationThread(bool paused);
  void StopEmulationThreadLoop();

  void EmulationThreadEntryPoint(JNIEnv* env, jobject emulation_activity, jobject initial_surface,
                                 SystemBootParameters boot_params, bool resume_state);

  void SurfaceChanged(ANativeWindow* surface, int format, int width, int height);
  void SetDisplayAlignment(HostDisplay::Alignment alignment);

  void SetControllerType(u32 index, std::string_view type_name);
  void SetControllerButtonState(u32 index, s32 button_code, bool pressed);
  void SetControllerAxisState(u32 index, s32 button_code, float value);
  void HandleControllerButtonEvent(u32 controller_index, u32 button_index, bool pressed);
  void HandleControllerAxisEvent(u32 controller_index, u32 axis_index, float value);
  void SetFastForwardEnabled(bool enabled);

  void RefreshGameList(bool invalidate_cache, bool invalidate_database, ProgressCallback* progress_callback);
  void ApplySettings(bool display_osd_messages);

  bool ImportPatchCodesFromString(const std::string& str);

  jobjectArray GetInputProfileNames(JNIEnv* env) const;
  bool ApplyInputProfile(const char* profile_name);
  bool SaveInputProfile(const char* profile_name);

protected:
  void SetUserDirectory() override;
  void LoadSettings() override;
  void UpdateInputMap() override;
  void RegisterHotkeys() override;

  bool AcquireHostDisplay() override;
  void ReleaseHostDisplay() override;
  std::unique_ptr<AudioStream> CreateAudioStream(AudioBackend backend) override;
  void UpdateControllerInterface() override;

  void OnSystemPaused(bool paused) override;
  void OnSystemDestroyed() override;
  void OnRunningGameChanged() override;

private:
  void EmulationThreadLoop(JNIEnv* env);

  void CreateImGuiContext();
  void DestroyImGuiContext();

  void LoadAndConvertSettings();
  void SetVibration(bool enabled);
  void UpdateVibration();

  jobject m_java_object = {};
  jobject m_emulation_activity_object = {};

  AndroidSettingsInterface m_settings_interface;

  ANativeWindow* m_surface = nullptr;

  std::mutex m_mutex;
  std::condition_variable m_sleep_cv;
  std::deque<std::function<void()>> m_callback_queue;
  std::atomic_bool m_callbacks_outstanding{false};

  std::atomic_bool m_emulation_thread_stop_request{false};
  std::atomic_bool m_emulation_thread_running{false};

  HostDisplay::Alignment m_display_alignment = HostDisplay::Alignment::Center;

  u64 m_last_vibration_update_time = 0;
  bool m_last_vibration_state = false;
  bool m_vibration_enabled = false;
};

namespace AndroidHelpers {

JNIEnv* GetJNIEnv();
AndroidHostInterface* GetNativeClass(JNIEnv* env, jobject obj);
std::string JStringToString(JNIEnv* env, jstring str);
std::unique_ptr<GrowableMemoryByteStream> ReadInputStreamToMemory(JNIEnv* env, jobject obj, u32 chunk_size = 65536);
jclass GetStringClass();

} // namespace AndroidHelpers

template<typename T>
class LocalRefHolder
{
public:
  LocalRefHolder() : m_env(nullptr), m_object(nullptr) {}

  LocalRefHolder(JNIEnv* env, T object) : m_env(env), m_object(object) {}

  LocalRefHolder(const LocalRefHolder<T>&) = delete;
  LocalRefHolder(LocalRefHolder&& move) : m_env(move.m_env), m_object(move.m_object)
  {
    move.m_env = nullptr;
    move.m_object = {};
  }

  ~LocalRefHolder()
  {
    if (m_object)
      m_env->DeleteLocalRef(m_object);
  }

  operator T() const { return m_object; }
  T operator*() const { return m_object; }

  LocalRefHolder& operator=(const LocalRefHolder&) = delete;
  LocalRefHolder& operator=(LocalRefHolder&& move)
  {
    if (m_object)
      m_env->DeleteLocalRef(m_object);
    m_env = move.m_env;
    m_object = move.m_object;
    move.m_env = nullptr;
    move.m_object = {};
    return *this;
  }

  T Get() const { return m_object; }

private:
  JNIEnv* m_env;
  T m_object;
};
