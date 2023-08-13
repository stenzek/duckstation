// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "gpu_shader_cache.h"
#include "gpu_texture.h"
#include "window_info.h"

#include "common/bitfield.h"
#include "common/heap_array.h"
#include "common/rectangle.h"
#include "common/types.h"

#include "gsl/span"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

enum class RenderAPI : u32
{
  None,
  D3D11,
  D3D12,
  Vulkan,
  OpenGL,
  OpenGLES,
  Metal
};

class GPUFramebuffer
{
public:
  GPUFramebuffer(GPUTexture* rt, GPUTexture* ds, u32 width, u32 height);
  virtual ~GPUFramebuffer();

  ALWAYS_INLINE GPUTexture* GetRT() const { return m_rt; }
  ALWAYS_INLINE GPUTexture* GetDS() const { return m_ds; }

  ALWAYS_INLINE u32 GetWidth() const { return m_width; }
  ALWAYS_INLINE u32 GetHeight() const { return m_height; }

  virtual void SetDebugName(const std::string_view& name) = 0;

protected:
  GPUTexture* m_rt;
  GPUTexture* m_ds;
  u32 m_width;
  u32 m_height;
};

class GPUSampler
{
public:
  enum class Filter
  {
    Nearest,
    Linear,

    MaxCount
  };

  enum class AddressMode
  {
    Repeat,
    ClampToEdge,
    ClampToBorder,

    MaxCount
  };

  union Config
  {
    static constexpr u8 LOD_MAX = 15;

    BitField<u64, Filter, 0, 1> min_filter;
    BitField<u64, Filter, 1, 1> mag_filter;
    BitField<u64, Filter, 2, 1> mip_filter;
    BitField<u64, AddressMode, 3, 2> address_u;
    BitField<u64, AddressMode, 5, 2> address_v;
    BitField<u64, AddressMode, 7, 2> address_w;
    BitField<u64, u8, 9, 5> anisotropy;
    BitField<u64, u8, 14, 4> min_lod;
    BitField<u64, u8, 18, 4> max_lod;
    BitField<u64, u32, 32, 32> border_color;
    u64 key;

    // clang-format off
    ALWAYS_INLINE float GetBorderRed() const { return static_cast<float>(border_color.GetValue() & 0xFF) / 255.0f; }
    ALWAYS_INLINE float GetBorderGreen() const { return static_cast<float>((border_color.GetValue() >> 8) & 0xFF) / 255.0f; }
    ALWAYS_INLINE float GetBorderBlue() const { return static_cast<float>((border_color.GetValue() >> 16) & 0xFF) / 255.0f; }
    ALWAYS_INLINE float GetBorderAlpha() const { return static_cast<float>((border_color.GetValue() >> 24) & 0xFF) / 255.0f; }
    // clang-format on
    ALWAYS_INLINE std::array<float, 4> GetBorderFloatColor() const
    {
      return std::array<float, 4>{GetBorderRed(), GetBorderGreen(), GetBorderBlue(), GetBorderAlpha()};
    }
  };

  GPUSampler();
  virtual ~GPUSampler();

  virtual void SetDebugName(const std::string_view& name) = 0;

  static Config GetNearestConfig();
  static Config GetLinearConfig();
};

enum class GPUShaderStage : u8
{
  Vertex,
  Fragment,
  Compute,

  MaxCount
};

class GPUShader
{
public:
  GPUShader(GPUShaderStage stage);
  virtual ~GPUShader();

  static const char* GetStageName(GPUShaderStage stage);

  ALWAYS_INLINE GPUShaderStage GetStage() const { return m_stage; }

  virtual void SetDebugName(const std::string_view& name) = 0;

protected:
  GPUShaderStage m_stage;
};

class GPUPipeline
{
public:
  enum class Layout : u8
  {
    // 1 streamed UBO, 1 texture in PS.
    SingleTextureAndUBO,

    // 128 byte UBO via push constants, 1 texture.
    SingleTextureAndPushConstants,

    // 128 byte UBO via push constants, 1 texture buffer/SSBO.
    SingleTextureBufferAndPushConstants,

    // Multiple textures, 1 streamed UBO.
    MultiTextureAndUBO,

    // Multiple textures, 128 byte UBO via push constants.
    MultiTextureAndPushConstants,

    MaxCount
  };

  enum class Primitive : u8
  {
    Points,
    Lines,
    Triangles,
    TriangleStrips,

    MaxCount
  };

  union VertexAttribute
  {
    static constexpr u32 MaxAttributes = 16;

    enum class Semantic : u8
    {
      Position,
      TexCoord,
      Color,

      MaxCount
    };

    enum class Type : u8
    {
      Float,
      UInt8,
      SInt8,
      UNorm8,
      UInt16,
      SInt16,
      UNorm16,
      UInt32,
      SInt32,

      MaxCount
    };

    BitField<u32, u8, 0, 4> index;
    BitField<u32, Semantic, 4, 2> semantic;
    BitField<u32, u8, 6, 2> semantic_index;
    BitField<u32, Type, 8, 4> type;
    BitField<u32, u8, 12, 3> components;
    BitField<u32, u16, 16, 16> offset;

    u32 key;

    // clang-format off
    ALWAYS_INLINE VertexAttribute& operator=(const VertexAttribute& rhs) { key = rhs.key; return *this; }
    ALWAYS_INLINE bool operator==(const VertexAttribute& rhs) const { return key == rhs.key; }
    ALWAYS_INLINE bool operator!=(const VertexAttribute& rhs) const { return key != rhs.key; }
    ALWAYS_INLINE bool operator<(const VertexAttribute& rhs) const { return key < rhs.key; }
    // clang-format on

    static constexpr VertexAttribute Make(u8 index, Semantic semantic, u8 semantic_index, Type type, u8 components,
                                          u16 offset)
    {
      // Nasty :/ can't access an inactive element of a union here..
      return VertexAttribute{{(static_cast<u32>(index) & 0xf) | ((static_cast<u32>(semantic) & 0x3) << 4) |
                              ((static_cast<u32>(semantic_index) & 0x3) << 6) | ((static_cast<u32>(type) & 0xf) << 8) |
                              ((static_cast<u32>(components) & 0x7) << 12) |
                              ((static_cast<u32>(offset) & 0xffff) << 16)}};
    }
  };

  struct InputLayout
  {
    gsl::span<const VertexAttribute> vertex_attributes;
    u32 vertex_stride;

    bool operator==(const InputLayout& rhs) const;
    bool operator!=(const InputLayout& rhs) const;
  };

  struct InputLayoutHash
  {
    size_t operator()(const InputLayout& il) const;
  };

  enum class CullMode : u8
  {
    None,
    Front,
    Back,

    MaxCount
  };

  enum class DepthFunc : u8
  {
    Never,
    Always,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    Equal,

    MaxCount
  };

  enum class BlendFunc : u8
  {
    Zero,
    One,
    SrcColor,
    InvSrcColor,
    DstColor,
    InvDstColor,
    SrcAlpha,
    InvSrcAlpha,
    SrcAlpha1,
    InvSrcAlpha1,
    DstAlpha,
    InvDstAlpha,
    ConstantColor,
    InvConstantColor,

    MaxCount
  };

  enum class BlendOp : u8
  {
    Add,
    Subtract,
    ReverseSubtract,
    Min,
    Max,

    MaxCount
  };

  // TODO: purge this?
  union RasterizationState
  {
    BitField<u8, CullMode, 0, 2> cull_mode;
    u8 key;

    // clang-format off
    ALWAYS_INLINE RasterizationState& operator=(const RasterizationState& rhs) { key = rhs.key; return *this; }
    ALWAYS_INLINE bool operator==(const RasterizationState& rhs) const { return key == rhs.key; }
    ALWAYS_INLINE bool operator!=(const RasterizationState& rhs) const { return key != rhs.key; }
    ALWAYS_INLINE bool operator<(const RasterizationState& rhs) const { return key < rhs.key; }
    // clang-format on

    static RasterizationState GetNoCullState();
  };

  union DepthState
  {
    BitField<u8, DepthFunc, 0, 3> depth_test;
    BitField<u8, bool, 4, 1> depth_write;
    u8 key;

    // clang-format off
    ALWAYS_INLINE DepthState& operator=(const DepthState& rhs) { key = rhs.key; return *this; }
    ALWAYS_INLINE bool operator==(const DepthState& rhs) const { return key == rhs.key; }
    ALWAYS_INLINE bool operator!=(const DepthState& rhs) const { return key != rhs.key; }
    ALWAYS_INLINE bool operator<(const DepthState& rhs) const { return key < rhs.key; }
    // clang-format on

    static DepthState GetNoTestsState();
    static DepthState GetAlwaysWriteState();
  };

  union BlendState
  {
    BitField<u64, bool, 0, 1> enable;
    BitField<u64, BlendFunc, 1, 4> src_blend;
    BitField<u64, BlendFunc, 5, 4> src_alpha_blend;
    BitField<u64, BlendFunc, 9, 4> dst_blend;
    BitField<u64, BlendFunc, 13, 4> dst_alpha_blend;
    BitField<u64, BlendOp, 17, 3> blend_op;
    BitField<u64, BlendOp, 20, 3> alpha_blend_op;
    BitField<u64, bool, 24, 1> write_r;
    BitField<u64, bool, 25, 1> write_g;
    BitField<u64, bool, 26, 1> write_b;
    BitField<u64, bool, 27, 1> write_a;
    BitField<u64, u8, 24, 4> write_mask;
    BitField<u64, u32, 32, 32> constant;
    u64 key;

    // clang-format off
    ALWAYS_INLINE BlendState& operator=(const BlendState& rhs) { key = rhs.key; return *this; }
    ALWAYS_INLINE bool operator==(const BlendState& rhs) const { return key == rhs.key; }
    ALWAYS_INLINE bool operator!=(const BlendState& rhs) const { return key != rhs.key; }
    ALWAYS_INLINE bool operator<(const BlendState& rhs) const { return key < rhs.key; }
    // clang-format on

    // clang-format off
    ALWAYS_INLINE float GetConstantRed() const { return static_cast<float>(constant.GetValue() & 0xFF) / 255.0f; }
    ALWAYS_INLINE float GetConstantGreen() const { return static_cast<float>((constant.GetValue() >> 8) & 0xFF) / 255.0f; }
    ALWAYS_INLINE float GetConstantBlue() const { return static_cast<float>((constant.GetValue() >> 16) & 0xFF) / 255.0f; }
    ALWAYS_INLINE float GetConstantAlpha() const { return static_cast<float>((constant.GetValue() >> 24) & 0xFF) / 255.0f; }
    // clang-format on
    ALWAYS_INLINE std::array<float, 4> GetConstantFloatColor() const
    {
      return std::array<float, 4>{GetConstantRed(), GetConstantGreen(), GetConstantBlue(), GetConstantAlpha()};
    }

    static BlendState GetNoBlendingState();
    static BlendState GetAlphaBlendingState();
  };

  struct GraphicsConfig
  {
    Layout layout;

    Primitive primitive;
    InputLayout input_layout;

    RasterizationState rasterization;
    DepthState depth;
    BlendState blend;

    const GPUShader* vertex_shader;
    const GPUShader* fragment_shader;

    GPUTexture::Format color_format;
    GPUTexture::Format depth_format;
    u32 samples;
    bool per_sample_shading;
  };

  GPUPipeline();
  virtual ~GPUPipeline();

  virtual void SetDebugName(const std::string_view& name) = 0;
};

class GPUTextureBuffer
{
public:
  enum class Format
  {
    R16UI,

    MaxCount
  };

  GPUTextureBuffer(Format format, u32 size_in_elements);
  virtual ~GPUTextureBuffer();

  static u32 GetElementSize(Format format);

  ALWAYS_INLINE Format GetFormat() const { return m_format; }
  ALWAYS_INLINE u32 GetSizeInElements() const { return m_size_in_elements; }
  ALWAYS_INLINE u32 GetSizeInBytes() const { return m_size_in_elements * GetElementSize(m_format); }
  ALWAYS_INLINE u32 GetCurrentPosition() const { return m_current_position; }

  virtual void* Map(u32 required_elements) = 0;
  virtual void Unmap(u32 used_elements) = 0;

  virtual void SetDebugName(const std::string_view& name) = 0;

protected:
  Format m_format;
  u32 m_size_in_elements;
  u32 m_current_position;
};

// TODO: remove
class PostProcessingChain;

class GPUDevice
{
public:
  // TODO: drop virtuals
  // TODO: gpu crash handling on present
  using DrawIndex = u16;

  struct Features
  {
    bool dual_source_blend : 1;
    bool per_sample_shading : 1;
    bool noperspective_interpolation : 1;
    bool supports_texture_buffers : 1;
    bool texture_buffers_emulated_with_ssbo : 1;
    bool partial_msaa_resolve : 1;
    bool gpu_timing : 1;
    bool shader_cache : 1;
    bool pipeline_cache : 1;
  };

  struct AdapterAndModeList
  {
    std::vector<std::string> adapter_names;
    std::vector<std::string> fullscreen_modes;
  };

  static constexpr u32 MAX_TEXTURE_SAMPLERS = 8;

  virtual ~GPUDevice();

  /// Returns the default/preferred API for the system.
  static RenderAPI GetPreferredAPI();

  /// Returns a string representing the specified API.
  static const char* RenderAPIToString(RenderAPI api);

  /// Returns a new device for the specified API.
  static std::unique_ptr<GPUDevice> CreateDeviceForAPI(RenderAPI api);

  /// Parses a fullscreen mode into its components (width * height @ refresh hz)
  static bool GetRequestedExclusiveFullscreenMode(u32* width, u32* height, float* refresh_rate);

  /// Converts a fullscreen mode to a string.
  static std::string GetFullscreenModeString(u32 width, u32 height, float refresh_rate);

  /// Returns the directory bad shaders are saved to.
  static std::string GetShaderDumpPath(const std::string_view& name);

  /// Converts a RGBA8 value to 4 floating-point values.
  static std::array<float, 4> RGBA8ToFloat(u32 rgba);

  /// Returns the number of texture bindings for a given pipeline layout.
  static constexpr u32 GetActiveTexturesForLayout(GPUPipeline::Layout layout)
  {
    constexpr std::array<u8, static_cast<u8>(GPUPipeline::Layout::MaxCount)> counts = {
      1,                    // SingleTextureAndUBO
      1,                    // SingleTextureAndPushConstants
      0,                    // SingleTextureBufferAndPushConstants
      MAX_TEXTURE_SAMPLERS, // MultiTextureAndUBO
      MAX_TEXTURE_SAMPLERS, // MultiTextureAndPushConstants
    };

    return counts[static_cast<u8>(layout)];
  }
  
#ifdef __APPLE__
  // We have to define these in the base class, because they're in Objective C++.
  static std::unique_ptr<GPUDevice> WrapNewMetalDevice();
  static AdapterAndModeList WrapGetMetalAdapterAndModeList();
#endif

  ALWAYS_INLINE const Features& GetFeatures() const { return m_features; }
  ALWAYS_INLINE u32 GetMaxTextureSize() const { return m_max_texture_size; }
  ALWAYS_INLINE u32 GetMaxMultisamples() const { return m_max_multisamples; }

  ALWAYS_INLINE const WindowInfo& GetWindowInfo() const { return m_window_info; }
  ALWAYS_INLINE s32 GetWindowWidth() const { return static_cast<s32>(m_window_info.surface_width); }
  ALWAYS_INLINE s32 GetWindowHeight() const { return static_cast<s32>(m_window_info.surface_height); }
  ALWAYS_INLINE float GetWindowScale() const { return m_window_info.surface_scale; }
  ALWAYS_INLINE GPUTexture::Format GetWindowFormat() const { return m_window_info.surface_format; }

  ALWAYS_INLINE GPUSampler* GetLinearSampler() const { return m_linear_sampler.get(); }
  ALWAYS_INLINE GPUSampler* GetNearestSampler() const { return m_nearest_sampler.get(); }

  // Position is relative to the top-left corner of the window.
  ALWAYS_INLINE s32 GetMousePositionX() const { return m_mouse_position_x; }
  ALWAYS_INLINE s32 GetMousePositionY() const { return m_mouse_position_y; }
  ALWAYS_INLINE void SetMousePosition(s32 x, s32 y)
  {
    m_mouse_position_x = x;
    m_mouse_position_y = y;
  }

  ALWAYS_INLINE const void* GetDisplayTextureHandle() const { return m_display_texture; }
  ALWAYS_INLINE s32 GetDisplayWidth() const { return m_display_width; }
  ALWAYS_INLINE s32 GetDisplayHeight() const { return m_display_height; }
  ALWAYS_INLINE float GetDisplayAspectRatio() const { return m_display_aspect_ratio; }
  ALWAYS_INLINE bool IsGPUTimingEnabled() const { return m_gpu_timing_enabled; }

  virtual RenderAPI GetRenderAPI() const = 0;

  bool Create(const std::string_view& adapter, const std::string_view& shader_cache_path, u32 shader_cache_version,
              bool debug_device, bool vsync, bool threaded_presentation);
  void Destroy();

  virtual bool HasSurface() const = 0;
  virtual void DestroySurface() = 0;
  virtual bool UpdateWindow() = 0;

  virtual bool SupportsExclusiveFullscreen() const;
  virtual AdapterAndModeList GetAdapterAndModeList() = 0;

  bool SetPostProcessingChain(const std::string_view& config);

  /// Call when the window size changes externally to recreate any resources.
  virtual void ResizeWindow(s32 new_window_width, s32 new_window_height, float new_window_scale) = 0;

  virtual std::string GetDriverInfo() const = 0;

  /// Creates an abstracted RGBA8 texture. If dynamic, the texture can be updated with UpdateTexture() below.
  virtual std::unique_ptr<GPUTexture> CreateTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                                    GPUTexture::Type type, GPUTexture::Format format,
                                                    const void* data = nullptr, u32 data_stride = 0,
                                                    bool dynamic = false) = 0;
  virtual std::unique_ptr<GPUSampler> CreateSampler(const GPUSampler::Config& config) = 0;
  virtual std::unique_ptr<GPUTextureBuffer> CreateTextureBuffer(GPUTextureBuffer::Format format,
                                                                u32 size_in_elements) = 0;

  virtual bool DownloadTexture(GPUTexture* texture, u32 x, u32 y, u32 width, u32 height, void* out_data,
                               u32 out_data_stride) = 0;
  virtual void CopyTextureRegion(GPUTexture* dst, u32 dst_x, u32 dst_y, u32 dst_layer, u32 dst_level, GPUTexture* src,
                                 u32 src_x, u32 src_y, u32 src_layer, u32 src_level, u32 width, u32 height) = 0;
  virtual void ResolveTextureRegion(GPUTexture* dst, u32 dst_x, u32 dst_y, u32 dst_layer, u32 dst_level,
                                    GPUTexture* src, u32 src_x, u32 src_y, u32 width, u32 height) = 0;
  virtual void ClearRenderTarget(GPUTexture* t, u32 c);
  virtual void ClearDepth(GPUTexture* t, float d);
  virtual void InvalidateRenderTarget(GPUTexture* t);

  /// Framebuffer abstraction.
  virtual std::unique_ptr<GPUFramebuffer> CreateFramebuffer(GPUTexture* rt_or_ds, GPUTexture* ds = nullptr) = 0;

  /// Shader abstraction.
  std::unique_ptr<GPUShader> CreateShader(GPUShaderStage stage, const std::string_view& source,
                                          const char* entry_point = "main");
  virtual std::unique_ptr<GPUPipeline> CreatePipeline(const GPUPipeline::GraphicsConfig& config) = 0;

  /// Debug messaging.
  virtual void PushDebugGroup(const char* fmt, ...) = 0;
  virtual void PopDebugGroup() = 0;
  virtual void InsertDebugMessage(const char* fmt, ...) = 0;

  /// Vertex/index buffer abstraction.
  virtual void MapVertexBuffer(u32 vertex_size, u32 vertex_count, void** map_ptr, u32* map_space,
                               u32* map_base_vertex) = 0;
  virtual void UnmapVertexBuffer(u32 vertex_size, u32 vertex_count) = 0;
  virtual void MapIndexBuffer(u32 index_count, DrawIndex** map_ptr, u32* map_space, u32* map_base_index) = 0;
  virtual void UnmapIndexBuffer(u32 used_size) = 0;

  void UploadVertexBuffer(const void* vertices, u32 vertex_size, u32 vertex_count, u32* base_vertex);
  void UploadIndexBuffer(const DrawIndex* indices, u32 index_count, u32* base_index);

  /// Uniform buffer abstraction.
  virtual void PushUniformBuffer(const void* data, u32 data_size) = 0;
  virtual void* MapUniformBuffer(u32 size) = 0;
  virtual void UnmapUniformBuffer(u32 size) = 0;
  void UploadUniformBuffer(const void* data, u32 data_size);

  /// Drawing setup abstraction.
  virtual void SetFramebuffer(GPUFramebuffer* fb) = 0;
  virtual void SetPipeline(GPUPipeline* pipeline) = 0;
  virtual void SetTextureSampler(u32 slot, GPUTexture* texture, GPUSampler* sampler) = 0;
  virtual void SetTextureBuffer(u32 slot, GPUTextureBuffer* buffer) = 0;
  virtual void SetViewport(s32 x, s32 y, s32 width, s32 height) = 0; // TODO: Rectangle
  virtual void SetScissor(s32 x, s32 y, s32 width, s32 height) = 0;
  void SetViewportAndScissor(s32 x, s32 y, s32 width, s32 height);

  // Drawing abstraction.
  virtual void Draw(u32 vertex_count, u32 base_vertex) = 0;
  virtual void DrawIndexed(u32 index_count, u32 base_index, u32 base_vertex) = 0;

  /// Returns false if the window was completely occluded.
  virtual bool BeginPresent(bool skip_present) = 0;
  virtual void EndPresent() = 0;
  bool Render(bool skip_present);

  /// Renders the display with postprocessing to the specified image.
  bool RenderScreenshot(u32 width, u32 height, const Common::Rectangle<s32>& draw_rect, std::vector<u32>* out_pixels,
                        u32* out_stride, GPUTexture::Format* out_format);

  ALWAYS_INLINE bool IsVsyncEnabled() const { return m_vsync_enabled; }
  virtual void SetVSync(bool enabled) = 0;

  ALWAYS_INLINE bool IsDebugDevice() const { return m_debug_device; }

  bool UpdateImGuiFontTexture();
  bool UsesLowerLeftOrigin() const;
  void SetDisplayMaxFPS(float max_fps);
  bool ShouldSkipDisplayingFrame();
  void ThrottlePresentation();

  void ClearDisplayTexture();
  void SetDisplayTexture(GPUTexture* texture, s32 view_x, s32 view_y, s32 view_width, s32 view_height);
  void SetDisplayTextureRect(s32 view_x, s32 view_y, s32 view_width, s32 view_height);
  void SetDisplayParameters(s32 display_width, s32 display_height, s32 active_left, s32 active_top, s32 active_width,
                            s32 active_height, float display_aspect_ratio);

  virtual bool SupportsTextureFormat(GPUTexture::Format format) const = 0;

  virtual bool GetHostRefreshRate(float* refresh_rate);

  /// Enables/disables GPU frame timing.
  virtual bool SetGPUTimingEnabled(bool enabled);

  /// Returns the amount of GPU time utilized since the last time this method was called.
  virtual float GetAndResetAccumulatedGPUTime();

  /// Sets the software cursor to the specified texture. Ownership of the texture is transferred.
  void SetSoftwareCursor(std::unique_ptr<GPUTexture> texture, float scale = 1.0f);

  /// Sets the software cursor to the specified image.
  bool SetSoftwareCursor(const void* pixels, u32 width, u32 height, u32 stride, float scale = 1.0f);

  /// Sets the software cursor to the specified path (png image).
  bool SetSoftwareCursor(const char* path, float scale = 1.0f);

  /// Disables the software cursor.
  void ClearSoftwareCursor();

  /// Helper function for computing the draw rectangle in a larger window.
  std::tuple<s32, s32, s32, s32> CalculateDrawRect(s32 window_width, s32 window_height,
                                                   bool apply_aspect_ratio = true) const;

  /// Helper function for converting window coordinates to display coordinates.
  std::tuple<float, float> ConvertWindowCoordinatesToDisplayCoordinates(s32 window_x, s32 window_y, s32 window_width,
                                                                        s32 window_height) const;

  /// Helper function to save texture data to a PNG. If flip_y is set, the image will be flipped aka OpenGL.
  bool WriteTextureToFile(GPUTexture* texture, u32 x, u32 y, u32 width, u32 height, std::string filename,
                          bool clear_alpha = true, bool flip_y = false, u32 resize_width = 0, u32 resize_height = 0,
                          bool compress_on_thread = false);

  /// Helper function to save current display texture to PNG.
  bool WriteDisplayTextureToFile(std::string filename, bool full_resolution = true, bool apply_aspect_ratio = true,
                                 bool compress_on_thread = false);

  /// Helper function to save current display texture to a buffer.
  bool WriteDisplayTextureToBuffer(std::vector<u32>* buffer, u32 resize_width = 0, u32 resize_height = 0,
                                   bool clear_alpha = true);

  /// Helper function to save screenshot to PNG.
  bool WriteScreenshotToFile(std::string filename, bool internal_resolution = false, bool compress_on_thread = false);

protected:
  virtual bool CreateDevice(const std::string_view& adapter, bool threaded_presentation) = 0;
  virtual void DestroyDevice() = 0;

  std::string GetShaderCacheBaseName(const std::string_view& type) const;
  virtual bool ReadPipelineCache(const std::string& filename);
  virtual bool GetPipelineCacheData(DynamicHeapArray<u8>* data);

  virtual std::unique_ptr<GPUShader> CreateShaderFromBinary(GPUShaderStage stage, gsl::span<const u8> data) = 0;
  virtual std::unique_ptr<GPUShader> CreateShaderFromSource(GPUShaderStage stage, const std::string_view& source,
                                                            const char* entry_point,
                                                            DynamicHeapArray<u8>* out_binary) = 0;

  bool AcquireWindow(bool recreate_window);

  Features m_features = {};
  u32 m_max_texture_size = 0;
  u32 m_max_multisamples = 0;

  WindowInfo m_window_info;

  GPUShaderCache m_shader_cache;

  std::unique_ptr<GPUSampler> m_nearest_sampler;
  std::unique_ptr<GPUSampler> m_linear_sampler;

  bool m_gpu_timing_enabled = false;
  bool m_vsync_enabled = false;
  bool m_debug_device = false;

private:
  ALWAYS_INLINE bool HasSoftwareCursor() const { return static_cast<bool>(m_cursor_texture); }
  ALWAYS_INLINE bool HasDisplayTexture() const { return (m_display_texture != nullptr); }

  void OpenShaderCache(const std::string_view& base_path, u32 version);
  void CloseShaderCache();
  bool CreateResources();
  void DestroyResources();

  bool IsUsingLinearFiltering() const;

  void CalculateDrawRect(s32 window_width, s32 window_height, float* out_left, float* out_top, float* out_width,
                         float* out_height, float* out_left_padding, float* out_top_padding, float* out_scale,
                         float* out_x_scale, bool apply_aspect_ratio = true) const;

  std::tuple<s32, s32, s32, s32> CalculateSoftwareCursorDrawRect() const;
  std::tuple<s32, s32, s32, s32> CalculateSoftwareCursorDrawRect(s32 cursor_x, s32 cursor_y) const;

  void RenderImGui();

  void RenderSoftwareCursor();

  bool RenderDisplay(GPUFramebuffer* target, s32 left, s32 top, s32 width, s32 height, GPUTexture* texture,
                     s32 texture_view_x, s32 texture_view_y, s32 texture_view_width, s32 texture_view_height,
                     bool linear_filter);
  void RenderSoftwareCursor(s32 left, s32 top, s32 width, s32 height, GPUTexture* texture);

  u64 m_last_frame_displayed_time = 0;

  s32 m_mouse_position_x = 0;
  s32 m_mouse_position_y = 0;

  s32 m_display_width = 0;
  s32 m_display_height = 0;
  s32 m_display_active_left = 0;
  s32 m_display_active_top = 0;
  s32 m_display_active_width = 0;
  s32 m_display_active_height = 0;
  float m_display_aspect_ratio = 1.0f;
  float m_display_frame_interval = 0.0f;

  std::unique_ptr<GPUPipeline> m_display_pipeline;
  GPUTexture* m_display_texture = nullptr;
  s32 m_display_texture_view_x = 0;
  s32 m_display_texture_view_y = 0;
  s32 m_display_texture_view_width = 0;
  s32 m_display_texture_view_height = 0;

  std::unique_ptr<GPUPipeline> m_imgui_pipeline;
  std::unique_ptr<GPUTexture> m_imgui_font_texture;

  std::unique_ptr<GPUPipeline> m_cursor_pipeline;
  std::unique_ptr<GPUTexture> m_cursor_texture;
  float m_cursor_texture_scale = 1.0f;

  bool m_display_changed = false;

  std::unique_ptr<PostProcessingChain> m_post_processing_chain;
};

extern std::unique_ptr<GPUDevice> g_gpu_device;

namespace Host {
/// Called when the core is creating a render device.
/// This could also be fullscreen transition.
std::optional<WindowInfo> AcquireRenderWindow(bool recreate_window);

/// Called when the core is finished with a render window.
void ReleaseRenderWindow();

/// Returns true if the hosting application is currently fullscreen.
bool IsFullscreen();

/// Alters fullscreen state of hosting application.
void SetFullscreen(bool enabled);
} // namespace Host

// Macros for debug messages.
#ifdef _DEBUG
struct GLAutoPop
{
  GLAutoPop(int dummy) {}
  ~GLAutoPop() { g_gpu_device->PopDebugGroup(); }
};

#define GL_SCOPE(...) GLAutoPop gl_auto_pop((g_gpu_device->PushDebugGroup(__VA_ARGS__), 0))
#define GL_PUSH(...) g_gpu_device->PushDebugGroup(__VA_ARGS__)
#define GL_POP() g_gpu_device->PopDebugGroup()
#define GL_INS(...) g_gpu_device->InsertDebugMessage(__VA_ARGS__)
#define GL_OBJECT_NAME(obj, ...) (obj)->SetDebugName(StringUtil::StdStringFromFormat(__VA_ARGS__))
#else
#define GL_SCOPE(...) (void)0
#define GL_PUSH(...) (void)0
#define GL_POP() (void)0
#define GL_INS(...) (void)0
#define GL_OBJECT_NAME(...) (void)0
#endif
