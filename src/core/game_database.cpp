// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "game_database.h"
#include "controller.h"
#include "host.h"
#include "system.h"

#include "util/cd_image.h"
#include "util/imgui_manager.h"

#include "common/assert.h"
#include "common/binary_reader_writer.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/heterogeneous_containers.h"
#include "common/log.h"
#include "common/path.h"
#include "common/ryml_helpers.h"
#include "common/string_util.h"
#include "common/timer.h"

#include "ryml.hpp"

#include <bit>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <type_traits>

#include "IconsEmoji.h"
#include "IconsFontAwesome5.h"
#include "fmt/format.h"

LOG_CHANNEL(GameDatabase);

namespace GameDatabase {

enum : u32
{
  GAME_DATABASE_CACHE_SIGNATURE = 0x45434C48,
  GAME_DATABASE_CACHE_VERSION = 26,
};

static const Entry* GetEntryForId(std::string_view code);

static bool LoadFromCache();
static bool SaveToCache();

static bool LoadGameDBYaml();
static bool ParseYamlEntry(Entry* entry, const ryml::ConstNodeRef& value);
static bool ParseYamlCodes(PreferUnorderedStringMap<std::string_view>& lookup, const ryml::ConstNodeRef& value,
                           std::string_view serial);
static bool LoadTrackHashes();

static constexpr const std::array<const char*, static_cast<int>(CompatibilityRating::Count)>
  s_compatibility_rating_names = {{
    "Unknown",
    "DoesntBoot",
    "CrashesInIntro",
    "CrashesInGame",
    "GraphicalAudioIssues",
    "NoIssues",
  }};

static constexpr const std::array<const char*, static_cast<size_t>(CompatibilityRating::Count)>
  s_compatibility_rating_display_names = {{
    TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Unknown", "CompatibilityRating"),
    TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Doesn't Boot", "CompatibilityRating"),
    TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Crashes In Intro", "CompatibilityRating"),
    TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Crashes In-Game", "CompatibilityRating"),
    TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Graphical/Audio Issues", "CompatibilityRating"),
    TRANSLATE_DISAMBIG_NOOP("GameDatabase", "No Issues", "CompatibilityRating"),
  }};

static constexpr const std::array s_trait_names = {
  "ForceInterpreter",
  "ForceSoftwareRenderer",
  "ForceSoftwareRendererForReadbacks",
  "ForceRoundTextureCoordinates",
  "ForceShaderBlending",
  "ForceFullTrueColor",
  "ForceDeinterlacing",
  "ForceFullBoot",
  "DisableAutoAnalogMode",
  "DisableMultitap",
  "DisableCDROMReadSpeedup",
  "DisableCDROMSeekSpeedup",
  "DisableTrueColor",
  "DisableFullTrueColor",
  "DisableUpscaling",
  "DisableTextureFiltering",
  "DisableSpriteTextureFiltering",
  "DisableScaledDithering",
  "DisableScaledInterlacing",
  "DisableWidescreen",
  "DisablePGXP",
  "DisablePGXPCulling",
  "DisablePGXPTextureCorrection",
  "DisablePGXPColorCorrection",
  "DisablePGXPDepthBuffer",
  "DisablePGXPOn2DPolygons",
  "ForcePGXPVertexCache",
  "ForcePGXPCPUMode",
  "ForceRecompilerICache",
  "ForceCDROMSubQSkew",
  "IsLibCryptProtected",
};
static_assert(s_trait_names.size() == static_cast<size_t>(Trait::MaxCount));

static constexpr const std::array s_trait_display_names = {
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Force Interpreter", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Force Software Renderer", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Force Software Renderer For Readbacks", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Force Round Texture Coordinates", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Force Shader Blending", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Force Full True Color", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Force Deinterlacing", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Force Full Boot", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Disable Automatic Analog Mode", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Disable Multitap", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Disable CD-ROM Read Speedup", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Disable CD-ROM Seek Speedup", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Disable True Color", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Disable Full True Color", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Disable Upscaling", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Disable Texture Filtering", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Disable Sprite Texture Filtering", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Disable Scaled Dithering", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Disable Scaled Interlacing", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Disable Widescreen", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Disable PGXP", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Disable PGXP Culling", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Disable PGXP Texture Correction", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Disable PGXP Color Correction", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Disable PGXP Depth Buffer", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Disable PGXP on 2D Polygons", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Force PGXP Vertex Cache", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Force PGXP CPU Mode", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Force Recompiler ICache", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Force CD-ROM SubQ Skew", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Is LibCrypt Protected", "GameDatabase::Trait"),
};
static_assert(s_trait_display_names.size() == static_cast<size_t>(Trait::MaxCount));

static constexpr std::array s_language_names = {
  "Catalan", "Chinese",    "Czech",   "Danish",  "Dutch",   "English",  "Finnish", "French",
  "German",  "Greek",      "Hebrew",  "Iranian", "Italian", "Japanese", "Korean",  "Norwegian",
  "Polish",  "Portuguese", "Russian", "Spanish", "Swedish", "Turkish",
};
static_assert(s_language_names.size() == static_cast<size_t>(Language::MaxCount));

static constexpr const char* GAMEDB_YAML_FILENAME = "gamedb.yaml";
static constexpr const char* DISCDB_YAML_FILENAME = "discdb.yaml";
static DisplayDeinterlacingMode DEFAULT_DEINTERLACING_MODE = DisplayDeinterlacingMode::Adaptive;

static bool s_loaded = false;
static bool s_track_hashes_loaded = false;

static DynamicHeapArray<u8> s_db_data; // we take strings from the data, so store a copy
static std::vector<GameDatabase::Entry> s_entries;
static PreferUnorderedStringMap<u32> s_code_lookup;

static TrackHashesMap s_track_hashes_map;
} // namespace GameDatabase

void GameDatabase::EnsureLoaded()
{
  if (s_loaded)
    return;

  Timer timer;

  s_loaded = true;

  if (!LoadFromCache())
  {
    s_entries = {};
    s_code_lookup = {};

    if (LoadGameDBYaml())
    {
      SaveToCache();
    }
    else
    {
      s_entries = {};
      s_code_lookup = {};
    }
  }

  INFO_LOG("Database load of {} entries took {:.0f}ms.", s_entries.size(), timer.GetTimeMilliseconds());
}

void GameDatabase::Unload()
{
  s_entries = {};
  s_code_lookup = {};
  s_loaded = false;
}

const GameDatabase::Entry* GameDatabase::GetEntryForId(std::string_view code)
{
  if (code.empty())
    return nullptr;

  EnsureLoaded();

  auto iter = s_code_lookup.find(code);
  return (iter != s_code_lookup.end()) ? &s_entries[iter->second] : nullptr;
}

std::string GameDatabase::GetSerialForDisc(CDImage* image)
{
  std::string ret;

  const GameDatabase::Entry* entry = GetEntryForDisc(image);
  if (entry)
    ret = entry->serial;

  return ret;
}

std::string GameDatabase::GetSerialForPath(const char* path)
{
  std::string ret;

  if (System::IsLoadablePath(path) && !System::IsExePath(path) && !System::IsPsfPath(path))
  {
    std::unique_ptr<CDImage> image(CDImage::Open(path, false, nullptr));
    if (image)
      ret = GetSerialForDisc(image.get());
  }

  return ret;
}

const GameDatabase::Entry* GameDatabase::GetEntryForDisc(CDImage* image)
{
  std::string id;
  GameHash hash;
  System::GetGameDetailsFromImage(image, &id, &hash);
  const Entry* entry = GetEntryForGameDetails(id, hash);
  if (entry)
    return entry;

  WARNING_LOG("No entry found for disc '{}'", id);
  return nullptr;
}

const GameDatabase::Entry* GameDatabase::GetEntryForGameDetails(const std::string& id, u64 hash)
{
  const Entry* entry;

  if (!id.empty())
  {
    entry = GetEntryForId(id);
    if (entry)
      return entry;
  }

  // some games with invalid serials use the hash
  entry = GetEntryForId(System::GetGameHashId(hash));
  if (entry)
    return entry;

  return nullptr;
}

const GameDatabase::Entry* GameDatabase::GetEntryForSerial(std::string_view serial)
{
  if (serial.empty())
    return nullptr;

  EnsureLoaded();

  const auto it =
    std::lower_bound(s_entries.cbegin(), s_entries.cend(), serial,
                     [](const Entry& entry, const std::string_view& search) { return (entry.serial < search); });
  return (it != s_entries.end() && it->serial == serial) ? &(*it) : nullptr;
}

const char* GameDatabase::GetTraitName(Trait trait)
{
  return s_trait_names[static_cast<size_t>(trait)];
}

const char* GameDatabase::GetTraitDisplayName(Trait trait)
{
  return Host::TranslateToCString("GameDatabase", s_trait_display_names[static_cast<size_t>(trait)]);
}

const char* GameDatabase::GetCompatibilityRatingName(CompatibilityRating rating)
{
  return s_compatibility_rating_names[static_cast<int>(rating)];
}

const char* GameDatabase::GetCompatibilityRatingDisplayName(CompatibilityRating rating)
{
  return (rating >= CompatibilityRating::Unknown && rating < CompatibilityRating::Count) ?
           Host::TranslateToCString("GameDatabase", s_compatibility_rating_display_names[static_cast<size_t>(rating)],
                                    "CompatibilityRating") :
           "";
}

const char* GameDatabase::GetLanguageName(Language language)
{
  return s_language_names[static_cast<size_t>(language)];
}

std::optional<GameDatabase::Language> GameDatabase::ParseLanguageName(std::string_view str)
{
  for (size_t i = 0; i < static_cast<size_t>(Language::MaxCount); i++)
  {
    if (str == s_language_names[i])
      return static_cast<Language>(i);
  }

  return std::nullopt;
}

TinyString GameDatabase::GetLanguageFlagResourceName(std::string_view language_name)
{
  return TinyString::from_format("images/flags/{}.svg", language_name);
}

std::string_view GameDatabase::Entry::GetLanguageFlagName(DiscRegion region) const
{
  // If there's only one language, this is the flag we want to use.
  // Except if it's English, then we want to use the disc region's flag.
  std::string_view ret;
  if (languages.count() == 1 && !languages.test(static_cast<size_t>(GameDatabase::Language::English)))
    ret = GameDatabase::GetLanguageName(static_cast<GameDatabase::Language>(std::countr_zero(languages.to_ulong())));
  else
    ret = Settings::GetDiscRegionName(region);

  return ret;
}

SmallString GameDatabase::Entry::GetLanguagesString() const
{
  SmallString ret;

  bool first = true;
  for (u32 i = 0; i < static_cast<u32>(Language::MaxCount); i++)
  {
    if (languages.test(i))
    {
      ret.append_format("{}{}", first ? "" : ", ", GetLanguageName(static_cast<Language>(i)));
      first = false;
    }
  }

  if (ret.empty())
    ret.append(TRANSLATE_SV("GameDatabase", "Unknown"));

  return ret;
}

void GameDatabase::Entry::ApplySettings(Settings& settings, bool display_osd_messages) const
{
  if (display_active_start_offset.has_value())
  {
    settings.display_active_start_offset = display_active_start_offset.value();
    if (display_osd_messages)
      INFO_LOG("GameDB: Display active start offset set to {}.", settings.display_active_start_offset);
  }
  if (display_active_end_offset.has_value())
  {
    settings.display_active_end_offset = display_active_end_offset.value();
    if (display_osd_messages)
      INFO_LOG("GameDB: Display active end offset set to {}.", settings.display_active_end_offset);
  }
  if (display_line_start_offset.has_value())
  {
    settings.display_line_start_offset = display_line_start_offset.value();
    if (display_osd_messages)
      INFO_LOG("GameDB: Display line start offset set to {}.", settings.display_line_start_offset);
  }
  if (display_line_end_offset.has_value())
  {
    settings.display_line_end_offset = display_line_end_offset.value();
    if (display_osd_messages)
      INFO_LOG("GameDB: Display line end offset set to {}.", settings.display_line_start_offset);
  }
  if (dma_max_slice_ticks.has_value())
  {
    settings.dma_max_slice_ticks = dma_max_slice_ticks.value();
    if (display_osd_messages)
      INFO_LOG("GameDB: DMA max slice ticks set to {}.", settings.dma_max_slice_ticks);
  }
  if (dma_halt_ticks.has_value())
  {
    settings.dma_halt_ticks = dma_halt_ticks.value();
    if (display_osd_messages)
      INFO_LOG("GameDB: DMA halt ticks set to {}.", settings.dma_halt_ticks);
  }
  if (cdrom_max_seek_speedup_cycles.has_value() && g_settings.cdrom_seek_speedup == 0)
  {
    settings.cdrom_max_seek_speedup_cycles = cdrom_max_seek_speedup_cycles.value();
    if (display_osd_messages)
      INFO_LOG("GameDB: CDROM maximum seek speedup cycles set to {}.", settings.cdrom_max_seek_speedup_cycles);
  }
  if (cdrom_max_read_speedup_cycles.has_value() && g_settings.cdrom_read_speedup == 0)
  {
    settings.cdrom_max_read_speedup_cycles = cdrom_max_read_speedup_cycles.value();
    if (display_osd_messages)
      INFO_LOG("GameDB: CDROM maximum read speedup cycles set to {}.", settings.cdrom_max_read_speedup_cycles);
  }
  if (gpu_fifo_size.has_value())
  {
    settings.gpu_fifo_size = gpu_fifo_size.value();
    if (display_osd_messages)
      INFO_LOG("GameDB: GPU FIFO size set to {}.", settings.gpu_fifo_size);
  }
  if (gpu_max_run_ahead.has_value())
  {
    settings.gpu_max_run_ahead = gpu_max_run_ahead.value();
    if (display_osd_messages)
      INFO_LOG("GameDB: GPU max runahead set to {}.", settings.gpu_max_run_ahead);
  }
  if (gpu_pgxp_tolerance.has_value())
  {
    settings.gpu_pgxp_tolerance = gpu_pgxp_tolerance.value();
    if (display_osd_messages)
      INFO_LOG("GameDB: GPU PGXP tolerance set to {}.", settings.gpu_pgxp_tolerance);
  }
  if (gpu_pgxp_depth_threshold.has_value())
  {
    settings.SetPGXPDepthClearThreshold(gpu_pgxp_depth_threshold.value());
    if (display_osd_messages)
      INFO_LOG("GameDB: GPU depth clear threshold set to {}.", settings.GetPGXPDepthClearThreshold());
  }
  if (gpu_line_detect_mode.has_value())
  {
    settings.gpu_line_detect_mode = gpu_line_detect_mode.value();
    if (display_osd_messages)
    {
      INFO_LOG("GameDB: GPU line detect mode set to {}.",
               Settings::GetLineDetectModeName(settings.gpu_line_detect_mode));
    }
  }

  SmallStackString<512> messages;
#define APPEND_MESSAGE(msg)                                                                                            \
  do                                                                                                                   \
  {                                                                                                                    \
    messages.append("\n        \u2022 ");                                                                              \
    messages.append(msg);                                                                                              \
  } while (0)
#define APPEND_MESSAGE_FMT(...)                                                                                        \
  do                                                                                                                   \
  {                                                                                                                    \
    messages.append("\n        \u2022 ");                                                                              \
    messages.append_format(__VA_ARGS__);                                                                               \
  } while (0)

  if (HasTrait(Trait::ForceInterpreter))
  {
    if (display_osd_messages && settings.cpu_execution_mode != CPUExecutionMode::Interpreter)
      APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "CPU recompiler disabled."));

    settings.cpu_execution_mode = CPUExecutionMode::Interpreter;
  }

  if (HasTrait(Trait::ForceFullBoot))
  {
    if (display_osd_messages && settings.bios_patch_fast_boot)
      APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "Fast boot disabled."));

    settings.bios_patch_fast_boot = false;
  }

  if (HasTrait(Trait::DisableMultitap))
  {
    if (display_osd_messages && settings.multitap_mode != MultitapMode::Disabled)
      APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "Multitap disabled."));

    settings.multitap_mode = MultitapMode::Disabled;
  }

  if (HasTrait(Trait::DisableCDROMReadSpeedup))
  {
    if (settings.cdrom_read_speedup != 1)
      APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "CD-ROM read speedup disabled."));

    settings.cdrom_read_speedup = 1;
  }

  if (HasTrait(Trait::DisableCDROMSeekSpeedup))
  {
    if (settings.cdrom_seek_speedup != 1)
      APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "CD-ROM seek speedup disabled."));

    settings.cdrom_seek_speedup = 1;
  }

  if (display_crop_mode.has_value())
  {
    if (display_osd_messages && settings.display_crop_mode != display_crop_mode.value())
    {
      APPEND_MESSAGE_FMT(TRANSLATE_FS("GameDatabase", "Display cropping set to {}."),
                         Settings::GetDisplayCropModeDisplayName(display_crop_mode.value()));
    }

    settings.display_crop_mode = display_crop_mode.value();
  }

  if (HasTrait(Trait::ForceSoftwareRenderer))
  {
    if (display_osd_messages && settings.gpu_renderer != GPURenderer::Software)
      APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "Hardware rendering disabled."));

    settings.gpu_renderer = GPURenderer::Software;
  }

  if (HasTrait(Trait::ForceSoftwareRendererForReadbacks))
  {
    if (display_osd_messages && settings.gpu_renderer != GPURenderer::Software)
      APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "Software renderer readbacks enabled."));

    settings.gpu_use_software_renderer_for_readbacks = true;
  }

  if (HasTrait(Trait::ForceRoundUpscaledTextureCoordinates))
  {
    settings.gpu_force_round_texcoords = true;
  }

  if (HasTrait(Trait::ForceDeinterlacing))
  {
    const DisplayDeinterlacingMode new_mode = display_deinterlacing_mode.value_or(
      (settings.display_deinterlacing_mode != DisplayDeinterlacingMode::Disabled &&
       settings.display_deinterlacing_mode != DisplayDeinterlacingMode::Progressive) ?
        settings.display_deinterlacing_mode :
        DEFAULT_DEINTERLACING_MODE);
    if (display_osd_messages && settings.display_deinterlacing_mode != new_mode)
    {
      APPEND_MESSAGE_FMT(TRANSLATE_FS("GameDatabase", "Deinterlacing set to {}."),
                         Settings::GetDisplayDeinterlacingModeDisplayName(new_mode));
    }

    settings.display_deinterlacing_mode = new_mode;
  }
  else if (display_deinterlacing_mode.has_value())
  {
    // If the user has it set to progressive, then preserve that.
    if (settings.display_deinterlacing_mode != DisplayDeinterlacingMode::Progressive)
    {
      if (display_osd_messages && settings.display_deinterlacing_mode != display_deinterlacing_mode.value())
      {
        APPEND_MESSAGE_FMT(TRANSLATE_FS("GameDatabase", "Deinterlacing set to {}."),
                           Settings::GetDisplayDeinterlacingModeDisplayName(display_deinterlacing_mode.value()));
      }

      settings.display_deinterlacing_mode = display_deinterlacing_mode.value();
    }
  }

  if (HasTrait(Trait::DisableTrueColor) || HasTrait(Trait::DisableFullTrueColor) ||
      HasTrait(Trait::DisableScaledDithering) || HasTrait(Trait::ForceShaderBlending) ||
      HasTrait(Trait::ForceFullTrueColor))
  {
    // Note: The order these are applied matters.
    const GPUDitheringMode old_mode = settings.gpu_dithering_mode;
    if (HasTrait(Trait::DisableTrueColor) && settings.IsUsingTrueColor())
    {
      settings.gpu_dithering_mode = GPUDitheringMode::Scaled;
    }
    if (HasTrait(Trait::DisableScaledDithering) && settings.IsUsingDithering())
    {
      settings.gpu_dithering_mode =
        (settings.IsUsingShaderBlending() ? GPUDitheringMode::UnscaledShaderBlend : GPUDitheringMode::Unscaled);
    }
    if (HasTrait(Trait::ForceShaderBlending) && settings.IsUsingDithering() && !settings.IsUsingShaderBlending())
    {
      settings.gpu_dithering_mode = (settings.gpu_dithering_mode == GPUDitheringMode::Scaled) ?
                                      GPUDitheringMode::ScaledShaderBlend :
                                      GPUDitheringMode::UnscaledShaderBlend;
    }
    if (HasTrait(Trait::ForceFullTrueColor) && settings.gpu_dithering_mode == GPUDitheringMode::TrueColor)
    {
      settings.gpu_dithering_mode = GPUDitheringMode::TrueColorFull;
    }
    if (HasTrait(Trait::DisableFullTrueColor) && settings.gpu_dithering_mode == GPUDitheringMode::TrueColorFull)
    {
      settings.gpu_dithering_mode = GPUDitheringMode::TrueColor;
    }

    if (display_osd_messages && settings.gpu_dithering_mode != old_mode)
    {
      APPEND_MESSAGE_FMT(TRANSLATE_FS("GameDatabase", "Dithering set to {}."),
                         Settings::GetGPUDitheringModeDisplayName(settings.gpu_dithering_mode));
    }
  }

  if (HasTrait(Trait::DisableUpscaling))
  {
    if (display_osd_messages)
    {
      if (settings.gpu_resolution_scale != 1)
        APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "Upscaling disabled."));
      if (settings.gpu_multisamples != 1)
        APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "MSAA disabled."));
    }

    settings.gpu_resolution_scale = 1;
    settings.gpu_automatic_resolution_scale = false;
    settings.gpu_multisamples = 1;
  }

  if (HasTrait(Trait::DisableTextureFiltering))
  {
    if (display_osd_messages && (settings.gpu_texture_filter != GPUTextureFilter::Nearest ||
                                 g_settings.gpu_sprite_texture_filter != GPUTextureFilter::Nearest))
    {
      APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "Texture filtering disabled."));
    }

    settings.gpu_texture_filter = GPUTextureFilter::Nearest;
    settings.gpu_sprite_texture_filter = GPUTextureFilter::Nearest;
  }

  if (HasTrait(Trait::DisableSpriteTextureFiltering))
  {
    if (display_osd_messages && g_settings.gpu_sprite_texture_filter != GPUTextureFilter::Nearest)
    {
      APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "Sprite texture filtering disabled."));
    }

    settings.gpu_sprite_texture_filter = GPUTextureFilter::Nearest;
  }

  if (HasTrait(Trait::DisableScaledInterlacing))
  {
    if (display_osd_messages && settings.gpu_scaled_interlacing &&
        settings.display_deinterlacing_mode != DisplayDeinterlacingMode::Progressive)
    {
      APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "Scaled interlacing disabled."));
    }

    settings.gpu_scaled_interlacing = false;
  }

  if (HasTrait(Trait::DisableWidescreen))
  {
    if (display_osd_messages && settings.gpu_widescreen_hack)
      APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "Widescreen rendering disabled."));

    settings.gpu_widescreen_hack = false;
  }

  if (HasTrait(Trait::DisablePGXP))
  {
    if (display_osd_messages && settings.gpu_pgxp_enable)
      APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "PGXP geometry correction disabled."));

    settings.gpu_pgxp_enable = false;
  }

  if (HasTrait(Trait::DisablePGXPCulling))
  {
    if (display_osd_messages && settings.gpu_pgxp_enable && settings.gpu_pgxp_culling)
      APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "PGXP culling correction disabled."));

    settings.gpu_pgxp_culling = false;
  }

  if (HasTrait(Trait::DisablePGXPTextureCorrection))
  {
    if (display_osd_messages && settings.gpu_pgxp_enable && settings.gpu_pgxp_texture_correction)
      APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "PGXP perspective correct textures disabled."));

    settings.gpu_pgxp_texture_correction = false;
  }

  if (HasTrait(Trait::DisablePGXPColorCorrection))
  {
    if (display_osd_messages && settings.gpu_pgxp_enable && settings.gpu_pgxp_texture_correction &&
        settings.gpu_pgxp_color_correction)
    {
      APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "PGXP perspective correct colors disabled."));
    }

    settings.gpu_pgxp_color_correction = false;
  }

  if (HasTrait(Trait::ForcePGXPVertexCache))
  {
    if (display_osd_messages && settings.gpu_pgxp_enable && !settings.gpu_pgxp_vertex_cache)
      APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "PGXP vertex cache enabled."));

    settings.gpu_pgxp_vertex_cache = settings.gpu_pgxp_enable;
  }
  else if (settings.gpu_pgxp_enable && settings.gpu_pgxp_vertex_cache)
  {
    Host::AddIconOSDWarning(
      "gamedb_force_pgxp_vertex_cache", ICON_EMOJI_WARNING,
      TRANSLATE_STR(
        "GameDatabase",
        "PGXP Vertex Cache is enabled, but it is not required for this game. This may cause rendering errors."),
      Host::OSD_WARNING_DURATION);
  }

  if (HasTrait(Trait::ForcePGXPCPUMode))
  {
    if (display_osd_messages && settings.gpu_pgxp_enable && !settings.gpu_pgxp_cpu)
    {
#ifndef __ANDROID__
      APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "PGXP CPU mode enabled."));
#else
      Host::AddIconOSDWarning("gamedb_force_pgxp_cpu", ICON_EMOJI_WARNING,
                              "This game requires PGXP CPU mode, which increases system requirements.\n"
                              "       If the game runs too slow, disable PGXP for this game.",
                              Host::OSD_WARNING_DURATION);
#endif
    }

    settings.gpu_pgxp_cpu = settings.gpu_pgxp_enable;
  }
  else if (settings.UsingPGXPCPUMode())
  {
    Host::AddIconOSDWarning(
      "gamedb_force_pgxp_cpu", ICON_EMOJI_WARNING,
      TRANSLATE_STR("GameDatabase",
                    "PGXP CPU mode is enabled, but it is not required for this game. This may cause rendering errors."),
      Host::OSD_WARNING_DURATION);
  }

  if (HasTrait(Trait::DisablePGXPDepthBuffer))
  {
    if (display_osd_messages && settings.gpu_pgxp_enable && settings.gpu_pgxp_depth_buffer)
      APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "PGXP depth buffer disabled."));

    settings.gpu_pgxp_depth_buffer = false;
  }

  if (HasTrait(Trait::DisablePGXPOn2DPolygons))
  {
    if (display_osd_messages && settings.gpu_pgxp_enable && !settings.gpu_pgxp_disable_2d)
      APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "PGXP disabled on 2D polygons."));

    g_settings.gpu_pgxp_disable_2d = true;
  }

  if (gpu_pgxp_preserve_proj_fp.has_value())
  {
    if (display_osd_messages)
    {
      INFO_LOG("GameDB: GPU preserve projection precision set to {}.",
               gpu_pgxp_preserve_proj_fp.value() ? "true" : "false");

      if (settings.gpu_pgxp_enable && settings.gpu_pgxp_preserve_proj_fp && !gpu_pgxp_preserve_proj_fp.value())
        APPEND_MESSAGE(TRANSLATE_SV("GameDatabase", "PGXP preserve projection precision disabled."));
    }

    settings.gpu_pgxp_preserve_proj_fp = gpu_pgxp_preserve_proj_fp.value();
  }

  if (HasTrait(Trait::ForceRecompilerICache))
  {
    WARNING_LOG("ICache for recompiler forced by compatibility settings.");
    settings.cpu_recompiler_icache = true;
  }

  if (HasTrait(Trait::ForceCDROMSubQSkew))
  {
    WARNING_LOG("CD-ROM SubQ Skew forced by compatibility settings.");
    settings.cdrom_subq_skew = true;
  }

  if (!messages.empty())
  {
    Host::AddIconOSDMessage(
      "GameDBCompatibility", ICON_EMOJI_INFORMATION,
      fmt::format("{}{}", TRANSLATE_SV("GameDatabase", "Compatibility settings for this game have been applied."),
                  messages.view()),
      Host::OSD_INFO_DURATION);
  }

#undef APPEND_MESSAGE_FMT
#undef APPEND_MESSAGE

#define BIT_FOR(ctype) (static_cast<u16>(1) << static_cast<u32>(ctype))

  if (supported_controllers != 0 && supported_controllers != static_cast<u16>(-1))
  {
    for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
    {
      const ControllerType ctype = settings.controller_types[i];
      if (ctype == ControllerType::None)
        continue;

      if (supported_controllers & BIT_FOR(ctype))
        continue;

      // Special case: Dualshock is permitted when not supported as long as it's in digital mode.
      if (ctype == ControllerType::AnalogController &&
          (supported_controllers & BIT_FOR(ControllerType::DigitalController)) != 0)
      {
        continue;
      }

      if (display_osd_messages)
      {
        SmallString supported_controller_string;
        for (u32 j = 0; j < static_cast<u32>(ControllerType::Count); j++)
        {
          const ControllerType supported_ctype = static_cast<ControllerType>(j);
          if ((supported_controllers & BIT_FOR(supported_ctype)) == 0)
            continue;

          if (!supported_controller_string.empty())
            supported_controller_string.append(", ");

          supported_controller_string.append(Controller::GetControllerInfo(supported_ctype).GetDisplayName());
        }

        Host::AddIconOSDWarning(
          fmt::format("GameDBController{}Unsupported", i), ICON_EMOJI_WARNING,
          fmt::format(
            TRANSLATE_FS("GameDatabase",
                         "Controller in Port {0} ({1}) is not supported for this game.\nSupported controllers: "
                         "{2}\nPlease configure a supported controller from the list above."),
            i + 1u, Controller::GetControllerInfo(ctype).GetDisplayName(), supported_controller_string),
          Host::OSD_CRITICAL_ERROR_DURATION);
      }
    }

    if (g_settings.multitap_mode != MultitapMode::Disabled && !(supported_controllers & SUPPORTS_MULTITAP_BIT))
    {
      Host::AddIconOSDMessage("GameDBMultitapUnsupported", ICON_EMOJI_WARNING,
                              TRANSLATE_STR("GameDatabase",
                                            "This game does not support multitap, but multitap is enabled.\n"
                                            "       This may result in dropped controller inputs."),
                              Host::OSD_CRITICAL_ERROR_DURATION);
    }
  }

#undef BIT_FOR
}

template<typename T>
static inline void AppendIntegerSetting(SmallStringBase& str, bool& heading, std::string_view title,
                                        const std::optional<T>& value)
{
  if (!value.has_value())
    return;

  if (!heading)
  {
    heading = true;
    str.append_format("**{}**\n\n", TRANSLATE_SV("GameDatabase", "Settings"));
  }

  str.append_format(" - {}: {}\n", title, value.value());
}

static inline void AppendFloatSetting(SmallStringBase& str, bool& heading, std::string_view title,
                                      const std::optional<float>& value)
{
  if (!value.has_value())
    return;

  if (!heading)
  {
    heading = true;
    str.append_format("**{}**\n\n", TRANSLATE_SV("GameDatabase", "Settings"));
  }

  str.append_format(" - {}: {:.2f}\n", title, value.value());
}

template<typename T>
static inline void AppendEnumSetting(SmallStringBase& str, bool& heading, std::string_view title,
                                     const char* (*get_display_name_func)(T), const std::optional<T>& value)
{
  if (!value.has_value())
    return;

  if (!heading)
  {
    heading = true;
    str.append_format("**{}**\n\n", TRANSLATE_SV("GameDatabase", "Settings"));
  }

  str.append_format(" - {}: {}\n", title, get_display_name_func(value.value()));
}

std::string GameDatabase::Entry::GenerateCompatibilityReport() const
{
  LargeString ret;
  ret.append_format("**{}:** {}\n\n", TRANSLATE_SV("GameDatabase", "Title"), title);
  ret.append_format("**{}:** {}\n\n", TRANSLATE_SV("GameDatabase", "Serial"), serial);

  if (languages.any())
    ret.append_format("**{}:** {}\n\n", TRANSLATE_SV("GameDatabase", "Languages"), GetLanguagesString());

  ret.append_format("**{}:** {}\n\n", TRANSLATE_SV("GameDatabase", "Rating"),
                    GetCompatibilityRatingDisplayName(compatibility));

  if (!compatibility_version_tested.empty())
    ret.append_format("**{}:**\n{}\n\n", TRANSLATE_SV("GameDatabase", "Version Tested"), compatibility_version_tested);

  if (!compatibility_comments.empty())
    ret.append_format("**{}**\n\n{}\n\n", TRANSLATE_SV("GameDatabase", "Comments"), compatibility_comments);

  if (supported_controllers != 0)
  {
    ret.append_format("**{}**\n\n", TRANSLATE_SV("GameDatabase", "Supported Controllers"));

    for (u32 j = 0; j < static_cast<u32>(ControllerType::Count); j++)
    {
      if ((supported_controllers & (static_cast<u16>(1) << j)) == 0)
        continue;

      ret.append_format(" - {}\n", Controller::GetControllerInfo(static_cast<ControllerType>(j)).GetDisplayName());
    }

    if (supported_controllers & SUPPORTS_MULTITAP_BIT)
      ret.append(" - Multitap\n");

    ret.append("\n");
  }

  if (traits.any())
  {
    ret.append_format("**{}**\n\n", TRANSLATE_SV("GameDatabase", "Traits"));
    for (u32 i = 0; i < static_cast<u32>(Trait::MaxCount); i++)
    {
      if (traits.test(i))
        ret.append_format(" - {}\n", GetTraitDisplayName(static_cast<Trait>(i)));
    }
    ret.append("\n");
  }

  bool settings_heading = false;
  AppendIntegerSetting(ret, settings_heading, TRANSLATE_SV("GameDatabase", "Display Active Start Offset"),
                       display_active_start_offset);
  AppendIntegerSetting(ret, settings_heading, TRANSLATE_SV("GameDatabase", "Display Active End Offset"),
                       display_active_end_offset);
  AppendIntegerSetting(ret, settings_heading, TRANSLATE_SV("GameDatabase", "Display Line Start Offset"),
                       display_line_start_offset);
  AppendIntegerSetting(ret, settings_heading, TRANSLATE_SV("GameDatabase", "Display Line End Offset"),
                       display_line_end_offset);
  AppendEnumSetting(ret, settings_heading, TRANSLATE_SV("GameDatabase", "Display Crop Mode"),
                    &Settings::GetDisplayCropModeDisplayName, display_crop_mode);
  AppendEnumSetting(ret, settings_heading, TRANSLATE_SV("GameDatabase", "Display Deinterlacing Mode"),
                    &Settings::GetDisplayDeinterlacingModeDisplayName, display_deinterlacing_mode);
  AppendIntegerSetting(ret, settings_heading, TRANSLATE_SV("GameDatabase", "DMA Max Slice Ticks"), dma_max_slice_ticks);
  AppendIntegerSetting(ret, settings_heading, TRANSLATE_SV("GameDatabase", "DMA Halt Ticks"), dma_halt_ticks);
  AppendIntegerSetting(ret, settings_heading, TRANSLATE_SV("GameDatabase", "GPU FIFO Size"), gpu_fifo_size);
  AppendIntegerSetting(ret, settings_heading, TRANSLATE_SV("GameDatabase", "GPU Max Runahead"), gpu_max_run_ahead);
  AppendFloatSetting(ret, settings_heading, TRANSLATE_SV("GameDatabase", "GPU PGXP Tolerance"), gpu_pgxp_tolerance);
  AppendFloatSetting(ret, settings_heading, TRANSLATE_SV("GameDatabase", "GPU PGXP Depth Threshold"),
                     gpu_pgxp_depth_threshold);
  AppendEnumSetting(ret, settings_heading, TRANSLATE_SV("GameDatabase", "GPU Line Detect Mode"),
                    &Settings::GetLineDetectModeDisplayName, gpu_line_detect_mode);

  if (!disc_set_name.empty())
  {
    ret.append_format("**{}:** {}\n", TRANSLATE_SV("GameDatabase", "Disc Set"), disc_set_name);
    for (const std::string& ds_serial : disc_set_serials)
      ret.append_format(" - {}\n", ds_serial);
  }

  return std::string(ret.view());
}

static std::string GetCacheFile()
{
  return Path::Combine(EmuFolders::Cache, "gamedb.cache");
}

bool GameDatabase::LoadFromCache()
{
  Error error;
  std::optional<DynamicHeapArray<u8>> db_data = FileSystem::ReadBinaryFile(GetCacheFile().c_str(), &error);
  if (!db_data.has_value())
  {
    DEV_LOG("Failed to read cache, loading full database: {}", error.GetDescription());
    return false;
  }

  BinarySpanReader reader(db_data->cspan());
  const u64 gamedb_ts = Host::GetResourceFileTimestamp("gamedb.yaml", false).value_or(0);

  u32 signature, version, num_entries, num_codes;
  u64 file_gamedb_ts;
  if (!reader.ReadU32(&signature) || !reader.ReadU32(&version) || !reader.ReadU64(&file_gamedb_ts) ||
      !reader.ReadU32(&num_entries) || !reader.ReadU32(&num_codes) || signature != GAME_DATABASE_CACHE_SIGNATURE ||
      version != GAME_DATABASE_CACHE_VERSION)
  {
    DEV_LOG("Cache header is corrupted or version mismatch.");
    return false;
  }

  if (gamedb_ts != file_gamedb_ts)
  {
    DEV_LOG("Cache is out of date, recreating.");
    return false;
  }

  s_entries.reserve(num_entries);

  for (u32 i = 0; i < num_entries; i++)
  {
    Entry& entry = s_entries.emplace_back();

    constexpr u32 trait_num_bytes = (static_cast<u32>(Trait::MaxCount) + 7) / 8;
    constexpr u32 language_num_bytes = (static_cast<u32>(Language::MaxCount) + 7) / 8;
    std::array<u8, trait_num_bytes> trait_bits;
    std::array<u8, language_num_bytes> language_bits;
    u8 compatibility;
    u32 num_disc_set_serials;

    if (!reader.ReadSizePrefixedString(&entry.serial) || !reader.ReadSizePrefixedString(&entry.title) ||
        !reader.ReadSizePrefixedString(&entry.genre) || !reader.ReadSizePrefixedString(&entry.developer) ||
        !reader.ReadSizePrefixedString(&entry.publisher) ||
        !reader.ReadSizePrefixedString(&entry.compatibility_version_tested) ||
        !reader.ReadSizePrefixedString(&entry.compatibility_comments) || !reader.ReadU64(&entry.release_date) ||
        !reader.ReadU8(&entry.min_players) || !reader.ReadU8(&entry.max_players) || !reader.ReadU8(&entry.min_blocks) ||
        !reader.ReadU8(&entry.max_blocks) || !reader.ReadU16(&entry.supported_controllers) ||
        !reader.ReadU8(&compatibility) || compatibility >= static_cast<u8>(GameDatabase::CompatibilityRating::Count) ||
        !reader.Read(trait_bits.data(), trait_num_bytes) || !reader.Read(language_bits.data(), language_num_bytes) ||
        !reader.ReadOptionalT(&entry.display_active_start_offset) ||
        !reader.ReadOptionalT(&entry.display_active_end_offset) ||
        !reader.ReadOptionalT(&entry.display_line_start_offset) ||
        !reader.ReadOptionalT(&entry.display_line_end_offset) || !reader.ReadOptionalT(&entry.display_crop_mode) ||
        !reader.ReadOptionalT(&entry.display_deinterlacing_mode) || !reader.ReadOptionalT(&entry.dma_max_slice_ticks) ||
        !reader.ReadOptionalT(&entry.dma_halt_ticks) || !reader.ReadOptionalT(&entry.cdrom_max_seek_speedup_cycles) ||
        !reader.ReadOptionalT(&entry.cdrom_max_read_speedup_cycles) || !reader.ReadOptionalT(&entry.gpu_fifo_size) ||
        !reader.ReadOptionalT(&entry.gpu_max_run_ahead) || !reader.ReadOptionalT(&entry.gpu_pgxp_tolerance) ||
        !reader.ReadOptionalT(&entry.gpu_pgxp_depth_threshold) ||
        !reader.ReadOptionalT(&entry.gpu_pgxp_preserve_proj_fp) || !reader.ReadOptionalT(&entry.gpu_line_detect_mode) ||
        !reader.ReadSizePrefixedString(&entry.disc_set_name) || !reader.ReadU32(&num_disc_set_serials))
    {
      DEV_LOG("Cache entry is corrupted.");
      return false;
    }

    if (num_disc_set_serials > 0)
    {
      entry.disc_set_serials.reserve(num_disc_set_serials);
      for (u32 j = 0; j < num_disc_set_serials; j++)
      {
        if (!reader.ReadSizePrefixedString(&entry.disc_set_serials.emplace_back()))
        {
          DEV_LOG("Cache entry is corrupted.");
          return false;
        }
      }
    }

    entry.compatibility = static_cast<GameDatabase::CompatibilityRating>(compatibility);
    entry.traits.reset();
    for (size_t j = 0; j < static_cast<size_t>(Trait::MaxCount); j++)
    {
      if ((trait_bits[j / 8] & (1u << (j % 8))) != 0)
        entry.traits[j] = true;
    }
    for (size_t j = 0; j < static_cast<size_t>(Language::MaxCount); j++)
    {
      if ((language_bits[j / 8] & (1u << (j % 8))) != 0)
        entry.languages[j] = true;
    }
  }

  for (u32 i = 0; i < num_codes; i++)
  {
    std::string code;
    u32 index;
    if (!reader.ReadSizePrefixedString(&code) || !reader.ReadU32(&index) || index >= static_cast<u32>(s_entries.size()))
    {
      DEV_LOG("Cache code entry is corrupted.");
      return false;
    }

    s_code_lookup.emplace(std::move(code), index);
  }

  s_db_data = std::move(db_data.value());
  return true;
}

bool GameDatabase::SaveToCache()
{
  const u64 gamedb_ts = Host::GetResourceFileTimestamp("gamedb.yaml", false).value_or(0);

  Error error;
  FileSystem::AtomicRenamedFile file = FileSystem::CreateAtomicRenamedFile(GetCacheFile(), &error);
  if (!file)
  {
    ERROR_LOG("Failed to open cache file for writing: {}", error.GetDescription());
    return false;
  }

  BinaryFileWriter writer(file.get());
  writer.WriteU32(GAME_DATABASE_CACHE_SIGNATURE);
  writer.WriteU32(GAME_DATABASE_CACHE_VERSION);
  writer.WriteU64(static_cast<u64>(gamedb_ts));

  writer.WriteU32(static_cast<u32>(s_entries.size()));
  writer.WriteU32(static_cast<u32>(s_code_lookup.size()));

  for (const Entry& entry : s_entries)
  {
    writer.WriteSizePrefixedString(entry.serial);
    writer.WriteSizePrefixedString(entry.title);
    writer.WriteSizePrefixedString(entry.genre);
    writer.WriteSizePrefixedString(entry.developer);
    writer.WriteSizePrefixedString(entry.publisher);
    writer.WriteSizePrefixedString(entry.compatibility_version_tested);
    writer.WriteSizePrefixedString(entry.compatibility_comments);
    writer.WriteU64(entry.release_date);
    writer.WriteU8(entry.min_players);
    writer.WriteU8(entry.max_players);
    writer.WriteU8(entry.min_blocks);
    writer.WriteU8(entry.max_blocks);
    writer.WriteU16(entry.supported_controllers);
    writer.WriteU8(static_cast<u8>(entry.compatibility));

    constexpr u32 trait_num_bytes = (static_cast<u32>(Trait::MaxCount) + 7) / 8;
    std::array<u8, trait_num_bytes> trait_bits = {};
    for (size_t j = 0; j < static_cast<size_t>(Trait::MaxCount); j++)
    {
      if (entry.traits[j])
        trait_bits[j / 8] |= (1u << (j % 8));
    }

    writer.Write(trait_bits.data(), trait_num_bytes);

    constexpr u32 language_num_bytes = (static_cast<u32>(Language::MaxCount) + 7) / 8;
    std::array<u8, language_num_bytes> language_bits = {};
    for (size_t j = 0; j < static_cast<size_t>(Language::MaxCount); j++)
    {
      if (entry.languages[j])
        language_bits[j / 8] |= (1u << (j % 8));
    }
    writer.Write(language_bits.data(), language_num_bytes);

    writer.WriteOptionalT(entry.display_active_start_offset);
    writer.WriteOptionalT(entry.display_active_end_offset);
    writer.WriteOptionalT(entry.display_line_start_offset);
    writer.WriteOptionalT(entry.display_line_end_offset);
    writer.WriteOptionalT(entry.display_crop_mode);
    writer.WriteOptionalT(entry.display_deinterlacing_mode);
    writer.WriteOptionalT(entry.dma_max_slice_ticks);
    writer.WriteOptionalT(entry.dma_halt_ticks);
    writer.WriteOptionalT(entry.cdrom_max_seek_speedup_cycles);
    writer.WriteOptionalT(entry.cdrom_max_read_speedup_cycles);
    writer.WriteOptionalT(entry.gpu_fifo_size);
    writer.WriteOptionalT(entry.gpu_max_run_ahead);
    writer.WriteOptionalT(entry.gpu_pgxp_tolerance);
    writer.WriteOptionalT(entry.gpu_pgxp_depth_threshold);
    writer.WriteOptionalT(entry.gpu_pgxp_preserve_proj_fp);
    writer.WriteOptionalT(entry.gpu_line_detect_mode);

    writer.WriteSizePrefixedString(entry.disc_set_name);
    writer.WriteU32(static_cast<u32>(entry.disc_set_serials.size()));
    for (const std::string& serial : entry.disc_set_serials)
      writer.WriteSizePrefixedString(serial);
  }

  for (const auto& it : s_code_lookup)
  {
    writer.WriteSizePrefixedString(it.first);
    writer.WriteU32(it.second);
  }

  return true;
}

bool GameDatabase::LoadGameDBYaml()
{
  Error error;
  std::optional<DynamicHeapArray<u8>> gamedb_data = Host::ReadResourceFile(GAMEDB_YAML_FILENAME, false, &error);
  if (!gamedb_data.has_value())
  {
    ERROR_LOG("Failed to read game database: {}", error.GetDescription());
    return false;
  }

  SetRymlCallbacks();

  const ryml::Tree tree = ryml::parse_in_place(
    to_csubstr(GAMEDB_YAML_FILENAME), c4::substr(reinterpret_cast<char*>(gamedb_data->data()), gamedb_data->size()));
  const ryml::ConstNodeRef root = tree.rootref();
  s_entries.reserve(root.num_children());

  PreferUnorderedStringMap<std::string_view> code_lookup;

  for (const ryml::ConstNodeRef& current : root.cchildren())
  {
    const std::string_view serial = to_stringview(current.key());
    if (current.empty())
    {
      ERROR_LOG("Missing serial for entry.");
      return false;
    }

    Entry& entry = s_entries.emplace_back();
    entry.serial = serial;
    if (!ParseYamlEntry(&entry, current))
    {
      s_entries.pop_back();
      continue;
    }

    ParseYamlCodes(code_lookup, current, serial);
  }

  // Sorting must be done before generating code lookup, because otherwise the indices won't match.
  s_entries.shrink_to_fit();
  std::sort(s_entries.begin(), s_entries.end(),
            [](const Entry& lhs, const Entry& rhs) { return (lhs.serial < rhs.serial); });

  ryml::reset_callbacks();

  for (const auto& [code, serial] : code_lookup)
  {
    const auto it =
      std::lower_bound(s_entries.cbegin(), s_entries.cend(), serial,
                       [](const Entry& entry, const std::string_view& search) { return (entry.serial < search); });
    if (it == s_entries.end() || it->serial != serial)
    {
      ERROR_LOG("Somehow we messed up our code lookup for {} and {}?!", code, serial);
      continue;
    }

    if (!s_code_lookup.emplace(code, static_cast<u32>(std::distance(s_entries.cbegin(), it))).second)
      ERROR_LOG("Failed to insert code {}", code);
  }

  if (s_entries.empty())
  {
    ERROR_LOG("Game database is empty.");
    return false;
  }

  s_db_data = std::move(gamedb_data.value());
  return true;
}

bool GameDatabase::ParseYamlEntry(Entry* entry, const ryml::ConstNodeRef& value)
{
  GetStringFromObject(value, "name", &entry->title);

  entry->supported_controllers = static_cast<u16>(~0u);

  if (const ryml::ConstNodeRef controllers = value.find_child(to_csubstr("controllers"));
      controllers.valid() && controllers.has_children())
  {
    bool first = true;
    for (const ryml::ConstNodeRef& controller : controllers.cchildren())
    {
      const std::string_view controller_str = to_stringview(controller.val());
      if (controller_str.empty())
      {
        WARNING_LOG("controller is not a string in {}", entry->serial);
        return false;
      }

      const Controller::ControllerInfo* cinfo = Controller::GetControllerInfo(controller_str);
      if (!cinfo)
      {
        WARNING_LOG("Invalid controller type {} in {}", controller_str, entry->serial);
        continue;
      }

      if (first)
      {
        entry->supported_controllers = 0;
        first = false;
      }

      entry->supported_controllers |= (1u << static_cast<u16>(cinfo->type));
    }
  }

  if (const ryml::ConstNodeRef metadata = value.find_child(to_csubstr("metadata")); metadata.valid())
  {
    GetStringFromObject(metadata, "genre", &entry->genre);
    GetStringFromObject(metadata, "developer", &entry->developer);
    GetStringFromObject(metadata, "publisher", &entry->publisher);

    GetUIntFromObject(metadata, "minPlayers", &entry->min_players);
    GetUIntFromObject(metadata, "maxPlayers", &entry->max_players);
    GetUIntFromObject(metadata, "minBlocks", &entry->min_blocks);
    GetUIntFromObject(metadata, "maxBlocks", &entry->max_blocks);

    if (const ryml::ConstNodeRef languages = metadata.find_child(to_csubstr("languages")); languages.valid())
    {
      for (const ryml::ConstNodeRef language : languages.cchildren())
      {
        const std::string_view vlanguage = to_stringview(language.val());
        if (const std::optional<Language> planguage = ParseLanguageName(vlanguage); planguage.has_value())
          entry->languages[static_cast<size_t>(planguage.value())] = true;
        else
          WARNING_LOG("Unknown language {} in {}.", vlanguage, entry->serial);
      }
    }

    entry->release_date = 0;
    {
      std::string release_date;
      if (GetStringFromObject(metadata, "releaseDate", &release_date))
      {
        std::istringstream iss(release_date);
        struct tm parsed_time = {};
        iss >> std::get_time(&parsed_time, "%Y-%m-%d");
        if (!iss.fail())
        {
          parsed_time.tm_isdst = 0;
#ifdef _WIN32
          entry->release_date = _mkgmtime(&parsed_time);
#else
          entry->release_date = timegm(&parsed_time);
#endif
        }
      }
    }

    if (const ryml::ConstNodeRef& multitap = metadata.find_child(to_csubstr("multitap")); multitap.valid())
    {
      if (const std::optional multitap_val = StringUtil::FromChars<bool>(to_stringview(multitap.val()));
          multitap_val.has_value())
      {
        if (multitap_val.value())
          entry->supported_controllers |= Entry::SUPPORTS_MULTITAP_BIT;
      }
      else
      {
        WARNING_LOG("Invalid multitap value in {}", entry->serial);
      }
    }
  }

  if (const ryml::ConstNodeRef compatibility = value.find_child(to_csubstr("compatibility"));
      compatibility.valid() && compatibility.has_children())
  {
    const ryml::ConstNodeRef rating = compatibility.find_child(to_csubstr("rating"));
    if (rating.valid())
    {
      const std::string_view rating_str = to_stringview(rating.val());

      const auto iter = std::find(s_compatibility_rating_names.begin(), s_compatibility_rating_names.end(), rating_str);
      if (iter != s_compatibility_rating_names.end())
      {
        const size_t rating_idx = static_cast<size_t>(std::distance(s_compatibility_rating_names.begin(), iter));
        DebugAssert(rating_idx < static_cast<size_t>(CompatibilityRating::Count));
        entry->compatibility = static_cast<CompatibilityRating>(rating_idx);
      }
      else
      {
        WARNING_LOG("Unknown compatibility rating {} in {}", rating_str, entry->serial);
      }
    }

    GetStringFromObject(compatibility, "versionTested", &entry->compatibility_version_tested);
    GetStringFromObject(compatibility, "comments", &entry->compatibility_comments);
  }

  if (const ryml::ConstNodeRef traits = value.find_child(to_csubstr("traits")); traits.valid() && traits.has_children())
  {
    for (const ryml::ConstNodeRef& trait : traits.cchildren())
    {
      const std::string_view trait_str = to_stringview(trait.val());
      if (trait_str.empty())
      {
        WARNING_LOG("Empty trait in {}", entry->serial);
        continue;
      }

      const auto iter = std::find(s_trait_names.begin(), s_trait_names.end(), trait_str);
      if (iter == s_trait_names.end())
      {
        WARNING_LOG("Unknown trait {} in {}", trait_str, entry->serial);
        continue;
      }

      const size_t trait_idx = static_cast<size_t>(std::distance(s_trait_names.begin(), iter));
      DebugAssert(trait_idx < static_cast<size_t>(Trait::MaxCount));
      entry->traits[trait_idx] = true;
    }
  }

  if (const ryml::ConstNodeRef& libcrypt = value.find_child(to_csubstr("libcrypt")); libcrypt.valid())
  {
    if (const std::optional libcrypt_val = StringUtil::FromChars<bool>(to_stringview(libcrypt.val()));
        libcrypt_val.has_value())
    {
      entry->traits[static_cast<size_t>(Trait::IsLibCryptProtected)] = libcrypt_val.value();
    }
    else
    {
      WARNING_LOG("Invalid libcrypt value in {}", entry->serial);
    }
  }

  if (const ryml::ConstNodeRef settings = value.find_child(to_csubstr("settings"));
      settings.valid() && settings.has_children())
  {
    entry->display_active_start_offset = GetOptionalTFromObject<s16>(settings, "displayActiveStartOffset");
    entry->display_active_end_offset = GetOptionalTFromObject<s16>(settings, "displayActiveEndOffset");
    entry->display_line_start_offset = GetOptionalTFromObject<s8>(settings, "displayLineStartOffset");
    entry->display_line_end_offset = GetOptionalTFromObject<s8>(settings, "displayLineEndOffset");
    entry->display_crop_mode =
      ParseOptionalTFromObject<DisplayCropMode>(settings, "displayCropMode", &Settings::ParseDisplayCropMode);
    entry->display_deinterlacing_mode = ParseOptionalTFromObject<DisplayDeinterlacingMode>(
      settings, "displayDeinterlacingMode", &Settings::ParseDisplayDeinterlacingMode);
    entry->dma_max_slice_ticks = GetOptionalTFromObject<u32>(settings, "dmaMaxSliceTicks");
    entry->dma_halt_ticks = GetOptionalTFromObject<u32>(settings, "dmaHaltTicks");
    entry->cdrom_max_seek_speedup_cycles = GetOptionalTFromObject<u32>(settings, "cdromMaxSeekSpeedupCycles");
    entry->cdrom_max_read_speedup_cycles = GetOptionalTFromObject<u32>(settings, "cdromMaxReadSpeedupCycles");
    entry->gpu_fifo_size = GetOptionalTFromObject<u32>(settings, "gpuFIFOSize");
    entry->gpu_max_run_ahead = GetOptionalTFromObject<u32>(settings, "gpuMaxRunAhead");
    entry->gpu_pgxp_tolerance = GetOptionalTFromObject<float>(settings, "gpuPGXPTolerance");
    entry->gpu_pgxp_depth_threshold = GetOptionalTFromObject<float>(settings, "gpuPGXPDepthThreshold");
    entry->gpu_pgxp_preserve_proj_fp = GetOptionalTFromObject<bool>(settings, "gpuPGXPPreserveProjFP");
    entry->gpu_line_detect_mode =
      ParseOptionalTFromObject<GPULineDetectMode>(settings, "gpuLineDetectMode", &Settings::ParseLineDetectModeName);
  }

  if (const ryml::ConstNodeRef disc_set = value.find_child("discSet"); disc_set.valid() && disc_set.has_children())
  {
    GetStringFromObject(disc_set, "name", &entry->disc_set_name);

    if (const ryml::ConstNodeRef set_serials = disc_set.find_child("serials");
        set_serials.valid() && set_serials.has_children())
    {
      entry->disc_set_serials.reserve(set_serials.num_children());
      for (const ryml::ConstNodeRef& serial : set_serials)
      {
        const std::string_view serial_str = to_stringview(serial.val());
        if (serial_str.empty())
        {
          WARNING_LOG("Empty disc set serial in {}", entry->serial);
          continue;
        }

        if (std::find(entry->disc_set_serials.begin(), entry->disc_set_serials.end(), serial_str) !=
            entry->disc_set_serials.end())
        {
          WARNING_LOG("Duplicate serial {} in disc set serials for {}", serial_str, entry->serial);
          continue;
        }

        entry->disc_set_serials.emplace_back(serial_str);
      }
    }
  }

  return true;
}

bool GameDatabase::ParseYamlCodes(PreferUnorderedStringMap<std::string_view>& lookup, const ryml::ConstNodeRef& value,
                                  std::string_view serial)
{
  const ryml::ConstNodeRef& codes = value.find_child(to_csubstr("codes"));
  if (!codes.valid() || !codes.has_children())
  {
    // use serial instead
    auto iter = lookup.find(serial);
    if (iter != lookup.end())
    {
      WARNING_LOG("Duplicate code '{}'", serial);
      return false;
    }

    lookup.emplace(serial, serial);
    return true;
  }

  u32 added = 0;
  for (const ryml::ConstNodeRef& current_code : codes)
  {
    const std::string_view current_code_str = to_stringview(current_code.val());
    if (current_code_str.empty())
    {
      WARNING_LOG("code is not a string in {}", serial);
      continue;
    }

    auto iter = lookup.find(current_code_str);
    if (iter != lookup.end())
    {
      WARNING_LOG("Duplicate code '{}' in {}", current_code_str, serial);
      continue;
    }

    lookup.emplace(current_code_str, serial);
    added++;
  }

  return (added > 0);
}

void GameDatabase::EnsureTrackHashesMapLoaded()
{
  if (s_track_hashes_loaded)
    return;

  s_track_hashes_loaded = true;
  LoadTrackHashes();
}

bool GameDatabase::LoadTrackHashes()
{
  Timer load_timer;

  Error error;
  std::optional<std::string> gamedb_data(Host::ReadResourceFileToString(DISCDB_YAML_FILENAME, false, &error));
  if (!gamedb_data.has_value())
  {
    ERROR_LOG("Failed to read disc database: {}", error.GetDescription());
    return false;
  }

  SetRymlCallbacks();

  // TODO: Parse in-place, avoid string allocations.
  const ryml::Tree tree = ryml::parse_in_arena(to_csubstr(DISCDB_YAML_FILENAME), to_csubstr(gamedb_data.value()));
  const ryml::ConstNodeRef root = tree.rootref();

  s_track_hashes_map = {};

  size_t serials = 0;
  for (const ryml::ConstNodeRef& current : root.cchildren())
  {
    const std::string_view serial = to_stringview(current.key());
    if (serial.empty() || !current.has_children())
    {
      WARNING_LOG("entry is not an object");
      continue;
    }

    const ryml::ConstNodeRef track_data = current.find_child(to_csubstr("trackData"));
    if (!track_data.valid() || !track_data.has_children())
    {
      WARNING_LOG("trackData is missing in {}", serial);
      continue;
    }

    u32 revision = 0;
    for (const ryml::ConstNodeRef& track_revisions : track_data.cchildren())
    {
      const ryml::ConstNodeRef tracks = track_revisions.find_child(to_csubstr("tracks"));
      if (!tracks.valid() || !tracks.has_children())
      {
        WARNING_LOG("tracks member is missing in {}", serial);
        continue;
      }

      std::string revision_string;
      GetStringFromObject(track_revisions, "version", &revision_string);

      for (const ryml::ConstNodeRef& track : tracks)
      {
        const ryml::ConstNodeRef md5 = track.find_child("md5");
        std::string_view md5_str;
        if (!md5.valid() || (md5_str = to_stringview(md5.val())).empty())
        {
          WARNING_LOG("md5 is missing in track in {}", serial);
          continue;
        }

        const std::optional<CDImageHasher::Hash> md5o = CDImageHasher::HashFromString(md5_str);
        if (md5o.has_value())
        {
          s_track_hashes_map.emplace(std::piecewise_construct, std::forward_as_tuple(md5o.value()),
                                     std::forward_as_tuple(std::string(serial), revision_string, revision));
        }
        else
        {
          WARNING_LOG("invalid md5 in {}", serial);
        }
      }
      revision++;
    }

    serials++;
  }

  ryml::reset_callbacks();
  INFO_LOG("Loaded {} track hashes from {} serials in {:.0f}ms.", s_track_hashes_map.size(), serials,
           load_timer.GetTimeMilliseconds());
  return !s_track_hashes_map.empty();
}

const GameDatabase::TrackHashesMap& GameDatabase::GetTrackHashesMap()
{
  EnsureTrackHashesMapLoaded();
  return s_track_hashes_map;
}
