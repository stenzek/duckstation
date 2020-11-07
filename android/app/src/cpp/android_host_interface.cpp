#include "android_host_interface.h"
#include "android_progress_callback.h"
#include "common/assert.h"
#include "common/audio_stream.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/string.h"
#include "common/string_util.h"
#include "common/timestamp.h"
#include "core/bios.h"
#include "core/cheats.h"
#include "core/controller.h"
#include "core/gpu.h"
#include "core/host_display.h"
#include "core/system.h"
#include "frontend-common/game_list.h"
#include "frontend-common/imgui_styles.h"
#include "frontend-common/opengl_host_display.h"
#include "frontend-common/vulkan_host_display.h"
#include "scmversion/scmversion.h"
#include <android/native_window_jni.h>
#include <cmath>
#include <imgui.h>
Log_SetChannel(AndroidHostInterface);

#ifdef USE_OPENSLES
#include "opensles_audio_stream.h"
#endif

static JavaVM* s_jvm;
static jclass s_AndroidHostInterface_class;
static jmethodID s_AndroidHostInterface_constructor;
static jfieldID s_AndroidHostInterface_field_mNativePointer;
static jmethodID s_AndroidHostInterface_method_reportError;
static jmethodID s_AndroidHostInterface_method_reportMessage;
static jmethodID s_EmulationActivity_method_reportError;
static jmethodID s_EmulationActivity_method_reportMessage;
static jmethodID s_EmulationActivity_method_onEmulationStarted;
static jmethodID s_EmulationActivity_method_onEmulationStopped;
static jmethodID s_EmulationActivity_method_onGameTitleChanged;
static jclass s_CheatCode_class;
static jmethodID s_CheatCode_constructor;

namespace AndroidHelpers {
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
} // namespace AndroidHelpers

AndroidHostInterface::AndroidHostInterface(jobject java_object, jobject context_object, std::string user_directory)
  : m_java_object(java_object), m_settings_interface(context_object)
{
  m_user_directory = std::move(user_directory);
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
}

void AndroidHostInterface::ReportMessage(const char* message)
{
  CommonHostInterface::ReportMessage(message);

  JNIEnv* env = AndroidHelpers::GetJNIEnv();
  jstring message_jstr = env->NewStringUTF(message);
  if (m_emulation_activity_object)
    env->CallVoidMethod(m_emulation_activity_object, s_EmulationActivity_method_reportMessage, message_jstr);
  else
    env->CallVoidMethod(m_java_object, s_AndroidHostInterface_method_reportMessage, message_jstr);
}

std::string AndroidHostInterface::GetStringSettingValue(const char* section, const char* key, const char* default_value)
{
  return m_settings_interface.GetStringValue(section, key, default_value);
}

bool AndroidHostInterface::GetBoolSettingValue(const char* section, const char* key, bool default_value /* = false */)
{
  return m_settings_interface.GetBoolValue(section, key, default_value);
}

int AndroidHostInterface::GetIntSettingValue(const char* section, const char* key, int default_value /* = 0 */)
{
  return m_settings_interface.GetIntValue(section, key, default_value);
}

float AndroidHostInterface::GetFloatSettingValue(const char* section, const char* key, float default_value /* = 0.0f */)
{
  return m_settings_interface.GetFloatValue(section, key, default_value);
}

void AndroidHostInterface::SetUserDirectory()
{
  // Already set in constructor.
  Assert(!m_user_directory.empty());
}

void AndroidHostInterface::LoadSettings()
{
  LoadAndConvertSettings();
  CommonHostInterface::FixIncompatibleSettings(false);
  CommonHostInterface::UpdateInputMap(m_settings_interface);
}

void AndroidHostInterface::LoadAndConvertSettings()
{
  CommonHostInterface::LoadSettings(m_settings_interface);

  const std::string msaa_str = m_settings_interface.GetStringValue("GPU", "MSAA", "1");
  g_settings.gpu_multisamples = std::max<u32>(StringUtil::FromChars<u32>(msaa_str).value_or(1), 1);
  g_settings.gpu_per_sample_shading = StringUtil::EndsWith(msaa_str, "-ssaa");

  // turn percentage into fraction for overclock
  const u32 overclock_percent = static_cast<u32>(std::max(m_settings_interface.GetIntValue("CPU", "Overclock", 100), 1));
  Settings::CPUOverclockPercentToFraction(overclock_percent, &g_settings.cpu_overclock_numerator,
                                          &g_settings.cpu_overclock_denominator);
  g_settings.cpu_overclock_enable = (overclock_percent != 100);
  g_settings.UpdateOverclockActive();
}

void AndroidHostInterface::UpdateInputMap()
{
  CommonHostInterface::UpdateInputMap(m_settings_interface);
}

bool AndroidHostInterface::IsEmulationThreadPaused() const
{
  return System::IsValid() && System::IsPaused();
}

bool AndroidHostInterface::StartEmulationThread(jobject emulation_activity, ANativeWindow* initial_surface,
                                                SystemBootParameters boot_params, bool resume_state)
{
  Assert(!IsEmulationThreadRunning());

  emulation_activity = AndroidHelpers::GetJNIEnv()->NewGlobalRef(emulation_activity);

  Log_DevPrintf("Starting emulation thread...");
  m_emulation_thread_stop_request.store(false);
  m_emulation_thread = std::thread(&AndroidHostInterface::EmulationThreadEntryPoint, this, emulation_activity,
                                   initial_surface, std::move(boot_params), resume_state);
  return true;
}

void AndroidHostInterface::PauseEmulationThread(bool paused)
{
  Assert(IsEmulationThreadRunning());
  RunOnEmulationThread([this, paused]() { PauseSystem(paused); });
}

void AndroidHostInterface::StopEmulationThread()
{
  if (!IsEmulationThreadRunning())
    return;

  Log_InfoPrint("Stopping emulation thread...");
  {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_emulation_thread_stop_request.store(true);
    m_sleep_cv.notify_one();
  }
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

void AndroidHostInterface::EmulationThreadEntryPoint(jobject emulation_activity, ANativeWindow* initial_surface,
                                                     SystemBootParameters boot_params, bool resume_state)
{
  JNIEnv* thread_env;
  if (s_jvm->AttachCurrentThread(&thread_env, nullptr) != JNI_OK)
  {
    ReportError("Failed to attach JNI to thread");
    return;
  }

  CreateImGuiContext();
  m_surface = initial_surface;
  m_emulation_activity_object = emulation_activity;
  ApplySettings(true);

  // Boot system.
  bool boot_result = false;
  if (resume_state)
  {
    if (boot_params.filename.empty())
      boot_result = ResumeSystemFromMostRecentState();
    else
      boot_result = ResumeSystemFromState(boot_params.filename.c_str(), true);
  }
  else
  {
    boot_result = BootSystem(boot_params);
  }

  if (!boot_result)
  {
    ReportFormattedError("Failed to boot system on emulation thread (file:%s).", boot_params.filename.c_str());
    DestroyImGuiContext();
    thread_env->CallVoidMethod(m_emulation_activity_object, s_EmulationActivity_method_onEmulationStopped);
    thread_env->DeleteGlobalRef(m_emulation_activity_object);
    m_emulation_activity_object = {};
    s_jvm->DetachCurrentThread();
    return;
  }

  // System is ready to go.
  thread_env->CallVoidMethod(m_emulation_activity_object, s_EmulationActivity_method_onEmulationStarted);
  EmulationThreadLoop();

  thread_env->CallVoidMethod(m_emulation_activity_object, s_EmulationActivity_method_onEmulationStopped);
  PowerOffSystem();
  DestroyImGuiContext();
  thread_env->DeleteGlobalRef(m_emulation_activity_object);
  m_emulation_activity_object = {};
  s_jvm->DetachCurrentThread();
}

void AndroidHostInterface::EmulationThreadLoop()
{
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
          }
          while (!m_callback_queue.empty());
          m_callbacks_outstanding.store(false);
        }

        if (m_emulation_thread_stop_request.load())
          return;

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

    // simulate the system if not paused
    if (System::IsRunning())
      System::RunFrame();

    // rendering
    {
      DrawImGuiWindows();

      m_display->Render();
      ImGui::NewFrame();

      if (System::IsRunning())
      {
        System::UpdatePerformanceCounters();

        if (m_speed_limiter_enabled)
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

  std::unique_ptr<HostDisplay> display;
  switch (g_settings.gpu_renderer)
  {
    case GPURenderer::HardwareVulkan:
      display = std::make_unique<FrontendCommon::VulkanHostDisplay>();
      break;

    case GPURenderer::HardwareOpenGL:
    default:
      display = std::make_unique<FrontendCommon::OpenGLHostDisplay>();
      break;
  }

  if (!display->CreateRenderDevice(wi, {}, g_settings.gpu_use_debug_device) ||
      !display->InitializeRenderDevice(GetShaderCacheBasePath(), g_settings.gpu_use_debug_device))
  {
    ReportError("Failed to acquire host display.");
    display->DestroyRenderDevice();
    return false;
  }

  m_display = std::move(display);

  if (!CreateHostDisplayResources())
  {
    ReportError("Failed to create host display resources");
    ReleaseHostDisplay();
    return false;
  }

  ImGui::NewFrame();
  return true;
}

void AndroidHostInterface::ReleaseHostDisplay()
{
  ReleaseHostDisplayResources();
  m_display->DestroyRenderDevice();
  m_display.reset();
}

std::unique_ptr<AudioStream> AndroidHostInterface::CreateAudioStream(AudioBackend backend)
{
#ifdef USE_OPENSLES
  if (backend == AudioBackend::OpenSLES)
    return OpenSLESAudioStream::Create();
#endif

  return CommonHostInterface::CreateAudioStream(backend);
}

void AndroidHostInterface::OnSystemDestroyed()
{
  CommonHostInterface::OnSystemDestroyed();
  ClearOSDMessages();
}

void AndroidHostInterface::OnRunningGameChanged()
{
  CommonHostInterface::OnRunningGameChanged();

  if (m_emulation_activity_object)
  {
    JNIEnv* env = AndroidHelpers::GetJNIEnv();
    jstring title_string = env->NewStringUTF(System::GetRunningTitle().c_str());
    env->CallVoidMethod(m_emulation_activity_object, s_EmulationActivity_method_onGameTitleChanged, title_string);
  }
}

void AndroidHostInterface::SurfaceChanged(ANativeWindow* surface, int format, int width, int height)
{
  Log_InfoPrintf("SurfaceChanged %p %d %d %d", surface, format, width, height);
  if (m_surface == surface)
  {
    if (m_display)
      m_display->ResizeRenderWindow(width, height);

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

    m_display->ChangeRenderWindow(wi);

    if (surface && System::GetState() == System::State::Paused)
      System::SetState(System::State::Running);
    else if (!surface && System::IsRunning())
      System::SetState(System::State::Paused);
  }
}

void AndroidHostInterface::CreateImGuiContext()
{
  ImGui::CreateContext();

  const float framebuffer_scale = 2.0f;

  auto& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.DisplayFramebufferScale.x = framebuffer_scale;
  io.DisplayFramebufferScale.y = framebuffer_scale;
  ImGui::GetStyle().ScaleAllSizes(framebuffer_scale);

  ImGui::StyleColorsDarker();
  ImGui::AddRobotoRegularFont(15.0f * framebuffer_scale);
}

void AndroidHostInterface::DestroyImGuiContext()
{
  ImGui::DestroyContext();
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

void AndroidHostInterface::SetFastForwardEnabled(bool enabled)
{
  m_fast_forward_enabled = enabled;
  UpdateSpeedLimiterState();
}

void AndroidHostInterface::RefreshGameList(bool invalidate_cache, bool invalidate_database,
                                           ProgressCallback* progress_callback)
{
  m_game_list->SetSearchDirectoriesFromSettings(m_settings_interface);
  m_game_list->Refresh(invalidate_cache, invalidate_database, progress_callback);
}

void AndroidHostInterface::ApplySettings(bool display_osd_messages)
{
  Settings old_settings = std::move(g_settings);
  LoadAndConvertSettings();
  CommonHostInterface::FixIncompatibleSettings(display_osd_messages);

  // Defer renderer changes, the app really doesn't like it.
  if (System::IsValid() && g_settings.gpu_renderer != old_settings.gpu_renderer)
  {
    AddFormattedOSDMessage(5.0f,
                           TranslateString("OSDMessage", "Change to %s GPU renderer will take effect on restart."),
                           Settings::GetRendererName(g_settings.gpu_renderer));
    g_settings.gpu_renderer = old_settings.gpu_renderer;
  }

  CheckForSettingsChanges(old_settings);
}

extern "C" jint JNI_OnLoad(JavaVM* vm, void* reserved)
{
  Log::SetDebugOutputParams(true, nullptr, LOGLEVEL_DEV);
  s_jvm = vm;

  // Create global reference so it doesn't get cleaned up.
  JNIEnv* env = AndroidHelpers::GetJNIEnv();
  if ((s_AndroidHostInterface_class = env->FindClass("com/github/stenzek/duckstation/AndroidHostInterface")) ==
        nullptr ||
      (s_AndroidHostInterface_class = static_cast<jclass>(env->NewGlobalRef(s_AndroidHostInterface_class))) ==
        nullptr ||
      (s_CheatCode_class = env->FindClass("com/github/stenzek/duckstation/CheatCode")) == nullptr ||
      (s_CheatCode_class = static_cast<jclass>(env->NewGlobalRef(s_CheatCode_class))) == nullptr)
  {
    Log_ErrorPrint("AndroidHostInterface class lookup failed");
    return -1;
  }

  jclass emulation_activity_class;
  if ((s_AndroidHostInterface_constructor =
         env->GetMethodID(s_AndroidHostInterface_class, "<init>", "(Landroid/content/Context;)V")) == nullptr ||
      (s_AndroidHostInterface_field_mNativePointer =
         env->GetFieldID(s_AndroidHostInterface_class, "mNativePointer", "J")) == nullptr ||
      (s_AndroidHostInterface_method_reportError =
         env->GetMethodID(s_AndroidHostInterface_class, "reportError", "(Ljava/lang/String;)V")) == nullptr ||
      (s_AndroidHostInterface_method_reportMessage =
         env->GetMethodID(s_AndroidHostInterface_class, "reportMessage", "(Ljava/lang/String;)V")) == nullptr ||
      (emulation_activity_class = env->FindClass("com/github/stenzek/duckstation/EmulationActivity")) == nullptr ||
      (s_EmulationActivity_method_reportError =
         env->GetMethodID(emulation_activity_class, "reportError", "(Ljava/lang/String;)V")) == nullptr ||
      (s_EmulationActivity_method_reportMessage =
         env->GetMethodID(emulation_activity_class, "reportMessage", "(Ljava/lang/String;)V")) == nullptr ||
      (s_EmulationActivity_method_onEmulationStarted =
         env->GetMethodID(emulation_activity_class, "onEmulationStarted", "()V")) == nullptr ||
      (s_EmulationActivity_method_onEmulationStopped =
         env->GetMethodID(emulation_activity_class, "onEmulationStopped", "()V")) == nullptr ||
      (s_EmulationActivity_method_onGameTitleChanged =
         env->GetMethodID(emulation_activity_class, "onGameTitleChanged", "(Ljava/lang/String;)V")) == nullptr ||
      (s_CheatCode_constructor = env->GetMethodID(s_CheatCode_class, "<init>", "(ILjava/lang/String;Z)V")) == nullptr)
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

DEFINE_JNI_ARGS_METHOD(jstring, AndroidHostInterface_getScmVersion, jobject unused)
{
  return env->NewStringUTF(g_scm_tag_str);
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

DEFINE_JNI_ARGS_METHOD(jboolean, AndroidHostInterface_startEmulationThread, jobject obj, jobject emulationActivity,
                       jobject surface, jstring filename, jboolean resume_state, jstring state_filename)
{
  ANativeWindow* native_surface = ANativeWindow_fromSurface(env, surface);
  if (!native_surface)
  {
    Log_ErrorPrint("ANativeWindow_fromSurface() returned null");
    return false;
  }

  std::string state_filename_str = AndroidHelpers::JStringToString(env, state_filename);

  SystemBootParameters boot_params;
  boot_params.filename = AndroidHelpers::JStringToString(env, filename);

  return AndroidHelpers::GetNativeClass(env, obj)->StartEmulationThread(emulationActivity, native_surface,
                                                                        std::move(boot_params), resume_state);
}

DEFINE_JNI_ARGS_METHOD(void, AndroidHostInterface_stopEmulationThread, jobject obj)
{
  AndroidHelpers::GetNativeClass(env, obj)->StopEmulationThread();
}

DEFINE_JNI_ARGS_METHOD(void, AndroidHostInterface_surfaceChanged, jobject obj, jobject surface, jint format, jint width,
                       jint height)
{
  ANativeWindow* native_surface = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
  if (surface && !native_surface)
    Log_ErrorPrint("ANativeWindow_fromSurface() returned null");

  AndroidHostInterface* hi = AndroidHelpers::GetNativeClass(env, obj);
  hi->RunOnEmulationThread(
    [hi, native_surface, format, width, height]() { hi->SurfaceChanged(native_surface, format, width, height); },
    false);
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

DEFINE_JNI_ARGS_METHOD(jarray, AndroidHostInterface_getGameListEntries, jobject obj)
{
  jclass entry_class = env->FindClass("com/github/stenzek/duckstation/GameListEntry");
  Assert(entry_class != nullptr);

  jmethodID entry_constructor = env->GetMethodID(entry_class, "<init>",
                                                 "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;JLjava/lang/"
                                                 "String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
  Assert(entry_constructor != nullptr);

  AndroidHostInterface* hi = AndroidHelpers::GetNativeClass(env, obj);
  jobjectArray entry_array = env->NewObjectArray(hi->GetGameList()->GetEntryCount(), entry_class, nullptr);
  Assert(entry_array != nullptr);

  u32 counter = 0;
  for (const GameListEntry& entry : hi->GetGameList()->GetEntries())
  {
    const Timestamp modified_ts(
      Timestamp::FromUnixTimestamp(static_cast<Timestamp::UnixTimestampValue>(entry.last_modified_time)));

    jstring path = env->NewStringUTF(entry.path.c_str());
    jstring code = env->NewStringUTF(entry.code.c_str());
    jstring title = env->NewStringUTF(entry.title.c_str());
    jstring region = env->NewStringUTF(DiscRegionToString(entry.region));
    jstring type = env->NewStringUTF(GameList::EntryTypeToString(entry.type));
    jstring compatibility_rating =
      env->NewStringUTF(GameList::EntryCompatibilityRatingToString(entry.compatibility_rating));
    jstring modified_time = env->NewStringUTF(modified_ts.ToString("%Y/%m/%d, %H:%M:%S"));
    jlong size = entry.total_size;

    jobject entry_jobject = env->NewObject(entry_class, entry_constructor, path, code, title, size, modified_time,
                                           region, type, compatibility_rating);

    env->SetObjectArrayElement(entry_array, counter++, entry_jobject);
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
  AndroidHostInterface* hi = AndroidHelpers::GetNativeClass(env, obj);
  hi->RunOnEmulationThread([hi]() { hi->SaveResumeSaveState(); }, wait_for_completion);
}

DEFINE_JNI_ARGS_METHOD(void, AndroidHostInterface_setDisplayAlignment, jobject obj, jint alignment)
{
  AndroidHostInterface* hi = AndroidHelpers::GetNativeClass(env, obj);
  hi->RunOnEmulationThread(
    [hi, alignment]() { hi->GetDisplay()->SetDisplayAlignment(static_cast<HostDisplay::Alignment>(alignment)); });
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

DEFINE_JNI_ARGS_METHOD(jobject, AndroidHostInterface_getCheatList, jobject obj)
{
  if (!System::IsValid() || !System::HasCheatList())
    return nullptr;

  CheatList* cl = System::GetCheatList();
  const u32 count = cl->GetCodeCount();

  jobjectArray arr = env->NewObjectArray(count, s_CheatCode_class, nullptr);
  for (u32 i = 0; i < count; i++)
  {
    const CheatCode& cc = cl->GetCode(i);

    jobject java_cc = env->NewObject(s_CheatCode_class, s_CheatCode_constructor, static_cast<jint>(i),
                                     env->NewStringUTF(cc.description.c_str()), cc.enabled);
    env->SetObjectArrayElement(arr, i, java_cc);
  }

  return arr;
}

DEFINE_JNI_ARGS_METHOD(void, AndroidHostInterface_setCheatEnabled, jobject obj, jint index, jboolean enabled)
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
  return AndroidHelpers::GetNativeClass(env, obj)->IsFastForwardEnabled();
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
