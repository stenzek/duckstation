// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

// No better place for this..
#define VMA_IMPLEMENTATION

#include "vulkan_loader.h"

#include "common/assert.h"
#include "common/log.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifndef _WIN32
#include <dlfcn.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

Log_SetChannel(VulkanDevice);

extern "C" {

#define VULKAN_MODULE_ENTRY_POINT(name, required) PFN_##name name;
#define VULKAN_INSTANCE_ENTRY_POINT(name, required) PFN_##name name;
#define VULKAN_DEVICE_ENTRY_POINT(name, required) PFN_##name name;
#include "vulkan_entry_points.inl"
#undef VULKAN_DEVICE_ENTRY_POINT
#undef VULKAN_INSTANCE_ENTRY_POINT
#undef VULKAN_MODULE_ENTRY_POINT
}

void Vulkan::ResetVulkanLibraryFunctionPointers()
{
#define VULKAN_MODULE_ENTRY_POINT(name, required) name = nullptr;
#define VULKAN_INSTANCE_ENTRY_POINT(name, required) name = nullptr;
#define VULKAN_DEVICE_ENTRY_POINT(name, required) name = nullptr;
#include "vulkan_entry_points.inl"
#undef VULKAN_DEVICE_ENTRY_POINT
#undef VULKAN_INSTANCE_ENTRY_POINT
#undef VULKAN_MODULE_ENTRY_POINT
}

#if defined(_WIN32)

static HMODULE s_vulkan_module;

bool Vulkan::IsVulkanLibraryLoaded()
{
  return s_vulkan_module != NULL;
}

bool Vulkan::LoadVulkanLibrary()
{
  AssertMsg(!s_vulkan_module, "Vulkan module is not loaded.");

  s_vulkan_module = LoadLibraryA("vulkan-1.dll");
  if (!s_vulkan_module)
  {
    Log_ErrorPrintf("Failed to load vulkan-1.dll");
    return false;
  }

  bool required_functions_missing = false;
  auto LoadFunction = [&](FARPROC* func_ptr, const char* name, bool is_required) {
    *func_ptr = GetProcAddress(s_vulkan_module, name);
    if (!(*func_ptr) && is_required)
    {
      Log_ErrorPrintf("Vulkan: Failed to load required module function %s", name);
      required_functions_missing = true;
    }
  };

#define VULKAN_MODULE_ENTRY_POINT(name, required) LoadFunction(reinterpret_cast<FARPROC*>(&name), #name, required);
#include "vulkan_entry_points.inl"
#undef VULKAN_MODULE_ENTRY_POINT

  if (required_functions_missing)
  {
    ResetVulkanLibraryFunctionPointers();
    FreeLibrary(s_vulkan_module);
    s_vulkan_module = nullptr;
    return false;
  }

  return true;
}

void Vulkan::UnloadVulkanLibrary()
{
  ResetVulkanLibraryFunctionPointers();
  if (s_vulkan_module)
    FreeLibrary(s_vulkan_module);
  s_vulkan_module = nullptr;
}

#else

static void* s_vulkan_module;

bool Vulkan::IsVulkanLibraryLoaded()
{
  return s_vulkan_module != nullptr;
}

bool Vulkan::LoadVulkanLibrary()
{
  AssertMsg(!s_vulkan_module, "Vulkan module is not loaded.");

#if defined(__APPLE__)
  // Check if a path to a specific Vulkan library has been specified.
  char* libvulkan_env = getenv("LIBVULKAN_PATH");
  if (libvulkan_env)
    s_vulkan_module = dlopen(libvulkan_env, RTLD_NOW);
  if (!s_vulkan_module)
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
        path += "/../Frameworks/libMoltenVK.dylib";
        s_vulkan_module = dlopen(path.c_str(), RTLD_NOW);
      }
    }
  }
  if (!s_vulkan_module)
    s_vulkan_module = dlopen("libvulkan.dylib", RTLD_NOW);
#else
  // Names of libraries to search. Desktop should use libvulkan.so.1 or libvulkan.so.
  static const char* search_lib_names[] = {"libvulkan.so.1", "libvulkan.so"};
  for (size_t i = 0; i < sizeof(search_lib_names) / sizeof(search_lib_names[0]); i++)
  {
    s_vulkan_module = dlopen(search_lib_names[i], RTLD_NOW);
    if (s_vulkan_module)
      break;
  }
#endif

  if (!s_vulkan_module)
  {
    Log_ErrorPrintf("Failed to load or locate libvulkan.so");
    return false;
  }

  bool required_functions_missing = false;
  auto LoadFunction = [&](void** func_ptr, const char* name, bool is_required) {
    *func_ptr = dlsym(s_vulkan_module, name);
    if (!(*func_ptr) && is_required)
    {
      Log_ErrorPrintf("Vulkan: Failed to load required module function %s", name);
      required_functions_missing = true;
    }
  };

#define VULKAN_MODULE_ENTRY_POINT(name, required) LoadFunction(reinterpret_cast<void**>(&name), #name, required);
#include "vulkan_entry_points.inl"
#undef VULKAN_MODULE_ENTRY_POINT

  if (required_functions_missing)
  {
    ResetVulkanLibraryFunctionPointers();
    dlclose(s_vulkan_module);
    s_vulkan_module = nullptr;
    return false;
  }

  return true;
}

void Vulkan::UnloadVulkanLibrary()
{
  ResetVulkanLibraryFunctionPointers();
  if (s_vulkan_module)
    dlclose(s_vulkan_module);
  s_vulkan_module = nullptr;
}

#endif

bool Vulkan::LoadVulkanInstanceFunctions(VkInstance instance)
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
#include "vulkan_entry_points.inl"
#undef VULKAN_INSTANCE_ENTRY_POINT

  return !required_functions_missing;
}

bool Vulkan::LoadVulkanDeviceFunctions(VkDevice device)
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
#include "vulkan_entry_points.inl"
#undef VULKAN_DEVICE_ENTRY_POINT

  return !required_functions_missing;
}
