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
  DisableCDROMReadSpeedup,
  DisableCDROMSeekSpeedup,
  DisableCDROMSpeedupOnMDEC,
  DisableTrueColor,
  DisableFullTrueColor,
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

struct DiscSetEntry;

struct Entry
{
  static constexpr u16 SUPPORTS_MULTITAP_BIT = (1u << static_cast<u8>(ControllerType::Count));

  std::string_view serial;                       ///< Official serial of the game, e.g. "SLUS-00001".
  std::string_view title;                        ///< Official title of the game.
  std::string_view sort_title;                   ///< Title used for sorting in game lists.
  std::string_view localized_title;              ///< Title in the native language, if available.
  std::string_view save_title;                   ///< Title used for per-game memory cards.
  std::string_view genre;                        ///< Genre of the game.
  std::string_view developer;                    ///< Developer of the game.
  std::string_view publisher;                    ///< Publisher of the game.
  std::string_view compatibility_version_tested; ///< Version of the application the game was tested with.
  std::string_view compatibility_comments;       ///< Comments about the game's compatibility.
  const DiscSetEntry* disc_set;                  ///< Pointer to the disc set entry, if applicable.
  u64 release_date;                              ///< Number of seconds since Epoch.
  u8 min_players;                                ///< Minimum number of players supported.
  u8 max_players;                                ///< Maximum number of players supported.
  u8 min_blocks;                                 ///< Minimum number of blocks the game uses.
  u8 max_blocks;                                 ///< Maximum number of blocks the game uses.
  u16 supported_controllers;                     ///< Bitfield of supported controllers.
  CompatibilityRating compatibility;             ///< Compatibility rating of the game.

  std::bitset<static_cast<size_t>(Trait::MaxCount)> traits{};         ///< Traits for the game.
  std::bitset<static_cast<size_t>(Language::MaxCount)> languages{};   ///< Languages supported by the game.
  std::optional<s16> display_active_start_offset;                     ///< Display active start offset override.
  std::optional<s16> display_active_end_offset;                       ///< Display active end offset override.
  std::optional<s8> display_line_start_offset;                        ///< Display line start offset override.
  std::optional<s8> display_line_end_offset;                          ///< Display line end offset override.
  std::optional<DisplayCropMode> display_crop_mode;                   ///< Display crop mode override.
  std::optional<DisplayDeinterlacingMode> display_deinterlacing_mode; ///< Display deinterlacing mode override.
  std::optional<GPULineDetectMode> gpu_line_detect_mode;              ///< GPU line detect mode override.
  std::optional<u8> cpu_overclock;                                    ///< CPU overclock percentage override.
  std::optional<u32> dma_max_slice_ticks;                             ///< DMA max slice ticks override.
  std::optional<u32> dma_halt_ticks;                                  ///< DMA halt ticks override.
  std::optional<u32> cdrom_max_seek_speedup_cycles;                   ///< CD-ROM max seek speedup cycles override.
  std::optional<u32> cdrom_max_read_speedup_cycles;                   ///< CD-ROM max read speedup cycles override.
  std::optional<u32> gpu_fifo_size;                                   ///< GPU FIFO size override.
  std::optional<u32> gpu_max_run_ahead;                               ///< GPU max runahead override.
  std::optional<float> gpu_pgxp_tolerance;                            ///< GPU PGXP tolerance override.
  std::optional<float> gpu_pgxp_depth_threshold;                      ///< GPU PGXP depth threshold override.
  std::optional<bool> gpu_pgxp_preserve_proj_fp; ///< GPU PGXP preserve projection precision override.

  /// Checks if a trait is present.
  ALWAYS_INLINE bool HasTrait(Trait trait) const { return traits[static_cast<int>(trait)]; }

  /// Checks if a language is present.
  ALWAYS_INLINE bool HasLanguage(Language language) const { return languages.test(static_cast<size_t>(language)); }

  /// Checks if any language is present.
  ALWAYS_INLINE bool HasAnyLanguage() const { return languages.any(); }

  /// Returns the flag for the game's language if it only has one, and it is not English. Otherwise the region.
  std::string_view GetLanguageFlagName(DiscRegion region) const;

  /// Returns a comma-separated list of language names.
  SmallString GetLanguagesString() const;

  /// Returns the title that should be displayed for this game.
  std::string_view GetDisplayTitle(bool localized) const;

  /// Returns the sort name if present, otherwise the title.
  std::string_view GetSortTitle() const;

  /// Returns the name to use when creating memory cards for this game.
  std::string_view GetSaveTitle() const;

  /// Returns true if we are the first disc in a disc set.
  bool IsFirstDiscInSet() const;

  /// Applies any settings overrides to the given settings object.
  void ApplySettings(Settings& settings, bool display_osd_messages) const;

  /// Generates a compatibility report in markdown format.
  std::string GenerateCompatibilityReport() const;
};

struct DiscSetEntry
{
  std::string_view title;                ///< Name of the disc set.
  std::string_view sort_title;           ///< Sort name of the disc set.
  std::string_view localized_title;      ///< Localized name of the disc set.
  std::string_view save_title;           ///< Name used for per-game memory cards.
  std::vector<std::string_view> serials; ///< Serials of all discs in the set.

  /// Returns the title that should be displayed for this game.
  std::string_view GetDisplayTitle(bool localized) const;

  /// Returns the sort name if present, otherwise the title.
  std::string_view GetSortTitle() const;

  /// Returns the name to use when creating memory cards for this game.
  std::string_view GetSaveTitle() const;

  /// Returns the first serial in the set.
  std::string_view GetFirstSerial() const;
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
