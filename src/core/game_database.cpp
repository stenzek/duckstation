// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "game_database.h"
#include "controller.h"
#include "host.h"
#include "system.h"

#include "util/cd_image.h"
#include "util/imgui_manager.h"
#include "util/translation.h"

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
#include <mutex>
#include <optional>
#include <sstream>
#include <type_traits>

#include "IconsEmoji.h"
#include "IconsFontAwesome.h"
#include "fmt/format.h"

LOG_CHANNEL(GameDatabase);

namespace GameDatabase {

enum : u32
{
  GAME_DATABASE_CACHE_SIGNATURE = 0x45434C48,
  GAME_DATABASE_CACHE_VERSION = 33,
};

static const Entry* GetEntryForId(std::string_view code);

static void EnsureLoaded();
static void Load();
static bool LoadFromCache();
static bool SaveToCache();

static bool LoadGameDBYaml();
static bool ParseYamlEntry(Entry* entry, const ryml::ConstNodeRef& value);
static bool ParseYamlDiscSetEntry(DiscSetEntry* entry, const ryml::ConstNodeRef& value);
static bool ParseYamlCodes(UnorderedStringMap<std::string_view>& lookup, const ryml::ConstNodeRef& value,
                           std::string_view serial);
static void BindDiscSetsToEntries();
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
  "DisableFastForwardMemoryCardAccess",
  "DisableCDROMReadSpeedup",
  "DisableCDROMSeekSpeedup",
  "DisableCDROMSpeedupOnMDEC",
  "DisableTrueColor",
  "DisableFullTrueColor",
  "DisableUpscaling",
  "DisableTextureFiltering",
  "DisableSpriteTextureFiltering",
  "DisableScaledDithering",
  "DisableScaledInterlacing",
  "DisableAllBordersCrop",
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
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Disable Fast Forward Memory Card Access", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Disable CD-ROM Read Speedup", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Disable CD-ROM Seek Speedup", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Disable CD-ROM Speedup on MDEC", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Disable True Color", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Disable Full True Color", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Disable Upscaling", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Disable Texture Filtering", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Disable Sprite Texture Filtering", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Disable Scaled Dithering", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Disable Scaled Interlacing", "GameDatabase::Trait"),
  TRANSLATE_DISAMBIG_NOOP("GameDatabase", "Disable All Borders Crop", "GameDatabase::Trait"),
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
static constexpr const char* DISCSETS_YAML_FILENAME = "discsets.yaml";
static constexpr const char* DISCDB_YAML_FILENAME = "discdb.yaml";
static DisplayDeinterlacingMode DEFAULT_DEINTERLACING_MODE = DisplayDeinterlacingMode::Adaptive;

namespace {
struct State
{
  bool loaded;
  bool track_hashes_loaded;

  DynamicHeapArray<u8> db_data;          // we take strings from the data, so store a copy
  DynamicHeapArray<u8> disc_set_db_data; // if loaded from binary cache, this will be empty

  std::vector<GameDatabase::Entry> entries;
  std::vector<GameDatabase::DiscSetEntry> disc_sets;
  UnorderedStringMap<u32> code_lookup;

  TrackHashesMap track_hashes_map;

  std::once_flag load_once_flag;
};
} // namespace

ALIGN_TO_CACHE_LINE static State s_state;

} // namespace GameDatabase

void GameDatabase::EnsureLoaded()
{
  if (s_state.loaded)
    return;

  std::call_once(s_state.load_once_flag, &GameDatabase::Load);
}

void GameDatabase::Load()
{
  Timer timer;

  if (!LoadFromCache())
  {
    s_state.entries = {};
    s_state.code_lookup = {};
    s_state.db_data.deallocate();
    s_state.disc_set_db_data.deallocate();

    if (LoadGameDBYaml())
    {
      SaveToCache();
    }
    else
    {
      s_state.entries = {};
      s_state.code_lookup = {};
      s_state.db_data.deallocate();
      s_state.disc_set_db_data.deallocate();
    }
  }

  s_state.loaded = true;

  INFO_LOG("Database load of {} entries took {:.0f}ms.", s_state.entries.size(), timer.GetTimeMilliseconds());
}

const GameDatabase::Entry* GameDatabase::GetEntryForId(std::string_view code)
{
  if (code.empty())
    return nullptr;

  EnsureLoaded();

  auto iter = s_state.code_lookup.find(code);
  return (iter != s_state.code_lookup.end()) ? &s_state.entries[iter->second] : nullptr;
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
    std::lower_bound(s_state.entries.cbegin(), s_state.entries.cend(), serial,
                     [](const Entry& entry, const std::string_view& search) { return (entry.serial < search); });
  return (it != s_state.entries.end() && it->serial == serial) ? &(*it) : nullptr;
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

std::string_view GameDatabase::Entry::GetDisplayTitle(bool localized) const
{
  return (localized && !localized_title.empty()) ? localized_title : title;
}

std::string_view GameDatabase::Entry::GetSortTitle() const
{
  return !sort_title.empty() ? sort_title : title;
}

std::string_view GameDatabase::Entry::GetSaveTitle() const
{
  return !save_title.empty() ? save_title : title;
}

bool GameDatabase::Entry::IsFirstDiscInSet() const
{
  return (disc_set && disc_set->GetFirstSerial() == serial);
}

std::string_view GameDatabase::DiscSetEntry::GetDisplayTitle(bool localized) const
{
  return (localized && !localized_title.empty()) ? localized_title : title;
}

std::string_view GameDatabase::DiscSetEntry::GetSortTitle() const
{
  return !sort_title.empty() ? sort_title : title;
}

std::string_view GameDatabase::DiscSetEntry::GetSaveTitle() const
{
  return !save_title.empty() ? save_title : title;
}

std::string_view GameDatabase::DiscSetEntry::GetFirstSerial() const
{
  return serials[0];
}

std::optional<size_t> GameDatabase::DiscSetEntry::GetDiscIndex(std::string_view serial) const
{
  for (size_t i = 0; i < serials.size(); i++)
  {
    if (serials[i] == serial)
      return i;
  }

  return std::nullopt;
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
      INFO_LOG("GameDB: PGXP tolerance set to {}.", settings.gpu_pgxp_tolerance);
  }
  if (gpu_pgxp_depth_threshold.has_value())
  {
    settings.SetPGXPDepthClearThreshold(gpu_pgxp_depth_threshold.value());
    if (display_osd_messages)
      INFO_LOG("GameDB: PGXP depth clear threshold set to {}.", settings.GetPGXPDepthClearThreshold());
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
  if (cpu_overclock.has_value() && (!settings.cpu_overclock_enable || settings.disable_all_enhancements))
  {
    settings.SetCPUOverclockPercent(cpu_overclock.value());
    settings.cpu_overclock_enable = settings.cpu_overclock_active = true;
    if (display_osd_messages)
      INFO_LOG("GameDB: CPU overclock set to {}.", cpu_overclock.value());
  }

  LargeString messages;
  const auto append_message = [&messages](std::string_view msg) {
    messages.append(" \u2022 ");
    messages.append(msg);
    messages.append('\n');
  };
  const auto append_message_fmt = [&messages]<typename... T>(fmt::format_string<T...> fmt, T&&... args) {
    messages.append(" \u2022 ");
    messages.append_vformat(fmt, fmt::make_format_args(args...));
    messages.append('\n');
  };

  if (HasTrait(Trait::ForceInterpreter))
  {
    if (display_osd_messages && settings.cpu_execution_mode != CPUExecutionMode::Interpreter)
      append_message(TRANSLATE_SV("GameDatabase", "CPU recompiler disabled."));

    settings.cpu_execution_mode = CPUExecutionMode::Interpreter;
  }

  if (HasTrait(Trait::ForceFullBoot))
  {
    if (display_osd_messages && settings.bios_patch_fast_boot)
      append_message(TRANSLATE_SV("GameDatabase", "Fast boot disabled."));

    settings.bios_patch_fast_boot = false;
  }

  if (HasTrait(Trait::DisableMultitap))
  {
    if (display_osd_messages && settings.multitap_mode != MultitapMode::Disabled)
      append_message(TRANSLATE_SV("GameDatabase", "Multitap disabled."));

    settings.multitap_mode = MultitapMode::Disabled;
  }

  if (HasTrait(Trait::DisableFastForwardMemoryCardAccess) && g_settings.memory_card_fast_forward_access)
  {
    if (display_osd_messages)
      append_message(TRANSLATE_SV("GameDatabase", "Fast forward memory card access disabled."));

    settings.memory_card_fast_forward_access = false;
  }

  if (HasTrait(Trait::DisableCDROMReadSpeedup))
  {
    if (settings.cdrom_read_speedup != 1)
      append_message(TRANSLATE_SV("GameDatabase", "CD-ROM read speedup disabled."));

    settings.cdrom_read_speedup = 1;
  }

  if (HasTrait(Trait::DisableCDROMSeekSpeedup))
  {
    if (settings.cdrom_seek_speedup != 1)
      append_message(TRANSLATE_SV("GameDatabase", "CD-ROM seek speedup disabled."));

    settings.cdrom_seek_speedup = 1;
  }

  if (HasTrait(Trait::DisableCDROMSpeedupOnMDEC))
  {
    WARNING_LOG("Disabling CD-ROM speedup on MDEC.");
    settings.mdec_disable_cdrom_speedup = true;
  }
  else if (settings.mdec_disable_cdrom_speedup && settings.cdrom_read_speedup != 1)
  {
    Host::AddIconOSDMessage(
      OSDMessageType::Warning, "GameDBDisableCDROMSpeedupUnnecessary", ICON_EMOJI_WARNING,
      TRANSLATE_STR("GameDatabase",
                    "Disable CD-ROM speedup on MDEC is enabled, but it is not required for this game."));
  }

  if (display_crop_mode.has_value())
  {
    if (display_osd_messages && settings.display_crop_mode != display_crop_mode.value())
    {
      append_message_fmt(TRANSLATE_FS("GameDatabase", "Display cropping set to {}."),
                         Settings::GetDisplayCropModeDisplayName(display_crop_mode.value()));
    }

    settings.display_crop_mode = display_crop_mode.value();
  }
  else if (HasTrait(Trait::DisableAllBordersCrop) && settings.display_crop_mode >= DisplayCropMode::Borders &&
           settings.display_crop_mode <= DisplayCropMode::BordersUncorrected)
  {
    constexpr DisplayCropMode new_mode = DisplayCropMode::Overscan;
    if (display_osd_messages)
    {
      append_message_fmt(TRANSLATE_FS("GameDatabase", "Display cropping set to {}."),
                         Settings::GetDisplayCropModeDisplayName(new_mode));
    }

    settings.display_crop_mode = new_mode;
  }

  if (HasTrait(Trait::ForceSoftwareRenderer))
  {
    if (display_osd_messages && settings.gpu_renderer != GPURenderer::Software)
      append_message(TRANSLATE_SV("GameDatabase", "Hardware rendering disabled."));

    settings.gpu_renderer = GPURenderer::Software;
  }

  if (HasTrait(Trait::ForceSoftwareRendererForReadbacks))
  {
    if (display_osd_messages && settings.gpu_renderer != GPURenderer::Software)
      append_message(TRANSLATE_SV("GameDatabase", "Software renderer readbacks enabled."));

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
      append_message_fmt(TRANSLATE_FS("GameDatabase", "Deinterlacing set to {}."),
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
        append_message_fmt(TRANSLATE_FS("GameDatabase", "Deinterlacing set to {}."),
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
      append_message_fmt(TRANSLATE_FS("GameDatabase", "Dithering set to {}."),
                         Settings::GetGPUDitheringModeDisplayName(settings.gpu_dithering_mode));
    }
  }

  if (HasTrait(Trait::DisableUpscaling))
  {
    if (display_osd_messages)
    {
      if (settings.gpu_resolution_scale != 1)
        append_message(TRANSLATE_SV("GameDatabase", "Upscaling disabled."));
      if (settings.gpu_multisamples != 1)
        append_message(TRANSLATE_SV("GameDatabase", "MSAA disabled."));
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
      append_message(TRANSLATE_SV("GameDatabase", "Texture filtering disabled."));
    }

    settings.gpu_texture_filter = GPUTextureFilter::Nearest;
    settings.gpu_sprite_texture_filter = GPUTextureFilter::Nearest;
  }

  if (HasTrait(Trait::DisableSpriteTextureFiltering))
  {
    if (display_osd_messages && g_settings.gpu_sprite_texture_filter != GPUTextureFilter::Nearest)
    {
      append_message(TRANSLATE_SV("GameDatabase", "Sprite texture filtering disabled."));
    }

    settings.gpu_sprite_texture_filter = GPUTextureFilter::Nearest;
  }

  if (HasTrait(Trait::DisableScaledInterlacing))
  {
    if (display_osd_messages && settings.gpu_scaled_interlacing &&
        settings.display_deinterlacing_mode != DisplayDeinterlacingMode::Progressive)
    {
      append_message(TRANSLATE_SV("GameDatabase", "Scaled interlacing disabled."));
    }

    settings.gpu_scaled_interlacing = false;
  }

  if (HasTrait(Trait::DisableWidescreen))
  {
    if (display_osd_messages && settings.gpu_widescreen_rendering)
      append_message(TRANSLATE_SV("GameDatabase", "Widescreen rendering disabled."));

    settings.gpu_widescreen_rendering = false;
    settings.gpu_widescreen_hack = false;
  }

  if (HasTrait(Trait::DisablePGXP))
  {
    if (display_osd_messages && settings.gpu_pgxp_enable)
      append_message(TRANSLATE_SV("GameDatabase", "PGXP geometry correction disabled."));

    settings.gpu_pgxp_enable = false;
  }

  if (HasTrait(Trait::DisablePGXPCulling))
  {
    if (display_osd_messages && settings.gpu_pgxp_enable && settings.gpu_pgxp_culling)
      append_message(TRANSLATE_SV("GameDatabase", "PGXP culling correction disabled."));

    settings.gpu_pgxp_culling = false;
  }

  if (HasTrait(Trait::DisablePGXPTextureCorrection))
  {
    if (display_osd_messages && settings.gpu_pgxp_enable && settings.gpu_pgxp_texture_correction)
      append_message(TRANSLATE_SV("GameDatabase", "PGXP perspective correct textures disabled."));

    settings.gpu_pgxp_texture_correction = false;
  }

  if (HasTrait(Trait::DisablePGXPColorCorrection))
  {
    if (display_osd_messages && settings.gpu_pgxp_enable && settings.gpu_pgxp_texture_correction &&
        settings.gpu_pgxp_color_correction)
    {
      append_message(TRANSLATE_SV("GameDatabase", "PGXP perspective correct colors disabled."));
    }

    settings.gpu_pgxp_color_correction = false;
  }

  if (HasTrait(Trait::ForcePGXPVertexCache))
  {
    if (display_osd_messages && settings.gpu_pgxp_enable && !settings.gpu_pgxp_vertex_cache)
      append_message(TRANSLATE_SV("GameDatabase", "PGXP vertex cache enabled."));

    settings.gpu_pgxp_vertex_cache = settings.gpu_pgxp_enable;
  }
  else if (settings.gpu_pgxp_enable && settings.gpu_pgxp_vertex_cache)
  {
    Host::AddIconOSDMessage(
      OSDMessageType::Warning, "gamedb_force_pgxp_vertex_cache", ICON_EMOJI_WARNING,
      TRANSLATE_STR(
        "GameDatabase",
        "PGXP Vertex Cache is enabled, but it is not required for this game. This may cause rendering errors."));
  }

  if (HasTrait(Trait::ForcePGXPCPUMode))
  {
    if (display_osd_messages && settings.gpu_pgxp_enable && !settings.gpu_pgxp_cpu)
    {
#ifndef __ANDROID__
      append_message(TRANSLATE_SV("GameDatabase", "PGXP CPU mode enabled."));
#else
      Host::AddIconOSDMessage(OSDMessageType::Warning, "gamedb_force_pgxp_cpu", ICON_EMOJI_WARNING,
                              "This game requires PGXP CPU mode, which increases system requirements."
                              "If the game runs too slow, disable PGXP for this game.");
#endif
    }

    settings.gpu_pgxp_cpu = settings.gpu_pgxp_enable;
  }
  else if (settings.UsingPGXPCPUMode())
  {
    Host::AddIconOSDMessage(
      OSDMessageType::Warning, "gamedb_force_pgxp_cpu", ICON_EMOJI_WARNING,
      TRANSLATE_STR(
        "GameDatabase",
        "PGXP CPU mode is enabled, but it is not required for this game. This may cause rendering errors."));
  }

  if (HasTrait(Trait::DisablePGXPDepthBuffer))
  {
    if (display_osd_messages && settings.gpu_pgxp_enable && settings.gpu_pgxp_depth_buffer)
      append_message(TRANSLATE_SV("GameDatabase", "PGXP depth buffer disabled."));

    settings.gpu_pgxp_depth_buffer = false;
  }

  if (HasTrait(Trait::DisablePGXPOn2DPolygons))
  {
    if (display_osd_messages && settings.gpu_pgxp_enable && !settings.gpu_pgxp_disable_2d)
      append_message(TRANSLATE_SV("GameDatabase", "PGXP disabled on 2D polygons."));

    g_settings.gpu_pgxp_disable_2d = true;
  }

  if (gpu_pgxp_preserve_proj_fp.has_value())
  {
    if (display_osd_messages)
    {
      INFO_LOG("GameDB: GPU preserve projection precision set to {}.",
               gpu_pgxp_preserve_proj_fp.value() ? "true" : "false");

      if (settings.gpu_pgxp_enable && settings.gpu_pgxp_preserve_proj_fp && !gpu_pgxp_preserve_proj_fp.value())
        append_message(TRANSLATE_SV("GameDatabase", "PGXP preserve projection precision disabled."));
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
    if (messages.back() == '\n')
      messages.pop_back();

    Host::AddIconOSDMessage(OSDMessageType::Info, "GameDBCompatibility", ICON_EMOJI_INFORMATION,
                            TRANSLATE_STR("GameDatabase", "Compatibility settings for this game have been applied."),
                            std::string(messages.view()));
  }

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
        SmallString supported_controller_string(
          TRANSLATE_SV("GameDatabase", "Please configure a supported controller from the following list:"));

        for (u32 j = 0; j < static_cast<u32>(ControllerType::Count); j++)
        {
          const ControllerType supported_ctype = static_cast<ControllerType>(j);
          if ((supported_controllers & BIT_FOR(supported_ctype)) == 0)
            continue;

          supported_controller_string.append_format("\n \u2022 {}",
                                                    Controller::GetControllerInfo(supported_ctype).GetDisplayName());
        }

        Host::AddIconOSDMessage(
          OSDMessageType::Error, fmt::format("GameDBController{}Unsupported", i), ICON_EMOJI_WARNING,
          fmt::format(TRANSLATE_FS("GameDatabase", "Controller in Port {0} ({1}) is not supported for this game."),
                      i + 1u, Controller::GetControllerInfo(ctype).GetDisplayName()),
          std::string(supported_controller_string));
      }
    }

    if (g_settings.multitap_mode != MultitapMode::Disabled && !(supported_controllers & SUPPORTS_MULTITAP_BIT))
    {
      Host::AddIconOSDMessage(
        OSDMessageType::Error, "GameDBMultitapUnsupported", ICON_EMOJI_WARNING,
        TRANSLATE_STR("GameDatabase", "This game does not support multitap, but multitap is enabled."),
        TRANSLATE_STR("GameDatabase", "This may result in dropped controller inputs."));
    }
  }

#undef BIT_FOR
}

static inline void AppendSettingsHeading(SmallStringBase& str, bool& heading)
{
  if (!heading)
  {
    heading = true;
    str.append_format("**{}**\n\n", TRANSLATE_SV("GameDatabase", "Settings"));
  }
}

static inline void AppendBoolSetting(SmallStringBase& str, bool& heading, std::string_view title,
                                     const std::optional<bool>& value)
{
  if (!value.has_value())
    return;

  AppendSettingsHeading(str, heading);
  str.append_format(" - {}: {}\n", title, value.value() ? "Enabled" : "Disabled");
}

template<typename T>
static inline void AppendIntegerSetting(SmallStringBase& str, bool& heading, std::string_view title,
                                        const std::optional<T>& value)
{
  if (!value.has_value())
    return;

  AppendSettingsHeading(str, heading);
  str.append_format(" - {}: {}\n", title, value.value());
}

static inline void AppendFloatSetting(SmallStringBase& str, bool& heading, std::string_view title,
                                      const std::optional<float>& value)
{
  if (!value.has_value())
    return;

  AppendSettingsHeading(str, heading);
  str.append_format(" - {}: {:.2f}\n", title, value.value());
}

template<typename T>
static inline void AppendEnumSetting(SmallStringBase& str, bool& heading, std::string_view title,
                                     const char* (*get_display_name_func)(T), const std::optional<T>& value)
{
  if (!value.has_value())
    return;

  AppendSettingsHeading(str, heading);
  str.append_format(" - {}: {}\n", title, get_display_name_func(value.value()));
}

std::string GameDatabase::Entry::GenerateCompatibilityReport() const
{
  LargeString ret;
  ret.append_format("**{}:** {}\n\n", TRANSLATE_SV("GameDatabase", "Serial"), serial);
  ret.append_format("**{}:** {}\n\n", TRANSLATE_SV("GameDatabase", "Title"), title);
  if (!sort_title.empty())
    ret.append_format("**{}:** {}\n\n", TRANSLATE_SV("GameDatabase", "Sort Title"), sort_title);
  if (!localized_title.empty())
    ret.append_format("**{}:** {}\n\n", TRANSLATE_SV("GameDatabase", "Localized Title"), localized_title);
  if (!save_title.empty())
    ret.append_format("**{}:** {}\n\n", TRANSLATE_SV("GameDatabase", "Save Title"), save_title);

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
  AppendIntegerSetting(ret, settings_heading, TRANSLATE_SV("GameDatabase", "CPU Overclock Percent"), cpu_overclock);
  AppendIntegerSetting(ret, settings_heading, TRANSLATE_SV("GameDatabase", "DMA Max Slice Ticks"), dma_max_slice_ticks);
  AppendIntegerSetting(ret, settings_heading, TRANSLATE_SV("GameDatabase", "DMA Halt Ticks"), dma_halt_ticks);
  AppendIntegerSetting(ret, settings_heading, TRANSLATE_SV("GameDatabase", "CD-ROM Max Seek Speedup Cycles"),
                       cdrom_max_seek_speedup_cycles);
  AppendIntegerSetting(ret, settings_heading, TRANSLATE_SV("GameDatabase", "CD-ROM Max Read Speedup Cycles"),
                       cdrom_max_read_speedup_cycles);
  AppendIntegerSetting(ret, settings_heading, TRANSLATE_SV("GameDatabase", "GPU FIFO Size"), gpu_fifo_size);
  AppendIntegerSetting(ret, settings_heading, TRANSLATE_SV("GameDatabase", "GPU Max Runahead"), gpu_max_run_ahead);
  AppendEnumSetting(ret, settings_heading, TRANSLATE_SV("GameDatabase", "GPU Line Detect Mode"),
                    &Settings::GetLineDetectModeDisplayName, gpu_line_detect_mode);
  AppendFloatSetting(ret, settings_heading, TRANSLATE_SV("GameDatabase", "PGXP Tolerance"), gpu_pgxp_tolerance);
  AppendFloatSetting(ret, settings_heading, TRANSLATE_SV("GameDatabase", "PGXP Depth Clear Threshold"),
                     gpu_pgxp_depth_threshold);
  AppendBoolSetting(ret, settings_heading, TRANSLATE_SV("GameDatabase", "PGXP Preserve Projection Precision"),
                    gpu_pgxp_preserve_proj_fp);

  if (settings_heading)
    ret.append("\n");

  if (disc_set)
  {
    ret.append_format("**{}:** {}\n", TRANSLATE_SV("GameDatabase", "Disc Set"), disc_set->GetDisplayTitle(true));
    for (const std::string_view& ds_serial : disc_set->serials)
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
  const u64 gamedb_ts = static_cast<u64>(Host::GetResourceFileTimestamp(GAMEDB_YAML_FILENAME, false).value_or(0));
  const u64 discsets_ts = static_cast<u64>(Host::GetResourceFileTimestamp(DISCSETS_YAML_FILENAME, false).value_or(0));

  u32 signature, version, num_disc_set_entries, num_entries, num_codes;
  u64 file_gamedb_ts, file_discsets_ts;
  if (!reader.ReadU32(&signature) || !reader.ReadU32(&version) || !reader.ReadU64(&file_gamedb_ts) ||
      !reader.ReadU64(&file_discsets_ts) || !reader.ReadU32(&num_disc_set_entries) || !reader.ReadU32(&num_entries) ||
      !reader.ReadU32(&num_codes) || signature != GAME_DATABASE_CACHE_SIGNATURE ||
      version != GAME_DATABASE_CACHE_VERSION)
  {
    DEV_LOG("Cache header is corrupted or version mismatch.");
    return false;
  }

  if (gamedb_ts != file_gamedb_ts || discsets_ts != file_discsets_ts)
  {
    DEV_LOG("Cache is out of date, recreating.");
    return false;
  }

  s_state.entries.reserve(num_entries);
  s_state.disc_sets.reserve(num_disc_set_entries);

  for (u32 i = 0; i < num_disc_set_entries; i++)
  {
    DiscSetEntry& disc_set = s_state.disc_sets.emplace_back();
    u32 num_serials;
    if (!reader.ReadSizePrefixedString(&disc_set.title) || !reader.ReadSizePrefixedString(&disc_set.sort_title) ||
        !reader.ReadSizePrefixedString(&disc_set.localized_title) ||
        !reader.ReadSizePrefixedString(&disc_set.save_title) || !reader.ReadU32(&num_serials))
    {
      DEV_LOG("Cache disc set entry is corrupted.");
      return false;
    }
    if (num_serials > 0)
    {
      disc_set.serials.reserve(num_serials);
      for (u32 j = 0; j < num_serials; j++)
      {
        if (!reader.ReadSizePrefixedString(&disc_set.serials.emplace_back()))
        {
          DEV_LOG("Cache disc set entry is corrupted.");
          return false;
        }
      }
    }
  }

  for (u32 i = 0; i < num_entries; i++)
  {
    Entry& entry = s_state.entries.emplace_back();

    constexpr u32 trait_num_bytes = (static_cast<u32>(Trait::MaxCount) + 7) / 8;
    constexpr u32 language_num_bytes = (static_cast<u32>(Language::MaxCount) + 7) / 8;
    std::array<u8, trait_num_bytes> trait_bits;
    std::array<u8, language_num_bytes> language_bits;
    u8 compatibility;
    s32 disc_set_index;

    if (!reader.ReadSizePrefixedString(&entry.serial) || !reader.ReadSizePrefixedString(&entry.title) ||
        !reader.ReadSizePrefixedString(&entry.sort_title) || !reader.ReadSizePrefixedString(&entry.localized_title) ||
        !reader.ReadSizePrefixedString(&entry.save_title) || !reader.ReadSizePrefixedString(&entry.genre) ||
        !reader.ReadSizePrefixedString(&entry.developer) || !reader.ReadSizePrefixedString(&entry.publisher) ||
        !reader.ReadSizePrefixedString(&entry.compatibility_version_tested) ||
        !reader.ReadSizePrefixedString(&entry.compatibility_comments) || !reader.ReadS32(&disc_set_index) ||
        (disc_set_index >= 0 && static_cast<u32>(disc_set_index) >= num_disc_set_entries) ||
        !reader.ReadU64(&entry.release_date) || !reader.ReadU8(&entry.min_players) ||
        !reader.ReadU8(&entry.max_players) || !reader.ReadU8(&entry.min_blocks) || !reader.ReadU8(&entry.max_blocks) ||
        !reader.ReadU16(&entry.supported_controllers) || !reader.ReadU8(&compatibility) ||
        compatibility >= static_cast<u8>(GameDatabase::CompatibilityRating::Count) ||
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
        !reader.ReadOptionalT(&entry.cpu_overclock))
    {
      DEV_LOG("Cache entry is corrupted.");
      return false;
    }

    entry.disc_set = (disc_set_index >= 0) ? &s_state.disc_sets[static_cast<u32>(disc_set_index)] : nullptr;
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
    if (!reader.ReadSizePrefixedString(&code) || !reader.ReadU32(&index) ||
        index >= static_cast<u32>(s_state.entries.size()))
    {
      DEV_LOG("Cache code entry is corrupted.");
      return false;
    }

    s_state.code_lookup.emplace(std::move(code), index);
  }

  s_state.db_data = std::move(db_data.value());
  return true;
}

bool GameDatabase::SaveToCache()
{
  const u64 gamedb_ts = static_cast<u64>(Host::GetResourceFileTimestamp(GAMEDB_YAML_FILENAME, false).value_or(0));
  const u64 discsets_ts = static_cast<u64>(Host::GetResourceFileTimestamp(DISCSETS_YAML_FILENAME, false).value_or(0));

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
  writer.WriteU64(gamedb_ts);
  writer.WriteU64(discsets_ts);

  writer.WriteU32(static_cast<u32>(s_state.disc_sets.size()));
  writer.WriteU32(static_cast<u32>(s_state.entries.size()));
  writer.WriteU32(static_cast<u32>(s_state.code_lookup.size()));

  for (const DiscSetEntry& disc_set : s_state.disc_sets)
  {
    writer.WriteSizePrefixedString(disc_set.title);
    writer.WriteSizePrefixedString(disc_set.sort_title);
    writer.WriteSizePrefixedString(disc_set.localized_title);
    writer.WriteSizePrefixedString(disc_set.save_title);
    writer.WriteU32(static_cast<u32>(disc_set.serials.size()));
    for (const std::string_view& ds_serial : disc_set.serials)
      writer.WriteSizePrefixedString(ds_serial);
  }

  for (const Entry& entry : s_state.entries)
  {
    writer.WriteSizePrefixedString(entry.serial);
    writer.WriteSizePrefixedString(entry.title);
    writer.WriteSizePrefixedString(entry.sort_title);
    writer.WriteSizePrefixedString(entry.localized_title);
    writer.WriteSizePrefixedString(entry.save_title);
    writer.WriteSizePrefixedString(entry.genre);
    writer.WriteSizePrefixedString(entry.developer);
    writer.WriteSizePrefixedString(entry.publisher);
    writer.WriteSizePrefixedString(entry.compatibility_version_tested);
    writer.WriteSizePrefixedString(entry.compatibility_comments);
    writer.WriteS32(entry.disc_set ? static_cast<s32>(entry.disc_set - &s_state.disc_sets[0]) : -1);
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
    writer.WriteOptionalT(entry.cpu_overclock);
  }

  for (const auto& it : s_state.code_lookup)
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

  std::optional<DynamicHeapArray<u8>> disc_set_data = Host::ReadResourceFile(DISCSETS_YAML_FILENAME, false, &error);
  if (!disc_set_data.has_value())
  {
    ERROR_LOG("Failed to read disc set database: {}", error.GetDescription());
    return false;
  }

  UnorderedStringMap<std::string_view> code_lookup;
  {
    const ryml::Tree tree = ryml::parse_in_place(
      to_csubstr(GAMEDB_YAML_FILENAME), c4::substr(reinterpret_cast<char*>(gamedb_data->data()), gamedb_data->size()));
    const ryml::ConstNodeRef root = tree.rootref();
    s_state.entries.reserve(root.num_children());

    for (const ryml::ConstNodeRef& current : root.cchildren())
    {
      const std::string_view serial = to_stringview(current.key());
      if (current.empty())
      {
        ERROR_LOG("Missing serial for entry.");
        return false;
      }

      Entry& entry = s_state.entries.emplace_back();
      entry.serial = serial;
      if (!ParseYamlEntry(&entry, current))
      {
        s_state.entries.pop_back();
        continue;
      }

      ParseYamlCodes(code_lookup, current, serial);
    }
  }

  // Sorting must be done before generating code lookup, because otherwise the indices won't match.
  s_state.entries.shrink_to_fit();
  std::sort(s_state.entries.begin(), s_state.entries.end(),
            [](const Entry& lhs, const Entry& rhs) { return (lhs.serial < rhs.serial); });

  for (const auto& [code, serial] : code_lookup)
  {
    const auto it =
      std::lower_bound(s_state.entries.cbegin(), s_state.entries.cend(), serial,
                       [](const Entry& entry, const std::string_view& search) { return (entry.serial < search); });
    if (it == s_state.entries.end() || it->serial != serial)
    {
      ERROR_LOG("Somehow we messed up our code lookup for {} and {}?!", code, serial);
      continue;
    }

    if (!s_state.code_lookup.emplace(code, static_cast<u32>(std::distance(s_state.entries.cbegin(), it))).second)
      ERROR_LOG("Failed to insert code {}", code);
  }

  if (s_state.entries.empty())
  {
    ERROR_LOG("Game database is empty.");
    return false;
  }

  // parse disc sets
  {
    const ryml::Tree tree =
      ryml::parse_in_place(to_csubstr(DISCSETS_YAML_FILENAME),
                           c4::substr(reinterpret_cast<char*>(disc_set_data->data()), disc_set_data->size()));
    const ryml::ConstNodeRef root = tree.rootref();
    s_state.disc_sets.reserve(root.num_children());
    for (const ryml::ConstNodeRef& current : root.cchildren())
    {
      DiscSetEntry& disc_set = s_state.disc_sets.emplace_back();
      if (!ParseYamlDiscSetEntry(&disc_set, current))
      {
        s_state.disc_sets.pop_back();
        continue;
      }
    }

    s_state.disc_sets.shrink_to_fit();
    BindDiscSetsToEntries();
  }

  s_state.db_data = std::move(gamedb_data.value());
  s_state.disc_set_db_data = std::move(disc_set_data.value());
  return true;
}

bool GameDatabase::ParseYamlEntry(Entry* entry, const ryml::ConstNodeRef& value)
{
  GetStringFromObject(value, "name", &entry->title);
  GetStringFromObject(value, "sortName", &entry->sort_title);
  GetStringFromObject(value, "localizedName", &entry->localized_title);
  GetStringFromObject(value, "saveName", &entry->save_title);

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

    GetIntFromObject(metadata, "minPlayers", &entry->min_players);
    GetIntFromObject(metadata, "maxPlayers", &entry->max_players);
    GetIntFromObject(metadata, "minBlocks", &entry->min_blocks);
    GetIntFromObject(metadata, "maxBlocks", &entry->max_blocks);

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
    entry->cpu_overclock = GetOptionalTFromObject<u8>(settings, "cpuOverclockPercent");
  }

  return true;
}

bool GameDatabase::ParseYamlDiscSetEntry(DiscSetEntry* entry, const ryml::ConstNodeRef& value)
{
  if (!GetStringFromObject(value, "name", &entry->title) || entry->title.empty())
  {
    WARNING_LOG("Disc set entry is missing a title.");
    return false;
  }

  GetStringFromObject(value, "sortName", &entry->sort_title);
  GetStringFromObject(value, "localizedName", &entry->localized_title);
  GetStringFromObject(value, "saveName", &entry->save_title);

  // ensure there are no duplicates, linear search is okay here because there aren't that many disc sets
  const std::string_view save_title = entry->GetSaveTitle();
  const size_t search_count = s_state.disc_sets.size() - 1;
  for (size_t i = 0; i < search_count; i++)
  {
    if (s_state.disc_sets[i].GetSaveTitle() == save_title)
    {
      WARNING_LOG("Duplicate disc set title {}.", save_title);
      return false;
    }
  }

  if (const ryml::ConstNodeRef set_serials = value.find_child("serials");
      set_serials.valid() && set_serials.has_children())
  {
    entry->serials.reserve(set_serials.num_children());
    for (const ryml::ConstNodeRef& serial : set_serials)
    {
      const std::string_view serial_str = to_stringview(serial.val());
      if (serial_str.empty())
      {
        WARNING_LOG("Empty disc set serial in {}", entry->title);
        continue;
      }

      if (std::find(entry->serials.begin(), entry->serials.end(), serial_str) != entry->serials.end())
      {
        WARNING_LOG("Duplicate serial {} in disc set serials for {}", serial_str, entry->title);
        continue;
      }

      const auto entry_it =
        std::lower_bound(s_state.entries.begin(), s_state.entries.end(), serial_str,
                         [](const Entry& entry, const std::string_view& search) { return (entry.serial < search); });
      if (entry_it == s_state.entries.end() || entry_it->serial != serial_str)
      {
        WARNING_LOG("Serial {} in disc set {} does not exist in game database.", serial_str, entry->title);
        continue;
      }
      else if (entry_it->disc_set)
      {
        WARNING_LOG("Serial {} in disc set {} is already part of another disc set.", serial_str, entry->title);
        continue;
      }

      entry->serials.emplace_back(serial_str);
    }
  }

  if (entry->serials.empty())
  {
    WARNING_LOG("Disc set {} has {} serials, dropping.", entry->title, entry->serials.size());
    return false;
  }

  return true;
}

void GameDatabase::BindDiscSetsToEntries()
{
  for (DiscSetEntry& dsentry : s_state.disc_sets)
  {
    for (const std::string_view& serial : dsentry.serials)
    {
      const auto entry_it =
        std::lower_bound(s_state.entries.begin(), s_state.entries.end(), serial,
                         [](const Entry& entry, const std::string_view& search) { return (entry.serial < search); });
      Assert(entry_it != s_state.entries.end() && entry_it->serial == serial);

      // link the entry to this disc set
      entry_it->disc_set = &dsentry;
    }
  }
}

bool GameDatabase::ParseYamlCodes(UnorderedStringMap<std::string_view>& lookup, const ryml::ConstNodeRef& value,
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
  if (s_state.track_hashes_loaded)
    return;

  s_state.track_hashes_loaded = true;
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

  // TODO: Parse in-place, avoid string allocations.
  const ryml::Tree tree = ryml::parse_in_arena(to_csubstr(DISCDB_YAML_FILENAME), to_csubstr(gamedb_data.value()));
  const ryml::ConstNodeRef root = tree.rootref();

  s_state.track_hashes_map = {};

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
          s_state.track_hashes_map.emplace(std::piecewise_construct, std::forward_as_tuple(md5o.value()),
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
  INFO_LOG("Loaded {} track hashes from {} serials in {:.0f}ms.", s_state.track_hashes_map.size(), serials,
           load_timer.GetTimeMilliseconds());
  return !s_state.track_hashes_map.empty();
}

const GameDatabase::TrackHashesMap& GameDatabase::GetTrackHashesMap()
{
  EnsureTrackHashesMapLoaded();
  return s_state.track_hashes_map;
}
