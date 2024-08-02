// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "gpu_types.h"
#include "texture_replacements.h"

#include <unordered_set>

class GPUTexture;
class StateWrapper;

struct Settings;

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
};

bool Initialize();
void UpdateSettings(const Settings& old_settings);
bool DoState(StateWrapper& sw, bool skip);
void Shutdown();

void Invalidate();

void AddWrittenRectangle(const GSVector4i rect);
void AddCopiedRectanglePart1(const GSVector4i rect); // TODO: Rename this shit
void AddCopiedRectanglePart2(const GSVector4i rect);
void AddDrawnRectangle(const GSVector4i rect);

void TrackVRAMWrite(const GSVector4i rect);
void UpdateVRAMTrackingState();

const Source* LookupSource(SourceKey key, const GSVector4i uv_rect, PaletteRecordFlags flags);

bool IsPageDrawn(u32 page_index);
bool IsPageDrawn(u32 page_index, const GSVector4i rect);
bool IsRectDrawn(const GSVector4i rect);
bool AreSourcePagesDrawn(SourceKey key, const GSVector4i rect);

void InvalidatePageSources(u32 pn);
void InvalidatePageSources(u32 pn, const GSVector4i rc);
void DestroySource(Source* src);

void AgeHashCache();

void DumpAvailableTextures(); // TODO: Hotkey

void DecodeTexture(GPUTextureMode mode, const u16* page_ptr, const u16* palette, u32* dest, u32 dest_stride, u32 width,
                   u32 height);
HashType HashPartialPalette(GPUTexturePaletteReg palette, GPUTextureMode mode, u32 min, u32 max);

} // namespace GPUTextureCache
