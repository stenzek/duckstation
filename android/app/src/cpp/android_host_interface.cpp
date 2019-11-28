#include "android_host_interface.h"
#include "YBaseLib/Assert.h"
#include "YBaseLib/Log.h"
#include "YBaseLib/String.h"
#include "android_audio_stream.h"
#include "android_gles2_host_display.h"
#include "core/gpu.h"
#include "core/host_display.h"
#include "core/system.h"
#include <android/native_window_jni.h>
#include <imgui.h>
Log_SetChannel(AndroidHostInterface);

static JavaVM* s_jvm;
static jclass s_AndroidHostInterface_class;
static jmethodID s_AndroidHostInterface_constructor;
static jfieldID s_AndroidHostInterface_field_nativePointer;

// helper for retrieving the current per-thread jni environment
static JNIEnv* GetJNIEnv()
{
  JNIEnv* env;
  if (s_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK)
    return nullptr;
  else
    return env;
}

static AndroidHostInterface* GetNativeClass(JNIEnv* env, jobject obj)
{
  return reinterpret_cast<AndroidHostInterface*>(
    static_cast<uintptr_t>(env->GetLongField(obj, s_AndroidHostInterface_field_nativePointer)));
}

static std::string JStringToString(JNIEnv* env, jstring str)
{
  jsize length = env->GetStringUTFLength(str);
  if (length == 0)
    return {};

  const char* data = env->GetStringUTFChars(str, nullptr);
  Assert(data != nullptr);

  std::string ret(data, length);
  env->ReleaseStringUTFChars(str, data);

  return ret;
}

AndroidHostInterface::AndroidHostInterface(jobject java_object) : m_java_object(java_object)
{
  m_settings.SetDefaults();
  m_settings.bios_path = "/sdcard/PSX/BIOS/scph1001.bin";
  m_settings.memory_card_a_path = "/sdcard/PSX/memory_card_a.mcd";
  m_settings.gpu_renderer = GPURenderer::Software;
  m_settings.video_sync_enabled = true;
  m_settings.audio_sync_enabled = false;
  // m_settings.debugging.show_vram = true;
}

AndroidHostInterface::~AndroidHostInterface()
{
  ImGui::DestroyContext();
  GetJNIEnv()->DeleteGlobalRef(m_java_object);
}

void AndroidHostInterface::ReportError(const char* message)
{
  HostInterface::ReportError(message);
}

void AndroidHostInterface::ReportMessage(const char* message)
{
  HostInterface::ReportMessage(message);
}

bool AndroidHostInterface::StartEmulationThread(ANativeWindow* initial_surface, std::string initial_filename,
                                                std::string initial_state_filename)
{
  Assert(!IsEmulationThreadRunning());

  Log_DevPrintf("Starting emulation thread...");
  m_emulation_thread_stop_request.store(false);
  m_emulation_thread = std::thread(&AndroidHostInterface::EmulationThreadEntryPoint, this, initial_surface,
                                   std::move(initial_filename), std::move(initial_state_filename));
  m_emulation_thread_started.Wait();
  if (!m_emulation_thread_start_result.load())
  {
    m_emulation_thread.join();
    Log_ErrorPrint("Failed to start emulation in thread");
    return false;
  }

  return true;
}

void AndroidHostInterface::StopEmulationThread()
{
  Assert(IsEmulationThreadRunning());
  Log_InfoPrint("Stopping emulation thread...");
  m_emulation_thread_stop_request.store(true);
  m_emulation_thread.join();
  Log_InfoPrint("Emulation thread stopped");
}

void AndroidHostInterface::RunOnEmulationThread(std::function<void()> function, bool blocking)
{
  if (!IsEmulationThreadRunning())
  {
    function();
    return;
  }

  m_callback_mutex.lock();
  m_callback_queue.push_back(std::move(function));

  if (blocking)
  {
    // TODO: Don't spin
    for (;;)
    {
      if (m_callback_queue.empty())
        break;

      m_callback_mutex.unlock();
      m_callback_mutex.lock();
    }
  }

  m_callback_mutex.unlock();
}

void AndroidHostInterface::EmulationThreadEntryPoint(ANativeWindow* initial_surface, std::string initial_filename,
                                                     std::string initial_state_filename)
{
  CreateImGuiContext();

  // Create display.
  m_display = AndroidGLES2HostDisplay::Create(initial_surface);
  if (!m_display)
  {
    Log_ErrorPrint("Failed to create display on emulation thread.");
    DestroyImGuiContext();
    m_emulation_thread_start_result.store(false);
    m_emulation_thread_started.Signal();
    return;
  }

  // Create audio stream.
  m_audio_stream = AndroidAudioStream::Create();
  if (!m_audio_stream || !m_audio_stream->Reconfigure(44100, 2))
  {
    Log_ErrorPrint("Failed to create audio stream on emulation thread.");
    m_audio_stream.reset();
    m_display.reset();
    DestroyImGuiContext();
    m_emulation_thread_start_result.store(false);
    m_emulation_thread_started.Signal();
    return;
  }

  // Boot system.
  if (!CreateSystem() || !BootSystem(initial_filename.empty() ? nullptr : initial_filename.c_str(),
                                     initial_state_filename.empty() ? nullptr : initial_state_filename.c_str()))
  {
    Log_ErrorPrintf("Failed to boot system on emulation thread (file:%s state:%s).", initial_filename.c_str(),
                    initial_state_filename.c_str());
    m_audio_stream.reset();
    m_display.reset();
    DestroyImGuiContext();
    m_emulation_thread_start_result.store(false);
    m_emulation_thread_started.Signal();
    return;
  }

  // System is ready to go.
  m_emulation_thread_start_result.store(true);
  m_emulation_thread_started.Signal();

  ImGui::NewFrame();

  while (!m_emulation_thread_stop_request.load())
  {
    // run any events
    m_callback_mutex.lock();
    for (;;)
    {
      if (m_callback_queue.empty())
        break;

      auto callback = std::move(m_callback_queue.front());
      m_callback_queue.pop_front();
      m_callback_mutex.unlock();
      callback();
      m_callback_mutex.lock();
    }
    m_callback_mutex.unlock();

    // simulate the system if not paused
    if (m_system && !m_paused)
      m_system->RunFrame();

    // rendering
    {
      DrawImGui();

      if (m_system)
        m_system->GetGPU()->ResetGraphicsAPIState();

      ImGui::Render();
      m_display->Render();

      ImGui::NewFrame();

      if (m_system)
      {
        m_system->GetGPU()->RestoreGraphicsAPIState();

        if (m_speed_limiter_enabled)
          Throttle();
      }
    }
  }

  m_display.reset();
  m_audio_stream.reset();
  DestroyImGuiContext();
}

void AndroidHostInterface::CreateImGuiContext()
{
  ImGui::CreateContext();

  ImGui::GetIO().IniFilename = nullptr;
  // ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
  // ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_HasGamepad;
}

void AndroidHostInterface::DestroyImGuiContext()
{
  ImGui::DestroyContext();
}

void AndroidHostInterface::DrawImGui()
{
  DrawOSDMessages();

  ImGui::Render();
}

void AndroidHostInterface::AddOSDMessage(const char* message, float duration)
{
  OSDMessage msg;
  msg.text = message;
  msg.duration = duration;

  std::unique_lock<std::mutex> lock(m_osd_messages_lock);
  m_osd_messages.push_back(std::move(msg));
}

void AndroidHostInterface::DrawOSDMessages()
{
  constexpr ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs |
                                            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav |
                                            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing;

  std::unique_lock<std::mutex> lock(m_osd_messages_lock);
  const float scale = ImGui::GetIO().DisplayFramebufferScale.x;

  auto iter = m_osd_messages.begin();
  float position_x = 10.0f * scale;
  float position_y = (10.0f + (m_settings.display_fullscreen ? 0.0f : 20.0f)) * scale;
  u32 index = 0;
  while (iter != m_osd_messages.end())
  {
    const OSDMessage& msg = *iter;
    const double time = msg.time.GetTimeSeconds();
    const float time_remaining = static_cast<float>(msg.duration - time);
    if (time_remaining <= 0.0f)
    {
      iter = m_osd_messages.erase(iter);
      continue;
    }

    const float opacity = std::min(time_remaining, 1.0f);
    ImGui::SetNextWindowPos(ImVec2(position_x, position_y));
    ImGui::SetNextWindowSize(ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, opacity);

    if (ImGui::Begin(SmallString::FromFormat("osd_%u", index++), nullptr, window_flags))
    {
      ImGui::TextUnformatted(msg.text.c_str());
      position_y += ImGui::GetWindowSize().y + (4.0f * scale);
    }

    ImGui::End();
    ImGui::PopStyleVar();
    ++iter;
  }
}

void AndroidHostInterface::SurfaceChanged(ANativeWindow* window, int format, int width, int height)
{
  Log_InfoPrintf("SurfaceChanged %p %d %d %d", window, format, width, height);
  if (m_display->GetRenderWindow() == window)
  {
    m_display->WindowResized();
    return;
  }

  m_display->ChangeRenderWindow(window);
}

extern "C" jint JNI_OnLoad(JavaVM* vm, void* reserved)
{
  Log::GetInstance().SetDebugOutputParams(true, nullptr, LOGLEVEL_DEV);
  s_jvm = vm;

  JNIEnv* env = GetJNIEnv();
  if ((s_AndroidHostInterface_class = env->FindClass("com/github/stenzek/duckstation/AndroidHostInterface")) == nullptr)
  {
    Log_ErrorPrint("AndroidHostInterface class lookup failed");
    return -1;
  }

  // Create global reference so it doesn't get cleaned up.
  s_AndroidHostInterface_class = static_cast<jclass>(env->NewGlobalRef(s_AndroidHostInterface_class));
  if (!s_AndroidHostInterface_class)
  {
    Log_ErrorPrint("Failed to get reference to AndroidHostInterface");
    return -1;
  }

  if ((s_AndroidHostInterface_constructor = env->GetMethodID(s_AndroidHostInterface_class, "<init>", "()V")) ==
        nullptr ||
      (s_AndroidHostInterface_field_nativePointer =
         env->GetFieldID(s_AndroidHostInterface_class, "nativePointer", "J")) == nullptr)
  {
    Log_ErrorPrint("AndroidHostInterface lookups failed");
    return -1;
  }

  return JNI_VERSION_1_6;
}

#define DEFINE_JNI_METHOD(return_type, name)                                                                           \
  extern "C" JNIEXPORT return_type JNICALL Java_com_github_stenzek_duckstation_##name(JNIEnv* env)

#define DEFINE_JNI_ARGS_METHOD(return_type, name, ...)                                                                 \
  extern "C" JNIEXPORT return_type JNICALL Java_com_github_stenzek_duckstation_##name(JNIEnv* env, __VA_ARGS__)

DEFINE_JNI_ARGS_METHOD(jobject, AndroidHostInterface_create, jobject unused)
{
  // initialize the java side
  jobject java_obj = env->NewObject(s_AndroidHostInterface_class, s_AndroidHostInterface_constructor);
  if (!java_obj)
  {
    Log_ErrorPrint("Failed to create Java AndroidHostInterface");
    return nullptr;
  }

  jobject java_obj_ref = env->NewGlobalRef(java_obj);
  Assert(java_obj_ref != nullptr);

  // initialize the C++ side
  AndroidHostInterface* cpp_obj = new AndroidHostInterface(java_obj_ref);
  if (!cpp_obj)
  {
    // TODO: Do we need to release the original java object reference?
    Log_ErrorPrint("Failed to create C++ AndroidHostInterface");
    env->DeleteGlobalRef(java_obj_ref);
    return nullptr;
  }

  env->SetLongField(java_obj, s_AndroidHostInterface_field_nativePointer,
                    static_cast<long>(reinterpret_cast<uintptr_t>(cpp_obj)));

  return java_obj;
}

DEFINE_JNI_ARGS_METHOD(jboolean, AndroidHostInterface_isEmulationThreadRunning, jobject obj)
{
  return GetNativeClass(env, obj)->IsEmulationThreadRunning();
}

DEFINE_JNI_ARGS_METHOD(jboolean, AndroidHostInterface_startEmulationThread, jobject obj, jobject surface,
                       jstring filename, jstring state_filename)
{
  ANativeWindow* native_surface = ANativeWindow_fromSurface(env, surface);
  if (!native_surface)
  {
    Log_ErrorPrint("ANativeWindow_fromSurface() returned null");
    return false;
  }

  return GetNativeClass(env, obj)->StartEmulationThread(native_surface, JStringToString(env, filename),
                                                        JStringToString(env, state_filename));
}

DEFINE_JNI_ARGS_METHOD(void, AndroidHostInterface_stopEmulationThread, jobject obj)
{
  GetNativeClass(env, obj)->StopEmulationThread();
}

DEFINE_JNI_ARGS_METHOD(void, AndroidHostInterface_surfaceChanged, jobject obj, jobject surface, jint format, jint width,
                       jint height)
{
  ANativeWindow* native_surface = ANativeWindow_fromSurface(env, surface);
  if (!native_surface)
    Log_ErrorPrint("ANativeWindow_fromSurface() returned null");

  AndroidHostInterface* hi = GetNativeClass(env, obj);
  hi->RunOnEmulationThread(
    [hi, native_surface, format, width, height]() { hi->SurfaceChanged(native_surface, format, width, height); }, true);
}
