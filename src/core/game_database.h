// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once
#include "core/types.h"
#include "util/cd_image_hasher.h"
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
  ForceInterlacing,
  DisableTrueColor,
  DisableUpscaling,
  DisableTextureFiltering,
  DisableScaledDithering,
  DisableForceNTSCTimings,
  DisableWidescreen,
  DisablePGXP,
  DisablePGXPCulling,
  DisablePGXPTextureCorrection,
  DisablePGXPColorCorrection,
  DisablePGXPDepthBuffer,
  ForcePGXPVertexCache,
  ForcePGXPCPUMode,
  ForceRecompilerMemoryExceptions,
  ForceRecompilerICache,
  ForceRecompilerLUTFastmem,
  IsLibCryptProtected,

  Count
};

struct Entry
{
  // TODO: Make string_view.
  std::string serial;
  std::string title;
  std::string genre;
  std::string developer;
  std::string publisher;
  u64 release_date;
  u8 min_players;
  u8 max_players;
  u8 min_blocks;
  u8 max_blocks;
  u16 supported_controllers;
  CompatibilityRating compatibility;

  std::bitset<static_cast<int>(Trait::Count)> traits{};
  std::optional<s16> display_active_start_offset;
  std::optional<s16> display_active_end_offset;
  std::optional<s8> display_line_start_offset;
  std::optional<s8> display_line_end_offset;
  std::optional<u32> dma_max_slice_ticks;
  std::optional<u32> dma_halt_ticks;
  std::optional<u32> gpu_fifo_size;
  std::optional<u32> gpu_max_run_ahead;
  std::optional<float> gpu_pgxp_tolerance;
  std::optional<float> gpu_pgxp_depth_threshold;
  std::optional<GPULineDetectMode> gpu_line_detect_mode;

  std::string disc_set_name;
  std::vector<std::string> disc_set_serials;

  ALWAYS_INLINE bool HasTrait(Trait trait) const { return traits[static_cast<int>(trait)]; }

  void ApplySettings(Settings& settings, bool display_osd_messages) const;
};

void EnsureLoaded();
void Unload();

const Entry* GetEntryForDisc(CDImage* image);
const Entry* GetEntryForGameDetails(const std::string& id, u64 hash);
const Entry* GetEntryForSerial(const std::string_view& serial);
std::string GetSerialForDisc(CDImage* image);
std::string GetSerialForPath(const char* path);

const char* GetCompatibilityRatingName(CompatibilityRating rating);
const char* GetCompatibilityRatingDisplayName(CompatibilityRating rating);

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
