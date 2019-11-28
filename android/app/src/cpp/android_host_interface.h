#pragma once
#include "YBaseLib/Event.h"
#include "YBaseLib/Timer.h"
#include "core/host_interface.h"
#include <atomic>
#include <deque>
#include <functional>
#include <jni.h>
#include <mutex>
#include <thread>

struct ANativeWindow;

class AndroidHostInterface final : public HostInterface
{
public:
  AndroidHostInterface(jobject java_object);
  ~AndroidHostInterface() override;

  void ReportError(const char* message) override;
  void ReportMessage(const char* message) override;
  void AddOSDMessage(const char* message, float duration = 2.0f) override;

  bool IsEmulationThreadRunning() const { return m_emulation_thread.joinable(); }
  bool StartEmulationThread(ANativeWindow* initial_surface, std::string initial_filename,
                            std::string initial_state_filename);
  void RunOnEmulationThread(std::function<void()> function, bool blocking = false);
  void StopEmulationThread();

  void SurfaceChanged(ANativeWindow* window, int format, int width, int height);

private:
  struct OSDMessage
  {
    std::string text;
    Timer time;
    float duration;
  };

  void EmulationThreadEntryPoint(ANativeWindow* initial_surface, std::string initial_filename,
                                 std::string initial_state_filename);

  void CreateImGuiContext();
  void DestroyImGuiContext();
  void DrawImGui();
  void DrawOSDMessages();

  jobject m_java_object = {};

  std::deque<OSDMessage> m_osd_messages;
  std::mutex m_osd_messages_lock;

  std::mutex m_callback_mutex;
  std::deque<std::function<void()>> m_callback_queue;

  std::thread m_emulation_thread;
  std::atomic_bool m_emulation_thread_stop_request{false};
  std::atomic_bool m_emulation_thread_start_result{false};
  Event m_emulation_thread_started;
};
