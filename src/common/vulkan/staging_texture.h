// Copyright 2016 Dolphin Emulator Project
// Copyright 2020 DuckStation Emulator Project
// Licensed under GPLv2+
// Refer to the LICENSE file included.

#pragma once
#include "staging_buffer.h"
#include "texture.h"

namespace Vulkan {

class StagingTexture final
{
public:
  StagingTexture();
  StagingTexture(StagingTexture&& move);
  StagingTexture(const StagingTexture&) = delete;
  ~StagingTexture();

  StagingTexture& operator=(StagingTexture&& move);
  StagingTexture& operator=(const StagingTexture&) = delete;

  ALWAYS_INLINE bool IsValid() const { return m_staging_buffer.IsValid(); }
  ALWAYS_INLINE bool IsMapped() const { return m_staging_buffer.IsMapped(); }
  ALWAYS_INLINE const char* GetMappedPointer() const { return m_staging_buffer.GetMapPointer(); }
  ALWAYS_INLINE char* GetMappedPointer() { return m_staging_buffer.GetMapPointer(); }
  ALWAYS_INLINE u32 GetMappedStride() const { return m_map_stride; }
  ALWAYS_INLINE u32 GetWidth() const { return m_width; }
  ALWAYS_INLINE u32 GetHeight() const { return m_height; }

  bool Create(StagingBuffer::Type type, VkFormat format, u32 width, u32 height);
  void Destroy(bool defer = true);

  // Copies from the GPU texture object to the staging texture, which can be mapped/read by the CPU.
  // Both src_rect and dst_rect must be with within the bounds of the the specified textures.
  void CopyFromTexture(VkCommandBuffer command_buffer, Texture& src_texture, u32 src_x, u32 src_y, u32 src_layer,
                       u32 src_level, u32 dst_x, u32 dst_y, u32 width, u32 height);
  void CopyFromTexture(Texture& src_texture, u32 src_x, u32 src_y, u32 src_layer, u32 src_level, u32 dst_x, u32 dst_y,
                       u32 width, u32 height);

  // Wrapper for copying a whole layer of a texture to a readback texture.
  // Assumes that the level of src texture and this texture have the same dimensions.
  void CopyToTexture(VkCommandBuffer command_buffer, u32 src_x, u32 src_y, Texture& dst_texture, u32 dst_x, u32 dst_y,
                     u32 dst_layer, u32 dst_level, u32 width, u32 height);
  void CopyToTexture(u32 src_x, u32 src_y, Texture& dst_texture, u32 dst_x, u32 dst_y, u32 dst_layer, u32 dst_level,
                     u32 width, u32 height);

  // Flushes pending writes from the CPU to the GPU, and reads from the GPU to the CPU.
  // This may cause a command buffer flush depending on if one has occurred between the last
  // call to CopyFromTexture()/CopyToTexture() and the Flush() call.
  void Flush();

  // Reads the specified rectangle from the staging texture to out_ptr, with the specified stride
  // (length in bytes of each row). CopyFromTexture must be called first. The contents of any
  // texels outside of the rectangle used for CopyFromTexture is undefined.
  void ReadTexels(u32 src_x, u32 src_y, u32 width, u32 height, void* out_ptr, u32 out_stride);
  void ReadTexel(u32 x, u32 y, void* out_ptr);

  // Copies the texels from in_ptr to the staging texture, which can be read by the GPU, with the
  // specified stride (length in bytes of each row). After updating the staging texture with all
  // changes, call CopyToTexture() to update the GPU copy.
  void WriteTexels(u32 dst_x, u32 dst_y, u32 width, u32 height, const void* in_ptr, u32 in_stride);
  void WriteTexel(u32 x, u32 y, const void* in_ptr);

private:
  void PrepareForAccess();

  StagingBuffer m_staging_buffer;
  u64 m_flush_fence_counter = 0;
  u32 m_width = 0;
  u32 m_height = 0;
  u32 m_texel_size = 0;
  u32 m_map_stride = 0;
  bool m_needs_flush = false;
};

} // namespace Vulkan