// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "game_database.h"
#include "types.h"

#include "util/cd_image.h"

#include "common/small_string.h"

#include <ctime>
#include <functional>
#include <mutex>
#include <span>
#include <string>

class ProgressCallback;

namespace GameList {
enum class EntryType : u8
{
  Disc,
  DiscSet,
  PSExe,
  Playlist,
  PSF,
  MaxCount
};

struct Entry
{
  EntryType type = EntryType::MaxCount;
  DiscRegion region = DiscRegion::Other;

  s8 disc_set_index = -1;
  bool disc_set_member = false;
  bool has_custom_title = false;
  bool has_custom_region = false;
  GameDatabase::Language custom_language = GameDatabase::Language::MaxCount;

  std::string path;
  std::string serial;
  std::string title;
  std::string disc_set_name;

  const GameDatabase::Entry* dbentry = nullptr;

  u64 hash = 0;
  s64 file_size = 0;
  u64 uncompressed_size = 0;
  std::time_t last_modified_time = 0;
  std::time_t last_played_time = 0;
  std::time_t total_played_time = 0;

  std::array<u8, 16> achievements_hash = {};
  u32 achievements_game_id = 0;
  u16 num_achievements = 0;
  u16 unlocked_achievements = 0;
  u16 unlocked_achievements_hc = 0;

  std::string_view GetLanguageIcon() const;

  TinyString GetLanguageIconName() const;
  TinyString GetCompatibilityIconFileName() const;

  TinyString GetReleaseDateString() const;

  ALWAYS_INLINE bool IsValid() const { return (type < EntryType::MaxCount); }
  ALWAYS_INLINE bool IsDisc() const { return (type == EntryType::Disc); }
  ALWAYS_INLINE bool IsDiscSet() const { return (type == EntryType::DiscSet); }
  ALWAYS_INLINE bool HasCustomLanguage() const { return (custom_language != GameDatabase::Language::MaxCount); }
  ALWAYS_INLINE EntryType GetSortType() const { return (type == EntryType::DiscSet) ? EntryType::Disc : type; }
  ALWAYS_INLINE bool AreAchievementsMastered() const
  {
    return (num_achievements > 0 &&
            ((unlocked_achievements > unlocked_achievements_hc) ? unlocked_achievements : unlocked_achievements_hc) ==
              num_achievements);
  }
};

using EntryList = std::vector<Entry>;

const char* GetEntryTypeName(EntryType type);
const char* GetEntryTypeDisplayName(EntryType type);

bool IsScannableFilename(std::string_view path);

/// Populates a game list entry struct with information from the iso/elf.
/// Do *not* call while the system is running, it will mess with CDVD state.
bool PopulateEntryFromPath(const std::string& path, Entry* entry);

// Game list access. It's the caller's responsibility to hold the lock while manipulating the entry in any way.
std::unique_lock<std::recursive_mutex> GetLock();
const Entry* GetEntryByIndex(u32 index);
const Entry* GetEntryForPath(std::string_view path);
const Entry* GetEntryBySerial(std::string_view serial);
const Entry* GetEntryBySerialAndHash(std::string_view serial, u64 hash);
std::vector<const Entry*> GetDiscSetMembers(std::string_view disc_set_name, bool sort_by_most_recent = false);
const Entry* GetFirstDiscSetMember(std::string_view disc_set_name);
u32 GetEntryCount();

bool IsGameListLoaded();

/// Populates the game list with files in the configured directories.
/// If invalidate_cache is set, all files will be re-scanned.
/// If only_cache is set, no new files will be scanned, only those present in the cache.
void Refresh(bool invalidate_cache, bool only_cache = false, ProgressCallback* progress = nullptr);

/// Moves the current game list, which can be temporarily displayed in the UI until refresh completes.
/// The caller **must** call Refresh() afterward, otherwise it will be permanently lost.
EntryList TakeEntryList();

/// Add played time for the specified serial.
void AddPlayedTimeForSerial(const std::string& serial, std::time_t last_time, std::time_t add_time);

/// Resets played time for the specified serial to zero.
void ClearPlayedTimeForSerial(const std::string& serial);

/// Resets played time for the specified entry to zero.
void ClearPlayedTimeForEntry(const Entry* entry);

/// Returns the total time played for a game. Requires the game to be scanned in the list.
std::time_t GetCachedPlayedTimeForSerial(const std::string& serial);

/// Formats a timestamp to something human readable (e.g. Today, Yesterday, 10/11/12).
TinyString FormatTimestamp(std::time_t timestamp);

/// Formats a timespan to something human readable (e.g. 1h2m3s or 1 hour).
TinyString FormatTimespan(std::time_t timespan, bool long_format = false);

std::string GetCoverImagePathForEntry(const Entry* entry);
std::string GetCoverImagePath(const std::string& path, const std::string& serial, const std::string& title);
std::string GetNewCoverImagePathForEntry(const Entry* entry, const char* new_filename, bool use_serial);

/// Returns a list of (title, entry) for entries matching serials. Titles will match the gamedb title,
/// except when two files have the same serial, in which case the filename will be used instead.
std::vector<std::pair<std::string, const Entry*>>
GetMatchingEntriesForSerial(const std::span<const std::string> serials);

/// Downloads covers using the specified URL templates. By default, covers are saved by title, but this can be changed
/// with the use_serial parameter. save_callback optionall takes the entry and the path the new cover is saved to.
bool DownloadCovers(const std::vector<std::string>& url_templates, bool use_serial = false,
                    ProgressCallback* progress = nullptr,
                    std::function<void(const Entry*, std::string)> save_callback = {});

// Custom properties support
bool SaveCustomTitleForPath(const std::string& path, const std::string& custom_title);
bool SaveCustomRegionForPath(const std::string& path, const std::optional<DiscRegion> custom_region);
bool SaveCustomLanguageForPath(const std::string& path, const std::optional<GameDatabase::Language> custom_language);
std::string GetCustomTitleForPath(const std::string_view path);
std::optional<DiscRegion> GetCustomRegionForPath(const std::string_view path);

/// The purpose of this cache is to stop us trying to constantly extract memory card icons, when we know a game
/// doesn't have any saves yet. It caches the serial:memcard_timestamp pair, and only tries extraction when the
/// timestamp of the memory card has changed.
std::string GetGameIconPath(std::string_view serial, std::string_view path);
void ReloadMemcardTimestampCache();

/// Updates game list with new achievement unlocks.
void UpdateAchievementData(const std::span<u8, 16> hash, u32 game_id, u32 num_achievements, u32 num_unlocked,
                           u32 num_unlocked_hardcore);
void UpdateAllAchievementData();

} // namespace GameList

namespace Host {
/// Asynchronously starts refreshing the game list.
void RefreshGameListAsync(bool invalidate_cache);

/// Cancels game list refresh, if there is one in progress.
void CancelGameListRefresh();

/// Called when game list rows are updated.
void OnGameListEntriesChanged(std::span<const u32> changed_indices);
} // namespace Host
