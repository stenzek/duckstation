// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "gpu_types.h"

class Image;
class GPUTexture;
class StateWrapper;

struct Settings;
class GPU_HW;

//////////////////////////////////////////////////////////////////////////
// Texture Cache
//////////////////////////////////////////////////////////////////////////
namespace GPUTextureCache {

/// 4 pages in C16 mode, 2+4 pages in P8 mode, 1+1 pages in P4 mode.
static constexpr u32 MAX_PAGE_REFS_PER_SOURCE = 6;

static constexpr u32 MAX_PAGE_REFS_PER_WRITE = 32;

enum class PaletteRecordFlags : u32
{
  None = 0,
  HasSemiTransparentDraws = (1 << 0),
};
IMPLEMENT_ENUM_CLASS_BITWISE_OPERATORS(PaletteRecordFlags);

using HashType = u64;
using TextureReplacementImage = Image;

struct Source;
struct HashCacheEntry;

template<typename T>
struct TList;
template<typename T>
struct TListNode;

template<typename T>
struct TList
{
  TListNode<T>* head;
  TListNode<T>* tail;
};

template<typename T>
struct TListNode
{
  // why inside itself? because we have 3 lists
  T* ref;
  TList<T>* list;
  TListNode<T>* prev;
  TListNode<T>* next;
};

struct SourceKey
{
  u8 page;
  GPUTextureMode mode;
  GPUTexturePaletteReg palette;

  SourceKey() = default;
  ALWAYS_INLINE constexpr SourceKey(u8 page_, GPUTexturePaletteReg palette_, GPUTextureMode mode_)
    : page(page_), mode(mode_), palette(palette_)
  {
  }
  ALWAYS_INLINE constexpr SourceKey(const SourceKey& k) : page(k.page), mode(k.mode), palette(k.palette) {}

  ALWAYS_INLINE bool HasPalette() const { return (mode < GPUTextureMode::Direct16Bit); }

  ALWAYS_INLINE SourceKey& operator=(const SourceKey& k)
  {
    page = k.page;
    mode = k.mode;
    palette.bits = k.palette.bits;
    return *this;
  }

  ALWAYS_INLINE bool operator==(const SourceKey& k) const { return (std::memcmp(&k, this, sizeof(SourceKey)) == 0); }
  ALWAYS_INLINE bool operator!=(const SourceKey& k) const { return (std::memcmp(&k, this, sizeof(SourceKey)) != 0); }
};
static_assert(sizeof(SourceKey) == 4);

// TODO: Pool objects
struct Source
{
  SourceKey key;
  u32 num_page_refs;
  GPUTexture* texture;
  HashCacheEntry* from_hash_cache;
  GSVector4i texture_rect;
  GSVector4i palette_rect;
  HashType texture_hash;
  HashType palette_hash;
  GSVector4i active_uv_rect;
  PaletteRecordFlags palette_record_flags;

  std::array<TListNode<Source>, MAX_PAGE_REFS_PER_SOURCE> page_refs;
  TListNode<Source> hash_cache_ref;
};

bool Initialize(GPU_HW* backend);
void UpdateSettings(bool use_texture_cache, const Settings& old_settings);

bool GetStateSize(StateWrapper& sw, u32* size);
bool DoState(StateWrapper& sw, bool skip);

void Shutdown();

void Invalidate();

void AddWrittenRectangle(const GSVector4i rect, bool update_vram_writes = false, bool remove_from_hash_cache = false);
void AddDrawnRectangle(const GSVector4i rect, const GSVector4i clip_rect);

void CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height, bool set_mask, bool check_mask,
              const GSVector4i src_bounds, const GSVector4i dst_bounds);
void WriteVRAM(u32 x, u32 y, u32 width, u32 height, const void* data, bool set_mask, bool check_mask,
               const GSVector4i bounds);

const Source* LookupSource(SourceKey key, const GSVector4i uv_rect, PaletteRecordFlags flags);

bool IsRectDrawn(const GSVector4i rect);
bool AreSourcePagesDrawn(SourceKey key, const GSVector4i rect);

void Compact();

void SetGameID(std::string game_id);
void ReloadTextureReplacements(bool show_info);

// VRAM Write Replacements
GPUTexture* GetVRAMReplacement(u32 width, u32 height, const void* pixels);
void DumpVRAMWrite(u32 width, u32 height, const void* pixels);
bool ShouldDumpVRAMWrite(u32 width, u32 height);

} // namespace GPUTextureCache
