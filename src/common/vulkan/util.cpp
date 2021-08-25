// Copyright 2016 Dolphin Emulator Project
// Copyright 2020 DuckStation Emulator Project
// Licensed under GPLv2+
// Refer to the LICENSE file included.

#include "util.h"
#include "../assert.h"
#include "../log.h"
#include "../string_util.h"
#include "context.h"
#include "shader_compiler.h"

#include <cmath>

namespace Vulkan {
namespace Util {
bool IsDepthFormat(VkFormat format)
{
  switch (format)
  {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
      return true;
    default:
      return false;
  }
}

bool IsCompressedFormat(VkFormat format)
{
  switch (format)
  {
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC2_UNORM_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK:
      return true;

    default:
      return false;
  }
}

VkFormat GetLinearFormat(VkFormat format)
{
  switch (format)
  {
    case VK_FORMAT_R8_SRGB:
      return VK_FORMAT_R8_UNORM;
    case VK_FORMAT_R8G8_SRGB:
      return VK_FORMAT_R8G8_UNORM;
    case VK_FORMAT_R8G8B8_SRGB:
      return VK_FORMAT_R8G8B8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8_SRGB:
      return VK_FORMAT_B8G8R8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB:
      return VK_FORMAT_B8G8R8A8_UNORM;
    default:
      return format;
  }
}

u32 GetTexelSize(VkFormat format)
{
  // Only contains pixel formats we use.
  switch (format)
  {
    case VK_FORMAT_R32_SFLOAT:
      return 4;

    case VK_FORMAT_D32_SFLOAT:
      return 4;

    case VK_FORMAT_R8G8B8A8_UNORM:
      return 4;

    case VK_FORMAT_B8G8R8A8_UNORM:
      return 4;

    case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
    case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
    case VK_FORMAT_R5G6B5_UNORM_PACK16:
    case VK_FORMAT_B5G6R5_UNORM_PACK16:
      return 2;

    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
      return 8;

    case VK_FORMAT_BC2_UNORM_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK:
      return 16;

    default:
      Panic("Unhandled pixel format");
      return 1;
  }
}

u32 GetBlockSize(VkFormat format)
{
  switch (format)
  {
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC2_UNORM_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK:
      return 4;

    default:
      return 1;
  }
}

VkRect2D ClampRect2D(const VkRect2D& rect, u32 width, u32 height)
{
  VkRect2D out;
  out.offset.x = std::clamp(rect.offset.x, 0, static_cast<int>(width - 1));
  out.offset.y = std::clamp(rect.offset.y, 0, static_cast<int>(height - 1));
  out.extent.width = std::min(rect.extent.width, width - static_cast<int>(rect.offset.x));
  out.extent.height = std::min(rect.extent.height, height - static_cast<int>(rect.offset.y));
  return out;
}

VkBlendFactor GetAlphaBlendFactor(VkBlendFactor factor)
{
  switch (factor)
  {
    case VK_BLEND_FACTOR_SRC_COLOR:
      return VK_BLEND_FACTOR_SRC_ALPHA;
    case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR:
      return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case VK_BLEND_FACTOR_DST_COLOR:
      return VK_BLEND_FACTOR_DST_ALPHA;
    case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR:
      return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    default:
      return factor;
  }
}

void SetViewport(VkCommandBuffer command_buffer, int x, int y, int width, int height, float min_depth /*= 0.0f*/,
                 float max_depth /*= 1.0f*/)
{
  const VkViewport vp{static_cast<float>(x),
                      static_cast<float>(y),
                      static_cast<float>(width),
                      static_cast<float>(height),
                      min_depth,
                      max_depth};
  vkCmdSetViewport(command_buffer, 0, 1, &vp);
}

void SetScissor(VkCommandBuffer command_buffer, int x, int y, int width, int height)
{
  const VkRect2D scissor{{x, y}, {static_cast<u32>(width), static_cast<u32>(height)}};
  vkCmdSetScissor(command_buffer, 0, 1, &scissor);
}

void SetViewportAndScissor(VkCommandBuffer command_buffer, int x, int y, int width, int height,
                           float min_depth /* = 0.0f */, float max_depth /* = 1.0f */)
{
  const VkViewport vp{static_cast<float>(x),
                      static_cast<float>(y),
                      static_cast<float>(width),
                      static_cast<float>(height),
                      min_depth,
                      max_depth};
  const VkRect2D scissor{{x, y}, {static_cast<u32>(width), static_cast<u32>(height)}};
  vkCmdSetViewport(command_buffer, 0, 1, &vp);
  vkCmdSetScissor(command_buffer, 0, 1, &scissor);
}

void SafeDestroyFramebuffer(VkFramebuffer& fb)
{
  if (fb != VK_NULL_HANDLE)
  {
    vkDestroyFramebuffer(g_vulkan_context->GetDevice(), fb, nullptr);
    fb = VK_NULL_HANDLE;
  }
}

void SafeDestroyShaderModule(VkShaderModule& sm)
{
  if (sm != VK_NULL_HANDLE)
  {
    vkDestroyShaderModule(g_vulkan_context->GetDevice(), sm, nullptr);
    sm = VK_NULL_HANDLE;
  }
}

void SafeDestroyPipeline(VkPipeline& p)
{
  if (p != VK_NULL_HANDLE)
  {
    vkDestroyPipeline(g_vulkan_context->GetDevice(), p, nullptr);
    p = VK_NULL_HANDLE;
  }
}

void SafeDestroyPipelineLayout(VkPipelineLayout& pl)
{
  if (pl != VK_NULL_HANDLE)
  {
    vkDestroyPipelineLayout(g_vulkan_context->GetDevice(), pl, nullptr);
    pl = VK_NULL_HANDLE;
  }
}

void SafeDestroyDescriptorSetLayout(VkDescriptorSetLayout& dsl)
{
  if (dsl != VK_NULL_HANDLE)
  {
    vkDestroyDescriptorSetLayout(g_vulkan_context->GetDevice(), dsl, nullptr);
    dsl = VK_NULL_HANDLE;
  }
}

void SafeDestroyBufferView(VkBufferView& bv)
{
  if (bv != VK_NULL_HANDLE)
  {
    vkDestroyBufferView(g_vulkan_context->GetDevice(), bv, nullptr);
    bv = VK_NULL_HANDLE;
  }
}

void SafeDestroyImageView(VkImageView& iv)
{
  if (iv != VK_NULL_HANDLE)
  {
    vkDestroyImageView(g_vulkan_context->GetDevice(), iv, nullptr);
    iv = VK_NULL_HANDLE;
  }
}

void SafeDestroySampler(VkSampler& samp)
{
  if (samp != VK_NULL_HANDLE)
  {
    vkDestroySampler(g_vulkan_context->GetDevice(), samp, nullptr);
    samp = VK_NULL_HANDLE;
  }
}

void SafeDestroySemaphore(VkSemaphore& sem)
{
  if (sem != VK_NULL_HANDLE)
  {
    vkDestroySemaphore(g_vulkan_context->GetDevice(), sem, nullptr);
    sem = VK_NULL_HANDLE;
  }
}

void SafeFreeGlobalDescriptorSet(VkDescriptorSet& ds)
{
  if (ds != VK_NULL_HANDLE)
  {
    g_vulkan_context->FreeGlobalDescriptorSet(ds);
    ds = VK_NULL_HANDLE;
  }
}

void BufferMemoryBarrier(VkCommandBuffer command_buffer, VkBuffer buffer, VkAccessFlags src_access_mask,
                         VkAccessFlags dst_access_mask, VkDeviceSize offset, VkDeviceSize size,
                         VkPipelineStageFlags src_stage_mask, VkPipelineStageFlags dst_stage_mask)
{
  VkBufferMemoryBarrier buffer_info = {
    VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, // VkStructureType    sType
    nullptr,                                 // const void*        pNext
    src_access_mask,                         // VkAccessFlags      srcAccessMask
    dst_access_mask,                         // VkAccessFlags      dstAccessMask
    VK_QUEUE_FAMILY_IGNORED,                 // uint32_t           srcQueueFamilyIndex
    VK_QUEUE_FAMILY_IGNORED,                 // uint32_t           dstQueueFamilyIndex
    buffer,                                  // VkBuffer           buffer
    offset,                                  // VkDeviceSize       offset
    size                                     // VkDeviceSize       size
  };

  vkCmdPipelineBarrier(command_buffer, src_stage_mask, dst_stage_mask, 0, 0, nullptr, 1, &buffer_info, 0, nullptr);
}

VkShaderModule CreateShaderModule(const u32* spv, size_t spv_word_count)
{
  VkShaderModuleCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  info.codeSize = spv_word_count * sizeof(u32);
  info.pCode = spv;

  VkShaderModule module;
  VkResult res = vkCreateShaderModule(g_vulkan_context->GetDevice(), &info, nullptr, &module);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateShaderModule failed: ");
    return VK_NULL_HANDLE;
  }

  return module;
}

VkShaderModule CompileAndCreateVertexShader(std::string_view source_code)
{
  std::optional<ShaderCompiler::SPIRVCodeVector> code = ShaderCompiler::CompileVertexShader(source_code);
  if (!code)
    return VK_NULL_HANDLE;

  return CreateShaderModule(code->data(), code->size());
}

VkShaderModule CompileAndCreateGeometryShader(std::string_view source_code)
{
  std::optional<ShaderCompiler::SPIRVCodeVector> code = ShaderCompiler::CompileGeometryShader(source_code);
  if (!code)
    return VK_NULL_HANDLE;

  return CreateShaderModule(code->data(), code->size());
}

VkShaderModule CompileAndCreateFragmentShader(std::string_view source_code)
{
  std::optional<ShaderCompiler::SPIRVCodeVector> code = ShaderCompiler::CompileFragmentShader(source_code);
  if (!code)
    return VK_NULL_HANDLE;

  return CreateShaderModule(code->data(), code->size());
}

VkShaderModule CompileAndCreateComputeShader(std::string_view source_code)
{
  std::optional<ShaderCompiler::SPIRVCodeVector> code = ShaderCompiler::CompileComputeShader(source_code);
  if (!code)
    return VK_NULL_HANDLE;

  return CreateShaderModule(code->data(), code->size());
}

const char* VkResultToString(VkResult res)
{
  switch (res)
  {
    case VK_SUCCESS:
      return "VK_SUCCESS";

    case VK_NOT_READY:
      return "VK_NOT_READY";

    case VK_TIMEOUT:
      return "VK_TIMEOUT";

    case VK_EVENT_SET:
      return "VK_EVENT_SET";

    case VK_EVENT_RESET:
      return "VK_EVENT_RESET";

    case VK_INCOMPLETE:
      return "VK_INCOMPLETE";

    case VK_ERROR_OUT_OF_HOST_MEMORY:
      return "VK_ERROR_OUT_OF_HOST_MEMORY";

    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
      return "VK_ERROR_OUT_OF_DEVICE_MEMORY";

    case VK_ERROR_INITIALIZATION_FAILED:
      return "VK_ERROR_INITIALIZATION_FAILED";

    case VK_ERROR_DEVICE_LOST:
      return "VK_ERROR_DEVICE_LOST";

    case VK_ERROR_MEMORY_MAP_FAILED:
      return "VK_ERROR_MEMORY_MAP_FAILED";

    case VK_ERROR_LAYER_NOT_PRESENT:
      return "VK_ERROR_LAYER_NOT_PRESENT";

    case VK_ERROR_EXTENSION_NOT_PRESENT:
      return "VK_ERROR_EXTENSION_NOT_PRESENT";

    case VK_ERROR_FEATURE_NOT_PRESENT:
      return "VK_ERROR_FEATURE_NOT_PRESENT";

    case VK_ERROR_INCOMPATIBLE_DRIVER:
      return "VK_ERROR_INCOMPATIBLE_DRIVER";

    case VK_ERROR_TOO_MANY_OBJECTS:
      return "VK_ERROR_TOO_MANY_OBJECTS";

    case VK_ERROR_FORMAT_NOT_SUPPORTED:
      return "VK_ERROR_FORMAT_NOT_SUPPORTED";

    case VK_ERROR_SURFACE_LOST_KHR:
      return "VK_ERROR_SURFACE_LOST_KHR";

    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
      return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";

    case VK_SUBOPTIMAL_KHR:
      return "VK_SUBOPTIMAL_KHR";

    case VK_ERROR_OUT_OF_DATE_KHR:
      return "VK_ERROR_OUT_OF_DATE_KHR";

    case VK_ERROR_INCOMPATIBLE_DISPLAY_KHR:
      return "VK_ERROR_INCOMPATIBLE_DISPLAY_KHR";

    case VK_ERROR_VALIDATION_FAILED_EXT:
      return "VK_ERROR_VALIDATION_FAILED_EXT";

    case VK_ERROR_INVALID_SHADER_NV:
      return "VK_ERROR_INVALID_SHADER_NV";

    default:
      return "UNKNOWN_VK_RESULT";
  }
}
const char* VkImageLayoutToString(VkImageLayout layout)
{
  switch (layout)
  {
    case VK_IMAGE_LAYOUT_UNDEFINED:
      return "VK_IMAGE_LAYOUT_UNDEFINED";

    case VK_IMAGE_LAYOUT_GENERAL:
      return "VK_IMAGE_LAYOUT_GENERAL";

    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
      return "VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL";

    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
      return "VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL";

    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
      return "VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL";

    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
      return "VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL";

    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
      return "VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL";

    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
      return "VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL";

    case VK_IMAGE_LAYOUT_PREINITIALIZED:
      return "VK_IMAGE_LAYOUT_PREINITIALIZED";

    case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL:
      return "VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL";

    case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL:
      return "VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL";

    case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
      return "VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL";

    case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL:
      return "VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL";

    case VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL:
      return "VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL";

    case VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL:
      return "VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL";

    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
      return "VK_IMAGE_LAYOUT_PRESENT_SRC_KHR";

    case VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR:
      return "VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR";

    case VK_IMAGE_LAYOUT_SHADING_RATE_OPTIMAL_NV:
      return "VK_IMAGE_LAYOUT_SHADING_RATE_OPTIMAL_NV";

    case VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT:
      return "VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT";

    default:
      return "UNKNOWN_VK_RESULT";
  }
}

void LogVulkanResult(int level, const char* func_name, VkResult res, const char* msg, ...)
{
  std::va_list ap;
  va_start(ap, msg);
  std::string real_msg = StringUtil::StdStringFromFormatV(msg, ap);
  va_end(ap);

  Log::Writef("Vulkan", func_name, static_cast<LOGLEVEL>(level), "(%s) %s (%d: %s)", func_name, real_msg.c_str(),
              static_cast<int>(res), VkResultToString(res));
}

#ifdef ENABLE_VULKAN_DEBUG_OBJECTS

u8 DebugScope<VkCommandBuffer>::depth = 0;
u8 DebugScope<VkQueue>::depth = 0;

static std::array<float, 4> Palette(float phase, const std::array<float, 3>& a, const std::array<float, 3>& b,
                                    const std::array<float, 3>& c, const std::array<float, 3>& d)
{
  std::array<float, 4> result;
  result[0] = a[0] + b[0] * std::cos(6.28318f * (c[0] * phase + d[0]));
  result[1] = a[1] + b[1] * std::cos(6.28318f * (c[1] * phase + d[1]));
  result[2] = a[2] + b[2] * std::cos(6.28318f * (c[2] * phase + d[2]));
  result[3] = 1.0f;
  return result;
}

DebugScope<VkCommandBuffer>::DebugScope(VkCommandBuffer context, const char* format, ...) : command_buffer(context)
{
  if (command_buffer)
  {
    std::va_list ap;

    SmallString str;
    va_start(ap, format);
    str.FormatVA(format, ap);
    va_end(ap);

    ++depth;
    const float depth_phase = depth / static_cast<float>(max_depth);
    BeginDebugScope(
      command_buffer, str,
      Palette(depth_phase, {0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {1.0f, 1.0f, 0.5f}, {0.8f, 0.90f, 0.30f}));
  }
}

DebugScope<VkCommandBuffer>::~DebugScope()
{
  if (command_buffer)
  {
    --depth;
    EndDebugScope(command_buffer);
  }
}

DebugScope<VkQueue>::DebugScope(VkQueue context, const char* format, ...) : queue(context)
{
  if (queue)
  {
    std::va_list ap;
    va_start(ap, format);

    SmallString str;
    str.FormatVA(format, ap);
    va_end(ap);

    const float depth_phase = depth / static_cast<float>(max_depth);
    BeginDebugScope(queue, str,
                    Palette(depth_phase, {0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {2.0f, 1.0f, 0.0f}, {0.5f, 0.20f, 0.25f}));
    ++depth;
  }
}

DebugScope<VkQueue>::~DebugScope()
{
  if (queue)
  {
    --depth;
    EndDebugScope(queue);
  }
}

#endif

} // namespace Util

} // namespace Vulkan
