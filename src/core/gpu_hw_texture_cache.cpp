// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "gpu_hw_texture_cache.h"
#include "gpu_hw.h"
#include "settings.h"

#include "util/gpu_device.h"

#include "common/gsvector_formatter.h"
#include "common/log.h"

#define XXH_STATIC_LINKING_ONLY
#include "xxhash.h"
#ifdef CPU_ARCH_SSE
#include "xxh_x86dispatch.h"
#endif

#include <algorithm>
#include <cmath>
#include <numeric>

Log_SetChannel(GPUTextureCache);

namespace GPUTextureCache {
struct VRAMWrite
{
  GSVector4i active_rect;
  GSVector4i write_rect;
  HashType hash;

  struct PaletteRecord
  {
    // TODO: Flag HasSemitransparentDraws = (1 << 0),
    // TODO: Texture window, for sub texture dumping.
    SourceKey key;
    GSVector4i rect;
  };

  // List of palettes and rectangles drawn for dumping.
  // TODO: Keep these in texel-local space, not global space, that way texture sizes aren't aligned to 4 pixels.
  // But realistically, that probably isn't super common, and also requires modifying the renderer side of things.
  std::vector<PaletteRecord> palette_records;

  u32 num_splits;
  u32 num_page_refs;
  std::array<TListNode<VRAMWrite>, MAX_PAGE_REFS_PER_WRITE> page_refs;
};

struct PageEntry
{
  TList<Source> sources;
  TList<VRAMWrite> writes; // TODO: Split to own list
  GSVector4i draw_rect;    // NOTE: In global VRAM space.
  bool is_drawn = false;   // TODO: Split to bitset
};

struct HashCacheKey
{
  HashType texture_hash;
  HashType palette_hash;
  HashType mode;

  ALWAYS_INLINE bool operator==(const HashCacheKey& k) const
  {
    return (std::memcmp(&k, this, sizeof(HashCacheKey)) == 0);
  }
  ALWAYS_INLINE bool operator!=(const HashCacheKey& k) const
  {
    return (std::memcmp(&k, this, sizeof(HashCacheKey)) != 0);
  }
};
struct HashCacheKeyHash
{
  size_t operator()(const HashCacheKey& k) const;
};

struct HashCacheEntry
{
  std::unique_ptr<GPUTexture> texture;
  u32 ref_count;
  u32 age;
};

using HashCache = std::unordered_map<HashCacheKey, HashCacheEntry, HashCacheKeyHash>;

template<typename T>
ALWAYS_INLINE_RELEASE static void ListPrepend(TList<T>* list, T* item, TListNode<T>* item_node)
{
  item_node->ref = item;
  item_node->list = list;
  item_node->prev = nullptr;
  if (list->tail)
  {
    item_node->next = list->head;
    list->head->prev = item_node;
    list->head = item_node;
  }
  else
  {
    item_node->next = nullptr;
    list->head = item_node;
    list->tail = item_node;
  }
}

template<typename T>
ALWAYS_INLINE_RELEASE static void ListAppend(TList<T>* list, T* item, TListNode<T>* item_node)
{
  item_node->ref = item;
  item_node->list = list;
  item_node->next = nullptr;
  if (list->tail)
  {
    item_node->prev = list->tail;
    list->tail->next = item_node;
    list->tail = item_node;
  }
  else
  {
    item_node->prev = nullptr;
    list->head = item_node;
    list->tail = item_node;
  }
}

template<typename T>
ALWAYS_INLINE_RELEASE static void ListMoveToFront(TList<T>* list, TListNode<T>* item_node)
{
  DebugAssert(list->head);
  if (!item_node->prev)
    return;

  item_node->prev->next = item_node->next;
  if (item_node->next)
    item_node->next->prev = item_node->prev;
  else
    list->tail = item_node->prev;

  item_node->prev = nullptr;
  list->head->prev = item_node;
  item_node->next = list->head;
  list->head = item_node;
}

template<typename T>
ALWAYS_INLINE_RELEASE static void ListUnlink(const TListNode<T>& node)
{
  if (node.prev)
    node.prev->next = node.next;
  else
    node.list->head = node.next;
  if (node.next)
    node.next->prev = node.prev;
  else
    node.list->tail = node.prev;
}

template<typename T, typename F>
ALWAYS_INLINE_RELEASE static void ListIterate(const TList<T>& list, const F& f)
{
  for (const GPUTextureCache::TListNode<T>* n = list.head; n; n = n->next)
    f(n->ref);
}

template<typename T, typename F>
ALWAYS_INLINE_RELEASE static void ListIterateWithEarlyExit(const TList<T>& list, const F& f)
{
  for (const GPUTextureCache::TListNode<T>* n = list.head; n; n = n->next)
  {
    if (!f(n->ref))
      break;
  }
}

template<typename F>
ALWAYS_INLINE_RELEASE static void LoopRectPages(u32 left, u32 top, u32 right, u32 bottom, const F& f)
{
  DebugAssert(right <= VRAM_WIDTH && bottom <= VRAM_HEIGHT);
  DebugAssert((right - left) > 0 && (bottom - top) > 0);

  const u32 start_x = left / VRAM_PAGE_WIDTH;
  const u32 end_x = (right - 1) / VRAM_PAGE_WIDTH;
  const u32 start_y = top / VRAM_PAGE_HEIGHT;
  const u32 end_y = (bottom - 1) / VRAM_PAGE_HEIGHT;

  u32 page_number = VRAMPageIndex(start_x, start_y);
  for (u32 page_y = start_y; page_y <= end_y; page_y++)
  {
    u32 y_page_number = page_number;

    for (u32 page_x = start_x; page_x <= end_x; page_x++)
      f(y_page_number++);

    page_number += VRAM_PAGES_WIDE;
  }
}

template<typename F>
ALWAYS_INLINE_RELEASE static void LoopRectPagesWithEarlyExit(u32 left, u32 top, u32 right, u32 bottom, const F& f)
{
  DebugAssert(right <= VRAM_WIDTH && bottom <= VRAM_HEIGHT);
  DebugAssert((right - left) > 0 && (bottom - top) > 0);

  const u32 start_x = left / VRAM_PAGE_WIDTH;
  const u32 end_x = (right - 1) / VRAM_PAGE_WIDTH;
  const u32 start_y = top / VRAM_PAGE_HEIGHT;
  const u32 end_y = (bottom - 1) / VRAM_PAGE_HEIGHT;

  u32 page_number = VRAMPageIndex(start_x, start_y);
  for (u32 page_y = start_y; page_y <= end_y; page_y++)
  {
    u32 y_page_number = page_number;

    for (u32 page_x = start_x; page_x <= end_x; page_x++)
    {
      if (!f(y_page_number++))
        return;
    }

    page_number += VRAM_PAGES_WIDE;
  }
}

template<typename F>
ALWAYS_INLINE_RELEASE static void LoopRectPages(const GSVector4i& rc, const F& f)
{
  LoopRectPages(rc.left, rc.top, rc.right, rc.bottom, f);
}

template<typename F>
ALWAYS_INLINE_RELEASE static void LoopRectPagesWithEarlyExit(const GSVector4i& rc, const F& f)
{
  LoopRectPagesWithEarlyExit(rc.left, rc.top, rc.right, rc.bottom, f);
}

template<typename F>
ALWAYS_INLINE_RELEASE static void LoopXWrappedPages(u32 page, u32 num_pages, const F& f)
{
  for (u32 i = 0; i < num_pages; i++)
    f((page & VRAM_PAGE_Y_MASK) | ((page + i) & VRAM_PAGE_X_MASK));
}

static bool ShouldTrackVRAMWrites();
static bool IsDumpingVRAMWrites();

static const Source* ReturnSource(Source* source, const GSVector4i uv_rect);
static Source* CreateSource(SourceKey key);

static HashCacheEntry* LookupHashCache(SourceKey key, HashType tex_hash, HashType pal_hash);
static void ApplyTextureReplacements(SourceKey key, HashType tex_hash, HashType pal_hash, HashCacheEntry* entry);
static void RemoveFromHashCache(HashCache::iterator it);
static void ClearHashCache();

static HashType HashPage(u8 page, GPUTextureMode mode);
static HashType HashPalette(GPUTexturePaletteReg palette, GPUTextureMode mode);
static HashType HashRect(const GSVector4i& rc);

static void SyncVRAMWritePaletteRecords(VRAMWrite* entry);
static void UpdateVRAMWriteSources(VRAMWrite* entry, SourceKey source_key, const GSVector4i global_uv_rect);
static void SplitVRAMWrite(VRAMWrite* entry, const GSVector4i written_rect);
static void RemoveVRAMWrite(VRAMWrite* entry);
static void DumpTexturesFromVRAMWrite(VRAMWrite* entry);

static void DecodeTexture4(const u16* page, const u16* palette, u32 width, u32 height, u32* dest, u32 dest_stride);
static void DecodeTexture8(const u16* page, const u16* palette, u32 width, u32 height, u32* dest, u32 dest_stride);
static void DecodeTexture16(const u16* page, u32 width, u32 height, u32* dest, u32 dest_stride);
static void DecodeTexture(u8 page, GPUTexturePaletteReg palette, GPUTextureMode mode, GPUTexture* texture);

static constexpr const GSVector4i& INVALID_RECT = GPU_HW::INVALID_RECT;

static HashCache s_hash_cache;

static std::array<PageEntry, NUM_VRAM_PAGES> s_pages = {};

/// List of candidates for purging when the hash cache gets too large.
static std::vector<std::pair<HashCache::iterator, s32>> s_hash_cache_purge_list;

static bool s_track_vram_writes = false;
static u32 s_max_vram_write_splits = 1;

} // namespace GPUTextureCache

bool GPUTextureCache::ShouldTrackVRAMWrites()
{
  return (IsDumpingVRAMWrites() || (g_settings.texture_replacements.enable_texture_replacements &&
                                    TextureReplacements::HasVRAMWriteTextureReplacements()));
}

bool GPUTextureCache::IsDumpingVRAMWrites()
{
  return (g_settings.texture_replacements.dump_textures && !g_settings.texture_replacements.dump_texture_pages);
}

void GPUTextureCache::Initialize()
{
  UpdateVRAMTrackingState();
}

void GPUTextureCache::UpdateSettings(const Settings& old_settings)
{
  UpdateVRAMTrackingState();

  if (g_settings.texture_replacements.enable_texture_replacements !=
      old_settings.texture_replacements.enable_texture_replacements)
  {
    Invalidate();
  }
}

void GPUTextureCache::Shutdown()
{
  Invalidate();
  ClearHashCache();
  s_hash_cache_purge_list = {};
  s_track_vram_writes = false;
}

void GPUTextureCache::AddDrawnRectangle(const GSVector4i rect)
{
  // TODO: This might be a bit slow...
  LoopRectPages(rect, [&rect](u32 pn) {
    PageEntry& page = s_pages[pn];
    const GSVector4i rc = rect.rintersect(VRAMPageRect(pn));
    if (page.is_drawn)
    {
      if (!page.draw_rect.rcontains(rc))
      {
        page.draw_rect = page.draw_rect.runion(rc);
        GL_INS_FMT("Page {} drawn rect is now {}", pn, page.draw_rect);
        InvalidatePageSources(pn, page.draw_rect);
      }
    }
    else
    {
      GL_INS_FMT("Page {} drawn rect is now {}", pn, rc);
      page.draw_rect = rc;
      page.is_drawn = true;

      // remove all sources, let them re-lookup if needed
      InvalidatePageSources(pn, rc);
    }

    for (TListNode<VRAMWrite>* n = page.writes.head; n;)
    {
      VRAMWrite* it = n->ref;
      n = n->next;
      if (it->active_rect.rintersects(rect))
        RemoveVRAMWrite(it);
    }
  });
}

void GPUTextureCache::AddWrittenRectangle(const GSVector4i rect)
{
  LoopRectPages(rect, [&rect](u32 pn) {
    PageEntry& page = s_pages[pn];
    InvalidatePageSources(pn, rect);
    if (page.is_drawn)
    {
      if (page.draw_rect.rintersects(rect))
      {
        GL_INS_FMT("Clearing page {} draw rect due to write overlap", pn);
        page.is_drawn = false;
        page.draw_rect = INVALID_RECT;
      }
    }

    for (TListNode<VRAMWrite>* n = page.writes.head; n;)
    {
      VRAMWrite* it = n->ref;
      n = n->next;

      const GSVector4i intersection = it->active_rect.rintersect(rect);
      if (!intersection.rempty())
      {
        if (it->num_splits < s_max_vram_write_splits && !it->active_rect.eq(intersection))
          SplitVRAMWrite(it, intersection);
        else
          RemoveVRAMWrite(it);
      }
    }
  });
}

void GPUTextureCache::AddCopiedRectanglePart1(const GSVector4i rect)
{
  LoopRectPages(rect, [&rect](u32 pn) {
    PageEntry& page = s_pages[pn];
    InvalidatePageSources(pn, rect);
    if (page.is_drawn)
    {
      if (page.draw_rect.rintersects(rect))
      {
        GL_INS_FMT("Clearing page {} draw rect due to copy overlap", pn);
        page.is_drawn = false;
        page.draw_rect = INVALID_RECT;
      }
    }

    for (TListNode<VRAMWrite>* n = page.writes.head; n;)
    {
      VRAMWrite* it = n->ref;
      n = n->next;
      if (it->active_rect.rintersects(rect))
        DumpTexturesFromVRAMWrite(it);
    }
  });
}

void GPUTextureCache::AddCopiedRectanglePart2(const GSVector4i rect)
{
  LoopRectPages(rect, [&rect](u32 pn) {
    PageEntry& page = s_pages[pn];
    for (TListNode<VRAMWrite>* n = page.writes.head; n;)
    {
      VRAMWrite* it = n->ref;
      n = n->next;
      if (it->write_rect.rintersects(rect))
      {
        const HashType new_hash = HashRect(it->write_rect);
        DEV_LOG("VRAM Copy {:016X} => {:016X}", it->hash, new_hash);
        it->hash = new_hash;
      }
    }
  });
}

[[maybe_unused]] ALWAYS_INLINE static TinyString SourceKeyToString(const GPUTextureCache::SourceKey& key)
{
  static constexpr const std::array<const char*, 4> texture_modes = {
    {"Palette4Bit", "Palette8Bit", "Direct16Bit", "Reserved_Direct16Bit"}};

  TinyString ret;
  if (key.mode < GPUTextureMode::Direct16Bit)
  {
    ret.format("{} Page[{}] CLUT@[{},{}]", texture_modes[static_cast<u8>(key.mode)], key.page, key.palette.GetXBase(),
               key.palette.GetYBase());
  }
  else
  {
    ret.format("{} Page[{}]", texture_modes[static_cast<u8>(key.mode)], key.page);
  }
  return ret;
}

[[maybe_unused]] ALWAYS_INLINE static TinyString SourceToString(const GPUTextureCache::Source* src)
{
  return SourceKeyToString(src->key);
}

ALWAYS_INLINE_RELEASE static const u16* VRAMPagePointer(u32 pn)
{
  const u32 start_y = VRAMPageStartY(pn);
  const u32 start_x = VRAMPageStartX(pn);
  return &g_vram[start_y * VRAM_WIDTH + start_x];
}

// TODO: Vectorize these.
void GPUTextureCache::DecodeTexture4(const u16* page, const u16* palette, u32 width, u32 height, u32* dest,
                                     u32 dest_stride)
{
  if ((width % 4u) == 0)
  {
    const u32 vram_width = width / 4;
    for (u32 y = 0; y < height; y++)
    {
      const u16* page_ptr = page;
      u32* dest_ptr = dest;

      for (u32 x = 0; x < vram_width; x++)
      {
        const u32 pp = *(page_ptr++);
        *(dest_ptr++) = VRAMRGBA5551ToRGBA8888(palette[pp & 0x0F]);
        *(dest_ptr++) = VRAMRGBA5551ToRGBA8888(palette[(pp >> 4) & 0x0F]);
        *(dest_ptr++) = VRAMRGBA5551ToRGBA8888(palette[(pp >> 8) & 0x0F]);
        *(dest_ptr++) = VRAMRGBA5551ToRGBA8888(palette[pp >> 12]);
      }

      page += VRAM_WIDTH;
      dest = reinterpret_cast<u32*>(reinterpret_cast<u8*>(dest) + dest_stride);
    }
  }
  else
  {
    for (u32 y = 0; y < height; y++)
    {
      const u16* page_ptr = page;
      u32* dest_ptr = dest;

      u32 offs = 0;
      u16 texel = 0;
      for (u32 x = 0; x < width; x++)
      {
        if (offs == 0)
          texel = *(page_ptr++);

        *(dest_ptr++) = VRAMRGBA5551ToRGBA8888(palette[texel & 0x0F]);
        texel >>= 4;

        offs = (offs + 1) % 4;
      }

      page += VRAM_WIDTH;
      dest = reinterpret_cast<u32*>(reinterpret_cast<u8*>(dest) + dest_stride);
    }
  }
}
void GPUTextureCache::DecodeTexture8(const u16* page, const u16* palette, u32 width, u32 height, u32* dest,
                                     u32 dest_stride)
{
  if ((width % 2u) == 0)
  {
    const u32 vram_width = width / 2;
    for (u32 y = 0; y < height; y++)
    {
      const u16* page_ptr = page;
      u32* dest_ptr = dest;

      for (u32 x = 0; x < vram_width; x++)
      {
        const u32 pp = *(page_ptr++);
        *(dest_ptr++) = VRAMRGBA5551ToRGBA8888(palette[pp & 0xFF]);
        *(dest_ptr++) = VRAMRGBA5551ToRGBA8888(palette[pp >> 8]);
      }

      page += VRAM_WIDTH;
      dest = reinterpret_cast<u32*>(reinterpret_cast<u8*>(dest) + dest_stride);
    }
  }
  else
  {
    for (u32 y = 0; y < height; y++)
    {
      const u16* page_ptr = page;
      u32* dest_ptr = dest;

      u32 offs = 0;
      u16 texel = 0;
      for (u32 x = 0; x < width; x++)
      {
        if (offs == 0)
          texel = *(page_ptr++);

        *(dest_ptr++) = VRAMRGBA5551ToRGBA8888(palette[texel & 0xFF]);
        texel >>= 8;

        offs ^= 1;
      }

      page += VRAM_WIDTH;
      dest = reinterpret_cast<u32*>(reinterpret_cast<u8*>(dest) + dest_stride);
    }
  }
}

void GPUTextureCache::DecodeTexture16(const u16* page, u32 width, u32 height, u32* dest, u32 dest_stride)
{
  for (u32 y = 0; y < height; y++)
  {
    const u16* page_ptr = page;
    u32* dest_ptr = dest;

    for (u32 x = 0; x < width; x++)
      *(dest_ptr++) = VRAMRGBA5551ToRGBA8888(*(page_ptr++));

    page += VRAM_WIDTH;
    dest = reinterpret_cast<u32*>(reinterpret_cast<u8*>(dest) + dest_stride);
  }
}

void GPUTextureCache::DecodeTexture(const u16* page_ptr, GPUTexturePaletteReg palette, GPUTextureMode mode, u32* dest,
                                    u32 dest_stride, u32 width, u32 height)
{
  switch (mode)
  {
    case GPUTextureMode::Palette4Bit:
      DecodeTexture4(page_ptr, &g_vram[palette.GetYBase() * VRAM_WIDTH + palette.GetXBase()], width, height, dest,
                     dest_stride);
      break;
    case GPUTextureMode::Palette8Bit:
      DecodeTexture8(page_ptr, &g_vram[palette.GetYBase() * VRAM_WIDTH + palette.GetXBase()], width, height, dest,
                     dest_stride);
      break;
    case GPUTextureMode::Direct16Bit:
    case GPUTextureMode::Reserved_Direct16Bit:
      DecodeTexture16(page_ptr, width, height, dest, dest_stride);
      break;

      DefaultCaseIsUnreachable()
  }
}

void GPUTextureCache::DecodeTexture(u8 page, GPUTexturePaletteReg palette, GPUTextureMode mode, GPUTexture* texture)
{
  alignas(16) static u32 s_temp_buffer[TEXTURE_PAGE_WIDTH * TEXTURE_PAGE_HEIGHT];

  static constexpr bool DUMP = false;

  u32* tex_map;
  u32 tex_stride;
  const bool mapped = !DUMP && texture->Map(reinterpret_cast<void**>(&tex_map), &tex_stride, 0, 0, TEXTURE_PAGE_WIDTH,
                                            TEXTURE_PAGE_HEIGHT);
  if (!mapped)
  {
    tex_map = s_temp_buffer;
    tex_stride = sizeof(u32) * TEXTURE_PAGE_WIDTH;
  }

  const u16* page_ptr = VRAMPagePointer(page);
  DecodeTexture(page_ptr, palette, mode, tex_map, tex_stride, TEXTURE_PAGE_WIDTH, TEXTURE_PAGE_HEIGHT);

  if constexpr (DUMP)
  {
    static u32 n = 0;
    RGBA8Image image(TEXTURE_PAGE_WIDTH, TEXTURE_PAGE_HEIGHT, tex_map);
    image.SaveToFile(TinyString::from_format("D:\\dump\\hc_{}.png", ++n));
  }

  if (mapped)
    texture->Unmap();
  else
    texture->Update(0, 0, TEXTURE_PAGE_WIDTH, TEXTURE_PAGE_HEIGHT, tex_map, tex_stride);
}

const GPUTextureCache::Source* GPUTextureCache::LookupSource(SourceKey key, const GSVector4i rect)
{
  GL_SCOPE_FMT("TC: Lookup source {}", SourceKeyToString(key));

  TList<Source>& list = s_pages[key.page].sources;
  for (TListNode<Source>* n = list.head; n; n = n->next)
  {
    if (n->ref->key == key)
    {
      GL_INS("TC: Source hit");
      ListMoveToFront(&list, n);
      return ReturnSource(n->ref, rect);
    }
  }

  return ReturnSource(CreateSource(key), rect);
}

const GPUTextureCache::Source* GPUTextureCache::ReturnSource(Source* source, const GSVector4i uv_rect)
{
#ifdef _DEBUG
  if (source && !uv_rect.eq(INVALID_RECT))
  {
    LoopXWrappedPages(source->key.page, TexturePageCountForMode(source->key.mode), [&uv_rect](u32 pn) {
      const PageEntry& pe = s_pages[pn];
      ListIterate(pe.writes, [&uv_rect](const VRAMWrite* vrw) {
        if (const GSVector4i intersection = uv_rect.rintersect(vrw->write_rect); !intersection.rempty())
          GL_INS_FMT("TC: VRAM write was {:016X} ({})", vrw->hash, intersection);
      });
    });
    if (TextureModeHasPalette(source->key.mode))
      GL_INS_FMT("TC: Palette was {:016X}", source->palette_hash);
  }
#endif

  // TODO: Cache var.
  if (g_settings.texture_replacements.dump_textures)
    source->active_uv_rect = source->active_uv_rect.runion(uv_rect);

  return source;
}

bool GPUTextureCache::IsPageDrawn(u32 page_index)
{
  return s_pages[page_index].is_drawn;
}

bool GPUTextureCache::IsPageDrawn(u32 page_index, const GSVector4i rect)
{
  return s_pages[page_index].is_drawn && s_pages[page_index].draw_rect.rintersects(rect);
}

bool GPUTextureCache::IsRectDrawn(const GSVector4i rect)
{
  // TODO: This is potentially hot, so replace it with an explicit loop over the pages instead.
  bool drawn = false;
  LoopRectPages(rect, [&rect, &drawn](u32 pn) {
    if (IsPageDrawn(pn, rect))
    {
      drawn = true;
      return false;
    }
    return true;
  });
  return drawn;
}

bool GPUTextureCache::AreSourcePagesDrawn(SourceKey key, const GSVector4i rect)
{
#ifdef _DEBUG
  for (u32 offset = 0; offset < TexturePageCountForMode(key.mode); offset++)
  {
    if (IsPageDrawn(((key.page + offset) & VRAM_PAGE_X_MASK) + (key.page & VRAM_PAGE_Y_MASK), rect))
    {
      GL_INS_FMT("UV rect {} intersects page [{}] dirty rect {}, disabling TC", rect, key.page + offset,
                 s_pages[key.page + offset].draw_rect);
    }
  }
#endif

  switch (key.mode)
  {
    case GPUTextureMode::Palette4Bit:
    {
      return IsPageDrawn(key.page, rect);
    }

    case GPUTextureMode::Palette8Bit:
    {
      // 2 P4 pages per P8 page.
      const u32 yoffs = (key.page & VRAM_PAGE_Y_MASK);
      return (IsPageDrawn(key.page, rect) || IsPageDrawn(((key.page + 1) & VRAM_PAGE_X_MASK) + yoffs, rect));
    }

    case GPUTextureMode::Direct16Bit:
    case GPUTextureMode::Reserved_Direct16Bit:
    {
      // 4 P4 pages per C16 page.
      const u32 yoffs = (key.page & VRAM_PAGE_Y_MASK);
      return (IsPageDrawn(key.page, rect) || IsPageDrawn(((key.page + 1) & VRAM_PAGE_X_MASK) + yoffs, rect) ||
              IsPageDrawn(((key.page + 2) & VRAM_PAGE_X_MASK) + yoffs, rect) ||
              IsPageDrawn(((key.page + 3) & VRAM_PAGE_X_MASK) + yoffs, rect));
    }

      DefaultCaseIsUnreachable()
  }
}

GSVector4i GPUTextureCache::GetPageDrawnRect(u32 page_index)
{
  return s_pages[page_index].draw_rect;
}

void GPUTextureCache::Invalidate()
{
  for (u32 i = 0; i < NUM_VRAM_PAGES; i++)
  {
    InvalidatePageSources(i);

    PageEntry& page = s_pages[i];
    page.is_drawn = false;
    page.draw_rect = GSVector4i::zero();

    while (page.writes.tail)
      RemoveVRAMWrite(page.writes.tail->ref);
  }

  // should all be null
#ifdef _DEBUG
  for (u32 i = 0; i < NUM_VRAM_PAGES; i++)
    DebugAssert(!s_pages[i].sources.head && !s_pages[i].sources.tail);
#endif
}

void GPUTextureCache::InvalidatePageSources(u32 pn)
{
  DebugAssert(pn < NUM_VRAM_PAGES);

  TList<Source>& ps = s_pages[pn].sources;
  if (ps.head)
    GL_INS_FMT("Invalidate page {} sources", pn);

  for (TListNode<Source>* n = ps.head; n;)
  {
    Source* src = n->ref;
    n = n->next;

    DestroySource(src);
  }

  DebugAssert(!ps.head && !ps.tail);
}

void GPUTextureCache::InvalidatePageSources(u32 pn, const GSVector4i rc)
{
  DebugAssert(pn < NUM_VRAM_PAGES);

  TList<Source>& ps = s_pages[pn].sources;
  if (ps.head)
    GL_INS_FMT("Invalidate page {} sources overlapping with {}", pn, rc);

  for (TListNode<Source>* n = ps.head; n;)
  {
    Source* src = n->ref;
    n = n->next;

    // TODO: Make faster?
    if (!src->texture_rect.rintersects(rc) &&
        (src->key.mode == GPUTextureMode::Direct16Bit || !src->palette_rect.rintersects(rc)))
    {
      continue;
    }

    DestroySource(src);
  }
}

void GPUTextureCache::DestroySource(Source* src)
{
  GL_INS_FMT("Invalidate source {}", SourceToString(src));

  if (g_settings.texture_replacements.dump_textures && !src->active_uv_rect.eq(INVALID_RECT))
  {
    if (!g_settings.texture_replacements.dump_texture_pages)
    {
      // Find VRAM writes that overlap with this source
      LoopRectPages(src->active_uv_rect, [src](const u32 pn) {
        PageEntry& pg = s_pages[pn];
        ListIterate(pg.writes, [src](VRAMWrite* vw) {
          UpdateVRAMWriteSources(vw, src->key, src->active_uv_rect);
        });
        return true;
      });
    }
    else
    {
      // Dump active area from page
      TextureReplacements::DumpTexture(TextureReplacements::ReplacementType::TextureFromPage, src->texture_rect,
                                       src->texture_hash, src->palette_hash, src->key.mode, src->key.palette,
                                       src->active_uv_rect);
    }
  }

  for (u32 i = 0; i < src->num_page_refs; i++)
    ListUnlink(src->page_refs[i]);

  if (src->from_hash_cache)
  {
    DebugAssert(src->from_hash_cache->ref_count > 0);
    src->from_hash_cache->ref_count--;
  }
  else
  {
    g_gpu_device->RecycleTexture(std::unique_ptr<GPUTexture>(src->texture));
  }

  delete src;
}

GPUTextureCache::Source* GPUTextureCache::CreateSource(SourceKey key)
{
  GL_INS_FMT("TC: Create source {}", SourceKeyToString(key));

  const HashType tex_hash = HashPage(key.page, key.mode);
  const HashType pal_hash = (key.mode < GPUTextureMode::Direct16Bit) ? HashPalette(key.palette, key.mode) : 0;
  HashCacheEntry* hcentry = LookupHashCache(key, tex_hash, pal_hash);
  if (!hcentry)
  {
    GL_INS("TC: Hash cache lookup fail?!");
    return nullptr;
  }

  hcentry->ref_count++;
  hcentry->age = 0;

  Source* src = new Source();
  src->key = key;
  src->num_page_refs = 0;
  src->texture = hcentry->texture.get();
  src->from_hash_cache = hcentry;
  src->texture_hash = tex_hash;
  src->palette_hash = pal_hash;

  // Textures at front, CLUTs at back.
  std::array<u32, MAX_PAGE_REFS_PER_SOURCE> page_refns;
  const auto add_page_ref = [src, &page_refns](u32 pn) {
    // Don't double up references
    for (u32 i = 0; i < src->num_page_refs; i++)
    {
      if (page_refns[i] == pn)
        return;
    }

    const u32 ri = src->num_page_refs++;
    page_refns[ri] = pn;

    ListPrepend(&s_pages[pn].sources, src, &src->page_refs[ri]);
  };
  const auto add_page_ref_back = [src, &page_refns](u32 pn) {
    // Don't double up references
    for (u32 i = 0; i < src->num_page_refs; i++)
    {
      if (page_refns[i] == pn)
        return;
    }

    const u32 ri = src->num_page_refs++;
    page_refns[ri] = pn;

    ListAppend(&s_pages[pn].sources, src, &src->page_refs[ri]);
  };

  src->texture_rect = GetTextureRect(key.page, key.mode);
  src->active_uv_rect = INVALID_RECT;
  LoopXWrappedPages(key.page, TexturePageCountForMode(key.mode), add_page_ref);

  if (key.mode < GPUTextureMode::Direct16Bit)
  {
    src->palette_rect = GetPaletteRect(key.palette, key.mode);
    LoopXWrappedPages(PalettePageNumber(key.palette), PalettePageCountForMode(key.mode), add_page_ref_back);
  }

  GL_INS_FMT("Appended new source {} to {} pages", SourceToString(src), src->num_page_refs);
  return src;
}

void GPUTextureCache::TrackVRAMWrite(const GSVector4i rect)
{
  if (!s_track_vram_writes)
    return;

  VRAMWrite* it = new VRAMWrite();
  it->active_rect = rect;
  it->write_rect = rect;
  it->hash = HashRect(rect);
  it->num_page_refs = 0;
  LoopRectPages(rect, [it](u32 pn) {
    DebugAssert(it->num_page_refs < MAX_PAGE_REFS_PER_WRITE);
    ListAppend(&s_pages[pn].writes, it, &it->page_refs[it->num_page_refs++]);
    return true;
  });

  DEV_LOG("New VRAM write {:016X} at {} touching {} pages", it->hash, rect, it->num_page_refs);
}

void GPUTextureCache::UpdateVRAMTrackingState()
{
  s_track_vram_writes = ShouldTrackVRAMWrites();
}

void GPUTextureCache::SyncVRAMWritePaletteRecords(VRAMWrite* entry)
{
  // Have to go through any sources that intersect this write, because they may not have been invalidated yet, in which
  // case the active rect also will not have been updated.
  if (IsDumpingVRAMWrites())
  {
    LoopRectPages(entry->active_rect, [entry](const u32 pn) {
      const PageEntry& page = s_pages[pn];
      ListIterate(page.sources, [entry](const Source* src) {
        if (!src->active_uv_rect.eq(INVALID_RECT))
          UpdateVRAMWriteSources(entry, src->key, src->active_uv_rect);
      });

      return true;
    });
  }
}

void GPUTextureCache::UpdateVRAMWriteSources(VRAMWrite* entry, SourceKey source_key, const GSVector4i global_uv_rect)
{
  // convert to VRAM write space
  const GSVector4i write_intersection = entry->active_rect.rintersect(global_uv_rect);
  if (write_intersection.rempty())
    return;

  // Add to the palette tracking list
  auto iter = std::find_if(entry->palette_records.begin(), entry->palette_records.end(),
                           [&source_key](const auto& it) { return (it.key == source_key); });
  if (iter != entry->palette_records.end())
    iter->rect = iter->rect.runion(write_intersection);
  else
    entry->palette_records.emplace_back(source_key, write_intersection);
}

void GPUTextureCache::SplitVRAMWrite(VRAMWrite* entry, const GSVector4i written_rect)
{
  SyncVRAMWritePaletteRecords(entry);

  const s32 to_left = (written_rect.left - entry->active_rect.left);
  const s32 to_right = (entry->active_rect.right - written_rect.right);
  const s32 to_top = (written_rect.top - entry->active_rect.top);
  const s32 to_bottom = (entry->active_rect.bottom - written_rect.bottom);
  DebugAssert(to_left > 0 || to_right > 0 || to_top > 0 || to_bottom > 0);

  entry->num_splits++;

  GSVector4i rects[4];

  // TODO: more efficient vector swizzle
  if (std::max(to_top, to_bottom) > std::max(to_left, to_right))
  {
    // split top/bottom, then left/right
    rects[0] = GSVector4i(entry->active_rect.left, entry->active_rect.top, entry->active_rect.right, written_rect.top);
    rects[1] =
      GSVector4i(entry->active_rect.left, written_rect.bottom, entry->active_rect.right, entry->active_rect.bottom);
    rects[2] = GSVector4i(entry->active_rect.left, entry->active_rect.top + to_top, entry->active_rect.left + to_left,
                          entry->active_rect.bottom - to_bottom);
    rects[3] = GSVector4i(entry->active_rect.right - to_right, entry->active_rect.top + to_top,
                          entry->active_rect.right, entry->active_rect.bottom - to_bottom);
  }
  else
  {
    // split left/right, then top/bottom
    rects[0] =
      GSVector4i(entry->active_rect.left, entry->active_rect.top, written_rect.left, entry->active_rect.bottom);
    rects[1] =
      GSVector4i(written_rect.right, entry->active_rect.top, entry->active_rect.right, entry->active_rect.bottom);
    rects[2] = GSVector4i(entry->active_rect.left + to_left, entry->active_rect.top + to_top,
                          written_rect.right - to_right, entry->active_rect.top - to_top);
    rects[3] = GSVector4i(entry->active_rect.left + to_left, entry->active_rect.bottom - to_bottom,
                          written_rect.right - to_right, entry->active_rect.bottom);
  }

  for (size_t i = 0; i < std::size(rects); i++)
  {
    const GSVector4i splitr = rects[i];
    if (splitr.rempty())
      continue;

    VRAMWrite* it = new VRAMWrite();
    it->write_rect = entry->write_rect;
    it->active_rect = splitr;
    it->hash = entry->hash;
    it->num_splits = entry->num_splits;
    it->num_page_refs = 0;

    // TODO: We probably want to share this...
    it->palette_records.reserve(entry->palette_records.size());
    for (const auto& [source_key, source_rect] : it->palette_records)
    {
      if (source_rect.rintersects(splitr))
        it->palette_records.emplace_back(source_key, source_rect);
    }

    LoopRectPages(splitr, [it](u32 pn) {
      DebugAssert(it->num_page_refs < MAX_PAGE_REFS_PER_WRITE);
      ListAppend(&s_pages[pn].writes, it, &it->page_refs[it->num_page_refs++]);
      return true;
    });

    DEV_LOG("Split VRAM write {:016X} at {} in direction {} => {}", it->hash, entry->active_rect, i, splitr);
  }

  for (u32 i = 0; i < entry->num_page_refs; i++)
    ListUnlink(entry->page_refs[i]);

  delete entry;
}

void GPUTextureCache::RemoveVRAMWrite(VRAMWrite* entry)
{
  DEV_LOG("Remove VRAM write {:016X} at {}", entry->hash, entry->write_rect);

  SyncVRAMWritePaletteRecords(entry);

  if (entry->num_splits > 0 && !entry->palette_records.empty())
  {
    // Combine palette records with another write.
    VRAMWrite* other_write = nullptr;
    LoopRectPagesWithEarlyExit(entry->write_rect, [&entry, &other_write](u32 pn) {
      PageEntry& pg = s_pages[pn];
      ListIterateWithEarlyExit(pg.writes, [&entry, &other_write](VRAMWrite* cur) {
        if (cur->hash != entry->hash)
          return true;

        other_write = cur;
        return false;
      });
      return (other_write == nullptr);
    });
    if (other_write)
    {
      for (const auto& [source_key, local_rect] : entry->palette_records)
      {
        const auto iter =
          std::find_if(other_write->palette_records.begin(), other_write->palette_records.end(),
                       [&source_key](const VRAMWrite::PaletteRecord& it) { return it.key == source_key; });
        if (iter != other_write->palette_records.end())
          iter->rect = iter->rect.runion(local_rect);
        else
          other_write->palette_records.emplace_back(source_key, local_rect);
      }

      // No dumping from here!
      entry->palette_records.clear();
    }
  }

  for (u32 i = 0; i < entry->num_page_refs; i++)
    ListUnlink(entry->page_refs[i]);

  DumpTexturesFromVRAMWrite(entry);

  delete entry;
}

void GPUTextureCache::DumpTexturesFromVRAMWrite(VRAMWrite* entry)
{
  if (g_settings.texture_replacements.dump_textures && !g_settings.texture_replacements.dump_texture_pages)
  {
    for (const auto& [source_key, local_rect] : entry->palette_records)
    {
      const HashType pal_hash =
        (source_key.mode < GPUTextureMode::Direct16Bit) ? HashPalette(source_key.palette, source_key.mode) : 0;

      // TODO: Option to disable C16
      if (source_key.mode == GPUTextureMode::Direct16Bit)
        continue;

      TextureReplacements::DumpTexture(TextureReplacements::ReplacementType::TextureFromVRAMWrite, entry->write_rect,
                                       entry->hash, pal_hash, source_key.mode, source_key.palette, local_rect);
#if 0
      if (TextureModeHasPalette(source_key.mode))
        TextureReplacements::DumpTexture(TextureReplacements::ReplacementType::TextureFromVRAMWrite, entry->write_rect,
          entry->hash, 0, GPUTextureMode::Direct16Bit, {}, entry->rect);
#endif
    }
  }
}

GPUTextureCache::HashType GPUTextureCache::HashPage(u8 page, GPUTextureMode mode)
{
  XXH3_state_t state;
  XXH3_64bits_reset(&state);

  // Pages aren't contiguous in memory :(
  const u16* page_ptr = VRAMPagePointer(page);

  switch (mode)
  {
    case GPUTextureMode::Palette4Bit:
    {
      for (u32 y = 0; y < VRAM_PAGE_HEIGHT; y++)
      {
        XXH3_64bits_update(&state, page_ptr, VRAM_PAGE_WIDTH * sizeof(u16));
        page_ptr += VRAM_WIDTH;
      }
    }
    break;

    case GPUTextureMode::Palette8Bit:
    {
      for (u32 y = 0; y < VRAM_PAGE_HEIGHT; y++)
      {
        XXH3_64bits_update(&state, page_ptr, VRAM_PAGE_WIDTH * 2 * sizeof(u16));
        page_ptr += VRAM_WIDTH;
      }
    }
    break;

    case GPUTextureMode::Direct16Bit:
    {
      for (u32 y = 0; y < VRAM_PAGE_HEIGHT; y++)
      {
        XXH3_64bits_update(&state, page_ptr, VRAM_PAGE_WIDTH * 4 * sizeof(u16));
        page_ptr += VRAM_WIDTH;
      }
    }
    break;

      DefaultCaseIsUnreachable()
  }

  return XXH3_64bits_digest(&state);
}

GPUTextureCache::HashType GPUTextureCache::HashPalette(GPUTexturePaletteReg palette, GPUTextureMode mode)
{
  const u16* base = &g_vram[palette.GetYBase() * VRAM_WIDTH + palette.GetXBase()];

  switch (mode)
  {
    case GPUTextureMode::Palette4Bit:
      return XXH3_64bits(base, sizeof(u16) * 16);

    case GPUTextureMode::Palette8Bit:
      return XXH3_64bits(base, sizeof(u16) * 256);

      DefaultCaseIsUnreachable()
  }
}

GPUTextureCache::HashType GPUTextureCache::HashRect(const GSVector4i& rc)
{
  XXH3_state_t state;
  XXH3_64bits_reset(&state);

  const u32 width = rc.width();
  const u32 height = rc.height();
  const u16* ptr = &g_vram[rc.top * VRAM_WIDTH + rc.left];
  for (u32 y = 0; y < height; y++)
  {
    XXH3_64bits_update(&state, ptr, width * sizeof(u16));
    ptr += VRAM_WIDTH;
  }

  return XXH3_64bits_digest(&state);
}

GPUTextureCache::HashCacheEntry* GPUTextureCache::LookupHashCache(SourceKey key, HashType tex_hash, HashType pal_hash)
{
  const HashCacheKey hkey = {tex_hash, pal_hash, static_cast<HashType>(key.mode)};

  const auto it = s_hash_cache.find(hkey);
  if (it != s_hash_cache.end())
  {
    GL_INS_FMT("TC: Hash cache hit {:X} {:X}", hkey.texture_hash, hkey.palette_hash);
    return &it->second;
  }

  GL_INS_FMT("TC: Hash cache miss {:X} {:X}", hkey.texture_hash, hkey.palette_hash);

  HashCacheEntry entry;
  entry.ref_count = 0;
  entry.age = 0;
  entry.texture = g_gpu_device->FetchTexture(TEXTURE_PAGE_WIDTH, TEXTURE_PAGE_HEIGHT, 1, 1, 1,
                                             GPUTexture::Type::Texture, GPUTexture::Format::RGBA8);
  if (!entry.texture)
  {
    ERROR_LOG("Failed to create texture.");
    return nullptr;
  }

  DecodeTexture(key.page, key.palette, key.mode, entry.texture.get());

  if (g_settings.texture_replacements.enable_texture_replacements)
    ApplyTextureReplacements(key, tex_hash, pal_hash, &entry);

  return &s_hash_cache.emplace(hkey, std::move(entry)).first->second;
}

void GPUTextureCache::RemoveFromHashCache(HashCache::iterator it)
{
  g_gpu_device->RecycleTexture(std::move(it->second.texture));
  s_hash_cache.erase(it);
}

void GPUTextureCache::ClearHashCache()
{
  while (!s_hash_cache.empty())
    RemoveFromHashCache(s_hash_cache.begin());
}

void GPUTextureCache::AgeHashCache()
{
  // Number of frames before unused hash cache entries are evicted.
  static constexpr u32 MAX_HASH_CACHE_AGE = 600;

  // Maximum number of textures which are permitted in the hash cache at the end of the frame.
  static constexpr u32 MAX_HASH_CACHE_SIZE = 500;

  bool might_need_cache_purge = (s_hash_cache.size() > MAX_HASH_CACHE_SIZE);
  if (might_need_cache_purge)
    s_hash_cache_purge_list.clear();

  for (auto it = s_hash_cache.begin(); it != s_hash_cache.end();)
  {
    HashCacheEntry& e = it->second;
    if (e.ref_count > 0)
    {
      ++it;
      continue;
    }

    if (++e.age > MAX_HASH_CACHE_AGE)
    {
      RemoveFromHashCache(it++);
      continue;
    }

    // We might free up enough just with "normal" removals above.
    if (might_need_cache_purge)
    {
      might_need_cache_purge = (s_hash_cache.size() > MAX_HASH_CACHE_SIZE);
      if (might_need_cache_purge)
        s_hash_cache_purge_list.emplace_back(it, static_cast<s32>(e.age));
    }

    ++it;
  }

  // Pushing to a list, sorting, and removing ends up faster than re-iterating the map.
  if (might_need_cache_purge)
  {
    std::sort(s_hash_cache_purge_list.begin(), s_hash_cache_purge_list.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.second > rhs.second; });

    const u32 entries_to_purge = std::min(static_cast<u32>(s_hash_cache.size() - MAX_HASH_CACHE_SIZE),
                                          static_cast<u32>(s_hash_cache_purge_list.size()));
    for (u32 i = 0; i < entries_to_purge; i++)
      RemoveFromHashCache(s_hash_cache_purge_list[i].first);
  }
}

size_t GPUTextureCache::HashCacheKeyHash::operator()(const HashCacheKey& k) const
{
  std::size_t h = 0;
  hash_combine(h, k.texture_hash, k.palette_hash, k.mode);
  return h;
}

void GPUTextureCache::ApplyTextureReplacements(SourceKey key, HashType tex_hash, HashType pal_hash,
                                               HashCacheEntry* entry)
{
  std::vector<TextureReplacements::ReplacementSubImage> subimages;
  if (TextureReplacements::HasTexturePageTextureReplacements())
    TextureReplacements::GetTexturePageTextureReplacements(subimages, tex_hash, pal_hash, key.mode);

  if (TextureReplacements::HasVRAMWriteTextureReplacements())
  {
    const GSVector4i page_rect = VRAMPageRect(key.page);
    LoopRectPages(page_rect, [&key, &pal_hash, &subimages, &page_rect](u32 pn) {
      const PageEntry& page = s_pages[pn];
      ListIterate(page.writes, [&key, &pal_hash, &subimages, &page_rect](const VRAMWrite* vrw) {
        // TODO: Is this needed?
        if (!vrw->write_rect.rintersects(page_rect))
          return;

        // Map VRAM write to the start of the page.
        GSVector2i offset_to_page = page_rect.sub32(vrw->write_rect).xy();

        // Need to apply the texture shift on the X dimension, not Y. No SLLV on SSE4.. :(
        offset_to_page.x = ApplyTextureModeShift(key.mode, offset_to_page.x);

        TextureReplacements::GetVRAMWriteTextureReplacements(subimages, vrw->hash, pal_hash, key.mode, offset_to_page);
      });
    });
  }

  if (subimages.empty())
    return;

  float max_scale_x = subimages[0].scale_x, max_scale_y = subimages[0].scale_y;
  for (size_t i = 0; i < subimages.size(); i++)
  {
    max_scale_x = std::max(max_scale_x, subimages[i].scale_x);
    max_scale_y = std::max(max_scale_y, subimages[i].scale_y);
  }
  const GSVector4 max_scale_v = GSVector4(max_scale_x, max_scale_y).xyxy();

  // TODO: Clamp to max texture size
  const u32 new_width = static_cast<u32>(std::ceil(static_cast<float>(entry->texture->GetWidth()) * max_scale_x));
  const u32 new_height = static_cast<u32>(std::ceil(static_cast<float>(entry->texture->GetHeight()) * max_scale_y));
  std::unique_ptr<GPUTexture> new_texture = g_gpu_device->FetchTexture(
    new_width, new_height, 1, 1, 1, GPUTexture::Type::RenderTarget, GPUTexture::Format::RGBA8);
  if (!new_texture)
    return;

  // TODO: This is AWFUL. Need a better way.
  static constexpr const float u_src_rect[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  g_gpu_device->SetRenderTarget(new_texture.get());
  g_gpu_device->SetViewportAndScissor(0, 0, new_width, new_height);
  g_gpu_device->SetPipeline(static_cast<GPU_HW*>(g_gpu.get())->GetVRAMReplacementPipeline());
  g_gpu_device->PushUniformBuffer(u_src_rect, sizeof(u_src_rect));
  g_gpu_device->SetTextureSampler(0, entry->texture.get(),
                                  g_gpu_device->GetNearestSampler()); // TODO: nearest vs linear
  g_gpu_device->Draw(3, 0);
  for (const TextureReplacements::ReplacementSubImage& si : subimages)
  {
    const auto temp_texture = g_gpu_device->FetchAutoRecycleTexture(
      si.image.GetWidth(), si.image.GetHeight(), 1, 1, 1, GPUTexture::Type::Texture, GPUTexture::Format::RGBA8,
      si.image.GetPixels(), si.image.GetPitch());
    if (!temp_texture)
      continue;

    const GSVector4i dst_rect = GSVector4i(GSVector4(si.dst_rect) * max_scale_v);
    g_gpu_device->SetViewportAndScissor(dst_rect);
    g_gpu_device->SetTextureSampler(0, temp_texture.get(),
                                    g_gpu_device->GetNearestSampler()); // TODO: nearest vs linear
    g_gpu_device->Draw(3, 0);
  }

  g_gpu_device->RecycleTexture(std::move(entry->texture));
  entry->texture = std::move(new_texture);

  g_gpu->RestoreDeviceContext();
}