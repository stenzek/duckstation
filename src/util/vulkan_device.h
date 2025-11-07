// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "gpu_device.h"
#include "gpu_framebuffer_manager.h"
#include "gpu_texture.h"
#include "vulkan_loader.h"
#include "vulkan_stream_buffer.h"

#include "common/dimensional_array.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <string>
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
    bool vk_ext_external_memory_host : 1;
    bool vk_ext_fragment_shader_interlock : 1;
    bool vk_ext_full_screen_exclusive : 1;
    bool vk_ext_memory_budget : 1;
    bool vk_ext_rasterization_order_attachment_access : 1;
    bool vk_ext_surface_maintenance1 : 1;
    bool vk_ext_swapchain_maintenance1 : 1;
    bool vk_khr_get_physical_device_properties2 : 1;
    bool vk_khr_driver_properties : 1;
    bool vk_khr_dynamic_rendering : 1;
    bool vk_khr_dynamic_rendering_local_read : 1;
    bool vk_khr_get_surface_capabilities2 : 1;
    bool vk_khr_maintenance4 : 1;
    bool vk_khr_maintenance5 : 1;
    bool vk_khr_push_descriptor : 1;
    bool vk_khr_shader_non_semantic_info : 1;
  };

  static GPUTexture::Format GetFormatForVkFormat(VkFormat format);

  static const std::array<VkFormat, static_cast<u32>(GPUTexture::Format::MaxCount)> TEXTURE_FORMAT_MAPPING;

public:
  VulkanDevice();
  ~VulkanDevice() override;

  // Returns a list of Vulkan-compatible GPUs.
  using GPUList = std::vector<std::pair<VkPhysicalDevice, AdapterInfo>>;
  static GPUList EnumerateGPUs(VkInstance instance);
  static GPUList EnumerateGPUs();
  static AdapterInfoList GetAdapterList();

  std::string GetDriverInfo() const override;

  void FlushCommands() override;
  void WaitForGPUIdle() override;

  std::unique_ptr<GPUSwapChain> CreateSwapChain(const WindowInfo& wi, GPUVSyncMode vsync_mode,
                                                bool allow_present_throttle,
                                                const ExclusiveFullscreenMode* exclusive_fullscreen_mode,
                                                std::optional<bool> exclusive_fullscreen_control,
                                                Error* error) override;
  std::unique_ptr<GPUTexture> CreateTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                            GPUTexture::Type type, GPUTexture::Format format, GPUTexture::Flags flags,
                                            const void* data = nullptr, u32 data_stride = 0,
                                            Error* error = nullptr) override;
  std::unique_ptr<GPUSampler> CreateSampler(const GPUSampler::Config& config, Error* error = nullptr) override;
  std::unique_ptr<GPUTextureBuffer> CreateTextureBuffer(GPUTextureBuffer::Format format, u32 size_in_elements,
                                                        Error* error = nullptr) override;

  std::unique_ptr<GPUDownloadTexture> CreateDownloadTexture(u32 width, u32 height, GPUTexture::Format format,
                                                            Error* error = nullptr) override;
  std::unique_ptr<GPUDownloadTexture> CreateDownloadTexture(u32 width, u32 height, GPUTexture::Format format,
                                                            void* memory, size_t memory_size, u32 memory_stride,
                                                            Error* error = nullptr) override;

  bool SupportsTextureFormat(GPUTexture::Format format) const override;
  void CopyTextureRegion(GPUTexture* dst, u32 dst_x, u32 dst_y, u32 dst_layer, u32 dst_level, GPUTexture* src,
                         u32 src_x, u32 src_y, u32 src_layer, u32 src_level, u32 width, u32 height) override;
  void ResolveTextureRegion(GPUTexture* dst, u32 dst_x, u32 dst_y, u32 dst_layer, u32 dst_level, GPUTexture* src,
                            u32 src_x, u32 src_y, u32 width, u32 height) override;
  void ClearRenderTarget(GPUTexture* t, u32 c) override;
  void ClearDepth(GPUTexture* t, float d) override;
  void InvalidateRenderTarget(GPUTexture* t) override;

  std::unique_ptr<GPUShader> CreateShaderFromBinary(GPUShaderStage stage, std::span<const u8> data,
                                                    Error* error) override;
  std::unique_ptr<GPUShader> CreateShaderFromSource(GPUShaderStage stage, GPUShaderLanguage language,
                                                    std::string_view source, const char* entry_point,
                                                    DynamicHeapArray<u8>* out_binary, Error* error) override;
  std::unique_ptr<GPUPipeline> CreatePipeline(const GPUPipeline::GraphicsConfig& config, Error* error) override;
  std::unique_ptr<GPUPipeline> CreatePipeline(const GPUPipeline::ComputeConfig& config, Error* error) override;

#ifdef ENABLE_GPU_OBJECT_NAMES
  void PushDebugGroup(const char* name) override;
  void PopDebugGroup() override;
  void InsertDebugMessage(const char* msg) override;
#endif

  void MapVertexBuffer(u32 vertex_size, u32 vertex_count, void** map_ptr, u32* map_space,
                       u32* map_base_vertex) override;
  void UnmapVertexBuffer(u32 vertex_size, u32 vertex_count) override;
  void MapIndexBuffer(u32 index_count, DrawIndex** map_ptr, u32* map_space, u32* map_base_index) override;
  void UnmapIndexBuffer(u32 used_index_count) override;
  void* MapUniformBuffer(u32 size) override;
  void UnmapUniformBuffer(u32 size) override;
  void SetRenderTargets(GPUTexture* const* rts, u32 num_rts, GPUTexture* ds,
                        GPUPipeline::RenderPassFlag flags = GPUPipeline::NoRenderPassFlags) override;
  void SetPipeline(GPUPipeline* pipeline) override;
  void SetTextureSampler(u32 slot, GPUTexture* texture, GPUSampler* sampler) override;
  void SetTextureBuffer(u32 slot, GPUTextureBuffer* buffer) override;
  void SetViewport(const GSVector4i rc) override;
  void SetScissor(const GSVector4i rc) override;
  void Draw(u32 vertex_count, u32 base_vertex) override;
  void DrawWithPushConstants(u32 vertex_count, u32 base_vertex, const void* push_constants,
                             u32 push_constants_size) override;
  void DrawIndexed(u32 index_count, u32 base_index, u32 base_vertex) override;
  void DrawIndexedWithPushConstants(u32 index_count, u32 base_index, u32 base_vertex, const void* push_constants,
                                    u32 push_constants_size) override;
  void DrawIndexedWithBarrier(u32 index_count, u32 base_index, u32 base_vertex, DrawBarrier type) override;
  void DrawIndexedWithBarrierWithPushConstants(u32 index_count, u32 base_index, u32 base_vertex,
                                               const void* push_constants, u32 push_constants_size,
                                               DrawBarrier type) override;
  void Dispatch(u32 threads_x, u32 threads_y, u32 threads_z, u32 group_size_x, u32 group_size_y,
                u32 group_size_z) override;
  void DispatchWithPushConstants(u32 threads_x, u32 threads_y, u32 threads_z, u32 group_size_x, u32 group_size_y,
                                 u32 group_size_z, const void* push_constants, u32 push_constants_size) override;

  bool SetGPUTimingEnabled(bool enabled) override;
  float GetAndResetAccumulatedGPUTime() override;

  PresentResult BeginPresent(GPUSwapChain* swap_chain, u32 clear_color) override;
  void EndPresent(GPUSwapChain* swap_chain, bool explicit_present, u64 present_time) override;
  void SubmitPresent(GPUSwapChain* swap_chain) override;

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

  // Helpers for getting constants
  ALWAYS_INLINE u32 GetBufferCopyOffsetAlignment() const
  {
    return static_cast<u32>(m_device_properties.limits.optimalBufferCopyOffsetAlignment);
  }
  ALWAYS_INLINE u32 GetBufferCopyRowPitchAlignment() const
  {
    return static_cast<u32>(m_device_properties.limits.optimalBufferCopyRowPitchAlignment);
  }

  void WaitForAllFences();

  // Creates a simple render pass.
  VkRenderPass GetRenderPass(const GPUPipeline::GraphicsConfig& config);
  VkRenderPass GetRenderPass(VulkanTexture* const* rts, u32 num_rts, VulkanTexture* ds,
                             GPUPipeline::RenderPassFlag render_pass_flags);
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
  void DeferSamplerDestruction(VkSampler object);

  // Wait for a fence to be completed.
  // Also invokes callbacks for completion.
  void WaitForFenceCounter(u64 fence_counter);

  // Ends a render pass if we're currently in one.
  // When Bind() is next called, the pass will be restarted.
  void BeginRenderPass();
  void EndRenderPass();
  bool InRenderPass();

  /// Ends any render pass, executes the command buffer, and invalidates cached state.
  void SubmitCommandBuffer(bool wait_for_completion);
  void SubmitCommandBuffer(bool wait_for_completion, const std::string_view reason);
  void SubmitCommandBufferAndRestartRenderPass(const std::string_view reason);

  void UnbindPipeline(VulkanPipeline* pl);
  void UnbindTexture(VulkanTexture* tex);
  void UnbindTextureBuffer(VulkanTextureBuffer* buf);

protected:
  bool CreateDeviceAndMainSwapChain(std::string_view adapter, CreateFlags create_flags, const WindowInfo& wi,
                                    GPUVSyncMode vsync_mode, bool allow_present_throttle,
                                    const ExclusiveFullscreenMode* exclusive_fullscreen_mode,
                                    std::optional<bool> exclusive_fullscreen_control, Error* error) override;
  void DestroyDevice() override;

  bool ReadPipelineCache(DynamicHeapArray<u8> data, Error* error) override;
  bool CreatePipelineCache(const std::string& path, Error* error) override;
  bool GetPipelineCacheData(DynamicHeapArray<u8>* data, Error* error) override;

private:
  enum DIRTY_FLAG : u32
  {
    DIRTY_FLAG_INITIAL = (1 << 0),
    DIRTY_FLAG_PIPELINE_LAYOUT = (1 << 1),
    DIRTY_FLAG_DYNAMIC_OFFSETS = (1 << 2),
    DIRTY_FLAG_TEXTURES_OR_SAMPLERS = (1 << 3),
    DIRTY_FLAG_INPUT_ATTACHMENT = (1 << 4),

    ALL_DIRTY_STATE = DIRTY_FLAG_INITIAL | DIRTY_FLAG_PIPELINE_LAYOUT | DIRTY_FLAG_DYNAMIC_OFFSETS |
                      DIRTY_FLAG_TEXTURES_OR_SAMPLERS | DIRTY_FLAG_INPUT_ATTACHMENT,
  };

  enum class PipelineLayoutType : u8
  {
    Normal,
    ColorFeedbackLoop,
    BindRenderTargetsAsImages,
    MaxCount,
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
    u8 feedback_loop : 2;
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
    bool needs_descriptor_pool_reset = false;
    bool timestamp_written = false;
  };

  using CleanupObjectFunction = void (*)(VulkanDevice& dev, void* obj);

  // Helper method to create a Vulkan instance.
  static VkInstance CreateVulkanInstance(const WindowInfo& wi, OptionalExtensions* oe, bool enable_debug_utils,
                                         bool enable_validation_layer);

  bool ValidatePipelineCacheHeader(const VK_PIPELINE_CACHE_HEADER& header, Error* error);
  void FillPipelineCacheHeader(VK_PIPELINE_CACHE_HEADER* header);

  // Enable/disable debug message runtime.
  bool EnableDebugUtils();
  void DisableDebugUtils();

  using ExtensionList = std::vector<const char*>;
  static bool SelectInstanceExtensions(ExtensionList* extension_list, const WindowInfo& wi, OptionalExtensions* oe,
                                       bool enable_debug_utils);
  bool CreateDevice(VkPhysicalDevice physical_device, VkSurfaceKHR surface, bool enable_validation_layer,
                    CreateFlags create_flags, Error* error);
  bool EnableOptionalDeviceExtensions(VkPhysicalDevice physical_device,
                                      std::span<const VkExtensionProperties> available_extensions,
                                      ExtensionList& enabled_extensions, VkPhysicalDeviceFeatures& enabled_features,
                                      bool enable_surface, Error* error);
  void SetFeatures(CreateFlags create_flags, VkPhysicalDevice physical_device,
                   const VkPhysicalDeviceFeatures& vk_features);

  static GPUDriverType GuessDriverType(const VkPhysicalDeviceProperties& device_properties,
                                       const VkPhysicalDeviceDriverProperties& driver_properties);
  static u32 GetMaxMultisamples(VkPhysicalDevice physical_device, const VkPhysicalDeviceProperties& properties);

  bool CreateAllocator();
  void DestroyAllocator();
  bool CreateCommandBuffers();
  void DestroyCommandBuffers();
  bool CreatePersistentDescriptorPool();
  void DestroyPersistentDescriptorPool();
  bool CreateNullTexture(Error* error);
  bool CreateBuffers();
  void DestroyBuffers();
  bool CreatePipelineLayouts();
  void DestroyPipelineLayouts();
  bool CreatePersistentDescriptorSets();
  void DestroyPersistentDescriptorSets();

  void RenderBlankFrame(VulkanSwapChain* swap_chain);

  bool TryImportHostMemory(void* data, size_t data_size, VkBufferUsageFlags buffer_usage, VkDeviceMemory* out_memory,
                           VkBuffer* out_buffer, VkDeviceSize* out_offset, Error* error);

  /// Set dirty flags on everything to force re-bind at next draw time.
  void InvalidateCachedState();

  s32 IsRenderTargetBoundIndex(const GPUTexture* tex) const;

  /// Applies any changed state.
  static PipelineLayoutType GetPipelineLayoutType(GPUPipeline::RenderPassFlag flags);
  VkPipelineLayout GetCurrentVkPipelineLayout(bool is_compute) const;
  void SetInitialPipelineState();
  void PreDrawCheck();
  void PreDispatchCheck();
  void PushUniformBuffer(bool is_compute, const void* data, u32 data_size);
  void SubmitDrawIndexedWithBarrier(u32 index_count, u32 base_index, u32 base_vertex, DrawBarrier type);

  template<GPUPipeline::Layout layout>
  bool UpdateDescriptorSetsForLayout(u32 dirty);
  bool UpdateDescriptorSets(u32 dirty);

  void BeginSwapChainRenderPass(VulkanSwapChain* swap_chain, u32 clear_color);

  VkRenderPass CreateCachedRenderPass(RenderPassCacheKey key);
  static VkFramebuffer CreateFramebuffer(GPUTexture* const* rts, u32 num_rts, GPUTexture* ds, u32 flags);
  static void DestroyFramebuffer(VkFramebuffer fbo);

  VkImageMemoryBarrier GetColorBufferBarrier(const VulkanTexture* rt) const;

  void BeginCommandBuffer(u32 index);
  void WaitForCommandBufferCompletion(u32 index);
  void EndAndSubmitCommandBuffer(VulkanSwapChain* present_swap_chain, bool explicit_present);
  void QueuePresent(VulkanSwapChain* present_swap_chain);

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

  bool m_device_was_lost = false;

  std::unordered_map<RenderPassCacheKey, VkRenderPass, RenderPassCacheKeyHash> m_render_pass_cache;
  GPUFramebufferManager<VkFramebuffer, CreateFramebuffer, DestroyFramebuffer> m_framebuffer_manager;
  VkPipelineCache m_pipeline_cache = VK_NULL_HANDLE;

  // TODO: Move to static?
  VkDebugUtilsMessengerEXT m_debug_messenger_callback = VK_NULL_HANDLE;

  VkPhysicalDeviceProperties m_device_properties = {};
  VkPhysicalDeviceDriverProperties m_device_driver_properties = {};
  OptionalExtensions m_optional_extensions = {};
  std::optional<bool> m_exclusive_fullscreen_control;

  VkDescriptorSetLayout m_ubo_ds_layout = VK_NULL_HANDLE;
  VkDescriptorSetLayout m_single_texture_ds_layout = VK_NULL_HANDLE;
  VkDescriptorSetLayout m_single_texture_buffer_ds_layout = VK_NULL_HANDLE;
  VkDescriptorSetLayout m_multi_texture_ds_layout = VK_NULL_HANDLE;
  VkDescriptorSetLayout m_feedback_loop_ds_layout = VK_NULL_HANDLE;
  VkDescriptorSetLayout m_image_ds_layout = VK_NULL_HANDLE;
  DimensionalArray<VkPipelineLayout, static_cast<size_t>(GPUPipeline::Layout::MaxCount),
                   static_cast<size_t>(PipelineLayoutType::MaxCount)>
    m_pipeline_layouts = {};

  VulkanStreamBuffer m_vertex_buffer;
  VulkanStreamBuffer m_index_buffer;
  VulkanStreamBuffer m_uniform_buffer;
  VulkanStreamBuffer m_texture_upload_buffer;

  VkDescriptorSet m_ubo_descriptor_set = VK_NULL_HANDLE;
  u32 m_uniform_buffer_position = 0;

  // Which bindings/state has to be updated before the next draw.
  u32 m_dirty_flags = ALL_DIRTY_STATE;

  u32 m_num_current_render_targets = 0;
  GPUPipeline::RenderPassFlag m_current_render_pass_flags = GPUPipeline::NoRenderPassFlags;
  std::array<VulkanTexture*, MAX_RENDER_TARGETS> m_current_render_targets = {};
  VulkanTexture* m_current_depth_target = nullptr;
  VkFramebuffer m_current_framebuffer = VK_NULL_HANDLE;
  VkRenderPass m_current_render_pass = VK_NULL_HANDLE;

  VulkanPipeline* m_current_pipeline = nullptr;
  GPUPipeline::Layout m_current_pipeline_layout = GPUPipeline::Layout::SingleTextureAndPushConstants;

  std::array<VulkanTexture*, MAX_TEXTURE_SAMPLERS> m_current_textures = {};
  std::array<VkSampler, MAX_TEXTURE_SAMPLERS> m_current_samplers = {};
  VulkanTextureBuffer* m_current_texture_buffer = nullptr;
  GSVector4i m_current_viewport = GSVector4i::cxpr(0, 0, 1, 1);
  GSVector4i m_current_scissor = GSVector4i::cxpr(0, 0, 1, 1);
  VulkanSwapChain* m_current_swap_chain = nullptr;
};
