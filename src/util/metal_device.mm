// SPDX-FileCopyrightText: 2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "metal_device.h"
#include "spirv_compiler.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"

// TODO FIXME...
#define FMT_EXCEPTIONS 0
#include "fmt/format.h"

#include <array>
#include <pthread.h>

Log_SetChannel(MetalDevice);

// TODO: Disable hazard tracking and issue barriers explicitly.

// Looking across a range of GPUs, the optimal copy alignment for Vulkan drivers seems
// to be between 1 (AMD/NV) and 64 (Intel). So, we'll go with 64 here.
static constexpr u32 TEXTURE_UPLOAD_ALIGNMENT = 64;

// The pitch alignment must be less or equal to the upload alignment.
// We need 32 here for AVX2, so 64 is also fine.
static constexpr u32 TEXTURE_UPLOAD_PITCH_ALIGNMENT = 64;

static constexpr std::array<MTLPixelFormat, static_cast<u32>(GPUTexture::Format::MaxCount)> s_pixel_format_mapping = {
  MTLPixelFormatInvalid,      // Unknown
  MTLPixelFormatRGBA8Unorm,   // RGBA8
  MTLPixelFormatBGRA8Unorm,   // BGRA8
  MTLPixelFormatB5G6R5Unorm,  // RGB565
  MTLPixelFormatA1BGR5Unorm,  // RGBA5551
  MTLPixelFormatR8Unorm,      // R8
  MTLPixelFormatDepth16Unorm, // D16
  MTLPixelFormatR16Unorm,     // R16
  MTLPixelFormatR16Sint,      // R16I
  MTLPixelFormatR16Uint,      // R16U
  MTLPixelFormatR16Float,     // R16F
  MTLPixelFormatR32Sint,      // R32I
  MTLPixelFormatR32Uint,      // R32U
  MTLPixelFormatR32Float,     // R32F
  MTLPixelFormatRG8Unorm,     // RG8
  MTLPixelFormatRG16Unorm,    // RG16
  MTLPixelFormatRG16Float,    // RG16F
  MTLPixelFormatRG32Float,    // RG32F
  MTLPixelFormatRGBA16Unorm,  // RGBA16
  MTLPixelFormatRGBA16Float,  // RGBA16F
  MTLPixelFormatRGBA32Float,  // RGBA32F
  MTLPixelFormatBGR10A2Unorm, // RGB10A2
};

static unsigned s_next_bad_shader_id = 1;

static NSString* StringViewToNSString(const std::string_view& str)
{
  if (str.empty())
    return nil;

  return [[[NSString alloc] autorelease] initWithBytes:str.data()
                                                length:static_cast<NSUInteger>(str.length())
                                              encoding:NSUTF8StringEncoding];
}

static void LogNSError(NSError* error, const char* desc, ...)
{
  std::va_list ap;
  va_start(ap, desc);
  Log::Writev("MetalDevice", "", LOGLEVEL_ERROR, desc, ap);
  va_end(ap);

  Log::Writef("MetalDevice", "", LOGLEVEL_ERROR, "  NSError Code: %u", static_cast<u32>(error.code));
  Log::Writef("MetalDevice", "", LOGLEVEL_ERROR, "  NSError Description: %s", [error.description UTF8String]);
}

static GPUTexture::Format GetTextureFormatForMTLFormat(MTLPixelFormat fmt)
{
  for (u32 i = 0; i < static_cast<u32>(GPUTexture::Format::MaxCount); i++)
  {
    if (s_pixel_format_mapping[i] == fmt)
      return static_cast<GPUTexture::Format>(i);
  }

  return GPUTexture::Format::Unknown;
}

template<typename F>
static void RunOnMainThread(F&& f)
{
  if ([NSThread isMainThread])
    f();
  else
    dispatch_sync(dispatch_get_main_queue(), f);
}

MetalDevice::MetalDevice() : m_current_viewport(0, 0, 1, 1), m_current_scissor(0, 0, 1, 1)
{
}

MetalDevice::~MetalDevice()
{
  Assert(m_layer == nil);
  Assert(m_device == nil);
}

RenderAPI MetalDevice::GetRenderAPI() const
{
  return RenderAPI::Metal;
}

bool MetalDevice::HasSurface() const
{
  return (m_layer != nil);
}

bool MetalDevice::GetHostRefreshRate(float* refresh_rate)
{
  return GPUDevice::GetHostRefreshRate(refresh_rate);
}

void MetalDevice::SetSyncMode(DisplaySyncMode mode)
{
  m_sync_mode = mode;

  if (m_layer != nil)
  {
    const bool enabled = (mode == DisplaySyncMode::VSync || mode == DisplaySyncMode::VSyncRelaxed);
    [m_layer setDisplaySyncEnabled:enabled];
  }
}

bool MetalDevice::CreateDevice(const std::string_view& adapter, bool threaded_presentation,
                               std::optional<bool> exclusive_fullscreen_control, FeatureMask disabled_features,
                               Error* error)
{
  @autoreleasepool
  {
    id<MTLDevice> device = nil;
    if (!adapter.empty())
    {
      NSArray<id<MTLDevice>>* devices = [MTLCopyAllDevices() autorelease];
      const u32 count = static_cast<u32>([devices count]);
      for (u32 i = 0; i < count; i++)
      {
        if (adapter == [[devices[i] name] UTF8String])
        {
          device = devices[i];
          break;
        }
      }

      if (device == nil)
        Log_ErrorFmt("Failed to find device named '{}'. Trying default.", adapter);
    }

    if (device == nil)
    {
      device = [MTLCreateSystemDefaultDevice() autorelease];
      if (device == nil)
      {
        Error::SetStringView(error, "Failed to create default Metal device.");
        return false;
      }
    }

    id<MTLCommandQueue> queue = [[device newCommandQueue] autorelease];
    if (queue == nil)
    {
      Error::SetStringView(error, "Failed to create command queue.");
      return false;
    }

    m_device = [device retain];
    m_queue = [queue retain];
    Log_InfoPrintf("Metal Device: %s", [[m_device name] UTF8String]);

    SetFeatures(disabled_features);

    if (m_window_info.type != WindowInfo::Type::Surfaceless && !CreateLayer())
    {
      Error::SetStringView(error, "Failed to create layer.");
      return false;
    }

    CreateCommandBuffer();
    RenderBlankFrame();

    if (!LoadShaders())
    {
      Error::SetStringView(error, "Failed to load shaders.");
      return false;
    }

    if (!CreateBuffers())
    {
      Error::SetStringView(error, "Failed to create buffers.");
      return false;
    }

    return true;
  }
}

void MetalDevice::SetFeatures(FeatureMask disabled_features)
{
  // https://gist.github.com/kylehowells/63d0723abc9588eb734cade4b7df660d
  if ([m_device supportsFamily:MTLGPUFamilyMacCatalyst1] || [m_device supportsFamily:MTLGPUFamilyMac1] ||
      [m_device supportsFamily:MTLGPUFamilyApple3])
  {
    m_max_texture_size = 16384;
  }
  else
  {
    m_max_texture_size = 8192;
  }

  m_max_multisamples = 0;
  for (u32 multisamples = 1; multisamples < 16; multisamples *= 2)
  {
    if (![m_device supportsTextureSampleCount:multisamples])
      break;
    m_max_multisamples = multisamples;
  }

  m_features.dual_source_blend = !(disabled_features & FEATURE_MASK_DUAL_SOURCE_BLEND);
  m_features.framebuffer_fetch = !(disabled_features & FEATURE_MASK_FRAMEBUFFER_FETCH) && false; // TODO
  m_features.per_sample_shading = true;
  m_features.noperspective_interpolation = true;
  m_features.texture_copy_to_self = !(disabled_features & FEATURE_MASK_TEXTURE_COPY_TO_SELF);
  m_features.supports_texture_buffers = !(disabled_features & FEATURE_MASK_TEXTURE_BUFFERS);
  m_features.texture_buffers_emulated_with_ssbo = true;
  m_features.geometry_shaders = false;
  m_features.partial_msaa_resolve = false;
  m_features.memory_import = true;
  m_features.shader_cache = true;
  m_features.pipeline_cache = false;
  m_features.prefer_unused_textures = true;
}

bool MetalDevice::LoadShaders()
{
  @autoreleasepool
  {
    auto try_lib = [this](NSString* name) -> id<MTLLibrary> {
      NSBundle* bundle = [NSBundle mainBundle];
      NSString* path = [bundle pathForResource:name ofType:@"metallib"];
      if (path == nil)
      {
        // Xcode places it alongside the binary.
        path = [NSString stringWithFormat:@"%@/%@.metallib", [bundle bundlePath], name];
        if (![[NSFileManager defaultManager] fileExistsAtPath:path])
          return nil;
      }

      id<MTLLibrary> lib = [m_device newLibraryWithFile:path error:nil];
      if (lib == nil)
        return nil;

      return [lib retain];
    };

    if (!(m_shaders = try_lib(@"Metal23")) && !(m_shaders = try_lib(@"Metal22")) &&
        !(m_shaders = try_lib(@"Metal21")) && !(m_shaders = try_lib(@"default")))
    {
      return false;
    }

    return true;
  }
}

id<MTLFunction> MetalDevice::GetFunctionFromLibrary(id<MTLLibrary> library, NSString* name)
{
  id<MTLFunction> function = [library newFunctionWithName:name];
  return function;
}

id<MTLComputePipelineState> MetalDevice::CreateComputePipeline(id<MTLFunction> function, NSString* name)
{
  MTLComputePipelineDescriptor* desc = [MTLComputePipelineDescriptor new];
  if (name != nil)
    [desc setLabel:name];
  [desc setComputeFunction:function];

  NSError* err = nil;
  id<MTLComputePipelineState> pipeline = [m_device newComputePipelineStateWithDescriptor:desc
                                                                                 options:MTLPipelineOptionNone
                                                                              reflection:nil
                                                                                   error:&err];
  [desc release];
  if (pipeline == nil)
  {
    LogNSError(err, "Create compute pipeline failed:");
    return nil;
  }

  return pipeline;
}

void MetalDevice::DestroyDevice()
{
  WaitForPreviousCommandBuffers();

  if (InRenderPass())
    EndRenderPass();

  if (m_upload_cmdbuf != nil)
  {
    [m_upload_encoder endEncoding];
    [m_upload_encoder release];
    m_upload_encoder = nil;
    [m_upload_cmdbuf release];
    m_upload_cmdbuf = nil;
  }
  if (m_render_cmdbuf != nil)
  {
    [m_render_cmdbuf release];
    m_render_cmdbuf = nil;
  }

  DestroyBuffers();

  for (auto& it : m_cleanup_objects)
    [it.second release];
  m_cleanup_objects.clear();

  for (auto& it : m_resolve_pipelines)
  {
    if (it.second != nil)
      [it.second release];
  }
  m_resolve_pipelines.clear();
  if (m_shaders != nil)
  {
    [m_shaders release];
    m_shaders = nil;
  }
  if (m_queue != nil)
  {
    [m_queue release];
    m_queue = nil;
  }
  if (m_device != nil)
  {
    [m_device release];
    m_device = nil;
  }
}

bool MetalDevice::CreateLayer()
{
  @autoreleasepool
  {
    RunOnMainThread([this]() {
      @autoreleasepool
      {
        Log_InfoFmt("Creating a {}x{} Metal layer.", m_window_info.surface_width, m_window_info.surface_height);
        const auto size =
          CGSizeMake(static_cast<float>(m_window_info.surface_width), static_cast<float>(m_window_info.surface_height));
        m_layer = [CAMetalLayer layer];
        [m_layer setDevice:m_device];
        [m_layer setDrawableSize:size];

        // Default should be BGRA8.
        const MTLPixelFormat layer_fmt = [m_layer pixelFormat];
        m_window_info.surface_format = GetTextureFormatForMTLFormat(layer_fmt);
        if (m_window_info.surface_format == GPUTexture::Format::Unknown)
        {
          Log_ErrorFmt("Invalid pixel format {} in layer, using BGRA8.", static_cast<u32>(layer_fmt));
          [m_layer setPixelFormat:MTLPixelFormatBGRA8Unorm];
          m_window_info.surface_format = GPUTexture::Format::BGRA8;
        }

        Log_VerboseFmt("Metal layer pixel format is {}.", GPUTexture::GetFormatName(m_window_info.surface_format));

        NSView* view = GetWindowView();
        [view setWantsLayer:TRUE];
        [view setLayer:m_layer];
      }
    });

    const bool sync_enabled = (m_sync_mode == DisplaySyncMode::VSync || m_sync_mode == DisplaySyncMode::VSyncRelaxed);
    [m_layer setDisplaySyncEnabled:sync_enabled];

    DebugAssert(m_layer_pass_desc == nil);
    m_layer_pass_desc = [[MTLRenderPassDescriptor renderPassDescriptor] retain];
    m_layer_pass_desc.renderTargetWidth = m_window_info.surface_width;
    m_layer_pass_desc.renderTargetHeight = m_window_info.surface_height;
    m_layer_pass_desc.colorAttachments[0].loadAction = MTLLoadActionClear;
    m_layer_pass_desc.colorAttachments[0].storeAction = MTLStoreActionStore;
    m_layer_pass_desc.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
    return true;
  }
}

void MetalDevice::DestroyLayer()
{
  if (m_layer == nil)
    return;

  // Should wait for previous command buffers to finish, which might be rendering to drawables.
  WaitForPreviousCommandBuffers();

  [m_layer_pass_desc release];
  m_layer_pass_desc = nil;
  m_window_info.surface_format = GPUTexture::Format::Unknown;

  RunOnMainThread([this]() {
    NSView* view = GetWindowView();
    [view setLayer:nil];
    [view setWantsLayer:FALSE];
    [m_layer release];
    m_layer = nullptr;
  });
}

void MetalDevice::RenderBlankFrame()
{
  DebugAssert(!InRenderPass());
  if (m_layer == nil)
    return;

  @autoreleasepool
  {
    id<MTLDrawable> drawable = [[m_layer nextDrawable] retain];
    m_layer_pass_desc.colorAttachments[0].texture = [drawable texture];
    id<MTLRenderCommandEncoder> encoder = [m_render_cmdbuf renderCommandEncoderWithDescriptor:m_layer_pass_desc];
    [encoder endEncoding];
    [m_render_cmdbuf presentDrawable:drawable];
    DeferRelease(drawable);
    SubmitCommandBuffer();
  }
}

bool MetalDevice::UpdateWindow()
{
  if (InRenderPass())
    EndRenderPass();
  DestroyLayer();

  if (!AcquireWindow(false))
    return false;

  if (m_window_info.type != WindowInfo::Type::Surfaceless && !CreateLayer())
  {
    Log_ErrorPrintf("Failed to create layer on updated window");
    return false;
  }

  return true;
}

void MetalDevice::DestroySurface()
{
  DestroyLayer();
}

void MetalDevice::ResizeWindow(s32 new_window_width, s32 new_window_height, float new_window_scale)
{
  @autoreleasepool
  {
    m_window_info.surface_scale = new_window_scale;
    if (static_cast<u32>(new_window_width) == m_window_info.surface_width &&
        static_cast<u32>(new_window_height) == m_window_info.surface_height)
    {
      return;
    }

    m_window_info.surface_width = new_window_width;
    m_window_info.surface_height = new_window_height;

    [m_layer setDrawableSize:CGSizeMake(new_window_width, new_window_height)];
    m_layer_pass_desc.renderTargetWidth = m_window_info.surface_width;
    m_layer_pass_desc.renderTargetHeight = m_window_info.surface_height;
  }
}

std::string MetalDevice::GetDriverInfo() const
{
  @autoreleasepool
  {
    return ([[m_device description] UTF8String]);
  }
}

bool MetalDevice::CreateBuffers()
{
  if (!m_vertex_buffer.Create(m_device, VERTEX_BUFFER_SIZE) || !m_index_buffer.Create(m_device, INDEX_BUFFER_SIZE) ||
      !m_uniform_buffer.Create(m_device, UNIFORM_BUFFER_SIZE) ||
      !m_texture_upload_buffer.Create(m_device, TEXTURE_STREAM_BUFFER_SIZE))
  {
    Log_ErrorPrintf("Failed to create vertex/index/uniform buffers.");
    return false;
  }

  return true;
}

void MetalDevice::DestroyBuffers()
{
  m_texture_upload_buffer.Destroy();
  m_uniform_buffer.Destroy();
  m_vertex_buffer.Destroy();
  m_index_buffer.Destroy();

  for (auto& it : m_depth_states)
  {
    if (it.second != nil)
      [it.second release];
  }
  m_depth_states.clear();
}

bool MetalDevice::IsRenderTargetBound(const GPUTexture* tex) const
{
  for (u32 i = 0; i < m_num_current_render_targets; i++)
  {
    if (m_current_render_targets[i] == tex)
      return true;
  }

  return false;
}

GPUDevice::AdapterAndModeList MetalDevice::StaticGetAdapterAndModeList()
{
  AdapterAndModeList ret;
  @autoreleasepool
  {
    NSArray<id<MTLDevice>>* devices = [MTLCopyAllDevices() autorelease];
    const u32 count = static_cast<u32>([devices count]);
    ret.adapter_names.reserve(count);
    for (u32 i = 0; i < count; i++)
      ret.adapter_names.emplace_back([devices[i].name UTF8String]);
  }

  return ret;
}

GPUDevice::AdapterAndModeList MetalDevice::GetAdapterAndModeList()
{
  return StaticGetAdapterAndModeList();
}

bool MetalDevice::SetGPUTimingEnabled(bool enabled)
{
  if (m_gpu_timing_enabled == enabled)
    return true;

  std::unique_lock lock(m_fence_mutex);
  m_gpu_timing_enabled = enabled;
  m_accumulated_gpu_time = 0.0;
  m_last_gpu_time_end = 0.0;
  return true;
}

float MetalDevice::GetAndResetAccumulatedGPUTime()
{
  std::unique_lock lock(m_fence_mutex);
  return std::exchange(m_accumulated_gpu_time, 0.0) * 1000.0;
}

MetalShader::MetalShader(GPUShaderStage stage, id<MTLLibrary> library, id<MTLFunction> function)
  : GPUShader(stage), m_library(library), m_function(function)
{
}

MetalShader::~MetalShader()
{
  MetalDevice::DeferRelease(m_function);
  MetalDevice::DeferRelease(m_library);
}

void MetalShader::SetDebugName(const std::string_view& name)
{
  @autoreleasepool
  {
    [m_function setLabel:StringViewToNSString(name)];
  }
}

// TODO: Clean this up, somehow..
namespace EmuFolders {
extern std::string DataRoot;
}
static void DumpShader(u32 n, const std::string_view& suffix, const std::string_view& data)
{
  if (data.empty())
    return;

  auto fp = FileSystem::OpenManagedCFile(
    Path::Combine(EmuFolders::DataRoot, fmt::format("shader{}_{}.txt", suffix, n)).c_str(), "wb");
  if (!fp)
    return;

  std::fwrite(data.data(), data.length(), 1, fp.get());
}

std::unique_ptr<GPUShader> MetalDevice::CreateShaderFromMSL(GPUShaderStage stage, const std::string_view& source,
                                                            const std::string_view& entry_point)
{
  @autoreleasepool
  {
    NSString* const ns_source = StringViewToNSString(source);
    NSError* error = nullptr;
    id<MTLLibrary> library = [m_device newLibraryWithSource:ns_source options:nil error:&error];
    if (!library)
    {
      LogNSError(error, "Failed to compile %s shader", GPUShader::GetStageName(stage));

      auto fp = FileSystem::OpenManagedCFile(
        Path::Combine(EmuFolders::DataRoot, fmt::format("bad_shader_{}.txt", s_next_bad_shader_id++)).c_str(), "wb");
      if (fp)
      {
        std::fwrite(source.data(), source.size(), 1, fp.get());
        std::fprintf(fp.get(), "\n\nCompile %s failed: %u\n", GPUShader::GetStageName(stage),
                     static_cast<u32>(error.code));

        const char* utf_error = [error.description UTF8String];
        std::fwrite(utf_error, std::strlen(utf_error), 1, fp.get());
      }

      return {};
    }

    id<MTLFunction> function = [library newFunctionWithName:StringViewToNSString(entry_point)];
    if (!function)
    {
      Log_ErrorPrintf("Failed to get main function in compiled library");
      return {};
    }

    return std::unique_ptr<MetalShader>(new MetalShader(stage, [library retain], [function retain]));
  }
}

std::unique_ptr<GPUShader> MetalDevice::CreateShaderFromBinary(GPUShaderStage stage, std::span<const u8> data)
{
  const std::string_view str_data(reinterpret_cast<const char*>(data.data()), data.size());
  return CreateShaderFromMSL(stage, str_data, "main0");
}

std::unique_ptr<GPUShader> MetalDevice::CreateShaderFromSource(GPUShaderStage stage, const std::string_view& source,
                                                               const char* entry_point,
                                                               DynamicHeapArray<u8>* out_binary /* = nullptr */)
{
  const u32 options = (m_debug_device ? SPIRVCompiler::DebugInfo : 0) | SPIRVCompiler::VulkanRules;
  static constexpr bool dump_shaders = false;

  if (std::strcmp(entry_point, "main") != 0)
  {
    Log_ErrorPrintf("Entry point must be 'main', but got '%s' instead.", entry_point);
    return {};
  }

  std::optional<SPIRVCompiler::SPIRVCodeVector> spirv = SPIRVCompiler::CompileShader(stage, source, options);
  if (!spirv.has_value())
  {
    Log_ErrorPrintf("Failed to compile shader to SPIR-V.");
    return {};
  }

  std::optional<std::string> msl = SPIRVCompiler::CompileSPIRVToMSL(stage, spirv.value());
  if (!msl.has_value())
  {
    Log_ErrorPrintf("Failed to compile SPIR-V to MSL.");
    return {};
  }
  if constexpr (dump_shaders)
  {
    DumpShader(s_next_bad_shader_id, "_input", source);
    DumpShader(s_next_bad_shader_id, "_msl", msl.value());
    s_next_bad_shader_id++;
  }

  if (out_binary)
  {
    out_binary->resize(msl->size());
    std::memcpy(out_binary->data(), msl->data(), msl->size());
  }

  return CreateShaderFromMSL(stage, msl.value(), "main0");
}

MetalPipeline::MetalPipeline(id<MTLRenderPipelineState> pipeline, id<MTLDepthStencilState> depth, MTLCullMode cull_mode,
                             MTLPrimitiveType primitive)
  : m_pipeline(pipeline), m_depth(depth), m_cull_mode(cull_mode), m_primitive(primitive)
{
}

MetalPipeline::~MetalPipeline()
{
  MetalDevice::DeferRelease(m_pipeline);
}

void MetalPipeline::SetDebugName(const std::string_view& name)
{
  // readonly property :/
}

id<MTLDepthStencilState> MetalDevice::GetDepthState(const GPUPipeline::DepthState& ds)
{
  const auto it = m_depth_states.find(ds.key);
  if (it != m_depth_states.end())
    return it->second;

  @autoreleasepool
  {
    static constexpr std::array<MTLCompareFunction, static_cast<u32>(GPUPipeline::DepthFunc::MaxCount)> func_mapping = {
      {
        MTLCompareFunctionNever,        // Never
        MTLCompareFunctionAlways,       // Always
        MTLCompareFunctionLess,         // Less
        MTLCompareFunctionLessEqual,    // LessEqual
        MTLCompareFunctionGreater,      // Greater
        MTLCompareFunctionGreaterEqual, // GreaterEqual
        MTLCompareFunctionEqual,        // Equal
      }};

    MTLDepthStencilDescriptor* desc = [[MTLDepthStencilDescriptor new] autorelease];
    desc.depthCompareFunction = func_mapping[static_cast<u8>(ds.depth_test.GetValue())];
    desc.depthWriteEnabled = ds.depth_write ? TRUE : FALSE;

    id<MTLDepthStencilState> state = [m_device newDepthStencilStateWithDescriptor:desc];
    m_depth_states.emplace(ds.key, state);
    if (state == nil)
      Log_ErrorPrintf("Failed to create depth-stencil state.");

    return state;
  }
}

std::unique_ptr<GPUPipeline> MetalDevice::CreatePipeline(const GPUPipeline::GraphicsConfig& config)
{
  @autoreleasepool
  {
    static constexpr std::array<MTLPrimitiveTopologyClass, static_cast<u32>(GPUPipeline::Primitive::MaxCount)>
      primitive_classes = {{
        MTLPrimitiveTopologyClassPoint,    // Points
        MTLPrimitiveTopologyClassLine,     // Lines
        MTLPrimitiveTopologyClassTriangle, // Triangles
        MTLPrimitiveTopologyClassTriangle, // TriangleStrips
      }};
    static constexpr std::array<MTLPrimitiveType, static_cast<u32>(GPUPipeline::Primitive::MaxCount)> primitives = {{
      MTLPrimitiveTypePoint,         // Points
      MTLPrimitiveTypeLine,          // Lines
      MTLPrimitiveTypeTriangle,      // Triangles
      MTLPrimitiveTypeTriangleStrip, // TriangleStrips
    }};

    static constexpr u32 MAX_COMPONENTS = 4;
    static constexpr const MTLVertexFormat
      format_mapping[static_cast<u8>(GPUPipeline::VertexAttribute::Type::MaxCount)][MAX_COMPONENTS] = {
        {MTLVertexFormatFloat, MTLVertexFormatFloat2, MTLVertexFormatFloat3, MTLVertexFormatFloat4}, // Float
        {MTLVertexFormatUChar, MTLVertexFormatUChar2, MTLVertexFormatUChar3, MTLVertexFormatUChar4}, // UInt8
        {MTLVertexFormatChar, MTLVertexFormatChar2, MTLVertexFormatChar3, MTLVertexFormatChar4},     // SInt8
        {MTLVertexFormatUCharNormalized, MTLVertexFormatUChar2Normalized, MTLVertexFormatUChar3Normalized,
         MTLVertexFormatUChar4Normalized},                                                               // UNorm8
        {MTLVertexFormatUShort, MTLVertexFormatUShort2, MTLVertexFormatUShort3, MTLVertexFormatUShort4}, // UInt16
        {MTLVertexFormatShort, MTLVertexFormatShort2, MTLVertexFormatShort3, MTLVertexFormatShort4},     // SInt16
        {MTLVertexFormatUShortNormalized, MTLVertexFormatUShort2Normalized, MTLVertexFormatUShort3Normalized,
         MTLVertexFormatUShort4Normalized},                                                      // UNorm16
        {MTLVertexFormatUInt, MTLVertexFormatUInt2, MTLVertexFormatUInt3, MTLVertexFormatUInt4}, // UInt32
        {MTLVertexFormatInt, MTLVertexFormatInt2, MTLVertexFormatInt3, MTLVertexFormatInt4},     // SInt32
      };

    static constexpr std::array<MTLCullMode, static_cast<u32>(GPUPipeline::CullMode::MaxCount)> cull_mapping = {{
      MTLCullModeNone,  // None
      MTLCullModeFront, // Front
      MTLCullModeBack,  // Back
    }};

    static constexpr std::array<MTLBlendFactor, static_cast<u32>(GPUPipeline::BlendFunc::MaxCount)> blend_mapping = {{
      MTLBlendFactorZero,                     // Zero
      MTLBlendFactorOne,                      // One
      MTLBlendFactorSourceColor,              // SrcColor
      MTLBlendFactorOneMinusSourceColor,      // InvSrcColor
      MTLBlendFactorDestinationColor,         // DstColor
      MTLBlendFactorOneMinusDestinationColor, // InvDstColor
      MTLBlendFactorSourceAlpha,              // SrcAlpha
      MTLBlendFactorOneMinusSourceAlpha,      // InvSrcAlpha
      MTLBlendFactorSource1Alpha,             // SrcAlpha1
      MTLBlendFactorOneMinusSource1Alpha,     // InvSrcAlpha1
      MTLBlendFactorDestinationAlpha,         // DstAlpha
      MTLBlendFactorOneMinusDestinationAlpha, // InvDstAlpha
      MTLBlendFactorBlendColor,               // ConstantAlpha
      MTLBlendFactorOneMinusBlendColor,       // InvConstantAlpha
    }};

    static constexpr std::array<MTLBlendOperation, static_cast<u32>(GPUPipeline::BlendOp::MaxCount)> op_mapping = {{
      MTLBlendOperationAdd,             // Add
      MTLBlendOperationSubtract,        // Subtract
      MTLBlendOperationReverseSubtract, // ReverseSubtract
      MTLBlendOperationMin,             // Min
      MTLBlendOperationMax,             // Max
    }};

    MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor new] autorelease];
    desc.vertexFunction = static_cast<const MetalShader*>(config.vertex_shader)->GetFunction();
    desc.fragmentFunction = static_cast<const MetalShader*>(config.fragment_shader)->GetFunction();

    for (u32 i = 0; i < MAX_RENDER_TARGETS; i++)
    {
      if (config.color_formats[i] == GPUTexture::Format::Unknown)
        break;
      desc.colorAttachments[0].pixelFormat = s_pixel_format_mapping[static_cast<u8>(config.color_formats[i])];
    }
    desc.depthAttachmentPixelFormat = s_pixel_format_mapping[static_cast<u8>(config.depth_format)];

    // Input assembly.
    MTLVertexDescriptor* vdesc = nil;
    if (!config.input_layout.vertex_attributes.empty())
    {
      vdesc = [MTLVertexDescriptor vertexDescriptor];
      for (u32 i = 0; i < static_cast<u32>(config.input_layout.vertex_attributes.size()); i++)
      {
        const GPUPipeline::VertexAttribute& va = config.input_layout.vertex_attributes[i];
        DebugAssert(va.components > 0 && va.components <= MAX_COMPONENTS);

        MTLVertexAttributeDescriptor* vd = vdesc.attributes[i];
        vd.format = format_mapping[static_cast<u8>(va.type.GetValue())][va.components - 1];
        vd.offset = static_cast<NSUInteger>(va.offset.GetValue());
        vd.bufferIndex = 1;
      }

      vdesc.layouts[1].stepFunction = MTLVertexStepFunctionPerVertex;
      vdesc.layouts[1].stepRate = 1;
      vdesc.layouts[1].stride = config.input_layout.vertex_stride;

      desc.vertexDescriptor = vdesc;
    }

    // Rasterization state.
    const MTLCullMode cull_mode = cull_mapping[static_cast<u8>(config.rasterization.cull_mode.GetValue())];
    desc.rasterizationEnabled = TRUE;
    desc.inputPrimitiveTopology = primitive_classes[static_cast<u8>(config.primitive)];

    // Depth state
    id<MTLDepthStencilState> depth = GetDepthState(config.depth);
    if (depth == nil)
      return {};

    // Blending state
    MTLRenderPipelineColorAttachmentDescriptor* ca = desc.colorAttachments[0];
    ca.writeMask = (config.blend.write_r ? MTLColorWriteMaskRed : MTLColorWriteMaskNone) |
                   (config.blend.write_g ? MTLColorWriteMaskGreen : MTLColorWriteMaskNone) |
                   (config.blend.write_b ? MTLColorWriteMaskBlue : MTLColorWriteMaskNone) |
                   (config.blend.write_a ? MTLColorWriteMaskAlpha : MTLColorWriteMaskNone);

    // General
    const MTLPrimitiveType primitive = primitives[static_cast<u8>(config.primitive)];
    desc.rasterSampleCount = config.samples;

    // Metal-specific stuff
    desc.vertexBuffers[0].mutability = MTLMutabilityImmutable;
    desc.fragmentBuffers[0].mutability = MTLMutabilityImmutable;
    if (!config.input_layout.vertex_attributes.empty())
      desc.vertexBuffers[1].mutability = MTLMutabilityImmutable;
    if (config.layout == GPUPipeline::Layout::SingleTextureBufferAndPushConstants)
      desc.fragmentBuffers[1].mutability = MTLMutabilityImmutable;

    ca.blendingEnabled = config.blend.enable;
    if (config.blend.enable)
    {
      ca.sourceRGBBlendFactor = blend_mapping[static_cast<u8>(config.blend.src_blend.GetValue())];
      ca.destinationRGBBlendFactor = blend_mapping[static_cast<u8>(config.blend.dst_blend.GetValue())];
      ca.rgbBlendOperation = op_mapping[static_cast<u8>(config.blend.blend_op.GetValue())];
      ca.sourceAlphaBlendFactor = blend_mapping[static_cast<u8>(config.blend.src_alpha_blend.GetValue())];
      ca.destinationAlphaBlendFactor = blend_mapping[static_cast<u8>(config.blend.dst_alpha_blend.GetValue())];
      ca.alphaBlendOperation = op_mapping[static_cast<u8>(config.blend.alpha_blend_op.GetValue())];
    }

    NSError* error = nullptr;
    id<MTLRenderPipelineState> pipeline = [m_device newRenderPipelineStateWithDescriptor:desc error:&error];
    if (pipeline == nil)
    {
      LogNSError(error, "Failed to create render pipeline state");
      return {};
    }

    return std::unique_ptr<GPUPipeline>(new MetalPipeline(pipeline, depth, cull_mode, primitive));
  }
}

MetalTexture::MetalTexture(id<MTLTexture> texture, u16 width, u16 height, u8 layers, u8 levels, u8 samples, Type type,
                           Format format)
  : GPUTexture(width, height, layers, levels, samples, type, format), m_texture(texture)
{
}

MetalTexture::~MetalTexture()
{
  if (m_texture != nil)
  {
    MetalDevice::GetInstance().UnbindTexture(this);
    MetalDevice::DeferRelease(m_texture);
  }
}

bool MetalTexture::Update(u32 x, u32 y, u32 width, u32 height, const void* data, u32 pitch, u32 layer /*= 0*/,
                          u32 level /*= 0*/)
{
  const u32 aligned_pitch = Common::AlignUpPow2(width * GetPixelSize(), TEXTURE_UPLOAD_PITCH_ALIGNMENT);
  const u32 req_size = height * aligned_pitch;

  GPUDevice::GetStatistics().buffer_streamed += req_size;
  GPUDevice::GetStatistics().num_uploads++;

  MetalDevice& dev = MetalDevice::GetInstance();
  MetalStreamBuffer& sb = dev.GetTextureStreamBuffer();
  id<MTLBuffer> actual_buffer;
  u32 actual_offset;
  u32 actual_pitch;
  if (req_size >= (sb.GetCurrentSize() / 2u))
  {
    const u32 upload_size = height * pitch;
    const MTLResourceOptions options = MTLResourceStorageModeShared;
    actual_buffer = [dev.GetMTLDevice() newBufferWithBytes:data length:upload_size options:options];
    actual_offset = 0;
    actual_pitch = pitch;
    if (actual_buffer == nil)
    {
      Panic("Failed to allocate temporary buffer.");
      return false;
    }

    dev.DeferRelease(actual_buffer);
  }
  else
  {
    if (!sb.ReserveMemory(req_size, TEXTURE_UPLOAD_ALIGNMENT))
    {
      dev.SubmitCommandBuffer();
      if (!sb.ReserveMemory(req_size, TEXTURE_UPLOAD_ALIGNMENT))
      {
        Panic("Failed to reserve texture upload space.");
        return false;
      }
    }

    actual_offset = sb.GetCurrentOffset();
    StringUtil::StrideMemCpy(sb.GetCurrentHostPointer(), aligned_pitch, data, pitch, width * GetPixelSize(), height);
    sb.CommitMemory(req_size);
    actual_buffer = sb.GetBuffer();
    actual_pitch = aligned_pitch;
  }

  if (m_state == GPUTexture::State::Cleared && (x != 0 || y != 0 || width != m_width || height != m_height))
    dev.CommitClear(this);

  const bool is_inline = (m_use_fence_counter == dev.GetCurrentFenceCounter());

  id<MTLBlitCommandEncoder> encoder = dev.GetBlitEncoder(is_inline);
  [encoder copyFromBuffer:actual_buffer
             sourceOffset:actual_offset
        sourceBytesPerRow:actual_pitch
      sourceBytesPerImage:0
               sourceSize:MTLSizeMake(width, height, 1)
                toTexture:m_texture
         destinationSlice:layer
         destinationLevel:level
        destinationOrigin:MTLOriginMake(x, y, 0)];
  m_state = GPUTexture::State::Dirty;
  return true;
}

bool MetalTexture::Map(void** map, u32* map_stride, u32 x, u32 y, u32 width, u32 height, u32 layer /*= 0*/,
                       u32 level /*= 0*/)
{
  if ((x + width) > GetMipWidth(level) || (y + height) > GetMipHeight(level) || layer > m_layers || level > m_levels)
    return false;

  const u32 aligned_pitch = Common::AlignUpPow2(width * GetPixelSize(), TEXTURE_UPLOAD_PITCH_ALIGNMENT);
  const u32 req_size = height * aligned_pitch;

  MetalDevice& dev = MetalDevice::GetInstance();
  if (m_state == GPUTexture::State::Cleared && (x != 0 || y != 0 || width != m_width || height != m_height))
    dev.CommitClear(this);

  MetalStreamBuffer& sb = dev.GetTextureStreamBuffer();
  if (!sb.ReserveMemory(req_size, TEXTURE_UPLOAD_ALIGNMENT))
  {
    dev.SubmitCommandBuffer();
    if (!sb.ReserveMemory(req_size, TEXTURE_UPLOAD_ALIGNMENT))
    {
      Panic("Failed to allocate space in texture upload buffer");
      return false;
    }
  }

  *map = sb.GetCurrentHostPointer();
  *map_stride = aligned_pitch;
  m_map_x = x;
  m_map_y = y;
  m_map_width = width;
  m_map_height = height;
  m_map_layer = layer;
  m_map_level = level;
  m_state = GPUTexture::State::Dirty;
  return true;
}

void MetalTexture::Unmap()
{
  const u32 aligned_pitch = Common::AlignUpPow2(m_map_width * GetPixelSize(), TEXTURE_UPLOAD_PITCH_ALIGNMENT);
  const u32 req_size = m_map_height * aligned_pitch;

  GPUDevice::GetStatistics().buffer_streamed += req_size;
  GPUDevice::GetStatistics().num_uploads++;

  MetalDevice& dev = MetalDevice::GetInstance();
  MetalStreamBuffer& sb = dev.GetTextureStreamBuffer();
  const u32 offset = sb.GetCurrentOffset();
  sb.CommitMemory(req_size);

  // TODO: track this
  const bool is_inline = true;
  id<MTLBlitCommandEncoder> encoder = dev.GetBlitEncoder(is_inline);
  [encoder copyFromBuffer:sb.GetBuffer()
             sourceOffset:offset
        sourceBytesPerRow:aligned_pitch
      sourceBytesPerImage:0
               sourceSize:MTLSizeMake(m_map_width, m_map_height, 1)
                toTexture:m_texture
         destinationSlice:m_map_layer
         destinationLevel:m_map_level
        destinationOrigin:MTLOriginMake(m_map_x, m_map_y, 0)];

  m_map_x = 0;
  m_map_y = 0;
  m_map_width = 0;
  m_map_height = 0;
  m_map_layer = 0;
  m_map_level = 0;
}

void MetalTexture::MakeReadyForSampling()
{
  MetalDevice& dev = MetalDevice::GetInstance();
  if (!dev.InRenderPass())
    return;

  if (IsRenderTarget() ? dev.IsRenderTargetBound(this) : (dev.m_current_depth_target == this))
    dev.EndRenderPass();
}

void MetalTexture::SetDebugName(const std::string_view& name)
{
  @autoreleasepool
  {
    [m_texture setLabel:StringViewToNSString(name)];
  }
}

std::unique_ptr<GPUTexture> MetalDevice::CreateTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                                       GPUTexture::Type type, GPUTexture::Format format,
                                                       const void* data, u32 data_stride)
{
  if (!GPUTexture::ValidateConfig(width, height, layers, layers, samples, type, format))
    return {};

  const MTLPixelFormat pixel_format = s_pixel_format_mapping[static_cast<u8>(format)];
  if (pixel_format == MTLPixelFormatInvalid)
    return {};

  @autoreleasepool
  {
    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pixel_format
                                                                                    width:width
                                                                                   height:height
                                                                                mipmapped:(levels > 1)];

    desc.mipmapLevelCount = levels;
    desc.storageMode = MTLStorageModePrivate;
    if (samples > 1)
    {
      desc.textureType = (layers > 1) ? MTLTextureType2DMultisampleArray : MTLTextureType2DMultisample;
      desc.sampleCount = samples;
    }
    else if (layers > 1)
    {
      desc.textureType = MTLTextureType2DArray;
      desc.arrayLength = layers;
    }

    switch (type)
    {
      case GPUTexture::Type::Texture:
      case GPUTexture::Type::DynamicTexture:
        desc.usage = MTLTextureUsageShaderRead;
        break;

      case GPUTexture::Type::RenderTarget:
      case GPUTexture::Type::DepthStencil:
        desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        break;

      case GPUTexture::Type::RWTexture:
        desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        break;

      default:
        UnreachableCode();
        break;
    }

    id<MTLTexture> tex = [m_device newTextureWithDescriptor:desc];
    if (tex == nil)
    {
      Log_ErrorPrintf("Failed to create %ux%u texture.", width, height);
      return {};
    }

    // This one can *definitely* go on the upload buffer.
    std::unique_ptr<GPUTexture> gtex(
      new MetalTexture([tex retain], width, height, layers, levels, samples, type, format));
    if (data)
    {
      // TODO: handle multi-level uploads...
      gtex->Update(0, 0, width, height, data, data_stride, 0, 0);
    }

    return gtex;
  }
}

MetalDownloadTexture::MetalDownloadTexture(u32 width, u32 height, GPUTexture::Format format, u8* import_buffer,
                                           size_t buffer_offset, id<MTLBuffer> buffer, const u8* map_ptr, u32 map_pitch)
  : GPUDownloadTexture(width, height, format, (import_buffer != nullptr)), m_buffer_offset(buffer_offset),
    m_buffer(buffer)
{
  m_map_pointer = map_ptr;
  m_current_pitch = map_pitch;
}

MetalDownloadTexture::~MetalDownloadTexture()
{
  [m_buffer release];
}

std::unique_ptr<MetalDownloadTexture> MetalDownloadTexture::Create(u32 width, u32 height, GPUTexture::Format format,
                                                                   void* memory, size_t memory_size, u32 memory_stride)
{
  @autoreleasepool
  {
    MetalDevice& dev = MetalDevice::GetInstance();
    id<MTLBuffer> buffer = nil;
    size_t memory_offset = 0;
    const u8* map_ptr = nullptr;
    u32 map_pitch = 0;
    u32 buffer_size = 0;

    constexpr MTLResourceOptions options = MTLResourceStorageModeShared | MTLResourceCPUCacheModeDefaultCache;

    // not importing memory?
    if (!memory)
    {
      map_pitch = Common::AlignUpPow2(GPUTexture::CalcUploadPitch(format, width), TEXTURE_UPLOAD_PITCH_ALIGNMENT);
      buffer_size = height * map_pitch;
      buffer = [[dev.m_device newBufferWithLength:buffer_size options:options] retain];
      if (buffer == nil)
      {
        Log_ErrorFmt("Failed to create {} byte buffer", buffer_size);
        return {};
      }

      map_ptr = static_cast<u8*>([buffer contents]);
    }
    else
    {
      map_pitch = memory_stride;
      buffer_size = height * map_pitch;
      Assert(buffer_size <= memory_size);

      // Importing memory, we need to page align the buffer.
      void* page_aligned_memory =
        reinterpret_cast<void*>(Common::AlignDownPow2(reinterpret_cast<uintptr_t>(memory), HOST_PAGE_SIZE));
      const size_t page_offset = static_cast<size_t>(static_cast<u8*>(memory) - static_cast<u8*>(page_aligned_memory));
      const size_t page_aligned_size = Common::AlignUpPow2(page_offset + memory_size, HOST_PAGE_SIZE);
      Log_DevFmt("Trying to import {} bytes of memory at {} for download texture", page_aligned_memory,
                 page_aligned_size);

      buffer = [[dev.m_device newBufferWithBytesNoCopy:page_aligned_memory
                                                length:page_aligned_size
                                               options:options
                                           deallocator:nil] retain];
      if (buffer == nil)
      {
        Log_ErrorFmt("Failed to import {} byte buffer", page_aligned_size);
        return {};
      }

      map_ptr = static_cast<u8*>(memory);
    }

    return std::unique_ptr<MetalDownloadTexture>(new MetalDownloadTexture(
      width, height, format, static_cast<u8*>(memory), memory_offset, buffer, map_ptr, map_pitch));
  }
}

void MetalDownloadTexture::CopyFromTexture(u32 dst_x, u32 dst_y, GPUTexture* src, u32 src_x, u32 src_y, u32 width,
                                           u32 height, u32 src_layer, u32 src_level, bool use_transfer_pitch)
{
  MetalTexture* const mtlTex = static_cast<MetalTexture*>(src);
  MetalDevice& dev = MetalDevice::GetInstance();

  DebugAssert(mtlTex->GetFormat() == m_format);
  DebugAssert(src_level < mtlTex->GetLevels());
  DebugAssert((src_x + width) <= mtlTex->GetMipWidth(src_level) && (src_y + height) <= mtlTex->GetMipHeight(src_level));
  DebugAssert((dst_x + width) <= m_width && (dst_y + height) <= m_height);
  DebugAssert((dst_x == 0 && dst_y == 0) || !use_transfer_pitch);
  DebugAssert(!m_is_imported || !use_transfer_pitch);

  u32 copy_offset, copy_size, copy_rows;
  if (!m_is_imported)
    m_current_pitch = GetTransferPitch(use_transfer_pitch ? width : m_width, TEXTURE_UPLOAD_PITCH_ALIGNMENT);
  GetTransferSize(dst_x, dst_y, width, height, m_current_pitch, &copy_offset, &copy_size, &copy_rows);

  dev.GetStatistics().num_downloads++;

  dev.CommitClear(mtlTex);

  id<MTLBlitCommandEncoder> encoder = dev.GetBlitEncoder(true);
  [encoder copyFromTexture:mtlTex->GetMTLTexture()
                 sourceSlice:src_layer
                 sourceLevel:src_level
                sourceOrigin:MTLOriginMake(src_x, src_y, 0)
                  sourceSize:MTLSizeMake(width, height, 1)
                    toBuffer:m_buffer
           destinationOffset:m_buffer_offset + copy_offset
      destinationBytesPerRow:m_current_pitch
    destinationBytesPerImage:0];

  m_copy_fence_counter = dev.m_current_fence_counter;
  m_needs_flush = true;
}

bool MetalDownloadTexture::Map(u32 x, u32 y, u32 width, u32 height)
{
  // Always mapped.
  return true;
}

void MetalDownloadTexture::Unmap()
{
  // Always mapped.
}

void MetalDownloadTexture::Flush()
{
  if (!m_needs_flush)
    return;

  m_needs_flush = false;

  MetalDevice& dev = MetalDevice::GetInstance();
  if (dev.m_completed_fence_counter >= m_copy_fence_counter)
    return;

  // Need to execute command buffer.
  if (dev.GetCurrentFenceCounter() == m_copy_fence_counter)
    dev.SubmitCommandBuffer(true);
  else
    dev.WaitForFenceCounter(m_copy_fence_counter);
}

void MetalDownloadTexture::SetDebugName(std::string_view name)
{
  @autoreleasepool
  {
    [m_buffer setLabel:StringViewToNSString(name)];
  }
}

std::unique_ptr<GPUDownloadTexture> MetalDevice::CreateDownloadTexture(u32 width, u32 height, GPUTexture::Format format)
{
  return MetalDownloadTexture::Create(width, height, format, nullptr, 0, 0);
}

std::unique_ptr<GPUDownloadTexture> MetalDevice::CreateDownloadTexture(u32 width, u32 height, GPUTexture::Format format,
                                                                       void* memory, size_t memory_size,
                                                                       u32 memory_stride)
{
  return MetalDownloadTexture::Create(width, height, format, memory, memory_size, memory_stride);
}

MetalSampler::MetalSampler(id<MTLSamplerState> ss) : m_ss(ss)
{
}

MetalSampler::~MetalSampler() = default;

void MetalSampler::SetDebugName(const std::string_view& name)
{
  // lame.. have to put it on the descriptor :/
}

std::unique_ptr<GPUSampler> MetalDevice::CreateSampler(const GPUSampler::Config& config)
{
  @autoreleasepool
  {
    static constexpr std::array<MTLSamplerAddressMode, static_cast<u8>(GPUSampler::AddressMode::MaxCount)> ta = {{
      MTLSamplerAddressModeRepeat,             // Repeat
      MTLSamplerAddressModeClampToEdge,        // ClampToEdge
      MTLSamplerAddressModeClampToBorderColor, // ClampToBorder
      MTLSamplerAddressModeMirrorRepeat,       // MirrorRepeat
    }};
    static constexpr std::array<MTLSamplerMinMagFilter, static_cast<u8>(GPUSampler::Filter::MaxCount)> min_mag_filters =
      {{
        MTLSamplerMinMagFilterNearest, // Nearest
        MTLSamplerMinMagFilterLinear,  // Linear
      }};
    static constexpr std::array<MTLSamplerMipFilter, static_cast<u8>(GPUSampler::Filter::MaxCount)> mip_filters = {{
      MTLSamplerMipFilterNearest, // Nearest
      MTLSamplerMipFilterLinear,  // Linear
    }};

    struct BorderColorMapping
    {
      u32 color;
      MTLSamplerBorderColor mtl_color;
    };
    static constexpr BorderColorMapping border_color_mapping[] = {
      {0x00000000u, MTLSamplerBorderColorTransparentBlack},
      {0xFF000000u, MTLSamplerBorderColorOpaqueBlack},
      {0xFFFFFFFFu, MTLSamplerBorderColorOpaqueWhite},
    };

    MTLSamplerDescriptor* desc = [[MTLSamplerDescriptor new] autorelease];
    desc.normalizedCoordinates = true;
    desc.sAddressMode = ta[static_cast<u8>(config.address_u.GetValue())];
    desc.tAddressMode = ta[static_cast<u8>(config.address_v.GetValue())];
    desc.rAddressMode = ta[static_cast<u8>(config.address_w.GetValue())];
    desc.minFilter = min_mag_filters[static_cast<u8>(config.min_filter.GetValue())];
    desc.magFilter = min_mag_filters[static_cast<u8>(config.mag_filter.GetValue())];
    desc.mipFilter = (config.min_lod != config.max_lod) ? mip_filters[static_cast<u8>(config.mip_filter.GetValue())] :
                                                          MTLSamplerMipFilterNotMipmapped;
    desc.lodMinClamp = static_cast<float>(config.min_lod);
    desc.lodMaxClamp = static_cast<float>(config.max_lod);
    desc.maxAnisotropy = std::max<u8>(config.anisotropy, 1);

    if (config.address_u == GPUSampler::AddressMode::ClampToBorder ||
        config.address_v == GPUSampler::AddressMode::ClampToBorder ||
        config.address_w == GPUSampler::AddressMode::ClampToBorder)
    {
      u32 i;
      for (i = 0; i < static_cast<u32>(std::size(border_color_mapping)); i++)
      {
        if (border_color_mapping[i].color == config.border_color)
          break;
      }
      if (i == std::size(border_color_mapping))
      {
        Log_ErrorPrintf("Unsupported border color: %08X", config.border_color.GetValue());
        return {};
      }

      desc.borderColor = border_color_mapping[i].mtl_color;
    }

    // TODO: Pool?
    id<MTLSamplerState> ss = [m_device newSamplerStateWithDescriptor:desc];
    if (ss == nil)
    {
      Log_ErrorPrintf("Failed to create sampler state.");
      return {};
    }

    return std::unique_ptr<GPUSampler>(new MetalSampler([ss retain]));
  }
}

bool MetalDevice::SupportsTextureFormat(GPUTexture::Format format) const
{
  if (format == GPUTexture::Format::RGB565 || format == GPUTexture::Format::RGBA5551)
  {
    // These formats require an Apple Silicon GPU.
    // See https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf
    if (![m_device supportsFamily:MTLGPUFamilyApple2])
      return false;
  }

  return (s_pixel_format_mapping[static_cast<u8>(format)] != MTLPixelFormatInvalid);
}

void MetalDevice::CopyTextureRegion(GPUTexture* dst, u32 dst_x, u32 dst_y, u32 dst_layer, u32 dst_level,
                                    GPUTexture* src, u32 src_x, u32 src_y, u32 src_layer, u32 src_level, u32 width,
                                    u32 height)
{
  DebugAssert(src_level < src->GetLevels() && src_layer < src->GetLayers());
  DebugAssert((src_x + width) <= src->GetMipWidth(src_level));
  DebugAssert((src_y + height) <= src->GetMipHeight(src_level));
  DebugAssert(dst_level < dst->GetLevels() && dst_layer < dst->GetLayers());
  DebugAssert((dst_x + width) <= dst->GetMipWidth(dst_level));
  DebugAssert((dst_y + height) <= dst->GetMipHeight(dst_level));

  MetalTexture* D = static_cast<MetalTexture*>(dst);
  MetalTexture* S = static_cast<MetalTexture*>(src);

  if (D->IsRenderTargetOrDepthStencil())
  {
    if (S->GetState() == GPUTexture::State::Cleared)
    {
      if (S->GetWidth() == D->GetWidth() && S->GetHeight() == D->GetHeight())
      {
        // pass clear through
        D->m_state = S->m_state;
        D->m_clear_value = S->m_clear_value;
        return;
      }
    }
    else if (S->GetState() == GPUTexture::State::Invalidated)
    {
      // Contents are undefined ;)
      return;
    }
    else if (dst_x == 0 && dst_y == 0 && width == D->GetMipWidth(dst_level) && height == D->GetMipHeight(dst_level))
    {
      D->SetState(GPUTexture::State::Dirty);
    }

    CommitClear(D);
  }

  CommitClear(S);

  S->SetUseFenceCounter(m_current_fence_counter);
  D->SetUseFenceCounter(m_current_fence_counter);

  s_stats.num_copies++;

  @autoreleasepool
  {
    id<MTLBlitCommandEncoder> encoder = GetBlitEncoder(true);
    [encoder copyFromTexture:S->GetMTLTexture()
                 sourceSlice:src_level
                 sourceLevel:src_level
                sourceOrigin:MTLOriginMake(src_x, src_y, 0)
                  sourceSize:MTLSizeMake(width, height, 1)
                   toTexture:D->GetMTLTexture()
            destinationSlice:dst_layer
            destinationLevel:dst_level
           destinationOrigin:MTLOriginMake(dst_x, dst_y, 0)];
  }
}

void MetalDevice::ResolveTextureRegion(GPUTexture* dst, u32 dst_x, u32 dst_y, u32 dst_layer, u32 dst_level,
                                       GPUTexture* src, u32 src_x, u32 src_y, u32 width, u32 height)
{
  DebugAssert((src_x + width) <= src->GetWidth());
  DebugAssert((src_y + height) <= src->GetHeight());
  DebugAssert(dst_level < dst->GetLevels() && dst_layer < dst->GetLayers());
  DebugAssert((dst_x + width) <= dst->GetMipWidth(dst_level));
  DebugAssert((dst_y + height) <= dst->GetMipHeight(dst_level));
  DebugAssert(!dst->IsMultisampled() && src->IsMultisampled());

  // Only does first level for now..
  DebugAssert(dst_level == 0 && dst_layer == 0);

  const GPUTexture::Format src_format = dst->GetFormat();
  const GPUTexture::Format dst_format = dst->GetFormat();
  id<MTLComputePipelineState> resolve_pipeline = nil;
  if (auto iter = std::find_if(m_resolve_pipelines.begin(), m_resolve_pipelines.end(),
                               [src_format, dst_format](const auto& it) {
                                 return it.first.first == src_format && it.first.second == dst_format;
                               });
      iter != m_resolve_pipelines.end())
  {
    resolve_pipeline = iter->second;
  }
  else
  {
    // Need to compile it.
    @autoreleasepool
    {
      const bool is_depth = GPUTexture::IsDepthFormat(src_format);
      id<MTLFunction> function =
        [GetFunctionFromLibrary(m_shaders, is_depth ? @"depthResolveKernel" : @"colorResolveKernel") autorelease];
      if (function == nil)
        Panic("Failed to get resolve kernel");

      resolve_pipeline = [CreateComputePipeline(function, is_depth ? @"Depth Resolve" : @"Color Resolve") autorelease];
      if (resolve_pipeline != nil)
        [resolve_pipeline retain];
      m_resolve_pipelines.emplace_back(std::make_pair(src_format, dst_format), resolve_pipeline);
    }
  }
  if (resolve_pipeline == nil)
    Panic("Failed to get resolve pipeline");

  if (InRenderPass())
    EndRenderPass();

  s_stats.num_copies++;

  const u32 threadgroupHeight = resolve_pipeline.maxTotalThreadsPerThreadgroup / resolve_pipeline.threadExecutionWidth;
  const MTLSize intrinsicThreadgroupSize = MTLSizeMake(resolve_pipeline.threadExecutionWidth, threadgroupHeight, 1);
  const MTLSize threadgroupsInGrid =
    MTLSizeMake((src->GetWidth() + intrinsicThreadgroupSize.width - 1) / intrinsicThreadgroupSize.width,
                (src->GetHeight() + intrinsicThreadgroupSize.height - 1) / intrinsicThreadgroupSize.height, 1);

  id<MTLComputeCommandEncoder> computeEncoder = [m_render_cmdbuf computeCommandEncoder];
  [computeEncoder setComputePipelineState:resolve_pipeline];
  [computeEncoder setTexture:static_cast<MetalTexture*>(src)->GetMTLTexture() atIndex:0];
  [computeEncoder setTexture:static_cast<MetalTexture*>(dst)->GetMTLTexture() atIndex:1];
  [computeEncoder dispatchThreadgroups:threadgroupsInGrid threadsPerThreadgroup:intrinsicThreadgroupSize];
  [computeEncoder endEncoding];
}

void MetalDevice::ClearRenderTarget(GPUTexture* t, u32 c)
{
  GPUDevice::ClearRenderTarget(t, c);
  if (InRenderPass() && IsRenderTargetBound(t))
    EndRenderPass();
}

void MetalDevice::ClearDepth(GPUTexture* t, float d)
{
  GPUDevice::ClearDepth(t, d);
  if (InRenderPass() && m_current_depth_target == t)
    EndRenderPass();
}

void MetalDevice::InvalidateRenderTarget(GPUTexture* t)
{
  GPUDevice::InvalidateRenderTarget(t);
  if (InRenderPass() && (t->IsRenderTarget() ? IsRenderTargetBound(t) : (m_current_depth_target == t)))
    EndRenderPass();
}

void MetalDevice::CommitClear(MetalTexture* tex)
{
  if (tex->GetState() == GPUTexture::State::Cleared)
  {
    DebugAssert(tex->IsRenderTargetOrDepthStencil());
    tex->SetState(GPUTexture::State::Dirty);

    // TODO: We could combine it with the current render pass.
    if (InRenderPass())
      EndRenderPass();

    @autoreleasepool
    {
      // Allocating here seems a bit sad.
      MTLRenderPassDescriptor* desc = [MTLRenderPassDescriptor renderPassDescriptor];
      desc.renderTargetWidth = tex->GetWidth();
      desc.renderTargetHeight = tex->GetHeight();
      if (tex->IsRenderTarget())
      {
        const auto cc = tex->GetUNormClearColor();
        desc.colorAttachments[0].texture = tex->GetMTLTexture();
        desc.colorAttachments[0].loadAction = MTLLoadActionClear;
        desc.colorAttachments[0].storeAction = MTLStoreActionStore;
        desc.colorAttachments[0].clearColor = MTLClearColorMake(cc[0], cc[1], cc[2], cc[3]);
      }
      else
      {
        desc.depthAttachment.texture = tex->GetMTLTexture();
        desc.depthAttachment.loadAction = MTLLoadActionClear;
        desc.depthAttachment.storeAction = MTLStoreActionStore;
        desc.depthAttachment.clearDepth = tex->GetClearDepth();
      }

      id<MTLRenderCommandEncoder> encoder = [m_render_cmdbuf renderCommandEncoderWithDescriptor:desc];
      [encoder endEncoding];
    }
  }
}

MetalTextureBuffer::MetalTextureBuffer(Format format, u32 size_in_elements) : GPUTextureBuffer(format, size_in_elements)
{
}

MetalTextureBuffer::~MetalTextureBuffer()
{
  if (m_buffer.IsValid())
    MetalDevice::GetInstance().UnbindTextureBuffer(this);
  m_buffer.Destroy();
}

bool MetalTextureBuffer::CreateBuffer(id<MTLDevice> device)
{
  return m_buffer.Create(device, GetSizeInBytes());
}

void* MetalTextureBuffer::Map(u32 required_elements)
{
  const u32 esize = GetElementSize(m_format);
  const u32 req_size = esize * required_elements;
  if (!m_buffer.ReserveMemory(req_size, esize))
  {
    MetalDevice::GetInstance().SubmitCommandBufferAndRestartRenderPass("out of space in texture buffer");
    if (!m_buffer.ReserveMemory(req_size, esize))
      Panic("Failed to allocate texture buffer space.");
  }

  m_current_position = m_buffer.GetCurrentOffset() / esize;
  return m_buffer.GetCurrentHostPointer();
}

void MetalTextureBuffer::Unmap(u32 used_elements)
{
  const u32 size = GetElementSize(m_format) * used_elements;
  GPUDevice::GetStatistics().buffer_streamed += size;
  GPUDevice::GetStatistics().num_uploads++;
  m_buffer.CommitMemory(size);
}

void MetalTextureBuffer::SetDebugName(const std::string_view& name)
{
  @autoreleasepool
  {
    [m_buffer.GetBuffer() setLabel:StringViewToNSString(name)];
  }
}

std::unique_ptr<GPUTextureBuffer> MetalDevice::CreateTextureBuffer(GPUTextureBuffer::Format format,
                                                                   u32 size_in_elements)
{
  std::unique_ptr<MetalTextureBuffer> tb = std::make_unique<MetalTextureBuffer>(format, size_in_elements);
  if (!tb->CreateBuffer(m_device))
    tb.reset();

  return tb;
}

void MetalDevice::PushDebugGroup(const char* name)
{
}

void MetalDevice::PopDebugGroup()
{
}

void MetalDevice::InsertDebugMessage(const char* msg)
{
}

void MetalDevice::MapVertexBuffer(u32 vertex_size, u32 vertex_count, void** map_ptr, u32* map_space,
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

void MetalDevice::UnmapVertexBuffer(u32 vertex_size, u32 vertex_count)
{
  const u32 size = vertex_size * vertex_count;
  s_stats.buffer_streamed += size;
  m_vertex_buffer.CommitMemory(size);
}

void MetalDevice::MapIndexBuffer(u32 index_count, DrawIndex** map_ptr, u32* map_space, u32* map_base_index)
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

void MetalDevice::UnmapIndexBuffer(u32 used_index_count)
{
  const u32 size = sizeof(DrawIndex) * used_index_count;
  s_stats.buffer_streamed += size;
  m_index_buffer.CommitMemory(size);
}

void MetalDevice::PushUniformBuffer(const void* data, u32 data_size)
{
  s_stats.buffer_streamed += data_size;
  void* map = MapUniformBuffer(data_size);
  std::memcpy(map, data, data_size);
  UnmapUniformBuffer(data_size);
}

void* MetalDevice::MapUniformBuffer(u32 size)
{
  const u32 used_space = Common::AlignUpPow2(size, UNIFORM_BUFFER_ALIGNMENT);
  if (!m_uniform_buffer.ReserveMemory(used_space, UNIFORM_BUFFER_ALIGNMENT))
  {
    SubmitCommandBufferAndRestartRenderPass("out of uniform space");
    if (!m_uniform_buffer.ReserveMemory(used_space, UNIFORM_BUFFER_ALIGNMENT))
      Panic("Failed to allocate uniform space.");
  }

  return m_uniform_buffer.GetCurrentHostPointer();
}

void MetalDevice::UnmapUniformBuffer(u32 size)
{
  s_stats.buffer_streamed += size;
  m_current_uniform_buffer_position = m_uniform_buffer.GetCurrentOffset();
  m_uniform_buffer.CommitMemory(size);
  if (InRenderPass())
  {
    [m_render_encoder setVertexBufferOffset:m_current_uniform_buffer_position atIndex:0];
    [m_render_encoder setFragmentBufferOffset:m_current_uniform_buffer_position atIndex:0];
  }
}

void MetalDevice::SetRenderTargets(GPUTexture* const* rts, u32 num_rts, GPUTexture* ds)
{
  bool changed = (m_num_current_render_targets != num_rts || m_current_depth_target != ds);
  bool needs_ds_clear = (ds && ds->IsClearedOrInvalidated());
  bool needs_rt_clear = false;

  m_current_depth_target = static_cast<MetalTexture*>(ds);
  for (u32 i = 0; i < num_rts; i++)
  {
    MetalTexture* const RT = static_cast<MetalTexture*>(rts[i]);
    changed |= m_current_render_targets[i] != RT;
    m_current_render_targets[i] = RT;
    needs_rt_clear |= RT->IsClearedOrInvalidated();
  }
  for (u32 i = num_rts; i < m_num_current_render_targets; i++)
    m_current_render_targets[i] = nullptr;
  m_num_current_render_targets = num_rts;

  if (changed || needs_rt_clear || needs_ds_clear)
  {
    if (InRenderPass())
      EndRenderPass();
  }
}

void MetalDevice::SetPipeline(GPUPipeline* pipeline)
{
  DebugAssert(pipeline);
  if (m_current_pipeline == pipeline)
    return;

  m_current_pipeline = static_cast<MetalPipeline*>(pipeline);
  if (InRenderPass())
  {
    [m_render_encoder setRenderPipelineState:m_current_pipeline->GetPipelineState()];

    if (m_current_depth_state != m_current_pipeline->GetDepthState())
    {
      m_current_depth_state = m_current_pipeline->GetDepthState();
      [m_render_encoder setDepthStencilState:m_current_depth_state];
    }
    if (m_current_cull_mode != m_current_pipeline->GetCullMode())
    {
      m_current_cull_mode = m_current_pipeline->GetCullMode();
      [m_render_encoder setCullMode:m_current_cull_mode];
    }
  }
  else
  {
    // Still need to set depth state before the draw begins.
    m_current_depth_state = m_current_pipeline->GetDepthState();
    m_current_cull_mode = m_current_pipeline->GetCullMode();
  }
}

void MetalDevice::UnbindPipeline(MetalPipeline* pl)
{
  if (m_current_pipeline != pl)
    return;

  m_current_pipeline = nullptr;
  m_current_depth_state = nil;
}

void MetalDevice::SetTextureSampler(u32 slot, GPUTexture* texture, GPUSampler* sampler)
{
  DebugAssert(slot < MAX_TEXTURE_SAMPLERS);

  id<MTLTexture> T = texture ? static_cast<MetalTexture*>(texture)->GetMTLTexture() : nil;
  if (texture)
  {
    CommitClear(static_cast<MetalTexture*>(texture));
    static_cast<MetalTexture*>(texture)->SetUseFenceCounter(m_current_fence_counter);
  }

  if (m_current_textures[slot] != T)
  {
    m_current_textures[slot] = T;
    if (InRenderPass())
      [m_render_encoder setFragmentTexture:T atIndex:slot];
  }

  id<MTLSamplerState> S = sampler ? static_cast<MetalSampler*>(sampler)->GetSamplerState() : nil;
  if (m_current_samplers[slot] != S)
  {
    m_current_samplers[slot] = S;
    if (InRenderPass())
      [m_render_encoder setFragmentSamplerState:S atIndex:slot];
  }
}

void MetalDevice::SetTextureBuffer(u32 slot, GPUTextureBuffer* buffer)
{
  id<MTLBuffer> B = buffer ? static_cast<MetalTextureBuffer*>(buffer)->GetMTLBuffer() : nil;
  if (m_current_ssbo == B)
    return;

  m_current_ssbo = B;
  if (InRenderPass())
    [m_render_encoder setFragmentBuffer:B offset:0 atIndex:1];
}

void MetalDevice::UnbindTexture(MetalTexture* tex)
{
  const id<MTLTexture> T = tex->GetMTLTexture();
  for (u32 i = 0; i < MAX_TEXTURE_SAMPLERS; i++)
  {
    if (m_current_textures[i] == T)
    {
      m_current_textures[i] = nil;
      if (InRenderPass())
        [m_render_encoder setFragmentTexture:nil atIndex:i];
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
  }
  else if (tex->IsDepthStencil())
  {
    if (m_current_depth_target == tex)
    {
      Log_WarningPrint("Unbinding current DS");
      SetRenderTargets(nullptr, 0, nullptr);
    }
  }
}

void MetalDevice::UnbindTextureBuffer(MetalTextureBuffer* buf)
{
  if (m_current_ssbo != buf->GetMTLBuffer())
    return;

  m_current_ssbo = nil;
  if (InRenderPass())
    [m_render_encoder setFragmentBuffer:nil offset:0 atIndex:1];
}

void MetalDevice::SetViewport(s32 x, s32 y, s32 width, s32 height)
{
  const Common::Rectangle<s32> new_vp = Common::Rectangle<s32>::FromExtents(x, y, width, height);
  if (new_vp == m_current_viewport)
    return;

  m_current_viewport = new_vp;
  if (InRenderPass())
    SetViewportInRenderEncoder();
}

void MetalDevice::SetScissor(s32 x, s32 y, s32 width, s32 height)
{
  const Common::Rectangle<s32> new_sr = Common::Rectangle<s32>::FromExtents(x, y, width, height);
  if (new_sr == m_current_scissor)
    return;

  m_current_scissor = new_sr;
  if (InRenderPass())
    SetScissorInRenderEncoder();
}

void MetalDevice::BeginRenderPass()
{
  DebugAssert(m_render_encoder == nil);

  // Inline writes :(
  if (m_inline_upload_encoder != nil)
  {
    [m_inline_upload_encoder endEncoding];
    [m_inline_upload_encoder release];
    m_inline_upload_encoder = nil;
  }

  s_stats.num_render_passes++;

  @autoreleasepool
  {
    MTLRenderPassDescriptor* desc = [MTLRenderPassDescriptor renderPassDescriptor];
    if (m_num_current_render_targets == 0 && !m_current_depth_target)
    {
      // Rendering to view, but we got interrupted...
      desc.colorAttachments[0].texture = [m_layer_drawable texture];
      desc.colorAttachments[0].loadAction = MTLLoadActionLoad;
    }
    else
    {
      for (u32 i = 0; i < m_num_current_render_targets; i++)
      {
        MetalTexture* const RT = m_current_render_targets[i];
        desc.colorAttachments[i].texture = RT->GetMTLTexture();
        desc.colorAttachments[i].storeAction = MTLStoreActionStore;
        RT->SetUseFenceCounter(m_current_fence_counter);

        switch (RT->GetState())
        {
          case GPUTexture::State::Cleared:
          {
            const auto clear_color = RT->GetUNormClearColor();
            desc.colorAttachments[i].loadAction = MTLLoadActionClear;
            desc.colorAttachments[i].clearColor =
              MTLClearColorMake(clear_color[0], clear_color[1], clear_color[2], clear_color[3]);
            RT->SetState(GPUTexture::State::Dirty);
          }
          break;

          case GPUTexture::State::Invalidated:
          {
            desc.colorAttachments[i].loadAction = MTLLoadActionDontCare;
            RT->SetState(GPUTexture::State::Dirty);
          }
          break;

          case GPUTexture::State::Dirty:
          {
            desc.colorAttachments[i].loadAction = MTLLoadActionLoad;
          }
          break;

          default:
            UnreachableCode();
            break;
        }
      }

      if (MetalTexture* DS = m_current_depth_target)
      {
        desc.depthAttachment.texture = m_current_depth_target->GetMTLTexture();
        desc.depthAttachment.storeAction = MTLStoreActionStore;
        DS->SetUseFenceCounter(m_current_fence_counter);

        switch (DS->GetState())
        {
          case GPUTexture::State::Cleared:
          {
            desc.depthAttachment.loadAction = MTLLoadActionClear;
            desc.depthAttachment.clearDepth = DS->GetClearDepth();
            DS->SetState(GPUTexture::State::Dirty);
          }
          break;

          case GPUTexture::State::Invalidated:
          {
            desc.depthAttachment.loadAction = MTLLoadActionDontCare;
            DS->SetState(GPUTexture::State::Dirty);
          }
          break;

          case GPUTexture::State::Dirty:
          {
            desc.depthAttachment.loadAction = MTLLoadActionLoad;
          }
          break;

          default:
            UnreachableCode();
            break;
        }
      }
    }

    m_render_encoder = [[m_render_cmdbuf renderCommandEncoderWithDescriptor:desc] retain];
    SetInitialEncoderState();
  }
}

void MetalDevice::EndRenderPass()
{
  DebugAssert(InRenderPass() && !IsInlineUploading());
  [m_render_encoder endEncoding];
  [m_render_encoder release];
  m_render_encoder = nil;
}

void MetalDevice::EndInlineUploading()
{
  DebugAssert(IsInlineUploading() && !InRenderPass());
  [m_inline_upload_encoder endEncoding];
  [m_inline_upload_encoder release];
  m_inline_upload_encoder = nil;
}

void MetalDevice::EndAnyEncoding()
{
  if (InRenderPass())
    EndRenderPass();
  else if (IsInlineUploading())
    EndInlineUploading();
}

void MetalDevice::SetInitialEncoderState()
{
  // Set initial state.
  // TODO: avoid uniform set here? it's probably going to get changed...
  // Might be better off just deferring all the init until the first draw...
  [m_render_encoder setVertexBuffer:m_uniform_buffer.GetBuffer() offset:m_current_uniform_buffer_position atIndex:0];
  [m_render_encoder setFragmentBuffer:m_uniform_buffer.GetBuffer() offset:m_current_uniform_buffer_position atIndex:0];
  [m_render_encoder setVertexBuffer:m_vertex_buffer.GetBuffer() offset:0 atIndex:1];
  [m_render_encoder setCullMode:m_current_cull_mode];
  if (m_current_depth_state != nil)
    [m_render_encoder setDepthStencilState:m_current_depth_state];
  if (m_current_pipeline != nil)
    [m_render_encoder setRenderPipelineState:m_current_pipeline->GetPipelineState()];
  [m_render_encoder setFragmentTextures:m_current_textures.data() withRange:NSMakeRange(0, MAX_TEXTURE_SAMPLERS)];
  [m_render_encoder setFragmentSamplerStates:m_current_samplers.data() withRange:NSMakeRange(0, MAX_TEXTURE_SAMPLERS)];
  if (m_current_ssbo)
    [m_render_encoder setFragmentBuffer:m_current_ssbo offset:0 atIndex:1];
  SetViewportInRenderEncoder();
  SetScissorInRenderEncoder();
}

void MetalDevice::SetViewportInRenderEncoder()
{
  const Common::Rectangle<s32> rc = ClampToFramebufferSize(m_current_viewport);
  [m_render_encoder
    setViewport:(MTLViewport){static_cast<double>(rc.left), static_cast<double>(rc.top),
                              static_cast<double>(rc.GetWidth()), static_cast<double>(rc.GetHeight()), 0.0, 1.0}];
}

void MetalDevice::SetScissorInRenderEncoder()
{
  const Common::Rectangle<s32> rc = ClampToFramebufferSize(m_current_scissor);
  [m_render_encoder
    setScissorRect:(MTLScissorRect){static_cast<NSUInteger>(rc.left), static_cast<NSUInteger>(rc.top),
                                    static_cast<NSUInteger>(rc.GetWidth()), static_cast<NSUInteger>(rc.GetHeight())}];
}

Common::Rectangle<s32> MetalDevice::ClampToFramebufferSize(const Common::Rectangle<s32>& rc) const
{
  const MetalTexture* rt_or_ds =
    (m_num_current_render_targets > 0) ? m_current_render_targets[0] : m_current_depth_target;
  const s32 clamp_width = rt_or_ds ? rt_or_ds->GetWidth() : m_window_info.surface_width;
  const s32 clamp_height = rt_or_ds ? rt_or_ds->GetHeight() : m_window_info.surface_height;
  return rc.ClampedSize(clamp_width, clamp_height);
}

void MetalDevice::PreDrawCheck()
{
  if (!InRenderPass())
    BeginRenderPass();
}

void MetalDevice::Draw(u32 vertex_count, u32 base_vertex)
{
  PreDrawCheck();
  s_stats.num_draws++;
  [m_render_encoder drawPrimitives:m_current_pipeline->GetPrimitive() vertexStart:base_vertex vertexCount:vertex_count];
}

void MetalDevice::DrawIndexed(u32 index_count, u32 base_index, u32 base_vertex)
{
  PreDrawCheck();

  s_stats.num_draws++;

  const u32 index_offset = base_index * sizeof(u16);
  [m_render_encoder drawIndexedPrimitives:m_current_pipeline->GetPrimitive()
                               indexCount:index_count
                                indexType:MTLIndexTypeUInt16
                              indexBuffer:m_index_buffer.GetBuffer()
                        indexBufferOffset:index_offset
                            instanceCount:1
                               baseVertex:base_vertex
                             baseInstance:0];
}

id<MTLBlitCommandEncoder> MetalDevice::GetBlitEncoder(bool is_inline)
{
  @autoreleasepool
  {
    if (!is_inline)
    {
      if (!m_upload_cmdbuf)
      {
        m_upload_cmdbuf = [[m_queue commandBufferWithUnretainedReferences] retain];
        m_upload_encoder = [[m_upload_cmdbuf blitCommandEncoder] retain];
        [m_upload_encoder setLabel:@"Upload Encoder"];
      }
      return m_upload_encoder;
    }

    // Interleaved with draws.
    if (m_inline_upload_encoder != nil)
      return m_inline_upload_encoder;

    if (InRenderPass())
      EndRenderPass();
    m_inline_upload_encoder = [[m_render_cmdbuf blitCommandEncoder] retain];
    return m_inline_upload_encoder;
  }
}

bool MetalDevice::BeginPresent(bool skip_present)
{
  @autoreleasepool
  {
    if (skip_present)
      return false;

    if (m_layer == nil)
    {
      TrimTexturePool();
      return false;
    }

    EndAnyEncoding();

    m_layer_drawable = [[m_layer nextDrawable] retain];
    if (m_layer_drawable == nil)
    {
      TrimTexturePool();
      return false;
    }

    SetViewportAndScissor(0, 0, m_window_info.surface_width, m_window_info.surface_height);

    // Set up rendering to layer.
    id<MTLTexture> layer_texture = [m_layer_drawable texture];
    m_layer_pass_desc.colorAttachments[0].texture = layer_texture;
    m_layer_pass_desc.colorAttachments[0].loadAction = MTLLoadActionClear;
    m_render_encoder = [[m_render_cmdbuf renderCommandEncoderWithDescriptor:m_layer_pass_desc] retain];
    s_stats.num_render_passes++;
    std::memset(m_current_render_targets.data(), 0, sizeof(m_current_render_targets));
    m_num_current_render_targets = 0;
    m_current_depth_target = nullptr;
    m_current_pipeline = nullptr;
    m_current_depth_state = nil;
    SetInitialEncoderState();
    return true;
  }
}

void MetalDevice::EndPresent()
{
  DebugAssert(m_num_current_render_targets == 0 && !m_current_depth_target);
  EndAnyEncoding();

  [m_render_cmdbuf presentDrawable:m_layer_drawable];
  DeferRelease(m_layer_drawable);
  m_layer_drawable = nil;
  SubmitCommandBuffer();
  TrimTexturePool();
}

void MetalDevice::CreateCommandBuffer()
{
  @autoreleasepool
  {
    DebugAssert(m_render_cmdbuf == nil);
    const u64 fence_counter = ++m_current_fence_counter;
    m_render_cmdbuf = [[m_queue commandBufferWithUnretainedReferences] retain];
    [m_render_cmdbuf addCompletedHandler:[this, fence_counter](id<MTLCommandBuffer> buffer) {
      CommandBufferCompletedOffThread(buffer, fence_counter);
    }];
  }

  CleanupObjects();
}

void MetalDevice::CommandBufferCompletedOffThread(id<MTLCommandBuffer> buffer, u64 fence_counter)
{
  std::unique_lock lock(m_fence_mutex);
  m_completed_fence_counter.store(std::max(m_completed_fence_counter.load(std::memory_order_acquire), fence_counter),
                                  std::memory_order_release);

  if (m_gpu_timing_enabled)
  {
    const double begin = std::max(m_last_gpu_time_end, [buffer GPUStartTime]);
    const double end = [buffer GPUEndTime];
    if (end > begin)
    {
      m_accumulated_gpu_time += end - begin;
      m_last_gpu_time_end = end;
    }
  }
}

void MetalDevice::SubmitCommandBuffer(bool wait_for_completion)
{
  if (m_upload_cmdbuf != nil)
  {
    [m_upload_encoder endEncoding];
    [m_upload_encoder release];
    m_upload_encoder = nil;
    [m_upload_cmdbuf commit];
    [m_upload_cmdbuf release];
    m_upload_cmdbuf = nil;
  }

  if (m_render_cmdbuf != nil)
  {
    if (InRenderPass())
      EndRenderPass();
    else if (IsInlineUploading())
      EndInlineUploading();

    [m_render_cmdbuf commit];

    if (wait_for_completion)
      [m_render_cmdbuf waitUntilCompleted];

    [m_render_cmdbuf release];
    m_render_cmdbuf = nil;
  }

  CreateCommandBuffer();
}

void MetalDevice::SubmitCommandBufferAndRestartRenderPass(const char* reason)
{
  Log_DevPrintf("Submitting command buffer and restarting render pass due to %s", reason);

  const bool in_render_pass = InRenderPass();
  SubmitCommandBuffer();
  if (in_render_pass)
    BeginRenderPass();
}

void MetalDevice::WaitForFenceCounter(u64 counter)
{
  if (m_completed_fence_counter.load(std::memory_order_relaxed) >= counter)
    return;

  // TODO: There has to be a better way to do this..
  std::unique_lock lock(m_fence_mutex);
  while (m_completed_fence_counter.load(std::memory_order_acquire) < counter)
  {
    lock.unlock();
    pthread_yield_np();
    lock.lock();
  }

  CleanupObjects();
}

void MetalDevice::WaitForPreviousCommandBuffers()
{
  // Early init?
  if (m_current_fence_counter == 0)
    return;

  WaitForFenceCounter(m_current_fence_counter - 1);
}

void MetalDevice::CleanupObjects()
{
  const u64 counter = m_completed_fence_counter.load(std::memory_order_acquire);
  while (m_cleanup_objects.size() > 0 && m_cleanup_objects.front().first <= counter)
  {
    [m_cleanup_objects.front().second release];
    m_cleanup_objects.pop_front();
  }
}

void MetalDevice::DeferRelease(id obj)
{
  MetalDevice& dev = GetInstance();
  dev.m_cleanup_objects.emplace_back(dev.m_current_fence_counter, obj);
}

void MetalDevice::DeferRelease(u64 fence_counter, id obj)
{
  MetalDevice& dev = GetInstance();
  dev.m_cleanup_objects.emplace_back(fence_counter, obj);
}

std::unique_ptr<GPUDevice> GPUDevice::WrapNewMetalDevice()
{
  return std::unique_ptr<GPUDevice>(new MetalDevice());
}

GPUDevice::AdapterAndModeList GPUDevice::WrapGetMetalAdapterAndModeList()
{
  return MetalDevice::StaticGetAdapterAndModeList();
}
