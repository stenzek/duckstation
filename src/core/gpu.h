#pragma once
#include "common/bitfield.h"
#include "timers.h"
#include "types.h"
#include <array>
#include <deque>
#include <memory>
#include <tuple>
#include <vector>

class StateWrapper;

class System;
class DMA;
class InterruptController;
class Timers;

class GPU
{
public:
  enum : u32
  {
    VRAM_WIDTH = 1024,
    VRAM_HEIGHT = 512,
    VRAM_SIZE = VRAM_WIDTH * VRAM_HEIGHT * sizeof(u16),
    TEXTURE_PAGE_WIDTH = 256,
    TEXTURE_PAGE_HEIGHT = 256,
    DOT_TIMER_INDEX = 0,
    HBLANK_TIMER_INDEX = 1
  };

  GPU();
  virtual ~GPU();

  virtual bool Initialize(System* system, DMA* dma, InterruptController* interrupt_controller, Timers* timers);
  virtual void Reset();
  virtual bool DoState(StateWrapper& sw);

  // Graphics API state reset/restore - call when drawing the UI etc.
  virtual void ResetGraphicsAPIState();
  virtual void RestoreGraphicsAPIState();

  // Render statistics debug window.
  void DrawDebugStateWindow();
  virtual void DrawRendererStatsWindow();

  // MMIO access
  u32 ReadRegister(u32 offset);
  void WriteRegister(u32 offset, u32 value);

  // DMA access
  void DMARead(u32* words, u32 word_count);
  void DMAWrite(const u32* words, u32 word_count);

  // Resolution scaling.
  u32 GetResolutionScale() const { return m_resolution_scale; }
  u32 GetMaxResolutionScale() const { return m_max_resolution_scale; }
  virtual void UpdateResolutionScale();

  // Ticks for hblank/vblank.
  void Execute(TickCount ticks);

  // gpu_hw_opengl.cpp
  static std::unique_ptr<GPU> CreateHardwareOpenGLRenderer();

  // gpu_sw.cpp
  static std::unique_ptr<GPU> CreateSoftwareRenderer();

protected:
  // Helper/format conversion functions.
  static constexpr u8 Convert5To8(u8 x5) { return (x5 << 3) | (x5 & 7); }
  static constexpr u8 Convert8To5(u8 x8) { return (x8 >> 3); }

  static constexpr u32 RGBA5551ToRGBA8888(u16 color)
  {
    u8 r = Truncate8(color & 31);
    u8 g = Truncate8((color >> 5) & 31);
    u8 b = Truncate8((color >> 10) & 31);
    u8 a = Truncate8((color >> 15) & 1);

    // 00012345 -> 1234545
    b = (b << 3) | (b & 0b111);
    g = (g << 3) | (g & 0b111);
    r = (r << 3) | (r & 0b111);
    a = a ? 255 : 0;

    return ZeroExtend32(r) | (ZeroExtend32(g) << 8) | (ZeroExtend32(b) << 16) | (ZeroExtend32(a) << 24);
  }

  static constexpr u16 RGBA8888ToRGBA5551(u32 color)
  {
    const u16 r = Truncate16((color >> 3) & 0x1Fu);
    const u16 g = Truncate16((color >> 11) & 0x1Fu);
    const u16 b = Truncate16((color >> 19) & 0x1Fu);
    const u16 a = Truncate16((color >> 31) & 0x01u);

    return r | (g << 5) | (b << 10) | (a << 15);
  }

  static constexpr std::tuple<u8, u8> UnpackTexcoord(u16 texcoord)
  {
    return std::make_tuple(static_cast<u8>(texcoord), static_cast<u8>(texcoord >> 8));
  }

  static constexpr std::tuple<u8, u8, u8> UnpackColorRGB24(u32 rgb24)
  {
    return std::make_tuple(static_cast<u8>(rgb24), static_cast<u8>(rgb24 >> 8), static_cast<u8>(rgb24 >> 16));
  }
  static constexpr u32 PackColorRGB24(u8 r, u8 g, u8 b)
  {
    return ZeroExtend32(r) | (ZeroExtend32(g) << 8) | (ZeroExtend32(b) << 16);
  }

  static bool DumpVRAMToFile(const char* filename, u32 width, u32 height, u32 stride, const void* buffer,
                             bool remove_alpha);

  enum class DMADirection : u32
  {
    Off = 0,
    FIFO = 1,
    CPUtoGP0 = 2,
    GPUREADtoCPU = 3
  };

  enum class Primitive : u8
  {
    Reserved = 0,
    Polygon = 1,
    Line = 2,
    Rectangle = 3
  };

  enum class DrawRectangleSize : u8
  {
    Variable = 0,
    R1x1 = 1,
    R8x8 = 2,
    R16x16 = 3
  };

  enum class TextureColorMode : u8
  {
    Palette4Bit = 0,
    Palette8Bit = 1,
    Direct16Bit = 2,
    Reserved_Direct16Bit = 3
  };

  enum class TransparencyMode : u8
  {
    HalfBackgroundPlusHalfForeground = 0,
    BackgroundPlusForeground = 1,
    BackgroundMinusForeground = 2,
    BackgroundPlusQuarterForeground = 3
  };

  union RenderCommand
  {
    u32 bits;

    BitField<u32, u32, 0, 24> color_for_first_vertex;
    BitField<u32, bool, 24, 1> texture_blend_disable; // not valid for lines
    BitField<u32, bool, 25, 1> transparency_enable;
    BitField<u32, bool, 26, 1> texture_enable;
    BitField<u32, DrawRectangleSize, 27, 2> rectangle_size; // only for rectangles
    BitField<u32, bool, 27, 1> quad_polygon;                // only for polygons
    BitField<u32, bool, 27, 1> polyline;                    // only for lines
    BitField<u32, bool, 28, 1> shading_enable;              // 0 - flat, 1 = gouroud
    BitField<u32, Primitive, 29, 21> primitive;

    // Helper functions.
    bool IsTextureEnabled() const { return (primitive != Primitive::Line && texture_enable); }
    bool IsTextureBlendingEnabled() const { return (IsTextureEnabled() && !texture_blend_disable); }
    bool IsTransparencyEnabled() const { return transparency_enable; }
  };

  // TODO: Use BitField to do sign extending instead
  union VertexPosition
  {
    u32 bits;

    BitField<u32, s32, 0, 12> x;
    BitField<u32, s32, 16, 12> y;
  };

  union VRAMPixel
  {
    u16 bits;

    BitField<u16, u8, 0, 5> r;
    BitField<u16, u8, 5, 5> g;
    BitField<u16, u8, 10, 5> b;
    BitField<u16, bool, 15, 1> c;

    u8 GetR8() const { return Convert5To8(r); }
    u8 GetG8() const { return Convert5To8(g); }
    u8 GetB8() const { return Convert5To8(b); }

    void Set(u8 r_, u8 g_, u8 b_, bool c_ = false)
    {
      bits = (ZeroExtend16(r_)) | (ZeroExtend16(g_) << 5) | (ZeroExtend16(b_) << 10) | (static_cast<u16>(c_) << 15);
    }

    void ClampAndSet(u8 r_, u8 g_, u8 b_, bool c_ = false)
    {
      Set(std::min<u8>(r_, 0x1F), std::min<u8>(g_, 0x1F), std::min<u8>(b_, 0x1F), c_);
    }

    void SetRGB24(u32 rgb24, bool c_ = false)
    {
      bits = Truncate16(((rgb24 >> 3) & 0x1F) | (((rgb24 >> 11) & 0x1F) << 5) | (((rgb24 >> 19) & 0x1F) << 10)) |
             (static_cast<u16>(c_) << 15);
    }

    void SetRGB24(u8 r8, u8 g8, u8 b8, bool c_ = false)
    {
      bits = (ZeroExtend16(r8 >> 3)) | (ZeroExtend16(g8 >> 3) << 5) | (ZeroExtend16(b8 >> 3) << 10) |
             (static_cast<u16>(c_) << 15);
    }

    u32 ToRGB24() const
    {
      const u32 r_ = ZeroExtend32(r.GetValue());
      const u32 g_ = ZeroExtend32(g.GetValue());
      const u32 b_ = ZeroExtend32(b.GetValue());

      return ((r_ << 3) | (r_ & 7)) | (((g_ << 3) | (g_ & 7)) << 8) | (((b_ << 3) | (b_ & 7)) << 16);
    }
  };

  void SoftReset();

  // Sets dots per scanline
  void UpdateCRTCConfig();

  // Update ticks for this execution slice
  void UpdateSliceTicks();

  // Updates dynamic bits in GPUSTAT (ready to send VRAM/ready to receive DMA)
  void UpdateGPUSTAT();

  u32 ReadGPUREAD();
  void WriteGP0(u32 value);
  void WriteGP1(u32 value);
  void HandleGetGPUInfoCommand(u32 value);

  // Rendering in the backend
  virtual void UpdateDisplay();
  virtual void UpdateDrawingArea();
  virtual void ReadVRAM(u32 x, u32 y, u32 width, u32 height, void* buffer);
  virtual void FillVRAM(u32 x, u32 y, u32 width, u32 height, u16 color);
  virtual void UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data);
  virtual void CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height);
  virtual void DispatchRenderCommand(RenderCommand rc, u32 num_vertices, const u32* command_ptr);
  virtual void FlushRender();

  System* m_system = nullptr;
  DMA* m_dma = nullptr;
  InterruptController* m_interrupt_controller = nullptr;
  Timers* m_timers = nullptr;

  // Resolution scale.
  u32 m_resolution_scale = 1;
  u32 m_max_resolution_scale = 1;

  union GPUSTAT
  {
    u32 bits;
    BitField<u32, u8, 0, 4> texture_page_x_base;
    BitField<u32, u8, 4, 1> texture_page_y_base;
    BitField<u32, TransparencyMode, 5, 2> semi_transparency_mode;
    BitField<u32, TextureColorMode, 7, 2> texture_color_mode;
    BitField<u32, bool, 9, 1> dither_enable;
    BitField<u32, bool, 10, 1> draw_to_display_area;
    BitField<u32, bool, 11, 1> draw_set_mask_bit;
    BitField<u32, bool, 12, 1> draw_to_masked_pixels;
    BitField<u32, bool, 13, 1> interlaced_field;
    BitField<u32, bool, 14, 1> reverse_flag;
    BitField<u32, bool, 15, 1> texture_disable;
    BitField<u32, u8, 16, 1> horizontal_resolution_2;
    BitField<u32, u8, 17, 2> horizontal_resolution_1;
    BitField<u32, bool, 19, 1> vertical_resolution;
    BitField<u32, bool, 20, 1> pal_mode;
    BitField<u32, bool, 21, 1> display_area_color_depth_24;
    BitField<u32, bool, 22, 1> vertical_interlace;
    BitField<u32, bool, 23, 1> display_disable;
    BitField<u32, bool, 24, 1> interrupt_request;
    BitField<u32, bool, 25, 1> dma_data_request;
    BitField<u32, bool, 26, 1> ready_to_recieve_cmd;
    BitField<u32, bool, 27, 1> ready_to_send_vram;
    BitField<u32, bool, 28, 1> ready_to_recieve_dma;
    BitField<u32, DMADirection, 29, 2> dma_direction;
    BitField<u32, bool, 31, 1> drawing_even_line;
  } m_GPUSTAT = {};

  struct RenderState
  {
    static constexpr u16 PAGE_ATTRIBUTE_TEXTURE_PAGE_MASK = UINT16_C(0b0000000000011111);
    static constexpr u16 PAGE_ATTRIBUTE_MASK = UINT16_C(0b0000000111111111);
    static constexpr u16 PALETTE_ATTRIBUTE_MASK = UINT16_C(0b0111111111111111);
    static constexpr u32 TEXTURE_WINDOW_MASK = UINT16_C(0b11111111111111111111);

    // decoded values
    u32 texture_page_x;
    u32 texture_page_y;
    u32 texture_palette_x;
    u32 texture_palette_y;
    TextureColorMode texture_color_mode;
    TransparencyMode transparency_mode;
    u8 texture_window_mask_x;   // in 8 pixel steps
    u8 texture_window_mask_y;   // in 8 pixel steps
    u8 texture_window_offset_x; // in 8 pixel steps
    u8 texture_window_offset_y; // in 8 pixel steps
    bool texture_x_flip;
    bool texture_y_flip;

    // original values
    u16 texpage_attribute; // from register in rectangle modes/vertex in polygon modes
    u16 texlut_attribute;  // from vertex
    u32 texture_window_value;

    bool texture_page_changed = false;
    bool texture_color_mode_changed = false;
    bool transparency_mode_changed = false;
    bool texture_window_changed = false;

    bool IsChanged() const { return texture_page_changed || texture_color_mode_changed || transparency_mode_changed; }

    bool IsTexturePageChanged() const { return texture_page_changed; }
    void ClearTexturePageChangedFlag() { texture_page_changed = false; }

    bool IsTextureColorModeChanged() const { return texture_color_mode_changed; }
    void ClearTextureColorModeChangedFlag() { texture_color_mode_changed = false; }

    bool IsTransparencyModeChanged() const { return transparency_mode_changed; }
    void ClearTransparencyModeChangedFlag() { transparency_mode_changed = false; }

    bool IsTextureWindowChanged() const { return texture_window_changed; }
    void ClearTextureWindowChangedFlag() { texture_window_changed = false; }

    void SetFromPolygonTexcoord(u32 texcoord0, u32 texcoord1);
    void SetFromRectangleTexcoord(u32 texcoord);

    void SetFromPageAttribute(u16 value);
    void SetFromPaletteAttribute(u16 value);
    void SetTextureWindow(u32 value);
  } m_render_state = {};

  struct DrawingArea
  {
    u32 left, top;
    u32 right, bottom;
  } m_drawing_area = {};

  struct DrawingOffset
  {
    s32 x;
    s32 y;
  } m_drawing_offset = {};

  struct CRTCState
  {
    struct Regs
    {
      static constexpr u32 DISPLAY_ADDRESS_START_MASK = 0b111'11111111'11111111;
      static constexpr u32 HORIZONTAL_DISPLAY_RANGE_MASK = 0b11111111'11111111'11111111;
      static constexpr u32 VERTICAL_DISPLAY_RANGE_MASK = 0b1111'11111111'11111111;

      union
      {
        u32 display_address_start;
        BitField<u32, u32, 0, 10> X;
        BitField<u32, u32, 10, 9> Y;
      };
      union
      {
        u32 horizontal_display_range;
        BitField<u32, u32, 0, 12> X1;
        BitField<u32, u32, 12, 12> X2;
      };

      union
      {
        u32 vertical_display_range;
        BitField<u32, u32, 0, 10> Y1;
        BitField<u32, u32, 10, 10> Y2;
      };
    } regs;

    TickCount dot_clock_divider;

    u32 display_width;
    u32 display_height;

    TickCount ticks_per_scanline;
    TickCount visible_ticks_per_scanline;
    u32 visible_scanlines_per_frame;
    u32 total_scanlines_per_frame;

    TickCount fractional_ticks;
    TickCount current_tick_in_scanline;
    u32 current_scanline;

    float display_aspect_ratio;

    bool in_hblank;
    bool in_vblank;
  } m_crtc_state = {};

  std::vector<u32> m_GP0_buffer;
  std::deque<u32> m_GPUREAD_buffer;

private:
  using GP0CommandHandler = bool (GPU::*)(const u32*&, u32);
  using GP0CommandHandlerTable = std::array<GP0CommandHandler, 256>;
  static GP0CommandHandlerTable GenerateGP0CommandHandlerTable();

  // Rendering commands, returns false if not enough data is provided
  bool HandleUnknownGP0Command(const u32*& command_ptr, u32 command_size);
  bool HandleNOPCommand(const u32*& command_ptr, u32 command_size);
  bool HandleClearCacheCommand(const u32*& command_ptr, u32 command_size);
  bool HandleInterruptRequestCommand(const u32*& command_ptr, u32 command_size);
  bool HandleSetDrawModeCommand(const u32*& command_ptr, u32 command_size);
  bool HandleSetTextureWindowCommand(const u32*& command_ptr, u32 command_size);
  bool HandleSetDrawingAreaTopLeftCommand(const u32*& command_ptr, u32 command_size);
  bool HandleSetDrawingAreaBottomRightCommand(const u32*& command_ptr, u32 command_size);
  bool HandleSetDrawingOffsetCommand(const u32*& command_ptr, u32 command_size);
  bool HandleSetMaskBitCommand(const u32*& command_ptr, u32 command_size);
  bool HandleRenderCommand(const u32*& command_ptr, u32 command_size);
  bool HandleFillRectangleCommand(const u32*& command_ptr, u32 command_size);
  bool HandleCopyRectangleCPUToVRAMCommand(const u32*& command_ptr, u32 command_size);
  bool HandleCopyRectangleVRAMToCPUCommand(const u32*& command_ptr, u32 command_size);
  bool HandleCopyRectangleVRAMToVRAMCommand(const u32*& command_ptr, u32 command_size);

  static const GP0CommandHandlerTable s_GP0_command_handler_table;
};
