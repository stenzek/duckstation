// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "gpu_shader_cache.h"
#include "gpu_texture.h"
#include "window_info.h"

#include "common/bitfield.h"
#include "common/gsvector.h"
#include "common/heap_array.h"
#include "common/small_string.h"
#include "common/types.h"

#include "fmt/base.h"

#include <cstring>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

class Error;
class Image;

// Enables debug event generation and object names for graphics debuggers.
#if defined(_DEBUG) || defined(_DEVEL)
#define ENABLE_GPU_OBJECT_NAMES
#endif

enum class RenderAPI : u8
{
  None,
  D3D11,
  D3D12,
  Vulkan,
  OpenGL,
  OpenGLES,
  Metal
};

enum class GPUVSyncMode : u8
{
  Disabled,
  FIFO,
  Mailbox,
  Count
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
    MirrorRepeat,

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

#ifdef ENABLE_GPU_OBJECT_NAMES
  virtual void SetDebugName(std::string_view name) = 0;
  template<typename... T>
  void SetDebugName(fmt::format_string<T...> fmt, T&&... args)
  {
    SetDebugName(TinyString::from_vformat(fmt, fmt::make_format_args(args...)));
  }
#endif

  static Config GetNearestConfig();
  static Config GetLinearConfig();
};

enum class GPUShaderStage : u8
{
  Vertex,
  Fragment,
  Geometry,
  Compute,

  MaxCount
};

enum class GPUShaderLanguage : u8
{
  None,
  HLSL,
  GLSL,
  GLSLES,
  GLSLVK,
  MSL,
  SPV,
  Count
};

enum class GPUDriverType : u16
{
  MobileFlag = 0x100,
  SoftwareFlag = 0x200,

  Unknown = 0,
  AMDProprietary = 1,
  AMDMesa = 2,
  IntelProprietary = 3,
  IntelMesa = 4,
  NVIDIAProprietary = 5,
  NVIDIAMesa = 6,
  AppleProprietary = 7,
  AppleMesa = 8,
  DozenMesa = 9,

  ImaginationProprietary = MobileFlag | 1,
  ImaginationMesa = MobileFlag | 2,
  ARMProprietary = MobileFlag | 3,
  ARMMesa = MobileFlag | 4,
  QualcommProprietary = MobileFlag | 5,
  QualcommMesa = MobileFlag | 6,
  BroadcomProprietary = MobileFlag | 7,
  BroadcomMesa = MobileFlag | 8,

  LLVMPipe = SoftwareFlag | 1,
  SwiftShader = SoftwareFlag | 2,
  WARP = SoftwareFlag | 3,
};
IMPLEMENT_ENUM_CLASS_BITWISE_OPERATORS(GPUDriverType);

class GPUShader
{
public:
  explicit GPUShader(GPUShaderStage stage);
  virtual ~GPUShader();

  static const char* GetStageName(GPUShaderStage stage);

  ALWAYS_INLINE GPUShaderStage GetStage() const { return m_stage; }

#ifdef ENABLE_GPU_OBJECT_NAMES
  virtual void SetDebugName(std::string_view name) = 0;
  template<typename... T>
  void SetDebugName(fmt::format_string<T...> fmt, T&&... args)
  {
    SetDebugName(TinyString::from_vformat(fmt, fmt::make_format_args(args...)));
  }
#endif

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

    // Multiple textures, 1 streamed UBO, compute shader.
    ComputeMultiTextureAndUBO,

    // 128 byte UBO via push constants, multiple textures, compute shader.
    ComputeMultiTextureAndPushConstants,

    MaxCount
  };

  enum RenderPassFlag : u8
  {
    NoRenderPassFlags = 0,
    ColorFeedbackLoop = (1 << 0),
    SampleDepthBuffer = (1 << 1),
    BindRenderTargetsAsImages = (1 << 2),
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
    ALWAYS_INLINE VertexAttribute() = default;
    ALWAYS_INLINE constexpr VertexAttribute(const VertexAttribute& rhs) = default;
    ALWAYS_INLINE VertexAttribute& operator=(const VertexAttribute& rhs) = default;
    ALWAYS_INLINE bool operator==(const VertexAttribute& rhs) const { return key == rhs.key; }
    ALWAYS_INLINE bool operator!=(const VertexAttribute& rhs) const { return key != rhs.key; }
    ALWAYS_INLINE bool operator<(const VertexAttribute& rhs) const { return key < rhs.key; }
    // clang-format on

    static constexpr VertexAttribute Make(u8 index, Semantic semantic, u8 semantic_index, Type type, u8 components,
                                          u16 offset)
    {
      // Nasty :/ can't access an inactive element of a union here..
      return VertexAttribute((static_cast<u32>(index) & 0xf) | ((static_cast<u32>(semantic) & 0x3) << 4) |
                             ((static_cast<u32>(semantic_index) & 0x3) << 6) | ((static_cast<u32>(type) & 0xf) << 8) |
                             ((static_cast<u32>(components) & 0x7) << 12) |
                             ((static_cast<u32>(offset) & 0xffff) << 16));
    }

  private:
    ALWAYS_INLINE constexpr VertexAttribute(u32 key_) : key(key_) {}
  };

  struct InputLayout
  {
    std::span<const VertexAttribute> vertex_attributes;
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
    u8 key;

    BitField<u8, CullMode, 0, 2> cull_mode;

    // clang-format off
    ALWAYS_INLINE bool operator==(const RasterizationState& rhs) const { return key == rhs.key; }
    ALWAYS_INLINE bool operator!=(const RasterizationState& rhs) const { return key != rhs.key; }
    ALWAYS_INLINE bool operator<(const RasterizationState& rhs) const { return key < rhs.key; }
    // clang-format on

    static RasterizationState GetNoCullState();
  };

  union DepthState
  {
    u8 key;

    BitField<u8, DepthFunc, 0, 3> depth_test;
    BitField<u8, bool, 4, 1> depth_write;

    // clang-format off
    ALWAYS_INLINE bool operator==(const DepthState& rhs) const { return key == rhs.key; }
    ALWAYS_INLINE bool operator!=(const DepthState& rhs) const { return key != rhs.key; }
    ALWAYS_INLINE bool operator<(const DepthState& rhs) const { return key < rhs.key; }
    // clang-format on

    static DepthState GetNoTestsState();
    static DepthState GetAlwaysWriteState();
  };

  union BlendState
  {
    u64 key;

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

    BitField<u64, u16, 1, 16> blend_factors;
    BitField<u64, u8, 17, 6> blend_ops;

    // clang-format off
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

    GPUShader* vertex_shader;
    GPUShader* geometry_shader;
    GPUShader* fragment_shader;

    GPUTexture::Format color_formats[4];
    GPUTexture::Format depth_format;
    u8 samples;
    bool per_sample_shading;
    RenderPassFlag render_pass_flags;

    void SetTargetFormats(GPUTexture::Format color_format,
                          GPUTexture::Format depth_format_ = GPUTexture::Format::Unknown);
    u32 GetRenderTargetCount() const;
  };

  struct ComputeConfig
  {
    Layout layout;
    GPUShader* compute_shader;
  };

  GPUPipeline();
  virtual ~GPUPipeline();

#ifdef ENABLE_GPU_OBJECT_NAMES
  virtual void SetDebugName(std::string_view name) = 0;
  template<typename... T>
  void SetDebugName(fmt::format_string<T...> fmt, T&&... args)
  {
    SetDebugName(TinyString::from_vformat(fmt, fmt::make_format_args(args...)));
  }
#endif
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

#ifdef ENABLE_GPU_OBJECT_NAMES
  virtual void SetDebugName(std::string_view name) = 0;
  template<typename... T>
  void SetDebugName(fmt::format_string<T...> fmt, T&&... args)
  {
    SetDebugName(TinyString::from_vformat(fmt, fmt::make_format_args(args...)));
  }
#endif

protected:
  Format m_format;
  u32 m_size_in_elements;
  u32 m_current_position = 0;
};

class GPUSwapChain
{
public:
  GPUSwapChain(const WindowInfo& wi, GPUVSyncMode vsync_mode, bool allow_present_throttle);
  virtual ~GPUSwapChain();

  ALWAYS_INLINE const WindowInfo& GetWindowInfo() const { return m_window_info; }
  ALWAYS_INLINE u32 GetWidth() const { return m_window_info.surface_width; }
  ALWAYS_INLINE u32 GetHeight() const { return m_window_info.surface_height; }
  ALWAYS_INLINE u32 GetPostRotatedWidth() const { return m_window_info.GetPostRotatedWidth(); }
  ALWAYS_INLINE u32 GetPostRotatedHeight() const { return m_window_info.GetPostRotatedHeight(); }
  ALWAYS_INLINE float GetScale() const { return m_window_info.surface_scale; }
  ALWAYS_INLINE WindowInfo::PreRotation GetPreRotation() const { return m_window_info.surface_prerotation; }
  ALWAYS_INLINE GPUTexture::Format GetFormat() const { return m_window_info.surface_format; }
  ALWAYS_INLINE GSVector2i GetSizeVec() const
  {
    return GSVector2i(m_window_info.surface_width, m_window_info.surface_height);
  }
  ALWAYS_INLINE GSVector2i GetPostRotatedSizeVec() const
  {
    return GSVector2i(m_window_info.GetPostRotatedWidth(), m_window_info.GetPostRotatedHeight());
  }

  ALWAYS_INLINE GPUVSyncMode GetVSyncMode() const { return m_vsync_mode; }
  ALWAYS_INLINE bool IsVSyncModeBlocking() const { return (m_vsync_mode == GPUVSyncMode::FIFO); }
  ALWAYS_INLINE bool IsPresentThrottleAllowed() const { return m_allow_present_throttle; }

  virtual bool ResizeBuffers(u32 new_width, u32 new_height, float new_scale, Error* error) = 0;
  virtual bool SetVSyncMode(GPUVSyncMode mode, bool allow_present_throttle, Error* error) = 0;

  /// Returns true if exclusive fullscreen is currently active on this swap chain.
  virtual bool IsExclusiveFullscreen() const;

  bool ShouldSkipPresentingFrame();
  void ThrottlePresentation();

  static GSVector4i PreRotateClipRect(WindowInfo::PreRotation prerotation, const GSVector2i surface_size,
                                      const GSVector4i& v);

protected:
  // TODO: Merge WindowInfo into this struct...
  WindowInfo m_window_info;

  GPUVSyncMode m_vsync_mode = GPUVSyncMode::Disabled;
  bool m_allow_present_throttle = false;

  u64 m_last_frame_displayed_time = 0;
};

class GPUDevice
{
public:
  friend GPUTexture;

  using DrawIndex = u16;

  enum class CreateFlags : u32
  {
    None = 0,
    PreferGLESContext = (1 << 0),
    EnableDebugDevice = (1 << 1),
    EnableGPUValidation = (1 << 2),
    DisableShaderCache = (1 << 3),
    DisableDualSourceBlend = (1 << 4),
    DisableFeedbackLoops = (1 << 5),
    DisableFramebufferFetch = (1 << 6),
    DisableTextureBuffers = (1 << 7),
    DisableGeometryShaders = (1 << 8),
    DisableComputeShaders = (1 << 9),
    DisableTextureCopyToSelf = (1 << 10),
    DisableMemoryImport = (1 << 11),
    DisableRasterOrderViews = (1 << 12),
    DisableCompressedTextures = (1 << 13),
  };

  enum class DrawBarrier : u32
  {
    None,
    One,
    Full
  };

  enum class PresentResult : u32
  {
    OK,
    SkipPresent,
    ExclusiveFullscreenLost,
    DeviceLost,
  };

  struct Features
  {
    bool dual_source_blend : 1;
    bool framebuffer_fetch : 1;
    bool per_sample_shading : 1;
    bool noperspective_interpolation : 1;
    bool texture_copy_to_self : 1;
    bool texture_buffers : 1;
    bool texture_buffers_emulated_with_ssbo : 1;
    bool feedback_loops : 1;
    bool geometry_shaders : 1;
    bool compute_shaders : 1;
    bool partial_msaa_resolve : 1;
    bool memory_import : 1;
    bool exclusive_fullscreen : 1;
    bool explicit_present : 1;
    bool timed_present : 1;
    bool gpu_timing : 1;
    bool shader_cache : 1;
    bool pipeline_cache : 1;
    bool prefer_unused_textures : 1;
    bool raster_order_views : 1;
    bool dxt_textures : 1;
    bool bptc_textures : 1;
  };

  struct Statistics
  {
    size_t buffer_streamed;
    u32 num_draws;
    u32 num_barriers;
    u32 num_render_passes;
    u32 num_copies;
    u32 num_downloads;
    u32 num_uploads;
  };

  // Parameters for exclusive fullscreen.
  struct ExclusiveFullscreenMode
  {
    u32 width;
    u32 height;
    float refresh_rate;

    TinyString ToString() const;

    static std::optional<ExclusiveFullscreenMode> Parse(std::string_view str);
  };

  struct AdapterInfo
  {
    std::string name;
    std::vector<ExclusiveFullscreenMode> fullscreen_modes;
    u32 max_texture_size;
    u32 max_multisamples;
    GPUDriverType driver_type;
    bool supports_sample_shading;
  };
  using AdapterInfoList = std::vector<AdapterInfo>;

  struct PooledTextureDeleter
  {
    void operator()(GPUTexture* const tex);
  };
  using AutoRecycleTexture = std::unique_ptr<GPUTexture, PooledTextureDeleter>;

  static constexpr u32 MAX_TEXTURE_SAMPLERS = 8;
  static constexpr u32 MIN_TEXEL_BUFFER_ELEMENTS = 4 * 1024 * 512;
  static constexpr u32 MAX_RENDER_TARGETS = 4;
  static constexpr u32 MAX_IMAGE_RENDER_TARGETS = 2;
  static constexpr u32 DEFAULT_CLEAR_COLOR = 0xFF000000u;
  static constexpr u32 PIPELINE_CACHE_HASH_SIZE = 20;
  static_assert(sizeof(GPUPipeline::GraphicsConfig::color_formats) == sizeof(GPUTexture::Format) * MAX_RENDER_TARGETS);

  GPUDevice();
  virtual ~GPUDevice();

  /// Returns the default/preferred API for the system.
  static RenderAPI GetPreferredAPI();

  /// Returns a string representing the specified API.
  static const char* RenderAPIToString(RenderAPI api);

  /// Returns a string representing the specified language.
  static const char* ShaderLanguageToString(GPUShaderLanguage language);

  /// Returns a string representing the specified vsync mode.
  static const char* VSyncModeToString(GPUVSyncMode mode);

  /// Returns a new device for the specified API.
  static std::unique_ptr<GPUDevice> CreateDeviceForAPI(RenderAPI api);

  /// Returns true if the render API is the same (e.g. GLES and GL).
  static bool IsSameRenderAPI(RenderAPI lhs, RenderAPI rhs);

  /// Returns a list of adapters for the given API.
  static AdapterInfoList GetAdapterListForAPI(RenderAPI api);

  /// Dumps out a shader that failed compilation.
  static void DumpBadShader(std::string_view code, std::string_view errors);

  /// Converts a RGBA8 value to 4 floating-point values.
  static std::array<float, 4> RGBA8ToFloat(u32 rgba);

  /// Returns true if the given device creation flag is present.
  static constexpr bool HasCreateFlag(CreateFlags flags, CreateFlags flag)
  {
    return ((static_cast<u32>(flags) & static_cast<u32>(flag)) != 0);
  }

  /// Returns the number of texture bindings for a given pipeline layout.
  static constexpr u32 GetActiveTexturesForLayout(GPUPipeline::Layout layout)
  {
    constexpr std::array<u8, static_cast<u8>(GPUPipeline::Layout::MaxCount)> counts = {
      1,                    // SingleTextureAndUBO
      1,                    // SingleTextureAndPushConstants
      0,                    // SingleTextureBufferAndPushConstants
      MAX_TEXTURE_SAMPLERS, // MultiTextureAndUBO
      MAX_TEXTURE_SAMPLERS, // MultiTextureAndPushConstants
      MAX_TEXTURE_SAMPLERS, // ComputeMultiTextureAndUBO
      MAX_TEXTURE_SAMPLERS, // ComputeMultiTextureAndPushConstants
    };

    return counts[static_cast<u8>(layout)];
  }

  /// Returns true if the given pipeline layout is used for compute shaders.
  static constexpr bool IsComputeLayout(GPUPipeline::Layout layout)
  {
    return (layout >= GPUPipeline::Layout::ComputeMultiTextureAndUBO);
  }

  /// Returns the number of thread groups to dispatch for a given total count and local size.
  static constexpr std::tuple<u32, u32, u32> GetDispatchCount(u32 count_x, u32 count_y, u32 count_z, u32 local_size_x,
                                                              u32 local_size_y, u32 local_size_z)
  {
    return std::make_tuple((count_x + (local_size_x - 1)) / local_size_x, (count_y + (local_size_y - 1)) / local_size_y,
                           (count_z + (local_size_z - 1)) / local_size_z);
  }

  /// Determines the driver type for a given adapter.
  static GPUDriverType GuessDriverType(u32 pci_vendor_id, std::string_view vendor_name, std::string_view adapter_name);

  ALWAYS_INLINE const Features& GetFeatures() const { return m_features; }
  ALWAYS_INLINE RenderAPI GetRenderAPI() const { return m_render_api; }
  ALWAYS_INLINE u32 GetRenderAPIVersion() const { return m_render_api_version; }
  ALWAYS_INLINE u32 GetMaxTextureSize() const { return m_max_texture_size; }
  ALWAYS_INLINE u32 GetMaxMultisamples() const { return m_max_multisamples; }

  ALWAYS_INLINE GPUSwapChain* GetMainSwapChain() const { return m_main_swap_chain.get(); }
  ALWAYS_INLINE bool HasMainSwapChain() const { return static_cast<bool>(m_main_swap_chain); }

  ALWAYS_INLINE GPUTexture* GetEmptyTexture() const { return m_empty_texture.get(); }
  ALWAYS_INLINE GPUSampler* GetLinearSampler() const { return m_linear_sampler; }
  ALWAYS_INLINE GPUSampler* GetNearestSampler() const { return m_nearest_sampler; }

  ALWAYS_INLINE bool IsGPUTimingEnabled() const { return m_gpu_timing_enabled; }

  bool Create(std::string_view adapter, CreateFlags create_flags, std::string_view shader_dump_path,
              std::string_view shader_cache_path, u32 shader_cache_version, const WindowInfo& wi, GPUVSyncMode vsync,
              bool allow_present_throttle, const ExclusiveFullscreenMode* exclusive_fullscreen_mode,
              std::optional<bool> exclusive_fullscreen_control, Error* error);
  void Destroy();

  virtual std::unique_ptr<GPUSwapChain> CreateSwapChain(const WindowInfo& wi, GPUVSyncMode vsync_mode,
                                                        bool allow_present_throttle,
                                                        const ExclusiveFullscreenMode* exclusive_fullscreen_mode,
                                                        std::optional<bool> exclusive_fullscreen_control,
                                                        Error* error) = 0;
  virtual bool SwitchToSurfacelessRendering(Error* error);

  bool RecreateMainSwapChain(const WindowInfo& wi, GPUVSyncMode vsync_mode, bool allow_present_throttle,
                             const ExclusiveFullscreenMode* exclusive_fullscreen_mode,
                             std::optional<bool> exclusive_fullscreen_control, Error* error);
  void DestroyMainSwapChain();

  virtual std::string GetDriverInfo() const = 0;

  // Flushes current command buffer, but does not wait for completion.
  virtual void FlushCommands() = 0;

  // Executes current command buffer, waits for its completion, and destroys all pending resources.
  virtual void WaitForGPUIdle() = 0;

  virtual std::unique_ptr<GPUTexture> CreateTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                                    GPUTexture::Type type, GPUTexture::Format format,
                                                    GPUTexture::Flags flags, const void* data = nullptr,
                                                    u32 data_stride = 0, Error* error = nullptr) = 0;
  virtual std::unique_ptr<GPUSampler> CreateSampler(const GPUSampler::Config& config, Error* error = nullptr) = 0;
  virtual std::unique_ptr<GPUTextureBuffer> CreateTextureBuffer(GPUTextureBuffer::Format format, u32 size_in_elements,
                                                                Error* error = nullptr) = 0;

  GPUSampler* GetSampler(const GPUSampler::Config& config, Error* error = nullptr);

  // Texture pooling.
  std::unique_ptr<GPUTexture> FetchTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                           GPUTexture::Type type, GPUTexture::Format format, GPUTexture::Flags flags,
                                           const void* data = nullptr, u32 data_stride = 0, Error* error = nullptr);
  AutoRecycleTexture FetchAutoRecycleTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                             GPUTexture::Type type, GPUTexture::Format format, GPUTexture::Flags flags,
                                             const void* data = nullptr, u32 data_stride = 0, Error* error = nullptr);
  std::unique_ptr<GPUTexture> FetchAndUploadTextureImage(const Image& image,
                                                         GPUTexture::Flags flags = GPUTexture::Flags::None,
                                                         Error* error = nullptr);
  void RecycleTexture(std::unique_ptr<GPUTexture> texture);
  void PurgeTexturePool();

  virtual std::unique_ptr<GPUDownloadTexture> CreateDownloadTexture(u32 width, u32 height, GPUTexture::Format format,
                                                                    Error* error = nullptr) = 0;
  virtual std::unique_ptr<GPUDownloadTexture> CreateDownloadTexture(u32 width, u32 height, GPUTexture::Format format,
                                                                    void* memory, size_t memory_size, u32 memory_stride,
                                                                    Error* error = nullptr) = 0;

  virtual void CopyTextureRegion(GPUTexture* dst, u32 dst_x, u32 dst_y, u32 dst_layer, u32 dst_level, GPUTexture* src,
                                 u32 src_x, u32 src_y, u32 src_layer, u32 src_level, u32 width, u32 height) = 0;
  virtual void ResolveTextureRegion(GPUTexture* dst, u32 dst_x, u32 dst_y, u32 dst_layer, u32 dst_level,
                                    GPUTexture* src, u32 src_x, u32 src_y, u32 width, u32 height) = 0;
  virtual void ClearRenderTarget(GPUTexture* t, u32 c);
  virtual void ClearDepth(GPUTexture* t, float d);
  virtual void InvalidateRenderTarget(GPUTexture* t);

  /// Shader abstraction.
  std::unique_ptr<GPUShader> CreateShader(GPUShaderStage stage, GPUShaderLanguage language, std::string_view source,
                                          Error* error = nullptr, const char* entry_point = "main");
  virtual std::unique_ptr<GPUPipeline> CreatePipeline(const GPUPipeline::GraphicsConfig& config,
                                                      Error* error = nullptr) = 0;
  virtual std::unique_ptr<GPUPipeline> CreatePipeline(const GPUPipeline::ComputeConfig& config,
                                                      Error* error = nullptr) = 0;

#ifdef ENABLE_GPU_OBJECT_NAMES
  /// Debug messaging.
  virtual void PushDebugGroup(const char* name) = 0;
  virtual void PopDebugGroup() = 0;
  virtual void InsertDebugMessage(const char* msg) = 0;

  /// Formatted debug variants.
  template<typename... T>
  void PushDebugGroup(fmt::format_string<T...> fmt, T&&... args)
  {
    PushDebugGroup(TinyString::from_vformat(fmt, fmt::make_format_args(args...)));
  }
  template<typename... T>
  void InsertDebugMessage(fmt::format_string<T...> fmt, T&&... args)
  {
    InsertDebugMessage(TinyString::from_vformat(fmt, fmt::make_format_args(args...)));
  }
#endif

  /// Vertex/index buffer abstraction.
  virtual void MapVertexBuffer(u32 vertex_size, u32 vertex_count, void** map_ptr, u32* map_space,
                               u32* map_base_vertex) = 0;
  virtual void UnmapVertexBuffer(u32 vertex_size, u32 vertex_count) = 0;
  virtual void MapIndexBuffer(u32 index_count, DrawIndex** map_ptr, u32* map_space, u32* map_base_index) = 0;
  virtual void UnmapIndexBuffer(u32 used_size) = 0;

  void UploadVertexBuffer(const void* vertices, u32 vertex_size, u32 vertex_count, u32* base_vertex);
  void UploadIndexBuffer(const DrawIndex* indices, u32 index_count, u32* base_index);

  /// Uniform buffer abstraction.
  virtual void* MapUniformBuffer(u32 size) = 0;
  virtual void UnmapUniformBuffer(u32 size) = 0;
  void UploadUniformBuffer(const void* data, u32 data_size);

  /// Drawing setup abstraction.
  virtual void SetRenderTargets(GPUTexture* const* rts, u32 num_rts, GPUTexture* ds,
                                GPUPipeline::RenderPassFlag flags = GPUPipeline::NoRenderPassFlags) = 0;
  virtual void SetPipeline(GPUPipeline* pipeline) = 0;
  virtual void SetTextureSampler(u32 slot, GPUTexture* texture, GPUSampler* sampler) = 0;
  virtual void SetTextureBuffer(u32 slot, GPUTextureBuffer* buffer) = 0;
  virtual void SetViewport(const GSVector4i rc) = 0;
  virtual void SetScissor(const GSVector4i rc) = 0;
  void SetRenderTarget(GPUTexture* rt, GPUTexture* ds = nullptr,
                       GPUPipeline::RenderPassFlag flags = GPUPipeline::NoRenderPassFlags);
  void SetViewport(s32 x, s32 y, s32 width, s32 height);
  void SetScissor(s32 x, s32 y, s32 width, s32 height);
  void SetViewportAndScissor(s32 x, s32 y, s32 width, s32 height);
  void SetViewportAndScissor(const GSVector4i rc);

  // Drawing abstraction.
  virtual void Draw(u32 vertex_count, u32 base_vertex) = 0;
  virtual void DrawWithPushConstants(u32 vertex_count, u32 base_vertex, const void* push_constants,
                                     u32 push_constants_size) = 0;
  virtual void DrawIndexed(u32 index_count, u32 base_index, u32 base_vertex) = 0;
  virtual void DrawIndexedWithPushConstants(u32 index_count, u32 base_index, u32 base_vertex,
                                            const void* push_constants, u32 push_constants_size) = 0;
  virtual void DrawIndexedWithBarrier(u32 index_count, u32 base_index, u32 base_vertex, DrawBarrier type);
  virtual void DrawIndexedWithBarrierWithPushConstants(u32 index_count, u32 base_index, u32 base_vertex,
                                                       const void* push_constants, u32 push_constants_size,
                                                       DrawBarrier type);
  virtual void Dispatch(u32 threads_x, u32 threads_y, u32 threads_z, u32 group_size_x, u32 group_size_y,
                        u32 group_size_z) = 0;
  virtual void DispatchWithPushConstants(u32 threads_x, u32 threads_y, u32 threads_z, u32 group_size_x,
                                         u32 group_size_y, u32 group_size_z, const void* push_constants,
                                         u32 push_constants_size) = 0;

  /// Returns false if the window was completely occluded.
  virtual PresentResult BeginPresent(GPUSwapChain* swap_chain, u32 clear_color = DEFAULT_CLEAR_COLOR) = 0;
  virtual void EndPresent(GPUSwapChain* swap_chain, bool explicit_submit, u64 submit_time = 0) = 0;
  virtual void SubmitPresent(GPUSwapChain* swap_chain) = 0;

  ALWAYS_INLINE bool IsDebugDevice() const { return m_debug_device; }
  ALWAYS_INLINE size_t GetVRAMUsage() const { return s_total_vram_usage; }

  bool UsesLowerLeftOrigin() const;
  static GSVector4i FlipToLowerLeft(GSVector4i rc, s32 target_height);
  bool ResizeTexture(std::unique_ptr<GPUTexture>* tex, u32 new_width, u32 new_height, GPUTexture::Type type,
                     GPUTexture::Format format, GPUTexture::Flags flags, bool preserve = true, Error* error = nullptr);
  bool ResizeTexture(std::unique_ptr<GPUTexture>* tex, u32 new_width, u32 new_height, GPUTexture::Type type,
                     GPUTexture::Format format, GPUTexture::Flags flags, const void* replace_data,
                     u32 replace_data_pitch, Error* error = nullptr);

  virtual bool SupportsTextureFormat(GPUTexture::Format format) const = 0;

  /// Enables/disables GPU frame timing.
  virtual bool SetGPUTimingEnabled(bool enabled);

  /// Returns the amount of GPU time utilized since the last time this method was called.
  virtual float GetAndResetAccumulatedGPUTime();

  ALWAYS_INLINE static Statistics& GetStatistics() { return s_stats; }
  static void ResetStatistics();

protected:
  virtual bool CreateDeviceAndMainSwapChain(std::string_view adapter, CreateFlags create_flags, const WindowInfo& wi,
                                            GPUVSyncMode vsync_mode, bool allow_present_throttle,
                                            const ExclusiveFullscreenMode* exclusive_fullscreen_mode,
                                            std::optional<bool> exclusive_fullscreen_control, Error* error) = 0;
  virtual void DestroyDevice() = 0;

  std::string GetShaderCacheBaseName(std::string_view type) const;
  virtual bool OpenPipelineCache(const std::string& path, Error* error);
  virtual bool CreatePipelineCache(const std::string& path, Error* error);
  virtual bool ReadPipelineCache(DynamicHeapArray<u8> data, Error* error);
  virtual bool GetPipelineCacheData(DynamicHeapArray<u8>* data, Error* error);
  virtual bool ClosePipelineCache(const std::string& path, Error* error);

  virtual std::unique_ptr<GPUShader> CreateShaderFromBinary(GPUShaderStage stage, std::span<const u8> data,
                                                            Error* error) = 0;
  virtual std::unique_ptr<GPUShader> CreateShaderFromSource(GPUShaderStage stage, GPUShaderLanguage language,
                                                            std::string_view source, const char* entry_point,
                                                            DynamicHeapArray<u8>* out_binary, Error* error) = 0;

  void TrimTexturePool();

  bool CompileGLSLShaderToVulkanSpv(GPUShaderStage stage, GPUShaderLanguage source_language, std::string_view source,
                                    const char* entry_point, bool optimization, bool nonsemantic_debug_info,
                                    DynamicHeapArray<u8>* out_binary, Error* error);
  bool TranslateVulkanSpvToLanguage(const std::span<const u8> spirv, GPUShaderStage stage,
                                    GPUShaderLanguage target_language, u32 target_version, std::string* output,
                                    Error* error);
  std::unique_ptr<GPUShader> TranspileAndCreateShaderFromSource(GPUShaderStage stage, GPUShaderLanguage source_language,
                                                                std::string_view source, const char* entry_point,
                                                                GPUShaderLanguage target_language, u32 target_version,
                                                                DynamicHeapArray<u8>* out_binary, Error* error);
  static std::optional<DynamicHeapArray<u8>> OptimizeVulkanSpv(const std::span<const u8> spirv, Error* error);

  void SetDriverType(GPUDriverType type);

  Features m_features = {};
  RenderAPI m_render_api = RenderAPI::None;
  u32 m_render_api_version = 0;
  u32 m_max_texture_size = 0;
  GPUDriverType m_driver_type = GPUDriverType::Unknown;
  u16 m_max_multisamples = 0;

  std::unique_ptr<GPUSwapChain> m_main_swap_chain;
  std::unique_ptr<GPUTexture> m_empty_texture;
  GPUSampler* m_nearest_sampler = nullptr;
  GPUSampler* m_linear_sampler = nullptr;

  GPUShaderCache m_shader_cache;

private:
  static constexpr u32 MAX_TEXTURE_POOL_SIZE = 125;
  static constexpr u32 MAX_TARGET_POOL_SIZE = 50;
  static constexpr u32 POOL_PURGE_DELAY = 300;

  struct TexturePoolKey
  {
    u16 width;
    u16 height;
    u8 layers;
    u8 levels;
    u8 samples;
    GPUTexture::Type type;
    GPUTexture::Format format;
    GPUTexture::Flags flags;

    ALWAYS_INLINE bool operator==(const TexturePoolKey& rhs) const
    {
      return std::memcmp(this, &rhs, sizeof(TexturePoolKey)) == 0;
    }
    ALWAYS_INLINE bool operator!=(const TexturePoolKey& rhs) const
    {
      return std::memcmp(this, &rhs, sizeof(TexturePoolKey)) != 0;
    }
  };
  struct TexturePoolEntry
  {
    std::unique_ptr<GPUTexture> texture;
    u32 use_counter;
    TexturePoolKey key;
  };

  using TexturePool = std::deque<TexturePoolEntry>;
  using SamplerMap = std::unordered_map<u64, std::unique_ptr<GPUSampler>>;

#ifdef __APPLE__
  // We have to define these in the base class, because they're in Objective C++.
  static std::unique_ptr<GPUDevice> WrapNewMetalDevice();
  static AdapterInfoList WrapGetMetalAdapterList();
#endif

  void OpenShaderCache(std::string_view base_path, u32 version);
  void CloseShaderCache();
  bool CreateResources(Error* error);
  void DestroyResources();
  static bool IsTexturePoolType(GPUTexture::Type type);

  static size_t s_total_vram_usage;

  SamplerMap m_sampler_map;

  TexturePool m_texture_pool;
  TexturePool m_target_pool;
  size_t m_pool_vram_usage = 0;
  u32 m_texture_pool_counter = 0;

protected:
  static Statistics s_stats;

  bool m_gpu_timing_enabled = false;
  bool m_debug_device = false;
};

extern std::unique_ptr<GPUDevice> g_gpu_device;

IMPLEMENT_ENUM_CLASS_BITWISE_OPERATORS(GPUDevice::CreateFlags);

ALWAYS_INLINE void GPUDevice::PooledTextureDeleter::operator()(GPUTexture* const tex)
{
  g_gpu_device->RecycleTexture(std::unique_ptr<GPUTexture>(tex));
}

// C preprocessor workarounds.
#define GL_TOKEN_PASTE(x, y) x##y
#define GL_TOKEN_PASTE2(x, y) GL_TOKEN_PASTE(x, y)

// Macros for debug messages.
#ifdef ENABLE_GPU_OBJECT_NAMES
struct GLAutoPop
{
  GLAutoPop(const char* name)
  {
    if (g_gpu_device->IsDebugDevice()) [[unlikely]]
      g_gpu_device->PushDebugGroup(name);
  }

  template<typename... T>
  GLAutoPop(fmt::format_string<T...> fmt, T&&... args)
  {
    if (g_gpu_device->IsDebugDevice()) [[unlikely]]
      g_gpu_device->PushDebugGroup(SmallString::from_vformat(fmt, fmt::make_format_args(args...)));
  }

  ~GLAutoPop()
  {
    if (g_gpu_device->IsDebugDevice()) [[unlikely]]
      g_gpu_device->PopDebugGroup();
  }
};

#define GL_SCOPE(name) GLAutoPop GL_TOKEN_PASTE2(gl_auto_pop_, __LINE__)(name)
#define GL_INS(msg)                                                                                                    \
  do                                                                                                                   \
  {                                                                                                                    \
    if (g_gpu_device->IsDebugDevice()) [[unlikely]]                                                                    \
      g_gpu_device->InsertDebugMessage(msg);                                                                           \
  } while (0)
#define GL_OBJECT_NAME(obj, name)                                                                                      \
  do                                                                                                                   \
  {                                                                                                                    \
    if (g_gpu_device->IsDebugDevice()) [[unlikely]]                                                                    \
      (obj)->SetDebugName(name);                                                                                       \
  } while (0)

#define GL_SCOPE_FMT(...) GLAutoPop GL_TOKEN_PASTE2(gl_auto_pop_, __LINE__)(__VA_ARGS__)
#define GL_INS_FMT(...)                                                                                                \
  do                                                                                                                   \
  {                                                                                                                    \
    if (g_gpu_device->IsDebugDevice()) [[unlikely]]                                                                    \
      g_gpu_device->InsertDebugMessage(__VA_ARGS__);                                                                   \
  } while (0)
#define GL_OBJECT_NAME_FMT(obj, ...)                                                                                   \
  do                                                                                                                   \
  {                                                                                                                    \
    if (g_gpu_device->IsDebugDevice()) [[unlikely]]                                                                    \
      (obj)->SetDebugName(__VA_ARGS__);                                                                                \
  } while (0)
#else
#define GL_SCOPE(name) (void)0
#define GL_INS(msg) (void)0
#define GL_OBJECT_NAME(obj, name) (void)0

#define GL_SCOPE_FMT(...) (void)0
#define GL_INS_FMT(...) (void)0
#define GL_OBJECT_NAME_FMT(obj, ...) (void)0
#endif
