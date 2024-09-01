// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "types.h"

#include "util/image.h"

#include <string>

namespace TextureReplacements {

using ReplacementImage = RGBA8Image;

enum class ReplacmentType
{
  VRAMWrite,
};

void SetGameID(std::string game_id);

void Reload();

const ReplacementImage* GetVRAMReplacement(u32 width, u32 height, const void* pixels);
void DumpVRAMWrite(u32 width, u32 height, const void* pixels);

void Shutdown();

} // namespace TextureReplacements
