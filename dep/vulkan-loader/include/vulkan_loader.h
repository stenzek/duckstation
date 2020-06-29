// Copyright 2016 Dolphin Emulator Project
// Copyright 2020 DuckStation Emulator Project
// Licensed under GPLv2+
// Refer to the LICENSE file included.

#pragma once

#define VK_NO_PROTOTYPES

#if defined(WIN32)

#define VK_USE_PLATFORM_WIN32_KHR

// vulkan.h pulls in windows.h on Windows, so we need to include our replacement header first
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#ifndef NOMINMAX
#define NOMINMAX 1
#endif

// require vista+
#ifdef _WIN32_WINNT
#undef _WIN32_WINNT
#endif
#define _WIN32_WINNT _WIN32_WINNT_VISTA

#include <windows.h>

#endif

#if defined(VULKAN_USE_X11)
#define VK_USE_PLATFORM_XLIB_KHR
#endif

#if defined(ANDROID)
#define VK_USE_PLATFORM_ANDROID_KHR
#endif

#if defined(__APPLE__)
// #define VK_USE_PLATFORM_MACOS_MVK
#define VK_USE_PLATFORM_METAL_EXT
#endif

#include "vulkan/vulkan.h"

// Currently, exclusive fullscreen is only supported on Windows.
#if defined(WIN32)
#define SUPPORTS_VULKAN_EXCLUSIVE_FULLSCREEN 1
#endif

#if defined(VULKAN_USE_X11)

// This breaks a bunch of our code. They shouldn't be #defines in the first place.
#ifdef None
#undef None
#endif
#ifdef Status
#undef Status
#endif
#ifdef CursorShape
#undef CursorShape
#endif
#ifdef KeyPress
#undef KeyPress
#endif
#ifdef KeyRelease
#undef KeyRelease
#endif
#ifdef FocusIn
#undef FocusIn
#endif
#ifdef FocusOut
#undef FocusOut
#endif
#ifdef FontChange
#undef FontChange
#endif
#ifdef Expose
#undef Expose
#endif

#endif

// We abuse the preprocessor here to only need to specify function names once.
#define VULKAN_MODULE_ENTRY_POINT(name, required) extern PFN_##name name;
#define VULKAN_INSTANCE_ENTRY_POINT(name, required) extern PFN_##name name;
#define VULKAN_DEVICE_ENTRY_POINT(name, required) extern PFN_##name name;
#include "vulkan_entry_points.inl"
#undef VULKAN_DEVICE_ENTRY_POINT
#undef VULKAN_INSTANCE_ENTRY_POINT
#undef VULKAN_MODULE_ENTRY_POINT

namespace Vulkan {

bool LoadVulkanLibrary();
bool LoadVulkanInstanceFunctions(VkInstance instance);
bool LoadVulkanDeviceFunctions(VkDevice device);
void UnloadVulkanLibrary();

} // namespace Vulkan
