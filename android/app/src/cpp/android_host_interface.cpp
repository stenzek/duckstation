#include "android_host_interface.h"
#include "android_controller_interface.h"
#include "android_progress_callback.h"
#include "common/assert.h"
#include "common/audio_stream.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/string.h"
#include "common/string_util.h"
#include "common/timer.h"
#include "common/timestamp.h"
#include "core/bios.h"
#include "core/cheats.h"
#include "core/controller.h"
#include "core/gpu.h"
#include "core/host_display.h"
#include "core/memory_card_image.h"
#include "core/system.h"
#include "frontend-common/cheevos.h"
#include "frontend-common/game_list.h"
#include "frontend-common/imgui_fullscreen.h"
#include "frontend-common/imgui_styles.h"
#include "frontend-common/opengl_host_display.h"
#include "frontend-common/vulkan_host_display.h"
#include "scmversion/scmversion.h"
#include <android/native_window_jni.h>
#include <cmath>
#include <imgui.h>
#include <sched.h>
#include <unistd.h>
Log_SetChannel(AndroidHostInterface);

#ifdef USE_OPENSLES
#include "opensles_audio_stream.h"
#endif

static JavaVM* s_jvm;
static jclass s_String_class;
static jclass s_AndroidHostInterface_class;
static jmethodID s_AndroidHostInterface_constructor;
static jfieldID s_AndroidHostInterface_field_mNativePointer;
static jmethodID s_AndroidHostInterface_method_reportError;
static jmethodID s_AndroidHostInterface_method_reportMessage;
static jmethodID s_AndroidHostInterface_method_openAssetStream;
static jclass s_EmulationActivity_class;
static jmethodID s_EmulationActivity_method_reportError;
static jmethodID s_EmulationActivity_method_onEmulationStarted;
static jmethodID s_EmulationActivity_method_onEmulationStopped;
static jmethodID s_EmulationActivity_method_onGameTitleChanged;
static jmethodID s_EmulationActivity_method_setVibration;
static jmethodID s_EmulationActivity_method_getRefreshRate;
static jmethodID s_EmulationActivity_method_openPauseMenu;
static jmethodID s_EmulationActivity_method_getInputDeviceNames;
static jmethodID s_EmulationActivity_method_hasInputDeviceVibration;
static jmethodID s_EmulationActivity_method_setInputDeviceVibration;
static jclass s_PatchCode_class;
static jmethodID s_PatchCode_constructor;
static jclass s_GameListEntry_class;
static jmethodID s_GameListEntry_constructor;
static jclass s_SaveStateInfo_class;
static jmethodID s_SaveStateInfo_constructor;
static jclass s_Achievement_class;
static jmethodID s_Achievement_constructor;
static jclass s_MemoryCardFileInfo_class;
static jmethodID s_MemoryCardFileInfo_constructor;

namespace AndroidHelpers {
JavaVM* GetJavaVM()
{
  return s_jvm;
}

// helper for retrieving the current per-thread jni environment
JNIEnv* GetJNIEnv()
{
  JNIEnv* env;
  if (s_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK)
    return nullptr;
  else
    return env;
}

AndroidHostInterface* GetNativeClass(JNIEnv* env, jobject obj)
{
  return reinterpret_cast<AndroidHostInterface*>(
    static_cast<uintptr_t>(env->GetLongField(obj, s_AndroidHostInterface_field_mNativePointer)));
}

std::string JStringToString(JNIEnv* env, jstring str)
{
  if (str == nullptr)
    return {};

  jsize length = env->GetStringUTFLength(str);
  if (length == 0)
    return {};

  const char* data = env->GetStringUTFChars(str, nullptr);
  Assert(data != nullptr);

  std::string ret(data, length);
  env->ReleaseStringUTFChars(str, data);

  return ret;
}

jclass GetStringClass()
{
  return s_String_class;
}

std::unique_ptr<GrowableMemoryByteStream> ReadInputStreamToMemory(JNIEnv* env, jobject obj, u32 chunk_size /* = 65536*/)
{
  std::unique_ptr<GrowableMemoryByteStream> bs = std::make_unique<GrowableMemoryByteStream>(nullptr, 0);
  u32 position = 0;

  jclass cls = env->GetObjectClass(obj);
  jmethodID read_method = env->GetMethodID(cls, "read", "([B)I");
  Assert(read_method);

  jbyteArray temp = env->NewByteArray(chunk_size);
  for (;;)
  {
    int bytes_read = env->CallIntMethod(obj, read_method, temp);
    if (bytes_read <= 0)
      break;

    if ((position + static_cast<u32>(bytes_read)) > bs->GetMemorySize())
    {
      const u32 new_size = std::max<u32>(bs->GetMemorySize() * 2, position + static_cast<u32>(bytes_read));
      bs->ResizeMemory(new_size);
    }

    env->GetByteArrayRegion(temp, 0, bytes_read, reinterpret_cast<jbyte*>(bs->GetMemoryPointer() + position));
    position += static_cast<u32>(bytes_read);
  }

  bs->Resize(position);
  env->DeleteLocalRef(temp);
  env->DeleteLocalRef(cls);
  return bs;
}

std::vector<u8> ByteArrayToVector(JNIEnv* env, jbyteArray obj)
{
  std::vector<u8> ret;
  const jsize size = obj ? env->GetArrayLength(obj) : 0;
  if (size > 0)
  {
    jbyte* data = env->GetByteArrayElements(obj, nullptr);
    ret.resize(static_cast<size_t>(size));
    std::memcpy(ret.data(), data, ret.size());
    env->ReleaseByteArrayElements(obj, data, 0);
  }

  return ret;
}

jbyteArray NewByteArray(JNIEnv* env, const void* data, size_t size)
{
  if (!data || size == 0)
    return nullptr;

  jbyteArray obj = env->NewByteArray(static_cast<jsize>(size));
  jbyte* obj_data = env->GetByteArrayElements(obj, nullptr);
  std::memcpy(obj_data, data, static_cast<size_t>(static_cast<jsize>(size)));
  env->ReleaseByteArrayElements(obj, obj_data, 0);
  return obj;
}

jbyteArray VectorToByteArray(JNIEnv* env, const std::vector<u8>& data)
{
  if (data.empty())
    return nullptr;

  return NewByteArray(env, data.data(), data.size());
}

jobjectArray CreateObjectArray(JNIEnv* env, jclass object_class, const jobject* objects, size_t num_objects,
                               bool release_refs/* = false*/)
{
  if (!objects || num_objects == 0)
    return nullptr;

  jobjectArray arr = env->NewObjectArray(static_cast<jsize>(num_objects), object_class, nullptr);
  for (jsize i = 0; i < static_cast<jsize>(num_objects); i++)
  {
    env->SetObjectArrayElement(arr, i, objects[i]);
    if (release_refs && objects[i])
      env->DeleteLocalRef(objects[i]);
  }

  return arr;
}
} // namespace AndroidHelpers

AndroidHostInterface::AndroidHostInterface(jobject java_object, jobject context_object, std::string user_directory)
  : m_java_object(java_object)
{
  m_user_directory = std::move(user_directory);
  m_settings_interface = std::make_unique<AndroidSettingsInterface>(context_object);
}

AndroidHostInterface::~AndroidHostInterface()
{
  ImGui::DestroyContext();
  AndroidHelpers::GetJNIEnv()->DeleteGlobalRef(m_java_object);
}

bool AndroidHostInterface::Initialize()
{
  if (!CommonHostInterface::Initialize())
    return false;

  return true;
}

void AndroidHostInterface::Shutdown()
{
  HostInterface::Shutdown();
}

const char* AndroidHostInterface::GetFrontendName() const
{
  return "DuckStation Android";
}

void AndroidHostInterface::RequestExit()
{
  ReportError("Ignoring RequestExit()");
}

void AndroidHostInterface::ReportError(const char* message)
{
  CommonHostInterface::ReportError(message);

  JNIEnv* env = AndroidHelpers::GetJNIEnv();
  jstring message_jstr = env->NewStringUTF(message);
  if (m_emulation_activity_object)
    env->CallVoidMethod(m_emulation_activity_object, s_EmulationActivity_method_reportError, message_jstr);
  else
    env->CallVoidMethod(m_java_object, s_AndroidHostInterface_method_reportError, message_jstr);
  env->DeleteLocalRef(message_jstr);
}

void AndroidHostInterface::ReportMessage(const char* message)
{
  CommonHostInterface::ReportMessage(message);

  if (IsOnEmulationThread())
  {
    // The toasts are not visible when the emulation activity is running anyway.
    AddOSDMessage(message, 5.0f);
  }
  else
  {
    JNIEnv* env = AndroidHelpers::GetJNIEnv();
    LocalRefHolder<jstring> message_jstr(env, env->NewStringUTF(message));
    env->CallVoidMethod(m_java_object, s_AndroidHostInterface_method_reportMessage, message_jstr.Get());
  }
}

std::unique_ptr<ByteStream> AndroidHostInterface::OpenPackageFile(const char* path, u32 flags)
{
  Log_DevPrintf("OpenPackageFile(%s, %x)", path, flags);
  if (flags & (BYTESTREAM_OPEN_CREATE | BYTESTREAM_OPEN_WRITE))
    return {};

  JNIEnv* env = AndroidHelpers::GetJNIEnv();
  jobject stream =
    env->CallObjectMethod(m_java_object, s_AndroidHostInterface_method_openAssetStream, env->NewStringUTF(path));
  if (!stream)
  {
    Log_ErrorPrintf("Package file '%s' not found", path);
    return {};
  }

  std::unique_ptr<ByteStream> ret(AndroidHelpers::ReadInputStreamToMemory(env, stream, 65536));
  env->DeleteLocalRef(stream);
  return ret;
}

void AndroidHostInterface::RegisterHotkeys()
{
  RegisterHotkey(StaticString(TRANSLATABLE("Hotkeys", "General")), StaticString("OpenPauseMenu"),
                 StaticString(TRANSLATABLE("Hotkeys", "Open Pause Menu")), [this](bool pressed) {
                   if (pressed)
                   {
                     AndroidHelpers::GetJNIEnv()->CallVoidMethod(m_emulation_activity_object,
                                                                 s_EmulationActivity_method_openPauseMenu);
                   }
                 });

  CommonHostInterface::RegisterHotkeys();
}

bool AndroidHostInterface::GetMainDisplayRefreshRate(float* refresh_rate)
{
  if (!m_emulation_activity_object)
    return false;

  float value = AndroidHelpers::GetJNIEnv()->CallFloatMethod(m_emulation_activity_object,
                                                             s_EmulationActivity_method_getRefreshRate);
  if (value <= 0.0f)
    return false;

  *refresh_rate = value;
  return true;
}

void AndroidHostInterface::SetUserDirectory()
{
  // Already set in constructor.
  Assert(!m_user_directory.empty());
}

void AndroidHostInterface::LoadSettings(SettingsInterface& si)
{
  const GPURenderer old_renderer = g_settings.gpu_renderer;
  CommonHostInterface::LoadSettings(si);

  const std::string msaa_str = si.GetStringValue("GPU", "MSAA", "1");
  g_settings.gpu_multisamples = std::max<u32>(StringUtil::FromChars<u32>(msaa_str).value_or(1), 1);
  g_settings.gpu_per_sample_shading = StringUtil::EndsWith(msaa_str, "-ssaa");

  // turn percentage into fraction for overclock
  const u32 overclock_percent = static_cast<u32>(std::max(si.GetIntValue("CPU", "Overclock", 100), 1));
  Settings::CPUOverclockPercentToFraction(overclock_percent, &g_settings.cpu_overclock_numerator,
                                          &g_settings.cpu_overclock_denominator);
  g_settings.cpu_overclock_enable = (overclock_percent != 100);
  g_settings.UpdateOverclockActive();

  m_vibration_enabled = si.GetBoolValue("Controller1", "Vibration", false);

  // Defer renderer changes, the app really doesn't like it.
  if (System::IsValid() && g_settings.gpu_renderer != old_renderer)
  {
    AddFormattedOSDMessage(5.0f,
                           TranslateString("OSDMessage", "Change to %s GPU renderer will take effect on restart."),
                           Settings::GetRendererName(g_settings.gpu_renderer));
    g_settings.gpu_renderer = old_renderer;
  }
}

void AndroidHostInterface::UpdateInputMap(SettingsInterface& si)
{
  if (m_emulation_activity_object)
  {
    JNIEnv* env = AndroidHelpers::GetJNIEnv();
    DebugAssert(env);

    std::vector<std::string> device_names;

    jobjectArray const java_names = reinterpret_cast<jobjectArray>(
      env->CallObjectMethod(m_emulation_activity_object, s_EmulationActivity_method_getInputDeviceNames));
    if (java_names)
    {
      const u32 count = static_cast<u32>(env->GetArrayLength(java_names));
      for (u32 i = 0; i < count; i++)
      {
        device_names.push_back(
          AndroidHelpers::JStringToString(env, reinterpret_cast<jstring>(env->GetObjectArrayElement(java_names, i))));
      }

      env->DeleteLocalRef(java_names);
    }

    if (m_controller_interface)
    {
      AndroidControllerInterface* ci = static_cast<AndroidControllerInterface*>(m_controller_interface.get());
      if (ci)
      {
        ci->SetDeviceNames(std::move(device_names));
        for (u32 i = 0; i < ci->GetControllerCount(); i++)
        {
          const bool has_vibration = env->CallBooleanMethod(
            m_emulation_activity_object, s_EmulationActivity_method_hasInputDeviceVibration, static_cast<jint>(i));
          ci->SetDeviceRumble(i, has_vibration);
        }
      }
    }
  }

  CommonHostInterface::UpdateInputMap(si);
}

bool AndroidHostInterface::IsEmulationThreadPaused() const
{
  return System::IsValid() && System::IsPaused();
}

void AndroidHostInterface::PauseEmulationThread(bool paused)
{
  Assert(IsEmulationThreadRunning());
  RunOnEmulationThread([this, paused]() { PauseSystem(paused); });
}

void AndroidHostInterface::StopEmulationThreadLoop()
{
  if (!IsEmulationThreadRunning())
    return;

  std::unique_lock<std::mutex> lock(m_mutex);
  m_emulation_thread_stop_request.store(true);
  m_sleep_cv.notify_one();
}

bool AndroidHostInterface::IsOnEmulationThread() const
{
  return std::this_thread::get_id() == m_emulation_thread_id;
}

void AndroidHostInterface::RunOnEmulationThread(std::function<void()> function, bool blocking)
{
  if (!IsEmulationThreadRunning())
  {
    function();
    return;
  }

  m_mutex.lock();
  m_callback_queue.push_back(std::move(function));
  m_callbacks_outstanding.store(true);
  m_sleep_cv.notify_one();

  if (blocking)
  {
    // TODO: Don't spin
    for (;;)
    {
      if (!m_callbacks_outstanding.load())
        break;

      m_mutex.unlock();
      m_mutex.lock();
    }
  }

  m_mutex.unlock();
}

void AndroidHostInterface::RunLater(std::function<void()> func)
{
  std::unique_lock<std::mutex> lock(m_mutex);
  m_callback_queue.push_back(std::move(func));
  m_callbacks_outstanding.store(true);
}

void AndroidHostInterface::EmulationThreadEntryPoint(JNIEnv* env, jobject emulation_activity,
                                                     SystemBootParameters boot_params, bool resume_state)
{
  if (!m_surface)
  {
    Log_ErrorPrint("Emulation thread started without surface set.");
    env->CallVoidMethod(emulation_activity, s_EmulationActivity_method_onEmulationStopped);
    return;
  }

  {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_emulation_thread_running.store(true);
    m_emulation_activity_object = emulation_activity;
    m_emulation_thread_id = std::this_thread::get_id();
  }

  ApplySettings(true);

  // Boot system.
  bool boot_result = false;
  if (resume_state && boot_params.filename.empty())
    boot_result = ResumeSystemFromMostRecentState();
  else if (resume_state && CanResumeSystemFromFile(boot_params.filename.c_str()))
    boot_result = ResumeSystemFromState(boot_params.filename.c_str(), true);
  else
    boot_result = BootSystem(boot_params);

  if (boot_result)
  {
    // System is ready to go.
    EmulationThreadLoop(env);
    PowerOffSystem(ShouldSaveResumeState());
  }

  // Drain any callbacks so we don't leave things in a screwed-up state for next boot.
  {
    std::unique_lock<std::mutex> lock(m_mutex);
    while (!m_callback_queue.empty())
    {
      auto callback = std::move(m_callback_queue.front());
      m_callback_queue.pop_front();
      lock.unlock();
      callback();
      lock.lock();
    }
    m_emulation_thread_running.store(false);
    m_emulation_thread_id = {};
    m_emulation_activity_object = {};
    m_callbacks_outstanding.store(false);
  }

  env->CallVoidMethod(emulation_activity, s_EmulationActivity_method_onEmulationStopped);
}

void AndroidHostInterface::EmulationThreadLoop(JNIEnv* env)
{
  env->CallVoidMethod(m_emulation_activity_object, s_EmulationActivity_method_onEmulationStarted);

  for (;;)
  {
    // run any events
    {
      std::unique_lock<std::mutex> lock(m_mutex);
      for (;;)
      {
        if (!m_callback_queue.empty())
        {
          do
          {
            auto callback = std::move(m_callback_queue.front());
            m_callback_queue.pop_front();
            lock.unlock();
            callback();
            lock.lock();
          } while (!m_callback_queue.empty());
          m_callbacks_outstanding.store(false);
        }

        if (m_emulation_thread_stop_request.load())
        {
          m_emulation_thread_stop_request.store(false);
          return;
        }

        if (System::IsPaused())
        {
          // paused, wait for us to resume
          m_sleep_cv.wait(lock);
        }
        else
        {
          // done with callbacks, run the frame
          break;
        }
      }
    }

    // we don't do a full PollAndUpdate() here
    if (Cheevos::IsActive())
      Cheevos::Update();

    // simulate the system if not paused
    if (System::IsRunning())
    {
      if (m_throttler_enabled)
        System::RunFrames();
      else
        System::RunFrame();

      UpdateControllerRumble();
      if (m_vibration_enabled)
        UpdateVibration();
    }

    // rendering
    {
      ImGui::NewFrame();
      DrawImGuiWindows();

      m_display->Render();
      ImGui::EndFrame();

      if (System::IsRunning())
      {
        System::UpdatePerformanceCounters();

        if (m_throttler_enabled)
          System::Throttle();
      }
    }
  }
}

bool AndroidHostInterface::AcquireHostDisplay()
{
  WindowInfo wi;
  wi.type = WindowInfo::Type::Android;
  wi.window_handle = m_surface;
  wi.surface_width = ANativeWindow_getWidth(m_surface);
  wi.surface_height = ANativeWindow_getHeight(m_surface);

  // TODO: Really need a better way of determining this.
  wi.surface_scale = 2.0f;

  switch (g_settings.gpu_renderer)
  {
    case GPURenderer::HardwareVulkan:
      m_display = std::make_unique<FrontendCommon::VulkanHostDisplay>();
      break;

    case GPURenderer::HardwareOpenGL:
    default:
      m_display = std::make_unique<FrontendCommon::OpenGLHostDisplay>();
      break;
  }

  if (!m_display->CreateRenderDevice(wi, {}, g_settings.gpu_use_debug_device, g_settings.gpu_threaded_presentation) ||
      !m_display->InitializeRenderDevice(GetShaderCacheBasePath(), g_settings.gpu_use_debug_device,
                                         g_settings.gpu_threaded_presentation))
  {
    m_display->DestroyRenderDevice();
    m_display.reset();
    return false;
  }

  // The alignment was set prior to booting.
  m_display->SetDisplayAlignment(m_display_alignment);

  if (!CreateHostDisplayResources())
  {
    ReportError("Failed to create host display resources");
    ReleaseHostDisplayResources();
    ReleaseHostDisplay();
    return false;
  }

  return true;
}

void AndroidHostInterface::ReleaseHostDisplay()
{
  ReleaseHostDisplayResources();
  if (m_display)
  {
    m_display->DestroyRenderDevice();
    m_display.reset();
  }
}

std::unique_ptr<AudioStream> AndroidHostInterface::CreateAudioStream(AudioBackend backend)
{
#ifdef USE_OPENSLES
  if (backend == AudioBackend::OpenSLES)
    return OpenSLESAudioStream::Create();
#endif

  return CommonHostInterface::CreateAudioStream(backend);
}

void AndroidHostInterface::UpdateControllerInterface()
{
  if (m_controller_interface)
  {
    m_controller_interface->Shutdown();
    m_controller_interface.reset();
  }

  m_controller_interface = std::make_unique<AndroidControllerInterface>();
  if (!m_controller_interface || !m_controller_interface->Initialize(this))
  {
    Log_WarningPrintf("Failed to initialize controller interface, bindings are not possible.");
    if (m_controller_interface)
    {
      m_controller_interface->Shutdown();
      m_controller_interface.reset();
    }
  }
}

void AndroidHostInterface::OnSystemPaused(bool paused)
{
  CommonHostInterface::OnSystemPaused(paused);

  if (m_vibration_enabled)
    SetVibration(false);
}

void AndroidHostInterface::OnSystemDestroyed()
{
  CommonHostInterface::OnSystemDestroyed();
  ClearOSDMessages();

  if (m_vibration_enabled)
    SetVibration(false);
}

void AndroidHostInterface::OnRunningGameChanged(const std::string& path, CDImage* image, const std::string& game_code,
                                                const std::string& game_title)
{
  CommonHostInterface::OnRunningGameChanged(path, image, game_code, game_title);

  if (m_emulation_activity_object)
  {
    JNIEnv* env = AndroidHelpers::GetJNIEnv();
    jstring title_string = env->NewStringUTF(System::GetRunningTitle().c_str());
    env->CallVoidMethod(m_emulation_activity_object, s_EmulationActivity_method_onGameTitleChanged, title_string);
    env->DeleteLocalRef(title_string);
  }
}

void AndroidHostInterface::SurfaceChanged(ANativeWindow* surface, int format, int width, int height)
{
  Log_InfoPrintf("SurfaceChanged %p %d %d %d", surface, format, width, height);
  if (m_surface == surface)
  {
    if (m_display && (width != m_display->GetWindowWidth() || height != m_display->GetWindowHeight()))
    {
      m_display->ResizeRenderWindow(width, height);
      OnHostDisplayResized(width, height, m_display->GetWindowScale());
    }

    return;
  }

  m_surface = surface;

  if (m_display)
  {
    WindowInfo wi;
    wi.type = surface ? WindowInfo::Type::Android : WindowInfo::Type::Surfaceless;
    wi.window_handle = surface;
    wi.surface_width = width;
    wi.surface_height = height;
    wi.surface_scale = m_display->GetWindowScale();

    m_display->ChangeRenderWindow(wi);
    if (surface)
      OnHostDisplayResized(width, height, m_display->GetWindowScale());

    if (surface && System::GetState() == System::State::Paused)
      PauseSystem(false);
    else if (!surface && System::IsRunning())
      PauseSystem(true);
  }
}

void AndroidHostInterface::SetDisplayAlignment(HostDisplay::Alignment alignment)
{
  m_display_alignment = alignment;
  if (m_display)
    m_display->SetDisplayAlignment(alignment);
}

void AndroidHostInterface::SetControllerType(u32 index, std::string_view type_name)
{
  ControllerType type =
    Settings::ParseControllerTypeName(std::string(type_name).c_str()).value_or(ControllerType::None);

  if (!IsEmulationThreadRunning())
  {
    g_settings.controller_types[index] = type;
    return;
  }

  RunOnEmulationThread(
    [index, type]() {
      Log_InfoPrintf("Changing controller slot %d to %s", index, Settings::GetControllerTypeName(type));
      g_settings.controller_types[index] = type;
      System::UpdateControllers();
    },
    false);
}

void AndroidHostInterface::SetControllerButtonState(u32 index, s32 button_code, bool pressed)
{
  if (!IsEmulationThreadRunning())
    return;

  RunOnEmulationThread(
    [index, button_code, pressed]() {
      Controller* controller = System::GetController(index);
      if (!controller)
        return;

      controller->SetButtonState(button_code, pressed);
    },
    false);
}

void AndroidHostInterface::SetControllerAxisState(u32 index, s32 button_code, float value)
{
  if (!IsEmulationThreadRunning())
    return;

  RunOnEmulationThread(
    [index, button_code, value]() {
      Controller* controller = System::GetController(index);
      if (!controller)
        return;

      controller->SetAxisState(button_code, value);
    },
    false);
}

void AndroidHostInterface::HandleControllerButtonEvent(u32 controller_index, u32 button_index, bool pressed)
{
  if (!IsEmulationThreadRunning())
    return;

  RunOnEmulationThread([this, controller_index, button_index, pressed]() {
    AndroidControllerInterface* ci = static_cast<AndroidControllerInterface*>(m_controller_interface.get());
    if (ci)
      ci->HandleButtonEvent(controller_index, button_index, pressed);
  });
}

void AndroidHostInterface::HandleControllerAxisEvent(u32 controller_index, u32 axis_index, float value)
{
  if (!IsEmulationThreadRunning())
    return;

  RunOnEmulationThread([this, controller_index, axis_index, value]() {
    AndroidControllerInterface* ci = static_cast<AndroidControllerInterface*>(m_controller_interface.get());
    if (ci)
      ci->HandleAxisEvent(controller_index, axis_index, value);
  });
}

bool AndroidHostInterface::HasControllerButtonBinding(u32 controller_index, u32 button)
{
  AndroidControllerInterface* ci = static_cast<AndroidControllerInterface*>(m_controller_interface.get());
  if (!ci)
    return false;

  return ci->HasButtonBinding(controller_index, button);
}

void AndroidHostInterface::SetControllerVibration(u32 controller_index, float small_motor, float large_motor)
{
  if (!m_emulation_activity_object)
    return;

  JNIEnv* env = AndroidHelpers::GetJNIEnv();
  DebugAssert(env);

  env->CallVoidMethod(m_emulation_activity_object, s_EmulationActivity_method_setInputDeviceVibration,
                      static_cast<jint>(controller_index), static_cast<jfloat>(small_motor),
                      static_cast<jfloat>(large_motor));
}

void AndroidHostInterface::SetFastForwardEnabled(bool enabled)
{
  m_fast_forward_enabled = enabled;
  UpdateSpeedLimiterState();
}

void AndroidHostInterface::RefreshGameList(bool invalidate_cache, bool invalidate_database,
                                           ProgressCallback* progress_callback)
{
  m_game_list->SetSearchDirectoriesFromSettings(*m_settings_interface);
  m_game_list->Refresh(invalidate_cache, invalidate_database, progress_callback);
}

bool AndroidHostInterface::ImportPatchCodesFromString(const std::string& str)
{
  CheatList* cl = new CheatList();
  if (!cl->LoadFromString(str, CheatList::Format::Autodetect) || cl->GetCodeCount() == 0)
    return false;

  RunOnEmulationThread([this, cl]() {
    u32 imported_count;
    if (!System::HasCheatList())
    {
      imported_count = cl->GetCodeCount();
      System::SetCheatList(std::unique_ptr<CheatList>(cl));
    }
    else
    {
      const u32 old_count = System::GetCheatList()->GetCodeCount();
      System::GetCheatList()->MergeList(*cl);
      imported_count = System::GetCheatList()->GetCodeCount() - old_count;
      delete cl;
    }

    AddFormattedOSDMessage(20.0f, "Imported %u patch codes.", imported_count);
    CommonHostInterface::SaveCheatList();
  });

  return true;
}

void AndroidHostInterface::SetVibration(bool enabled)
{
  const u64 current_time = Common::Timer::GetValue();
  if (Common::Timer::ConvertValueToSeconds(current_time - m_last_vibration_update_time) < 0.1f &&
      m_last_vibration_state == enabled)
  {
    return;
  }

  m_last_vibration_state = enabled;
  m_last_vibration_update_time = current_time;

  JNIEnv* env = AndroidHelpers::GetJNIEnv();
  if (m_emulation_activity_object)
  {
    env->CallVoidMethod(m_emulation_activity_object, s_EmulationActivity_method_setVibration,
                        static_cast<jboolean>(enabled));
  }
}

void AndroidHostInterface::UpdateVibration()
{
  static constexpr float THRESHOLD = 0.5f;

  bool vibration_state = false;

  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    Controller* controller = System::GetController(i);
    if (!controller)
      continue;

    const u32 motors = controller->GetVibrationMotorCount();
    for (u32 j = 0; j < motors; j++)
    {
      if (controller->GetVibrationMotorStrength(j) >= THRESHOLD)
      {
        vibration_state = true;
        break;
      }
    }
  }

  SetVibration(vibration_state);
}

jobjectArray AndroidHostInterface::GetInputProfileNames(JNIEnv* env) const
{
  const InputProfileList profile_list(GetInputProfileList());
  if (profile_list.empty())
    return nullptr;

  jobjectArray name_array = env->NewObjectArray(static_cast<u32>(profile_list.size()), s_String_class, nullptr);
  u32 name_array_index = 0;
  Assert(name_array != nullptr);
  for (const InputProfileEntry& e : profile_list)
  {
    jstring axis_name_jstr = env->NewStringUTF(e.name.c_str());
    env->SetObjectArrayElement(name_array, name_array_index++, axis_name_jstr);
    env->DeleteLocalRef(axis_name_jstr);
  }

  return name_array;
}

extern "C" jint JNI_OnLoad(JavaVM* vm, void* reserved)
{
  Log::SetDebugOutputParams(true, nullptr, LOGLEVEL_DEV);
  s_jvm = vm;

  // Create global reference so it doesn't get cleaned up.
  JNIEnv* env = AndroidHelpers::GetJNIEnv();
  jclass string_class, host_interface_class, patch_code_class, game_list_entry_class, save_state_info_class,
    achievement_class, memory_card_file_info_class;
  if ((string_class = env->FindClass("java/lang/String")) == nullptr ||
      (s_String_class = static_cast<jclass>(env->NewGlobalRef(string_class))) == nullptr ||
      (host_interface_class = env->FindClass("com/github/stenzek/duckstation/AndroidHostInterface")) == nullptr ||
      (s_AndroidHostInterface_class = static_cast<jclass>(env->NewGlobalRef(host_interface_class))) == nullptr ||
      (patch_code_class = env->FindClass("com/github/stenzek/duckstation/PatchCode")) == nullptr ||
      (s_PatchCode_class = static_cast<jclass>(env->NewGlobalRef(patch_code_class))) == nullptr ||
      (game_list_entry_class = env->FindClass("com/github/stenzek/duckstation/GameListEntry")) == nullptr ||
      (s_GameListEntry_class = static_cast<jclass>(env->NewGlobalRef(game_list_entry_class))) == nullptr ||
      (save_state_info_class = env->FindClass("com/github/stenzek/duckstation/SaveStateInfo")) == nullptr ||
      (s_SaveStateInfo_class = static_cast<jclass>(env->NewGlobalRef(save_state_info_class))) == nullptr ||
      (achievement_class = env->FindClass("com/github/stenzek/duckstation/Achievement")) == nullptr ||
      (s_Achievement_class = static_cast<jclass>(env->NewGlobalRef(achievement_class))) == nullptr ||
      (memory_card_file_info_class = env->FindClass("com/github/stenzek/duckstation/MemoryCardFileInfo")) == nullptr ||
      (s_MemoryCardFileInfo_class = static_cast<jclass>(env->NewGlobalRef(memory_card_file_info_class))) == nullptr)
  {
    Log_ErrorPrint("AndroidHostInterface class lookup failed");
    return -1;
  }

  env->DeleteLocalRef(string_class);
  env->DeleteLocalRef(host_interface_class);
  env->DeleteLocalRef(patch_code_class);
  env->DeleteLocalRef(game_list_entry_class);
  env->DeleteLocalRef(achievement_class);
  env->DeleteLocalRef(memory_card_file_info_class);

  jclass emulation_activity_class;
  if ((s_AndroidHostInterface_constructor =
         env->GetMethodID(s_AndroidHostInterface_class, "<init>", "(Landroid/content/Context;)V")) == nullptr ||
      (s_AndroidHostInterface_field_mNativePointer =
         env->GetFieldID(s_AndroidHostInterface_class, "mNativePointer", "J")) == nullptr ||
      (s_AndroidHostInterface_method_reportError =
         env->GetMethodID(s_AndroidHostInterface_class, "reportError", "(Ljava/lang/String;)V")) == nullptr ||
      (s_AndroidHostInterface_method_reportMessage =
         env->GetMethodID(s_AndroidHostInterface_class, "reportMessage", "(Ljava/lang/String;)V")) == nullptr ||
      (s_AndroidHostInterface_method_openAssetStream = env->GetMethodID(
         s_AndroidHostInterface_class, "openAssetStream", "(Ljava/lang/String;)Ljava/io/InputStream;")) == nullptr ||
      (emulation_activity_class = env->FindClass("com/github/stenzek/duckstation/EmulationActivity")) == nullptr ||
      (s_EmulationActivity_class = static_cast<jclass>(env->NewGlobalRef(emulation_activity_class))) == nullptr ||
      (s_EmulationActivity_method_reportError =
         env->GetMethodID(s_EmulationActivity_class, "reportError", "(Ljava/lang/String;)V")) == nullptr ||
      (s_EmulationActivity_method_onEmulationStarted =
         env->GetMethodID(s_EmulationActivity_class, "onEmulationStarted", "()V")) == nullptr ||
      (s_EmulationActivity_method_onEmulationStopped =
         env->GetMethodID(s_EmulationActivity_class, "onEmulationStopped", "()V")) == nullptr ||
      (s_EmulationActivity_method_onGameTitleChanged =
         env->GetMethodID(s_EmulationActivity_class, "onGameTitleChanged", "(Ljava/lang/String;)V")) == nullptr ||
      (s_EmulationActivity_method_setVibration = env->GetMethodID(emulation_activity_class, "setVibration", "(Z)V")) ==
        nullptr ||
      (s_EmulationActivity_method_getRefreshRate =
         env->GetMethodID(emulation_activity_class, "getRefreshRate", "()F")) == nullptr ||
      (s_EmulationActivity_method_openPauseMenu = env->GetMethodID(emulation_activity_class, "openPauseMenu", "()V")) ==
        nullptr ||
      (s_EmulationActivity_method_getInputDeviceNames =
         env->GetMethodID(s_EmulationActivity_class, "getInputDeviceNames", "()[Ljava/lang/String;")) == nullptr ||
      (s_EmulationActivity_method_hasInputDeviceVibration =
         env->GetMethodID(s_EmulationActivity_class, "hasInputDeviceVibration", "(I)Z")) == nullptr ||
      (s_EmulationActivity_method_setInputDeviceVibration =
         env->GetMethodID(s_EmulationActivity_class, "setInputDeviceVibration", "(IFF)V")) == nullptr ||
      (s_PatchCode_constructor = env->GetMethodID(s_PatchCode_class, "<init>", "(ILjava/lang/String;Z)V")) == nullptr ||
      (s_GameListEntry_constructor = env->GetMethodID(
         s_GameListEntry_class, "<init>",
         "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;JLjava/lang/String;Ljava/lang/"
         "String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V")) == nullptr ||
      (s_SaveStateInfo_constructor = env->GetMethodID(
         s_SaveStateInfo_class, "<init>",
         "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;IZII[B)V")) ==
        nullptr ||
      (s_Achievement_constructor = env->GetMethodID(
         s_Achievement_class, "<init>",
         "(ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;IZ)V")) == nullptr ||
      (s_MemoryCardFileInfo_constructor = env->GetMethodID(s_MemoryCardFileInfo_class, "<init>",
                                                           "(Ljava/lang/String;Ljava/lang/String;III[[B)V")) == nullptr)
  {
    Log_ErrorPrint("AndroidHostInterface lookups failed");
    return -1;
  }

  env->DeleteLocalRef(emulation_activity_class);

  return JNI_VERSION_1_6;
}

#define DEFINE_JNI_METHOD(return_type, name)                                                                           \
  extern "C" JNIEXPORT return_type JNICALL Java_com_github_stenzek_duckstation_##name(JNIEnv* env)

#define DEFINE_JNI_ARGS_METHOD(return_type, name, ...)                                                                 \
  extern "C" JNIEXPORT return_type JNICALL Java_com_github_stenzek_duckstation_##name(JNIEnv* env, __VA_ARGS__)

DEFINE_JNI_ARGS_METHOD(jstring, AndroidHostInterface_getScmVersion, jobject unused)
{
  return env->NewStringUTF(g_scm_tag_str);
}

DEFINE_JNI_ARGS_METHOD(jstring, AndroidHostInterface_getFullScmVersion, jobject unused)
{
  return env->NewStringUTF(SmallString::FromFormat("DuckStation for Android %s (%s)\nBuilt %s %s", g_scm_tag_str,
                                                   g_scm_branch_str, __DATE__, __TIME__));
}

DEFINE_JNI_ARGS_METHOD(void, AndroidHostInterface_setThreadAffinity, jobject unused, jintArray cores)
{
  // https://github.com/googlearchive/android-audio-high-performance/blob/c232c21bf35d3bfea16537b781c526b8abdcc3cf/SimpleSynth/app/src/main/cpp/audio_player.cc
  int length = env->GetArrayLength(cores);
  int* p_cores = env->GetIntArrayElements(cores, nullptr);

  pid_t current_thread_id = gettid();
  cpu_set_t cpu_set;
  CPU_ZERO(&cpu_set);
  for (int i = 0; i < length; i++)
  {
    Log_InfoPrintf("Binding to CPU %d", p_cores[i]);
    CPU_SET(p_cores[i], &cpu_set);
  }

  int result = sched_setaffinity(current_thread_id, sizeof(cpu_set_t), &cpu_set);
  if (result != 0)
    Log_InfoPrintf("Thread affinity set.");
  else
    Log_ErrorPrintf("Error setting thread affinity: %d", result);

  env->ReleaseIntArrayElements(cores, p_cores, 0);
}

DEFINE_JNI_ARGS_METHOD(jobject, AndroidHostInterface_create, jobject unused, jobject context_object,
                       jstring user_directory)
{
  Log::SetDebugOutputParams(true, nullptr, LOGLEVEL_DEBUG);

  // initialize the java side
  jobject java_obj = env->NewObject(s_AndroidHostInterface_class, s_AndroidHostInterface_constructor, context_object);
  if (!java_obj)
  {
    Log_ErrorPrint("Failed to create Java AndroidHostInterface");
    return nullptr;
  }

  jobject java_obj_ref = env->NewGlobalRef(java_obj);
  Assert(java_obj_ref != nullptr);

  // initialize the C++ side
  std::string user_directory_str = AndroidHelpers::JStringToString(env, user_directory);
  AndroidHostInterface* cpp_obj = new AndroidHostInterface(java_obj_ref, context_object, std::move(user_directory_str));
  if (!cpp_obj->Initialize())
  {
    // TODO: Do we need to release the original java object reference?
    Log_ErrorPrint("Failed to create C++ AndroidHostInterface");
    env->DeleteGlobalRef(java_obj_ref);
    return nullptr;
  }

  env->SetLongField(java_obj, s_AndroidHostInterface_field_mNativePointer,
                    static_cast<long>(reinterpret_cast<uintptr_t>(cpp_obj)));

  return java_obj;
}

DEFINE_JNI_ARGS_METHOD(jboolean, AndroidHostInterface_isEmulationThreadRunning, jobject obj)
{
  return AndroidHelpers::GetNativeClass(env, obj)->IsEmulationThreadRunning();
}

DEFINE_JNI_ARGS_METHOD(void, AndroidHostInterface_runEmulationThread, jobject obj, jobject emulationActivity,
                       jstring filename, jboolean resume_state, jstring state_filename)
{
  std::string state_filename_str = AndroidHelpers::JStringToString(env, state_filename);

  SystemBootParameters boot_params;
  boot_params.filename = AndroidHelpers::JStringToString(env, filename);

  AndroidHelpers::GetNativeClass(env, obj)->EmulationThreadEntryPoint(env, emulationActivity, std::move(boot_params),
                                                                      resume_state);
}

DEFINE_JNI_ARGS_METHOD(void, AndroidHostInterface_stopEmulationThreadLoop, jobject obj)
{
  AndroidHelpers::GetNativeClass(env, obj)->StopEmulationThreadLoop();
}

DEFINE_JNI_ARGS_METHOD(void, AndroidHostInterface_surfaceChanged, jobject obj, jobject surface, jint format, jint width,
                       jint height)
{
  ANativeWindow* native_surface = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
  if (surface && !native_surface)
    Log_ErrorPrint("ANativeWindow_fromSurface() returned null");

  if (!surface && System::GetState() == System::State::Starting)
  {
    // User switched away from the app while it was compiling shaders.
    Log_ErrorPrintf("Surface destroyed while starting, cancelling");
    System::CancelPendingStartup();
  }

  // We should wait for the emu to finish if the surface is being destroyed or changed.
  AndroidHostInterface* hi = AndroidHelpers::GetNativeClass(env, obj);
  const bool block = (!native_surface || native_surface != hi->GetSurface());
  hi->RunOnEmulationThread(
    [hi, native_surface, format, width, height]() { hi->SurfaceChanged(native_surface, format, width, height); },
    block);
}

DEFINE_JNI_ARGS_METHOD(void, AndroidHostInterface_setControllerType, jobject obj, jint index, jstring controller_type)
{
  AndroidHelpers::GetNativeClass(env, obj)->SetControllerType(index,
                                                              AndroidHelpers::JStringToString(env, controller_type));
}

DEFINE_JNI_ARGS_METHOD(void, AndroidHostInterface_setControllerButtonState, jobject obj, jint index, jint button_code,
                       jboolean pressed)
{
  AndroidHelpers::GetNativeClass(env, obj)->SetControllerButtonState(index, button_code, pressed);
}

DEFINE_JNI_ARGS_METHOD(jint, AndroidHostInterface_getControllerButtonCode, jobject unused, jstring controller_type,
                       jstring button_name)
{
  std::optional<ControllerType> type =
    Settings::ParseControllerTypeName(AndroidHelpers::JStringToString(env, controller_type).c_str());
  if (!type)
    return -1;

  std::optional<s32> code =
    Controller::GetButtonCodeByName(type.value(), AndroidHelpers::JStringToString(env, button_name));
  return code.value_or(-1);
}

DEFINE_JNI_ARGS_METHOD(void, AndroidHostInterface_setControllerAxisState, jobject obj, jint index, jint button_code,
                       jfloat value)
{
  AndroidHelpers::GetNativeClass(env, obj)->SetControllerAxisState(index, button_code, value);
}

DEFINE_JNI_ARGS_METHOD(jint, AndroidHostInterface_getControllerAxisCode, jobject unused, jstring controller_type,
                       jstring axis_name)
{
  std::optional<ControllerType> type =
    Settings::ParseControllerTypeName(AndroidHelpers::JStringToString(env, controller_type).c_str());
  if (!type)
    return -1;

  std::optional<s32> code =
    Controller::GetAxisCodeByName(type.value(), AndroidHelpers::JStringToString(env, axis_name));
  return code.value_or(-1);
}

DEFINE_JNI_ARGS_METHOD(jobjectArray, AndroidHostInterface_getControllerButtonNames, jobject unused,
                       jstring controller_type)
{
  std::optional<ControllerType> type =
    Settings::ParseControllerTypeName(AndroidHelpers::JStringToString(env, controller_type).c_str());
  if (!type)
    return nullptr;

  const Controller::ButtonList buttons(Controller::GetButtonNames(type.value()));
  if (buttons.empty())
    return nullptr;

  jobjectArray name_array = env->NewObjectArray(static_cast<u32>(buttons.size()), s_String_class, nullptr);
  u32 name_array_index = 0;
  Assert(name_array != nullptr);
  for (const auto& [button_name, button_code] : buttons)
  {
    jstring button_name_jstr = env->NewStringUTF(button_name.c_str());
    env->SetObjectArrayElement(name_array, name_array_index++, button_name_jstr);
    env->DeleteLocalRef(button_name_jstr);
  }

  return name_array;
}

DEFINE_JNI_ARGS_METHOD(jobjectArray, AndroidHostInterface_getControllerAxisNames, jobject unused,
                       jstring controller_type)
{
  std::optional<ControllerType> type =
    Settings::ParseControllerTypeName(AndroidHelpers::JStringToString(env, controller_type).c_str());
  if (!type)
    return nullptr;

  const Controller::AxisList axes(Controller::GetAxisNames(type.value()));
  if (axes.empty())
    return nullptr;

  jobjectArray name_array = env->NewObjectArray(static_cast<u32>(axes.size()), s_String_class, nullptr);
  u32 name_array_index = 0;
  Assert(name_array != nullptr);
  for (const auto& [axis_name, axis_code, axis_type] : axes)
  {
    jstring axis_name_jstr = env->NewStringUTF(axis_name.c_str());
    env->SetObjectArrayElement(name_array, name_array_index++, axis_name_jstr);
    env->DeleteLocalRef(axis_name_jstr);
  }

  return name_array;
}

DEFINE_JNI_ARGS_METHOD(jint, AndroidHostInterface_getControllerVibrationMotorCount, jobject unused,
                       jstring controller_type)
{
  std::optional<ControllerType> type =
    Settings::ParseControllerTypeName(AndroidHelpers::JStringToString(env, controller_type).c_str());
  if (!type)
    return 0;

  return static_cast<jint>(Controller::GetVibrationMotorCount(type.value()));
}

DEFINE_JNI_ARGS_METHOD(void, AndroidHostInterface_handleControllerButtonEvent, jobject obj, jint controller_index,
                       jint button_index, jboolean pressed)
{
  AndroidHelpers::GetNativeClass(env, obj)->HandleControllerButtonEvent(static_cast<u32>(controller_index),
                                                                        static_cast<u32>(button_index), pressed);
}

DEFINE_JNI_ARGS_METHOD(void, AndroidHostInterface_handleControllerAxisEvent, jobject obj, jint controller_index,
                       jint axis_index, jfloat value)
{
  AndroidHelpers::GetNativeClass(env, obj)->HandleControllerAxisEvent(static_cast<u32>(controller_index),
                                                                      static_cast<u32>(axis_index), value);
}

DEFINE_JNI_ARGS_METHOD(jboolean, AndroidHostInterface_hasControllerButtonBinding, jobject obj, jint controller_index,
                       jint button_index)
{
  return AndroidHelpers::GetNativeClass(env, obj)->HasControllerButtonBinding(static_cast<u32>(controller_index),
                                                                              static_cast<u32>(button_index));
}

DEFINE_JNI_ARGS_METHOD(jobjectArray, AndroidHostInterface_getInputProfileNames, jobject obj)
{
  AndroidHostInterface* hi = AndroidHelpers::GetNativeClass(env, obj);
  return hi->GetInputProfileNames(env);
}

DEFINE_JNI_ARGS_METHOD(jboolean, AndroidHostInterface_loadInputProfile, jobject obj, jstring name)
{
  AndroidHostInterface* hi = AndroidHelpers::GetNativeClass(env, obj);
  const std::string profile_name(AndroidHelpers::JStringToString(env, name));
  if (profile_name.empty())
    return false;

  const std::string profile_path(hi->GetInputProfilePath(profile_name.c_str()));
  if (profile_path.empty())
    return false;

  return hi->ApplyInputProfile(profile_path.c_str());
}

DEFINE_JNI_ARGS_METHOD(jboolean, AndroidHostInterface_saveInputProfile, jobject obj, jstring name)
{
  AndroidHostInterface* hi = AndroidHelpers::GetNativeClass(env, obj);
  const std::string profile_name(AndroidHelpers::JStringToString(env, name));
  if (profile_name.empty())
    return false;

  const std::string profile_path(hi->GetSavePathForInputProfile(profile_name.c_str()));
  if (profile_path.empty())
    return false;

  return hi->SaveInputProfile(profile_path.c_str());
}

DEFINE_JNI_ARGS_METHOD(void, AndroidHostInterface_refreshGameList, jobject obj, jboolean invalidate_cache,
                       jboolean invalidate_database, jobject progress_callback)
{
  AndroidProgressCallback cb(env, progress_callback);
  AndroidHelpers::GetNativeClass(env, obj)->RefreshGameList(invalidate_cache, invalidate_database, &cb);
}

static const char* DiscRegionToString(DiscRegion region)
{
  static std::array<const char*, 4> names = {{"NTSC_J", "NTSC_U", "PAL", "Other"}};
  return names[static_cast<int>(region)];
}

static jobject CreateGameListEntry(JNIEnv* env, AndroidHostInterface* hi, const GameListEntry& entry)
{
  const Timestamp modified_ts(
    Timestamp::FromUnixTimestamp(static_cast<Timestamp::UnixTimestampValue>(entry.last_modified_time)));
  const std::string file_title_str(System::GetTitleForPath(entry.path.c_str()));
  const std::string cover_path_str(hi->GetGameList()->GetCoverImagePathForEntry(&entry));

  jstring path = env->NewStringUTF(entry.path.c_str());
  jstring code = env->NewStringUTF(entry.code.c_str());
  jstring title = env->NewStringUTF(entry.title.c_str());
  jstring file_title = env->NewStringUTF(file_title_str.c_str());
  jstring region = env->NewStringUTF(DiscRegionToString(entry.region));
  jstring type = env->NewStringUTF(GameList::EntryTypeToString(entry.type));
  jstring compatibility_rating =
    env->NewStringUTF(GameList::EntryCompatibilityRatingToString(entry.compatibility_rating));
  jstring cover_path = (cover_path_str.empty()) ? nullptr : env->NewStringUTF(cover_path_str.c_str());
  jstring modified_time = env->NewStringUTF(modified_ts.ToString("%Y/%m/%d, %H:%M:%S"));
  jlong size = entry.total_size;

  jobject entry_jobject =
    env->NewObject(s_GameListEntry_class, s_GameListEntry_constructor, path, code, title, file_title, size,
                   modified_time, region, type, compatibility_rating, cover_path);

  env->DeleteLocalRef(modified_time);
  if (cover_path)
    env->DeleteLocalRef(cover_path);
  env->DeleteLocalRef(compatibility_rating);
  env->DeleteLocalRef(type);
  env->DeleteLocalRef(region);
  env->DeleteLocalRef(file_title);
  env->DeleteLocalRef(title);
  env->DeleteLocalRef(code);
  env->DeleteLocalRef(path);

  return entry_jobject;
}

DEFINE_JNI_ARGS_METHOD(jarray, AndroidHostInterface_getGameListEntries, jobject obj)
{
  AndroidHostInterface* hi = AndroidHelpers::GetNativeClass(env, obj);
  jobjectArray entry_array = env->NewObjectArray(hi->GetGameList()->GetEntryCount(), s_GameListEntry_class, nullptr);
  Assert(entry_array != nullptr);

  u32 counter = 0;
  for (const GameListEntry& entry : hi->GetGameList()->GetEntries())
  {
    jobject entry_jobject = CreateGameListEntry(env, hi, entry);
    env->SetObjectArrayElement(entry_array, counter++, entry_jobject);
    env->DeleteLocalRef(entry_jobject);
  }

  return entry_array;
}

DEFINE_JNI_ARGS_METHOD(jobject, AndroidHostInterface_getGameListEntry, jobject obj, jstring path)
{
  AndroidHostInterface* hi = AndroidHelpers::GetNativeClass(env, obj);
  const std::string path_str(AndroidHelpers::JStringToString(env, path));
  const GameListEntry* entry = hi->GetGameList()->GetEntryForPath(path_str.c_str());
  if (!entry)
    return nullptr;

  return CreateGameListEntry(env, hi, *entry);
}

DEFINE_JNI_ARGS_METHOD(jstring, AndroidHostInterface_getGameSettingValue, jobject obj, jstring path, jstring key)
{
  AndroidHostInterface* hi = AndroidHelpers::GetNativeClass(env, obj);
  const std::string path_str(AndroidHelpers::JStringToString(env, path));
  const std::string key_str(AndroidHelpers::JStringToString(env, key));

  const GameListEntry* entry = hi->GetGameList()->GetEntryForPath(path_str.c_str());
  if (!entry)
    return nullptr;

  std::optional<std::string> value = entry->settings.GetValueForKey(key_str);
  if (!value.has_value())
    return nullptr;
  else
    return env->NewStringUTF(value->c_str());
}

DEFINE_JNI_ARGS_METHOD(void, AndroidHostInterface_setGameSettingValue, jobject obj, jstring path, jstring key,
                       jstring value)
{
  AndroidHostInterface* hi = AndroidHelpers::GetNativeClass(env, obj);
  const std::string path_str(AndroidHelpers::JStringToString(env, path));
  const std::string key_str(AndroidHelpers::JStringToString(env, key));

  const GameListEntry* entry = hi->GetGameList()->GetEntryForPath(path_str.c_str());
  if (!entry)
    return;

  GameSettings::Entry new_entry(entry->settings);

  std::optional<std::string> value_str;
  if (value)
    value_str = AndroidHelpers::JStringToString(env, value);

  new_entry.SetValueForKey(key_str, value_str);
  hi->GetGameList()->UpdateGameSettings(path_str, entry->code, entry->title, new_entry, true);
}

DEFINE_JNI_ARGS_METHOD(jobjectArray, AndroidHostInterface_getHotkeyInfoList, jobject obj)
{
  jclass entry_class = env->FindClass("com/github/stenzek/duckstation/HotkeyInfo");
  Assert(entry_class != nullptr);

  jmethodID entry_constructor =
    env->GetMethodID(entry_class, "<init>", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
  Assert(entry_constructor != nullptr);

  AndroidHostInterface* hi = AndroidHelpers::GetNativeClass(env, obj);
  const CommonHostInterface::HotkeyInfoList& hotkeys = hi->GetHotkeyInfoList();
  if (hotkeys.empty())
    return nullptr;

  jobjectArray entry_array = env->NewObjectArray(static_cast<jsize>(hotkeys.size()), entry_class, nullptr);
  Assert(entry_array != nullptr);

  u32 counter = 0;
  for (const CommonHostInterface::HotkeyInfo& hk : hotkeys)
  {
    jstring category = env->NewStringUTF(hk.category.GetCharArray());
    jstring name = env->NewStringUTF(hk.name.GetCharArray());
    jstring display_name = env->NewStringUTF(hk.display_name.GetCharArray());

    jobject entry_jobject = env->NewObject(entry_class, entry_constructor, category, name, display_name);

    env->SetObjectArrayElement(entry_array, counter++, entry_jobject);
    env->DeleteLocalRef(entry_jobject);
    env->DeleteLocalRef(display_name);
    env->DeleteLocalRef(name);
    env->DeleteLocalRef(category);
  }

  return entry_array;
}

DEFINE_JNI_ARGS_METHOD(void, AndroidHostInterface_applySettings, jobject obj)
{
  AndroidHostInterface* hi = AndroidHelpers::GetNativeClass(env, obj);
  if (hi->IsEmulationThreadRunning())
  {
    hi->RunOnEmulationThread([hi]() { hi->ApplySettings(false); });
  }
  else
  {
    hi->ApplySettings(false);
  }
}

DEFINE_JNI_ARGS_METHOD(void, AndroidHostInterface_updateInputMap, jobject obj)
{
  AndroidHostInterface* hi = AndroidHelpers::GetNativeClass(env, obj);
  if (hi->IsEmulationThreadRunning())
  {
    hi->RunOnEmulationThread([hi]() { hi->UpdateInputMap(); });
  }
  else
  {
    hi->UpdateInputMap();
  }
}

DEFINE_JNI_ARGS_METHOD(void, AndroidHostInterface_resetSystem, jobject obj, jboolean global, jint slot)
{
  AndroidHostInterface* hi = AndroidHelpers::GetNativeClass(env, obj);
  hi->RunOnEmulationThread([hi]() { hi->ResetSystem(); });
}

DEFINE_JNI_ARGS_METHOD(void, AndroidHostInterface_loadState, jobject obj, jboolean global, jint slot)
{
  AndroidHostInterface* hi = AndroidHelpers::GetNativeClass(env, obj);
  hi->RunOnEmulationThread([hi, global, slot]() { hi->LoadState(global, slot); });
}

DEFINE_JNI_ARGS_METHOD(void, AndroidHostInterface_saveState, jobject obj, jboolean global, jint slot)
{
  AndroidHostInterface* hi = AndroidHelpers::GetNativeClass(env, obj);
  hi->RunOnEmulationThread([hi, global, slot]() { hi->SaveState(global, slot); });
}

DEFINE_JNI_ARGS_METHOD(void, AndroidHostInterface_saveResumeState, jobject obj, jboolean wait_for_completion)
{
  if (!System::IsValid() || System::GetState() == System::State::Starting)
  {
    // This gets called when the surface is destroyed, which can happen while starting.
    return;
  }

  AndroidHostInterface* hi = AndroidHelpers::GetNativeClass(env, obj);
  hi->RunOnEmulationThread([hi]() { hi->SaveResumeSaveState(); }, wait_for_completion);
}

DEFINE_JNI_ARGS_METHOD(void, AndroidHostInterface_setDisplayAlignment, jobject obj, jint alignment)
{
  AndroidHostInterface* hi = AndroidHelpers::GetNativeClass(env, obj);
  hi->RunOnEmulationThread(
    [hi, alignment]() { hi->SetDisplayAlignment(static_cast<HostDisplay::Alignment>(alignment)); }, false);
}

DEFINE_JNI_ARGS_METHOD(bool, AndroidHostInterface_hasSurface, jobject obj)
{
  AndroidHostInterface* hi = AndroidHelpers::GetNativeClass(env, obj);
  HostDisplay* display = hi->GetDisplay();
  if (display)
    return display->HasRenderSurface();
  else
    return false;
}

DEFINE_JNI_ARGS_METHOD(bool, AndroidHostInterface_isEmulationThreadPaused, jobject obj)
{
  AndroidHostInterface* hi = AndroidHelpers::GetNativeClass(env, obj);
  return hi->IsEmulationThreadPaused();
}

DEFINE_JNI_ARGS_METHOD(void, AndroidHostInterface_pauseEmulationThread, jobject obj, jboolean paused)
{
  AndroidHostInterface* hi = AndroidHelpers::GetNativeClass(env, obj);
  hi->PauseEmulationThread(paused);
}

DEFINE_JNI_ARGS_METHOD(jobject, AndroidHostInterface_getPatchCodeList, jobject obj)
{
  if (!System::IsValid())
    return nullptr;

  AndroidHostInterface* hi = AndroidHelpers::GetNativeClass(env, obj);
  if (!System::HasCheatList())
  {
    // Hopefully this won't deadlock...
    hi->RunOnEmulationThread(
      [hi]() {
        if (!hi->LoadCheatListFromGameTitle())
          hi->LoadCheatListFromDatabase();
      },
      true);
  }

  if (!System::HasCheatList())
    return nullptr;

  CheatList* cl = System::GetCheatList();
  const u32 count = cl->GetCodeCount();

  jobjectArray arr = env->NewObjectArray(count, s_PatchCode_class, nullptr);
  for (u32 i = 0; i < count; i++)
  {
    const CheatCode& cc = cl->GetCode(i);

    jstring desc_str = env->NewStringUTF(cc.description.c_str());
    jobject java_cc =
      env->NewObject(s_PatchCode_class, s_PatchCode_constructor, static_cast<jint>(i), desc_str, cc.enabled);
    env->SetObjectArrayElement(arr, i, java_cc);
    env->DeleteLocalRef(java_cc);
    env->DeleteLocalRef(desc_str);
  }

  return arr;
}

DEFINE_JNI_ARGS_METHOD(jboolean, AndroidHostInterface_importPatchCodesFromString, jobject obj, jstring str)
{
  if (!System::IsValid())
    return false;

  AndroidHostInterface* hi = AndroidHelpers::GetNativeClass(env, obj);
  return hi->ImportPatchCodesFromString(AndroidHelpers::JStringToString(env, str));
}

DEFINE_JNI_ARGS_METHOD(void, AndroidHostInterface_setPatchCodeEnabled, jobject obj, jint index, jboolean enabled)
{
  if (!System::IsValid() || !System::HasCheatList())
    return;

  AndroidHostInterface* hi = AndroidHelpers::GetNativeClass(env, obj);
  hi->RunOnEmulationThread([index, enabled, hi]() { hi->SetCheatCodeState(static_cast<u32>(index), enabled, true); });
}

DEFINE_JNI_ARGS_METHOD(void, AndroidHostInterface_addOSDMessage, jobject obj, jstring message, jfloat duration)
{
  AndroidHostInterface* hi = AndroidHelpers::GetNativeClass(env, obj);
  hi->AddOSDMessage(AndroidHelpers::JStringToString(env, message), duration);
}

DEFINE_JNI_ARGS_METHOD(jboolean, AndroidHostInterface_hasAnyBIOSImages, jobject obj)
{
  AndroidHostInterface* hi = AndroidHelpers::GetNativeClass(env, obj);
  return hi->HasAnyBIOSImages();
}

DEFINE_JNI_ARGS_METHOD(jboolean, AndroidHostInterface_isFastForwardEnabled, jobject obj)
{
  return AndroidHelpers::GetNativeClass(env, obj)->IsRunningAtNonStandardSpeed();
}

DEFINE_JNI_ARGS_METHOD(void, AndroidHostInterface_setFastForwardEnabled, jobject obj, jboolean enabled)
{
  if (!System::IsValid())
    return;

  AndroidHostInterface* hi = AndroidHelpers::GetNativeClass(env, obj);
  hi->RunOnEmulationThread([enabled, hi]() { hi->SetFastForwardEnabled(enabled); });
}

DEFINE_JNI_ARGS_METHOD(jstring, AndroidHostInterface_importBIOSImage, jobject obj, jbyteArray data)
{
  AndroidHostInterface* hi = AndroidHelpers::GetNativeClass(env, obj);

  const jsize len = env->GetArrayLength(data);
  if (len != BIOS::BIOS_SIZE)
    return nullptr;

  BIOS::Image image;
  image.resize(static_cast<size_t>(len));
  env->GetByteArrayRegion(data, 0, len, reinterpret_cast<jbyte*>(image.data()));

  const BIOS::Hash hash = BIOS::GetHash(image);
  const BIOS::ImageInfo* ii = BIOS::GetImageInfoForHash(hash);

  const std::string dest_path(hi->GetUserDirectoryRelativePath("bios/%s.bin", hash.ToString().c_str()));
  if (FileSystem::FileExists(dest_path.c_str()) ||
      !FileSystem::WriteBinaryFile(dest_path.c_str(), image.data(), image.size()))
  {
    return nullptr;
  }

  if (ii)
    return env->NewStringUTF(ii->description);
  else
    return env->NewStringUTF(hash.ToString().c_str());
}

DEFINE_JNI_ARGS_METHOD(jobjectArray, AndroidHostInterface_getMediaPlaylistPaths, jobject obj)
{
  if (!System::IsValid())
    return nullptr;

  const u32 count = System::GetMediaPlaylistCount();
  if (count == 0)
    return nullptr;

  jobjectArray arr = env->NewObjectArray(static_cast<jsize>(count), s_String_class, nullptr);
  for (u32 i = 0; i < count; i++)
  {
    jstring str = env->NewStringUTF(System::GetMediaPlaylistPath(i).c_str());
    env->SetObjectArrayElement(arr, static_cast<jsize>(i), str);
    env->DeleteLocalRef(str);
  }

  return arr;
}

DEFINE_JNI_ARGS_METHOD(jint, AndroidHostInterface_getMediaPlaylistIndex, jobject obj)
{
  if (!System::IsValid())
    return -1;

  return System::GetMediaPlaylistIndex();
}

DEFINE_JNI_ARGS_METHOD(jboolean, AndroidHostInterface_setMediaPlaylistIndex, jobject obj, jint index)
{
  if (!System::IsValid() || index < 0 || static_cast<u32>(index) >= System::GetMediaPlaylistCount())
    return false;

  AndroidHostInterface* hi = AndroidHelpers::GetNativeClass(env, obj);
  hi->RunOnEmulationThread([index, hi]() {
    if (System::IsValid())
    {
      if (!System::SwitchMediaFromPlaylist(index))
        hi->AddOSDMessage("Disc switch failed. Please make sure the file exists.");
    }
  });

  return true;
}

DEFINE_JNI_ARGS_METHOD(jboolean, AndroidHostInterface_setMediaFilename, jstring obj, jstring filename)
{
  if (!System::IsValid() || !filename)
    return false;

  std::string filename_str(AndroidHelpers::JStringToString(env, filename));
  AndroidHostInterface* hi = AndroidHelpers::GetNativeClass(env, obj);
  hi->RunOnEmulationThread([filename_str, hi]() {
    if (System::IsValid())
    {
      if (!System::InsertMedia(filename_str.c_str()))
        hi->AddOSDMessage("Disc switch failed. Please make sure the file exists and is a supported disc image.");
    }
  });

  return true;
}

static jobject CreateSaveStateInfo(JNIEnv* env, const CommonHostInterface::ExtendedSaveStateInfo& ssi)
{
  LocalRefHolder<jstring> path(env, env->NewStringUTF(ssi.path.c_str()));
  LocalRefHolder<jstring> title(env, env->NewStringUTF(ssi.title.c_str()));
  LocalRefHolder<jstring> code(env, env->NewStringUTF(ssi.game_code.c_str()));
  LocalRefHolder<jstring> media_path(env, env->NewStringUTF(ssi.media_path.c_str()));
  LocalRefHolder<jstring> timestamp(env, env->NewStringUTF(Timestamp::FromUnixTimestamp(ssi.timestamp).ToString("%c")));
  LocalRefHolder<jbyteArray> screenshot_data;
  if (!ssi.screenshot_data.empty())
  {
    const jsize data_size = static_cast<jsize>(ssi.screenshot_data.size() * sizeof(u32));
    screenshot_data = LocalRefHolder<jbyteArray>(env, env->NewByteArray(data_size));
    env->SetByteArrayRegion(screenshot_data.Get(), 0, data_size,
                            reinterpret_cast<const jbyte*>(ssi.screenshot_data.data()));
  }

  return env->NewObject(s_SaveStateInfo_class, s_SaveStateInfo_constructor, path.Get(), title.Get(), code.Get(),
                        media_path.Get(), timestamp.Get(), static_cast<jint>(ssi.slot),
                        static_cast<jboolean>(ssi.global), static_cast<jint>(ssi.screenshot_width),
                        static_cast<jint>(ssi.screenshot_height), screenshot_data.Get());
}

static jobject CreateEmptySaveStateInfo(JNIEnv* env, s32 slot, bool global)
{
  return env->NewObject(s_SaveStateInfo_class, s_SaveStateInfo_constructor, nullptr, nullptr, nullptr, nullptr, nullptr,
                        static_cast<jint>(slot), static_cast<jboolean>(global), static_cast<jint>(0),
                        static_cast<jint>(0), nullptr);
}

DEFINE_JNI_ARGS_METHOD(jobjectArray, AndroidHostInterface_getSaveStateInfo, jobject obj, jboolean includeEmpty)
{
  if (!System::IsValid())
    return nullptr;

  AndroidHostInterface* hi = AndroidHelpers::GetNativeClass(env, obj);
  std::vector<jobject> infos;

  // +1 for the quick save only in android.
  infos.reserve(1 + CommonHostInterface::PER_GAME_SAVE_STATE_SLOTS + CommonHostInterface::GLOBAL_SAVE_STATE_SLOTS);

  const std::string& game_code = System::GetRunningCode();
  if (!game_code.empty())
  {
    for (u32 i = 0; i <= CommonHostInterface::PER_GAME_SAVE_STATE_SLOTS; i++)
    {
      std::optional<CommonHostInterface::ExtendedSaveStateInfo> esi =
        hi->GetExtendedSaveStateInfo(game_code.c_str(), static_cast<s32>(i));
      if (esi.has_value())
      {
        jobject obj = CreateSaveStateInfo(env, esi.value());
        if (obj)
          infos.push_back(obj);
      }
      else if (includeEmpty)
      {
        jobject obj = CreateEmptySaveStateInfo(env, static_cast<s32>(i), false);
        if (obj)
          infos.push_back(obj);
      }
    }
  }

  for (u32 i = 1; i <= CommonHostInterface::GLOBAL_SAVE_STATE_SLOTS; i++)
  {
    std::optional<CommonHostInterface::ExtendedSaveStateInfo> esi =
      hi->GetExtendedSaveStateInfo(nullptr, static_cast<s32>(i));
    if (esi.has_value())
    {
      jobject obj = CreateSaveStateInfo(env, esi.value());
      if (obj)
        infos.push_back(obj);
    }
    else if (includeEmpty)
    {
      jobject obj = CreateEmptySaveStateInfo(env, static_cast<s32>(i), true);
      if (obj)
        infos.push_back(obj);
    }
  }

  if (infos.empty())
    return nullptr;

  jobjectArray ret = env->NewObjectArray(static_cast<jsize>(infos.size()), s_SaveStateInfo_class, nullptr);
  for (size_t i = 0; i < infos.size(); i++)
  {
    env->SetObjectArrayElement(ret, static_cast<jsize>(i), infos[i]);
    env->DeleteLocalRef(infos[i]);
  }

  return ret;
}

DEFINE_JNI_ARGS_METHOD(void, AndroidHostInterface_toggleControllerAnalogMode, jobject obj)
{
  // hacky way to toggle analog mode
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    Controller* ctrl = System::GetController(i);
    if (!ctrl)
      continue;

    std::optional<s32> code = Controller::GetButtonCodeByName(ctrl->GetType(), "Analog");
    if (!code.has_value())
      continue;

    ctrl->SetButtonState(code.value(), true);
    ctrl->SetButtonState(code.value(), false);
  }
}

DEFINE_JNI_ARGS_METHOD(void, AndroidHostInterface_setFullscreenUINotificationVerticalPosition, jobject obj,
                       jfloat position, jfloat direction)
{
  AndroidHostInterface* hi = AndroidHelpers::GetNativeClass(env, obj);
  hi->RunOnEmulationThread(
    [position, direction]() { ImGuiFullscreen::SetNotificationVerticalPosition(position, direction); });
}

DEFINE_JNI_ARGS_METHOD(jboolean, AndroidHostInterface_isCheevosActive, jobject obj)
{
  return Cheevos::IsActive();
}

DEFINE_JNI_ARGS_METHOD(jboolean, AndroidHostInterface_isCheevosChallengeModeActive, jobject obj)
{
  return Cheevos::IsChallengeModeActive();
}

DEFINE_JNI_ARGS_METHOD(jobjectArray, AndroidHostInterface_getCheevoList, jobject obj)
{
  if (!Cheevos::IsActive())
    return nullptr;

  std::vector<jobject> cheevos;
  Cheevos::EnumerateAchievements([env, &cheevos](const Cheevos::Achievement& cheevo) {
    jstring title = env->NewStringUTF(cheevo.title.c_str());
    jstring description = env->NewStringUTF(cheevo.description.c_str());
    jstring locked_badge_path =
      cheevo.locked_badge_path.empty() ? nullptr : env->NewStringUTF(cheevo.locked_badge_path.c_str());
    jstring unlocked_badge_path =
      cheevo.unlocked_badge_path.empty() ? nullptr : env->NewStringUTF(cheevo.unlocked_badge_path.c_str());

    jobject object = env->NewObject(s_Achievement_class, s_Achievement_constructor, static_cast<jint>(cheevo.id), title,
                                    description, locked_badge_path, unlocked_badge_path,
                                    static_cast<jint>(cheevo.points), static_cast<jboolean>(cheevo.locked));
    cheevos.push_back(object);

    if (unlocked_badge_path)
      env->DeleteLocalRef(unlocked_badge_path);
    if (locked_badge_path)
      env->DeleteLocalRef(locked_badge_path);
    env->DeleteLocalRef(description);
    env->DeleteLocalRef(title);
    return true;
  });

  if (cheevos.empty())
    return nullptr;

  jobjectArray ret = env->NewObjectArray(static_cast<jsize>(cheevos.size()), s_Achievement_class, nullptr);
  for (size_t i = 0; i < cheevos.size(); i++)
  {
    env->SetObjectArrayElement(ret, static_cast<jsize>(i), cheevos[i]);
    env->DeleteLocalRef(cheevos[i]);
  }

  return ret;
}

DEFINE_JNI_ARGS_METHOD(jint, AndroidHostInterface_getCheevoCount, jobject obj)
{
  return Cheevos::GetAchievementCount();
}

DEFINE_JNI_ARGS_METHOD(jint, AndroidHostInterface_getUnlockedCheevoCount, jobject obj)
{
  return Cheevos::GetUnlockedAchiementCount();
}

DEFINE_JNI_ARGS_METHOD(jint, AndroidHostInterface_getCheevoPointsForGame, jobject obj)
{
  return Cheevos::GetCurrentPointsForGame();
}

DEFINE_JNI_ARGS_METHOD(jint, AndroidHostInterface_getCheevoMaximumPointsForGame, jobject obj)
{
  return Cheevos::GetMaximumPointsForGame();
}

DEFINE_JNI_ARGS_METHOD(jstring, AndroidHostInterface_getCheevoGameTitle, jobject obj)
{
  const std::string& title = Cheevos::GetGameTitle();
  return title.empty() ? nullptr : env->NewStringUTF(title.c_str());
}

DEFINE_JNI_ARGS_METHOD(jstring, AndroidHostInterface_getCheevoGameIconPath, jobject obj)
{
  const std::string& path = Cheevos::GetGameIcon();
  return path.empty() ? nullptr : env->NewStringUTF(path.c_str());
}

DEFINE_JNI_ARGS_METHOD(jboolean, AndroidHostInterface_cheevosLogin, jobject obj, jstring username, jstring password)
{
  const std::string username_str(AndroidHelpers::JStringToString(env, username));
  const std::string password_str(AndroidHelpers::JStringToString(env, password));
  return Cheevos::Login(username_str.c_str(), password_str.c_str());
}

DEFINE_JNI_ARGS_METHOD(void, AndroidHostInterface_cheevosLogout, jobject obj)
{
  return Cheevos::Logout();
}

static_assert(sizeof(MemoryCardImage::DataArray) == MemoryCardImage::DATA_SIZE);

static MemoryCardImage::DataArray* GetMemoryCardData(JNIEnv* env, jbyteArray obj)
{
  if (!obj || env->GetArrayLength(obj) != MemoryCardImage::DATA_SIZE)
    return nullptr;

  return reinterpret_cast<MemoryCardImage::DataArray*>(env->GetByteArrayElements(obj, nullptr));
}

static void ReleaseMemoryCardData(JNIEnv* env, jbyteArray obj, MemoryCardImage::DataArray* data)
{
  env->ReleaseByteArrayElements(obj, reinterpret_cast<jbyte*>(data), 0);
}

DEFINE_JNI_ARGS_METHOD(jboolean, MemoryCardImage_isValid, jclass clazz, jbyteArray obj)
{
  MemoryCardImage::DataArray* data = GetMemoryCardData(env, obj);
  if (!data)
    return false;

  const bool res = MemoryCardImage::IsValid(*data);
  ReleaseMemoryCardData(env, obj, data);
  return res;
}

DEFINE_JNI_ARGS_METHOD(void, MemoryCardImage_format, jclass clazz, jbyteArray obj)
{
  MemoryCardImage::DataArray* data = GetMemoryCardData(env, obj);
  if (!data)
    return;

  MemoryCardImage::Format(data);
  ReleaseMemoryCardData(env, obj, data);
}

DEFINE_JNI_ARGS_METHOD(jint, MemoryCardImage_getFreeBlocks, jclass clazz, jbyteArray obj)
{
  MemoryCardImage::DataArray* data = GetMemoryCardData(env, obj);
  if (!data)
    return 0;

  const u32 free_blocks = MemoryCardImage::GetFreeBlockCount(*data);
  ReleaseMemoryCardData(env, obj, data);
  return free_blocks;
}

DEFINE_JNI_ARGS_METHOD(jobjectArray, MemoryCardImage_getFiles, jclass clazz, jbyteArray obj)
{
  MemoryCardImage::DataArray* data = GetMemoryCardData(env, obj);
  if (!data)
    return nullptr;

  const std::vector<MemoryCardImage::FileInfo> files(MemoryCardImage::EnumerateFiles(*data));

  std::vector<jobject> file_objects;
  file_objects.reserve(files.size());

  jclass byteArrayClass = env->FindClass("[B");

  for (const MemoryCardImage::FileInfo& file : files)
  {
    jobject filename = env->NewStringUTF(file.filename.c_str());
    jobject title = env->NewStringUTF(file.title.c_str());
    jobjectArray frames = nullptr;
    if (!file.icon_frames.empty())
    {
      frames = env->NewObjectArray(static_cast<jint>(file.icon_frames.size()), byteArrayClass, nullptr);
      for (jsize i = 0; i < static_cast<jsize>(file.icon_frames.size()); i++)
      {
        static constexpr jsize frame_size = MemoryCardImage::ICON_WIDTH * MemoryCardImage::ICON_HEIGHT * sizeof(u32);
        jbyteArray frame_data = env->NewByteArray(frame_size);
        jbyte* frame_data_ptr = env->GetByteArrayElements(frame_data, nullptr);
        std::memcpy(frame_data_ptr, file.icon_frames[i].pixels, frame_size);
        env->ReleaseByteArrayElements(frame_data, frame_data_ptr, 0);
        env->SetObjectArrayElement(frames, i, frame_data);
        env->DeleteLocalRef(frame_data);
      }
    }

    file_objects.push_back(env->NewObject(s_MemoryCardFileInfo_class, s_MemoryCardFileInfo_constructor, filename, title,
                                          static_cast<jint>(file.size), static_cast<jint>(file.first_block),
                                          static_cast<int>(file.num_blocks), frames));

    env->DeleteLocalRef(frames);
    env->DeleteLocalRef(title);
    env->DeleteLocalRef(filename);
  }

  jobjectArray file_object_array =
    AndroidHelpers::CreateObjectArray(env, s_MemoryCardFileInfo_class, file_objects.data(), file_objects.size(), true);
  ReleaseMemoryCardData(env, obj, data);
  return file_object_array;
}

DEFINE_JNI_ARGS_METHOD(jboolean, MemoryCardImage_hasFile, jclass clazz, jbyteArray obj, jstring filename)
{
  MemoryCardImage::DataArray* data = GetMemoryCardData(env, obj);
  if (!data)
    return false;

  const std::string filename_str(AndroidHelpers::JStringToString(env, filename));
  bool result = false;
  if (!filename_str.empty())
  {
    const std::vector<MemoryCardImage::FileInfo> files(MemoryCardImage::EnumerateFiles(*data));
    result = std::any_of(files.begin(), files.end(),
                         [&filename_str](const MemoryCardImage::FileInfo& fi) { return fi.filename == filename_str; });
  }

  ReleaseMemoryCardData(env, obj, data);
  return result;
}

DEFINE_JNI_ARGS_METHOD(jbyteArray, MemoryCardImage_readFile, jclass clazz, jbyteArray obj, jstring filename)
{
  MemoryCardImage::DataArray* data = GetMemoryCardData(env, obj);
  if (!data)
    return nullptr;

  const std::string filename_str(AndroidHelpers::JStringToString(env, filename));
  jbyteArray ret = nullptr;
  if (!filename_str.empty())
  {
    const std::vector<MemoryCardImage::FileInfo> files(MemoryCardImage::EnumerateFiles(*data));
    auto iter = std::find_if(files.begin(), files.end(), [&filename_str](const MemoryCardImage::FileInfo& fi) {
      return fi.filename == filename_str;
    });
    if (iter != files.end())
    {
      std::vector<u8> file_data;
      if (MemoryCardImage::ReadFile(*data, *iter, &file_data))
        ret = AndroidHelpers::VectorToByteArray(env, file_data);
    }
  }

  ReleaseMemoryCardData(env, obj, data);
  return ret;
}

DEFINE_JNI_ARGS_METHOD(jboolean, MemoryCardImage_writeFile, jclass clazz, jbyteArray obj, jstring filename,
                       jbyteArray file_data)
{
  MemoryCardImage::DataArray* data = GetMemoryCardData(env, obj);
  if (!data)
    return false;

  const std::string filename_str(AndroidHelpers::JStringToString(env, filename));
  const std::vector<u8> file_data_vec(AndroidHelpers::ByteArrayToVector(env, file_data));
  bool ret = false;
  if (!filename_str.empty())
    ret = MemoryCardImage::WriteFile(data, filename_str, file_data_vec);

  ReleaseMemoryCardData(env, obj, data);
  return ret;
}

DEFINE_JNI_ARGS_METHOD(jboolean, MemoryCardImage_deleteFile, jclass clazz, jbyteArray obj, jstring filename)
{
  MemoryCardImage::DataArray* data = GetMemoryCardData(env, obj);
  if (!data)
    return false;

  const std::string filename_str(AndroidHelpers::JStringToString(env, filename));
  bool ret = false;

  if (!filename_str.empty())
  {
    const std::vector<MemoryCardImage::FileInfo> files(MemoryCardImage::EnumerateFiles(*data));
    auto iter = std::find_if(files.begin(), files.end(), [&filename_str](const MemoryCardImage::FileInfo& fi) {
      return fi.filename == filename_str;
    });

    if (iter != files.end())
      ret = MemoryCardImage::DeleteFile(data, *iter);
  }

  ReleaseMemoryCardData(env, obj, data);
  return ret;
}

DEFINE_JNI_ARGS_METHOD(jboolean, MemoryCardImage_importCard, jclass clazz, jbyteArray obj, jstring filename,
                       jbyteArray import_data)
{
  MemoryCardImage::DataArray* data = GetMemoryCardData(env, obj);
  if (!data)
    return false;

  const std::string filename_str(AndroidHelpers::JStringToString(env, filename));
  std::vector<u8> import_data_vec(AndroidHelpers::ByteArrayToVector(env, import_data));
  bool ret = false;
  if (!filename_str.empty() && !import_data_vec.empty())
    ret = MemoryCardImage::ImportCard(data, filename_str.c_str(), std::move(import_data_vec));

  ReleaseMemoryCardData(env, obj, data);
  return ret;
}