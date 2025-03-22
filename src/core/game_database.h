// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "types.h"

#include "util/cd_image_hasher.h"

#include "common/small_string.h"

#include <bitset>
#include <map>
#include <string>
#include <string_view>
#include <vector>

class CDImage;

struct Settings;

namespace GameDatabase {
enum class CompatibilityRating : u8
{
  Unknown = 0,
  DoesntBoot = 1,
  CrashesInIntro = 2,
  CrashesInGame = 3,
  GraphicalAudioIssues = 4,
  NoIssues = 5,
  Count,
};

enum class Trait : u32
{
  ForceInterpreter,
  ForceSoftwareRenderer,
  ForceSoftwareRendererForReadbacks,
  ForceRoundUpscaledTextureCoordinates,
  ForceShaderBlending,
  ForceFullTrueColor,
  ForceDeinterlacing,
  ForceFullBoot,
  DisableAutoAnalogMode,
  DisableMultitap,
  DisableTrueColor,
  DisableUpscaling,
  DisableTextureFiltering,
  DisableSpriteTextureFiltering,
  DisableScaledDithering,
  DisableScaledInterlacing,
  DisableWidescreen,
  DisablePGXP,
  DisablePGXPCulling,
  DisablePGXPTextureCorrection,
  DisablePGXPColorCorrection,
  DisablePGXPDepthBuffer,
  DisablePGXPOn2DPolygons,
  ForcePGXPVertexCache,
  ForcePGXPCPUMode,
  ForceRecompilerICache,
  ForceCDROMSubQSkew,
  IsLibCryptProtected,

  MaxCount
};

enum class Language : u8
{
  Catalan,
  Chinese,
  Czech,
  Danish,
  Dutch,
  English,
  Finnish,
  French,
  German,
  Greek,
  Hebrew,
  Iranian,
  Italian,
  Japanese,
  Korean,
  Norwegian,
  Polish,
  Portuguese,
  Russian,
  Spanish,
  Swedish,
  Turkish,
  MaxCount,
};

struct Entry
{
  static constexpr u16 SUPPORTS_MULTITAP_BIT = (1u << static_cast<u8>(ControllerType::Count));

  std::string_view serial;
  std::string_view title;
  std::string_view genre;
  std::string_view developer;
  std::string_view publisher;
  std::string_view compatibility_version_tested;
  std::string_view compatibility_comments;
  u64 release_date;
  u8 min_players;
  u8 max_players;
  u8 min_blocks;
  u8 max_blocks;
  u16 supported_controllers;
  CompatibilityRating compatibility;

  std::bitset<static_cast<size_t>(Trait::MaxCount)> traits{};
  std::bitset<static_cast<size_t>(Language::MaxCount)> languages{};
  std::optional<s16> display_active_start_offset;
  std::optional<s16> display_active_end_offset;
  std::optional<s8> display_line_start_offset;
  std::optional<s8> display_line_end_offset;
  std::optional<DisplayCropMode> display_crop_mode;
  std::optional<DisplayDeinterlacingMode> display_deinterlacing_mode;
  std::optional<GPULineDetectMode> gpu_line_detect_mode;
  std::optional<u32> dma_max_slice_ticks;
  std::optional<u32> dma_halt_ticks;
  std::optional<u32> gpu_fifo_size;
  std::optional<u32> gpu_max_run_ahead;
  std::optional<float> gpu_pgxp_tolerance;
  std::optional<float> gpu_pgxp_depth_threshold;
  std::optional<bool> gpu_pgxp_preserve_proj_fp;

  std::string disc_set_name;
  std::vector<std::string> disc_set_serials;

  ALWAYS_INLINE bool HasTrait(Trait trait) const { return traits[static_cast<int>(trait)]; }
  ALWAYS_INLINE bool HasLanguage(Language language) const { return languages.test(static_cast<size_t>(language)); }
  ALWAYS_INLINE bool HasAnyLanguage() const { return languages.any(); }

  std::string_view GetLanguageFlagName(DiscRegion region) const;
  SmallString GetLanguagesString() const;

  void ApplySettings(Settings& settings, bool display_osd_messages) const;

  std::string GenerateCompatibilityReport() const;
};

void EnsureLoaded();
void Unload();

const Entry* GetEntryForDisc(CDImage* image);
const Entry* GetEntryForGameDetails(const std::string& id, u64 hash);
const Entry* GetEntryForSerial(std::string_view serial);
std::string GetSerialForDisc(CDImage* image);
std::string GetSerialForPath(const char* path);

const char* GetTraitName(Trait trait);
const char* GetTraitDisplayName(Trait trait);

const char* GetCompatibilityRatingName(CompatibilityRating rating);
const char* GetCompatibilityRatingDisplayName(CompatibilityRating rating);

const char* GetLanguageName(Language language);
std::optional<Language> ParseLanguageName(std::string_view str);
TinyString GetLanguageFlagResourceName(std::string_view language_name);

/// Map of track hashes for image verification
struct TrackData
{
  TrackData(std::string serial_, std::string revision_str_, uint32_t revision_)
    : serial(std::move(serial_)), revision_str(std::move(revision_str_)), revision(revision_)
  {
  }

  friend bool operator==(const TrackData& left, const TrackData& right)
  {
    // 'revisionString' is deliberately ignored in comparisons as it's redundant with comparing 'revision'! Do not
    // change!
    return left.serial == right.serial && left.revision == right.revision;
  }

  std::string serial;
  std::string revision_str;
  u32 revision;
};

using TrackHashesMap = std::multimap<CDImageHasher::Hash, TrackData>;
const TrackHashesMap& GetTrackHashesMap();
void EnsureTrackHashesMapLoaded();

} // namespace GameDatabase
