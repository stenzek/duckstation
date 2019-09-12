#include "gpu.h"
#include "YBaseLib/Log.h"
#include "bus.h"
#include "dma.h"
Log_SetChannel(GPU);

GPU::GPU() = default;

GPU::~GPU() = default;

bool GPU::Initialize(Bus* bus, DMA* dma)
{
  m_bus = bus;
  m_dma = dma;
  return true;
}

void GPU::Reset()
{
  SoftReset();
}

void GPU::SoftReset()
{
  m_GPUSTAT.bits = 0x14802000;
  UpdateDMARequest();
}

void GPU::UpdateDMARequest()
{
  const bool request = m_GPUSTAT.dma_direction != DMADirection::Off;
  m_GPUSTAT.dma_data_request = request;
  m_dma->SetRequest(DMA::Channel::GPU, request);
}

u32 GPU::ReadRegister(u32 offset)
{
  switch (offset)
  {
    case 0x00:
      return ReadGPUREAD();

    case 0x04:
      return m_GPUSTAT.bits;

    default:
      Log_ErrorPrintf("Unhandled register read: %02X", offset);
      return UINT32_C(0xFFFFFFFF);
  }
}

void GPU::WriteRegister(u32 offset, u32 value)
{
  switch (offset)
  {
    case 0x00:
      WriteGP0(value);
      return;

    case 0x04:
      WriteGP1(value);
      return;

    default:
      Log_ErrorPrintf("Unhandled register write: %02X <- %08X", offset, value);
      return;
  }
}

u32 GPU::DMARead()
{
  if (m_GPUSTAT.dma_direction != DMADirection::GPUREADtoCPU)
  {
    Log_ErrorPrintf("Invalid DMA direction from GPU DMA read");
    return UINT32_C(0xFFFFFFFF);
  }

  return ReadGPUREAD();
}

void GPU::DMAWrite(u32 value)
{
  switch (m_GPUSTAT.dma_direction)
  {
    case DMADirection::CPUtoGP0:
      WriteGP0(value);
      break;

    default:
      Log_ErrorPrintf("Unhandled GPU DMA write mode %u for value %08X",
                      static_cast<u32>(m_GPUSTAT.dma_direction.GetValue()), value);
      break;
  }
}

u32 GPU::ReadGPUREAD()
{
  Log_ErrorPrintf("GPUREAD not implemented");
  return UINT32_C(0xFFFFFFFF);
}

void GPU::WriteGP0(u32 value)
{
  m_GP0_command.push_back(value);
  Assert(m_GP0_command.size() <= 128);

  const u8 command = Truncate8(m_GP0_command[0] >> 24);
  const u32 param = m_GP0_command[0] & UINT32_C(0x00FFFFFF);

  if (command >= 0x20 && command <= 0x7F)
  {
    // Draw polygon
    if (!HandleRenderCommand())
      return;
  }
  else
  {
    switch (command)
    {
      case 0x00: // NOP
        break;

      case 0xE1: // Set draw mode
      {
        // 0..10 bits match GPUSTAT
        const u32 MASK = ((UINT32_C(1) << 11) - 1);
        m_GPUSTAT.bits = (m_GPUSTAT.bits & ~MASK) | param & MASK;
        m_GPUSTAT.texture_disable = (param & (UINT32_C(1) << 11)) != 0;
        m_texture_config.x_flip = (param & (UINT32_C(1) << 12)) != 0;
        m_texture_config.y_flip = (param & (UINT32_C(1) << 13)) != 0;
        Log_DebugPrintf("Set draw mode %08X", param);
      }
      break;

      case 0xE2: // set texture window
      {
        m_texture_config.window_mask_x = param & UINT32_C(0x1F);
        m_texture_config.window_mask_y = (param >> 5) & UINT32_C(0x1F);
        m_texture_config.window_offset_x = (param >> 10) & UINT32_C(0x1F);
        m_texture_config.window_offset_y = (param >> 15) & UINT32_C(0x1F);
        Log_DebugPrintf("Set texture window %02X %02X %02X %02X", m_texture_config.window_mask_x,
                        m_texture_config.window_mask_y, m_texture_config.window_offset_x,
                        m_texture_config.window_offset_y);
      }
      break;

      case 0xE3: // Set drawing area top left
      {
        m_drawing_area.top_left_x = param & UINT32_C(0x3FF);
        m_drawing_area.top_left_y = (param >> 10) & UINT32_C(0x1FF);
        Log_DebugPrintf("Set drawing area top-left: (%u, %u)", m_drawing_area.top_left_x, m_drawing_area.top_left_y);
      }
      break;

      case 0xE4: // Set drawing area bottom right
      {
        m_drawing_area.bottom_right_x = param & UINT32_C(0x3FF);
        m_drawing_area.bottom_right_y = (param >> 10) & UINT32_C(0x1FF);
        Log_DebugPrintf("Set drawing area bottom-right: (%u, %u)", m_drawing_area.bottom_right_x,
                        m_drawing_area.bottom_right_y);
      }
      break;

      case 0xE5: // Set drawing offset
      {
        m_drawing_offset.x = S11ToS32(param & UINT32_C(0x7FF));
        m_drawing_offset.y = S11ToS32((param >> 11) & UINT32_C(0x7FF));
        Log_DebugPrintf("Set drawing offset (%d, %d)", m_drawing_offset.x, m_drawing_offset.y);
      }
      break;

      case 0xE6: // Mask bit setting
      {
        m_GPUSTAT.draw_set_mask_bit = (param & UINT32_C(0x01)) != 0;
        m_GPUSTAT.draw_to_masked_pixels = (param & UINT32_C(0x01)) != 0;
        Log_DebugPrintf("Set mask bit %u %u", BoolToUInt32(m_GPUSTAT.draw_set_mask_bit),
                        BoolToUInt32(m_GPUSTAT.draw_to_masked_pixels));
      }
      break;

      default:
      {
        Log_ErrorPrintf("Unimplemented GP0 command 0x%02X", command);
      }
      break;
    }
  }

  m_GP0_command.clear();
}

void GPU::WriteGP1(u32 value)
{
  const u8 command = Truncate8(value >> 24);
  const u32 param = value & UINT32_C(0x00FFFFFF);
  switch (command)
  {
    case 0x04: // DMA Direction
    {
      m_GPUSTAT.dma_direction = static_cast<DMADirection>(param);
      Log_DebugPrintf("DMA direction <- 0x%02X", static_cast<u32>(m_GPUSTAT.dma_direction.GetValue()));
      UpdateDMARequest();
    }
    break;

    default:
      Log_ErrorPrintf("Unimplemented GP1 command 0x%02X", command);
      break;
  }
}

bool GPU::HandleRenderCommand()
{
  const u8 command = Truncate8(m_GP0_command[0] >> 24);

  const RenderCommand rc{m_GP0_command[0]};
  u8 words_per_vertex;
  u32 num_vertices;
  u32 total_words;
  switch (rc.primitive)
  {
    case Primitive::Polygon:
    {
      // shaded vertices use the colour from the first word for the first vertex
      words_per_vertex = 1 + BoolToUInt8(rc.texture_enable) + BoolToUInt8(rc.shading_enable);
      num_vertices = rc.quad_polygon ? 4 : 3;
      total_words = words_per_vertex * num_vertices + BoolToUInt8(!rc.shading_enable);
    }
    break;

    case Primitive::Line:
    {
      words_per_vertex = 1 + BoolToUInt8(rc.shading_enable);
      if (rc.polyline)
      {
        // polyline goes until we hit the termination code
        num_vertices = 0;
        bool found_terminator = false;
        for (size_t pos = 0; pos < m_GP0_command.size(); pos += words_per_vertex)
        {
          if (m_GP0_command[pos] == 0x55555555)
          {
            found_terminator = true;
            break;
          }

          num_vertices++;
        }
        if (!found_terminator)
          return false;
      }
      else
      {
        num_vertices = 2;
      }

      total_words = words_per_vertex * num_vertices + BoolToUInt8(!rc.shading_enable);
    }
    break;

    case Primitive::Rectangle:
    {
      words_per_vertex =
        1 + BoolToUInt8(rc.texture_enable) + BoolToUInt8(rc.rectangle_size == DrawRectangleSize::Variable);
      num_vertices = 1;
      total_words = words_per_vertex;
    }
    break;

    default:
      UnreachableCode();
      return true;
  }

  if (m_GP0_command.size() < total_words)
    return false;

  static constexpr std::array<const char*, 4> primitive_names = {{"", "polygon", "line", "rectangle"}};

  Log_DebugPrintf("Render %s %s %s %s %s (%u verts, %u words per vert)", rc.quad_polygon ? "four-point" : "three-point",
                  rc.transparency_enable ? "semi-transparent" : "opaque",
                  rc.texture_enable ? "textured" : "non-textured", rc.shading_enable ? "shaded" : "monochrome",
                  primitive_names[static_cast<u8>(rc.primitive.GetValue())], ZeroExtend32(num_vertices),
                  ZeroExtend32(words_per_vertex));

  DispatchRenderCommand(rc, num_vertices);
  return true;
}

void GPU::DispatchRenderCommand(RenderCommand rc, u32 num_vertices) {}

void GPU::FlushRender() {}