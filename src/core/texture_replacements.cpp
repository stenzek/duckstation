// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "texture_replacements.h"
#include "gpu_types.h"
#include "host.h"
#include "settings.h"

#include "common/bitutils.h"
#include "common/file_system.h"
#include "common/hash_combine.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"
#include "common/timer.h"

#include "fmt/format.h"
#include "xxhash.h"
#if defined(CPU_ARCH_X86) || defined(CPU_ARCH_X64)
#include "xxh_x86dispatch.h"
#endif

#include <cinttypes>
#include <tuple>
#include <unordered_map>
#include <vector>

Log_SetChannel(TextureReplacements);

namespace TextureReplacements {
namespace {
struct VRAMReplacementHash
{
  u64 low;
  u64 high;

  TinyString ToString() const;
  bool ParseString(std::string_view sv);

  bool operator<(const VRAMReplacementHash& rhs) const { return std::tie(low, high) < std::tie(rhs.low, rhs.high); }
  bool operator==(const VRAMReplacementHash& rhs) const { return low == rhs.low && high == rhs.high; }
  bool operator!=(const VRAMReplacementHash& rhs) const { return low != rhs.low || high != rhs.high; }
};

struct VRAMReplacementHashMapHash
{
  size_t operator()(const VRAMReplacementHash& hash) const;
};
} // namespace

using VRAMWriteReplacementMap = std::unordered_map<VRAMReplacementHash, std::string, VRAMReplacementHashMapHash>;
using TextureCache = std::unordered_map<std::string, ReplacementImage>;

static bool ParseReplacementFilename(const std::string& filename, VRAMReplacementHash* replacement_hash,
                                     ReplacmentType* replacement_type);

static std::string GetSourceDirectory();
static std::string GetDumpDirectory();

static VRAMReplacementHash GetVRAMWriteHash(u32 width, u32 height, const void* pixels);
static std::string GetVRAMWriteDumpFilename(u32 width, u32 height, const void* pixels);

static void FindTextures(const std::string& dir);

static const ReplacementImage* LoadTexture(const std::string& filename);
static void PreloadTextures();
static void PurgeUnreferencedTexturesFromCache();

static std::string s_game_id;

// TODO: Check the size, purge some when it gets too large.
static TextureCache s_texture_cache;

static VRAMWriteReplacementMap s_vram_write_replacements;
} // namespace TextureReplacements

size_t TextureReplacements::VRAMReplacementHashMapHash::operator()(const VRAMReplacementHash& hash) const
{
  size_t hash_hash = std::hash<u64>{}(hash.low);
  hash_combine(hash_hash, hash.high);
  return hash_hash;
}

TinyString TextureReplacements::VRAMReplacementHash::ToString() const
{
  return TinyString::from_format("{:08X}{:08X}", high, low);
}

bool TextureReplacements::VRAMReplacementHash::ParseString(std::string_view sv)
{
  if (sv.length() != 32)
    return false;

  std::optional<u64> high_value = StringUtil::FromChars<u64>(sv.substr(0, 16), 16);
  std::optional<u64> low_value = StringUtil::FromChars<u64>(sv.substr(16), 16);
  if (!high_value.has_value() || !low_value.has_value())
    return false;

  low = low_value.value();
  high = high_value.value();
  return true;
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
  const VRAMReplacementHash hash = GetVRAMWriteHash(width, height, pixels);

  const auto it = s_vram_write_replacements.find(hash);
  if (it == s_vram_write_replacements.end())
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

void TextureReplacements::Shutdown()
{
  s_texture_cache.clear();
  s_vram_write_replacements.clear();
  s_game_id.clear();
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

TextureReplacements::VRAMReplacementHash TextureReplacements::GetVRAMWriteHash(u32 width, u32 height,
                                                                               const void* pixels)
{
  XXH128_hash_t hash = XXH3_128bits(pixels, width * height * sizeof(u16));
  return {hash.low64, hash.high64};
}

std::string TextureReplacements::GetVRAMWriteDumpFilename(u32 width, u32 height, const void* pixels)
{
  if (s_game_id.empty())
    return {};

  const VRAMReplacementHash hash = GetVRAMWriteHash(width, height, pixels);
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
  s_vram_write_replacements.clear();

  if (g_settings.texture_replacements.AnyReplacementsEnabled())
    FindTextures(GetSourceDirectory());

  if (g_settings.texture_replacements.preload_textures)
    PreloadTextures();

  PurgeUnreferencedTexturesFromCache();
}

void TextureReplacements::PurgeUnreferencedTexturesFromCache()
{
  TextureCache old_map = std::move(s_texture_cache);
  s_texture_cache = {};

  for (const auto& it : s_vram_write_replacements)
  {
    auto it2 = old_map.find(it.second);
    if (it2 != old_map.end())
    {
      s_texture_cache[it.second] = std::move(it2->second);
      old_map.erase(it2);
    }
  }
}

bool TextureReplacements::ParseReplacementFilename(const std::string& filename, VRAMReplacementHash* replacement_hash,
                                                   ReplacmentType* replacement_type)
{
  const std::string_view file_title = Path::GetFileTitle(filename);
  if (!file_title.starts_with("vram-write-"))
    return false;

  const std::string_view hashpart = file_title.substr(11);
  if (!replacement_hash->ParseString(hashpart))
    return false;

  const std::string_view file_extension = Path::GetExtension(filename);
  bool valid_extension = false;
  for (const char* test_extension : {"png", "jpg", "webp"})
  {
    if (StringUtil::EqualNoCase(file_extension, test_extension))
    {
      valid_extension = true;
      break;
    }
  }

  *replacement_type = ReplacmentType::VRAMWrite;
  return valid_extension;
}

void TextureReplacements::FindTextures(const std::string& dir)
{
  FileSystem::FindResultsArray files;
  FileSystem::FindFiles(dir.c_str(), "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_RECURSIVE, &files);

  for (FILESYSTEM_FIND_DATA& fd : files)
  {
    if (fd.Attributes & FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY)
      continue;

    VRAMReplacementHash hash;
    ReplacmentType type;
    if (!ParseReplacementFilename(fd.FileName, &hash, &type))
      continue;

    switch (type)
    {
      case ReplacmentType::VRAMWrite:
      {
        auto it = s_vram_write_replacements.find(hash);
        if (it != s_vram_write_replacements.end())
        {
          WARNING_LOG("Duplicate VRAM write replacement: '{}' and '{}'", it->second, fd.FileName);
          continue;
        }

        s_vram_write_replacements.emplace(hash, std::move(fd.FileName));
      }
      break;
    }
  }

  INFO_LOG("Found {} replacement VRAM writes for '{}'", s_vram_write_replacements.size(), s_game_id);
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
  const u32 total_textures = static_cast<u32>(s_vram_write_replacements.size());

#define UPDATE_PROGRESS()                                                                                              \
  if (last_update_time.GetTimeSeconds() >= UPDATE_INTERVAL)                                                            \
  {                                                                                                                    \
    Host::DisplayLoadingScreen("Preloading replacement textures...", 0, static_cast<int>(total_textures),              \
                               static_cast<int>(num_textures_loaded));                                                 \
    last_update_time.Reset();                                                                                          \
  }

  for (const auto& it : s_vram_write_replacements)
  {
    UPDATE_PROGRESS();

    LoadTexture(it.second);
    num_textures_loaded++;
  }

#undef UPDATE_PROGRESS
}
