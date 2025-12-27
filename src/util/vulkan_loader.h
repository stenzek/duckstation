// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "gpu_device.h"
#include "vulkan_headers.h"
#include "window_info.h"

#include "common/types.h"

#include <optional>
#include <utility>
#include <vector>

namespace VulkanLoader {

/// @brief List of Vulkan-compatible GPUs and associated adapter information.
using GPUList = std::vector<std::pair<VkPhysicalDevice, GPUDevice::AdapterInfo>>;

/// @brief Optional extensions for an instance.
struct OptionalExtensions
{
  bool vk_khr_get_surface_capabilities2 : 1;
  bool vk_khr_get_physical_device_properties2 : 1;
  bool vk_khr_surface_maintenance1 : 1;
};

/// Creates the shared Vulkan instance. If debug_instance is changed, the instance will be recreated.
/// @param window_type Window type for selecting required extensions.
/// @param request_debug_instance Set to true if a debug instance is requested. May be modified to reflect actual state.
/// @param error Error information if the instance could not be created.
/// @return The Vulkan instance, or VK_NULL_HANDLE on failure.
bool CreateVulkanInstance(WindowInfoType window_type, bool* request_debug_instance, Error* error);

/// Returns the shared Vulkan instance.
VkInstance GetVulkanInstance();

/// Releases the shared Vulkan instance.
void ReleaseVulkanInstance();

/// Destroys the instance, if any, and unloads the Vulkan library.
void DestroyVulkanInstance();

/// Returns optional extensions for the current instance.
const OptionalExtensions& GetOptionalExtensions();

/// Enumerates Vulkan devices.
GPUList EnumerateGPUs(Error* error);

/// Safely creates the instance and returns a list of adapters and associated information.
std::optional<GPUDevice::AdapterInfoList> GetAdapterList(WindowInfoType window_type, Error* error);

/// Returns true if Vulkan is suitable as a default for the devices in the system.
bool IsSuitableDefaultRenderer(WindowInfoType window_type);

/// Loads Vulkan device-level functions for the given device.
/// @param device The Vulkan device to load functions for.
bool LoadDeviceFunctions(VkDevice device, Error* error);

/// Releases Vulkan device-level functions.
void ResetDeviceFunctions();

/// @brief Guesses the GPU driver type based on device and driver properties.
GPUDriverType GuessDriverType(const VkPhysicalDeviceProperties& device_properties,
                              const VkPhysicalDeviceDriverProperties& driver_properties);

} // namespace VulkanLoader
