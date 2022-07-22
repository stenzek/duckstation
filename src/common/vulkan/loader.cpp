// Copyright 2016 Dolphin Emulator Project
// Copyright 2020 DuckStation Emulator Project
// Licensed under GPLv2+
// Refer to the LICENSE file included.

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "loader.h"

#ifndef _WIN32
#include <dlfcn.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#define VULKAN_MODULE_ENTRY_POINT(name, required) PFN_##name ds_##name;
#define VULKAN_INSTANCE_ENTRY_POINT(name, required) PFN_##name ds_##name;
#define VULKAN_DEVICE_ENTRY_POINT(name, required) PFN_##name ds_##name;
#include "entry_points.inl"
#undef VULKAN_DEVICE_ENTRY_POINT
#undef VULKAN_INSTANCE_ENTRY_POINT
#undef VULKAN_MODULE_ENTRY_POINT

namespace Vulkan {

void ResetVulkanLibraryFunctionPointers()
{
#define VULKAN_MODULE_ENTRY_POINT(name, required) ds_##name = nullptr;
#define VULKAN_INSTANCE_ENTRY_POINT(name, required) ds_##name = nullptr;
#define VULKAN_DEVICE_ENTRY_POINT(name, required) ds_##name = nullptr;
#include "entry_points.inl"
#undef VULKAN_DEVICE_ENTRY_POINT
#undef VULKAN_INSTANCE_ENTRY_POINT
#undef VULKAN_MODULE_ENTRY_POINT
}

#if defined(_WIN32)

static HMODULE vulkan_module;
static std::atomic_int vulkan_module_ref_count = {0};

bool LoadVulkanLibrary()
{
  // Not thread safe if a second thread calls the loader whilst the first is still in-progress.
  if (vulkan_module)
  {
    vulkan_module_ref_count++;
    return true;
  }

#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP | WINAPI_PARTITION_SYSTEM | WINAPI_PARTITION_GAMES)
  vulkan_module = LoadLibraryA("vulkan-1.dll");
#else
  vulkan_module = NULL;
#endif
  if (!vulkan_module)
  {
    std::fprintf(stderr, "Failed to load vulkan-1.dll\n");
    return false;
  }

  bool required_functions_missing = false;
  auto LoadFunction = [&](FARPROC* func_ptr, const char* name, bool is_required) {
    *func_ptr = GetProcAddress(vulkan_module, name);
    if (!(*func_ptr) && is_required)
    {
      std::fprintf(stderr, "Vulkan: Failed to load required module function %s\n", name);
      required_functions_missing = true;
    }
  };

#define VULKAN_MODULE_ENTRY_POINT(name, required) LoadFunction(reinterpret_cast<FARPROC*>(&name), #name, required);
#include "entry_points.inl"
#undef VULKAN_MODULE_ENTRY_POINT

  if (required_functions_missing)
  {
    ResetVulkanLibraryFunctionPointers();
    FreeLibrary(vulkan_module);
    vulkan_module = nullptr;
    return false;
  }

  vulkan_module_ref_count++;
  return true;
}

void UnloadVulkanLibrary()
{
  if ((--vulkan_module_ref_count) > 0)
    return;

  ResetVulkanLibraryFunctionPointers();
  FreeLibrary(vulkan_module);
  vulkan_module = nullptr;
}

#else

static void* vulkan_module;
static std::atomic_int vulkan_module_ref_count = {0};

bool LoadVulkanLibrary()
{
  // Not thread safe if a second thread calls the loader whilst the first is still in-progress.
  if (vulkan_module)
  {
    vulkan_module_ref_count++;
    return true;
  }

#if defined(__APPLE__)
  // Check if a path to a specific Vulkan library has been specified.
  char* libvulkan_env = getenv("LIBVULKAN_PATH");
  if (libvulkan_env)
    vulkan_module = dlopen(libvulkan_env, RTLD_NOW);
  if (!vulkan_module)
  {
    unsigned path_size = 0;
    _NSGetExecutablePath(nullptr, &path_size);
    std::string path;
    path.resize(path_size);
    if (_NSGetExecutablePath(path.data(), &path_size) == 0)
    {
      path[path_size] = 0;

      size_t pos = path.rfind('/');
      if (pos != std::string::npos)
      {
        path.erase(pos);
        path += "/../Frameworks/libvulkan.dylib";
        vulkan_module = dlopen(path.c_str(), RTLD_NOW);
      }
    }
  }
  if (!vulkan_module)
    vulkan_module = dlopen("libvulkan.dylib", RTLD_NOW);
#else
  // Names of libraries to search. Desktop should use libvulkan.so.1 or libvulkan.so.
  static const char* search_lib_names[] = {"libvulkan.so.1", "libvulkan.so"};
  for (size_t i = 0; i < sizeof(search_lib_names) / sizeof(search_lib_names[0]); i++)
  {
    vulkan_module = dlopen(search_lib_names[i], RTLD_NOW);
    if (vulkan_module)
      break;
  }
#endif

  if (!vulkan_module)
  {
    std::fprintf(stderr, "Failed to load or locate libvulkan.so\n");
    return false;
  }

  bool required_functions_missing = false;
  auto LoadFunction = [&](void** func_ptr, const char* name, bool is_required) {
    *func_ptr = dlsym(vulkan_module, name);
    if (!(*func_ptr) && is_required)
    {
      std::fprintf(stderr, "Vulkan: Failed to load required module function %s\n", name);
      required_functions_missing = true;
    }
  };

#define VULKAN_MODULE_ENTRY_POINT(name, required) LoadFunction(reinterpret_cast<void**>(&name), #name, required);
#include "entry_points.inl"
#undef VULKAN_MODULE_ENTRY_POINT

  if (required_functions_missing)
  {
    ResetVulkanLibraryFunctionPointers();
    dlclose(vulkan_module);
    vulkan_module = nullptr;
    return false;
  }

  vulkan_module_ref_count++;
  return true;
}

void UnloadVulkanLibrary()
{
  if ((--vulkan_module_ref_count) > 0)
    return;

  ResetVulkanLibraryFunctionPointers();
  dlclose(vulkan_module);
  vulkan_module = nullptr;
}

#endif

bool LoadVulkanInstanceFunctions(VkInstance instance)
{
  bool required_functions_missing = false;
  auto LoadFunction = [&](PFN_vkVoidFunction* func_ptr, const char* name, bool is_required) {
    *func_ptr = vkGetInstanceProcAddr(instance, name);
    if (!(*func_ptr) && is_required)
    {
      std::fprintf(stderr, "Vulkan: Failed to load required instance function %s\n", name);
      required_functions_missing = true;
    }
  };

#define VULKAN_INSTANCE_ENTRY_POINT(name, required)                                                                    \
  LoadFunction(reinterpret_cast<PFN_vkVoidFunction*>(&name), #name, required);
#include "entry_points.inl"
#undef VULKAN_INSTANCE_ENTRY_POINT

  return !required_functions_missing;
}

bool LoadVulkanDeviceFunctions(VkDevice device)
{
  bool required_functions_missing = false;
  auto LoadFunction = [&](PFN_vkVoidFunction* func_ptr, const char* name, bool is_required) {
    *func_ptr = vkGetDeviceProcAddr(device, name);
    if (!(*func_ptr) && is_required)
    {
      std::fprintf(stderr, "Vulkan: Failed to load required device function %s\n", name);
      required_functions_missing = true;
    }
  };

#define VULKAN_DEVICE_ENTRY_POINT(name, required)                                                                      \
  LoadFunction(reinterpret_cast<PFN_vkVoidFunction*>(&name), #name, required);
#include "entry_points.inl"
#undef VULKAN_DEVICE_ENTRY_POINT

  return !required_functions_missing;
}

} // namespace Vulkan
