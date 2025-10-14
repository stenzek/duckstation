// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

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
#include "common/heap_array.h"
#include "common/log.h"
#include "common/path.h"
#include "common/scoped_guard.h"
#include "common/small_string.h"

#include "fmt/format.h"
#include "xxhash.h"

#ifdef ENABLE_SDL
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#endif

#include <cstdlib>
#include <limits>
#include <mutex>

LOG_CHANNEL(GPUDevice);

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
  MAX_INPUT_ATTACHMENT_DESCRIPTORS_PER_FRAME = MAX_DRAW_CALLS_PER_FRAME,
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
  VK_FORMAT_A1R5G5B5_UNORM_PACK16,    // RGB5A1
  VK_FORMAT_R5G5B5A1_UNORM_PACK16,    // A1BGR5
  VK_FORMAT_R8_UNORM,                 // R8
  VK_FORMAT_D16_UNORM,                // D16
  VK_FORMAT_D24_UNORM_S8_UINT,        // D24S8
  VK_FORMAT_D32_SFLOAT,               // D32F
  VK_FORMAT_D32_SFLOAT_S8_UINT,       // D32FS8
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
  VK_FORMAT_BC1_RGBA_UNORM_BLOCK,     // BC1
  VK_FORMAT_BC2_UNORM_BLOCK,          // BC2
  VK_FORMAT_BC3_UNORM_BLOCK,          // BC3
  VK_FORMAT_BC7_UNORM_BLOCK,          // BC7
};

// Handles are always 64-bit, even on 32-bit platforms.
static const VkRenderPass DYNAMIC_RENDERING_RENDER_PASS = ((VkRenderPass) static_cast<s64>(-1LL));

#ifdef ENABLE_GPU_OBJECT_NAMES
static u32 s_debug_scope_depth = 0;
#endif

// We need to synchronize instance creation because of adapter enumeration from the UI thread.
static std::mutex s_instance_mutex;

VulkanDevice::VulkanDevice()
{
  m_render_api = RenderAPI::Vulkan;

#ifdef ENABLE_GPU_OBJECT_NAMES
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
    ERROR_LOG("Vulkan: No extensions supported by instance.");
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
      DEV_LOG("Enabling extension: {}", name);
      extension_list->push_back(name);
      return true;
    }

    if (required)
      ERROR_LOG("Vulkan: Missing required extension {}.", name);

    return false;
  };

#if defined(VK_USE_PLATFORM_WIN32_KHR)
  if (wi.type == WindowInfo::Type::Win32 && (!SupportsExtension(VK_KHR_SURFACE_EXTENSION_NAME, true) ||
                                             !SupportsExtension(VK_KHR_WIN32_SURFACE_EXTENSION_NAME, true)))
    return false;
#endif
#if defined(VK_USE_PLATFORM_XCB_KHR)
  if (wi.type == WindowInfo::Type::XCB && (!SupportsExtension(VK_KHR_SURFACE_EXTENSION_NAME, true) ||
                                           !SupportsExtension(VK_KHR_XCB_SURFACE_EXTENSION_NAME, true)))
    return false;
#endif
#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
  if (wi.type == WindowInfo::Type::Wayland && (!SupportsExtension(VK_KHR_SURFACE_EXTENSION_NAME, true) ||
                                               !SupportsExtension(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME, true)))
    return false;
#endif
#if defined(VK_USE_PLATFORM_METAL_EXT)
  if (wi.type == WindowInfo::Type::MacOS && (!SupportsExtension(VK_KHR_SURFACE_EXTENSION_NAME, true) ||
                                             !SupportsExtension(VK_EXT_METAL_SURFACE_EXTENSION_NAME, true)))
  {
    return false;
  }
#endif
#if defined(VK_USE_PLATFORM_ANDROID_KHR)
  if (wi.type == WindowInfo::Type::Android && (!SupportsExtension(VK_KHR_SURFACE_EXTENSION_NAME, true) ||
                                               !SupportsExtension(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME, true)))
  {
    return false;
  }
#endif

#if defined(ENABLE_SDL)
  if (wi.type == WindowInfo::Type::SDL)
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
  if (enable_debug_utils && !SupportsExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME, false))
    WARNING_LOG("Vulkan: Debug report requested, but extension is not available.");

  // Needed for exclusive fullscreen control.
  SupportsExtension(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, false);

  oe->vk_khr_get_surface_capabilities2 = (wi.type != WindowInfo::Type::Surfaceless &&
                                          SupportsExtension(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, false));
  oe->vk_ext_surface_maintenance1 =
    (wi.type != WindowInfo::Type::Surfaceless && SupportsExtension(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME, false));
  oe->vk_ext_swapchain_maintenance1 = (wi.type != WindowInfo::Type::Surfaceless &&
                                       SupportsExtension(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME, false));
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
    WARNING_LOG("First vkEnumeratePhysicalDevices() call returned {} devices, but second returned {}",
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

    AdapterInfo ai;
    ai.name = props.properties.deviceName;
    ai.max_texture_size =
      std::min(props.properties.limits.maxFramebufferWidth, props.properties.limits.maxImageDimension2D);
    ai.max_multisamples = GetMaxMultisamples(device, props.properties);
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

VulkanDevice::GPUList VulkanDevice::EnumerateGPUs()
{
  GPUList ret;
  std::unique_lock lock(s_instance_mutex);

  // Device shouldn't be torn down since we have the lock.
  if (g_gpu_device && g_gpu_device->GetRenderAPI() == RenderAPI::Vulkan && Vulkan::IsVulkanLibraryLoaded())
  {
    ret = EnumerateGPUs(VulkanDevice::GetInstance().m_instance);
  }
  else
  {
    if (Vulkan::LoadVulkanLibrary(nullptr))
    {
      OptionalExtensions oe = {};
      const VkInstance instance = CreateVulkanInstance(WindowInfo(), &oe, false, false);
      if (instance != VK_NULL_HANDLE)
      {
        if (Vulkan::LoadVulkanInstanceFunctions(instance))
          ret = EnumerateGPUs(instance);

        if (vkDestroyInstance)
          vkDestroyInstance(instance, nullptr);
        else
          ERROR_LOG("Vulkan instance was leaked because vkDestroyInstance() could not be loaded.");
      }

      Vulkan::UnloadVulkanLibrary();
    }
  }

  return ret;
}

GPUDevice::AdapterInfoList VulkanDevice::GetAdapterList()
{
  AdapterInfoList ret;
  GPUList gpus = EnumerateGPUs();
  ret.reserve(gpus.size());
  for (auto& [physical_device, adapter_info] : gpus)
    ret.push_back(std::move(adapter_info));
  return ret;
}

bool VulkanDevice::EnableOptionalDeviceExtensions(VkPhysicalDevice physical_device,
                                                  std::span<const VkExtensionProperties> available_extensions,
                                                  ExtensionList& enabled_extensions,
                                                  VkPhysicalDeviceFeatures& enabled_features, bool enable_surface,
                                                  Error* error)
{
  const auto SupportsExtension = [&available_extensions](const char* name) {
    return (std::find_if(available_extensions.begin(), available_extensions.end(),
                         [&](const VkExtensionProperties& properties) {
                           return (std::strcmp(name, properties.extensionName) == 0);
                         }) != available_extensions.end());
  };

  const auto AddExtension = [&enabled_extensions](const char* name) {
    if (std::none_of(enabled_extensions.begin(), enabled_extensions.end(),
                     [&](const char* existing_name) { return (std::strcmp(existing_name, name) == 0); }))
    {
      DEV_LOG("Enabling extension: {}", name);
      enabled_extensions.push_back(name);
    }

    return true;
  };
  const auto SupportsAndAddExtension = [&](const char* name) {
    if (!SupportsExtension(name))
      return false;

    AddExtension(name);
    return true;
  };

  if (enable_surface && !SupportsAndAddExtension(VK_KHR_SWAPCHAIN_EXTENSION_NAME))
    return false;

  // get api version, and fixup any bad values from the driver
  vkGetPhysicalDeviceProperties(physical_device, &m_device_properties);
  m_device_properties.limits.minUniformBufferOffsetAlignment =
    std::max(m_device_properties.limits.minUniformBufferOffsetAlignment, static_cast<VkDeviceSize>(16));
  m_device_properties.limits.minTexelBufferOffsetAlignment =
    std::max(m_device_properties.limits.minTexelBufferOffsetAlignment, static_cast<VkDeviceSize>(1));
  m_device_properties.limits.optimalBufferCopyOffsetAlignment =
    std::max(m_device_properties.limits.optimalBufferCopyOffsetAlignment, static_cast<VkDeviceSize>(1));
  m_device_properties.limits.optimalBufferCopyRowPitchAlignment =
    std::max(m_device_properties.limits.optimalBufferCopyRowPitchAlignment, static_cast<VkDeviceSize>(1));
  m_device_properties.limits.bufferImageGranularity =
    std::max(m_device_properties.limits.bufferImageGranularity, static_cast<VkDeviceSize>(1));

  // advanced feature checks
  VkPhysicalDeviceFeatures2 features2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, nullptr, {}};
  VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT rasterization_order_access_feature = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT, nullptr, VK_FALSE, VK_FALSE,
    VK_FALSE};
  VkPhysicalDeviceDynamicRenderingFeatures dynamic_rendering_feature = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, nullptr, VK_FALSE};
  VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR dynamic_rendering_local_read_feature = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR, nullptr, VK_FALSE};
  VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT fragment_shader_interlock_feature = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT, nullptr, VK_FALSE, VK_FALSE, VK_FALSE};
  VkPhysicalDeviceMaintenance4Features maintenance4_features = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES, nullptr, VK_FALSE};
  VkPhysicalDeviceMaintenance5FeaturesKHR maintenance5_features = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR, nullptr, VK_FALSE};

  // add in optional feature structs
  // Gate most of the extension checks behind a Vulkan 1.1 device, so we don't have to deal with situations where
  // some extensions are supported but not others, and the prerequisite extensions for those extensions.
  if (m_device_properties.apiVersion >= VK_API_VERSION_1_1)
  {
    if (SupportsExtension(VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME) ||
        SupportsExtension(VK_ARM_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME))
    {
      m_optional_extensions.vk_ext_rasterization_order_attachment_access = true;
      Vulkan::AddPointerToChain(&features2, &rasterization_order_access_feature);
    }
    if (SupportsExtension(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME) &&
        SupportsExtension(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME) &&
        SupportsExtension(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME))
    {
      m_optional_extensions.vk_khr_dynamic_rendering = true;
      Vulkan::AddPointerToChain(&features2, &dynamic_rendering_feature);

      if (SupportsExtension(VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME))
      {
        m_optional_extensions.vk_khr_dynamic_rendering_local_read = true;
        Vulkan::AddPointerToChain(&features2, &dynamic_rendering_local_read_feature);
      }

      if (SupportsExtension(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME))
      {
        m_optional_extensions.vk_ext_fragment_shader_interlock = true;
        Vulkan::AddPointerToChain(&features2, &fragment_shader_interlock_feature);
      }
    }
    if (SupportsExtension(VK_KHR_MAINTENANCE_4_EXTENSION_NAME))
    {
      m_optional_extensions.vk_khr_maintenance4 = true;
      Vulkan::AddPointerToChain(&features2, &maintenance4_features);

      if (SupportsExtension(VK_KHR_MAINTENANCE_5_EXTENSION_NAME))
      {
        m_optional_extensions.vk_khr_maintenance5 = true;
        Vulkan::AddPointerToChain(&features2, &maintenance5_features);
      }
    }
  }

  // we might not have VK_KHR_get_physical_device_properties2...
  if (!vkGetPhysicalDeviceFeatures2 || !vkGetPhysicalDeviceProperties2 || !vkGetPhysicalDeviceMemoryProperties2)
  {
    if (!vkGetPhysicalDeviceFeatures2KHR || !vkGetPhysicalDeviceProperties2KHR ||
        !vkGetPhysicalDeviceMemoryProperties2KHR)
    {
      ERROR_LOG("One or more functions from VK_KHR_get_physical_device_properties2 is missing, disabling extension.");
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
    vkGetPhysicalDeviceFeatures2(physical_device, &features2);
  else
    vkGetPhysicalDeviceFeatures(physical_device, &features2.features);

  // confirm we actually support it
  m_optional_extensions.vk_ext_rasterization_order_attachment_access &=
    (rasterization_order_access_feature.rasterizationOrderColorAttachmentAccess == VK_TRUE);
  m_optional_extensions.vk_khr_dynamic_rendering &= (dynamic_rendering_feature.dynamicRendering == VK_TRUE);
  m_optional_extensions.vk_khr_dynamic_rendering_local_read &=
    (dynamic_rendering_local_read_feature.dynamicRenderingLocalRead == VK_TRUE);
  m_optional_extensions.vk_ext_fragment_shader_interlock &=
    (m_optional_extensions.vk_khr_dynamic_rendering &&
     fragment_shader_interlock_feature.fragmentShaderPixelInterlock == VK_TRUE);
  m_optional_extensions.vk_khr_maintenance4 &= (maintenance4_features.maintenance4 == VK_TRUE);
  m_optional_extensions.vk_khr_maintenance5 &= (maintenance5_features.maintenance5 == VK_TRUE);

  VkPhysicalDeviceProperties2 properties2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, nullptr, {}};
  VkPhysicalDevicePushDescriptorPropertiesKHR push_descriptor_properties = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR, nullptr, 0u};
  VkPhysicalDeviceExternalMemoryHostPropertiesEXT external_memory_host_properties = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT, nullptr, 0};

  if (SupportsExtension(VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME))
  {
    m_optional_extensions.vk_khr_driver_properties = true;
    m_device_driver_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    Vulkan::AddPointerToChain(&properties2, &m_device_driver_properties);
  }

  if (SupportsExtension(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME))
  {
    m_optional_extensions.vk_khr_push_descriptor = true;
    Vulkan::AddPointerToChain(&properties2, &push_descriptor_properties);
  }

  if (SupportsExtension(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME))
  {
    m_optional_extensions.vk_ext_external_memory_host = true;
    Vulkan::AddPointerToChain(&properties2, &external_memory_host_properties);
  }

  // don't bother querying if we're not actually looking at any features
  if (vkGetPhysicalDeviceProperties2 && properties2.pNext)
    vkGetPhysicalDeviceProperties2(physical_device, &properties2);

  // set driver type
  SetDriverType(GuessDriverType(m_device_properties, m_device_driver_properties));

  // check we actually support enough
  m_optional_extensions.vk_khr_push_descriptor &= (push_descriptor_properties.maxPushDescriptors >= 1);

  // vk_ext_external_memory_host is only used if the import alignment is the same as the system's page size
  m_optional_extensions.vk_ext_external_memory_host &=
    (external_memory_host_properties.minImportedHostPointerAlignment <= HOST_PAGE_SIZE);

  if (m_driver_type == GPUDriverType::QualcommProprietary || m_driver_type == GPUDriverType::ARMProprietary ||
      m_driver_type == GPUDriverType::ImaginationProprietary)
  {
    // Push descriptor is broken on Adreno v502.. don't want to think about dynamic rendending.
    if (m_optional_extensions.vk_khr_dynamic_rendering)
    {
      m_optional_extensions.vk_khr_dynamic_rendering = false;
      m_optional_extensions.vk_khr_dynamic_rendering_local_read = false;
      m_optional_extensions.vk_ext_fragment_shader_interlock = false;
      WARNING_LOG("Disabling VK_KHR_dynamic_rendering on broken mobile driver.");
    }
    if (m_optional_extensions.vk_khr_push_descriptor)
    {
      m_optional_extensions.vk_khr_push_descriptor = false;
      WARNING_LOG("Disabling VK_KHR_push_descriptor on broken mobile driver.");
    }
  }
  else if (m_driver_type == GPUDriverType::AMDProprietary)
  {
    // VK_KHR_dynamic_rendering_local_read appears to be broken on RDNA3, like everything else...
    // Just causes GPU resets when you actually use a feedback loop. Assume Mesa is fine.
    // VK_EXT_fragment_shader_interlock is similar, random GPU hangs.
#if defined(_WIN32) || defined(__ANDROID__)
    m_optional_extensions.vk_ext_fragment_shader_interlock = false;
    m_optional_extensions.vk_khr_dynamic_rendering_local_read = false;
    WARNING_LOG(
      "Disabling VK_EXT_fragment_shader_interlock and VK_KHR_dynamic_rendering_local_read on broken AMD driver.");
#endif
  }

  // Actually enable the extensions. See above for VK1.1 reasoning.
  if (m_device_properties.apiVersion >= VK_API_VERSION_1_1)
  {
    m_optional_extensions.vk_ext_memory_budget = SupportsAndAddExtension(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    m_optional_extensions.vk_khr_driver_properties = SupportsAndAddExtension(VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME);

    // glslang generates debug info instructions before phi nodes at the beginning of blocks when non-semantic debug
    // info is enabled, triggering errors by spirv-val. Gate it by an environment variable if you want source debugging
    // until this is fixed.
    if (const char* val = std::getenv("USE_NON_SEMANTIC_DEBUG_INFO");
        val && StringUtil::FromChars<bool>(val).value_or(false))
    {
      m_optional_extensions.vk_khr_shader_non_semantic_info =
        SupportsAndAddExtension(VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME);
    }

    if (m_optional_extensions.vk_ext_rasterization_order_attachment_access)
    {
      if (!SupportsAndAddExtension(VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME))
        SupportsAndAddExtension(VK_ARM_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME);
    }
    if (m_optional_extensions.vk_khr_dynamic_rendering)
    {
      AddExtension(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME);
      AddExtension(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
      AddExtension(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);

      if (m_optional_extensions.vk_khr_dynamic_rendering_local_read)
        AddExtension(VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME);
    }
    if (m_optional_extensions.vk_khr_push_descriptor)
      AddExtension(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);

    if (m_optional_extensions.vk_ext_external_memory_host)
      AddExtension(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);

    // Dynamic rendering isn't strictly needed for FSI, but we want it with framebufferless rendering.
    if (m_optional_extensions.vk_ext_fragment_shader_interlock)
      AddExtension(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME);

    if (m_optional_extensions.vk_khr_maintenance4)
      AddExtension(VK_KHR_MAINTENANCE_4_EXTENSION_NAME);

    if (m_optional_extensions.vk_khr_maintenance5)
      AddExtension(VK_KHR_MAINTENANCE_5_EXTENSION_NAME);

    m_optional_extensions.vk_ext_swapchain_maintenance1 =
      enable_surface && SupportsAndAddExtension(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
  }

  // Enable the features we use.
  enabled_features.dualSrcBlend |= features2.features.dualSrcBlend;
  enabled_features.largePoints |= features2.features.largePoints;
  enabled_features.wideLines |= features2.features.wideLines;
  enabled_features.samplerAnisotropy |= features2.features.samplerAnisotropy;
  enabled_features.sampleRateShading |= features2.features.sampleRateShading;
  enabled_features.geometryShader |= features2.features.geometryShader;
  enabled_features.fragmentStoresAndAtomics |= features2.features.fragmentStoresAndAtomics;
  enabled_features.textureCompressionBC |= features2.features.textureCompressionBC;

#define LOG_EXT(name, field)                                                                                           \
  Log::FastWrite(___LogChannel___, Log::Level::Info,                                                                   \
                 m_optional_extensions.field ? Log::Color::StrongGreen : Log::Color::StrongOrange, name " is {}",      \
                 m_optional_extensions.field ? "supported" : "NOT supported")

  LOG_EXT("VK_EXT_external_memory_host", vk_ext_external_memory_host);
  LOG_EXT("VK_EXT_fragment_shader_interlock", vk_ext_fragment_shader_interlock);
  LOG_EXT("VK_EXT_memory_budget", vk_ext_memory_budget);
  LOG_EXT("VK_EXT_rasterization_order_attachment_access", vk_ext_rasterization_order_attachment_access);
  LOG_EXT("VK_EXT_surface_maintenance1", vk_ext_surface_maintenance1);
  LOG_EXT("VK_EXT_swapchain_maintenance1", vk_ext_swapchain_maintenance1);
  LOG_EXT("VK_KHR_get_physical_device_properties2", vk_khr_get_physical_device_properties2);
  LOG_EXT("VK_KHR_driver_properties", vk_khr_driver_properties);
  LOG_EXT("VK_KHR_dynamic_rendering", vk_khr_dynamic_rendering);
  LOG_EXT("VK_KHR_dynamic_rendering_local_read", vk_khr_dynamic_rendering_local_read);
  LOG_EXT("VK_KHR_get_surface_capabilities2", vk_khr_get_surface_capabilities2);
  LOG_EXT("VK_KHR_maintenance4", vk_khr_maintenance4);
  LOG_EXT("VK_KHR_maintenance5", vk_khr_maintenance5);
  LOG_EXT("VK_KHR_push_descriptor", vk_khr_push_descriptor);

#ifdef _WIN32
  m_optional_extensions.vk_ext_full_screen_exclusive =
    enable_surface && SupportsAndAddExtension(VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME);
  LOG_EXT("VK_EXT_full_screen_exclusive", vk_ext_full_screen_exclusive);
#endif

#undef LOG_EXT

  return true;
}

bool VulkanDevice::CreateDevice(VkPhysicalDevice physical_device, VkSurfaceKHR surface, bool enable_validation_layer,
                                CreateFlags create_flags, Error* error)
{
  u32 queue_family_count;
  vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, nullptr);
  if (queue_family_count == 0)
  {
    ERROR_LOG("No queue families found on specified vulkan physical device.");
    Error::SetStringView(error, "No queue families found on specified vulkan physical device.");
    return false;
  }

  DynamicHeapArray<VkQueueFamilyProperties> queue_family_properties(queue_family_count);
  vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, queue_family_properties.data());
  DEV_LOG("{} vulkan queue families", queue_family_count);

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
        break;
    }

    if (surface)
    {
      VkBool32 present_supported;
      VkResult res = vkGetPhysicalDeviceSurfaceSupportKHR(physical_device, i, surface, &present_supported);
      if (res != VK_SUCCESS)
      {
        LOG_VULKAN_ERROR(res, "vkGetPhysicalDeviceSurfaceSupportKHR failed: ");
        Vulkan::SetErrorObject(error, "vkGetPhysicalDeviceSurfaceSupportKHR failed: ", res);
        return false;
      }

      if (present_supported)
        m_present_queue_family_index = i;

      // Prefer one queue family index that does both graphics and present.
      if (graphics_supported && present_supported)
        break;
    }
  }
  if (m_graphics_queue_family_index == queue_family_count)
  {
    ERROR_LOG("Vulkan: Failed to find an acceptable graphics queue.");
    Error::SetStringView(error, "Vulkan: Failed to find an acceptable graphics queue.");
    return false;
  }
  if (surface != VK_NULL_HANDLE && m_present_queue_family_index == queue_family_count)
  {
    ERROR_LOG("Vulkan: Failed to find an acceptable present queue.");
    Error::SetStringView(error, "Vulkan: Failed to find an acceptable present queue.");
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

  u32 extension_count = 0;
  VkResult res = vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &extension_count, nullptr);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkEnumerateDeviceExtensionProperties failed: ");
    Vulkan::SetErrorObject(error, "vkEnumerateDeviceExtensionProperties failed: ", res);
    return false;
  }

  if (extension_count == 0)
  {
    ERROR_LOG("No extensions supported by device.");
    Error::SetStringView(error, "No extensions supported by device.");
    return false;
  }

  DynamicHeapArray<VkExtensionProperties> available_extension_list(extension_count);
  res =
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &extension_count, available_extension_list.data());
  DebugAssert(res == VK_SUCCESS);

  VkPhysicalDeviceFeatures enabled_features = {};
  ExtensionList enabled_extensions;
  if (!EnableOptionalDeviceExtensions(physical_device, available_extension_list.cspan(), enabled_extensions,
                                      enabled_features, surface != VK_NULL_HANDLE, error))
  {
    return false;
  }

  device_info.enabledExtensionCount = static_cast<uint32_t>(enabled_extensions.size());
  device_info.ppEnabledExtensionNames = enabled_extensions.data();
  device_info.pEnabledFeatures = &enabled_features;

  // Optional feature structs
  VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT rasterization_order_access_feature = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT, nullptr, VK_TRUE, VK_FALSE,
    VK_FALSE};
  VkPhysicalDeviceDynamicRenderingFeatures dynamic_rendering_feature = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, nullptr, VK_TRUE};
  VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR dynamic_rendering_local_read_feature = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR, nullptr, VK_TRUE};
  VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT swapchain_maintenance1_feature = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT, nullptr, VK_TRUE};
  VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT fragment_shader_interlock_feature = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT, nullptr, VK_FALSE, VK_TRUE, VK_FALSE};
  VkPhysicalDeviceMaintenance4Features maintenance4_features = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES, nullptr, VK_TRUE};
  VkPhysicalDeviceMaintenance5FeaturesKHR maintenance5_features = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR, nullptr, VK_TRUE};

  if (m_optional_extensions.vk_ext_rasterization_order_attachment_access)
    Vulkan::AddPointerToChain(&device_info, &rasterization_order_access_feature);
  if (m_optional_extensions.vk_ext_swapchain_maintenance1)
    Vulkan::AddPointerToChain(&device_info, &swapchain_maintenance1_feature);
  if (m_optional_extensions.vk_khr_dynamic_rendering)
  {
    Vulkan::AddPointerToChain(&device_info, &dynamic_rendering_feature);
    if (m_optional_extensions.vk_khr_dynamic_rendering_local_read)
      Vulkan::AddPointerToChain(&device_info, &dynamic_rendering_local_read_feature);
    if (m_optional_extensions.vk_ext_fragment_shader_interlock)
      Vulkan::AddPointerToChain(&device_info, &fragment_shader_interlock_feature);
  }
  if (m_optional_extensions.vk_khr_maintenance4)
  {
    Vulkan::AddPointerToChain(&device_info, &maintenance4_features);
    if (m_optional_extensions.vk_khr_maintenance5)
      Vulkan::AddPointerToChain(&device_info, &maintenance5_features);
  }

  res = vkCreateDevice(physical_device, &device_info, nullptr, &m_device);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateDevice failed: ");
    Vulkan::SetErrorObject(error, "vkCreateDevice failed: ", res);
    return false;
  }

  // With the device created, we can fill the remaining entry points.
  m_physical_device = physical_device;
  if (!Vulkan::LoadVulkanDeviceFunctions(m_device))
    return false;

  // Grab the graphics and present queues.
  vkGetDeviceQueue(m_device, m_graphics_queue_family_index, 0, &m_graphics_queue);
  if (surface)
    vkGetDeviceQueue(m_device, m_present_queue_family_index, 0, &m_present_queue);

  m_features.gpu_timing = (m_device_properties.limits.timestampComputeAndGraphics != 0 &&
                           queue_family_properties[m_graphics_queue_family_index].timestampValidBits > 0 &&
                           m_device_properties.limits.timestampPeriod > 0);
  DEV_LOG("GPU timing is {} (TS={} TS valid bits={}, TS period={})",
          m_features.gpu_timing ? "supported" : "not supported",
          static_cast<u32>(m_device_properties.limits.timestampComputeAndGraphics),
          queue_family_properties[m_graphics_queue_family_index].timestampValidBits,
          m_device_properties.limits.timestampPeriod);

  SetFeatures(create_flags, physical_device, enabled_features);
  return true;
}

bool VulkanDevice::CreateAllocator()
{
  const u32 apiVersion = std::min(m_device_properties.apiVersion, VK_API_VERSION_1_1);
  INFO_LOG("Supported device API version: {}.{}.{}, using version {}.{}.{} for allocator.",
           VK_API_VERSION_MAJOR(m_device_properties.apiVersion), VK_API_VERSION_MINOR(m_device_properties.apiVersion),
           VK_API_VERSION_PATCH(m_device_properties.apiVersion), VK_API_VERSION_MAJOR(apiVersion),
           VK_API_VERSION_MINOR(apiVersion), VK_API_VERSION_PATCH(apiVersion));

  VmaAllocatorCreateInfo ci = {};
  ci.vulkanApiVersion = apiVersion;
  ci.flags = VMA_ALLOCATOR_CREATE_EXTERNALLY_SYNCHRONIZED_BIT;
  ci.physicalDevice = m_physical_device;
  ci.device = m_device;
  ci.instance = m_instance;

  if (m_optional_extensions.vk_ext_memory_budget)
  {
    DEV_LOG("Enabling VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT.");
    ci.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
  }

  if (m_optional_extensions.vk_khr_maintenance4)
  {
    DEV_LOG("Enabling VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE4_BIT");
    ci.flags |= VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE4_BIT;
  }

  if (m_optional_extensions.vk_khr_maintenance5)
  {
    DEV_LOG("Enabling VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE5_BIT");
    ci.flags |= VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE5_BIT;
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
        WARNING_LOG("Disabling allocation from upload heap #{} ({:.2f} MB) due to debug device.", type.heapIndex,
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

  u32 frame_index = 0;
  for (CommandBuffer& resources : m_frame_resources)
  {
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

    u32 num_pools = 0;
    VkDescriptorPoolSize pool_sizes[2];
    if (!m_optional_extensions.vk_khr_push_descriptor)
    {
      pool_sizes[num_pools++] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                 MAX_COMBINED_IMAGE_SAMPLER_DESCRIPTORS_PER_FRAME};
    }
    pool_sizes[num_pools++] = {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, MAX_INPUT_ATTACHMENT_DESCRIPTORS_PER_FRAME};

    VkDescriptorPoolCreateInfo pool_create_info = {
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, MAX_DESCRIPTOR_SETS_PER_FRAME, num_pools, pool_sizes};

    res = vkCreateDescriptorPool(m_device, &pool_create_info, nullptr, &resources.descriptor_pool);
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkCreateDescriptorPool failed: ");
      return false;
    }
    Vulkan::SetObjectName(m_device, resources.descriptor_pool,
                          TinyString::from_format("Frame Descriptor Pool {}", frame_index));

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

  key.samples = static_cast<u8>(config.samples);
  key.feedback_loop = config.render_pass_flags;

  const auto it = m_render_pass_cache.find(key);
  return (it != m_render_pass_cache.end()) ? it->second : CreateCachedRenderPass(key);
}

VkRenderPass VulkanDevice::GetRenderPass(VulkanTexture* const* rts, u32 num_rts, VulkanTexture* ds,
                                         GPUPipeline::RenderPassFlag feedback_loop)
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

  key.feedback_loop = feedback_loop;

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
  CommandBuffer& fres = m_frame_resources[m_current_frame];
  VkDescriptorSetAllocateInfo allocate_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr,
                                               fres.descriptor_pool, 1, &set_layout};

  VkDescriptorSet descriptor_set;
  VkResult res = vkAllocateDescriptorSets(m_device, &allocate_info, &descriptor_set);
  if (res != VK_SUCCESS)
  {
    // Failing to allocate a descriptor set is not a fatal error, we can
    // recover by moving to the next command buffer.
    return VK_NULL_HANDLE;
  }

  fres.needs_descriptor_pool_reset = true;
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

void VulkanDevice::WaitForAllFences()
{
  u32 index = (m_current_frame + 1) % NUM_COMMAND_BUFFERS;
  for (u32 i = 0; i < (NUM_COMMAND_BUFFERS - 1); i++)
  {
    WaitForCommandBufferCompletion(index);
    index = (index + 1) % NUM_COMMAND_BUFFERS;
  }
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
  if (m_device_was_lost)
    return;

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
      ERROR_LOG("vkWaitForFences() for cmdbuffer {} failed with VK_TIMEOUT, trying again.", index);
      continue;
    }
    else if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, TinyString::from_format("vkWaitForFences() for cmdbuffer {} failed: ", index));
      m_device_was_lost = true;
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

void VulkanDevice::EndAndSubmitCommandBuffer(VulkanSwapChain* present_swap_chain, bool explicit_present)
{
  if (m_device_was_lost) [[unlikely]]
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
    submit_info.pWaitSemaphores = present_swap_chain->GetImageAcquireSemaphorePtr();
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitDstStageMask = &wait_bits;

    submit_info.pSignalSemaphores = present_swap_chain->GetPresentSemaphorePtr();
    submit_info.signalSemaphoreCount = 1;
  }

  res = vkQueueSubmit(m_graphics_queue, 1, &submit_info, resources.fence);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkQueueSubmit failed: ");
    m_device_was_lost = true;
    return;
  }

  BeginCommandBuffer((m_current_frame + 1) % NUM_COMMAND_BUFFERS);

  if (present_swap_chain && !explicit_present)
    QueuePresent(present_swap_chain);
}

void VulkanDevice::QueuePresent(VulkanSwapChain* present_swap_chain)
{
  const VkPresentInfoKHR present_info = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                         nullptr,
                                         1,
                                         present_swap_chain->GetPresentSemaphorePtr(),
                                         1,
                                         present_swap_chain->GetSwapChainPtr(),
                                         present_swap_chain->GetCurrentImageIndexPtr(),
                                         nullptr};

  present_swap_chain->ResetImageAcquireResult();

  const VkResult res = vkQueuePresentKHR(m_present_queue, &present_info);
  if (res != VK_SUCCESS)
  {
    VkResult handled_res = res;
    if (!present_swap_chain->HandleAcquireOrPresentError(handled_res, true))
    {
      LOG_VULKAN_ERROR(res, "vkQueuePresentKHR failed: ");
      return;
    }
  }

  // Grab the next image as soon as possible, that way we spend less time blocked on the next
  // submission. Don't care if it fails, we'll deal with that at the presentation call site.
  // Credit to dxvk for the idea.
  present_swap_chain->AcquireNextImage(false);
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
  if (resources.needs_descriptor_pool_reset)
  {
    resources.needs_descriptor_pool_reset = false;
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
  EndAndSubmitCommandBuffer(nullptr, false);

  if (wait_for_completion)
    WaitForCommandBufferCompletion(current_frame);

  InvalidateCachedState();
}

void VulkanDevice::SubmitCommandBuffer(bool wait_for_completion, const std::string_view reason)
{
  WARNING_LOG("Executing command buffer due to '{}'", reason);
  SubmitCommandBuffer(wait_for_completion);
}

void VulkanDevice::SubmitCommandBufferAndRestartRenderPass(const std::string_view reason)
{
  if (InRenderPass())
    EndRenderPass();

  VulkanPipeline* pl = m_current_pipeline;
  SubmitCommandBuffer(false, reason);

  SetPipeline(pl);
  BeginRenderPass();
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

void VulkanDevice::DeferSamplerDestruction(VkSampler object)
{
  m_cleanup_objects.emplace_back(GetCurrentFenceCounter(),
                                 [this, object]() { vkDestroySampler(m_device, object, nullptr); });
}

VKAPI_ATTR VkBool32 VKAPI_CALL DebugMessengerCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
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

VkRenderPass VulkanDevice::CreateCachedRenderPass(RenderPassCacheKey key)
{
  std::array<VkAttachmentReference, MAX_RENDER_TARGETS> color_references;
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
      (key.feedback_loop & GPUPipeline::ColorFeedbackLoop) ?
        (m_optional_extensions.vk_khr_dynamic_rendering_local_read ? VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ_KHR :
                                                                     VK_IMAGE_LAYOUT_GENERAL) :
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
    color_references[num_attachments].attachment = num_attachments;
    color_references[num_attachments].layout = layout;
    color_reference_ptr = color_references.data();

    if (key.feedback_loop & GPUPipeline::ColorFeedbackLoop)
    {
      DebugAssert(i == 0);
      input_reference.attachment = num_attachments;
      input_reference.layout = layout;
      input_reference_ptr = &input_reference;

      if (!m_optional_extensions.vk_ext_rasterization_order_attachment_access)
      {
        // don't need the framebuffer-local dependency when we have rasterization order attachment access
        subpass_dependency.srcSubpass = 0;
        subpass_dependency.dstSubpass = 0;
        subpass_dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        subpass_dependency.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        subpass_dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        subpass_dependency.dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
        subpass_dependency.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
        subpass_dependency_ptr = &subpass_dependency;
      }
    }

    num_attachments++;
  }

  const u32 num_rts = num_attachments;

  if (key.depth_format != static_cast<u8>(GPUTexture::Format::Unknown))
  {
    const VkImageLayout layout = (key.feedback_loop & GPUPipeline::SampleDepthBuffer) ?
                                   VK_IMAGE_LAYOUT_GENERAL :
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
    ((key.feedback_loop & GPUPipeline::ColorFeedbackLoop) &&
     m_optional_extensions.vk_ext_rasterization_order_attachment_access) ?
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
  VkRenderPass render_pass =
    dev.GetRenderPass(reinterpret_cast<VulkanTexture* const*>(rts), num_rts, static_cast<VulkanTexture*>(ds),
                      static_cast<GPUPipeline::RenderPassFlag>(flags));

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

bool VulkanDevice::IsSuitableDefaultRenderer()
{
#ifdef __ANDROID__
  // No way in hell.
  return false;
#else
  GPUList gpus = EnumerateGPUs();
  if (gpus.empty())
  {
    // No adapters, not gonna be able to use VK.
    return false;
  }

  // Check the first GPU, should be enough.
  const AdapterInfo& ainfo = gpus.front().second;
  INFO_LOG("Using Vulkan GPU '{}' for automatic renderer check.", ainfo.name);

  // Any software rendering (LLVMpipe, SwiftShader).
  if ((ainfo.driver_type & GPUDriverType::SoftwareFlag) == GPUDriverType::SoftwareFlag)
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

bool VulkanDevice::CreateDeviceAndMainSwapChain(std::string_view adapter, CreateFlags create_flags,
                                                const WindowInfo& wi, GPUVSyncMode vsync_mode,
                                                bool allow_present_throttle,
                                                const ExclusiveFullscreenMode* exclusive_fullscreen_mode,
                                                std::optional<bool> exclusive_fullscreen_control, Error* error)
{
  std::unique_lock lock(s_instance_mutex);
  bool enable_debug_utils = m_debug_device;
  bool enable_validation_layer = m_debug_device;

#ifdef ENABLE_SDL
  const bool library_loaded =
    (wi.type == WindowInfo::Type::SDL) ? Vulkan::LoadVulkanLibraryFromSDL(error) : Vulkan::LoadVulkanLibrary(error);
#else
  const bool library_loaded = Vulkan::LoadVulkanLibrary(error);
#endif
  if (!library_loaded)
  {
    Error::AddPrefix(error,
                     "Failed to load Vulkan library. Does your GPU and/or driver support Vulkan?\nThe error was:");
    return false;
  }

  m_instance = CreateVulkanInstance(wi, &m_optional_extensions, enable_debug_utils, enable_validation_layer);
  if (m_instance == VK_NULL_HANDLE)
  {
    if (enable_debug_utils || enable_validation_layer)
    {
      // Try again without the validation layer.
      enable_debug_utils = false;
      enable_validation_layer = false;
      m_instance = CreateVulkanInstance(wi, &m_optional_extensions, enable_debug_utils, enable_validation_layer);
      if (m_instance == VK_NULL_HANDLE)
      {
        Error::SetStringView(error, "Failed to create Vulkan instance. Does your GPU and/or driver support Vulkan?");
        return false;
      }

      ERROR_LOG("Vulkan validation/debug layers requested but are unavailable. Creating non-debug device.");
    }
  }

  if (!Vulkan::LoadVulkanInstanceFunctions(m_instance))
  {
    ERROR_LOG("Failed to load Vulkan instance functions");
    Error::SetStringView(error, "Failed to load Vulkan instance functions");

    if (vkDestroyInstance)
      vkDestroyInstance(std::exchange(m_instance, nullptr), nullptr);
    else
      ERROR_LOG("Vulkan instance was leaked because vkDestroyInstance() could not be loaded.");

    return false;
  }

  GPUList gpus = EnumerateGPUs(m_instance);
  if (gpus.empty())
  {
    Error::SetStringView(error, "No physical devices found. Does your GPU and/or driver support Vulkan?");
    return false;
  }

  VkPhysicalDevice physical_device = VK_NULL_HANDLE;
  if (!adapter.empty())
  {
    u32 gpu_index = 0;
    for (; gpu_index < static_cast<u32>(gpus.size()); gpu_index++)
    {
      INFO_LOG("GPU {}: {}", gpu_index, gpus[gpu_index].second.name);
      if (gpus[gpu_index].second.name == adapter)
      {
        physical_device = gpus[gpu_index].first;
        break;
      }
    }

    if (physical_device == VK_NULL_HANDLE)
    {
      WARNING_LOG("Requested GPU '{}' not found, using first ({})", adapter, gpus[0].second.name);
      physical_device = gpus[0].first;
    }
  }
  else
  {
    INFO_LOG("No GPU requested, using first ({})", gpus[0].second.name);
    physical_device = gpus[0].first;
  }

  if (enable_debug_utils)
    EnableDebugUtils();

  std::unique_ptr<VulkanSwapChain> swap_chain;
  if (!wi.IsSurfaceless())
  {
    swap_chain =
      std::make_unique<VulkanSwapChain>(wi, vsync_mode, allow_present_throttle, exclusive_fullscreen_control);
    if (!swap_chain->CreateSurface(m_instance, physical_device, error))
    {
      swap_chain->Destroy(*this, false);
      return false;
    }
  }

  // Attempt to create the device.
  if (!CreateDevice(physical_device, swap_chain ? swap_chain->GetSurface() : VK_NULL_HANDLE, enable_validation_layer,
                    create_flags, error))
  {
    return false;
  }

  // And critical resources.
  if (!CreateAllocator() || !CreatePersistentDescriptorPool() || !CreateCommandBuffers() || !CreatePipelineLayouts())
    return false;

  m_exclusive_fullscreen_control = exclusive_fullscreen_control;

  if (swap_chain)
  {
    // Render a frame as soon as possible to clear out whatever was previously being displayed.
    if (!swap_chain->CreateSwapChain(*this, error) || !swap_chain->CreateSwapChainImages(*this, error))
      return false;

    RenderBlankFrame(swap_chain.get());
    m_main_swap_chain = std::move(swap_chain);
  }

  if (!CreateNullTexture(error))
    return false;

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
    vkDeviceWaitIdle(m_device);

  if (m_main_swap_chain)
  {
    // Explicit swap chain destroy, we don't want to execute the current cmdbuffer.
    static_cast<VulkanSwapChain*>(m_main_swap_chain.get())->Destroy(*this, false);
    m_main_swap_chain.reset();
  }

  for (auto& it : m_cleanup_objects)
    it.second();
  m_cleanup_objects.clear();
  DestroyPersistentDescriptorSets();
  DestroyBuffers();

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

bool VulkanDevice::ValidatePipelineCacheHeader(const VK_PIPELINE_CACHE_HEADER& header, Error* error)
{
  if (header.header_length < sizeof(VK_PIPELINE_CACHE_HEADER))
  {
    Error::SetStringView(error, "Invalid header length");
    return false;
  }

  if (header.header_version != VK_PIPELINE_CACHE_HEADER_VERSION_ONE)
  {
    Error::SetStringView(error, "Invalid header version");
    return false;
  }

  if (header.vendor_id != m_device_properties.vendorID)
  {
    Error::SetStringFmt(error, "Incorrect vendor ID (file: 0x{:X}, device: 0x{:X})", header.vendor_id,
                        m_device_properties.vendorID);
    return false;
  }

  if (header.device_id != m_device_properties.deviceID)
  {
    Error::SetStringFmt(error, "Incorrect device ID (file: 0x{:X}, device: 0x{:X})", header.device_id,
                        m_device_properties.deviceID);
    return false;
  }

  if (std::memcmp(header.uuid, m_device_properties.pipelineCacheUUID, VK_UUID_SIZE) != 0)
  {
    Error::SetStringView(error, "Incorrect UUID");
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

bool VulkanDevice::ReadPipelineCache(DynamicHeapArray<u8> data, Error* error)
{
  if (data.size() < sizeof(VK_PIPELINE_CACHE_HEADER))
  {
    Error::SetStringView(error, "Pipeline cache is too small.");
    return false;
  }

  // alignment reasons...
  VK_PIPELINE_CACHE_HEADER header;
  std::memcpy(&header, data.data(), sizeof(header));
  if (!ValidatePipelineCacheHeader(header, error))
    return false;

  const VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, nullptr, 0, data.size(),
                                     data.data()};
  VkResult res = vkCreatePipelineCache(m_device, &ci, nullptr, &m_pipeline_cache);
  if (res != VK_SUCCESS)
  {
    Vulkan::SetErrorObject(error, "vkCreatePipelineCache() failed: ", res);
    return false;
  }

  return true;
}

bool VulkanDevice::CreatePipelineCache(const std::string& path, Error* error)
{
  const VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, nullptr, 0, 0, nullptr};
  VkResult res = vkCreatePipelineCache(m_device, &ci, nullptr, &m_pipeline_cache);
  if (res != VK_SUCCESS)
  {
    Vulkan::SetErrorObject(error, "vkCreatePipelineCache() failed: ", res);
    return false;
  }

  return true;
}

bool VulkanDevice::GetPipelineCacheData(DynamicHeapArray<u8>* data, Error* error)
{
  if (m_pipeline_cache == VK_NULL_HANDLE)
    return false;

  size_t data_size;
  VkResult res = vkGetPipelineCacheData(m_device, m_pipeline_cache, &data_size, nullptr);
  if (res != VK_SUCCESS)
  {
    Vulkan::SetErrorObject(error, "vkGetPipelineCacheData() failed: ", res);
    return false;
  }

  data->resize(data_size);
  res = vkGetPipelineCacheData(m_device, m_pipeline_cache, &data_size, data->data());
  if (res != VK_SUCCESS)
  {
    Vulkan::SetErrorObject(error, "vkGetPipelineCacheData() (2) failed: ", res);
    return false;
  }

  data->resize(data_size);
  return true;
}

std::unique_ptr<GPUSwapChain> VulkanDevice::CreateSwapChain(const WindowInfo& wi, GPUVSyncMode vsync_mode,
                                                            bool allow_present_throttle,
                                                            const ExclusiveFullscreenMode* exclusive_fullscreen_mode,
                                                            std::optional<bool> exclusive_fullscreen_control,
                                                            Error* error)
{
  std::unique_ptr<VulkanSwapChain> swap_chain =
    std::make_unique<VulkanSwapChain>(wi, vsync_mode, allow_present_throttle, exclusive_fullscreen_control);
  if (swap_chain->CreateSurface(m_instance, m_physical_device, error) && swap_chain->CreateSwapChain(*this, error) &&
      swap_chain->CreateSwapChainImages(*this, error))
  {
    if (InRenderPass())
      EndRenderPass();
    RenderBlankFrame(swap_chain.get());
  }
  else
  {
    swap_chain.reset();
  }

  return swap_chain;
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

void VulkanDevice::FlushCommands()
{
  if (InRenderPass())
    EndRenderPass();

  SubmitCommandBuffer(false);
  TrimTexturePool();
}

void VulkanDevice::WaitForGPUIdle()
{
  if (InRenderPass())
    EndRenderPass();

  SubmitCommandBuffer(true);
}

GPUDevice::PresentResult VulkanDevice::BeginPresent(GPUSwapChain* swap_chain, u32 clear_color)
{
  if (InRenderPass())
    EndRenderPass();

  if (m_device_was_lost) [[unlikely]]
    return PresentResult::DeviceLost;

  VulkanSwapChain* const SC = static_cast<VulkanSwapChain*>(swap_chain);
  VkResult res = SC->AcquireNextImage(true);

  // This can happen when multiple resize events happen in quick succession.
  // In this case, just wait until the next frame to try again.
  if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
  {
    // Still submit the command buffer, otherwise we'll end up with several frames waiting.
    SubmitCommandBuffer(false);
    TrimTexturePool();
    return PresentResult::SkipPresent;
  }

  BeginSwapChainRenderPass(SC, clear_color);
  return PresentResult::OK;
}

void VulkanDevice::EndPresent(GPUSwapChain* swap_chain, bool explicit_present, u64 present_time)
{
  VulkanSwapChain* const SC = static_cast<VulkanSwapChain*>(swap_chain);

  DebugAssert(present_time == 0);
  DebugAssert(InRenderPass() && m_num_current_render_targets == 0 && !m_current_depth_target);
  EndRenderPass();

  DebugAssert(SC == m_current_swap_chain);
  m_current_swap_chain = nullptr;

  VulkanTexture::TransitionSubresourcesToLayout(
    m_current_command_buffer, SC->GetCurrentImage(), GPUTexture::Type::RenderTarget, 0, 1, 0, 1,
    VulkanTexture::Layout::ColorAttachment, VulkanTexture::Layout::PresentSrc);
  EndAndSubmitCommandBuffer(SC, explicit_present);
  InvalidateCachedState();
  TrimTexturePool();
}

void VulkanDevice::SubmitPresent(GPUSwapChain* swap_chain)
{
  DebugAssert(swap_chain);
  if (m_device_was_lost) [[unlikely]]
    return;

  QueuePresent(static_cast<VulkanSwapChain*>(swap_chain));
}

#ifdef ENABLE_GPU_OBJECT_NAMES
static std::array<float, 3> Palette(float phase, const std::array<float, 3>& a, const std::array<float, 3>& b,
                                    const std::array<float, 3>& c, const std::array<float, 3>& d)
{
  std::array<float, 3> result;
  result[0] = a[0] + b[0] * std::cos(6.28318f * (c[0] * phase + d[0]));
  result[1] = a[1] + b[1] * std::cos(6.28318f * (c[1] * phase + d[1]));
  result[2] = a[2] + b[2] * std::cos(6.28318f * (c[2] * phase + d[2]));
  return result;
}

void VulkanDevice::PushDebugGroup(const char* name)
{
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
  vkCmdBeginDebugUtilsLabelEXT(m_current_command_buffer, &label);
}

void VulkanDevice::PopDebugGroup()
{
  if (!vkCmdEndDebugUtilsLabelEXT || !m_debug_device)
    return;

  s_debug_scope_depth = (s_debug_scope_depth == 0) ? 0 : (s_debug_scope_depth - 1u);

  vkCmdEndDebugUtilsLabelEXT(m_current_command_buffer);
}

void VulkanDevice::InsertDebugMessage(const char* msg)
{
  if (!vkCmdInsertDebugUtilsLabelEXT || !m_debug_device)
    return;

  const VkDebugUtilsLabelEXT label = {VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT, nullptr, msg, {0.0f, 0.0f, 0.0f, 1.0f}};
  vkCmdInsertDebugUtilsLabelEXT(m_current_command_buffer, &label);
}

#endif

u32 VulkanDevice::GetMaxMultisamples(VkPhysicalDevice physical_device, const VkPhysicalDeviceProperties& properties)
{
  VkImageFormatProperties color_properties = {};
  vkGetPhysicalDeviceImageFormatProperties(physical_device, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
                                           VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0,
                                           &color_properties);
  VkImageFormatProperties depth_properties = {};
  vkGetPhysicalDeviceImageFormatProperties(physical_device, VK_FORMAT_D32_SFLOAT, VK_IMAGE_TYPE_2D,
                                           VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0,
                                           &depth_properties);
  const VkSampleCountFlags combined_properties = properties.limits.framebufferColorSampleCounts &
                                                 properties.limits.framebufferDepthSampleCounts &
                                                 color_properties.sampleCounts & depth_properties.sampleCounts;
  if (combined_properties & VK_SAMPLE_COUNT_64_BIT)
    return 64;
  else if (combined_properties & VK_SAMPLE_COUNT_32_BIT)
    return 32;
  else if (combined_properties & VK_SAMPLE_COUNT_16_BIT)
    return 16;
  else if (combined_properties & VK_SAMPLE_COUNT_8_BIT)
    return 8;
  else if (combined_properties & VK_SAMPLE_COUNT_4_BIT)
    return 4;
  else if (combined_properties & VK_SAMPLE_COUNT_2_BIT)
    return 2;
  else
    return 1;
}

void VulkanDevice::SetFeatures(CreateFlags create_flags, VkPhysicalDevice physical_device,
                               const VkPhysicalDeviceFeatures& vk_features)
{
  const u32 store_api_version = std::min(m_device_properties.apiVersion, VK_API_VERSION_1_1);
  m_render_api_version = (VK_API_VERSION_MAJOR(store_api_version) * 100u) +
                         (VK_API_VERSION_MINOR(store_api_version) * 10u) + (VK_API_VERSION_PATCH(store_api_version));
  m_max_texture_size =
    std::min(m_device_properties.limits.maxImageDimension2D, m_device_properties.limits.maxFramebufferWidth);
  m_max_multisamples = static_cast<u16>(GetMaxMultisamples(physical_device, m_device_properties));

  m_features.dual_source_blend =
    !HasCreateFlag(create_flags, CreateFlags::DisableDualSourceBlend) && vk_features.dualSrcBlend;
  m_features.framebuffer_fetch =
    !HasCreateFlag(create_flags, CreateFlags::DisableFeedbackLoops | CreateFlags::DisableFramebufferFetch) &&
    m_optional_extensions.vk_ext_rasterization_order_attachment_access;

  if (!m_features.dual_source_blend)
    WARNING_LOG("Vulkan driver is missing dual-source blending. This will have an impact on performance.");

  m_features.noperspective_interpolation = true;
  m_features.texture_copy_to_self = !HasCreateFlag(create_flags, CreateFlags::DisableTextureCopyToSelf);
  m_features.per_sample_shading = vk_features.sampleRateShading;
  m_features.texture_buffers = !HasCreateFlag(create_flags, CreateFlags::DisableTextureBuffers);
  m_features.feedback_loops = !HasCreateFlag(create_flags, CreateFlags::DisableFeedbackLoops);

#ifdef __APPLE__
  // Partial texture buffer uploads appear to be broken in macOS/MoltenVK.
  m_features.texture_buffers_emulated_with_ssbo = true;
#else
  const u32 max_texel_buffer_elements = m_device_properties.limits.maxTexelBufferElements;
  INFO_LOG("Max texel buffer elements: {}", max_texel_buffer_elements);
  if (max_texel_buffer_elements < MIN_TEXEL_BUFFER_ELEMENTS)
  {
    m_features.texture_buffers_emulated_with_ssbo = true;
  }
#endif

  if (m_features.texture_buffers_emulated_with_ssbo)
    WARNING_LOG("Emulating texture buffers with SSBOs.");

  m_features.geometry_shaders =
    !HasCreateFlag(create_flags, CreateFlags::DisableGeometryShaders) && vk_features.geometryShader;
  m_features.compute_shaders = !HasCreateFlag(create_flags, CreateFlags::DisableComputeShaders);

  m_features.partial_msaa_resolve = true;
  m_features.memory_import = m_optional_extensions.vk_ext_external_memory_host;
  m_features.exclusive_fullscreen = false;
  m_features.explicit_present = true;
  m_features.timed_present = false;
  m_features.shader_cache = true;
  m_features.pipeline_cache = true;
  m_features.prefer_unused_textures = true;
  m_features.raster_order_views =
    (!HasCreateFlag(create_flags, CreateFlags::DisableRasterOrderViews) && vk_features.fragmentStoresAndAtomics &&
     m_optional_extensions.vk_ext_fragment_shader_interlock);

  // Same feature bit for both.
  m_features.dxt_textures = m_features.bptc_textures =
    (!HasCreateFlag(create_flags, CreateFlags::DisableCompressedTextures) && vk_features.textureCompressionBC);
}

GPUDriverType VulkanDevice::GuessDriverType(const VkPhysicalDeviceProperties& device_properties,
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
    {VK_DRIVER_ID_MESA_AGXV, GPUDriverType::AppleMesa},
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
  S->TransitionToLayout(m_current_command_buffer,
                        (D == S) ? VulkanTexture::Layout::TransferSelf : VulkanTexture::Layout::TransferSrc);
  D->TransitionToLayout(m_current_command_buffer,
                        (D == S) ? VulkanTexture::Layout::TransferSelf : VulkanTexture::Layout::TransferDst);

  vkCmdCopyImage(m_current_command_buffer, S->GetImage(), S->GetVkLayout(), D->GetImage(), D->GetVkLayout(), 1, &ic);

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

  if (S->GetState() == GPUTexture::State::Cleared)
    S->CommitClear(m_current_command_buffer);
  if (D->IsRenderTargetOrDepthStencil() && D->GetState() == GPUTexture::State::Cleared)
  {
    if (width < dst->GetWidth() || height < dst->GetHeight())
      D->CommitClear(m_current_command_buffer);
    else
      D->SetState(GPUTexture::State::Dirty);
  }

  S->TransitionSubresourcesToLayout(m_current_command_buffer, 0, 1, 0, 1, S->GetLayout(),
                                    VulkanTexture::Layout::TransferSrc);
  D->TransitionSubresourcesToLayout(m_current_command_buffer, dst_layer, 1, dst_level, 1, D->GetLayout(),
                                    VulkanTexture::Layout::TransferDst);

  const VkImageResolve resolve = {{VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u},
                                  {static_cast<s32>(src_x), static_cast<s32>(src_y), 0},
                                  {VK_IMAGE_ASPECT_COLOR_BIT, dst_level, dst_layer, 1u},
                                  {static_cast<s32>(dst_x), static_cast<s32>(dst_y), 0},
                                  {width, height, 1}};
  vkCmdResolveImage(m_current_command_buffer, S->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, D->GetImage(),
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &resolve);

  S->TransitionSubresourcesToLayout(m_current_command_buffer, 0, 1, 0, 1, VulkanTexture::Layout::TransferSrc,
                                    S->GetLayout());
  D->TransitionSubresourcesToLayout(m_current_command_buffer, dst_layer, 1, dst_level, 1,
                                    VulkanTexture::Layout::TransferDst, D->GetLayout());
}

void VulkanDevice::ClearRenderTarget(GPUTexture* t, u32 c)
{
  GPUDevice::ClearRenderTarget(t, c);
  if (InRenderPass())
  {
    const s32 idx = IsRenderTargetBoundIndex(t);
    if (idx >= 0)
    {
      VulkanTexture* T = static_cast<VulkanTexture*>(t);

      if (m_driver_type == GPUDriverType::NVIDIAProprietary)
      {
        EndRenderPass();
      }
      else
      {
        // Use an attachment clear so the render pass isn't restarted.
        const VkClearAttachment ca = {VK_IMAGE_ASPECT_COLOR_BIT,
                                      static_cast<u32>(idx),
                                      {.color = static_cast<VulkanTexture*>(T)->GetClearColorValue()}};
        const VkClearRect rc = {{{0, 0}, {T->GetWidth(), T->GetHeight()}}, 0u, 1u};
        vkCmdClearAttachments(m_current_command_buffer, 1, &ca, 1, &rc);
        T->SetState(GPUTexture::State::Dirty);
      }
    }
  }
}

void VulkanDevice::ClearDepth(GPUTexture* t, float d)
{
  GPUDevice::ClearDepth(t, d);
  if (InRenderPass() && m_current_depth_target == t)
  {
    // Using vkCmdClearAttachments() within a render pass on NVIDIA seems to cause dependency issues
    // between draws that are testing depth which precede it. The result is flickering where Z tests
    // should be failing. Breaking/restarting the render pass isn't enough to work around the bug,
    // it needs an explicit pipeline barrier.
    VulkanTexture* T = static_cast<VulkanTexture*>(t);
    if (m_driver_type == GPUDriverType::NVIDIAProprietary)
    {
      EndRenderPass();
      T->TransitionSubresourcesToLayout(m_current_command_buffer, 0, 1, 0, 1, T->GetLayout(), T->GetLayout());
    }
    else
    {
      // Use an attachment clear so the render pass isn't restarted.
      const VkClearAttachment ca = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, {.depthStencil = T->GetClearDepthValue()}};
      const VkClearRect rc = {{{0, 0}, {T->GetWidth(), T->GetHeight()}}, 0u, 1u};
      vkCmdClearAttachments(m_current_command_buffer, 1, &ca, 1, &rc);
      T->SetState(GPUTexture::State::Dirty);
    }
  }
}

void VulkanDevice::InvalidateRenderTarget(GPUTexture* t)
{
  GPUDevice::InvalidateRenderTarget(t);
  if (InRenderPass() && (t->IsDepthStencil() ? (m_current_depth_target == t) : (IsRenderTargetBoundIndex(t) >= 0)))
  {
    // Invalidate includes leaving whatever's in the current buffer.
    GL_INS_FMT("Invalidating current {}", t->IsDepthStencil() ? "DS" : "RT");
    t->SetState(GPUTexture::State::Dirty);
  }
}

bool VulkanDevice::CreateBuffers()
{
  if (!m_vertex_buffer.Create(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VERTEX_BUFFER_SIZE))
  {
    ERROR_LOG("Failed to allocate vertex buffer");
    return false;
  }

  if (!m_index_buffer.Create(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, INDEX_BUFFER_SIZE))
  {
    ERROR_LOG("Failed to allocate index buffer");
    return false;
  }

  if (!m_uniform_buffer.Create(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VERTEX_UNIFORM_BUFFER_SIZE))
  {
    ERROR_LOG("Failed to allocate uniform buffer");
    return false;
  }

  if (!m_texture_upload_buffer.Create(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, TEXTURE_BUFFER_SIZE))
  {
    ERROR_LOG("Failed to allocate texture upload buffer");
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

void VulkanDevice::PushUniformBuffer(bool is_compute, const void* data, u32 data_size)
{
  DebugAssert(data_size < UNIFORM_PUSH_CONSTANTS_SIZE);
  s_stats.buffer_streamed += data_size;
  vkCmdPushConstants(m_current_command_buffer, GetCurrentVkPipelineLayout(is_compute),
                     is_compute ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : UNIFORM_PUSH_CONSTANTS_STAGES, 0, data_size,
                     data);
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

bool VulkanDevice::CreateNullTexture(Error* error)
{
  std::unique_ptr<VulkanTexture> null_texture =
    VulkanTexture::Create(1, 1, 1, 1, 1, GPUTexture::Type::Texture, GPUTexture::Format::RGBA8,
                          GPUTexture::Flags::AllowBindAsImage, VK_FORMAT_R8G8B8A8_UNORM, error);
  if (!null_texture)
  {
    Error::AddPrefix(error, "Failed to create null texture: ");
    return false;
  }

  const VkCommandBuffer cmdbuf = GetCurrentInitCommandBuffer();
  const VkImageSubresourceRange srr{VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
  const VkClearColorValue ccv{};
  null_texture->TransitionToLayout(cmdbuf, VulkanTexture::Layout::ClearDst);
  vkCmdClearColorImage(cmdbuf, null_texture->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &ccv, 1, &srr);
  null_texture->TransitionToLayout(cmdbuf, VulkanTexture::Layout::General);
  Vulkan::SetObjectName(m_device, null_texture->GetImage(), "Null texture");
  Vulkan::SetObjectName(m_device, null_texture->GetView(), "Null texture view");
  m_empty_texture = std::move(null_texture);

  // Bind null texture and point sampler state to all.
  GPUSampler* point_sampler = GetSampler(GPUSampler::GetNearestConfig(), error);
  if (!point_sampler)
  {
    Error::AddPrefix(error, "Failed to get nearest sampler for init bind: ");
    return false;
  }

  for (u32 i = 0; i < MAX_TEXTURE_SAMPLERS; i++)
    m_current_samplers[i] = static_cast<VulkanSampler*>(point_sampler)->GetSampler();

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
    dslb.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                    VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT);
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
      dslb.AddBinding(i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                      VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT);
    if ((m_multi_texture_ds_layout = dslb.Create(m_device)) == VK_NULL_HANDLE)
      return false;
    Vulkan::SetObjectName(m_device, m_multi_texture_ds_layout, "Multi Texture Descriptor Set Layout");
  }

  if (m_features.feedback_loops)
  {
    // TODO: This isn't ideal, since we can't push the RT descriptors.
    dslb.AddBinding(0, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    if ((m_feedback_loop_ds_layout = dslb.Create(m_device)) == VK_NULL_HANDLE)
      return false;
    Vulkan::SetObjectName(m_device, m_feedback_loop_ds_layout, "Feedback Loop Descriptor Set Layout");
  }

  for (u32 i = 0; i < MAX_IMAGE_RENDER_TARGETS; i++)
  {
    dslb.AddBinding(i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT);
  }
  if ((m_image_ds_layout = dslb.Create(m_device)) == VK_NULL_HANDLE)
    return false;
  Vulkan::SetObjectName(m_device, m_image_ds_layout, "ROV Descriptor Set Layout");

  for (u32 type = 0; type < 3; type++)
  {
    const bool feedback_loop = (type == 1);
    const bool rov = (type == 2);
    if ((feedback_loop && !m_features.feedback_loops) || (rov && !m_features.raster_order_views))
      continue;

    {
      VkPipelineLayout& pl = m_pipeline_layouts[type][static_cast<u8>(GPUPipeline::Layout::SingleTextureAndUBO)];
      plb.AddDescriptorSet(m_ubo_ds_layout);
      plb.AddDescriptorSet(m_single_texture_ds_layout);
      if (feedback_loop)
        plb.AddDescriptorSet(m_feedback_loop_ds_layout);
      else if (rov)
        plb.AddDescriptorSet(m_image_ds_layout);
      if ((pl = plb.Create(m_device)) == VK_NULL_HANDLE)
        return false;
      Vulkan::SetObjectName(m_device, pl, "Single Texture + UBO Pipeline Layout");
    }

    {
      VkPipelineLayout& pl =
        m_pipeline_layouts[type][static_cast<u8>(GPUPipeline::Layout::SingleTextureAndPushConstants)];
      plb.AddDescriptorSet(m_single_texture_ds_layout);
      if (feedback_loop)
        plb.AddDescriptorSet(m_feedback_loop_ds_layout);
      else if (rov)
        plb.AddDescriptorSet(m_image_ds_layout);
      plb.AddPushConstants(UNIFORM_PUSH_CONSTANTS_STAGES, 0, UNIFORM_PUSH_CONSTANTS_SIZE);
      if ((pl = plb.Create(m_device)) == VK_NULL_HANDLE)
        return false;
      Vulkan::SetObjectName(m_device, pl, "Single Texture Pipeline Layout");
    }

    {
      VkPipelineLayout& pl =
        m_pipeline_layouts[type][static_cast<u8>(GPUPipeline::Layout::SingleTextureBufferAndPushConstants)];
      plb.AddDescriptorSet(m_single_texture_buffer_ds_layout);
      if (feedback_loop)
        plb.AddDescriptorSet(m_feedback_loop_ds_layout);
      else if (rov)
        plb.AddDescriptorSet(m_image_ds_layout);
      plb.AddPushConstants(UNIFORM_PUSH_CONSTANTS_STAGES, 0, UNIFORM_PUSH_CONSTANTS_SIZE);
      if ((pl = plb.Create(m_device)) == VK_NULL_HANDLE)
        return false;
      Vulkan::SetObjectName(m_device, pl, "Single Texture Buffer + UBO Pipeline Layout");
    }

    {
      VkPipelineLayout& pl = m_pipeline_layouts[type][static_cast<u8>(GPUPipeline::Layout::MultiTextureAndUBO)];
      plb.AddDescriptorSet(m_ubo_ds_layout);
      plb.AddDescriptorSet(m_multi_texture_ds_layout);
      if (feedback_loop)
        plb.AddDescriptorSet(m_feedback_loop_ds_layout);
      else if (rov)
        plb.AddDescriptorSet(m_image_ds_layout);
      if ((pl = plb.Create(m_device)) == VK_NULL_HANDLE)
        return false;
      Vulkan::SetObjectName(m_device, pl, "Multi Texture + UBO + Push Constant Pipeline Layout");
    }

    {
      VkPipelineLayout& pl =
        m_pipeline_layouts[type][static_cast<u8>(GPUPipeline::Layout::MultiTextureAndPushConstants)];
      plb.AddDescriptorSet(m_multi_texture_ds_layout);
      plb.AddPushConstants(UNIFORM_PUSH_CONSTANTS_STAGES, 0, UNIFORM_PUSH_CONSTANTS_SIZE);
      if (feedback_loop)
        plb.AddDescriptorSet(m_feedback_loop_ds_layout);
      else if (rov)
        plb.AddDescriptorSet(m_image_ds_layout);
      if ((pl = plb.Create(m_device)) == VK_NULL_HANDLE)
        return false;
      Vulkan::SetObjectName(m_device, pl, "Multi Texture Pipeline Layout");
    }

    {
      VkPipelineLayout& pl =
        m_pipeline_layouts[type][static_cast<u8>(GPUPipeline::Layout::MultiTextureAndUBOAndPushConstants)];
      plb.AddDescriptorSet(m_ubo_ds_layout);
      plb.AddDescriptorSet(m_multi_texture_ds_layout);
      plb.AddPushConstants(UNIFORM_PUSH_CONSTANTS_STAGES, 0, UNIFORM_PUSH_CONSTANTS_SIZE);
      if (feedback_loop)
        plb.AddDescriptorSet(m_feedback_loop_ds_layout);
      else if (rov)
        plb.AddDescriptorSet(m_image_ds_layout);
      if ((pl = plb.Create(m_device)) == VK_NULL_HANDLE)
        return false;
      Vulkan::SetObjectName(m_device, pl, "Multi Texture + UBO + Push Constant Pipeline Layout");
    }
  }

  {
    VkPipelineLayout& pl = m_pipeline_layouts[0][static_cast<u8>(GPUPipeline::Layout::ComputeMultiTextureAndUBO)];
    plb.AddDescriptorSet(m_ubo_ds_layout);
    plb.AddDescriptorSet(m_multi_texture_ds_layout);
    plb.AddDescriptorSet(m_image_ds_layout);
    if ((pl = plb.Create(m_device)) == VK_NULL_HANDLE)
      return false;
    Vulkan::SetObjectName(m_device, pl, "Compute Multi Texture + UBO Pipeline Layout");
  }

  {
    VkPipelineLayout& pl =
      m_pipeline_layouts[0][static_cast<u8>(GPUPipeline::Layout::ComputeMultiTextureAndPushConstants)];
    plb.AddDescriptorSet(m_multi_texture_ds_layout);
    plb.AddDescriptorSet(m_image_ds_layout);
    plb.AddPushConstants(VK_SHADER_STAGE_COMPUTE_BIT, 0, UNIFORM_PUSH_CONSTANTS_SIZE);
    if ((pl = plb.Create(m_device)) == VK_NULL_HANDLE)
      return false;
    Vulkan::SetObjectName(m_device, pl, "Compute Multi Texture Pipeline Layout");
  }

  return true;
}

void VulkanDevice::DestroyPipelineLayouts()
{
  m_pipeline_layouts.enumerate([this](auto& pl) {
    if (pl != VK_NULL_HANDLE)
    {
      vkDestroyPipelineLayout(m_device, pl, nullptr);
      pl = VK_NULL_HANDLE;
    }
  });

  auto destroy_dsl = [this](VkDescriptorSetLayout& l) {
    if (l != VK_NULL_HANDLE)
    {
      vkDestroyDescriptorSetLayout(m_device, l, nullptr);
      l = VK_NULL_HANDLE;
    }
  };
  destroy_dsl(m_image_ds_layout);
  destroy_dsl(m_feedback_loop_ds_layout);
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

void VulkanDevice::RenderBlankFrame(VulkanSwapChain* swap_chain)
{
  VkResult res = swap_chain->AcquireNextImage(true);
  if (res != VK_SUCCESS)
  {
    ERROR_LOG("Failed to acquire image for blank frame present");
    return;
  }

  const VkImage image = swap_chain->GetCurrentImage();
  static constexpr VkImageSubresourceRange srr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  static constexpr VkClearColorValue clear_color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  VulkanTexture::TransitionSubresourcesToLayout(m_current_command_buffer, image, GPUTexture::Type::RenderTarget, 0, 1,
                                                0, 1, VulkanTexture::Layout::Undefined,
                                                VulkanTexture::Layout::TransferDst);
  vkCmdClearColorImage(m_current_command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_color, 1, &srr);
  VulkanTexture::TransitionSubresourcesToLayout(m_current_command_buffer, image, GPUTexture::Type::RenderTarget, 0, 1,
                                                0, 1, VulkanTexture::Layout::TransferDst,
                                                VulkanTexture::Layout::PresentSrc);

  EndAndSubmitCommandBuffer(swap_chain, false);

  InvalidateCachedState();
}

bool VulkanDevice::TryImportHostMemory(void* data, size_t data_size, VkBufferUsageFlags buffer_usage,
                                       VkDeviceMemory* out_memory, VkBuffer* out_buffer, VkDeviceSize* out_offset,
                                       Error* error)
{
  if (!m_optional_extensions.vk_ext_external_memory_host)
  {
    Error::SetStringView(error, "VK_EXT_external_memory_host is not supported.");
    return false;
  }

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
    Vulkan::SetErrorObject(error, "vkGetMemoryHostPointerPropertiesEXT() failed: ", res);
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
    Vulkan::SetErrorObject(error, "vmaFindMemoryTypeIndex() failed: ", res);
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
    Vulkan::SetErrorObject(error, "vkAllocateMemory() failed: ", res);
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
    Vulkan::SetErrorObject(error, "vkCreateBuffer() failed: ", res);
    if (imported_memory != VK_NULL_HANDLE)
      vkFreeMemory(m_device, imported_memory, nullptr);

    return false;
  }

  vkBindBufferMemory(m_device, imported_buffer, imported_memory, 0);

  *out_memory = imported_memory;
  *out_buffer = imported_buffer;
  *out_offset = data_offset;
  DEV_LOG("Imported {} byte buffer covering {} bytes at {}", data_size, data_size_aligned, data);
  return true;
}

void VulkanDevice::SetRenderTargets(GPUTexture* const* rts, u32 num_rts, GPUTexture* ds,
                                    GPUPipeline::RenderPassFlag flags)
{
  const bool changed_layout =
    (m_current_render_pass_flags & (GPUPipeline::ColorFeedbackLoop | GPUPipeline::BindRenderTargetsAsImages)) !=
    (flags & (GPUPipeline::ColorFeedbackLoop | GPUPipeline::BindRenderTargetsAsImages));
  bool changed =
    (m_num_current_render_targets != num_rts || m_current_depth_target != ds || m_current_render_pass_flags != flags);
  bool needs_ds_clear = (ds && ds->IsClearedOrInvalidated());
  bool needs_rt_clear = false;

  m_current_depth_target = static_cast<VulkanTexture*>(ds);
  for (u32 i = 0; i < num_rts; i++)
  {
    VulkanTexture* const RT = static_cast<VulkanTexture*>(rts[i]);
    changed |= m_current_render_targets[i] != RT;
    m_current_render_targets[i] = RT;
    needs_rt_clear |= RT->IsClearedOrInvalidated();
  }
  for (u32 i = num_rts; i < m_num_current_render_targets; i++)
    m_current_render_targets[i] = nullptr;
  m_num_current_render_targets = Truncate8(num_rts);
  m_current_render_pass_flags = flags;

  if (changed)
  {
    if (InRenderPass())
      EndRenderPass();

    m_current_framebuffer = VK_NULL_HANDLE;
    if (m_num_current_render_targets == 0 && !m_current_depth_target)
      return;

    if (!(flags & GPUPipeline::BindRenderTargetsAsImages) &&
        (!m_optional_extensions.vk_khr_dynamic_rendering ||
         ((flags & GPUPipeline::ColorFeedbackLoop) && !m_optional_extensions.vk_khr_dynamic_rendering_local_read)))
    {
      m_current_framebuffer = m_framebuffer_manager.Lookup(
        (m_num_current_render_targets > 0) ? reinterpret_cast<GPUTexture**>(m_current_render_targets.data()) : nullptr,
        m_num_current_render_targets, m_current_depth_target, flags);
      if (m_current_framebuffer == VK_NULL_HANDLE)
      {
        ERROR_LOG("Failed to create framebuffer");
        return;
      }
    }

    m_dirty_flags = (m_dirty_flags & ~DIRTY_FLAG_INPUT_ATTACHMENT) | (changed_layout ? DIRTY_FLAG_PIPELINE_LAYOUT : 0) |
                    ((flags & (GPUPipeline::ColorFeedbackLoop | GPUPipeline::BindRenderTargetsAsImages)) ?
                       DIRTY_FLAG_INPUT_ATTACHMENT :
                       0);
  }
  else if (needs_rt_clear || needs_ds_clear)
  {
    // TODO: This could use vkCmdClearAttachments() instead.
    if (InRenderPass())
      EndRenderPass();
  }
}

void VulkanDevice::BeginRenderPass()
{
  DebugAssert(!InRenderPass());

  // All textures should be in shader read only optimal already, but just in case..
  const u32 num_textures = GetActiveTexturesForLayout(m_current_pipeline_layout);
  for (u32 i = 0; i < num_textures; i++)
  {
    if (m_current_textures[i])
      m_current_textures[i]->TransitionToLayout(m_current_command_buffer, VulkanTexture::Layout::ShaderReadOnly);
  }

  // NVIDIA drivers appear to return random garbage when sampling the RT via a feedback loop, if the load op for
  // the render pass is CLEAR. Using vkCmdClearAttachments() doesn't work, so we have to clear the image instead.
  if (m_current_render_pass_flags & GPUPipeline::ColorFeedbackLoop && m_driver_type == GPUDriverType::NVIDIAProprietary)
  {
    for (u32 i = 0; i < m_num_current_render_targets; i++)
    {
      if (m_current_render_targets[i]->GetState() == GPUTexture::State::Cleared)
        m_current_render_targets[i]->CommitClear(m_current_command_buffer);
    }
  }

  if (m_optional_extensions.vk_khr_dynamic_rendering &&
      (m_optional_extensions.vk_khr_dynamic_rendering_local_read ||
       !(m_current_render_pass_flags & GPUPipeline::ColorFeedbackLoop)))
  {
    VkRenderingInfoKHR ri = {
      VK_STRUCTURE_TYPE_RENDERING_INFO_KHR, nullptr, 0u, {}, 1u, 0u, 0u, nullptr, nullptr, nullptr};

    std::array<VkRenderingAttachmentInfoKHR, MAX_RENDER_TARGETS> attachments;
    VkRenderingAttachmentInfoKHR depth_attachment;

    if (m_num_current_render_targets > 0 || m_current_depth_target)
    {
      if (!(m_current_render_pass_flags & GPUPipeline::BindRenderTargetsAsImages))
      {
        ri.colorAttachmentCount = m_num_current_render_targets;
        ri.pColorAttachments = (m_num_current_render_targets > 0) ? attachments.data() : nullptr;

        // set up clear values and transition targets
        for (u32 i = 0; i < m_num_current_render_targets; i++)
        {
          VulkanTexture* const rt = static_cast<VulkanTexture*>(m_current_render_targets[i]);
          rt->TransitionToLayout(m_current_command_buffer,
                                 (m_current_render_pass_flags & GPUPipeline::ColorFeedbackLoop) ?
                                   VulkanTexture::Layout::FeedbackLoop :
                                   VulkanTexture::Layout::ColorAttachment);
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
      }
      else
      {
        // Binding as image, but we still need to clear it.
        for (u32 i = 0; i < m_num_current_render_targets; i++)
        {
          VulkanTexture* rt = m_current_render_targets[i];
          if (rt->GetState() == GPUTexture::State::Cleared)
            rt->CommitClear(m_current_command_buffer);
          rt->SetState(GPUTexture::State::Dirty);
          rt->TransitionToLayout(m_current_command_buffer, VulkanTexture::Layout::ReadWriteImage);
          rt->SetUseFenceCounter(GetCurrentFenceCounter());
        }
      }

      if (VulkanTexture* const ds = m_current_depth_target)
      {
        ds->TransitionToLayout(m_current_command_buffer, VulkanTexture::Layout::DepthStencilAttachment);
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

      const VulkanTexture* const rt_or_ds =
        (m_num_current_render_targets > 0) ? m_current_render_targets[0] : m_current_depth_target;
      ri.renderArea = {{}, {rt_or_ds->GetWidth(), rt_or_ds->GetHeight()}};
    }
    else
    {
      VkRenderingAttachmentInfo& ai = attachments[0];
      ai.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
      ai.pNext = nullptr;
      ai.imageView = m_current_swap_chain->GetCurrentImageView();
      ai.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      ai.resolveMode = VK_RESOLVE_MODE_NONE_KHR;
      ai.resolveImageView = VK_NULL_HANDLE;
      ai.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      ai.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      ai.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

      ri.colorAttachmentCount = 1;
      ri.pColorAttachments = attachments.data();
      ri.renderArea = {{}, {m_current_swap_chain->GetWidth(), m_current_swap_chain->GetHeight()}};
    }

    m_current_render_pass = DYNAMIC_RENDERING_RENDER_PASS;
    vkCmdBeginRenderingKHR(m_current_command_buffer, &ri);
  }
  else
  {
    VkRenderPassBeginInfo bi = {
      VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr, VK_NULL_HANDLE, VK_NULL_HANDLE, {}, 0u, nullptr};
    std::array<VkClearValue, MAX_RENDER_TARGETS + 1> clear_values;

    if (m_current_framebuffer != VK_NULL_HANDLE)
    {
      bi.framebuffer = m_current_framebuffer;
      bi.renderPass = m_current_render_pass =
        GetRenderPass(m_current_render_targets.data(), m_num_current_render_targets, m_current_depth_target,
                      m_current_render_pass_flags);
      if (bi.renderPass == VK_NULL_HANDLE)
      {
        ERROR_LOG("Failed to create render pass");
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
        rt->TransitionToLayout(m_current_command_buffer,
                               (m_current_render_pass_flags & GPUPipeline::ColorFeedbackLoop) ?
                                 VulkanTexture::Layout::FeedbackLoop :
                                 VulkanTexture::Layout::ColorAttachment);
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
        ds->TransitionToLayout(m_current_command_buffer, VulkanTexture::Layout::DepthStencilAttachment);
        ds->SetUseFenceCounter(GetCurrentFenceCounter());
      }

      const VulkanTexture* const rt_or_ds = static_cast<const VulkanTexture*>(
        (m_num_current_render_targets > 0) ? m_current_render_targets[0] : m_current_depth_target);
      bi.renderArea.extent = {rt_or_ds->GetWidth(), rt_or_ds->GetHeight()};
    }
    else
    {
      // Re-rendering to swap chain.
      bi.framebuffer = m_current_swap_chain->GetCurrentFramebuffer();
      bi.renderPass = m_current_render_pass =
        GetSwapChainRenderPass(m_current_swap_chain->GetFormat(), VK_ATTACHMENT_LOAD_OP_LOAD);
      bi.renderArea.extent = {m_current_swap_chain->GetWidth(), m_current_swap_chain->GetHeight()};
    }

    DebugAssert(m_current_render_pass);
    vkCmdBeginRenderPass(m_current_command_buffer, &bi, VK_SUBPASS_CONTENTS_INLINE);
  }

  s_stats.num_render_passes++;

  // If this is a new command buffer, bind the pipeline and such.
  if (m_dirty_flags & DIRTY_FLAG_INITIAL)
    SetInitialPipelineState();
}

void VulkanDevice::BeginSwapChainRenderPass(VulkanSwapChain* swap_chain, u32 clear_color)
{
  DebugAssert(!InRenderPass());

  const VkImage swap_chain_image = swap_chain->GetCurrentImage();

  // Swap chain images start in undefined
  VulkanTexture::TransitionSubresourcesToLayout(
    m_current_command_buffer, swap_chain_image, GPUTexture::Type::RenderTarget, 0, 1, 0, 1,
    VulkanTexture::Layout::Undefined, VulkanTexture::Layout::ColorAttachment);

  // All textures should be in shader read only optimal already, but just in case..
  const u32 num_textures = GetActiveTexturesForLayout(m_current_pipeline_layout);
  for (u32 i = 0; i < num_textures; i++)
  {
    if (m_current_textures[i])
      m_current_textures[i]->TransitionToLayout(m_current_command_buffer, VulkanTexture::Layout::ShaderReadOnly);
  }

  VkClearValue clear_value;
  GSVector4::store<false>(&clear_value.color.float32, GSVector4::unorm8(clear_color));
  if (m_optional_extensions.vk_khr_dynamic_rendering)
  {
    VkRenderingAttachmentInfo ai = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR,
                                    nullptr,
                                    swap_chain->GetCurrentImageView(),
                                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    VK_RESOLVE_MODE_NONE_KHR,
                                    VK_NULL_HANDLE,
                                    VK_IMAGE_LAYOUT_UNDEFINED,
                                    VK_ATTACHMENT_LOAD_OP_CLEAR,
                                    VK_ATTACHMENT_STORE_OP_STORE,
                                    clear_value};

    const VkRenderingInfoKHR ri = {VK_STRUCTURE_TYPE_RENDERING_INFO_KHR,
                                   nullptr,
                                   0u,
                                   {{}, {swap_chain->GetPostRotatedWidth(), swap_chain->GetPostRotatedHeight()}},
                                   1u,
                                   0u,
                                   1u,
                                   &ai,
                                   nullptr,
                                   nullptr};

    m_current_render_pass = DYNAMIC_RENDERING_RENDER_PASS;
    vkCmdBeginRenderingKHR(m_current_command_buffer, &ri);
  }
  else
  {
    m_current_render_pass =
      GetSwapChainRenderPass(swap_chain->GetWindowInfo().surface_format, VK_ATTACHMENT_LOAD_OP_CLEAR);
    DebugAssert(m_current_render_pass);

    const VkRenderPassBeginInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                      nullptr,
                                      m_current_render_pass,
                                      swap_chain->GetCurrentFramebuffer(),
                                      {{0, 0}, {swap_chain->GetPostRotatedWidth(), swap_chain->GetPostRotatedHeight()}},
                                      1u,
                                      &clear_value};
    vkCmdBeginRenderPass(m_current_command_buffer, &rp, VK_SUBPASS_CONTENTS_INLINE);
  }

  m_dirty_flags |=
    (m_current_render_pass_flags & (GPUPipeline::ColorFeedbackLoop | GPUPipeline::BindRenderTargetsAsImages)) ?
      DIRTY_FLAG_PIPELINE_LAYOUT :
      0;
  s_stats.num_render_passes++;
  m_num_current_render_targets = 0;
  m_current_render_pass_flags = GPUPipeline::NoRenderPassFlags;
  std::memset(m_current_render_targets.data(), 0, sizeof(m_current_render_targets));
  m_current_depth_target = nullptr;
  m_current_framebuffer = VK_NULL_HANDLE;
  m_current_swap_chain = swap_chain;
}

bool VulkanDevice::InRenderPass()
{
  return m_current_render_pass != VK_NULL_HANDLE;
}

void VulkanDevice::EndRenderPass()
{
  DebugAssert(m_current_render_pass != VK_NULL_HANDLE);

  // TODO: stats
  if (std::exchange(m_current_render_pass, VK_NULL_HANDLE) == DYNAMIC_RENDERING_RENDER_PASS)
    vkCmdEndRenderingKHR(m_current_command_buffer);
  else
    vkCmdEndRenderPass(m_current_command_buffer);
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

  vkCmdBindPipeline(m_current_command_buffer,
                    IsComputeLayout(m_current_pipeline->GetLayout()) ? VK_PIPELINE_BIND_POINT_COMPUTE :
                                                                       VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_current_pipeline->GetPipeline());

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
  DebugAssert(!m_current_render_pass);
  m_dirty_flags = ALL_DIRTY_STATE |
                  ((m_current_render_pass_flags & GPUPipeline::ColorFeedbackLoop) ? DIRTY_FLAG_INPUT_ATTACHMENT : 0);
}

s32 VulkanDevice::IsRenderTargetBoundIndex(const GPUTexture* tex) const
{
  for (u32 i = 0; i < m_num_current_render_targets; i++)
  {
    if (m_current_render_targets[i] == tex)
      return static_cast<s32>(i);
  }

  return -1;
}

VulkanDevice::PipelineLayoutType VulkanDevice::GetPipelineLayoutType(GPUPipeline::RenderPassFlag flags)
{
  return (flags & GPUPipeline::BindRenderTargetsAsImages) ?
           PipelineLayoutType::BindRenderTargetsAsImages :
           ((flags & GPUPipeline::ColorFeedbackLoop) ? PipelineLayoutType::ColorFeedbackLoop :
                                                       PipelineLayoutType::Normal);
}

VkPipelineLayout VulkanDevice::GetCurrentVkPipelineLayout(bool is_compute) const
{
  return m_pipeline_layouts[is_compute ? 0 : static_cast<size_t>(GetPipelineLayoutType(m_current_render_pass_flags))]
                           [static_cast<size_t>(m_current_pipeline_layout)];
}

void VulkanDevice::SetInitialPipelineState()
{
  DebugAssert(m_current_pipeline);
  m_dirty_flags &= ~DIRTY_FLAG_INITIAL;

  const VkDeviceSize offset = 0;
  vkCmdBindVertexBuffers(m_current_command_buffer, 0, 1, m_vertex_buffer.GetBufferPtr(), &offset);
  vkCmdBindIndexBuffer(m_current_command_buffer, m_index_buffer.GetBuffer(), 0, VK_INDEX_TYPE_UINT16);

  m_current_pipeline_layout = m_current_pipeline->GetLayout();
  vkCmdBindPipeline(m_current_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_current_pipeline->GetPipeline());

  const VkViewport vp = {static_cast<float>(m_current_viewport.left),
                         static_cast<float>(m_current_viewport.top),
                         static_cast<float>(m_current_viewport.width()),
                         static_cast<float>(m_current_viewport.height()),
                         0.0f,
                         1.0f};
  vkCmdSetViewport(m_current_command_buffer, 0, 1, &vp);

  const VkRect2D vrc = {{m_current_scissor.left, m_current_scissor.top},
                        {static_cast<u32>(m_current_scissor.width()), static_cast<u32>(m_current_scissor.height())}};
  vkCmdSetScissor(m_current_command_buffer, 0, 1, &vrc);
}

void VulkanDevice::SetTextureSampler(u32 slot, GPUTexture* texture, GPUSampler* sampler)
{
  VulkanTexture* T = static_cast<VulkanTexture*>(texture);
  const VkSampler vsampler = static_cast<VulkanSampler*>(sampler ? sampler : m_nearest_sampler)->GetSampler();
  if (m_current_textures[slot] != T || m_current_samplers[slot] != vsampler)
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
      T->TransitionToLayout(m_current_command_buffer, VulkanTexture::Layout::ShaderReadOnly);
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
      m_current_textures[i] = nullptr;
      m_dirty_flags |= DIRTY_FLAG_TEXTURES_OR_SAMPLERS;
    }
  }

  if (tex->IsRenderTarget())
  {
    for (u32 i = 0; i < m_num_current_render_targets; i++)
    {
      if (m_current_render_targets[i] == tex)
      {
        DEV_LOG("Unbinding current RT");
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
      DEV_LOG("Unbinding current DS");
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

void VulkanDevice::SetViewport(const GSVector4i rc)
{
  if (m_current_viewport.eq(rc))
    return;

  m_current_viewport = rc;

  if (m_dirty_flags & DIRTY_FLAG_INITIAL)
    return;

  const VkViewport vp = {static_cast<float>(rc.x),
                         static_cast<float>(rc.y),
                         static_cast<float>(rc.width()),
                         static_cast<float>(rc.height()),
                         0.0f,
                         1.0f};
  vkCmdSetViewport(m_current_command_buffer, 0, 1, &vp);
}

void VulkanDevice::SetScissor(const GSVector4i rc)
{
  if (m_current_scissor.eq(rc))
    return;

  m_current_scissor = rc;

  if (m_dirty_flags & DIRTY_FLAG_INITIAL)
    return;

  const GSVector4i clamped_rc = rc.max_s32(GSVector4i::zero());
  const VkRect2D vrc = {{clamped_rc.x, clamped_rc.y},
                        {static_cast<u32>(clamped_rc.width()), static_cast<u32>(clamped_rc.height())}};
  vkCmdSetScissor(m_current_command_buffer, 0, 1, &vrc);
}

void VulkanDevice::PreDrawCheck()
{
  if (!InRenderPass())
    BeginRenderPass();

  DebugAssert(!(m_dirty_flags & DIRTY_FLAG_INITIAL));
  const u32 update_mask = (m_current_render_pass_flags ? ~0u : ~DIRTY_FLAG_INPUT_ATTACHMENT);
  const u32 dirty = m_dirty_flags & update_mask;
  m_dirty_flags = m_dirty_flags & ~update_mask;

  if (dirty != 0)
  {
    if (!UpdateDescriptorSets(dirty))
    {
      SubmitCommandBufferAndRestartRenderPass("out of descriptor sets");
      PreDrawCheck();
      return;
    }
  }
}

void VulkanDevice::PreDispatchCheck()
{
  // All textures should be in shader read only optimal already, but just in case..
  const u32 num_textures = GetActiveTexturesForLayout(m_current_pipeline_layout);
  for (u32 i = 0; i < num_textures; i++)
  {
    if (m_current_textures[i])
      m_current_textures[i]->TransitionToLayout(m_current_command_buffer, VulkanTexture::Layout::ShaderReadOnly);
  }

  // Binding as image, but we still need to clear it.
  for (u32 i = 0; i < m_num_current_render_targets; i++)
  {
    VulkanTexture* rt = m_current_render_targets[i];
    if (rt->GetState() == GPUTexture::State::Cleared)
      rt->CommitClear(m_current_command_buffer);
    rt->SetState(GPUTexture::State::Dirty);
    rt->TransitionToLayout(m_current_command_buffer, VulkanTexture::Layout::ReadWriteImage);
    rt->SetUseFenceCounter(GetCurrentFenceCounter());
  }

  // If this is a new command buffer, bind the pipeline and such.
  if (m_dirty_flags & DIRTY_FLAG_INITIAL)
    SetInitialPipelineState();

  DebugAssert(!(m_dirty_flags & DIRTY_FLAG_INITIAL));
  const u32 update_mask = (m_current_render_pass_flags ? ~0u : ~DIRTY_FLAG_INPUT_ATTACHMENT);
  const u32 dirty = m_dirty_flags & update_mask;
  m_dirty_flags = m_dirty_flags & ~update_mask;

  if (dirty != 0)
  {
    if (!UpdateDescriptorSets(dirty))
    {
      SubmitCommandBuffer(false, "out of descriptor sets");
      PreDispatchCheck();
      return;
    }
  }
}

template<GPUPipeline::Layout layout>
bool VulkanDevice::UpdateDescriptorSetsForLayout(u32 dirty)
{
  [[maybe_unused]] bool new_dynamic_offsets = false;

  constexpr bool is_compute = IsComputeLayout(layout);
  constexpr VkPipelineBindPoint vk_bind_point =
    (is_compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS);
  const VkPipelineLayout vk_pipeline_layout = GetCurrentVkPipelineLayout(is_compute);
  std::array<VkDescriptorSet, 3> ds;
  u32 first_ds = 0;
  u32 num_ds = 0;

  if constexpr (layout == GPUPipeline::Layout::SingleTextureAndUBO ||
                layout == GPUPipeline::Layout::MultiTextureAndUBO ||
                layout == GPUPipeline::Layout::MultiTextureAndUBOAndPushConstants ||
                layout == GPUPipeline::Layout::ComputeMultiTextureAndUBO)
  {
    new_dynamic_offsets = ((dirty & DIRTY_FLAG_DYNAMIC_OFFSETS) != 0);

    if (dirty & (DIRTY_FLAG_PIPELINE_LAYOUT | DIRTY_FLAG_DYNAMIC_OFFSETS))
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
    VulkanTexture* const tex =
      m_current_textures[0] ? m_current_textures[0] : static_cast<VulkanTexture*>(m_empty_texture.get());
    DebugAssert(tex && m_current_samplers[0] != VK_NULL_HANDLE);
    ds[num_ds++] = tex->GetDescriptorSetWithSampler(m_current_samplers[0]);
  }
  else if constexpr (layout == GPUPipeline::Layout::SingleTextureBufferAndPushConstants)
  {
    DebugAssert(m_current_texture_buffer);
    ds[num_ds++] = m_current_texture_buffer->GetDescriptorSet();
  }
  else if constexpr (layout == GPUPipeline::Layout::MultiTextureAndUBO ||
                     layout == GPUPipeline::Layout::MultiTextureAndPushConstants ||
                     layout == GPUPipeline::Layout::MultiTextureAndUBOAndPushConstants ||
                     layout == GPUPipeline::Layout::ComputeMultiTextureAndUBO ||
                     layout == GPUPipeline::Layout::ComputeMultiTextureAndPushConstants)
  {
    Vulkan::DescriptorSetUpdateBuilder dsub;

    if (m_optional_extensions.vk_khr_push_descriptor)
    {
      for (u32 i = 0; i < MAX_TEXTURE_SAMPLERS; i++)
      {
        VulkanTexture* const tex =
          m_current_textures[i] ? m_current_textures[i] : static_cast<VulkanTexture*>(m_empty_texture.get());
        DebugAssert(tex && m_current_samplers[i] != VK_NULL_HANDLE);
        dsub.AddCombinedImageSamplerDescriptorWrite(VK_NULL_HANDLE, i, tex->GetView(), m_current_samplers[i],
                                                    tex->GetVkLayout());
      }

      const u32 set = (layout == GPUPipeline::Layout::MultiTextureAndUBO ||
                       layout == GPUPipeline::Layout::MultiTextureAndUBOAndPushConstants) ?
                        1 :
                        0;
      dsub.PushUpdate(m_current_command_buffer, vk_bind_point, vk_pipeline_layout, set);
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
        VulkanTexture* const tex =
          m_current_textures[i] ? m_current_textures[i] : static_cast<VulkanTexture*>(m_empty_texture.get());
        DebugAssert(tex && m_current_samplers[i] != VK_NULL_HANDLE);
        dsub.AddCombinedImageSamplerDescriptorWrite(tds, i, tex->GetView(), m_current_samplers[i], tex->GetVkLayout());
      }

      dsub.Update(m_device, false);
    }
  }

  if (m_num_current_render_targets > 0 &&
      ((dirty & DIRTY_FLAG_INPUT_ATTACHMENT) ||
       (dirty & DIRTY_FLAG_PIPELINE_LAYOUT &&
        (m_current_render_pass_flags & (GPUPipeline::ColorFeedbackLoop | GPUPipeline::BindRenderTargetsAsImages)))))
  {
    if (m_current_render_pass_flags & GPUPipeline::BindRenderTargetsAsImages)
    {
      VkDescriptorSet ids = AllocateDescriptorSet(m_image_ds_layout);
      if (ids == VK_NULL_HANDLE)
        return false;

      ds[num_ds++] = ids;

      Vulkan::DescriptorSetUpdateBuilder dsub;
      for (u32 i = 0; i < m_num_current_render_targets; i++)
      {
        dsub.AddStorageImageDescriptorWrite(ids, i, m_current_render_targets[i]->GetView(),
                                            m_current_render_targets[i]->GetVkLayout());
      }

      // Annoyingly, have to update all slots...
      const VkImageView null_view = static_cast<VulkanTexture*>(m_empty_texture.get())->GetView();
      const VkImageLayout null_layout = static_cast<VulkanTexture*>(m_empty_texture.get())->GetVkLayout();
      for (u32 i = m_num_current_render_targets; i < MAX_IMAGE_RENDER_TARGETS; i++)
        dsub.AddStorageImageDescriptorWrite(ids, i, null_view, null_layout);

      dsub.Update(m_device, false);
    }
    else
    {
      VkDescriptorSet ids = AllocateDescriptorSet(m_feedback_loop_ds_layout);
      if (ids == VK_NULL_HANDLE)
        return false;

      ds[num_ds++] = ids;

      Vulkan::DescriptorSetUpdateBuilder dsub;
      dsub.AddInputAttachmentDescriptorWrite(ids, 0, m_current_render_targets[0]->GetView(),
                                             m_current_render_targets[0]->GetVkLayout());
      dsub.Update(m_device, false);
    }
  }

  DebugAssert(num_ds > 0);
  vkCmdBindDescriptorSets(m_current_command_buffer, vk_bind_point, vk_pipeline_layout, first_ds, num_ds, ds.data(),
                          static_cast<u32>(new_dynamic_offsets),
                          new_dynamic_offsets ? &m_uniform_buffer_position : nullptr);

  return true;
}

bool VulkanDevice::UpdateDescriptorSets(u32 dirty)
{
  switch (m_current_pipeline_layout)
  {
    case GPUPipeline::Layout::SingleTextureAndUBO:
      return UpdateDescriptorSetsForLayout<GPUPipeline::Layout::SingleTextureAndUBO>(dirty);

    case GPUPipeline::Layout::SingleTextureAndPushConstants:
      return UpdateDescriptorSetsForLayout<GPUPipeline::Layout::SingleTextureAndPushConstants>(dirty);

    case GPUPipeline::Layout::SingleTextureBufferAndPushConstants:
      return UpdateDescriptorSetsForLayout<GPUPipeline::Layout::SingleTextureBufferAndPushConstants>(dirty);

    case GPUPipeline::Layout::MultiTextureAndUBO:
      return UpdateDescriptorSetsForLayout<GPUPipeline::Layout::MultiTextureAndUBO>(dirty);

    case GPUPipeline::Layout::MultiTextureAndPushConstants:
      return UpdateDescriptorSetsForLayout<GPUPipeline::Layout::MultiTextureAndPushConstants>(dirty);

    case GPUPipeline::Layout::MultiTextureAndUBOAndPushConstants:
      return UpdateDescriptorSetsForLayout<GPUPipeline::Layout::MultiTextureAndUBOAndPushConstants>(dirty);

    case GPUPipeline::Layout::ComputeMultiTextureAndUBO:
      return UpdateDescriptorSetsForLayout<GPUPipeline::Layout::ComputeMultiTextureAndUBO>(dirty);

    case GPUPipeline::Layout::ComputeMultiTextureAndPushConstants:
      return UpdateDescriptorSetsForLayout<GPUPipeline::Layout::ComputeMultiTextureAndPushConstants>(dirty);

    default:
      UnreachableCode();
  }
}

void VulkanDevice::Draw(u32 vertex_count, u32 base_vertex)
{
  PreDrawCheck();
  s_stats.num_draws++;
  vkCmdDraw(m_current_command_buffer, vertex_count, 1, base_vertex, 0);
}

void VulkanDevice::DrawWithPushConstants(u32 vertex_count, u32 base_vertex, const void* push_constants,
                                         u32 push_constants_size)
{
  PreDrawCheck();
  s_stats.num_draws++;

  PushUniformBuffer(false, push_constants, push_constants_size);
  vkCmdDraw(m_current_command_buffer, vertex_count, 1, base_vertex, 0);
}

void VulkanDevice::DrawIndexed(u32 index_count, u32 base_index, u32 base_vertex)
{
  PreDrawCheck();
  s_stats.num_draws++;
  vkCmdDrawIndexed(m_current_command_buffer, index_count, 1, base_index, base_vertex, 0);
}

void VulkanDevice::DrawIndexedWithPushConstants(u32 index_count, u32 base_index, u32 base_vertex,
                                                const void* push_constants, u32 push_constants_size)
{
  PreDrawCheck();
  s_stats.num_draws++;
  PushUniformBuffer(false, push_constants, push_constants_size);
  vkCmdDrawIndexed(m_current_command_buffer, index_count, 1, base_index, base_vertex, 0);
}

VkImageMemoryBarrier VulkanDevice::GetColorBufferBarrier(const VulkanTexture* rt) const
{
  const VkImageLayout vk_layout = m_optional_extensions.vk_khr_dynamic_rendering_local_read ?
                                    VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ_KHR :
                                    VK_IMAGE_LAYOUT_GENERAL;
  DebugAssert(rt->GetLayout() == VulkanTexture::Layout::FeedbackLoop);

  return {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          nullptr,
          VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
          VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,
          vk_layout,
          vk_layout,
          VK_QUEUE_FAMILY_IGNORED,
          VK_QUEUE_FAMILY_IGNORED,
          rt->GetImage(),
          {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u}};
}

void VulkanDevice::DrawIndexedWithBarrier(u32 index_count, u32 base_index, u32 base_vertex, DrawBarrier type)
{
  PreDrawCheck();
  SubmitDrawIndexedWithBarrier(index_count, base_index, base_vertex, type);
}

void VulkanDevice::DrawIndexedWithBarrierWithPushConstants(u32 index_count, u32 base_index, u32 base_vertex,
                                                           const void* push_constants, u32 push_constants_size,
                                                           DrawBarrier type)
{
  PreDrawCheck();
  PushUniformBuffer(false, push_constants, push_constants_size);
  SubmitDrawIndexedWithBarrier(index_count, base_index, base_vertex, type);
}

void VulkanDevice::SubmitDrawIndexedWithBarrier(u32 index_count, u32 base_index, u32 base_vertex, DrawBarrier type)
{
  switch (type)
  {
    case GPUDevice::DrawBarrier::None:
    {
      s_stats.num_draws++;
      vkCmdDrawIndexed(m_current_command_buffer, index_count, 1, base_index, base_vertex, 0);
    }
    break;

    case GPUDevice::DrawBarrier::One:
    {
      DebugAssert(m_num_current_render_targets == 1);
      s_stats.num_barriers++;
      s_stats.num_draws++;

      const VkImageMemoryBarrier barrier =
        GetColorBufferBarrier(static_cast<VulkanTexture*>(m_current_render_targets[0]));
      vkCmdPipelineBarrier(m_current_command_buffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_DEPENDENCY_BY_REGION_BIT, 0, nullptr, 0, nullptr,
                           1, &barrier);
      vkCmdDrawIndexed(m_current_command_buffer, index_count, 1, base_index, base_vertex, 0);
    }
    break;

    case GPUDevice::DrawBarrier::Full:
    {
      DebugAssert(m_num_current_render_targets == 1);

      const VkImageMemoryBarrier barrier =
        GetColorBufferBarrier(static_cast<VulkanTexture*>(m_current_render_targets[0]));
      const u32 indices_per_primitive = m_current_pipeline->GetVerticesPerPrimitive();
      const u32 end_batch = base_index + index_count;

      for (; base_index < end_batch; base_index += indices_per_primitive)
      {
        s_stats.num_barriers++;
        s_stats.num_draws++;

        vkCmdPipelineBarrier(m_current_command_buffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_DEPENDENCY_BY_REGION_BIT, 0, nullptr, 0, nullptr,
                             1, &barrier);
        vkCmdDrawIndexed(m_current_command_buffer, indices_per_primitive, 1, base_index, base_vertex, 0);
      }
    }
    break;

      DefaultCaseIsUnreachable();
  }
}

void VulkanDevice::Dispatch(u32 threads_x, u32 threads_y, u32 threads_z, u32 group_size_x, u32 group_size_y,
                            u32 group_size_z)
{
  PreDispatchCheck();
  s_stats.num_draws++;

  const u32 groups_x = threads_x / group_size_x;
  const u32 groups_y = threads_y / group_size_y;
  const u32 groups_z = threads_z / group_size_z;
  vkCmdDispatch(m_current_command_buffer, groups_x, groups_y, groups_z);
}

void VulkanDevice::DispatchWithPushConstants(u32 threads_x, u32 threads_y, u32 threads_z, u32 group_size_x,
                                             u32 group_size_y, u32 group_size_z, const void* push_constants,
                                             u32 push_constants_size)
{
  PreDispatchCheck();
  s_stats.num_draws++;

  PushUniformBuffer(true, push_constants, push_constants_size);

  const u32 groups_x = threads_x / group_size_x;
  const u32 groups_y = threads_y / group_size_y;
  const u32 groups_z = threads_z / group_size_z;
  vkCmdDispatch(m_current_command_buffer, groups_x, groups_y, groups_z);
}
