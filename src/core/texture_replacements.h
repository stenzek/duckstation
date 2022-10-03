#pragma once
#include "common/hash_combine.h"
#include "common/image.h"
#include "gpu_types.h"
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

using TextureReplacementTexture = Common::RGBA8Image;

class TextureReplacements
{
public:
  enum class ReplacmentType
  {
    VRAMWrite,
    Texture
  };

  enum : u32
  {
    TEXTURE_REPLACEMENT_PAGE_WIDTH = 256,       // in page space
    TEXTURE_REPLACEMENT_PAGE_HEIGHT = 256,      // in page space
    TEXTURE_REPLACEMENT_PAGE_VRAM_WIDTH = 64,   // in vram space
    TEXTURE_REPLACEMENT_PAGE_VRAM_HEIGHT = 256, // in vram space
    TEXTURE_REPLACEMENT_X_PAGES = (VRAM_WIDTH / TEXTURE_REPLACEMENT_PAGE_VRAM_WIDTH),
    TEXTURE_REPLACEMENT_Y_PAGES = (VRAM_HEIGHT / TEXTURE_REPLACEMENT_PAGE_HEIGHT),
    TEXTURE_REPLACEMENT_PAGE_COUNT = (TEXTURE_REPLACEMENT_X_PAGES * TEXTURE_REPLACEMENT_Y_PAGES),
  };

  TextureReplacements();
  ~TextureReplacements();

  static constexpr u32 GetTextureReplacementXPage(u32 vram_x) { return (vram_x * 4) / TEXTURE_REPLACEMENT_PAGE_WIDTH; }
  static constexpr u32 GetTextureReplacementXPageOffset(u32 vram_x)
  {
    return (vram_x * 4) % TEXTURE_REPLACEMENT_PAGE_WIDTH;
  }
  static constexpr u32 GetTextureReplacementYPage(u32 vram_y) { return vram_y / TEXTURE_REPLACEMENT_PAGE_HEIGHT; }
  static constexpr u32 GetTextureReplacementYPageOffset(u32 vram_y) { return vram_y % TEXTURE_REPLACEMENT_PAGE_HEIGHT; }
  static constexpr u32 GetTextureReplacementPageIndex(u32 page_x, u32 page_y)
  {
    return (page_y * TEXTURE_REPLACEMENT_X_PAGES) + page_x;
  }

  u32 GetScaledReplacementTextureWidth();
  u32 GetScaledReplacementTextureHeight();
  void ReuploadReplacementTextures();

  void OnSystemReset();

  void Reload();

  static TextureReplacementHash GetVRAMWriteHash(u32 width, u32 height, const void* pixels);

  const TextureReplacementTexture* GetVRAMWriteReplacement(u32 width, u32 height, const void* pixels);

  void UploadReplacementTextures(u32 vram_x, u32 vram_y, u32 width, u32 height, const void* pixels);

  void Shutdown();

private:
  struct ReplacementHashMapHash
  {
    size_t operator()(const TextureReplacementHash& hash);
  };

  struct ReplacementEntry
  {
    std::string filename;
    u16 offset_x, offset_y;
    u16 width, height;
    GPUTextureMode mode;
  };

  using VRAMWriteReplacementMap = std::unordered_map<TextureReplacementHash, std::string>;
  using TextureReplacementMap = std::unordered_multimap<TextureReplacementHash, ReplacementEntry>;
  using TextureCache = std::unordered_map<std::string, TextureReplacementTexture>;

  static bool ParseReplacementFilename(const std::string& filename, TextureReplacementHash* replacement_hash,
                                       ReplacmentType* replacement_type, GPUTextureMode* out_mode, u16* out_offset_x,
                                       u16* out_offset_y, u16* out_width, u16* out_height);

  std::string GetSourceDirectory() const;
  std::string GetDumpDirectory() const;

  void FindTextures(const std::string& dir);

  const TextureReplacementTexture* LoadTexture(const std::string& filename);
  void PreloadTextures();
  void PurgeUnreferencedTexturesFromCache();

  void TransformTextureCoordinates(GPUTextureMode mode, u32 vram_x, u32 vram_y, u32 vram_width, u32 vram_height,
                                   u32* out_vram_width, u32* out_vram_height, u32* page_index, u32* page_offset_x,
                                   u32* page_offset_y, u32* page_width, u32* page_height);

  void UntransformTextureCoordinates(GPUTextureMode mode, u32 texture_width, u32 texture_height, u32 vram_x, u32 vram_y,
                                     u32* out_vram_width, u32* out_vram_height, u32* page_index, u32* page_offset_x,
                                     u32* page_offset_y, u32* page_width, u32* page_height);

  void InvalidateReplacementTextures();
  void InvalidateReplacementTexture(GPUTextureMode mode, u32 vram_x, u32 vram_y, u32 width, u32 height);
  void UploadReplacementTexture(const TextureReplacementTexture* texture, const ReplacementEntry& entry, u32 vram_x,
                                u32 vram_y);

  TextureCache m_texture_cache;

  VRAMWriteReplacementMap m_vram_write_replacements;
  TextureReplacementMap m_texture_replacements;

  std::vector<u32> m_texture_replacement_invalidate_buffer;
};

extern TextureReplacements g_texture_replacements;