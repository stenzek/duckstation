// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "performance_counters.h"
#include "gpu.h"
#include "gpu_backend.h"
#include "gpu_thread.h"
#include "system.h"
#include "system_private.h"

#include "util/media_capture.h"

#include "common/log.h"
#include "common/threading.h"
#include "common/timer.h"

#include <utility>

LOG_CHANNEL(PerfMon);

namespace PerformanceCounters {

namespace {

struct State
{
  Timer::Value last_update_time;
  Timer::Value last_frame_time;

  u32 last_frame_number;
  u32 last_internal_frame_number;
  u32 presents_since_last_update;

  float average_frame_time_accumulator;
  float minimum_frame_time_accumulator;
  float maximum_frame_time_accumulator;

  float vps;
  float fps;
  float speed;

  float minimum_frame_time;
  float maximum_frame_time;
  float average_frame_time;

  u64 last_cpu_time;
  float cpu_thread_usage;
  float cpu_thread_time;

  u64 last_gpu_thread_time;
  float gpu_thread_usage;
  float gpu_thread_time;

  float average_gpu_time;
  float accumulated_gpu_time;
  float gpu_usage;

  FrameTimeHistory frame_time_history;
  u32 frame_time_history_pos;
};

} // namespace

static constexpr const float PERFORMANCE_COUNTER_UPDATE_INTERVAL = 1.0f;

ALIGN_TO_CACHE_LINE State s_state = {};

} // namespace PerformanceCounters

float PerformanceCounters::GetFPS()
{
  return s_state.fps;
}

float PerformanceCounters::GetVPS()
{
  return s_state.vps;
}

float PerformanceCounters::GetEmulationSpeed()
{
  return s_state.speed;
}

float PerformanceCounters::GetAverageFrameTime()
{
  return s_state.average_frame_time;
}

float PerformanceCounters::GetMinimumFrameTime()
{
  return s_state.minimum_frame_time;
}

float PerformanceCounters::GetMaximumFrameTime()
{
  return s_state.maximum_frame_time;
}

float PerformanceCounters::GetCPUThreadUsage()
{
  return s_state.cpu_thread_usage;
}

float PerformanceCounters::GetCPUThreadAverageTime()
{
  return s_state.cpu_thread_time;
}

float PerformanceCounters::GetGPUThreadUsage()
{
  return s_state.gpu_thread_usage;
}

float PerformanceCounters::GetGPUThreadAverageTime()
{
  return s_state.gpu_thread_time;
}

float PerformanceCounters::GetGPUUsage()
{
  return s_state.gpu_usage;
}

float PerformanceCounters::GetGPUAverageTime()
{
  return s_state.average_gpu_time;
}

const PerformanceCounters::FrameTimeHistory& PerformanceCounters::GetFrameTimeHistory()
{
  return s_state.frame_time_history;
}

u32 PerformanceCounters::GetFrameTimeHistoryPos()
{
  return s_state.frame_time_history_pos;
}

void PerformanceCounters::Clear()
{
  s_state = {};
}

void PerformanceCounters::Reset()
{
  const Timer::Value now_ticks = Timer::GetCurrentValue();

  s_state.last_frame_time = now_ticks;
  s_state.last_update_time = now_ticks;

  s_state.last_frame_number = System::GetFrameNumber();
  s_state.last_internal_frame_number = System::GetInternalFrameNumber();
  s_state.last_cpu_time = System::GetCPUThreadHandle().GetCPUTime();
  s_state.last_gpu_thread_time = GPUThread::Internal::GetThreadHandle().GetCPUTime();

  s_state.average_frame_time_accumulator = 0.0f;
  s_state.minimum_frame_time_accumulator = 0.0f;
  s_state.maximum_frame_time_accumulator = 0.0f;

  std::atomic_thread_fence(std::memory_order_release);
}

void PerformanceCounters::Update(GPUBackend* gpu, u32 frame_number, u32 internal_frame_number)
{
  const Timer::Value now_ticks = Timer::GetCurrentValue();

  const float frame_time = static_cast<float>(
    Timer::ConvertValueToMilliseconds(now_ticks - std::exchange(s_state.last_frame_time, now_ticks)));
  s_state.minimum_frame_time_accumulator = (s_state.minimum_frame_time_accumulator == 0.0f) ?
                                             frame_time :
                                             std::min(s_state.minimum_frame_time_accumulator, frame_time);
  s_state.average_frame_time_accumulator += frame_time;
  s_state.maximum_frame_time_accumulator = std::max(s_state.maximum_frame_time_accumulator, frame_time);
  s_state.frame_time_history[s_state.frame_time_history_pos] = frame_time;
  s_state.frame_time_history_pos = (s_state.frame_time_history_pos + 1) % NUM_FRAME_TIME_SAMPLES;

  // update fps counter
  const Timer::Value ticks_diff = now_ticks - s_state.last_update_time;
  const float time = static_cast<float>(Timer::ConvertValueToSeconds(ticks_diff));
  if (time < PERFORMANCE_COUNTER_UPDATE_INTERVAL || s_state.last_frame_number == frame_number)
    return;

  s_state.last_update_time = now_ticks;

  const u32 frames_run = frame_number - std::exchange(s_state.last_frame_number, frame_number);
  const u32 internal_frames_run =
    internal_frame_number - std::exchange(s_state.last_internal_frame_number, internal_frame_number);
  const float frames_runf = static_cast<float>(frames_run);

  // TODO: Make the math here less rubbish
  const double pct_divider =
    100.0 * (1.0 / ((static_cast<double>(ticks_diff) * static_cast<double>(Threading::GetThreadTicksPerSecond())) /
                    Timer::GetFrequency() / 1000000000.0));
  const double time_divider = 1000.0 * (1.0 / static_cast<double>(Threading::GetThreadTicksPerSecond())) *
                              (1.0 / static_cast<double>(frames_runf));

  s_state.minimum_frame_time = std::exchange(s_state.minimum_frame_time_accumulator, 0.0f);
  s_state.average_frame_time = std::exchange(s_state.average_frame_time_accumulator, 0.0f) / frames_runf;
  s_state.maximum_frame_time = std::exchange(s_state.maximum_frame_time_accumulator, 0.0f);

  s_state.vps = static_cast<float>(frames_runf / time);
  s_state.fps = static_cast<float>(internal_frames_run) / time;
  s_state.speed = (s_state.vps / System::GetVideoFrameRate()) * 100.0f;

  const u64 cpu_time = System::GetCPUThreadHandle().GetCPUTime();
  const u64 gpu_thread_time = GPUThread::Internal::GetThreadHandle().GetCPUTime();
  const u64 cpu_delta = cpu_time - s_state.last_cpu_time;
  const u64 gpu_thread_delta = gpu_thread_time - s_state.last_gpu_thread_time;
  s_state.last_cpu_time = cpu_time;
  s_state.last_gpu_thread_time = gpu_thread_time;

  s_state.cpu_thread_usage = static_cast<float>(static_cast<double>(cpu_delta) * pct_divider);
  s_state.cpu_thread_time = static_cast<float>(static_cast<double>(cpu_delta) * time_divider);
  s_state.gpu_thread_usage = static_cast<float>(static_cast<double>(gpu_thread_delta) * pct_divider);
  s_state.gpu_thread_time = static_cast<float>(static_cast<double>(gpu_thread_delta) * time_divider);

  if (MediaCapture* cap = System::GetMediaCapture())
    cap->UpdateCaptureThreadUsage(pct_divider, time_divider);

  if (g_gpu_device->IsGPUTimingEnabled())
  {
    s_state.average_gpu_time =
      s_state.accumulated_gpu_time / static_cast<float>(std::max(s_state.presents_since_last_update, 1u));
    s_state.gpu_usage = s_state.accumulated_gpu_time / (time * 10.0f);
  }
  s_state.accumulated_gpu_time = 0.0f;
  s_state.presents_since_last_update = 0;

  if (g_settings.display_show_gpu_stats)
    gpu->UpdateStatistics(frames_run);

  VERBOSE_LOG("FPS: {:.2f} VPS: {:.2f} CPU: {:.2f} RNDR: {:.2f} GPU: {:.2f} Avg: {:.2f}ms Min: {:.2f}ms Max: {:.2f}ms",
              s_state.fps, s_state.vps, s_state.cpu_thread_usage, s_state.gpu_thread_usage, s_state.gpu_usage,
              s_state.average_frame_time, s_state.minimum_frame_time, s_state.maximum_frame_time);

  Host::OnPerformanceCountersUpdated(gpu);
}

void PerformanceCounters::AccumulateGPUTime()
{
  s_state.accumulated_gpu_time += g_gpu_device->GetAndResetAccumulatedGPUTime();
  s_state.presents_since_last_update++;
}
