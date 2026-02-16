// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

// No better place for this..
#define VMA_IMPLEMENTATION

#include "vulkan_loader.h"
#include "vulkan_builders.h"
#include "vulkan_device.h"

#include "core/settings.h"

#include "common/assert.h"
#include "common/dynamic_library.h"
#include "common/error.h"
#include "common/log.h"

#ifdef ENABLE_SDL
#include "sdl_video_helpers.h"
#include <SDL3/SDL_vulkan.h>
#endif

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

LOG_CHANNEL(GPUDevice);

extern "C" {

#define VULKAN_MODULE_ENTRY_POINT(name, required) PFN_##name name;
#define VULKAN_INSTANCE_ENTRY_POINT(name, required) PFN_##name name;
#define VULKAN_DEVICE_ENTRY_POINT(name, required) PFN_##name name;
#include "vulkan_entry_points.inl"
#undef VULKAN_DEVICE_ENTRY_POINT
#undef VULKAN_INSTANCE_ENTRY_POINT
#undef VULKAN_MODULE_ENTRY_POINT
}

namespace VulkanLoader {

static bool LoadVulkanLibrary(WindowInfoType wtype, Error* error);
static void ResetModuleFunctions();
static bool LoadInstanceFunctions(VkInstance instance, Error* error);
static void ResetInstanceFunctions();
static void UnloadVulkanLibrary();

#ifdef ENABLE_SDL
static bool LoadVulkanLibraryFromSDL(Error* error);
static void UnloadVulkanLibraryFromSDL();
#endif

static bool LockedCreateVulkanInstance(WindowInfoType wtype, bool* request_debug_instance, Error* error);
static void LockedReleaseVulkanInstance();
static void LockedDestroyVulkanInstance();

static bool SelectInstanceExtensions(VulkanDevice::ExtensionList* extension_list, WindowInfoType wtype,
                                     bool debug_instance, Error* error);

static std::vector<GPUDevice::ExclusiveFullscreenMode> EnumerateFullscreenModes(WindowInfoType wtype);

VKAPI_ATTR static VkBool32 VKAPI_CALL DebugMessengerCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                             VkDebugUtilsMessageTypeFlagsEXT messageType,
                                                             const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
                                                             void* pUserData);

namespace {
struct Locals
{
  DynamicLibrary library;
  VkInstance instance = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT debug_messenger_callback = VK_NULL_HANDLE;
  u32 reference_count = 0;
  OptionalExtensions optional_extensions{};
  WindowInfoType window_type = WindowInfoType::Surfaceless;
  bool is_debug_instance = false;
#ifdef ENABLE_SDL
  bool library_loaded_from_sdl = false;
#endif

  std::mutex mutex;
};

} // namespace

ALIGN_TO_CACHE_LINE static Locals s_locals;

} // namespace VulkanLoader

bool VulkanLoader::LoadVulkanLibrary(WindowInfoType wtype, Error* error)
{
#ifdef ENABLE_SDL
  // Switching to/from SDL?
  if (wtype == WindowInfoType::SDL)
  {
    if (s_locals.library_loaded_from_sdl)
      return true;

    UnloadVulkanLibrary();
    if (!LoadVulkanLibraryFromSDL(error))
      return false;
  }
  else
  {
    // Unload from SDL if we were previously using it.. unlikely.
    if (s_locals.library_loaded_from_sdl)
      UnloadVulkanLibraryFromSDL();
  }
#endif

  if (s_locals.library.IsOpen())
    return true;

#ifdef __APPLE__
  // Check if a path to a specific Vulkan library has been specified.
  char* libvulkan_env = getenv("LIBVULKAN_PATH");
  if (libvulkan_env)
    s_locals.library.Open(libvulkan_env, error);
  if (!s_locals.library.IsOpen() &&
      !s_locals.library.Open(DynamicLibrary::GetVersionedFilename("MoltenVK").c_str(), error))
  {
    return false;
  }
#else
  // try versioned first, then unversioned.
  if (!s_locals.library.Open(DynamicLibrary::GetVersionedFilename("vulkan", 1).c_str(), error) &&
      !s_locals.library.Open(DynamicLibrary::GetVersionedFilename("vulkan").c_str(), error))
  {
    return false;
  }
#endif

  bool required_functions_missing = false;
  const auto load_function = [&error, &required_functions_missing](PFN_vkVoidFunction* func_ptr, const char* name,
                                                                   bool is_required) {
    if (!s_locals.library.GetSymbol(name, func_ptr) && is_required && !required_functions_missing)
    {
      Error::SetStringFmt(error, "Failed to load required module function {}", name);
      required_functions_missing = true;
    }
  };

#define VULKAN_MODULE_ENTRY_POINT(name, required)                                                                      \
  load_function(reinterpret_cast<PFN_vkVoidFunction*>(&name), #name, required);
#include "vulkan_entry_points.inl"
#undef VULKAN_MODULE_ENTRY_POINT

  if (required_functions_missing)
  {
    ResetModuleFunctions();
    s_locals.library.Close();
    return false;
  }

  return true;
}

void VulkanLoader::UnloadVulkanLibrary()
{
#ifdef ENABLE_SDL
  if (s_locals.library_loaded_from_sdl)
  {
    UnloadVulkanLibraryFromSDL();
    return;
  }
#endif

  ResetModuleFunctions();
  s_locals.library.Close();
}

void VulkanLoader::ResetModuleFunctions()
{
#define VULKAN_MODULE_ENTRY_POINT(name, required) name = nullptr;
#include "vulkan_entry_points.inl"
#undef VULKAN_MODULE_ENTRY_POINT
}

#ifdef ENABLE_SDL

bool VulkanLoader::LoadVulkanLibraryFromSDL(Error* error)
{
  if (!SDL_Vulkan_LoadLibrary(nullptr))
  {
    Error::SetStringFmt(error, "SDL_Vulkan_LoadLibrary() failed: {}", SDL_GetError());
    return false;
  }

  vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
  if (!vkGetInstanceProcAddr)
  {
    Error::SetStringFmt(error, "SDL_Vulkan_GetVkGetInstanceProcAddr() failed: {}", SDL_GetError());
    SDL_Vulkan_UnloadLibrary();
    return false;
  }

  bool required_functions_missing = false;
  const auto load_function = [&error, &required_functions_missing](PFN_vkVoidFunction* func_ptr, const char* name,
                                                                   bool is_required) {
    // vkGetInstanceProcAddr() can't resolve itself until Vulkan 1.2.
    if (func_ptr == reinterpret_cast<PFN_vkVoidFunction*>(&vkGetInstanceProcAddr))
      return;

    *func_ptr = vkGetInstanceProcAddr(nullptr, name);
    if (!(*func_ptr) && is_required && !required_functions_missing)
    {
      Error::SetStringFmt(error, "Failed to load required module function {}", name);
      required_functions_missing = true;
    }
  };

#define VULKAN_MODULE_ENTRY_POINT(name, required)                                                                      \
  load_function(reinterpret_cast<PFN_vkVoidFunction*>(&name), #name, required);
#include "vulkan_entry_points.inl"
#undef VULKAN_MODULE_ENTRY_POINT

  if (required_functions_missing)
  {
    ResetModuleFunctions();
    SDL_Vulkan_UnloadLibrary();
    return false;
  }

  s_locals.library_loaded_from_sdl = true;
  return true;
}

void VulkanLoader::UnloadVulkanLibraryFromSDL()
{
  ResetModuleFunctions();
  s_locals.library_loaded_from_sdl = false;
  SDL_Vulkan_UnloadLibrary();
}

#endif // ENABLE_SDL

bool VulkanLoader::LoadInstanceFunctions(VkInstance instance, Error* error)
{
  bool required_functions_missing = false;
  const auto load_function = [&instance, &error, &required_functions_missing](PFN_vkVoidFunction* func_ptr,
                                                                              const char* name, bool is_required) {
    *func_ptr = vkGetInstanceProcAddr(instance, name);
    if (!(*func_ptr) && is_required && !required_functions_missing)
    {
      Error::SetStringFmt(error, "Failed to load required instance function {}", name);
      required_functions_missing = true;
    }
  };

#define VULKAN_INSTANCE_ENTRY_POINT(name, required)                                                                    \
  load_function(reinterpret_cast<PFN_vkVoidFunction*>(&name), #name, required);
#include "vulkan_entry_points.inl"
#undef VULKAN_INSTANCE_ENTRY_POINT

  // we might not have VK_KHR_get_physical_device_properties2...
  if (!vkGetPhysicalDeviceFeatures2 || !vkGetPhysicalDeviceProperties2 || !vkGetPhysicalDeviceMemoryProperties2)
  {
    if (!vkGetPhysicalDeviceFeatures2KHR || !vkGetPhysicalDeviceProperties2KHR ||
        !vkGetPhysicalDeviceMemoryProperties2KHR)
    {
      ERROR_LOG("One or more functions from VK_KHR_get_physical_device_properties2 is missing, disabling extension.");
      s_locals.optional_extensions.vk_khr_get_physical_device_properties2 = false;
      vkGetPhysicalDeviceFeatures2 = nullptr;
      vkGetPhysicalDeviceProperties2 = nullptr;
      vkGetPhysicalDeviceMemoryProperties2 = nullptr;
    }
    else
    {
      vkGetPhysicalDeviceFeatures2 = vkGetPhysicalDeviceFeatures2KHR;
      vkGetPhysicalDeviceProperties2 = vkGetPhysicalDeviceProperties2KHR;
      vkGetPhysicalDeviceMemoryProperties2 = vkGetPhysicalDeviceMemoryProperties2KHR;
    }
  }

  return !required_functions_missing;
}

void VulkanLoader::ResetInstanceFunctions()
{
#define VULKAN_INSTANCE_ENTRY_POINT(name, required) name = nullptr;
#include "vulkan_entry_points.inl"
#undef VULKAN_INSTANCE_ENTRY_POINT
}

bool VulkanLoader::LoadDeviceFunctions(VkDevice device, Error* error)
{
  bool required_functions_missing = false;
  const auto load_function = [&device, &error, &required_functions_missing](PFN_vkVoidFunction* func_ptr,
                                                                            const char* name, bool is_required) {
    *func_ptr = vkGetDeviceProcAddr(device, name);
    if (!(*func_ptr) && is_required && !required_functions_missing)
    {
      Error::SetStringFmt(error, "Failed to load required device function {}", name);
      required_functions_missing = true;
    }
  };

#define VULKAN_DEVICE_ENTRY_POINT(name, required)                                                                      \
  load_function(reinterpret_cast<PFN_vkVoidFunction*>(&name), #name, required);
#include "vulkan_entry_points.inl"
#undef VULKAN_DEVICE_ENTRY_POINT

  if (required_functions_missing)
    return false;

  // Alias for swapchain maintenance.
  if (!vkReleaseSwapchainImagesKHR)
    vkReleaseSwapchainImagesKHR = vkReleaseSwapchainImagesEXT;

  return true;
}

void VulkanLoader::ResetDeviceFunctions()
{
#define VULKAN_DEVICE_ENTRY_POINT(name, required) name = nullptr;
#include "vulkan_entry_points.inl"
#undef VULKAN_DEVICE_ENTRY_POINT
}

bool VulkanLoader::LockedCreateVulkanInstance(WindowInfoType wtype, bool* request_debug_instance, Error* error)
{
  if (s_locals.instance != VK_NULL_HANDLE &&
      ((request_debug_instance && *request_debug_instance != s_locals.is_debug_instance) ||
       s_locals.window_type != wtype))
  {
    // Different debug setting, need to recreate the instance.
    if (s_locals.reference_count > 0)
      ERROR_LOG("Cannot change Vulkan instance window type/debug setting while in use.");
    else
      LockedDestroyVulkanInstance();
  }

  if (s_locals.instance != VK_NULL_HANDLE)
  {
    s_locals.reference_count++;
    DEV_LOG("Using cached Vulkan instance, reference count {}", s_locals.reference_count);
    return s_locals.instance;
  }

  if (!LoadVulkanLibrary(wtype, error))
    return false;

  bool debug_instance = request_debug_instance ? *request_debug_instance : g_settings.gpu_use_debug_device;
  INFO_LOG("Creating new Vulkan instance (debug {}, wtype {})...", debug_instance, static_cast<u32>(wtype));

  VulkanDevice::ExtensionList enabled_extensions;
  if (!SelectInstanceExtensions(&enabled_extensions, wtype, debug_instance, error))
    return false;

  u32 maxApiVersion = VK_API_VERSION_1_0;
  if (vkEnumerateInstanceVersion)
  {
    VkResult res = vkEnumerateInstanceVersion(&maxApiVersion);
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkEnumerateInstanceVersion() failed: ");
      maxApiVersion = VK_API_VERSION_1_0;
    }
  }
  else
  {
    WARNING_LOG("Driver does not provide vkEnumerateInstanceVersion().");
  }

  // Cap out at 1.1 for consistency.
  const u32 apiVersion = std::min(maxApiVersion, VK_API_VERSION_1_1);
  INFO_LOG("Supported instance version: {}.{}.{}, requesting version {}.{}.{}", VK_API_VERSION_MAJOR(maxApiVersion),
           VK_API_VERSION_MINOR(maxApiVersion), VK_API_VERSION_PATCH(maxApiVersion), VK_API_VERSION_MAJOR(apiVersion),
           VK_API_VERSION_MINOR(apiVersion), VK_API_VERSION_PATCH(apiVersion));

  // Remember to manually update this every release. We don't pull in svnrev.h here, because
  // it's only the major/minor version, and rebuilding the file every time something else changes
  // is unnecessary.
  VkApplicationInfo app_info = {};
  app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app_info.pNext = nullptr;
  app_info.pApplicationName = "DuckStation";
  app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
  app_info.pEngineName = "DuckStation";
  app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
  app_info.apiVersion = apiVersion;

  VkInstanceCreateInfo instance_create_info = {};
  instance_create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  instance_create_info.pNext = nullptr;
  instance_create_info.flags = 0;
  instance_create_info.pApplicationInfo = &app_info;
  instance_create_info.enabledExtensionCount = static_cast<uint32_t>(enabled_extensions.size());
  instance_create_info.ppEnabledExtensionNames = enabled_extensions.data();
  instance_create_info.enabledLayerCount = 0;
  instance_create_info.ppEnabledLayerNames = nullptr;

  // Enable debug layer on debug builds
  if (debug_instance)
  {
    static const char* layer_names[] = {"VK_LAYER_KHRONOS_validation"};
    instance_create_info.enabledLayerCount = 1;
    instance_create_info.ppEnabledLayerNames = layer_names;
  }

  DebugAssert(s_locals.instance == VK_NULL_HANDLE && s_locals.reference_count == 0);
  VkResult res = vkCreateInstance(&instance_create_info, nullptr, &s_locals.instance);
  if (res != VK_SUCCESS)
  {
    // If creation failed, try without the debug flag.
    if (debug_instance)
    {
      LOG_VULKAN_ERROR(res, "vkCreateInstance() failed, trying without debug layers: ");
      debug_instance = false;
      if (SelectInstanceExtensions(&enabled_extensions, wtype, false, error))
      {
        instance_create_info.enabledExtensionCount = static_cast<uint32_t>(enabled_extensions.size());
        instance_create_info.ppEnabledExtensionNames = enabled_extensions.data();
        instance_create_info.enabledLayerCount = 0;
        instance_create_info.ppEnabledLayerNames = nullptr;
        res = vkCreateInstance(&instance_create_info, nullptr, &s_locals.instance);
      }
    }

    if (res != VK_SUCCESS)
    {
      Vulkan::SetErrorObject(error, "vkCreateInstance() failed: ", res);
      return false;
    }
  }

  if (!LoadInstanceFunctions(s_locals.instance, error))
  {
    LockedDestroyVulkanInstance();
    return false;
  }

  DEV_LOG("Created new Vulkan instance.");
  s_locals.reference_count = 1;
  s_locals.window_type = wtype;
  s_locals.is_debug_instance = debug_instance;
  if (request_debug_instance)
    *request_debug_instance = debug_instance;

  // Check for presence of the functions before calling
  if (debug_instance)
  {
    if (vkCreateDebugUtilsMessengerEXT && vkDestroyDebugUtilsMessengerEXT && vkSubmitDebugUtilsMessageEXT)
    {
      const VkDebugUtilsMessengerCreateInfoEXT messenger_info = {
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        nullptr,
        0,
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
          VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT,
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT |
          VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
        DebugMessengerCallback,
        nullptr};

      res =
        vkCreateDebugUtilsMessengerEXT(s_locals.instance, &messenger_info, nullptr, &s_locals.debug_messenger_callback);
      if (res != VK_SUCCESS)
        LOG_VULKAN_ERROR(res, "vkCreateDebugUtilsMessengerEXT failed: ");
    }
    else
    {
      WARNING_LOG("Vulkan: Debug messenger requested, but functions are not available.");
    }
  }

  return true;
}

bool VulkanLoader::SelectInstanceExtensions(VulkanDevice::ExtensionList* extension_list, WindowInfoType wtype,
                                            bool debug_instance, Error* error)
{
  u32 extension_count = 0;
  VkResult res = vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, nullptr);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkEnumerateInstanceExtensionProperties failed: ");
    return false;
  }

  if (extension_count == 0)
  {
    ERROR_LOG("Vulkan: No extensions supported by instance.");
    return false;
  }

  std::vector<VkExtensionProperties> available_extension_list(extension_count);
  res = vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, available_extension_list.data());
  DebugAssert(res == VK_SUCCESS);

  const auto SupportsExtension = [&available_extension_list, &extension_list](const char* name, bool required) {
    if (std::find_if(available_extension_list.begin(), available_extension_list.end(),
                     [&](const VkExtensionProperties& properties) {
                       return (std::strcmp(name, properties.extensionName) == 0);
                     }) != available_extension_list.end())
    {
      DEV_LOG("Enabling extension: {}", name);
      extension_list->push_back(name);
      return true;
    }

    if (required)
      ERROR_LOG("Vulkan: Missing required extension {}.", name);

    return false;
  };

#if defined(VK_USE_PLATFORM_WIN32_KHR)
  if (wtype == WindowInfoType::Win32 && (!SupportsExtension(VK_KHR_SURFACE_EXTENSION_NAME, true) ||
                                         !SupportsExtension(VK_KHR_WIN32_SURFACE_EXTENSION_NAME, true)))
  {
    return false;
  }
#endif
#if defined(VK_USE_PLATFORM_XCB_KHR)
  if (wtype == WindowInfoType::XCB && (!SupportsExtension(VK_KHR_SURFACE_EXTENSION_NAME, true) ||
                                       !SupportsExtension(VK_KHR_XCB_SURFACE_EXTENSION_NAME, true)))
  {
    return false;
  }
#endif
#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
  if (wtype == WindowInfoType::Wayland && (!SupportsExtension(VK_KHR_SURFACE_EXTENSION_NAME, true) ||
                                           !SupportsExtension(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME, true)))
  {
    return false;
  }
#endif
#if defined(VK_USE_PLATFORM_METAL_EXT)
  if (wtype == WindowInfoType::MacOS && (!SupportsExtension(VK_KHR_SURFACE_EXTENSION_NAME, true) ||
                                         !SupportsExtension(VK_EXT_METAL_SURFACE_EXTENSION_NAME, true)))
  {
    return false;
  }
#endif
#if defined(VK_USE_PLATFORM_ANDROID_KHR)
  if (wtype == WindowInfoType::Android && (!SupportsExtension(VK_KHR_SURFACE_EXTENSION_NAME, true) ||
                                           !SupportsExtension(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME, true)))
  {
    return false;
  }
#endif

#if defined(ENABLE_SDL)
  if (wtype == WindowInfoType::SDL)
  {
    Uint32 sdl_extension_count = 0;
    const char* const* sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&sdl_extension_count);
    if (!sdl_extensions)
    {
      ERROR_LOG("SDL_Vulkan_GetInstanceExtensions() failed: {}", SDL_GetError());
      return false;
    }

    for (unsigned int i = 0; i < sdl_extension_count; i++)
    {
      if (!SupportsExtension(sdl_extensions[i], true))
        return false;
    }
  }
#endif

  // VK_EXT_debug_utils
  if (debug_instance && !SupportsExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME, false))
    WARNING_LOG("Vulkan: Debug report requested, but extension is not available.");

  // Needed for exclusive fullscreen control.
  SupportsExtension(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, false);

  s_locals.optional_extensions.vk_khr_get_surface_capabilities2 =
    (wtype != WindowInfoType::Surfaceless &&
     SupportsExtension(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, false));
  s_locals.optional_extensions.vk_khr_surface_maintenance1 =
    (wtype != WindowInfoType::Surfaceless && (SupportsExtension(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME, false) ||
                                              SupportsExtension(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME, false)));
  s_locals.optional_extensions.vk_khr_get_physical_device_properties2 =
    SupportsExtension(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, false);

#define LOG_EXT(name, field)                                                                                           \
  GENERIC_LOG(___LogChannel___, Log::Level::Info,                                                                      \
              s_locals.optional_extensions.field ? Log::Color::StrongGreen : Log::Color::StrongOrange, name " is {}",  \
              s_locals.optional_extensions.field ? "supported" : "NOT supported")

  LOG_EXT("VK_KHR_get_physical_device_properties2", vk_khr_get_physical_device_properties2);
  LOG_EXT("VK_KHR_get_surface_capabilities2", vk_khr_get_surface_capabilities2);
  LOG_EXT("VK_KHR_surface_maintenance1", vk_khr_surface_maintenance1);

#undef LOG_EXT

  return true;
}

void VulkanLoader::LockedReleaseVulkanInstance()
{
  Assert(s_locals.reference_count > 0);
  s_locals.reference_count--;

  // We specifically keep the instance around even after releasing it.
  // Both AMD on Windows and Mesa leak a few tens of megabytes for every instance...
  DEV_LOG("Released Vulkan instance, reference count {}", s_locals.reference_count);

#ifdef ENABLE_SDL
  // SDL Vulkan kinda breaks OpenGL contexts if the instance isn't destroyed...
  if (s_locals.window_type == WindowInfoType::SDL && s_locals.reference_count == 0)
    LockedDestroyVulkanInstance();
#endif
}

void VulkanLoader::LockedDestroyVulkanInstance()
{
  DebugAssert(s_locals.reference_count == 0);
  DebugAssert(s_locals.instance != VK_NULL_HANDLE);

  if (s_locals.debug_messenger_callback != VK_NULL_HANDLE)
  {
    vkDestroyDebugUtilsMessengerEXT(s_locals.instance, s_locals.debug_messenger_callback, nullptr);
    s_locals.debug_messenger_callback = VK_NULL_HANDLE;
  }

  if (vkDestroyInstance)
    vkDestroyInstance(s_locals.instance, nullptr);
  else
    ERROR_LOG("Vulkan instance was leaked because vkDestroyInstance() could not be loaded.");

  s_locals.optional_extensions = {};
  s_locals.instance = VK_NULL_HANDLE;
  ResetInstanceFunctions();
}

bool VulkanLoader::CreateVulkanInstance(WindowInfoType window_type, bool* request_debug_instance, Error* error)
{
  const std::lock_guard lock(s_locals.mutex);
  return LockedCreateVulkanInstance(window_type, request_debug_instance, error);
}

VkInstance VulkanLoader::GetVulkanInstance()
{
  // Doesn't need to be locked, but should have an instance.
  DebugAssert(s_locals.instance != VK_NULL_HANDLE);
  return s_locals.instance;
}

void VulkanLoader::ReleaseVulkanInstance()
{
  const std::lock_guard lock(s_locals.mutex);
  LockedReleaseVulkanInstance();
}

void VulkanLoader::DestroyVulkanInstance()
{
  const std::lock_guard lock(s_locals.mutex);
  if (s_locals.instance != VK_NULL_HANDLE)
    LockedDestroyVulkanInstance();
  UnloadVulkanLibrary();
}

const VulkanLoader::OptionalExtensions& VulkanLoader::GetOptionalExtensions()
{
  return s_locals.optional_extensions;
}

VulkanLoader::GPUList VulkanLoader::EnumerateGPUs(Error* error)
{
  GPUList gpus;

  u32 gpu_count = 0;
  VkResult res = vkEnumeratePhysicalDevices(s_locals.instance, &gpu_count, nullptr);
  if ((res != VK_SUCCESS && res != VK_INCOMPLETE) || gpu_count == 0)
  {
    Vulkan::SetErrorObject(error, "vkEnumeratePhysicalDevices (1) failed: ", res);
    return gpus;
  }

  std::vector<VkPhysicalDevice> physical_devices(gpu_count);
  res = vkEnumeratePhysicalDevices(s_locals.instance, &gpu_count, physical_devices.data());
  if (res == VK_INCOMPLETE)
  {
    WARNING_LOG("First vkEnumeratePhysicalDevices() call returned {} devices, but second returned {}",
                physical_devices.size(), gpu_count);
  }
  else if (res != VK_SUCCESS)
  {
    Vulkan::SetErrorObject(error, "vkEnumeratePhysicalDevices (2) failed: ", res);
    return gpus;
  }

  if (gpu_count == 0)
  {
    Error::SetStringView(error, "No Vulkan physical devices available.");
    return gpus;
  }

  // Maybe we lost a GPU?
  if (gpu_count < physical_devices.size())
    physical_devices.resize(gpu_count);

  gpus.reserve(physical_devices.size());
  for (VkPhysicalDevice device : physical_devices)
  {
    VkPhysicalDeviceProperties2 props = {};
    VkPhysicalDeviceDriverProperties driver_props = {};

    if (vkGetPhysicalDeviceProperties2)
    {
      props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
      driver_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
      Vulkan::AddPointerToChain(&props, &driver_props);
      vkGetPhysicalDeviceProperties2(device, &props);
    }

    // just in case the chained version fails
    vkGetPhysicalDeviceProperties(device, &props.properties);

    VkPhysicalDeviceFeatures available_features = {};
    vkGetPhysicalDeviceFeatures(device, &available_features);

    GPUDevice::AdapterInfo ai;
    ai.name = props.properties.deviceName;
    ai.max_texture_size =
      std::min(props.properties.limits.maxFramebufferWidth, props.properties.limits.maxImageDimension2D);
    ai.max_multisamples = Vulkan::GetMaxMultisamples(device, props.properties);
    ai.driver_type = GuessDriverType(props.properties, driver_props);
    ai.supports_sample_shading = available_features.sampleRateShading;

    // handle duplicate adapter names
    if (std::any_of(gpus.begin(), gpus.end(), [&ai](const auto& other) { return (ai.name == other.second.name); }))
    {
      std::string original_adapter_name = std::move(ai.name);

      u32 current_extra = 2;
      do
      {
        ai.name = fmt::format("{} ({})", original_adapter_name, current_extra);
        current_extra++;
      } while (
        std::any_of(gpus.begin(), gpus.end(), [&ai](const auto& other) { return (ai.name == other.second.name); }));
    }

    gpus.emplace_back(device, std::move(ai));
  }

  return gpus;
}

std::vector<GPUDevice::ExclusiveFullscreenMode> VulkanLoader::EnumerateFullscreenModes(WindowInfoType wtype)
{
#ifdef ENABLE_SDL
  if (wtype == WindowInfoType::SDL)
    return SDLVideoHelpers::GetFullscreenModeList();
#endif

  return {};
}

bool VulkanLoader::IsSuitableDefaultRenderer(WindowInfoType window_type)
{
#ifdef __ANDROID__
  // No way in hell.
  return false;
#else
  const std::optional<GPUDevice::AdapterInfoList> adapter_list = GetAdapterList(window_type, nullptr);
  if (!adapter_list.has_value() || adapter_list->empty())
  {
    // No adapters, not gonna be able to use VK.
    return false;
  }

  // Check the first GPU, should be enough.
  const GPUDevice::AdapterInfo& ainfo = adapter_list->front();
  INFO_LOG("Using Vulkan GPU '{}' for automatic renderer check.", ainfo.name);

  // Any software rendering (LLVMpipe, SwiftShader).
  if ((static_cast<u16>(ainfo.driver_type) & static_cast<u16>(GPUDriverType::SoftwareFlag)) ==
      static_cast<u32>(GPUDriverType::SoftwareFlag))
  {
    INFO_LOG("Not using Vulkan for software renderer.");
    return false;
  }

#ifdef __linux__
  // Intel Ivy Bridge/Haswell/Broadwell drivers are incomplete.
  if (ainfo.driver_type == GPUDriverType::IntelMesa &&
      (ainfo.name.find("Ivy Bridge") != std::string::npos || ainfo.name.find("Haswell") != std::string::npos ||
       ainfo.name.find("Broadwell") != std::string::npos || ainfo.name.find("(IVB") != std::string::npos ||
       ainfo.name.find("(HSW") != std::string::npos || ainfo.name.find("(BDW") != std::string::npos))
  {
    INFO_LOG("Not using Vulkan for Intel GPU with incomplete driver.");
    return false;
  }
#endif

#if defined(__linux__) || defined(__ANDROID__)
  // V3D is buggy, image copies with larger textures are broken.
  if (ainfo.driver_type == GPUDriverType::BroadcomMesa)
  {
    INFO_LOG("Not using Vulkan for V3D GPU with buggy driver.");
    return false;
  }
#endif

  INFO_LOG("Allowing Vulkan as default renderer.");
  return true;
#endif
}

GPUDriverType VulkanLoader::GuessDriverType(const VkPhysicalDeviceProperties& device_properties,
                                            const VkPhysicalDeviceDriverProperties& driver_properties)
{
  static constexpr const std::pair<VkDriverId, GPUDriverType> table[] = {
    {VK_DRIVER_ID_NVIDIA_PROPRIETARY, GPUDriverType::NVIDIAProprietary},
    {VK_DRIVER_ID_AMD_PROPRIETARY, GPUDriverType::AMDProprietary},
    {VK_DRIVER_ID_AMD_OPEN_SOURCE, GPUDriverType::AMDProprietary},
    {VK_DRIVER_ID_MESA_RADV, GPUDriverType::AMDMesa},
    {VK_DRIVER_ID_NVIDIA_PROPRIETARY, GPUDriverType::NVIDIAProprietary},
    {VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS, GPUDriverType::IntelProprietary},
    {VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA, GPUDriverType::IntelMesa},
    {VK_DRIVER_ID_IMAGINATION_PROPRIETARY, GPUDriverType::ImaginationProprietary},
    {VK_DRIVER_ID_QUALCOMM_PROPRIETARY, GPUDriverType::QualcommProprietary},
    {VK_DRIVER_ID_ARM_PROPRIETARY, GPUDriverType::ARMProprietary},
    {VK_DRIVER_ID_GOOGLE_SWIFTSHADER, GPUDriverType::SwiftShader},
    {VK_DRIVER_ID_GGP_PROPRIETARY, GPUDriverType::Unknown},
    {VK_DRIVER_ID_BROADCOM_PROPRIETARY, GPUDriverType::BroadcomProprietary},
    {VK_DRIVER_ID_MESA_LLVMPIPE, GPUDriverType::LLVMPipe},
    {VK_DRIVER_ID_MOLTENVK, GPUDriverType::AppleProprietary},
    {VK_DRIVER_ID_COREAVI_PROPRIETARY, GPUDriverType::Unknown},
    {VK_DRIVER_ID_JUICE_PROPRIETARY, GPUDriverType::Unknown},
    {VK_DRIVER_ID_VERISILICON_PROPRIETARY, GPUDriverType::Unknown},
    {VK_DRIVER_ID_MESA_TURNIP, GPUDriverType::QualcommMesa},
    {VK_DRIVER_ID_MESA_V3DV, GPUDriverType::BroadcomMesa},
    {VK_DRIVER_ID_MESA_PANVK, GPUDriverType::ARMMesa},
    {VK_DRIVER_ID_SAMSUNG_PROPRIETARY, GPUDriverType::AMDProprietary},
    {VK_DRIVER_ID_MESA_VENUS, GPUDriverType::Unknown},
    {VK_DRIVER_ID_MESA_DOZEN, GPUDriverType::DozenMesa},
    {VK_DRIVER_ID_MESA_NVK, GPUDriverType::NVIDIAMesa},
    {VK_DRIVER_ID_IMAGINATION_OPEN_SOURCE_MESA, GPUDriverType::ImaginationMesa},
    {VK_DRIVER_ID_MESA_HONEYKRISP, GPUDriverType::AppleMesa},
  };

  const auto iter = std::find_if(std::begin(table), std::end(table), [&driver_properties](const auto& it) {
    return (driver_properties.driverID == it.first);
  });
  if (iter != std::end(table))
    return iter->second;

  return GPUDevice::GuessDriverType(
    device_properties.vendorID, {},
    std::string_view(device_properties.deviceName,
                     StringUtil::Strnlen(device_properties.deviceName, std::size(device_properties.deviceName))));
}

std::optional<GPUDevice::AdapterInfoList> VulkanLoader::GetAdapterList(WindowInfoType window_type, Error* error)
{
  std::optional<GPUDevice::AdapterInfoList> ret;
  std::vector<GPUDevice::ExclusiveFullscreenMode> fullscreen_modes;
  GPUList gpus;
  {
    const std::lock_guard lock(s_locals.mutex);

    // Prefer re-using the instance if we can to avoid expensive loading.
    if (s_locals.instance != VK_NULL_HANDLE)
    {
      gpus = EnumerateGPUs(error);
      fullscreen_modes = EnumerateFullscreenModes(window_type);
    }
    else
    {
      // Otherwise we need to create a temporary instance.
      // Hold the lock for both creation and querying, otherwise the UI thread could race creation.
      if (!LockedCreateVulkanInstance(window_type, nullptr, error))
        return ret;

      gpus = EnumerateGPUs(error);
      fullscreen_modes = EnumerateFullscreenModes(window_type);

      LockedReleaseVulkanInstance();
    }
  }

  ret.emplace();
  ret->reserve(gpus.size());
  for (size_t i = 0; i < gpus.size(); i++)
  {
    GPUDevice::AdapterInfo& ai = gpus[i].second;

    // splat fullscreen modes across gpus
    if (i == (gpus.size() - 1))
      ai.fullscreen_modes = std::move(fullscreen_modes);
    else
      ai.fullscreen_modes = fullscreen_modes;

    ret->push_back(std::move(ai));
  }

  return ret;
}

VkBool32 VulkanLoader::DebugMessengerCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT messageType,
                                              const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
                                              void* pUserData)
{
  if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
  {
    ERROR_LOG("Vulkan debug report: ({}) {}", pCallbackData->pMessageIdName ? pCallbackData->pMessageIdName : "",
              pCallbackData->pMessage);
  }
  else if (severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT))
  {
    WARNING_LOG("Vulkan debug report: ({}) {}", pCallbackData->pMessageIdName ? pCallbackData->pMessageIdName : "",
                pCallbackData->pMessage);
  }
  else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)
  {
    INFO_LOG("Vulkan debug report: ({}) {}", pCallbackData->pMessageIdName ? pCallbackData->pMessageIdName : "",
             pCallbackData->pMessage);
  }
  else
  {
    DEV_LOG("Vulkan debug report: ({}) {}", pCallbackData->pMessageIdName ? pCallbackData->pMessageIdName : "",
            pCallbackData->pMessage);
  }

  return VK_FALSE;
}
