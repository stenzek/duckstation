// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "texture_replacements.h"
#include "gpu.h"
#include "gpu_hw_texture_cache.h"
#include "gpu_types.h"
#include "host.h"
#include "settings.h"

#include "common/bitutils.h"
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
  u64 pal_hash;
  GPUTextureMode mode;

  bool operator<(const TextureReplacementIndex& rhs) const
  {
    return std::tie(src_hash, pal_hash, mode) < std::tie(src_hash, pal_hash, mode);
  }
  bool operator==(const TextureReplacementIndex& rhs) const
  {
    return src_hash == rhs.src_hash && pal_hash == rhs.pal_hash && mode == rhs.mode;
  }
  bool operator!=(const TextureReplacementIndex& rhs) const
  {
    return src_hash != rhs.src_hash || pal_hash != rhs.pal_hash || mode != rhs.mode;
  }
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
  GPUTextureMode mode;
  u16 offset_x;
  u16 offset_y;
  u16 width;
  u16 height;

  TinyString ToString() const;
  bool Parse(const std::string_view file_title);
  TextureReplacementIndex Index() const;

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
  GPUTextureMode mode;
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

static std::string GetSourceDirectory();
static std::string GetDumpDirectory();

static VRAMReplacementName GetVRAMWriteHash(u32 width, u32 height, const void* pixels);
static std::string GetVRAMWriteDumpFilename(u32 width, u32 height, const void* pixels);

static void GetTextureReplacements(TextureReplacementMap& map, std::vector<ReplacementSubImage>& replacements,
                                   TextureSourceHash vram_write_hash, TextureSourceHash palette_hash,
                                   GPUTextureMode mode, const GSVector2i& offset_to_page);

static void FindTextures(const std::string& dir);

static const ReplacementImage* LoadTexture(const std::string& filename);
static void PreloadTextures();
static void PurgeUnreferencedTexturesFromCache();

static std::string s_game_id;

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

static constexpr const char* s_texture_replacement_mode_names[] = {"P4", "P8", "C16", "C16"};

TinyString TextureReplacements::TextureReplacementName::ToString() const
{
  const char* type_str = (type == ReplacementType::TextureFromVRAMWrite) ? "texupload" : "texpage";
  return TinyString::from_format("{}-{:016X}-{:016X}-{}x{}-{}-{}-{}-{}x{}", type_str, src_hash, pal_hash, src_width,
                                 src_height, s_texture_replacement_mode_names[static_cast<u8>(mode)], offset_x,
                                 offset_y, width, height);
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

  // mode
  token = file_title.substr(start_pos, end_pos - start_pos);
  std::optional<GPUTextureMode> mode_opt;
  for (size_t i = 0; i < std::size(s_texture_replacement_mode_names); i++)
  {
    if (token == s_texture_replacement_mode_names[i])
    {
      mode_opt = static_cast<GPUTextureMode>(i);
      break;
    }
  }
  if (!mode_opt.has_value())
    return false;
  mode = mode_opt.value();
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

  return true;
}

TextureReplacements::TextureReplacementIndex TextureReplacements::TextureReplacementName::Index() const
{
  return {src_hash, pal_hash, mode};
}

size_t TextureReplacements::TextureReplacementIndexHash::operator()(const TextureReplacementIndex& name) const
{
  // TODO: This sucks ass, do better.
  size_t seed = std::hash<u64>{}(name.src_hash);
  hash_combine(seed, name.pal_hash, static_cast<u8>(name.mode));
  return seed;
}

size_t TextureReplacements::DumpedTextureKeyHash::operator()(const DumpedTextureKey& k) const
{
  // TODO: This is slow
  std::size_t hash = 0;
  hash_combine(hash, k.tex_hash, k.pal_hash, k.width, k.height, static_cast<u8>(k.mode));
  return hash;
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

void TextureReplacements::SetGameID(std::string game_id)
{
  if (s_game_id == game_id)
    return;

  s_game_id = game_id;
  Reload();
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

  if (g_settings.texture_replacements.dump_vram_write_force_alpha_channel)
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

void TextureReplacements::DumpTexture(ReplacementType type, const GSVector4i src_rect, TextureSourceHash src_hash,
                                      TexturePaletteHash pal_hash, GPUTextureMode mode, GPUTexturePaletteReg palette,
                                      const GSVector4i rect)
{
  const u32 offset_x = ApplyTextureModeShift(mode, rect.left - src_rect.left);
  const u32 offset_y = rect.top - src_rect.top;
  const u32 width = ApplyTextureModeShift(mode, rect.width());
  const u32 height = rect.height();

  if (width < g_settings.texture_replacements.texture_dump_width_threshold ||
      height < g_settings.texture_replacements.texture_dump_height_threshold)
  {
    return;
  }

  const DumpedTextureKey key = {
    src_hash, pal_hash, Truncate16(offset_x), Truncate16(offset_y), Truncate16(width), Truncate16(height), type,
    mode,     {}};
  if (s_dumped_textures.find(key) != s_dumped_textures.end())
    return;

  s_dumped_textures.insert(key);

  // TODO: Size check
  // probably a clut
  if (height == 1)
    return;

  const TextureReplacementName name = {
    .src_hash = src_hash,
    .pal_hash = pal_hash,
    .src_width = Truncate16(static_cast<u32>(src_rect.width())),
    .src_height = Truncate16(static_cast<u32>(src_rect.height())),
    .type = type,
    .mode = mode,
    .offset_x = Truncate16(offset_x),
    .offset_y = Truncate16(offset_y),
    .width = Truncate16(width),
    .height = Truncate16(height),
  };

  SmallString filename = name.ToString();
  filename.append(".png");

  const std::string dump_directory = GetDumpDirectory();
  const std::string path = Path::Combine(dump_directory, filename);
  if (FileSystem::FileExists(path.c_str()))
    return;

  if (!FileSystem::EnsureDirectoryExists(dump_directory.c_str(), false))
    return;

  DEV_LOG("Dumping VRAM write {:016X} [{}x{}] at {}", src_hash, width, height, rect);

  RGBA8Image image(width, height);
  GPUTextureCache::DecodeTexture(&g_vram[rect.top * VRAM_WIDTH + rect.left], palette, mode, image.GetPixels(),
                                 image.GetPitch(), width, height);

  if (g_settings.texture_replacements.dump_texture_force_alpha_channel)
  {
    for (u32 y = 0; y < image.GetHeight(); y++)
    {
      for (u32 x = 0; x < image.GetWidth(); x++)
        image.SetPixel(x, y, image.GetPixel(x, y) | 0xFF000000u);
    }
  }

  if (!image.SaveToFile(path.c_str()))
    ERROR_LOG("Failed to write texture dump to {}.", Path::GetFileName(path));
}

void TextureReplacements::GetTextureReplacements(TextureReplacementMap& map,
                                                 std::vector<ReplacementSubImage>& replacements,
                                                 TextureSourceHash vram_write_hash, TextureSourceHash palette_hash,
                                                 GPUTextureMode mode, const GSVector2i& offset_to_page)
{
  const TextureReplacementIndex index = {vram_write_hash, palette_hash, mode};
  const auto& [begin, end] = map.equal_range(index);
  if (begin == end)
    return;

  const GSVector4i offset_to_page_v = GSVector4i(offset_to_page).xyxy();

  for (auto it = begin; it != end; ++it)
  {
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

    DebugAssert(rect_in_page_space.width() == name.width && rect_in_page_space.height() == name.height);
    DebugAssert(rect_in_page_space.width() <= static_cast<s32>(TEXTURE_PAGE_WIDTH));
    DebugAssert(rect_in_page_space.height() <= static_cast<s32>(TEXTURE_PAGE_HEIGHT));

    replacements.emplace_back(rect_in_page_space, GSVector4i::zero(), *image, scale_x, scale_y);
  }
}

bool TextureReplacements::HasVRAMWriteTextureReplacements()
{
  return !s_vram_write_texture_replacements.empty();
}

void TextureReplacements::GetVRAMWriteTextureReplacements(std::vector<ReplacementSubImage>& replacements,
                                                          TextureSourceHash vram_write_hash,
                                                          TextureSourceHash palette_hash, GPUTextureMode mode,
                                                          const GSVector2i& offset_to_page)
{
  return GetTextureReplacements(s_vram_write_texture_replacements, replacements, vram_write_hash, palette_hash, mode,
                                offset_to_page);
}

bool TextureReplacements::HasTexturePageTextureReplacements()
{
  return !s_texture_page_texture_replacements.empty();
}

void TextureReplacements::GetTexturePageTextureReplacements(std::vector<ReplacementSubImage>& replacements,
                                                            TextureSourceHash vram_write_hash,
                                                            TextureSourceHash palette_hash, GPUTextureMode mode)
{
  return GetTextureReplacements(s_texture_page_texture_replacements, replacements, vram_write_hash, palette_hash, mode,
                                GSVector2i(0, 0));
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

        const TextureReplacementIndex index = name.Index();
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

// TODO: Organize into PCSX2-style.
std::string TextureReplacements::GetSourceDirectory()
{
  return Path::Combine(EmuFolders::Textures, s_game_id);
}

std::string TextureReplacements::GetDumpDirectory()
{
  return Path::Combine(EmuFolders::Dumps, Path::Combine("textures", s_game_id));
}

TextureReplacements::VRAMReplacementName TextureReplacements::GetVRAMWriteHash(u32 width, u32 height,
                                                                               const void* pixels)
{
  XXH128_hash_t hash = XXH3_128bits(pixels, width * height * sizeof(u16));
  return {hash.low64, hash.high64};
}

std::string TextureReplacements::GetVRAMWriteDumpFilename(u32 width, u32 height, const void* pixels)
{
  if (s_game_id.empty())
    return {};

  const VRAMReplacementName hash = GetVRAMWriteHash(width, height, pixels);
  const std::string dump_directory(GetDumpDirectory());
  std::string filename(Path::Combine(dump_directory, fmt::format("vram-write-{}.png", hash.ToString())));

  if (FileSystem::FileExists(filename.c_str()))
    return {};

  if (!FileSystem::EnsureDirectoryExists(dump_directory.c_str(), false))
    return {};

  return filename;
}

void TextureReplacements::Reload()
{
  s_vram_replacements.clear();

  if (g_settings.texture_replacements.AnyReplacementsEnabled())
    FindTextures(GetSourceDirectory());

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