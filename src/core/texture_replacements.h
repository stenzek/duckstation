// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "util/image.h"

#include "common/hash_combine.h"

#include "types.h"

#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

struct TextureReplacementHash
{
  u64 low;
  u64 high;

  std::string ToString() const;
  bool ParseString(const std::string_view& sv);

  bool operator<(const TextureReplacementHash& rhs) const { return std::tie(low, high) < std::tie(rhs.low, rhs.high); }
  bool operator==(const TextureReplacementHash& rhs) const { return low == rhs.low && high == rhs.high; }
  bool operator!=(const TextureReplacementHash& rhs) const { return low != rhs.low || high != rhs.high; }
};

namespace std {
template<>
struct hash<TextureReplacementHash>
{
  size_t operator()(const TextureReplacementHash& h) const
  {
    size_t hash_hash = std::hash<u64>{}(h.low);
    hash_combine(hash_hash, h.high);
    return hash_hash;
  }
};
} // namespace std

using TextureReplacementTexture = RGBA8Image;

class TextureReplacements
{
public:
  enum class ReplacmentType
  {
    VRAMWrite
  };

  TextureReplacements();
  ~TextureReplacements();

  const std::string GetGameID() const { return m_game_id; }
  void SetGameID(std::string game_id);

  void Reload();

  const TextureReplacementTexture* GetVRAMWriteReplacement(u32 width, u32 height, const void* pixels);
  void DumpVRAMWrite(u32 width, u32 height, const void* pixels);

  void Shutdown();

private:
  struct ReplacementHashMapHash
  {
    size_t operator()(const TextureReplacementHash& hash);
  };

  using VRAMWriteReplacementMap = std::unordered_map<TextureReplacementHash, std::string>;
  using TextureCache = std::unordered_map<std::string, TextureReplacementTexture>;

  static bool ParseReplacementFilename(const std::string& filename, TextureReplacementHash* replacement_hash,
                                       ReplacmentType* replacement_type);

  std::string GetSourceDirectory() const;
  std::string GetDumpDirectory() const;

  TextureReplacementHash GetVRAMWriteHash(u32 width, u32 height, const void* pixels) const;
  std::string GetVRAMWriteDumpFilename(u32 width, u32 height, const void* pixels) const;

  void FindTextures(const std::string& dir);

  const TextureReplacementTexture* LoadTexture(const std::string& filename);
  void PreloadTextures();
  void PurgeUnreferencedTexturesFromCache();

  std::string m_game_id;

  TextureCache m_texture_cache;

  VRAMWriteReplacementMap m_vram_write_replacements;
};

extern TextureReplacements g_texture_replacements;