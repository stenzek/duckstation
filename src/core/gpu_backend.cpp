// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gpu_backend.h"
#include "gpu.h"
#include "gpu_shadergen.h"
#include "gpu_sw_rasterizer.h"
#include "gpu_thread.h"
#include "host.h"
#include "performance_counters.h"
#include "save_state_version.h"
#include "settings.h"
#include "system.h"
#include "system_private.h"

#include "util/gpu_device.h"
#include "util/image.h"
#include "util/imgui_manager.h"
#include "util/media_capture.h"
#include "util/postprocessing.h"
#include "util/state_wrapper.h"

#include "common/align.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/gsvector_formatter.h"
#include "common/log.h"
#include "common/path.h"
#include "common/small_string.h"
#include "common/string_util.h"
#include "common/timer.h"

#include "IconsEmoji.h"
#include "IconsFontAwesome5.h"
#include "fmt/format.h"

#include <numbers>
#include <thread>

LOG_CHANNEL(GPU);

namespace {

struct Counters
{
  u32 num_reads;
  u32 num_writes;
  u32 num_copies;
  u32 num_vertices;
  u32 num_primitives;
};

struct Stats : Counters
{
  size_t host_buffer_streamed;
  u32 host_num_draws;
  u32 host_num_barriers;
  u32 host_num_render_passes;
  u32 host_num_copies;
  u32 host_num_downloads;
  u32 host_num_uploads;
};

struct ALIGN_TO_CACHE_LINE CPUThreadState
{
  std::atomic<u32> queued_frames;
  std::atomic_bool waiting_for_gpu_thread;
  Threading::KernelSemaphore gpu_thread_wait;
};

} // namespace

static bool CompressAndWriteTextureToFile(u32 width, u32 height, std::string path, FileSystem::ManagedCFilePtr fp,
                                          u8 quality, bool clear_alpha, bool flip_y, Image image, std::string osd_key);

static constexpr GPUTexture::Format DISPLAY_INTERNAL_POSTFX_FORMAT = GPUTexture::Format::RGBA8;

static Counters s_counters = {};
static Stats s_stats = {};
static CPUThreadState s_cpu_thread_state = {};

GPUBackend::GPUBackend()
{
  GPU_SW_Rasterizer::SelectImplementation();
  ResetStatistics();

  // Should be zero.
  Assert(s_cpu_thread_state.queued_frames.load(std::memory_order_acquire) == 0);
  Assert(!s_cpu_thread_state.waiting_for_gpu_thread.load(std::memory_order_acquire));
}

GPUBackend::~GPUBackend()
{
  DestroyDeinterlaceTextures();
  g_gpu_device->RecycleTexture(std::move(m_chroma_smoothing_texture));
}

bool GPUBackend::Initialize(bool clear_vram, Error* error)
{
  if (!CompileDisplayPipelines(true, true, g_gpu_settings.display_24bit_chroma_smoothing, error))
    return false;

  return true;
}

void GPUBackend::UpdateSettings(const Settings& old_settings)
{
  FlushRender();

  if (g_gpu_settings.display_show_gpu_stats != old_settings.display_show_gpu_stats)
    GPUBackend::ResetStatistics();

  if (g_gpu_settings.display_scaling != old_settings.display_scaling ||
      g_gpu_settings.display_deinterlacing_mode != old_settings.display_deinterlacing_mode ||
      g_gpu_settings.display_24bit_chroma_smoothing != old_settings.display_24bit_chroma_smoothing)
  {
    // Toss buffers on mode change.
    if (g_gpu_settings.display_deinterlacing_mode != old_settings.display_deinterlacing_mode)
      DestroyDeinterlaceTextures();

    if (!CompileDisplayPipelines(
          g_gpu_settings.display_scaling != old_settings.display_scaling,
          g_gpu_settings.display_deinterlacing_mode != old_settings.display_deinterlacing_mode,
          g_gpu_settings.display_24bit_chroma_smoothing != old_settings.display_24bit_chroma_smoothing, nullptr))
    {
      Panic("Failed to compile display pipeline on settings change.");
    }
  }
}

void GPUBackend::UpdateResolutionScale()
{
}

u32 GPUBackend::GetResolutionScale() const
{
  return 1u;
}

void GPUBackend::RestoreDeviceContext()
{
}

GPUThreadCommand* GPUBackend::NewClearVRAMCommand()
{
  return static_cast<GPUThreadCommand*>(
    GPUThread::AllocateCommand(GPUBackendCommandType::ClearVRAM, sizeof(GPUThreadCommand)));
}

GPUThreadCommand* GPUBackend::NewClearDisplayCommand()
{
  return static_cast<GPUThreadCommand*>(
    GPUThread::AllocateCommand(GPUBackendCommandType::ClearDisplay, sizeof(GPUThreadCommand)));
}

GPUBackendUpdateDisplayCommand* GPUBackend::NewUpdateDisplayCommand()
{
  return static_cast<GPUBackendUpdateDisplayCommand*>(
    GPUThread::AllocateCommand(GPUBackendCommandType::UpdateDisplay, sizeof(GPUBackendUpdateDisplayCommand)));
}

GPUBackendSubmitFrameCommand* GPUBackend::NewSubmitFrameCommand()
{
  return static_cast<GPUBackendSubmitFrameCommand*>(
    GPUThread::AllocateCommand(GPUBackendCommandType::SubmitFrame, sizeof(GPUBackendUpdateDisplayCommand)));
}

GPUThreadCommand* GPUBackend::NewClearCacheCommand()
{
  return static_cast<GPUThreadCommand*>(
    GPUThread::AllocateCommand(GPUBackendCommandType::ClearCache, sizeof(GPUThreadCommand)));
}

GPUThreadCommand* GPUBackend::NewBufferSwappedCommand()
{
  return static_cast<GPUThreadCommand*>(
    GPUThread::AllocateCommand(GPUBackendCommandType::BufferSwapped, sizeof(GPUThreadCommand)));
}

GPUThreadCommand* GPUBackend::NewUpdateResolutionScaleCommand()
{
  return static_cast<GPUThreadCommand*>(
    GPUThread::AllocateCommand(GPUBackendCommandType::UpdateResolutionScale, sizeof(GPUThreadCommand)));
}

GPUBackendReadVRAMCommand* GPUBackend::NewReadVRAMCommand()
{
  return static_cast<GPUBackendReadVRAMCommand*>(
    GPUThread::AllocateCommand(GPUBackendCommandType::ReadVRAM, sizeof(GPUBackendReadVRAMCommand)));
}

GPUBackendFillVRAMCommand* GPUBackend::NewFillVRAMCommand()
{
  return static_cast<GPUBackendFillVRAMCommand*>(
    GPUThread::AllocateCommand(GPUBackendCommandType::FillVRAM, sizeof(GPUBackendFillVRAMCommand)));
}

GPUBackendUpdateVRAMCommand* GPUBackend::NewUpdateVRAMCommand(u32 num_words)
{
  const u32 size = sizeof(GPUBackendUpdateVRAMCommand) + (num_words * sizeof(u16));
  GPUBackendUpdateVRAMCommand* cmd =
    static_cast<GPUBackendUpdateVRAMCommand*>(GPUThread::AllocateCommand(GPUBackendCommandType::UpdateVRAM, size));
  return cmd;
}

GPUBackendCopyVRAMCommand* GPUBackend::NewCopyVRAMCommand()
{
  return static_cast<GPUBackendCopyVRAMCommand*>(
    GPUThread::AllocateCommand(GPUBackendCommandType::CopyVRAM, sizeof(GPUBackendCopyVRAMCommand)));
}

GPUBackendSetDrawingAreaCommand* GPUBackend::NewSetDrawingAreaCommand()
{
  return static_cast<GPUBackendSetDrawingAreaCommand*>(
    GPUThread::AllocateCommand(GPUBackendCommandType::SetDrawingArea, sizeof(GPUBackendSetDrawingAreaCommand)));
}

GPUBackendUpdateCLUTCommand* GPUBackend::NewUpdateCLUTCommand()
{
  return static_cast<GPUBackendUpdateCLUTCommand*>(
    GPUThread::AllocateCommand(GPUBackendCommandType::UpdateCLUT, sizeof(GPUBackendUpdateCLUTCommand)));
}

GPUBackendDrawPolygonCommand* GPUBackend::NewDrawPolygonCommand(u32 num_vertices)
{
  const u32 size = sizeof(GPUBackendDrawPolygonCommand) + (num_vertices * sizeof(GPUBackendDrawPolygonCommand::Vertex));
  GPUBackendDrawPolygonCommand* cmd =
    static_cast<GPUBackendDrawPolygonCommand*>(GPUThread::AllocateCommand(GPUBackendCommandType::DrawPolygon, size));
  cmd->num_vertices = Truncate8(num_vertices);
  return cmd;
}

GPUBackendDrawPrecisePolygonCommand* GPUBackend::NewDrawPrecisePolygonCommand(u32 num_vertices)
{
  const u32 size =
    sizeof(GPUBackendDrawPrecisePolygonCommand) + (num_vertices * sizeof(GPUBackendDrawPrecisePolygonCommand::Vertex));
  GPUBackendDrawPrecisePolygonCommand* cmd = static_cast<GPUBackendDrawPrecisePolygonCommand*>(
    GPUThread::AllocateCommand(GPUBackendCommandType::DrawPrecisePolygon, size));
  cmd->num_vertices = Truncate8(num_vertices);
  return cmd;
}

GPUBackendDrawRectangleCommand* GPUBackend::NewDrawRectangleCommand()
{
  return static_cast<GPUBackendDrawRectangleCommand*>(
    GPUThread::AllocateCommand(GPUBackendCommandType::DrawRectangle, sizeof(GPUBackendDrawRectangleCommand)));
}

GPUBackendDrawLineCommand* GPUBackend::NewDrawLineCommand(u32 num_vertices)
{
  const u32 size = sizeof(GPUBackendDrawLineCommand) + (num_vertices * sizeof(GPUBackendDrawLineCommand::Vertex));
  GPUBackendDrawLineCommand* cmd =
    static_cast<GPUBackendDrawLineCommand*>(GPUThread::AllocateCommand(GPUBackendCommandType::DrawLine, size));
  cmd->num_vertices = Truncate16(num_vertices);
  return cmd;
}

void GPUBackend::PushCommand(GPUThreadCommand* cmd)
{
  GPUThread::PushCommand(cmd);
}

void GPUBackend::PushCommandAndWakeThread(GPUThreadCommand* cmd)
{
  GPUThread::PushCommandAndWakeThread(cmd);
}

void GPUBackend::PushCommandAndSync(GPUThreadCommand* cmd, bool spin)
{
  GPUThread::PushCommandAndSync(cmd, spin);
}

void GPUBackend::SyncGPUThread(bool spin)
{
  GPUThread::SyncGPUThread(spin);
}

bool GPUBackend::IsUsingHardwareBackend()
{
  return (GPUThread::GetRequestedRenderer().value_or(GPURenderer::Software) != GPURenderer::Software);
}

bool GPUBackend::BeginQueueFrame()
{
  const u32 queued_frames = s_cpu_thread_state.queued_frames.fetch_add(1, std::memory_order_acq_rel) + 1;
  if (queued_frames < g_settings.gpu_max_queued_frames)
    return false;

  DEV_LOG("<-- {} queued frames, {} max, blocking CPU thread", queued_frames, g_settings.gpu_max_queued_frames);
  s_cpu_thread_state.waiting_for_gpu_thread.store(true, std::memory_order_release);
  return true;
}

void GPUBackend::WaitForOneQueuedFrame()
{
  // Inbetween this and the post call, we may have finished the frame. Check.
  if (s_cpu_thread_state.queued_frames.load(std::memory_order_acquire) < g_settings.gpu_max_queued_frames)
  {
    // It's possible that the GPU thread has already signaled the semaphore.
    // If so, then we still need to drain it, otherwise waits in the future will return prematurely.
    bool expected = true;
    if (s_cpu_thread_state.waiting_for_gpu_thread.compare_exchange_strong(expected, false, std::memory_order_acq_rel,
                                                                          std::memory_order_relaxed))
    {
      return;
    }
  }

  s_cpu_thread_state.gpu_thread_wait.Wait();

  // Sanity check: queued frames should be in range now. If they're not, we fucked up the semaphore.
  Assert(s_cpu_thread_state.queued_frames.load(std::memory_order_acquire) < g_settings.gpu_max_queued_frames);
}

bool GPUBackend::AllocateMemorySaveStates(std::span<System::MemorySaveState> states, Error* error)
{
  bool result;
  GPUBackendAllocateMemoryStatesCommand* cmd =
    static_cast<GPUBackendAllocateMemoryStatesCommand*>(GPUThread::AllocateCommand(
      GPUBackendCommandType::AllocateMemoryStates, sizeof(GPUBackendAllocateMemoryStatesCommand)));
  cmd->memory_save_state_count = states.size();
  cmd->memory_save_states = states.data();
  cmd->out_error = error;
  cmd->out_result = &result;
  PushCommandAndSync(cmd, false);
  return result;
}

void GPUBackend::HandleAllocateMemorySaveStatesCommand(const GPUBackendAllocateMemoryStatesCommand* cmd)
{
  for (size_t i = 0; i < cmd->memory_save_state_count; i++)
  {
    if (!AllocateMemorySaveState(cmd->memory_save_states[i], cmd->out_error))
    {
      // Free anything that was allocated.
      for (size_t j = 0; j <= i; i++)
      {
        cmd->memory_save_states[j].state_data.deallocate();
        cmd->memory_save_states[j].vram_texture.reset();
        *cmd->out_result = false;
        return;
      }
    }
  }

  *cmd->out_result = true;
}

bool GPUBackend::RenderScreenshotToBuffer(u32 width, u32 height, bool postfx, Image* out_image)
{
  bool result;

  GPUThreadRenderScreenshotToBufferCommand* cmd =
    static_cast<GPUThreadRenderScreenshotToBufferCommand*>(GPUThread::AllocateCommand(
      GPUBackendCommandType::RenderScreenshotToBuffer, sizeof(GPUThreadRenderScreenshotToBufferCommand)));
  cmd->width = width;
  cmd->height = height;
  cmd->out_image = out_image;
  cmd->out_result = &result;
  cmd->postfx = postfx;
  PushCommandAndSync(cmd, false);

  return result;
}

void GPUBackend::HandleCommand(const GPUThreadCommand* cmd)
{
  switch (cmd->type)
  {
    case GPUBackendCommandType::ClearVRAM:
    {
      ClearVRAM();
    }
    break;

    case GPUBackendCommandType::LoadState:
    {
      LoadState(static_cast<const GPUBackendLoadStateCommand*>(cmd));
    }
    break;

    case GPUBackendCommandType::LoadMemoryState:
    {
      System::MemorySaveState& mss = *static_cast<const GPUBackendDoMemoryStateCommand*>(cmd)->memory_save_state;
      StateWrapper sw(mss.gpu_state_data.span(mss.gpu_state_size), StateWrapper::Mode::Read, SAVE_STATE_VERSION);
      DoMemoryState(sw, mss);
    }
    break;

    case GPUBackendCommandType::SaveMemoryState:
    {
      System::MemorySaveState& mss = *static_cast<const GPUBackendDoMemoryStateCommand*>(cmd)->memory_save_state;
      StateWrapper sw(mss.gpu_state_data.span(), StateWrapper::Mode::Write, SAVE_STATE_VERSION);
      DoMemoryState(sw, mss);
      mss.gpu_state_size = static_cast<u32>(sw.GetPosition());
    }
    break;

    case GPUBackendCommandType::AllocateMemoryStates:
    {
      HandleAllocateMemorySaveStatesCommand(static_cast<const GPUBackendAllocateMemoryStatesCommand*>(cmd));
    }
    break;

    case GPUBackendCommandType::ClearDisplay:
    {
      ClearDisplay();
    }
    break;

    case GPUBackendCommandType::UpdateDisplay:
    {
      HandleUpdateDisplayCommand(static_cast<const GPUBackendUpdateDisplayCommand*>(cmd));
    }
    break;

    case GPUBackendCommandType::SubmitFrame:
    {
      HandleSubmitFrameCommand(&static_cast<const GPUBackendSubmitFrameCommand*>(cmd)->frame);
    }
    break;

    case GPUBackendCommandType::ClearCache:
    {
      ClearCache();
    }
    break;

    case GPUBackendCommandType::BufferSwapped:
    {
      OnBufferSwapped();
    }
    break;

    case GPUBackendCommandType::UpdateResolutionScale:
    {
      UpdateResolutionScale();
    }
    break;

    case GPUBackendCommandType::RenderScreenshotToBuffer:
    {
      HandleRenderScreenshotToBuffer(static_cast<const GPUThreadRenderScreenshotToBufferCommand*>(cmd));
    }
    break;

    case GPUBackendCommandType::RenderScreenshotToFile:
    {
      HandleRenderScreenshotToFile(static_cast<const GPUThreadRenderScreenshotToFileCommand*>(cmd));
    }
    break;

    case GPUBackendCommandType::ReadVRAM:
    {
      const GPUBackendReadVRAMCommand* ccmd = static_cast<const GPUBackendReadVRAMCommand*>(cmd);
      s_counters.num_reads++;
      ReadVRAM(ZeroExtend32(ccmd->x), ZeroExtend32(ccmd->y), ZeroExtend32(ccmd->width), ZeroExtend32(ccmd->height));
    }
    break;

    case GPUBackendCommandType::FillVRAM:
    {
      const GPUBackendFillVRAMCommand* ccmd = static_cast<const GPUBackendFillVRAMCommand*>(cmd);
      FillVRAM(ZeroExtend32(ccmd->x), ZeroExtend32(ccmd->y), ZeroExtend32(ccmd->width), ZeroExtend32(ccmd->height),
               ccmd->color, ccmd->params);
    }
    break;

    case GPUBackendCommandType::UpdateVRAM:
    {
      const GPUBackendUpdateVRAMCommand* ccmd = static_cast<const GPUBackendUpdateVRAMCommand*>(cmd);
      s_counters.num_writes++;
      UpdateVRAM(ZeroExtend32(ccmd->x), ZeroExtend32(ccmd->y), ZeroExtend32(ccmd->width), ZeroExtend32(ccmd->height),
                 ccmd->data, ccmd->params);
    }
    break;

    case GPUBackendCommandType::CopyVRAM:
    {
      const GPUBackendCopyVRAMCommand* ccmd = static_cast<const GPUBackendCopyVRAMCommand*>(cmd);
      s_counters.num_copies++;
      CopyVRAM(ZeroExtend32(ccmd->src_x), ZeroExtend32(ccmd->src_y), ZeroExtend32(ccmd->dst_x),
               ZeroExtend32(ccmd->dst_y), ZeroExtend32(ccmd->width), ZeroExtend32(ccmd->height), ccmd->params);
    }
    break;

    case GPUBackendCommandType::SetDrawingArea:
    {
      FlushRender();
      const GPUBackendSetDrawingAreaCommand* ccmd = static_cast<const GPUBackendSetDrawingAreaCommand*>(cmd);
      GPU_SW_Rasterizer::g_drawing_area = ccmd->new_area;
      DrawingAreaChanged();
    }
    break;

    case GPUBackendCommandType::UpdateCLUT:
    {
      const GPUBackendUpdateCLUTCommand* ccmd = static_cast<const GPUBackendUpdateCLUTCommand*>(cmd);
      UpdateCLUT(ccmd->reg, ccmd->clut_is_8bit);
    }
    break;

    case GPUBackendCommandType::DrawPolygon:
    {
      const GPUBackendDrawPolygonCommand* ccmd = static_cast<const GPUBackendDrawPolygonCommand*>(cmd);
      s_counters.num_vertices += ccmd->num_vertices;
      s_counters.num_primitives++;
      DrawPolygon(ccmd);
    }
    break;

    case GPUBackendCommandType::DrawPrecisePolygon:
    {
      const GPUBackendDrawPolygonCommand* ccmd = static_cast<const GPUBackendDrawPolygonCommand*>(cmd);
      s_counters.num_vertices += ccmd->num_vertices;
      s_counters.num_primitives++;
      DrawPrecisePolygon(static_cast<const GPUBackendDrawPrecisePolygonCommand*>(cmd));
    }
    break;

    case GPUBackendCommandType::DrawRectangle:
    {
      const GPUBackendDrawRectangleCommand* ccmd = static_cast<const GPUBackendDrawRectangleCommand*>(cmd);
      s_counters.num_vertices++;
      s_counters.num_primitives++;
      DrawSprite(ccmd);
    }
    break;

    case GPUBackendCommandType::DrawLine:
    {
      const GPUBackendDrawLineCommand* ccmd = static_cast<const GPUBackendDrawLineCommand*>(cmd);
      s_counters.num_vertices += ccmd->num_vertices;
      s_counters.num_primitives += ccmd->num_vertices / 2;
      DrawLine(ccmd);
    }
    break;

      DefaultCaseIsUnreachable();
  }
}

bool GPUBackend::CompileDisplayPipelines(bool display, bool deinterlace, bool chroma_smoothing, Error* error)
{
  const GPUShaderGen shadergen(g_gpu_device->GetRenderAPI(), g_gpu_device->GetFeatures().dual_source_blend,
                               g_gpu_device->GetFeatures().framebuffer_fetch);

  GPUPipeline::GraphicsConfig plconfig;
  plconfig.input_layout.vertex_stride = 0;
  plconfig.primitive = GPUPipeline::Primitive::Triangles;
  plconfig.rasterization = GPUPipeline::RasterizationState::GetNoCullState();
  plconfig.depth = GPUPipeline::DepthState::GetNoTestsState();
  plconfig.blend = GPUPipeline::BlendState::GetNoBlendingState();
  plconfig.geometry_shader = nullptr;
  plconfig.depth_format = GPUTexture::Format::Unknown;
  plconfig.samples = 1;
  plconfig.per_sample_shading = false;
  plconfig.render_pass_flags = GPUPipeline::NoRenderPassFlags;

  if (display)
  {
    plconfig.layout = GPUPipeline::Layout::SingleTextureAndPushConstants;
    plconfig.SetTargetFormats(g_gpu_device->HasMainSwapChain() ? g_gpu_device->GetMainSwapChain()->GetFormat() :
                                                                 GPUTexture::Format::RGBA8);

    std::string vs = shadergen.GenerateDisplayVertexShader();
    std::string fs;
    switch (g_gpu_settings.display_scaling)
    {
      case DisplayScalingMode::BilinearSharp:
        fs = shadergen.GenerateDisplaySharpBilinearFragmentShader();
        break;

      case DisplayScalingMode::BilinearSmooth:
      case DisplayScalingMode::BilinearInteger:
        fs = shadergen.GenerateDisplayFragmentShader(true, false);
        break;

      case DisplayScalingMode::Nearest:
      case DisplayScalingMode::NearestInteger:
      default:
        fs = shadergen.GenerateDisplayFragmentShader(false, true);
        break;
    }

    std::unique_ptr<GPUShader> vso =
      g_gpu_device->CreateShader(GPUShaderStage::Vertex, shadergen.GetLanguage(), vs, error);
    std::unique_ptr<GPUShader> fso =
      g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(), fs, error);
    if (!vso || !fso)
      return false;
    GL_OBJECT_NAME(vso, "Display Vertex Shader");
    GL_OBJECT_NAME_FMT(fso, "Display Fragment Shader [{}]",
                       Settings::GetDisplayScalingName(g_gpu_settings.display_scaling));
    plconfig.vertex_shader = vso.get();
    plconfig.fragment_shader = fso.get();
    if (!(m_display_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
      return false;
    GL_OBJECT_NAME_FMT(m_display_pipeline, "Display Pipeline [{}]",
                       Settings::GetDisplayScalingName(g_gpu_settings.display_scaling));
  }

  if (deinterlace)
  {
    plconfig.SetTargetFormats(GPUTexture::Format::RGBA8);

    std::unique_ptr<GPUShader> vso = g_gpu_device->CreateShader(GPUShaderStage::Vertex, shadergen.GetLanguage(),
                                                                shadergen.GenerateScreenQuadVertexShader(), error);
    if (!vso)
      return false;
    GL_OBJECT_NAME(vso, "Deinterlace Vertex Shader");

    std::unique_ptr<GPUShader> fso;
    if (!(fso = g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(),
                                           shadergen.GenerateInterleavedFieldExtractFragmentShader(), error)))
    {
      return false;
    }

    GL_OBJECT_NAME(fso, "Deinterlace Field Extract Fragment Shader");

    plconfig.layout = GPUPipeline::Layout::SingleTextureAndPushConstants;
    plconfig.vertex_shader = vso.get();
    plconfig.fragment_shader = fso.get();
    if (!(m_deinterlace_extract_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
      return false;

    GL_OBJECT_NAME(m_deinterlace_extract_pipeline, "Deinterlace Field Extract Pipeline");

    switch (g_gpu_settings.display_deinterlacing_mode)
    {
      case DisplayDeinterlacingMode::Disabled:
      case DisplayDeinterlacingMode::Progressive:
        break;

      case DisplayDeinterlacingMode::Weave:
      {
        if (!(fso = g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(),
                                               shadergen.GenerateDeinterlaceWeaveFragmentShader(), error)))
        {
          return false;
        }

        GL_OBJECT_NAME(fso, "Weave Deinterlace Fragment Shader");

        plconfig.layout = GPUPipeline::Layout::SingleTextureAndPushConstants;
        plconfig.vertex_shader = vso.get();
        plconfig.fragment_shader = fso.get();
        if (!(m_deinterlace_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
          return false;

        GL_OBJECT_NAME(m_deinterlace_pipeline, "Weave Deinterlace Pipeline");
      }
      break;

      case DisplayDeinterlacingMode::Blend:
      {
        if (!(fso = g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(),
                                               shadergen.GenerateDeinterlaceBlendFragmentShader(), error)))
        {
          return false;
        }

        GL_OBJECT_NAME(fso, "Blend Deinterlace Fragment Shader");

        plconfig.layout = GPUPipeline::Layout::MultiTextureAndPushConstants;
        plconfig.vertex_shader = vso.get();
        plconfig.fragment_shader = fso.get();
        if (!(m_deinterlace_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
          return false;

        GL_OBJECT_NAME(m_deinterlace_pipeline, "Blend Deinterlace Pipeline");
      }
      break;

      case DisplayDeinterlacingMode::Adaptive:
      {
        fso = g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(),
                                         shadergen.GenerateFastMADReconstructFragmentShader(), error);
        if (!fso)
          return false;

        GL_OBJECT_NAME(fso, "FastMAD Reconstruct Fragment Shader");

        plconfig.layout = GPUPipeline::Layout::MultiTextureAndPushConstants;
        plconfig.fragment_shader = fso.get();
        if (!(m_deinterlace_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
          return false;

        GL_OBJECT_NAME(m_deinterlace_pipeline, "FastMAD Reconstruct Pipeline");
      }
      break;

      default:
        UnreachableCode();
    }
  }

  if (chroma_smoothing)
  {
    m_chroma_smoothing_pipeline.reset();
    g_gpu_device->RecycleTexture(std::move(m_chroma_smoothing_texture));

    if (g_gpu_settings.display_24bit_chroma_smoothing)
    {
      plconfig.layout = GPUPipeline::Layout::SingleTextureAndPushConstants;
      plconfig.SetTargetFormats(GPUTexture::Format::RGBA8);

      std::unique_ptr<GPUShader> vso = g_gpu_device->CreateShader(GPUShaderStage::Vertex, shadergen.GetLanguage(),
                                                                  shadergen.GenerateScreenQuadVertexShader(), error);
      std::unique_ptr<GPUShader> fso = g_gpu_device->CreateShader(
        GPUShaderStage::Fragment, shadergen.GetLanguage(), shadergen.GenerateChromaSmoothingFragmentShader(), error);
      if (!vso || !fso)
        return false;
      GL_OBJECT_NAME(vso, "Chroma Smoothing Vertex Shader");
      GL_OBJECT_NAME(fso, "Chroma Smoothing Fragment Shader");

      plconfig.vertex_shader = vso.get();
      plconfig.fragment_shader = fso.get();
      if (!(m_chroma_smoothing_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
        return false;
      GL_OBJECT_NAME(m_chroma_smoothing_pipeline, "Chroma Smoothing Pipeline");
    }
  }

  return true;
}

void GPUBackend::HandleUpdateDisplayCommand(const GPUBackendUpdateDisplayCommand* cmd)
{
  const GPUBackendUpdateDisplayCommand* ccmd = static_cast<const GPUBackendUpdateDisplayCommand*>(cmd);
  m_display_width = ccmd->display_width;
  m_display_height = ccmd->display_height;
  m_display_origin_left = ccmd->display_origin_left;
  m_display_origin_top = ccmd->display_origin_top;
  m_display_vram_width = ccmd->display_vram_width;
  m_display_vram_height = ccmd->display_vram_height;
  m_display_pixel_aspect_ratio = ccmd->display_pixel_aspect_ratio;

  UpdateDisplay(ccmd);

  if (cmd->submit_frame)
    HandleSubmitFrameCommand(&cmd->frame);
}

void GPUBackend::HandleSubmitFrameCommand(const GPUBackendFramePresentationParameters* cmd)
{
  // For regtest.
  Host::FrameDoneOnGPUThread(this, cmd->frame_number);

  if (cmd->media_capture)
    SendDisplayToMediaCapture(cmd->media_capture);

  if (cmd->present_frame)
  {
    GPUThread::Internal::PresentFrame(cmd->allow_present_skip, cmd->present_time);

    s_cpu_thread_state.queued_frames.fetch_sub(1, std::memory_order_acq_rel);

    bool expected = true;
    if (s_cpu_thread_state.waiting_for_gpu_thread.compare_exchange_strong(expected, false, std::memory_order_acq_rel,
                                                                          std::memory_order_relaxed))
    {
      DEV_LOG("--> Unblocking CPU thread");
      s_cpu_thread_state.gpu_thread_wait.Post();
    }
  }

  // Update perf counters *after* throttling, we want to measure from start-of-frame
  // to start-of-frame, not end-of-frame to end-of-frame (will be noisy due to different
  // amounts of computation happening in each frame).
  if (cmd->update_performance_counters)
    PerformanceCounters::Update(this, cmd->frame_number, cmd->internal_frame_number);
}

void GPUBackend::ClearDisplay()
{
  ClearDisplayTexture();

  // Just recycle the textures, it'll get re-fetched.
  DestroyDeinterlaceTextures();
}

void GPUBackend::ClearDisplayTexture()
{
  m_display_texture = nullptr;
  m_display_texture_view_x = 0;
  m_display_texture_view_y = 0;
  m_display_texture_view_width = 0;
  m_display_texture_view_height = 0;
}

void GPUBackend::SetDisplayTexture(GPUTexture* texture, GPUTexture* depth_buffer, s32 view_x, s32 view_y,
                                   s32 view_width, s32 view_height)
{
  DebugAssert(texture);

  if (g_gpu_settings.display_auto_resize_window &&
      (view_width != m_display_texture_view_width || view_height != m_display_texture_view_height))
  {
    Host::RunOnCPUThread([]() { System::RequestDisplaySize(); });
  }

  m_display_texture = texture;
  m_display_depth_buffer = depth_buffer;
  m_display_texture_view_x = view_x;
  m_display_texture_view_y = view_y;
  m_display_texture_view_width = view_width;
  m_display_texture_view_height = view_height;
}

GPUDevice::PresentResult GPUBackend::PresentDisplay()
{
  FlushRender();

  if (!g_gpu_device->HasMainSwapChain())
    return GPUDevice::PresentResult::SkipPresent;

  GSVector4i display_rect;
  GSVector4i draw_rect;
  CalculateDrawRect(g_gpu_device->GetMainSwapChain()->GetWidth(), g_gpu_device->GetMainSwapChain()->GetHeight(),
                    !g_gpu_settings.debugging.show_vram, true, &display_rect, &draw_rect);
  return RenderDisplay(nullptr, display_rect, draw_rect, !g_gpu_settings.debugging.show_vram);
}

GPUDevice::PresentResult GPUBackend::RenderDisplay(GPUTexture* target, const GSVector4i display_rect,
                                                   const GSVector4i draw_rect, bool postfx)
{
  GL_SCOPE_FMT("RenderDisplay: {}", draw_rect);

  if (m_display_texture)
    m_display_texture->MakeReadyForSampling();

  // Internal post-processing.
  GPUTexture* display_texture = m_display_texture;
  s32 display_texture_view_x = m_display_texture_view_x;
  s32 display_texture_view_y = m_display_texture_view_y;
  s32 display_texture_view_width = m_display_texture_view_width;
  s32 display_texture_view_height = m_display_texture_view_height;
  if (postfx && display_texture && PostProcessing::InternalChain.IsActive() &&
      PostProcessing::InternalChain.CheckTargets(DISPLAY_INTERNAL_POSTFX_FORMAT, display_texture_view_width,
                                                 display_texture_view_height))
  {
    DebugAssert(display_texture_view_x == 0 && display_texture_view_y == 0 &&
                static_cast<s32>(display_texture->GetWidth()) == display_texture_view_width &&
                static_cast<s32>(display_texture->GetHeight()) == display_texture_view_height);

    // Now we can apply the post chain.
    GPUTexture* post_output_texture = PostProcessing::InternalChain.GetOutputTexture();
    if (PostProcessing::InternalChain.Apply(display_texture, m_display_depth_buffer, post_output_texture,
                                            GSVector4i(0, 0, display_texture_view_width, display_texture_view_height),
                                            display_texture_view_width, display_texture_view_height, m_display_width,
                                            m_display_height) == GPUDevice::PresentResult::OK)
    {
      display_texture_view_x = 0;
      display_texture_view_y = 0;
      display_texture = post_output_texture;
      display_texture->MakeReadyForSampling();
    }
  }

  const GPUTexture::Format hdformat = target ? target->GetFormat() : g_gpu_device->GetMainSwapChain()->GetFormat();
  const u32 target_width = target ? target->GetWidth() : g_gpu_device->GetMainSwapChain()->GetWidth();
  const u32 target_height = target ? target->GetHeight() : g_gpu_device->GetMainSwapChain()->GetHeight();
  const bool really_postfx = (postfx && PostProcessing::DisplayChain.IsActive() && g_gpu_device->HasMainSwapChain() &&
                              hdformat != GPUTexture::Format::Unknown && target_width > 0 && target_height > 0 &&
                              PostProcessing::DisplayChain.CheckTargets(hdformat, target_width, target_height));
  GSVector4i real_draw_rect =
    (target || really_postfx) ? draw_rect : g_gpu_device->GetMainSwapChain()->PreRotateClipRect(draw_rect);
  if (g_gpu_device->UsesLowerLeftOrigin())
  {
    real_draw_rect = GPUDevice::FlipToLowerLeft(
      real_draw_rect,
      (target || really_postfx) ? target_height : g_gpu_device->GetMainSwapChain()->GetPostRotatedHeight());
  }
  if (really_postfx)
  {
    g_gpu_device->ClearRenderTarget(PostProcessing::DisplayChain.GetInputTexture(), GPUDevice::DEFAULT_CLEAR_COLOR);
    g_gpu_device->SetRenderTarget(PostProcessing::DisplayChain.GetInputTexture());
  }
  else
  {
    if (target)
    {
      g_gpu_device->SetRenderTarget(target);
    }
    else
    {
      const GPUDevice::PresentResult pres = g_gpu_device->BeginPresent(g_gpu_device->GetMainSwapChain());
      if (pres != GPUDevice::PresentResult::OK)
        return pres;
    }
  }

  if (display_texture)
  {
    bool texture_filter_linear = false;

    struct Uniforms
    {
      float src_rect[4];
      float src_size[4];
      float clamp_rect[4];
      float params[4];
      float rotation_matrix[2][2];
    } uniforms;
    std::memset(uniforms.params, 0, sizeof(uniforms.params));

    switch (g_gpu_settings.display_scaling)
    {
      case DisplayScalingMode::Nearest:
      case DisplayScalingMode::NearestInteger:
        break;

      case DisplayScalingMode::BilinearSmooth:
      case DisplayScalingMode::BilinearInteger:
        texture_filter_linear = true;
        break;

      case DisplayScalingMode::BilinearSharp:
      {
        texture_filter_linear = true;
        uniforms.params[0] = std::max(
          std::floor(static_cast<float>(draw_rect.width()) / static_cast<float>(m_display_texture_view_width)), 1.0f);
        uniforms.params[1] = std::max(
          std::floor(static_cast<float>(draw_rect.height()) / static_cast<float>(m_display_texture_view_height)), 1.0f);
        uniforms.params[2] = 0.5f - 0.5f / uniforms.params[0];
        uniforms.params[3] = 0.5f - 0.5f / uniforms.params[1];
      }
      break;

      default:
        UnreachableCode();
        break;
    }

    g_gpu_device->SetPipeline(m_display_pipeline.get());
    g_gpu_device->SetTextureSampler(
      0, display_texture, texture_filter_linear ? g_gpu_device->GetLinearSampler() : g_gpu_device->GetNearestSampler());

    // For bilinear, clamp to 0.5/SIZE-0.5 to avoid bleeding from the adjacent texels in VRAM. This is because
    // 1.0 in UV space is not the bottom-right texel, but a mix of the bottom-right and wrapped/next texel.
    const float rcp_width = 1.0f / static_cast<float>(display_texture->GetWidth());
    const float rcp_height = 1.0f / static_cast<float>(display_texture->GetHeight());
    uniforms.src_rect[0] = static_cast<float>(display_texture_view_x) * rcp_width;
    uniforms.src_rect[1] = static_cast<float>(display_texture_view_y) * rcp_height;
    uniforms.src_rect[2] = static_cast<float>(display_texture_view_width) * rcp_width;
    uniforms.src_rect[3] = static_cast<float>(display_texture_view_height) * rcp_height;
    uniforms.clamp_rect[0] = (static_cast<float>(display_texture_view_x) + 0.5f) * rcp_width;
    uniforms.clamp_rect[1] = (static_cast<float>(display_texture_view_y) + 0.5f) * rcp_height;
    uniforms.clamp_rect[2] =
      (static_cast<float>(display_texture_view_x + display_texture_view_width) - 0.5f) * rcp_width;
    uniforms.clamp_rect[3] =
      (static_cast<float>(display_texture_view_y + display_texture_view_height) - 0.5f) * rcp_height;
    uniforms.src_size[0] = static_cast<float>(display_texture->GetWidth());
    uniforms.src_size[1] = static_cast<float>(display_texture->GetHeight());
    uniforms.src_size[2] = rcp_width;
    uniforms.src_size[3] = rcp_height;

    const WindowInfo::PreRotation surface_prerotation = (target || really_postfx) ?
                                                          WindowInfo::PreRotation::Identity :
                                                          g_gpu_device->GetMainSwapChain()->GetPreRotation();
    if (g_gpu_settings.display_rotation != DisplayRotation::Normal ||
        surface_prerotation != WindowInfo::PreRotation::Identity)
    {
      static constexpr const std::array<float, static_cast<size_t>(DisplayRotation::Count)> rotation_radians = {{
        0.0f,
        static_cast<float>(std::numbers::pi * 1.5f), // Rotate90
        static_cast<float>(std::numbers::pi),        // Rotate180
        static_cast<float>(std::numbers::pi / 2.0),  // Rotate270
      }};

      const u32 rotation_idx =
        (static_cast<u32>(g_gpu_settings.display_rotation) + static_cast<u32>(surface_prerotation)) %
        static_cast<u32>(rotation_radians.size());
      GSMatrix2x2::Rotation(rotation_radians[rotation_idx]).store(uniforms.rotation_matrix);
    }
    else
    {
      GSMatrix2x2::Identity().store(uniforms.rotation_matrix);
    }

    g_gpu_device->PushUniformBuffer(&uniforms, sizeof(uniforms));

    g_gpu_device->SetViewportAndScissor(real_draw_rect);
    g_gpu_device->Draw(3, 0);
  }

  if (really_postfx)
  {
    DebugAssert(!g_gpu_settings.debugging.show_vram);

    // "original size" in postfx includes padding.
    const float upscale_x =
      m_display_texture ? static_cast<float>(m_display_texture_view_width) / static_cast<float>(m_display_vram_width) :
                          1.0f;
    const float upscale_y = m_display_texture ? static_cast<float>(m_display_texture_view_height) /
                                                  static_cast<float>(m_display_vram_height) :
                                                1.0f;
    const s32 orig_width = static_cast<s32>(std::ceil(static_cast<float>(m_display_width) * upscale_x));
    const s32 orig_height = static_cast<s32>(std::ceil(static_cast<float>(m_display_height) * upscale_y));

    return PostProcessing::DisplayChain.Apply(PostProcessing::DisplayChain.GetInputTexture(), nullptr, target,
                                              display_rect, orig_width, orig_height, m_display_width, m_display_height);
  }
  else
  {
    return GPUDevice::PresentResult::OK;
  }
}

void GPUBackend::SendDisplayToMediaCapture(MediaCapture* cap)
{
  GPUTexture* target = cap->GetRenderTexture();
  if (!target) [[unlikely]]
  {
    WARNING_LOG("Failed to get video capture render texture.");
    Host::RunOnCPUThread(&System::StopMediaCapture);
    return;
  }

  const bool apply_aspect_ratio =
    (g_gpu_settings.display_screenshot_mode != DisplayScreenshotMode::UncorrectedInternalResolution);
  const bool postfx = (g_gpu_settings.display_screenshot_mode != DisplayScreenshotMode::InternalResolution);
  GSVector4i display_rect, draw_rect;
  CalculateDrawRect(target->GetWidth(), target->GetHeight(), !g_gpu_settings.debugging.show_vram, apply_aspect_ratio,
                    &display_rect, &draw_rect);

  // Not cleared by RenderDisplay().
  g_gpu_device->ClearRenderTarget(target, GPUDevice::DEFAULT_CLEAR_COLOR);

  if (RenderDisplay(target, display_rect, draw_rect, postfx) != GPUDevice::PresentResult::OK ||
      !cap->DeliverVideoFrame(target)) [[unlikely]]
  {
    WARNING_LOG("Failed to render/deliver video capture frame.");
    Host::RunOnCPUThread(&System::StopMediaCapture);
    return;
  }
}

void GPUBackend::DestroyDeinterlaceTextures()
{
  for (std::unique_ptr<GPUTexture>& tex : m_deinterlace_buffers)
    g_gpu_device->RecycleTexture(std::move(tex));
  g_gpu_device->RecycleTexture(std::move(m_deinterlace_texture));
  m_current_deinterlace_buffer = 0;
}

bool GPUBackend::Deinterlace(u32 field, u32 line_skip)
{
  GPUTexture* src = m_display_texture;
  const u32 x = m_display_texture_view_x;
  const u32 y = m_display_texture_view_y;
  const u32 width = m_display_texture_view_width;
  const u32 height = m_display_texture_view_height;

  switch (g_gpu_settings.display_deinterlacing_mode)
  {
    case DisplayDeinterlacingMode::Disabled:
    {
      if (line_skip == 0)
        return true;

      // Still have to extract the field.
      if (!DeinterlaceExtractField(0, src, x, y, width, height, line_skip)) [[unlikely]]
        return false;

      SetDisplayTexture(m_deinterlace_buffers[0].get(), m_display_depth_buffer, 0, 0, width, height);
      return true;
    }

    case DisplayDeinterlacingMode::Weave:
    {
      GL_SCOPE_FMT("DeinterlaceWeave({{{},{}}}, {}x{}, field={}, line_skip={})", x, y, width, height, field, line_skip);

      const u32 full_height = height * 2;
      if (!DeinterlaceSetTargetSize(width, full_height, true)) [[unlikely]]
      {
        ClearDisplayTexture();
        return false;
      }

      src->MakeReadyForSampling();

      g_gpu_device->SetRenderTarget(m_deinterlace_texture.get());
      g_gpu_device->SetPipeline(m_deinterlace_pipeline.get());
      g_gpu_device->SetTextureSampler(0, src, g_gpu_device->GetNearestSampler());
      const u32 uniforms[] = {x, y, field, line_skip};
      g_gpu_device->PushUniformBuffer(uniforms, sizeof(uniforms));
      g_gpu_device->SetViewportAndScissor(0, 0, width, full_height);
      g_gpu_device->Draw(3, 0);

      m_deinterlace_texture->MakeReadyForSampling();
      SetDisplayTexture(m_deinterlace_texture.get(), m_display_depth_buffer, 0, 0, width, full_height);
      return true;
    }

    case DisplayDeinterlacingMode::Blend:
    {
      constexpr u32 NUM_BLEND_BUFFERS = 2;

      GL_SCOPE_FMT("DeinterlaceBlend({{{},{}}}, {}x{}, field={}, line_skip={})", x, y, width, height, field, line_skip);

      const u32 this_buffer = m_current_deinterlace_buffer;
      m_current_deinterlace_buffer = (m_current_deinterlace_buffer + 1u) % NUM_BLEND_BUFFERS;
      GL_INS_FMT("Current buffer: {}", this_buffer);
      if (!DeinterlaceExtractField(this_buffer, src, x, y, width, height, line_skip) ||
          !DeinterlaceSetTargetSize(width, height, false)) [[unlikely]]
      {
        ClearDisplayTexture();
        return false;
      }

      // TODO: could be implemented with alpha blending instead..

      g_gpu_device->InvalidateRenderTarget(m_deinterlace_texture.get());
      g_gpu_device->SetRenderTarget(m_deinterlace_texture.get());
      g_gpu_device->SetPipeline(m_deinterlace_pipeline.get());
      g_gpu_device->SetTextureSampler(0, m_deinterlace_buffers[this_buffer].get(), g_gpu_device->GetNearestSampler());
      g_gpu_device->SetTextureSampler(1, m_deinterlace_buffers[(this_buffer - 1) % NUM_BLEND_BUFFERS].get(),
                                      g_gpu_device->GetNearestSampler());
      g_gpu_device->SetViewportAndScissor(0, 0, width, height);
      g_gpu_device->Draw(3, 0);

      m_deinterlace_texture->MakeReadyForSampling();
      SetDisplayTexture(m_deinterlace_texture.get(), m_display_depth_buffer, 0, 0, width, height);
      return true;
    }

    case DisplayDeinterlacingMode::Adaptive:
    {
      GL_SCOPE_FMT("DeinterlaceAdaptive({{{},{}}}, {}x{}, field={}, line_skip={})", x, y, width, height, field,
                   line_skip);

      const u32 full_height = height * 2;
      const u32 this_buffer = m_current_deinterlace_buffer;
      m_current_deinterlace_buffer = (m_current_deinterlace_buffer + 1u) % DEINTERLACE_BUFFER_COUNT;
      GL_INS_FMT("Current buffer: {}", this_buffer);
      if (!DeinterlaceExtractField(this_buffer, src, x, y, width, height, line_skip) ||
          !DeinterlaceSetTargetSize(width, full_height, false)) [[unlikely]]
      {
        ClearDisplayTexture();
        return false;
      }

      g_gpu_device->SetRenderTarget(m_deinterlace_texture.get());
      g_gpu_device->SetPipeline(m_deinterlace_pipeline.get());
      g_gpu_device->SetTextureSampler(0, m_deinterlace_buffers[this_buffer].get(), g_gpu_device->GetNearestSampler());
      g_gpu_device->SetTextureSampler(1, m_deinterlace_buffers[(this_buffer - 1) % DEINTERLACE_BUFFER_COUNT].get(),
                                      g_gpu_device->GetNearestSampler());
      g_gpu_device->SetTextureSampler(2, m_deinterlace_buffers[(this_buffer - 2) % DEINTERLACE_BUFFER_COUNT].get(),
                                      g_gpu_device->GetNearestSampler());
      g_gpu_device->SetTextureSampler(3, m_deinterlace_buffers[(this_buffer - 3) % DEINTERLACE_BUFFER_COUNT].get(),
                                      g_gpu_device->GetNearestSampler());
      const u32 uniforms[] = {field, full_height};
      g_gpu_device->PushUniformBuffer(uniforms, sizeof(uniforms));
      g_gpu_device->SetViewportAndScissor(0, 0, width, full_height);
      g_gpu_device->Draw(3, 0);

      m_deinterlace_texture->MakeReadyForSampling();
      SetDisplayTexture(m_deinterlace_texture.get(), m_display_depth_buffer, 0, 0, width, full_height);
      return true;
    }

    default:
      UnreachableCode();
  }
}

bool GPUBackend::DeinterlaceExtractField(u32 dst_bufidx, GPUTexture* src, u32 x, u32 y, u32 width, u32 height,
                                         u32 line_skip)
{
  if (!m_deinterlace_buffers[dst_bufidx] || m_deinterlace_buffers[dst_bufidx]->GetWidth() != width ||
      m_deinterlace_buffers[dst_bufidx]->GetHeight() != height)
  {
    if (!g_gpu_device->ResizeTexture(&m_deinterlace_buffers[dst_bufidx], width, height, GPUTexture::Type::RenderTarget,
                                     GPUTexture::Format::RGBA8, GPUTexture::Flags::None, false)) [[unlikely]]
    {
      return false;
    }

    GL_OBJECT_NAME_FMT(m_deinterlace_buffers[dst_bufidx], "Blend Deinterlace Buffer {}", dst_bufidx);
  }

  GPUTexture* dst = m_deinterlace_buffers[dst_bufidx].get();
  g_gpu_device->InvalidateRenderTarget(dst);

  // If we're not skipping lines, then we can simply copy the texture.
  if (line_skip == 0 && src->GetFormat() == dst->GetFormat())
  {
    GL_INS_FMT("DeinterlaceExtractField({{{},{}}} {}x{} line_skip={}) => copy direct", x, y, width, height, line_skip);
    g_gpu_device->CopyTextureRegion(dst, 0, 0, 0, 0, src, x, y, 0, 0, width, height);
  }
  else
  {
    GL_SCOPE_FMT("DeinterlaceExtractField({{{},{}}} {}x{} line_skip={}) => shader copy", x, y, width, height,
                 line_skip);

    // Otherwise, we need to extract every other line from the texture.
    src->MakeReadyForSampling();
    g_gpu_device->SetRenderTarget(dst);
    g_gpu_device->SetPipeline(m_deinterlace_extract_pipeline.get());
    g_gpu_device->SetTextureSampler(0, src, g_gpu_device->GetNearestSampler());
    const u32 uniforms[] = {x, y, line_skip};
    g_gpu_device->PushUniformBuffer(uniforms, sizeof(uniforms));
    g_gpu_device->SetViewportAndScissor(0, 0, width, height);
    g_gpu_device->Draw(3, 0);
  }

  dst->MakeReadyForSampling();
  return true;
}

bool GPUBackend::DeinterlaceSetTargetSize(u32 width, u32 height, bool preserve)
{
  if (!m_deinterlace_texture || m_deinterlace_texture->GetWidth() != width ||
      m_deinterlace_texture->GetHeight() != height)
  {
    if (!g_gpu_device->ResizeTexture(&m_deinterlace_texture, width, height, GPUTexture::Type::RenderTarget,
                                     GPUTexture::Format::RGBA8, GPUTexture::Flags::None, preserve)) [[unlikely]]
    {
      return false;
    }

    GL_OBJECT_NAME(m_deinterlace_texture, "Deinterlace target texture");
  }

  return true;
}

bool GPUBackend::ApplyChromaSmoothing()
{
  const u32 x = m_display_texture_view_x;
  const u32 y = m_display_texture_view_y;
  const u32 width = m_display_texture_view_width;
  const u32 height = m_display_texture_view_height;
  if (!m_chroma_smoothing_texture || m_chroma_smoothing_texture->GetWidth() != width ||
      m_chroma_smoothing_texture->GetHeight() != height)
  {
    if (!g_gpu_device->ResizeTexture(&m_chroma_smoothing_texture, width, height, GPUTexture::Type::RenderTarget,
                                     GPUTexture::Format::RGBA8, GPUTexture::Flags::None, false))
    {
      ClearDisplayTexture();
      return false;
    }

    GL_OBJECT_NAME(m_chroma_smoothing_texture, "Chroma smoothing texture");
  }

  GL_SCOPE_FMT("ApplyChromaSmoothing({{{},{}}}, {}x{})", x, y, width, height);

  m_display_texture->MakeReadyForSampling();
  g_gpu_device->InvalidateRenderTarget(m_chroma_smoothing_texture.get());
  g_gpu_device->SetRenderTarget(m_chroma_smoothing_texture.get());
  g_gpu_device->SetPipeline(m_chroma_smoothing_pipeline.get());
  g_gpu_device->SetTextureSampler(0, m_display_texture, g_gpu_device->GetNearestSampler());
  const u32 uniforms[] = {x, y, width - 1, height - 1};
  g_gpu_device->PushUniformBuffer(uniforms, sizeof(uniforms));
  g_gpu_device->SetViewportAndScissor(0, 0, width, height);
  g_gpu_device->Draw(3, 0);

  m_chroma_smoothing_texture->MakeReadyForSampling();
  SetDisplayTexture(m_chroma_smoothing_texture.get(), m_display_depth_buffer, 0, 0, width, height);
  return true;
}

void GPUBackend::UpdateCLUT(GPUTexturePaletteReg reg, bool clut_is_8bit)
{
}

void GPUBackend::CalculateDrawRect(s32 window_width, s32 window_height, bool apply_rotation, bool apply_aspect_ratio,
                                   GSVector4i* display_rect, GSVector4i* draw_rect) const
{
  const bool integer_scale = (g_gpu_settings.display_scaling == DisplayScalingMode::NearestInteger ||
                              g_gpu_settings.display_scaling == DisplayScalingMode::BilinearInteger);
  const bool show_vram = g_gpu_settings.debugging.show_vram;
  const u32 display_width = show_vram ? VRAM_WIDTH : m_display_width;
  const u32 display_height = show_vram ? VRAM_HEIGHT : m_display_height;
  const s32 display_origin_left = show_vram ? 0 : m_display_origin_left;
  const s32 display_origin_top = show_vram ? 0 : m_display_origin_top;
  const u32 display_vram_width = show_vram ? VRAM_WIDTH : m_display_vram_width;
  const u32 display_vram_height = show_vram ? VRAM_HEIGHT : m_display_vram_height;
  const float display_pixel_aspect_ratio = show_vram ? 1.0f : m_display_pixel_aspect_ratio;
  GPU::CalculateDrawRect(window_width, window_height, display_width, display_height, display_origin_left,
                         display_origin_top, display_vram_width, display_vram_height, g_gpu_settings.display_rotation,
                         display_pixel_aspect_ratio, g_gpu_settings.display_stretch_vertically, integer_scale,
                         display_rect, draw_rect);
}

bool CompressAndWriteTextureToFile(u32 width, u32 height, std::string path, FileSystem::ManagedCFilePtr fp, u8 quality,
                                   bool clear_alpha, bool flip_y, Image image, std::string osd_key)
{

  Error error;

  if (flip_y)
    image.FlipY();

  if (image.GetFormat() != ImageFormat::RGBA8)
  {
    std::optional<Image> convert_image = image.ConvertToRGBA8(&error);
    if (!convert_image.has_value())
    {
      ERROR_LOG("Failed to convert {} screenshot to RGBA8: {}", Image::GetFormatName(image.GetFormat()),
                error.GetDescription());
      image.Invalidate();
    }
    else
    {
      image = std::move(convert_image.value());
    }
  }

  bool result = false;
  if (image.IsValid())
  {
    if (clear_alpha)
      image.SetAllPixelsOpaque();

    result = image.SaveToFile(path.c_str(), fp.get(), quality, &error);
    if (!result)
      ERROR_LOG("Failed to save screenshot to '{}': '{}'", Path::GetFileName(path), error.GetDescription());
  }

  if (!osd_key.empty())
  {
    Host::AddIconOSDMessage(std::move(osd_key), ICON_EMOJI_CAMERA,
                            fmt::format(result ? TRANSLATE_FS("GPU", "Saved screenshot to '{}'.") :
                                                 TRANSLATE_FS("GPU", "Failed to save screenshot to '{}'."),
                                        Path::GetFileName(path),
                                        result ? Host::OSD_INFO_DURATION : Host::OSD_ERROR_DURATION));
  }

  return result;
}

bool GPUBackend::WriteDisplayTextureToFile(std::string filename)
{
  if (!m_display_texture)
    return false;

  const u32 read_x = static_cast<u32>(m_display_texture_view_x);
  const u32 read_y = static_cast<u32>(m_display_texture_view_y);
  const u32 read_width = static_cast<u32>(m_display_texture_view_width);
  const u32 read_height = static_cast<u32>(m_display_texture_view_height);
  const ImageFormat read_format = GPUTexture::GetImageFormatForTextureFormat(m_display_texture->GetFormat());
  if (read_format == ImageFormat::None)
    return false;

  Image image(read_width, read_height, read_format);
  std::unique_ptr<GPUDownloadTexture> dltex;
  if (g_gpu_device->GetFeatures().memory_import)
  {
    dltex = g_gpu_device->CreateDownloadTexture(read_width, read_height, m_display_texture->GetFormat(),
                                                image.GetPixels(), image.GetStorageSize(), image.GetPitch());
  }
  if (!dltex)
  {
    if (!(dltex = g_gpu_device->CreateDownloadTexture(read_width, read_height, m_display_texture->GetFormat())))
    {
      ERROR_LOG("Failed to create {}x{} {} download texture", read_width, read_height,
                GPUTexture::GetFormatName(m_display_texture->GetFormat()));
      return false;
    }
  }

  dltex->CopyFromTexture(0, 0, m_display_texture, read_x, read_y, read_width, read_height, 0, 0, !dltex->IsImported());
  if (!dltex->ReadTexels(0, 0, read_width, read_height, image.GetPixels(), image.GetPitch()))
  {
    RestoreDeviceContext();
    return false;
  }

  RestoreDeviceContext();

  Error error;
  auto fp = FileSystem::OpenManagedCFile(filename.c_str(), "wb", &error);
  if (!fp)
  {
    ERROR_LOG("Can't open file '{}': {}", Path::GetFileName(filename), error.GetDescription());
    return false;
  }

  constexpr bool clear_alpha = true;
  const bool flip_y = g_gpu_device->UsesLowerLeftOrigin();

  return CompressAndWriteTextureToFile(read_width, read_height, std::move(filename), std::move(fp),
                                       g_gpu_settings.display_screenshot_quality, clear_alpha, flip_y, std::move(image),
                                       std::string());
}

void GPUBackend::HandleRenderScreenshotToBuffer(const GPUThreadRenderScreenshotToBufferCommand* cmd)
{
  GSVector4i draw_rect, display_rect;
  CalculateDrawRect(static_cast<s32>(cmd->width), static_cast<s32>(cmd->height), true, true, &display_rect, &draw_rect);

  // Crop it.
  const u32 width = static_cast<u32>(display_rect.width());
  const u32 height = static_cast<u32>(display_rect.height());
  draw_rect = draw_rect.sub32(display_rect.xyxy());
  display_rect = display_rect.sub32(display_rect.xyxy());
  *cmd->out_result = RenderScreenshotToBuffer(width, height, display_rect, draw_rect, cmd->postfx, cmd->out_image);

  RestoreDeviceContext();
}

bool GPUBackend::RenderScreenshotToBuffer(u32 width, u32 height, const GSVector4i display_rect,
                                          const GSVector4i draw_rect, bool postfx, Image* out_image)
{
  const GPUTexture::Format hdformat =
    g_gpu_device->HasMainSwapChain() ? g_gpu_device->GetMainSwapChain()->GetFormat() : GPUTexture::Format::RGBA8;
  const ImageFormat image_format = GPUTexture::GetImageFormatForTextureFormat(hdformat);
  if (image_format == ImageFormat::None)
    return false;

  auto render_texture = g_gpu_device->FetchAutoRecycleTexture(width, height, 1, 1, 1, GPUTexture::Type::RenderTarget,
                                                              hdformat, GPUTexture::Flags::None);
  if (!render_texture)
    return false;

  g_gpu_device->ClearRenderTarget(render_texture.get(), GPUDevice::DEFAULT_CLEAR_COLOR);

  // TODO: this should use copy shader instead.
  RenderDisplay(render_texture.get(), display_rect, draw_rect, postfx);

  Image image(width, height, image_format);

  Error error;
  std::unique_ptr<GPUDownloadTexture> dltex;
  if (g_gpu_device->GetFeatures().memory_import)
  {
    dltex = g_gpu_device->CreateDownloadTexture(width, height, hdformat, image.GetPixels(), image.GetStorageSize(),
                                                image.GetPitch(), &error);
  }
  if (!dltex)
  {
    if (!(dltex = g_gpu_device->CreateDownloadTexture(width, height, hdformat, &error)))
    {
      ERROR_LOG("Failed to create {}x{} download texture: {}", width, height, error.GetDescription());
      return false;
    }
  }

  dltex->CopyFromTexture(0, 0, render_texture.get(), 0, 0, width, height, 0, 0, false);
  if (!dltex->ReadTexels(0, 0, width, height, image.GetPixels(), image.GetPitch()))
  {
    RestoreDeviceContext();
    return false;
  }

  RestoreDeviceContext();
  *out_image = std::move(image);
  return true;
}

void GPUBackend::CalculateScreenshotSize(DisplayScreenshotMode mode, u32* width, u32* height, GSVector4i* display_rect,
                                         GSVector4i* draw_rect) const
{
  const bool internal_resolution =
    (mode != DisplayScreenshotMode::ScreenResolution || g_gpu_settings.debugging.show_vram);
  if (internal_resolution && m_display_texture_view_width != 0 && m_display_texture_view_height != 0)
  {
    if (mode == DisplayScreenshotMode::InternalResolution)
    {
      float f_width = static_cast<float>(m_display_texture_view_width);
      float f_height = static_cast<float>(m_display_texture_view_height);
      if (!g_gpu_settings.debugging.show_vram)
        GPU::ApplyPixelAspectRatioToSize(m_display_pixel_aspect_ratio, &f_width, &f_height);

      // DX11 won't go past 16K texture size.
      const float max_texture_size = static_cast<float>(g_gpu_device->GetMaxTextureSize());
      if (f_width > max_texture_size)
      {
        f_height = f_height / (f_width / max_texture_size);
        f_width = max_texture_size;
      }
      if (f_height > max_texture_size)
      {
        f_height = max_texture_size;
        f_width = f_width / (f_height / max_texture_size);
      }

      *width = static_cast<u32>(std::ceil(f_width));
      *height = static_cast<u32>(std::ceil(f_height));
    }
    else // if (mode == DisplayScreenshotMode::UncorrectedInternalResolution)
    {
      *width = m_display_texture_view_width;
      *height = m_display_texture_view_height;
    }

    // Remove padding, it's not part of the framebuffer.
    *draw_rect = GSVector4i(0, 0, static_cast<s32>(*width), static_cast<s32>(*height));
    *display_rect = *draw_rect;
  }
  else
  {
    *width = g_gpu_device->HasMainSwapChain() ? g_gpu_device->GetMainSwapChain()->GetWidth() : 1;
    *height = g_gpu_device->HasMainSwapChain() ? g_gpu_device->GetMainSwapChain()->GetHeight() : 1;
    CalculateDrawRect(*width, *height, true, !g_settings.debugging.show_vram, display_rect, draw_rect);
  }
}

void GPUBackend::RenderScreenshotToFile(const std::string_view path, DisplayScreenshotMode mode, u8 quality,
                                        bool compress_on_thread, bool show_osd_message)
{
  GPUThreadRenderScreenshotToFileCommand* cmd = static_cast<GPUThreadRenderScreenshotToFileCommand*>(
    GPUThread::AllocateCommand(GPUBackendCommandType::RenderScreenshotToFile,
                               sizeof(GPUThreadRenderScreenshotToFileCommand) + static_cast<u32>(path.length())));
  cmd->mode = mode;
  cmd->quality = quality;
  cmd->compress_on_thread = compress_on_thread;
  cmd->show_osd_message = show_osd_message;
  cmd->path_length = static_cast<u32>(path.length());
  std::memcpy(cmd->path, path.data(), cmd->path_length);
  GPUThread::PushCommandAndWakeThread(cmd);
}

void GPUBackend::HandleRenderScreenshotToFile(const GPUThreadRenderScreenshotToFileCommand* cmd)
{
  const std::string path(cmd->path, cmd->path_length);

  u32 width, height;
  GSVector4i display_rect, draw_rect;
  CalculateScreenshotSize(cmd->mode, &width, &height, &display_rect, &draw_rect);

  const bool internal_resolution = (cmd->mode != DisplayScreenshotMode::ScreenResolution);
  if (width == 0 || height == 0)
    return;

  Image image;
  if (!RenderScreenshotToBuffer(width, height, display_rect, draw_rect, !internal_resolution, &image))
  {
    ERROR_LOG("Failed to render {}x{} screenshot", width, height);
    return;
  }

  Error error;
  auto fp = FileSystem::OpenManagedCFile(path.c_str(), "wb", &error);
  if (!fp)
  {
    ERROR_LOG("Can't open file '{}': {}", Path::GetFileName(path), error.GetDescription());
    return;
  }

  std::string osd_key;
  if (cmd->show_osd_message)
  {
    // Use a 60 second timeout to give it plenty of time to actually save.
    osd_key = fmt::format("ScreenshotSaver_{}", path);
    Host::AddIconOSDMessage(osd_key, ICON_EMOJI_CAMERA_WITH_FLASH,
                            fmt::format(TRANSLATE_FS("GPU", "Saving screenshot to '{}'."), Path::GetFileName(path)),
                            60.0f);
  }

  if (cmd->compress_on_thread)
  {
    System::QueueTaskOnThread([width, height, path = std::move(path), fp = fp.release(), quality = cmd->quality,
                               flip_y = g_gpu_device->UsesLowerLeftOrigin(), image = std::move(image),
                               osd_key = std::move(osd_key)]() mutable {
      CompressAndWriteTextureToFile(width, height, std::move(path), FileSystem::ManagedCFilePtr(fp), quality, true,
                                    flip_y, std::move(image), std::move(osd_key));
      System::RemoveSelfFromTaskThreads();
    });
  }
  else
  {
    CompressAndWriteTextureToFile(width, height, std::move(path), std::move(fp), cmd->quality, true,
                                  g_gpu_device->UsesLowerLeftOrigin(), std::move(image), std::move(osd_key));
  }
}

void GPUBackend::GetStatsString(SmallStringBase& str) const
{
  if (IsUsingHardwareBackend())
  {
    str.format("{}{} HW | {} P | {} DC | {} B | {} RP | {} RB | {} C | {} W",
               GPUDevice::RenderAPIToString(g_gpu_device->GetRenderAPI()), g_gpu_settings.gpu_use_thread ? "-MT" : "",
               s_stats.num_primitives, s_stats.host_num_draws, s_stats.host_num_barriers,
               s_stats.host_num_render_passes, s_stats.host_num_downloads, s_stats.num_copies, s_stats.num_writes);
  }
  else
  {
    str.format("{}{} SW | {} P | {} R | {} C | {} W", GPUDevice::RenderAPIToString(g_gpu_device->GetRenderAPI()),
               g_gpu_settings.gpu_use_thread ? "-MT" : "", s_stats.num_primitives, s_stats.num_reads,
               s_stats.num_copies, s_stats.num_writes);
  }
}

void GPUBackend::GetMemoryStatsString(SmallStringBase& str) const
{
  const u32 vram_usage_mb = static_cast<u32>((g_gpu_device->GetVRAMUsage() + (1048576 - 1)) / 1048576);
  const u32 stream_kb = static_cast<u32>((s_stats.host_buffer_streamed + (1024 - 1)) / 1024);

  str.format("{} MB VRAM | {} KB STR | {} TC | {} TU", vram_usage_mb, stream_kb, s_stats.host_num_copies,
             s_stats.host_num_uploads);
}

void GPUBackend::ResetStatistics()
{
  s_counters = {};
  g_gpu_device->ResetStatistics();
}

void GPUBackend::UpdateStatistics(u32 frame_count)
{
  const GPUDevice::Statistics& stats = g_gpu_device->GetStatistics();
  const u32 round = (frame_count - 1);

#define UPDATE_COUNTER(x) s_stats.x = (s_counters.x + round) / frame_count
#define UPDATE_GPU_STAT(x) s_stats.host_##x = (stats.x + round) / frame_count

  UPDATE_COUNTER(num_reads);
  UPDATE_COUNTER(num_writes);
  UPDATE_COUNTER(num_copies);
  UPDATE_COUNTER(num_vertices);
  UPDATE_COUNTER(num_primitives);

  // UPDATE_COUNTER(num_read_texture_updates);
  // UPDATE_COUNTER(num_ubo_updates);

  UPDATE_GPU_STAT(buffer_streamed);
  UPDATE_GPU_STAT(num_draws);
  UPDATE_GPU_STAT(num_barriers);
  UPDATE_GPU_STAT(num_render_passes);
  UPDATE_GPU_STAT(num_copies);
  UPDATE_GPU_STAT(num_downloads);
  UPDATE_GPU_STAT(num_uploads);

#undef UPDATE_GPU_STAT
#undef UPDATE_COUNTER

  ResetStatistics();
}
