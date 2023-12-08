// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gpu_sw.h"
#include "gpu.h"
#include "gpu_sw_rasterizer.h"
#include "settings.h"
#include "system_private.h"

#include "util/gpu_device.h"
#include "util/state_wrapper.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/intrin.h"
#include "common/log.h"

#include <algorithm>

LOG_CHANNEL(GPU);

GPU_SW::GPU_SW() = default;

GPU_SW::~GPU_SW() = default;

u32 GPU_SW::GetResolutionScale() const
{
  return 1u;
}

bool GPU_SW::Initialize(bool upload_vram, Error* error)
{
  if (!GPUBackend::Initialize(upload_vram, error))
    return false;

  static constexpr const std::array formats_for_16bit = {GPUTexture::Format::RGB5A1, GPUTexture::Format::A1BGR5,
                                                         GPUTexture::Format::RGB565, GPUTexture::Format::RGBA8};
  for (const GPUTexture::Format format : formats_for_16bit)
  {
    if (g_gpu_device->SupportsTextureFormat(format))
    {
      m_16bit_display_format = format;
      break;
    }
  }

  // RGBA8 will always be supported, hence we'll find one.
  INFO_LOG("Using {} format for 16-bit display", GPUTexture::GetFormatName(m_16bit_display_format));
  Assert(m_16bit_display_format != GPUTexture::Format::Unknown);

  // if we're using "new" vram, clear it out here
  if (!upload_vram)
    std::memset(g_vram, 0, sizeof(g_vram));

  return true;
}

void GPU_SW::ClearVRAM()
{
  std::memset(g_vram, 0, sizeof(g_vram));
  std::memset(g_gpu_clut, 0, sizeof(g_gpu_clut));
}

void GPU_SW::UpdateResolutionScale()
{
}

void GPU_SW::LoadState(const GPUBackendLoadStateCommand* cmd)
{
  std::memcpy(g_vram, cmd->vram_data, sizeof(g_vram));
  std::memcpy(g_gpu_clut, cmd->clut_data, sizeof(g_gpu_clut));
}

bool GPU_SW::AllocateMemorySaveState(System::MemorySaveState& mss, Error* error)
{
  mss.gpu_state_data.resize(sizeof(g_vram) + sizeof(g_gpu_clut));
  return true;
}

void GPU_SW::DoMemoryState(StateWrapper& sw, System::MemorySaveState& mss)
{
  sw.DoBytes(g_vram, sizeof(g_vram));
  sw.DoBytes(g_gpu_clut, sizeof(g_gpu_clut));
  DebugAssert(!sw.HasError());
}

void GPU_SW::ReadVRAM(u32 x, u32 y, u32 width, u32 height)
{
}

void GPU_SW::FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color, GPUBackendCommandParameters params)
{
  GPU_SW_Rasterizer::FillVRAM(x, y, width, height, color, params.interlaced_rendering, params.active_line_lsb);
}

void GPU_SW::UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data, GPUBackendCommandParameters params)
{
  GPU_SW_Rasterizer::WriteVRAM(x, y, width, height, data, params.set_mask_while_drawing, params.check_mask_before_draw);
}

void GPU_SW::CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height,
                      GPUBackendCommandParameters params)
{
  GPU_SW_Rasterizer::CopyVRAM(src_x, src_y, dst_x, dst_y, width, height, params.set_mask_while_drawing,
                              params.check_mask_before_draw);
}

void GPU_SW::DrawPolygon(const GPUBackendDrawPolygonCommand* cmd)
{
  const GPURenderCommand rc{cmd->rc.bits};

  const GPU_SW_Rasterizer::DrawTriangleFunction DrawFunction = GPU_SW_Rasterizer::GetDrawTriangleFunction(
    rc.shading_enable, rc.texture_enable, rc.raw_texture_enable, rc.transparency_enable);

  DrawFunction(cmd, &cmd->vertices[0], &cmd->vertices[1], &cmd->vertices[2]);
  if (cmd->num_vertices > 3)
    DrawFunction(cmd, &cmd->vertices[2], &cmd->vertices[1], &cmd->vertices[3]);
}

void GPU_SW::DrawPrecisePolygon(const GPUBackendDrawPrecisePolygonCommand* cmd)
{
  const GPURenderCommand rc{cmd->rc.bits};

  const GPU_SW_Rasterizer::DrawTriangleFunction DrawFunction = GPU_SW_Rasterizer::GetDrawTriangleFunction(
    rc.shading_enable, rc.texture_enable, rc.raw_texture_enable, rc.transparency_enable);

  // Need to cut out the irrelevant bits.
  // TODO: In _theory_ we could use the fixed-point parts here.
  GPUBackendDrawPolygonCommand::Vertex vertices[4];
  for (u32 i = 0; i < cmd->num_vertices; i++)
  {
    const GPUBackendDrawPrecisePolygonCommand::Vertex& src = cmd->vertices[i];
    vertices[i] = GPUBackendDrawPolygonCommand::Vertex{
      .x = src.native_x, .y = src.native_y, .color = src.color, .texcoord = src.texcoord};
  }

  DrawFunction(cmd, &vertices[0], &vertices[1], &vertices[2]);
  if (cmd->num_vertices > 3)
    DrawFunction(cmd, &vertices[2], &vertices[1], &vertices[3]);
}

void GPU_SW::DrawSprite(const GPUBackendDrawRectangleCommand* cmd)
{
  const GPURenderCommand rc{cmd->rc.bits};

  const GPU_SW_Rasterizer::DrawRectangleFunction DrawFunction =
    GPU_SW_Rasterizer::GetDrawRectangleFunction(rc.texture_enable, rc.raw_texture_enable, rc.transparency_enable);

  DrawFunction(cmd);
}

void GPU_SW::DrawLine(const GPUBackendDrawLineCommand* cmd)
{
  const GPU_SW_Rasterizer::DrawLineFunction DrawFunction =
    GPU_SW_Rasterizer::GetDrawLineFunction(cmd->rc.shading_enable, cmd->rc.transparency_enable);

  for (u16 i = 0; i < cmd->num_vertices; i += 2)
    DrawFunction(cmd, &cmd->vertices[i], &cmd->vertices[i + 1]);
}

void GPU_SW::DrawingAreaChanged()
{
  // GPU_SW_Rasterizer::g_drawing_area set by base class.
}

void GPU_SW::ClearCache()
{
}

void GPU_SW::UpdateCLUT(GPUTexturePaletteReg reg, bool clut_is_8bit)
{
  GPU_SW_Rasterizer::UpdateCLUT(reg, clut_is_8bit);
}

void GPU_SW::OnBufferSwapped()
{
}

void GPU_SW::FlushRender()
{
}

void GPU_SW::RestoreDeviceContext()
{
}

GPUTexture* GPU_SW::GetDisplayTexture(u32 width, u32 height, GPUTexture::Format format)
{
  if (!m_upload_texture || m_upload_texture->GetWidth() != width || m_upload_texture->GetHeight() != height ||
      m_upload_texture->GetFormat() != format)
  {
    ClearDisplayTexture();
    g_gpu_device->RecycleTexture(std::move(m_upload_texture));
    m_upload_texture = g_gpu_device->FetchTexture(width, height, 1, 1, 1, GPUTexture::Type::Texture, format,
                                                  GPUTexture::Flags::AllowMap, nullptr, 0);
    if (!m_upload_texture) [[unlikely]]
      ERROR_LOG("Failed to create {}x{} {} texture", width, height, static_cast<u32>(format));
  }

  return m_upload_texture.get();
}

template<GPUTexture::Format display_format>
ALWAYS_INLINE_RELEASE bool GPU_SW::CopyOut15Bit(u32 src_x, u32 src_y, u32 width, u32 height, u32 line_skip)
{
  GPUTexture* texture = GetDisplayTexture(width, height, display_format);
  if (!texture) [[unlikely]]
    return false;

  u32 dst_stride = Common::AlignUpPow2(width * texture->GetPixelSize(), 4);
  u8* dst_ptr = m_upload_buffer.data();
  const bool mapped = texture->Map(reinterpret_cast<void**>(&dst_ptr), &dst_stride, 0, 0, width, height);

  // Fast path when not wrapping around.
  if ((src_x + width) <= VRAM_WIDTH && (src_y + height) <= VRAM_HEIGHT)
  {
    [[maybe_unused]] constexpr u32 pixels_per_vec = 8;
    [[maybe_unused]] const u32 aligned_width = Common::AlignDownPow2(width, pixels_per_vec);

    const u16* src_ptr = &g_vram[src_y * VRAM_WIDTH + src_x];
    const u32 src_step = VRAM_WIDTH << line_skip;

    for (u32 row = 0; row < height; row++)
    {
      const u16* src_row_ptr = src_ptr;
      u8* dst_row_ptr = dst_ptr;
      u32 x = 0;

#ifdef CPU_ARCH_SIMD
      for (; x < aligned_width; x += pixels_per_vec)
      {
        ConvertVRAMPixels<display_format>(dst_row_ptr, GSVector4i::load<false>(src_row_ptr));
        src_row_ptr += pixels_per_vec;
      }
#endif

      for (; x < width; x++)
        ConvertVRAMPixel<display_format>(dst_row_ptr, *(src_row_ptr++));

      src_ptr += src_step;
      dst_ptr += dst_stride;
    }
  }
  else
  {
    const u32 end_x = src_x + width;
    const u32 y_step = (1 << line_skip);
    for (u32 row = 0; row < height; row++)
    {
      const u16* src_row_ptr = &g_vram[(src_y % VRAM_HEIGHT) * VRAM_WIDTH];
      u8* dst_row_ptr = dst_ptr;

      for (u32 col = src_x; col < end_x; col++)
        ConvertVRAMPixel<display_format>(dst_row_ptr, src_row_ptr[col % VRAM_WIDTH]);

      src_y += y_step;
      dst_ptr += dst_stride;
    }
  }

  if (mapped)
    texture->Unmap();
  else
    texture->Update(0, 0, width, height, m_upload_buffer.data(), dst_stride);

  return true;
}

ALWAYS_INLINE_RELEASE bool GPU_SW::CopyOut24Bit(u32 src_x, u32 src_y, u32 skip_x, u32 width, u32 height, u32 line_skip)
{
  GPUTexture* texture = GetDisplayTexture(width, height, FORMAT_FOR_24BIT);
  if (!texture) [[unlikely]]
    return false;

  u32 dst_stride = width * sizeof(u32);
  u8* dst_ptr = m_upload_buffer.data();
  const bool mapped = texture->Map(reinterpret_cast<void**>(&dst_ptr), &dst_stride, 0, 0, width, height);

  if ((src_x + width) <= VRAM_WIDTH && (src_y + (height << line_skip)) <= VRAM_HEIGHT)
  {
    const u8* src_ptr = reinterpret_cast<const u8*>(&g_vram[src_y * VRAM_WIDTH + src_x]) + (skip_x * 3);
    const u32 src_stride = (VRAM_WIDTH << line_skip) * sizeof(u16);
    for (u32 row = 0; row < height; row++)
    {
      const u8* src_row_ptr = src_ptr;
      u8* dst_row_ptr = reinterpret_cast<u8*>(dst_ptr);
      for (u32 col = 0; col < width; col++)
      {
        *(dst_row_ptr++) = *(src_row_ptr++);
        *(dst_row_ptr++) = *(src_row_ptr++);
        *(dst_row_ptr++) = *(src_row_ptr++);
        *(dst_row_ptr++) = 0xFF;
      }

      src_ptr += src_stride;
      dst_ptr += dst_stride;
    }
  }
  else
  {
    const u32 y_step = (1 << line_skip);

    for (u32 row = 0; row < height; row++)
    {
      const u16* src_row_ptr = &g_vram[(src_y % VRAM_HEIGHT) * VRAM_WIDTH];
      u32* dst_row_ptr = reinterpret_cast<u32*>(dst_ptr);

      for (u32 col = 0; col < width; col++)
      {
        const u32 offset = (src_x + (((skip_x + col) * 3) / 2));
        const u16 s0 = src_row_ptr[offset % VRAM_WIDTH];
        const u16 s1 = src_row_ptr[(offset + 1) % VRAM_WIDTH];
        const u8 shift = static_cast<u8>(col & 1u) * 8;
        const u32 rgb = (((ZeroExtend32(s1) << 16) | ZeroExtend32(s0)) >> shift);

        *(dst_row_ptr++) = rgb | 0xFF000000u;
      }

      src_y += y_step;
      dst_ptr += dst_stride;
    }
  }

  if (mapped)
    texture->Unmap();
  else
    texture->Update(0, 0, width, height, m_upload_buffer.data(), dst_stride);

  return true;
}

bool GPU_SW::CopyOut(u32 src_x, u32 src_y, u32 skip_x, u32 width, u32 height, u32 line_skip, bool is_24bit)
{
  if (!is_24bit)
  {
    DebugAssert(skip_x == 0);

    switch (m_16bit_display_format)
    {
      case GPUTexture::Format::RGB5A1:
        return CopyOut15Bit<GPUTexture::Format::RGB5A1>(src_x, src_y, width, height, line_skip);

      case GPUTexture::Format::A1BGR5:
        return CopyOut15Bit<GPUTexture::Format::A1BGR5>(src_x, src_y, width, height, line_skip);

      case GPUTexture::Format::RGB565:
        return CopyOut15Bit<GPUTexture::Format::RGB565>(src_x, src_y, width, height, line_skip);

      case GPUTexture::Format::RGBA8:
        return CopyOut15Bit<GPUTexture::Format::RGBA8>(src_x, src_y, width, height, line_skip);

      case GPUTexture::Format::BGRA8:
        return CopyOut15Bit<GPUTexture::Format::BGRA8>(src_x, src_y, width, height, line_skip);

      default:
        UnreachableCode();
    }
  }
  else
  {
    return CopyOut24Bit(src_x, src_y, skip_x, width, height, line_skip);
  }
}

void GPU_SW::UpdateDisplay(const GPUBackendUpdateDisplayCommand* cmd)
{
  if (!g_settings.debugging.show_vram)
  {
    if (cmd->display_disabled)
    {
      ClearDisplayTexture();
      return;
    }

    const bool is_24bit = cmd->display_24bit;
    const bool interlaced = cmd->interlaced_display_enabled;
    const u32 field = cmd->interlaced_display_field;
    const u32 vram_offset_x = is_24bit ? cmd->X : cmd->display_vram_left;
    const u32 vram_offset_y = cmd->display_vram_top + ((interlaced && cmd->interlaced_display_interleaved) ? field : 0);
    const u32 skip_x = is_24bit ? (cmd->display_vram_left - cmd->X) : 0;
    const u32 read_width = cmd->display_vram_width;
    const u32 read_height = interlaced ? (cmd->display_vram_height / 2) : cmd->display_vram_height;

    if (cmd->interlaced_display_enabled)
    {
      const u32 line_skip = cmd->interlaced_display_interleaved;
      if (CopyOut(vram_offset_x, vram_offset_y, skip_x, read_width, read_height, line_skip, is_24bit))
      {
        SetDisplayTexture(m_upload_texture.get(), nullptr, 0, 0, read_width, read_height);
        if (is_24bit && g_settings.display_24bit_chroma_smoothing)
        {
          if (ApplyChromaSmoothing())
            Deinterlace(field, 0);
        }
        else
        {
          Deinterlace(field, 0);
        }
      }
    }
    else
    {
      if (CopyOut(vram_offset_x, vram_offset_y, skip_x, read_width, read_height, 0, is_24bit))
      {
        SetDisplayTexture(m_upload_texture.get(), nullptr, 0, 0, read_width, read_height);
        if (is_24bit && g_settings.display_24bit_chroma_smoothing)
          ApplyChromaSmoothing();
      }
    }
  }
  else
  {
    if (CopyOut(0, 0, 0, VRAM_WIDTH, VRAM_HEIGHT, 0, false))
      SetDisplayTexture(m_upload_texture.get(), nullptr, 0, 0, VRAM_WIDTH, VRAM_HEIGHT);
  }
}

std::unique_ptr<GPUBackend> GPUBackend::CreateSoftwareBackend()
{
  return std::make_unique<GPU_SW>();
}
