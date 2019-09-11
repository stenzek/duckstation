#include "gpu.h"
#include "YBaseLib/Log.h"
#include "bus.h"
#include "dma.h"
Log_SetChannel(GPU);

static constexpr s32 S11ToS32(u32 value)
{
  if (value & (UINT16_C(1) << 10))
    return static_cast<s32>(UINT32_C(0xFFFFF800) | value);
  else
    return value;
}

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
  Assert(m_GP0_command_length < MAX_GP0_COMMAND_LENGTH);

  m_GP0_command[m_GP0_command_length++] = value;

  const u8 command = Truncate8(m_GP0_command[0] >> 24);
  const u32 param = m_GP0_command[0] & UINT32_C(0x00FFFFFF);
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
      if (command < 0x20)
      {
      }
      else if (command < 0x40)
      {
        // Draw polygon
        if (!HandleRenderPolygonCommand())
          return;

        break;
      }

      Log_ErrorPrintf("Unimplemented GP0 command 0x%02X", command);
    }
    break;
  }

  m_GP0_command.fill(UINT32_C(0));
  m_GP0_command_length = 0;
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

bool GPU::HandleRenderPolygonCommand()
{
  const u8 command = Truncate8(m_GP0_command[0] >> 24);
  const bool semi_transparent = !!(command & 0x02);
  const bool textured = !!(command & 0x04);
  const bool four_points = !!(command & 0x08);
  const bool shaded = !!(command & 0x10);

  // shaded vertices use the colour from the first word for the first vertex
  const u8 words_per_vertex = 1 + BoolToUInt8(textured) + BoolToUInt8(shaded);
  const u8 num_vertices = four_points ? 4 : 3;
  const u8 total_words = words_per_vertex * num_vertices + BoolToUInt8(!shaded);
  if (m_GP0_command_length < total_words)
    return false;

  Log_DebugPrintf("Render %s %s %s %s polygon (%u verts, %u words per vert)",
                  four_points ? "four-point" : "three-point", semi_transparent ? "semi-transparent" : "opaque",
                  textured ? "textured" : "non-textured", shaded ? "shaded" : "monochrome", ZeroExtend32(num_vertices),
                  ZeroExtend32(words_per_vertex));

  return true;
}
