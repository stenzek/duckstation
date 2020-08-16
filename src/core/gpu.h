#pragma once
#include "common/bitfield.h"
#include "common/fifo_queue.h"
#include "common/rectangle.h"
#include "timers.h"
#include "types.h"
#include <algorithm>
#include <array>
#include <deque>
#include <memory>
#include <tuple>
#include <vector>

class StateWrapper;

class HostDisplay;

class TimingEvent;
class Timers;

class GPU
{
public:
  enum class BlitterState : u8
  {
    Idle,
    ReadingVRAM,
    WritingVRAM,
    DrawingPolyLine
  };

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

  enum class TextureMode : u8
  {
    Palette4Bit = 0,
    Palette8Bit = 1,
    Direct16Bit = 2,
    Reserved_Direct16Bit = 3,

    // Not register values.
    RawTextureBit = 4,
    RawPalette4Bit = RawTextureBit | Palette4Bit,
    RawPalette8Bit = RawTextureBit | Palette8Bit,
    RawDirect16Bit = RawTextureBit | Direct16Bit,
    Reserved_RawDirect16Bit = RawTextureBit | Reserved_Direct16Bit,

    Disabled = 8 // Not a register value
  };

  enum class TransparencyMode : u8
  {
    HalfBackgroundPlusHalfForeground = 0,
    BackgroundPlusForeground = 1,
    BackgroundMinusForeground = 2,
    BackgroundPlusQuarterForeground = 3,

    Disabled = 4 // Not a register value
  };

  enum : u32
  {
    VRAM_WIDTH = 1024,
    VRAM_HEIGHT = 512,
    VRAM_SIZE = VRAM_WIDTH * VRAM_HEIGHT * sizeof(u16),
    VRAM_WIDTH_MASK = VRAM_WIDTH - 1,
    VRAM_HEIGHT_MASK = VRAM_HEIGHT - 1,
    VRAM_COORD_MASK = 0x3FF,
    MAX_FIFO_SIZE = 4096,
    TEXTURE_PAGE_WIDTH = 256,
    TEXTURE_PAGE_HEIGHT = 256,
    MAX_PRIMITIVE_WIDTH = 1024,
    MAX_PRIMITIVE_HEIGHT = 512,
    DOT_TIMER_INDEX = 0,
    HBLANK_TIMER_INDEX = 1,
    MAX_RESOLUTION_SCALE = 16,
    DITHER_MATRIX_SIZE = 4
  };

  enum : u16
  {
    NTSC_TICKS_PER_LINE = 3413,
    NTSC_HSYNC_TICKS = 200,
    NTSC_TOTAL_LINES = 263,
    PAL_TICKS_PER_LINE = 3406,
    PAL_HSYNC_TICKS = 200, // actually one more on odd lines
    PAL_TOTAL_LINES = 314,
  };

  // 4x4 dither matrix.
  static constexpr s32 DITHER_MATRIX[DITHER_MATRIX_SIZE][DITHER_MATRIX_SIZE] = {{-4, +0, -3, +1},  // row 0
                                                                                {+2, -2, +3, -1},  // row 1
                                                                                {-3, +1, -4, +0},  // row 2
                                                                                {+4, -1, +2, -2}}; // row 3

  // Base class constructor.
  GPU();
  virtual ~GPU();

  virtual bool IsHardwareRenderer() const = 0;

  virtual bool Initialize(HostDisplay* host_display);
  virtual void Reset();
  virtual bool DoState(StateWrapper& sw);

  // Graphics API state reset/restore - call when drawing the UI etc.
  virtual void ResetGraphicsAPIState();
  virtual void RestoreGraphicsAPIState();

  // Render statistics debug window.
  void DrawDebugStateWindow();

  // MMIO access
  u32 ReadRegister(u32 offset);
  void WriteRegister(u32 offset, u32 value);

  // DMA access
  void DMARead(u32* words, u32 word_count);

  ALWAYS_INLINE bool BeginDMAWrite() const { return (m_GPUSTAT.dma_direction == DMADirection::CPUtoGP0); }
  ALWAYS_INLINE void DMAWrite(u32 address, u32 value)
  {
    m_fifo.Push((ZeroExtend64(address) << 32) | ZeroExtend64(value));
  }
  void EndDMAWrite();

  /// Returns false if the DAC is loading any data from VRAM.
  ALWAYS_INLINE bool IsDisplayDisabled() const
  {
    return m_GPUSTAT.display_disable || m_crtc_state.display_vram_width == 0 || m_crtc_state.display_vram_height == 0;
  }

  /// Returns true if scanout should be interlaced.
  ALWAYS_INLINE bool IsInterlacedDisplayEnabled() const
  {
    return (!m_force_progressive_scan) & m_GPUSTAT.vertical_interlace;
  }

  /// Returns true if interlaced rendering is enabled and force progressive scan is disabled.
  ALWAYS_INLINE bool IsInterlacedRenderingEnabled() const
  {
    return (!m_force_progressive_scan) & m_GPUSTAT.SkipDrawingToActiveField();
  }

  /// Returns the number of pending GPU ticks.
  TickCount GetPendingCRTCTicks() const;
  TickCount GetPendingCommandTicks() const;

  /// Returns true if enough ticks have passed for the raster to be on the next line.
  bool IsCRTCScanlinePending() const;

  /// Returns true if a raster scanline or command execution is pending.
  bool IsCommandCompletionPending() const;

  /// Synchronizes the CRTC, updating the hblank timer.
  void SynchronizeCRTC();

  /// Recompile shaders/recreate framebuffers when needed.
  virtual void UpdateSettings();

  /// Updates the resolution scale when it's set to automatic.
  virtual void UpdateResolutionScale();

  /// Returns the effective display resolution of the GPU.
  virtual std::tuple<u32, u32> GetEffectiveDisplayResolution();

  // gpu_hw_d3d11.cpp
  static std::unique_ptr<GPU> CreateHardwareD3D11Renderer();

  // gpu_hw_opengl.cpp
  static std::unique_ptr<GPU> CreateHardwareOpenGLRenderer();

  // gpu_hw_vulkan.cpp
  static std::unique_ptr<GPU> CreateHardwareVulkanRenderer();

  // gpu_sw.cpp
  static std::unique_ptr<GPU> CreateSoftwareRenderer();

  // Converts window coordinates into horizontal ticks and scanlines. Returns false if out of range. Used for lightguns.
  bool ConvertScreenCoordinatesToBeamTicksAndLines(s32 window_x, s32 window_y, u32* out_tick, u32* out_line) const;

  // Returns the video clock frequency.
  TickCount GetCRTCFrequency() const;

protected:
  TickCount CRTCTicksToSystemTicks(TickCount crtc_ticks, TickCount fractional_ticks) const;
  TickCount SystemTicksToCRTCTicks(TickCount sysclk_ticks, TickCount* fractional_ticks) const;

  // The GPU internally appears to run at 2x the system clock.
  ALWAYS_INLINE static constexpr TickCount GPUTicksToSystemTicks(TickCount gpu_ticks)
  {
    return std::max<TickCount>(gpu_ticks >> 1, 1);
  }
  ALWAYS_INLINE static constexpr TickCount SystemTicksToGPUTicks(TickCount sysclk_ticks) { return sysclk_ticks << 1; }

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

  union RenderCommand
  {
    u32 bits;

    BitField<u32, u32, 0, 24> color_for_first_vertex;
    BitField<u32, bool, 24, 1> raw_texture_enable; // not valid for lines
    BitField<u32, bool, 25, 1> transparency_enable;
    BitField<u32, bool, 26, 1> texture_enable;
    BitField<u32, DrawRectangleSize, 27, 2> rectangle_size; // only for rectangles
    BitField<u32, bool, 27, 1> quad_polygon;                // only for polygons
    BitField<u32, bool, 27, 1> polyline;                    // only for lines
    BitField<u32, bool, 28, 1> shading_enable;              // 0 - flat, 1 = gouroud
    BitField<u32, Primitive, 29, 21> primitive;

    /// Returns true if texturing should be enabled. Depends on the primitive type.
    bool IsTexturingEnabled() const { return (primitive != Primitive::Line) ? texture_enable : false; }

    /// Returns true if dithering should be enabled. Depends on the primitive type.
    bool IsDitheringEnabled() const
    {
      switch (primitive)
      {
        case Primitive::Polygon:
          return shading_enable || (texture_enable && !raw_texture_enable);

        case Primitive::Line:
          return true;

        case Primitive::Rectangle:
        default:
          return false;
      }
    }
  };

  union VertexPosition
  {
    u32 bits;

    BitField<u32, s32, 0, 12> x;
    BitField<u32, s32, 16, 12> y;
  };

  // Sprites/rectangles should be clipped to 12 bits before drawing.
  static constexpr s32 TruncateVertexPosition(s32 x) { return SignExtendN<11, s32>(x); }

  struct NativeVertex
  {
    s16 x;
    s16 y;
    u32 color;
    u16 texcoord;
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

    void SetRGB24Dithered(u32 x, u32 y, u8 r8, u8 g8, u8 b8, bool c_ = false)
    {
      const s32 offset = DITHER_MATRIX[y & 3][x & 3];
      r8 = static_cast<u8>(std::clamp<s32>(static_cast<s32>(ZeroExtend32(r8)) + offset, 0, 255));
      g8 = static_cast<u8>(std::clamp<s32>(static_cast<s32>(ZeroExtend32(g8)) + offset, 0, 255));
      b8 = static_cast<u8>(std::clamp<s32>(static_cast<s32>(ZeroExtend32(b8)) + offset, 0, 255));
      SetRGB24(r8, g8, b8, c_);
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
  float ComputeHorizontalFrequency() const;
  float ComputeVerticalFrequency() const;
  void UpdateCRTCConfig();
  void UpdateCRTCDisplayParameters();

  // Update ticks for this execution slice
  void UpdateCRTCTickEvent();
  void UpdateCommandTickEvent();

  // Updates dynamic bits in GPUSTAT (ready to send VRAM/ready to receive DMA)
  void UpdateDMARequest();
  void UpdateGPUIdle();

  // Ticks for hblank/vblank.
  void CRTCTickEvent(TickCount ticks);
  void CommandTickEvent(TickCount ticks);

  /// Returns 0 if the currently-displayed field is on odd lines (1,3,5,...) or 1 if even (2,4,6,...).
  ALWAYS_INLINE u32 GetInterlacedDisplayField() const { return ZeroExtend32(m_crtc_state.interlaced_field); }

  /// Returns 0 if the currently-displayed field is on an even line in VRAM, otherwise 1.
  ALWAYS_INLINE u32 GetActiveLineLSB() const { return ZeroExtend32(m_crtc_state.active_line_lsb); }

  /// Sets/decodes GP0(E1h) (set draw mode).
  void SetDrawMode(u16 bits);

  /// Sets/decodes polygon/rectangle texture palette value.
  void SetTexturePalette(u16 bits);

  /// Sets/decodes texture window bits.
  void SetTextureWindow(u32 value);

  u32 ReadGPUREAD();
  void FinishVRAMWrite();

  /// Returns the number of vertices in the buffered poly-line.
  ALWAYS_INLINE u32 GetPolyLineVertexCount() const
  {
    return (static_cast<u32>(m_blit_buffer.size()) + BoolToUInt32(m_render_command.shading_enable)) >>
           BoolToUInt8(m_render_command.shading_enable);
  }

  /// Returns true if the drawing area is valid (i.e. left <= right, top <= bottom).
  ALWAYS_INLINE bool IsDrawingAreaIsValid() const { return m_drawing_area.Valid(); }

  void AddCommandTicks(TickCount ticks);

  void WriteGP1(u32 value);
  void EndCommand();
  void ExecuteCommands();
  void HandleGetGPUInfoCommand(u32 value);

  // Rendering in the backend
  virtual void ReadVRAM(u32 x, u32 y, u32 width, u32 height);
  virtual void FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color);
  virtual void UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data);
  virtual void CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height);
  virtual void DispatchRenderCommand();
  virtual void FlushRender();
  virtual void ClearDisplay();
  virtual void UpdateDisplay();
  virtual void DrawRendererStats(bool is_idle_frame);

  // These are **very** approximate.
  ALWAYS_INLINE void AddDrawTriangleTicks(u32 width, u32 height, bool shaded, bool textured, bool semitransparent)
  {
    const u32 average_width = ((width + 2) / 3);
    u32 ticks_per_row = average_width;
    if (textured)
      ticks_per_row += average_width;
    if (semitransparent || m_GPUSTAT.check_mask_before_draw)
      ticks_per_row += (average_width + 1u) / 2u;
    if (IsInterlacedRenderingEnabled())
      height = std::max<u32>(height / 2, 1u);

    AddCommandTicks(ticks_per_row * height);
  }
  ALWAYS_INLINE void AddDrawRectangleTicks(u32 width, u32 height, bool textured, bool semitransparent)
  {
    u32 ticks_per_row = width;
    if (textured)
      ticks_per_row += width;
    if (semitransparent || m_GPUSTAT.check_mask_before_draw)
      ticks_per_row += (width + 1u) / 2u;
    if (IsInterlacedRenderingEnabled())
      height = std::max<u32>(height / 2, 1u);

    AddCommandTicks(ticks_per_row * height);
  }
  ALWAYS_INLINE void AddDrawLineTicks(u32 width, u32 height, bool shaded)
  {
    if (IsInterlacedRenderingEnabled())
      height = std::max<u32>(height / 2, 1u);

    AddCommandTicks(std::max(width, height));
  }

  HostDisplay* m_host_display = nullptr;

  std::unique_ptr<TimingEvent> m_crtc_tick_event;
  std::unique_ptr<TimingEvent> m_command_tick_event;

  // Pointer to VRAM, used for reads/writes. In the hardware backends, this is the shadow buffer.
  u16* m_vram_ptr = nullptr;

  union GPUSTAT
  {
    u32 bits;
    BitField<u32, u8, 0, 4> texture_page_x_base;
    BitField<u32, u8, 4, 1> texture_page_y_base;
    BitField<u32, TransparencyMode, 5, 2> semi_transparency_mode;
    BitField<u32, TextureMode, 7, 2> texture_color_mode;
    BitField<u32, bool, 9, 1> dither_enable;
    BitField<u32, bool, 10, 1> draw_to_displayed_field;
    BitField<u32, bool, 11, 1> set_mask_while_drawing;
    BitField<u32, bool, 12, 1> check_mask_before_draw;
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
    BitField<u32, bool, 26, 1> gpu_idle;
    BitField<u32, bool, 27, 1> ready_to_send_vram;
    BitField<u32, bool, 28, 1> ready_to_recieve_dma;
    BitField<u32, DMADirection, 29, 2> dma_direction;
    BitField<u32, bool, 31, 1> display_line_lsb;

    bool IsMaskingEnabled() const
    {
      static constexpr u32 MASK = ((1 << 11) | (1 << 12));
      return ((bits & MASK) != 0);
    }
    bool SkipDrawingToActiveField() const
    {
      static constexpr u32 MASK = (1 << 19) | (1 << 22) | (1 << 10);
      static constexpr u32 ACTIVE = (1 << 19) | (1 << 22);
      return ((bits & MASK) == ACTIVE);
    }
    bool InInterleaved480iMode() const
    {
      static constexpr u32 ACTIVE = (1 << 19) | (1 << 22);
      return ((bits & ACTIVE) == ACTIVE);
    }

    // During transfer/render operations, if ((dst_pixel & mask_and) == 0) { pixel = src_pixel | mask_or }
    u16 GetMaskAND() const
    {
      // return check_mask_before_draw ? 0x8000 : 0x0000;
      return Truncate16((bits << 3) & 0x8000);
    }
    u16 GetMaskOR() const
    {
      // return set_mask_while_drawing ? 0x8000 : 0x0000;
      return Truncate16((bits << 4) & 0x8000);
    }
  } m_GPUSTAT = {};

  struct DrawMode
  {
    static constexpr u16 PALETTE_MASK = UINT16_C(0b0111111111111111);
    static constexpr u32 TEXTURE_WINDOW_MASK = UINT32_C(0b11111111111111111111);

    // bits in GP0(E1h) or texpage part of polygon
    union Reg
    {
      static constexpr u16 MASK = 0b1111111111111;
      static constexpr u16 TEXTURE_PAGE_MASK = UINT16_C(0b0000000000011111);

      // Polygon texpage commands only affect bits 0-8, 11
      static constexpr u16 POLYGON_TEXPAGE_MASK = 0b0000100111111111;

      // Bits 0..5 are returned in the GPU status register, latched at E1h/polygon draw time.
      static constexpr u32 GPUSTAT_MASK = 0b11111111111;

      u16 bits;

      BitField<u16, u8, 0, 4> texture_page_x_base;
      BitField<u16, u8, 4, 1> texture_page_y_base;
      BitField<u16, TransparencyMode, 5, 2> transparency_mode;
      BitField<u16, TextureMode, 7, 2> texture_mode;
      BitField<u16, bool, 9, 1> dither_enable;
      BitField<u16, bool, 10, 1> draw_to_displayed_field;
      BitField<u16, bool, 11, 1> texture_disable;
      BitField<u16, bool, 12, 1> texture_x_flip;
      BitField<u16, bool, 13, 1> texture_y_flip;

      u32 GetTexturePageXBase() const { return ZeroExtend32(texture_page_x_base.GetValue()) * 64; }
      u32 GetTexturePageYBase() const { return ZeroExtend32(texture_page_y_base.GetValue()) * 256; }
    };

    // original values
    Reg mode_reg;
    u16 palette_reg; // from vertex
    u32 texture_window_value;

    // decoded values
    u32 texture_page_x;
    u32 texture_page_y;
    u32 texture_palette_x;
    u32 texture_palette_y;
    u8 texture_window_mask_x;   // in 8 pixel steps
    u8 texture_window_mask_y;   // in 8 pixel steps
    u8 texture_window_offset_x; // in 8 pixel steps
    u8 texture_window_offset_y; // in 8 pixel steps
    bool texture_x_flip;
    bool texture_y_flip;
    bool texture_page_changed;
    bool texture_window_changed;

    /// Returns the texture/palette rendering mode.
    TextureMode GetTextureMode() const { return mode_reg.texture_mode; }

    /// Returns the semi-transparency mode when enabled.
    TransparencyMode GetTransparencyMode() const { return mode_reg.transparency_mode; }

    /// Returns true if the texture mode requires a palette.
    bool IsUsingPalette() const { return (mode_reg.bits & (2 << 7)) == 0; }

    /// Returns a rectangle comprising the texture page area.
    Common::Rectangle<u32> GetTexturePageRectangle() const
    {
      static constexpr std::array<u32, 4> texture_page_widths = {
        {TEXTURE_PAGE_WIDTH / 4, TEXTURE_PAGE_WIDTH / 2, TEXTURE_PAGE_WIDTH, TEXTURE_PAGE_WIDTH}};
      return Common::Rectangle<u32>::FromExtents(texture_page_x, texture_page_y,
                                                 texture_page_widths[static_cast<u8>(mode_reg.texture_mode.GetValue())],
                                                 TEXTURE_PAGE_HEIGHT);
    }

    /// Returns a rectangle comprising the texture palette area.
    Common::Rectangle<u32> GetTexturePaletteRectangle() const
    {
      static constexpr std::array<u32, 4> palette_widths = {{16, 256, 0, 0}};
      return Common::Rectangle<u32>::FromExtents(texture_palette_x, texture_palette_y,
                                                 palette_widths[static_cast<u8>(mode_reg.texture_mode.GetValue())], 1);
    }

    bool IsTexturePageChanged() const { return texture_page_changed; }
    void SetTexturePageChanged() { texture_page_changed = true; }
    void ClearTexturePageChangedFlag() { texture_page_changed = false; }

    bool IsTextureWindowChanged() const { return texture_window_changed; }
    void SetTextureWindowChanged() { texture_window_changed = true; }
    void ClearTextureWindowChangedFlag() { texture_window_changed = false; }
  } m_draw_mode = {};

  Common::Rectangle<u32> m_drawing_area{0, 0, VRAM_WIDTH, VRAM_HEIGHT};

  struct DrawingOffset
  {
    s32 x;
    s32 y;
  } m_drawing_offset = {};

  bool m_console_is_pal = false;
  bool m_set_texture_disable_mask = false;
  bool m_drawing_area_changed = false;
  bool m_force_progressive_scan = false;
  bool m_force_ntsc_timings = false;

  struct CRTCState
  {
    struct Regs
    {
      static constexpr u32 DISPLAY_ADDRESS_START_MASK = 0b111'11111111'11111110;
      static constexpr u32 HORIZONTAL_DISPLAY_RANGE_MASK = 0b11111111'11111111'11111111;
      static constexpr u32 VERTICAL_DISPLAY_RANGE_MASK = 0b1111'11111111'11111111;

      union
      {
        u32 display_address_start;
        BitField<u32, u16, 0, 10> X;
        BitField<u32, u16, 10, 9> Y;
      };
      union
      {
        u32 horizontal_display_range;
        BitField<u32, u16, 0, 12> X1;
        BitField<u32, u16, 12, 12> X2;
      };

      union
      {
        u32 vertical_display_range;
        BitField<u32, u16, 0, 10> Y1;
        BitField<u32, u16, 10, 10> Y2;
      };
    } regs;

    u16 dot_clock_divider;

    // Size of the simulated screen in pixels. Depending on crop mode, this may include overscan area.
    u16 display_width;
    u16 display_height;

    // Top-left corner where the VRAM is displayed. Depending on the CRTC config, this may indicate padding.
    u16 display_origin_left;
    u16 display_origin_top;

    // Rectangle describing the displayed area of VRAM, in coordinates.
    u16 display_vram_left;
    u16 display_vram_top;
    u16 display_vram_width;
    u16 display_vram_height;

    u16 horizontal_total;
    u16 horizontal_sync_start; // <- not currently saved to state, so we don't have to bump the version
    u16 horizontal_active_start;
    u16 horizontal_active_end;
    u16 horizontal_display_start;
    u16 horizontal_display_end;
    u16 vertical_total;
    u16 vertical_active_start;
    u16 vertical_active_end;
    u16 vertical_display_start;
    u16 vertical_display_end;

    TickCount fractional_ticks;
    TickCount current_tick_in_scanline;
    u32 current_scanline;

    float display_aspect_ratio;
    bool in_hblank;
    bool in_vblank;

    u8 interlaced_field; // 0 = odd, 1 = even
    u8 interlaced_display_field;
    u8 active_line_lsb;
  } m_crtc_state = {};

  BlitterState m_blitter_state = BlitterState::Idle;
  u32 m_command_total_words = 0;
  TickCount m_pending_command_ticks = 0;

  /// GPUREAD value for non-VRAM-reads.
  u32 m_GPUREAD_latch = 0;

  /// True if currently executing/syncing.
  bool m_syncing = false;
  bool m_fifo_pushed = false;

  struct VRAMTransfer
  {
    u16 x;
    u16 y;
    u16 width;
    u16 height;
    u16 col;
    u16 row;
  } m_vram_transfer = {};

  HeapFIFOQueue<u64, MAX_FIFO_SIZE> m_fifo;
  std::vector<u32> m_blit_buffer;
  u32 m_blit_remaining_words;
  RenderCommand m_render_command{};

  ALWAYS_INLINE u32 FifoPop() { return Truncate32(m_fifo.Pop()); }
  ALWAYS_INLINE u32 FifoPeek() { return Truncate32(m_fifo.Peek()); }
  ALWAYS_INLINE u32 FifoPeek(u32 i) { return Truncate32(m_fifo.Peek(i)); }

  TickCount m_max_run_ahead = 128;
  u32 m_fifo_size = 128;

  struct Stats
  {
    u32 num_vram_reads;
    u32 num_vram_fills;
    u32 num_vram_writes;
    u32 num_vram_copies;
    u32 num_vertices;
    u32 num_polygons;
  };
  Stats m_stats = {};
  Stats m_last_stats = {};

private:
  using GP0CommandHandler = bool (GPU::*)();
  using GP0CommandHandlerTable = std::array<GP0CommandHandler, 256>;
  static GP0CommandHandlerTable GenerateGP0CommandHandlerTable();

  // Rendering commands, returns false if not enough data is provided
  bool HandleUnknownGP0Command();
  bool HandleNOPCommand();
  bool HandleClearCacheCommand();
  bool HandleInterruptRequestCommand();
  bool HandleSetDrawModeCommand();
  bool HandleSetTextureWindowCommand();
  bool HandleSetDrawingAreaTopLeftCommand();
  bool HandleSetDrawingAreaBottomRightCommand();
  bool HandleSetDrawingOffsetCommand();
  bool HandleSetMaskBitCommand();
  bool HandleRenderPolygonCommand();
  bool HandleRenderRectangleCommand();
  bool HandleRenderLineCommand();
  bool HandleRenderPolyLineCommand();
  bool HandleFillRectangleCommand();
  bool HandleCopyRectangleCPUToVRAMCommand();
  bool HandleCopyRectangleVRAMToCPUCommand();
  bool HandleCopyRectangleVRAMToVRAMCommand();

  static const GP0CommandHandlerTable s_GP0_command_handler_table;
};

IMPLEMENT_ENUM_CLASS_BITWISE_OPERATORS(GPU::TextureMode);

extern std::unique_ptr<GPU> g_gpu;
