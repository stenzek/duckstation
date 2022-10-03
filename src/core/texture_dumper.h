#pragma once
#include "common/hash_combine.h"
#include "common/image.h"
#include "common/rectangle.h"
#include "gpu_types.h"
#include "types.h"
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace TextureDumper {
std::string GetDumpDirectory();

void ClearState();
void Shutdown();

void AddClear(u32 x, u32 y, u32 width, u32 height);
void AddVRAMWrite(u32 x, u32 y, u32 width, u32 height, const void* pixels);
void AddDraw(u16 draw_mode, u16 palette, u32 min_uv_x, u32 min_uv_y, u32 max_uv_x, u32 max_uv_y, bool transparent);
} // namespace TextureDumper
