// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gpu_backend.h"
#include "gpu.h"
#include "gpu_presenter.h"
#include "gpu_sw_rasterizer.h"
#include "gpu_thread.h"
#include "host.h"
#include "performance_counters.h"
#include "save_state_version.h"
#include "settings.h"
#include "system.h"
#include "system_private.h"

#include "util/gpu_device.h"
#include "util/imgui_manager.h"
#include "util/state_wrapper.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/threading.h"

#include "IconsEmoji.h"
#include "IconsFontAwesome6.h"
#include "fmt/format.h"

LOG_CHANNEL(GPU);

namespace {

struct ALIGN_TO_CACHE_LINE CPUThreadState
{
  static constexpr u32 WAIT_NONE = 0;
  static constexpr u32 WAIT_CPU_THREAD_WAITING = 1;
  static constexpr u32 WAIT_GPU_THREAD_SIGNALING = 2;
  static constexpr u32 WAIT_GPU_THREAD_POSTED = 3;

  std::atomic<u32> queued_frames;
  std::atomic<u32> wait_state;
  Threading::KernelSemaphore gpu_thread_wait;
};

} // namespace

GPUBackend::Counters GPUBackend::s_counters = {};
GPUBackend::Stats GPUBackend::s_stats = {};

static CPUThreadState s_cpu_thread_state = {};

GPUBackend::GPUBackend(GPUPresenter& presenter) : m_presenter(presenter)
{
  GPU_SW_Rasterizer::SelectImplementation();
  ResetStatistics();
}

GPUBackend::~GPUBackend()
{
  m_presenter.ClearDisplayTexture();
}

void GPUBackend::SetScreenQuadInputLayout(GPUPipeline::GraphicsConfig& config)
{
  static constexpr GPUPipeline::VertexAttribute screen_vertex_attributes[] = {
    GPUPipeline::VertexAttribute::Make(0, GPUPipeline::VertexAttribute::Semantic::Position, 0,
                                       GPUPipeline::VertexAttribute::Type::Float, 2, OFFSETOF(ScreenVertex, x)),
    GPUPipeline::VertexAttribute::Make(1, GPUPipeline::VertexAttribute::Semantic::TexCoord, 0,
                                       GPUPipeline::VertexAttribute::Type::Float, 2, OFFSETOF(ScreenVertex, u)),
  };

  // common state
  config.input_layout.vertex_attributes = screen_vertex_attributes;
  config.input_layout.vertex_stride = sizeof(ScreenVertex);
  config.primitive = GPUPipeline::Primitive::TriangleStrips;
}

GSVector4 GPUBackend::GetScreenQuadClipSpaceCoordinates(const GSVector4i bounds, const GSVector2i rt_size)
{
  const GSVector4 fboundsxxyy = GSVector4(bounds.xzyw());
  const GSVector2 fsize = GSVector2(rt_size);
  const GSVector2 x = ((fboundsxxyy.xy() * GSVector2::cxpr(2.0f)) / fsize.xx()) - GSVector2::cxpr(1.0f);
  const GSVector2 y = GSVector2::cxpr(1.0f) - (GSVector2::cxpr(2.0f) * (fboundsxxyy.zw() / fsize.yy()));
  return GSVector4::xyxy(x, y).xzyw();
}

void GPUBackend::DrawScreenQuad(const GSVector4i bounds, const GSVector2i rt_size, const GSVector4 uv_bounds,
                                const void* push_constants, u32 push_constants_size)
{
  const GSVector4 xy = GetScreenQuadClipSpaceCoordinates(bounds, rt_size);

  ScreenVertex* vertices;
  u32 space;
  u32 base_vertex;
  g_gpu_device->MapVertexBuffer(sizeof(ScreenVertex), 4, reinterpret_cast<void**>(&vertices), &space, &base_vertex);

  vertices[0].Set(xy.xy(), uv_bounds.xy());
  vertices[1].Set(xy.zyzw().xy(), uv_bounds.zyzw().xy());
  vertices[2].Set(xy.xwzw().xy(), uv_bounds.xwzw().xy());
  vertices[3].Set(xy.zw(), uv_bounds.zw());

  g_gpu_device->UnmapVertexBuffer(sizeof(ScreenVertex), 4);

  if (push_constants_size > 0)
    g_gpu_device->DrawWithPushConstants(4, base_vertex, push_constants, push_constants_size);
  else
    g_gpu_device->Draw(4, base_vertex);
}

bool GPUBackend::Initialize(bool clear_vram, Error* error)
{
  m_clamped_drawing_area = GPU::GetClampedDrawingArea(GPU_SW_Rasterizer::g_drawing_area);
  return true;
}

bool GPUBackend::UpdateSettings(const GPUSettings& old_settings, Error* error)
{
  if (g_gpu_settings.display_show_gpu_stats != old_settings.display_show_gpu_stats)
    GPUBackend::ResetStatistics();

  return true;
}

void GPUBackend::UpdatePostProcessingSettings(bool force_reload)
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
  cmd->num_vertices = Truncate16(num_vertices);
  return cmd;
}

GPUBackendDrawPrecisePolygonCommand* GPUBackend::NewDrawPrecisePolygonCommand(u32 num_vertices)
{
  const u32 size =
    sizeof(GPUBackendDrawPrecisePolygonCommand) + (num_vertices * sizeof(GPUBackendDrawPrecisePolygonCommand::Vertex));
  GPUBackendDrawPrecisePolygonCommand* cmd = static_cast<GPUBackendDrawPrecisePolygonCommand*>(
    GPUThread::AllocateCommand(GPUBackendCommandType::DrawPrecisePolygon, size));
  cmd->num_vertices = Truncate16(num_vertices);
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

GPUBackendDrawPreciseLineCommand* GPUBackend::NewDrawPreciseLineCommand(u32 num_vertices)
{
  const u32 size =
    sizeof(GPUBackendDrawPreciseLineCommand) + (num_vertices * sizeof(GPUBackendDrawPreciseLineCommand::Vertex));
  GPUBackendDrawPreciseLineCommand* cmd = static_cast<GPUBackendDrawPreciseLineCommand*>(
    GPUThread::AllocateCommand(GPUBackendCommandType::DrawPreciseLine, size));
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
  if (queued_frames <= g_settings.gpu_max_queued_frames)
    return false;

  if (g_settings.gpu_max_queued_frames > 0)
    DEV_LOG("<-- {} queued frames, {} max, blocking CPU thread", queued_frames, g_settings.gpu_max_queued_frames);

  s_cpu_thread_state.wait_state.store(CPUThreadState::WAIT_CPU_THREAD_WAITING, std::memory_order_release);
  return true;
}

void GPUBackend::WaitForOneQueuedFrame()
{
  // Inbetween this and the post call, we may have finished the frame. Check.
  if (s_cpu_thread_state.queued_frames.load(std::memory_order_acquire) <= g_settings.gpu_max_queued_frames)
  {
    // It's possible that the GPU thread has already signaled the semaphore.
    // If so, then we still need to drain it, otherwise waits in the future will return prematurely.
    u32 expected = CPUThreadState::WAIT_CPU_THREAD_WAITING;
    if (s_cpu_thread_state.wait_state.compare_exchange_strong(expected, CPUThreadState::WAIT_NONE,
                                                              std::memory_order_acq_rel, std::memory_order_acquire))
    {
      return;
    }
  }

  s_cpu_thread_state.gpu_thread_wait.Wait();

  // Depending on where the GPU thread is, now we can either be in WAIT_GPU_THREAD_SIGNALING or WAIT_GPU_THREAD_POSTED
  // state. We want to clear the flag here regardless, so a store-release is fine. Because the GPU thread has a
  // compare-exchange on WAIT_GPU_THREAD_SIGNALING, it can't "overwrite" the value we store here.
  s_cpu_thread_state.wait_state.store(CPUThreadState::WAIT_NONE, std::memory_order_release);

  // Sanity check: queued frames should be in range now. If they're not, we fucked up the semaphore.
  if (const u32 queued_frames = s_cpu_thread_state.queued_frames.load(std::memory_order_acquire);
      queued_frames > g_settings.gpu_max_queued_frames) [[unlikely]]
  {
    ERROR_LOG("queued_frames {} above max queued frames {} after CPU wait", queued_frames,
              g_settings.gpu_max_queued_frames);
  }
}

u32 GPUBackend::GetQueuedFrameCount()
{
  return s_cpu_thread_state.queued_frames.load(std::memory_order_acquire);
}

void GPUBackend::ReleaseQueuedFrame()
{
  s_cpu_thread_state.queued_frames.fetch_sub(1, std::memory_order_acq_rel);

  // We need two states here in case we get preempted in between the compare_exchange_strong() and Post().
  // This means that we will only release the semaphore once the CPU is guaranteed to be in a waiting state,
  // and ensure that we don't post twice if the CPU thread lags and we process 2 frames before it wakes up.
  u32 expected = CPUThreadState::WAIT_CPU_THREAD_WAITING;
  if (s_cpu_thread_state.wait_state.compare_exchange_strong(expected, CPUThreadState::WAIT_GPU_THREAD_SIGNALING,
                                                            std::memory_order_acq_rel, std::memory_order_acquire))
  {
    if (g_gpu_settings.gpu_max_queued_frames > 0)
      DEV_LOG("--> Unblocking CPU thread");

    s_cpu_thread_state.gpu_thread_wait.Post();

    // This needs to be a compare_exchange, because the CPU thread can clear the flag before we execute this line.
    expected = CPUThreadState::WAIT_GPU_THREAD_SIGNALING;
    s_cpu_thread_state.wait_state.compare_exchange_strong(expected, CPUThreadState::WAIT_GPU_THREAD_POSTED,
                                                          std::memory_order_acq_rel, std::memory_order_acquire);
  }
}

bool GPUBackend::AllocateMemorySaveStates(std::span<System::MemorySaveState> states, Error* error)
{
  bool result;
  GPUThread::RunOnBackend(
    [states, error, &result](GPUBackend* backend) {
      // Free old textures first.
      for (size_t i = 0; i < states.size(); i++)
        g_gpu_device->RecycleTexture(std::move(states[i].vram_texture));

      // Maximize potential for texture reuse by flushing the current command buffer.
      g_gpu_device->WaitForGPUIdle();

      for (size_t i = 0; i < states.size(); i++)
      {
        if (!backend->AllocateMemorySaveState(states[i], error))
        {
          // Try flushing the pool.
          WARNING_LOG("Failed to allocate memory save state texture, trying flushing pool.");
          g_gpu_device->PurgeTexturePool();
          g_gpu_device->WaitForGPUIdle();
          if (!backend->AllocateMemorySaveState(states[i], error))
          {
            // Free anything that was allocated.
            for (size_t j = 0; j <= i; i++)
            {
              states[j].state_data.deallocate();
              states[j].vram_texture.reset();
              result = false;
              return;
            }
          }
        }
      }

      backend->RestoreDeviceContext();
      result = true;
    },
    true, false);
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
      StateWrapper sw(mss.gpu_state_data.span(0, mss.gpu_state_size), StateWrapper::Mode::Read, SAVE_STATE_VERSION);
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

    case GPUBackendCommandType::ClearDisplay:
    {
      m_presenter.ClearDisplay();
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
               ccmd->color, ccmd->interlaced_rendering, ccmd->active_line_lsb);
    }
    break;

    case GPUBackendCommandType::UpdateVRAM:
    {
      const GPUBackendUpdateVRAMCommand* ccmd = static_cast<const GPUBackendUpdateVRAMCommand*>(cmd);
      s_counters.num_writes++;
      UpdateVRAM(ZeroExtend32(ccmd->x), ZeroExtend32(ccmd->y), ZeroExtend32(ccmd->width), ZeroExtend32(ccmd->height),
                 ccmd->data, ccmd->set_mask_while_drawing, ccmd->check_mask_before_draw);
    }
    break;

    case GPUBackendCommandType::CopyVRAM:
    {
      const GPUBackendCopyVRAMCommand* ccmd = static_cast<const GPUBackendCopyVRAMCommand*>(cmd);
      s_counters.num_copies++;
      CopyVRAM(ZeroExtend32(ccmd->src_x), ZeroExtend32(ccmd->src_y), ZeroExtend32(ccmd->dst_x),
               ZeroExtend32(ccmd->dst_y), ZeroExtend32(ccmd->width), ZeroExtend32(ccmd->height),
               ccmd->set_mask_while_drawing, ccmd->check_mask_before_draw);
    }
    break;

    case GPUBackendCommandType::SetDrawingArea:
    {
      const GPUBackendSetDrawingAreaCommand* ccmd = static_cast<const GPUBackendSetDrawingAreaCommand*>(cmd);
      GPU_SW_Rasterizer::g_drawing_area = ccmd->new_area;
      m_clamped_drawing_area = GPU::GetClampedDrawingArea(ccmd->new_area);
      DrawingAreaChanged();
    }
    break;

    case GPUBackendCommandType::UpdateCLUT:
    {
      const GPUBackendUpdateCLUTCommand* ccmd = static_cast<const GPUBackendUpdateCLUTCommand*>(cmd);
      GPU_SW_Rasterizer::UpdateCLUT(ccmd->reg, ccmd->clut_is_8bit);
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

    case GPUBackendCommandType::DrawPreciseLine:
    {
      const GPUBackendDrawPreciseLineCommand* ccmd = static_cast<const GPUBackendDrawPreciseLineCommand*>(cmd);
      s_counters.num_vertices += ccmd->num_vertices;
      s_counters.num_primitives += ccmd->num_vertices / 2;
      DrawPreciseLine(ccmd);
    }
    break;

      DefaultCaseIsUnreachable();
  }
}

void GPUBackend::HandleUpdateDisplayCommand(const GPUBackendUpdateDisplayCommand* cmd)
{
  // Height has to be doubled because we halved it on the GPU side.
  m_presenter.SetDisplayParameters(cmd->display_width, cmd->display_height, cmd->display_origin_left,
                                   cmd->display_origin_top, cmd->display_vram_width,
                                   cmd->display_vram_height << BoolToUInt32(cmd->interlaced_display_enabled),
                                   cmd->display_pixel_aspect_ratio, cmd->display_24bit);

  UpdateDisplay(cmd);
  if (cmd->submit_frame)
    HandleSubmitFrameCommand(&cmd->frame);
}

void GPUBackend::HandleSubmitFrameCommand(const GPUBackendFramePresentationParameters* cmd)
{
  // For regtest.
  Host::FrameDoneOnGPUThread(this, cmd->frame_number);

  if (cmd->media_capture)
    m_presenter.SendDisplayToMediaCapture(cmd->media_capture);

  // If this returns false, our backend object is deleted and replaced with null, so bail out.
  if (cmd->present_frame)
  {
    const bool result = m_presenter.PresentFrame(&m_presenter, this, cmd->allow_present_skip, cmd->present_time);
    ReleaseQueuedFrame();
    if (!result)
      return;
  }

  // Update perf counters *after* throttling, we want to measure from start-of-frame
  // to start-of-frame, not end-of-frame to end-of-frame (will be noisy due to different
  // amounts of computation happening in each frame).
  if (cmd->update_performance_counters)
    PerformanceCounters::Update(this, cmd->frame_number, cmd->internal_frame_number);

  RestoreDeviceContext();
}

void GPUBackend::GetStatsString(SmallStringBase& str) const
{
  if (IsUsingHardwareBackend())
  {
    if (g_gpu_settings.gpu_pgxp_depth_buffer)
    {
      str.format("\x02{}{} HW | \x01{}\x02 P | \x01{}\x02 DC | \x01{}\x02 B | \x01{}\x02 RP | \x01{}\x02 RB | "
                 "\x01{}\x02 C | \x01{}\x02 W | \x01{}\x02 DBC",
                 GPUDevice::RenderAPIToString(g_gpu_device->GetRenderAPI()), g_gpu_settings.gpu_use_thread ? "-MT" : "",
                 s_stats.num_primitives, s_stats.host_num_draws, s_stats.host_num_barriers,
                 s_stats.host_num_render_passes, s_stats.host_num_downloads, s_stats.num_copies, s_stats.num_writes,
                 s_stats.num_depth_buffer_clears);
    }
    else
    {
      str.format("\x02{}{} HW | \x01{}\x02 P | \x01{}\x02 DC | \x01{}\x02 B | \x01{}\x02 RP | \x01{}\x02 RB | "
                 "\x01{}\x02 C | \x01{}\x02 W",
                 GPUDevice::RenderAPIToString(g_gpu_device->GetRenderAPI()), g_gpu_settings.gpu_use_thread ? "-MT" : "",
                 s_stats.num_primitives, s_stats.host_num_draws, s_stats.host_num_barriers,
                 s_stats.host_num_render_passes, s_stats.host_num_downloads, s_stats.num_copies, s_stats.num_writes);
    }
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
  UPDATE_COUNTER(num_depth_buffer_clears);

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

bool GPUBackend::RenderScreenshotToBuffer(u32 width, u32 height, bool postfx, bool apply_aspect_ratio, Image* out_image,
                                          Error* error)
{
  bool result;
  GPUThread::RunOnBackend(
    [width, height, postfx, apply_aspect_ratio, out_image, error, &result](GPUBackend* backend) {
      if (!backend)
      {
        Error::SetStringView(error, "No GPU backend.");
        result = false;
        return;
      }

      // Post-processing requires that the size match the window.
      const bool really_postfx = postfx && g_gpu_device->HasMainSwapChain();
      u32 image_width, image_height;
      if (really_postfx)
      {
        image_width = g_gpu_device->GetMainSwapChain()->GetWidth();
        image_height = g_gpu_device->GetMainSwapChain()->GetHeight();
      }
      else
      {
        // Crop it if border overlay isn't enabled.
        GSVector4i draw_rect, display_rect;
        backend->GetPresenter().CalculateDrawRect(static_cast<s32>(width), static_cast<s32>(height), apply_aspect_ratio,
                                                  false, false, &display_rect, &draw_rect);
        image_width = static_cast<u32>(display_rect.width());
        image_height = static_cast<u32>(display_rect.height());
      }

      result = backend->GetPresenter().RenderScreenshotToBuffer(image_width, image_height, really_postfx,
                                                                apply_aspect_ratio, out_image, error);
      backend->RestoreDeviceContext();
    },
    true, false);

  return result;
}

void GPUBackend::RenderScreenshotToFile(const std::string_view path, DisplayScreenshotMode mode, u8 quality,
                                        bool show_osd_message)
{
  GPUThread::RunOnBackend(
    [path = std::string(path), mode, quality, show_osd_message](GPUBackend* backend) mutable {
      if (!backend)
        return;

      const GSVector2i size = backend->GetPresenter().CalculateScreenshotSize(mode);
      if (size.x == 0 || size.y == 0)
        return;

      std::string osd_key;
      if (show_osd_message)
        osd_key = fmt::format("ScreenshotSaver_{}", path);

      const bool internal_resolution = (mode != DisplayScreenshotMode::ScreenResolution);
      const bool apply_aspect_ratio = (mode != DisplayScreenshotMode::UncorrectedInternalResolution);
      Error error;
      Image image;
      if (!backend->m_presenter.RenderScreenshotToBuffer(size.x, size.y, !internal_resolution, apply_aspect_ratio,
                                                         &image, &error))
      {
        ERROR_LOG("Failed to render {}x{} screenshot: {}", size.x, size.y, error.GetDescription());
        if (show_osd_message)
        {
          Host::AddIconOSDWarning(
            std::move(osd_key), ICON_EMOJI_WARNING,
            fmt::format(TRANSLATE_FS("GPU", "Failed to save screenshot:\n{}"), error.GetDescription()));
        }

        backend->RestoreDeviceContext();
        return;
      }

      // no more GPU calls
      backend->RestoreDeviceContext();

      auto fp = FileSystem::OpenManagedCFile(path.c_str(), "wb", &error);
      if (!fp)
      {
        ERROR_LOG("Can't open file '{}': {}", Path::GetFileName(path), error.GetDescription());
        if (show_osd_message)
        {
          Host::AddIconOSDWarning(
            std::move(osd_key), ICON_EMOJI_WARNING,
            fmt::format(TRANSLATE_FS("GPU", "Failed to save screenshot:\n{}"), error.GetDescription()));
        }

        return;
      }

      if (show_osd_message)
      {
        // Use a 60 second timeout to give it plenty of time to actually save.
        Host::AddIconOSDMessage(osd_key, ICON_EMOJI_CAMERA_WITH_FLASH,
                                fmt::format(TRANSLATE_FS("GPU", "Saving screenshot to '{}'."), Path::GetFileName(path)),
                                60.0f);
      }

      System::QueueAsyncTask([path = std::move(path), fp = fp.release(), quality,
                              flip_y = g_gpu_device->UsesLowerLeftOrigin(), image = std::move(image),
                              osd_key = std::move(osd_key)]() mutable {
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
          image.SetAllPixelsOpaque();

          result = image.SaveToFile(path.c_str(), fp, quality, &error);
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

        std::fclose(fp);
        return result;
      });
    },
    false, false);
}

namespace {

class GPUNullBackend final : public GPUBackend
{
public:
  GPUNullBackend(GPUPresenter& presenter);
  ~GPUNullBackend() override;

  bool Initialize(bool upload_vram, Error* error) override;
  bool UpdateSettings(const GPUSettings& old_settings, Error* error) override;

  u32 GetResolutionScale() const override;

  void RestoreDeviceContext() override;
  void FlushRender() override;

  void ReadVRAM(u32 x, u32 y, u32 width, u32 height) override;
  void FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color, bool interlaced_rendering,
                u8 interlaced_display_field) override;
  void UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data, bool set_mask, bool check_mask) override;
  void CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height, bool set_mask,
                bool check_mask) override;

  void DrawPolygon(const GPUBackendDrawPolygonCommand* cmd) override;
  void DrawPrecisePolygon(const GPUBackendDrawPrecisePolygonCommand* cmd) override;
  void DrawSprite(const GPUBackendDrawRectangleCommand* cmd) override;
  void DrawLine(const GPUBackendDrawLineCommand* cmd) override;
  void DrawPreciseLine(const GPUBackendDrawPreciseLineCommand* cmd) override;

  void DrawingAreaChanged() override;
  void ClearCache() override;
  void OnBufferSwapped() override;
  void ClearVRAM() override;

  void UpdateDisplay(const GPUBackendUpdateDisplayCommand* cmd) override;

  void LoadState(const GPUBackendLoadStateCommand* cmd) override;

  bool AllocateMemorySaveState(System::MemorySaveState& mss, Error* error) override;
  void DoMemoryState(StateWrapper& sw, System::MemorySaveState& mss) override;
};

} // namespace

GPUNullBackend::GPUNullBackend(GPUPresenter& presenter) : GPUBackend(presenter)
{
}

GPUNullBackend::~GPUNullBackend() = default;

bool GPUNullBackend::Initialize(bool upload_vram, Error* error)
{
  return GPUBackend::Initialize(upload_vram, error);
}

bool GPUNullBackend::UpdateSettings(const GPUSettings& old_settings, Error* error)
{
  return GPUBackend::UpdateSettings(old_settings, error);
}

u32 GPUNullBackend::GetResolutionScale() const
{
  return 1;
}

void GPUNullBackend::RestoreDeviceContext()
{
}

void GPUNullBackend::FlushRender()
{
}

void GPUNullBackend::ReadVRAM(u32 x, u32 y, u32 width, u32 height)
{
}

void GPUNullBackend::FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color, bool interlaced_rendering,
                              u8 interlaced_display_field)
{
}

void GPUNullBackend::UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data, bool set_mask, bool check_mask)
{
}

void GPUNullBackend::CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height, bool set_mask,
                              bool check_mask)
{
}

void GPUNullBackend::DrawPolygon(const GPUBackendDrawPolygonCommand* cmd)
{
}

void GPUNullBackend::DrawPrecisePolygon(const GPUBackendDrawPrecisePolygonCommand* cmd)
{
}

void GPUNullBackend::DrawSprite(const GPUBackendDrawRectangleCommand* cmd)
{
}

void GPUNullBackend::DrawLine(const GPUBackendDrawLineCommand* cmd)
{
}

void GPUNullBackend::DrawPreciseLine(const GPUBackendDrawPreciseLineCommand* cmd)
{
}

void GPUNullBackend::DrawingAreaChanged()
{
}

void GPUNullBackend::ClearCache()
{
}

void GPUNullBackend::OnBufferSwapped()
{
}

void GPUNullBackend::ClearVRAM()
{
}

void GPUNullBackend::UpdateDisplay(const GPUBackendUpdateDisplayCommand* cmd)
{
}

void GPUNullBackend::LoadState(const GPUBackendLoadStateCommand* cmd)
{
}

bool GPUNullBackend::AllocateMemorySaveState(System::MemorySaveState& mss, Error* error)
{
  return false;
}

void GPUNullBackend::DoMemoryState(StateWrapper& sw, System::MemorySaveState& mss)
{
}

std::unique_ptr<GPUBackend> GPUBackend::CreateNullBackend(GPUPresenter& presenter)
{
  return std::make_unique<GPUNullBackend>(presenter);
}
