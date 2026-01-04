// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

class Error;

#define VK_NO_PROTOTYPES

#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR

// vulkan.h pulls in windows.h on Windows, so we need to include our replacement header first
#include "common/windows_headers.h"
#elif defined(__APPLE__)
#define VK_USE_PLATFORM_METAL_EXT
#elif defined(__ANDROID__)
#define VK_USE_PLATFORM_ANDROID_KHR
#else
#ifdef ENABLE_X11
#define VK_USE_PLATFORM_XCB_KHR
#endif

#ifdef ENABLE_WAYLAND
#define VK_USE_PLATFORM_WAYLAND_KHR
#endif
#endif

#include "vulkan/vulkan.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VULKAN_MODULE_ENTRY_POINT(name, required) extern PFN_##name name;
#define VULKAN_INSTANCE_ENTRY_POINT(name, required) extern PFN_##name name;
#define VULKAN_DEVICE_ENTRY_POINT(name, required) extern PFN_##name name;
#include "vulkan_entry_points.inl"
#undef VULKAN_DEVICE_ENTRY_POINT
#undef VULKAN_INSTANCE_ENTRY_POINT
#undef VULKAN_MODULE_ENTRY_POINT

#ifdef __cplusplus
}
#endif

// We include vk_mem_alloc globally, so we don't accidentally include it before the vulkan header somewhere.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wunused-function"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif

#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#define VMA_STATS_STRING_ENABLED 0
#include "vulkan/vk_mem_alloc.h"

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif
