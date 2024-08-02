// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "texture_replacements.h"
#include "gpu.h"
#include "gpu_hw_texture_cache.h"
#include "gpu_types.h"
#include "host.h"
#include "settings.h"

#include "util/ini_settings_interface.h"

#include "common/bitutils.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/gsvector_formatter.h"
#include "common/hash_combine.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"
#include "common/timer.h"

#include "fmt/format.h"

#define XXH_STATIC_LINKING_ONLY
#include "xxhash.h"
#ifdef CPU_ARCH_SSE
#include "xxh_x86dispatch.h"
#endif

#include <cinttypes>
#include <cstring>
#include <optional>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

Log_SetChannel(TextureReplacements);

namespace TextureReplacements {
namespace {
struct VRAMReplacementName
{
  u64 low;
  u64 high;

  TinyString ToString() const;
  bool Parse(const std::string_view file_title);

  bool operator<(const VRAMReplacementName& rhs) const { return std::tie(low, high) < std::tie(rhs.low, rhs.high); }
  bool operator==(const VRAMReplacementName& rhs) const { return low == rhs.low && high == rhs.high; }
  bool operator!=(const VRAMReplacementName& rhs) const { return low != rhs.low || high != rhs.high; }
};

struct VRAMReplacementNameHash
{
  size_t operator()(const VRAMReplacementName& hash) const;
};

struct TextureReplacementIndex
{
  u64 src_hash;
  GPUTextureMode mode;

  bool operator<(const TextureReplacementIndex& rhs) const
  {
    return std::tie(src_hash, mode) < std::tie(src_hash, mode);
  }
  bool operator==(const TextureReplacementIndex& rhs) const { return src_hash == rhs.src_hash && mode == rhs.mode; }
  bool operator!=(const TextureReplacementIndex& rhs) const { return src_hash != rhs.src_hash || mode != rhs.mode; }
};

struct TextureReplacementIndexHash
{
  size_t operator()(const TextureReplacementIndex& hash) const;
};

struct TextureReplacementName
{
  u64 src_hash;
  u64 pal_hash;
  u16 src_width;
  u16 src_height;
  ReplacementType type;
  u8 texture_mode;
  u16 offset_x;
  u16 offset_y;
  u16 width;
  u16 height;
  u8 pal_min;
  u8 pal_max;

  TinyString ToString() const;
  bool Parse(const std::string_view file_title);
  TextureReplacementIndex GetIndex() const;
  GPUTextureMode GetTextureMode() const;
  bool IsSemitransparent() const;

  bool operator<(const TextureReplacementName& rhs) const { return (std::memcmp(this, &rhs, sizeof(*this)) < 0); }
  bool operator==(const TextureReplacementName& rhs) const { return (std::memcmp(this, &rhs, sizeof(*this)) == 0); }
  bool operator!=(const TextureReplacementName& rhs) const { return (std::memcmp(this, &rhs, sizeof(*this)) != 0); }
};

struct DumpedTextureKey
{
  TextureSourceHash tex_hash;
  TexturePaletteHash pal_hash;
  u16 offset_x, offset_y;
  u16 width, height;
  ReplacementType type;
  u8 texture_mode;
  u8 pad[6];

  ALWAYS_INLINE bool operator==(const DumpedTextureKey& k) const
  {
    return (std::memcmp(&k, this, sizeof(DumpedTextureKey)) == 0);
  }
  ALWAYS_INLINE bool operator!=(const DumpedTextureKey& k) const
  {
    return (std::memcmp(&k, this, sizeof(DumpedTextureKey)) != 0);
  }
};
struct DumpedTextureKeyHash
{
  size_t operator()(const DumpedTextureKey& k) const;
};
} // namespace

using TextureCache = std::unordered_map<std::string, ReplacementImage>;

using VRAMReplacementMap = std::unordered_map<VRAMReplacementName, std::string, VRAMReplacementNameHash>;
using TextureReplacementMap =
  std::unordered_multimap<TextureReplacementIndex, std::pair<TextureReplacementName, std::string>,
                          TextureReplacementIndexHash>;

static std::optional<ReplacementType> GetReplacementTypeFromFileTitle(const std::string_view file_title);
static bool HasValidReplacementExtension(const std::string_view path);

static bool EnsureGameDirectoryExists();
static std::string GetSourceDirectory();
static std::string GetDumpDirectory();

static VRAMReplacementName GetVRAMWriteHash(u32 width, u32 height, const void* pixels);
static std::string GetVRAMWriteDumpFilename(u32 width, u32 height, const void* pixels);

static bool IsMatchingReplacementPalette(TexturePaletteHash full_palette_hash, GPUTextureMode mode,
                                         GPUTexturePaletteReg palette, const TextureReplacementName& name);
static void GetTextureReplacements(TextureReplacementMap& map, std::vector<ReplacementSubImage>& replacements,
                                   TextureSourceHash vram_write_hash, TextureSourceHash palette_hash,
                                   GPUTextureMode mode, GPUTexturePaletteReg palette, const GSVector2i& offset_to_page);

static bool LoadLocalConfiguration();

static void FindTextures(const std::string& dir);

static const ReplacementImage* LoadTexture(const std::string& filename);
static void PreloadTextures();
static void PurgeUnreferencedTexturesFromCache();

static std::string s_game_id;
static Settings::TextureReplacementSettings::Configuration s_config;

// TODO: Check the size, purge some when it gets too large.
static TextureCache s_texture_cache;

static VRAMReplacementMap s_vram_replacements;

// TODO: Combine these into one map?
static TextureReplacementMap s_vram_write_texture_replacements;
static TextureReplacementMap s_texture_page_texture_replacements;

static std::unordered_set<DumpedTextureKey, DumpedTextureKeyHash> s_dumped_textures;
} // namespace TextureReplacements

static_assert(std::is_same_v<TextureReplacements::TextureSourceHash, GPUTextureCache::HashType>);
static_assert(std::is_same_v<TextureReplacements::TexturePaletteHash, GPUTextureCache::HashType>);

static constexpr const char CONFIGURATION_TEMPLATE[] = R"(;DuckStation Texture Replacement Configuration
; This file allows you to set a per-game configuration for the dumping and
; replacement system, avoiding the need to use the normal per-game settings
; when moving files to a different computer. It also allows for the definition
; of texture aliases, for reducing duplicate files.
;
; All options are commented out by default. If an option is commented, the user's
; current setting will be used instead. If an option is defined in this file, it
; will always take precedence over the user's choice.

[Configuration]
; Enables texture page dumping mode.
; Instead of tracking VRAM writes and attempting to identify the "real" size of
; textures, entire 256x256 pages will be dumped/replaced instead. In most games,
; this will lead to significant duplication in dumps, and reduce replacement
; reliability. However, some games are incompatible with write tracking, and must
; use page mode.
;DumpTexturePages = false

; Enables the dumping of direct textures (i.e. C16 format).
; Most games do not use direct textures, and when they do, it is usually for
; post-processing or FMVs. Ignoring C16 textures typically reduces garbage/false
; positive texture dumps, however, some games may require it.
;DumpC16Textures = false

; Reduces the size of palettes (i.e. CLUTs) to only those indices that are used.
; This can help reduce duplication and improve replacement reliability in games
; that use 8-bit textures, but do not reserve or use the full 1x256 region in
; video memory for storage of the palette. When replacing textures dumped with
; this option enabled, CPU usage on the GPU thread does increase trivially,
; however, generally it is worthwhile for the reliability improvement. Games
; that require this option include Metal Gear Solid.
;ReducePaletteRange = true

; Converts VRAM copies to VRAM writes, when a copy of performed into a previously
; tracked VRAM write. This is required for some games that construct animated
; textures by copying and replacing small portions of the texture with the parts
; that are animated. Generally this option will cause duplication when dumping,
; but it is required in some games, such as Final Fantasy VIII.
;ConvertCopiesToWrites = false

; Determines the maximum number of times a VRAM write/upload can be split, before
; it is discarded and no longer tracked. This is required for games that partially
; overwrite texture data, such as Gran Turismo.
;MaxVRAMWriteSplits = 0

; Determines the minimum size of a texture that will be dumped. Textures with a
; width smaller than this value will be ignored.
;DumpTextureWidthThreshold = 8

; Determines the minimum size of a texture that will be dumped. Textures with a
; height smaller than this value will be ignored.
;DumpTextureHeightThreshold = 8

; Determines the minimum size of a VRAM write that will be dumped, in background
; dumping mode. Uploads smaller than this size will be ignored.
;DumpVRAMWriteWidthThreshold = 256

; Determines the minimum size of a VRAM write that will be dumped, in background
; dumping mode. Uploads smaller than this size will be ignored.
;DumpVRAMWriteHeightThreshold = 128

; Enables the use of a bilinear filter when scaling replacement textures.
; If more than one replacement texture in a 256x256 texture page has a different
; scaling over the native resolution, or the texture page is not covered, a
; bilinear filter will be used to resize/stretch the replacement texture, and/or
; the original native data.
;ReplacementScaleLinearFilter = false

[Aliases]
; Use this section to define replacement aliases. One line per replacement
; texture, with the key set to the source ID, and the value set to the ID used
; in place of the source. For example, without the newline:
;  texupload-P4-AAAAAAAAAAAAAAAA-BBBBBBBBBBBBBBBB-64x256-0-192-64x64-P0-14 =
;    texupload-P4-BBBBBBBBBBBBBBBB-BBBBBBBBBBBBBBBB-64x256-0-64-64x64-P0-13
)";

TinyString TextureReplacements::VRAMReplacementName::ToString() const
{
  return TinyString::from_format("{:08X}{:08X}", high, low);
}

bool TextureReplacements::VRAMReplacementName::Parse(const std::string_view file_title)
{
  if (file_title.length() != 43)
    return false;

  const std::optional<u64> high_value = StringUtil::FromChars<u64>(file_title.substr(11, 11 + 16), 16);
  const std::optional<u64> low_value = StringUtil::FromChars<u64>(file_title.substr(11 + 16), 16);
  if (!high_value.has_value() || !low_value.has_value())
    return false;

  low = low_value.value();
  high = high_value.value();
  return true;
}

size_t TextureReplacements::VRAMReplacementNameHash::operator()(const VRAMReplacementName& name) const
{
  size_t seed = std::hash<u64>{}(name.low);
  hash_combine(seed, name.high);
  return seed;
}

static constexpr const char* s_texture_replacement_mode_names[] = {"P4",   "P8",   "C16",   "C16",
                                                                   "STP4", "STP8", "STC16", "STC16"};

TinyString TextureReplacements::TextureReplacementName::ToString() const
{
  const char* type_str = (type == ReplacementType::TextureFromVRAMWrite) ? "texupload" : "texpage";
  const char* mode_str = s_texture_replacement_mode_names[texture_mode];
  if (GetTextureMode() < GPUTextureMode::Direct16Bit)
  {
    return TinyString::from_format("{}-{}-{:016X}-{:016X}-{}x{}-{}-{}-{}x{}-P{}-{}", type_str, mode_str, src_hash,
                                   pal_hash, src_width, src_height, offset_x, offset_y, width, height, pal_min,
                                   pal_max);
  }
  else
  {
    return TinyString::from_format("{}-{}-{:016X}-{}x{}-{}-{}-{}x{}", type_str, mode_str, src_hash, src_width,
                                   src_height, offset_x, offset_y, width, height);
  }
}

bool TextureReplacements::TextureReplacementName::Parse(const std::string_view file_title)
{
  // TODO: Swap to https://github.com/eliaskosunen/scnlib

  std::string_view::size_type start_pos = 0;
  std::string_view::size_type end_pos = file_title.find("-", start_pos);
  if (end_pos == std::string_view::npos)
    return false;

  // type
  std::string_view token = file_title.substr(start_pos, end_pos);
  if (token == "texupload")
    type = ReplacementType::TextureFromVRAMWrite;
  else if (token == "texpage")
    type = ReplacementType::TextureFromPage;
  else
    return false;
  start_pos = end_pos + 1;
  end_pos = file_title.find("-", start_pos + 1);
  if (end_pos == std::string_view::npos)
    return false;

  // mode
  token = file_title.substr(start_pos, end_pos - start_pos);
  std::optional<u8> mode_opt;
  for (size_t i = 0; i < std::size(s_texture_replacement_mode_names); i++)
  {
    if (token == s_texture_replacement_mode_names[i])
    {
      mode_opt = static_cast<u8>(i);
      break;
    }
  }
  if (!mode_opt.has_value())
    return false;
  texture_mode = mode_opt.value();
  start_pos = end_pos + 1;
  end_pos = file_title.find("-", start_pos + 1);
  if (end_pos == std::string_view::npos)
    return false;

  // src_hash
  token = file_title.substr(start_pos, end_pos - start_pos);
  std::optional<u64> val64;
  if (token.size() != 16 || !(val64 = StringUtil::FromChars<u64>(token, 16)).has_value())
    return false;
  src_hash = val64.value();
  start_pos = end_pos + 1;
  end_pos = file_title.find("-", start_pos + 1);
  if (end_pos == std::string_view::npos)
    return false;

  if (GetTextureMode() < GPUTextureMode::Direct16Bit)
  {
    // pal_hash
    token = file_title.substr(start_pos, end_pos - start_pos);
    if (token.size() != 16 || !(val64 = StringUtil::FromChars<u64>(token, 16)).has_value())
      return false;
    pal_hash = val64.value();
    start_pos = end_pos + 1;
    end_pos = file_title.find("x", start_pos + 1);
    if (end_pos == std::string_view::npos)
      return false;

    // src_width
    token = file_title.substr(start_pos, end_pos - start_pos);
    std::optional<u16> val16;
    if (!(val16 = StringUtil::FromChars<u16>(token)).has_value())
      return false;
    src_width = val16.value();
    if (src_width == 0)
      return false;
    start_pos = end_pos + 1;
    end_pos = file_title.find("-", start_pos + 1);
    if (end_pos == std::string_view::npos)
      return false;

    // src_height
    token = file_title.substr(start_pos, end_pos - start_pos);
    if (!(val16 = StringUtil::FromChars<u16>(token)).has_value())
      return false;
    src_height = val16.value();
    if (src_height == 0)
      return false;
    start_pos = end_pos + 1;
    end_pos = file_title.find("-", start_pos + 1);
    if (end_pos == std::string_view::npos)
      return false;

    // offset_x
    token = file_title.substr(start_pos, end_pos - start_pos);
    if (!(val16 = StringUtil::FromChars<u16>(token)).has_value())
      return false;
    offset_x = val16.value();
    start_pos = end_pos + 1;
    end_pos = file_title.find("-", start_pos + 1);
    if (end_pos == std::string_view::npos)
      return false;

    // offset_y
    token = file_title.substr(start_pos, end_pos - start_pos);
    if (!(val16 = StringUtil::FromChars<u16>(token)).has_value())
      return false;
    offset_y = val16.value();
    start_pos = end_pos + 1;
    end_pos = file_title.find("x", start_pos + 1);
    if (end_pos == std::string_view::npos)
      return false;

    // width
    token = file_title.substr(start_pos, end_pos - start_pos);
    if (!(val16 = StringUtil::FromChars<u16>(token)).has_value())
      return false;
    width = val16.value();
    if (width == 0)
      return false;
    start_pos = end_pos + 1;
    end_pos = file_title.find("-", start_pos + 1);
    if (end_pos == std::string_view::npos)
      return false;

    // height
    token = file_title.substr(start_pos, end_pos - start_pos);
    if (!(val16 = StringUtil::FromChars<u16>(token)).has_value())
      return false;
    height = val16.value();
    if (height == 0)
      return false;
    start_pos = end_pos + 1;
    end_pos = file_title.find("-", start_pos + 1);
    if (end_pos == std::string_view::npos || file_title[start_pos] != 'P')
      return false;

    // pal_min
    token = file_title.substr(start_pos + 1, end_pos - start_pos - 1);
    std::optional<u8> val8;
    if (!(val8 = StringUtil::FromChars<u8>(token)).has_value())
      return false;
    pal_min = val8.value();
    start_pos = end_pos + 1;

    // pal_max
    token = file_title.substr(start_pos);
    if (!(val8 = StringUtil::FromChars<u8>(token)).has_value())
      return false;
    pal_max = val8.value();
    if (pal_min > pal_max)
      return false;
  }
  else
  {
    // src_width
    token = file_title.substr(start_pos, end_pos - start_pos);
    std::optional<u16> val16;
    if (!(val16 = StringUtil::FromChars<u16>(token)).has_value())
      return false;
    src_width = val16.value();
    if (src_width == 0)
      return false;
    start_pos = end_pos + 1;
    end_pos = file_title.find("-", start_pos + 1);
    if (end_pos == std::string_view::npos)
      return false;

    // src_height
    token = file_title.substr(start_pos, end_pos - start_pos);
    if (!(val16 = StringUtil::FromChars<u16>(token)).has_value())
      return false;
    src_height = val16.value();
    if (src_height == 0)
      return false;
    start_pos = end_pos + 1;
    end_pos = file_title.find("-", start_pos + 1);
    if (end_pos == std::string_view::npos)
      return false;

    // offset_x
    token = file_title.substr(start_pos, end_pos - start_pos);
    if (!(val16 = StringUtil::FromChars<u16>(token)).has_value())
      return false;
    offset_x = val16.value();
    start_pos = end_pos + 1;
    end_pos = file_title.find("-", start_pos + 1);
    if (end_pos == std::string_view::npos)
      return false;

    // offset_y
    token = file_title.substr(start_pos, end_pos - start_pos);
    if (!(val16 = StringUtil::FromChars<u16>(token)).has_value())
      return false;
    offset_y = val16.value();
    start_pos = end_pos + 1;
    end_pos = file_title.find("x", start_pos + 1);
    if (end_pos == std::string_view::npos)
      return false;

    // width
    token = file_title.substr(start_pos, end_pos - start_pos);
    if (!(val16 = StringUtil::FromChars<u16>(token)).has_value())
      return false;
    width = val16.value();
    if (width == 0)
      return false;
    start_pos = end_pos + 1;

    // height
    token = file_title.substr(start_pos);
    if (!(val16 = StringUtil::FromChars<u16>(token)).has_value())
      return false;
    height = val16.value();
    if (height == 0)
      return false;
  }

  return true;
}

TextureReplacements::TextureReplacementIndex TextureReplacements::TextureReplacementName::GetIndex() const
{
  return {src_hash, GetTextureMode()};
}

GPUTextureMode TextureReplacements::TextureReplacementName::GetTextureMode() const
{
  return static_cast<GPUTextureMode>(texture_mode & 3u);
}

bool TextureReplacements::TextureReplacementName::IsSemitransparent() const
{
  return (texture_mode >= 4);
}

size_t TextureReplacements::TextureReplacementIndexHash::operator()(const TextureReplacementIndex& name) const
{
  // TODO: This sucks ass, do better.
  size_t seed = std::hash<u64>{}(name.src_hash);
  hash_combine(seed, static_cast<u8>(name.mode));
  return seed;
}

size_t TextureReplacements::DumpedTextureKeyHash::operator()(const DumpedTextureKey& k) const
{
  // TODO: This is slow
  std::size_t hash = 0;
  hash_combine(hash, k.tex_hash, k.pal_hash, k.width, k.height, k.texture_mode);
  return hash;
}

const Settings::TextureReplacementSettings::Configuration& TextureReplacements::GetConfig()
{
  return s_config;
}

void TextureReplacements::SetGameID(std::string game_id)
{
  if (s_game_id == game_id)
    return;

  s_game_id = game_id;
  Reload();
}

void TextureReplacements::Shutdown()
{
  s_texture_cache.clear();
  s_vram_replacements.clear();
  s_vram_write_texture_replacements.clear();
  s_texture_page_texture_replacements.clear();
  s_dumped_textures.clear();
  s_game_id = {};
}

const TextureReplacements::ReplacementImage* TextureReplacements::GetVRAMReplacement(u32 width, u32 height,
                                                                                     const void* pixels)
{
  const VRAMReplacementName hash = GetVRAMWriteHash(width, height, pixels);

  const auto it = s_vram_replacements.find(hash);
  if (it == s_vram_replacements.end())
    return nullptr;

  return LoadTexture(it->second);
}

bool TextureReplacements::ShouldDumpVRAMWrite(u32 width, u32 height)
{
  return g_settings.texture_replacements.dump_vram_writes && width < s_config.vram_write_dump_width_threshold &&
         height < s_config.vram_write_dump_height_threshold;
}

void TextureReplacements::DumpVRAMWrite(u32 width, u32 height, const void* pixels)
{
  const std::string filename = GetVRAMWriteDumpFilename(width, height, pixels);
  if (filename.empty())
    return;

  RGBA8Image image;
  image.SetSize(width, height);

  const u16* src_pixels = reinterpret_cast<const u16*>(pixels);

  for (u32 y = 0; y < height; y++)
  {
    for (u32 x = 0; x < width; x++)
    {
      image.SetPixel(x, y, VRAMRGBA5551ToRGBA8888(*src_pixels));
      src_pixels++;
    }
  }

  if (s_config.dump_vram_write_force_alpha_channel)
  {
    for (u32 y = 0; y < height; y++)
    {
      for (u32 x = 0; x < width; x++)
        image.SetPixel(x, y, image.GetPixel(x, y) | 0xFF000000u);
    }
  }

  INFO_LOG("Dumping {}x{} VRAM write to '{}'", width, height, Path::GetFileName(filename));
  if (!image.SaveToFile(filename.c_str())) [[unlikely]]
    ERROR_LOG("Failed to dump {}x{} VRAM write to '{}'", width, height, filename);
}

void TextureReplacements::DumpTexture(ReplacementType type, const GSVector4i src_rect, GPUTextureMode mode,
                                      TextureSourceHash src_hash, TexturePaletteHash pal_hash, u32 pal_min, u32 pal_max,
                                      const u16* palette_data, const GSVector4i rect,
                                      GPUTextureCache::PaletteRecordFlags flags)
{
  const u32 offset_x = ApplyTextureModeShift(mode, rect.left - src_rect.left);
  const u32 offset_y = rect.top - src_rect.top;
  const u32 width = ApplyTextureModeShift(mode, rect.width());
  const u32 height = rect.height();

  if (width < s_config.texture_dump_width_threshold || height < s_config.texture_dump_height_threshold)
    return;

  const bool semitransparent = ((flags & GPUTextureCache::PaletteRecordFlags::HasSemiTransparentDraws) !=
                                  GPUTextureCache::PaletteRecordFlags::None &&
                                !s_config.dump_texture_force_alpha_channel);
  const u8 dumped_texture_mode = static_cast<u8>(mode) | (semitransparent ? 4 : 0);

  const DumpedTextureKey key = {src_hash,
                                pal_hash,
                                Truncate16(offset_x),
                                Truncate16(offset_y),
                                Truncate16(width),
                                Truncate16(height),
                                type,
                                dumped_texture_mode,
                                {}};
  if (s_dumped_textures.find(key) != s_dumped_textures.end())
    return;

  if (!EnsureGameDirectoryExists())
    return;

  const std::string dump_directory = GetDumpDirectory();
  if (!FileSystem::EnsureDirectoryExists(dump_directory.c_str(), false))
    return;

  s_dumped_textures.insert(key);

  const TextureReplacementName name = {
    .src_hash = src_hash,
    .pal_hash = pal_hash,
    .src_width = Truncate16(static_cast<u32>(src_rect.width())),
    .src_height = Truncate16(static_cast<u32>(src_rect.height())),
    .type = type,
    .texture_mode = dumped_texture_mode,
    .offset_x = Truncate16(offset_x),
    .offset_y = Truncate16(offset_y),
    .width = Truncate16(width),
    .height = Truncate16(height),
    .pal_min = Truncate8(pal_min),
    .pal_max = Truncate8(pal_max),
  };

  SmallString filename = name.ToString();
  filename.append(".png");

  const std::string path = Path::Combine(dump_directory, filename);
  if (FileSystem::FileExists(path.c_str()))
    return;

  DEV_LOG("Dumping VRAM write {:016X} [{}x{}] at {}", src_hash, width, height, rect);

  RGBA8Image image(width, height);
  GPUTextureCache::DecodeTexture(mode, &g_vram[rect.top * VRAM_WIDTH + rect.left], palette_data, image.GetPixels(),
                                 image.GetPitch(), width, height);

  u32* image_pixels = image.GetPixels();
  const u32* image_pixels_end = image.GetPixels() + (width * height);
  if (s_config.dump_texture_force_alpha_channel)
  {
    for (u32* pixel = image_pixels; pixel != image_pixels_end; pixel++)
      *pixel |= 0xFF000000u;
  }
  else
  {
    if (semitransparent)
    {
      // Alpha channel should be inverted, because 0 means opaque, 1 is semitransparent.
      // Pixel value of 0000 is still completely transparent.
      for (u32* pixel = image_pixels; pixel != image_pixels_end; pixel++)
      {
        const u32 val = *pixel;
        *pixel = (val == 0u) ? 0u : ((val & 0xFFFFFFFu) | ((val & 0x80000000u) ? 0x80000000u : 0xFF000000u));
      }
    }
    else
    {
      // Only cut out 0000 pixels.
      for (u32* pixel = image_pixels; pixel != image_pixels_end; pixel++)
      {
        const u32 val = *pixel;
        *pixel = (val == 0u) ? 0u : (val | 0xFF000000u);
      }
    }
  }

  if (!image.SaveToFile(path.c_str()))
    ERROR_LOG("Failed to write texture dump to {}.", Path::GetFileName(path));
}

bool TextureReplacements::IsMatchingReplacementPalette(TexturePaletteHash full_palette_hash, GPUTextureMode mode,
                                                       GPUTexturePaletteReg palette, const TextureReplacementName& name)
{
  if (!TextureModeHasPalette(mode))
    return true;

  const u32 full_pal_max = GetPaletteWidth(mode) - 1;
  if (name.pal_min == 0 && name.pal_max == full_pal_max)
    return (name.pal_hash == full_palette_hash);

  // If the range goes off the edge of VRAM, it's not a match.
  if ((palette.GetXBase() + name.pal_max) >= VRAM_WIDTH)
    return false;

  // This needs to re-hash every lookup, which is a bit of a bummer.
  // But at least there's the hash cache, so it shouldn't be too painful...
  const TexturePaletteHash partial_hash =
    GPUTextureCache::HashPartialPalette(palette, mode, name.pal_min, name.pal_max);
  return (partial_hash == name.pal_hash);
}

void TextureReplacements::GetTextureReplacements(TextureReplacementMap& map,
                                                 std::vector<ReplacementSubImage>& replacements,
                                                 TextureSourceHash vram_write_hash, TextureSourceHash palette_hash,
                                                 GPUTextureMode mode, GPUTexturePaletteReg palette,
                                                 const GSVector2i& offset_to_page)
{
  const TextureReplacementIndex index = {vram_write_hash, mode};
  const auto& [begin, end] = map.equal_range(index);
  if (begin == end)
    return;

  const GSVector4i offset_to_page_v = GSVector4i(offset_to_page).xyxy();

  for (auto it = begin; it != end; ++it)
  {
    if (!IsMatchingReplacementPalette(palette_hash, mode, palette, it->second.first))
      continue;

    const ReplacementImage* image = LoadTexture(it->second.second);
    if (!image)
      continue;

    // TODO: GSVector4 (or 2, really)
    const TextureReplacementName& name = it->second.first;
    const float scale_x = static_cast<float>(image->GetWidth()) / static_cast<float>(name.width);
    const float scale_y = static_cast<float>(image->GetHeight()) / static_cast<float>(name.height);

    const GSVector4i rect_in_write_space = GSVector4i(static_cast<u32>(name.offset_x), static_cast<u32>(name.offset_y),
                                                      static_cast<u32>(name.offset_x) + static_cast<u32>(name.width),
                                                      static_cast<u32>(name.offset_y) + static_cast<u32>(name.height));
    const GSVector4i rect_in_page_space = rect_in_write_space.sub32(offset_to_page_v);

    // zw <= 0 or zw >= TEXTURE_PAGE_SIZE
    if (!(rect_in_page_space.le32(
            GSVector4i::cxpr(std::numeric_limits<s32>::min(), std::numeric_limits<s32>::min(), 0, 0)) |
          rect_in_page_space.ge32(GSVector4i::cxpr(TEXTURE_PAGE_WIDTH, TEXTURE_PAGE_HEIGHT,
                                                   std::numeric_limits<s32>::max(), std::numeric_limits<s32>::max())))
           .allfalse())
    {
      // Rect is out of bounds.
      continue;
    }

    // TODO: This fails in Wild Arms 2, writes that are wider than a page.
    DebugAssert(rect_in_page_space.width() == name.width && rect_in_page_space.height() == name.height);
    DebugAssert(rect_in_page_space.width() <= static_cast<s32>(TEXTURE_PAGE_WIDTH));
    DebugAssert(rect_in_page_space.height() <= static_cast<s32>(TEXTURE_PAGE_HEIGHT));

    replacements.push_back(
      ReplacementSubImage{rect_in_page_space, GSVector4i::zero(), *image, scale_x, scale_y, name.IsSemitransparent()});
  }
}

bool TextureReplacements::HasVRAMWriteTextureReplacements()
{
  return !s_vram_write_texture_replacements.empty();
}

void TextureReplacements::GetVRAMWriteTextureReplacements(std::vector<ReplacementSubImage>& replacements,
                                                          TextureSourceHash vram_write_hash,
                                                          TextureSourceHash palette_hash, GPUTextureMode mode,
                                                          GPUTexturePaletteReg palette,
                                                          const GSVector2i& offset_to_page)
{
  return GetTextureReplacements(s_vram_write_texture_replacements, replacements, vram_write_hash, palette_hash, mode,
                                palette, offset_to_page);
}

bool TextureReplacements::HasTexturePageTextureReplacements()
{
  return !s_texture_page_texture_replacements.empty();
}

void TextureReplacements::GetTexturePageTextureReplacements(std::vector<ReplacementSubImage>& replacements,
                                                            TextureSourceHash vram_write_hash,
                                                            TextureSourceHash palette_hash, GPUTextureMode mode,
                                                            GPUTexturePaletteReg palette)
{
  return GetTextureReplacements(s_texture_page_texture_replacements, replacements, vram_write_hash, palette_hash, mode,
                                palette, GSVector2i(0, 0));
}

std::optional<TextureReplacements::ReplacementType>
TextureReplacements::GetReplacementTypeFromFileTitle(const std::string_view path)
{
  if (path.starts_with("vram-write-"))
    return ReplacementType::VRAMReplacement;

  if (path.starts_with("texupload-"))
    return ReplacementType::TextureFromVRAMWrite;

  if (path.starts_with("texpage-"))
    return ReplacementType::TextureFromPage;

  return std::nullopt;
}

bool TextureReplacements::HasValidReplacementExtension(const std::string_view path)
{
  const std::string_view extension = Path::GetExtension(path);
  for (const char* test_extension : {"png", "jpg", "webp"})
  {
    if (StringUtil::EqualNoCase(extension, test_extension))
      return true;
  }

  return false;
}

void TextureReplacements::FindTextures(const std::string& dir)
{
  s_vram_replacements.clear();
  s_vram_write_texture_replacements.clear();
  s_texture_page_texture_replacements.clear();

  FileSystem::FindResultsArray files;
  FileSystem::FindFiles(dir.c_str(), "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_RECURSIVE, &files);

  for (FILESYSTEM_FIND_DATA& fd : files)
  {
    if ((fd.Attributes & FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY) || !HasValidReplacementExtension(fd.FileName))
      continue;

    const std::string_view file_title = Path::GetFileTitle(fd.FileName);
    const std::optional<ReplacementType> type = GetReplacementTypeFromFileTitle(file_title);
    if (!type.has_value())
      continue;

    switch (type.value())
    {
      case ReplacementType::VRAMReplacement:
      {
        VRAMReplacementName name;
        if (!g_settings.texture_replacements.enable_vram_write_replacements || !name.Parse(file_title))
          continue;

        if (const auto it = s_vram_replacements.find(name); it != s_vram_replacements.end())
        {
          WARNING_LOG("Duplicate VRAM replacement: '{}' and '{}'", Path::GetFileName(it->second),
                      Path::GetFileName(fd.FileName));
          continue;
        }

        s_vram_replacements.emplace(name, std::move(fd.FileName));
      }
      break;

      case ReplacementType::TextureFromVRAMWrite:
      case ReplacementType::TextureFromPage:
      {
        TextureReplacementName name;
        if (!g_settings.texture_replacements.enable_texture_replacements || !name.Parse(file_title))
          continue;

        DebugAssert(name.type == type.value());

        const TextureReplacementIndex index = name.GetIndex();
        TextureReplacementMap& dest_map = (type.value() == ReplacementType::TextureFromVRAMWrite) ?
                                            s_vram_write_texture_replacements :
                                            s_texture_page_texture_replacements;

        // Multiple replacements in the same write are fine. But they should have different rects.
        const auto range = dest_map.equal_range(index);
        bool duplicate = false;
        for (auto it = range.first; it != range.second; ++it)
        {
          if (it->second.first == name) [[unlikely]]
          {
            WARNING_LOG("Duplicate texture replacement: '{}' and '{}'", Path::GetFileName(it->second.second),
                        Path::GetFileName(fd.FileName));
            duplicate = true;
          }
        }
        if (duplicate) [[unlikely]]
          continue;

        dest_map.emplace(index, std::make_pair(name, std::move(fd.FileName)));
      }
      break;

        DefaultCaseIsUnreachable()
    }
  }

  if (g_settings.texture_replacements.enable_texture_replacements)
  {
    INFO_LOG("Found {} replacement upload textures for '{}'", s_vram_write_texture_replacements.size(), s_game_id);
    INFO_LOG("Found {} replacement page textures for '{}'", s_texture_page_texture_replacements.size(), s_game_id);
  }

  if (g_settings.texture_replacements.enable_vram_write_replacements)
    INFO_LOG("Found {} replacement VRAM for '{}'", s_vram_replacements.size(), s_game_id);
}

const TextureReplacements::ReplacementImage* TextureReplacements::LoadTexture(const std::string& filename)
{
  auto it = s_texture_cache.find(filename);
  if (it != s_texture_cache.end())
    return &it->second;

  RGBA8Image image;
  if (!image.LoadFromFile(filename.c_str()))
  {
    ERROR_LOG("Failed to load '{}'", Path::GetFileName(filename));
    return nullptr;
  }

  INFO_LOG("Loaded '{}': {}x{}", Path::GetFileName(filename), image.GetWidth(), image.GetHeight());
  it = s_texture_cache.emplace(filename, std::move(image)).first;
  return &it->second;
}

void TextureReplacements::PreloadTextures()
{
  static constexpr float UPDATE_INTERVAL = 1.0f;

  Common::Timer last_update_time;
  u32 num_textures_loaded = 0;
  const size_t total_textures =
    s_vram_replacements.size() + s_vram_write_texture_replacements.size() + s_texture_page_texture_replacements.size();

#define UPDATE_PROGRESS()                                                                                              \
  if (last_update_time.GetTimeSeconds() >= UPDATE_INTERVAL)                                                            \
  {                                                                                                                    \
    Host::DisplayLoadingScreen("Preloading replacement textures...", 0, static_cast<int>(total_textures),              \
                               static_cast<int>(num_textures_loaded));                                                 \
    last_update_time.Reset();                                                                                          \
  }

  for (const auto& it : s_vram_replacements)
  {
    UPDATE_PROGRESS();
    LoadTexture(it.second);
    num_textures_loaded++;
  }

#define PROCESS_MAP(map)                                                                                               \
  for (const auto& it : map)                                                                                           \
  {                                                                                                                    \
    UPDATE_PROGRESS();                                                                                                 \
    LoadTexture(it.second.second);                                                                                     \
    num_textures_loaded++;                                                                                             \
  }

  PROCESS_MAP(s_vram_write_texture_replacements);
  PROCESS_MAP(s_texture_page_texture_replacements);
#undef PROCESS_MAP
#undef UPDATE_PROGRESS
}

bool TextureReplacements::EnsureGameDirectoryExists()
{
  if (s_game_id.empty())
    return false;

  const std::string game_directory = Path::Combine(EmuFolders::Textures, s_game_id);
  if (FileSystem::DirectoryExists(game_directory.c_str()))
    return true;

  Error error;
  if (!FileSystem::CreateDirectory(game_directory.c_str(), false, &error))
  {
    ERROR_LOG("Failed to create game directory: {}", error.GetDescription());
    return false;
  }

  if (const std::string config_path = Path::Combine(game_directory, "config.ini");
      !FileSystem::FileExists(config_path.c_str()) &&
      !FileSystem::WriteStringToFile(config_path.c_str(), CONFIGURATION_TEMPLATE, &error))
  {
    ERROR_LOG("Failed to write configuration template: {}", error.GetDescription());
    return false;
  }

  if (!FileSystem::CreateDirectory(Path::Combine(game_directory, "dumps").c_str(), false, &error))
  {
    ERROR_LOG("Failed to create dumps directory: {}", error.GetDescription());
    return false;
  }

  if (!FileSystem::CreateDirectory(Path::Combine(game_directory, "replacements").c_str(), false, &error))
  {
    ERROR_LOG("Failed to create replacements directory: {}", error.GetDescription());
    return false;
  }

  return true;
}

std::string TextureReplacements::GetSourceDirectory()
{
  return Path::Combine(EmuFolders::Textures,
                       SmallString::from_format("{}" FS_OSPATH_SEPARATOR_STR "replacements", s_game_id));
}

std::string TextureReplacements::GetDumpDirectory()
{
  return Path::Combine(EmuFolders::Textures, SmallString::from_format("{}" FS_OSPATH_SEPARATOR_STR "dumps", s_game_id));
}

TextureReplacements::VRAMReplacementName TextureReplacements::GetVRAMWriteHash(u32 width, u32 height,
                                                                               const void* pixels)
{
  const XXH128_hash_t hash = XXH3_128bits(pixels, width * height * sizeof(u16));
  return {hash.low64, hash.high64};
}

std::string TextureReplacements::GetVRAMWriteDumpFilename(u32 width, u32 height, const void* pixels)
{
  std::string ret;
  if (!EnsureGameDirectoryExists())
    return ret;

  const std::string dump_directory = GetDumpDirectory();
  if (!FileSystem::EnsureDirectoryExists(dump_directory.c_str(), false))
    return ret;

  const VRAMReplacementName hash = GetVRAMWriteHash(width, height, pixels);
  ret = Path::Combine(dump_directory, fmt::format("vram-write-{}.png", hash.ToString()));
  if (FileSystem::FileExists(ret.c_str()))
    ret.clear();

  return ret;
}

bool TextureReplacements::LoadLocalConfiguration()
{
  const Settings::TextureReplacementSettings::Configuration old_config = s_config;

  // load settings from ini
  s_config = g_settings.texture_replacements.config;

  if (!s_game_id.empty())
  {
    if (std::string ini_path = Path::Combine(
          EmuFolders::Textures, SmallString::from_format("{}" FS_OSPATH_SEPARATOR_STR "config.ini", s_game_id));
        FileSystem::FileExists(ini_path.c_str()))
    {
      INISettingsInterface si(std::move(ini_path));
      if (si.Load())
      {
        s_config.Load(si, "Configuration", g_settings.texture_replacements.config);

        // TODO: Load aliases
      }
    }
  }

  // Any change?
  return (s_config != old_config);
}

void TextureReplacements::UpdateConfiguration()
{
  // Reload textures if configuration changes.
  if (LoadLocalConfiguration())
    Reload();
}

void TextureReplacements::Reload()
{
  s_vram_replacements.clear();

  LoadLocalConfiguration();

  // TODO: Don't load texture replacements if texture cache is not enabled.
  if (g_settings.texture_replacements.enable_texture_replacements ||
      g_settings.texture_replacements.enable_vram_write_replacements)
  {
    FindTextures(GetSourceDirectory());
  }

  if (g_settings.texture_replacements.preload_textures)
    PreloadTextures();

  PurgeUnreferencedTexturesFromCache();

  DebugAssert(g_gpu);
  GPUTextureCache::UpdateVRAMTrackingState();
}

void TextureReplacements::PurgeUnreferencedTexturesFromCache()
{
  TextureCache old_map = std::move(s_texture_cache);
  for (const auto& it : s_vram_replacements)
  {
    const auto it2 = old_map.find(it.second);
    if (it2 != old_map.end())
    {
      s_texture_cache[it.second] = std::move(it2->second);
      old_map.erase(it2);
    }
  }

  for (const auto& map : {s_vram_write_texture_replacements, s_texture_page_texture_replacements})
  {
    for (const auto& it : map)
    {
      const auto it2 = old_map.find(it.second.second);
      if (it2 != old_map.end())
      {
        s_texture_cache[it.second.second] = std::move(it2->second);
        old_map.erase(it2);
      }
    }
  }
}