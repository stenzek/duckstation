// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gpu_backend.h"

#include "util/state_wrapper.h"

#include "common/align.h"
#include "common/log.h"
#include "common/timer.h"

LOG_CHANNEL(GPU);

std::unique_ptr<GPUBackend> g_gpu_backend;

GPUBackend::GPUBackend() = default;

GPUBackend::~GPUBackend() = default;

bool GPUBackend::Initialize(bool use_thread)
{
  if (use_thread)
    StartGPUThread();

  return true;
}

void GPUBackend::Reset()
{
  Sync(true);
  DrawingAreaChanged(GPUDrawingArea{0, 0, 0, 0}, GSVector4i::zero());
}

void GPUBackend::SetThreadEnabled(bool use_thread)
{
  Sync(true);

  if (m_use_gpu_thread != use_thread)
  {
    if (!use_thread)
      StopGPUThread();
    else
      StartGPUThread();
  }
}

void GPUBackend::Shutdown()
{
  StopGPUThread();
}

GPUBackendFillVRAMCommand* GPUBackend::NewFillVRAMCommand()
{
  return static_cast<GPUBackendFillVRAMCommand*>(
    AllocateCommand(GPUBackendCommandType::FillVRAM, sizeof(GPUBackendFillVRAMCommand)));
}

GPUBackendUpdateVRAMCommand* GPUBackend::NewUpdateVRAMCommand(u32 num_words)
{
  const u32 size = sizeof(GPUBackendUpdateVRAMCommand) + (num_words * sizeof(u16));
  GPUBackendUpdateVRAMCommand* cmd =
    static_cast<GPUBackendUpdateVRAMCommand*>(AllocateCommand(GPUBackendCommandType::UpdateVRAM, size));
  return cmd;
}

GPUBackendCopyVRAMCommand* GPUBackend::NewCopyVRAMCommand()
{
  return static_cast<GPUBackendCopyVRAMCommand*>(
    AllocateCommand(GPUBackendCommandType::CopyVRAM, sizeof(GPUBackendCopyVRAMCommand)));
}

GPUBackendSetDrawingAreaCommand* GPUBackend::NewSetDrawingAreaCommand()
{
  return static_cast<GPUBackendSetDrawingAreaCommand*>(
    AllocateCommand(GPUBackendCommandType::SetDrawingArea, sizeof(GPUBackendSetDrawingAreaCommand)));
}

GPUBackendUpdateCLUTCommand* GPUBackend::NewUpdateCLUTCommand()
{
  return static_cast<GPUBackendUpdateCLUTCommand*>(
    AllocateCommand(GPUBackendCommandType::UpdateCLUT, sizeof(GPUBackendUpdateCLUTCommand)));
}

GPUBackendDrawPolygonCommand* GPUBackend::NewDrawPolygonCommand(u32 num_vertices)
{
  const u32 size = sizeof(GPUBackendDrawPolygonCommand) + (num_vertices * sizeof(GPUBackendDrawPolygonCommand::Vertex));
  GPUBackendDrawPolygonCommand* cmd =
    static_cast<GPUBackendDrawPolygonCommand*>(AllocateCommand(GPUBackendCommandType::DrawPolygon, size));
  cmd->num_vertices = Truncate16(num_vertices);
  return cmd;
}

GPUBackendDrawRectangleCommand* GPUBackend::NewDrawRectangleCommand()
{
  return static_cast<GPUBackendDrawRectangleCommand*>(
    AllocateCommand(GPUBackendCommandType::DrawRectangle, sizeof(GPUBackendDrawRectangleCommand)));
}

GPUBackendDrawLineCommand* GPUBackend::NewDrawLineCommand(u32 num_vertices)
{
  const u32 size = sizeof(GPUBackendDrawLineCommand) + (num_vertices * sizeof(GPUBackendDrawLineCommand::Vertex));
  GPUBackendDrawLineCommand* cmd =
    static_cast<GPUBackendDrawLineCommand*>(AllocateCommand(GPUBackendCommandType::DrawLine, size));
  cmd->num_vertices = Truncate16(num_vertices);
  return cmd;
}

void* GPUBackend::AllocateCommand(GPUBackendCommandType command, u32 size)
{
  // Ensure size is a multiple of 4 so we don't end up with an unaligned command.
  size = Common::AlignUpPow2(size, 4);

  for (;;)
  {
    u32 read_ptr = m_command_fifo_read_ptr.load();
    u32 write_ptr = m_command_fifo_write_ptr.load();
    if (read_ptr > write_ptr)
    {
      u32 available_size = read_ptr - write_ptr;
      while (available_size < (size + sizeof(GPUBackendCommandType)))
      {
        WakeGPUThread();
        read_ptr = m_command_fifo_read_ptr.load();
        available_size = (read_ptr > write_ptr) ? (read_ptr - write_ptr) : (COMMAND_QUEUE_SIZE - write_ptr);
      }
    }
    else
    {
      const u32 available_size = COMMAND_QUEUE_SIZE - write_ptr;
      if ((size + sizeof(GPUBackendCommand)) > available_size)
      {
        // allocate a dummy command to wrap the buffer around
        GPUBackendCommand* dummy_cmd = reinterpret_cast<GPUBackendCommand*>(&m_command_fifo_data[write_ptr]);
        dummy_cmd->type = GPUBackendCommandType::Wraparound;
        dummy_cmd->size = available_size;
        dummy_cmd->params.bits = 0;
        m_command_fifo_write_ptr.store(0);
        continue;
      }
    }

    GPUBackendCommand* cmd = reinterpret_cast<GPUBackendCommand*>(&m_command_fifo_data[write_ptr]);
    cmd->type = command;
    cmd->size = size;
    return cmd;
  }
}

u32 GPUBackend::GetPendingCommandSize() const
{
  const u32 read_ptr = m_command_fifo_read_ptr.load();
  const u32 write_ptr = m_command_fifo_write_ptr.load();
  return (write_ptr >= read_ptr) ? (write_ptr - read_ptr) : (COMMAND_QUEUE_SIZE - read_ptr + write_ptr);
}

void GPUBackend::PushCommand(GPUBackendCommand* cmd)
{
  if (!m_use_gpu_thread)
  {
    // single-thread mode
    if (cmd->type != GPUBackendCommandType::Sync)
      HandleCommand(cmd);
  }
  else
  {
    const u32 new_write_ptr = m_command_fifo_write_ptr.fetch_add(cmd->size) + cmd->size;
    DebugAssert(new_write_ptr <= COMMAND_QUEUE_SIZE);
    UNREFERENCED_VARIABLE(new_write_ptr);
    if (GetPendingCommandSize() >= THRESHOLD_TO_WAKE_GPU)
      WakeGPUThread();
  }
}

void GPUBackend::WakeGPUThread()
{
  std::unique_lock<std::mutex> lock(m_sync_mutex);
  if (!m_gpu_thread_sleeping.load())
    return;

  m_wake_gpu_thread_cv.notify_one();
}

void GPUBackend::StartGPUThread()
{
  m_gpu_loop_done.store(false);
  m_use_gpu_thread = true;
  m_gpu_thread.Start([this]() { RunGPULoop(); });
  INFO_LOG("GPU thread started.");
}

void GPUBackend::StopGPUThread()
{
  if (!m_use_gpu_thread)
    return;

  m_gpu_loop_done.store(true);
  WakeGPUThread();
  m_gpu_thread.Join();
  m_use_gpu_thread = false;
  INFO_LOG("GPU thread stopped.");
}

void GPUBackend::Sync(bool allow_sleep)
{
  if (!m_use_gpu_thread)
    return;

  GPUBackendSyncCommand* cmd =
    static_cast<GPUBackendSyncCommand*>(AllocateCommand(GPUBackendCommandType::Sync, sizeof(GPUBackendSyncCommand)));
  cmd->allow_sleep = allow_sleep;
  PushCommand(cmd);
  WakeGPUThread();

  m_sync_semaphore.Wait();
}

void GPUBackend::RunGPULoop()
{
  static constexpr double SPIN_TIME_NS = 1 * 1000000;
  Timer::Value last_command_time = 0;

  for (;;)
  {
    u32 write_ptr = m_command_fifo_write_ptr.load();
    u32 read_ptr = m_command_fifo_read_ptr.load();
    if (read_ptr == write_ptr)
    {
      const Timer::Value current_time = Timer::GetCurrentValue();
      if (Timer::ConvertValueToNanoseconds(current_time - last_command_time) < SPIN_TIME_NS)
        continue;

      std::unique_lock<std::mutex> lock(m_sync_mutex);
      m_gpu_thread_sleeping.store(true);
      m_wake_gpu_thread_cv.wait(lock, [this]() { return m_gpu_loop_done.load() || GetPendingCommandSize() > 0; });
      m_gpu_thread_sleeping.store(false);

      if (m_gpu_loop_done.load())
        break;
      else
        continue;
    }

    if (write_ptr < read_ptr)
      write_ptr = COMMAND_QUEUE_SIZE;

    bool allow_sleep = false;
    while (read_ptr < write_ptr)
    {
      const GPUBackendCommand* cmd = reinterpret_cast<const GPUBackendCommand*>(&m_command_fifo_data[read_ptr]);
      read_ptr += cmd->size;

      switch (cmd->type)
      {
        case GPUBackendCommandType::Wraparound:
        {
          DebugAssert(read_ptr == COMMAND_QUEUE_SIZE);
          write_ptr = m_command_fifo_write_ptr.load();
          read_ptr = 0;
        }
        break;

        case GPUBackendCommandType::Sync:
        {
          DebugAssert(read_ptr == write_ptr);
          m_sync_semaphore.Post();
          allow_sleep = static_cast<const GPUBackendSyncCommand*>(cmd)->allow_sleep;
        }
        break;

        default:
          HandleCommand(cmd);
          break;
      }
    }

    last_command_time = allow_sleep ? 0 : Timer::GetCurrentValue();
    m_command_fifo_read_ptr.store(read_ptr);
  }
}

void GPUBackend::HandleCommand(const GPUBackendCommand* cmd)
{
  switch (cmd->type)
  {
    case GPUBackendCommandType::FillVRAM:
    {
      FlushRender();
      const GPUBackendFillVRAMCommand* ccmd = static_cast<const GPUBackendFillVRAMCommand*>(cmd);
      FillVRAM(ZeroExtend32(ccmd->x), ZeroExtend32(ccmd->y), ZeroExtend32(ccmd->width), ZeroExtend32(ccmd->height),
               ccmd->color, ccmd->params);
    }
    break;

    case GPUBackendCommandType::UpdateVRAM:
    {
      FlushRender();
      const GPUBackendUpdateVRAMCommand* ccmd = static_cast<const GPUBackendUpdateVRAMCommand*>(cmd);
      UpdateVRAM(ZeroExtend32(ccmd->x), ZeroExtend32(ccmd->y), ZeroExtend32(ccmd->width), ZeroExtend32(ccmd->height),
                 ccmd->data, ccmd->params);
    }
    break;

    case GPUBackendCommandType::CopyVRAM:
    {
      FlushRender();
      const GPUBackendCopyVRAMCommand* ccmd = static_cast<const GPUBackendCopyVRAMCommand*>(cmd);
      CopyVRAM(ZeroExtend32(ccmd->src_x), ZeroExtend32(ccmd->src_y), ZeroExtend32(ccmd->dst_x),
               ZeroExtend32(ccmd->dst_y), ZeroExtend32(ccmd->width), ZeroExtend32(ccmd->height), ccmd->params);
    }
    break;

    case GPUBackendCommandType::SetDrawingArea:
    {
      FlushRender();
      const GPUBackendSetDrawingAreaCommand* ccmd = static_cast<const GPUBackendSetDrawingAreaCommand*>(cmd);
      DrawingAreaChanged(ccmd->new_area, GSVector4i::load<false>(ccmd->new_clamped_area));
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
      DrawPolygon(static_cast<const GPUBackendDrawPolygonCommand*>(cmd));
    }
    break;

    case GPUBackendCommandType::DrawRectangle:
    {
      DrawRectangle(static_cast<const GPUBackendDrawRectangleCommand*>(cmd));
    }
    break;

    case GPUBackendCommandType::DrawLine:
    {
      DrawLine(static_cast<const GPUBackendDrawLineCommand*>(cmd));
    }
    break;

    default:
      UnreachableCode();
  }
}
