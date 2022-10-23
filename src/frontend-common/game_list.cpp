#include "game_list.h"
#include "common/assert.h"
#include "common/byte_stream.h"
#include "common/file_system.h"
#include "common/heterogeneous_containers.h"
#include "common/http_downloader.h"
#include "common/log.h"
#include "common/make_array.h"
#include "common/path.h"
#include "common/progress_callback.h"
#include "common/string_util.h"
#include "core/bios.h"
#include "core/host.h"
#include "core/host_settings.h"
#include "core/psf_loader.h"
#include "core/settings.h"
#include "core/system.h"
#include "util/cd_image.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <string_view>
#include <tinyxml2.h>
#include <unordered_map>
#include <utility>
Log_SetChannel(GameList);

#ifdef _WIN32
#include "common/windows_headers.h"
#endif

namespace GameList {
enum : u32
{
  GAME_LIST_CACHE_SIGNATURE = 0x45434C47,
  GAME_LIST_CACHE_VERSION = 32,

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

using CacheMap = UnorderedStringMap<Entry>;
using PlayedTimeMap = UnorderedStringMap<PlayedTimeEntry>;

static bool GetExeListEntry(const std::string& path, Entry* entry);
static bool GetPsfListEntry(const std::string& path, Entry* entry);
static bool GetDiscListEntry(const std::string& path, Entry* entry);

static bool GetGameListEntryFromCache(const std::string& path, Entry* entry);
static void ScanDirectory(const char* path, bool recursive, bool only_cache,
                          const std::vector<std::string>& excluded_paths, const PlayedTimeMap& played_time_map,
                          ProgressCallback* progress);
static bool AddFileFromCache(const std::string& path, std::time_t timestamp, const PlayedTimeMap& played_time_map);
static bool ScanFile(std::string path, std::time_t timestamp, std::unique_lock<std::recursive_mutex>& lock,
                     const PlayedTimeMap& played_time_map);

static std::string GetCacheFilename();
static void LoadCache();
static bool LoadEntriesFromCache(ByteStream* stream);
static bool OpenCacheForWriting();
static bool WriteEntryToCache(const Entry* entry);
static void CloseCacheFileStream();
static void DeleteCacheFile();

static std::string GetPlayedTimeFile();
static bool ParsePlayedTimeLine(char* line, std::string& serial, PlayedTimeEntry& entry);
static std::string MakePlayedTimeLine(const std::string& serial, const PlayedTimeEntry& entry);
static PlayedTimeMap LoadPlayedTimeMap(const std::string& path);
static PlayedTimeEntry UpdatePlayedTimeFile(const std::string& path, const std::string& serial, std::time_t last_time,
                                            std::time_t add_time);
} // namespace GameList

static std::vector<GameList::Entry> s_entries;
static std::recursive_mutex s_mutex;
static GameList::CacheMap s_cache_map;
static std::unique_ptr<ByteStream> s_cache_write_stream;

static bool m_game_list_loaded = false;

const char* GameList::GetEntryTypeName(EntryType type)
{
  static std::array<const char*, static_cast<int>(EntryType::Count)> names = {{"Disc", "PSExe", "Playlist", "PSF"}};
  return names[static_cast<int>(type)];
}

const char* GameList::GetEntryTypeDisplayName(EntryType type)
{
  static std::array<const char*, static_cast<int>(EntryType::Count)> names = {
    {TRANSLATABLE("GameList", "Disc"), TRANSLATABLE("GameList", "PS-EXE"), TRANSLATABLE("GameList", "Playlist"),
     TRANSLATABLE("GameList", "PSF")}};
  return names[static_cast<int>(type)];
}

bool GameList::IsGameListLoaded()
{
  return m_game_list_loaded;
}

bool GameList::IsScannableFilename(const std::string_view& path)
{
  // we don't scan bin files because they'll duplicate
  if (StringUtil::EndsWithNoCase(path, ".bin"))
    return false;

  return System::IsLoadableFilename(path);
}

bool GameList::GetExeListEntry(const std::string& path, GameList::Entry* entry)
{
  std::FILE* fp = FileSystem::OpenCFile(path.c_str(), "rb");
  if (!fp)
    return false;

  std::fseek(fp, 0, SEEK_END);
  const u32 file_size = static_cast<u32>(std::ftell(fp));
  std::fseek(fp, 0, SEEK_SET);

  BIOS::PSEXEHeader header;
  if (std::fread(&header, sizeof(header), 1, fp) != 1)
  {
    std::fclose(fp);
    return false;
  }

  std::fclose(fp);

  if (!BIOS::IsValidPSExeHeader(header, file_size))
  {
    Log_DebugPrintf("%s is not a valid PS-EXE", path.c_str());
    return false;
  }

  const std::string display_name(FileSystem::GetDisplayNameFromPath(path));
  entry->serial.clear();
  entry->title = Path::GetFileTitle(display_name);
  entry->region = BIOS::GetPSExeDiscRegion(header);
  entry->total_size = ZeroExtend64(file_size);
  entry->type = EntryType::PSExe;
  entry->compatibility = GameDatabase::CompatibilityRating::Unknown;

  return true;
}

bool GameList::GetPsfListEntry(const std::string& path, Entry* entry)
{
  // we don't need to walk the library chain here - the top file is enough
  PSFLoader::File file;
  if (!file.Load(path.c_str()))
    return false;

  entry->serial.clear();
  entry->region = file.GetRegion();
  entry->total_size = static_cast<u32>(file.GetProgramData().size());
  entry->type = EntryType::PSF;
  entry->compatibility = GameDatabase::CompatibilityRating::Unknown;

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
  entry->total_size = static_cast<u64>(CDImage::RAW_SECTOR_SIZE) * static_cast<u64>(cdi->GetLBACount());
  entry->type = EntryType::Disc;
  entry->compatibility = GameDatabase::CompatibilityRating::Unknown;

  // try the database first
  const GameDatabase::Entry* dentry = GameDatabase::GetEntryForDisc(cdi.get());
  if (dentry)
  {
    // pull from database
    entry->serial = dentry->serial;
    entry->title = dentry->title;
    entry->genre = dentry->genre;
    entry->publisher = dentry->publisher;
    entry->developer = dentry->developer;
    entry->release_date = dentry->release_date;
    entry->min_players = dentry->min_players;
    entry->max_players = dentry->max_players;
    entry->min_blocks = dentry->min_blocks;
    entry->max_blocks = dentry->max_blocks;
    entry->supported_controllers = dentry->supported_controllers;
    entry->compatibility = dentry->compatibility;
  }
  else
  {
    const std::string display_name(FileSystem::GetDisplayNameFromPath(path));

    // no game code, so use the filename title
    entry->serial = System::GetGameIdFromImage(cdi.get(), true);
    entry->title = Path::GetFileTitle(display_name);
    entry->compatibility = GameDatabase::CompatibilityRating::Unknown;
    entry->release_date = 0;
    entry->min_players = 0;
    entry->max_players = 0;
    entry->min_blocks = 0;
    entry->max_blocks = 0;
    entry->supported_controllers = ~0u;
  }

  // region detection
  entry->region = System::GetRegionFromSystemArea(cdi.get());
  if (entry->region == DiscRegion::Other)
    entry->region = System::GetRegionForSerial(entry->serial);

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
        Log_ErrorPrintf("Failed to switch to subimage %u in '%s'", i, entry->path.c_str());
        continue;
      }

      entry->total_size += static_cast<u64>(CDImage::RAW_SECTOR_SIZE) * static_cast<u64>(cdi->GetLBACount());
    }
  }

  return true;
}

bool GameList::PopulateEntryFromPath(const std::string& path, Entry* entry)
{
  if (System::IsExeFileName(path))
    return GetExeListEntry(path, entry);
  if (System::IsPsfFileName(path.c_str()))
    return GetPsfListEntry(path, entry);
  return GetDiscListEntry(path, entry);
}

bool GameList::GetGameListEntryFromCache(const std::string& path, Entry* entry)
{
  auto iter = UnorderedStringMapFind(s_cache_map, path);
  if (iter == s_cache_map.end())
    return false;

  *entry = std::move(iter->second);
  s_cache_map.erase(iter);
  return true;
}

bool GameList::LoadEntriesFromCache(ByteStream* stream)
{
  u32 file_signature, file_version;
  if (!stream->ReadU32(&file_signature) || !stream->ReadU32(&file_version) ||
      file_signature != GAME_LIST_CACHE_SIGNATURE || file_version != GAME_LIST_CACHE_VERSION)
  {
    Log_WarningPrintf("Game list cache is corrupted");
    return false;
  }

  while (stream->GetPosition() != stream->GetSize())
  {
    std::string path;
    Entry ge;

    u8 type;
    u8 region;
    u8 compatibility_rating;

    if (!stream->ReadU8(&type) || !stream->ReadU8(&region) || !stream->ReadSizePrefixedString(&path) ||
        !stream->ReadSizePrefixedString(&ge.serial) || !stream->ReadSizePrefixedString(&ge.title) ||
        !stream->ReadSizePrefixedString(&ge.genre) || !stream->ReadSizePrefixedString(&ge.publisher) ||
        !stream->ReadSizePrefixedString(&ge.developer) || !stream->ReadU64(&ge.total_size) ||
        !stream->ReadU64(reinterpret_cast<u64*>(&ge.last_modified_time)) || !stream->ReadU64(&ge.release_date) ||
        !stream->ReadU32(&ge.supported_controllers) || !stream->ReadU8(&ge.min_players) ||
        !stream->ReadU8(&ge.max_players) || !stream->ReadU8(&ge.min_blocks) || !stream->ReadU8(&ge.max_blocks) ||
        !stream->ReadU8(&compatibility_rating) || region >= static_cast<u8>(DiscRegion::Count) ||
        type >= static_cast<u8>(EntryType::Count) ||
        compatibility_rating >= static_cast<u8>(GameDatabase::CompatibilityRating::Count))
    {
      Log_WarningPrintf("Game list cache entry is corrupted");
      return false;
    }

    ge.path = path;
    ge.region = static_cast<DiscRegion>(region);
    ge.type = static_cast<EntryType>(type);
    ge.compatibility = static_cast<GameDatabase::CompatibilityRating>(compatibility_rating);

    auto iter = UnorderedStringMapFind(s_cache_map, ge.path);
    if (iter != s_cache_map.end())
      iter->second = std::move(ge);
    else
      s_cache_map.emplace(std::move(path), std::move(ge));
  }

  return true;
}

bool GameList::WriteEntryToCache(const Entry* entry)
{
  bool result = true;
  result &= s_cache_write_stream->WriteU8(static_cast<u8>(entry->type));
  result &= s_cache_write_stream->WriteU8(static_cast<u8>(entry->region));
  result &= s_cache_write_stream->WriteSizePrefixedString(entry->path);
  result &= s_cache_write_stream->WriteSizePrefixedString(entry->serial);
  result &= s_cache_write_stream->WriteSizePrefixedString(entry->title);
  result &= s_cache_write_stream->WriteSizePrefixedString(entry->genre);
  result &= s_cache_write_stream->WriteSizePrefixedString(entry->publisher);
  result &= s_cache_write_stream->WriteSizePrefixedString(entry->developer);
  result &= s_cache_write_stream->WriteU64(entry->total_size);
  result &= s_cache_write_stream->WriteU64(entry->last_modified_time);
  result &= s_cache_write_stream->WriteU64(entry->release_date);
  result &= s_cache_write_stream->WriteU32(entry->supported_controllers);
  result &= s_cache_write_stream->WriteU8(entry->min_players);
  result &= s_cache_write_stream->WriteU8(entry->max_players);
  result &= s_cache_write_stream->WriteU8(entry->min_blocks);
  result &= s_cache_write_stream->WriteU8(entry->max_blocks);
  result &= s_cache_write_stream->WriteU8(static_cast<u8>(entry->compatibility));
  return result;
}

static std::string GameList::GetCacheFilename()
{
  return Path::Combine(EmuFolders::Cache, "gamelist.cache");
}

void GameList::LoadCache()
{
  std::string filename(GetCacheFilename());
  std::unique_ptr<ByteStream> stream =
    ByteStream::OpenFile(filename.c_str(), BYTESTREAM_OPEN_READ | BYTESTREAM_OPEN_STREAMED);
  if (!stream)
    return;

  if (!LoadEntriesFromCache(stream.get()))
  {
    Log_WarningPrintf("Deleting corrupted cache file '%s'", filename.c_str());
    stream.reset();
    s_cache_map.clear();
    DeleteCacheFile();
    return;
  }
}

bool GameList::OpenCacheForWriting()
{
  const std::string cache_filename(GetCacheFilename());
  Assert(!s_cache_write_stream);

  s_cache_write_stream = ByteStream::OpenFile(cache_filename.c_str(),
                                              BYTESTREAM_OPEN_READ | BYTESTREAM_OPEN_WRITE | BYTESTREAM_OPEN_SEEKABLE);
  if (s_cache_write_stream)
  {
    // check the header
    u32 signature, version;
    if (s_cache_write_stream->ReadU32(&signature) && signature == GAME_LIST_CACHE_SIGNATURE &&
        s_cache_write_stream->ReadU32(&version) && version == GAME_LIST_CACHE_VERSION &&
        s_cache_write_stream->SeekToEnd())
    {
      return true;
    }

    s_cache_write_stream.reset();
  }

  Log_InfoPrintf("Creating new game list cache file: '%s'", cache_filename.c_str());

  s_cache_write_stream = ByteStream::OpenFile(
    cache_filename.c_str(), BYTESTREAM_OPEN_CREATE | BYTESTREAM_OPEN_TRUNCATE | BYTESTREAM_OPEN_WRITE);
  if (!s_cache_write_stream)
    return false;

  // new cache file, write header
  if (!s_cache_write_stream->WriteU32(GAME_LIST_CACHE_SIGNATURE) ||
      !s_cache_write_stream->WriteU32(GAME_LIST_CACHE_VERSION))
  {
    Log_ErrorPrintf("Failed to write game list cache header");
    s_cache_write_stream.reset();
    FileSystem::DeleteFile(cache_filename.c_str());
    return false;
  }

  return true;
}

void GameList::CloseCacheFileStream()
{
  if (!s_cache_write_stream)
    return;

  s_cache_write_stream->Commit();
  s_cache_write_stream.reset();
}

void GameList::DeleteCacheFile()
{
  Assert(!s_cache_write_stream);

  const std::string filename(GetCacheFilename());
  if (!FileSystem::FileExists(filename.c_str()))
    return;

  if (FileSystem::DeleteFile(filename.c_str()))
    Log_InfoPrintf("Deleted game list cache '%s'", filename.c_str());
  else
    Log_WarningPrintf("Failed to delete game list cache '%s'", filename.c_str());
}

static bool IsPathExcluded(const std::vector<std::string>& excluded_paths, const std::string& path)
{
  return (std::find(excluded_paths.begin(), excluded_paths.end(), path) != excluded_paths.end());
}

void GameList::ScanDirectory(const char* path, bool recursive, bool only_cache,
                             const std::vector<std::string>& excluded_paths, const PlayedTimeMap& played_time_map,
                             ProgressCallback* progress)
{
  Log_InfoPrintf("Scanning %s%s", path, recursive ? " (recursively)" : "");

  progress->SetFormattedStatusText("Scanning directory '%s'%s...", path, recursive ? " (recursively)" : "");

  FileSystem::FindResultsArray files;
  FileSystem::FindFiles(path, "*",
                        recursive ? (FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES | FILESYSTEM_FIND_RECURSIVE) :
                                    (FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES),
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

    if (progress->IsCancelled() || !GameList::IsScannableFilename(ffd.FileName) ||
        IsPathExcluded(excluded_paths, ffd.FileName))
    {
      continue;
    }

    std::unique_lock lock(s_mutex);
    if (GetEntryForPath(ffd.FileName.c_str()) ||
        AddFileFromCache(ffd.FileName, ffd.ModificationTime, played_time_map) || only_cache)
    {
      continue;
    }

    progress->SetFormattedStatusText("Scanning '%s'...", FileSystem::GetDisplayNameFromPath(ffd.FileName).c_str());
    ScanFile(std::move(ffd.FileName), ffd.ModificationTime, lock, played_time_map);
    progress->SetProgressValue(files_scanned);
  }

  progress->SetProgressValue(files_scanned);
  progress->PopState();
}

bool GameList::AddFileFromCache(const std::string& path, std::time_t timestamp, const PlayedTimeMap& played_time_map)
{
  Entry entry;
  if (!GetGameListEntryFromCache(path, &entry) || entry.last_modified_time != timestamp)
    return false;

  auto iter = UnorderedStringMapFind(played_time_map, entry.serial);
  if (iter != played_time_map.end())
  {
    entry.last_played_time = iter->second.last_played_time;
    entry.total_played_time = iter->second.total_played_time;
  }

  s_entries.push_back(std::move(entry));
  return true;
}

bool GameList::ScanFile(std::string path, std::time_t timestamp, std::unique_lock<std::recursive_mutex>& lock,
                        const PlayedTimeMap& played_time_map)
{
  // don't block UI while scanning
  lock.unlock();

  Log_DevPrintf("Scanning '%s'...", path.c_str());

  Entry entry;
  if (!PopulateEntryFromPath(path, &entry))
    return false;

  entry.path = std::move(path);
  entry.last_modified_time = timestamp;

  if (s_cache_write_stream || OpenCacheForWriting())
  {
    if (!WriteEntryToCache(&entry))
      Log_WarningPrintf("Failed to write entry '%s' to cache", entry.path.c_str());
  }

  auto iter = UnorderedStringMapFind(played_time_map, entry.serial);
  if (iter != played_time_map.end())
  {
    entry.last_played_time = iter->second.last_played_time;
    entry.total_played_time = iter->second.total_played_time;
  }

  lock.lock();
  s_entries.push_back(std::move(entry));
  return true;
}

std::unique_lock<std::recursive_mutex> GameList::GetLock()
{
  return std::unique_lock<std::recursive_mutex>(s_mutex);
}

const GameList::Entry* GameList::GetEntryByIndex(u32 index)
{
  return (index < s_entries.size()) ? &s_entries[index] : nullptr;
}

const GameList::Entry* GameList::GetEntryForPath(const char* path)
{
  const size_t path_length = std::strlen(path);
  for (const Entry& entry : s_entries)
  {
    if (entry.path.size() == path_length && StringUtil::Strcasecmp(entry.path.c_str(), path) == 0)
      return &entry;
  }

  return nullptr;
}

const GameList::Entry* GameList::GetEntryBySerial(const std::string_view& serial)
{
  for (const Entry& entry : s_entries)
  {
    if (entry.serial.length() == serial.length() &&
        StringUtil::Strncasecmp(entry.serial.c_str(), serial.data(), serial.length()) == 0)
    {
      return &entry;
    }
  }

  return nullptr;
}

u32 GameList::GetEntryCount()
{
  return static_cast<u32>(s_entries.size());
}

void GameList::Refresh(bool invalidate_cache, bool only_cache, ProgressCallback* progress /* = nullptr */)
{
  m_game_list_loaded = true;

  if (!progress)
    progress = ProgressCallback::NullProgressCallback;

  if (invalidate_cache)
    DeleteCacheFile();
  else
    LoadCache();

  // don't delete the old entries, since the frontend might still access them
  std::vector<Entry> old_entries;
  {
    std::unique_lock lock(s_mutex);
    old_entries.swap(s_entries);
  }

  const std::vector<std::string> excluded_paths(Host::GetBaseStringListSetting("GameList", "ExcludedPaths"));
  const std::vector<std::string> dirs(Host::GetBaseStringListSetting("GameList", "Paths"));
  const std::vector<std::string> recursive_dirs(Host::GetBaseStringListSetting("GameList", "RecursivePaths"));
  const PlayedTimeMap played_time(LoadPlayedTimeMap(GetPlayedTimeFile()));

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

      ScanDirectory(dir.c_str(), false, only_cache, excluded_paths, played_time, progress);
      progress->SetProgressValue(++directory_counter);
    }
    for (const std::string& dir : recursive_dirs)
    {
      if (progress->IsCancelled())
        break;

      ScanDirectory(dir.c_str(), true, only_cache, excluded_paths, played_time, progress);
      progress->SetProgressValue(++directory_counter);
    }
  }

  // don't need unused cache entries
  CloseCacheFileStream();
  s_cache_map.clear();
}

std::string GameList::GetCoverImagePathForEntry(const Entry* entry)
{
  return GetCoverImagePath(entry->path, entry->serial, entry->title);
}

static std::string GetFullCoverPath(const std::string_view& filename, const std::string_view& extension)
{
  return fmt::format("{}" FS_OSPATH_SEPARATOR_STR "{}.{}", EmuFolders::Covers, filename, extension);
}

std::string GameList::GetCoverImagePath(const std::string& path, const std::string& serial, const std::string& title)
{
  static constexpr auto extensions = make_array("jpg", "jpeg", "png", "webp");

  for (const char* extension : extensions)
  {
    // Prioritize lookup by serial (Most specific)
    if (!serial.empty())
    {
      const std::string cover_path(GetFullCoverPath(serial, extension));
      if (FileSystem::FileExists(cover_path.c_str()))
        return cover_path;
    }

    // Try file title (for modded games or specific like above)
    const std::string_view file_title(Path::GetFileTitle(path));
    if (!file_title.empty() && title != file_title)
    {
      const std::string cover_path(GetFullCoverPath(file_title, extension));
      if (FileSystem::FileExists(cover_path.c_str()))
        return cover_path;
    }

    // Last resort, check the game title
    if (!title.empty())
    {
      const std::string cover_path(GetFullCoverPath(title, extension));
      if (FileSystem::FileExists(cover_path.c_str()))
        return cover_path;
    }
  }

  return {};
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
  const std::string sanitized_name(Path::SanitizeFileName(entry->title));

  std::string name;
  if (sanitized_name != entry->title || use_serial)
    name = fmt::format("{}{}", entry->serial, extension);
  else
    name = fmt::format("{}{}", entry->title, extension);

  return Path::Combine(EmuFolders::Covers, Path::SanitizeFileName(name));
}

size_t GameList::Entry::GetReleaseDateString(char* buffer, size_t buffer_size) const
{
  if (release_date == 0)
    return StringUtil::Strlcpy(buffer, "Unknown", buffer_size);

  std::time_t date_as_time = static_cast<std::time_t>(release_date);
#ifdef _WIN32
  tm date_tm = {};
  gmtime_s(&date_tm, &date_as_time);
#else
  tm date_tm = {};
  gmtime_r(&date_as_time, &date_tm);
#endif

  return std::strftime(buffer, buffer_size, "%d %B %Y", &date_tm);
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
    Log_WarningPrintf("Malformed line: '%s'", line);
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
    Log_WarningPrintf("Malformed line: '%s'", line);
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
  auto fp = FileSystem::OpenManagedCFile(path.c_str(), "r+b");

#ifdef _WIN32
  // On Windows, the file is implicitly locked.
  while (!fp && GetLastError() == ERROR_SHARING_VIOLATION)
  {
    Sleep(10);
    fp = FileSystem::OpenManagedCFile(path.c_str(), "r+b");
  }
#endif

  if (fp)
  {
#ifndef _WIN32
    FileSystem::POSIXLock flock(fp.get());
#endif

    char line[256];
    while (std::fgets(line, sizeof(line), fp.get()))
    {
      std::string serial;
      PlayedTimeEntry entry;
      if (!ParsePlayedTimeLine(line, serial, entry))
        continue;

      if (UnorderedStringMapFind(ret, serial) != ret.end())
      {
        Log_WarningPrintf("Duplicate entry: '%s'", serial.c_str());
        continue;
      }

      ret.emplace(std::move(serial), entry);
    }
  }

  return ret;
}

GameList::PlayedTimeEntry GameList::UpdatePlayedTimeFile(const std::string& path, const std::string& serial,
                                                         std::time_t last_time, std::time_t add_time)
{
  const PlayedTimeEntry new_entry{last_time, add_time};

  auto fp = FileSystem::OpenManagedCFile(path.c_str(), "r+b");

#ifdef _WIN32
  // On Windows, the file is implicitly locked.
  while (!fp && GetLastError() == ERROR_SHARING_VIOLATION)
  {
    Sleep(10);
    fp = FileSystem::OpenManagedCFile(path.c_str(), "r+b");
  }
#endif

  // Doesn't exist? Create it.
  if (!fp && errno == ENOENT)
    fp = FileSystem::OpenManagedCFile(path.c_str(), "w+b");

  if (!fp)
  {
    Log_ErrorPrintf("Failed to open '%s' for update.", path.c_str());
    return new_entry;
  }

#ifndef _WIN32
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
    line_entry.last_played_time = last_time;
    line_entry.total_played_time += add_time;

    std::string new_line(MakePlayedTimeLine(serial, line_entry));
    if (FileSystem::FSeek64(fp.get(), line_pos, SEEK_SET) != 0 ||
        std::fwrite(new_line.data(), new_line.length(), 1, fp.get()) != 1)
    {
      Log_ErrorPrintf("Failed to update '%s'.", path.c_str());
    }

    return line_entry;
  }

  // new entry.
  std::string new_line(MakePlayedTimeLine(serial, new_entry));
  if (FileSystem::FSeek64(fp.get(), 0, SEEK_END) != 0 ||
      std::fwrite(new_line.data(), new_line.length(), 1, fp.get()) != 1)
  {
    Log_ErrorPrintf("Failed to write '%s'.", path.c_str());
  }

  return new_entry;
}

void GameList::AddPlayedTimeForSerial(const std::string& serial, std::time_t last_time, std::time_t add_time)
{
  if (serial.empty())
    return;

  const PlayedTimeEntry pt(UpdatePlayedTimeFile(GetPlayedTimeFile(), serial, last_time, add_time));
  Log_VerbosePrintf("Add %u seconds play time to %s -> now %u", static_cast<unsigned>(add_time), serial.c_str(),
                    static_cast<unsigned>(pt.total_played_time));

  std::unique_lock<std::recursive_mutex> lock(s_mutex);
  for (GameList::Entry& entry : s_entries)
  {
    if (entry.serial != serial)
      continue;

    entry.last_played_time = pt.last_played_time;
    entry.total_played_time = pt.total_played_time;
  }
}

std::time_t GameList::GetCachedPlayedTimeForSerial(const std::string& serial)
{
  if (serial.empty())
    return 0;

  std::unique_lock<std::recursive_mutex> lock(s_mutex);
  for (GameList::Entry& entry : s_entries)
  {
    if (entry.serial == serial)
      return entry.total_played_time;
  }

  return 0;
}

TinyString GameList::FormatTimestamp(std::time_t timestamp)
{
  TinyString ret;

  if (timestamp == 0)
  {
    ret = Host::TranslateString("GameList", "Never");
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
      ret = Host::TranslateString("GameList", "Today");
    }
    else if ((ctime.tm_year == ttime.tm_year && ctime.tm_yday == (ttime.tm_yday + 1)) ||
             (ctime.tm_yday == 0 && (ctime.tm_year - 1) == ttime.tm_year))
    {
      ret = Host::TranslateString("GameList", "Yesterday");
    }
    else
    {
      char buf[128];
      std::strftime(buf, std::size(buf), "%x", &ttime);
      ret.Assign(buf);
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
      ret.Fmt(Host::TranslateString("GameList", "{}h {}m").GetCharArray(), hours, minutes);
    else if (hours > 0)
      ret.Fmt(Host::TranslateString("GameList", "{}h {}m {}s").GetCharArray(), hours, minutes, seconds);
    else if (minutes > 0)
      ret.Fmt(Host::TranslateString("GameList", "{}m {}s").GetCharArray(), minutes, seconds);
    else if (seconds > 0)
      ret.Fmt(Host::TranslateString("GameList", "{}s").GetCharArray(), seconds);
    else
      ret = Host::TranslateString("GameList", "None");
  }
  else
  {
    if (hours > 0)
      ret = fmt::format(Host::TranslateString("GameList", "{} hours").GetCharArray(), hours);
    else
      ret = fmt::format(Host::TranslateString("GameList", "{} minutes").GetCharArray(), minutes);
  }

  return ret;
}

bool GameList::DownloadCovers(const std::vector<std::string>& url_templates, bool use_serial,
                              ProgressCallback* progress, std::function<void(const Entry*, std::string)> save_callback)
{
  if (!progress)
    progress = ProgressCallback::NullProgressCallback;

  bool has_title = false;
  bool has_file_title = false;
  bool has_serial = false;
  for (const std::string& url_template : url_templates)
  {
    if (!has_title && url_template.find("${title}") != std::string::npos)
      has_title = true;
    if (!has_file_title && url_template.find("${filetitle}") != std::string::npos)
      has_file_title = true;
    if (!has_serial && url_template.find("${serial}") != std::string::npos)
      has_serial = true;
  }
  if (!has_title && !has_file_title && !has_serial)
  {
    progress->DisplayError("URL template must contain at least one of ${title}, ${filetitle}, or ${serial}.");
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
          StringUtil::ReplaceAll(&url, "${title}", Common::HTTPDownloader::URLEncode(entry.title));
        if (has_file_title)
        {
          std::string display_name(FileSystem::GetDisplayNameFromPath(entry.path));
          StringUtil::ReplaceAll(&url, "${filetitle}",
                                 Common::HTTPDownloader::URLEncode(Path::GetFileTitle(display_name)));
        }
        if (has_serial)
          StringUtil::ReplaceAll(&url, "${serial}", Common::HTTPDownloader::URLEncode(entry.serial));

        download_urls.emplace_back(entry.path, std::move(url));
      }
    }
  }
  if (download_urls.empty())
  {
    progress->DisplayError("No URLs to download enumerated.");
    return false;
  }

  std::unique_ptr<Common::HTTPDownloader> downloader(Common::HTTPDownloader::Create());
  if (!downloader)
  {
    progress->DisplayError("Failed to create HTTP downloader.");
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
      const GameList::Entry* entry = GetEntryForPath(entry_path.c_str());
      if (!entry || !GetCoverImagePathForEntry(entry).empty())
      {
        progress->IncrementProgressValue();
        continue;
      }

      progress->SetFormattedStatusText("Downloading cover for %s...", entry->title.c_str());
    }

    // we could actually do a few in parallel here...
    std::string filename(Common::HTTPDownloader::URLDecode(url));
    downloader->CreateRequest(
      std::move(url), [use_serial, &save_callback, entry_path = std::move(entry_path), filename = std::move(filename)](
                        s32 status_code, std::string content_type, Common::HTTPDownloader::Request::Data data) {
        if (status_code != Common::HTTPDownloader::HTTP_OK || data.empty())
          return;

        std::unique_lock lock(s_mutex);
        const GameList::Entry* entry = GetEntryForPath(entry_path.c_str());
        if (!entry || !GetCoverImagePathForEntry(entry).empty())
          return;

        // prefer the content type from the response for the extension
        // otherwise, if it's missing, and the request didn't have an extension.. fall back to jpegs.
        std::string template_filename;
        std::string content_type_extension(Common::HTTPDownloader::GetExtensionForContentType(content_type));

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
