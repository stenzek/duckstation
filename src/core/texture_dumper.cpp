#include "texture_dumper.h"
#include "common/align.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/platform.h"
#include "common/string_util.h"
#include "common/timer.h"
#include "settings.h"
#include "system.h"
#include "texture_replacements.h"
#include <cinttypes>
#include <vector>
Log_SetChannel(TextureDumper);

namespace TextureDumper {

using U32Rectangle = Common::Rectangle<u32>;
using Palette = std::vector<u32>;

struct VRAMWrite
{
  union PixelValue
  {
    u32 bits;

    BitField<u32, u32, 0, 10> palette_x;
    BitField<u32, u32, 10, 10> palette_y;
    BitField<u32, GPUTextureMode, 20, 2> mode;
    BitField<u32, u32, 0, 22> palette_and_mode_bits;
    BitField<u32, bool, 22, 1> transparent;

    ALWAYS_INLINE bool IsValid() const { return (bits != 0xFFFFFFFFu); }
    ALWAYS_INLINE void SetInvalid() { bits = 0xFFFFFFFFu; }
    ALWAYS_INLINE void Set(u32 palette_x_, u32 palette_y_, GPUTextureMode mode_, bool transparent_)
    {
      palette_x = palette_x_;
      palette_y = palette_y_;
      mode = mode_;
      transparent = transparent_;
    }

    ALWAYS_INLINE static PixelValue InvalidValue() { return PixelValue{0xFFFFFFFFu}; }

    ALWAYS_INLINE bool operator==(const PixelValue& rhs) const { return bits == rhs.bits; }
    ALWAYS_INLINE bool operator!=(const PixelValue& rhs) const { return bits != rhs.bits; }

    ALWAYS_INLINE PixelValue& operator=(const PixelValue& rhs) { bits = rhs.bits; }
  };

  TextureReplacementHash hash;
  U32Rectangle rect;
  std::vector<PixelValue> palette_values;
  std::unordered_map<u32, Palette> palettes;
  u32 resolved_pixels;
  bool can_merge;

  ALWAYS_INLINE u32 GetWidth() const { return rect.GetWidth(); }
  ALWAYS_INLINE u32 GetHeight() const { return rect.GetHeight(); }
};

using PendingVRAMWriteList = std::vector<std::unique_ptr<VRAMWrite>>;

static std::string GetVRAMWriteDumpFilename(u32 width, u32 height, const void* pixels);
static std::string GetTextureDumpFilename(const VRAMWrite& vrw, GPUTextureMode mode, const U32Rectangle& rect);
static std::optional<GPUTextureMode> GetTextureDumpMode(const VRAMWrite& vrw);
static void DumpVRAMWriteForDisplay(u32 width, u32 height, const void* pixels);

static void AddPaletteToVRAMWrite(VRAMWrite* vrw, const VRAMWrite::PixelValue& pv);
static bool CanDumpPendingVRAMWrite(const VRAMWrite& vrw, bool invalidating);
static bool TryMergingVRAMWrite(const VRAMWrite& vrw);
static void DumpVRAMWriteForTexture(const VRAMWrite& vrw);
static void DumpPendingWrites();

static PendingVRAMWriteList s_pending_vram_writes;

static std::array<u16, VRAM_WIDTH * VRAM_HEIGHT> s_vram_shadow{};

enum : u32
{
  VRAM_WRITE_CELL_WIDTH = 64,
  VRAM_WRITE_CELL_HEIGHT = 64,
  VRAM_WRITE_NUM_CELLS_X = (VRAM_WIDTH + VRAM_WRITE_CELL_WIDTH - 1) / VRAM_WRITE_CELL_WIDTH,
  VRAM_WRITE_NUM_CELLS_Y = (VRAM_HEIGHT + VRAM_WRITE_CELL_HEIGHT - 1) / VRAM_WRITE_CELL_HEIGHT,
};

using VRAMCellType = std::vector<VRAMWrite*>;

ALWAYS_INLINE u32 GetCellX(u32 x)
{
  return x / VRAM_WRITE_CELL_WIDTH;
}
ALWAYS_INLINE u32 GetCellY(u32 y)
{
  return y / VRAM_WRITE_CELL_HEIGHT;
}

static VRAMCellType s_vram_write_cells[VRAM_WRITE_NUM_CELLS_X][VRAM_WRITE_NUM_CELLS_Y];

static void AddVRAMWriteToGrid(VRAMWrite* vrw)
{
  const u32 sx = GetCellX(vrw->rect.left);
  const u32 sy = GetCellY(vrw->rect.top);
  const u32 ex = GetCellX(vrw->rect.right - 1);
  const u32 ey = GetCellY(vrw->rect.bottom - 1);

  for (u32 cy = sy; cy <= ey; cy++)
  {
    for (u32 cx = sx; cx <= ex; cx++)
    {
      VRAMCellType& cell = s_vram_write_cells[cy][cx];
      cell.push_back(vrw);
    }
  }
}

static void AddVRAMWrite(std::unique_ptr<VRAMWrite> vrw)
{
  Log_DevPrintf("Now tracking %ux%u VRAM write at %u,%u", vrw->rect.GetWidth(), vrw->rect.GetHeight(), vrw->rect.left,
                vrw->rect.top);

  AddVRAMWriteToGrid(vrw.get());
  s_pending_vram_writes.push_back(std::move(vrw));
}

static void RemoveVRAMWriteFromGrid(VRAMWrite* vrw)
{
  const u32 sx = GetCellX(vrw->rect.left);
  const u32 sy = GetCellY(vrw->rect.top);
  const u32 ex = GetCellX(vrw->rect.right - 1);
  const u32 ey = GetCellY(vrw->rect.bottom - 1);
  for (u32 cy = sy; cy <= ey; cy++)
  {
    for (u32 cx = sx; cx <= ex; cx++)
    {
      VRAMCellType& cell = s_vram_write_cells[cy][cx];
      auto iter = std::find(cell.begin(), cell.end(), vrw);
      Assert(iter != cell.end());
      cell.erase(iter);
    }
  }
}

static void RemoveVRAMWrite(VRAMWrite* vrw)
{
  Log_DevPrintf("No longer tracking %ux%u VRAM write at %u,%u", vrw->rect.GetWidth(), vrw->rect.GetHeight(),
                vrw->rect.left, vrw->rect.top);

  RemoveVRAMWriteFromGrid(vrw);

  for (auto iter = s_pending_vram_writes.begin(); iter != s_pending_vram_writes.end(); ++iter)
  {
    if (iter->get() == vrw)
    {
      s_pending_vram_writes.erase(iter);
      break;
    }
  }
}

void AddClear(u32 x, u32 y, u32 width, u32 height)
{
  // purge overlapping copies
  const U32Rectangle rect(x, y, x + width, y + height);
  const u32 sx = GetCellX(rect.left);
  const u32 sy = GetCellY(rect.top);
  const u32 ex = GetCellX(rect.right - 1);
  const u32 ey = GetCellY(rect.bottom - 1);
  for (u32 cy = sy; cy <= ey; cy++)
  {
    for (u32 cx = sx; cx <= ex; cx++)
    {
      VRAMCellType& cell = s_vram_write_cells[cy][cx];
      for (u32 i = 0; i < static_cast<u32>(cell.size());)
      {
        VRAMWrite* pvw = cell[i];
        if (!pvw->rect.Intersects(rect))
        {
          i++;
          continue;
        }

        if (CanDumpPendingVRAMWrite(*pvw, true))
          DumpVRAMWriteForTexture(*pvw);

        RemoveVRAMWrite(pvw);
      }
    }
  }
}

void AddVRAMWrite(u32 x, u32 y, u32 width, u32 height, const void* pixels)
{
  if (g_settings.texture_replacements.dump_vram_writes &&
      width >= g_settings.texture_replacements.dump_vram_write_width_threshold &&
      height >= g_settings.texture_replacements.dump_vram_write_height_threshold)
  {
    DumpVRAMWriteForDisplay(width, height, pixels);
  }

  // TODO: oversized copies
  if ((x + width) > VRAM_WIDTH || (y + height) > VRAM_HEIGHT)
  {
    Log_ErrorPrintf("Skipping %ux%u oversized write to %u,%u", width, height, x, y);
    return;
  }

  // purge overlapping copies
  const U32Rectangle rect(x, y, x + width, y + height);
  const u32 sx = GetCellX(rect.left);
  const u32 sy = GetCellY(rect.top);
  const u32 ex = GetCellX(rect.right - 1);
  const u32 ey = GetCellY(rect.bottom - 1);
  for (u32 cy = sy; cy <= ey; cy++)
  {
    for (u32 cx = sx; cx <= ex; cx++)
    {
      VRAMCellType& cell = s_vram_write_cells[cy][cx];
      for (u32 i = 0; i < static_cast<u32>(cell.size());)
      {
        VRAMWrite* pvw = cell[i];
        if (!pvw->rect.Intersects(rect))
        {
          i++;
          continue;
        }

        if (CanDumpPendingVRAMWrite(*pvw, true))
          DumpVRAMWriteForTexture(*pvw);

        RemoveVRAMWrite(pvw);
      }
    }
  }

  for (u32 row = 0; row < height; row++)
  {
    u16* dst = &s_vram_shadow[(y + row) * VRAM_WIDTH + x];
    const u8* src = reinterpret_cast<const u8*>(pixels) + ((row * width) * sizeof(u16));
    std::memcpy(dst, src, sizeof(u16) * width);
  }

  std::unique_ptr<VRAMWrite> vrw = std::make_unique<VRAMWrite>();
  vrw->hash = g_texture_replacements.GetVRAMWriteHash(width, height, pixels);
  vrw->rect = rect;
  vrw->palette_values.resize(width * height, VRAMWrite::PixelValue::InvalidValue());
  vrw->resolved_pixels = 0;
  vrw->can_merge = true;

  Log_VerbosePrintf("New VRAM %ux%u write at %u,%u", width, height, x, y);

  if (!TryMergingVRAMWrite(*vrw))
    AddVRAMWrite(std::move(vrw));
}

bool TryMergingVRAMWrite(const VRAMWrite& vrw)
{
  const u32 max_merge_width = g_settings.texture_replacements.dump_textures_max_merge_width;
  const u32 max_merge_height = g_settings.texture_replacements.dump_textures_max_merge_height;

  const u32 max_mergee_width = g_settings.texture_replacements.dump_textures_max_mergee_width;
  const u32 max_mergee_height = g_settings.texture_replacements.dump_textures_max_mergee_height;
  if (vrw.rect.GetWidth() > max_mergee_width || vrw.rect.GetHeight() > max_mergee_height)
    return false;

  const u32 sx = GetCellX(vrw.rect.left);
  const u32 sy = GetCellY(vrw.rect.top);
  const u32 ex = GetCellX(vrw.rect.right - 1);
  const u32 ey = GetCellY(vrw.rect.bottom - 1);
  for (u32 cy = sy; cy <= ey; cy++)
  {
    for (u32 cx = sx; cx <= ex; cx++)
    {
      for (VRAMWrite* other_vrw : s_vram_write_cells[cy][cx])
      {
        if (!other_vrw->can_merge || other_vrw->rect.GetWidth() > max_merge_width ||
            other_vrw->rect.GetHeight() > max_merge_height)
        {
          continue;
        }

        // left/right
        if (other_vrw->rect.top == vrw.rect.top && other_vrw->rect.bottom == vrw.rect.bottom)
        {
          if (other_vrw->rect.right == vrw.rect.left)
          {
            // merge to right of other_vrw
            Log_VerbosePrintf("  Merging right %ux%u @ %u,%u with %ux%u @ %u,%u", vrw.rect.GetWidth(),
                              vrw.rect.GetHeight(), vrw.rect.left, vrw.rect.top, other_vrw->rect.GetWidth(),
                              other_vrw->rect.GetHeight(), other_vrw->rect.left, other_vrw->rect.top);

            RemoveVRAMWriteFromGrid(other_vrw);
            other_vrw->rect.right = vrw.rect.right;
            goto merge;
          }
          else if (other_vrw->rect.left == vrw.rect.right)
          {
            // merge to left of other_vrw
            Log_VerbosePrintf("  Merging left %ux%u @ %u,%u with %ux%u @ %u,%u", vrw.rect.GetWidth(),
                              vrw.rect.GetHeight(), vrw.rect.left, vrw.rect.top, other_vrw->rect.GetWidth(),
                              other_vrw->rect.GetHeight(), other_vrw->rect.left, other_vrw->rect.top);

            RemoveVRAMWriteFromGrid(other_vrw);
            other_vrw->rect.left = vrw.rect.left;
            goto merge;
          }
        }

        // top/bottom
        if (other_vrw->rect.left == vrw.rect.left && other_vrw->rect.right == vrw.rect.right)
        {
          if (other_vrw->rect.bottom == vrw.rect.top)
          {
            // merge to bottom of other_vrw
            Log_VerbosePrintf("  Merging bottom %ux%u @ %u,%u with %ux%u @ %u,%u", vrw.rect.GetWidth(),
                              vrw.rect.GetHeight(), vrw.rect.left, vrw.rect.top, other_vrw->rect.GetWidth(),
                              other_vrw->rect.GetHeight(), other_vrw->rect.left, other_vrw->rect.top);

            RemoveVRAMWriteFromGrid(other_vrw);
            other_vrw->rect.bottom = vrw.rect.bottom;
            goto merge;
          }
          else if (other_vrw->rect.top == vrw.rect.bottom)
          {
            // merge to top of other_vrw
            Log_VerbosePrintf("  Merging top %ux%u @ %u,%u with %ux%u @ %u,%u", vrw.rect.GetWidth(),
                              vrw.rect.GetHeight(), vrw.rect.left, vrw.rect.top, other_vrw->rect.GetWidth(),
                              other_vrw->rect.GetHeight(), other_vrw->rect.left, other_vrw->rect.top);

            RemoveVRAMWriteFromGrid(other_vrw);
            other_vrw->rect.top = vrw.rect.top;
            goto merge;
          }
        }

        continue;

      merge:
        other_vrw->palette_values.clear();
        other_vrw->palette_values.resize(other_vrw->rect.GetWidth() * other_vrw->rect.GetHeight(),
                                         VRAMWrite::PixelValue::InvalidValue());
        AddVRAMWriteToGrid(other_vrw);
        return true;
      }
    }
  }

  return false;
}

void AddDraw(u16 draw_mode, u16 palette, u32 min_uv_x, u32 min_uv_y, u32 max_uv_x, u32 max_uv_y, bool transparent)
{
  const GPUDrawModeReg drawmode_reg{draw_mode};
  const GPUTextureMode texture_mode = drawmode_reg.texture_mode;
  const u32 page_x = drawmode_reg.GetTexturePageBaseX();
  const u32 page_y = drawmode_reg.GetTexturePageBaseY();

  u32 min_uv_x_vram = min_uv_x;
  u32 max_uv_x_vram = max_uv_x;
  switch (texture_mode)
  {
    case GPUTextureMode::Palette4Bit:
      min_uv_x_vram = (min_uv_x + 3) / 4;
      max_uv_x_vram = (max_uv_x + 3) / 4;
      break;

    case GPUTextureMode::Palette8Bit:
      min_uv_x_vram = (min_uv_x + 1) / 2;
      max_uv_x_vram = (max_uv_x + 1) / 2;
      break;

    default:
      break;
  }

#if 0
  if (texture_mode == GPUTextureMode::Direct16Bit)
  {
    Log_WarningPrintf("16bit @ %u,%u %u,%u", page_x + min_uv_x_vram, page_y + min_uv_y, page_x + max_uv_x_vram,
                      page_y + max_uv_y);
  }
#endif

  const U32Rectangle uv_rect(page_x + min_uv_x_vram, page_y + min_uv_y, page_x + max_uv_x_vram + 1u,
                             page_y + max_uv_y + 1u);
  const GPUTexturePaletteReg palette_reg{palette};
  const u32 palette_x = palette_reg.GetXBase();
  const u32 palette_y = palette_reg.GetYBase();

  VRAMWrite::PixelValue ppv{};
  ppv.Set(palette_x, palette_y, texture_mode, transparent);

  const u32 sx = GetCellX(uv_rect.left);
  const u32 sy = GetCellY(uv_rect.top);
  const u32 ex = GetCellX(uv_rect.right - 1);
  const u32 ey = GetCellY(uv_rect.bottom - 1);
  for (u32 cy = sy; cy <= ey; cy++)
  {
    for (u32 cx = sx; cx <= ex; cx++)
    {
      VRAMCellType& cell = s_vram_write_cells[cy][cx];
      for (u32 i = 0; i < static_cast<u32>(cell.size());)
      {
        VRAMWrite* pvw = cell[i];
        if (!pvw->rect.Intersects(uv_rect))
        {
          i++;
          continue;
        }

        U32Rectangle cropped = pvw->rect;
        if (cropped.left < uv_rect.left)
          cropped.left += (uv_rect.left - cropped.left);
        if (cropped.right > uv_rect.right)
          cropped.right -= (cropped.right - uv_rect.right);
        if (cropped.top < uv_rect.top)
          cropped.top += (uv_rect.top - cropped.top);
        if (cropped.bottom > uv_rect.bottom)
          cropped.bottom -= (cropped.bottom - uv_rect.bottom);

        const u32 left_in_write = cropped.left - pvw->rect.left;
        const u32 top_in_write = cropped.top - pvw->rect.top;
        const u32 right_in_write = cropped.right - pvw->rect.left;
        const u32 bottom_in_write = cropped.bottom - pvw->rect.top;

        const u32 stride = pvw->rect.GetWidth();
        u32 num_resolved = 0;

        for (u32 row = top_in_write; row < bottom_in_write; row++)
        {
          VRAMWrite::PixelValue* pvp = &pvw->palette_values[row * stride];
          for (u32 col = left_in_write; col < right_in_write; col++)
          {
            if (pvp[col].bits == ppv.bits)
              continue;

            if (!pvp[col].IsValid())
            {
              pvp[col].bits = ppv.bits;
              pvw->resolved_pixels++;
              num_resolved++;
            }
            else if (pvp[col].mode != texture_mode)
            {
#if 0
              Log_WarningPrintf("%u,%u changed %u[%u,%u] -> %u[%u,%u]", uv_rect.left + col, uv_rect.top + row,
                                pvp[col].mode.GetValue(), pvp[col].palette_x.GetValue(), pvp[col].palette_y.GetValue(),
                                ppv.mode.GetValue(), ppv.palette_x.GetValue(), ppv.palette_y.GetValue());
#endif
              pvp[col].bits = ppv.bits;
            }
          }
        }

        if (num_resolved > 0)
          AddPaletteToVRAMWrite(pvw, ppv);

        if (CanDumpPendingVRAMWrite(*pvw, false))
        {
          DumpVRAMWriteForTexture(*pvw);
          RemoveVRAMWrite(pvw);
        }
        else
        {
          // since we've written palette values, this can no longer by merged
          pvw->can_merge = false;
          i++;
        }
      }
    }
  }
}

static constexpr u8 UNRESOLVED_ALPHA_VALUE = 254;

ALWAYS_INLINE bool IsUnresolvedColor(u32 color)
{
  return ((color >> 24) == UNRESOLVED_ALPHA_VALUE);
}

ALWAYS_INLINE u32 MarkAsUnresolved(u32 color)
{
  return (color & 0xFFFFFFu) | (ZeroExtend32(UNRESOLVED_ALPHA_VALUE) << 24);
}

ALWAYS_INLINE static u32 ConvertVRAMPixelForDumping(u16 color, bool force_alpha)
{
  if (color == 0)
    return 0;

  const u32 r = VRAMConvert5To8(color & 31u);
  const u32 g = VRAMConvert5To8((color >> 5) & 31u);
  const u32 b = VRAMConvert5To8((color >> 10) & 31u);
  const u32 a = force_alpha ? 255 : (((color >> 15) != 0) ? 255 : 128);
  return ZeroExtend32(r) | (ZeroExtend32(g) << 8) | (ZeroExtend32(b) << 16) | (ZeroExtend32(a) << 24);
}

template<u32 size>
static constexpr std::array<u32, size> MakeGreyscalePalette()
{
  const u8 increment = static_cast<u8>(256u / size);
  u8 value = 0;
  std::array<u32, size> colours{};
  for (u32 i = 0; i < size; i++)
  {
    colours[i] = ZeroExtend32(value) | (ZeroExtend32(value) << 8) | (ZeroExtend32(value) << 16) |
                 (ZeroExtend32(UNRESOLVED_ALPHA_VALUE) << 24);
    value += increment;
  }

  return colours;
}

void AddPaletteToVRAMWrite(VRAMWrite* vrw, const VRAMWrite::PixelValue& pv)
{
  if (pv.mode >= GPUTextureMode::Direct16Bit || vrw->palettes.find(pv.bits) != vrw->palettes.end())
    return;

  const bool force_alpha = !pv.transparent || g_settings.texture_replacements.dump_textures_force_alpha_channel;

  Palette palette;
  switch (pv.mode)
  {
    case GPUTextureMode::Palette4Bit:
    {
      const u16* base = &s_vram_shadow[pv.palette_y * VRAM_WIDTH + pv.palette_x];
      palette.reserve(16);
      for (u32 i = 0; i < 16; i++)
        palette.push_back(ConvertVRAMPixelForDumping(base[i], force_alpha));
    }
    break;

    case GPUTextureMode::Palette8Bit:
    {
      const u16* base = &s_vram_shadow[pv.palette_y * VRAM_WIDTH + pv.palette_x];
      palette.reserve(256);
      for (u32 i = 0; i < 256; i++)
        palette.push_back(ConvertVRAMPixelForDumping(base[i], force_alpha));
    }
    break;

    default:
      break;
  }

  vrw->palettes.emplace(pv.bits, std::move(palette));
}

std::optional<GPUTextureMode> GetTextureDumpMode(const VRAMWrite& vrw)
{
  GPUTextureMode mode = GPUTextureMode::Disabled;
  for (const VRAMWrite::PixelValue& pv : vrw.palette_values)
  {
    if (!pv.IsValid() || pv.mode == mode)
      continue;

    if (mode == GPUTextureMode::Disabled)
    {
      mode = pv.mode;
      continue;
    }

    Log_ErrorPrintf("VRAM write has multiple texture modes");
    return std::nullopt;
  }

  if (mode == GPUTextureMode::Disabled)
    return std::nullopt;

  return mode;
}

void DumpVRAMWriteForDisplay(u32 width, u32 height, const void* pixels)
{
  std::string filename = GetVRAMWriteDumpFilename(width, height, pixels);
  if (filename.empty() || FileSystem::FileExists(filename.c_str()))
    return;

  Common::RGBA8Image image;
  image.SetSize(width, height);

  const u16* src_pixels = reinterpret_cast<const u16*>(pixels);
  const bool force_alpha = g_settings.texture_replacements.dump_vram_write_force_alpha_channel;

  for (u32 y = 0; y < height; y++)
  {
    for (u32 x = 0; x < width; x++)
    {
      image.SetPixel(x, y, ConvertVRAMPixelForDumping(*src_pixels, force_alpha));
      src_pixels++;
    }
  }

  Log_InfoPrintf("Dumping %ux%u VRAM write to '%s'", width, height, filename.c_str());
  if (!image.SaveToFile(filename.c_str()))
    Log_ErrorPrintf("Failed to dump %ux%u VRAM write to '%s'", width, height, filename.c_str());
}

bool CanDumpPendingVRAMWrite(const VRAMWrite& vrw, bool invalidating)
{
  const u32 total_pixels = static_cast<u32>(vrw.palette_values.size());
  const u32 valid_pixels = vrw.resolved_pixels;
  if (!invalidating)
    return valid_pixels == total_pixels;

#if 0
  const u32 percent = (valid_pixels * 100) / total_pixels;
  const u32 threshold = invalidating ? 10 : 95;
  return (percent >= threshold);
#endif

  return valid_pixels > 0;
}

static const VRAMWrite::PixelValue* GetNeighbourValidValue(const VRAMWrite& vrw, GPUTextureMode mode, u32 x, u32 y)
{
#if 0
  // don't search outside of the texture page
  const u32 min_vram_search_x = std::max(vrw.rect.left, Common::AlignDown(vrw.rect.left + x, 64));
  const u32 max_vram_search_x = std::min(vrw.rect.right, min_vram_search_x + 64u);
  const u32 min_vram_search_y = std::max(vrw.rect.top, Common::AlignDown(vrw.rect.top + y, 256));
  const u32 max_vram_search_y = std::min(vrw.rect.bottom, min_vram_search_y + 256u);
  const u32 min_search_x = min_vram_search_x - vrw.rect.left;
  const u32 max_search_x = max_vram_search_x - vrw.rect.left;
  const u32 min_search_y = min_vram_search_y - vrw.rect.top;
  const u32 max_search_y = max_vram_search_y - vrw.rect.top;
#else
  static constexpr u32 search_size = 32;
  const u32 min_search_x = x - std::min(x, search_size);
  const u32 max_search_x = x + std::min(search_size, vrw.rect.GetWidth() - x);
  const u32 min_search_y = y - std::min(y, search_size);
  const u32 max_search_y = y + std::min(search_size, vrw.rect.GetHeight() - y);
#endif
  DebugAssert(min_search_x < vrw.rect.GetWidth() && max_search_x <= vrw.rect.GetWidth());
  DebugAssert(min_search_y < vrw.rect.GetHeight() && max_search_y <= vrw.rect.GetHeight());

  const u32 stride = vrw.rect.GetWidth();
  const VRAMWrite::PixelValue* pv = nullptr;
  u32 pv_distance = std::numeric_limits<u32>::max();

  for (u32 search_y = min_search_y; search_y < max_search_y; search_y++)
  {
    for (u32 search_x = min_search_x; search_x < max_search_x; search_x++)
    {
      const VRAMWrite::PixelValue& check_pv = vrw.palette_values[search_y * stride + search_x];
      if (!check_pv.IsValid() || check_pv.mode != mode)
        continue;

      const u32 dx = static_cast<u32>(std::abs(static_cast<s32>(x) - static_cast<s32>(search_x)));
      const u32 dy = static_cast<u32>(std::abs(static_cast<s32>(y) - static_cast<s32>(search_y)));
      const u32 check_distance = (dx * dx + dy * dy);
      if (check_distance < pv_distance)
      {
        pv = &check_pv;
        pv_distance = check_distance;
      }
    }
  }

  return pv;
}

static u32 ResolvePalette(const VRAMWrite& vrw, const VRAMWrite::PixelValue& pv, u8 index)
{
  if (auto iter = vrw.palettes.find(pv.bits); iter != vrw.palettes.end())
    return iter->second[index];
  else
    return ConvertVRAMPixelForDumping(s_vram_shadow[pv.palette_y * VRAM_WIDTH + pv.palette_x],
                                      !pv.transparent ||
                                        g_settings.texture_replacements.dump_textures_force_alpha_channel);
}

static bool DumpSubTexture(const VRAMWrite& vrw, GPUTextureMode mode, const U32Rectangle& rect)
{
  const std::string filename(GetTextureDumpFilename(vrw, mode, rect));
  if (filename.empty())
    return true;

  Common::RGBA8Image image;
  Common::RGBA8Image existing_image;

  switch (mode)
  {
    case GPUTextureMode::Palette4Bit:
    {
      const u32 width = rect.GetWidth() * 4;
      const u32 height = rect.GetHeight();
      image.SetSize(width, height);
    }
    break;

    case GPUTextureMode::Palette8Bit:
    {
      const u32 width = rect.GetWidth() * 2;
      const u32 height = rect.GetHeight();
      image.SetSize(width, height);
    }
    break;

    case GPUTextureMode::Direct16Bit:
    {
      const u32 width = rect.GetWidth();
      const u32 height = rect.GetHeight();
      image.SetSize(width, height);
    }
    break;

    default:
      break;
  }

  const u32 total_pixels = image.GetWidth() * image.GetHeight();

  u32 resolved_pixels = 0;
  u32 existing_resolved_pixels = 0;
  if (FileSystem::FileExists(filename.c_str()) && existing_image.LoadFromFile(filename.c_str()))
  {
    if (existing_image.GetWidth() == image.GetWidth() && existing_image.GetHeight() == image.GetHeight())
    {
      for (u32 pixel : existing_image)
      {
        if (!IsUnresolvedColor(pixel))
          existing_resolved_pixels++;
      }

      Log_VerbosePrintf("Loaded existing dump '%s' with %u of %u resolved pixels", filename.c_str(),
                        existing_resolved_pixels, total_pixels);
    }
    else
    {
      Log_WarningPrintf("Cannot use existing dump as palette resolve source due to dimension mismatch (%ux%u vs %ux%u)",
                        existing_image.GetWidth(), existing_image.GetHeight(), image.GetWidth(), image.GetHeight());
      existing_image.Invalidate();
    }
  }

  const u32 vrw_left = rect.left;
  const u32 vrw_top = rect.top;
  const u32 vram_left = vrw.rect.left + vrw_left;
  const u32 vram_top = vrw.rect.top + vrw_top;
  const u32 stride = vrw.rect.GetWidth();
  const u32 width = image.GetWidth();
  const u32 height = image.GetHeight();

  switch (mode)
  {
    case GPUTextureMode::Palette4Bit:
    {
      static constexpr std::array<u32, 16> fallback_palette = MakeGreyscalePalette<16>();

      for (u32 y = 0; y < height; y++)
      {
        const VRAMWrite::PixelValue* pvs = &vrw.palette_values[(vrw_top + y) * stride + vrw_left];
        const u16* vram = &s_vram_shadow[((vram_top + y) * VRAM_WIDTH) + vram_left];

        for (u32 x = 0; x < width; x++)
        {
          const VRAMWrite::PixelValue& pv = pvs[x / 4];
          // const u8 shift = Truncate8(x % 4);
          const u8 index = Truncate8(vram[x / 4] >> ((x % 4) * 4) & 0x0F);

          u32 color = fallback_palette[index];
          if (pv.IsValid())
          {
            color = ResolvePalette(vrw, pv, index);
            resolved_pixels++;
          }
          else if (existing_image.IsValid())
          {
            const u32 existing_pixel = existing_image.GetPixel(x, y);
            if (!IsUnresolvedColor(existing_pixel))
            {
              color = existing_pixel;
              resolved_pixels++;
            }
            else
            {
              const VRAMWrite::PixelValue* neighbour_pv = GetNeighbourValidValue(vrw, mode, x / 4, y);
              if (neighbour_pv)
                color = MarkAsUnresolved(ResolvePalette(vrw, *neighbour_pv, index));
            }
          }
          else
          {
            const VRAMWrite::PixelValue* neighbour_pv = GetNeighbourValidValue(vrw, mode, x / 4, y);
            if (neighbour_pv)
              color = MarkAsUnresolved(ResolvePalette(vrw, *neighbour_pv, index));
          }

          image.SetPixel(x, y, color);
        }
      }
    }
    break;

    case GPUTextureMode::Palette8Bit:
    {
      static constexpr std::array<u32, 256> fallback_palette = MakeGreyscalePalette<256>();

      for (u32 y = 0; y < height; y++)
      {
        const VRAMWrite::PixelValue* pvs = &vrw.palette_values[(vrw_top + y) * stride + vrw_left];
        const u16* vram = &s_vram_shadow[((vram_top + y) * VRAM_WIDTH) + vram_left];

        for (u32 x = 0; x < width; x++)
        {
          const VRAMWrite::PixelValue& pv = pvs[x / 2];
          // const u8 shift = Truncate8(x % 2);
          const u8 index = Truncate8(vram[x / 2] >> ((x % 2) * 8) & 0xFF);

          u32 color = fallback_palette[index];
          if (pv.IsValid())
          {
            color = ResolvePalette(vrw, pv, index);
            resolved_pixels++;
          }
          else if (existing_image.IsValid())
          {
            const u32 existing_pixel = existing_image.GetPixel(x, y);
            if (!IsUnresolvedColor(existing_pixel))
            {
              color = existing_pixel;
              resolved_pixels++;
            }
            else
            {
              const VRAMWrite::PixelValue* neighbour_pv = GetNeighbourValidValue(vrw, mode, x / 2, y);
              if (neighbour_pv)
                color = MarkAsUnresolved(ResolvePalette(vrw, *neighbour_pv, index));
            }
          }
          else
          {
            const VRAMWrite::PixelValue* neighbour_pv = GetNeighbourValidValue(vrw, mode, x / 2, y);
            if (neighbour_pv)
              color = MarkAsUnresolved(ResolvePalette(vrw, *neighbour_pv, index));
          }

          image.SetPixel(x, y, color);
        }
      }
    }
    break;

    case GPUTextureMode::Direct16Bit:
    {
      const bool force_alpha = g_settings.texture_replacements.dump_textures_force_alpha_channel;

      for (u32 y = 0; y < height; y++)
      {
        const u16* vram = &s_vram_shadow[((vram_top + y) * VRAM_WIDTH) + vram_left];
        for (u32 x = 0; x < width; x++)
        {
          image.SetPixel(x, y, ConvertVRAMPixelForDumping(vram[x], force_alpha));
          resolved_pixels++;
        }
      }
    }
    break;

    default:
      break;
  }

  // Did we change?
  if (existing_image.IsValid() && resolved_pixels == existing_resolved_pixels)
  {
    Log_VerbosePrintf("Skipping dumping '%s' because no new pixels were resolved", filename.c_str());
    return true;
  }

  Log_InfoPrintf("Dumping %ux%u texture to '%s'", image.GetWidth(), image.GetHeight(), filename.c_str());
  if (!image.SaveToFile(filename.c_str()))
  {
    Log_ErrorPrintf("Failed to dump %ux%u texture to '%s'", image.GetWidth(), image.GetHeight(), filename.c_str());
    return false;
  }

  return true;
}

static bool DumpSingleTexture(const VRAMWrite& vrw)
{
  std::optional<GPUTextureMode> mode = GetTextureDumpMode(vrw);
  if (mode.has_value())
    return DumpSubTexture(vrw, mode.value(), U32Rectangle(0, 0, vrw.GetWidth(), vrw.GetHeight()));
  else
    return false;
}

static void DumpSubTexturesByCLUT(const VRAMWrite& vrw)
{
  const u32 width = vrw.GetWidth();
  const u32 height = vrw.GetHeight();
  std::vector<bool> done(width * height);
  for (u32 y = 0; y < height; y++)
  {
    for (u32 x = 0; x < width; x++)
    {
      if (done[(y * width) + x])
        continue;

      const VRAMWrite::PixelValue& pv = vrw.palette_values[(y * width) + x];

      U32Rectangle rect(x, y, x + 1, y + 1);

#if 0
      // grow the rectangle by checking the east, south-east and south texels
      while (rect.right < width && rect.bottom < height)
      {
        const u32 east_index = (y * width) + x;
        const u32 southeast_index = ((y + 1) * width) + (x + 1);
        const u32 south_index = ((y + 1) * width) + x;
        if (done[east_index] || done[southeast_index] || done[south_index])
          break;

        const VRAMWrite::PixelValue& east_pv = vrw.palette_values[east_index];
        const VRAMWrite::PixelValue& southeast_pv = vrw.palette_values[southeast_index];
        const VRAMWrite::PixelValue& south_pv = vrw.palette_values[south_index];
        if (pv != east_pv || pv != southeast_pv || pv != south_pv)
          break;

        rect.right++;
        rect.bottom++;
      }
#else
      // grow to the east
      while (rect.right < width)
      {
        for (u32 check_y = rect.top; check_y < rect.bottom; check_y++)
        {
          const u32 check_index = check_y * width + rect.right;
          if (done[check_index] || vrw.palette_values[check_index].palette_and_mode_bits != pv.palette_and_mode_bits)
            goto cancel_east;
        }

        rect.right++;
        continue;

      cancel_east:
        break;
      }

      // grow to the south
      while (rect.bottom < height)
      {
        for (u32 check_x = rect.left; check_x < rect.right; check_x++)
        {
          const u32 check_index = rect.bottom * width + check_x;
          if (done[check_index] || vrw.palette_values[check_index].palette_and_mode_bits != pv.palette_and_mode_bits)
            goto cancel_south;
        }

        rect.bottom++;
        continue;

      cancel_south:
        break;
      }
#endif

      for (u32 mark_y = rect.top; mark_y < rect.bottom; mark_y++)
      {
        for (u32 mark_x = rect.left; mark_x < rect.right; mark_x++)
          done[mark_y * width + mark_x] = true;
      }

      DumpSubTexture(vrw, pv.IsValid() ? pv.mode : GPUTextureMode::Palette4Bit, rect);
    }
  }
}

void DumpVRAMWriteForTexture(const VRAMWrite& vrw)
{
  bool dump_single_texture = g_settings.texture_replacements.dump_textures_by_vram_write;
  bool dump_sub_textures = g_settings.texture_replacements.dump_textures_by_palette;

  if (dump_single_texture)
  {
    if (!DumpSingleTexture(vrw) && !dump_sub_textures)
    {
      Log_WarningPrintf("Dumping single texture failed, force dumping sub-textures");
      dump_sub_textures = true;
    }
  }

  if (dump_sub_textures)
    DumpSubTexturesByCLUT(vrw);
}

void DumpPendingWrites()
{
  for (u32 i = 0; i < static_cast<u32>(s_pending_vram_writes.size());)
  {
    VRAMWrite* pvw = s_pending_vram_writes[i].get();
    if (CanDumpPendingVRAMWrite(*pvw, true))
    {
      DumpVRAMWriteForTexture(*pvw);
      RemoveVRAMWrite(pvw);
    }
    else
    {
      i++;
    }
  }
}

void ClearState()
{
  DumpPendingWrites();
  std::memset(s_vram_write_cells, 0, sizeof(s_vram_write_cells));
  s_pending_vram_writes.clear();
  s_vram_shadow.fill(0);
}

void Shutdown()
{
  ClearState();
}

std::string GetDumpDirectory()
{
  const std::string& code = System::GetRunningCode();
  if (code.empty())
    return Path::Combine(EmuFolders::Dumps, "textures");
  else
    return Path::Combine(EmuFolders::Dumps, Path::Combine("textures", code));
}

std::string GetVRAMWriteDumpFilename(u32 width, u32 height, const void* pixels)
{
  const std::string& game_id = System::GetRunningCode();
  if (game_id.empty())
    return {};

  const std::string dump_directory(GetDumpDirectory());
  if (!FileSystem::DirectoryExists(dump_directory.c_str()) &&
      !FileSystem::CreateDirectory(dump_directory.c_str(), false))
  {
    return {};
  }

  const TextureReplacementHash hash = g_texture_replacements.GetVRAMWriteHash(width, height, pixels);
  return Path::Combine(dump_directory, fmt::format("vram-write-{}.png", hash.ToString()));
}

std::string GetTextureDumpFilename(const VRAMWrite& vrw, GPUTextureMode mode, const U32Rectangle& rect)
{
  if (System::GetRunningCode().empty())
    return {};

  const std::string dump_directory(GetDumpDirectory());
  if (!FileSystem::DirectoryExists(dump_directory.c_str()) &&
      !FileSystem::CreateDirectory(dump_directory.c_str(), false))
  {
    return {};
  }

  return Path::Combine(dump_directory, fmt::format("texture-{}-{}-{}-{}-{}-{}.png", vrw.hash.ToString().c_str(),
                                                   static_cast<unsigned>(mode), rect.left, rect.top, rect.GetWidth(),
                                                   rect.GetHeight()));
}

} // namespace TextureDumper