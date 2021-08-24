// Copyright 2016 Dolphin Emulator Project
// Copyright 2020 DuckStation Emulator Project
// Licensed under GPLv2+
// Refer to the LICENSE file included.

#pragma once

#include "../string.h"
#include "../types.h"
#include "vulkan_loader.h"
#include <algorithm>
#include <array>
#include <cstdarg>
#include <string_view>
namespace Vulkan {
namespace Util {

inline constexpr u32 MakeRGBA8Color(float r, float g, float b, float a)
{
  return (static_cast<u32>(std::clamp(static_cast<int>(r * 255.0f), 0, 255)) << 0) |
         (static_cast<u32>(std::clamp(static_cast<int>(g * 255.0f), 0, 255)) << 8) |
         (static_cast<u32>(std::clamp(static_cast<int>(b * 255.0f), 0, 255)) << 16) |
         (static_cast<u32>(std::clamp(static_cast<int>(a * 255.0f), 0, 255)) << 24);
}

bool IsDepthFormat(VkFormat format);
bool IsCompressedFormat(VkFormat format);
VkFormat GetLinearFormat(VkFormat format);
u32 GetTexelSize(VkFormat format);
u32 GetBlockSize(VkFormat format);

// Clamps a VkRect2D to the specified dimensions.
VkRect2D ClampRect2D(const VkRect2D& rect, u32 width, u32 height);

// Map {SRC,DST}_COLOR to {SRC,DST}_ALPHA
VkBlendFactor GetAlphaBlendFactor(VkBlendFactor factor);

// Safe destroy helpers
void SafeDestroyFramebuffer(VkFramebuffer& fb);
void SafeDestroyShaderModule(VkShaderModule& sm);
void SafeDestroyPipeline(VkPipeline& p);
void SafeDestroyPipelineLayout(VkPipelineLayout& pl);
void SafeDestroyDescriptorSetLayout(VkDescriptorSetLayout& dsl);
void SafeDestroyBufferView(VkBufferView& bv);
void SafeDestroyImageView(VkImageView& iv);
void SafeDestroySampler(VkSampler& samp);
void SafeDestroySemaphore(VkSemaphore& sem);
void SafeFreeGlobalDescriptorSet(VkDescriptorSet& ds);

void SetViewport(VkCommandBuffer command_buffer, int x, int y, int width, int height, float min_depth = 0.0f,
                 float max_depth = 1.0f);
void SetScissor(VkCommandBuffer command_buffer, int x, int y, int width, int height);

// Combines viewport and scissor updates
void SetViewportAndScissor(VkCommandBuffer command_buffer, int x, int y, int width, int height, float min_depth = 0.0f,
                           float max_depth = 1.0f);

// Wrapper for creating an barrier on a buffer
void BufferMemoryBarrier(VkCommandBuffer command_buffer, VkBuffer buffer, VkAccessFlags src_access_mask,
                         VkAccessFlags dst_access_mask, VkDeviceSize offset, VkDeviceSize size,
                         VkPipelineStageFlags src_stage_mask, VkPipelineStageFlags dst_stage_mask);

// Create a shader module from the specified SPIR-V.
VkShaderModule CreateShaderModule(const u32* spv, size_t spv_word_count);

// Compile a vertex shader and create a shader module, discarding the intermediate SPIR-V.
VkShaderModule CompileAndCreateVertexShader(std::string_view source_code);

// Compile a geometry shader and create a shader module, discarding the intermediate SPIR-V.
VkShaderModule CompileAndCreateGeometryShader(std::string_view source_code);

// Compile a fragment shader and create a shader module, discarding the intermediate SPIR-V.
VkShaderModule CompileAndCreateFragmentShader(std::string_view source_code);

// Compile a compute shader and create a shader module, discarding the intermediate SPIR-V.
VkShaderModule CompileAndCreateComputeShader(std::string_view source_code);

const char* VkResultToString(VkResult res);
const char* VkImageLayoutToString(VkImageLayout layout);
void LogVulkanResult(int level, const char* func_name, VkResult res, const char* msg, ...) printflike(4, 5);

#define LOG_VULKAN_ERROR(res, ...) ::Vulkan::Util::LogVulkanResult(1, __func__, res, __VA_ARGS__)

#if defined(_DEBUG)

// We can't use the templates below because they're all the same type on 32-bit.
#if defined(__LP64__) || defined(_WIN64) || (defined(__x86_64__) && !defined(__ILP32__)) || defined(_M_X64) ||         \
  defined(__ia64) || defined(_M_IA64) || defined(__aarch64__) || defined(__powerpc64__)
#define ENABLE_VULKAN_DEBUG_OBJECTS 1
#endif

#endif

#ifdef ENABLE_VULKAN_DEBUG_OBJECTS

// Provides a compile-time mapping between a Vulkan-type into its matching VkObjectType
template<typename T>
struct VkObjectTypeMap;

// clang-format off
template<> struct VkObjectTypeMap<VkInstance                > { using type = VkInstance                ; static constexpr VkObjectType value = VK_OBJECT_TYPE_INSTANCE;                   };
template<> struct VkObjectTypeMap<VkPhysicalDevice          > { using type = VkPhysicalDevice          ; static constexpr VkObjectType value = VK_OBJECT_TYPE_PHYSICAL_DEVICE;            };
template<> struct VkObjectTypeMap<VkDevice                  > { using type = VkDevice                  ; static constexpr VkObjectType value = VK_OBJECT_TYPE_DEVICE;                     };
template<> struct VkObjectTypeMap<VkQueue                   > { using type = VkQueue                   ; static constexpr VkObjectType value = VK_OBJECT_TYPE_QUEUE;                      };
template<> struct VkObjectTypeMap<VkSemaphore               > { using type = VkSemaphore               ; static constexpr VkObjectType value = VK_OBJECT_TYPE_SEMAPHORE;                  };
template<> struct VkObjectTypeMap<VkCommandBuffer           > { using type = VkCommandBuffer           ; static constexpr VkObjectType value = VK_OBJECT_TYPE_COMMAND_BUFFER;             };
template<> struct VkObjectTypeMap<VkFence                   > { using type = VkFence                   ; static constexpr VkObjectType value = VK_OBJECT_TYPE_FENCE;                      };
template<> struct VkObjectTypeMap<VkDeviceMemory            > { using type = VkDeviceMemory            ; static constexpr VkObjectType value = VK_OBJECT_TYPE_DEVICE_MEMORY;              };
template<> struct VkObjectTypeMap<VkBuffer                  > { using type = VkBuffer                  ; static constexpr VkObjectType value = VK_OBJECT_TYPE_BUFFER;                     };
template<> struct VkObjectTypeMap<VkImage                   > { using type = VkImage                   ; static constexpr VkObjectType value = VK_OBJECT_TYPE_IMAGE;                      };
template<> struct VkObjectTypeMap<VkEvent                   > { using type = VkEvent                   ; static constexpr VkObjectType value = VK_OBJECT_TYPE_EVENT;                      };
template<> struct VkObjectTypeMap<VkQueryPool               > { using type = VkQueryPool               ; static constexpr VkObjectType value = VK_OBJECT_TYPE_QUERY_POOL;                 };
template<> struct VkObjectTypeMap<VkBufferView              > { using type = VkBufferView              ; static constexpr VkObjectType value = VK_OBJECT_TYPE_BUFFER_VIEW;                };
template<> struct VkObjectTypeMap<VkImageView               > { using type = VkImageView               ; static constexpr VkObjectType value = VK_OBJECT_TYPE_IMAGE_VIEW;                 };
template<> struct VkObjectTypeMap<VkShaderModule            > { using type = VkShaderModule            ; static constexpr VkObjectType value = VK_OBJECT_TYPE_SHADER_MODULE;              };
template<> struct VkObjectTypeMap<VkPipelineCache           > { using type = VkPipelineCache           ; static constexpr VkObjectType value = VK_OBJECT_TYPE_PIPELINE_CACHE;             };
template<> struct VkObjectTypeMap<VkPipelineLayout          > { using type = VkPipelineLayout          ; static constexpr VkObjectType value = VK_OBJECT_TYPE_PIPELINE_LAYOUT;            };
template<> struct VkObjectTypeMap<VkRenderPass              > { using type = VkRenderPass              ; static constexpr VkObjectType value = VK_OBJECT_TYPE_RENDER_PASS;                };
template<> struct VkObjectTypeMap<VkPipeline                > { using type = VkPipeline                ; static constexpr VkObjectType value = VK_OBJECT_TYPE_PIPELINE;                   };
template<> struct VkObjectTypeMap<VkDescriptorSetLayout     > { using type = VkDescriptorSetLayout     ; static constexpr VkObjectType value = VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT;      };
template<> struct VkObjectTypeMap<VkSampler                 > { using type = VkSampler                 ; static constexpr VkObjectType value = VK_OBJECT_TYPE_SAMPLER;                    };
template<> struct VkObjectTypeMap<VkDescriptorPool          > { using type = VkDescriptorPool          ; static constexpr VkObjectType value = VK_OBJECT_TYPE_DESCRIPTOR_POOL;            };
template<> struct VkObjectTypeMap<VkDescriptorSet           > { using type = VkDescriptorSet           ; static constexpr VkObjectType value = VK_OBJECT_TYPE_DESCRIPTOR_SET;             };
template<> struct VkObjectTypeMap<VkFramebuffer             > { using type = VkFramebuffer             ; static constexpr VkObjectType value = VK_OBJECT_TYPE_FRAMEBUFFER;                };
template<> struct VkObjectTypeMap<VkCommandPool             > { using type = VkCommandPool             ; static constexpr VkObjectType value = VK_OBJECT_TYPE_COMMAND_POOL;               };
template<> struct VkObjectTypeMap<VkDescriptorUpdateTemplate> { using type = VkDescriptorUpdateTemplate; static constexpr VkObjectType value = VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE; };
template<> struct VkObjectTypeMap<VkSurfaceKHR              > { using type = VkSurfaceKHR              ; static constexpr VkObjectType value = VK_OBJECT_TYPE_SURFACE_KHR;                };
template<> struct VkObjectTypeMap<VkSwapchainKHR            > { using type = VkSwapchainKHR            ; static constexpr VkObjectType value = VK_OBJECT_TYPE_SWAPCHAIN_KHR;              };
template<> struct VkObjectTypeMap<VkDebugUtilsMessengerEXT  > { using type = VkDebugUtilsMessengerEXT  ; static constexpr VkObjectType value = VK_OBJECT_TYPE_DEBUG_UTILS_MESSENGER_EXT;  };
// clang-format on

#endif

inline void SetObjectName(VkDevice device, void* object_handle, VkObjectType object_type, const char* format, ...)
{
#ifdef ENABLE_VULKAN_DEBUG_OBJECTS
  if (!vkSetDebugUtilsObjectNameEXT)
  {
    return;
  }
  std::va_list ap;

  SmallString str;
  va_start(ap, format);
  str.FormatVA(format, ap);
  va_end(ap);

  const VkDebugUtilsObjectNameInfoEXT nameInfo{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT, nullptr, object_type,
                                               reinterpret_cast<uint64_t>(object_handle), str};
  vkSetDebugUtilsObjectNameEXT(device, &nameInfo);
#endif
}

template<typename T>
inline void SetObjectName(VkDevice device, T object_handle, const char* format, ...)
{
#ifdef ENABLE_VULKAN_DEBUG_OBJECTS
  std::va_list ap;
  va_start(ap, format);
  SetObjectName(device, reinterpret_cast<void*>((typename VkObjectTypeMap<T>::type)object_handle),
                VkObjectTypeMap<T>::value, format, ap);
  va_end(ap);
#endif
}

// Command buffer debug utils
inline void BeginDebugScope(VkCommandBuffer command_buffer, const char* scope_name,
                            const std::array<float, 4>& scope_color = {0.5, 0.5, 0.5, 1.0})
{
#ifdef ENABLE_VULKAN_DEBUG_OBJECTS
  if (!vkCmdBeginDebugUtilsLabelEXT)
  {
    return;
  }
  const VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
                                   nullptr,
                                   scope_name,
                                   {scope_color[0], scope_color[1], scope_color[2], scope_color[3]}};
  vkCmdBeginDebugUtilsLabelEXT(command_buffer, &label);
#endif
}

inline void EndDebugScope(VkCommandBuffer command_buffer)
{
#ifdef ENABLE_VULKAN_DEBUG_OBJECTS
  if (!vkCmdEndDebugUtilsLabelEXT)
  {
    return;
  }
  vkCmdEndDebugUtilsLabelEXT(command_buffer);
#endif
}

inline void InsertDebugLabel(VkCommandBuffer command_buffer, const char* label_name,
                             const std::array<float, 4>& label_color = {0.5, 0.5, 0.5, 1.0})
{
#ifdef ENABLE_VULKAN_DEBUG_OBJECTS
  if (!vkCmdInsertDebugUtilsLabelEXT)
  {
    return;
  }
  const VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
                                   nullptr,
                                   label_name,
                                   {label_color[0], label_color[1], label_color[2], label_color[3]}};
  vkCmdInsertDebugUtilsLabelEXT(command_buffer, &label);
#endif
}

// Queue debug utils
inline void BeginDebugScope(VkQueue queue, const char* scope_name,
                            const std::array<float, 4>& scope_color = {0.75, 0.75, 0.75, 1.0})
{
#ifdef ENABLE_VULKAN_DEBUG_OBJECTS
  if (!vkQueueBeginDebugUtilsLabelEXT)
  {
    return;
  }
  const VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
                                   nullptr,
                                   scope_name,
                                   {scope_color[0], scope_color[1], scope_color[2], scope_color[3]}};
  vkQueueBeginDebugUtilsLabelEXT(queue, &label);
#endif
}

inline void EndDebugScope(VkQueue queue)
{
#ifdef ENABLE_VULKAN_DEBUG_OBJECTS
  if (!vkQueueEndDebugUtilsLabelEXT)
  {
    return;
  }
  vkQueueEndDebugUtilsLabelEXT(queue);
#endif
}

inline void InsertDebugLabel(VkQueue queue, const char* label_name,
                             const std::array<float, 4>& label_color = {0.75, 0.75, 0.75, 1.0})
{
#ifdef ENABLE_VULKAN_DEBUG_OBJECTS
  if (!vkQueueInsertDebugUtilsLabelEXT)
  {
    return;
  }
  const VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
                                   nullptr,
                                   label_name,
                                   {label_color[0], label_color[1], label_color[2], label_color[3]}};
  vkQueueInsertDebugUtilsLabelEXT(queue, &label);
#endif
}

template<typename T>
class DebugScope
{
public:
  DebugScope(T context, const char* format, ...) {}
};

#ifdef ENABLE_VULKAN_DEBUG_OBJECTS
template<>
class DebugScope<VkCommandBuffer>
{
public:
  DebugScope(VkCommandBuffer context, const char* format, ...);
  ~DebugScope();

private:
  static constexpr u8 max_depth = 8u;
  static u8 depth;
  VkCommandBuffer command_buffer;
};

template<>
class DebugScope<VkQueue>
{
public:
  DebugScope(VkQueue context, const char* format, ...);
  ~DebugScope();

private:
  static constexpr u8 max_depth = 8u;
  static u8 depth;
  VkQueue queue;
};
#endif

} // namespace Util

} // namespace Vulkan
