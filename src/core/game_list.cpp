// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "game_list.h"
#include "achievements.h"
#include "bios.h"
#include "fullscreen_ui.h"
#include "host.h"
#include "memory_card_image.h"
#include "psf_loader.h"
#include "settings.h"
#include "system.h"

#include "util/cd_image.h"
#include "util/elf_file.h"
#include "util/http_downloader.h"
#include "util/image.h"
#include "util/ini_settings_interface.h"

#include "common/assert.h"
#include "common/binary_reader_writer.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/heterogeneous_containers.h"
#include "common/log.h"
#include "common/path.h"
#include "common/progress_callback.h"
#include "common/string_util.h"
#include "common/thirdparty/SmallVector.h"
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
  GAME_LIST_CACHE_VERSION = 38,

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

using CacheMap = PreferUnorderedStringMap<Entry>;
using PlayedTimeMap = PreferUnorderedStringMap<PlayedTimeEntry>;

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

static std::string GetPlayedTimeFile();
static bool ParsePlayedTimeLine(char* line, std::string& serial, PlayedTimeEntry& entry);
static std::string MakePlayedTimeLine(const std::string& serial, const PlayedTimeEntry& entry);
static PlayedTimeMap LoadPlayedTimeMap(const std::string& path);
static PlayedTimeEntry UpdatePlayedTimeFile(const std::string& path, const std::string& serial, std::time_t last_time,
                                            std::time_t add_time);

static std::string GetCustomPropertiesFile();
static const std::string& GetCustomPropertiesSection(const std::string& path, std::string* temp_path);
static bool PutCustomPropertiesField(INISettingsInterface& ini, const std::string& path, const char* field,
                                     const char* value);

static FileSystem::ManagedCFilePtr OpenMemoryCardTimestampCache(bool for_write);
static bool UpdateMemcardTimestampCache(const MemcardTimestampCacheEntry& entry);

static EntryList s_entries;
static std::recursive_mutex s_mutex;
static CacheMap s_cache_map;
static std::vector<MemcardTimestampCacheEntry> s_memcard_timestamp_cache_entries;

static bool s_game_list_loaded = false;

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
  return s_game_list_loaded;
}

bool GameList::ShouldShowLocalizedTitles()
{
  return Host::GetBaseBoolSettingValue("UI", "GameListShowLocalizedTitles", true);
}

bool GameList::IsScannableFilename(std::string_view path)
{
  // we don't scan bin files because they'll duplicate
  if (StringUtil::EndsWithNoCase(path, ".bin"))
    return false;

  return (System::IsDiscPath(path) || System::IsExePath(path) || System::IsPsfPath(path));
}

bool GameList::ShouldLoadAchievementsProgress()
{
  return Host::ContainsBaseSettingValue("Cheevos", "Token");
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

  const auto data = FileSystem::ReadBinaryFile(fp.get());
  if (!data.has_value())
  {
    WARNING_LOG("Failed to read {}", path);
    return false;
  }

  const GameHash hash = System::GetGameHashFromBuffer(filename, data->cspan());
  entry->serial = hash ? System::GetGameHashId(hash) : std::string();
  return true;
}

bool GameList::GetPsfListEntry(const std::string& path, Entry* entry)
{
  // we don't need to walk the library chain here - the top file is enough
  PSFLoader::File file;
  if (!file.Load(path.c_str(), nullptr))
    return false;

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
  std::unique_ptr<CDImage> cdi = CDImage::Open(path.c_str(), false, nullptr);
  if (!cdi)
    return false;

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

    std::string image_title(cdi->GetMetadata("title"));
    if (!image_title.empty())
      entry->title = std::move(image_title);

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
  auto iter = s_cache_map.find(path);
  if (iter == s_cache_map.end())
    return false;

  *entry = std::move(iter->second);
  entry->dbentry = GameDatabase::GetEntryForSerial(entry->serial);
  s_cache_map.erase(iter);
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

    auto iter = s_cache_map.find(ge.path);
    if (iter != s_cache_map.end())
      iter->second = std::move(ge);
    else
      s_cache_map.emplace(std::move(path), std::move(ge));
  }

  return true;
}

bool GameList::WriteEntryToCache(const Entry* entry, const std::string& entry_path, BinaryFileWriter& writer)
{
  writer.WriteU8(static_cast<u8>(entry->type));
  writer.WriteU8(static_cast<u8>(entry->region));
  writer.WriteSizePrefixedString(entry_path);
  writer.WriteSizePrefixedString(entry->serial);
  writer.WriteSizePrefixedString(entry->title);
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
  s_cache_map.clear();
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

    std::unique_lock lock(s_mutex);
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

  s_entries.push_back(std::move(entry));
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
  auto it = std::find_if(s_entries.begin(), s_entries.end(),
                         [&entry](const Entry& existing_entry) { return (existing_entry.path == entry.path); });
  if (it != s_entries.end())
    *it = std::move(entry);
  else
    s_entries.push_back(std::move(entry));
}

bool GameList::RescanCustomAttributesForPath(const std::string& path, const INISettingsInterface& custom_attributes_ini)
{
  FILESYSTEM_STAT_DATA sd;
  if (!FileSystem::StatFile(path.c_str(), &sd))
    return false;

  {
    // cancel if excluded
    const std::vector<std::string> excluded_paths(Host::GetBaseStringListSetting("GameList", "ExcludedPaths"));
    if (IsPathExcluded(excluded_paths, path))
      return false;
  }

  Entry entry;
  if (!PopulateEntryFromPath(path, &entry))
    return false;

  entry.path = path;
  entry.last_modified_time = sd.ModificationTime;

  const PlayedTimeMap played_time_map(LoadPlayedTimeMap(GetPlayedTimeFile()));
  const auto iter = played_time_map.find(entry.serial);
  if (iter != played_time_map.end())
  {
    entry.last_played_time = iter->second.last_played_time;
    entry.total_played_time = iter->second.total_played_time;
  }

  ApplyCustomAttributes(entry.path, &entry, custom_attributes_ini);

  std::unique_lock lock(s_mutex);

  // replace if present
  auto it = std::find_if(s_entries.begin(), s_entries.end(),
                         [&entry](const Entry& existing_entry) { return (existing_entry.path == entry.path); });
  if (it != s_entries.end())
    *it = std::move(entry);
  else
    s_entries.push_back(std::move(entry));

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
  std::unique_lock lock(s_mutex);
  llvm::SmallVector<u32, 32> changed_indices;

  for (size_t i = 0; i < s_entries.size(); i++)
  {
    Entry& entry = s_entries[i];
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

  std::unique_lock lock(s_mutex);

  // this is pretty jank, but the frontend should collapse it into a single update
  std::vector<u32> changed_indices;
  for (size_t i = 0; i < s_entries.size(); i++)
  {
    Entry& entry = s_entries[i];
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
  for (size_t i = 0; i < s_entries.size(); i++)
  {
    Entry& entry = s_entries[i];
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
  return std::unique_lock(s_mutex);
}

std::span<const GameList::Entry> GameList::GetEntries()
{
  return s_entries;
}

const GameList::Entry* GameList::GetEntryByIndex(u32 index)
{
  return (index < s_entries.size()) ? &s_entries[index] : nullptr;
}

const GameList::Entry* GameList::GetEntryForPath(std::string_view path)
{
  return GetMutableEntryForPath(path);
}

GameList::Entry* GameList::GetMutableEntryForPath(std::string_view path)
{
  for (Entry& entry : s_entries)
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

  for (const Entry& entry : s_entries)
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

  for (const Entry& entry : s_entries)
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
  for (const Entry& entry : s_entries)
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

  for (const Entry& entry : s_entries)
  {
    if (!entry.disc_set_member || !entry.dbentry || entry.dbentry->disc_set != dsentry)
      continue;

    // Disc set should not have been created without the first disc being present.
    if (entry.disc_set_index == 0)
      return &entry;
  }

  return nullptr;
}

u32 GameList::GetEntryCount()
{
  return static_cast<u32>(s_entries.size());
}

void GameList::Refresh(bool invalidate_cache, bool only_cache, ProgressCallback* progress /* = nullptr */)
{
  s_game_list_loaded = true;

  if (!progress)
    progress = ProgressCallback::NullProgressCallback;

  Error error;
  FileSystem::ManagedCFilePtr cache_file =
    FileSystem::OpenExistingOrCreateManagedCFile(Path::Combine(EmuFolders::Cache, "gamelist.cache").c_str(), 0, &error);
  if (!cache_file)
    ERROR_LOG("Failed to open game list cache: {}", error.GetDescription());

#ifdef HAS_POSIX_FILE_LOCK
  // Lock cache file for multi-instance on Linux. Implicitly done on Windows.
  std::optional<FileSystem::POSIXLock> cache_file_lock;
  if (cache_file)
    cache_file_lock.emplace(cache_file.get());
  if (!LoadOrInitializeCache(cache_file.get(), invalidate_cache))
  {
    cache_file_lock.reset();
    cache_file.reset();
  }
#else
  if (!LoadOrInitializeCache(cache_file.get(), invalidate_cache))
    cache_file.reset();
#endif
  BinaryFileWriter cache_writer(cache_file.get());

  // don't delete the old entries, since the frontend might still access them
  std::vector<Entry> old_entries;
  {
    std::unique_lock lock(s_mutex);
    old_entries.swap(s_entries);
  }

  const std::vector<std::string> excluded_paths(Host::GetBaseStringListSetting("GameList", "ExcludedPaths"));
  std::vector<std::string> dirs(Host::GetBaseStringListSetting("GameList", "Paths"));
  std::vector<std::string> recursive_dirs(Host::GetBaseStringListSetting("GameList", "RecursivePaths"));
  const PlayedTimeMap played_time(LoadPlayedTimeMap(GetPlayedTimeFile()));
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
  s_cache_map.clear();

  // merge multi-disc games
  CreateDiscSetEntries(excluded_paths, played_time);
}

GameList::EntryList GameList::TakeEntryList()
{
  EntryList ret = std::move(s_entries);
  s_entries = {};
  return ret;
}

void GameList::CreateDiscSetEntries(const std::vector<std::string>& excluded_paths,
                                    const PlayedTimeMap& played_time_map)
{
  std::unique_lock lock(s_mutex);

  for (size_t i = 0; i < s_entries.size(); i++)
  {
    const Entry& entry = s_entries[i];

    // only first discs can create sets
    if (entry.type != EntryType::Disc || !entry.dbentry || entry.disc_set_member || entry.disc_set_index != 0)
      continue;

    // need at least two discs for a set
    const GameDatabase::DiscSetEntry* dsentry = entry.dbentry->disc_set;
    bool found_another_disc = false;
    for (const Entry& other_entry : s_entries)
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
    for (Entry& other_entry : s_entries)
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
      s_entries.push_back(std::move(set_entry));
  }
}

std::string GameList::GetCoverImagePathForEntry(const Entry* entry)
{
  return GetCoverImagePath(entry->path, entry->serial, entry->GetSaveTitle());
}

static std::string GetFullCoverPath(std::string_view filename, std::string_view extension)
{
  return fmt::format("{}" FS_OSPATH_SEPARATOR_STR "{}.{}", EmuFolders::Covers, filename, extension);
}

std::string GameList::GetCoverImagePath(const std::string_view path, const std::string_view serial,
                                        const std::string_view title)
{
  static constexpr const std::array extensions = {"jpg", "jpeg", "png", "webp"};
  std::string ret;

  for (const char* extension : extensions)
  {
    // Prioritize lookup by serial (Most specific)
    if (!serial.empty())
    {
      std::string cover_path(GetFullCoverPath(serial, extension));
      if (FileSystem::FileExists(cover_path.c_str()))
      {
        ret = std::move(cover_path);
        return ret;
      }
    }

    // Try file title (for modded games or specific like above)
    const std::string_view file_title(Path::GetFileTitle(path));
    if (!file_title.empty() && title != file_title)
    {
      std::string cover_path(GetFullCoverPath(file_title, extension));
      if (FileSystem::FileExists(cover_path.c_str()))
      {
        ret = std::move(cover_path);
        return ret;
      }
    }

    // Last resort, check the game title
    if (!title.empty())
    {
      std::string cover_path(GetFullCoverPath(title, extension));
      if (FileSystem::FileExists(cover_path.c_str()))
      {
        ret = std::move(cover_path);
        return ret;
      }
    }
  }

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
  const std::string_view save_title = entry->GetSaveTitle();
  const std::string sanitized_name = Path::SanitizeFileName(save_title);

  std::string name;
  if (sanitized_name != save_title || use_serial)
    name = fmt::format("{}{}", entry->serial, extension);
  else
    name = fmt::format("{}{}", save_title, extension);

  return Path::Combine(EmuFolders::Covers, Path::SanitizeFileName(name));
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

std::string GameList::GetPlayedTimeFile()
{
  return Path::Combine(EmuFolders::DataRoot, "playtime.dat");
}

bool GameList::ParsePlayedTimeLine(char* line, std::string& serial, PlayedTimeEntry& entry)
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

std::string GameList::MakePlayedTimeLine(const std::string& serial, const PlayedTimeEntry& entry)
{
  return fmt::format("{:<{}} {:<{}} {:<{}}\n", serial, static_cast<unsigned>(PLAYED_TIME_SERIAL_LENGTH),
                     entry.total_played_time, static_cast<unsigned>(PLAYED_TIME_TOTAL_TIME_LENGTH),
                     entry.last_played_time, static_cast<unsigned>(PLAYED_TIME_LAST_TIME_LENGTH));
}

GameList::PlayedTimeMap GameList::LoadPlayedTimeMap(const std::string& path)
{
  PlayedTimeMap ret;

  // Use write mode here, even though we're not writing, so we can lock the file from other updates.
  Error error;
  auto fp = FileSystem::OpenExistingOrCreateManagedCFile(path.c_str(), 0, &error);
  if (!fp)
  {
    ERROR_LOG("Failed to open '{}' for load: {}", Path::GetFileName(path), error.GetDescription());
    return ret;
  }

#ifdef HAS_POSIX_FILE_LOCK
  FileSystem::POSIXLock flock(fp.get());
#endif

  char line[256];
  while (std::fgets(line, sizeof(line), fp.get()))
  {
    std::string serial;
    PlayedTimeEntry entry;
    if (!ParsePlayedTimeLine(line, serial, entry))
      continue;

    if (ret.find(serial) != ret.end())
    {
      WARNING_LOG("Duplicate entry: '{}'", serial);
      continue;
    }

    ret.emplace(std::move(serial), entry);
  }

  return ret;
}

GameList::PlayedTimeEntry GameList::UpdatePlayedTimeFile(const std::string& path, const std::string& serial,
                                                         std::time_t last_time, std::time_t add_time)
{
  const PlayedTimeEntry new_entry{last_time, add_time};

  Error error;
  auto fp = FileSystem::OpenExistingOrCreateManagedCFile(path.c_str(), 0, &error);
  if (!fp)
  {
    ERROR_LOG("Failed to open '{}' for update: {}", Path::GetFileName(path), error.GetDescription());
    return new_entry;
  }

#ifdef HAS_POSIX_FILE_LOCK
  FileSystem::POSIXLock flock(fp.get());
#endif

  for (;;)
  {
    char line[256];
    const s64 line_pos = FileSystem::FTell64(fp.get());
    if (!std::fgets(line, sizeof(line), fp.get()))
      break;

    std::string line_serial;
    PlayedTimeEntry line_entry;
    if (!ParsePlayedTimeLine(line, line_serial, line_entry))
      continue;

    if (line_serial != serial)
      continue;

    // found it!
    line_entry.last_played_time = (last_time != 0) ? last_time : 0;
    line_entry.total_played_time = (last_time != 0) ? (line_entry.total_played_time + add_time) : 0;

    std::string new_line(MakePlayedTimeLine(serial, line_entry));
    if (FileSystem::FSeek64(fp.get(), line_pos, SEEK_SET) != 0 ||
        std::fwrite(new_line.data(), new_line.length(), 1, fp.get()) != 1)
    {
      ERROR_LOG("Failed to update '{}'.", path);
    }

    return line_entry;
  }

  if (last_time != 0)
  {
    // new entry.
    std::string new_line(MakePlayedTimeLine(serial, new_entry));
    if (FileSystem::FSeek64(fp.get(), 0, SEEK_END) != 0 ||
        std::fwrite(new_line.data(), new_line.length(), 1, fp.get()) != 1)
    {
      ERROR_LOG("Failed to write '{}'.", path);
    }
  }

  return new_entry;
}

void GameList::AddPlayedTimeForSerial(const std::string& serial, std::time_t last_time, std::time_t add_time)
{
  if (serial.empty())
    return;

  const PlayedTimeEntry pt(UpdatePlayedTimeFile(GetPlayedTimeFile(), serial, last_time, add_time));
  VERBOSE_LOG("Add {} seconds play time to {} -> now {}", static_cast<unsigned>(add_time), serial.c_str(),
              static_cast<unsigned>(pt.total_played_time));

  std::unique_lock lock(s_mutex);
  const GameDatabase::Entry* dbentry = GameDatabase::GetEntryForSerial(serial);
  llvm::SmallVector<u32, 32> changed_indices;

  for (size_t i = 0; i < s_entries.size(); i++)
  {
    Entry& entry = s_entries[i];
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
    Host::OnGameListEntriesChanged(std::span<const u32>(changed_indices.begin(), changed_indices.end()));
}

void GameList::ClearPlayedTimeForSerial(const std::string& serial)
{
  if (serial.empty())
    return;

  UpdatePlayedTimeFile(GetPlayedTimeFile(), serial, 0, 0);

  std::unique_lock lock(s_mutex);
  for (GameList::Entry& entry : s_entries)
  {
    if (entry.serial != serial)
      continue;

    entry.last_played_time = 0;
    entry.total_played_time = 0;
  }
}

void GameList::ClearPlayedTimeForEntry(const GameList::Entry* entry)
{
  std::unique_lock lock(s_mutex);
  std::vector<std::string> serials;

  if (entry->IsDiscSet())
  {
    for (const GameList::Entry* member : GetDiscSetMembers(entry->dbentry->disc_set))
    {
      if (!member->serial.empty())
        serials.push_back(member->serial);
    }
  }
  else
  {
    if (!entry->serial.empty())
      serials.push_back(entry->serial);
  }

  auto played_time_file = GetPlayedTimeFile();
  for (const auto& serial : serials)
  {
    VERBOSE_LOG("Resetting played time for {}", serial);
    UpdatePlayedTimeFile(played_time_file, serial, 0, 0);
  }

  for (GameList::Entry& list_entry : s_entries)
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

  std::unique_lock lock(s_mutex);
  for (GameList::Entry& entry : s_entries)
  {
    if (entry.serial == serial)
      return entry.total_played_time;
  }

  return 0;
}

std::string GameList::FormatTimestamp(std::time_t timestamp)
{
  std::string ret;

  if (timestamp == 0)
  {
    ret = TRANSLATE_STR("GameList", "Never");
  }
  else
  {
    struct tm ctime = {};
    struct tm ttime = {};
    const std::time_t ctimestamp = std::time(nullptr);
#ifdef _MSC_VER
    localtime_s(&ctime, &ctimestamp);
    localtime_s(&ttime, &timestamp);
#else
    localtime_r(&ctimestamp, &ctime);
    localtime_r(&timestamp, &ttime);
#endif

    if (ctime.tm_year == ttime.tm_year && ctime.tm_yday == ttime.tm_yday)
    {
      ret = TRANSLATE_STR("GameList", "Today");
    }
    else if ((ctime.tm_year == ttime.tm_year && ctime.tm_yday == (ttime.tm_yday + 1)) ||
             (ctime.tm_yday == 0 && (ctime.tm_year - 1) == ttime.tm_year))
    {
      ret = TRANSLATE_STR("GameList", "Yesterday");
    }
    else
    {
      ret = Host::FormatNumber(Host::NumberFormatType::ShortDate, static_cast<s64>(timestamp));
    }
  }

  return ret;
}

TinyString GameList::FormatTimespan(std::time_t timespan, bool long_format)
{
  const u32 hours = static_cast<u32>(timespan / 3600);
  const u32 minutes = static_cast<u32>((timespan % 3600) / 60);
  const u32 seconds = static_cast<u32>((timespan % 3600) % 60);

  TinyString ret;
  if (!long_format)
  {
    if (hours >= 100)
      ret.format(TRANSLATE_FS("GameList", "{}h {}m"), hours, minutes);
    else if (hours > 0)
      ret.format(TRANSLATE_FS("GameList", "{}h {}m {}s"), hours, minutes, seconds);
    else if (minutes > 0)
      ret.format(TRANSLATE_FS("GameList", "{}m {}s"), minutes, seconds);
    else if (seconds > 0)
      ret.format(TRANSLATE_FS("GameList", "{}s"), seconds);
    else
      ret = TRANSLATE_SV("GameList", "None");
  }
  else
  {
    if (hours > 0)
      ret.assign(TRANSLATE_PLURAL_SSTR("GameList", "%n hours", "", hours));
    else if (minutes > 0)
      ret.assign(TRANSLATE_PLURAL_SSTR("GameList", "%n minutes", "", minutes));
    else
      ret.assign(TRANSLATE_PLURAL_SSTR("GameList", "%n seconds", "", seconds));
  }

  return ret;
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

    for (const Entry& entry : s_entries)
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
    for (const Entry& entry : s_entries)
    {
      if (entry.IsDiscSet() || entry.serial != serial)
        continue;

      ret.emplace_back(Path::GetFileName(entry.path), &entry);
    }
  }

  return ret;
}

bool GameList::DownloadCovers(const std::vector<std::string>& url_templates, bool use_serial,
                              ProgressCallback* progress, std::function<void(const Entry*, std::string)> save_callback)
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
    progress->DisplayError(TRANSLATE_SV(
      "GameList", "URL template must contain at least one of ${title}, ${savetitle}, ${filetitle}, or ${serial}."));
    return false;
  }

  std::vector<std::pair<std::string, std::string>> download_urls;
  {
    std::unique_lock lock(s_mutex);
    for (const GameList::Entry& entry : s_entries)
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
    progress->DisplayError(TRANSLATE_SV("GameList", "No URLs to download enumerated."));
    return false;
  }

  Error error;
  std::unique_ptr<HTTPDownloader> downloader(HTTPDownloader::Create(Host::GetHTTPUserAgent(), &error));
  if (!downloader)
  {
    progress->DisplayError(
      fmt::format(TRANSLATE_FS("GameList", "Failed to create HTTP downloader:\n{}"), error.GetDescription()));
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
      std::unique_lock lock(s_mutex);
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

      std::unique_lock lock(s_mutex);
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

  std::unique_lock lock(s_mutex);
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

FileSystem::ManagedCFilePtr GameList::OpenMemoryCardTimestampCache(bool for_write)
{
  const std::string filename = Path::Combine(EmuFolders::Cache, "memcard_icons.cache");
  const FileSystem::FileShareMode share_mode =
    for_write ? FileSystem::FileShareMode::DenyReadWrite : FileSystem::FileShareMode::DenyWrite;
#ifdef _WIN32
  const char* mode = for_write ? "r+b" : "rb";
#else
  // Always open read/write on Linux, since we need it for flock().
  const char* mode = "r+b";
#endif

  FileSystem::ManagedCFilePtr fp = FileSystem::OpenManagedSharedCFile(filename.c_str(), mode, share_mode, nullptr);
  if (fp)
    return fp;

  // Doesn't exist? Create it.
  if (errno == ENOENT)
  {
    if (!for_write)
      return nullptr;

    mode = "w+b";
    fp = FileSystem::OpenManagedSharedCFile(filename.c_str(), mode, share_mode, nullptr);
    if (fp)
      return fp;
  }

  // If there's a sharing violation, try again for 100ms.
  if (errno != EACCES)
    return nullptr;

  Timer timer;
  while (timer.GetTimeMilliseconds() <= 100.0f)
  {
    fp = FileSystem::OpenManagedSharedCFile(filename.c_str(), mode, share_mode, nullptr);
    if (fp)
      return fp;

    if (errno != EACCES)
      return nullptr;
  }

  ERROR_LOG("Timed out while trying to open memory card cache file.");
  return nullptr;
}

void GameList::ReloadMemcardTimestampCache()
{
  s_memcard_timestamp_cache_entries.clear();

  FileSystem::ManagedCFilePtr fp = OpenMemoryCardTimestampCache(false);
  if (!fp)
    return;

#ifdef HAS_POSIX_FILE_LOCK
  FileSystem::POSIXLock lock(fp.get());
#endif

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

  s_memcard_timestamp_cache_entries.resize(static_cast<size_t>(count));
  if (std::fread(s_memcard_timestamp_cache_entries.data(), sizeof(MemcardTimestampCacheEntry),
                 s_memcard_timestamp_cache_entries.size(), fp.get()) != s_memcard_timestamp_cache_entries.size())
  {
    s_memcard_timestamp_cache_entries = {};
    return;
  }

  // Just in case.
  for (MemcardTimestampCacheEntry& entry : s_memcard_timestamp_cache_entries)
    entry.serial[sizeof(entry.serial) - 1] = 0;
}

std::string GameList::GetGameIconPath(std::string_view serial, std::string_view path)
{
  std::string ret;

  if (serial.empty())
    return ret;

  // might exist already, or the user used a custom icon
  ret = Path::Combine(EmuFolders::GameIcons, TinyString::from_format("{}.png", serial));
  if (FileSystem::FileExists(ret.c_str()))
    return ret;

  MemoryCardType type;
  std::string memcard_path = System::GetGameMemoryCardPath(serial, path, 0, &type);
  FILESYSTEM_STAT_DATA memcard_sd;
  if (memcard_path.empty() || type == MemoryCardType::Shared ||
      !FileSystem::StatFile(memcard_path.c_str(), &memcard_sd))
  {
    ret = {};
    return ret;
  }

  const s64 timestamp = memcard_sd.ModificationTime;
  TinyString index_serial;
  index_serial.assign(
    serial.substr(0, std::min<size_t>(serial.length(), MemcardTimestampCacheEntry::MAX_SERIAL_LENGTH - 1)));

  MemcardTimestampCacheEntry* serial_entry = nullptr;
  for (MemcardTimestampCacheEntry& entry : s_memcard_timestamp_cache_entries)
  {
    if (StringUtil::EqualNoCase(index_serial, entry.serial))
    {
      // user might've deleted the file, so re-extract it if so
      // otherwise, card hasn't changed, still no icon
      if (entry.memcard_timestamp == timestamp && !entry.icon_was_extracted)
      {
        ret = {};
        return ret;
      }

      serial_entry = &entry;
      break;
    }
  }

  if (!serial_entry)
  {
    serial_entry = &s_memcard_timestamp_cache_entries.emplace_back();
    std::memset(serial_entry, 0, sizeof(MemcardTimestampCacheEntry));
  }

  serial_entry->memcard_timestamp = timestamp;
  serial_entry->icon_was_extracted = false;
  StringUtil::Strlcpy(serial_entry->serial, index_serial.view(), sizeof(serial_entry->serial));

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

        Image image(MemoryCardImage::ICON_WIDTH, MemoryCardImage::ICON_HEIGHT, ImageFormat::RGBA8);
        std::memcpy(image.GetPixels(), &fi.icon_frames.front().pixels,
                    MemoryCardImage::ICON_WIDTH * MemoryCardImage::ICON_HEIGHT * sizeof(u32));
        serial_entry->icon_was_extracted = image.SaveToFile(ret.c_str());
        if (serial_entry->icon_was_extracted)
        {
          return ret;
        }
        else
        {
          ERROR_LOG("Failed to save memory card icon to {}.", ret);
        }
      }
    }
  }
  else
  {
    ERROR_LOG("Failed to load memory card '{}': {}", Path::GetFileName(memcard_path), error.GetDescription());
  }

  ret = {};
  UpdateMemcardTimestampCache(*serial_entry);
  return ret;
}

bool GameList::UpdateMemcardTimestampCache(const MemcardTimestampCacheEntry& entry)
{
  FileSystem::ManagedCFilePtr fp = OpenMemoryCardTimestampCache(true);
  if (!fp)
    return false;

#ifdef HAS_POSIX_FILE_LOCK
  FileSystem::POSIXLock lock(fp.get());
#endif

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
