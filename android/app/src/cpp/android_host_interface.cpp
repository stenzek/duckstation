#include "android_host_interface.h"
#include "common/assert.h"
#include "common/audio_stream.h"
#include "common/log.h"
#include "common/string.h"
#include "common/timestamp.h"
#include "core/controller.h"
#include "core/game_list.h"
#include "core/gpu.h"
#include "core/host_display.h"
#include "core/system.h"
#include "frontend-common/imgui_styles.h"
#include "frontend-common/opengl_host_display.h"
#include "frontend-common/vulkan_host_display.h"
#include <android/native_window_jni.h>
#include <cmath>
#include <imgui.h>
Log_SetChannel(AndroidHostInterface);

static JavaVM* s_jvm;
static jclass s_AndroidHostInterface_class;
static jmethodID s_AndroidHostInterface_constructor;
static jfieldID s_AndroidHostInterface_field_nativePointer;

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
    static_cast<uintptr_t>(env->GetLongField(obj, s_AndroidHostInterface_field_nativePointer)));
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
  HostInterface::ReportError(message);
}

void AndroidHostInterface::ReportMessage(const char* message)
{
  HostInterface::ReportMessage(message);
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
  CommonHostInterface::LoadSettings(m_settings_interface);
  CommonHostInterface::FixIncompatibleSettings(false);
  CommonHostInterface::UpdateInputMap(m_settings_interface);
}

void AndroidHostInterface::UpdateInputMap()
{
  CommonHostInterface::UpdateInputMap(m_settings_interface);
}

bool AndroidHostInterface::StartEmulationThread(ANativeWindow* initial_surface, SystemBootParameters boot_params)
{
  Assert(!IsEmulationThreadRunning());

  Log_DevPrintf("Starting emulation thread...");
  m_emulation_thread_stop_request.store(false);
  m_emulation_thread =
    std::thread(&AndroidHostInterface::EmulationThreadEntryPoint, this, initial_surface, std::move(boot_params));
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

void AndroidHostInterface::EmulationThreadEntryPoint(ANativeWindow* initial_surface, SystemBootParameters boot_params)
{
  JNIEnv* thread_env;
  if (s_jvm->AttachCurrentThread(&thread_env, nullptr) != JNI_OK)
  {
    Log_ErrorPrintf("Failed to attach JNI to thread");
    m_emulation_thread_start_result.store(false);
    m_emulation_thread_started.Signal();
    return;
  }

  CreateImGuiContext();
  m_surface = initial_surface;
  ApplySettings();

  // Boot system.
  if (!BootSystem(boot_params))
  {
    Log_ErrorPrintf("Failed to boot system on emulation thread (file:%s).", boot_params.filename.c_str());
    DestroyImGuiContext();
    m_emulation_thread_start_result.store(false);
    m_emulation_thread_started.Signal();
    s_jvm->DetachCurrentThread();
    return;
  }

  // System is ready to go.
  m_emulation_thread_start_result.store(true);
  m_emulation_thread_started.Signal();

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

  DestroySystem();
  DestroyImGuiContext();
  s_jvm->DetachCurrentThread();
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
    return false;
  }

  m_display = std::move(display);
  ImGui::NewFrame();
  return true;
}

void AndroidHostInterface::ReleaseHostDisplay()
{
  m_display->DestroyRenderDevice();
  m_display.reset();
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
    wi.type = WindowInfo::Type::Android;
    wi.window_handle = surface;
    wi.surface_width = width;
    wi.surface_height = height;

    m_display->ChangeRenderWindow(wi);
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
    [this, index, type]() {
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
    [this, index, button_code, pressed]() {
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
    [this, index, button_code, value]() {
      Controller* controller = System::GetController(index);
      if (!controller)
        return;

      controller->SetAxisState(button_code, value);
    },
    false);
}

void AndroidHostInterface::RefreshGameList(bool invalidate_cache, bool invalidate_database)
{
  m_game_list->SetSearchDirectoriesFromSettings(m_settings_interface);
  m_game_list->Refresh(invalidate_cache, invalidate_database);
}

void AndroidHostInterface::ApplySettings()
{
  Settings old_settings = std::move(g_settings);
  CommonHostInterface::LoadSettings(m_settings_interface);
  CheckForSettingsChanges(old_settings);
}

extern "C" jint JNI_OnLoad(JavaVM* vm, void* reserved)
{
  Log::SetDebugOutputParams(true, nullptr, LOGLEVEL_DEV);
  s_jvm = vm;

  JNIEnv* env = AndroidHelpers::GetJNIEnv();
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

DEFINE_JNI_ARGS_METHOD(jobject, AndroidHostInterface_create, jobject unused, jobject context_object, jstring user_directory)
{
  Log::SetDebugOutputParams(true, nullptr, LOGLEVEL_DEBUG);

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
  std::string user_directory_str = AndroidHelpers::JStringToString(env, user_directory);
  AndroidHostInterface* cpp_obj = new AndroidHostInterface(java_obj_ref, context_object, std::move(user_directory_str));
  if (!cpp_obj->Initialize())
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
  return AndroidHelpers::GetNativeClass(env, obj)->IsEmulationThreadRunning();
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

  std::string state_filename_str = AndroidHelpers::JStringToString(env, state_filename);

  SystemBootParameters boot_params;
  boot_params.filename = AndroidHelpers::JStringToString(env, filename);

  return AndroidHelpers::GetNativeClass(env, obj)->StartEmulationThread(native_surface, std::move(boot_params));
}

DEFINE_JNI_ARGS_METHOD(void, AndroidHostInterface_stopEmulationThread, jobject obj)
{
  AndroidHelpers::GetNativeClass(env, obj)->StopEmulationThread();
}

DEFINE_JNI_ARGS_METHOD(void, AndroidHostInterface_surfaceChanged, jobject obj, jobject surface, jint format, jint width,
                       jint height)
{
  ANativeWindow* native_surface = ANativeWindow_fromSurface(env, surface);
  if (!native_surface)
    Log_ErrorPrint("ANativeWindow_fromSurface() returned null");

  AndroidHostInterface* hi = AndroidHelpers::GetNativeClass(env, obj);
  hi->RunOnEmulationThread(
    [hi, native_surface, format, width, height]() { hi->SurfaceChanged(native_surface, format, width, height); }, true);
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
                       jboolean invalidate_database)
{
  AndroidHelpers::GetNativeClass(env, obj)->RefreshGameList(invalidate_cache, invalidate_database);
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
    hi->RunOnEmulationThread([hi]() { hi->ApplySettings(); });
  }
  else
  {
    hi->ApplySettings();
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
