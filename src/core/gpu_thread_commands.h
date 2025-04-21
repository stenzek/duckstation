// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "gpu_types.h"
#include "settings.h"

#include "common/align.h"

#include <functional>
#include <string>
#include <vector>

class Error;

enum class GPUVSyncMode : u8;
class MediaCapture;
class StateWrapper;

class GPUBackend;

namespace System {
struct MemorySaveState;
}

enum class GPUBackendCommandType : u8
{
  Wraparound,
  AsyncCall,
  AsyncBackendCall,
  Reconfigure,
  UpdateSettings,
  Shutdown,
  ClearVRAM,
  ClearDisplay,
  UpdateDisplay,
  SubmitFrame,
  BufferSwapped,
  LoadState,
  LoadMemoryState,
  SaveMemoryState,
  ReadVRAM,
  FillVRAM,
  UpdateVRAM,
  CopyVRAM,
  SetDrawingArea,
  UpdateCLUT,
  ClearCache,
  DrawPolygon,
  DrawPrecisePolygon,
  DrawRectangle,
  DrawLine,
  DrawPreciseLine,
};

struct GPUThreadCommand
{
  u32 size;
  GPUBackendCommandType type;

  static constexpr u32 AlignCommandSize(u32 size)
  {
    // Ensure size is a multiple of 16 (minimum data size) so we don't end up with an unaligned command.
    constexpr u32 COMMAND_QUEUE_ALLOCATION_ALIGNMENT = 16;
    return Common::AlignUpPow2(size, COMMAND_QUEUE_ALLOCATION_ALIGNMENT);
  }
};

struct GPUThreadReconfigureCommand : public GPUThreadCommand
{
  Error* error_ptr;
  bool* out_result;
  std::string game_serial;
  std::optional<GPURenderer> renderer;
  std::optional<bool> fullscreen;
  std::optional<bool> start_fullscreen_ui;
  GPUVSyncMode vsync_mode;
  bool allow_present_throttle;
  bool force_recreate_device;
  bool upload_vram;
  GPUSettings settings;
};

struct GPUThreadUpdateSettingsCommand : public GPUThreadCommand
{
  GPUThreadUpdateSettingsCommand(const GPUSettings& settings_) : settings(settings_) {}

  GPUSettings settings;
};

struct GPUThreadAsyncCallCommand : public GPUThreadCommand
{
  GPUThreadAsyncCallCommand() = default;
  GPUThreadAsyncCallCommand(std::function<void()> func_) : func(std::move(func_)) {}

  std::function<void()> func;
};

struct GPUThreadAsyncBackendCallCommand : public GPUThreadCommand
{
  GPUThreadAsyncBackendCallCommand(std::function<void(GPUBackend*)> func_) : func(std::move(func_)) {}

  std::function<void(GPUBackend*)> func;
};

struct GPUBackendLoadStateCommand : public GPUThreadCommand
{
  u16 vram_data[VRAM_WIDTH * VRAM_HEIGHT];
  u16 clut_data[GPU_CLUT_SIZE];
  u32 texture_cache_state_version;
  u32 texture_cache_state_size;
  u8 texture_cache_state[0]; // texture_cache_state_size
};

struct GPUBackendDoMemoryStateCommand : public GPUThreadCommand
{
  System::MemorySaveState* memory_save_state;
};

struct GPUBackendFramePresentationParameters
{
  u32 frame_number;
  u32 internal_frame_number;

  u64 present_time;
  MediaCapture* media_capture;

  union
  {
    u8 bits;

    BitField<u16, bool, 0, 1> allow_present_skip;
    BitField<u16, bool, 1, 1> present_frame;
    BitField<u16, bool, 2, 1> update_performance_counters;
  };
};

struct GPUBackendUpdateDisplayCommand : public GPUThreadCommand
{
  u16 display_width;
  u16 display_height;
  u16 display_origin_left;
  u16 display_origin_top;
  u16 display_vram_left;
  u16 display_vram_top;
  u16 display_vram_width;
  u16 display_vram_height;
  float display_pixel_aspect_ratio;

  u16 X; // TODO: Can we get rid of this?

  bool interlaced_display_enabled : 1;
  bool interlaced_display_field : 1;
  bool interlaced_display_interleaved : 1;
  bool interleaved_480i_mode : 1;
  bool display_24bit : 1;
  bool display_disabled : 1;
  bool submit_frame : 1;
  bool : 1;

  GPUBackendFramePresentationParameters frame;
};

// Only used for runahead.
struct GPUBackendSubmitFrameCommand : public GPUThreadCommand
{
  GPUBackendFramePresentationParameters frame;
};

struct GPUBackendReadVRAMCommand : public GPUThreadCommand
{
  u16 x;
  u16 y;
  u16 width;
  u16 height;
};

struct GPUBackendFillVRAMCommand : public GPUThreadCommand
{
  u16 x;
  u16 y;
  u16 width;
  u16 height;
  u32 color;
  bool interlaced_rendering;
  u8 active_line_lsb;
};

struct GPUBackendUpdateVRAMCommand : public GPUThreadCommand
{
  u16 x;
  u16 y;
  u16 width;
  u16 height;
  bool set_mask_while_drawing;
  bool check_mask_before_draw;
  u16 data[0];
};

struct GPUBackendCopyVRAMCommand : public GPUThreadCommand
{
  u16 src_x;
  u16 src_y;
  u16 dst_x;
  u16 dst_y;
  u16 width;
  u16 height;
  bool set_mask_while_drawing;
  bool check_mask_before_draw;
};

struct GPUBackendSetDrawingAreaCommand : public GPUThreadCommand
{
  GPUDrawingArea new_area;
};

struct GPUBackendUpdateCLUTCommand : public GPUThreadCommand
{
  GPUTexturePaletteReg reg;
  bool clut_is_8bit;
};

struct GPUBackendDrawCommand : public GPUThreadCommand
{
  bool interlaced_rendering : 1;

  /// Returns 0 if the currently-displayed field is on an even line in VRAM, otherwise 1.
  bool active_line_lsb : 1;

  bool set_mask_while_drawing : 1;
  bool check_mask_before_draw : 1;

  bool texture_enable : 1;
  bool raw_texture_enable : 1;
  bool transparency_enable : 1;
  bool shading_enable : 1;
  bool quad_polygon : 1;
  bool dither_enable : 1;

  bool valid_w : 1; // only used for precise polygons

  // During transfer/render operations, if ((dst_pixel & mask_and) == 0) { pixel = src_pixel | mask_or }
  ALWAYS_INLINE u16 GetMaskAND() const { return check_mask_before_draw ? 0x8000 : 0x0000; }
  ALWAYS_INLINE u16 GetMaskOR() const { return set_mask_while_drawing ? 0x8000 : 0x0000; }

  u16 num_vertices;
  GPUDrawModeReg draw_mode;
  GPUTexturePaletteReg palette;
  GPUTextureWindow window;
};

struct GPUBackendDrawPolygonCommand : public GPUBackendDrawCommand
{
  struct Vertex
  {
    s32 x, y;
    union
    {
      struct
      {
        u8 r, g, b, a;
      };
      u32 color;
    };
    union
    {
      struct
      {
        u8 u, v;
      };
      u16 texcoord;
    };
  };

  Vertex vertices[0];
};

struct GPUBackendDrawPrecisePolygonCommand : public GPUBackendDrawCommand
{
  GPUBackendDrawCommand params;

  struct Vertex
  {
    float x, y, w;
    s32 native_x, native_y;
    u32 color;
    u16 texcoord;
  };

  Vertex vertices[0];
};

struct GPUBackendDrawRectangleCommand : public GPUBackendDrawCommand
{
  u16 width, height;
  u16 texcoord;
  s32 x, y;
  u32 color;
};

struct GPUBackendDrawLineCommand : public GPUBackendDrawCommand
{
  struct Vertex
  {
    s32 x, y;
    union
    {
      struct
      {
        u8 r, g, b, a;
      };
      u32 color;
    };

    ALWAYS_INLINE void Set(s32 x_, s32 y_, u32 color_)
    {
      x = x_;
      y = y_;
      color = color_;
    }
  };

  Vertex vertices[0];
};

struct GPUBackendDrawPreciseLineCommand : public GPUBackendDrawCommand
{
  struct Vertex
  {
    float x, y, w;
    s32 native_x, native_y;
    u32 color;
  };

  Vertex vertices[0];
};
