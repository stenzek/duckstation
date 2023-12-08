// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/types.h"

class GPUBackend;

namespace PerformanceCounters
{
static constexpr u32 NUM_FRAME_TIME_SAMPLES = 150;
using FrameTimeHistory = std::array<float, NUM_FRAME_TIME_SAMPLES>;

float GetFPS();
float GetVPS();
float GetEmulationSpeed();
float GetAverageFrameTime();
float GetMinimumFrameTime();
float GetMaximumFrameTime();
float GetCPUThreadUsage();
float GetCPUThreadAverageTime();
float GetGPUThreadUsage();
float GetGPUThreadAverageTime();
float GetGPUUsage();
float GetGPUAverageTime();
const FrameTimeHistory& GetFrameTimeHistory();
u32 GetFrameTimeHistoryPos();

void Clear();
void Reset();
void Update(GPUBackend* gpu, u32 frame_number, u32 internal_frame_number);
void AccumulateGPUTime();

} // namespace Host
