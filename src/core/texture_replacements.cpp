#include "texture_replacements.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/platform.h"
#include "common/string_util.h"
#include "common/timer.h"
#include "fmt/format.h"
#include "gpu_hw.h"
#include "host.h"
#include "settings.h"
#include "system.h"
#include "xxhash.h"
#if defined(CPU_X86) || defined(CPU_X64)
#include "xxh_x86dispatch.h"
#endif
#include <cinttypes>
Log_SetChannel(TextureReplacements);

TextureReplacements g_texture_replacements;

static GPU_HW* GetHWGPU()
{
  return static_cast<GPU_HW*>(g_gpu.get());
}

u32 TextureReplacements::GetScaledReplacementTextureWidth()
{
  return TEXTURE_REPLACEMENT_PAGE_WIDTH * g_settings.texture_replacements.replacement_texture_scale;
}

u32 TextureReplacements::GetScaledReplacementTextureHeight()
{
  return TEXTURE_REPLACEMENT_PAGE_HEIGHT * g_settings.texture_replacements.replacement_texture_scale;
}

std::string TextureReplacementHash::ToString() const
{
  return StringUtil::StdStringFromFormat("%016" PRIx64 "%016" PRIx64, high, low);
}

bool TextureReplacementHash::ParseString(const std::string_view& sv)
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

TextureReplacements::TextureReplacements() = default;

TextureReplacements::~TextureReplacements() = default;

void TextureReplacements::InvalidateReplacementTextures()
{
  Assert(g_settings.texture_replacements.enable_texture_replacements);

  // TODO: Clear cached textures.

  if (GetHWGPU()->IsTextureReplacementEnabled())
    GetHWGPU()->InvalidateTextureReplacements();
}

void TextureReplacements::ReuploadReplacementTextures()
{
  Assert(g_settings.texture_replacements.enable_texture_replacements);
}

void TextureReplacements::OnSystemReset()
{
  if (g_settings.texture_replacements.enable_texture_replacements)
    InvalidateReplacementTextures();
}

const TextureReplacementTexture* TextureReplacements::GetVRAMWriteReplacement(u32 width, u32 height, const void* pixels)
{
  const TextureReplacementHash hash = GetVRAMWriteHash(width, height, pixels);

  const auto it = m_vram_write_replacements.find(hash);
  if (it == m_vram_write_replacements.end())
    return nullptr;

  return LoadTexture(it->second);
}

static constexpr std::array<u32, 4> s_texture_mode_shifts = {{2, 1, 0}};
static constexpr std::array<u32, 4> s_texture_mode_revshifts = {{0, 1, 2}};
static constexpr std::array<u32, 4> s_texture_mode_masks = {{3, 1, 0}};

void TextureReplacements::TransformTextureCoordinates(GPUTextureMode mode, u32 vram_x, u32 vram_y, u32 vram_width,
                                                      u32 vram_height, u32* out_vram_width, u32* out_vram_height,
                                                      u32* page_index, u32* page_offset_x, u32* page_offset_y,
                                                      u32* page_width, u32* page_height)
{
  const u32 shift = s_texture_mode_shifts[static_cast<u32>(mode)];
  const u32 revshift = s_texture_mode_revshifts[static_cast<u32>(mode)];
  const u32 page_x = vram_x / TEXTURE_REPLACEMENT_PAGE_VRAM_WIDTH;
  const u32 page_y = vram_y / TEXTURE_REPLACEMENT_PAGE_VRAM_HEIGHT;

  *page_index = GetTextureReplacementPageIndex(page_x, page_y);

  *page_offset_x = (vram_x % TEXTURE_REPLACEMENT_PAGE_VRAM_WIDTH) << shift;
  *page_offset_y = (vram_y % TEXTURE_REPLACEMENT_PAGE_VRAM_HEIGHT);

  *page_width = std::min<u32>(vram_width << shift, (TEXTURE_REPLACEMENT_PAGE_WIDTH >> revshift) - *page_offset_x);
  *page_height = std::min<u32>(vram_height, TEXTURE_REPLACEMENT_PAGE_HEIGHT - *page_offset_y);

  *out_vram_width = *page_width >> shift;
  *out_vram_height = *page_height;
}

void TextureReplacements::UntransformTextureCoordinates(GPUTextureMode mode, u32 texture_width, u32 texture_height,
                                                        u32 vram_x, u32 vram_y, u32* out_vram_width,
                                                        u32* out_vram_height, u32* page_index, u32* page_offset_x,
                                                        u32* page_offset_y, u32* page_width, u32* page_height)
{
  const u32 shift = s_texture_mode_shifts[static_cast<u32>(mode)];
  const u32 revshift = s_texture_mode_revshifts[static_cast<u32>(mode)];
  const u32 page_x = vram_x / TEXTURE_REPLACEMENT_PAGE_VRAM_WIDTH;
  const u32 page_y = vram_y / TEXTURE_REPLACEMENT_PAGE_VRAM_HEIGHT;

  *page_index = GetTextureReplacementPageIndex(page_x, page_y);

  *page_offset_x = (vram_x % TEXTURE_REPLACEMENT_PAGE_VRAM_WIDTH) << shift;
  *page_offset_y = (vram_y % TEXTURE_REPLACEMENT_PAGE_VRAM_HEIGHT);

  *page_width = std::min<u32>(texture_width, (TEXTURE_REPLACEMENT_PAGE_WIDTH >> revshift) - *page_offset_x);
  *page_height = std::min<u32>(texture_height, TEXTURE_REPLACEMENT_PAGE_HEIGHT - *page_offset_y);

  *out_vram_width = (*page_width + s_texture_mode_masks[static_cast<u32>(mode)]) >> shift;
  *out_vram_height = *page_height;
}

void TextureReplacements::InvalidateReplacementTexture(GPUTextureMode mode, u32 vram_x, u32 vram_y, u32 width,
                                                       u32 height)
{
  const u32 scale = g_settings.texture_replacements.replacement_texture_scale;

  while (height > 0)
  {
    u32 current_x = vram_x;
    u32 remaining_width = width;
    u32 consume_height = height;

    while (remaining_width > 0)
    {
      u32 consume_width, page_index, page_offset_x, page_offset_y, page_width, page_height;
      TransformTextureCoordinates(mode, current_x, vram_y, remaining_width, height, &consume_width, &consume_height,
                                  &page_index, &page_offset_x, &page_offset_y, &page_width, &page_height);

      const u32 dummy_data_size = (page_width * scale) * (page_height * scale);
      if (m_texture_replacement_invalidate_buffer.size() < dummy_data_size)
        m_texture_replacement_invalidate_buffer.resize(dummy_data_size);

      GetHWGPU()->UploadTextureReplacement(page_index, page_offset_x * scale, page_offset_y * scale, page_width * scale,
                                           page_height * scale, m_texture_replacement_invalidate_buffer.data(),
                                           page_width * scale * sizeof(u32));

      current_x += consume_width;
      remaining_width -= consume_width;
    }

    vram_y += consume_height;
    height -= consume_height;
  }
}

void TextureReplacements::UploadReplacementTexture(const TextureReplacementTexture* texture,
                                                   const ReplacementEntry& entry, u32 vram_x, u32 vram_y)
{
  const u32 scale = g_settings.texture_replacements.replacement_texture_scale;
  const u32 unscaled_width = (entry.width << s_texture_mode_shifts[static_cast<u32>(entry.mode)]);
  const u32 scaled_width = unscaled_width * scale;
  const u32 scaled_height = entry.height * scale;

  if (texture->GetWidth() != scaled_width || texture->GetHeight() != scaled_height)
  {
    Log_VerbosePrintf("Resizing replacement texture from %ux%u to %ux%u", texture->GetWidth(), texture->GetHeight(),
                      scaled_width, scaled_height);

    TextureReplacementTexture resized_texture;
    resized_texture.ResizeFrom(texture, scaled_width, scaled_height);
    UploadReplacementTexture(&resized_texture, entry, vram_x, vram_y);
    return;
  }

  // this is the tricky part
  u32 current_y = vram_y + entry.offset_y;
  u32 remaining_height = entry.height;
  u32 texture_y = 0;

  while (remaining_height > 0)
  {
    u32 current_x = vram_x + entry.offset_x;
    u32 remaining_width = unscaled_width;
    u32 consume_height = remaining_height;

    u32 texture_x = 0;

    while (remaining_width > 0)
    {
      u32 consume_width, page_index, page_offset_x, page_offset_y, page_width, page_height;
      UntransformTextureCoordinates(entry.mode, remaining_width, remaining_height, current_x, current_y, &consume_width,
                                    &consume_height, &page_index, &page_offset_x, &page_offset_y, &page_width,
                                    &page_height);

      const u32 upload_width = std::min(page_width, remaining_width);
      const u32 upload_height = std::min(page_height, remaining_height);
      const u32 scaled_upload_width = upload_width * scale;
      const u32 scaled_upload_height = upload_height * scale;
      const u32 scaled_page_offset_x = page_offset_x * scale;
      const u32 scaled_page_offset_y = page_offset_y * scale;

      Log_InfoPrintf("Uploading %ux%u to replacement page %u @ %u,%u", scaled_upload_width, scaled_upload_height,
                     page_index, scaled_page_offset_x, scaled_page_offset_y);

      GetHWGPU()->UploadTextureReplacement(page_index, scaled_page_offset_x, scaled_page_offset_y, scaled_upload_width,
                                           scaled_upload_height, texture->GetRowPixels(texture_y) + texture_x,
                                           texture->GetPitch());

      texture_x += scaled_upload_width;
      remaining_width -= upload_width;
      current_x += consume_width;
      consume_height = upload_height;
    }

    remaining_height -= consume_height;
    texture_y += consume_height;
    vram_y += consume_height;
  }
}

void TextureReplacements::UploadReplacementTextures(u32 vram_x, u32 vram_y, u32 width, u32 height, const void* pixels)
{
  const TextureReplacementHash hash = GetVRAMWriteHash(width, height, pixels);

  const auto [lower, upper] = m_texture_replacements.equal_range(hash);
  if (lower == upper)
  {
    InvalidateReplacementTexture(GPUTextureMode::Palette4Bit, vram_x, vram_y, width, height);
    return;
  }

  for (auto iter = lower; iter != upper; ++iter)
  {
    const ReplacementEntry& entry = iter->second;

    const TextureReplacementTexture* texture = LoadTexture(entry.filename);
    if (!texture)
    {
      InvalidateReplacementTexture(entry.mode, vram_x + entry.offset_x, vram_y + entry.offset_y, entry.width,
                                   entry.height);
      continue;
    }

    UploadReplacementTexture(texture, entry, vram_x, vram_y);
  }
}

void TextureReplacements::Shutdown()
{
  m_texture_cache.clear();
  m_vram_write_replacements.clear();
  m_texture_replacements.clear();
  decltype(m_texture_replacement_invalidate_buffer)().swap(m_texture_replacement_invalidate_buffer);
}

std::string TextureReplacements::GetSourceDirectory() const
{
  const std::string& code = System::GetRunningCode();
  if (code.empty())
    return EmuFolders::Textures;
  else
    return Path::Combine(EmuFolders::Textures, code);
}

TextureReplacementHash TextureReplacements::GetVRAMWriteHash(u32 width, u32 height, const void* pixels)
{
  XXH128_hash_t hash = XXH3_128bits(pixels, width * height * sizeof(u16));
  return {hash.low64, hash.high64};
}

void TextureReplacements::Reload()
{
  m_vram_write_replacements.clear();
  m_texture_replacements.clear();

  if (!g_gpu || !g_gpu->IsHardwareRenderer())
  {
    m_texture_cache.clear();
    return;
  }

  if (g_settings.texture_replacements.AnyReplacementsEnabled())
    FindTextures(GetSourceDirectory());

  if (g_settings.texture_replacements.preload_textures)
    PreloadTextures();

  PurgeUnreferencedTexturesFromCache();

  if (g_settings.texture_replacements.enable_texture_replacements)
    ReuploadReplacementTextures();
}

void TextureReplacements::PurgeUnreferencedTexturesFromCache()
{
  TextureCache old_map = std::move(m_texture_cache);
  for (const auto& it : m_vram_write_replacements)
  {
    auto it2 = old_map.find(it.second);
    if (it2 != old_map.end())
    {
      m_texture_cache[it.second] = std::move(it2->second);
      old_map.erase(it2);
    }
  }

  for (const auto& it : m_texture_replacements)
  {
    auto it2 = old_map.find(it.second.filename);
    if (it2 != old_map.end())
    {
      m_texture_cache[it.second.filename] = std::move(it2->second);
      old_map.erase(it2);
    }
  }
}

bool TextureReplacements::ParseReplacementFilename(const std::string& filename,
                                                   TextureReplacementHash* replacement_hash,
                                                   ReplacmentType* replacement_type, GPUTextureMode* out_mode,
                                                   u16* out_offset_x, u16* out_offset_y, u16* out_width,
                                                   u16* out_height)
{
  const char* title = std::strrchr(filename.c_str(), '/');
#ifdef _WIN32
  const char* title2 = std::strrchr(filename.c_str(), '\\');
  if (title2 && (!title || title2 > title))
    title = title2;
#endif

  if (!title)
    return false;

  title++;

  const char* hashpart;
  const char* extension;

  if (StringUtil::Strncasecmp(title, "vram-write-", 11) == 0)
  {
    hashpart = title + 11;
    if (std::strlen(hashpart) < 32)
      return false;

    if (!replacement_hash->ParseString(std::string_view(hashpart, 32)))
      return false;

    *replacement_type = ReplacmentType::VRAMWrite;
    *out_mode = GPUTextureMode::Direct16Bit;
    *out_offset_x = 0;
    *out_offset_y = 0;
    *out_width = 0;
    *out_height = 0;

    extension = hashpart + 32;
  }
  else if (StringUtil::Strncasecmp(title, "texture-", 8) == 0)
  {
    // TODO: Make this much better...
    hashpart = title + 8;
    if (std::strlen(hashpart) < 42)
      return false;

    if (!replacement_hash->ParseString(std::string_view(hashpart, 32)))
      return false;

    const char* datapart = hashpart + 33;
    unsigned file_mode, file_offset_x, file_offset_y, file_width, file_height;
    if (std::sscanf(datapart, "%u-%u-%u-%u-%u", &file_mode, &file_offset_x, &file_offset_y, &file_width,
                    &file_height) != 5)
    {
      return false;
    }

    if (file_mode > static_cast<unsigned>(GPUTextureMode::Reserved_Direct16Bit) || file_offset_x >= VRAM_WIDTH ||
        (file_offset_x + file_width) > VRAM_WIDTH || file_offset_y >= VRAM_HEIGHT ||
        (file_offset_y + file_height) > VRAM_HEIGHT)
    {
      return false;
    }

    *replacement_type = ReplacmentType::Texture;
    *out_mode = static_cast<GPUTextureMode>(file_mode);
    *out_offset_x = static_cast<u16>(file_offset_x);
    *out_offset_y = static_cast<u16>(file_offset_y);
    *out_width = static_cast<u16>(file_width);
    *out_height = static_cast<u16>(file_height);

    extension = std::strchr(datapart, '.');
  }
  else
  {
    return false;
  }

  if (!extension || *extension == '\0')
    return false;

  extension++;

  bool valid_extension = false;
  for (const char* test_extension : {"png", "jpg", "tga", "bmp"})
  {
    if (StringUtil::Strcasecmp(extension, test_extension) == 0)
    {
      valid_extension = true;
      break;
    }
  }

  return valid_extension;
}

void TextureReplacements::FindTextures(const std::string& dir)
{
  const bool recursive = !System::GetRunningCode().empty();

  FileSystem::FindResultsArray files;
  FileSystem::FindFiles(dir.c_str(), "*",
                        recursive ? (FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_RECURSIVE) : (FILESYSTEM_FIND_FILES),
                        &files);

  for (FILESYSTEM_FIND_DATA& fd : files)
  {
    if (fd.Attributes & FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY)
      continue;

    TextureReplacementHash hash;
    ReplacmentType type;
    GPUTextureMode texture_mode;
    u16 texture_offset_x, texture_offset_y;
    u16 texture_width, texture_height;
    if (!ParseReplacementFilename(fd.FileName, &hash, &type, &texture_mode, &texture_offset_x, &texture_offset_y,
                                  &texture_width, &texture_height))
    {
      continue;
    }

    switch (type)
    {
      case ReplacmentType::VRAMWrite:
      {
        if (!g_settings.texture_replacements.enable_vram_write_replacements)
          continue;

        auto it = m_vram_write_replacements.find(hash);
        if (it != m_vram_write_replacements.end())
        {
          Log_WarningPrintf("Duplicate VRAM write replacement: '%s' and '%s'", it->second.c_str(), fd.FileName.c_str());
          continue;
        }

        m_vram_write_replacements.emplace(hash, std::move(fd.FileName));
      }
      break;

      case ReplacmentType::Texture:
      {
        if (!g_settings.texture_replacements.enable_texture_replacements)
          continue;

        ReplacementEntry entry;
        entry.filename = std::move(fd.FileName);
        entry.mode = texture_mode;
        entry.offset_x = texture_offset_x;
        entry.offset_y = texture_offset_y;
        entry.width = texture_width;
        entry.height = texture_height;
        m_texture_replacements.emplace(hash, std::move(entry));
      }
      break;
    }
  }

  if (g_settings.texture_replacements.enable_vram_write_replacements)
    Log_InfoPrintf("Found %zu replacement VRAM writes", m_vram_write_replacements.size());

  if (g_settings.texture_replacements.enable_texture_replacements)
    Log_InfoPrintf("Found %zu replacement textures", m_texture_replacements.size());
}

const TextureReplacementTexture* TextureReplacements::LoadTexture(const std::string& filename)
{
  auto it = m_texture_cache.find(filename);
  if (it != m_texture_cache.end())
    return &it->second;

  Common::RGBA8Image image;
  if (!image.LoadFromFile(filename.c_str()))
  {
    Log_ErrorPrintf("Failed to load '%s'", filename.c_str());
    return nullptr;
  }

  Log_InfoPrintf("Loaded '%s': %ux%u", filename.c_str(), image.GetWidth(), image.GetHeight());
  it = m_texture_cache.emplace(filename, std::move(image)).first;
  return &it->second;
}

void TextureReplacements::PreloadTextures()
{
  static constexpr float UPDATE_INTERVAL = 1.0f;

  Common::Timer last_update_time;
  u32 num_textures_loaded = 0;
  const u32 total_textures = static_cast<u32>(m_vram_write_replacements.size());

#define UPDATE_PROGRESS()                                                                                              \
  if (last_update_time.GetTimeSeconds() >= UPDATE_INTERVAL)                                                            \
  {                                                                                                                    \
    Host::DisplayLoadingScreen("Preloading replacement textures...", 0, static_cast<int>(total_textures),              \
                               static_cast<int>(num_textures_loaded));                                                 \
    last_update_time.Reset();                                                                                          \
  }

  for (const auto& it : m_vram_write_replacements)
  {
    UPDATE_PROGRESS();

    LoadTexture(it.second);
    num_textures_loaded++;
  }

#undef UPDATE_PROGRESS
}
