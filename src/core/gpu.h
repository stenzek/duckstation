// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "gpu_types.h"
#include "types.h"

#include "common/gsvector.h"

#include <algorithm>
#include <span>
#include <string>
#include <utility>

class Error;

class StateWrapper;

namespace GPUDump {
enum class PacketType : u8;
class Recorder;
} // namespace GPUDump

struct Settings;

namespace System {
struct MemorySaveState;
}

namespace GPU {

/// The maximum resolution scale factor that can be applied to rendering.
inline constexpr u32 MAX_RESOLUTION_SCALE = 32;

void Initialize();
void Shutdown();
void Reset(bool clear_vram);
bool DoState(StateWrapper& sw);
void DoMemoryState(StateWrapper& sw, System::MemorySaveState& mss);

// Render statistics debug window.
void DrawDebugStateWindow(float scale);

void CPUClockChanged();

// MMIO access
u32 ReadRegister(u32 offset);
void WriteRegister(u32 offset, u32 value);

// DMA access
void DMARead(u32* RESTRICT words, u32 word_count);
void DMAWrite(const u32* RESTRICT words, u32 address, u32 increment, u32 word_count);

/// Writing to GPU dump.
GPUDump::Recorder* GetGPUDump();
bool StartRecordingGPUDump(const char* path, u32 num_frames = 1);
void StopRecordingGPUDump();
void WriteCurrentVideoModeToDump(GPUDump::Recorder* dump);
void ProcessGPUDumpPacket(GPUDump::PacketType type, const std::span<const u32> data);

/// Returns true if scanout should be interlaced.
bool IsInterlacedDisplayEnabled();

/// Returns true if scanout is forced to progressive.
bool IsProgressiveDisplayScanForced();

/// Returns true if we're in PAL mode, otherwise false if NTSC.
bool IsInPALMode();

/// Returns true if enough ticks have passed for the raster to be on the next line.
bool IsCRTCScanlinePending();

/// Synchronizes the CRTC, updating the hblank timer.
void SynchronizeCRTC();

/// Recompile shaders/recreate framebuffers when needed.
void UpdateSettings(const Settings& old_settings);

/// Returns the full display resolution of the GPU, including padding.
std::pair<u32, u32> GetFullDisplayResolution();

/// Computes clamped drawing area.
GSVector4i GetClampedDrawingArea(const GPUDrawingArea& drawing_area);

/// Computes the pixel aspect ratio based on the current display mode and settings.
float ComputePixelAspectRatio();

/// Computes aspect ratio correction, i.e. the scale to apply to the source aspect ratio to preserve
/// the original pixel aspect ratio regardless of how much cropping has been applied.
float ComputeAspectRatioCorrection();

/// Applies the pixel aspect ratio to a given size, preserving the larger dimension.
GSVector2 CalculateRenderWindowSize(DisplayFineCropMode mode, std::span<const s16, 4> amount, float pixel_aspect_ratio,
                                    const GSVector2 video_size, const GSVector2 source_size,
                                    const GSVector2 window_size);

// Converts window coordinates into horizontal ticks and scanlines. Returns -1 if out of range. Used for lightguns.
GSVector2 ConvertScreenCoordinatesToDisplayCoordinates(GSVector2 window_pos);
bool ConvertDisplayCoordinatesToBeamTicksAndLines(const GSVector2& display_pos, float x_scale, u32* out_tick,
                                                  u32* out_line);

// Returns the current beam position.
void GetBeamPosition(u32* out_ticks, u32* out_line);

// Returns the number of system clock ticks until the specified tick/line.
TickCount GetSystemTicksUntilTicksAndLine(u32 ticks, u32 line);

// Returns the number of visible lines.
u16 GetCRTCActiveStartLine();
u16 GetCRTCActiveEndLine();

// Returns the video clock frequency.
TickCount GetCRTCFrequency();

// Video output access.
GSVector2i GetCRTCVideoSize();
GSVector4i GetCRTCVideoActiveRect();
GSVector4i GetCRTCVRAMSourceRect();

// Dumps raw VRAM to a file.
bool DumpVRAMToFile(std::string path, Error* error);

// Kicks the current frame to the backend for display.
void UpdateDisplay(bool submit_frame);

// Queues the current frame for presentation. Should only be used with runahead.
void QueuePresentCurrentFrame();

/// Computes the effective resolution scale when it is set to automatic.
u8 CalculateAutomaticResolutionScale();

/// Helper function for computing the draw rectangle in a larger window.
void CalculateDrawRect(const GSVector2i& window_size, const GSVector2i& video_size, const GSVector4i& video_active_rect,
                       const GSVector4i& source_rect, DisplayRotation rotation, DisplayAlignment alignment,
                       float pixel_aspect_ratio, bool integer_scale, DisplayFineCropMode fine_crop,
                       const std::span<const s16, 4>& fine_crop_amount, GSVector4i* out_source_rect,
                       GSVector4i* out_display_rect, GSVector4i* out_draw_rect, GSVector4* out_crop_amount = nullptr);
}; // namespace GPU

extern u16 g_vram[VRAM_SIZE / sizeof(u16)];
extern u16 g_gpu_clut[GPU_CLUT_SIZE];
