// Copyright 2016 Dolphin Emulator Project
// Copyright 2020 DuckStation Emulator Project
// Licensed under GPLv2+
// Refer to the LICENSE file included.

#pragma once

#include "../types.h"
#include "loader.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct WindowInfo;

namespace Vulkan {

class SwapChain;

class Context
{
public:
  enum : u32
  {
    NUM_COMMAND_BUFFERS = 2
  };

  ~Context();

  // Determines if the Vulkan validation layer is available on the system.
  static bool CheckValidationLayerAvailablility();

  // Helper method to create a Vulkan instance.
  static VkInstance CreateVulkanInstance(const WindowInfo* wi, bool enable_debug_utils, bool enable_validation_layer);

  // Returns a list of Vulkan-compatible GPUs.
  using GPUList = std::vector<VkPhysicalDevice>;
  using GPUNameList = std::vector<std::string>;
  static GPUList EnumerateGPUs(VkInstance instance);
  static GPUNameList EnumerateGPUNames(VkInstance instance);

  // Creates a new context and sets it up as global.
  static bool Create(std::string_view gpu_name, const WindowInfo* wi, std::unique_ptr<SwapChain>* out_swap_chain,
                     bool threaded_presentation, bool enable_debug_utils, bool enable_validation_layer);

  // Creates a new context from a pre-existing instance.
  static bool CreateFromExistingInstance(VkInstance instance, VkPhysicalDevice gpu, VkSurfaceKHR surface,
                                         bool take_ownership, bool enable_validation_layer, bool enable_debug_utils,
                                         const char** required_device_extensions = nullptr,
                                         u32 num_required_device_extensions = 0,
                                         const char** required_device_layers = nullptr,
                                         u32 num_required_device_layers = 0,
                                         const VkPhysicalDeviceFeatures* required_features = nullptr);

  // Destroys context.
  static void Destroy();

  // Enable/disable debug message runtime.
  bool EnableDebugUtils();
  void DisableDebugUtils();

  // Global state accessors
  ALWAYS_INLINE VkInstance GetVulkanInstance() const { return m_instance; }
  ALWAYS_INLINE VkPhysicalDevice GetPhysicalDevice() const { return m_physical_device; }
  ALWAYS_INLINE VkDevice GetDevice() const { return m_device; }
  ALWAYS_INLINE VkQueue GetGraphicsQueue() const { return m_graphics_queue; }
  ALWAYS_INLINE u32 GetGraphicsQueueFamilyIndex() const { return m_graphics_queue_family_index; }
  ALWAYS_INLINE VkQueue GetPresentQueue() const { return m_present_queue; }
  ALWAYS_INLINE u32 GetPresentQueueFamilyIndex() const { return m_present_queue_family_index; }
  ALWAYS_INLINE const VkQueueFamilyProperties& GetGraphicsQueueProperties() const
  {
    return m_graphics_queue_properties;
  }
  ALWAYS_INLINE const VkPhysicalDeviceMemoryProperties& GetDeviceMemoryProperties() const
  {
    return m_device_memory_properties;
  }
  ALWAYS_INLINE const VkPhysicalDeviceProperties& GetDeviceProperties() const { return m_device_properties; }
  ALWAYS_INLINE const VkPhysicalDeviceFeatures& GetDeviceFeatures() const { return m_device_features; }
  ALWAYS_INLINE const VkPhysicalDeviceLimits& GetDeviceLimits() const { return m_device_properties.limits; }

  // Support bits
  ALWAYS_INLINE bool SupportsGeometryShaders() const { return m_device_features.geometryShader == VK_TRUE; }
  ALWAYS_INLINE bool SupportsDualSourceBlend() const { return m_device_features.dualSrcBlend == VK_TRUE; }

  // Helpers for getting constants
  ALWAYS_INLINE VkDeviceSize GetUniformBufferAlignment() const
  {
    return m_device_properties.limits.minUniformBufferOffsetAlignment;
  }
  ALWAYS_INLINE VkDeviceSize GetTexelBufferAlignment() const
  {
    return m_device_properties.limits.minTexelBufferOffsetAlignment;
  }
  ALWAYS_INLINE VkDeviceSize GetStorageBufferAlignment() const
  {
    return m_device_properties.limits.minStorageBufferOffsetAlignment;
  }
  ALWAYS_INLINE VkDeviceSize GetBufferImageGranularity() const
  {
    return m_device_properties.limits.bufferImageGranularity;
  }

  // Finds a memory type index for the specified memory properties and the bits returned by
  // vkGetImageMemoryRequirements
  bool GetMemoryType(u32 bits, VkMemoryPropertyFlags properties, u32* out_type_index);
  u32 GetMemoryType(u32 bits, VkMemoryPropertyFlags properties);

  // Finds a memory type for upload or readback buffers.
  u32 GetUploadMemoryType(u32 bits, bool* is_coherent = nullptr);
  u32 GetReadbackMemoryType(u32 bits, bool* is_coherent = nullptr, bool* is_cached = nullptr);

  // Creates a simple render pass.
  VkRenderPass GetRenderPass(VkFormat color_format, VkFormat depth_format, VkSampleCountFlagBits samples,
                             VkAttachmentLoadOp load_op);

  // These command buffers are allocated per-frame. They are valid until the command buffer
  // is submitted, after that you should call these functions again.
  ALWAYS_INLINE VkDescriptorPool GetGlobalDescriptorPool() const { return m_global_descriptor_pool; }
  ALWAYS_INLINE VkCommandBuffer GetCurrentCommandBuffer() const { return m_current_command_buffer; }
  ALWAYS_INLINE VkDescriptorPool GetCurrentDescriptorPool() const
  {
    return m_frame_resources[m_current_frame].descriptor_pool;
  }

  /// Allocates a descriptor set from the pool reserved for the current frame.
  VkDescriptorSet AllocateDescriptorSet(VkDescriptorSetLayout set_layout);

  /// Allocates a descriptor set from the pool reserved for the current frame.
  VkDescriptorSet AllocateGlobalDescriptorSet(VkDescriptorSetLayout set_layout);

  /// Frees a descriptor set allocated from the global pool.
  void FreeGlobalDescriptorSet(VkDescriptorSet set);

  // Gets the fence that will be signaled when the currently executing command buffer is
  // queued and executed. Do not wait for this fence before the buffer is executed.
  ALWAYS_INLINE VkFence GetCurrentCommandBufferFence() const { return m_frame_resources[m_current_frame].fence; }

  // Fence "counters" are used to track which commands have been completed by the GPU.
  // If the last completed fence counter is greater or equal to N, it means that the work
  // associated counter N has been completed by the GPU. The value of N to associate with
  // commands can be retreived by calling GetCurrentFenceCounter().
  u64 GetCompletedFenceCounter() const { return m_completed_fence_counter; }

  // Gets the fence that will be signaled when the currently executing command buffer is
  // queued and executed. Do not wait for this fence before the buffer is executed.
  u64 GetCurrentFenceCounter() const { return m_frame_resources[m_current_frame].fence_counter; }

  void SubmitCommandBuffer(VkSemaphore wait_semaphore = VK_NULL_HANDLE, VkSemaphore signal_semaphore = VK_NULL_HANDLE,
                           VkSwapchainKHR present_swap_chain = VK_NULL_HANDLE,
                           uint32_t present_image_index = 0xFFFFFFFF, bool submit_on_thread = false);
  void MoveToNextCommandBuffer();

  void ExecuteCommandBuffer(bool wait_for_completion);
  void WaitForPresentComplete();

  // Was the last present submitted to the queue a failure? If so, we must recreate our swapchain.
  bool CheckLastPresentFail();

  // Schedule a vulkan resource for destruction later on. This will occur when the command buffer
  // is next re-used, and the GPU has finished working with the specified resource.
  void DeferBufferDestruction(VkBuffer object);
  void DeferBufferViewDestruction(VkBufferView object);
  void DeferDeviceMemoryDestruction(VkDeviceMemory object);
  void DeferFramebufferDestruction(VkFramebuffer object);
  void DeferImageDestruction(VkImage object);
  void DeferImageViewDestruction(VkImageView object);
  void DeferPipelineDestruction(VkPipeline pipeline);

  // Wait for a fence to be completed.
  // Also invokes callbacks for completion.
  void WaitForFenceCounter(u64 fence_counter);

  void WaitForGPUIdle();

private:
  Context(VkInstance instance, VkPhysicalDevice physical_device, bool owns_device);

  using ExtensionList = std::vector<const char*>;
  static bool SelectInstanceExtensions(ExtensionList* extension_list, const WindowInfo* wi, bool enable_debug_utils);
  bool SelectDeviceExtensions(ExtensionList* extension_list, bool enable_surface);
  bool SelectDeviceFeatures(const VkPhysicalDeviceFeatures* required_features);
  bool CreateDevice(VkSurfaceKHR surface, bool enable_validation_layer, const char** required_device_extensions,
                    u32 num_required_device_extensions, const char** required_device_layers,
                    u32 num_required_device_layers, const VkPhysicalDeviceFeatures* required_features);

  bool CreateCommandBuffers();
  void DestroyCommandBuffers();
  bool CreateGlobalDescriptorPool();
  void DestroyGlobalDescriptorPool();
  void DestroyRenderPassCache();

  void ActivateCommandBuffer(u32 index);
  void WaitForCommandBufferCompletion(u32 index);

  void DoSubmitCommandBuffer(u32 index, VkSemaphore wait_semaphore, VkSemaphore signal_semaphore);
  void DoPresent(VkSemaphore wait_semaphore, VkSwapchainKHR present_swap_chain, uint32_t present_image_index);
  void WaitForPresentComplete(std::unique_lock<std::mutex>& lock);
  void PresentThread();
  void StartPresentThread();
  void StopPresentThread();

  struct FrameResources
  {
    // [0] - Init (upload) command buffer, [1] - draw command buffer
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    u64 fence_counter = 0;
    bool needs_fence_wait = false;

    std::vector<std::function<void()>> cleanup_resources;
  };

  VkInstance m_instance = VK_NULL_HANDLE;
  VkPhysicalDevice m_physical_device = VK_NULL_HANDLE;
  VkDevice m_device = VK_NULL_HANDLE;

  VkCommandBuffer m_current_command_buffer = VK_NULL_HANDLE;

  VkDescriptorPool m_global_descriptor_pool = VK_NULL_HANDLE;

  VkQueue m_graphics_queue = VK_NULL_HANDLE;
  u32 m_graphics_queue_family_index = 0;
  VkQueue m_present_queue = VK_NULL_HANDLE;
  u32 m_present_queue_family_index = 0;

  std::array<FrameResources, NUM_COMMAND_BUFFERS> m_frame_resources;
  u64 m_next_fence_counter = 1;
  u64 m_completed_fence_counter = 0;
  u32 m_current_frame;

  bool m_owns_device = false;

  std::atomic_bool m_last_present_failed{false};
  std::atomic_bool m_present_done{true};
  std::mutex m_present_mutex;
  std::condition_variable m_present_queued_cv;
  std::condition_variable m_present_done_cv;
  std::thread m_present_thread;
  std::atomic_bool m_present_thread_done{false};

  struct QueuedPresent
  {
    VkSemaphore wait_semaphore;
    VkSemaphore signal_semaphore;
    VkSwapchainKHR present_swap_chain;
    u32 command_buffer_index;
    u32 present_image_index;
  };

  QueuedPresent m_queued_present = {};

  // Render pass cache
  using RenderPassCacheKey = std::tuple<VkFormat, VkFormat, VkSampleCountFlagBits, VkAttachmentLoadOp>;
  std::map<RenderPassCacheKey, VkRenderPass> m_render_pass_cache;

  VkDebugUtilsMessengerEXT m_debug_messenger_callback = VK_NULL_HANDLE;

  VkQueueFamilyProperties m_graphics_queue_properties = {};
  VkPhysicalDeviceFeatures m_device_features = {};
  VkPhysicalDeviceProperties m_device_properties = {};
  VkPhysicalDeviceMemoryProperties m_device_memory_properties = {};
};

} // namespace Vulkan

extern std::unique_ptr<Vulkan::Context> g_vulkan_context;
