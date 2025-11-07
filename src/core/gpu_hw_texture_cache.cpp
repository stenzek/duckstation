// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gpu_hw_texture_cache.h"
#include "fullscreenui_widgets.h"
#include "game_database.h"
#include "gpu_hw.h"
#include "gpu_hw_shadergen.h"
#include "gpu_sw_rasterizer.h"
#include "gpu_thread.h"
#include "host.h"
#include "imgui_overlays.h"
#include "settings.h"
#include "system.h"

#include "util/gpu_device.h"
#include "util/imgui_manager.h"
#include "util/state_wrapper.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/gsvector_formatter.h"
#include "common/heterogeneous_containers.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"
#include "common/timer.h"

#include "IconsEmoji.h"
#include "IconsFontAwesome6.h"

#ifndef XXH_STATIC_LINKING_ONLY
#define XXH_STATIC_LINKING_ONLY
#endif
#include "xxhash.h"
#ifdef CPU_ARCH_SSE
#include "xxh_x86dispatch.h"
#endif

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_set>

LOG_CHANNEL(GPU_HW);

#include "common/ryml_helpers.h"

// #define ALWAYS_TRACK_VRAM_WRITES 1

namespace GPUTextureCache {
static constexpr u32 MAX_CLUT_SIZE = 256;
static constexpr u32 NUM_PAGE_DRAW_RECTS = 4;
static constexpr const GSVector4i& INVALID_RECT = GPU_HW::INVALID_RECT;
static constexpr const GPUTexture::Format REPLACEMENT_TEXTURE_FORMAT = GPUTexture::Format::RGBA8;
static constexpr const char LOCAL_CONFIG_FILENAME[] = "config.yaml";

static constexpr u32 STATE_PALETTE_RECORD_SIZE =
  sizeof(GSVector4i) + sizeof(SourceKey) + sizeof(PaletteRecordFlags) + sizeof(HashType) + sizeof(u16) * MAX_CLUT_SIZE;

// Has to be public because it's referenced in Source.
struct HashCacheEntry
{
  std::unique_ptr<GPUTexture> texture;
  u32 ref_count;
  u32 last_used_frame;
  TList<Source> sources;
};

namespace {
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
  u32 num_draw_rects;
  GSVector4i total_draw_rect; // NOTE: In global VRAM space.
  std::array<GSVector4i, NUM_PAGE_DRAW_RECTS> draw_rects;
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

enum class TextureReplacementType : u8
{
  VRAMReplacement,
  TextureFromVRAMWrite,
  TextureFromPage,
};

struct TextureReplacementSubImage
{
  GSVector4i dst_rect;
  GSVector4i src_rect;
  GPUTexture* texture;
  float scale_x;
  float scale_y;
  bool invert_alpha;
};

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
  TextureReplacementType type;
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

  ALWAYS_INLINE GSVector2i GetSizeVec() const { return GSVector2i(width, height); }
  ALWAYS_INLINE GSVector2i GetOffsetVec() const { return GSVector2i(offset_x, offset_y); }
  ALWAYS_INLINE GSVector4i GetDestRect() const
  {
    return GSVector4i(GSVector4i(GetOffsetVec()).xyxy().add32(GSVector4i(GetSizeVec()).zwxy()));
  }
};

struct DumpedTextureKey
{
  HashType tex_hash;
  HashType pal_hash;
  u16 offset_x, offset_y;
  u16 width, height;
  TextureReplacementType type;
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

  static DumpedTextureKey FromName(const TextureReplacementName& name)
  {
    return DumpedTextureKey{name.src_hash, name.pal_hash,     name.offset_x,
                            name.offset_y, name.width,        name.height,
                            name.type,     name.texture_mode, {}};
  }
};
struct DumpedTextureKeyHash
{
  size_t operator()(const DumpedTextureKey& k) const;
};
} // namespace

using HashCache = std::unordered_map<HashCacheKey, HashCacheEntry, HashCacheKeyHash>;
using ReplacementImageCache = PreferUnorderedStringMap<TextureReplacementImage>;
using GPUReplacementImageCache = PreferUnorderedStringMap<std::pair<std::unique_ptr<GPUTexture>, u32>>;

using VRAMReplacementMap = std::unordered_map<VRAMReplacementName, std::string, VRAMReplacementNameHash>;
using TextureReplacementMap =
  std::unordered_multimap<TextureReplacementIndex, std::pair<TextureReplacementName, std::string>,
                          TextureReplacementIndexHash>;

static bool ShouldTrackVRAMWrites();
static bool IsDumpingVRAMWriteTextures();
static void UpdateVRAMTrackingState();

static void SetHashCacheTextureFormat();
static bool CompilePipelines(Error* error);
static void DestroyPipelines();

static std::unique_ptr<GPUTexture> FetchTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                                GPUTexture::Type type, GPUTexture::Format format,
                                                GPUTexture::Flags flags, const void* data = nullptr,
                                                u32 data_stride = 0);
static const Source* ReturnSource(Source* source, const GSVector4i uv_rect, PaletteRecordFlags flags);
static Source* CreateSource(SourceKey key);

static HashCacheKey GetHashCacheKey(SourceKey key, HashType tex_hash, HashType pal_hash);
static HashCacheEntry* LookupHashCache(SourceKey key, HashType tex_hash, HashType pal_hash);
static void ApplyTextureReplacements(SourceKey key, HashType tex_hash, HashType pal_hash, HashCacheEntry* entry);
static void RemoveFromHashCache(HashCacheEntry* entry, SourceKey key, HashType tex_hash, HashType pal_hash);
static void RemoveFromHashCache(HashCache::iterator it);
static void ClearHashCache();

static bool IsPageDrawn(u32 page_index, const GSVector4i rect);
static void InvalidatePageSources(u32 pn);
static void InvalidatePageSources(u32 pn, const GSVector4i rc, bool remove_from_hash_cache = false);
static void InvalidateSources();
static void DestroySource(Source* src, bool remove_from_hash_cache = false);

static HashType HashPage(u8 page, GPUTextureMode mode);
static HashType HashPalette(GPUTexturePaletteReg palette, GPUTextureMode mode);
static HashType HashPartialPalette(const u16* palette, u32 min, u32 max);
static HashType HashPartialPalette(GPUTexturePaletteReg palette, GPUTextureMode mode, u32 min, u32 max);
static HashType HashRect(const GSVector4i rc);

static std::pair<u32, u32> ReducePaletteBounds(const GSVector4i rect, GPUTextureMode mode,
                                               GPUTexturePaletteReg palette);
static void SyncVRAMWritePaletteRecords(VRAMWrite* entry);
static void MergeAdjacentVRAMWritePaletteRecords(VRAMWrite* entry);
static void InitializeVRAMWritePaletteRecord(VRAMWrite::PaletteRecord* record, SourceKey source_key,
                                             const GSVector4i rect, PaletteRecordFlags flags);
static void UpdateVRAMWriteSources(VRAMWrite* entry, SourceKey source_key, const GSVector4i global_uv_rect,
                                   PaletteRecordFlags flags);
static void SplitVRAMWrite(VRAMWrite* entry, const GSVector4i written_rect);
static bool TryMergeVRAMWrite(VRAMWrite* entry, const GSVector4i written_rect);
static void RemoveVRAMWrite(VRAMWrite* entry);
static void DumpTexturesFromVRAMWrite(VRAMWrite* entry);
static void DumpTextureFromPage(const Source* src);

static void DecodeTexture(GPUTextureMode mode, const u16* page_ptr, const u16* palette, u8* dest, u32 dest_stride,
                          u32 width, u32 height, GPUTexture::Format dest_format);
template<GPUTexture::Format dest_format>
static void DecodeTexture4(const u16* page, const u16* palette, u32 width, u32 height, u8* dest, u32 dest_stride);
template<GPUTexture::Format dest_format>
static void DecodeTexture8(const u16* page, const u16* palette, u32 width, u32 height, u8* dest, u32 dest_stride);
template<GPUTexture::Format dest_format>
static void DecodeTexture16(const u16* page, u32 width, u32 height, u8* dest, u32 dest_stride);
static void DecodeTexture(u8 page, GPUTexturePaletteReg palette, GPUTextureMode mode, GPUTexture* texture);

static std::optional<TextureReplacementType> GetTextureReplacementTypeFromFileTitle(const std::string_view file_title);
static bool HasValidReplacementExtension(const std::string_view path);

static bool EnsureGameDirectoryExists();
static std::string GetTextureReplacementDirectory();
static std::string GetTextureDumpDirectory();

static VRAMReplacementName GetVRAMWriteHash(u32 width, u32 height, const void* pixels);
static std::string GetVRAMWriteDumpPath(const VRAMReplacementName& name);

static bool IsMatchingReplacementPalette(HashType full_palette_hash, GPUTextureMode mode, GPUTexturePaletteReg palette,
                                         const TextureReplacementName& name);
static bool LoadLocalConfiguration(bool load_vram_write_replacement_aliases, bool load_texture_replacement_aliases);

static void FindTextureReplacements(bool load_vram_write_replacements, bool load_texture_replacements,
                                    bool prefill_dumped_texture_list, bool prefill_dumped_vram_list);
static void LoadTextureReplacementAliases(const ryml::ConstNodeRef& root, bool load_vram_write_replacement_aliases,
                                          bool load_texture_replacement_aliases);

static const TextureReplacementImage* GetTextureReplacementImage(const std::string& path);
static GPUTexture* GetTextureReplacementGPUImage(const std::string& path);
static void CompactTextureReplacementGPUImages();
static void PreloadReplacementTextures();
static void PurgeUnreferencedTexturesFromCache();

static void DumpTexture(TextureReplacementType type, u32 offset_x, u32 offset_y, u32 src_width, u32 src_height,
                        GPUTextureMode mode, HashType src_hash, HashType pal_hash, u32 pal_min, u32 pal_max,
                        const u16* palette, const GSVector4i rect, PaletteRecordFlags flags);

static bool HasVRAMWriteTextureReplacements();
static void GetVRAMWriteTextureReplacements(std::vector<TextureReplacementSubImage>& replacements,
                                            HashType vram_write_hash, HashType palette_hash, GPUTextureMode mode,
                                            GPUTexturePaletteReg palette, const GSVector2i& offset_to_page);

static bool HasTexturePageTextureReplacements();
static void GetTexturePageTextureReplacements(std::vector<TextureReplacementSubImage>& replacements,
                                              u32 start_page_number, HashType page_hash, HashType palette_hash,
                                              GPUTextureMode mode, GPUTexturePaletteReg palette);

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
  for (const GPUTextureCache::TListNode<T>* n = list.head; n;)
  {
    const GPUTextureCache::TListNode<T>* tn = n;
    n = n->next;
    f(tn->ref);
  }
}

template<typename T, typename F>
ALWAYS_INLINE_RELEASE static bool ListIterateWithEarlyExit(const TList<T>& list, const F& f)
{
  for (const GPUTextureCache::TListNode<T>* n = list.head; n; n = n->next)
  {
    if (!f(n->ref))
      return false;
  }

  return true;
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
ALWAYS_INLINE_RELEASE static bool LoopRectPagesWithEarlyExit(u32 left, u32 top, u32 right, u32 bottom, const F& f)
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
        return false;
    }

    page_number += VRAM_PAGES_WIDE;
  }

  return true;
}

template<typename F>
ALWAYS_INLINE_RELEASE static void LoopRectPages(const GSVector4i& rc, const F& f)
{
  LoopRectPages(rc.left, rc.top, rc.right, rc.bottom, f);
}

template<typename F>
ALWAYS_INLINE_RELEASE static bool LoopRectPagesWithEarlyExit(const GSVector4i& rc, const F& f)
{
  return LoopRectPagesWithEarlyExit(rc.left, rc.top, rc.right, rc.bottom, f);
}

template<typename F>
ALWAYS_INLINE_RELEASE static void LoopXWrappedPages(u32 page, u32 num_pages, const F& f)
{
  for (u32 i = 0; i < num_pages; i++)
    f((page & VRAM_PAGE_Y_MASK) | ((page + i) & VRAM_PAGE_X_MASK));
}

ALWAYS_INLINE static void DoStateVector(StateWrapper& sw, GSVector4i* vec)
{
  sw.DoBytes(vec->S32, sizeof(vec->S32));
}

ALWAYS_INLINE static float RectDistance(const GSVector4i& lhs, const GSVector4i& rhs)
{
  const GSVector4 flhs(lhs);
  const GSVector4 frhs(rhs);
  const GSVector2 clhs = flhs.xy() + ((flhs.zw() - flhs.xy()) * 0.5f);
  const GSVector2 crhs = frhs.xy() + ((frhs.zw() - flhs.xy()) * 0.5f);
  return clhs.dot(crhs);
}

namespace {
struct GPUTextureCacheState
{
  Settings::TextureReplacementSettings::Configuration config;
  size_t hash_cache_memory_usage = 0;
  VRAMWrite* last_vram_write = nullptr;
  bool track_vram_writes = false;

  GPUTexture::Format hash_cache_texture_format = GPUTexture::Format::Unknown;
  HashCache hash_cache;
  GPU_HW* hw_backend = nullptr; // TODO:FIXME: remove me

  /// List of candidates for purging when the hash cache gets too large.
  std::vector<std::pair<HashCache::iterator, s32>> hash_cache_purge_list;

  /// List of VRAM writes collected when saving state.
  std::vector<VRAMWrite*> temp_vram_write_list;

  std::unique_ptr<GPUTexture> replacement_texture_render_target;
  std::unique_ptr<GPUPipeline> replacement_upscale_pipeline;              // copies alpha as-is with optional bilinear
  std::unique_ptr<GPUPipeline> replacement_draw_pipeline;                 // copies alpha as-is
  std::unique_ptr<GPUPipeline> replacement_semitransparent_draw_pipeline; // inverts alpha (i.e. semitransparent)

  VRAMReplacementMap vram_replacements;

  // TODO: Combine these into one map?
  TextureReplacementMap vram_write_texture_replacements;
  TextureReplacementMap texture_page_texture_replacements;

  // TODO: Check the size, purge some when it gets too large.
  ReplacementImageCache replacement_image_cache;
  GPUReplacementImageCache gpu_replacement_image_cache;
  size_t gpu_replacement_image_cache_vram_usage = 0;
  std::vector<std::pair<GPUReplacementImageCache::iterator, s32>> gpu_replacement_image_cache_purge_list;

  std::unordered_set<VRAMReplacementName, VRAMReplacementNameHash> dumped_vram_writes;
  std::unordered_set<DumpedTextureKey, DumpedTextureKeyHash> dumped_textures;

  ALIGN_TO_CACHE_LINE std::array<PageEntry, NUM_VRAM_PAGES> pages = {};
};
} // namespace

ALIGN_TO_CACHE_LINE GPUTextureCacheState s_state;

} // namespace GPUTextureCache

bool GPUTextureCache::ShouldTrackVRAMWrites()
{
  if (!g_gpu_settings.gpu_texture_cache)
    return false;

  if (g_gpu_settings.texture_replacements.always_track_uploads)
    return true;

#ifdef ALWAYS_TRACK_VRAM_WRITES
  return true;
#else
  return (IsDumpingVRAMWriteTextures() ||
          (g_gpu_settings.texture_replacements.enable_texture_replacements && HasVRAMWriteTextureReplacements()));
#endif
}

bool GPUTextureCache::IsDumpingVRAMWriteTextures()
{
  return (g_gpu_settings.texture_replacements.dump_textures && !s_state.config.dump_texture_pages);
}

bool GPUTextureCache::Initialize(GPU_HW* backend, Error* error)
{
  s_state.hw_backend = backend;

  SetHashCacheTextureFormat();

  // note: safe because the CPU thread is waiting for the GPU thread to finish initializing
  ReloadTextureReplacements(System::GetState() == System::State::Starting, false);

  UpdateVRAMTrackingState();

  if (!CompilePipelines(error))
    return false;

  return true;
}

bool GPUTextureCache::UpdateSettings(bool use_texture_cache, const GPUSettings& old_settings, Error* error)
{
  const bool prev_tracking_state = s_state.track_vram_writes;

  // Reload textures if configuration changes.
  const bool old_replacement_scale_linear_filter = s_state.config.replacement_scale_linear_filter;
  if (LoadLocalConfiguration(false, false) ||
      g_gpu_settings.texture_replacements.enable_texture_replacements !=
        old_settings.texture_replacements.enable_texture_replacements ||
      g_gpu_settings.texture_replacements.enable_vram_write_replacements !=
        old_settings.texture_replacements.enable_vram_write_replacements ||
      g_gpu_settings.texture_replacements.dump_textures != old_settings.texture_replacements.dump_textures ||
      g_gpu_settings.texture_replacements.dump_vram_writes != old_settings.texture_replacements.dump_vram_writes ||
      (g_gpu_settings.texture_replacements.dump_replaced_textures !=
         old_settings.texture_replacements.dump_replaced_textures &&
       (g_gpu_settings.texture_replacements.dump_textures || g_gpu_settings.texture_replacements.dump_vram_writes)))
  {
    if (use_texture_cache)
    {
      if (g_gpu_settings.texture_replacements.enable_texture_replacements !=
            old_settings.texture_replacements.enable_texture_replacements ||
          s_state.config.replacement_scale_linear_filter != old_replacement_scale_linear_filter)
      {
        DestroyPipelines();
        if (!CompilePipelines(error)) [[unlikely]]
        {
          Error::AddPrefix(error, "Failed to compile pipelines on TC replacement settings change: ");
          return false;
        }
      }
    }

    ReloadTextureReplacements(false, false);
  }

  UpdateVRAMTrackingState();

  if (s_state.track_vram_writes != prev_tracking_state)
    Invalidate();

  return true;
}

bool GPUTextureCache::GetStateSize(StateWrapper& sw, u32* size)
{
  if (sw.GetVersion() < 73)
  {
    *size = 0;
    return true;
  }

  const size_t start = sw.GetPosition();
  if (!sw.DoMarker("GPUTextureCache")) [[unlikely]]
    return false;

  u32 num_vram_writes = 0;
  sw.Do(&num_vram_writes);

  for (u32 i = 0; i < num_vram_writes; i++)
  {
    sw.SkipBytes(sizeof(GSVector4i) * 2 + sizeof(HashType));

    u32 num_palette_records = 0;
    sw.Do(&num_palette_records);
    sw.SkipBytes(num_palette_records * STATE_PALETTE_RECORD_SIZE);
  }

  if (sw.HasError()) [[unlikely]]
    return false;

  *size = static_cast<u32>(sw.GetPosition() - start);
  return true;
}

bool GPUTextureCache::DoState(StateWrapper& sw, bool skip)
{
  if (sw.GetVersion() < 73)
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

    const bool skip_writes = (skip || !s_state.track_vram_writes);

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
        if (g_gpu_settings.texture_replacements.dump_textures)
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
          ListAppend(&s_state.pages[pn].writes, vrw, &vrw->page_refs[vrw->num_page_refs++]);
          return true;
        });
      }
    }
  }
  else
  {
    s_state.temp_vram_write_list.clear();

    if (!skip && s_state.track_vram_writes)
    {
      for (PageEntry& page : s_state.pages)
      {
        ListIterate(page.writes, [](VRAMWrite* vrw) {
          if (std::find(s_state.temp_vram_write_list.begin(), s_state.temp_vram_write_list.end(), vrw) !=
              s_state.temp_vram_write_list.end())
          {
            return;
          }

          // try not to lose data... pull it from the sources
          if (g_settings.texture_replacements.dump_textures)
            SyncVRAMWritePaletteRecords(vrw);

          s_state.temp_vram_write_list.push_back(vrw);
        });
      }
    }

    u32 num_vram_writes = static_cast<u32>(s_state.temp_vram_write_list.size());
    sw.Do(&num_vram_writes);
    for (VRAMWrite* vrw : s_state.temp_vram_write_list)
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
  s_state.replacement_texture_render_target.reset();
  s_state.gpu_replacement_image_cache_purge_list = {};
  s_state.hash_cache_purge_list = {};
  s_state.temp_vram_write_list = {};
  s_state.track_vram_writes = false;
  s_state.hw_backend = nullptr;

  for (auto it = s_state.gpu_replacement_image_cache.begin(); it != s_state.gpu_replacement_image_cache.end();)
  {
    g_gpu_device->RecycleTexture(std::move(it->second.first));
    it = s_state.gpu_replacement_image_cache.erase(it);
  }
  s_state.gpu_replacement_image_cache_vram_usage = 0;

  s_state.replacement_image_cache.clear();
  s_state.vram_replacements.clear();
  s_state.vram_write_texture_replacements.clear();
  s_state.texture_page_texture_replacements.clear();
  s_state.dumped_textures.clear();
  s_state.dumped_vram_writes.clear();
}

void GPUTextureCache::SetHashCacheTextureFormat()
{
  // Prefer 16-bit texture formats where possible.
  if (g_gpu_device->SupportsTextureFormat(GPUTexture::Format::RGB5A1))
    s_state.hash_cache_texture_format = GPUTexture::Format::RGB5A1;
  else if (g_gpu_device->SupportsTextureFormat(GPUTexture::Format::A1BGR5))
    s_state.hash_cache_texture_format = GPUTexture::Format::A1BGR5;
  else
    s_state.hash_cache_texture_format = GPUTexture::Format::RGBA8;

  INFO_LOG("Using {} format for hash cache entries.", GPUTexture::GetFormatName(s_state.hash_cache_texture_format));
}

bool GPUTextureCache::CompilePipelines(Error* error)
{
  if (!g_gpu_settings.texture_replacements.enable_texture_replacements)
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
  plconfig.samples = 1;
  plconfig.per_sample_shading = false;
  plconfig.render_pass_flags = GPUPipeline::NoRenderPassFlags;
  plconfig.SetTargetFormats(REPLACEMENT_TEXTURE_FORMAT);

  // Most flags don't matter here.
  const GPUDevice::Features features = g_gpu_device->GetFeatures();
  const GPU_HW_ShaderGen shadergen(g_gpu_device->GetRenderAPI(), features.dual_source_blend,
                                   features.framebuffer_fetch);
  std::unique_ptr<GPUShader> fullscreen_quad_vertex_shader = g_gpu_device->CreateShader(
    GPUShaderStage::Vertex, shadergen.GetLanguage(), shadergen.GenerateScreenQuadVertexShader());
  if (!fullscreen_quad_vertex_shader)
    return false;

  plconfig.vertex_shader = fullscreen_quad_vertex_shader.get();

  std::unique_ptr<GPUShader> fs = g_gpu_device->CreateShader(
    GPUShaderStage::Fragment, shadergen.GetLanguage(),
    shadergen.GenerateReplacementMergeFragmentShader(false, false, s_state.config.replacement_scale_linear_filter));
  if (!fs)
    return false;
  GL_OBJECT_NAME(fs, "Replacement upscale shader");
  plconfig.fragment_shader = fs.get();
  if (!(s_state.replacement_upscale_pipeline = g_gpu_device->CreatePipeline(plconfig)))
    return false;
  GL_OBJECT_NAME(s_state.replacement_upscale_pipeline, "Replacement upscale pipeline");

  fs = g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(),
                                  shadergen.GenerateReplacementMergeFragmentShader(true, false, false));
  if (!fs)
    return false;
  GL_OBJECT_NAME(fs, "Replacement draw shader");
  plconfig.fragment_shader = fs.get();
  if (!(s_state.replacement_draw_pipeline = g_gpu_device->CreatePipeline(plconfig)))
    return false;
  GL_OBJECT_NAME(s_state.replacement_draw_pipeline, "Replacement draw pipeline");

  fs = g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(),
                                  shadergen.GenerateReplacementMergeFragmentShader(true, true, false));
  if (!fs)
    return false;
  GL_OBJECT_NAME(fs, "Replacement semitransparent draw shader");
  plconfig.blend = GPUPipeline::BlendState::GetNoBlendingState();
  plconfig.fragment_shader = fs.get();
  if (!(s_state.replacement_semitransparent_draw_pipeline = g_gpu_device->CreatePipeline(plconfig)))
    return false;
  GL_OBJECT_NAME(s_state.replacement_semitransparent_draw_pipeline, "Replacement semitransparent draw pipeline");

  return true;
}

void GPUTextureCache::DestroyPipelines()
{
  s_state.replacement_upscale_pipeline.reset();
  s_state.replacement_draw_pipeline.reset();
  s_state.replacement_semitransparent_draw_pipeline.reset();
}

void GPUTextureCache::AddDrawnRectangle(const GSVector4i rect, const GSVector4i clip_rect)
{
  // TODO: This might be a bit slow...
  LoopRectPages(rect, [&rect, &clip_rect](u32 pn) {
    PageEntry& page = s_state.pages[pn];

    for (TListNode<VRAMWrite>* n = page.writes.head; n;)
    {
      VRAMWrite* it = n->ref;
      n = n->next;
      if (it->active_rect.rintersects(rect))
        RemoveVRAMWrite(it);
    }

    const GSVector4i rc = rect.rintersect(VRAMPageRect(pn));
    if (page.num_draw_rects > 0)
    {
      u32 candidate = page.num_draw_rects;
      for (u32 i = 0; i < page.num_draw_rects; i++)
      {
        const GSVector4i page_draw_rect = page.draw_rects[i];
        if (page_draw_rect.rcontains(rc))
        {
          // already contained
          return;
        }
        else if (clip_rect.rintersects(page_draw_rect))
        {
          // this one's probably for the draw rect, so use it
          candidate = i;
        }
      }
      if (candidate == NUM_PAGE_DRAW_RECTS)
      {
        // we're out of draw rects.. pick the one that's the closest, and hope for the best
        GL_INS_FMT("Out of draw rects for page {}", pn);
        candidate = 0;
        float closest_dist = RectDistance(rc, page.draw_rects[0]);
        for (u32 i = 1; i < NUM_PAGE_DRAW_RECTS; i++)
        {
          const float dist = RectDistance(rc, page.draw_rects[i]);
          candidate = (dist < closest_dist) ? i : candidate;
          closest_dist = (dist < closest_dist) ? dist : closest_dist;
        }
      }

      if (candidate != page.num_draw_rects)
      {
        const GSVector4i new_draw_rect = page.draw_rects[candidate].runion(rc);
        page.draw_rects[candidate] = new_draw_rect;
        InvalidatePageSources(pn, new_draw_rect);
      }
      else
      {
        DebugAssert(page.num_draw_rects < NUM_PAGE_DRAW_RECTS);
        page.draw_rects[candidate] = rc;
        page.num_draw_rects++;
        InvalidatePageSources(pn, rc);
      }

      page.total_draw_rect = page.total_draw_rect.runion(rc);
      GL_INS_FMT("Page {} drawn rect is now {}", pn, page.total_draw_rect);
    }
    else
    {
      GL_INS_FMT("Page {} drawn rect is now {}", pn, rc);
      page.total_draw_rect = rc;
      page.draw_rects[0] = rc;
      page.num_draw_rects = 1;

      // remove all sources, let them re-lookup if needed
      InvalidatePageSources(pn, rc);
    }
  });
}

void GPUTextureCache::CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height, bool check_mask,
                               bool set_mask, const GSVector4i src_bounds, const GSVector4i dst_bounds)
{
  const bool convert_copies_to_writes = s_state.config.convert_copies_to_writes;

  // first dump out any overlapping writes with the old data
  if (convert_copies_to_writes)
  {
    LoopRectPages(dst_bounds, [&dst_bounds](u32 pn) {
      PageEntry& page = s_state.pages[pn];
      for (TListNode<VRAMWrite>* n = page.writes.head; n; n = n->next)
      {
        VRAMWrite* it = n->ref;
        if (it->active_rect.rintersects(dst_bounds))
        {
          SyncVRAMWritePaletteRecords(it);
          DumpTexturesFromVRAMWrite(it);
        }
      }
    });
  }

  // copy and invalidate
  GPU_SW_Rasterizer::CopyVRAM(src_x, src_y, dst_x, dst_y, width, height, check_mask, set_mask);
  AddWrittenRectangle(dst_bounds, convert_copies_to_writes, true);
}

void GPUTextureCache::WriteVRAM(u32 x, u32 y, u32 width, u32 height, const void* data, bool set_mask, bool check_mask,
                                const GSVector4i bounds)
{
  GPU_SW_Rasterizer::WriteVRAM(x, y, width, height, data, set_mask, check_mask);

  if (!s_state.track_vram_writes)
    return;

  if (s_state.last_vram_write && TryMergeVRAMWrite(s_state.last_vram_write, bounds))
    return;

  VRAMWrite* it = new VRAMWrite();
  it->active_rect = bounds;
  it->write_rect = bounds;
  it->hash = HashRect(bounds);
  it->num_page_refs = 0;
  LoopRectPages(bounds, [it](u32 pn) {
    DebugAssert(it->num_page_refs < MAX_PAGE_REFS_PER_WRITE);
    ListAppend(&s_state.pages[pn].writes, it, &it->page_refs[it->num_page_refs++]);
    return true;
  });

  DEV_LOG("New VRAM write {:016X} at {} touching {} pages", it->hash, bounds, it->num_page_refs);
  s_state.last_vram_write = it;
}

void GPUTextureCache::AddWrittenRectangle(const GSVector4i rect, bool update_vram_writes, bool remove_from_hash_cache)
{
  LoopRectPages(rect, [&rect, &update_vram_writes, &remove_from_hash_cache](u32 pn) {
    PageEntry& page = s_state.pages[pn];
    InvalidatePageSources(pn, rect, remove_from_hash_cache);

    if (page.num_draw_rects > 0)
    {
      const u32 prev_draw_rects = page.num_draw_rects;
      for (u32 i = 0; i < page.num_draw_rects;)
      {
        const GSVector4i page_draw_rect = page.draw_rects[i];
        if (!page_draw_rect.rintersects(rect))
        {
          i++;
          continue;
        }

        GL_INS_FMT("Clearing page {} draw rect {} due to write", pn, page_draw_rect);
        page.num_draw_rects--;
        if (page.num_draw_rects > 0)
        {
          // reorder it
          const u32 remaining_rects = page.num_draw_rects - i;
          if (remaining_rects > 0)
            std::memmove(&page.draw_rects[i], &page.draw_rects[i + 1], sizeof(GSVector4i) * remaining_rects);
        }
      }

      if (page.num_draw_rects != prev_draw_rects)
      {
        if (page.num_draw_rects == 0)
        {
          page.total_draw_rect = INVALID_RECT;
          GL_INS_FMT("Page {} no longer has any draw rects", pn);
        }
        else
        {
          GSVector4i new_total_draw_rect = page.draw_rects[0];
          for (u32 i = 1; i < page.num_draw_rects; i++)
            new_total_draw_rect = new_total_draw_rect.runion(page.draw_rects[i]);
          page.total_draw_rect = new_total_draw_rect;
          GL_INS_FMT("Page {} total draw rect is now {}", pn, new_total_draw_rect);
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
        if (update_vram_writes && it->active_rect.rcontains(rect))
        {
          const HashType new_hash = HashRect(it->write_rect);
          DEV_LOG("New VRAM write hash {:016X} => {:016X}", it->hash, new_hash);
          it->hash = new_hash;
        }
        else if (it->num_splits < s_state.config.max_vram_write_splits && !it->active_rect.eq(intersection))
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

template<GPUTexture::Format format>
void GPUTextureCache::DecodeTexture4(const u16* page, const u16* palette, u32 width, u32 height, u8* dest,
                                     u32 dest_stride)
{
  if ((width % 4u) == 0)
  {
    const u32 vram_width = width / 4;
    [[maybe_unused]] constexpr u32 vram_pixels_per_vec = 2;
    [[maybe_unused]] const u32 aligned_vram_width = Common::AlignDownPow2(vram_width, vram_pixels_per_vec);

    for (u32 y = 0; y < height; y++)
    {
      const u16* page_ptr = page;
      u8* dest_ptr = dest;
      u32 x = 0;

#ifdef CPU_ARCH_SIMD
      for (; x < aligned_vram_width; x += vram_pixels_per_vec)
      {
        // No variable shift without AVX, kinda pointless to vectorize the extract...
        alignas(VECTOR_ALIGNMENT) u16 c16[vram_pixels_per_vec * 4];
        u32 pp = *(page_ptr++);
        c16[0] = palette[pp & 0x0F];
        c16[1] = palette[(pp >> 4) & 0x0F];
        c16[2] = palette[(pp >> 8) & 0x0F];
        c16[3] = palette[pp >> 12];
        pp = *(page_ptr++);
        c16[4] = palette[pp & 0x0F];
        c16[5] = palette[(pp >> 4) & 0x0F];
        c16[6] = palette[(pp >> 8) & 0x0F];
        c16[7] = palette[pp >> 12];
        ConvertVRAMPixels<format>(dest_ptr, GSVector4i::load<true>(c16));
      }
#endif

      for (; x < vram_width; x++)
      {
        const u32 pp = *(page_ptr++);
        ConvertVRAMPixel<format>(dest_ptr, palette[pp & 0x0F]);
        ConvertVRAMPixel<format>(dest_ptr, palette[(pp >> 4) & 0x0F]);
        ConvertVRAMPixel<format>(dest_ptr, palette[(pp >> 8) & 0x0F]);
        ConvertVRAMPixel<format>(dest_ptr, palette[pp >> 12]);
      }

      page += VRAM_WIDTH;
      dest += dest_stride;
    }
  }
  else
  {
    for (u32 y = 0; y < height; y++)
    {
      const u16* page_ptr = page;
      u8* dest_ptr = dest;

      u32 offs = 0;
      u16 texel = 0;
      for (u32 x = 0; x < width; x++)
      {
        if (offs == 0)
          texel = *(page_ptr++);

        ConvertVRAMPixel<format>(dest_ptr, palette[texel & 0x0F]);
        texel >>= 4;

        offs = (offs + 1) % 4;
      }

      page += VRAM_WIDTH;
      dest += dest_stride;
    }
  }
}

template<GPUTexture::Format format>
void GPUTextureCache::DecodeTexture8(const u16* page, const u16* palette, u32 width, u32 height, u8* dest,
                                     u32 dest_stride)
{
  if ((width % 2u) == 0)
  {
    const u32 vram_width = width / 2;
    [[maybe_unused]] constexpr u32 vram_pixels_per_vec = 4;
    [[maybe_unused]] const u32 aligned_vram_width = Common::AlignDownPow2(vram_width, vram_pixels_per_vec);

    for (u32 y = 0; y < height; y++)
    {
      const u16* page_ptr = page;
      u8* dest_ptr = dest;
      u32 x = 0;

#ifdef CPU_ARCH_SIMD
      for (; x < aligned_vram_width; x += vram_pixels_per_vec)
      {
        // No variable shift without AVX, kinda pointless to vectorize the extract...
        alignas(VECTOR_ALIGNMENT) u16 c16[vram_pixels_per_vec * 2];
        u32 pp = *(page_ptr++);
        c16[0] = palette[pp & 0xFF];
        c16[1] = palette[(pp >> 8) & 0xFF];
        pp = *(page_ptr++);
        c16[2] = palette[pp & 0xFF];
        c16[3] = palette[(pp >> 8) & 0xFF];
        pp = *(page_ptr++);
        c16[4] = palette[pp & 0xFF];
        c16[5] = palette[(pp >> 8) & 0xFF];
        pp = *(page_ptr++);
        c16[6] = palette[pp & 0xFF];
        c16[7] = palette[(pp >> 8) & 0xFF];
        ConvertVRAMPixels<format>(dest_ptr, GSVector4i::load<true>(c16));
      }
#endif

      for (; x < vram_width; x++)
      {
        const u32 pp = *(page_ptr++);
        ConvertVRAMPixel<format>(dest_ptr, palette[pp & 0xFF]);
        ConvertVRAMPixel<format>(dest_ptr, palette[pp >> 8]);
      }

      page += VRAM_WIDTH;
      dest += dest_stride;
    }
  }
  else
  {
    for (u32 y = 0; y < height; y++)
    {
      const u16* page_ptr = page;
      u8* dest_ptr = dest;

      u32 offs = 0;
      u16 texel = 0;
      for (u32 x = 0; x < width; x++)
      {
        if (offs == 0)
          texel = *(page_ptr++);

        ConvertVRAMPixel<format>(dest_ptr, palette[texel & 0xFF]);
        texel >>= 8;

        offs ^= 1;
      }

      page += VRAM_WIDTH;
      dest += dest_stride;
    }
  }
}

template<GPUTexture::Format format>
void GPUTextureCache::DecodeTexture16(const u16* page, u32 width, u32 height, u8* dest, u32 dest_stride)
{
  [[maybe_unused]] constexpr u32 pixels_per_vec = 8;
  [[maybe_unused]] const u32 aligned_width = Common::AlignDownPow2(width, pixels_per_vec);

  for (u32 y = 0; y < height; y++)
  {
    const u16* page_ptr = page;
    u8* dest_ptr = dest;
    u32 x = 0;

#ifdef CPU_ARCH_SIMD
    for (; x < aligned_width; x += pixels_per_vec)
    {
      ConvertVRAMPixels<format>(dest_ptr, GSVector4i::load<false>(page_ptr));
      page_ptr += pixels_per_vec;
    }
#endif

    for (; x < width; x++)
      ConvertVRAMPixel<format>(dest_ptr, *(page_ptr++));

    page += VRAM_WIDTH;
    dest += dest_stride;
  }
}

void GPUTextureCache::DecodeTexture(GPUTextureMode mode, const u16* page_ptr, const u16* palette, u8* dest,
                                    u32 dest_stride, u32 width, u32 height, GPUTexture::Format dest_format)
{
  if (dest_format == GPUTexture::Format::RGBA8)
  {
    switch (mode)
    {
      case GPUTextureMode::Palette4Bit:
        DecodeTexture4<GPUTexture::Format::RGBA8>(page_ptr, palette, width, height, dest, dest_stride);
        break;
      case GPUTextureMode::Palette8Bit:
        DecodeTexture8<GPUTexture::Format::RGBA8>(page_ptr, palette, width, height, dest, dest_stride);
        break;
      case GPUTextureMode::Direct16Bit:
      case GPUTextureMode::Reserved_Direct16Bit:
        DecodeTexture16<GPUTexture::Format::RGBA8>(page_ptr, width, height, dest, dest_stride);
        break;

        DefaultCaseIsUnreachable()
    }
  }
  else if (dest_format == GPUTexture::Format::RGB5A1)
  {
    switch (mode)
    {
      case GPUTextureMode::Palette4Bit:
        DecodeTexture4<GPUTexture::Format::RGB5A1>(page_ptr, palette, width, height, dest, dest_stride);
        break;
      case GPUTextureMode::Palette8Bit:
        DecodeTexture8<GPUTexture::Format::RGB5A1>(page_ptr, palette, width, height, dest, dest_stride);
        break;
      case GPUTextureMode::Direct16Bit:
      case GPUTextureMode::Reserved_Direct16Bit:
        DecodeTexture16<GPUTexture::Format::RGB5A1>(page_ptr, width, height, dest, dest_stride);
        break;

        DefaultCaseIsUnreachable()
    }
  }
  else if (dest_format == GPUTexture::Format::A1BGR5)
  {
    switch (mode)
    {
      case GPUTextureMode::Palette4Bit:
        DecodeTexture4<GPUTexture::Format::A1BGR5>(page_ptr, palette, width, height, dest, dest_stride);
        break;
      case GPUTextureMode::Palette8Bit:
        DecodeTexture8<GPUTexture::Format::A1BGR5>(page_ptr, palette, width, height, dest, dest_stride);
        break;
      case GPUTextureMode::Direct16Bit:
      case GPUTextureMode::Reserved_Direct16Bit:
        DecodeTexture16<GPUTexture::Format::A1BGR5>(page_ptr, width, height, dest, dest_stride);
        break;

        DefaultCaseIsUnreachable()
    }
  }
  else
  {
    Panic("Unsupported texture format.");
  }
}

void GPUTextureCache::DecodeTexture(u8 page, GPUTexturePaletteReg palette, GPUTextureMode mode, GPUTexture* texture)
{
  alignas(16) static u8 s_temp_buffer[TEXTURE_PAGE_WIDTH * TEXTURE_PAGE_HEIGHT * sizeof(u32)];

  const u32 ps = texture->GetPixelSize();
  u8* tex_map;
  u32 tex_stride;
  const bool mapped =
    texture->Map(reinterpret_cast<void**>(&tex_map), &tex_stride, 0, 0, TEXTURE_PAGE_WIDTH, TEXTURE_PAGE_HEIGHT);
  if (!mapped)
  {
    tex_map = s_temp_buffer;
    tex_stride = Common::AlignUpPow2(ps * TEXTURE_PAGE_WIDTH, 4);
  }

  const u16* page_ptr = VRAMPagePointer(page);
  const u16* palette_ptr = TextureModeHasPalette(mode) ? VRAMPalettePointer(palette) : nullptr;
  DecodeTexture(mode, page_ptr, palette_ptr, tex_map, tex_stride, TEXTURE_PAGE_WIDTH, TEXTURE_PAGE_HEIGHT,
                texture->GetFormat());

  if (mapped)
    texture->Unmap();
  else
    texture->Update(0, 0, TEXTURE_PAGE_WIDTH, TEXTURE_PAGE_HEIGHT, tex_map, tex_stride);
}

std::unique_ptr<GPUTexture> GPUTextureCache::FetchTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                                          GPUTexture::Type type, GPUTexture::Format format,
                                                          GPUTexture::Flags flags, const void* data /* = nullptr */,
                                                          u32 data_stride /* = 0 */)
{
  Error error;
  std::unique_ptr<GPUTexture> tex =
    g_gpu_device->FetchTexture(width, height, layers, levels, samples, type, format, flags, data, data_stride, &error);
  if (!tex) [[unlikely]]
  {
    ERROR_LOG("Failed to create {}x{} texture for cache: {}", width, height, error.GetDescription());
    Host::AddIconOSDWarning("TCFetchTextureFailed", ICON_EMOJI_WARNING,
                            fmt::format(TRANSLATE_FS("GPU_HW", "Failed to allocate {}x{} texture for cache:\n{}"),
                                        width, height, error.GetDescription()),
                            Host::OSD_ERROR_DURATION);
  }

  return tex;
}

const GPUTextureCache::Source* GPUTextureCache::LookupSource(SourceKey key, const GSVector4i rect,
                                                             PaletteRecordFlags flags)
{
  GL_SCOPE_FMT("TC: Lookup source {}", SourceKeyToString(key));

  TList<Source>& list = s_state.pages[key.page].sources;
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
  if (source) [[likely]]
  {
#if defined(_DEBUG) || defined(_DEVEL)
    // GL_INS_FMT("Tex hash: {:016X}", source->texture_hash);
    // GL_INS_FMT("Palette hash: {:016X}", source->palette_hash);
    if (!uv_rect.eq(INVALID_RECT))
    {
      LoopXWrappedPages(source->key.page, TexturePageCountForMode(source->key.mode), [&uv_rect](u32 pn) {
        const PageEntry& pe = s_state.pages[pn];
        ListIterate(pe.writes, [&uv_rect](const VRAMWrite* vrw) {
          if (const GSVector4i intersection = uv_rect.rintersect(vrw->write_rect); !intersection.rempty())
            GL_INS_FMT("TC: VRAM write was {:016X} ({})", vrw->hash, intersection);
        });
      });
      if (TextureModeHasPalette(source->key.mode))
        GL_INS_FMT("TC: Palette was {:016X}", source->palette_hash);
    }
#endif

    DebugAssert(source->from_hash_cache);
    source->from_hash_cache->last_used_frame = System::GetFrameNumber();

    // TODO: Cache var.
    if (g_gpu_settings.texture_replacements.dump_textures)
    {
      source->active_uv_rect = source->active_uv_rect.runion(uv_rect);
      source->palette_record_flags |= flags;
    }
  }

  return source;
}

bool GPUTextureCache::IsPageDrawn(u32 page_index, const GSVector4i rect)
{
  const PageEntry& page = s_state.pages[page_index];
  if (page.num_draw_rects == 0 || !page.total_draw_rect.rintersects(rect))
    return false;

  // if there's only a single draw rect, it'll match the total
  if (page.num_draw_rects == 1)
    return true;

  for (u32 i = 0; i < page.num_draw_rects; i++)
  {
    if (page.draw_rects[i].rintersects(rect))
      return true;
  }

  return false;
}

bool GPUTextureCache::IsRectDrawn(const GSVector4i rect)
{
  // TODO: This is potentially hot, so replace it with an explicit loop over the pages instead.
  return !LoopRectPagesWithEarlyExit(rect, [&rect](u32 pn) { return !IsPageDrawn(pn, rect); });
}

bool GPUTextureCache::AreSourcePagesDrawn(SourceKey key, const GSVector4i rect)
{
  // NOTE: This doesn't handle VRAM wrapping. But neither does the caller. YOLO?
#if defined(_DEBUG) || defined(_DEVEL)
  {
    for (u32 offset = 0; offset < TexturePageCountForMode(key.mode); offset++)
    {
      const u32 wrapped_page = ((key.page + offset) & VRAM_PAGE_X_MASK) + (key.page & VRAM_PAGE_Y_MASK);
      if (IsPageDrawn(wrapped_page, rect))
      {
        GL_INS_FMT("UV rect {} intersects page [{}] dirty rect {}, disabling TC", rect, wrapped_page,
                   s_state.pages[wrapped_page].total_draw_rect);
      }
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

void GPUTextureCache::Invalidate()
{
  for (u32 i = 0; i < NUM_VRAM_PAGES; i++)
  {
    InvalidatePageSources(i);

    PageEntry& page = s_state.pages[i];
    page.num_draw_rects = 0;
    page.total_draw_rect = GSVector4i::zero();
    std::memset(page.draw_rects.data(), 0, sizeof(page.draw_rects));

    while (page.writes.tail)
      RemoveVRAMWrite(page.writes.tail->ref);
  }

  // should all be null
#if defined(_DEBUG) || defined(_DEVEL)
  for (u32 i = 0; i < NUM_VRAM_PAGES; i++)
    DebugAssert(!s_state.pages[i].sources.head && !s_state.pages[i].sources.tail);
  DebugAssert(!s_state.last_vram_write);
#endif

  ClearHashCache();
}

void GPUTextureCache::InvalidateSources()
{
  // keep draw rects and vram writes
  for (u32 i = 0; i < NUM_VRAM_PAGES; i++)
    InvalidatePageSources(i);

  ClearHashCache();
}

void GPUTextureCache::InvalidatePageSources(u32 pn)
{
  DebugAssert(pn < NUM_VRAM_PAGES);

  TList<Source>& ps = s_state.pages[pn].sources;
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

void GPUTextureCache::InvalidatePageSources(u32 pn, const GSVector4i rc, bool remove_from_hash_cache)
{
  DebugAssert(pn < NUM_VRAM_PAGES);

  TList<Source>& ps = s_state.pages[pn].sources;
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
    DestroySource(src, remove_from_hash_cache);
  }
}

void GPUTextureCache::DestroySource(Source* src, bool remove_from_hash_cache)
{
  GL_INS_FMT("Invalidate source {}", SourceToString(src));

  if (g_gpu_settings.texture_replacements.dump_textures && !src->active_uv_rect.eq(INVALID_RECT))
  {
    if (!s_state.config.dump_texture_pages)
    {
      // Find VRAM writes that overlap with this source
      LoopRectPages(src->active_uv_rect, [src](const u32 pn) {
        PageEntry& pg = s_state.pages[pn];
        ListIterate(pg.writes, [src](VRAMWrite* vw) {
          UpdateVRAMWriteSources(vw, src->key, src->active_uv_rect, src->palette_record_flags);
        });
        return true;
      });
    }
    else
    {
      DumpTextureFromPage(src);
    }
  }

  for (u32 i = 0; i < src->num_page_refs; i++)
    ListUnlink(src->page_refs[i]);

  HashCacheEntry* hcentry = src->from_hash_cache;
  DebugAssert(hcentry && hcentry->ref_count > 0);
  ListUnlink(src->hash_cache_ref);
  hcentry->ref_count--;
  if (hcentry->ref_count == 0 && remove_from_hash_cache)
    RemoveFromHashCache(hcentry, src->key, src->texture_hash, src->palette_hash);

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

  Source* src = new Source();
  src->key = key;
  src->num_page_refs = 0;
  src->texture = hcentry->texture.get();
  src->from_hash_cache = hcentry;
  ListAppend(&hcentry->sources, src, &src->hash_cache_ref);
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

    ListPrepend(&s_state.pages[pn].sources, src, &src->page_refs[ri]);
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

    ListAppend(&s_state.pages[pn].sources, src, &src->page_refs[ri]);
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

void GPUTextureCache::UpdateVRAMTrackingState()
{
  s_state.track_vram_writes = ShouldTrackVRAMWrites();
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
    const u16* row_ptr = &g_vram[rect.y * VRAM_WIDTH + rect.x];
    for (u32 y = 0; y < rect_height; y++)
    {
      const u16* ptr = row_ptr;
      row_ptr += VRAM_WIDTH;

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
      const u16* ptr = row_ptr;
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
      const PageEntry& page = s_state.pages[pn];
      ListIterate(page.sources, [entry](const Source* src) {
        if (!src->active_uv_rect.eq(INVALID_RECT))
          UpdateVRAMWriteSources(entry, src->key, src->active_uv_rect, src->palette_record_flags);
      });

      return true;
    });
  }
}

void GPUTextureCache::MergeAdjacentVRAMWritePaletteRecords(VRAMWrite* entry)
{
  if (!IsDumpingVRAMWriteTextures())
    return;

retry:
  for (auto it = entry->palette_records.begin(); it != entry->palette_records.end(); ++it)
  {
    VRAMWrite::PaletteRecord& rec = *it;

    for (auto oit = entry->palette_records.begin(); oit != entry->palette_records.end(); ++oit)
    {
      const VRAMWrite::PaletteRecord& orec = *oit;
      if (it == oit || rec.flags != orec.flags || rec.palette_hash != orec.palette_hash ||
          rec.key.mode != orec.key.mode)
      {
        continue;
      }

      if (rec.rect.zzyw().eq(orec.rect.xxyw()))
      {
        // horizontal merge, make sure the left side aligns with the page
        const GSVector4i vpr = VRAMPageRect(rec.key.page);
        if (vpr.x != rec.rect.x)
          continue;

        // mergey merge
        rec.rect.z = orec.rect.z;
        entry->palette_records.erase(oit);
        goto retry;
      }

      // not worrying with vertical merges until we find something that actually needs it
    }
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
  std::vector<VRAMWrite::PaletteRecord>::iterator iter;
  if (source_key.HasPalette())
  {
    // Palette requires exact match.
    iter = std::find_if(entry->palette_records.begin(), entry->palette_records.end(),
                        [&source_key](const auto& it) { return (it.key == source_key); });
  }
  else
  {
    // C16 only needs to match on the mode, palette is not used, and page doesn't matter.
    // In theory we could extend the page skipping to palette textures too, but is it needed?
    iter = std::find_if(entry->palette_records.begin(), entry->palette_records.end(),
                        [&source_key](const auto& it) { return (it.key.mode == source_key.mode); });
  }

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
      ListAppend(&s_state.pages[pn].writes, it, &it->page_refs[it->num_page_refs++]);
      return true;
    });

    DEV_LOG("Split VRAM write {:016X} at {} in direction {} => {}", it->hash, entry->active_rect, i, splitr);
  }

  for (u32 i = 0; i < entry->num_page_refs; i++)
    ListUnlink(entry->page_refs[i]);

  delete entry;
}

bool GPUTextureCache::TryMergeVRAMWrite(VRAMWrite* entry, const GSVector4i written_rect)
{
  // It shouldn't have been split. Don't want to update after it has been.
  if (s_state.last_vram_write->num_splits != 0)
    return false;

  // Check coalesce bounds/config.
  const u32 coalesce_width = s_state.config.max_vram_write_coalesce_width;
  const u32 coalesce_height = s_state.config.max_vram_write_coalesce_height;
  const bool merge_vertical = (static_cast<u32>(written_rect.height()) <= coalesce_height &&
                               s_state.last_vram_write->write_rect.left == written_rect.left &&
                               s_state.last_vram_write->write_rect.right == written_rect.right &&
                               s_state.last_vram_write->write_rect.bottom == written_rect.top);
  const bool merge_horizontal = (static_cast<u32>(written_rect.width()) <= coalesce_width &&
                                 s_state.last_vram_write->write_rect.top == written_rect.top &&
                                 s_state.last_vram_write->write_rect.bottom == written_rect.bottom &&
                                 s_state.last_vram_write->write_rect.right == written_rect.left);
  if (!merge_vertical && !merge_horizontal)
    return false;

  // Double-check that nothing has used this write as a source yet (i.e. drawn).
  // Don't want to merge textures that are already completely uploaded...
  if (!LoopRectPagesWithEarlyExit(entry->active_rect, [entry](const u32 pn) {
        return ListIterateWithEarlyExit(s_state.pages[pn].sources, [entry](const Source* src) {
          return (!src->active_uv_rect.eq(INVALID_RECT) || !src->active_uv_rect.rintersects(entry->active_rect));
        });
      }))
  {
    return false;
  }

  // Remove from old pages, we'll re-add it.
  for (u32 i = 0; i < entry->num_page_refs; i++)
    ListUnlink(entry->page_refs[i]);
  entry->num_page_refs = 0;

  // Expand the write.
  const GSVector4i new_rect = entry->write_rect.runion(written_rect);
  DEV_LOG("Expanding VRAM write {:016X} from {} to {}", entry->hash, entry->write_rect, new_rect);
  entry->active_rect = new_rect;
  entry->write_rect = new_rect;
  entry->hash = HashRect(new_rect);

  // Re-add to pages.
  LoopRectPages(new_rect, [entry](u32 pn) {
    DebugAssert(entry->num_page_refs < MAX_PAGE_REFS_PER_WRITE);
    ListAppend(&s_state.pages[pn].writes, entry, &entry->page_refs[entry->num_page_refs++]);
    return true;
  });

  return true;
}

void GPUTextureCache::RemoveVRAMWrite(VRAMWrite* entry)
{
  DEV_LOG("Remove VRAM write {:016X} at {}", entry->hash, entry->write_rect);

  SyncVRAMWritePaletteRecords(entry);
  MergeAdjacentVRAMWritePaletteRecords(entry);

  if (entry->num_splits > 0 && !entry->palette_records.empty())
  {
    // Combine palette records with another write.
    VRAMWrite* other_write = nullptr;
    LoopRectPagesWithEarlyExit(entry->write_rect, [&entry, &other_write](u32 pn) {
      PageEntry& pg = s_state.pages[pn];
      ListIterateWithEarlyExit(pg.writes, [&entry, &other_write](VRAMWrite* cur) {
        if (cur == entry || cur->hash != entry->hash)
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

  s_state.last_vram_write = (s_state.last_vram_write == entry) ? nullptr : s_state.last_vram_write;
  delete entry;
}

void GPUTextureCache::DumpTexturesFromVRAMWrite(VRAMWrite* entry)
{
  if (g_gpu_settings.texture_replacements.dump_textures && !s_state.config.dump_texture_pages)
  {
    for (const VRAMWrite::PaletteRecord& prec : entry->palette_records)
    {
      if (prec.key.mode == GPUTextureMode::Direct16Bit && !s_state.config.dump_c16_textures)
        continue;

      HashType pal_hash =
        (prec.key.mode < GPUTextureMode::Direct16Bit) ? HashPalette(prec.key.palette, prec.key.mode) : 0;

      // If it's 8-bit, try reducing the range of the palette.
      u32 pal_min = 0, pal_max = prec.key.HasPalette() ? (GetPaletteWidth(prec.key.mode) - 1) : 0;
      if (prec.key.HasPalette() && s_state.config.reduce_palette_range)
      {
        std::tie(pal_min, pal_max) = ReducePaletteBounds(prec.rect, prec.key.mode, prec.key.palette);
        pal_hash = HashPartialPalette(prec.palette, pal_min, pal_max);
      }

      const u32 offset_x = ApplyTextureModeShift(prec.key.mode, prec.rect.left - entry->write_rect.left);
      const u32 offset_y = prec.rect.top - entry->write_rect.top;

      DumpTexture(TextureReplacementType::TextureFromVRAMWrite, offset_x, offset_y, entry->write_rect.width(),
                  entry->write_rect.height(), prec.key.mode, entry->hash, pal_hash, pal_min, pal_max, prec.palette,
                  prec.rect, prec.flags);
    }
  }
}

void GPUTextureCache::DumpTextureFromPage(const Source* src)
{
  // C16 filter
  if (!s_state.config.dump_c16_textures && src->key.mode >= GPUTextureMode::Direct16Bit)
    return;

  const bool dump_full_page = s_state.config.dump_full_texture_pages;

  // Dump active area from page
  HashType pal_hash = src->palette_hash;
  const u16* pal_ptr = src->key.HasPalette() ? VRAMPalettePointer(src->key.palette) : nullptr;

  // We don't want to dump the wraparound
  const GSVector4i unwrapped_texture_rect =
    (TexturePageIsWrapping(src->key.mode, src->key.page) ?
       GSVector4i(VRAMPageStartX(src->key.page), src->texture_rect.y, VRAM_WIDTH, src->texture_rect.w) :
       src->texture_rect);
  const GSVector4i dump_rect =
    dump_full_page ? unwrapped_texture_rect : src->active_uv_rect.rintersect(unwrapped_texture_rect);
  if (dump_rect.rempty())
    return;

  // Need to hash only the active area.
  const HashType tex_hash = HashRect(dump_rect);

  // Source rect needs the offset, but we still only want to hash the active area when replacing
  const GSVector4i dump_offset_in_page = dump_rect.sub32(unwrapped_texture_rect);

  // If it's 8-bit, try reducing the range of the palette.
  u32 pal_min = 0, pal_max = src->key.HasPalette() ? (GetPaletteWidth(src->key.mode) - 1) : 0;
  if (src->key.HasPalette() && s_state.config.reduce_palette_range)
  {
    std::tie(pal_min, pal_max) = ReducePaletteBounds(dump_rect, src->key.mode, src->key.palette);
    pal_hash = HashPartialPalette(pal_ptr, pal_min, pal_max);
  }

  DumpTexture(TextureReplacementType::TextureFromPage, ApplyTextureModeShift(src->key.mode, dump_offset_in_page.x),
              dump_offset_in_page.y, unwrapped_texture_rect.width(), unwrapped_texture_rect.height(), src->key.mode,
              tex_hash, pal_hash, pal_min, pal_max, pal_ptr, dump_rect, src->palette_record_flags);
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

GPUTextureCache::HashCacheKey GPUTextureCache::GetHashCacheKey(SourceKey key, HashType tex_hash, HashType pal_hash)
{
  return HashCacheKey{tex_hash, pal_hash, static_cast<HashType>(key.mode)};
}

GPUTextureCache::HashCacheEntry* GPUTextureCache::LookupHashCache(SourceKey key, HashType tex_hash, HashType pal_hash)
{
  const HashCacheKey hkey = GetHashCacheKey(key, tex_hash, pal_hash);

  const auto it = s_state.hash_cache.find(hkey);
  if (it != s_state.hash_cache.end())
  {
    GL_INS_FMT("TC: Hash cache hit {:X} {:X}", hkey.texture_hash, hkey.palette_hash);
    return &it->second;
  }

  GL_INS_FMT("TC: Hash cache miss {:X} {:X}", hkey.texture_hash, hkey.palette_hash);

  HashCacheEntry entry;
  entry.ref_count = 0;
  entry.last_used_frame = 0;
  entry.sources = {};
  entry.texture = FetchTexture(TEXTURE_PAGE_WIDTH, TEXTURE_PAGE_HEIGHT, 1, 1, 1, GPUTexture::Type::Texture,
                               s_state.hash_cache_texture_format, GPUTexture::Flags::None);
  if (!entry.texture)
    return nullptr;

  DecodeTexture(key.page, key.palette, key.mode, entry.texture.get());

  if (g_gpu_settings.texture_replacements.enable_texture_replacements)
    ApplyTextureReplacements(key, tex_hash, pal_hash, &entry);

  s_state.hash_cache_memory_usage += entry.texture->GetVRAMUsage();

  return &s_state.hash_cache.emplace(hkey, std::move(entry)).first->second;
}

void GPUTextureCache::RemoveFromHashCache(HashCacheEntry* entry, SourceKey key, HashType tex_hash, HashType pal_hash)
{
  const HashCacheKey hckey = GetHashCacheKey(key, tex_hash, pal_hash);
  const auto iter = s_state.hash_cache.find(hckey);
  Assert(iter != s_state.hash_cache.end() && &iter->second == entry);
  RemoveFromHashCache(iter);
}

void GPUTextureCache::RemoveFromHashCache(HashCache::iterator it)
{
  ListIterate(it->second.sources, [](Source* source) { DestroySource(source); });

  const size_t vram_usage = it->second.texture->GetVRAMUsage();
  DebugAssert(s_state.hash_cache_memory_usage >= vram_usage);
  s_state.hash_cache_memory_usage -= vram_usage;

  g_gpu_device->RecycleTexture(std::move(it->second.texture));
  s_state.hash_cache.erase(it);
}

void GPUTextureCache::ClearHashCache()
{
  while (!s_state.hash_cache.empty())
    RemoveFromHashCache(s_state.hash_cache.begin());
}

void GPUTextureCache::Compact()
{
  // Number of frames before unused hash cache entries are evicted.
  static constexpr u32 MAX_HASH_CACHE_AGE = 600;

  // Maximum number of textures which are permitted in the hash cache at the end of the frame.
  const u32 max_hash_cache_size = s_state.config.max_hash_cache_entries;
  const size_t max_hash_cache_memory = static_cast<size_t>(s_state.config.max_hash_cache_vram_usage_mb) * 1048576;

  bool might_need_cache_purge =
    (s_state.hash_cache.size() > max_hash_cache_size || s_state.hash_cache_memory_usage >= max_hash_cache_memory);
  if (might_need_cache_purge)
    s_state.hash_cache_purge_list.clear();

  const u32 frame_number = System::GetFrameNumber();
  const u32 min_frame_number = ((frame_number > MAX_HASH_CACHE_AGE) ? (frame_number - MAX_HASH_CACHE_AGE) : 0);

  for (auto it = s_state.hash_cache.begin(); it != s_state.hash_cache.end();)
  {
    HashCacheEntry& e = it->second;
    if (e.ref_count == 0 && e.last_used_frame < min_frame_number)
    {
      RemoveFromHashCache(it++);
      continue;
    }

    // We might free up enough just with "normal" removals above.
    if (might_need_cache_purge)
    {
      might_need_cache_purge =
        (s_state.hash_cache.size() > max_hash_cache_size || s_state.hash_cache_memory_usage >= max_hash_cache_memory);
      if (might_need_cache_purge)
        s_state.hash_cache_purge_list.emplace_back(it, static_cast<s32>(e.last_used_frame));
    }

    ++it;
  }

  // Pushing to a list, sorting, and removing ends up faster than re-iterating the map.
  if (might_need_cache_purge)
  {
    DEV_LOG("Force compacting hash cache, count = {}, size = {:.1f} MB", s_state.hash_cache.size(),
            static_cast<float>(s_state.hash_cache_memory_usage) / 1048576.0f);

    std::sort(s_state.hash_cache_purge_list.begin(), s_state.hash_cache_purge_list.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.second < rhs.second; });

    size_t purge_index = 0;
    while (s_state.hash_cache.size() > max_hash_cache_size || s_state.hash_cache_memory_usage >= max_hash_cache_memory)
    {
      if (purge_index == s_state.hash_cache_purge_list.size())
      {
        WARNING_LOG("Cannot find hash cache entries to purge, current hash cache size is {} MB in {} textures.",
                    static_cast<double>(s_state.hash_cache_memory_usage) / 1048576.0, s_state.hash_cache.size());
        break;
      }

      RemoveFromHashCache(s_state.hash_cache_purge_list[purge_index++].first);
    }

    DEV_LOG("Finished compacting hash cache, count = {}, size = {:.1f} MB", s_state.hash_cache.size(),
            static_cast<float>(s_state.hash_cache_memory_usage) / 1048576.0f);
  }

  CompactTextureReplacementGPUImages();
}

size_t GPUTextureCache::HashCacheKeyHash::operator()(const HashCacheKey& k) const
{
  std::size_t h = 0;
  hash_combine(h, k.texture_hash, k.palette_hash, k.mode);
  return h;
}

TinyString GPUTextureCache::VRAMReplacementName::ToString() const
{
  return TinyString::from_format("{:016X}{:016X}", high, low);
}

bool GPUTextureCache::VRAMReplacementName::Parse(const std::string_view file_title)
{
  if (file_title.length() != 43)
    return false;

  const std::optional<u64> high_value = StringUtil::FromChars<u64>(file_title.substr(11, 16), 16);
  const std::optional<u64> low_value = StringUtil::FromChars<u64>(file_title.substr(11 + 16), 16);
  if (!high_value.has_value() || !low_value.has_value())
    return false;

  low = low_value.value();
  high = high_value.value();
  return true;
}

size_t GPUTextureCache::VRAMReplacementNameHash::operator()(const VRAMReplacementName& name) const
{
  size_t seed = std::hash<u64>{}(name.low);
  hash_combine(seed, name.high);
  return seed;
}

static constexpr const char* s_texture_replacement_mode_names[] = {"P4",   "P8",   "C16",   "C16",
                                                                   "STP4", "STP8", "STC16", "STC16"};

TinyString GPUTextureCache::TextureReplacementName::ToString() const
{
  const char* type_str = (type == TextureReplacementType::TextureFromVRAMWrite) ? "texupload" : "texpage";
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

bool GPUTextureCache::TextureReplacementName::Parse(const std::string_view file_title)
{
  // TODO: Swap to https://github.com/eliaskosunen/scnlib

  std::string_view::size_type start_pos = 0;
  std::string_view::size_type end_pos = file_title.find("-", start_pos);
  if (end_pos == std::string_view::npos)
    return false;

  // type
  std::string_view token = file_title.substr(start_pos, end_pos);
  if (token == "texupload")
    type = TextureReplacementType::TextureFromVRAMWrite;
  else if (token == "texpage")
    type = TextureReplacementType::TextureFromPage;
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

  if (GetTextureMode() < GPUTextureMode::Direct16Bit)
  {
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

GPUTextureCache::TextureReplacementIndex GPUTextureCache::TextureReplacementName::GetIndex() const
{
  return {src_hash, GetTextureMode()};
}

GPUTextureMode GPUTextureCache::TextureReplacementName::GetTextureMode() const
{
  return static_cast<GPUTextureMode>(texture_mode & 3u);
}

bool GPUTextureCache::TextureReplacementName::IsSemitransparent() const
{
  return (texture_mode >= 4);
}

size_t GPUTextureCache::TextureReplacementIndexHash::operator()(const TextureReplacementIndex& name) const
{
  // TODO: This sucks ass, do better.
  size_t seed = std::hash<u64>{}(name.src_hash);
  hash_combine(seed, static_cast<u8>(name.mode));
  return seed;
}

size_t GPUTextureCache::DumpedTextureKeyHash::operator()(const DumpedTextureKey& k) const
{
  // TODO: This is slow
  std::size_t hash = 0;
  hash_combine(hash, k.tex_hash, k.pal_hash, k.width, k.height, k.texture_mode);
  return hash;
}

void GPUTextureCache::GameSerialChanged()
{
  ReloadTextureReplacements(false, false);
}

GPUTexture* GPUTextureCache::GetVRAMReplacement(u32 width, u32 height, const void* pixels)
{
  const VRAMReplacementName hash = GetVRAMWriteHash(width, height, pixels);

  const auto it = s_state.vram_replacements.find(hash);
  if (it == s_state.vram_replacements.end())
    return nullptr;

  return GetTextureReplacementGPUImage(it->second);
}

bool GPUTextureCache::ShouldDumpVRAMWrite(u32 width, u32 height)
{
  return (g_gpu_settings.texture_replacements.dump_vram_writes &&
          width >= s_state.config.vram_write_dump_width_threshold &&
          height >= s_state.config.vram_write_dump_height_threshold);
}

void GPUTextureCache::DumpVRAMWrite(u32 width, u32 height, const void* pixels)
{
  const VRAMReplacementName name = GetVRAMWriteHash(width, height, pixels);
  if (s_state.dumped_vram_writes.find(name) != s_state.dumped_vram_writes.end())
  {
    DEV_COLOR_LOG(Green, "Not dumping {}", name.ToString());
    return;
  }

  s_state.dumped_vram_writes.insert(name);

  const std::string path = GetVRAMWriteDumpPath(name);
  if (path.empty() || FileSystem::FileExists(path.c_str()))
    return;

  Image image(width, height, ImageFormat::RGBA8);

  const u16* src_pixels = reinterpret_cast<const u16*>(pixels);

  for (u32 y = 0; y < height; y++)
  {
    u8* row_ptr = image.GetRowPixels(y);
    for (u32 x = 0; x < width; x++)
    {
      const u32 pixel32 = VRAMRGBA5551ToRGBA8888(*(src_pixels++));
      std::memcpy(row_ptr, &pixel32, sizeof(pixel32));
      row_ptr += sizeof(pixel32);
    }
  }

  if (s_state.config.dump_vram_write_force_alpha_channel)
    image.SetAllPixelsOpaque();

  INFO_LOG("Dumping {}x{} VRAM write to '{}'", width, height, Path::GetFileName(path));

  Error error;
  if (!image.SaveToFile(path.c_str(), Image::DEFAULT_SAVE_QUALITY, &error)) [[unlikely]]
  {
    ERROR_LOG("Failed to dump {}x{} VRAM write to '{}': {}", width, height, Path::GetFileName(path),
              error.GetDescription());
  }
}

void GPUTextureCache::DumpTexture(TextureReplacementType type, u32 offset_x, u32 offset_y, u32 src_width,
                                  u32 src_height, GPUTextureMode mode, HashType src_hash, HashType pal_hash,
                                  u32 pal_min, u32 pal_max, const u16* palette_data, const GSVector4i rect,
                                  PaletteRecordFlags flags)
{
  const u32 width = ApplyTextureModeShift(mode, rect.width());
  const u32 height = rect.height();

  if (width < s_state.config.texture_dump_width_threshold || height < s_state.config.texture_dump_height_threshold)
    return;

  const bool semitransparent = ((flags & PaletteRecordFlags::HasSemiTransparentDraws) != PaletteRecordFlags::None &&
                                !s_state.config.dump_texture_force_alpha_channel);
  const u8 dumped_texture_mode = static_cast<u8>(mode) | (semitransparent ? 4 : 0);

  const TextureReplacementName name = {
    .src_hash = src_hash,
    .pal_hash = pal_hash,
    .src_width = Truncate16(src_width),
    .src_height = Truncate16(src_height),
    .type = type,
    .texture_mode = dumped_texture_mode,
    .offset_x = Truncate16(offset_x),
    .offset_y = Truncate16(offset_y),
    .width = Truncate16(width),
    .height = Truncate16(height),
    .pal_min = Truncate8(pal_min),
    .pal_max = Truncate8(pal_max),
  };

  const DumpedTextureKey key = DumpedTextureKey::FromName(name);
  if (s_state.dumped_textures.find(key) != s_state.dumped_textures.end())
  {
    DEV_COLOR_LOG(Green, "Not dumping {}", name.ToString());
    return;
  }

  if (!EnsureGameDirectoryExists())
    return;

  const std::string dump_directory = GetTextureDumpDirectory();
  if (!FileSystem::EnsureDirectoryExists(dump_directory.c_str(), false))
    return;

  s_state.dumped_textures.insert(key);

  SmallString filename = name.ToString();
  filename.append(".png");

  std::string path = Path::Combine(dump_directory, filename);
  if (FileSystem::FileExists(path.c_str()))
    return;

  DEV_LOG("Dumping VRAM write {:016X} [{}x{}] at {}", src_hash, width, height, rect);

  Image image(width, height, ImageFormat::RGBA8);
  GPUTextureCache::DecodeTexture(mode, &g_vram[rect.top * VRAM_WIDTH + rect.left], palette_data, image.GetPixels(),
                                 image.GetPitch(), width, height, GPUTexture::Format::RGBA8);

  System::QueueAsyncTask([path = std::move(path), image = std::move(image), width, height, semitransparent]() mutable {
    // TODO: Vectorize this.
    u32* image_pixels = reinterpret_cast<u32*>(image.GetPixels());
    const u32* image_pixels_end = image_pixels + (width * height);
    if (s_state.config.dump_texture_force_alpha_channel)
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
  });
}

bool GPUTextureCache::IsMatchingReplacementPalette(HashType full_palette_hash, GPUTextureMode mode,
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
  const HashType partial_hash = GPUTextureCache::HashPartialPalette(palette, mode, name.pal_min, name.pal_max);
  return (partial_hash == name.pal_hash);
}

bool GPUTextureCache::HasVRAMWriteTextureReplacements()
{
  return !s_state.vram_write_texture_replacements.empty();
}

void GPUTextureCache::GetVRAMWriteTextureReplacements(std::vector<TextureReplacementSubImage>& replacements,
                                                      HashType vram_write_hash, HashType palette_hash,
                                                      GPUTextureMode mode, GPUTexturePaletteReg palette,
                                                      const GSVector2i& offset_to_page)
{
  const TextureReplacementIndex index = {vram_write_hash, mode};
  const auto& [begin, end] = s_state.vram_write_texture_replacements.equal_range(index);
  if (begin == end)
    return;

  const GSVector4i offset_to_page_v = GSVector4i(offset_to_page).xyxy();

  for (auto it = begin; it != end; ++it)
  {
    if (!IsMatchingReplacementPalette(palette_hash, mode, palette, it->second.first))
      continue;

    const TextureReplacementName& name = it->second.first;
    const GSVector4i rect_in_write_space = name.GetDestRect();
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

    GPUTexture* texture = GetTextureReplacementGPUImage(it->second.second);
    if (!texture)
      continue;

    // Especially for C16 textures, the write may span multiple pages. In this case, we need to offset
    // the start of the page into the replacement texture.
    const GSVector2i rect_in_page_space_start = rect_in_page_space.xy();
    const GSVector2i src_offset =
      GSVector2i::zero().sub32(rect_in_page_space_start) & rect_in_page_space_start.lt32(GSVector2i::zero());
    const GSVector4i clamped_rect_in_page_space =
      rect_in_page_space.add32(GSVector4i::xyxy(src_offset, GSVector2i::zero()))
        .rintersect(GSVector4i::cxpr(0, 0, TEXTURE_PAGE_WIDTH, TEXTURE_PAGE_HEIGHT));

    // TODO: This fails in Wild Arms 2, writes that are wider than a page.
    DebugAssert(rect_in_page_space.width() == name.width && rect_in_page_space.height() == name.height);
    DebugAssert(clamped_rect_in_page_space.width() <= static_cast<s32>(TEXTURE_PAGE_WIDTH));
    DebugAssert(clamped_rect_in_page_space.height() <= static_cast<s32>(TEXTURE_PAGE_HEIGHT));

    const GSVector2 scale = GSVector2(texture->GetSizeVec()) / GSVector2(name.GetSizeVec());
    replacements.push_back(TextureReplacementSubImage{
      clamped_rect_in_page_space, GSVector4i::xyxy(src_offset, src_offset.add32(clamped_rect_in_page_space.rsize())),
      texture, scale.x, scale.y, name.IsSemitransparent()});
  }
}

bool GPUTextureCache::HasTexturePageTextureReplacements()
{
  return !s_state.texture_page_texture_replacements.empty();
}

void GPUTextureCache::GetTexturePageTextureReplacements(std::vector<TextureReplacementSubImage>& replacements,
                                                        u32 start_page_number, HashType page_hash,
                                                        HashType palette_hash, GPUTextureMode mode,
                                                        GPUTexturePaletteReg palette)
{
  // This is truely awful. Because we can dump a sub-page worth of texture, we need to examine the entire replacement
  // list, because any of them could match up...

  const u8 shift = GetTextureModeShift(mode);
  const GSVector4i page_start_in_vram =
    GSVector4i(GSVector2i(VRAMPageStartX(start_page_number), VRAMPageStartY(start_page_number))).xyxy();

  for (TextureReplacementMap::const_iterator it = s_state.texture_page_texture_replacements.begin();
       it != s_state.texture_page_texture_replacements.end(); ++it)
  {
    if (it->first.mode != mode)
      continue;

    // Early-out if the palette mismatches, at least that'll save some cycles...
    if (!IsMatchingReplacementPalette(palette_hash, mode, palette, it->second.first))
      continue;

    const TextureReplacementName& name = it->second.first;
    GSVector4i rect_in_page_space;
    if (name.width == TEXTURE_PAGE_WIDTH && name.height == TEXTURE_PAGE_HEIGHT)
    {
      // This replacement is an entire page, so we can simply check the already-computed page hash.
      DebugAssert(name.offset_x == 0 && name.offset_y == 0);
      if (it->first.src_hash != page_hash)
        continue;

      rect_in_page_space = GSVector4i::cxpr(0, 0, TEXTURE_PAGE_WIDTH, TEXTURE_PAGE_HEIGHT);
    }
    else
    {
      // Unlike write replacements, the
      // Replacement is part of a page, need to re-hash.
      rect_in_page_space = name.GetDestRect();
      const GSVector4i hash_rect =
        rect_in_page_space.blend32<0x5>(rect_in_page_space.srl32(shift)).add32(page_start_in_vram);
      const GPUTextureCache::HashType hash = GPUTextureCache::HashRect(hash_rect);
      if (it->first.src_hash != hash)
        continue;
    }

    GPUTexture* texture = GetTextureReplacementGPUImage(it->second.second);
    if (!texture)
      continue;

    const GSVector2 scale = GSVector2(texture->GetSizeVec()) / GSVector2(name.GetSizeVec());
    replacements.push_back(TextureReplacementSubImage{rect_in_page_space, GSVector4i::loadh(name.GetSizeVec()), texture,
                                                      scale.x, scale.y, name.IsSemitransparent()});
  }
}

std::optional<GPUTextureCache::TextureReplacementType>
GPUTextureCache::GetTextureReplacementTypeFromFileTitle(const std::string_view path)
{
  if (path.starts_with("vram-write-"))
    return TextureReplacementType::VRAMReplacement;

  if (path.starts_with("texupload-"))
    return TextureReplacementType::TextureFromVRAMWrite;

  if (path.starts_with("texpage-"))
    return TextureReplacementType::TextureFromPage;

  return std::nullopt;
}

bool GPUTextureCache::HasValidReplacementExtension(const std::string_view path)
{
  const std::string_view extension = Path::GetExtension(path);
  for (const char* test_extension : {"png", "jpg", "webp"})
  {
    if (StringUtil::EqualNoCase(extension, test_extension))
      return true;
  }

  return false;
}

void GPUTextureCache::FindTextureReplacements(bool load_vram_write_replacements, bool load_texture_replacements,
                                              bool prefill_dumped_texture_list, bool prefill_dumped_vram_list)
{
  if (GPUThread::GetGameSerial().empty())
    return;

  FileSystem::FindResultsArray files;
  FileSystem::FindFiles(GetTextureReplacementDirectory().c_str(), "*",
                        FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_RECURSIVE, &files);

  const bool add_texture_replacements_to_dumped =
    prefill_dumped_texture_list && !g_gpu_settings.texture_replacements.dump_replaced_textures;
  const bool add_vram_replacements_to_dumped =
    prefill_dumped_vram_list && !g_gpu_settings.texture_replacements.dump_replaced_textures;

  for (FILESYSTEM_FIND_DATA& fd : files)
  {
    if ((fd.Attributes & FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY) || !HasValidReplacementExtension(fd.FileName))
      continue;

    const std::string_view file_title = Path::GetFileTitle(fd.FileName);
    const std::optional<TextureReplacementType> type = GetTextureReplacementTypeFromFileTitle(file_title);
    if (!type.has_value())
      continue;

    switch (type.value())
    {
      case TextureReplacementType::VRAMReplacement:
      {
        VRAMReplacementName name;
        if (!name.Parse(file_title))
          continue;

        if (add_vram_replacements_to_dumped)
          s_state.dumped_vram_writes.insert(name);

        if (load_vram_write_replacements)
        {
          if (const auto it = s_state.vram_replacements.find(name); it != s_state.vram_replacements.end())
          {
            WARNING_LOG("Duplicate VRAM replacement: '{}' and '{}'", Path::GetFileName(it->second),
                        Path::GetFileName(fd.FileName));
            continue;
          }

          s_state.vram_replacements.emplace(name, std::move(fd.FileName));
        }
      }
      break;

      case TextureReplacementType::TextureFromVRAMWrite:
      case TextureReplacementType::TextureFromPage:
      {
        TextureReplacementName name;
        if (!name.Parse(file_title))
          continue;

        if (add_texture_replacements_to_dumped)
          s_state.dumped_textures.insert(DumpedTextureKey::FromName(name));

        if (load_texture_replacements)
        {
          DebugAssert(name.type == type.value());

          const TextureReplacementIndex index = name.GetIndex();
          TextureReplacementMap& dest_map = (type.value() == TextureReplacementType::TextureFromVRAMWrite) ?
                                              s_state.vram_write_texture_replacements :
                                              s_state.texture_page_texture_replacements;

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
      }
      break;

        DefaultCaseIsUnreachable()
    }
  }

  if (g_gpu_settings.texture_replacements.enable_texture_replacements)
  {
    INFO_LOG("Found {} replacement upload textures for '{}'", s_state.vram_write_texture_replacements.size(),
             GPUThread::GetGameSerial());
    INFO_LOG("Found {} replacement page textures for '{}'", s_state.texture_page_texture_replacements.size(),
             GPUThread::GetGameSerial());
  }

  if (g_gpu_settings.texture_replacements.enable_vram_write_replacements)
    INFO_LOG("Found {} replacement VRAM for '{}'", s_state.vram_replacements.size(), GPUThread::GetGameSerial());

  // if we're dumping, need to prefill the dumped list with those in the dumps directory as well
  if (prefill_dumped_texture_list || prefill_dumped_vram_list)
  {
    FileSystem::FindFiles(GetTextureDumpDirectory().c_str(), "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_RECURSIVE,
                          &files);

    for (FILESYSTEM_FIND_DATA& fd : files)
    {
      if ((fd.Attributes & FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY) || !HasValidReplacementExtension(fd.FileName))
        continue;

      const std::string_view file_title = Path::GetFileTitle(fd.FileName);
      const std::optional<TextureReplacementType> type = GetTextureReplacementTypeFromFileTitle(file_title);
      if (!type.has_value())
        continue;

      switch (type.value())
      {
        case TextureReplacementType::VRAMReplacement:
        {
          VRAMReplacementName name;
          if (!name.Parse(file_title))
            continue;

          if (prefill_dumped_vram_list)
            s_state.dumped_vram_writes.insert(name);
        }
        break;

        case TextureReplacementType::TextureFromVRAMWrite:
        case TextureReplacementType::TextureFromPage:
        {
          TextureReplacementName name;
          if (!name.Parse(file_title))
            continue;

          if (prefill_dumped_texture_list)
            s_state.dumped_textures.insert(DumpedTextureKey::FromName(name));
        }
        break;

          DefaultCaseIsUnreachable()
      }
    }
  }
}

void GPUTextureCache::LoadTextureReplacementAliases(const ryml::ConstNodeRef& root,
                                                    bool load_vram_write_replacement_aliases,
                                                    bool load_texture_replacement_aliases)
{
  if (GPUThread::GetGameSerial().empty())
    return;

  const std::string source_dir = GetTextureReplacementDirectory();

  for (const ryml::ConstNodeRef& current : root.cchildren())
  {
    const std::string_view key = to_stringview(current.key());
    const std::optional<TextureReplacementType> type = GetTextureReplacementTypeFromFileTitle(key);
    if (!type.has_value())
      continue;

    const std::string_view replacement_filename = to_stringview(current.val());
    std::string replacement_path = Path::Combine(source_dir, replacement_filename);
    if (!FileSystem::FileExists(replacement_path.c_str()))
    {
      ERROR_LOG("File '{}' for alias '{}' does not exist.", key, replacement_filename);
      continue;
    }

    switch (type.value())
    {
      case TextureReplacementType::VRAMReplacement:
      {
        VRAMReplacementName name;
        if (!load_vram_write_replacement_aliases || !name.Parse(key))
          continue;

        if (const auto it = s_state.vram_replacements.find(name); it != s_state.vram_replacements.end())
        {
          WARNING_LOG("Duplicate VRAM replacement alias: '{}' and '{}'", Path::GetFileName(it->second),
                      replacement_filename);
          continue;
        }

        s_state.vram_replacements.emplace(name, std::move(replacement_path));
      }
      break;

      case TextureReplacementType::TextureFromVRAMWrite:
      case TextureReplacementType::TextureFromPage:
      {
        TextureReplacementName name;
        if (!load_texture_replacement_aliases || !name.Parse(key))
          continue;

        DebugAssert(name.type == type.value());

        const TextureReplacementIndex index = name.GetIndex();
        TextureReplacementMap& dest_map = (type.value() == TextureReplacementType::TextureFromVRAMWrite) ?
                                            s_state.vram_write_texture_replacements :
                                            s_state.texture_page_texture_replacements;

        // Multiple replacements in the same write are fine. But they should have different rects.
        const auto range = dest_map.equal_range(index);
        bool duplicate = false;
        for (auto it = range.first; it != range.second; ++it)
        {
          if (it->second.first == name) [[unlikely]]
          {
            WARNING_LOG("Duplicate texture replacement alias: '{}' and '{}'", Path::GetFileName(it->second.second),
                        replacement_filename);
            duplicate = true;
          }
        }
        if (duplicate) [[unlikely]]
          continue;

        dest_map.emplace(index, std::make_pair(name, std::move(replacement_path)));
      }
      break;

        DefaultCaseIsUnreachable()
    }
  }

  if (g_gpu_settings.texture_replacements.enable_texture_replacements)
  {
    INFO_LOG("Found {} replacement upload textures after applying aliases for '{}'",
             s_state.vram_write_texture_replacements.size(), GPUThread::GetGameSerial());
    INFO_LOG("Found {} replacement page textures after applying aliases for '{}'",
             s_state.texture_page_texture_replacements.size(), GPUThread::GetGameSerial());
  }

  if (g_gpu_settings.texture_replacements.enable_vram_write_replacements)
  {
    INFO_LOG("Found {} replacement VRAM after applying aliases for '{}'", s_state.vram_replacements.size(),
             GPUThread::GetGameSerial());
  }
}

const GPUTextureCache::TextureReplacementImage* GPUTextureCache::GetTextureReplacementImage(const std::string& path)
{
  auto it = s_state.replacement_image_cache.find(path);
  if (it != s_state.replacement_image_cache.end())
    return &it->second;

  Image image;
  Error error;
  if (!image.LoadFromFile(path.c_str(), &error))
  {
    ERROR_LOG("Failed to load '{}': {}", Path::GetFileName(path), error.GetDescription());
    return nullptr;
  }

  VERBOSE_LOG("Loaded '{}': {}x{} {}", Path::GetFileName(path), image.GetWidth(), image.GetHeight(),
              Image::GetFormatName(image.GetFormat()));
  it = s_state.replacement_image_cache.emplace(path, std::move(image)).first;
  return &it->second;
}

GPUTexture* GPUTextureCache::GetTextureReplacementGPUImage(const std::string& path)
{
  // Already in cache?
  const auto git = s_state.gpu_replacement_image_cache.find(path);
  if (git != s_state.gpu_replacement_image_cache.end())
  {
    git->second.second = System::GetFrameNumber();
    return git->second.first.get();
  }

  // Need to upload it.
  Error error;
  std::unique_ptr<GPUTexture> tex;

  // Check CPU cache first.
  const auto it = s_state.replacement_image_cache.find(path);
  if (it != s_state.replacement_image_cache.end())
  {
    tex = g_gpu_device->FetchAndUploadTextureImage(it->second, GPUTexture::Flags::None, &error);
  }
  else
  {
    // Need to load it.
    Image cpu_image;
    if (cpu_image.LoadFromFile(path.c_str(), &error))
      tex = g_gpu_device->FetchAndUploadTextureImage(cpu_image, GPUTexture::Flags::None, &error);
  }

  if (!tex)
  {
    ERROR_LOG("Failed to load/upload '{}': {}", Path::GetFileName(path), error.GetDescription());
    return nullptr;
  }

  const size_t vram_usage = tex->GetVRAMUsage();
  s_state.gpu_replacement_image_cache_vram_usage += vram_usage;

  VERBOSE_LOG("Uploaded '{}': {}x{} {} {:.2f} KB", Path::GetFileName(path), tex->GetWidth(), tex->GetHeight(),
              GPUTexture::GetFormatName(tex->GetFormat()), static_cast<float>(vram_usage) / 1024.0f);

  return s_state.gpu_replacement_image_cache.emplace(path, std::make_pair(std::move(tex), System::GetFrameNumber()))
    .first->second.first.get();
}

void GPUTextureCache::CompactTextureReplacementGPUImages()
{
  // Instead of compacting to exactly the maximum, let's go down to the maximum less 16MB.
  // That way we can hopefully avoid compacting again for a few frames.
  static constexpr size_t EXTRA_COMPACT_SIZE = 16 * 1024 * 1024;

  const size_t max_usage = static_cast<size_t>(s_state.config.max_replacement_cache_vram_usage_mb) * 1048576;
  if (s_state.gpu_replacement_image_cache_vram_usage <= max_usage)
    return;

  DEV_LOG("Compacting replacement GPU image cache, count = {}, size = {:.1f} MB",
          s_state.gpu_replacement_image_cache.size(),
          static_cast<float>(s_state.gpu_replacement_image_cache_vram_usage) / 1048576.0f);

  const u32 frame_number = System::GetFrameNumber();
  s_state.gpu_replacement_image_cache_purge_list.reserve(s_state.gpu_replacement_image_cache.size());
  for (auto it = s_state.gpu_replacement_image_cache.begin(); it != s_state.gpu_replacement_image_cache.end(); ++it)
    s_state.gpu_replacement_image_cache_purge_list.emplace_back(it, frame_number - it->second.second);

  // Reverse sort, put the oldest on the end.
  std::sort(s_state.gpu_replacement_image_cache_purge_list.begin(),
            s_state.gpu_replacement_image_cache_purge_list.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.second > rhs.second; });

  // See first comment above.
  const size_t target_size = (max_usage < EXTRA_COMPACT_SIZE) ? max_usage : (max_usage - EXTRA_COMPACT_SIZE);
  while (s_state.gpu_replacement_image_cache_vram_usage > target_size &&
         !s_state.gpu_replacement_image_cache_purge_list.empty())
  {
    GPUReplacementImageCache::iterator iter = s_state.gpu_replacement_image_cache_purge_list.back().first;
    s_state.gpu_replacement_image_cache_purge_list.pop_back();

    std::unique_ptr<GPUTexture> tex = std::move(iter->second.first);
    s_state.gpu_replacement_image_cache.erase(iter);
    s_state.gpu_replacement_image_cache_vram_usage -= tex->GetVRAMUsage();
    g_gpu_device->RecycleTexture(std::move(tex));
  }

  s_state.gpu_replacement_image_cache_purge_list.clear();

  DEV_LOG("Finished compacting replacement GPU image cache, count = {}, size = {:.1f} MB",
          s_state.gpu_replacement_image_cache.size(),
          static_cast<float>(s_state.gpu_replacement_image_cache_vram_usage) / 1048576.0f);
}

void GPUTextureCache::PreloadReplacementTextures()
{
  static constexpr float UPDATE_INTERVAL = 1.0f;

  Timer last_update_time;
  u32 num_textures_loaded = 0;
  const size_t total_textures = s_state.vram_replacements.size() + s_state.vram_write_texture_replacements.size() +
                                s_state.texture_page_texture_replacements.size();
  std::string image_path = System::GetImageForLoadingScreen(GPUThread::GetGamePath());

#define UPDATE_PROGRESS()                                                                                              \
  if (last_update_time.GetTimeSeconds() >= UPDATE_INTERVAL)                                                            \
  {                                                                                                                    \
    FullscreenUI::RenderLoadingScreen(                                                                                 \
      image_path, TRANSLATE_SV("GPU_HW", "Preloading replacement textures..."),                                        \
      TinyString::from_format(TRANSLATE_FS("GPU_HW", "{0} of {1} textures"), num_textures_loaded, total_textures), 0,  \
      static_cast<int>(total_textures), static_cast<int>(num_textures_loaded));                                        \
    last_update_time.Reset();                                                                                          \
  }

  for (const auto& it : s_state.vram_replacements)
  {
    UPDATE_PROGRESS();
    GetTextureReplacementImage(it.second);
    num_textures_loaded++;
  }

#define PROCESS_MAP(map)                                                                                               \
  for (const auto& it : map)                                                                                           \
  {                                                                                                                    \
    UPDATE_PROGRESS();                                                                                                 \
    GetTextureReplacementImage(it.second.second);                                                                      \
    num_textures_loaded++;                                                                                             \
  }

  PROCESS_MAP(s_state.vram_write_texture_replacements);
  PROCESS_MAP(s_state.texture_page_texture_replacements);
#undef PROCESS_MAP
#undef UPDATE_PROGRESS
}

bool GPUTextureCache::EnsureGameDirectoryExists()
{
  if (GPUThread::GetGameSerial().empty())
    return false;

  const std::string game_directory = Path::Combine(EmuFolders::Textures, GPUThread::GetGameSerial());
  if (FileSystem::DirectoryExists(game_directory.c_str()))
    return true;

  Error error;
  if (!FileSystem::CreateDirectory(game_directory.c_str(), false, &error))
  {
    ERROR_LOG("Failed to create game directory: {}", error.GetDescription());
    return false;
  }

  if (const std::string config_path = Path::Combine(game_directory, LOCAL_CONFIG_FILENAME);
      !FileSystem::FileExists(config_path.c_str()) &&
      !FileSystem::WriteStringToFile(config_path.c_str(),
                                     Settings::TextureReplacementSettings().config.ExportToYAML(true), &error))
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

std::string GPUTextureCache::GetTextureReplacementDirectory()
{
  const std::string& serial = GPUThread::GetGameSerial();
  std::string dir =
    Path::Combine(EmuFolders::Textures, SmallString::from_format("{}" FS_OSPATH_SEPARATOR_STR "replacements", serial));
  if (!FileSystem::DirectoryExists(dir.c_str()))
  {
    // Check for the old directory structure without a replacements subdirectory.
    std::string altdir = Path::Combine(EmuFolders::Textures, serial);
    if (FileSystem::DirectoryExists(altdir.c_str()))
    {
      WARNING_LOG("Using deprecated texture replacement directory {}", altdir);
      dir = std::move(altdir);
    }
    else
    {
      // If this is a multi-disc game, try the first disc.
      const GameDatabase::Entry* dbentry = GameDatabase::GetEntryForSerial(serial);
      if (dbentry && dbentry->disc_set && serial != dbentry->disc_set->serials.front())
      {
        altdir =
          Path::Combine(EmuFolders::Textures, SmallString::from_format("{}" FS_OSPATH_SEPARATOR_STR "replacements",
                                                                       dbentry->disc_set->serials.front()));
        if (FileSystem::DirectoryExists(altdir.c_str()))
        {
          WARNING_LOG("Using texture replacements from first disc {}", dbentry->disc_set->serials.front());
          dir = std::move(altdir);
        }
      }
    }
  }

  return dir;
}

std::string GPUTextureCache::GetTextureDumpDirectory()
{
  return Path::Combine(EmuFolders::Textures,
                       SmallString::from_format("{}" FS_OSPATH_SEPARATOR_STR "dumps", GPUThread::GetGameSerial()));
}

GPUTextureCache::VRAMReplacementName GPUTextureCache::GetVRAMWriteHash(u32 width, u32 height, const void* pixels)
{
  const XXH128_hash_t hash = XXH3_128bits(pixels, width * height * sizeof(u16));
  return {hash.low64, hash.high64};
}

std::string GPUTextureCache::GetVRAMWriteDumpPath(const VRAMReplacementName& name)
{
  std::string ret;
  if (!EnsureGameDirectoryExists())
    return ret;

  const std::string dump_directory = GetTextureDumpDirectory();
  if (!FileSystem::EnsureDirectoryExists(dump_directory.c_str(), false))
    return ret;

  return Path::Combine(dump_directory, SmallString::from_format("vram-write-{}.png", name.ToString()));
}

bool GPUTextureCache::LoadLocalConfiguration(bool load_vram_write_replacement_aliases,
                                             bool load_texture_replacement_aliases)
{
  const Settings::TextureReplacementSettings::Configuration old_config = s_state.config;

  // load settings from ini
  s_state.config = g_gpu_settings.texture_replacements.config;

  const std::string& game_serial = GPUThread::GetGameSerial();
  if (game_serial.empty())
    return (s_state.config != old_config);

  const std::optional<std::string> ini_data = FileSystem::ReadFileToString(
    Path::Combine(EmuFolders::Textures,
                  SmallString::from_format("{}" FS_OSPATH_SEPARATOR_STR "{}", game_serial, LOCAL_CONFIG_FILENAME))
      .c_str());
  if (!ini_data.has_value() || ini_data->empty())
    return (s_state.config != old_config);

  const ryml::Tree tree = ryml::parse_in_arena(LOCAL_CONFIG_FILENAME, to_csubstr(ini_data.value()));
  const ryml::ConstNodeRef root = tree.rootref();

  // This is false if all we have are comments
  if (!root.is_map())
    return (s_state.config != old_config);

  s_state.config.dump_texture_pages = GetOptionalTFromObject<bool>(root, "DumpTexturePages")
                                        .value_or(static_cast<bool>(s_state.config.dump_texture_pages));
  s_state.config.dump_full_texture_pages = GetOptionalTFromObject<bool>(root, "DumpFullTexturePages")
                                             .value_or(static_cast<bool>(s_state.config.dump_full_texture_pages));
  s_state.config.dump_texture_force_alpha_channel =
    GetOptionalTFromObject<bool>(root, "DumpTextureForceAlphaChannel")
      .value_or(static_cast<bool>(s_state.config.dump_texture_force_alpha_channel));
  s_state.config.dump_vram_write_force_alpha_channel =
    GetOptionalTFromObject<bool>(root, "DumpVRAMWriteForceAlphaChannel")
      .value_or(static_cast<bool>(s_state.config.dump_vram_write_force_alpha_channel));
  s_state.config.dump_c16_textures =
    GetOptionalTFromObject<bool>(root, "DumpC16Textures").value_or(static_cast<bool>(s_state.config.dump_c16_textures));
  s_state.config.reduce_palette_range = GetOptionalTFromObject<bool>(root, "ReducePaletteRange")
                                          .value_or(static_cast<bool>(s_state.config.reduce_palette_range));
  s_state.config.convert_copies_to_writes = GetOptionalTFromObject<bool>(root, "ConvertCopiesToWrites")
                                              .value_or(static_cast<bool>(s_state.config.convert_copies_to_writes));
  s_state.config.max_vram_write_splits =
    GetOptionalTFromObject<bool>(root, "MaxVRAMWriteSplits").value_or(s_state.config.max_vram_write_splits);
  s_state.config.max_vram_write_coalesce_width = GetOptionalTFromObject<u16>(root, "MaxVRAMWriteCoalesceWidth")
                                                   .value_or(s_state.config.max_vram_write_coalesce_width);
  s_state.config.max_vram_write_coalesce_height = GetOptionalTFromObject<u16>(root, "MaxVRAMWriteCoalesceHeight")
                                                    .value_or(s_state.config.max_vram_write_coalesce_height);
  s_state.config.texture_dump_width_threshold = GetOptionalTFromObject<u16>(root, "DumpTextureWidthThreshold")
                                                  .value_or(s_state.config.texture_dump_width_threshold);
  s_state.config.texture_dump_height_threshold = GetOptionalTFromObject<u16>(root, "DumpTextureHeightThreshold")
                                                   .value_or(s_state.config.texture_dump_height_threshold);
  s_state.config.vram_write_dump_width_threshold = GetOptionalTFromObject<u16>(root, "DumpVRAMWriteWidthThreshold")
                                                     .value_or(s_state.config.vram_write_dump_width_threshold);
  s_state.config.vram_write_dump_height_threshold = GetOptionalTFromObject<u16>(root, "DumpVRAMWriteHeightThreshold")
                                                      .value_or(s_state.config.vram_write_dump_height_threshold);
  s_state.config.max_hash_cache_entries =
    GetOptionalTFromObject<u32>(root, "MaxHashCacheEntries").value_or(s_state.config.max_hash_cache_entries);
  s_state.config.max_hash_cache_vram_usage_mb =
    GetOptionalTFromObject<u32>(root, "MaxHashCacheVRAMUsageMB").value_or(s_state.config.max_hash_cache_vram_usage_mb);
  s_state.config.max_replacement_cache_vram_usage_mb = GetOptionalTFromObject<u32>(root, "MaxReplacementCacheVRAMUsage")
                                                         .value_or(s_state.config.max_replacement_cache_vram_usage_mb);
  s_state.config.replacement_scale_linear_filter =
    GetOptionalTFromObject<bool>(root, "ReplacementScaleLinearFilter")
      .value_or(static_cast<bool>(s_state.config.replacement_scale_linear_filter));

  if (load_vram_write_replacement_aliases || load_texture_replacement_aliases)
  {
    const ryml::ConstNodeRef aliases = root.find_child("Aliases");
    if (aliases.valid() && aliases.has_children())
      LoadTextureReplacementAliases(aliases, load_vram_write_replacement_aliases, load_texture_replacement_aliases);
  }

  // Any change?
  return (s_state.config != old_config);
}

void GPUTextureCache::ReloadTextureReplacements(bool show_info, bool show_info_if_none)
{
  s_state.dumped_textures.clear();
  s_state.dumped_vram_writes.clear();
  s_state.vram_replacements.clear();
  s_state.vram_write_texture_replacements.clear();
  s_state.texture_page_texture_replacements.clear();

  const bool load_vram_write_replacements = (g_gpu_settings.texture_replacements.enable_vram_write_replacements);
  const bool load_texture_replacements =
    (g_gpu_settings.gpu_texture_cache && g_gpu_settings.texture_replacements.enable_texture_replacements);
  const bool prefill_dumped_texture_list =
    (g_gpu_settings.texture_replacements.dump_vram_writes || g_gpu_settings.texture_replacements.dump_textures);
  const bool prefill_dumped_vram_list =
    (g_gpu_settings.texture_replacements.dump_vram_writes || g_gpu_settings.texture_replacements.dump_textures);
  if (load_vram_write_replacements || load_texture_replacements || prefill_dumped_texture_list ||
      prefill_dumped_vram_list)
  {
    FindTextureReplacements(load_vram_write_replacements, load_texture_replacements, prefill_dumped_texture_list,
                            prefill_dumped_vram_list);
  }

  LoadLocalConfiguration(load_vram_write_replacements, load_texture_replacements);

  if (g_gpu_settings.texture_replacements.preload_textures)
    PreloadReplacementTextures();

  PurgeUnreferencedTexturesFromCache();

  UpdateVRAMTrackingState();
  InvalidateSources();

  if (show_info)
  {
    const int total =
      static_cast<int>(s_state.vram_replacements.size() + s_state.vram_write_texture_replacements.size() +
                       s_state.texture_page_texture_replacements.size());
    if (total > 0 || show_info_if_none)
    {
      Host::AddIconOSDMessage("ReloadTextureReplacements", ICON_FA_IMAGES,
                              (total > 0) ? TRANSLATE_PLURAL_STR("GPU_HW", "%n replacement textures found.",
                                                                 "Replacement texture count", total) :
                                            TRANSLATE_STR("GPU_HW", "No replacement textures found."),
                              Host::OSD_INFO_DURATION);
    }
  }
}

void GPUTextureCache::PurgeUnreferencedTexturesFromCache()
{
  ReplacementImageCache old_map = std::move(s_state.replacement_image_cache);
  GPUReplacementImageCache old_gpu_map = std::move(s_state.gpu_replacement_image_cache);
  s_state.replacement_image_cache = ReplacementImageCache();
  s_state.gpu_replacement_image_cache = GPUReplacementImageCache();

  const auto reinsert_texture = [&old_map, &old_gpu_map](const std::string& name) {
    const auto it2 = old_map.find(name);
    if (it2 != old_map.end())
    {
      s_state.replacement_image_cache.emplace(name, std::move(it2->second));
      old_map.erase(it2);
    }

    const auto it3 = old_gpu_map.find(name);
    if (it3 != old_gpu_map.end())
    {
      s_state.gpu_replacement_image_cache.emplace(name, std::move(it3->second));
      old_gpu_map.erase(it3);
    }
  };

  for (const auto& it : s_state.vram_replacements)
    reinsert_texture(it.second);

  for (const auto& it : s_state.vram_write_texture_replacements)
    reinsert_texture(it.second.second);

  for (const auto& it : s_state.texture_page_texture_replacements)
    reinsert_texture(it.second.second);
}

void GPUTextureCache::ApplyTextureReplacements(SourceKey key, HashType tex_hash, HashType pal_hash,
                                               HashCacheEntry* entry)
{
  std::vector<TextureReplacementSubImage> subimages;
  if (HasTexturePageTextureReplacements())
  {
    GetTexturePageTextureReplacements(subimages, key.page, tex_hash, pal_hash, key.mode, key.palette);
  }

  if (HasVRAMWriteTextureReplacements())
  {
    // Wrapping around the edge for replacement testing breaks 8/16-bit textures in the rightmost page.
    const GSVector4i page_rect = GetTextureRectWithoutWrap(key.page, key.mode);
    LoopRectPages(page_rect, [&key, &pal_hash, &subimages, &page_rect](u32 pn) {
      const PageEntry& page = s_state.pages[pn];
      ListIterate(page.writes, [&key, &pal_hash, &subimages, &page_rect](const VRAMWrite* vrw) {
        // TODO: Is this needed?
        if (!vrw->write_rect.rintersects(page_rect))
          return;

        // Map VRAM write to the start of the page.
        GSVector2i offset_to_page = page_rect.sub32(vrw->write_rect).xy();

        // Need to apply the texture shift on the X dimension, not Y. No SLLV on SSE4.. :(
        offset_to_page.x = ApplyTextureModeShift(key.mode, offset_to_page.x);

        GetVRAMWriteTextureReplacements(subimages, vrw->hash, pal_hash, key.mode, key.palette, offset_to_page);
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
  const u32 new_width = static_cast<u32>(std::ceil(static_cast<float>(TEXTURE_PAGE_WIDTH) * max_scale_x));
  const u32 new_height = static_cast<u32>(std::ceil(static_cast<float>(TEXTURE_PAGE_HEIGHT) * max_scale_y));
  if (!s_state.replacement_texture_render_target || s_state.replacement_texture_render_target->GetWidth() < new_width ||
      s_state.replacement_texture_render_target->GetHeight() < new_height)
  {
    // NOTE: Not recycled, it's unlikely to be reused.
    s_state.replacement_texture_render_target.reset();
    if (!(s_state.replacement_texture_render_target =
            g_gpu_device->CreateTexture(new_width, new_height, 1, 1, 1, GPUTexture::Type::RenderTarget,
                                        REPLACEMENT_TEXTURE_FORMAT, GPUTexture::Flags::None)))
    {
      ERROR_LOG("Failed to create {}x{} render target.", new_width, new_height);
      return;
    }
  }

  // Grab the actual texture beforehand, in case we OOM.
  std::unique_ptr<GPUTexture> replacement_tex = FetchTexture(new_width, new_height, 1, 1, 1, GPUTexture::Type::Texture,
                                                             REPLACEMENT_TEXTURE_FORMAT, GPUTexture::Flags::None);
  if (!replacement_tex)
  {
    ERROR_LOG("Failed to create {}x{} texture.", new_width, new_height);
    return;
  }

  GL_SCOPE_FMT("ApplyTextureReplacements({:016X}, {:016X}) => {}x{}", tex_hash, pal_hash, replacement_tex->GetWidth(),
               replacement_tex->GetHeight());

  // TODO: Use rects instead of fullscreen tris, maybe avoid the copy..
  g_gpu_device->InvalidateRenderTarget(s_state.replacement_texture_render_target.get());
  g_gpu_device->SetRenderTarget(s_state.replacement_texture_render_target.get());

  GL_INS("Upscale Texture Page");
  alignas(VECTOR_ALIGNMENT) float uniforms[8];
  GSVector2 texture_size = GSVector2(GSVector2i(entry->texture->GetWidth(), entry->texture->GetHeight()));
  GSVector4::store<true>(&uniforms[0], GSVector4::cxpr(0.0f, 0.0f, 1.0f, 1.0f));
  GSVector2::store<true>(&uniforms[4], texture_size);
  GSVector2::store<true>(&uniforms[6], GSVector2::cxpr(1.0f) / texture_size);
  g_gpu_device->SetViewportAndScissor(0, 0, new_width, new_height);
  g_gpu_device->SetPipeline(s_state.replacement_upscale_pipeline.get());
  g_gpu_device->SetTextureSampler(0, entry->texture.get(), g_gpu_device->GetNearestSampler());
  g_gpu_device->DrawWithPushConstants(3, 0, uniforms, sizeof(uniforms));

  for (const TextureReplacementSubImage& si : subimages)
  {
    GL_INS_FMT("Blit {}x{} replacement from {} to {}", si.texture->GetWidth(), si.texture->GetHeight(), si.src_rect,
               si.dst_rect);

    const GSVector4 src_rect = (GSVector4(GSVector4i::xyxy(si.src_rect.xy(), si.src_rect.rsize())) *
                                GSVector4::xyxy(GSVector2(si.scale_x, si.scale_y))) /
                               GSVector4(GSVector4i::xyxy(si.texture->GetSizeVec()));
    const GSVector4i dst_rect = GSVector4i(GSVector4(si.dst_rect) * max_scale_v);
    texture_size = GSVector2(si.texture->GetSizeVec());
    GSVector4::store<true>(&uniforms[0], src_rect);
    GSVector2::store<true>(&uniforms[4], texture_size);
    GSVector2::store<true>(&uniforms[6], GSVector2::cxpr(1.0f) / texture_size);
    g_gpu_device->SetViewportAndScissor(dst_rect);
    g_gpu_device->SetTextureSampler(0, si.texture,
                                    s_state.config.replacement_scale_linear_filter ? g_gpu_device->GetLinearSampler() :
                                                                                     g_gpu_device->GetNearestSampler());
    g_gpu_device->SetPipeline(si.invert_alpha ? s_state.replacement_semitransparent_draw_pipeline.get() :
                                                s_state.replacement_draw_pipeline.get());
    g_gpu_device->DrawWithPushConstants(3, 0, uniforms, sizeof(uniforms));
  }

  g_gpu_device->CopyTextureRegion(replacement_tex.get(), 0, 0, 0, 0, s_state.replacement_texture_render_target.get(), 0,
                                  0, 0, 0, new_width, new_height);
  g_gpu_device->RecycleTexture(std::move(entry->texture));
  entry->texture = std::move(replacement_tex);

  s_state.hw_backend->RestoreDeviceContext();
}