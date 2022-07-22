#pragma once
#include "core/game_database.h"
#include "core/types.h"
#include "util/cd_image.h"
#include <ctime>
#include <mutex>
#include <string>

class ByteStream;
class ProgressCallback;

struct SystemBootParameters;

namespace GameList {
enum class EntryType
{
  Disc,
  PSExe,
  Playlist,
  PSF,
  Count
};

struct Entry
{
  EntryType type = EntryType::Disc;
  DiscRegion region = DiscRegion::Other;

  std::string path;
  std::string serial;
  std::string title;
  std::string genre;
  std::string publisher;
  std::string developer;
  u64 total_size = 0;
  std::time_t last_modified_time = 0;

  u64 release_date = 0;
  u32 supported_controllers = ~static_cast<u32>(0);
  u8 min_players = 1;
  u8 max_players = 1;
  u8 min_blocks = 0;
  u8 max_blocks = 0;

  GameDatabase::CompatibilityRating compatibility = GameDatabase::CompatibilityRating::Unknown;

  size_t GetReleaseDateString(char* buffer, size_t buffer_size) const;

  ALWAYS_INLINE bool IsDisc() const { return (type == EntryType::Disc); }

  static_assert(sizeof(std::time_t) == sizeof(u64));
};

const char* GetEntryTypeName(EntryType type);
const char* GetEntryTypeDisplayName(EntryType type);

bool IsScannableFilename(const std::string_view& path);

/// Populates a game list entry struct with information from the iso/elf.
/// Do *not* call while the system is running, it will mess with CDVD state.
bool PopulateEntryFromPath(const std::string& path, Entry* entry);

// Game list access. It's the caller's responsibility to hold the lock while manipulating the entry in any way.
std::unique_lock<std::recursive_mutex> GetLock();
const Entry* GetEntryByIndex(u32 index);
const Entry* GetEntryForPath(const char* path);
const Entry* GetEntryBySerial(const std::string_view& serial);
u32 GetEntryCount();

bool IsGameListLoaded();

/// Populates the game list with files in the configured directories.
/// If invalidate_cache is set, all files will be re-scanned.
/// If only_cache is set, no new files will be scanned, only those present in the cache.
void Refresh(bool invalidate_cache, bool only_cache = false, ProgressCallback* progress = nullptr);

std::string GetCoverImagePathForEntry(const Entry* entry);
std::string GetCoverImagePath(const std::string& path, const std::string& serial, const std::string& title);
std::string GetNewCoverImagePathForEntry(const Entry* entry, const char* new_filename);
}; // namespace GameList

namespace Host {
/// Asynchronously starts refreshing the game list.
void RefreshGameListAsync(bool invalidate_cache);

/// Cancels game list refresh, if there is one in progress.
void CancelGameListRefresh();
} // namespace Host