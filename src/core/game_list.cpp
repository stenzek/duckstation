// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "game_list.h"
#include "achievements.h"
#include "bios.h"
#include "core.h"
#include "fullscreenui.h"
#include "memory_card_image.h"
#include "psf_loader.h"
#include "settings.h"
#include "system.h"

#include "util/animated_image.h"
#include "util/cd_image.h"
#include "util/elf_file.h"
#include "util/http_downloader.h"
#include "util/image.h"
#include "util/ini_settings_interface.h"
#include "util/translation.h"

#include "common/assert.h"
#include "common/binary_reader_writer.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/heterogeneous_containers.h"
#include "common/log.h"
#include "common/path.h"
#include "common/progress_callback.h"
#include "common/string_pool.h"
#include "common/string_util.h"
#include "common/thirdparty/SmallVector.h"
#include "common/time_helpers.h"
#include "common/timer.h"

#include "fmt/format.h"

#include <algorithm>
#include <array>
#include <bit>
#include <ctime>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

LOG_CHANNEL(GameList);

#ifdef _WIN32
#include "common/windows_headers.h"
#endif

namespace GameList {
namespace {

enum : u32
{
  GAME_LIST_CACHE_SIGNATURE = 0x45434C48,
  GAME_LIST_CACHE_VERSION = 39,

  PLAYED_TIME_SERIAL_LENGTH = 32,
  PLAYED_TIME_LAST_TIME_LENGTH = 20,  // uint64
  PLAYED_TIME_TOTAL_TIME_LENGTH = 20, // uint64
  PLAYED_TIME_LINE_LENGTH =
    PLAYED_TIME_SERIAL_LENGTH + 1 + PLAYED_TIME_LAST_TIME_LENGTH + 1 + PLAYED_TIME_TOTAL_TIME_LENGTH,
};

struct PlayedTimeEntry
{
  std::time_t last_played_time;
  std::time_t total_played_time;
};

#pragma pack(push, 1)
struct MemcardTimestampCacheEntry
{
  enum : u32
  {
    MAX_SERIAL_LENGTH = 31,
  };

  char serial[MAX_SERIAL_LENGTH];
  bool icon_was_extracted;
  s64 memcard_timestamp;
};
#pragma pack(pop)

} // namespace

using CacheMap = UnorderedStringMap<Entry>;
using PlayedTimeMap = UnorderedStringMap<PlayedTimeEntry>;

static_assert(std::is_same_v<decltype(Entry::hash), GameHash>);

static bool ShouldLoadAchievementsProgress();

static bool GetExeListEntry(const std::string& path, Entry* entry);
static bool GetPsfListEntry(const std::string& path, Entry* entry);
static bool GetDiscListEntry(const std::string& path, Entry* entry);
static void MakeInvalidEntry(Entry* entry);

static void ApplyCustomAttributes(const std::string& path, Entry* entry,
                                  const INISettingsInterface& custom_attributes_ini);
static bool RescanCustomAttributesForPath(const std::string& path, const INISettingsInterface& custom_attributes_ini);
static void PopulateEntryAchievements(Entry* entry, const Achievements::ProgressDatabase& achievements_progress);
static bool GetGameListEntryFromCache(const std::string& path, Entry* entry,
                                      const INISettingsInterface& custom_attributes_ini,
                                      const Achievements::ProgressDatabase& achievements_progress);
static Entry* GetMutableEntryForPath(std::string_view path);
static void ScanDirectory(const std::string& path, bool recursive, bool only_cache,
                          const std::vector<std::string>& excluded_paths, const PlayedTimeMap& played_time_map,
                          const INISettingsInterface& custom_attributes_ini,
                          const Achievements::ProgressDatabase& achievements_progress, BinaryFileWriter& cache_writer,
                          ProgressCallback* progress);
static bool AddFileFromCache(const std::string& path, const std::string& path_in_cache, std::time_t timestamp,
                             const PlayedTimeMap& played_time_map, const INISettingsInterface& custom_attributes_ini,
                             const Achievements::ProgressDatabase& achievements_progress);
static void ScanFile(std::string path, std::time_t timestamp, std::unique_lock<std::recursive_mutex>& lock,
                     const PlayedTimeMap& played_time_map, const INISettingsInterface& custom_attributes_ini,
                     const Achievements::ProgressDatabase& achievements_progress, const std::string& path_for_cache,
                     BinaryFileWriter& cache_writer);

static bool LoadOrInitializeCache(std::FILE* fp, bool invalidate_cache);
static bool LoadEntriesFromCache(BinaryFileReader& reader);
static bool WriteEntryToCache(const Entry* entry, const std::string& entry_path, BinaryFileWriter& writer);
static void CreateDiscSetEntries(const std::vector<std::string>& excluded_paths, const PlayedTimeMap& played_time_map);

static std::string GetPlayedTimePath();
static bool ParsePlayedTimeLine(char* line, std::string_view& serial, PlayedTimeEntry& entry);
static std::string MakePlayedTimeLine(std::string_view serial, const PlayedTimeEntry& entry);
static PlayedTimeMap LoadPlayedTimeMap();
static PlayedTimeEntry UpdatePlayedTimeFile(std::string_view serial, std::time_t last_time, std::time_t add_time);

static std::string GetCustomPropertiesFile();
static const std::string& GetCustomPropertiesSection(const std::string& path, std::string* temp_path);
static bool PutCustomPropertiesField(INISettingsInterface& ini, const std::string& path, const char* field,
                                     const char* value);

static std::string GetMemcardTimestampCachePath();
static bool UpdateMemcardTimestampCache(const MemcardTimestampCacheEntry& entry);

static std::string GetAchievementGameBadgeCachePath();
static void LoadAchievementGameBadges();

struct State
{
  EntryList entries;
  std::recursive_mutex mutex;
  CacheMap cache_map;
  std::vector<MemcardTimestampCacheEntry> memcard_timestamp_cache_entries;

  // TODO: Turn this into a proper cache of achievement data, not just the badge names.
  std::vector<std::pair<u32, u32>> achievement_game_id_badges; // game_id, string_pool_offset
  BumpStringPool achievement_game_badge_names;

  bool game_list_loaded = false;
  bool achievement_game_badges_loaded = false;
};

ALIGN_TO_CACHE_LINE static State s_state;

} // namespace GameList

const char* GameList::GetEntryTypeName(EntryType type)
{
  static std::array<const char*, static_cast<int>(EntryType::MaxCount)> names = {{
    "Disc",
    "DiscSet",
    "PSExe",
    "Playlist",
    "PSF",
  }};
  return names[static_cast<size_t>(type)];
}

const char* GameList::GetEntryTypeDisplayName(EntryType type)
{
  static std::array<const char*, static_cast<int>(EntryType::MaxCount)> names = {{
    TRANSLATE_DISAMBIG_NOOP("GameList", "Disc", "EntryType"),
    TRANSLATE_DISAMBIG_NOOP("GameList", "Disc Set", "EntryType"),
    TRANSLATE_DISAMBIG_NOOP("GameList", "PS-EXE", "EntryType"),
    TRANSLATE_DISAMBIG_NOOP("GameList", "Playlist", "EntryType"),
    TRANSLATE_DISAMBIG_NOOP("GameList", "PSF", "EntryType"),
  }};
  return Host::TranslateToCString("GameList", names[static_cast<size_t>(type)], "EntryType");
}

bool GameList::IsGameListLoaded()
{
  return s_state.game_list_loaded;
}

bool GameList::ShouldShowLocalizedTitles()
{
  return Core::GetBaseBoolSettingValue("UI", "GameListShowLocalizedTitles", true);
}

bool GameList::ShouldLoadAchievementsProgress()
{
  return Core::ContainsBaseSettingValue("Cheevos", "Token");
}

bool GameList::PreferAchievementGameBadgesForIcons()
{
  return (ShouldLoadAchievementsProgress() &&
          Core::GetBaseBoolSettingValue("UI", "GameListPreferAchievementGameBadgesForIcons", false));
}

bool GameList::IsScannableFilename(std::string_view path)
{
  // we don't scan bin files because they'll duplicate
  if (StringUtil::EndsWithNoCase(path, ".bin"))
    return false;

  return (System::IsDiscPath(path) || System::IsExePath(path) || System::IsPsfPath(path));
}

bool GameList::CanEditGameSettingsForPath(const std::string_view path, const std::string_view serial)
{
  return (!path.empty() && !serial.empty() && !System::IsPsfPath(path) && !System::IsGPUDumpPath(path));
}

bool GameList::GetExeListEntry(const std::string& path, GameList::Entry* entry)
{
  const auto fp = FileSystem::OpenManagedCFile(path.c_str(), "rb");
  if (!fp)
    return false;

  entry->file_size = FileSystem::FSize64(fp.get());
  entry->uncompressed_size = entry->file_size;
  if (entry->file_size < 0)
    return false;

  // Stupid Android...
  const std::string filename = FileSystem::GetDisplayNameFromPath(path);

  entry->title = Path::GetFileTitle(filename);
  entry->type = EntryType::PSExe;

  if (StringUtil::EndsWithNoCase(filename, ".cpe"))
  {
    u32 magic;
    if (std::fread(&magic, sizeof(magic), 1, fp.get()) != 1 || magic != BIOS::CPE_MAGIC)
    {
      WARNING_LOG("{} is not a valid CPE", path);
      return false;
    }

    // Who knows
    entry->region = DiscRegion::Other;
  }
  else if (StringUtil::EndsWithNoCase(filename, ".elf"))
  {
    ELFFile::Elf32_Ehdr header;
    if (std::fread(&header, sizeof(header), 1, fp.get()) != 1 || !ELFFile::IsValidElfHeader(header))
    {
      WARNING_LOG("{} is not a valid ELF.", path);
      return false;
    }

    // Who knows
    entry->region = DiscRegion::Other;
  }
  else
  {
    BIOS::PSEXEHeader header;
    if (std::fread(&header, sizeof(header), 1, fp.get()) != 1 ||
        !BIOS::IsValidPSExeHeader(header, static_cast<size_t>(entry->file_size)))
    {
      WARNING_LOG("{} is not a valid PS-EXE", path);
      return false;
    }

    entry->region = BIOS::GetPSExeDiscRegion(header);
  }

  Error error;
  const auto data = FileSystem::ReadBinaryFile(fp.get(), &error);
  if (!data.has_value())
  {
    WARNING_LOG("Failed to read {}: {}", Path::GetFileName(path), error.GetDescription());
    return false;
  }

  const GameHash hash = System::GetGameHashFromBuffer(filename, data->cspan());
  entry->serial = hash ? System::GetGameHashId(hash) : std::string();
  return true;
}

bool GameList::GetPsfListEntry(const std::string& path, Entry* entry)
{
  // we don't need to walk the library chain here - the top file is enough
  Error error;
  PSFLoader::File file;
  if (!file.Load(path.c_str(), &error))
  {
    ERROR_LOG("Failed to load PSF file '{}': {}", Path::GetFileName(path), error.GetDescription());
    return false;
  }

  entry->serial.clear();
  entry->region = file.GetRegion();
  entry->file_size = static_cast<u32>(file.GetProgramData().size());
  entry->uncompressed_size = entry->file_size;
  entry->type = EntryType::PSF;

  // Game - Title
  std::optional<std::string> game(file.GetTagString("game"));
  if (game.has_value())
  {
    entry->title = std::move(game.value());
    entry->title += " - ";
  }
  else
  {
    entry->title.clear();
  }

  std::optional<std::string> title(file.GetTagString("title"));
  if (title.has_value())
  {
    entry->title += title.value();
  }
  else
  {
    const std::string display_name(FileSystem::GetDisplayNameFromPath(path));
    entry->title += Path::GetFileTitle(display_name);
  }

  return true;
}

bool GameList::GetDiscListEntry(const std::string& path, Entry* entry)
{
  Error error;
  std::unique_ptr<CDImage> cdi = CDImage::Open(path.c_str(), false, &error);
  if (!cdi)
  {
    ERROR_LOG("Failed to open disc image '{}': {}", Path::GetFileName(path), error.GetDescription());
    return false;
  }

  entry->path = path;
  entry->file_size = cdi->GetSizeOnDisk();
  entry->uncompressed_size = static_cast<u64>(CDImage::RAW_SECTOR_SIZE) * static_cast<u64>(cdi->GetLBACount());
  entry->type = EntryType::Disc;

  // use the same buffer for game and achievement hashing, to avoid double decompression
  std::string id, executable_name;
  std::vector<u8> executable_data;
  if (System::GetGameDetailsFromImage(cdi.get(), &id, &entry->hash, &executable_name, &executable_data))
  {
    // used for achievement count lookup later
    const std::optional<Achievements::GameHash> hash = Achievements::GetGameHash(executable_name, executable_data);
    if (hash.has_value())
      entry->achievements_hash = hash.value();
  }

  // try the database first
  const GameDatabase::Entry* dentry = GameDatabase::GetEntryForGameDetails(id, entry->hash);
  if (dentry)
  {
    // pull from database
    entry->serial = dentry->serial;
    entry->dbentry = dentry;

    if (!cdi->HasSubImages() && dentry->disc_set)
    {
      for (size_t i = 0; i < dentry->disc_set->serials.size(); i++)
      {
        if (dentry->disc_set->serials[i] == entry->serial)
        {
          entry->disc_set_index = static_cast<s8>(i);
          break;
        }
      }
    }
  }
  else
  {
    // no game code, so use the filename title
    entry->serial = std::move(id);
    entry->title = Path::GetFileTitle(FileSystem::GetDisplayNameFromPath(path));
  }

  // region detection
  entry->region = System::GetRegionForImage(cdi.get());

  if (cdi->HasSubImages())
  {
    entry->type = EntryType::Playlist;
    entry->title = Path::GetFileTitle(FileSystem::GetDisplayNameFromPath(path));

    // get the size of all the subimages
    const u32 subimage_count = cdi->GetSubImageCount();
    for (u32 i = 1; i < subimage_count; i++)
    {
      if (!cdi->SwitchSubImage(i, nullptr))
      {
        ERROR_LOG("Failed to switch to subimage {} in '{}'", i, entry->path);
        continue;
      }

      entry->uncompressed_size += static_cast<u64>(CDImage::RAW_SECTOR_SIZE) * static_cast<u64>(cdi->GetLBACount());
    }
  }

  return true;
}

void GameList::MakeInvalidEntry(Entry* entry)
{
  entry->type = EntryType::MaxCount;
  entry->region = DiscRegion::Other;
  entry->disc_set_index = -1;
  entry->disc_set_member = false;
  entry->has_custom_title = false;
  entry->has_custom_region = false;
  entry->is_runtime_populated = false;
  entry->custom_language = GameDatabase::Language::MaxCount;
  entry->path = {};
  entry->serial = {};
  entry->title = {};
  entry->dbentry = nullptr;
  entry->hash = 0;
  entry->file_size = 0;
  entry->uncompressed_size = 0;
  entry->last_modified_time = 0;
  entry->last_played_time = 0;
  entry->achievements_hash = {};
  entry->achievements_game_id = 0;
  entry->num_achievements = 0;
  entry->unlocked_achievements = 0;
  entry->unlocked_achievements_hc = 0;
}

bool GameList::PopulateEntryFromPath(const std::string& path, Entry* entry)
{
  if (System::IsExePath(path))
    return GetExeListEntry(path, entry);
  if (System::IsPsfPath(path.c_str()))
    return GetPsfListEntry(path, entry);
  return GetDiscListEntry(path, entry);
}

bool GameList::GetGameListEntryFromCache(const std::string& path, Entry* entry,
                                         const INISettingsInterface& custom_attributes_ini,
                                         const Achievements::ProgressDatabase& achievements_progress)
{
  auto iter = s_state.cache_map.find(path);
  if (iter == s_state.cache_map.end())
    return false;

  *entry = std::move(iter->second);
  entry->dbentry = GameDatabase::GetEntryForSerial(entry->serial);
  s_state.cache_map.erase(iter);
  ApplyCustomAttributes(path, entry, custom_attributes_ini);
  if (entry->IsDisc())
    PopulateEntryAchievements(entry, achievements_progress);

  return true;
}

bool GameList::LoadEntriesFromCache(BinaryFileReader& reader)
{
  u32 file_signature, file_version;
  if (!reader.ReadU32(&file_signature) || !reader.ReadU32(&file_version) ||
      file_signature != GAME_LIST_CACHE_SIGNATURE || file_version != GAME_LIST_CACHE_VERSION)
  {
    WARNING_LOG("Game list cache is corrupted");
    return false;
  }

  while (!reader.IsAtEnd())
  {
    std::string path;
    Entry ge;

    u8 type;
    u8 region;

    if (!reader.ReadU8(&type) || !reader.ReadU8(&region) || !reader.ReadSizePrefixedString(&path) ||
        !reader.ReadSizePrefixedString(&ge.serial) || !reader.ReadSizePrefixedString(&ge.title) ||
        !reader.ReadU64(&ge.hash) || !reader.ReadS64(&ge.file_size) || !reader.ReadU64(&ge.uncompressed_size) ||
        !reader.ReadU64(reinterpret_cast<u64*>(&ge.last_modified_time)) || !reader.ReadS8(&ge.disc_set_index) ||
        !reader.Read(ge.achievements_hash.data(), ge.achievements_hash.size()) ||
        region >= static_cast<u8>(DiscRegion::Count) || type > static_cast<u8>(EntryType::MaxCount))
    {
      WARNING_LOG("Game list cache entry is corrupted");
      return false;
    }

    ge.path = path;
    ge.region = static_cast<DiscRegion>(region);
    ge.type = static_cast<EntryType>(type);

    auto iter = s_state.cache_map.find(ge.path);
    if (iter != s_state.cache_map.end())
      iter->second = std::move(ge);
    else
      s_state.cache_map.emplace(std::move(path), std::move(ge));
  }

  return true;
}

bool GameList::WriteEntryToCache(const Entry* entry, const std::string& entry_path, BinaryFileWriter& writer)
{
  writer.WriteU8(static_cast<u8>(entry->type));
  writer.WriteU8(static_cast<u8>(entry->region));
  writer.WriteSizePrefixedString(entry_path);
  writer.WriteSizePrefixedString(entry->serial);
  writer.WriteSizePrefixedString(entry->has_custom_title ? std::string_view() : std::string_view(entry->title));
  writer.WriteU64(entry->hash);
  writer.WriteS64(entry->file_size);
  writer.WriteU64(entry->uncompressed_size);
  writer.WriteU64(entry->last_modified_time);
  writer.WriteS8(entry->disc_set_index);
  writer.Write(entry->achievements_hash.data(), entry->achievements_hash.size());
  return writer.IsGood();
}

bool GameList::LoadOrInitializeCache(std::FILE* fp, bool invalidate_cache)
{
  BinaryFileReader reader(fp);
  if (!invalidate_cache && !reader.IsAtEnd() && LoadEntriesFromCache(reader))
  {
    // Prepare for writing.
    return (FileSystem::FSeek64(fp, 0, SEEK_END) == 0);
  }

  WARNING_LOG("Initializing game list cache.");
  s_state.cache_map.clear();
  if (!fp)
    return false;

  // Truncate file, and re-write header.
  Error error;
  if (!FileSystem::FSeek64(fp, 0, SEEK_SET, &error) || !FileSystem::FTruncate64(fp, 0, &error))
  {
    ERROR_LOG("Failed to truncate game list cache: {}", error.GetDescription());
    return false;
  }

  BinaryFileWriter writer(fp);
  writer.WriteU32(GAME_LIST_CACHE_SIGNATURE);
  writer.WriteU32((GAME_LIST_CACHE_VERSION));
  if (!writer.Flush(&error))
  {
    ERROR_LOG("Failed to write game list cache header: {}", error.GetDescription());
    return false;
  }

  return true;
}

static bool IsPathExcluded(const std::vector<std::string>& excluded_paths, const std::string_view& path)
{
  return std::find_if(excluded_paths.begin(), excluded_paths.end(),
                      [&path](const std::string& entry) { return path.starts_with(entry); }) != excluded_paths.end();
}

void GameList::ScanDirectory(const std::string& path, bool recursive, bool only_cache,
                             const std::vector<std::string>& excluded_paths, const PlayedTimeMap& played_time_map,
                             const INISettingsInterface& custom_attributes_ini,
                             const Achievements::ProgressDatabase& achievements_progress,
                             BinaryFileWriter& cache_writer, ProgressCallback* progress)
{
  VERBOSE_LOG("Scanning {}{}", path, recursive ? " (recursively)" : "");

  progress->SetStatusText(SmallString::from_format(TRANSLATE_FS("GameList", "Scanning directory '{}'..."), path));

  // relative paths require extra care
  std::string relative_full_path;
  const bool is_relative_scan = !Path::IsAbsolute(path);
  if (is_relative_scan)
    relative_full_path = Path::Combine(EmuFolders::DataRoot, path);

  FileSystem::FindResultsArray files;
  FileSystem::FindFiles(is_relative_scan ? relative_full_path.c_str() : path.c_str(), "*",
                        (FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES) |
                          (recursive ? FILESYSTEM_FIND_RECURSIVE : 0) |
                          (is_relative_scan ? FILESYSTEM_FIND_RELATIVE_PATHS : 0),
                        &files);
  if (files.empty())
    return;

  progress->PushState();
  progress->SetProgressRange(static_cast<u32>(files.size()));
  progress->SetProgressValue(0);

  u32 files_scanned = 0;
  for (FILESYSTEM_FIND_DATA& ffd : files)
  {
    files_scanned++;

    if (progress->IsCancelled() || !IsScannableFilename(ffd.FileName) || IsPathExcluded(excluded_paths, ffd.FileName))
      continue;

    // scan dir = games, subdir/file => /root/games/subdir/file, cache path games/subdir/file
    std::string path_in_cache;
    if (is_relative_scan)
    {
      // need to prefix the relative directory
      path_in_cache = Path::Combine(path, ffd.FileName);
      ffd.FileName = Path::Combine(EmuFolders::DataRoot, path_in_cache);
    }

    std::unique_lock lock(s_state.mutex);
    if (GetEntryForPath(ffd.FileName) ||
        AddFileFromCache(ffd.FileName, path_in_cache, ffd.ModificationTime, played_time_map, custom_attributes_ini,
                         achievements_progress) ||
        only_cache)
    {
      continue;
    }

    progress->SetStatusText(SmallString::from_format(TRANSLATE_FS("GameList", "Scanning '{}'..."),
                                                     FileSystem::GetDisplayNameFromPath(ffd.FileName)));
    ScanFile(std::move(ffd.FileName), ffd.ModificationTime, lock, played_time_map, custom_attributes_ini,
             achievements_progress, path_in_cache, cache_writer);
    progress->SetProgressValue(files_scanned);
  }

  progress->SetProgressValue(files_scanned);
  progress->PopState();
}

bool GameList::AddFileFromCache(const std::string& path, const std::string& path_in_cache, std::time_t timestamp,
                                const PlayedTimeMap& played_time_map, const INISettingsInterface& custom_attributes_ini,
                                const Achievements::ProgressDatabase& achievements_progress)
{
  Entry entry;
  if (!GetGameListEntryFromCache(path_in_cache.empty() ? path : path_in_cache, &entry, custom_attributes_ini,
                                 achievements_progress) ||
      entry.last_modified_time != timestamp)
  {
    return false;
  }

  // don't add invalid entries to the list, but don't scan them either
  if (!entry.IsValid())
    return true;

  auto iter = played_time_map.find(entry.serial);
  if (iter != played_time_map.end())
  {
    entry.last_played_time = iter->second.last_played_time;
    entry.total_played_time = iter->second.total_played_time;
  }

  // for relative paths, we need to restore the full path
  if (!path_in_cache.empty())
    entry.path = path;

  s_state.entries.push_back(std::move(entry));
  return true;
}

void GameList::ScanFile(std::string path, std::time_t timestamp, std::unique_lock<std::recursive_mutex>& lock,
                        const PlayedTimeMap& played_time_map, const INISettingsInterface& custom_attributes_ini,
                        const Achievements::ProgressDatabase& achievements_progress, const std::string& path_for_cache,
                        BinaryFileWriter& cache_writer)
{
  // don't block UI while scanning
  lock.unlock();

  VERBOSE_LOG("Scanning '{}'...", path);

  Entry entry;
  if (PopulateEntryFromPath(path, &entry))
  {
    const auto iter = played_time_map.find(entry.serial);
    if (iter != played_time_map.end())
    {
      entry.last_played_time = iter->second.last_played_time;
      entry.total_played_time = iter->second.total_played_time;
    }

    ApplyCustomAttributes(path_for_cache.empty() ? path : path_for_cache, &entry, custom_attributes_ini);

    if (entry.IsDisc())
      PopulateEntryAchievements(&entry, achievements_progress);
  }
  else
  {
    MakeInvalidEntry(&entry);
  }

  entry.path = std::move(path);
  entry.last_modified_time = timestamp;

  // write the relative path to the cache if this is a relative scan
  if (cache_writer.IsOpen() &&
      !WriteEntryToCache(&entry, path_for_cache.empty() ? entry.path : path_for_cache, cache_writer)) [[unlikely]]
  {
    WARNING_LOG("Failed to write entry '{}' to cache", entry.path);
  }

  lock.lock();

  // don't add invalid entries to the list
  if (!entry.IsValid())
    return;

  // replace if present
  auto it = std::find_if(s_state.entries.begin(), s_state.entries.end(),
                         [&entry](const Entry& existing_entry) { return (existing_entry.path == entry.path); });
  if (it != s_state.entries.end())
    *it = std::move(entry);
  else
    s_state.entries.push_back(std::move(entry));
}

bool GameList::RescanCustomAttributesForPath(const std::string& path, const INISettingsInterface& custom_attributes_ini)
{
  FILESYSTEM_STAT_DATA sd;
  if (!FileSystem::StatFile(path.c_str(), &sd))
    return false;

  {
    // cancel if excluded
    const std::vector<std::string> excluded_paths(Core::GetBaseStringListSetting("GameList", "ExcludedPaths"));
    if (IsPathExcluded(excluded_paths, path))
      return false;
  }

  Entry entry;
  if (!PopulateEntryFromPath(path, &entry))
    return false;

  entry.path = path;
  entry.last_modified_time = sd.ModificationTime;

  const PlayedTimeMap played_time_map = LoadPlayedTimeMap();
  const auto iter = played_time_map.find(entry.serial);
  if (iter != played_time_map.end())
  {
    entry.last_played_time = iter->second.last_played_time;
    entry.total_played_time = iter->second.total_played_time;
  }

  ApplyCustomAttributes(entry.path, &entry, custom_attributes_ini);

  std::unique_lock lock(s_state.mutex);

  // replace if present
  auto it = std::find_if(s_state.entries.begin(), s_state.entries.end(),
                         [&entry](const Entry& existing_entry) { return (existing_entry.path == entry.path); });
  if (it != s_state.entries.end())
    *it = std::move(entry);
  else
    s_state.entries.push_back(std::move(entry));

  return true;
}

void GameList::ApplyCustomAttributes(const std::string& path, Entry* entry,
                                     const INISettingsInterface& custom_attributes_ini)
{
  std::string temp_path;
  const std::string& section = GetCustomPropertiesSection(path, &temp_path);

  std::optional<std::string> custom_title = custom_attributes_ini.GetOptionalStringValue(section.c_str(), "Title");
  if (custom_title.has_value())
  {
    entry->title = std::move(custom_title.value());
    entry->has_custom_title = true;
  }
  const std::optional<SmallString> custom_region_str =
    custom_attributes_ini.GetOptionalSmallStringValue(section.c_str(), "Region");
  if (custom_region_str.has_value())
  {
    const std::optional<DiscRegion> custom_region = Settings::ParseDiscRegionName(custom_region_str.value());
    if (custom_region.has_value())
    {
      entry->region = custom_region.value();
      entry->has_custom_region = true;
    }
    else
    {
      WARNING_LOG("Invalid region '{}' in custom attributes for '{}'", custom_region_str.value(), path);
    }
  }
  const std::optional<TinyString> custom_language_str =
    custom_attributes_ini.GetOptionalTinyStringValue(section.c_str(), "Language");
  if (custom_language_str.has_value())
  {
    const std::optional<GameDatabase::Language> custom_region =
      GameDatabase::ParseLanguageName(custom_language_str.value());
    if (custom_region.has_value())
    {
      entry->custom_language = custom_region.value();
    }
    else
    {
      WARNING_LOG("Invalid language '{}' in custom attributes for '{}'", custom_language_str.value(), path);
    }
  }
}

void GameList::PopulateEntryAchievements(Entry* entry, const Achievements::ProgressDatabase& achievements_progress)
{
  const Achievements::HashDatabaseEntry* hentry = Achievements::LookupGameHash(entry->achievements_hash);
  if (!hentry)
    return;

  entry->achievements_game_id = hentry->game_id;
  entry->num_achievements = Truncate16(hentry->num_achievements);
  entry->unlocked_achievements = 0;
  entry->unlocked_achievements_hc = 0;
  if (entry->num_achievements > 0)
  {
    const Achievements::ProgressDatabase::Entry* apd_entry = achievements_progress.LookupGame(hentry->game_id);
    if (apd_entry)
    {
      entry->unlocked_achievements = apd_entry->num_achievements_unlocked;
      entry->unlocked_achievements_hc = apd_entry->num_hc_achievements_unlocked;
    }
  }
}

void GameList::UpdateAchievementData(const std::span<u8, 16> hash, u32 game_id, u32 num_achievements, u32 num_unlocked,
                                     u32 num_unlocked_hardcore)
{
  std::unique_lock lock(s_state.mutex);
  llvm::SmallVector<u32, 32> changed_indices;

  for (size_t i = 0; i < s_state.entries.size(); i++)
  {
    Entry& entry = s_state.entries[i];
    if (std::memcmp(entry.achievements_hash.data(), hash.data(), hash.size()) != 0 &&
        entry.achievements_game_id != game_id)
    {
      continue;
    }

    if (entry.achievements_game_id == game_id && entry.num_achievements == num_achievements &&
        entry.unlocked_achievements == num_unlocked && entry.unlocked_achievements_hc == num_unlocked_hardcore)
    {
      continue;
    }

    entry.achievements_game_id = game_id;
    entry.num_achievements = Truncate16(num_achievements);
    entry.unlocked_achievements = Truncate16(num_unlocked);
    entry.unlocked_achievements_hc = Truncate16(num_unlocked_hardcore);

    changed_indices.push_back(static_cast<u32>(i));
  }

  if (!changed_indices.empty())
    Host::OnGameListEntriesChanged(changed_indices);
}

void GameList::UpdateAllAchievementData()
{
  Achievements::ProgressDatabase achievements_progress;
  if (ShouldLoadAchievementsProgress())
  {
    Error error;
    if (!achievements_progress.Load(&error))
      WARNING_LOG("Failed to load achievements progress: {}", error.GetDescription());
  }

  std::unique_lock lock(s_state.mutex);

  // this is pretty jank, but the frontend should collapse it into a single update
  std::vector<u32> changed_indices;
  for (size_t i = 0; i < s_state.entries.size(); i++)
  {
    Entry& entry = s_state.entries[i];
    if (!entry.IsDisc())
      continue;

    // Game ID is delibately not tested, because it has no effect on the UI.
    const u16 old_num_achievements = entry.num_achievements;
    const u16 old_unlocked_achievements = entry.unlocked_achievements;
    const u16 old_unlocked_achievements_hc = entry.unlocked_achievements_hc;
    PopulateEntryAchievements(&entry, achievements_progress);
    if (entry.num_achievements == old_num_achievements && entry.unlocked_achievements == old_unlocked_achievements &&
        entry.unlocked_achievements_hc == old_unlocked_achievements_hc)
    {
      // no update needed
      continue;
    }

    changed_indices.push_back(static_cast<u32>(i));
  }

  // and now the disc sets, messier :(
  for (size_t i = 0; i < s_state.entries.size(); i++)
  {
    Entry& entry = s_state.entries[i];
    if (!entry.IsDiscSet())
      continue;

    const Entry* any_entry = GetEntryBySerial(entry.serial);
    if (!any_entry)
      continue;

    if (entry.num_achievements != any_entry->num_achievements ||
        entry.unlocked_achievements != any_entry->unlocked_achievements ||
        entry.unlocked_achievements_hc != any_entry->unlocked_achievements_hc)
    {
      changed_indices.push_back(static_cast<u32>(i));
    }

    entry.achievements_game_id = any_entry->achievements_game_id;
    entry.num_achievements = any_entry->num_achievements;
    entry.unlocked_achievements = any_entry->unlocked_achievements;
    entry.unlocked_achievements_hc = any_entry->unlocked_achievements_hc;
  }

  if (!changed_indices.empty())
    Host::OnGameListEntriesChanged(changed_indices);
}

std::unique_lock<std::recursive_mutex> GameList::GetLock()
{
  return std::unique_lock(s_state.mutex);
}

std::span<const GameList::Entry> GameList::GetEntries()
{
  return s_state.entries;
}

const GameList::Entry* GameList::GetEntryByIndex(size_t index)
{
  return (index < s_state.entries.size()) ? &s_state.entries[index] : nullptr;
}

const GameList::Entry* GameList::GetEntryForPath(std::string_view path)
{
  return GetMutableEntryForPath(path);
}

GameList::Entry* GameList::GetMutableEntryForPath(std::string_view path)
{
  for (Entry& entry : s_state.entries)
  {
    // Use case-insensitive compare on Windows, since it's the same file.
#ifdef _WIN32
    if (StringUtil::EqualNoCase(entry.path, path))
      return &entry;
#else
    if (entry.path == path)
      return &entry;
#endif
  }

  return nullptr;
}

const GameList::Entry* GameList::GetEntryBySerial(std::string_view serial)
{
  const Entry* fallback_entry = nullptr;

  for (const Entry& entry : s_state.entries)
  {
    if (!entry.IsDiscSet() && entry.serial == serial)
    {
      // prefer actual discs
      if (!entry.IsDisc())
        fallback_entry = fallback_entry ? fallback_entry : &entry;
      else
        return &entry;
    }
  }

  return fallback_entry;
}

const GameList::Entry* GameList::GetEntryBySerialAndHash(std::string_view serial, u64 hash)
{
  const Entry* fallback_entry = nullptr;

  for (const Entry& entry : s_state.entries)
  {
    if (!entry.IsDiscSet() && entry.serial == serial && entry.hash == hash)
    {
      // prefer actual discs
      if (!entry.IsDisc())
        fallback_entry = fallback_entry ? fallback_entry : &entry;
      else
        return &entry;
    }
  }

  return nullptr;
}

std::vector<const GameList::Entry*> GameList::GetDiscSetMembers(const GameDatabase::DiscSetEntry* dsentry,
                                                                bool sort_by_most_recent)
{
  Assert(dsentry);

  std::vector<const Entry*> ret;
  for (const Entry& entry : s_state.entries)
  {
    if (!entry.disc_set_member || !entry.dbentry || entry.dbentry->disc_set != dsentry)
      continue;

    ret.push_back(&entry);
  }

  if (sort_by_most_recent)
  {
    std::sort(ret.begin(), ret.end(), [](const Entry* lhs, const Entry* rhs) {
      if (lhs->last_played_time == rhs->last_played_time)
        return (lhs->disc_set_index < rhs->disc_set_index);
      else
        return (lhs->last_played_time > rhs->last_played_time);
    });
  }
  else
  {
    std::sort(ret.begin(), ret.end(),
              [](const Entry* lhs, const Entry* rhs) { return (lhs->disc_set_index < rhs->disc_set_index); });
  }

  return ret;
}

const GameList::Entry* GameList::GetFirstDiscSetMember(const GameDatabase::DiscSetEntry* dsentry)
{
  Assert(dsentry);

  for (const Entry& entry : s_state.entries)
  {
    if (!entry.disc_set_member || !entry.dbentry || entry.dbentry->disc_set != dsentry)
      continue;

    // Disc set should not have been created without the first disc being present.
    if (entry.disc_set_index == 0)
      return &entry;
  }

  return nullptr;
}

size_t GameList::GetEntryCount()
{
  return s_state.entries.size();
}

void GameList::Refresh(bool invalidate_cache, bool only_cache, ProgressCallback* progress /* = nullptr */)
{
  s_state.game_list_loaded = true;

  if (!progress)
    progress = ProgressCallback::NullProgressCallback;

  Error error;
  FileSystem::LockedFile cache_file =
    FileSystem::OpenLockedFile(Path::Combine(EmuFolders::Cache, "gamelist.cache").c_str(), true, &error);
  if (!cache_file)
    ERROR_LOG("Failed to open game list cache: {}", error.GetDescription());
  else if (!LoadOrInitializeCache(cache_file.get(), invalidate_cache))
    cache_file.reset();

  BinaryFileWriter cache_writer(cache_file.get());

  // don't delete the old entries, since the frontend might still access them
  std::vector<Entry> old_entries;
  {
    std::unique_lock lock(s_state.mutex);
    old_entries.swap(s_state.entries);
  }

  const std::vector<std::string> excluded_paths(Core::GetBaseStringListSetting("GameList", "ExcludedPaths"));
  std::vector<std::string> dirs(Core::GetBaseStringListSetting("GameList", "Paths"));
  std::vector<std::string> recursive_dirs(Core::GetBaseStringListSetting("GameList", "RecursivePaths"));
  const PlayedTimeMap played_time = LoadPlayedTimeMap();
  INISettingsInterface custom_attributes_ini(GetCustomPropertiesFile());
  custom_attributes_ini.Load();

  Achievements::ProgressDatabase achievements_progress;
  if (ShouldLoadAchievementsProgress())
  {
    if (!achievements_progress.Load(&error))
      WARNING_LOG("Failed to load achievements progress: {}", error.GetDescription());
  }

#ifdef __ANDROID__
  recursive_dirs.push_back(Path::Combine(EmuFolders::DataRoot, "games"));
#endif

  if (!dirs.empty() || !recursive_dirs.empty())
  {
    progress->SetProgressRange(static_cast<u32>(dirs.size() + recursive_dirs.size()));
    progress->SetProgressValue(0);

    // we manually count it here, because otherwise pop state updates it itself
    int directory_counter = 0;
    for (const std::string& dir : dirs)
    {
      if (progress->IsCancelled())
        break;

      ScanDirectory(dir, false, only_cache, excluded_paths, played_time, custom_attributes_ini, achievements_progress,
                    cache_writer, progress);
      progress->SetProgressValue(++directory_counter);
    }
    for (const std::string& dir : recursive_dirs)
    {
      if (progress->IsCancelled())
        break;

      ScanDirectory(dir, true, only_cache, excluded_paths, played_time, custom_attributes_ini, achievements_progress,
                    cache_writer, progress);
      progress->SetProgressValue(++directory_counter);
    }
  }

  // don't need unused cache entries
  s_state.cache_map.clear();

  // merge multi-disc games
  CreateDiscSetEntries(excluded_paths, played_time);
}

GameList::EntryList GameList::TakeEntryList()
{
  EntryList ret = std::move(s_state.entries);
  s_state.entries = {};
  return ret;
}

void GameList::CreateDiscSetEntries(const std::vector<std::string>& excluded_paths,
                                    const PlayedTimeMap& played_time_map)
{
  std::unique_lock lock(s_state.mutex);

  for (size_t i = 0; i < s_state.entries.size(); i++)
  {
    const Entry& entry = s_state.entries[i];

    // only first discs can create sets
    if (entry.type != EntryType::Disc || !entry.dbentry || entry.disc_set_member || entry.disc_set_index != 0)
      continue;

    // need at least two discs for a set
    const GameDatabase::DiscSetEntry* dsentry = entry.dbentry->disc_set;
    bool found_another_disc = false;
    for (const Entry& other_entry : s_state.entries)
    {
      if (other_entry.type != EntryType::Disc || other_entry.disc_set_member || !other_entry.dbentry ||
          other_entry.dbentry->disc_set != dsentry || other_entry.disc_set_index == entry.disc_set_index)
      {
        continue;
      }
      found_another_disc = true;
      break;
    }
    if (!found_another_disc)
    {
      DEV_LOG("Not creating disc set {}, only one disc found", dsentry->title);
      continue;
    }

    Entry set_entry;
    set_entry.dbentry = entry.dbentry;
    set_entry.type = EntryType::DiscSet;
    set_entry.region = entry.region;
    set_entry.path = entry.dbentry->disc_set->GetSaveTitle();
    set_entry.serial = entry.serial;
    set_entry.hash = entry.hash;
    set_entry.file_size = 0;
    set_entry.uncompressed_size = 0;
    set_entry.last_modified_time = entry.last_modified_time;
    set_entry.last_played_time = 0;
    set_entry.total_played_time = 0;
    set_entry.achievements_hash = entry.achievements_hash;
    set_entry.achievements_game_id = entry.achievements_game_id;
    set_entry.num_achievements = entry.num_achievements;
    set_entry.unlocked_achievements = entry.unlocked_achievements;
    set_entry.unlocked_achievements_hc = entry.unlocked_achievements_hc;

    // figure out play time for all discs, and sum it
    // we do this via lookups, rather than the other entries, because of duplicates
    for (const std::string_view& set_serial : dsentry->serials)
    {
      const auto it = played_time_map.find(set_serial);
      if (it == played_time_map.end())
        continue;

      set_entry.last_played_time =
        (set_entry.last_played_time == 0) ?
          it->second.last_played_time :
          ((it->second.last_played_time != 0) ? std::max(set_entry.last_played_time, it->second.last_played_time) :
                                                set_entry.last_played_time);
      set_entry.total_played_time += it->second.total_played_time;
    }

    // mark all discs for this set as part of it, so we don't try to add them again, and for filtering
    u32 num_parts = 0;
    for (Entry& other_entry : s_state.entries)
    {
      if (other_entry.type != EntryType::Disc || other_entry.disc_set_member || !other_entry.dbentry ||
          other_entry.dbentry->disc_set != dsentry)
      {
        continue;
      }

      DEV_LOG("Adding {} to disc set {}", Path::GetFileName(other_entry.path), dsentry->title);
      other_entry.disc_set_member = true;
      set_entry.last_modified_time = std::min(set_entry.last_modified_time, other_entry.last_modified_time);
      set_entry.file_size += other_entry.file_size;
      set_entry.uncompressed_size += other_entry.uncompressed_size;
      num_parts++;
    }

    DEV_LOG("Created disc set {} from {} entries", dsentry->title, num_parts);

    // we have to do the exclusion check at the end, because otherwise the individual discs get added
    if (!IsPathExcluded(excluded_paths, dsentry->title))
      s_state.entries.push_back(std::move(set_entry));
  }
}

std::string GameList::GetCoverImagePathForEntry(const Entry* entry)
{
  return GetCoverImagePath(entry->path, entry->serial, entry->GetSaveTitle(), entry->has_custom_title);
}

static std::string GetFullCoverPath(std::string_view title, std::string_view extension)
{
  std::string filename = fmt::format("{}.{}", title, extension);
  Path::SanitizeFileName(&filename);
  return Path::Combine(EmuFolders::Covers, filename);
}

std::string GameList::GetCoverImagePath(const std::string_view path, const std::string_view serial,
                                        const std::string_view title, bool is_custom_title)
{
  static constexpr const std::array extensions = {"jpg", "jpeg", "png", "webp"};
  std::string ret;

  const auto try_name = [&ret](std::string_view name) {
    for (const char* extension : extensions)
    {
      std::string cover_path = GetFullCoverPath(name, extension);
      if (FileSystem::FileExists(cover_path.c_str()))
      {
        ret = std::move(cover_path);
        return true;
      }
    }
    return false;
  };

  // Check the title first if this is a custom title
  if (is_custom_title && try_name(title))
    return ret;

  // Prioritize lookup by serial (Most specific)
  if (!serial.empty() && try_name(serial))
    return ret;

  // Try file title (for modded games or specific like above)
  const std::string_view file_title = Path::GetFileTitle(path);
  if (!file_title.empty() && (title != file_title || is_custom_title) && try_name(file_title))
    return ret;

  // Last resort, check the game title
  if (!title.empty() && !is_custom_title && try_name(title))
    return ret;

  return ret;
}

std::string GameList::GetNewCoverImagePathForEntry(const Entry* entry, const char* new_filename, bool use_serial)
{
  const char* extension = std::strrchr(new_filename, '.');
  if (!extension)
    return {};

  std::string existing_filename = GetCoverImagePathForEntry(entry);
  if (!existing_filename.empty())
  {
    std::string::size_type pos = existing_filename.rfind('.');
    if (pos != std::string::npos && existing_filename.compare(pos, std::strlen(extension), extension) == 0)
      return existing_filename;
  }

  // Check for illegal characters, use serial instead.
  std::string filename =
    fmt::format("{}{}", use_serial ? std::string_view(entry->serial) : entry->GetSaveTitle(), extension);
  if (!Path::IsFileNameValid(filename))
    filename = fmt::format("{}{}", entry->serial, extension);

  return Path::Combine(EmuFolders::Covers, Path::SanitizeFileName(filename));
}

std::string_view GameList::Entry::GetDisplayTitle(bool localized) const
{
  // if custom title is present, use that for display too
  return !title.empty() ? std::string_view(title) :
                          (IsDiscSet() ? dbentry->disc_set->GetDisplayTitle(localized) :
                                         (dbentry ? dbentry->GetDisplayTitle(localized) : std::string_view()));
}

std::string_view GameList::Entry::GetSortTitle() const
{
  // if custom title is present, use that for sorting too
  return !title.empty() ?
           std::string_view(title) :
           (IsDiscSet() ? dbentry->disc_set->GetSortTitle() : (dbentry ? dbentry->GetSortTitle() : std::string_view()));
}

std::string_view GameList::Entry::GetSaveTitle() const
{
  // if custom title is present, use that for save folder too
  return !title.empty() ?
           std::string_view(title) :
           (IsDiscSet() ? dbentry->disc_set->GetSaveTitle() : (dbentry ? dbentry->GetSaveTitle() : std::string_view()));
}

std::string_view GameList::Entry::GetLanguageIcon() const
{
  std::string_view ret;
  if (custom_language != GameDatabase::Language::MaxCount)
    ret = GameDatabase::GetLanguageName(custom_language);
  else if (dbentry)
    ret = dbentry->GetLanguageFlagName(region);
  else
    ret = Settings::GetDiscRegionName(region);

  return ret;
}

TinyString GameList::Entry::GetLanguageIconName() const
{
  return GameDatabase::GetLanguageFlagResourceName(GetLanguageIcon());
}

TinyString GameList::Entry::GetCompatibilityIconFileName() const
{
  return TinyString::from_format(
    "images/star-{}.svg",
    static_cast<u32>(dbentry ? dbentry->compatibility : GameDatabase::CompatibilityRating::Unknown));
}

std::string GameList::Entry::GetReleaseDateString() const
{
  std::string ret;

  if (!dbentry || dbentry->release_date == 0)
    ret = TRANSLATE_STR("GameList", "Unknown");
  else
    ret = Host::FormatNumber(Host::NumberFormatType::LongDate, static_cast<s64>(dbentry->release_date));

  return ret;
}

std::string GameList::GetPlayedTimePath()
{
  return Path::Combine(EmuFolders::DataRoot, "playtime.dat");
}

bool GameList::ParsePlayedTimeLine(char* line, std::string_view& serial, PlayedTimeEntry& entry)
{
  size_t len = std::strlen(line);
  if (len != (PLAYED_TIME_LINE_LENGTH + 1)) // \n
  {
    WARNING_LOG("Malformed line: '{}'", line);
    return false;
  }

  const std::string_view serial_tok(StringUtil::StripWhitespace(std::string_view(line, PLAYED_TIME_SERIAL_LENGTH)));
  const std::string_view total_played_time_tok(
    StringUtil::StripWhitespace(std::string_view(line + PLAYED_TIME_SERIAL_LENGTH + 1, PLAYED_TIME_LAST_TIME_LENGTH)));
  const std::string_view last_played_time_tok(StringUtil::StripWhitespace(std::string_view(
    line + PLAYED_TIME_SERIAL_LENGTH + 1 + PLAYED_TIME_LAST_TIME_LENGTH + 1, PLAYED_TIME_TOTAL_TIME_LENGTH)));

  const std::optional<u64> total_played_time(StringUtil::FromChars<u64>(total_played_time_tok));
  const std::optional<u64> last_played_time(StringUtil::FromChars<u64>(last_played_time_tok));
  if (serial_tok.empty() || !last_played_time.has_value() || !total_played_time.has_value())
  {
    WARNING_LOG("Malformed line: '{}'", line);
    return false;
  }

  serial = serial_tok;
  entry.last_played_time = static_cast<std::time_t>(last_played_time.value());
  entry.total_played_time = static_cast<std::time_t>(total_played_time.value());
  return true;
}

std::string GameList::MakePlayedTimeLine(std::string_view serial, const PlayedTimeEntry& entry)
{
  return fmt::format("{:<{}} {:<{}} {:<{}}\n", serial, static_cast<unsigned>(PLAYED_TIME_SERIAL_LENGTH),
                     entry.total_played_time, static_cast<unsigned>(PLAYED_TIME_TOTAL_TIME_LENGTH),
                     entry.last_played_time, static_cast<unsigned>(PLAYED_TIME_LAST_TIME_LENGTH));
}

GameList::PlayedTimeMap GameList::LoadPlayedTimeMap()
{
  PlayedTimeMap ret;

  Error error;
  FileSystem::LockedFile fp = FileSystem::OpenLockedFile(GetPlayedTimePath().c_str(), false, &error);
  if (!fp)
  {
    ERROR_LOG("Failed to load played time map: {}", error.GetDescription());
    return ret;
  }

  char line[256];
  while (std::fgets(line, sizeof(line), fp.get()))
  {
    std::string_view serial;
    PlayedTimeEntry entry;
    if (!ParsePlayedTimeLine(line, serial, entry))
      continue;

    if (ret.find(serial) != ret.end())
    {
      WARNING_LOG("Duplicate entry: '{}'", serial);
      continue;
    }

    ret.emplace(std::string(serial), entry);
  }

  return ret;
}

GameList::PlayedTimeEntry GameList::UpdatePlayedTimeFile(const std::string_view serial, std::time_t last_time,
                                                         std::time_t add_time)
{
  const PlayedTimeEntry new_entry{last_time, add_time};

  Error error;
  FileSystem::LockedFile fp = FileSystem::OpenLockedFile(GetPlayedTimePath().c_str(), true, &error);
  if (!fp)
  {
    ERROR_LOG("Failed to open played time map for update: {}", error.GetDescription());
    return new_entry;
  }

  for (;;)
  {
    char line[256];
    const s64 line_pos = FileSystem::FTell64(fp.get());
    if (!std::fgets(line, sizeof(line), fp.get()))
      break;

    std::string_view line_serial;
    PlayedTimeEntry line_entry;
    if (!ParsePlayedTimeLine(line, line_serial, line_entry))
      continue;

    if (line_serial != serial)
      continue;

    // found it!
    line_entry.last_played_time = (last_time != 0) ? last_time : 0;
    line_entry.total_played_time = (last_time != 0) ? (line_entry.total_played_time + add_time) : 0;

    const std::string new_line = MakePlayedTimeLine(serial, line_entry);
    if (FileSystem::FSeek64(fp.get(), line_pos, SEEK_SET) != 0 ||
        std::fwrite(new_line.data(), new_line.length(), 1, fp.get()) != 1)
    {
      ERROR_LOG("Failed to update '{}' in played time map.", serial);
    }

    return line_entry;
  }

  if (last_time != 0)
  {
    // new entry.
    const std::string new_line = MakePlayedTimeLine(serial, new_entry);
    if (FileSystem::FSeek64(fp.get(), 0, SEEK_END) != 0 ||
        std::fwrite(new_line.data(), new_line.length(), 1, fp.get()) != 1)
    {
      ERROR_LOG("Failed to append '{}' to played time map.", serial);
    }
  }

  return new_entry;
}

void GameList::AddPlayedTimeForSerial(const std::string& serial, std::time_t last_time, std::time_t add_time)
{
  if (serial.empty())
    return;

  const PlayedTimeEntry pt = UpdatePlayedTimeFile(serial, last_time, add_time);
  VERBOSE_LOG("Add {} seconds play time to {} -> now {}", static_cast<unsigned>(add_time), serial.c_str(),
              static_cast<unsigned>(pt.total_played_time));

  std::unique_lock lock(s_state.mutex);
  const GameDatabase::Entry* dbentry = GameDatabase::GetEntryForSerial(serial);
  llvm::SmallVector<u32, 32> changed_indices;

  for (size_t i = 0; i < s_state.entries.size(); i++)
  {
    Entry& entry = s_state.entries[i];
    if (entry.IsDisc())
    {
      if (entry.serial != serial)
        continue;

      entry.last_played_time = pt.last_played_time;
      entry.total_played_time = pt.total_played_time;
      changed_indices.push_back(static_cast<u32>(i));
    }
    else if (entry.IsDiscSet())
    {
      if (!dbentry || entry.dbentry->disc_set != dbentry->disc_set)
        continue;

      // have to add here, because other discs are already included in the sum
      entry.last_played_time = pt.last_played_time;
      entry.total_played_time += add_time;
      changed_indices.push_back(static_cast<u32>(i));
    }
  }

  if (!changed_indices.empty())
    Host::OnGameListEntriesChanged(changed_indices);
}

void GameList::ClearPlayedTimeForSerial(const std::string& serial)
{
  if (serial.empty())
    return;

  UpdatePlayedTimeFile(serial, 0, 0);

  std::unique_lock lock(s_state.mutex);
  for (GameList::Entry& entry : s_state.entries)
  {
    if (entry.serial != serial)
      continue;

    entry.last_played_time = 0;
    entry.total_played_time = 0;
  }
}

void GameList::ClearPlayedTimeForEntry(const GameList::Entry* entry)
{
  std::unique_lock lock(s_state.mutex);
  llvm::SmallVector<std::string_view, 8> serials;

  if (entry->IsDiscSet())
  {
    for (const GameList::Entry* member : GetDiscSetMembers(entry->dbentry->disc_set))
    {
      if (!member->serial.empty())
      {
        if (std::find(serials.begin(), serials.end(), member->serial) == serials.end())
          serials.emplace_back(member->serial);
      }
    }
  }
  else if (!entry->serial.empty())
  {
    serials.emplace_back(entry->serial);
  }

  for (const auto& serial : serials)
  {
    VERBOSE_LOG("Resetting played time for {}", serial);
    UpdatePlayedTimeFile(serial, 0, 0);
  }

  for (GameList::Entry& list_entry : s_state.entries)
  {
    if (std::find(serials.begin(), serials.end(), list_entry.serial) == serials.end())
      continue;

    list_entry.last_played_time = 0;
    list_entry.total_played_time = 0;
  }
}

std::time_t GameList::GetCachedPlayedTimeForSerial(const std::string& serial)
{
  if (serial.empty())
    return 0;

  std::unique_lock lock(s_state.mutex);
  for (GameList::Entry& entry : s_state.entries)
  {
    if (entry.serial == serial)
      return entry.total_played_time;
  }

  return 0;
}

std::string GameList::FormatTimestamp(std::time_t timestamp)
{
  if (timestamp == 0)
  {
    return TRANSLATE_STR("GameList", "Never");
  }
  else
  {
    const std::time_t current_time = std::time(nullptr);

    // Avoid localtime call when more than two days have passed.
    if (current_time < timestamp || (current_time - timestamp) <= (2 * 24 * 60 * 60))
    {
      const std::optional<std::tm> ctime = Common::LocalTime(current_time);
      const std::optional<std::tm> ttime = Common::LocalTime(timestamp);
      if (ctime.has_value() && ttime.has_value() && ctime->tm_year == ttime->tm_year &&
          ctime->tm_yday == ttime->tm_yday)
      {
        return TRANSLATE_STR("GameList", "Today");
      }
      else if (ctime.has_value() && ttime.has_value() &&
               ((ctime->tm_year == ttime->tm_year && ctime->tm_yday == (ttime->tm_yday + 1)) ||
                (ctime->tm_yday == 0 && ctime->tm_mon == 0 && (ctime->tm_year - 1) == ttime->tm_year &&
                 ttime->tm_mon == 11 && ttime->tm_mday == 31)))
      {
        return TRANSLATE_STR("GameList", "Yesterday");
      }
    }

    return Host::FormatNumber(Host::NumberFormatType::ShortDate, static_cast<s64>(timestamp));
  }
}

TinyString GameList::FormatTimespan(std::time_t timespan, bool long_format)
{
  const u32 hours = static_cast<u32>(timespan / 3600);
  const u32 minutes = static_cast<u32>((timespan % 3600) / 60);
  const u32 seconds = static_cast<u32>((timespan % 3600) % 60);

  if (!long_format)
  {
    if (hours >= 100)
      return TinyString::from_format(TRANSLATE_FS("GameList", "{}h {}m"), hours, minutes);
    else if (hours > 0)
      return TinyString::from_format(TRANSLATE_FS("GameList", "{}h {}m {}s"), hours, minutes, seconds);
    else if (minutes > 0)
      return TinyString::from_format(TRANSLATE_FS("GameList", "{}m {}s"), minutes, seconds);
    else if (seconds > 0)
      return TinyString::from_format(TRANSLATE_FS("GameList", "{}s"), seconds);
    else
      return TinyString(TRANSLATE_SV("GameList", "None"));
  }
  else
  {
    if (hours > 0)
      return TRANSLATE_PLURAL_SSTR("GameList", "%n hours", "", hours);
    else if (minutes > 0)
      return TRANSLATE_PLURAL_SSTR("GameList", "%n minutes", "", minutes);
    else if (seconds > 0)
      return TRANSLATE_PLURAL_SSTR("GameList", "%n seconds", "", seconds);
    else
      return TinyString(TRANSLATE_SV("GameList", "None"));
  }
}

std::vector<std::pair<std::string_view, const GameList::Entry*>>
GameList::GetEntriesInDiscSet(const GameDatabase::DiscSetEntry* dsentry, bool localized_titles)
{
  std::vector<std::pair<std::string_view, const GameList::Entry*>> ret;
  ret.reserve(dsentry->serials.size());

  for (const std::string_view& serial : dsentry->serials)
  {
    const Entry* matching_entry = nullptr;
    bool has_multiple_entries = false;

    for (const Entry& entry : s_state.entries)
    {
      if (entry.IsDiscSet() || entry.serial != serial)
        continue;

      if (!matching_entry)
        matching_entry = &entry;
      else
        has_multiple_entries = true;
    }

    if (!matching_entry)
      continue;

    if (!has_multiple_entries)
    {
      ret.emplace_back(matching_entry->GetDisplayTitle(localized_titles), matching_entry);
      continue;
    }

    // Have to add all matching files.
    for (const Entry& entry : s_state.entries)
    {
      if (entry.IsDiscSet() || entry.serial != serial)
        continue;

      ret.emplace_back(Path::GetFileName(entry.path), &entry);
    }
  }

  return ret;
}

bool GameList::DownloadCovers(const std::vector<std::string>& url_templates, bool use_serial,
                              ProgressCallback* progress, Error* error,
                              std::function<void(const Entry*, std::string)> save_callback)
{
  if (!progress)
    progress = ProgressCallback::NullProgressCallback;

  bool has_title = false;
  bool has_localized_title = false;
  bool has_save_title = false;
  bool has_file_title = false;
  bool has_serial = false;
  for (const std::string& url_template : url_templates)
  {
    if (!has_title && url_template.find("${title}") != std::string::npos)
      has_title = true;
    if (!has_title && url_template.find("${localizedtitle}") != std::string::npos)
      has_localized_title = true;
    if (!has_title && url_template.find("${savetitle}") != std::string::npos)
      has_save_title = true;
    if (!has_file_title && url_template.find("${filetitle}") != std::string::npos)
      has_file_title = true;
    if (!has_serial && url_template.find("${serial}") != std::string::npos)
      has_serial = true;
  }
  if (!has_title && !has_save_title && !has_file_title && !has_serial)
  {
    Error::SetStringView(
      error,
      TRANSLATE_SV("GameList",
                   "URL template must contain at least one of ${title}, ${savetitle}, ${filetitle}, or ${serial}."));
    return false;
  }

  std::vector<std::pair<std::string, std::string>> download_urls;
  {
    std::unique_lock lock(s_state.mutex);
    for (const GameList::Entry& entry : s_state.entries)
    {
      const std::string existing_path(GetCoverImagePathForEntry(&entry));
      if (!existing_path.empty())
        continue;

      for (const std::string& url_template : url_templates)
      {
        std::string url(url_template);
        if (has_title)
          StringUtil::ReplaceAll(&url, "${title}", Path::URLEncode(entry.GetDisplayTitle(false)));
        if (has_localized_title)
          StringUtil::ReplaceAll(&url, "${localizedtitle}", Path::URLEncode(entry.GetDisplayTitle(true)));
        if (has_save_title)
          StringUtil::ReplaceAll(&url, "${savetitle}", Path::URLEncode(entry.GetSaveTitle()));
        if (has_file_title)
        {
          std::string display_name(FileSystem::GetDisplayNameFromPath(entry.path));
          StringUtil::ReplaceAll(&url, "${filetitle}", Path::URLEncode(Path::GetFileTitle(display_name)));
        }
        if (has_serial)
          StringUtil::ReplaceAll(&url, "${serial}", Path::URLEncode(entry.serial));

        download_urls.emplace_back(entry.path, std::move(url));
      }
    }
  }
  if (download_urls.empty())
  {
    Error::SetStringView(error, TRANSLATE_SV("GameList", "No URLs to download enumerated."));
    return false;
  }

  std::unique_ptr<HTTPDownloader> downloader(HTTPDownloader::Create(Core::GetHTTPUserAgent(), error));
  if (!downloader)
  {
    Error::AddPrefix(error, "Failed to create HTTP downloader: ");
    return false;
  }

  progress->SetCancellable(true);
  progress->SetProgressRange(static_cast<u32>(download_urls.size()));

  for (auto& [entry_path, url] : download_urls)
  {
    if (progress->IsCancelled())
      break;

    // make sure it didn't get done already
    {
      std::unique_lock lock(s_state.mutex);
      const GameList::Entry* entry = GetEntryForPath(entry_path);
      if (!entry || !GetCoverImagePathForEntry(entry).empty())
      {
        progress->IncrementProgressValue();
        continue;
      }

      progress->SetStatusText(entry->GetDisplayTitle(true));
    }

    // we could actually do a few in parallel here...
    std::string filename = Path::URLDecode(url);
    downloader->CreateRequest(std::move(url), [use_serial, &save_callback, entry_path = std::move(entry_path),
                                               filename = std::move(filename)](s32 status_code, const Error& error,
                                                                               const std::string& content_type,
                                                                               HTTPDownloader::Request::Data data) {
      if (status_code != HTTPDownloader::HTTP_STATUS_OK || data.empty())
      {
        ERROR_LOG("Download for {} failed: {}", Path::GetFileName(filename), error.GetDescription());
        return;
      }

      std::unique_lock lock(s_state.mutex);
      const GameList::Entry* entry = GetEntryForPath(entry_path);
      if (!entry || !GetCoverImagePathForEntry(entry).empty())
        return;

      // prefer the content type from the response for the extension
      // otherwise, if it's missing, and the request didn't have an extension.. fall back to jpegs.
      std::string template_filename;
      std::string content_type_extension(HTTPDownloader::GetExtensionForContentType(content_type));

      // don't treat the domain name as an extension..
      const std::string::size_type last_slash = filename.find('/');
      const std::string::size_type last_dot = filename.find('.');
      if (!content_type_extension.empty())
        template_filename = fmt::format("cover.{}", content_type_extension);
      else if (last_slash != std::string::npos && last_dot != std::string::npos && last_dot > last_slash)
        template_filename = Path::GetFileName(filename);
      else
        template_filename = "cover.jpg";

      std::string write_path(GetNewCoverImagePathForEntry(entry, template_filename.c_str(), use_serial));
      if (write_path.empty())
        return;

      if (FileSystem::WriteBinaryFile(write_path.c_str(), data.data(), data.size()) && save_callback)
        save_callback(entry, std::move(write_path));
    });
    downloader->WaitForAllRequests();
    progress->IncrementProgressValue();
  }

  return true;
}

std::string GameList::GetCustomPropertiesFile()
{
  return Path::Combine(EmuFolders::DataRoot, "custom_properties.ini");
}

const std::string& GameList::GetCustomPropertiesSection(const std::string& path, std::string* temp_path)
{
  // pretty much everything is fine in an ini section, except for square brackets.
  if (path.find_first_of("[]") == std::string::npos)
    return path;

  // otherwise, URLencode it
  return (*temp_path = Path::URLEncode(path));
}

bool GameList::PutCustomPropertiesField(INISettingsInterface& ini, const std::string& path, const char* field,
                                        const char* value)
{
  ini.Load();

  std::string temp_path;
  const std::string& section = GetCustomPropertiesSection(path, &temp_path);

  if (value && *value != '\0')
  {
    ini.SetStringValue(section.c_str(), field, value);
  }
  else
  {
    ini.DeleteValue(section.c_str(), field);
    ini.RemoveEmptySections();
  }

  Error error;
  if (!ini.Save(&error))
  {
    ERROR_LOG("Failed to save custom attributes: {}", error.GetDescription());
    return false;
  }

  return true;
}

bool GameList::SaveCustomTitleForPath(const std::string& path, const std::string& custom_title)
{
  INISettingsInterface custom_attributes_ini(GetCustomPropertiesFile());
  if (!PutCustomPropertiesField(custom_attributes_ini, path, "Title", custom_title.c_str()))
    return false;

  if (!custom_title.empty())
  {
    // Can skip the rescan and just update the value directly.
    auto lock = GetLock();
    Entry* entry = GetMutableEntryForPath(path);
    if (entry)
    {
      entry->title = custom_title;
      entry->has_custom_title = true;
    }
  }
  else
  {
    // Let the cache update by rescanning. Only need to do this on deletion, to get the original value.
    RescanCustomAttributesForPath(path, custom_attributes_ini);
  }

  return true;
}

bool GameList::SaveCustomRegionForPath(const std::string& path, const std::optional<DiscRegion> custom_region)
{
  INISettingsInterface custom_attributes_ini(GetCustomPropertiesFile());
  if (!PutCustomPropertiesField(custom_attributes_ini, path, "Region",
                                custom_region.has_value() ? Settings::GetDiscRegionName(custom_region.value()) :
                                                            nullptr))
  {
    return false;
  }

  if (custom_region.has_value())
  {
    // Can skip the rescan and just update the value directly.
    auto lock = GetLock();
    Entry* entry = GetMutableEntryForPath(path);
    if (entry)
    {
      entry->region = custom_region.value();
      entry->has_custom_region = true;
    }
  }
  else
  {
    // Let the cache update by rescanning. Only need to do this on deletion, to get the original value.
    RescanCustomAttributesForPath(path, custom_attributes_ini);
  }

  return true;
}

bool GameList::SaveCustomLanguageForPath(const std::string& path,
                                         const std::optional<GameDatabase::Language> custom_language)
{
  INISettingsInterface custom_attributes_ini(GetCustomPropertiesFile());
  if (!PutCustomPropertiesField(custom_attributes_ini, path, "Language",
                                custom_language.has_value() ? GameDatabase::GetLanguageName(custom_language.value()) :
                                                              nullptr))
  {
    return false;
  }

  // Don't need to rescan, since there's no original value to restore.
  auto lock = GetLock();
  Entry* entry = GetMutableEntryForPath(path);
  if (entry)
    entry->custom_language = custom_language.value_or(GameDatabase::Language::MaxCount);

  return true;
}

std::string GameList::GetCustomTitleForPath(const std::string_view path)
{
  std::string ret;

  std::unique_lock lock(s_state.mutex);
  const GameList::Entry* entry = GetEntryForPath(path);
  if (entry && entry->has_custom_title)
    ret = entry->title;

  return ret;
}

std::optional<DiscRegion> GameList::GetCustomRegionForPath(const std::string_view path)
{
  const GameList::Entry* entry = GetEntryForPath(path);
  if (entry && entry->has_custom_region)
    return entry->region;
  else
    return std::nullopt;
}

static constexpr const char MEMCARD_TIMESTAMP_CACHE_SIGNATURE[] = {'M', 'C', 'D', 'I', 'C', 'N', '0', '3'};

std::string GameList::GetMemcardTimestampCachePath()
{
  return Path::Combine(EmuFolders::Cache, "memcard_icons.cache");
}

void GameList::ReloadMemcardTimestampCache()
{
  s_state.memcard_timestamp_cache_entries.clear();

  Error error;
  FileSystem::LockedFile fp = FileSystem::OpenLockedFile(GetMemcardTimestampCachePath().c_str(), false, &error);
  if (!fp)
  {
    ERROR_LOG("Failed to open memory card cache for read: {}", error.GetDescription());
    return;
  }

  const s64 file_size = FileSystem::FSize64(fp.get());
  if (file_size < static_cast<s64>(sizeof(MEMCARD_TIMESTAMP_CACHE_SIGNATURE)))
    return;

  const size_t count =
    (static_cast<size_t>(file_size) - sizeof(MEMCARD_TIMESTAMP_CACHE_SIGNATURE)) / sizeof(MemcardTimestampCacheEntry);
  if (count <= 0)
    return;

  char signature[sizeof(MEMCARD_TIMESTAMP_CACHE_SIGNATURE)];
  if (std::fread(signature, sizeof(signature), 1, fp.get()) != 1 ||
      std::memcmp(signature, MEMCARD_TIMESTAMP_CACHE_SIGNATURE, sizeof(signature)) != 0)
  {
    return;
  }

  s_state.memcard_timestamp_cache_entries.resize(static_cast<size_t>(count));
  if (std::fread(s_state.memcard_timestamp_cache_entries.data(), sizeof(MemcardTimestampCacheEntry),
                 s_state.memcard_timestamp_cache_entries.size(),
                 fp.get()) != s_state.memcard_timestamp_cache_entries.size())
  {
    s_state.memcard_timestamp_cache_entries = {};
    return;
  }

  // Just in case.
  for (MemcardTimestampCacheEntry& entry : s_state.memcard_timestamp_cache_entries)
    entry.serial[sizeof(entry.serial) - 1] = 0;
}

std::string GameList::GetGameIconPath(const GameList::Entry* entry)
{
  return GetGameIconPath(entry->title, entry->serial, entry->path, entry->achievements_game_id);
}

std::string GameList::GetGameIconPath(std::string_view custom_title, std::string_view serial, std::string_view path,
                                      u32 achievements_game_id)
{
  std::string ret;

  std::string fallback_path;
  if (achievements_game_id != 0)
  {
    fallback_path = GetAchievementGameBadgePath(achievements_game_id);
    if (!fallback_path.empty() && PreferAchievementGameBadgesForIcons())
      return (ret = std::move(fallback_path));
  }

  if (serial.empty())
    return (ret = std::move(fallback_path));

  // might exist already, or the user used a custom icon
  ret = Path::Combine(EmuFolders::GameIcons, TinyString::from_format("{}.png", serial));
  if (FileSystem::FileExists(ret.c_str()))
    return ret;

  MemoryCardType type;
  std::string memcard_path = System::GetGameMemoryCardPath(custom_title, serial, path, 0, &type);
  FILESYSTEM_STAT_DATA memcard_sd;
  if (memcard_path.empty() || type == MemoryCardType::Shared ||
      !FileSystem::StatFile(memcard_path.c_str(), &memcard_sd))
  {
    return (ret = std::move(fallback_path));
  }

  const s64 timestamp = memcard_sd.ModificationTime;
  TinyString index_serial;
  index_serial.assign(
    serial.substr(0, std::min<size_t>(serial.length(), MemcardTimestampCacheEntry::MAX_SERIAL_LENGTH - 1)));

  MemcardTimestampCacheEntry* cache_entry = nullptr;
  for (MemcardTimestampCacheEntry& it : s_state.memcard_timestamp_cache_entries)
  {
    if (StringUtil::EqualNoCase(index_serial, it.serial))
    {
      // user might've deleted the file, so re-extract it if so
      // otherwise, card hasn't changed, still no icon
      if (it.memcard_timestamp == timestamp && !it.icon_was_extracted)
        return (ret = std::move(fallback_path));

      cache_entry = &it;
      break;
    }
  }

  if (!cache_entry)
  {
    cache_entry = &s_state.memcard_timestamp_cache_entries.emplace_back();
    std::memset(cache_entry, 0, sizeof(MemcardTimestampCacheEntry));
  }

  cache_entry->memcard_timestamp = timestamp;
  cache_entry->icon_was_extracted = false;
  StringUtil::Strlcpy(cache_entry->serial, index_serial.view(), sizeof(cache_entry->serial));

  // Try extracting an icon.
  Error error;
  std::unique_ptr<MemoryCardImage::DataArray> data = std::make_unique<MemoryCardImage::DataArray>();
  if (MemoryCardImage::LoadFromFile(data.get(), memcard_path.c_str(), &error))
  {
    std::vector<MemoryCardImage::FileInfo> files = MemoryCardImage::EnumerateFiles(*data.get(), false);
    if (!files.empty())
    {
      const MemoryCardImage::FileInfo& fi = files.front();
      if (!fi.icon_frames.empty())
      {
        INFO_LOG("Extracting memory card icon from {} ({}) to {}", fi.filename, Path::GetFileTitle(memcard_path),
                 Path::GetFileTitle(ret));

        static constexpr AnimatedImage::FrameDelay ICON_FRAME_DELAY = {1, 5}; // 200ms per frame
        AnimatedImage image(MemoryCardImage::ICON_WIDTH, MemoryCardImage::ICON_HEIGHT,
                            static_cast<u32>(fi.icon_frames.size()), ICON_FRAME_DELAY);
        for (size_t i = 0; i < fi.icon_frames.size(); i++)
          image.SetPixels(static_cast<u32>(i), fi.icon_frames[i].pixels, MemoryCardImage::ICON_WIDTH * sizeof(u32));

        cache_entry->icon_was_extracted = image.SaveToFile(ret.c_str(), AnimatedImage::DEFAULT_SAVE_QUALITY, &error);
        if (cache_entry->icon_was_extracted)
          return ret;
        else
          ERROR_LOG("Failed to save memory card icon to {}: {}", Path::GetFileName(ret), error.GetDescription());
      }
    }
  }
  else
  {
    ERROR_LOG("Failed to load memory card '{}': {}", Path::GetFileName(memcard_path), error.GetDescription());
  }

  UpdateMemcardTimestampCache(*cache_entry);
  return (ret = std::move(fallback_path));
}

bool GameList::UpdateMemcardTimestampCache(const MemcardTimestampCacheEntry& entry)
{
  Error error;
  FileSystem::LockedFile fp = FileSystem::OpenLockedFile(GetMemcardTimestampCachePath().c_str(), true, &error);
  if (!fp)
  {
    ERROR_LOG("Failed to open memory card cache for update: {}", error.GetDescription());
    return false;
  }

  // check signature, write it if it's non-existent or invalid
  char signature[sizeof(MEMCARD_TIMESTAMP_CACHE_SIGNATURE)];
  if (std::fread(signature, sizeof(signature), 1, fp.get()) != 1 ||
      std::memcmp(signature, MEMCARD_TIMESTAMP_CACHE_SIGNATURE, sizeof(signature)) != 0)
  {
    if (!FileSystem::FTruncate64(fp.get(), 0) || FileSystem::FSeek64(fp.get(), 0, SEEK_SET) != 0 ||
        std::fwrite(MEMCARD_TIMESTAMP_CACHE_SIGNATURE, sizeof(MEMCARD_TIMESTAMP_CACHE_SIGNATURE), 1, fp.get()) != 1)
    {
      return false;
    }
  }

  // need to seek to switch from read->write?
  s64 current_pos = sizeof(MEMCARD_TIMESTAMP_CACHE_SIGNATURE);
  if (FileSystem::FSeek64(fp.get(), current_pos, SEEK_SET) != 0)
    return false;

  for (;;)
  {
    MemcardTimestampCacheEntry existing_entry;
    if (std::fread(&existing_entry, sizeof(existing_entry), 1, fp.get()) != 1)
      break;

    existing_entry.serial[sizeof(existing_entry.serial) - 1] = 0;
    if (!StringUtil::EqualNoCase(existing_entry.serial, entry.serial))
    {
      current_pos += sizeof(existing_entry);
      continue;
    }

    // found it here, so overwrite
    return (FileSystem::FSeek64(fp.get(), current_pos, SEEK_SET) == 0 &&
            std::fwrite(&entry, sizeof(entry), 1, fp.get()) == 1);
  }

  if (FileSystem::FSeek64(fp.get(), current_pos, SEEK_SET) != 0)
    return false;

  // append it.
  return (std::fwrite(&entry, sizeof(entry), 1, fp.get()) == 1);
}

std::string GameList::GetAchievementGameBadgeCachePath()
{
  return Path::Combine(EmuFolders::Cache, "achievement_game_badges.cache");
}

std::string GameList::GetAchievementGameBadgePath(u32 game_id)
{
  LoadAchievementGameBadges();

  std::string ret;

  const auto iter =
    std::lower_bound(s_state.achievement_game_id_badges.begin(), s_state.achievement_game_id_badges.end(), game_id,
                     [](const auto& entry, u32 search) { return entry.first < search; });
  if (iter != s_state.achievement_game_id_badges.end() && iter->first == game_id)
  {
    const std::string_view badge_name = s_state.achievement_game_badge_names.GetString(iter->second);
    if (!badge_name.empty())
    {
      ret = Achievements::GetGameBadgePath(badge_name);
      if (!FileSystem::FileExists(ret.c_str()))
        ret.clear();
    }
  }

  return ret;
}

void GameList::LoadAchievementGameBadges()
{
  if (s_state.achievement_game_badges_loaded)
    return;

  s_state.achievement_game_badges_loaded = true;

  Error error;
  FileSystem::LockedFile fp = FileSystem::OpenLockedFile(GetAchievementGameBadgeCachePath().c_str(), false, &error);
  if (!fp)
  {
    ERROR_LOG("Failed to load cache: {}", error.GetDescription());
    return;
  }

  // avoid heap allocations by using the file size as a guide
  static constexpr u32 MAX_RESERVE_SIZE = 1 * 1024 * 1024;
  s_state.achievement_game_badge_names.Reserve(
    static_cast<size_t>(std::clamp<s64>(FileSystem::FSize64(fp.get()), 0, MAX_RESERVE_SIZE)));

  char line[256];
  while (std::fgets(line, sizeof(line), fp.get()))
  {
    const std::string_view line_sv = StringUtil::StripWhitespace(line);
    if (line_sv.empty())
      continue;

    const std::string_view::size_type pos = line_sv.find(',');
    if (pos != std::string_view::npos)
    {
      const std::optional<u32> game_id = StringUtil::FromChars<u32>(line_sv.substr(0, pos));
      const std::string_view badge_name = StringUtil::StripWhitespace(line_sv.substr(pos + 1));
      if (game_id.has_value() && !badge_name.empty())
      {
        s_state.achievement_game_id_badges.emplace_back(
          game_id.value(), static_cast<u32>(s_state.achievement_game_badge_names.AddString(badge_name)));
        continue;
      }
    }

    WARNING_LOG("Malformed line in cache: '{}'", line_sv);
  }

  DEV_LOG("Loaded {} achievement badge names", s_state.achievement_game_id_badges.size());

  // the file may not be sorted, so sort it now.
  std::sort(s_state.achievement_game_id_badges.begin(), s_state.achievement_game_id_badges.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
}

void GameList::UpdateAchievementBadgeName(u32 game_id, std::string_view badge_name)
{
  if (game_id == 0)
    return;

  std::unique_lock lock(s_state.mutex);

  LoadAchievementGameBadges();

  const auto iter =
    std::lower_bound(s_state.achievement_game_id_badges.begin(), s_state.achievement_game_id_badges.end(), game_id,
                     [](const auto& entry, u32 search) { return entry.first < search; });
  bool game_exists = false;
  if (iter != s_state.achievement_game_id_badges.end() && iter->first == game_id)
  {
    if (s_state.achievement_game_badge_names.GetString(iter->second) == badge_name)
      return;

    iter->second = static_cast<u32>(s_state.achievement_game_badge_names.AddString(badge_name));
    game_exists = true;
  }
  else
  {
    s_state.achievement_game_id_badges.insert(
      iter, {game_id, static_cast<u32>(s_state.achievement_game_badge_names.AddString(badge_name))});
  }

  Error error;
  FileSystem::LockedFile fp = FileSystem::OpenLockedFile(GetAchievementGameBadgeCachePath().c_str(), true, &error);
  if (!fp)
  {
    ERROR_LOG("Failed to open cache for update: {}", error.GetDescription());
    return;
  }

  // this is really terrible, but the case where a badge name changes is so rare that it's not worth handling well
  if (game_exists)
  {
    if (!FileSystem::FTruncate64(fp.get(), 0, &error))
    {
      ERROR_LOG("Failed to truncate cache: {}", error.GetDescription());
      return;
    }

    for (const auto& entry : s_state.achievement_game_id_badges)
    {
      const std::string_view entry_badge = s_state.achievement_game_badge_names.GetString(entry.second);
      if (std::fprintf(fp.get(), "%u,%.*s\n", entry.first, static_cast<int>(entry_badge.size()), entry_badge.data()) <
          0)
      {
        ERROR_LOG("Failed to rewrite cache: errno {}", errno);
      }
    }
  }
  else
  {
    if (!FileSystem::FSeek64(fp.get(), 0, SEEK_END, &error) ||
        std::fprintf(fp.get(), "%u,%.*s\n", game_id, static_cast<int>(badge_name.size()), badge_name.data()) < 0)
    {
      ERROR_LOG("Failed to append to cache: errno {}", errno);
    }
  }
}
