// Copyright 2016 Dolphin Emulator Project
// Copyright 2020 DuckStation Emulator Project
// Licensed under GPLv2+
// Refer to the LICENSE file included.

#pragma once

#define VK_NO_PROTOTYPES

#if defined(WIN32)

#define VK_USE_PLATFORM_WIN32_KHR

// vulkan.h pulls in windows.h on Windows, so we need to include our replacement header first
#include "../windows_headers.h"

#endif

#if defined(USE_X11)
#define VK_USE_PLATFORM_XLIB_KHR
#endif

#if defined(USE_WAYLAND)
#define VK_USE_PLATFORM_WAYLAND_KHR
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

#if defined(USE_X11)

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
#ifdef Unsorted
#undef Unsorted
#endif
#ifdef Bool
#undef Bool
#endif

#endif

#include "entry_points.h"

namespace Vulkan {

bool LoadVulkanLibrary();
bool LoadVulkanInstanceFunctions(VkInstance instance);
bool LoadVulkanDeviceFunctions(VkDevice device);
void UnloadVulkanLibrary();
void ResetVulkanLibraryFunctionPointers();

} // namespace Vulkan
