#pragma once
#include "common/bitfield.h"
#include "types.h"
#include <array>

class Bus;
class DMA;

class GPU
{
public:
  GPU();
  ~GPU();

  bool Initialize(Bus* bus, DMA* dma);
  void Reset();

  u32 ReadRegister(u32 offset);
  void WriteRegister(u32 offset, u32 value);

  // DMA access
  u32 DMARead();
  void DMAWrite(u32 value);

private:
  enum class DMADirection : u32
  {
    Off = 0,
    FIFO = 1,
    CPUtoGP0 = 2,
    GPUREADtoCPU = 3
  };

  void SoftReset();
  void UpdateDMARequest();
  u32 ReadGPUREAD();
  void WriteGP0(u32 value);
  void WriteGP1(u32 value);

  Bus* m_bus = nullptr;
  DMA* m_dma = nullptr;

  union GPUSTAT
  {
    u32 bits;
    BitField<u32, u8, 0, 4> texture_page_x_base;
    BitField<u32, u8, 4, 1> texture_page_y_base;
    BitField<u32, u8, 5, 2> semi_transparency;
    BitField<u32, u8, 7, 2> texture_page_colors;
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
};
