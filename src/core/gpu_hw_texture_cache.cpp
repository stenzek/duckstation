// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "gpu_hw_texture_cache.h"
#include "gpu_hw.h"
#include "gpu_hw_shadergen.h"
#include "settings.h"
#include "system.h"

#include "util/gpu_device.h"
#include "util/state_wrapper.h"

#include "common/gsvector_formatter.h"
#include "common/log.h"
#include "common/string_util.h"

#define XXH_STATIC_LINKING_ONLY
#include "xxhash.h"
#ifdef CPU_ARCH_SSE
#include "xxh_x86dispatch.h"
#endif

#include <algorithm>
#include <cmath>
#include <numeric>

Log_SetChannel(GPUTextureCache);

// TODO: Fix copy-as-write.
// TODO: Fix VRAM usage.
// TODO: Write coalescing, xenogears.

// #define ALWAYS_TRACK_VRAM_WRITES 1

namespace GPUTextureCache {
static constexpr u32 MAX_CLUT_SIZE = 256;

struct VRAMWrite
{
  GSVector4i active_rect;
  GSVector4i write_rect;
  HashType hash;

  struct PaletteRecord
  {
    // TODO: Texture window, for sub texture dumping.
    GSVector4i rect;
    SourceKey key;
    PaletteRecordFlags flags;

    // Awkward to store, but we need to keep a backup copy of each CLUT, because if the CLUT gets overwritten
    // before the VRAM write, when we go to dump the texture, it'll be incorrect.
    HashType palette_hash;
    u16 palette[MAX_CLUT_SIZE];
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
  u32 last_used_frame;
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

ALWAYS_INLINE void DoStateVector(StateWrapper& sw, GSVector4i* vec)
{
  sw.DoBytes(vec->I32, sizeof(vec->I32));
}

static bool ShouldTrackVRAMWrites();
static bool IsDumpingVRAMWriteTextures();

static bool CompilePipelines();
static void DestroyPipelines();

static const Source* ReturnSource(Source* source, const GSVector4i uv_rect, PaletteRecordFlags flags);
static Source* CreateSource(SourceKey key);

static HashCacheEntry* LookupHashCache(SourceKey key, HashType tex_hash, HashType pal_hash);
static void ApplyTextureReplacements(SourceKey key, HashType tex_hash, HashType pal_hash, HashCacheEntry* entry);
static void RemoveFromHashCache(HashCache::iterator it);
static void ClearHashCache();

static HashType HashPage(u8 page, GPUTextureMode mode);
static HashType HashPalette(GPUTexturePaletteReg palette, GPUTextureMode mode);
static HashType HashPartialPalette(const u16* palette, u32 min, u32 max);
static HashType HashRect(const GSVector4i rc);

static std::pair<u32, u32> ReducePaletteBounds(const GSVector4i rect, GPUTextureMode mode,
                                               GPUTexturePaletteReg palette);
static void SyncVRAMWritePaletteRecords(VRAMWrite* entry);
static void InitializeVRAMWritePaletteRecord(VRAMWrite::PaletteRecord* record, SourceKey source_key,
                                             const GSVector4i rect, PaletteRecordFlags flags);
static void UpdateVRAMWriteSources(VRAMWrite* entry, SourceKey source_key, const GSVector4i global_uv_rect,
                                   PaletteRecordFlags flags);
static void SplitVRAMWrite(VRAMWrite* entry, const GSVector4i written_rect);
static void RemoveVRAMWrite(VRAMWrite* entry);
static void DumpTexturesFromVRAMWrite(VRAMWrite* entry);

static void DecodeTexture4(const u16* page, const u16* palette, u32 width, u32 height, u32* dest, u32 dest_stride);
static void DecodeTexture8(const u16* page, const u16* palette, u32 width, u32 height, u32* dest, u32 dest_stride);
static void DecodeTexture16(const u16* page, u32 width, u32 height, u32* dest, u32 dest_stride);
static void DecodeTexture(u8 page, GPUTexturePaletteReg palette, GPUTextureMode mode, GPUTexture* texture);

static constexpr const GSVector4i& INVALID_RECT = GPU_HW::INVALID_RECT;
static constexpr const GPUTexture::Format REPLACEMENT_TEXTURE_FORMAT = GPUTexture::Format::RGBA8;

// TODO: Pack in struct

static HashCache s_hash_cache;
static size_t s_hash_cache_memory_usage = 0;
static size_t s_max_hash_cache_memory_usage = 1ULL * 1024ULL * 1024ULL * 1024ULL; // 2GB

static std::array<PageEntry, NUM_VRAM_PAGES> s_pages = {};

/// List of candidates for purging when the hash cache gets too large.
static std::vector<std::pair<HashCache::iterator, s32>> s_hash_cache_purge_list;

/// List of VRAM writes collected when saving state.
static std::vector<VRAMWrite*> s_temp_vram_write_list;

static std::unique_ptr<GPUTexture> s_replacement_texture_render_target;
static std::unique_ptr<GPUPipeline> s_replacement_init_pipeline;
static std::unique_ptr<GPUPipeline> s_replacement_draw_pipeline;                 // copies alpha as-is
static std::unique_ptr<GPUPipeline> s_replacement_semitransparent_draw_pipeline; // inverts alpha (i.e. semitransparent)

static bool s_track_vram_writes = false;

} // namespace GPUTextureCache

bool GPUTextureCache::ShouldTrackVRAMWrites()
{
#ifdef ALWAYS_TRACK_VRAM_WRITES
  return true;
#else
  return (IsDumpingVRAMWriteTextures() || (g_settings.texture_replacements.enable_texture_replacements &&
                                           TextureReplacements::HasVRAMWriteTextureReplacements()));
#endif
}

bool GPUTextureCache::IsDumpingVRAMWriteTextures()
{
  return (g_settings.texture_replacements.dump_textures && !TextureReplacements::GetConfig().dump_texture_pages);
}

bool GPUTextureCache::Initialize()
{
  UpdateVRAMTrackingState();
  if (!CompilePipelines())
    return false;

  return true;
}

void GPUTextureCache::UpdateSettings(const Settings& old_settings)
{
  UpdateVRAMTrackingState();

  if (g_settings.texture_replacements.enable_texture_replacements !=
      old_settings.texture_replacements.enable_texture_replacements)
  {
    Invalidate();
    ClearHashCache();

    DestroyPipelines();
    if (!CompilePipelines())
      Panic("Failed to compile pipelines on TC settings change");
  }
}

bool GPUTextureCache::DoState(StateWrapper& sw, bool skip)
{
  if (sw.GetVersion() < 67)
  {
    if (!skip)
      WARNING_LOG("Texture cache not in save state due to old version.");

    Invalidate();
    return true;
  }

  if (!sw.DoMarker("GPUTextureCache"))
    return false;

  if (sw.IsReading())
  {
    if (!skip)
      Invalidate();

    u32 num_vram_writes = 0;
    sw.Do(&num_vram_writes);

    const bool skip_writes = (skip || !s_track_vram_writes);

    for (u32 i = 0; i < num_vram_writes; i++)
    {
      static constexpr u32 PALETTE_RECORD_SIZE = sizeof(GSVector4i) + sizeof(SourceKey) + sizeof(PaletteRecordFlags) +
                                                 sizeof(HashType) + sizeof(u16) * MAX_CLUT_SIZE;

      if (skip_writes)
      {
        sw.SkipBytes(sizeof(GSVector4i) * 2 + sizeof(HashType));

        u32 num_palette_records = 0;
        sw.Do(&num_palette_records);
        sw.SkipBytes(num_palette_records * PALETTE_RECORD_SIZE);
      }
      else
      {
        VRAMWrite* vrw = new VRAMWrite();
        DoStateVector(sw, &vrw->active_rect);
        DoStateVector(sw, &vrw->write_rect);
        sw.Do(&vrw->hash);

        u32 num_palette_records = 0;
        sw.Do(&num_palette_records);

        // Skip palette records if we're not dumping now.
        if (g_settings.texture_replacements.dump_textures)
        {
          vrw->palette_records.reserve(num_palette_records);
          for (u32 j = 0; j < num_palette_records; j++)
          {
            VRAMWrite::PaletteRecord& rec = vrw->palette_records.emplace_back();
            DoStateVector(sw, &rec.rect);
            sw.DoBytes(&rec.key, sizeof(rec.key));
            sw.Do(&rec.flags);
            sw.Do(&rec.palette_hash);
            sw.DoBytes(rec.palette, sizeof(rec.palette));
          }
        }
        else
        {
          sw.SkipBytes(num_palette_records * PALETTE_RECORD_SIZE);
        }

        if (sw.HasError())
        {
          delete vrw;
          Invalidate();
          return false;
        }

        vrw->num_page_refs = 0;
        LoopRectPages(vrw->active_rect, [vrw](u32 pn) {
          DebugAssert(vrw->num_page_refs < MAX_PAGE_REFS_PER_WRITE);
          ListAppend(&s_pages[pn].writes, vrw, &vrw->page_refs[vrw->num_page_refs++]);
          return true;
        });
      }
    }
  }
  else
  {
    s_temp_vram_write_list.clear();

    if (!skip && s_track_vram_writes)
    {
      for (PageEntry& page : s_pages)
      {
        ListIterate(page.writes, [](VRAMWrite* vrw) {
          if (std::find(s_temp_vram_write_list.begin(), s_temp_vram_write_list.end(), vrw) !=
              s_temp_vram_write_list.end())
          {
            return;
          }

          // try not to lose data... pull it from the sources
          if (g_settings.texture_replacements.dump_textures)
            SyncVRAMWritePaletteRecords(vrw);

          s_temp_vram_write_list.push_back(vrw);
        });
      }
    }

    u32 num_vram_writes = static_cast<u32>(s_temp_vram_write_list.size());
    sw.Do(&num_vram_writes);
    for (VRAMWrite* vrw : s_temp_vram_write_list)
    {
      DoStateVector(sw, &vrw->active_rect);
      DoStateVector(sw, &vrw->write_rect);
      sw.Do(&vrw->hash);

      u32 num_palette_records = static_cast<u32>(vrw->palette_records.size());
      sw.Do(&num_palette_records);
      for (VRAMWrite::PaletteRecord& rec : vrw->palette_records)
      {
        DoStateVector(sw, &rec.rect);
        sw.DoBytes(&rec.key, sizeof(rec.key));
        sw.Do(&rec.flags);
        sw.Do(&rec.palette_hash);
        sw.DoBytes(rec.palette, sizeof(rec.palette));
      }
    }
  }

  return !sw.HasError();
}

void GPUTextureCache::Shutdown()
{
  Invalidate();
  ClearHashCache();
  DestroyPipelines();
  s_replacement_texture_render_target.reset();
  s_hash_cache_purge_list = {};
  s_temp_vram_write_list = {};
  s_track_vram_writes = false;
}

bool GPUTextureCache::CompilePipelines()
{
  if (!g_settings.texture_replacements.enable_texture_replacements)
    return true;

  GPUPipeline::GraphicsConfig plconfig = {};
  plconfig.layout = GPUPipeline::Layout::SingleTextureAndPushConstants;
  plconfig.input_layout.vertex_attributes = {};
  plconfig.input_layout.vertex_stride = 0;
  plconfig.rasterization = GPUPipeline::RasterizationState::GetNoCullState();
  plconfig.depth = GPUPipeline::DepthState::GetNoTestsState();
  plconfig.blend = GPUPipeline::BlendState::GetNoBlendingState();
  plconfig.primitive = GPUPipeline::Primitive::Triangles;
  plconfig.geometry_shader = nullptr;
  plconfig.SetTargetFormats(REPLACEMENT_TEXTURE_FORMAT);

  // Most flags don't matter here.
  const GPUDevice::Features features = g_gpu_device->GetFeatures();
  GPU_HW_ShaderGen shadergen(g_gpu_device->GetRenderAPI(), 1, 1, false, false, false, false, false,
                             features.dual_source_blend, features.framebuffer_fetch, false);
  std::unique_ptr<GPUShader> fullscreen_quad_vertex_shader = g_gpu_device->CreateShader(
    GPUShaderStage::Vertex, shadergen.GetLanguage(), shadergen.GenerateScreenQuadVertexShader());
  if (!fullscreen_quad_vertex_shader)
    return false;

  plconfig.vertex_shader = fullscreen_quad_vertex_shader.get();

  std::unique_ptr<GPUShader> fs = g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(),
                                                             shadergen.GenerateCopyFragmentShader());
  if (!fs)
    return false;
  plconfig.fragment_shader = fs.get();
  if (!(s_replacement_init_pipeline = g_gpu_device->CreatePipeline(plconfig)))
    return false;

  g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(),
                             shadergen.GenerateReplacementMergeFragmentShader(false));
  if (!fs)
    return false;
  plconfig.fragment_shader = fs.get();
  if (!(s_replacement_draw_pipeline = g_gpu_device->CreatePipeline(plconfig)))
    return false;

  fs = g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(),
                                  shadergen.GenerateReplacementMergeFragmentShader(true));
  if (!fs)
    return false;
  plconfig.blend = GPUPipeline::BlendState::GetNoBlendingState();
  plconfig.fragment_shader = fs.get();
  if (!(s_replacement_semitransparent_draw_pipeline = g_gpu_device->CreatePipeline(plconfig)))
    return false;

  return true;
}

void GPUTextureCache::DestroyPipelines()
{
  s_replacement_init_pipeline.reset();
  s_replacement_draw_pipeline.reset();
  s_replacement_semitransparent_draw_pipeline.reset();
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
      const GSVector4i intersection = page.draw_rect.rintersect(rect);
      if (intersection.eq(page.draw_rect))
      {
        GL_INS_FMT("Clearing page {} draw rect due to write cover", pn);
        page.is_drawn = false;
        page.draw_rect = INVALID_RECT;
      }
      else if (!intersection.rempty())
      {
        // I hate this. It's a hack for FF8, where it copies the framebuffer behind the HUD below the framebuffer,
        // and copies it back to redraw the UI over it. If we toss the draw on any intersection, we lose the copied
        // portion, since local memory is stale.
        GSVector4i new_draw_rect = page.draw_rect;
        if (((static_cast<u32>(intersection.width()) * 100) / static_cast<u32>(page.draw_rect.width())) >= 90)
          new_draw_rect.y = intersection.w;
        else if (((static_cast<u32>(intersection.height()) * 100) / static_cast<u32>(page.draw_rect.height())) >= 90)
          new_draw_rect.x = intersection.z;
        if (new_draw_rect.rempty())
        {
          GL_INS_FMT("Clearing page {} draw rect due to write overlap", pn);
          page.is_drawn = false;
          page.draw_rect = INVALID_RECT;
        }
        else
        {
          GL_INS_FMT("Change page {} draw rect from {} to {} due to write overlap", pn, page.draw_rect, new_draw_rect);
          page.draw_rect = new_draw_rect;
        }
      }
    }

    for (TListNode<VRAMWrite>* n = page.writes.head; n;)
    {
      VRAMWrite* it = n->ref;
      n = n->next;

      const GSVector4i intersection = it->active_rect.rintersect(rect);
      if (!intersection.rempty())
      {
        if (it->num_splits < TextureReplacements::GetConfig().max_vram_write_splits &&
            !it->active_rect.eq(intersection))
        {
          SplitVRAMWrite(it, intersection);
        }
        else
        {
          RemoveVRAMWrite(it);
        }
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

ALWAYS_INLINE_RELEASE static const u16* VRAMPalettePointer(GPUTexturePaletteReg palette)
{
  return &g_vram[VRAM_WIDTH * palette.GetYBase() + palette.GetXBase()];
}

// TODO: Vectorize these with gather.
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

void GPUTextureCache::DecodeTexture(GPUTextureMode mode, const u16* page_ptr, const u16* palette, u32* dest,
                                    u32 dest_stride, u32 width, u32 height)
{
  switch (mode)
  {
    case GPUTextureMode::Palette4Bit:
      DecodeTexture4(page_ptr, palette, width, height, dest, dest_stride);
      break;
    case GPUTextureMode::Palette8Bit:
      DecodeTexture8(page_ptr, palette, width, height, dest, dest_stride);
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
  const u16* palette_ptr = TextureModeHasPalette(mode) ? VRAMPalettePointer(palette) : nullptr;
  DecodeTexture(mode, page_ptr, palette_ptr, tex_map, tex_stride, TEXTURE_PAGE_WIDTH, TEXTURE_PAGE_HEIGHT);

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

const GPUTextureCache::Source* GPUTextureCache::LookupSource(SourceKey key, const GSVector4i rect,
                                                             PaletteRecordFlags flags)
{
  GL_SCOPE_FMT("TC: Lookup source {}", SourceKeyToString(key));

  TList<Source>& list = s_pages[key.page].sources;
  for (TListNode<Source>* n = list.head; n; n = n->next)
  {
    if (n->ref->key == key)
    {
      GL_INS("TC: Source hit");
      ListMoveToFront(&list, n);
      return ReturnSource(n->ref, rect, flags);
    }
  }

  return ReturnSource(CreateSource(key), rect, flags);
}

const GPUTextureCache::Source* GPUTextureCache::ReturnSource(Source* source, const GSVector4i uv_rect,
                                                             PaletteRecordFlags flags)
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
  {
    source->active_uv_rect = source->active_uv_rect.runion(uv_rect);
    source->palette_record_flags |= flags;
  }

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
  LoopRectPagesWithEarlyExit(rect, [&rect, &drawn](u32 pn) {
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
  {
    const u32 shift = ((key.mode < GPUTextureMode::Direct16Bit) ? (2 - static_cast<u8>(key.mode)) : 0);
    const GSVector4i vram_rect = rect.add32(GSVector4i(0, 0, (1 << shift) - 1, 0)).srl32(shift).blend32<0xa>(rect);
    for (u32 offset = 0; offset < TexturePageCountForMode(key.mode); offset++)
    {
      const u32 wrapped_page = ((key.page + offset) & VRAM_PAGE_X_MASK) + (key.page & VRAM_PAGE_Y_MASK);
      const GSVector4i page_rect = vram_rect.sub32(GSVector4i(offset * 64, 0, offset * 64, 0));
      if (IsPageDrawn(wrapped_page, page_rect))
      {
        GL_INS_FMT("UV rect {} intersects page [{}] dirty rect {}, disabling TC", rect, wrapped_page,
                   s_pages[wrapped_page].draw_rect);
      }
    }
  }
#endif

  switch (key.mode)
  {
    case GPUTextureMode::Palette4Bit:
    {
      const GSVector4i vram_rect = rect.add32(GSVector4i::cxpr(0, 0, 3, 0)).srl32<2>().blend32<0xa>(rect);
      return IsPageDrawn(key.page, vram_rect);
    }

    case GPUTextureMode::Palette8Bit:
    {
      // 2 P4 pages per P8 page.
      const u32 yoffs = (key.page & VRAM_PAGE_Y_MASK);
      const GSVector4i vram_rect = rect.add32(GSVector4i::cxpr(0, 0, 1, 0)).srl32<1>().blend32<0xa>(rect);
      return (IsPageDrawn(key.page, vram_rect) || IsPageDrawn(((key.page + 1) & VRAM_PAGE_X_MASK) + yoffs,
                                                              vram_rect.sub32(GSVector4i::cxpr(64, 0, 64, 0))));
    }

    case GPUTextureMode::Direct16Bit:
    case GPUTextureMode::Reserved_Direct16Bit:
    {
      // 4 P4 pages per C16 page.
      const u32 yoffs = (key.page & VRAM_PAGE_Y_MASK);
      return (IsPageDrawn(key.page, rect) ||
              IsPageDrawn(((key.page + 1) & VRAM_PAGE_X_MASK) + yoffs, rect.sub32(GSVector4i::cxpr(64, 0, 64, 0))) ||
              IsPageDrawn(((key.page + 2) & VRAM_PAGE_X_MASK) + yoffs, rect.sub32(GSVector4i::cxpr(128, 0, 128, 0))) ||
              IsPageDrawn(((key.page + 3) & VRAM_PAGE_X_MASK) + yoffs, rect.sub32(GSVector4i::cxpr(192, 0, 192, 0))));
    }

      DefaultCaseIsUnreachable()
  }
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

    GL_INS_FMT("Invalidate source {} in page {} due to overlapping with {}", SourceToString(src), pn, rc);
    DestroySource(src);
  }
}

void GPUTextureCache::DestroySource(Source* src)
{
  GL_INS_FMT("Invalidate source {}", SourceToString(src));

  if (g_settings.texture_replacements.dump_textures && !src->active_uv_rect.eq(INVALID_RECT))
  {
    if (!TextureReplacements::GetConfig().dump_texture_pages)
    {
      // Find VRAM writes that overlap with this source
      LoopRectPages(src->active_uv_rect, [src](const u32 pn) {
        PageEntry& pg = s_pages[pn];
        ListIterate(pg.writes, [src](VRAMWrite* vw) {
          UpdateVRAMWriteSources(vw, src->key, src->active_uv_rect, src->palette_record_flags);
        });
        return true;
      });
    }
    else
    {
      // Dump active area from page
      // TODO: reduce palette bounds
      TextureReplacements::DumpTexture(TextureReplacements::ReplacementType::TextureFromPage, src->texture_rect,
                                       src->key.mode, src->texture_hash, src->palette_hash, 0,
                                       src->key.HasPalette() ? (GetPaletteWidth(src->key.mode) - 1) : 0,
                                       src->key.HasPalette() ? VRAMPalettePointer(src->key.palette) : nullptr,
                                       src->active_uv_rect, src->palette_record_flags);
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
  hcentry->last_used_frame = System::GetFrameNumber();

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
    src->palette_rect = GetPaletteRect(key.palette, key.mode, true);
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

std::pair<u32, u32> GPUTextureCache::ReducePaletteBounds(const GSVector4i rect, GPUTextureMode mode,
                                                         GPUTexturePaletteReg palette)
{
  DebugAssert(TextureModeHasPalette(mode));
  u32 pal_min = GetPaletteWidth(mode) - 1;
  u32 pal_max = 0;

  const u32 rect_width = rect.width();
  const u32 rect_height = rect.height();

  if (mode == GPUTextureMode::Palette4Bit)
  {
    for (u32 y = 0; y < rect_height; y++)
    {
      const u16* ptr = &g_vram[rect.y * VRAM_WIDTH + rect.x];
      for (u32 x = 0; x < rect_width; x++)
      {
        const u16 val = *(ptr++);
        const u32 p0 = val & 0xf;
        const u32 p1 = (val >> 4) & 0xf;
        const u32 p2 = (val >> 8) & 0xf;
        const u32 p3 = (val >> 12) & 0xf;
        pal_min = std::min(pal_min, std::min(p0, std::min(p1, std::min(p2, p3))));
        pal_max = std::max(pal_max, std::max(p0, std::max(p1, std::max(p2, p3))));
      }
    }
  }
  else // if (mode == GPUTextureMode::Palette8Bit)
  {
    const u32 aligned_width = Common::AlignDownPow2(rect_width, 8);
    const u16* row_ptr = &g_vram[rect.y * VRAM_WIDTH + rect.x];
    for (u32 y = 0; y < rect_height; y++)
    {
      const u16* ptr = reinterpret_cast<const u16*>(row_ptr);
      row_ptr += VRAM_WIDTH;

      if (aligned_width > 0) [[likely]]
      {
        GSVector4i min = GSVector4i::load<false>(ptr);
        GSVector4i max = min;
        ptr += 8;

        for (u32 x = 8; x < aligned_width; x += 8)
        {
          const GSVector4i v = GSVector4i::load<false>(ptr);
          ptr += 8;

          min = min.min_u8(v);
          max = max.max_u8(v);
        }

        pal_min = std::min<u32>(pal_min, min.minv_u8());
        pal_max = std::max<u32>(pal_max, max.maxv_u8());
      }

      for (u32 x = aligned_width; x < rect_width; x++)
      {
        const u16 val = *(ptr++);
        const u32 p0 = (val & 0xFF);
        const u32 p1 = (val >> 8);
        pal_min = std::min<u32>(pal_min, std::min(p0, p1));
        pal_max = std::max<u32>(pal_max, std::max(p0, p1));
      }
    }
  }

  // Clamp to VRAM bounds.
  const u32 x_base = palette.GetXBase();
  if ((x_base + pal_max) >= VRAM_WIDTH) [[unlikely]]
  {
    WARNING_LOG("Texture with CLUT at {},{} is outside of VRAM bounds, clamping.", x_base, palette.GetYBase());
    pal_min = std::min(pal_min, VRAM_WIDTH - x_base - 1);
    pal_max = std::min(pal_max, VRAM_WIDTH - x_base - 1);
  }

  return std::make_pair(pal_min, pal_max);
}

void GPUTextureCache::SyncVRAMWritePaletteRecords(VRAMWrite* entry)
{
  // Have to go through any sources that intersect this write, because they may not have been invalidated yet, in which
  // case the active rect also will not have been updated.
  if (IsDumpingVRAMWriteTextures())
  {
    LoopRectPages(entry->active_rect, [entry](const u32 pn) {
      const PageEntry& page = s_pages[pn];
      ListIterate(page.sources, [entry](const Source* src) {
        if (!src->active_uv_rect.eq(INVALID_RECT))
          UpdateVRAMWriteSources(entry, src->key, src->active_uv_rect, src->palette_record_flags);
      });

      return true;
    });
  }
}

void GPUTextureCache::UpdateVRAMWriteSources(VRAMWrite* entry, SourceKey source_key, const GSVector4i global_uv_rect,
                                             PaletteRecordFlags flags)
{
  // convert to VRAM write space
  const GSVector4i write_intersection = entry->active_rect.rintersect(global_uv_rect);
  if (write_intersection.rempty())
    return;

  // Add to the palette tracking list
  auto iter = std::find_if(entry->palette_records.begin(), entry->palette_records.end(),
                           [&source_key](const auto& it) { return (it.key == source_key); });
  if (iter != entry->palette_records.end())
  {
    iter->rect = iter->rect.runion(write_intersection);
    iter->flags |= flags;
  }
  else
  {
    InitializeVRAMWritePaletteRecord(&entry->palette_records.emplace_back(), source_key, write_intersection, flags);
  }
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
    for (const VRAMWrite::PaletteRecord& prec : it->palette_records)
    {
      if (prec.rect.rintersects(splitr))
        it->palette_records.push_back(prec);
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
      for (const VRAMWrite::PaletteRecord& prec : entry->palette_records)
      {
        const auto iter = std::find_if(other_write->palette_records.begin(), other_write->palette_records.end(),
                                       [&prec](const VRAMWrite::PaletteRecord& it) { return it.key == prec.key; });
        if (iter != other_write->palette_records.end())
          iter->rect = iter->rect.runion(prec.rect);
        else
          other_write->palette_records.push_back(prec);
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
  if (g_settings.texture_replacements.dump_textures && !TextureReplacements::GetConfig().dump_texture_pages)
  {
    for (const VRAMWrite::PaletteRecord& prec : entry->palette_records)
    {
      if (prec.key.mode == GPUTextureMode::Direct16Bit && !TextureReplacements::GetConfig().dump_c16_textures)
        continue;

      HashType pal_hash =
        (prec.key.mode < GPUTextureMode::Direct16Bit) ? HashPalette(prec.key.palette, prec.key.mode) : 0;

      // If it's 8-bit, try reducing the range of the palette.
      u32 pal_min = 0, pal_max = prec.key.HasPalette() ? (GetPaletteWidth(prec.key.mode) - 1) : 0;
      if (prec.key.HasPalette() && TextureReplacements::GetConfig().reduce_palette_range)
      {
        std::tie(pal_min, pal_max) = ReducePaletteBounds(prec.rect, prec.key.mode, prec.key.palette);
        pal_hash = HashPartialPalette(prec.palette, pal_min, pal_max);
      }

      TextureReplacements::DumpTexture(TextureReplacements::ReplacementType::TextureFromVRAMWrite, entry->write_rect,
                                       prec.key.mode, entry->hash, pal_hash, pal_min, pal_max, prec.palette, prec.rect,
                                       prec.flags);
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
  const u32 x_base = palette.GetXBase();
  const u16* base = VRAMPalettePointer(palette);

  switch (mode)
  {
    case GPUTextureMode::Palette4Bit:
      return XXH3_64bits(base, sizeof(u16) * 16);

    case GPUTextureMode::Palette8Bit:
    {
      // If the palette wraps around, chances are we aren't using those indices.
      // Games that do this: Metal Gear Solid.
      if ((x_base + 256) > VRAM_WIDTH) [[unlikely]]
        return XXH3_64bits(base, sizeof(u16) * (VRAM_WIDTH - x_base));
      else
        return XXH3_64bits(base, sizeof(u16) * 256);
    }

      DefaultCaseIsUnreachable()
  }
}

GPUTextureCache::HashType GPUTextureCache::HashPartialPalette(GPUTexturePaletteReg palette, GPUTextureMode mode,
                                                              u32 min, u32 max)
{
  DebugAssert((palette.GetXBase() + max + 1) <= VRAM_WIDTH);
  return HashPartialPalette(VRAMPalettePointer(palette), min, max);
}

GPUTextureCache::HashType GPUTextureCache::HashPartialPalette(const u16* palette, u32 min, u32 max)
{
  const u32 size = max - min + 1;
  return XXH3_64bits(palette, sizeof(u16) * size);
}

GPUTextureCache::HashType GPUTextureCache::HashRect(const GSVector4i rc)
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

void GPUTextureCache::InitializeVRAMWritePaletteRecord(VRAMWrite::PaletteRecord* record, SourceKey source_key,
                                                       const GSVector4i rect, PaletteRecordFlags flags)
{
  record->rect = rect;
  record->key = source_key;
  record->flags = flags;

  switch (source_key.mode)
  {
    case GPUTextureMode::Palette4Bit:
    {
      // Always has 16 colours.
      std::memcpy(record->palette, VRAMPalettePointer(source_key.palette), 16 * sizeof(u16));
      record->palette_hash = XXH3_64bits(record->palette, 16 * sizeof(u16));
    }
    break;

    case GPUTextureMode::Palette8Bit:
    {
      // Might have less if we're extending over the edge. Clamp it.
      const u32 pal_width = std::min<u32>(256, VRAM_WIDTH - source_key.palette.GetXBase());
      if (pal_width != 256)
      {
        std::memcpy(record->palette, VRAMPalettePointer(source_key.palette), pal_width * sizeof(u16));
        std::memset(&record->palette[pal_width], 0, sizeof(record->palette) - (pal_width * sizeof(u16)));
        record->palette_hash = XXH3_64bits(record->palette, pal_width * sizeof(u16));
      }
      else
      {
        // Whole thing, 2ez.
        std::memcpy(record->palette, VRAMPalettePointer(source_key.palette), 256 * sizeof(u16));
        record->palette_hash = XXH3_64bits(record->palette, 256 * sizeof(u16));
      }
    }
    break;

    case GPUTextureMode::Direct16Bit:
    {
      // No palette.
      std::memset(record->palette, 0, sizeof(record->palette));
      record->palette_hash = 0;
    }
    break;

      DefaultCaseIsUnreachable()
  }
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
  entry.last_used_frame = System::GetFrameNumber();
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

  s_hash_cache_memory_usage += entry.texture->GetVRAMUsage();

  return &s_hash_cache.emplace(hkey, std::move(entry)).first->second;
}

void GPUTextureCache::RemoveFromHashCache(HashCache::iterator it)
{
  const size_t vram_usage = it->second.texture->GetVRAMUsage();
  DebugAssert(s_hash_cache_memory_usage >= vram_usage);
  s_hash_cache_memory_usage -= vram_usage;
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

  bool might_need_cache_purge =
    (s_hash_cache.size() > MAX_HASH_CACHE_SIZE || s_hash_cache_memory_usage >= s_max_hash_cache_memory_usage);
  size_t pre_purge_hash_cache_memory_usage = s_hash_cache_memory_usage;
  if (might_need_cache_purge)
    s_hash_cache_purge_list.clear();

  const u32 frame_number = System::GetFrameNumber();
  const u32 min_frame_number = ((frame_number > MAX_HASH_CACHE_AGE) ? (frame_number - MAX_HASH_CACHE_AGE) : 0);

  for (auto it = s_hash_cache.begin(); it != s_hash_cache.end();)
  {
    HashCacheEntry& e = it->second;
    if (e.ref_count > 0)
    {
      ++it;
      continue;
    }

    if (e.last_used_frame < min_frame_number)
    {
      RemoveFromHashCache(it++);
      continue;
    }

    // We might free up enough just with "normal" removals above.
    if (might_need_cache_purge)
    {
      might_need_cache_purge =
        (s_hash_cache.size() > MAX_HASH_CACHE_SIZE || s_hash_cache_memory_usage >= s_max_hash_cache_memory_usage);
      if (might_need_cache_purge)
      {
        s_hash_cache_purge_list.emplace_back(it, static_cast<s32>(e.last_used_frame));
        pre_purge_hash_cache_memory_usage -= it->second.texture->GetVRAMUsage();
      }
    }

    ++it;
  }

  // Pushing to a list, sorting, and removing ends up faster than re-iterating the map.
  if (might_need_cache_purge)
  {
    std::sort(s_hash_cache_purge_list.begin(), s_hash_cache_purge_list.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.second < rhs.second; });

    size_t purge_index = 0;
    while (s_hash_cache.size() > MAX_HASH_CACHE_AGE || s_hash_cache_memory_usage >= s_max_hash_cache_memory_usage)
    {
      if (purge_index == s_hash_cache_purge_list.size())
      {
        WARNING_LOG("Cannot find hash cache entries to purge, current hash cache size is {} MB in {} textures.",
                    static_cast<double>(s_hash_cache_memory_usage) / 1048576.0, s_hash_cache.size());
        break;
      }

      RemoveFromHashCache(s_hash_cache_purge_list[purge_index++].first);
    }
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
    TextureReplacements::GetTexturePageTextureReplacements(subimages, tex_hash, pal_hash, key.mode, key.palette);

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

        TextureReplacements::GetVRAMWriteTextureReplacements(subimages, vrw->hash, pal_hash, key.mode, key.palette,
                                                             offset_to_page);
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

  // Clamp to max texture size
  const float max_possible_scale =
    static_cast<float>(g_gpu_device->GetMaxTextureSize()) / static_cast<float>(TEXTURE_PAGE_WIDTH);
  max_scale_x = std::min(max_scale_x, max_possible_scale);
  max_scale_y = std::min(max_scale_y, max_possible_scale);

  const GSVector4 max_scale_v = GSVector4(max_scale_x, max_scale_y).xyxy();
  GPUSampler* filter = TextureReplacements::GetConfig().replacement_scale_linear_filter ?
                         g_gpu_device->GetLinearSampler() :
                         g_gpu_device->GetNearestSampler();

  const u32 new_width = static_cast<u32>(std::ceil(static_cast<float>(TEXTURE_PAGE_WIDTH) * max_scale_x));
  const u32 new_height = static_cast<u32>(std::ceil(static_cast<float>(TEXTURE_PAGE_HEIGHT) * max_scale_y));
  if (!s_replacement_texture_render_target || s_replacement_texture_render_target->GetWidth() < new_width ||
      s_replacement_texture_render_target->GetHeight() < new_height)
  {
    // NOTE: Not recycled, it's unlikely to be reused.
    s_replacement_texture_render_target.reset();
    if (!(s_replacement_texture_render_target = g_gpu_device->CreateTexture(
            new_width, new_height, 1, 1, 1, GPUTexture::Type::RenderTarget, REPLACEMENT_TEXTURE_FORMAT)))
    {
      ERROR_LOG("Failed to create {}x{} render target.", new_width, new_height);
      return;
    }
  }

  // Grab the actual texture beforehand, in case we OOM.
  std::unique_ptr<GPUTexture> replacement_tex =
    g_gpu_device->FetchTexture(new_width, new_height, 1, 1, 1, GPUTexture::Type::Texture, REPLACEMENT_TEXTURE_FORMAT);
  if (!replacement_tex)
  {
    ERROR_LOG("Failed to create {}x{} texture.", new_width, new_height);
    return;
  }

  // TODO: This is AWFUL. Need a better way.
  // Linear filtering is also wrong, it should do hard edges for 0000 pixels.
  // We could just copy this from the original image...
  static constexpr const float u_src_rect[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  g_gpu_device->InvalidateRenderTarget(s_replacement_texture_render_target.get());
  g_gpu_device->SetRenderTarget(s_replacement_texture_render_target.get());
  g_gpu_device->SetViewportAndScissor(0, 0, new_width, new_height);
  g_gpu_device->SetPipeline(s_replacement_init_pipeline.get());
  g_gpu_device->PushUniformBuffer(u_src_rect, sizeof(u_src_rect));
  g_gpu_device->SetTextureSampler(0, entry->texture.get(), filter);
  g_gpu_device->Draw(3, 0);

  for (const TextureReplacements::ReplacementSubImage& si : subimages)
  {
    const auto temp_texture = g_gpu_device->FetchAutoRecycleTexture(
      si.image.GetWidth(), si.image.GetHeight(), 1, 1, 1, GPUTexture::Type::Texture, REPLACEMENT_TEXTURE_FORMAT,
      si.image.GetPixels(), si.image.GetPitch());
    if (!temp_texture)
      continue;

    const GSVector4i dst_rect = GSVector4i(GSVector4(si.dst_rect) * max_scale_v);
    g_gpu_device->SetViewportAndScissor(dst_rect);
    g_gpu_device->SetTextureSampler(0, temp_texture.get(), filter);
    g_gpu_device->SetPipeline(si.invert_alpha ? s_replacement_semitransparent_draw_pipeline.get() :
                                                s_replacement_draw_pipeline.get());
    g_gpu_device->Draw(3, 0);
  }

  g_gpu_device->CopyTextureRegion(replacement_tex.get(), 0, 0, 0, 0, s_replacement_texture_render_target.get(), 0, 0, 0,
                                  0, new_width, new_height);
  g_gpu_device->RecycleTexture(std::move(entry->texture));
  entry->texture = std::move(replacement_tex);

  g_gpu->RestoreDeviceContext();
}