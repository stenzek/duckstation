#pragma once
#include "common/bitfield.h"
#include "types.h"
#include <array>
#include <deque>
#include <vector>

class System;
class Bus;
class DMA;

class GPU
{
public:
  GPU();
  virtual ~GPU();

  virtual bool Initialize(System* system, Bus* bus, DMA* dma);
  virtual void Reset();

  u32 ReadRegister(u32 offset);
  void WriteRegister(u32 offset, u32 value);

  // DMA access
  u32 DMARead();
  void DMAWrite(u32 value);

  // gpu_hw_opengl.cpp
  static std::unique_ptr<GPU> CreateHardwareOpenGLRenderer();

protected:
  static constexpr u32 VRAM_WIDTH = 1024;
  static constexpr u32 VRAM_HEIGHT = 512;
  static constexpr u32 VRAM_SIZE = VRAM_WIDTH * VRAM_HEIGHT * sizeof(u16);
  static constexpr u32 TEXTURE_PAGE_WIDTH = 256;
  static constexpr u32 TEXTURE_PAGE_HEIGHT = 256;

  static constexpr s32 S11ToS32(u32 value)
  {
    if (value & (UINT16_C(1) << 10))
      return static_cast<s32>(UINT32_C(0xFFFFF800) | value);
    else
      return value;
  }

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

  union RenderCommand
  {
    u32 bits;

    BitField<u32, u32, 0, 23> color_for_first_vertex;
    BitField<u32, bool, 24, 1> texture_blending_raw; // not valid for lines
    BitField<u32, bool, 25, 1> transparency_enable;
    BitField<u32, bool, 26, 1> texture_enable;
    BitField<u32, DrawRectangleSize, 27, 2> rectangle_size; // only for rectangles
    BitField<u32, bool, 27, 1> quad_polygon;                // only for polygons
    BitField<u32, bool, 27, 1> polyline;                    // only for lines
    BitField<u32, bool, 28, 1> shading_enable;              // 0 - flat, 1 = gouroud
    BitField<u32, Primitive, 29, 21> primitive;
  };

  // TODO: Use BitField to do sign extending instead
  union VertexPosition
  {
    u32 bits;

    BitField<u32, u32, 0, 11> x_s11;
    BitField<u32, u32, 16, 11> y_s11;

    u32 x() const { return S11ToS32(x_s11); }
    u32 y() const { return S11ToS32(y_s11); }
  };

  void SoftReset();

  // Updates dynamic bits in GPUSTAT (ready to send VRAM/ready to receive DMA)
  void UpdateGPUSTAT();

  u32 ReadGPUREAD();
  void WriteGP0(u32 value);
  void WriteGP1(u32 value);

  // Rendering commands, returns false if not enough data is provided
  bool HandleRenderCommand();
  bool HandleCopyRectangleCPUToVRAMCommand();
  bool HandleCopyRectangleVRAMToCPUCommand();

  // Rendering in the backend
  virtual void UpdateDisplay();
  virtual void UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data);
  virtual void DispatchRenderCommand(RenderCommand rc, u32 num_vertices);
  virtual void FlushRender();

  System* m_system = nullptr;
  Bus* m_bus = nullptr;
  DMA* m_dma = nullptr;

  union GPUSTAT
  {
    u32 bits;
    BitField<u32, u8, 0, 4> texture_page_x_base;
    BitField<u32, u8, 4, 1> texture_page_y_base;
    BitField<u32, u8, 5, 2> semi_transparency;
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
    BitField<u32, u8, 19, 1> vetical_resolution;
    BitField<u32, bool, 20, 1> pal_mode;
    BitField<u32, bool, 21, 1> display_area_color_depth_24;
    BitField<u32, bool, 22, 1> vertical_interlace;
    BitField<u32, bool, 23, 1> display_enable;
    BitField<u32, bool, 24, 1> interrupt_request;
    BitField<u32, bool, 25, 1> dma_data_request;
    BitField<u32, bool, 26, 1> ready_to_recieve_cmd;
    BitField<u32, bool, 27, 1> ready_to_send_vram;
    BitField<u32, bool, 28, 1> ready_to_recieve_dma;
    BitField<u32, DMADirection, 29, 2> dma_direction;
    BitField<u32, bool, 31, 1> drawing_even_line;
  } m_GPUSTAT = {};

  struct TextureConfig
  {
    static constexpr u16 PAGE_ATTRIBUTE_MASK = UINT16_C(0b0000100111111111);
    static constexpr u16 PALETTE_ATTRIBUTE_MASK = UINT16_C(0b0111111111111111);

    // decoded values
    s32 base_x;
    s32 base_y;
    s32 palette_x;
    s32 palette_y;

    // original values
    u16 page_attribute;          // from register in rectangle modes/vertex in polygon modes
    u16 palette_attribute;       // from vertex
    TextureColorMode color_mode; // from register/vertex in polygon modes

    bool page_changed = false;

    bool IsPageChanged() const { return page_changed; }
    void ClearPageChangedFlag() { page_changed = false; }

    void SetColorMode(TextureColorMode new_color_mode);

    void SetFromPolygonTexcoord(u32 texcoord0, u32 texcoord1);
    void SetFromRectangleTexcoord(u32 texcoord);

    void SetFromPageAttribute(u16 value);
    void SetFromPaletteAttribute(u16 value);

    u8 window_mask_x;   // in 8 pixel steps
    u8 window_mask_y;   // in 8 pixel steps
    u8 window_offset_x; // in 8 pixel steps
    u8 window_offset_y; // in 8 pixel steps
    bool x_flip;
    bool y_flip;
  } m_texture_config = {};

  struct DrawingArea
  {
    u32 top_left_x, top_left_y;
    u32 bottom_right_x, bottom_right_y;
  } m_drawing_area = {};

  struct DrawingOffset
  {
    s32 x;
    s32 y;
  } m_drawing_offset = {};

  struct TexturePageConfig
  {

  } m_texture_page_config = {};

  std::vector<u32> m_GP0_command;
  std::deque<u32> m_GPUREAD_buffer;
};
