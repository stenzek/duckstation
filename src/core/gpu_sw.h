// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "gpu.h"
#include "gpu_sw_backend.h"

#include "util/gpu_device.h"

#include "common/heap_array.h"

#include <memory>

namespace Threading {
class Thread;
}

class GPUTexture;

class GPU_SW final : public GPU
{
public:
  GPU_SW();
  ~GPU_SW() override;

  ALWAYS_INLINE const GPU_SW_Backend& GetBackend() const { return m_backend; }

  const Threading::Thread* GetSWThread() const override;
  bool IsHardwareRenderer() const override;

  bool Initialize(Error* error) override;
  bool DoState(StateWrapper& sw, bool update_display) override;
  bool DoMemoryState(StateWrapper& sw, System::MemorySaveState& mss, bool update_display) override;
  void Reset(bool clear_vram) override;
  void UpdateSettings(const Settings& old_settings) override;

protected:
  void ReadVRAM(u32 x, u32 y, u32 width, u32 height) override;
  void FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color) override;
  void UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data, bool set_mask, bool check_mask) override;
  void CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height) override;
  void FlushRender() override;
  void UpdateCLUT(GPUTexturePaletteReg reg, bool clut_is_8bit) override;

  template<GPUTexture::Format display_format>
  bool CopyOut15Bit(u32 src_x, u32 src_y, u32 width, u32 height, u32 line_skip);

  bool CopyOut24Bit(u32 src_x, u32 src_y, u32 skip_x, u32 width, u32 height, u32 line_skip);

  bool CopyOut(u32 src_x, u32 src_y, u32 skip_x, u32 width, u32 height, u32 line_skip, bool is_24bit);

  void UpdateDisplay() override;

  void DispatchRenderCommand() override;

  void FillBackendCommandParameters(GPUBackendCommand* cmd) const;
  void FillDrawCommand(GPUBackendDrawCommand* cmd, GPURenderCommand rc) const;

private:
  static constexpr GPUTexture::Format FORMAT_FOR_24BIT = GPUTexture::Format::RGBA8; // RGBA8 always supported.

  GPUTexture* GetDisplayTexture(u32 width, u32 height, GPUTexture::Format format);

  FixedHeapArray<u8, GPU_MAX_DISPLAY_WIDTH * GPU_MAX_DISPLAY_HEIGHT * sizeof(u32)> m_upload_buffer;
  GPUTexture::Format m_16bit_display_format = GPUTexture::Format::Unknown;
  std::unique_ptr<GPUTexture> m_upload_texture;

  GPU_SW_Backend m_backend;
};
