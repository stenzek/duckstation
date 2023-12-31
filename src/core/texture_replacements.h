// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "gpu_types.h"

#include "util/image.h"

#include "common/gsvector.h"

#include <string>
#include <vector>

namespace TextureReplacements {

enum class ReplacementType : u8
{
  VRAMReplacement,
  TextureFromVRAMWrite,
  TextureFromPage,
};

using ReplacementImage = RGBA8Image;
using TextureSourceHash = u64;
using TexturePaletteHash = u64;

struct ReplacementSubImage
{
  GSVector4i dst_rect;
  GSVector4i src_rect;
  const ReplacementImage& image;
  float scale_x;
  float scale_y;
};

void SetGameID(std::string game_id);

void Reload();

const ReplacementImage* GetVRAMReplacement(u32 width, u32 height, const void* pixels);
void DumpVRAMWrite(u32 width, u32 height, const void* pixels);

void DumpTexture(ReplacementType type, const GSVector4i src_rect, TextureSourceHash src_hash, TexturePaletteHash pal_hash,
                 GPUTextureMode mode, GPUTexturePaletteReg palette, const GSVector4i rect);

bool HasVRAMWriteTextureReplacements();
void GetVRAMWriteTextureReplacements(std::vector<ReplacementSubImage>& replacements, TextureSourceHash vram_write_hash,
                                     TextureSourceHash palette_hash, GPUTextureMode mode, const GSVector2i& offset_to_page);

bool HasTexturePageTextureReplacements();
void GetTexturePageTextureReplacements(std::vector<ReplacementSubImage>& replacements, TextureSourceHash vram_write_hash,
                                       TextureSourceHash palette_hash, GPUTextureMode mode);

void Shutdown();

} // namespace TextureReplacements
