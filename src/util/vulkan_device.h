// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "gpu_device.h"
#include "gpu_framebuffer_manager.h"
#include "gpu_texture.h"
#include "vulkan_loader.h"
#include "vulkan_stream_buffer.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class VulkanPipeline;
class VulkanSwapChain;
class VulkanTexture;
class VulkanTextureBuffer;
class VulkanDownloadTexture;

struct VK_PIPELINE_CACHE_HEADER;

class VulkanDevice final : public GPUDevice
{
public:
  friend VulkanTexture;
  friend VulkanDownloadTexture;

  enum : u32
  {
    NUM_COMMAND_BUFFERS = 3,
  };

  struct OptionalExtensions
  {
    bool vk_ext_memory_budget : 1;
    bool vk_ext_rasterization_order_attachment_access : 1;
    bool vk_ext_attachment_feedback_loop_layout : 1;
    bool vk_ext_full_screen_exclusive : 1;
    bool vk_khr_get_memory_requirements2 : 1;
    bool vk_khr_bind_memory2 : 1;
    bool vk_khr_get_physical_device_properties2 : 1;
    bool vk_khr_dedicated_allocation : 1;
    bool vk_khr_driver_properties : 1;
    bool vk_khr_dynamic_rendering : 1;
    bool vk_khr_push_descriptor : 1;
    bool vk_ext_external_memory_host : 1;
  };

  static GPUTexture::Format GetFormatForVkFormat(VkFormat format);

  static const std::array<VkFormat, static_cast<u32>(GPUTexture::Format::MaxCount)> TEXTURE_FORMAT_MAPPING;

public:
  VulkanDevice();
  ~VulkanDevice() override;

  RenderAPI GetRenderAPI() const override;

  bool HasSurface() const override;

  bool UpdateWindow() override;
  void ResizeWindow(s32 new_window_width, s32 new_window_height, float new_window_scale) override;

  static AdapterAndModeList StaticGetAdapterAndModeList();
  AdapterAndModeList GetAdapterAndModeList() override;
  void DestroySurface() override;

  std::string GetDriverInfo() const override;

  std::unique_ptr<GPUTexture> CreateTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                            GPUTexture::Type type, GPUTexture::Format format,
                                            const void* data = nullptr, u32 data_stride = 0) override;
  std::unique_ptr<GPUSampler> CreateSampler(const GPUSampler::Config& config) override;
  std::unique_ptr<GPUTextureBuffer> CreateTextureBuffer(GPUTextureBuffer::Format format, u32 size_in_elements) override;

  std::unique_ptr<GPUDownloadTexture> CreateDownloadTexture(u32 width, u32 height, GPUTexture::Format format) override;
  std::unique_ptr<GPUDownloadTexture> CreateDownloadTexture(u32 width, u32 height, GPUTexture::Format format,
                                                            void* memory, size_t memory_size,
                                                            u32 memory_stride) override;

  bool SupportsTextureFormat(GPUTexture::Format format) const override;
  void CopyTextureRegion(GPUTexture* dst, u32 dst_x, u32 dst_y, u32 dst_layer, u32 dst_level, GPUTexture* src,
                         u32 src_x, u32 src_y, u32 src_layer, u32 src_level, u32 width, u32 height) override;
  void ResolveTextureRegion(GPUTexture* dst, u32 dst_x, u32 dst_y, u32 dst_layer, u32 dst_level, GPUTexture* src,
                            u32 src_x, u32 src_y, u32 width, u32 height) override;
  void ClearRenderTarget(GPUTexture* t, u32 c) override;
  void ClearDepth(GPUTexture* t, float d) override;
  void InvalidateRenderTarget(GPUTexture* t) override;

  std::unique_ptr<GPUShader> CreateShaderFromBinary(GPUShaderStage stage, std::span<const u8> data) override;
  std::unique_ptr<GPUShader> CreateShaderFromSource(GPUShaderStage stage, const std::string_view& source,
                                                    const char* entry_point, DynamicHeapArray<u8>* out_binary) override;
  std::unique_ptr<GPUPipeline> CreatePipeline(const GPUPipeline::GraphicsConfig& config) override;

  void PushDebugGroup(const char* name) override;
  void PopDebugGroup() override;
  void InsertDebugMessage(const char* msg) override;

  void MapVertexBuffer(u32 vertex_size, u32 vertex_count, void** map_ptr, u32* map_space,
                       u32* map_base_vertex) override;
  void UnmapVertexBuffer(u32 vertex_size, u32 vertex_count) override;
  void MapIndexBuffer(u32 index_count, DrawIndex** map_ptr, u32* map_space, u32* map_base_index) override;
  void UnmapIndexBuffer(u32 used_index_count) override;
  void PushUniformBuffer(const void* data, u32 data_size) override;
  void* MapUniformBuffer(u32 size) override;
  void UnmapUniformBuffer(u32 size) override;
  void SetRenderTargets(GPUTexture* const* rts, u32 num_rts, GPUTexture* ds) override;
  void SetPipeline(GPUPipeline* pipeline) override;
  void SetTextureSampler(u32 slot, GPUTexture* texture, GPUSampler* sampler) override;
  void SetTextureBuffer(u32 slot, GPUTextureBuffer* buffer) override;
  void SetViewport(s32 x, s32 y, s32 width, s32 height) override;
  void SetScissor(s32 x, s32 y, s32 width, s32 height) override;
  void Draw(u32 vertex_count, u32 base_vertex) override;
  void DrawIndexed(u32 index_count, u32 base_index, u32 base_vertex) override;

  bool SetGPUTimingEnabled(bool enabled) override;
  float GetAndResetAccumulatedGPUTime() override;

  void SetSyncMode(DisplaySyncMode mode) override;

  bool BeginPresent(bool skip_present) override;
  void EndPresent() override;

  // Global state accessors
  ALWAYS_INLINE static VulkanDevice& GetInstance() { return *static_cast<VulkanDevice*>(g_gpu_device.get()); }
  ALWAYS_INLINE VkInstance GetVulkanInstance() const { return m_instance; }
  ALWAYS_INLINE VkDevice GetVulkanDevice() const { return m_device; }
  ALWAYS_INLINE VmaAllocator GetAllocator() const { return m_allocator; }
  ALWAYS_INLINE VkPhysicalDevice GetVulkanPhysicalDevice() const { return m_physical_device; }
  ALWAYS_INLINE u32 GetGraphicsQueueFamilyIndex() const { return m_graphics_queue_family_index; }
  ALWAYS_INLINE u32 GetPresentQueueFamilyIndex() const { return m_present_queue_family_index; }
  ALWAYS_INLINE const OptionalExtensions& GetOptionalExtensions() const { return m_optional_extensions; }

  /// Returns true if Vulkan is suitable as a default for the devices in the system.
  static bool IsSuitableDefaultRenderer();

  // The interaction between raster order attachment access and fbfetch is unclear.
  ALWAYS_INLINE bool UseFeedbackLoopLayout() const
  {
    return (m_optional_extensions.vk_ext_attachment_feedback_loop_layout &&
            !m_optional_extensions.vk_ext_rasterization_order_attachment_access);
  }

  // Helpers for getting constants
  ALWAYS_INLINE u32 GetBufferCopyOffsetAlignment() const
  {
    return static_cast<u32>(m_device_properties.limits.optimalBufferCopyOffsetAlignment);
  }
  ALWAYS_INLINE u32 GetBufferCopyRowPitchAlignment() const
  {
    return static_cast<u32>(m_device_properties.limits.optimalBufferCopyRowPitchAlignment);
  }

  void WaitForGPUIdle();

  // Creates a simple render pass.
  VkRenderPass GetRenderPass(const GPUPipeline::GraphicsConfig& config);
  VkRenderPass GetRenderPass(GPUTexture* const* rts, u32 num_rts, GPUTexture* ds, bool color_feedback_loop = false,
                             bool depth_sampling = false);
  VkRenderPass GetSwapChainRenderPass(GPUTexture::Format format, VkAttachmentLoadOp load_op);

  // Gets a non-clearing version of the specified render pass. Slow, don't call in hot path.
  VkRenderPass GetRenderPassForRestarting(VkRenderPass pass);

  // These command buffers are allocated per-frame. They are valid until the command buffer
  // is submitted, after that you should call these functions again.
  ALWAYS_INLINE VkCommandBuffer GetCurrentCommandBuffer() const { return m_current_command_buffer; }
  ALWAYS_INLINE VulkanStreamBuffer& GetTextureUploadBuffer() { return m_texture_upload_buffer; }
  VkCommandBuffer GetCurrentInitCommandBuffer();

  /// Allocates a descriptor set from the pool reserved for the current frame.
  VkDescriptorSet AllocateDescriptorSet(VkDescriptorSetLayout set_layout);

  /// Allocates a descriptor set from the pool reserved for the current frame.
  VkDescriptorSet AllocatePersistentDescriptorSet(VkDescriptorSetLayout set_layout);

  /// Frees a descriptor set allocated from the global pool.
  void FreePersistentDescriptorSet(VkDescriptorSet set);

  // Fence "counters" are used to track which commands have been completed by the GPU.
  // If the last completed fence counter is greater or equal to N, it means that the work
  // associated counter N has been completed by the GPU. The value of N to associate with
  // commands can be retreived by calling GetCurrentFenceCounter().
  u64 GetCompletedFenceCounter() const { return m_completed_fence_counter; }

  // Gets the fence that will be signaled when the currently executing command buffer is
  // queued and executed. Do not wait for this fence before the buffer is executed.
  // TODO: move out of struct
  u64 GetCurrentFenceCounter() const { return m_frame_resources[m_current_frame].fence_counter; }

  // Schedule a vulkan resource for destruction later on. This will occur when the command buffer
  // is next re-used, and the GPU has finished working with the specified resource.
  void DeferBufferDestruction(VkBuffer object, VmaAllocation allocation);
  void DeferBufferDestruction(VkBuffer object, VkDeviceMemory memory);
  void DeferFramebufferDestruction(VkFramebuffer object);
  void DeferImageDestruction(VkImage object, VmaAllocation allocation);
  void DeferImageViewDestruction(VkImageView object);
  void DeferPipelineDestruction(VkPipeline object);
  void DeferBufferViewDestruction(VkBufferView object);
  void DeferPersistentDescriptorSetDestruction(VkDescriptorSet object);

  // Wait for a fence to be completed.
  // Also invokes callbacks for completion.
  void WaitForFenceCounter(u64 fence_counter);

  /// Ends any render pass, executes the command buffer, and invalidates cached state.
  void SubmitCommandBuffer(bool wait_for_completion);
  void SubmitCommandBuffer(bool wait_for_completion, const char* reason, ...);
  void SubmitCommandBufferAndRestartRenderPass(const char* reason);

  void UnbindFramebuffer(VulkanTexture* tex);
  void UnbindPipeline(VulkanPipeline* pl);
  void UnbindTexture(VulkanTexture* tex);
  void UnbindTextureBuffer(VulkanTextureBuffer* buf);

protected:
  bool CreateDevice(const std::string_view& adapter, bool threaded_presentation,
                    std::optional<bool> exclusive_fullscreen_control, FeatureMask disabled_features,
                    Error* error) override;
  void DestroyDevice() override;

  bool ReadPipelineCache(const std::string& filename) override;
  bool GetPipelineCacheData(DynamicHeapArray<u8>* data) override;

private:
  enum DIRTY_FLAG : u32
  {
    DIRTY_FLAG_INITIAL = (1 << 0),
    DIRTY_FLAG_PIPELINE_LAYOUT = (1 << 1),
    DIRTY_FLAG_DYNAMIC_OFFSETS = (1 << 2),
    DIRTY_FLAG_TEXTURES_OR_SAMPLERS = (1 << 3),

    ALL_DIRTY_STATE =
      DIRTY_FLAG_INITIAL | DIRTY_FLAG_PIPELINE_LAYOUT | DIRTY_FLAG_DYNAMIC_OFFSETS | DIRTY_FLAG_TEXTURES_OR_SAMPLERS,
  };

  struct RenderPassCacheKey
  {
    struct RenderTarget
    {
      u8 format : 5;
      u8 load_op : 2;
      u8 store_op : 1;
    };
    RenderTarget color[MAX_RENDER_TARGETS];

    u8 depth_format : 5;
    u8 depth_load_op : 2;
    u8 depth_store_op : 1;
    u8 stencil_load_op : 2;
    u8 stencil_store_op : 1;
    u8 depth_sampling : 1;
    u8 color_feedback_loop : 1;
    u8 samples;

    bool operator==(const RenderPassCacheKey& rhs) const;
    bool operator!=(const RenderPassCacheKey& rhs) const;
  };

  struct RenderPassCacheKeyHash
  {
    size_t operator()(const RenderPassCacheKey& rhs) const;
  };

  struct CommandBuffer
  {
    // [0] - Init (upload) command buffer, [1] - draw command buffer
    VkCommandPool command_pool = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, 2> command_buffers{VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    u64 fence_counter = 0;
    bool init_buffer_used = false;
    bool needs_fence_wait = false;
    bool timestamp_written = false;
  };

  using CleanupObjectFunction = void (*)(VulkanDevice& dev, void* obj);
  using SamplerMap = std::unordered_map<u64, VkSampler>;

  static void GetAdapterAndModeList(AdapterAndModeList* ret, VkInstance instance);

  // Helper method to create a Vulkan instance.
  static VkInstance CreateVulkanInstance(const WindowInfo& wi, OptionalExtensions* oe, bool enable_debug_utils,
                                         bool enable_validation_layer);

  // Returns a list of Vulkan-compatible GPUs.
  using GPUList = std::vector<std::pair<VkPhysicalDevice, std::string>>;
  static GPUList EnumerateGPUs(VkInstance instance);

  bool ValidatePipelineCacheHeader(const VK_PIPELINE_CACHE_HEADER& header);
  void FillPipelineCacheHeader(VK_PIPELINE_CACHE_HEADER* header);

  // Enable/disable debug message runtime.
  bool EnableDebugUtils();
  void DisableDebugUtils();

  // Vendor queries.
  bool IsDeviceAdreno() const;
  bool IsDeviceMali() const;
  bool IsDeviceImgTec() const;
  bool IsBrokenMobileDriver() const;

  void SubmitCommandBuffer(VulkanSwapChain* present_swap_chain = nullptr, bool submit_on_thread = false);
  void MoveToNextCommandBuffer();
  void WaitForPresentComplete();

  // Was the last present submitted to the queue a failure? If so, we must recreate our swapchain.
  bool CheckLastPresentFail();
  bool CheckLastSubmitFail();

  using ExtensionList = std::vector<const char*>;
  static bool SelectInstanceExtensions(ExtensionList* extension_list, const WindowInfo& wi, OptionalExtensions* oe,
                                       bool enable_debug_utils);
  bool SelectDeviceExtensions(ExtensionList* extension_list, bool enable_surface);
  bool SelectDeviceFeatures();
  bool CreateDevice(VkSurfaceKHR surface, bool enable_validation_layer);
  void ProcessDeviceExtensions();

  bool CheckFeatures(FeatureMask disabled_features);

  bool CreateAllocator();
  void DestroyAllocator();
  bool CreateCommandBuffers();
  void DestroyCommandBuffers();
  bool CreatePersistentDescriptorPool();
  void DestroyPersistentDescriptorPool();
  bool CreateNullTexture();
  bool CreateBuffers();
  void DestroyBuffers();
  bool CreatePipelineLayouts();
  void DestroyPipelineLayouts();
  bool CreatePersistentDescriptorSets();
  void DestroyPersistentDescriptorSets();
  VkSampler GetSampler(const GPUSampler::Config& config);
  void DestroySamplers();

  void RenderBlankFrame();

  bool TryImportHostMemory(void* data, size_t data_size, VkBufferUsageFlags buffer_usage, VkDeviceMemory* out_memory,
                           VkBuffer* out_buffer, VkDeviceSize* out_offset);

  /// Set dirty flags on everything to force re-bind at next draw time.
  void InvalidateCachedState();

  bool IsRenderTargetBound(const GPUTexture* tex) const;

  /// Applies any changed state.
  VkPipelineLayout GetCurrentVkPipelineLayout() const;
  void SetInitialPipelineState();
  void PreDrawCheck();

  template<GPUPipeline::Layout layout>
  bool UpdateDescriptorSetsForLayout(bool new_layout, bool new_dynamic_offsets);
  bool UpdateDescriptorSets(u32 dirty);

  // Ends a render pass if we're currently in one.
  // When Bind() is next called, the pass will be restarted.
  void BeginRenderPass();
  void BeginSwapChainRenderPass();
  void EndRenderPass();
  bool InRenderPass();

  VkRenderPass CreateCachedRenderPass(RenderPassCacheKey key);
  static VkFramebuffer CreateFramebuffer(GPUTexture* const* rts, u32 num_rts, GPUTexture* ds, u32 flags);
  static void DestroyFramebuffer(VkFramebuffer fbo);

  void BeginCommandBuffer(u32 index);
  void WaitForCommandBufferCompletion(u32 index);

  void DoSubmitCommandBuffer(u32 index, VulkanSwapChain* present_swap_chain);
  void DoPresent(VulkanSwapChain* present_swap_chain, bool acquire_next);
  void WaitForPresentComplete(std::unique_lock<std::mutex>& lock);
  void PresentThread();
  void StartPresentThread();
  void StopPresentThread();

  VkInstance m_instance = VK_NULL_HANDLE;
  VkPhysicalDevice m_physical_device = VK_NULL_HANDLE;
  VkDevice m_device = VK_NULL_HANDLE;
  VmaAllocator m_allocator = VK_NULL_HANDLE;

  VkCommandBuffer m_current_command_buffer = VK_NULL_HANDLE;

  VkDescriptorPool m_global_descriptor_pool = VK_NULL_HANDLE;

  VkQueue m_graphics_queue = VK_NULL_HANDLE;
  VkQueue m_present_queue = VK_NULL_HANDLE;
  u32 m_graphics_queue_family_index = 0;
  u32 m_present_queue_family_index = 0;

  VkQueryPool m_timestamp_query_pool = VK_NULL_HANDLE;
  float m_accumulated_gpu_time = 0.0f;

  std::array<CommandBuffer, NUM_COMMAND_BUFFERS> m_frame_resources;
  std::deque<std::pair<u64, std::function<void()>>> m_cleanup_objects; // [fence_counter, callback]
  u64 m_next_fence_counter = 1;
  u64 m_completed_fence_counter = 0;
  u32 m_current_frame = 0;

  std::atomic_bool m_last_submit_failed{false};
  std::atomic_bool m_last_present_failed{false};
  std::atomic_bool m_present_done{true};
  std::mutex m_present_mutex;
  std::condition_variable m_present_queued_cv;
  std::condition_variable m_present_done_cv;
  std::thread m_present_thread;
  std::atomic_bool m_present_thread_done{false};

  struct QueuedPresent
  {
    VulkanSwapChain* swap_chain;
    u32 command_buffer_index;
  };

  QueuedPresent m_queued_present = {nullptr, 0xFFFFFFFFu};

  std::unordered_map<RenderPassCacheKey, VkRenderPass, RenderPassCacheKeyHash> m_render_pass_cache;
  GPUFramebufferManager<VkFramebuffer, CreateFramebuffer, DestroyFramebuffer> m_framebuffer_manager;
  VkPipelineCache m_pipeline_cache = VK_NULL_HANDLE;

  // TODO: Move to static?
  VkDebugUtilsMessengerEXT m_debug_messenger_callback = VK_NULL_HANDLE;

  VkPhysicalDeviceFeatures m_device_features = {};
  VkPhysicalDeviceProperties m_device_properties = {};
  VkPhysicalDeviceDriverPropertiesKHR m_device_driver_properties = {};
  OptionalExtensions m_optional_extensions = {};
  std::optional<bool> m_exclusive_fullscreen_control;

  std::unique_ptr<VulkanSwapChain> m_swap_chain;
  std::unique_ptr<VulkanTexture> m_null_texture;

  VkDescriptorSetLayout m_ubo_ds_layout = VK_NULL_HANDLE;
  VkDescriptorSetLayout m_single_texture_ds_layout = VK_NULL_HANDLE;
  VkDescriptorSetLayout m_single_texture_buffer_ds_layout = VK_NULL_HANDLE;
  VkDescriptorSetLayout m_multi_texture_ds_layout = VK_NULL_HANDLE;
  std::array<VkPipelineLayout, static_cast<u8>(GPUPipeline::Layout::MaxCount)> m_pipeline_layouts = {};

  VulkanStreamBuffer m_vertex_buffer;
  VulkanStreamBuffer m_index_buffer;
  VulkanStreamBuffer m_uniform_buffer;
  VulkanStreamBuffer m_texture_upload_buffer;

  VkDescriptorSet m_ubo_descriptor_set = VK_NULL_HANDLE;
  u32 m_uniform_buffer_position = 0;

  SamplerMap m_sampler_map;

  // Which bindings/state has to be updated before the next draw.
  u32 m_dirty_flags = ALL_DIRTY_STATE;

  u32 m_num_current_render_targets = 0;
  std::array<GPUTexture*, MAX_RENDER_TARGETS> m_current_render_targets = {};
  GPUTexture* m_current_depth_target = nullptr;
  VkFramebuffer m_current_framebuffer = VK_NULL_HANDLE;
  VkRenderPass m_current_render_pass = VK_NULL_HANDLE;

  VulkanPipeline* m_current_pipeline = nullptr;
  GPUPipeline::Layout m_current_pipeline_layout = GPUPipeline::Layout::SingleTextureAndPushConstants;

  std::array<VulkanTexture*, MAX_TEXTURE_SAMPLERS> m_current_textures = {};
  std::array<VkSampler, MAX_TEXTURE_SAMPLERS> m_current_samplers = {};
  VulkanTextureBuffer* m_current_texture_buffer = nullptr;
  Common::Rectangle<s32> m_current_viewport{0, 0, 1, 1};
  Common::Rectangle<s32> m_current_scissor{0, 0, 1, 1};
};
