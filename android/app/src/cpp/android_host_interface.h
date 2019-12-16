#pragma once
#include "YBaseLib/Event.h"
#include "YBaseLib/Timer.h"
#include "core/host_interface.h"
#include <array>
#include <atomic>
#include <functional>
#include <jni.h>
#include <memory>
#include <thread>

struct ANativeWindow;

class Controller;

class AndroidHostInterface final : public HostInterface
{
public:
  AndroidHostInterface(jobject java_object);
  ~AndroidHostInterface() override;

  void ReportError(const char* message) override;
  void ReportMessage(const char* message) override;

  bool IsEmulationThreadRunning() const { return m_emulation_thread.joinable(); }
  bool StartEmulationThread(ANativeWindow* initial_surface, std::string initial_filename,
                            std::string initial_state_filename);
  void RunOnEmulationThread(std::function<void()> function, bool blocking = false);
  void StopEmulationThread();

  void SurfaceChanged(ANativeWindow* window, int format, int width, int height);

  void SetControllerType(u32 index, std::string_view type_name);
  void SetControllerButtonState(u32 index, s32 button_code, bool pressed);

private:
  enum : u32
  {
    NUM_CONTROLLERS = 2
  };

  void EmulationThreadEntryPoint(ANativeWindow* initial_surface, std::string initial_filename,
                                 std::string initial_state_filename);

  void CreateImGuiContext();
  void DestroyImGuiContext();
  void DrawImGui();

  void DrawFPSWindow();

  jobject m_java_object = {};

  std::mutex m_callback_mutex;
  std::deque<std::function<void()>> m_callback_queue;

  std::thread m_emulation_thread;
  std::atomic_bool m_emulation_thread_stop_request{false};
  std::atomic_bool m_emulation_thread_start_result{false};
  Event m_emulation_thread_started;
};
