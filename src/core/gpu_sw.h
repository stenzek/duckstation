// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "gpu.h"
#include "gpu_backend.h"

#include "util/gpu_device.h"

#include "common/heap_array.h"

#include <memory>

// TODO: Move to cpp
// TODO: Rename to GPUSWBackend, preserved to avoid conflicts.
class GPU_SW final : public GPUBackend
{
public:
  GPU_SW();
  ~GPU_SW() override;

  bool Initialize(bool upload_vram, Error* error) override;

  void RestoreDeviceContext() override;

  u32 GetResolutionScale() const override;

protected:
  void ReadVRAM(u32 x, u32 y, u32 width, u32 height) override;
  void FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color, GPUBackendCommandParameters params) override;
  void UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data, GPUBackendCommandParameters params) override;
  void CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height,
                GPUBackendCommandParameters params) override;

  void DrawPolygon(const GPUBackendDrawPolygonCommand* cmd) override;
  void DrawPrecisePolygon(const GPUBackendDrawPrecisePolygonCommand* cmd) override;
  void DrawLine(const GPUBackendDrawLineCommand* cmd) override;
  void DrawSprite(const GPUBackendDrawRectangleCommand* cmd) override;
  void DrawingAreaChanged() override;
  void ClearCache() override;
  void UpdateCLUT(GPUTexturePaletteReg reg, bool clut_is_8bit) override;
  void OnBufferSwapped() override;

  void UpdateDisplay(const GPUBackendUpdateDisplayCommand* cmd) override;

  void ClearVRAM() override;

  void FlushRender() override;

  void UpdateResolutionScale() override;

  void LoadState(const GPUBackendLoadStateCommand* cmd) override;

  bool AllocateMemorySaveState(System::MemorySaveState& mss, Error* error) override;
  void DoMemoryState(StateWrapper& sw, System::MemorySaveState& mss) override;

private:
  static constexpr GPUTexture::Format FORMAT_FOR_24BIT = GPUTexture::Format::RGBA8; // RGBA8 always supported.

  template<GPUTexture::Format display_format>
  bool CopyOut15Bit(u32 src_x, u32 src_y, u32 width, u32 height, u32 line_skip);

  bool CopyOut24Bit(u32 src_x, u32 src_y, u32 skip_x, u32 width, u32 height, u32 line_skip);

  bool CopyOut(u32 src_x, u32 src_y, u32 skip_x, u32 width, u32 height, u32 line_skip, bool is_24bit);

  GPUTexture* GetDisplayTexture(u32 width, u32 height, GPUTexture::Format format);

  FixedHeapArray<u8, GPU_MAX_DISPLAY_WIDTH * GPU_MAX_DISPLAY_HEIGHT * sizeof(u32)> m_upload_buffer;
  GPUTexture::Format m_16bit_display_format = GPUTexture::Format::Unknown;
  std::unique_ptr<GPUTexture> m_upload_texture;
};
