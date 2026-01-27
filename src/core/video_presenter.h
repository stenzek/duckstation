// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "types.h"

#include "util/gpu_types.h"

#include "common/gsvector.h"

#include <memory>
#include <string>
#include <vector>

class Error;
class Image;
class MediaCapture;
class GPUTexture;
class SettingsInterface;

enum class DisplayScreenshotMode : u8;
enum class WindowInfoPrerotation : u8;

class GPUBackend;

struct GPUSettings;

namespace VideoPresenter {

const GSVector2i& GetVideoSize();
GPUTexture* GetDisplayTexture();
const GSVector4i& GetDisplayTextureRect();
bool HasDisplayTexture();

bool IsInitialized();
bool Initialize(Error* error);
void Shutdown();

bool UpdateSettings(const GPUSettings& old_settings, Error* error);

void ClearDisplay();
void ClearDisplayTexture();
void SetDisplayParameters(const GSVector2i& video_size, const GSVector4i& video_active_rect,
                          float display_pixel_aspect_ratio, bool display_24bit);
void SetDisplayTexture(GPUTexture* texture, const GSVector4i& source_rect);
bool Deinterlace(u32 field);
bool ApplyChromaSmoothing();

/// Helper function for computing the draw rectangle in a larger window.
void CalculateDrawRect(const GSVector2i& window_size, bool apply_aspect_ratio, bool integer_scale, bool apply_crop,
                       bool apply_alignment, GSVector4i* source_rect, GSVector4i* display_rect, GSVector4i* draw_rect);

/// Helper function for computing screenshot bounds.
GSVector2i CalculateScreenshotSize(DisplayScreenshotMode mode);

/// Renders the display, optionally with postprocessing to the specified image.
bool RenderScreenshotToBuffer(u32 width, u32 height, bool postfx, bool apply_aspect_ratio, Image* out_image,
                              Error* error);

/// Sends the current frame to media capture.
void SendDisplayToMediaCapture(MediaCapture* cap);

/// Returns true if the frame scheduled for presentation at the specified time should be presented.
bool ShouldPresentFrame(u64 present_time);

/// Sets the present skip mode.
void SetPresentSkipMode(PresentSkipMode mode);

/// Limits presentation speed when running with no fixed rate, e.g. FullscreenUI open.
void ThrottlePresentation();

/// Main frame presenter - used both when a game is and is not running.
bool PresentFrame(GPUBackend* backend, u64 present_time);

/// Returns a list of border overlay presets.
std::vector<std::string> EnumerateBorderOverlayPresets();

/// Returns the settings interface to use for loading post-processing shader configuration.
/// Assumes the settings lock is being held.
SettingsInterface& GetPostProcessingSettingsInterface(const char* section);

/// Toggles post-processing. Only callable from the CPU thread.
void TogglePostProcessing();

/// Reloads post-processing settings. Only callable from the CPU thread.
void ReloadPostProcessingSettings(bool display, bool internal, bool reload_shaders);

// Draws the specified bounding box with display rotation and pre-rotation.
void DrawScreenQuad(const GSVector4i rect, const GSVector4 uv_rect, const GSVector2i target_size,
                    const GSVector2i final_target_size, DisplayRotation uv_rotation, WindowInfoPrerotation prerotation,
                    const void* push_constants, u32 push_constants_size);

} // namespace VideoPresenter

namespace Host {

/// Called at the end of the frame, before presentation.
void FrameDoneOnVideoThread(u32 frame_number);

} // namespace Host
