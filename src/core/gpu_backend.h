#pragma once
#include "common/event.h"
#include "common/heap_array.h"
#include "gpu_types.h"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324) // warning C4324: 'GPUBackend': structure was padded due to alignment specifier
#endif

class GPUBackend
{
public:
  GPUBackend();
  virtual ~GPUBackend();

  ALWAYS_INLINE u16* GetVRAM() const { return m_vram_ptr; }

  virtual bool Initialize(bool force_thread);
  virtual void UpdateSettings();
  virtual void Reset(bool clear_vram);
  virtual void Shutdown();

  GPUBackendFillVRAMCommand* NewFillVRAMCommand();
  GPUBackendUpdateVRAMCommand* NewUpdateVRAMCommand(u32 num_words);
  GPUBackendCopyVRAMCommand* NewCopyVRAMCommand();
  GPUBackendSetDrawingAreaCommand* NewSetDrawingAreaCommand();
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
  virtual void DrawingAreaChanged() = 0;

  void HandleCommand(const GPUBackendCommand* cmd);

  u16* m_vram_ptr = nullptr;

  Common::Rectangle<u32> m_drawing_area{};

  Common::Event m_sync_event;
  std::atomic_bool m_gpu_thread_sleeping{false};
  std::atomic_bool m_gpu_loop_done{false};
  std::thread m_gpu_thread;
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

  HeapArray<u8, COMMAND_QUEUE_SIZE> m_command_fifo_data;
  alignas(64) std::atomic<u32> m_command_fifo_read_ptr{0};
  alignas(64) std::atomic<u32> m_command_fifo_write_ptr{0};
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif
