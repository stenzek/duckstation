// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/types.h"

class GPUBackend;

namespace PerformanceCounters {

inline constexpr u32 NUM_FRAME_TIME_SAMPLES = 152;
using FrameTimeHistory = std::array<float, NUM_FRAME_TIME_SAMPLES>;

float GetFPS();
float GetVPS();
float GetEmulationSpeed();
float GetAverageFrameTime();
float GetMinimumFrameTime();
float GetMaximumFrameTime();
float GetCoreThreadUsage();
float GetCoreThreadAverageTime();
float GetVideoThreadUsage();
float GetVideoThreadAverageTime();
float GetGPUUsage();
float GetGPUAverageTime();
const FrameTimeHistory& GetFrameTimeHistory();
u32 GetFrameTimeHistoryPos();

void Clear();
void Reset();
void Update(GPUBackend* gpu, u32 frame_number, u32 internal_frame_number);
void AccumulateGPUTime();

} // namespace PerformanceCounters
