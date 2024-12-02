// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "gpu_types.h"

#include "common/heap_array.h"
#include "common/threading.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324) // warning C4324: 'GPUBackend': structure was padded due to alignment specifier
#endif

class GPUBackend
{
public:
  GPUBackend();
  virtual ~GPUBackend();

  ALWAYS_INLINE const Threading::Thread* GetThread() const { return m_use_gpu_thread ? &m_gpu_thread : nullptr; }
  ALWAYS_INLINE bool IsUsingThread() const { return m_use_gpu_thread; }

  virtual bool Initialize(bool use_thread);
  virtual void Reset();
  virtual void Shutdown();

  void SetThreadEnabled(bool use_thread);

  GPUBackendFillVRAMCommand* NewFillVRAMCommand();
  GPUBackendUpdateVRAMCommand* NewUpdateVRAMCommand(u32 num_words);
  GPUBackendCopyVRAMCommand* NewCopyVRAMCommand();
  GPUBackendSetDrawingAreaCommand* NewSetDrawingAreaCommand();
  GPUBackendUpdateCLUTCommand* NewUpdateCLUTCommand();
  GPUBackendDrawPolygonCommand* NewDrawPolygonCommand(u32 num_vertices);
  GPUBackendDrawRectangleCommand* NewDrawRectangleCommand();
  GPUBackendDrawLineCommand* NewDrawLineCommand(u32 num_vertices);

  void PushCommand(GPUBackendCommand* cmd);
  void Sync(bool allow_sleep);

  /// Processes all pending GPU commands.
  void RunGPULoop();

protected:
  void* AllocateCommand(GPUBackendCommandType command, u32 size);
  u32 GetPendingCommandSize() const;
  void WakeGPUThread();
  void StartGPUThread();
  void StopGPUThread();

  virtual void FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color, GPUBackendCommandParameters params) = 0;
  virtual void UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data,
                          GPUBackendCommandParameters params) = 0;
  virtual void CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height,
                        GPUBackendCommandParameters params) = 0;
  virtual void DrawPolygon(const GPUBackendDrawPolygonCommand* cmd) = 0;
  virtual void DrawRectangle(const GPUBackendDrawRectangleCommand* cmd) = 0;
  virtual void DrawLine(const GPUBackendDrawLineCommand* cmd) = 0;
  virtual void FlushRender() = 0;
  virtual void DrawingAreaChanged(const GPUDrawingArea& new_drawing_area, const GSVector4i clamped_drawing_area) = 0;
  virtual void UpdateCLUT(GPUTexturePaletteReg reg, bool clut_is_8bit) = 0;

  void HandleCommand(const GPUBackendCommand* cmd);

  Threading::KernelSemaphore m_sync_semaphore;
  std::atomic_bool m_gpu_thread_sleeping{false};
  std::atomic_bool m_gpu_loop_done{false};
  Threading::Thread m_gpu_thread;
  bool m_use_gpu_thread = false;

  std::mutex m_sync_mutex;
  std::condition_variable m_sync_cpu_thread_cv;
  std::condition_variable m_wake_gpu_thread_cv;
  bool m_sync_done = false;

  enum : u32
  {
    COMMAND_QUEUE_SIZE = 4 * 1024 * 1024,
    THRESHOLD_TO_WAKE_GPU = 256
  };

  FixedHeapArray<u8, COMMAND_QUEUE_SIZE> m_command_fifo_data;
  alignas(HOST_CACHE_LINE_SIZE) std::atomic<u32> m_command_fifo_read_ptr{0};
  alignas(HOST_CACHE_LINE_SIZE) std::atomic<u32> m_command_fifo_write_ptr{0};
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif
