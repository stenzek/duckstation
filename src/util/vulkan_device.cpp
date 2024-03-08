// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "vulkan_device.h"
#include "vulkan_builders.h"
#include "vulkan_pipeline.h"
#include "vulkan_stream_buffer.h"
#include "vulkan_swap_chain.h"
#include "vulkan_texture.h"

#include "core/host.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/bitutils.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/scoped_guard.h"
#include "common/small_string.h"

#include "fmt/format.h"
#include "xxhash.h"

#include <limits>
#include <mutex>

Log_SetChannel(VulkanDevice);

// TODO: VK_KHR_display.

#pragma pack(push, 4)
struct VK_PIPELINE_CACHE_HEADER
{
  u32 header_length;
  u32 header_version;
  u32 vendor_id;
  u32 device_id;
  u8 uuid[VK_UUID_SIZE];
};
#pragma pack(pop)

static VkAttachmentLoadOp GetLoadOpForTexture(const GPUTexture* tex)
{
  static constexpr VkAttachmentLoadOp ops[3] = {VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                                VK_ATTACHMENT_LOAD_OP_DONT_CARE};
  return ops[static_cast<u8>(tex->GetState())];
}

// Tweakables
enum : u32
{
  MAX_DRAW_CALLS_PER_FRAME = 2048,
  MAX_COMBINED_IMAGE_SAMPLER_DESCRIPTORS_PER_FRAME = GPUDevice::MAX_TEXTURE_SAMPLERS * MAX_DRAW_CALLS_PER_FRAME,
  MAX_DESCRIPTOR_SETS_PER_FRAME = MAX_DRAW_CALLS_PER_FRAME,
  MAX_SAMPLER_DESCRIPTORS = 8192,

  VERTEX_BUFFER_SIZE = 32 * 1024 * 1024,
  INDEX_BUFFER_SIZE = 16 * 1024 * 1024,
  VERTEX_UNIFORM_BUFFER_SIZE = 8 * 1024 * 1024,
  FRAGMENT_UNIFORM_BUFFER_SIZE = 8 * 1024 * 1024,
  TEXTURE_BUFFER_SIZE = 64 * 1024 * 1024,

  UNIFORM_PUSH_CONSTANTS_STAGES = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
  UNIFORM_PUSH_CONSTANTS_SIZE = 128,

  MAX_UNIFORM_BUFFER_SIZE = 1024,
};

const std::array<VkFormat, static_cast<u32>(GPUTexture::Format::MaxCount)> VulkanDevice::TEXTURE_FORMAT_MAPPING = {
  VK_FORMAT_UNDEFINED,                // Unknown
  VK_FORMAT_R8G8B8A8_UNORM,           // RGBA8
  VK_FORMAT_B8G8R8A8_UNORM,           // BGRA8
  VK_FORMAT_R5G6B5_UNORM_PACK16,      // RGB565
  VK_FORMAT_R5G5B5A1_UNORM_PACK16,    // RGBA5551
  VK_FORMAT_R8_UNORM,                 // R8
  VK_FORMAT_D16_UNORM,                // D16
  VK_FORMAT_R16_UNORM,                // R16
  VK_FORMAT_R16_SINT,                 // R16I
  VK_FORMAT_R16_UINT,                 // R16U
  VK_FORMAT_R16_SFLOAT,               // R16F
  VK_FORMAT_R32_SINT,                 // R32I
  VK_FORMAT_R32_UINT,                 // R32U
  VK_FORMAT_R32_SFLOAT,               // R32F
  VK_FORMAT_R8G8_UNORM,               // RG8
  VK_FORMAT_R16G16_UNORM,             // RG16
  VK_FORMAT_R16G16_SFLOAT,            // RG16F
  VK_FORMAT_R32G32_SFLOAT,            // RG32F
  VK_FORMAT_R16G16B16A16_UNORM,       // RGBA16
  VK_FORMAT_R16G16B16A16_SFLOAT,      // RGBA16F
  VK_FORMAT_R32G32B32A32_SFLOAT,      // RGBA32F
  VK_FORMAT_A2R10G10B10_UNORM_PACK32, // RGB10A2
};

static constexpr VkClearValue s_present_clear_color = {{{0.0f, 0.0f, 0.0f, 1.0f}}};

// Handles are always 64-bit, even on 32-bit platforms.
static const VkRenderPass DYNAMIC_RENDERING_RENDER_PASS = ((VkRenderPass) static_cast<s64>(-1LL));

#ifdef _DEBUG
static u32 s_debug_scope_depth = 0;
#endif

// We need to synchronize instance creation because of adapter enumeration from the UI thread.
static std::mutex s_instance_mutex;

VulkanDevice::VulkanDevice()
{
#ifdef _DEBUG
  s_debug_scope_depth = 0;
#endif
}

VulkanDevice::~VulkanDevice()
{
  Assert(m_device == VK_NULL_HANDLE);
}

GPUTexture::Format VulkanDevice::GetFormatForVkFormat(VkFormat format)
{
  for (u32 i = 0; i < static_cast<u32>(std::size(TEXTURE_FORMAT_MAPPING)); i++)
  {
    if (TEXTURE_FORMAT_MAPPING[i] == format)
      return static_cast<GPUTexture::Format>(i);
  }

  return GPUTexture::Format::Unknown;
}

VkInstance VulkanDevice::CreateVulkanInstance(const WindowInfo& wi, OptionalExtensions* oe, bool enable_debug_utils,
                                              bool enable_validation_layer)
{
  ExtensionList enabled_extensions;
  if (!SelectInstanceExtensions(&enabled_extensions, wi, oe, enable_debug_utils))
    return VK_NULL_HANDLE;

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
    Log_WarningPrint("Driver does not provide vkEnumerateInstanceVersion().");
  }

  // Cap out at 1.1 for consistency.
  const u32 apiVersion = std::min(maxApiVersion, VK_API_VERSION_1_1);
  Log_InfoFmt("Supported instance version: {}.{}.{}, requesting version {}.{}.{}", VK_API_VERSION_MAJOR(maxApiVersion),
              VK_API_VERSION_MINOR(maxApiVersion), VK_API_VERSION_PATCH(maxApiVersion),
              VK_API_VERSION_MAJOR(apiVersion), VK_API_VERSION_MINOR(apiVersion), VK_API_VERSION_PATCH(apiVersion));

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
  if (enable_validation_layer)
  {
    static const char* layer_names[] = {"VK_LAYER_KHRONOS_validation"};
    instance_create_info.enabledLayerCount = 1;
    instance_create_info.ppEnabledLayerNames = layer_names;
  }

  VkInstance instance;
  VkResult res = vkCreateInstance(&instance_create_info, nullptr, &instance);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateInstance failed: ");
    return nullptr;
  }

  return instance;
}

bool VulkanDevice::SelectInstanceExtensions(ExtensionList* extension_list, const WindowInfo& wi, OptionalExtensions* oe,
                                            bool enable_debug_utils)
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
    Log_ErrorPrintf("Vulkan: No extensions supported by instance.");
    return false;
  }

  std::vector<VkExtensionProperties> available_extension_list(extension_count);
  res = vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, available_extension_list.data());
  DebugAssert(res == VK_SUCCESS);

  auto SupportsExtension = [&](const char* name, bool required) {
    if (std::find_if(available_extension_list.begin(), available_extension_list.end(),
                     [&](const VkExtensionProperties& properties) {
                       return !strcmp(name, properties.extensionName);
                     }) != available_extension_list.end())
    {
      Log_DevPrintf("Enabling extension: %s", name);
      extension_list->push_back(name);
      return true;
    }

    if (required)
      Log_ErrorPrintf("Vulkan: Missing required extension %s.", name);

    return false;
  };

  // Common extensions
  if (wi.type != WindowInfo::Type::Surfaceless && !SupportsExtension(VK_KHR_SURFACE_EXTENSION_NAME, true))
    return false;

#if defined(VK_USE_PLATFORM_WIN32_KHR)
  if (wi.type == WindowInfo::Type::Win32 && !SupportsExtension(VK_KHR_WIN32_SURFACE_EXTENSION_NAME, true))
    return false;
#endif
#if defined(VK_USE_PLATFORM_XLIB_KHR)
  if (wi.type == WindowInfo::Type::X11 && !SupportsExtension(VK_KHR_XLIB_SURFACE_EXTENSION_NAME, true))
    return false;
#endif
#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
  if (wi.type == WindowInfo::Type::Wayland && !SupportsExtension(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME, true))
    return false;
#endif
#if defined(VK_USE_PLATFORM_METAL_EXT)
  if (wi.type == WindowInfo::Type::MacOS && !SupportsExtension(VK_EXT_METAL_SURFACE_EXTENSION_NAME, true))
    return false;
#endif
#if defined(VK_USE_PLATFORM_ANDROID_KHR)
  if (wi.type == WindowInfo::Type::Android && !SupportsExtension(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME, true))
    return false;
#endif

  // VK_EXT_debug_utils
  if (enable_debug_utils && !SupportsExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME, false))
    Log_WarningPrintf("Vulkan: Debug report requested, but extension is not available.");

  // Needed for exclusive fullscreen control.
  SupportsExtension(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, false);

  oe->vk_khr_get_physical_device_properties2 =
    SupportsExtension(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, false);

  return true;
}

VulkanDevice::GPUList VulkanDevice::EnumerateGPUs(VkInstance instance)
{
  GPUList gpus;

  u32 gpu_count = 0;
  VkResult res = vkEnumeratePhysicalDevices(instance, &gpu_count, nullptr);
  if ((res != VK_SUCCESS && res != VK_INCOMPLETE) || gpu_count == 0)
  {
    LOG_VULKAN_ERROR(res, "vkEnumeratePhysicalDevices (1) failed: ");
    return gpus;
  }

  std::vector<VkPhysicalDevice> physical_devices(gpu_count);
  res = vkEnumeratePhysicalDevices(instance, &gpu_count, physical_devices.data());
  if (res == VK_INCOMPLETE)
  {
    Log_WarningFmt("First vkEnumeratePhysicalDevices() call returned {} devices, but second returned {}",
                   physical_devices.size(), gpu_count);
  }
  else if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkEnumeratePhysicalDevices (2) failed: ");
    return gpus;
  }

  // Maybe we lost a GPU?
  if (gpu_count < physical_devices.size())
    physical_devices.resize(gpu_count);

  gpus.reserve(physical_devices.size());
  for (VkPhysicalDevice device : physical_devices)
  {
    VkPhysicalDeviceProperties props = {};
    vkGetPhysicalDeviceProperties(device, &props);

    std::string gpu_name = props.deviceName;

    // handle duplicate adapter names
    if (std::any_of(gpus.begin(), gpus.end(), [&gpu_name](const auto& other) { return (gpu_name == other.second); }))
    {
      std::string original_adapter_name = std::move(gpu_name);

      u32 current_extra = 2;
      do
      {
        gpu_name = fmt::format("{} ({})", original_adapter_name, current_extra);
        current_extra++;
      } while (
        std::any_of(gpus.begin(), gpus.end(), [&gpu_name](const auto& other) { return (gpu_name == other.second); }));
    }

    gpus.emplace_back(device, std::move(gpu_name));
  }

  return gpus;
}

bool VulkanDevice::SelectDeviceExtensions(ExtensionList* extension_list, bool enable_surface)
{
  u32 extension_count = 0;
  VkResult res = vkEnumerateDeviceExtensionProperties(m_physical_device, nullptr, &extension_count, nullptr);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkEnumerateDeviceExtensionProperties failed: ");
    return false;
  }

  if (extension_count == 0)
  {
    Log_ErrorPrintf("Vulkan: No extensions supported by device.");
    return false;
  }

  std::vector<VkExtensionProperties> available_extension_list(extension_count);
  res =
    vkEnumerateDeviceExtensionProperties(m_physical_device, nullptr, &extension_count, available_extension_list.data());
  DebugAssert(res == VK_SUCCESS);

  auto SupportsExtension = [&](const char* name, bool required) {
    if (std::find_if(available_extension_list.begin(), available_extension_list.end(),
                     [&](const VkExtensionProperties& properties) {
                       return !strcmp(name, properties.extensionName);
                     }) != available_extension_list.end())
    {
      if (std::none_of(extension_list->begin(), extension_list->end(),
                       [&](const char* existing_name) { return (std::strcmp(existing_name, name) == 0); }))
      {
        Log_DevPrintf("Enabling extension: %s", name);
        extension_list->push_back(name);
      }

      return true;
    }

    if (required)
      Log_ErrorPrintf("Vulkan: Missing required extension %s.", name);

    return false;
  };

  if (enable_surface && !SupportsExtension(VK_KHR_SWAPCHAIN_EXTENSION_NAME, true))
    return false;

  m_optional_extensions.vk_ext_memory_budget = SupportsExtension(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME, false);
  m_optional_extensions.vk_ext_rasterization_order_attachment_access =
    SupportsExtension(VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME, false) ||
    SupportsExtension(VK_ARM_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME, false);
  m_optional_extensions.vk_ext_attachment_feedback_loop_layout =
    SupportsExtension(VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME, false);
  m_optional_extensions.vk_khr_get_memory_requirements2 =
    SupportsExtension(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, false);
  m_optional_extensions.vk_khr_bind_memory2 = SupportsExtension(VK_KHR_BIND_MEMORY_2_EXTENSION_NAME, false);
  m_optional_extensions.vk_khr_dedicated_allocation =
    SupportsExtension(VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME, false);
  m_optional_extensions.vk_khr_driver_properties = SupportsExtension(VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME, false);
  m_optional_extensions.vk_khr_dynamic_rendering =
    SupportsExtension(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME, false) &&
    SupportsExtension(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME, false) &&
    SupportsExtension(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, false);
  m_optional_extensions.vk_khr_push_descriptor = SupportsExtension(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, false);
  m_optional_extensions.vk_ext_external_memory_host =
    SupportsExtension(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME, false);

#ifdef _WIN32
  m_optional_extensions.vk_ext_full_screen_exclusive =
    enable_surface && SupportsExtension(VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME, false);
  Log_InfoPrintf("VK_EXT_full_screen_exclusive is %s",
                 m_optional_extensions.vk_ext_full_screen_exclusive ? "supported" : "NOT supported");
#endif

  return true;
}

bool VulkanDevice::SelectDeviceFeatures()
{
  VkPhysicalDeviceFeatures available_features;
  vkGetPhysicalDeviceFeatures(m_physical_device, &available_features);

  // Enable the features we use.
  m_device_features.dualSrcBlend = available_features.dualSrcBlend;
  m_device_features.largePoints = available_features.largePoints;
  m_device_features.wideLines = available_features.wideLines;
  m_device_features.samplerAnisotropy = available_features.samplerAnisotropy;
  m_device_features.sampleRateShading = available_features.sampleRateShading;
  m_device_features.geometryShader = available_features.geometryShader;

  return true;
}

bool VulkanDevice::CreateDevice(VkSurfaceKHR surface, bool enable_validation_layer)
{
  u32 queue_family_count;
  vkGetPhysicalDeviceQueueFamilyProperties(m_physical_device, &queue_family_count, nullptr);
  if (queue_family_count == 0)
  {
    Log_ErrorPrintf("No queue families found on specified vulkan physical device.");
    return false;
  }

  std::vector<VkQueueFamilyProperties> queue_family_properties(queue_family_count);
  vkGetPhysicalDeviceQueueFamilyProperties(m_physical_device, &queue_family_count, queue_family_properties.data());
  Log_DevPrintf("%u vulkan queue families", queue_family_count);

  // Find graphics and present queues.
  m_graphics_queue_family_index = queue_family_count;
  m_present_queue_family_index = queue_family_count;
  for (uint32_t i = 0; i < queue_family_count; i++)
  {
    VkBool32 graphics_supported = queue_family_properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT;
    if (graphics_supported)
    {
      m_graphics_queue_family_index = i;
      // Quit now, no need for a present queue.
      if (!surface)
      {
        break;
      }
    }

    if (surface)
    {
      VkBool32 present_supported;
      VkResult res = vkGetPhysicalDeviceSurfaceSupportKHR(m_physical_device, i, surface, &present_supported);
      if (res != VK_SUCCESS)
      {
        LOG_VULKAN_ERROR(res, "vkGetPhysicalDeviceSurfaceSupportKHR failed: ");
        return false;
      }

      if (present_supported)
      {
        m_present_queue_family_index = i;
      }

      // Prefer one queue family index that does both graphics and present.
      if (graphics_supported && present_supported)
      {
        break;
      }
    }
  }
  if (m_graphics_queue_family_index == queue_family_count)
  {
    Log_ErrorPrintf("Vulkan: Failed to find an acceptable graphics queue.");
    return false;
  }
  if (surface != VK_NULL_HANDLE && m_present_queue_family_index == queue_family_count)
  {
    Log_ErrorPrintf("Vulkan: Failed to find an acceptable present queue.");
    return false;
  }

  VkDeviceCreateInfo device_info = {};
  device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  device_info.pNext = nullptr;
  device_info.flags = 0;
  device_info.queueCreateInfoCount = 0;

  static constexpr float queue_priorities[] = {1.0f};
  std::array<VkDeviceQueueCreateInfo, 2> queue_infos;
  VkDeviceQueueCreateInfo& graphics_queue_info = queue_infos[device_info.queueCreateInfoCount++];
  graphics_queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  graphics_queue_info.pNext = nullptr;
  graphics_queue_info.flags = 0;
  graphics_queue_info.queueFamilyIndex = m_graphics_queue_family_index;
  graphics_queue_info.queueCount = 1;
  graphics_queue_info.pQueuePriorities = queue_priorities;

  if (surface != VK_NULL_HANDLE && m_graphics_queue_family_index != m_present_queue_family_index)
  {
    VkDeviceQueueCreateInfo& present_queue_info = queue_infos[device_info.queueCreateInfoCount++];
    present_queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    present_queue_info.pNext = nullptr;
    present_queue_info.flags = 0;
    present_queue_info.queueFamilyIndex = m_present_queue_family_index;
    present_queue_info.queueCount = 1;
    present_queue_info.pQueuePriorities = queue_priorities;
  }

  device_info.pQueueCreateInfos = queue_infos.data();

  ExtensionList enabled_extensions;
  if (!SelectDeviceExtensions(&enabled_extensions, surface != VK_NULL_HANDLE))
    return false;

  device_info.enabledExtensionCount = static_cast<uint32_t>(enabled_extensions.size());
  device_info.ppEnabledExtensionNames = enabled_extensions.data();

  // Check for required features before creating.
  if (!SelectDeviceFeatures())
    return false;

  device_info.pEnabledFeatures = &m_device_features;

  // Enable debug layer on debug builds
  if (enable_validation_layer)
  {
    static const char* layer_names[] = {"VK_LAYER_LUNARG_standard_validation"};
    device_info.enabledLayerCount = 1;
    device_info.ppEnabledLayerNames = layer_names;
  }

  VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT rasterization_order_access_feature = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT, nullptr, VK_TRUE, VK_FALSE,
    VK_FALSE};
  VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT attachment_feedback_loop_feature = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT, nullptr, VK_TRUE};
  VkPhysicalDeviceDynamicRenderingFeatures dynamic_rendering_feature = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, nullptr, VK_TRUE};

  if (m_optional_extensions.vk_ext_rasterization_order_attachment_access)
    Vulkan::AddPointerToChain(&device_info, &rasterization_order_access_feature);
  if (m_optional_extensions.vk_ext_attachment_feedback_loop_layout)
    Vulkan::AddPointerToChain(&device_info, &attachment_feedback_loop_feature);
  if (m_optional_extensions.vk_khr_dynamic_rendering)
    Vulkan::AddPointerToChain(&device_info, &dynamic_rendering_feature);

  VkResult res = vkCreateDevice(m_physical_device, &device_info, nullptr, &m_device);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateDevice failed: ");
    return false;
  }

  // With the device created, we can fill the remaining entry points.
  if (!Vulkan::LoadVulkanDeviceFunctions(m_device))
    return false;

  // Grab the graphics and present queues.
  vkGetDeviceQueue(m_device, m_graphics_queue_family_index, 0, &m_graphics_queue);
  if (surface)
    vkGetDeviceQueue(m_device, m_present_queue_family_index, 0, &m_present_queue);

  m_features.gpu_timing = (m_device_properties.limits.timestampComputeAndGraphics != 0 &&
                           queue_family_properties[m_graphics_queue_family_index].timestampValidBits > 0 &&
                           m_device_properties.limits.timestampPeriod > 0);
  Log_DevPrintf("GPU timing is %s (TS=%u TS valid bits=%u, TS period=%f)",
                m_features.gpu_timing ? "supported" : "not supported",
                static_cast<u32>(m_device_properties.limits.timestampComputeAndGraphics),
                queue_family_properties[m_graphics_queue_family_index].timestampValidBits,
                m_device_properties.limits.timestampPeriod);

  ProcessDeviceExtensions();
  return true;
}

void VulkanDevice::ProcessDeviceExtensions()
{
  // advanced feature checks
  VkPhysicalDeviceFeatures2 features2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, nullptr, {}};
  VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT rasterization_order_access_feature = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT, nullptr, VK_FALSE, VK_FALSE,
    VK_FALSE};
  VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT attachment_feedback_loop_feature = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT, nullptr, VK_FALSE};
  VkPhysicalDeviceDynamicRenderingFeatures dynamic_rendering_feature = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, nullptr, VK_FALSE};

  // add in optional feature structs
  if (m_optional_extensions.vk_ext_rasterization_order_attachment_access)
    Vulkan::AddPointerToChain(&features2, &rasterization_order_access_feature);
  if (m_optional_extensions.vk_ext_attachment_feedback_loop_layout)
    Vulkan::AddPointerToChain(&features2, &attachment_feedback_loop_feature);
  if (m_optional_extensions.vk_khr_dynamic_rendering)
    Vulkan::AddPointerToChain(&features2, &dynamic_rendering_feature);

  // we might not have VK_KHR_get_physical_device_properties2...
  if (!vkGetPhysicalDeviceFeatures2 || !vkGetPhysicalDeviceProperties2 || !vkGetPhysicalDeviceMemoryProperties2)
  {
    if (!vkGetPhysicalDeviceFeatures2KHR || !vkGetPhysicalDeviceProperties2KHR ||
        !vkGetPhysicalDeviceMemoryProperties2KHR)
    {
      Log_ErrorPrint(
        "One or more functions from VK_KHR_get_physical_device_properties2 is missing, disabling extension.");
      m_optional_extensions.vk_khr_get_physical_device_properties2 = false;
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

  // don't bother querying if we're not actually looking at any features
  if (vkGetPhysicalDeviceFeatures2 && features2.pNext)
    vkGetPhysicalDeviceFeatures2(m_physical_device, &features2);

  // confirm we actually support it
  m_optional_extensions.vk_ext_rasterization_order_attachment_access &=
    (rasterization_order_access_feature.rasterizationOrderColorAttachmentAccess == VK_TRUE);
  m_optional_extensions.vk_ext_attachment_feedback_loop_layout &=
    (attachment_feedback_loop_feature.attachmentFeedbackLoopLayout == VK_TRUE);
  m_optional_extensions.vk_khr_dynamic_rendering &= (dynamic_rendering_feature.dynamicRendering == VK_TRUE);

  VkPhysicalDeviceProperties2 properties2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, nullptr, {}};
  VkPhysicalDevicePushDescriptorPropertiesKHR push_descriptor_properties = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR, nullptr, 0u};
  VkPhysicalDeviceExternalMemoryHostPropertiesEXT external_memory_host_properties = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT, nullptr, 0};

  if (m_optional_extensions.vk_khr_driver_properties)
  {
    m_device_driver_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    Vulkan::AddPointerToChain(&properties2, &m_device_driver_properties);
  }
  if (m_optional_extensions.vk_khr_push_descriptor)
    Vulkan::AddPointerToChain(&properties2, &push_descriptor_properties);

  if (m_optional_extensions.vk_ext_external_memory_host)
    Vulkan::AddPointerToChain(&properties2, &external_memory_host_properties);

  // don't bother querying if we're not actually looking at any features
  if (vkGetPhysicalDeviceProperties2 && properties2.pNext)
    vkGetPhysicalDeviceProperties2(m_physical_device, &properties2);

  m_optional_extensions.vk_khr_push_descriptor &= (push_descriptor_properties.maxPushDescriptors >= 1);

  // vk_ext_external_memory_host is only used if the import alignment is the same as the system's page size
  m_optional_extensions.vk_ext_external_memory_host &=
    (external_memory_host_properties.minImportedHostPointerAlignment == HOST_PAGE_SIZE);

  if (IsBrokenMobileDriver())
  {
    // Push descriptor is broken on Adreno v502.. don't want to think about dynamic rendending.
    if (m_optional_extensions.vk_khr_dynamic_rendering)
    {
      m_optional_extensions.vk_khr_dynamic_rendering = false;
      Log_WarningPrint("Disabling VK_KHR_dynamic_rendering on broken mobile driver.");
    }
    if (m_optional_extensions.vk_khr_push_descriptor)
    {
      m_optional_extensions.vk_khr_push_descriptor = false;
      Log_WarningPrint("Disabling VK_KHR_push_descriptor on broken mobile driver.");
    }
  }

  Log_InfoPrintf("VK_EXT_memory_budget is %s",
                 m_optional_extensions.vk_ext_memory_budget ? "supported" : "NOT supported");
  Log_InfoPrintf("VK_EXT_rasterization_order_attachment_access is %s",
                 m_optional_extensions.vk_ext_rasterization_order_attachment_access ? "supported" : "NOT supported");
  Log_InfoPrintf("VK_EXT_attachment_feedback_loop_layout is %s",
                 m_optional_extensions.vk_ext_attachment_feedback_loop_layout ? "supported" : "NOT supported");
  Log_InfoPrintf("VK_KHR_get_memory_requirements2 is %s",
                 m_optional_extensions.vk_khr_get_memory_requirements2 ? "supported" : "NOT supported");
  Log_InfoPrintf("VK_KHR_bind_memory2 is %s",
                 m_optional_extensions.vk_khr_bind_memory2 ? "supported" : "NOT supported");
  Log_InfoPrintf("VK_KHR_get_physical_device_properties2 is %s",
                 m_optional_extensions.vk_khr_get_physical_device_properties2 ? "supported" : "NOT supported");
  Log_InfoPrintf("VK_KHR_dedicated_allocation is %s",
                 m_optional_extensions.vk_khr_dedicated_allocation ? "supported" : "NOT supported");
  Log_InfoPrintf("VK_KHR_dynamic_rendering is %s",
                 m_optional_extensions.vk_khr_dynamic_rendering ? "supported" : "NOT supported");
  Log_InfoPrintf("VK_KHR_push_descriptor is %s",
                 m_optional_extensions.vk_khr_push_descriptor ? "supported" : "NOT supported");
  Log_InfoPrintf("VK_EXT_external_memory_host is %s",
                 m_optional_extensions.vk_ext_external_memory_host ? "supported" : "NOT supported");
}

bool VulkanDevice::CreateAllocator()
{
  const u32 apiVersion = std::min(m_device_properties.apiVersion, VK_API_VERSION_1_1);
  Log_InfoFmt("Supported device API version: {}.{}.{}, using version {}.{}.{} for allocator.",
              VK_API_VERSION_MAJOR(m_device_properties.apiVersion),
              VK_API_VERSION_MINOR(m_device_properties.apiVersion),
              VK_API_VERSION_PATCH(m_device_properties.apiVersion), VK_API_VERSION_MAJOR(apiVersion),
              VK_API_VERSION_MINOR(apiVersion), VK_API_VERSION_PATCH(apiVersion));

  VmaAllocatorCreateInfo ci = {};
  ci.vulkanApiVersion = apiVersion;
  ci.flags = VMA_ALLOCATOR_CREATE_EXTERNALLY_SYNCHRONIZED_BIT;
  ci.physicalDevice = m_physical_device;
  ci.device = m_device;
  ci.instance = m_instance;

  if (apiVersion < VK_API_VERSION_1_1)
  {
    if (m_optional_extensions.vk_khr_get_memory_requirements2 && m_optional_extensions.vk_khr_dedicated_allocation)
    {
      Log_DevPrint("Enabling VMA_ALLOCATOR_CREATE_KHR_DEDICATED_ALLOCATION_BIT on < Vulkan 1.1.");
      ci.flags |= VMA_ALLOCATOR_CREATE_KHR_DEDICATED_ALLOCATION_BIT;
    }
    if (m_optional_extensions.vk_khr_bind_memory2)
    {
      Log_DevPrint("Enabling VMA_ALLOCATOR_CREATE_KHR_BIND_MEMORY2_BIT on < Vulkan 1.1.");
      ci.flags |= VMA_ALLOCATOR_CREATE_KHR_BIND_MEMORY2_BIT;
    }
  }

  if (m_optional_extensions.vk_ext_memory_budget)
  {
    Log_DevPrint("Enabling VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT.");
    ci.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
  }

  // Limit usage of the DEVICE_LOCAL upload heap when we're using a debug device.
  // On NVIDIA drivers, it results in frequently running out of device memory when trying to
  // play back captures in RenderDoc, making life very painful. Re-BAR GPUs should be fine.
  constexpr VkDeviceSize UPLOAD_HEAP_SIZE_THRESHOLD = 512 * 1024 * 1024;
  constexpr VkMemoryPropertyFlags UPLOAD_HEAP_PROPERTIES =
    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
  std::array<VkDeviceSize, VK_MAX_MEMORY_HEAPS> heap_size_limits;
  if (m_debug_device)
  {
    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetPhysicalDeviceMemoryProperties(m_physical_device, &memory_properties);

    bool has_upload_heap = false;
    heap_size_limits.fill(VK_WHOLE_SIZE);
    for (u32 i = 0; i < memory_properties.memoryTypeCount; i++)
    {
      // Look for any memory types which are upload-like.
      const VkMemoryType& type = memory_properties.memoryTypes[i];
      if ((type.propertyFlags & UPLOAD_HEAP_PROPERTIES) != UPLOAD_HEAP_PROPERTIES)
        continue;

      const VkMemoryHeap& heap = memory_properties.memoryHeaps[type.heapIndex];
      if (heap.size >= UPLOAD_HEAP_SIZE_THRESHOLD)
        continue;

      if (heap_size_limits[type.heapIndex] == VK_WHOLE_SIZE)
      {
        Log_WarningPrintf("Disabling allocation from upload heap #%u (%.2f MB) due to debug device.", type.heapIndex,
                          static_cast<float>(heap.size) / 1048576.0f);
        heap_size_limits[type.heapIndex] = 0;
        has_upload_heap = true;
      }
    }

    if (has_upload_heap)
      ci.pHeapSizeLimit = heap_size_limits.data();
  }

  VkResult res = vmaCreateAllocator(&ci, &m_allocator);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vmaCreateAllocator failed: ");
    return false;
  }

  return true;
}

void VulkanDevice::DestroyAllocator()
{
  if (m_allocator == VK_NULL_HANDLE)
    return;

  vmaDestroyAllocator(m_allocator);
  m_allocator = VK_NULL_HANDLE;
}

bool VulkanDevice::CreateCommandBuffers()
{
  VkResult res;

  uint32_t frame_index = 0;
  for (CommandBuffer& resources : m_frame_resources)
  {
    resources.needs_fence_wait = false;

    VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, 0,
                                         m_graphics_queue_family_index};
    res = vkCreateCommandPool(m_device, &pool_info, nullptr, &resources.command_pool);
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkCreateCommandPool failed: ");
      return false;
    }
    Vulkan::SetObjectName(m_device, resources.command_pool,
                          TinyString::from_format("Frame Command Pool {}", frame_index));

    VkCommandBufferAllocateInfo buffer_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr,
                                               resources.command_pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                               static_cast<u32>(resources.command_buffers.size())};

    res = vkAllocateCommandBuffers(m_device, &buffer_info, resources.command_buffers.data());
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkAllocateCommandBuffers failed: ");
      return false;
    }
    for (u32 i = 0; i < resources.command_buffers.size(); i++)
    {
      Vulkan::SetObjectName(m_device, resources.command_buffers[i],
                            TinyString::from_format("Frame {} {}Command Buffer", frame_index, (i == 0) ? "Init" : ""));
    }

    VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};

    res = vkCreateFence(m_device, &fence_info, nullptr, &resources.fence);
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkCreateFence failed: ");
      return false;
    }
    Vulkan::SetObjectName(m_device, resources.fence, TinyString::from_format("Frame Fence {}", frame_index));

    if (!m_optional_extensions.vk_khr_push_descriptor)
    {
      VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_COMBINED_IMAGE_SAMPLER_DESCRIPTORS_PER_FRAME},
      };

      VkDescriptorPoolCreateInfo pool_create_info = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr,   0, MAX_DESCRIPTOR_SETS_PER_FRAME,
        static_cast<u32>(std::size(pool_sizes)),       pool_sizes};

      res = vkCreateDescriptorPool(m_device, &pool_create_info, nullptr, &resources.descriptor_pool);
      if (res != VK_SUCCESS)
      {
        LOG_VULKAN_ERROR(res, "vkCreateDescriptorPool failed: ");
        return false;
      }
      Vulkan::SetObjectName(m_device, resources.descriptor_pool,
                            TinyString::from_format("Frame Descriptor Pool {}", frame_index));
    }

    ++frame_index;
  }

  BeginCommandBuffer(0);
  return true;
}

void VulkanDevice::DestroyCommandBuffers()
{
  for (CommandBuffer& resources : m_frame_resources)
  {
    if (resources.fence != VK_NULL_HANDLE)
      vkDestroyFence(m_device, resources.fence, nullptr);
    if (resources.descriptor_pool != VK_NULL_HANDLE)
      vkDestroyDescriptorPool(m_device, resources.descriptor_pool, nullptr);
    if (resources.command_buffers[0] != VK_NULL_HANDLE)
    {
      vkFreeCommandBuffers(m_device, resources.command_pool, static_cast<u32>(resources.command_buffers.size()),
                           resources.command_buffers.data());
    }
    if (resources.command_pool != VK_NULL_HANDLE)
      vkDestroyCommandPool(m_device, resources.command_pool, nullptr);
  }
}

bool VulkanDevice::CreatePersistentDescriptorPool()
{
  static constexpr const VkDescriptorPoolSize pool_sizes[] = {
    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1},
    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_SAMPLER_DESCRIPTORS},
    {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 16},
    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16},
  };

  const VkDescriptorPoolCreateInfo pool_create_info = {
    VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,     nullptr,
    VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, MAX_SAMPLER_DESCRIPTORS,
    static_cast<u32>(std::size(pool_sizes)),           pool_sizes};

  VkResult res = vkCreateDescriptorPool(m_device, &pool_create_info, nullptr, &m_global_descriptor_pool);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateDescriptorPool failed: ");
    return false;
  }
  Vulkan::SetObjectName(m_device, m_global_descriptor_pool, "Global Descriptor Pool");

  if (m_features.gpu_timing)
  {
    const VkQueryPoolCreateInfo query_create_info = {
      VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, nullptr, 0, VK_QUERY_TYPE_TIMESTAMP, NUM_COMMAND_BUFFERS * 4, 0};
    res = vkCreateQueryPool(m_device, &query_create_info, nullptr, &m_timestamp_query_pool);
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkCreateQueryPool failed: ");
      m_features.gpu_timing = false;
      return false;
    }
  }

  return true;
}

void VulkanDevice::DestroyPersistentDescriptorPool()
{
  if (m_timestamp_query_pool != VK_NULL_HANDLE)
    vkDestroyQueryPool(m_device, m_timestamp_query_pool, nullptr);

  if (m_global_descriptor_pool != VK_NULL_HANDLE)
    vkDestroyDescriptorPool(m_device, m_global_descriptor_pool, nullptr);
}

bool VulkanDevice::RenderPassCacheKey::operator==(const RenderPassCacheKey& rhs) const
{
  return (std::memcmp(this, &rhs, sizeof(*this)) == 0);
}

bool VulkanDevice::RenderPassCacheKey::operator!=(const RenderPassCacheKey& rhs) const
{
  return (std::memcmp(this, &rhs, sizeof(*this)) != 0);
}

size_t VulkanDevice::RenderPassCacheKeyHash::operator()(const RenderPassCacheKey& rhs) const
{
  if constexpr (sizeof(void*) == 8)
    return XXH3_64bits(&rhs, sizeof(rhs));
  else
    return XXH32(&rhs, sizeof(rhs), 0x1337);
}

VkRenderPass VulkanDevice::GetRenderPass(const GPUPipeline::GraphicsConfig& config)
{
  RenderPassCacheKey key;
  std::memset(&key, 0, sizeof(key));

  for (u32 i = 0; i < MAX_RENDER_TARGETS; i++)
  {
    if (config.color_formats[i] == GPUTexture::Format::Unknown)
      break;

    key.color[i].format = static_cast<u8>(config.color_formats[i]);
    key.color[i].load_op = VK_ATTACHMENT_LOAD_OP_LOAD;
    key.color[i].store_op = VK_ATTACHMENT_STORE_OP_STORE;
  }

  if (config.depth_format != GPUTexture::Format::Unknown)
  {
    key.depth_format = static_cast<u8>(config.depth_format);
    key.depth_load_op = VK_ATTACHMENT_LOAD_OP_LOAD;
    key.depth_store_op = VK_ATTACHMENT_STORE_OP_STORE;

    const bool stencil = GPUTexture::IsDepthStencilFormat(config.depth_format);
    key.stencil_load_op = stencil ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    key.stencil_store_op = stencil ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
  }

  // key.color_feedback_loop = false;
  // key.depth_sampling = false;

  key.samples = static_cast<u8>(config.samples);

  const auto it = m_render_pass_cache.find(key);
  return (it != m_render_pass_cache.end()) ? it->second : CreateCachedRenderPass(key);
}

VkRenderPass VulkanDevice::GetRenderPass(GPUTexture* const* rts, u32 num_rts, GPUTexture* ds,
                                         bool color_feedback_loop /* = false */, bool depth_sampling /* = false */)
{
  RenderPassCacheKey key;
  std::memset(&key, 0, sizeof(key));

  static_assert(static_cast<u8>(GPUTexture::Format::Unknown) == 0);

  for (u32 i = 0; i < num_rts; i++)
  {
    key.color[i].format = static_cast<u8>(rts[i]->GetFormat());
    key.color[i].load_op = GetLoadOpForTexture(rts[i]);
    key.color[i].store_op = VK_ATTACHMENT_STORE_OP_STORE;
    key.samples = static_cast<u8>(rts[i]->GetSamples());
  }

  if (ds)
  {
    const VkAttachmentLoadOp load_op = GetLoadOpForTexture(ds);
    key.depth_format = static_cast<u8>(ds->GetFormat());
    key.depth_load_op = load_op;
    key.depth_store_op = VK_ATTACHMENT_STORE_OP_STORE;

    const bool stencil = GPUTexture::IsDepthStencilFormat(ds->GetFormat());
    key.stencil_load_op = stencil ? load_op : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    key.stencil_store_op = stencil ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;

    key.samples = static_cast<u8>(ds->GetSamples());
  }

  key.color_feedback_loop = color_feedback_loop;
  key.depth_sampling = depth_sampling;

  const auto it = m_render_pass_cache.find(key);
  return (it != m_render_pass_cache.end()) ? it->second : CreateCachedRenderPass(key);
}

VkRenderPass VulkanDevice::GetSwapChainRenderPass(GPUTexture::Format format, VkAttachmentLoadOp load_op)
{
  DebugAssert(format != GPUTexture::Format::Unknown);

  RenderPassCacheKey key;
  std::memset(&key, 0, sizeof(key));

  key.color[0].format = static_cast<u8>(format);
  key.color[0].load_op = load_op;
  key.color[0].store_op = VK_ATTACHMENT_STORE_OP_STORE;
  key.samples = 1;

  const auto it = m_render_pass_cache.find(key);
  return (it != m_render_pass_cache.end()) ? it->second : CreateCachedRenderPass(key);
}

VkRenderPass VulkanDevice::GetRenderPassForRestarting(VkRenderPass pass)
{
  for (const auto& it : m_render_pass_cache)
  {
    if (it.second != pass)
      continue;

    RenderPassCacheKey modified_key = it.first;
    for (u32 i = 0; i < MAX_RENDER_TARGETS; i++)
    {
      if (modified_key.color[i].load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
        modified_key.color[i].load_op = VK_ATTACHMENT_LOAD_OP_LOAD;
    }

    if (modified_key.depth_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
      modified_key.depth_load_op = VK_ATTACHMENT_LOAD_OP_LOAD;
    if (modified_key.stencil_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
      modified_key.stencil_load_op = VK_ATTACHMENT_LOAD_OP_LOAD;

    if (modified_key == it.first)
      return pass;

    auto fit = m_render_pass_cache.find(modified_key);
    if (fit != m_render_pass_cache.end())
      return fit->second;

    return CreateCachedRenderPass(modified_key);
  }

  return pass;
}

VkCommandBuffer VulkanDevice::GetCurrentInitCommandBuffer()
{
  CommandBuffer& res = m_frame_resources[m_current_frame];
  VkCommandBuffer buf = res.command_buffers[0];
  if (res.init_buffer_used)
    return buf;

  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                              VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
  vkBeginCommandBuffer(buf, &bi);
  res.init_buffer_used = true;
  return buf;
}

VkDescriptorSet VulkanDevice::AllocateDescriptorSet(VkDescriptorSetLayout set_layout)
{
  VkDescriptorSetAllocateInfo allocate_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr,
                                               m_frame_resources[m_current_frame].descriptor_pool, 1, &set_layout};

  VkDescriptorSet descriptor_set;
  VkResult res = vkAllocateDescriptorSets(m_device, &allocate_info, &descriptor_set);
  if (res != VK_SUCCESS)
  {
    // Failing to allocate a descriptor set is not a fatal error, we can
    // recover by moving to the next command buffer.
    return VK_NULL_HANDLE;
  }

  return descriptor_set;
}

VkDescriptorSet VulkanDevice::AllocatePersistentDescriptorSet(VkDescriptorSetLayout set_layout)
{
  VkDescriptorSetAllocateInfo allocate_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr,
                                               m_global_descriptor_pool, 1, &set_layout};

  VkDescriptorSet descriptor_set;
  VkResult res = vkAllocateDescriptorSets(m_device, &allocate_info, &descriptor_set);
  if (res != VK_SUCCESS)
    return VK_NULL_HANDLE;

  return descriptor_set;
}

void VulkanDevice::FreePersistentDescriptorSet(VkDescriptorSet set)
{
  vkFreeDescriptorSets(m_device, m_global_descriptor_pool, 1, &set);
}

void VulkanDevice::WaitForFenceCounter(u64 fence_counter)
{
  if (m_completed_fence_counter >= fence_counter)
    return;

  // Find the first command buffer which covers this counter value.
  u32 index = (m_current_frame + 1) % NUM_COMMAND_BUFFERS;
  while (index != m_current_frame)
  {
    if (m_frame_resources[index].fence_counter >= fence_counter)
      break;

    index = (index + 1) % NUM_COMMAND_BUFFERS;
  }

  DebugAssert(index != m_current_frame);
  WaitForCommandBufferCompletion(index);
}

void VulkanDevice::WaitForGPUIdle()
{
  WaitForPresentComplete();
  vkDeviceWaitIdle(m_device);
}

float VulkanDevice::GetAndResetAccumulatedGPUTime()
{
  const float time = m_accumulated_gpu_time;
  m_accumulated_gpu_time = 0.0f;
  return time;
}

bool VulkanDevice::SetGPUTimingEnabled(bool enabled)
{
  m_gpu_timing_enabled = enabled && m_features.gpu_timing;
  return (enabled == m_gpu_timing_enabled);
}

void VulkanDevice::WaitForCommandBufferCompletion(u32 index)
{
  // We might be waiting for the buffer we just submitted to the worker thread.
  if (m_queued_present.command_buffer_index == index && !m_present_done.load(std::memory_order_acquire))
  {
    Log_WarningFmt("Waiting for threaded submission of cmdbuffer {}", index);
    WaitForPresentComplete();
  }

  // Wait for this command buffer to be completed.
  static constexpr u32 MAX_TIMEOUTS = 10;
  u32 timeouts = 0;
  for (;;)
  {
    VkResult res = vkWaitForFences(m_device, 1, &m_frame_resources[index].fence, VK_TRUE, UINT64_MAX);
    if (res == VK_SUCCESS)
      break;

    if (res == VK_TIMEOUT && (++timeouts) <= MAX_TIMEOUTS)
    {
      Log_ErrorFmt("vkWaitForFences() for cmdbuffer {} failed with VK_TIMEOUT, trying again.", index);
      continue;
    }
    else if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkWaitForFences() for cmdbuffer %u failed: ", index);
      m_last_submit_failed.store(true, std::memory_order_release);
      return;
    }
  }

  // Clean up any resources for command buffers between the last known completed buffer and this
  // now-completed command buffer. If we use >2 buffers, this may be more than one buffer.
  const u64 now_completed_counter = m_frame_resources[index].fence_counter;
  u32 cleanup_index = (m_current_frame + 1) % NUM_COMMAND_BUFFERS;
  while (cleanup_index != m_current_frame)
  {
    CommandBuffer& resources = m_frame_resources[cleanup_index];
    if (resources.fence_counter > now_completed_counter)
      break;

    if (m_gpu_timing_enabled && resources.timestamp_written)
    {
      std::array<u64, 2> timestamps;
      VkResult res =
        vkGetQueryPoolResults(m_device, m_timestamp_query_pool, index * 2, static_cast<u32>(timestamps.size()),
                              sizeof(u64) * timestamps.size(), timestamps.data(), sizeof(u64), VK_QUERY_RESULT_64_BIT);
      if (res == VK_SUCCESS)
      {
        // if we didn't write the timestamp at the start of the cmdbuffer (just enabled timing), the first TS will be
        // zero
        if (timestamps[0] > 0 && m_gpu_timing_enabled)
        {
          const double ns_diff =
            (timestamps[1] - timestamps[0]) * static_cast<double>(m_device_properties.limits.timestampPeriod);
          m_accumulated_gpu_time += static_cast<float>(ns_diff / 1000000.0);
        }
      }
      else
      {
        LOG_VULKAN_ERROR(res, "vkGetQueryPoolResults failed: ");
      }
    }

    cleanup_index = (cleanup_index + 1) % NUM_COMMAND_BUFFERS;
  }

  m_completed_fence_counter = now_completed_counter;
  while (!m_cleanup_objects.empty())
  {
    auto& it = m_cleanup_objects.front();
    if (it.first > now_completed_counter)
      break;
    it.second();
    m_cleanup_objects.pop_front();
  }
}

void VulkanDevice::SubmitCommandBuffer(VulkanSwapChain* present_swap_chain /* = nullptr */,
                                       bool submit_on_thread /* = false */)
{
  if (m_last_submit_failed.load(std::memory_order_acquire))
    return;

  CommandBuffer& resources = m_frame_resources[m_current_frame];

  // End the current command buffer.
  VkResult res;
  if (resources.init_buffer_used)
  {
    res = vkEndCommandBuffer(resources.command_buffers[0]);
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkEndCommandBuffer failed: ");
      Panic("Failed to end command buffer");
    }
  }

  if (m_gpu_timing_enabled && resources.timestamp_written)
  {
    vkCmdWriteTimestamp(m_current_command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, m_timestamp_query_pool,
                        m_current_frame * 2 + 1);
  }

  res = vkEndCommandBuffer(resources.command_buffers[1]);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkEndCommandBuffer failed: ");
    Panic("Failed to end command buffer");
  }

  // This command buffer now has commands, so can't be re-used without waiting.
  resources.needs_fence_wait = true;

  std::unique_lock<std::mutex> lock(m_present_mutex);
  WaitForPresentComplete(lock);

  if (!submit_on_thread || !m_present_thread.joinable())
  {
    DoSubmitCommandBuffer(m_current_frame, present_swap_chain);
    if (present_swap_chain)
      DoPresent(present_swap_chain);
    return;
  }

  m_queued_present.command_buffer_index = m_current_frame;
  m_queued_present.swap_chain = present_swap_chain;
  m_present_done.store(false, std::memory_order_release);
  m_present_queued_cv.notify_one();
}

void VulkanDevice::DoSubmitCommandBuffer(u32 index, VulkanSwapChain* present_swap_chain)
{
  CommandBuffer& resources = m_frame_resources[index];

  uint32_t wait_bits = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo submit_info = {VK_STRUCTURE_TYPE_SUBMIT_INFO,
                              nullptr,
                              0u,
                              nullptr,
                              nullptr,
                              resources.init_buffer_used ? 2u : 1u,
                              resources.init_buffer_used ? resources.command_buffers.data() :
                                                           &resources.command_buffers[1],
                              0u,
                              nullptr};

  if (present_swap_chain)
  {
    submit_info.pWaitSemaphores = present_swap_chain->GetImageAvailableSemaphorePtr();
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitDstStageMask = &wait_bits;

    submit_info.pSignalSemaphores = present_swap_chain->GetRenderingFinishedSemaphorePtr();
    submit_info.signalSemaphoreCount = 1;
  }

  const VkResult res = vkQueueSubmit(m_graphics_queue, 1, &submit_info, resources.fence);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkQueueSubmit failed: ");
    m_last_submit_failed.store(true, std::memory_order_release);
    return;
  }
}

void VulkanDevice::DoPresent(VulkanSwapChain* present_swap_chain)
{
  const VkPresentInfoKHR present_info = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                         nullptr,
                                         1,
                                         present_swap_chain->GetRenderingFinishedSemaphorePtr(),
                                         1,
                                         present_swap_chain->GetSwapChainPtr(),
                                         present_swap_chain->GetCurrentImageIndexPtr(),
                                         nullptr};

  present_swap_chain->ReleaseCurrentImage();

  const VkResult res = vkQueuePresentKHR(m_present_queue, &present_info);
  if (res != VK_SUCCESS)
  {
    // VK_ERROR_OUT_OF_DATE_KHR is not fatal, just means we need to recreate our swap chain.
    if (res != VK_ERROR_OUT_OF_DATE_KHR && res != VK_SUBOPTIMAL_KHR)
      LOG_VULKAN_ERROR(res, "vkQueuePresentKHR failed: ");

    m_last_present_failed.store(true, std::memory_order_release);
    return;
  }

  // Grab the next image as soon as possible, that way we spend less time blocked on the next
  // submission. Don't care if it fails, we'll deal with that at the presentation call site.
  // Credit to dxvk for the idea.
  present_swap_chain->AcquireNextImage();
}

void VulkanDevice::WaitForPresentComplete()
{
  if (m_present_done.load(std::memory_order_acquire))
    return;

  std::unique_lock<std::mutex> lock(m_present_mutex);
  WaitForPresentComplete(lock);
}

void VulkanDevice::WaitForPresentComplete(std::unique_lock<std::mutex>& lock)
{
  if (m_present_done.load(std::memory_order_acquire))
    return;

  m_present_done_cv.wait(lock, [this]() { return m_present_done.load(std::memory_order_acquire); });
}

void VulkanDevice::PresentThread()
{
  std::unique_lock<std::mutex> lock(m_present_mutex);
  while (!m_present_thread_done.load(std::memory_order_acquire))
  {
    m_present_queued_cv.wait(lock, [this]() {
      return !m_present_done.load(std::memory_order_acquire) || m_present_thread_done.load(std::memory_order_acquire);
    });

    if (m_present_done.load(std::memory_order_acquire))
      continue;

    DoSubmitCommandBuffer(m_queued_present.command_buffer_index, m_queued_present.swap_chain);
    if (m_queued_present.swap_chain)
      DoPresent(m_queued_present.swap_chain);
    m_present_done.store(true, std::memory_order_release);
    m_present_done_cv.notify_one();
  }
}

void VulkanDevice::StartPresentThread()
{
  DebugAssert(!m_present_thread.joinable());
  m_present_thread_done.store(false, std::memory_order_release);
  m_present_thread = std::thread(&VulkanDevice::PresentThread, this);
}

void VulkanDevice::StopPresentThread()
{
  if (!m_present_thread.joinable())
    return;

  {
    std::unique_lock<std::mutex> lock(m_present_mutex);
    WaitForPresentComplete(lock);
    m_present_thread_done.store(true, std::memory_order_release);
    m_present_queued_cv.notify_one();
  }

  m_present_thread.join();
}

void VulkanDevice::MoveToNextCommandBuffer()
{
  BeginCommandBuffer((m_current_frame + 1) % NUM_COMMAND_BUFFERS);
}

void VulkanDevice::BeginCommandBuffer(u32 index)
{
  CommandBuffer& resources = m_frame_resources[index];

  // Wait for the GPU to finish with all resources for this command buffer.
  if (resources.fence_counter > m_completed_fence_counter)
    WaitForCommandBufferCompletion(index);

  // Reset fence to unsignaled before starting.
  VkResult res = vkResetFences(m_device, 1, &resources.fence);
  if (res != VK_SUCCESS)
    LOG_VULKAN_ERROR(res, "vkResetFences failed: ");

  // Reset command pools to beginning since we can re-use the memory now
  res = vkResetCommandPool(m_device, resources.command_pool, 0);
  if (res != VK_SUCCESS)
    LOG_VULKAN_ERROR(res, "vkResetCommandPool failed: ");

  // Enable commands to be recorded to the two buffers again.
  VkCommandBufferBeginInfo begin_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                         VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
  res = vkBeginCommandBuffer(resources.command_buffers[1], &begin_info);
  if (res != VK_SUCCESS)
    LOG_VULKAN_ERROR(res, "vkBeginCommandBuffer failed: ");

  // Also can do the same for the descriptor pools
  if (resources.descriptor_pool != VK_NULL_HANDLE)
  {
    res = vkResetDescriptorPool(m_device, resources.descriptor_pool, 0);
    if (res != VK_SUCCESS)
      LOG_VULKAN_ERROR(res, "vkResetDescriptorPool failed: ");
  }

  if (m_gpu_timing_enabled)
  {
    vkCmdResetQueryPool(resources.command_buffers[1], m_timestamp_query_pool, index * 2, 2);
    vkCmdWriteTimestamp(resources.command_buffers[1], VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, m_timestamp_query_pool,
                        index * 2);
  }

  resources.fence_counter = m_next_fence_counter++;
  resources.init_buffer_used = false;
  resources.timestamp_written = m_gpu_timing_enabled;

  m_current_frame = index;
  m_current_command_buffer = resources.command_buffers[1];

  // using the lower 32 bits of the fence index should be sufficient here, I hope...
  vmaSetCurrentFrameIndex(m_allocator, static_cast<u32>(m_next_fence_counter));
}

void VulkanDevice::SubmitCommandBuffer(bool wait_for_completion)
{
  DebugAssert(!InRenderPass());

  const u32 current_frame = m_current_frame;
  SubmitCommandBuffer();
  MoveToNextCommandBuffer();

  if (wait_for_completion)
    WaitForCommandBufferCompletion(current_frame);

  InvalidateCachedState();
}

void VulkanDevice::SubmitCommandBuffer(bool wait_for_completion, const char* reason, ...)
{
  std::va_list ap;
  va_start(ap, reason);
  const std::string reason_str(StringUtil::StdStringFromFormatV(reason, ap));
  va_end(ap);

  Log_WarningPrintf("Executing command buffer due to '%s'", reason_str.c_str());
  SubmitCommandBuffer(wait_for_completion);
}

void VulkanDevice::SubmitCommandBufferAndRestartRenderPass(const char* reason)
{
  if (InRenderPass())
    EndRenderPass();

  VulkanPipeline* pl = m_current_pipeline;
  SubmitCommandBuffer(false, "%s", reason);

  SetPipeline(pl);
  BeginRenderPass();
}

bool VulkanDevice::CheckLastPresentFail()
{
  return m_last_present_failed.exchange(false, std::memory_order_acq_rel);
}

bool VulkanDevice::CheckLastSubmitFail()
{
  return m_last_submit_failed.load(std::memory_order_acquire);
}

void VulkanDevice::DeferBufferDestruction(VkBuffer object, VmaAllocation allocation)
{
  m_cleanup_objects.emplace_back(GetCurrentFenceCounter(),
                                 [this, object, allocation]() { vmaDestroyBuffer(m_allocator, object, allocation); });
}

void VulkanDevice::DeferBufferDestruction(VkBuffer object, VkDeviceMemory memory)
{
  m_cleanup_objects.emplace_back(GetCurrentFenceCounter(), [this, object, memory]() {
    vkDestroyBuffer(m_device, object, nullptr);
    vkFreeMemory(m_device, memory, nullptr);
  });
}

void VulkanDevice::DeferFramebufferDestruction(VkFramebuffer object)
{
  m_cleanup_objects.emplace_back(GetCurrentFenceCounter(),
                                 [this, object]() { vkDestroyFramebuffer(m_device, object, nullptr); });
}

void VulkanDevice::DeferImageDestruction(VkImage object, VmaAllocation allocation)
{
  m_cleanup_objects.emplace_back(GetCurrentFenceCounter(),
                                 [this, object, allocation]() { vmaDestroyImage(m_allocator, object, allocation); });
}

void VulkanDevice::DeferImageViewDestruction(VkImageView object)
{
  m_cleanup_objects.emplace_back(GetCurrentFenceCounter(),
                                 [this, object]() { vkDestroyImageView(m_device, object, nullptr); });
}

void VulkanDevice::DeferPipelineDestruction(VkPipeline object)
{
  m_cleanup_objects.emplace_back(GetCurrentFenceCounter(),
                                 [this, object]() { vkDestroyPipeline(m_device, object, nullptr); });
}

void VulkanDevice::DeferBufferViewDestruction(VkBufferView object)
{
  m_cleanup_objects.emplace_back(GetCurrentFenceCounter(),
                                 [this, object]() { vkDestroyBufferView(m_device, object, nullptr); });
}

void VulkanDevice::DeferPersistentDescriptorSetDestruction(VkDescriptorSet object)
{
  m_cleanup_objects.emplace_back(GetCurrentFenceCounter(), [this, object]() { FreePersistentDescriptorSet(object); });
}

VKAPI_ATTR VkBool32 VKAPI_CALL DebugMessengerCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                      VkDebugUtilsMessageTypeFlagsEXT messageType,
                                                      const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
                                                      void* pUserData)
{
  if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
  {
    Log_ErrorPrintf("Vulkan debug report: (%s) %s", pCallbackData->pMessageIdName ? pCallbackData->pMessageIdName : "",
                    pCallbackData->pMessage);
  }
  else if (severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT))
  {
    Log_WarningPrintf("Vulkan debug report: (%s) %s",
                      pCallbackData->pMessageIdName ? pCallbackData->pMessageIdName : "", pCallbackData->pMessage);
  }
  else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)
  {
    Log_InfoPrintf("Vulkan debug report: (%s) %s", pCallbackData->pMessageIdName ? pCallbackData->pMessageIdName : "",
                   pCallbackData->pMessage);
  }
  else
  {
    Log_DevPrintf("Vulkan debug report: (%s) %s", pCallbackData->pMessageIdName ? pCallbackData->pMessageIdName : "",
                  pCallbackData->pMessage);
  }

  return VK_FALSE;
}

bool VulkanDevice::EnableDebugUtils()
{
  // Already enabled?
  if (m_debug_messenger_callback != VK_NULL_HANDLE)
    return true;

  // Check for presence of the functions before calling
  if (!vkCreateDebugUtilsMessengerEXT || !vkDestroyDebugUtilsMessengerEXT || !vkSubmitDebugUtilsMessageEXT)
  {
    return false;
  }

  VkDebugUtilsMessengerCreateInfoEXT messenger_info = {
    VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
    nullptr,
    0,
    VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
      VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT,
    VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT |
      VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
    DebugMessengerCallback,
    nullptr};

  const VkResult res =
    vkCreateDebugUtilsMessengerEXT(m_instance, &messenger_info, nullptr, &m_debug_messenger_callback);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateDebugUtilsMessengerEXT failed: ");
    return false;
  }

  return true;
}

void VulkanDevice::DisableDebugUtils()
{
  if (m_debug_messenger_callback != VK_NULL_HANDLE)
  {
    vkDestroyDebugUtilsMessengerEXT(m_instance, m_debug_messenger_callback, nullptr);
    m_debug_messenger_callback = VK_NULL_HANDLE;
  }
}

bool VulkanDevice::IsDeviceAdreno() const
{
  // Assume turnip is fine...
  return ((m_device_properties.vendorID == 0x5143 ||
           m_device_driver_properties.driverID == VK_DRIVER_ID_QUALCOMM_PROPRIETARY) &&
          m_device_driver_properties.driverID != VK_DRIVER_ID_MESA_TURNIP);
}

bool VulkanDevice::IsDeviceMali() const
{
  return (m_device_properties.vendorID == 0x13B5 ||
          m_device_driver_properties.driverID == VK_DRIVER_ID_ARM_PROPRIETARY);
}

bool VulkanDevice::IsDeviceImgTec() const
{
  return (m_device_properties.vendorID == 0x1010 ||
          m_device_driver_properties.driverID == VK_DRIVER_ID_IMAGINATION_PROPRIETARY);
}

bool VulkanDevice::IsBrokenMobileDriver() const
{
  return (IsDeviceAdreno() || IsDeviceMali() || IsDeviceImgTec());
}

VkRenderPass VulkanDevice::CreateCachedRenderPass(RenderPassCacheKey key)
{
  VkAttachmentReference color_reference;
  VkAttachmentReference* color_reference_ptr = nullptr;
  VkAttachmentReference depth_reference;
  VkAttachmentReference* depth_reference_ptr = nullptr;
  VkAttachmentReference input_reference;
  VkAttachmentReference* input_reference_ptr = nullptr;
  VkSubpassDependency subpass_dependency;
  VkSubpassDependency* subpass_dependency_ptr = nullptr;
  std::array<VkAttachmentDescription, MAX_RENDER_TARGETS + 1> attachments;
  u32 num_attachments = 0;

  for (u32 i = 0; i < MAX_RENDER_TARGETS; i++)
  {
    if (key.color[i].format == static_cast<u8>(GPUTexture::Format::Unknown))
      break;

    const VkImageLayout layout =
      key.color_feedback_loop ?
        (UseFeedbackLoopLayout() ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT : VK_IMAGE_LAYOUT_GENERAL) :
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    const RenderPassCacheKey::RenderTarget key_rt = key.color[i];
    attachments[num_attachments] = {i,
                                    TEXTURE_FORMAT_MAPPING[key_rt.format],
                                    static_cast<VkSampleCountFlagBits>(key.samples),
                                    static_cast<VkAttachmentLoadOp>(key_rt.load_op),
                                    static_cast<VkAttachmentStoreOp>(key_rt.store_op),
                                    VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                    VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                    layout,
                                    layout};
    color_reference.attachment = num_attachments;
    color_reference.layout = layout;
    color_reference_ptr = &color_reference;

    if (key.color_feedback_loop)
    {
      if (!UseFeedbackLoopLayout())
      {
        input_reference.attachment = num_attachments;
        input_reference.layout = layout;
        input_reference_ptr = &input_reference;
      }

      if (!m_optional_extensions.vk_ext_rasterization_order_attachment_access)
      {
        // don't need the framebuffer-local dependency when we have rasterization order attachment access
        subpass_dependency.srcSubpass = 0;
        subpass_dependency.dstSubpass = 0;
        subpass_dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        subpass_dependency.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        subpass_dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        subpass_dependency.dstAccessMask =
          UseFeedbackLoopLayout() ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
        subpass_dependency.dependencyFlags = UseFeedbackLoopLayout() ?
                                               (VK_DEPENDENCY_BY_REGION_BIT | VK_DEPENDENCY_FEEDBACK_LOOP_BIT_EXT) :
                                               VK_DEPENDENCY_BY_REGION_BIT;
        subpass_dependency_ptr = &subpass_dependency;
      }
    }

    num_attachments++;
  }

  const u32 num_rts = num_attachments;

  if (key.depth_format != static_cast<u8>(GPUTexture::Format::Unknown))
  {
    const VkImageLayout layout =
      key.depth_sampling ?
        (UseFeedbackLoopLayout() ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT : VK_IMAGE_LAYOUT_GENERAL) :
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachments[num_attachments] = {0,
                                    static_cast<VkFormat>(TEXTURE_FORMAT_MAPPING[key.depth_format]),
                                    static_cast<VkSampleCountFlagBits>(key.samples),
                                    static_cast<VkAttachmentLoadOp>(key.depth_load_op),
                                    static_cast<VkAttachmentStoreOp>(key.depth_store_op),
                                    static_cast<VkAttachmentLoadOp>(key.stencil_load_op),
                                    static_cast<VkAttachmentStoreOp>(key.stencil_store_op),
                                    layout,
                                    layout};
    depth_reference.attachment = num_attachments;
    depth_reference.layout = layout;
    depth_reference_ptr = &depth_reference;
    num_attachments++;
  }

  const VkSubpassDescriptionFlags subpass_flags =
    (key.color_feedback_loop && m_optional_extensions.vk_ext_rasterization_order_attachment_access) ?
      VK_SUBPASS_DESCRIPTION_RASTERIZATION_ORDER_ATTACHMENT_COLOR_ACCESS_BIT_EXT :
      0;
  const VkSubpassDescription subpass = {subpass_flags,
                                        VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        input_reference_ptr ? num_rts : 0u,
                                        input_reference_ptr,
                                        num_rts,
                                        color_reference_ptr,
                                        nullptr,
                                        depth_reference_ptr,
                                        0,
                                        nullptr};
  const VkRenderPassCreateInfo pass_info = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                            nullptr,
                                            0u,
                                            num_attachments,
                                            attachments.data(),
                                            1u,
                                            &subpass,
                                            subpass_dependency_ptr ? 1u : 0u,
                                            subpass_dependency_ptr};

  VkRenderPass pass;
  const VkResult res = vkCreateRenderPass(m_device, &pass_info, nullptr, &pass);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateRenderPass failed: ");
    return VK_NULL_HANDLE;
  }

  m_render_pass_cache.emplace(key, pass);
  return pass;
}

VkFramebuffer VulkanDevice::CreateFramebuffer(GPUTexture* const* rts, u32 num_rts, GPUTexture* ds, u32 flags)
{
  VulkanDevice& dev = VulkanDevice::GetInstance();
  VkRenderPass render_pass = dev.GetRenderPass(rts, num_rts, ds, false, false);

  const GPUTexture* rt_or_ds = (num_rts > 0) ? rts[0] : ds;
  DebugAssert(rt_or_ds);

  Vulkan::FramebufferBuilder fbb;
  fbb.SetRenderPass(render_pass);
  fbb.SetSize(rt_or_ds->GetWidth(), rt_or_ds->GetHeight(), 1);
  for (u32 i = 0; i < num_rts; i++)
    fbb.AddAttachment(static_cast<VulkanTexture*>(rts[i])->GetView());
  if (ds)
    fbb.AddAttachment(static_cast<VulkanTexture*>(ds)->GetView());

  return fbb.Create(dev.m_device, false);
}

void VulkanDevice::DestroyFramebuffer(VkFramebuffer fbo)
{
  if (fbo == VK_NULL_HANDLE)
    return;

  VulkanDevice::GetInstance().DeferFramebufferDestruction(fbo);
}

void VulkanDevice::GetAdapterAndModeList(AdapterAndModeList* ret, VkInstance instance)
{
  GPUList gpus = EnumerateGPUs(instance);
  ret->adapter_names.clear();
  for (auto& [gpu, name] : gpus)
    ret->adapter_names.push_back(std::move(name));
}

GPUDevice::AdapterAndModeList VulkanDevice::StaticGetAdapterAndModeList()
{
  AdapterAndModeList ret;
  std::unique_lock lock(s_instance_mutex);

  // Device shouldn't be torn down since we have the lock.
  if (g_gpu_device && g_gpu_device->GetRenderAPI() == RenderAPI::Vulkan && Vulkan::IsVulkanLibraryLoaded())
  {
    GetAdapterAndModeList(&ret, VulkanDevice::GetInstance().m_instance);
  }
  else
  {
    if (Vulkan::LoadVulkanLibrary())
    {
      ScopedGuard lib_guard([]() { Vulkan::UnloadVulkanLibrary(); });
      OptionalExtensions oe = {};
      const VkInstance instance = CreateVulkanInstance(WindowInfo(), &oe, false, false);
      if (instance != VK_NULL_HANDLE)
      {
        if (Vulkan::LoadVulkanInstanceFunctions(instance))
          GetAdapterAndModeList(&ret, instance);

        vkDestroyInstance(instance, nullptr);
      }
    }
  }

  return ret;
}

GPUDevice::AdapterAndModeList VulkanDevice::GetAdapterAndModeList()
{
  AdapterAndModeList ret;
  GetAdapterAndModeList(&ret, m_instance);
  return ret;
}

bool VulkanDevice::IsSuitableDefaultRenderer()
{
#ifdef __ANDROID__
  // No way in hell.
  return false;
#else
  AdapterAndModeList aml = StaticGetAdapterAndModeList();
  if (aml.adapter_names.empty())
  {
    // No adapters, not gonna be able to use VK.
    return false;
  }

  // Check the first GPU, should be enough.
  const std::string& name = aml.adapter_names.front();
  Log_InfoFmt("Using Vulkan GPU '{}' for automatic renderer check.", name);

  // Any software rendering (LLVMpipe, SwiftShader).
  if (StringUtil::StartsWithNoCase(name, "llvmpipe") || StringUtil::StartsWithNoCase(name, "SwiftShader"))
  {
    Log_InfoPrint("Not using Vulkan for software renderer.");
    return false;
  }

  // For Intel, OpenGL usually ends up faster on Linux, because of fbfetch.
  // Plus, the Ivy Bridge and Haswell drivers are incomplete.
  if (StringUtil::StartsWithNoCase(name, "Intel"))
  {
    Log_InfoPrint("Not using Vulkan for Intel GPU.");
    return false;
  }

  Log_InfoPrint("Allowing Vulkan as default renderer.");
  return true;
#endif
}

RenderAPI VulkanDevice::GetRenderAPI() const
{
  return RenderAPI::Vulkan;
}

bool VulkanDevice::HasSurface() const
{
  return static_cast<bool>(m_swap_chain);
}

bool VulkanDevice::CreateDevice(const std::string_view& adapter, bool threaded_presentation,
                                std::optional<bool> exclusive_fullscreen_control, FeatureMask disabled_features,
                                Error* error)
{
  std::unique_lock lock(s_instance_mutex);
  bool enable_debug_utils = m_debug_device;
  bool enable_validation_layer = m_debug_device;

  if (!Vulkan::LoadVulkanLibrary())
  {
    Error::SetStringView(error, "Failed to load Vulkan library. Does your GPU and/or driver support Vulkan?");
    return false;
  }

  m_instance = CreateVulkanInstance(m_window_info, &m_optional_extensions, enable_debug_utils, enable_validation_layer);
  if (m_instance == VK_NULL_HANDLE)
  {
    if (enable_debug_utils || enable_validation_layer)
    {
      // Try again without the validation layer.
      enable_debug_utils = false;
      enable_validation_layer = false;
      m_instance =
        CreateVulkanInstance(m_window_info, &m_optional_extensions, enable_debug_utils, enable_validation_layer);
      if (m_instance == VK_NULL_HANDLE)
      {
        Error::SetStringView(error, "Failed to create Vulkan instance. Does your GPU and/or driver support Vulkan?");
        return false;
      }

      Log_ErrorPrintf("Vulkan validation/debug layers requested but are unavailable. Creating non-debug device.");
    }
  }

  if (!Vulkan::LoadVulkanInstanceFunctions(m_instance))
  {
    Log_ErrorPrintf("Failed to load Vulkan instance functions");
    Error::SetStringView(error, "Failed to load Vulkan instance functions");
    return false;
  }

  GPUList gpus = EnumerateGPUs(m_instance);
  if (gpus.empty())
  {
    Error::SetStringView(error, "No physical devices found. Does your GPU and/or driver support Vulkan?");
    return false;
  }

  if (!adapter.empty())
  {
    u32 gpu_index = 0;
    for (; gpu_index < static_cast<u32>(gpus.size()); gpu_index++)
    {
      Log_InfoFmt("GPU {}: {}", gpu_index, gpus[gpu_index].second);
      if (gpus[gpu_index].second == adapter)
      {
        m_physical_device = gpus[gpu_index].first;
        break;
      }
    }

    if (gpu_index == static_cast<u32>(gpus.size()))
    {
      Log_WarningFmt("Requested GPU '{}' not found, using first ({})", adapter, gpus[0].second);
      m_physical_device = gpus[0].first;
    }
  }
  else
  {
    Log_InfoFmt("No GPU requested, using first ({})", gpus[0].second);
    m_physical_device = gpus[0].first;
  }

  // Read device physical memory properties, we need it for allocating buffers
  vkGetPhysicalDeviceProperties(m_physical_device, &m_device_properties);
  m_device_properties.limits.minUniformBufferOffsetAlignment =
    std::max(m_device_properties.limits.minUniformBufferOffsetAlignment, static_cast<VkDeviceSize>(1));
  m_device_properties.limits.minTexelBufferOffsetAlignment =
    std::max(m_device_properties.limits.minTexelBufferOffsetAlignment, static_cast<VkDeviceSize>(1));
  m_device_properties.limits.optimalBufferCopyOffsetAlignment =
    std::max(m_device_properties.limits.optimalBufferCopyOffsetAlignment, static_cast<VkDeviceSize>(1));
  m_device_properties.limits.optimalBufferCopyRowPitchAlignment =
    std::max(m_device_properties.limits.optimalBufferCopyRowPitchAlignment, static_cast<VkDeviceSize>(1));
  m_device_properties.limits.bufferImageGranularity =
    std::max(m_device_properties.limits.bufferImageGranularity, static_cast<VkDeviceSize>(1));

  if (enable_debug_utils)
    EnableDebugUtils();

  VkSurfaceKHR surface = VK_NULL_HANDLE;
  ScopedGuard surface_cleanup = [this, &surface]() {
    if (surface != VK_NULL_HANDLE)
      vkDestroySurfaceKHR(m_instance, surface, nullptr);
  };
  if (m_window_info.type != WindowInfo::Type::Surfaceless)
  {
    surface = VulkanSwapChain::CreateVulkanSurface(m_instance, m_physical_device, &m_window_info);
    if (surface == VK_NULL_HANDLE)
      return false;
  }

  // Attempt to create the device.
  if (!CreateDevice(surface, enable_validation_layer))
    return false;

  if (!CheckFeatures(disabled_features))
  {
    Error::SetStringView(error, "Your GPU does not support the required Vulkan features.");
    return false;
  }

  // And critical resources.
  if (!CreateAllocator() || !CreatePersistentDescriptorPool() || !CreateCommandBuffers() || !CreatePipelineLayouts())
    return false;

  if (threaded_presentation)
    StartPresentThread();

  m_exclusive_fullscreen_control = exclusive_fullscreen_control;

  if (surface != VK_NULL_HANDLE)
  {
    m_swap_chain = VulkanSwapChain::Create(m_window_info, surface, m_sync_mode, m_exclusive_fullscreen_control);
    if (!m_swap_chain)
    {
      Error::SetStringView(error, "Failed to create swap chain");
      return false;
    }

    // NOTE: This is assigned afterwards, because some platforms can modify the window info (e.g. Metal).
    m_window_info = m_swap_chain->GetWindowInfo();
  }

  surface_cleanup.Cancel();

  // Render a frame as soon as possible to clear out whatever was previously being displayed.
  if (m_window_info.type != WindowInfo::Type::Surfaceless)
    RenderBlankFrame();

  if (!CreateNullTexture())
  {
    Error::SetStringView(error, "Failed to create dummy texture");
    return false;
  }

  if (!CreateBuffers() || !CreatePersistentDescriptorSets())
  {
    Error::SetStringView(error, "Failed to create buffers/descriptor sets");
    return false;
  }

  return true;
}

void VulkanDevice::DestroyDevice()
{
  std::unique_lock lock(s_instance_mutex);

  if (InRenderPass())
    EndRenderPass();

  // Don't both submitting the current command buffer, just toss it.
  if (m_device != VK_NULL_HANDLE)
    WaitForGPUIdle();

  StopPresentThread();
  m_swap_chain.reset();

  if (m_null_texture)
  {
    m_null_texture->Destroy(false);
    m_null_texture.reset();
  }
  for (auto& it : m_cleanup_objects)
    it.second();
  m_cleanup_objects.clear();
  DestroyPersistentDescriptorSets();
  DestroyBuffers();
  DestroySamplers();

  DestroyPersistentDescriptorPool();
  DestroyPipelineLayouts();
  DestroyCommandBuffers();
  DestroyAllocator();

  for (auto& it : m_render_pass_cache)
    vkDestroyRenderPass(m_device, it.second, nullptr);
  m_render_pass_cache.clear();

  if (m_pipeline_cache != VK_NULL_HANDLE)
  {
    vkDestroyPipelineCache(m_device, m_pipeline_cache, nullptr);
    m_pipeline_cache = VK_NULL_HANDLE;
  }

  if (m_device != VK_NULL_HANDLE)
  {
    vkDestroyDevice(m_device, nullptr);
    m_device = VK_NULL_HANDLE;
  }

  if (m_debug_messenger_callback != VK_NULL_HANDLE)
    DisableDebugUtils();

  if (m_instance != VK_NULL_HANDLE)
  {
    vkDestroyInstance(m_instance, nullptr);
    m_instance = VK_NULL_HANDLE;
  }

  Vulkan::UnloadVulkanLibrary();
}

bool VulkanDevice::ValidatePipelineCacheHeader(const VK_PIPELINE_CACHE_HEADER& header)
{
  if (header.header_length < sizeof(VK_PIPELINE_CACHE_HEADER))
  {
    Log_ErrorPrintf("Pipeline cache failed validation: Invalid header length");
    return false;
  }

  if (header.header_version != VK_PIPELINE_CACHE_HEADER_VERSION_ONE)
  {
    Log_ErrorPrintf("Pipeline cache failed validation: Invalid header version");
    return false;
  }

  if (header.vendor_id != m_device_properties.vendorID)
  {
    Log_ErrorPrintf("Pipeline cache failed validation: Incorrect vendor ID (file: 0x%X, device: 0x%X)",
                    header.vendor_id, m_device_properties.vendorID);
    return false;
  }

  if (header.device_id != m_device_properties.deviceID)
  {
    Log_ErrorPrintf("Pipeline cache failed validation: Incorrect device ID (file: 0x%X, device: 0x%X)",
                    header.device_id, m_device_properties.deviceID);
    return false;
  }

  if (std::memcmp(header.uuid, m_device_properties.pipelineCacheUUID, VK_UUID_SIZE) != 0)
  {
    Log_ErrorPrintf("Pipeline cache failed validation: Incorrect UUID");
    return false;
  }

  return true;
}

void VulkanDevice::FillPipelineCacheHeader(VK_PIPELINE_CACHE_HEADER* header)
{
  header->header_length = sizeof(VK_PIPELINE_CACHE_HEADER);
  header->header_version = VK_PIPELINE_CACHE_HEADER_VERSION_ONE;
  header->vendor_id = m_device_properties.vendorID;
  header->device_id = m_device_properties.deviceID;
  std::memcpy(header->uuid, m_device_properties.pipelineCacheUUID, VK_UUID_SIZE);
}

bool VulkanDevice::ReadPipelineCache(const std::string& filename)
{
  std::optional<std::vector<u8>> data;

  auto fp = FileSystem::OpenManagedCFile(filename.c_str(), "rb");
  if (fp)
  {
    data = FileSystem::ReadBinaryFile(fp.get());

    if (data.has_value())
    {
      if (data->size() < sizeof(VK_PIPELINE_CACHE_HEADER))
      {
        Log_ErrorPrintf("Pipeline cache at '%s' is too small", filename.c_str());
        return false;
      }

      VK_PIPELINE_CACHE_HEADER header;
      std::memcpy(&header, data->data(), sizeof(header));
      if (!ValidatePipelineCacheHeader(header))
        data.reset();
    }
  }

  const VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, nullptr, 0,
                                     data.has_value() ? data->size() : 0, data.has_value() ? data->data() : nullptr};
  VkResult res = vkCreatePipelineCache(m_device, &ci, nullptr, &m_pipeline_cache);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreatePipelineCache() failed: ");
    return false;
  }

  return true;
}

bool VulkanDevice::GetPipelineCacheData(DynamicHeapArray<u8>* data)
{
  if (m_pipeline_cache == VK_NULL_HANDLE)
    return false;

  size_t data_size;
  VkResult res = vkGetPipelineCacheData(m_device, m_pipeline_cache, &data_size, nullptr);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkGetPipelineCacheData() failed: ");
    return false;
  }

  data->resize(data_size);
  res = vkGetPipelineCacheData(m_device, m_pipeline_cache, &data_size, data->data());
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkGetPipelineCacheData() (2) failed: ");
    return false;
  }

  data->resize(data_size);
  return true;
}

bool VulkanDevice::UpdateWindow()
{
  DestroySurface();

  if (!AcquireWindow(false))
    return false;

  if (m_window_info.IsSurfaceless())
    return true;

  // make sure previous frames are presented
  if (InRenderPass())
    EndRenderPass();
  SubmitCommandBuffer(false);
  WaitForGPUIdle();

  VkSurfaceKHR surface = VulkanSwapChain::CreateVulkanSurface(m_instance, m_physical_device, &m_window_info);
  if (surface == VK_NULL_HANDLE)
  {
    Log_ErrorPrintf("Failed to create new surface for swap chain");
    return false;
  }

  m_swap_chain = VulkanSwapChain::Create(m_window_info, surface, m_sync_mode, m_exclusive_fullscreen_control);
  if (!m_swap_chain)
  {
    Log_ErrorPrintf("Failed to create swap chain");
    VulkanSwapChain::DestroyVulkanSurface(m_instance, &m_window_info, surface);
    return false;
  }

  m_window_info = m_swap_chain->GetWindowInfo();
  RenderBlankFrame();
  return true;
}

void VulkanDevice::ResizeWindow(s32 new_window_width, s32 new_window_height, float new_window_scale)
{
  if (!m_swap_chain)
    return;

  if (m_swap_chain->GetWidth() == static_cast<u32>(new_window_width) &&
      m_swap_chain->GetHeight() == static_cast<u32>(new_window_height))
  {
    // skip unnecessary resizes
    m_window_info.surface_scale = new_window_scale;
    return;
  }

  // make sure previous frames are presented
  WaitForGPUIdle();

  if (!m_swap_chain->ResizeSwapChain(new_window_width, new_window_height, new_window_scale))
  {
    // AcquireNextImage() will fail, and we'll recreate the surface.
    Log_ErrorPrintf("Failed to resize swap chain. Next present will fail.");
    return;
  }

  m_window_info = m_swap_chain->GetWindowInfo();
}

void VulkanDevice::DestroySurface()
{
  WaitForGPUIdle();
  m_swap_chain.reset();
}

bool VulkanDevice::SupportsTextureFormat(GPUTexture::Format format) const
{
  return (TEXTURE_FORMAT_MAPPING[static_cast<u8>(format)] != VK_FORMAT_UNDEFINED);
}

std::string VulkanDevice::GetDriverInfo() const
{
  std::string ret;
  const u32 api_version = m_device_properties.apiVersion;
  const u32 driver_version = m_device_properties.driverVersion;
  if (m_optional_extensions.vk_khr_driver_properties)
  {
    const VkPhysicalDeviceDriverProperties& props = m_device_driver_properties;
    ret = fmt::format(
      "Driver {}.{}.{}\nVulkan {}.{}.{}\nConformance Version {}.{}.{}.{}\n{}\n{}\n{}", VK_VERSION_MAJOR(driver_version),
      VK_VERSION_MINOR(driver_version), VK_VERSION_PATCH(driver_version), VK_API_VERSION_MAJOR(api_version),
      VK_API_VERSION_MINOR(api_version), VK_API_VERSION_PATCH(api_version), props.conformanceVersion.major,
      props.conformanceVersion.minor, props.conformanceVersion.subminor, props.conformanceVersion.patch,
      props.driverInfo, props.driverName, m_device_properties.deviceName);
  }
  else
  {
    ret =
      fmt::format("Driver {}.{}.{}\nVulkan {}.{}.{}\n{}", VK_VERSION_MAJOR(driver_version),
                  VK_VERSION_MINOR(driver_version), VK_VERSION_PATCH(driver_version), VK_API_VERSION_MAJOR(api_version),
                  VK_API_VERSION_MINOR(api_version), VK_API_VERSION_PATCH(api_version), m_device_properties.deviceName);
  }

  return ret;
}

void VulkanDevice::SetSyncMode(DisplaySyncMode mode)
{
  if (m_sync_mode == mode)
    return;

  const DisplaySyncMode prev_mode = m_sync_mode;
  m_sync_mode = mode;
  if (!m_swap_chain)
    return;

  // This swap chain should not be used by the current buffer, thus safe to destroy.
  WaitForGPUIdle();
  if (!m_swap_chain->SetSyncMode(mode))
  {
    // Try switching back to the old mode..
    if (!m_swap_chain->SetSyncMode(prev_mode))
    {
      Panic("Failed to reset old vsync mode after failure");
      m_swap_chain.reset();
    }
  }
}

bool VulkanDevice::BeginPresent(bool frame_skip)
{
  if (InRenderPass())
    EndRenderPass();

  if (frame_skip)
    return false;

  // If we're running surfaceless, kick the command buffer so we don't run out of descriptors.
  if (!m_swap_chain)
  {
    SubmitCommandBuffer(false);
    TrimTexturePool();
    return false;
  }

  // Previous frame needs to be presented before we can acquire the swap chain.
  WaitForPresentComplete();

  // Check if the device was lost.
  if (CheckLastSubmitFail())
  {
    Panic("Fixme"); // TODO
    TrimTexturePool();
    return false;
  }

  VkResult res = m_swap_chain->AcquireNextImage();
  if (res != VK_SUCCESS)
  {
    m_swap_chain->ReleaseCurrentImage();

    if (res == VK_SUBOPTIMAL_KHR || res == VK_ERROR_OUT_OF_DATE_KHR)
    {
      ResizeWindow(0, 0, m_window_info.surface_scale);
      res = m_swap_chain->AcquireNextImage();
    }
    else if (res == VK_ERROR_SURFACE_LOST_KHR)
    {
      Log_WarningPrintf("Surface lost, attempting to recreate");
      if (!m_swap_chain->RecreateSurface(m_window_info))
      {
        Log_ErrorPrintf("Failed to recreate surface after loss");
        SubmitCommandBuffer(false);
        TrimTexturePool();
        return false;
      }

      res = m_swap_chain->AcquireNextImage();
    }

    // This can happen when multiple resize events happen in quick succession.
    // In this case, just wait until the next frame to try again.
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
    {
      // Still submit the command buffer, otherwise we'll end up with several frames waiting.
      LOG_VULKAN_ERROR(res, "vkAcquireNextImageKHR() failed: ");
      SubmitCommandBuffer(false);
      TrimTexturePool();
      return false;
    }
  }

  BeginSwapChainRenderPass();
  return true;
}

void VulkanDevice::EndPresent()
{
  DebugAssert(InRenderPass() && m_num_current_render_targets == 0 && !m_current_depth_target);
  EndRenderPass();

  VkCommandBuffer cmdbuf = GetCurrentCommandBuffer();
  VulkanTexture::TransitionSubresourcesToLayout(cmdbuf, m_swap_chain->GetCurrentImage(), GPUTexture::Type::RenderTarget,
                                                0, 1, 0, 1, VulkanTexture::Layout::ColorAttachment,
                                                VulkanTexture::Layout::PresentSrc);
  SubmitCommandBuffer(m_swap_chain.get(), !m_swap_chain->IsPresentModeSynchronizing());
  MoveToNextCommandBuffer();
  InvalidateCachedState();
  TrimTexturePool();
}

#ifdef _DEBUG
static std::array<float, 3> Palette(float phase, const std::array<float, 3>& a, const std::array<float, 3>& b,
                                    const std::array<float, 3>& c, const std::array<float, 3>& d)
{
  std::array<float, 3> result;
  result[0] = a[0] + b[0] * std::cos(6.28318f * (c[0] * phase + d[0]));
  result[1] = a[1] + b[1] * std::cos(6.28318f * (c[1] * phase + d[1]));
  result[2] = a[2] + b[2] * std::cos(6.28318f * (c[2] * phase + d[2]));
  return result;
}
#endif

void VulkanDevice::PushDebugGroup(const char* name)
{
#ifdef _DEBUG
  if (!vkCmdBeginDebugUtilsLabelEXT || !m_debug_device)
    return;

  const std::array<float, 3> color = Palette(static_cast<float>(++s_debug_scope_depth), {0.5f, 0.5f, 0.5f},
                                             {0.5f, 0.5f, 0.5f}, {1.0f, 1.0f, 0.5f}, {0.8f, 0.90f, 0.30f});

  const VkDebugUtilsLabelEXT label = {
    VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
    nullptr,
    name,
    {color[0], color[1], color[2], 1.0f},
  };
  vkCmdBeginDebugUtilsLabelEXT(GetCurrentCommandBuffer(), &label);
#endif
}

void VulkanDevice::PopDebugGroup()
{
#ifdef _DEBUG
  if (!vkCmdEndDebugUtilsLabelEXT || !m_debug_device)
    return;

  s_debug_scope_depth = (s_debug_scope_depth == 0) ? 0 : (s_debug_scope_depth - 1u);

  vkCmdEndDebugUtilsLabelEXT(GetCurrentCommandBuffer());
#endif
}

void VulkanDevice::InsertDebugMessage(const char* msg)
{
#ifdef _DEBUG
  if (!vkCmdInsertDebugUtilsLabelEXT || !m_debug_device)
    return;

  const VkDebugUtilsLabelEXT label = {VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT, nullptr, msg, {0.0f, 0.0f, 0.0f, 1.0f}};
  vkCmdInsertDebugUtilsLabelEXT(GetCurrentCommandBuffer(), &label);
#endif
}

bool VulkanDevice::CheckFeatures(FeatureMask disabled_features)
{
  m_max_texture_size = m_device_properties.limits.maxImageDimension2D;

  VkImageFormatProperties color_properties = {};
  vkGetPhysicalDeviceImageFormatProperties(m_physical_device, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
                                           VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0,
                                           &color_properties);
  VkImageFormatProperties depth_properties = {};
  vkGetPhysicalDeviceImageFormatProperties(m_physical_device, VK_FORMAT_D32_SFLOAT, VK_IMAGE_TYPE_2D,
                                           VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0,
                                           &depth_properties);
  const VkSampleCountFlags combined_properties = m_device_properties.limits.framebufferColorSampleCounts &
                                                 m_device_properties.limits.framebufferDepthSampleCounts &
                                                 color_properties.sampleCounts & depth_properties.sampleCounts;
  if (combined_properties & VK_SAMPLE_COUNT_64_BIT)
    m_max_multisamples = 64;
  else if (combined_properties & VK_SAMPLE_COUNT_32_BIT)
    m_max_multisamples = 32;
  else if (combined_properties & VK_SAMPLE_COUNT_16_BIT)
    m_max_multisamples = 16;
  else if (combined_properties & VK_SAMPLE_COUNT_8_BIT)
    m_max_multisamples = 8;
  else if (combined_properties & VK_SAMPLE_COUNT_4_BIT)
    m_max_multisamples = 4;
  else if (combined_properties & VK_SAMPLE_COUNT_2_BIT)
    m_max_multisamples = 2;
  else
    m_max_multisamples = 1;

  m_features.dual_source_blend =
    !(disabled_features & FEATURE_MASK_DUAL_SOURCE_BLEND) && m_device_features.dualSrcBlend;
  m_features.framebuffer_fetch = /*!(disabled_features & FEATURE_MASK_FRAMEBUFFER_FETCH) && */ false;

  if (!m_features.dual_source_blend)
    Log_WarningPrintf("Vulkan driver is missing dual-source blending. This will have an impact on performance.");

  m_features.noperspective_interpolation = true;
  m_features.texture_copy_to_self = !(disabled_features & FEATURE_MASK_TEXTURE_COPY_TO_SELF);
  m_features.per_sample_shading = m_device_features.sampleRateShading;
  m_features.supports_texture_buffers = !(disabled_features & FEATURE_MASK_TEXTURE_BUFFERS);

#ifdef __APPLE__
  // Partial texture buffer uploads appear to be broken in macOS/MoltenVK.
  m_features.texture_buffers_emulated_with_ssbo = true;
#else
  const u32 max_texel_buffer_elements = m_device_properties.limits.maxTexelBufferElements;
  Log_InfoPrintf("Max texel buffer elements: %u", max_texel_buffer_elements);
  if (max_texel_buffer_elements < MIN_TEXEL_BUFFER_ELEMENTS)
  {
    m_features.texture_buffers_emulated_with_ssbo = true;
  }
#endif

  if (m_features.texture_buffers_emulated_with_ssbo)
    Log_WarningPrintf("Emulating texture buffers with SSBOs.");

  m_features.geometry_shaders =
    !(disabled_features & FEATURE_MASK_GEOMETRY_SHADERS) && m_device_features.geometryShader;

  m_features.partial_msaa_resolve = true;
  m_features.memory_import = m_optional_extensions.vk_ext_external_memory_host;
  m_features.shader_cache = true;
  m_features.pipeline_cache = true;
  m_features.prefer_unused_textures = true;

  return true;
}

void VulkanDevice::CopyTextureRegion(GPUTexture* dst, u32 dst_x, u32 dst_y, u32 dst_layer, u32 dst_level,
                                     GPUTexture* src, u32 src_x, u32 src_y, u32 src_layer, u32 src_level, u32 width,
                                     u32 height)
{
  VulkanTexture* const S = static_cast<VulkanTexture*>(src);
  VulkanTexture* const D = static_cast<VulkanTexture*>(dst);

  if (S->GetState() == GPUTexture::State::Cleared)
  {
    // source is cleared. if destination is a render target, we can carry the clear forward
    if (D->IsRenderTargetOrDepthStencil())
    {
      if (dst_level == 0 && dst_x == 0 && dst_y == 0 && width == D->GetWidth() && height == D->GetHeight())
      {
        // pass it forward if we're clearing the whole thing
        if (S->IsDepthStencil())
          D->SetClearDepth(S->GetClearDepth());
        else
          D->SetClearColor(S->GetClearColor());

        return;
      }

      if (D->GetState() == GPUTexture::State::Cleared)
      {
        // destination is cleared, if it's the same colour and rect, we can just avoid this entirely
        if (D->IsDepthStencil())
        {
          if (D->GetClearDepth() == S->GetClearDepth())
            return;
        }
        else
        {
          if (D->GetClearColor() == S->GetClearColor())
            return;
        }
      }

      // TODO: Could use attachment clear here..
    }

    // commit the clear to the source first, then do normal copy
    S->CommitClear();
  }

  // if the destination has been cleared, and we're not overwriting the whole thing, commit the clear first
  // (the area outside of where we're copying to)
  if (D->GetState() == GPUTexture::State::Cleared &&
      (dst_level != 0 || dst_x != 0 || dst_y != 0 || width != D->GetWidth() || height != D->GetHeight()))
  {
    D->CommitClear();
  }

  // *now* we can do a normal image copy.
  const VkImageAspectFlags src_aspect = (S->IsDepthStencil()) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
  const VkImageAspectFlags dst_aspect = (D->IsDepthStencil()) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
  const VkImageCopy ic = {{src_aspect, src_level, src_layer, 1u},
                          {static_cast<s32>(src_x), static_cast<s32>(src_y), 0},
                          {dst_aspect, dst_level, dst_layer, 1u},
                          {static_cast<s32>(dst_x), static_cast<s32>(dst_y), 0},
                          {static_cast<u32>(width), static_cast<u32>(height), 1u}};

  if (InRenderPass())
    EndRenderPass();

  s_stats.num_copies++;

  S->SetUseFenceCounter(GetCurrentFenceCounter());
  D->SetUseFenceCounter(GetCurrentFenceCounter());
  S->TransitionToLayout((D == S) ? VulkanTexture::Layout::TransferSelf : VulkanTexture::Layout::TransferSrc);
  D->TransitionToLayout((D == S) ? VulkanTexture::Layout::TransferSelf : VulkanTexture::Layout::TransferDst);

  vkCmdCopyImage(GetCurrentCommandBuffer(), S->GetImage(), S->GetVkLayout(), D->GetImage(), D->GetVkLayout(), 1, &ic);

  D->SetState(GPUTexture::State::Dirty);
}

void VulkanDevice::ResolveTextureRegion(GPUTexture* dst, u32 dst_x, u32 dst_y, u32 dst_layer, u32 dst_level,
                                        GPUTexture* src, u32 src_x, u32 src_y, u32 width, u32 height)
{
  DebugAssert((src_x + width) <= src->GetWidth());
  DebugAssert((src_y + height) <= src->GetHeight());
  DebugAssert(src->IsMultisampled());
  DebugAssert(dst_level < dst->GetLevels() && dst_layer < dst->GetLayers());
  DebugAssert((dst_x + width) <= dst->GetMipWidth(dst_level));
  DebugAssert((dst_y + height) <= dst->GetMipHeight(dst_level));
  DebugAssert(!dst->IsMultisampled() && src->IsMultisampled());

  if (InRenderPass())
    EndRenderPass();

  s_stats.num_copies++;

  VulkanTexture* D = static_cast<VulkanTexture*>(dst);
  VulkanTexture* S = static_cast<VulkanTexture*>(src);
  const VkCommandBuffer cmdbuf = GetCurrentCommandBuffer();

  if (S->GetState() == GPUTexture::State::Cleared)
    S->CommitClear(cmdbuf);
  if (D->IsRenderTargetOrDepthStencil() && D->GetState() == GPUTexture::State::Cleared)
  {
    if (width < dst->GetWidth() || height < dst->GetHeight())
      D->CommitClear(cmdbuf);
    else
      D->SetState(GPUTexture::State::Dirty);
  }

  S->TransitionSubresourcesToLayout(cmdbuf, 0, 1, 0, 1, S->GetLayout(), VulkanTexture::Layout::TransferSrc);
  D->TransitionSubresourcesToLayout(cmdbuf, dst_layer, 1, dst_level, 1, D->GetLayout(),
                                    VulkanTexture::Layout::TransferDst);

  const VkImageResolve resolve = {{VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u},
                                  {static_cast<s32>(src_x), static_cast<s32>(src_y), 0},
                                  {VK_IMAGE_ASPECT_COLOR_BIT, dst_level, dst_layer, 1u},
                                  {static_cast<s32>(dst_x), static_cast<s32>(dst_y), 0},
                                  {width, height, 1}};
  vkCmdResolveImage(cmdbuf, S->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, D->GetImage(),
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &resolve);

  S->TransitionSubresourcesToLayout(cmdbuf, 0, 1, 0, 1, VulkanTexture::Layout::TransferSrc, S->GetLayout());
  D->TransitionSubresourcesToLayout(cmdbuf, dst_layer, 1, dst_level, 1, VulkanTexture::Layout::TransferDst,
                                    D->GetLayout());
}

void VulkanDevice::ClearRenderTarget(GPUTexture* t, u32 c)
{
  GPUDevice::ClearRenderTarget(t, c);
  if (InRenderPass() && IsRenderTargetBound(t))
    EndRenderPass();
}

void VulkanDevice::ClearDepth(GPUTexture* t, float d)
{
  GPUDevice::ClearDepth(t, d);
  if (InRenderPass() && m_current_depth_target == t)
    EndRenderPass();
}

void VulkanDevice::InvalidateRenderTarget(GPUTexture* t)
{
  GPUDevice::InvalidateRenderTarget(t);
  if (InRenderPass() && (t->IsRenderTarget() ? IsRenderTargetBound(t) : (m_current_depth_target == t)))
    EndRenderPass();
}

bool VulkanDevice::CreateBuffers()
{
  if (!m_vertex_buffer.Create(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VERTEX_BUFFER_SIZE))
  {
    Log_ErrorPrint("Failed to allocate vertex buffer");
    return false;
  }

  if (!m_index_buffer.Create(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, INDEX_BUFFER_SIZE))
  {
    Log_ErrorPrint("Failed to allocate index buffer");
    return false;
  }

  if (!m_uniform_buffer.Create(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VERTEX_UNIFORM_BUFFER_SIZE))
  {
    Log_ErrorPrint("Failed to allocate uniform buffer");
    return false;
  }

  if (!m_texture_upload_buffer.Create(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, TEXTURE_BUFFER_SIZE))
  {
    Log_ErrorPrint("Failed to allocate texture upload buffer");
    return false;
  }

  return true;
}

void VulkanDevice::DestroyBuffers()
{
  m_texture_upload_buffer.Destroy(false);
  m_uniform_buffer.Destroy(false);
  m_index_buffer.Destroy(false);
  m_vertex_buffer.Destroy(false);
}

void VulkanDevice::MapVertexBuffer(u32 vertex_size, u32 vertex_count, void** map_ptr, u32* map_space,
                                   u32* map_base_vertex)
{
  const u32 req_size = vertex_size * vertex_count;
  if (!m_vertex_buffer.ReserveMemory(req_size, vertex_size))
  {
    SubmitCommandBufferAndRestartRenderPass("out of vertex space");
    if (!m_vertex_buffer.ReserveMemory(req_size, vertex_size))
      Panic("Failed to allocate vertex space");
  }

  *map_ptr = m_vertex_buffer.GetCurrentHostPointer();
  *map_space = m_vertex_buffer.GetCurrentSpace() / vertex_size;
  *map_base_vertex = m_vertex_buffer.GetCurrentOffset() / vertex_size;
}

void VulkanDevice::UnmapVertexBuffer(u32 vertex_size, u32 vertex_count)
{
  const u32 size = vertex_size * vertex_count;
  s_stats.buffer_streamed += size;
  m_vertex_buffer.CommitMemory(size);
}

void VulkanDevice::MapIndexBuffer(u32 index_count, DrawIndex** map_ptr, u32* map_space, u32* map_base_index)
{
  const u32 req_size = sizeof(DrawIndex) * index_count;
  if (!m_index_buffer.ReserveMemory(req_size, sizeof(DrawIndex)))
  {
    SubmitCommandBufferAndRestartRenderPass("out of index space");
    if (!m_index_buffer.ReserveMemory(req_size, sizeof(DrawIndex)))
      Panic("Failed to allocate index space");
  }

  *map_ptr = reinterpret_cast<DrawIndex*>(m_index_buffer.GetCurrentHostPointer());
  *map_space = m_index_buffer.GetCurrentSpace() / sizeof(DrawIndex);
  *map_base_index = m_index_buffer.GetCurrentOffset() / sizeof(DrawIndex);
}

void VulkanDevice::UnmapIndexBuffer(u32 used_index_count)
{
  const u32 size = sizeof(DrawIndex) * used_index_count;
  s_stats.buffer_streamed += size;
  m_index_buffer.CommitMemory(size);
}

void VulkanDevice::PushUniformBuffer(const void* data, u32 data_size)
{
  DebugAssert(data_size < UNIFORM_PUSH_CONSTANTS_SIZE);
  s_stats.buffer_streamed += data_size;
  vkCmdPushConstants(GetCurrentCommandBuffer(), GetCurrentVkPipelineLayout(), UNIFORM_PUSH_CONSTANTS_STAGES, 0,
                     data_size, data);
}

void* VulkanDevice::MapUniformBuffer(u32 size)
{
  const u32 align = static_cast<u32>(m_device_properties.limits.minUniformBufferOffsetAlignment);
  const u32 used_space = Common::AlignUpPow2(size, align);
  if (!m_uniform_buffer.ReserveMemory(used_space + MAX_UNIFORM_BUFFER_SIZE, align))
  {
    SubmitCommandBufferAndRestartRenderPass("out of uniform space");
    if (!m_uniform_buffer.ReserveMemory(used_space + MAX_UNIFORM_BUFFER_SIZE, align))
      Panic("Failed to allocate uniform space.");
  }

  return m_uniform_buffer.GetCurrentHostPointer();
}

void VulkanDevice::UnmapUniformBuffer(u32 size)
{
  s_stats.buffer_streamed += size;
  m_uniform_buffer_position = m_uniform_buffer.GetCurrentOffset();
  m_uniform_buffer.CommitMemory(size);
  m_dirty_flags |= DIRTY_FLAG_DYNAMIC_OFFSETS;
}

bool VulkanDevice::CreateNullTexture()
{
  m_null_texture = VulkanTexture::Create(1, 1, 1, 1, 1, GPUTexture::Type::RenderTarget, GPUTexture::Format::RGBA8,
                                         VK_FORMAT_R8G8B8A8_UNORM);
  if (!m_null_texture)
    return false;

  const VkCommandBuffer cmdbuf = GetCurrentCommandBuffer();
  const VkImageSubresourceRange srr{VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
  const VkClearColorValue ccv{};
  m_null_texture->TransitionToLayout(cmdbuf, VulkanTexture::Layout::ClearDst);
  vkCmdClearColorImage(cmdbuf, m_null_texture->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &ccv, 1, &srr);
  m_null_texture->TransitionToLayout(cmdbuf, VulkanTexture::Layout::General);
  Vulkan::SetObjectName(m_device, m_null_texture->GetImage(), "Null texture");
  Vulkan::SetObjectName(m_device, m_null_texture->GetView(), "Null texture view");

  // Bind null texture and point sampler state to all.
  const VkSampler point_sampler = GetSampler(GPUSampler::GetNearestConfig());
  if (point_sampler == VK_NULL_HANDLE)
    return false;

  for (u32 i = 0; i < MAX_TEXTURE_SAMPLERS; i++)
  {
    m_current_textures[i] = m_null_texture.get();
    m_current_samplers[i] = point_sampler;
  }

  return true;
}

bool VulkanDevice::CreatePipelineLayouts()
{
  Vulkan::DescriptorSetLayoutBuilder dslb;
  Vulkan::PipelineLayoutBuilder plb;

  {
    dslb.AddBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    if ((m_ubo_ds_layout = dslb.Create(m_device)) == VK_NULL_HANDLE)
      return false;
    Vulkan::SetObjectName(m_device, m_ubo_ds_layout, "UBO Descriptor Set Layout");
  }

  {
    dslb.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    if ((m_single_texture_ds_layout = dslb.Create(m_device)) == VK_NULL_HANDLE)
      return false;
    Vulkan::SetObjectName(m_device, m_single_texture_ds_layout, "Single Texture Descriptor Set Layout");
  }

  {
    dslb.AddBinding(0,
                    m_features.texture_buffers_emulated_with_ssbo ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER :
                                                                    VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
                    1, VK_SHADER_STAGE_FRAGMENT_BIT);
    if ((m_single_texture_buffer_ds_layout = dslb.Create(m_device)) == VK_NULL_HANDLE)
      return false;
    Vulkan::SetObjectName(m_device, m_single_texture_buffer_ds_layout, "Texture Buffer Descriptor Set Layout");
  }

  {
    if (m_optional_extensions.vk_khr_push_descriptor)
      dslb.SetPushFlag();
    for (u32 i = 0; i < MAX_TEXTURE_SAMPLERS; i++)
      dslb.AddBinding(i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    if ((m_multi_texture_ds_layout = dslb.Create(m_device)) == VK_NULL_HANDLE)
      return false;
    Vulkan::SetObjectName(m_device, m_multi_texture_ds_layout, "Multi Texture Descriptor Set Layout");
  }

  {
    VkPipelineLayout& pl = m_pipeline_layouts[static_cast<u8>(GPUPipeline::Layout::SingleTextureAndUBO)];
    plb.AddDescriptorSet(m_ubo_ds_layout);
    plb.AddDescriptorSet(m_single_texture_ds_layout);
    if ((pl = plb.Create(m_device)) == VK_NULL_HANDLE)
      return false;
    Vulkan::SetObjectName(m_device, pl, "Single Texture + UBO Pipeline Layout");
  }

  {
    VkPipelineLayout& pl = m_pipeline_layouts[static_cast<u8>(GPUPipeline::Layout::SingleTextureAndPushConstants)];
    plb.AddDescriptorSet(m_single_texture_ds_layout);
    plb.AddPushConstants(UNIFORM_PUSH_CONSTANTS_STAGES, 0, UNIFORM_PUSH_CONSTANTS_SIZE);
    if ((pl = plb.Create(m_device)) == VK_NULL_HANDLE)
      return false;
    Vulkan::SetObjectName(m_device, pl, "Single Texture Pipeline Layout");
  }

  {
    VkPipelineLayout& pl =
      m_pipeline_layouts[static_cast<u8>(GPUPipeline::Layout::SingleTextureBufferAndPushConstants)];
    plb.AddDescriptorSet(m_single_texture_buffer_ds_layout);
    plb.AddPushConstants(UNIFORM_PUSH_CONSTANTS_STAGES, 0, UNIFORM_PUSH_CONSTANTS_SIZE);
    if ((pl = plb.Create(m_device)) == VK_NULL_HANDLE)
      return false;
    Vulkan::SetObjectName(m_device, pl, "Single Texture Buffer + UBO Pipeline Layout");
  }

  {
    VkPipelineLayout& pl = m_pipeline_layouts[static_cast<u8>(GPUPipeline::Layout::MultiTextureAndUBO)];
    plb.AddDescriptorSet(m_ubo_ds_layout);
    plb.AddDescriptorSet(m_multi_texture_ds_layout);
    if ((pl = plb.Create(m_device)) == VK_NULL_HANDLE)
      return false;
    Vulkan::SetObjectName(m_device, pl, "Multi Texture + UBO Pipeline Layout");
  }

  {
    VkPipelineLayout& pl = m_pipeline_layouts[static_cast<u8>(GPUPipeline::Layout::MultiTextureAndPushConstants)];
    plb.AddDescriptorSet(m_multi_texture_ds_layout);
    plb.AddPushConstants(UNIFORM_PUSH_CONSTANTS_STAGES, 0, UNIFORM_PUSH_CONSTANTS_SIZE);
    if ((pl = plb.Create(m_device)) == VK_NULL_HANDLE)
      return false;
    Vulkan::SetObjectName(m_device, pl, "Multi Texture Pipeline Layout");
  }

  return true;
}

void VulkanDevice::DestroyPipelineLayouts()
{
  for (VkPipelineLayout& pl : m_pipeline_layouts)
  {
    if (pl != VK_NULL_HANDLE)
    {
      vkDestroyPipelineLayout(m_device, pl, nullptr);
      pl = VK_NULL_HANDLE;
    }
  }

  auto destroy_dsl = [this](VkDescriptorSetLayout& l) {
    if (l != VK_NULL_HANDLE)
    {
      vkDestroyDescriptorSetLayout(m_device, l, nullptr);
      l = VK_NULL_HANDLE;
    }
  };
  destroy_dsl(m_multi_texture_ds_layout);
  destroy_dsl(m_single_texture_buffer_ds_layout);
  destroy_dsl(m_single_texture_ds_layout);
  destroy_dsl(m_ubo_ds_layout);
}

bool VulkanDevice::CreatePersistentDescriptorSets()
{
  Vulkan::DescriptorSetUpdateBuilder dsub;

  // TODO: is this a bad thing? choosing an upper bound.. so long as it's not going to fetch all of it :/
  m_ubo_descriptor_set = AllocatePersistentDescriptorSet(m_ubo_ds_layout);
  if (m_ubo_descriptor_set == VK_NULL_HANDLE)
    return false;
  dsub.AddBufferDescriptorWrite(m_ubo_descriptor_set, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                                m_uniform_buffer.GetBuffer(), 0, MAX_UNIFORM_BUFFER_SIZE);
  dsub.Update(m_device, false);

  return true;
}

void VulkanDevice::DestroyPersistentDescriptorSets()
{
  if (m_ubo_descriptor_set != VK_NULL_HANDLE)
    FreePersistentDescriptorSet(m_ubo_descriptor_set);
}

void VulkanDevice::RenderBlankFrame()
{
  VkResult res = m_swap_chain->AcquireNextImage();
  if (res != VK_SUCCESS)
  {
    Log_ErrorPrintf("Failed to acquire image for blank frame present");
    return;
  }

  VkCommandBuffer cmdbuf = GetCurrentCommandBuffer();

  const VkImage image = m_swap_chain->GetCurrentImage();
  static constexpr VkImageSubresourceRange srr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VulkanTexture::TransitionSubresourcesToLayout(cmdbuf, image, GPUTexture::Type::RenderTarget, 0, 1, 0, 1,
                                                VulkanTexture::Layout::Undefined, VulkanTexture::Layout::TransferDst);
  vkCmdClearColorImage(cmdbuf, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &s_present_clear_color.color, 1, &srr);
  VulkanTexture::TransitionSubresourcesToLayout(cmdbuf, image, GPUTexture::Type::RenderTarget, 0, 1, 0, 1,
                                                VulkanTexture::Layout::TransferDst, VulkanTexture::Layout::PresentSrc);

  SubmitCommandBuffer(m_swap_chain.get(), !m_swap_chain->IsPresentModeSynchronizing());
  MoveToNextCommandBuffer();

  InvalidateCachedState();
}

bool VulkanDevice::TryImportHostMemory(void* data, size_t data_size, VkBufferUsageFlags buffer_usage,
                                       VkDeviceMemory* out_memory, VkBuffer* out_buffer, VkDeviceSize* out_offset)
{
  if (!m_optional_extensions.vk_ext_external_memory_host)
    return false;

  // Align to the nearest page
  void* data_aligned =
    reinterpret_cast<void*>(Common::AlignDownPow2(reinterpret_cast<uintptr_t>(data), HOST_PAGE_SIZE));

  // Offset to the start of the data within the page
  const size_t data_offset = reinterpret_cast<uintptr_t>(data) & static_cast<uintptr_t>(HOST_PAGE_MASK);

  // Full amount of data that must be imported, including the pages
  const size_t data_size_aligned = Common::AlignUpPow2(data_offset + data_size, HOST_PAGE_SIZE);

  VkMemoryHostPointerPropertiesEXT pointer_properties = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT, nullptr,
                                                         0};
  VkResult res = vkGetMemoryHostPointerPropertiesEXT(m_device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                                     data_aligned, &pointer_properties);
  if (res != VK_SUCCESS || pointer_properties.memoryTypeBits == 0)
  {
    LOG_VULKAN_ERROR(res, "vkGetMemoryHostPointerPropertiesEXT() failed: ");
    return false;
  }

  VmaAllocationCreateInfo vma_alloc_info = {};
  vma_alloc_info.preferredFlags =
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
  vma_alloc_info.memoryTypeBits = pointer_properties.memoryTypeBits;

  u32 memory_index = 0;
  res = vmaFindMemoryTypeIndex(m_allocator, pointer_properties.memoryTypeBits, &vma_alloc_info, &memory_index);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vmaFindMemoryTypeIndex() failed: ");
    return false;
  }

  const VkImportMemoryHostPointerInfoEXT import_info = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, nullptr,
                                                        VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                                        const_cast<void*>(data_aligned)};

  const VkMemoryAllocateInfo alloc_info = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &import_info, data_size_aligned,
                                           memory_index};

  VkDeviceMemory imported_memory = VK_NULL_HANDLE;

  res = vkAllocateMemory(m_device, &alloc_info, nullptr, &imported_memory);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkAllocateMemory() failed: ");
    return false;
  }

  const VkExternalMemoryBufferCreateInfo external_info = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, nullptr,
                                                          VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};

  const VkBufferCreateInfo buffer_info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                          &external_info,
                                          0,
                                          data_size_aligned,
                                          buffer_usage,
                                          VK_SHARING_MODE_EXCLUSIVE,
                                          0,
                                          nullptr};

  VkBuffer imported_buffer = VK_NULL_HANDLE;
  res = vkCreateBuffer(m_device, &buffer_info, nullptr, &imported_buffer);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateBuffer() failed: ");
    if (imported_memory != VK_NULL_HANDLE)
      vkFreeMemory(m_device, imported_memory, nullptr);

    return false;
  }

  vkBindBufferMemory(m_device, imported_buffer, imported_memory, 0);

  *out_memory = imported_memory;
  *out_buffer = imported_buffer;
  *out_offset = data_offset;
  Log_DevFmt("Imported {} byte buffer covering {} bytes at {}", data_size, data_size_aligned, data);
  return true;
}

void VulkanDevice::SetRenderTargets(GPUTexture* const* rts, u32 num_rts, GPUTexture* ds)
{
  bool changed = (m_num_current_render_targets != num_rts || m_current_depth_target != ds);
  bool needs_ds_clear = (ds && ds->IsClearedOrInvalidated());
  bool needs_rt_clear = false;

  m_current_depth_target = ds;
  for (u32 i = 0; i < num_rts; i++)
  {
    VulkanTexture* const RT = static_cast<VulkanTexture*>(rts[i]);
    changed |= m_current_render_targets[i] != RT;
    m_current_render_targets[i] = RT;
    needs_rt_clear |= RT->IsClearedOrInvalidated();
  }
  for (u32 i = num_rts; i < m_num_current_render_targets; i++)
    m_current_render_targets[i] = nullptr;
  m_num_current_render_targets = num_rts;

  if (changed)
  {
    if (InRenderPass())
      EndRenderPass();

    if (m_num_current_render_targets == 0 && !m_current_depth_target)
    {
      m_current_framebuffer = VK_NULL_HANDLE;
      return;
    }

    if (!m_optional_extensions.vk_khr_dynamic_rendering)
    {
      m_current_framebuffer =
        m_framebuffer_manager.Lookup((m_num_current_render_targets > 0) ? m_current_render_targets.data() : nullptr,
                                     m_num_current_render_targets, m_current_depth_target, 0);
      if (m_current_framebuffer == VK_NULL_HANDLE)
      {
        Log_ErrorPrint("Failed to create framebuffer");
        return;
      }
    }
  }

  // TODO: This could use vkCmdClearAttachments() instead.
  if (needs_rt_clear || needs_ds_clear)
  {
    if (InRenderPass())
      EndRenderPass();
  }
}

void VulkanDevice::BeginRenderPass()
{
  // TODO: Stats
  DebugAssert(!InRenderPass());

  // All textures should be in shader read only optimal already, but just in case..
  const u32 num_textures = GetActiveTexturesForLayout(m_current_pipeline_layout);
  for (u32 i = 0; i < num_textures; i++)
    m_current_textures[i]->TransitionToLayout(VulkanTexture::Layout::ShaderReadOnly);

  if (m_optional_extensions.vk_khr_dynamic_rendering)
  {
    VkRenderingInfoKHR ri = {
      VK_STRUCTURE_TYPE_RENDERING_INFO_KHR, nullptr, 0u, {}, 1u, 0u, 0u, nullptr, nullptr, nullptr};

    std::array<VkRenderingAttachmentInfoKHR, MAX_RENDER_TARGETS> attachments;
    VkRenderingAttachmentInfoKHR depth_attachment;

    if (m_num_current_render_targets > 0 || m_current_depth_target)
    {
      ri.colorAttachmentCount = m_num_current_render_targets;
      ri.pColorAttachments = (m_num_current_render_targets > 0) ? attachments.data() : nullptr;

      // set up clear values and transition targets
      for (u32 i = 0; i < m_num_current_render_targets; i++)
      {
        VulkanTexture* const rt = static_cast<VulkanTexture*>(m_current_render_targets[i]);
        rt->TransitionToLayout(VulkanTexture::Layout::ColorAttachment);
        rt->SetUseFenceCounter(GetCurrentFenceCounter());

        VkRenderingAttachmentInfo& ai = attachments[i];
        ai.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
        ai.pNext = nullptr;
        ai.imageView = rt->GetView();
        ai.imageLayout = rt->GetVkLayout();
        ai.resolveMode = VK_RESOLVE_MODE_NONE_KHR;
        ai.resolveImageView = VK_NULL_HANDLE;
        ai.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ai.loadOp = GetLoadOpForTexture(rt);
        ai.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        if (rt->GetState() == GPUTexture::State::Cleared)
        {
          std::memcpy(ai.clearValue.color.float32, rt->GetUNormClearColor().data(),
                      sizeof(ai.clearValue.color.float32));
        }
        rt->SetState(GPUTexture::State::Dirty);
      }

      if (VulkanTexture* const ds = static_cast<VulkanTexture*>(m_current_depth_target))
      {
        ds->TransitionToLayout(VulkanTexture::Layout::DepthStencilAttachment);
        ds->SetUseFenceCounter(GetCurrentFenceCounter());

        depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
        depth_attachment.pNext = nullptr;
        depth_attachment.imageView = ds->GetView();
        depth_attachment.imageLayout = ds->GetVkLayout();
        depth_attachment.resolveMode = VK_RESOLVE_MODE_NONE_KHR;
        depth_attachment.resolveImageView = VK_NULL_HANDLE;
        depth_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depth_attachment.loadOp = GetLoadOpForTexture(ds);
        depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ri.pDepthAttachment = &depth_attachment;

        if (ds->GetState() == GPUTexture::State::Cleared)
          depth_attachment.clearValue.depthStencil = {ds->GetClearDepth(), 0u};

        ds->SetState(GPUTexture::State::Dirty);
      }

      const VulkanTexture* const rt_or_ds = static_cast<const VulkanTexture*>(
        (m_num_current_render_targets > 0) ? m_current_render_targets[0] : m_current_depth_target);
      ri.renderArea = {{}, {rt_or_ds->GetWidth(), rt_or_ds->GetHeight()}};
    }
    else
    {
      VkRenderingAttachmentInfo& ai = attachments[0];
      ai.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
      ai.pNext = nullptr;
      ai.imageView = m_swap_chain->GetCurrentImageView();
      ai.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      ai.resolveMode = VK_RESOLVE_MODE_NONE_KHR;
      ai.resolveImageView = VK_NULL_HANDLE;
      ai.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      ai.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      ai.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

      ri.colorAttachmentCount = 1;
      ri.pColorAttachments = attachments.data();
      ri.renderArea = {{}, {m_swap_chain->GetWidth(), m_swap_chain->GetHeight()}};
    }

    m_current_render_pass = DYNAMIC_RENDERING_RENDER_PASS;
    vkCmdBeginRenderingKHR(GetCurrentCommandBuffer(), &ri);
  }
  else
  {
    VkRenderPassBeginInfo bi = {
      VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr, VK_NULL_HANDLE, VK_NULL_HANDLE, {}, 0u, nullptr};
    std::array<VkClearValue, MAX_RENDER_TARGETS + 1> clear_values;

    if (m_current_framebuffer != VK_NULL_HANDLE)
    {
      bi.framebuffer = m_current_framebuffer;
      bi.renderPass = m_current_render_pass = GetRenderPass(
        m_current_render_targets.data(), m_num_current_render_targets, m_current_depth_target, false, false);
      if (bi.renderPass == VK_NULL_HANDLE)
      {
        Log_ErrorPrint("Failed to create render pass");
        return;
      }

      // set up clear values and transition targets
      for (u32 i = 0; i < m_num_current_render_targets; i++)
      {
        VulkanTexture* const rt = static_cast<VulkanTexture*>(m_current_render_targets[i]);
        if (rt->GetState() == GPUTexture::State::Cleared)
        {
          std::memcpy(clear_values[i].color.float32, rt->GetUNormClearColor().data(),
                      sizeof(clear_values[i].color.float32));
          bi.pClearValues = clear_values.data();
          bi.clearValueCount = i + 1;
        }
        rt->SetState(GPUTexture::State::Dirty);
        rt->TransitionToLayout(VulkanTexture::Layout::ColorAttachment);
        rt->SetUseFenceCounter(GetCurrentFenceCounter());
      }
      if (VulkanTexture* const ds = static_cast<VulkanTexture*>(m_current_depth_target))
      {
        if (ds->GetState() == GPUTexture::State::Cleared)
        {
          clear_values[m_num_current_render_targets].depthStencil = {ds->GetClearDepth(), 0u};
          bi.pClearValues = clear_values.data();
          bi.clearValueCount = m_num_current_render_targets + 1;
        }
        ds->SetState(GPUTexture::State::Dirty);
        ds->TransitionToLayout(VulkanTexture::Layout::DepthStencilAttachment);
        ds->SetUseFenceCounter(GetCurrentFenceCounter());
      }

      const VulkanTexture* const rt_or_ds = static_cast<const VulkanTexture*>(
        (m_num_current_render_targets > 0) ? m_current_render_targets[0] : m_current_depth_target);
      bi.renderArea.extent = {rt_or_ds->GetWidth(), rt_or_ds->GetHeight()};
    }
    else
    {
      // Re-rendering to swap chain.
      bi.framebuffer = m_swap_chain->GetCurrentFramebuffer();
      bi.renderPass = m_current_render_pass =
        GetSwapChainRenderPass(m_swap_chain->GetWindowInfo().surface_format, VK_ATTACHMENT_LOAD_OP_LOAD);
      bi.renderArea.extent = {m_swap_chain->GetWidth(), m_swap_chain->GetHeight()};
    }

    DebugAssert(m_current_render_pass);
    vkCmdBeginRenderPass(GetCurrentCommandBuffer(), &bi, VK_SUBPASS_CONTENTS_INLINE);
  }

  s_stats.num_render_passes++;

  // If this is a new command buffer, bind the pipeline and such.
  if (m_dirty_flags & DIRTY_FLAG_INITIAL)
    SetInitialPipelineState();
}

void VulkanDevice::BeginSwapChainRenderPass()
{
  DebugAssert(!InRenderPass());

  const VkCommandBuffer cmdbuf = GetCurrentCommandBuffer();
  const VkImage swap_chain_image = m_swap_chain->GetCurrentImage();

  // Swap chain images start in undefined
  VulkanTexture::TransitionSubresourcesToLayout(cmdbuf, swap_chain_image, GPUTexture::Type::RenderTarget, 0, 1, 0, 1,
                                                VulkanTexture::Layout::Undefined,
                                                VulkanTexture::Layout::ColorAttachment);

  // All textures should be in shader read only optimal already, but just in case..
  const u32 num_textures = GetActiveTexturesForLayout(m_current_pipeline_layout);
  for (u32 i = 0; i < num_textures; i++)
    m_current_textures[i]->TransitionToLayout(VulkanTexture::Layout::ShaderReadOnly);

  if (m_optional_extensions.vk_khr_dynamic_rendering)
  {
    const VkRenderingAttachmentInfo ai = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR,
                                          nullptr,
                                          m_swap_chain->GetCurrentImageView(),
                                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                          VK_RESOLVE_MODE_NONE_KHR,
                                          VK_NULL_HANDLE,
                                          VK_IMAGE_LAYOUT_UNDEFINED,
                                          VK_ATTACHMENT_LOAD_OP_CLEAR,
                                          VK_ATTACHMENT_STORE_OP_STORE,
                                          s_present_clear_color};

    const VkRenderingInfoKHR ri = {VK_STRUCTURE_TYPE_RENDERING_INFO_KHR,
                                   nullptr,
                                   0u,
                                   {{}, {m_swap_chain->GetWidth(), m_swap_chain->GetHeight()}},
                                   1u,
                                   0u,
                                   1u,
                                   &ai,
                                   nullptr,
                                   nullptr};

    m_current_render_pass = DYNAMIC_RENDERING_RENDER_PASS;
    vkCmdBeginRenderingKHR(GetCurrentCommandBuffer(), &ri);
  }
  else
  {
    m_current_render_pass =
      GetSwapChainRenderPass(m_swap_chain->GetWindowInfo().surface_format, VK_ATTACHMENT_LOAD_OP_CLEAR);
    DebugAssert(m_current_render_pass);

    const VkRenderPassBeginInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                      nullptr,
                                      m_current_render_pass,
                                      m_swap_chain->GetCurrentFramebuffer(),
                                      {{0, 0}, {m_swap_chain->GetWidth(), m_swap_chain->GetHeight()}},
                                      1u,
                                      &s_present_clear_color};
    vkCmdBeginRenderPass(GetCurrentCommandBuffer(), &rp, VK_SUBPASS_CONTENTS_INLINE);
  }

  s_stats.num_render_passes++;
  m_num_current_render_targets = 0;
  std::memset(m_current_render_targets.data(), 0, sizeof(m_current_render_targets));
  m_current_depth_target = nullptr;
  m_current_framebuffer = VK_NULL_HANDLE;

  // Clear pipeline, it's likely incompatible.
  m_current_pipeline = nullptr;
}

bool VulkanDevice::InRenderPass()
{
  return m_current_render_pass != VK_NULL_HANDLE;
}

void VulkanDevice::EndRenderPass()
{
  DebugAssert(m_current_render_pass != VK_NULL_HANDLE);

  // TODO: stats
  VkCommandBuffer cmdbuf = GetCurrentCommandBuffer();
  if (std::exchange(m_current_render_pass, VK_NULL_HANDLE) == DYNAMIC_RENDERING_RENDER_PASS)
    vkCmdEndRenderingKHR(cmdbuf);
  else
    vkCmdEndRenderPass(GetCurrentCommandBuffer());
}

void VulkanDevice::SetPipeline(GPUPipeline* pipeline)
{
  // First draw? Bind everything.
  if (m_dirty_flags & DIRTY_FLAG_INITIAL)
  {
    m_current_pipeline = static_cast<VulkanPipeline*>(pipeline);
    if (!m_current_pipeline)
      return;

    SetInitialPipelineState();
    return;
  }
  else if (m_current_pipeline == pipeline)
  {
    return;
  }

  m_current_pipeline = static_cast<VulkanPipeline*>(pipeline);

  vkCmdBindPipeline(m_current_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_current_pipeline->GetPipeline());

  if (m_current_pipeline_layout != m_current_pipeline->GetLayout())
  {
    m_current_pipeline_layout = m_current_pipeline->GetLayout();
    m_dirty_flags |= DIRTY_FLAG_PIPELINE_LAYOUT;
  }
}

void VulkanDevice::UnbindPipeline(VulkanPipeline* pl)
{
  if (m_current_pipeline != pl)
    return;

  m_current_pipeline = nullptr;
}

void VulkanDevice::InvalidateCachedState()
{
  m_dirty_flags = ALL_DIRTY_STATE;
  m_current_render_pass = VK_NULL_HANDLE;
  m_current_pipeline = nullptr;
}

bool VulkanDevice::IsRenderTargetBound(const GPUTexture* tex) const
{
  for (u32 i = 0; i < m_num_current_render_targets; i++)
  {
    if (m_current_render_targets[i] == tex)
      return true;
  }

  return false;
}

VkPipelineLayout VulkanDevice::GetCurrentVkPipelineLayout() const
{
  return m_pipeline_layouts[static_cast<u8>(m_current_pipeline_layout)];
}

void VulkanDevice::SetInitialPipelineState()
{
  DebugAssert(m_current_pipeline);
  m_dirty_flags &= ~DIRTY_FLAG_INITIAL;

  const VkDeviceSize offset = 0;
  const VkCommandBuffer cmdbuf = GetCurrentCommandBuffer();
  vkCmdBindVertexBuffers(cmdbuf, 0, 1, m_vertex_buffer.GetBufferPtr(), &offset);
  vkCmdBindIndexBuffer(cmdbuf, m_index_buffer.GetBuffer(), 0, VK_INDEX_TYPE_UINT16);

  m_current_pipeline_layout = m_current_pipeline->GetLayout();
  vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_current_pipeline->GetPipeline());

  const VkViewport vp = {static_cast<float>(m_current_viewport.left),
                         static_cast<float>(m_current_viewport.top),
                         static_cast<float>(m_current_viewport.GetWidth()),
                         static_cast<float>(m_current_viewport.GetHeight()),
                         0.0f,
                         1.0f};
  vkCmdSetViewport(GetCurrentCommandBuffer(), 0, 1, &vp);

  const VkRect2D vrc = {
    {m_current_scissor.left, m_current_scissor.top},
    {static_cast<u32>(m_current_scissor.GetWidth()), static_cast<u32>(m_current_scissor.GetHeight())}};
  vkCmdSetScissor(GetCurrentCommandBuffer(), 0, 1, &vrc);
}

void VulkanDevice::SetTextureSampler(u32 slot, GPUTexture* texture, GPUSampler* sampler)
{
  VulkanTexture* T = texture ? static_cast<VulkanTexture*>(texture) : m_null_texture.get();
  const VkSampler vsampler = static_cast<VulkanSampler*>(sampler ? sampler : m_nearest_sampler.get())->GetSampler();
  if (m_current_textures[slot] != texture || m_current_samplers[slot] != vsampler)
  {
    m_current_textures[slot] = T;
    m_current_samplers[slot] = vsampler;
    m_dirty_flags |= DIRTY_FLAG_TEXTURES_OR_SAMPLERS;
  }

  if (T)
  {
    T->CommitClear();
    T->SetUseFenceCounter(GetCurrentFenceCounter());
    if (T->GetLayout() != VulkanTexture::Layout::ShaderReadOnly)
    {
      if (InRenderPass())
        EndRenderPass();
      T->TransitionToLayout(VulkanTexture::Layout::ShaderReadOnly);
    }
  }
}

void VulkanDevice::SetTextureBuffer(u32 slot, GPUTextureBuffer* buffer)
{
  DebugAssert(slot == 0);
  if (m_current_texture_buffer == buffer)
    return;

  m_current_texture_buffer = static_cast<VulkanTextureBuffer*>(buffer);
  if (m_current_pipeline_layout == GPUPipeline::Layout::SingleTextureBufferAndPushConstants)
    m_dirty_flags |= DIRTY_FLAG_TEXTURES_OR_SAMPLERS;
}

void VulkanDevice::UnbindTexture(VulkanTexture* tex)
{
  for (u32 i = 0; i < MAX_TEXTURE_SAMPLERS; i++)
  {
    if (m_current_textures[i] == tex)
    {
      m_current_textures[i] = m_null_texture.get();
      m_dirty_flags |= DIRTY_FLAG_TEXTURES_OR_SAMPLERS;
    }
  }

  if (tex->IsRenderTarget())
  {
    for (u32 i = 0; i < m_num_current_render_targets; i++)
    {
      if (m_current_render_targets[i] == tex)
      {
        Log_WarningPrint("Unbinding current RT");
        SetRenderTargets(nullptr, 0, m_current_depth_target);
        break;
      }
    }

    m_framebuffer_manager.RemoveRTReferences(tex);
  }
  else if (tex->IsDepthStencil())
  {
    if (m_current_depth_target == tex)
    {
      Log_WarningPrint("Unbinding current DS");
      SetRenderTargets(nullptr, 0, nullptr);
    }

    m_framebuffer_manager.RemoveDSReferences(tex);
  }
}

void VulkanDevice::UnbindTextureBuffer(VulkanTextureBuffer* buf)
{
  if (m_current_texture_buffer != buf)
    return;

  m_current_texture_buffer = nullptr;

  if (m_current_pipeline_layout == GPUPipeline::Layout::SingleTextureBufferAndPushConstants)
    m_dirty_flags |= DIRTY_FLAG_TEXTURES_OR_SAMPLERS;
}

void VulkanDevice::SetViewport(s32 x, s32 y, s32 width, s32 height)
{
  const Common::Rectangle<s32> rc = Common::Rectangle<s32>::FromExtents(x, y, width, height);
  if (m_current_viewport == rc)
    return;

  m_current_viewport = rc;

  if (m_dirty_flags & DIRTY_FLAG_INITIAL)
    return;

  const VkViewport vp = {
    static_cast<float>(x), static_cast<float>(y), static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
  vkCmdSetViewport(GetCurrentCommandBuffer(), 0, 1, &vp);
}

void VulkanDevice::SetScissor(s32 x, s32 y, s32 width, s32 height)
{
  const Common::Rectangle<s32> rc = Common::Rectangle<s32>::FromExtents(x, y, width, height);
  if (m_current_scissor == rc)
    return;

  m_current_scissor = rc;

  if (m_dirty_flags & DIRTY_FLAG_INITIAL)
    return;

  const VkRect2D vrc = {{x, y}, {static_cast<u32>(width), static_cast<u32>(height)}};
  vkCmdSetScissor(GetCurrentCommandBuffer(), 0, 1, &vrc);
}

void VulkanDevice::PreDrawCheck()
{
  DebugAssert(!(m_dirty_flags & DIRTY_FLAG_INITIAL));
  const u32 dirty = std::exchange(m_dirty_flags, 0);
  if (dirty != 0)
  {
    if (dirty & (DIRTY_FLAG_PIPELINE_LAYOUT | DIRTY_FLAG_DYNAMIC_OFFSETS | DIRTY_FLAG_TEXTURES_OR_SAMPLERS))
    {
      if (!UpdateDescriptorSets(dirty))
      {
        SubmitCommandBufferAndRestartRenderPass("out of descriptor sets");
        PreDrawCheck();
        return;
      }
    }
  }

  if (!InRenderPass())
    BeginRenderPass();
}

template<GPUPipeline::Layout layout>
bool VulkanDevice::UpdateDescriptorSetsForLayout(bool new_layout, bool new_dynamic_offsets)
{
  std::array<VkDescriptorSet, 2> ds;
  u32 first_ds = 0;
  u32 num_ds = 0;

  if constexpr (layout == GPUPipeline::Layout::SingleTextureAndUBO || layout == GPUPipeline::Layout::MultiTextureAndUBO)
  {
    if (new_layout || new_dynamic_offsets)
    {
      ds[num_ds++] = m_ubo_descriptor_set;
      new_dynamic_offsets = true;
    }
    else
    {
      first_ds++;
    }
  }

  if constexpr (layout == GPUPipeline::Layout::SingleTextureAndUBO ||
                layout == GPUPipeline::Layout::SingleTextureAndPushConstants)
  {
    DebugAssert(m_current_textures[0] && m_current_samplers[0] != VK_NULL_HANDLE);
    ds[num_ds++] = m_current_textures[0]->GetDescriptorSetWithSampler(m_current_samplers[0]);
  }
  else if constexpr (layout == GPUPipeline::Layout::SingleTextureBufferAndPushConstants)
  {
    DebugAssert(m_current_texture_buffer);
    ds[num_ds++] = m_current_texture_buffer->GetDescriptorSet();
  }
  else if constexpr (layout == GPUPipeline::Layout::MultiTextureAndUBO ||
                     layout == GPUPipeline::Layout::MultiTextureAndPushConstants)
  {
    Vulkan::DescriptorSetUpdateBuilder dsub;

    if (m_optional_extensions.vk_khr_push_descriptor)
    {
      for (u32 i = 0; i < MAX_TEXTURE_SAMPLERS; i++)
      {
        DebugAssert(m_current_textures[i] && m_current_samplers[i] != VK_NULL_HANDLE);
        dsub.AddCombinedImageSamplerDescriptorWrite(VK_NULL_HANDLE, i, m_current_textures[i]->GetView(),
                                                    m_current_samplers[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      }

      const u32 set = (layout == GPUPipeline::Layout::MultiTextureAndUBO) ? 1 : 0;
      dsub.PushUpdate(GetCurrentCommandBuffer(), VK_PIPELINE_BIND_POINT_GRAPHICS,
                      m_pipeline_layouts[static_cast<u8>(m_current_pipeline_layout)], set);
      if (num_ds == 0)
        return true;
    }
    else
    {
      VkDescriptorSet tds = AllocateDescriptorSet(m_multi_texture_ds_layout);
      if (tds == VK_NULL_HANDLE)
        return false;

      ds[num_ds++] = tds;

      for (u32 i = 0; i < MAX_TEXTURE_SAMPLERS; i++)
      {
        DebugAssert(m_current_textures[i] && m_current_samplers[i] != VK_NULL_HANDLE);
        dsub.AddCombinedImageSamplerDescriptorWrite(tds, i, m_current_textures[i]->GetView(), m_current_samplers[i],
                                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      }

      dsub.Update(m_device, false);
    }
  }

  DebugAssert(num_ds > 0);
  vkCmdBindDescriptorSets(GetCurrentCommandBuffer(), VK_PIPELINE_BIND_POINT_GRAPHICS,
                          m_pipeline_layouts[static_cast<u8>(m_current_pipeline_layout)], first_ds, num_ds, ds.data(),
                          static_cast<u32>(new_dynamic_offsets),
                          new_dynamic_offsets ? &m_uniform_buffer_position : nullptr);

  return true;
}

bool VulkanDevice::UpdateDescriptorSets(u32 dirty)
{
  const bool new_layout = (dirty & DIRTY_FLAG_PIPELINE_LAYOUT) != 0;
  const bool new_dynamic_offsets = (dirty & DIRTY_FLAG_DYNAMIC_OFFSETS) != 0;

  switch (m_current_pipeline_layout)
  {
    case GPUPipeline::Layout::SingleTextureAndUBO:
      return UpdateDescriptorSetsForLayout<GPUPipeline::Layout::SingleTextureAndUBO>(new_layout, new_dynamic_offsets);

    case GPUPipeline::Layout::SingleTextureAndPushConstants:
      return UpdateDescriptorSetsForLayout<GPUPipeline::Layout::SingleTextureAndPushConstants>(new_layout, false);

    case GPUPipeline::Layout::SingleTextureBufferAndPushConstants:
      return UpdateDescriptorSetsForLayout<GPUPipeline::Layout::SingleTextureBufferAndPushConstants>(new_layout, false);

    case GPUPipeline::Layout::MultiTextureAndUBO:
      return UpdateDescriptorSetsForLayout<GPUPipeline::Layout::MultiTextureAndUBO>(new_layout, new_dynamic_offsets);

    case GPUPipeline::Layout::MultiTextureAndPushConstants:
      return UpdateDescriptorSetsForLayout<GPUPipeline::Layout::MultiTextureAndPushConstants>(new_layout, false);

    default:
      UnreachableCode();
  }
}

void VulkanDevice::Draw(u32 vertex_count, u32 base_vertex)
{
  PreDrawCheck();
  s_stats.num_draws++;
  vkCmdDraw(GetCurrentCommandBuffer(), vertex_count, 1, base_vertex, 0);
}

void VulkanDevice::DrawIndexed(u32 index_count, u32 base_index, u32 base_vertex)
{
  PreDrawCheck();
  s_stats.num_draws++;
  vkCmdDrawIndexed(GetCurrentCommandBuffer(), index_count, 1, base_index, base_vertex, 0);
}
