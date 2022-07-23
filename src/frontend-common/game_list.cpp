#include "game_list.h"
#include "common/assert.h"
#include "common/byte_stream.h"
#include "common/file_system.h"
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
#include <utility>
Log_SetChannel(GameList);

enum : u32
{
  GAME_LIST_CACHE_SIGNATURE = 0x45434C47,
  GAME_LIST_CACHE_VERSION = 32
};

namespace GameList {
using CacheMap = std::unordered_map<std::string, Entry>;

static bool GetExeListEntry(const std::string& path, Entry* entry);
static bool GetPsfListEntry(const std::string& path, Entry* entry);
static bool GetDiscListEntry(const std::string& path, Entry* entry);

static bool GetGameListEntryFromCache(const std::string& path, Entry* entry);
static void ScanDirectory(const char* path, bool recursive, bool only_cache,
                          const std::vector<std::string>& excluded_paths, ProgressCallback* progress);
static bool AddFileFromCache(const std::string& path, std::time_t timestamp);
static bool ScanFile(std::string path, std::time_t timestamp);

static std::string GetCacheFilename();
static void LoadCache();
static bool LoadEntriesFromCache(ByteStream* stream);
static bool OpenCacheForWriting();
static bool WriteEntryToCache(const Entry* entry);
static void CloseCacheFileStream();
static void DeleteCacheFile();
} // namespace GameList

static std::vector<GameList::Entry> m_entries;
static std::recursive_mutex s_mutex;
static GameList::CacheMap m_cache_map;
static std::unique_ptr<ByteStream> m_cache_write_stream;

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
  entry->title = Path::StripExtension(display_name);
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
    entry->title += Path::StripExtension(display_name);
  }

  return true;
}

bool GameList::GetDiscListEntry(const std::string& path, Entry* entry)
{
  std::unique_ptr<CDImage> cdi = CDImage::Open(path.c_str(), nullptr);
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
    const std::string display_name(Path::GetFileTitle(FileSystem::GetDisplayNameFromPath(path)));

    // no game code, so use the filename title
    entry->serial = System::GetGameCodeForImage(cdi.get(), true);
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
    entry->region = System::GetRegionForCode(entry->serial);

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
  auto iter = m_cache_map.find(path);
  if (iter == m_cache_map.end())
    return false;

  *entry = std::move(iter->second);
  m_cache_map.erase(iter);
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

    auto iter = m_cache_map.find(ge.path);
    if (iter != m_cache_map.end())
      iter->second = std::move(ge);
    else
      m_cache_map.emplace(std::move(path), std::move(ge));
  }

  return true;
}

bool GameList::WriteEntryToCache(const Entry* entry)
{
  bool result = true;
  result &= m_cache_write_stream->WriteU8(static_cast<u8>(entry->type));
  result &= m_cache_write_stream->WriteU8(static_cast<u8>(entry->region));
  result &= m_cache_write_stream->WriteSizePrefixedString(entry->path);
  result &= m_cache_write_stream->WriteSizePrefixedString(entry->serial);
  result &= m_cache_write_stream->WriteSizePrefixedString(entry->title);
  result &= m_cache_write_stream->WriteSizePrefixedString(entry->genre);
  result &= m_cache_write_stream->WriteSizePrefixedString(entry->publisher);
  result &= m_cache_write_stream->WriteSizePrefixedString(entry->developer);
  result &= m_cache_write_stream->WriteU64(entry->total_size);
  result &= m_cache_write_stream->WriteU64(entry->last_modified_time);
  result &= m_cache_write_stream->WriteU64(entry->release_date);
  result &= m_cache_write_stream->WriteU32(entry->supported_controllers);
  result &= m_cache_write_stream->WriteU8(entry->min_players);
  result &= m_cache_write_stream->WriteU8(entry->max_players);
  result &= m_cache_write_stream->WriteU8(entry->min_blocks);
  result &= m_cache_write_stream->WriteU8(entry->max_blocks);
  result &= m_cache_write_stream->WriteU8(static_cast<u8>(entry->compatibility));
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
    m_cache_map.clear();
    DeleteCacheFile();
    return;
  }
}

bool GameList::OpenCacheForWriting()
{
  const std::string cache_filename(GetCacheFilename());
  Assert(!m_cache_write_stream);

  m_cache_write_stream = ByteStream::OpenFile(cache_filename.c_str(),
                                              BYTESTREAM_OPEN_READ | BYTESTREAM_OPEN_WRITE | BYTESTREAM_OPEN_SEEKABLE);
  if (m_cache_write_stream)
  {
    // check the header
    u32 signature, version;
    if (m_cache_write_stream->ReadU32(&signature) && signature == GAME_LIST_CACHE_SIGNATURE &&
        m_cache_write_stream->ReadU32(&version) && version == GAME_LIST_CACHE_VERSION &&
        m_cache_write_stream->SeekToEnd())
    {
      return true;
    }

    m_cache_write_stream.reset();
  }

  Log_InfoPrintf("Creating new game list cache file: '%s'", cache_filename.c_str());

  m_cache_write_stream = ByteStream::OpenFile(
    cache_filename.c_str(), BYTESTREAM_OPEN_CREATE | BYTESTREAM_OPEN_TRUNCATE | BYTESTREAM_OPEN_WRITE);
  if (!m_cache_write_stream)
    return false;

  // new cache file, write header
  if (!m_cache_write_stream->WriteU32(GAME_LIST_CACHE_SIGNATURE) ||
      !m_cache_write_stream->WriteU32(GAME_LIST_CACHE_VERSION))
  {
    Log_ErrorPrintf("Failed to write game list cache header");
    m_cache_write_stream.reset();
    FileSystem::DeleteFile(cache_filename.c_str());
    return false;
  }

  return true;
}

void GameList::CloseCacheFileStream()
{
  if (!m_cache_write_stream)
    return;

  m_cache_write_stream->Commit();
  m_cache_write_stream.reset();
}

void GameList::DeleteCacheFile()
{
  Assert(!m_cache_write_stream);

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
                             const std::vector<std::string>& excluded_paths, ProgressCallback* progress)
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

    {
      std::unique_lock lock(s_mutex);
      if (GetEntryForPath(ffd.FileName.c_str()) || AddFileFromCache(ffd.FileName, ffd.ModificationTime) || only_cache)
      {
        continue;
      }
    }

    // ownership of fp is transferred
    progress->SetFormattedStatusText("Scanning '%s'...", FileSystem::GetDisplayNameFromPath(ffd.FileName).c_str());
    ScanFile(std::move(ffd.FileName), ffd.ModificationTime);
    progress->SetProgressValue(files_scanned);
  }

  progress->SetProgressValue(files_scanned);
  progress->PopState();
}

bool GameList::AddFileFromCache(const std::string& path, std::time_t timestamp)
{
  if (std::any_of(m_entries.begin(), m_entries.end(), [&path](const Entry& other) { return other.path == path; }))
  {
    // already exists
    return true;
  }

  Entry entry;
  if (!GetGameListEntryFromCache(path, &entry) || entry.last_modified_time != timestamp)
    return false;

  m_entries.push_back(std::move(entry));
  return true;
}

bool GameList::ScanFile(std::string path, std::time_t timestamp)
{
  Log_DevPrintf("Scanning '%s'...", path.c_str());

  Entry entry;
  if (!PopulateEntryFromPath(path, &entry))
    return false;

  entry.path = std::move(path);
  entry.last_modified_time = timestamp;

  if (m_cache_write_stream || OpenCacheForWriting())
  {
    if (!WriteEntryToCache(&entry))
      Log_WarningPrintf("Failed to write entry '%s' to cache", entry.path.c_str());
  }

  std::unique_lock lock(s_mutex);
  m_entries.push_back(std::move(entry));
  return true;
}

std::unique_lock<std::recursive_mutex> GameList::GetLock()
{
  return std::unique_lock<std::recursive_mutex>(s_mutex);
}

const GameList::Entry* GameList::GetEntryByIndex(u32 index)
{
  return (index < m_entries.size()) ? &m_entries[index] : nullptr;
}

const GameList::Entry* GameList::GetEntryForPath(const char* path)
{
  const size_t path_length = std::strlen(path);
  for (const Entry& entry : m_entries)
  {
    if (entry.path.size() == path_length && StringUtil::Strcasecmp(entry.path.c_str(), path) == 0)
      return &entry;
  }

  return nullptr;
}

const GameList::Entry* GameList::GetEntryBySerial(const std::string_view& serial)
{
  for (const Entry& entry : m_entries)
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
  return static_cast<u32>(m_entries.size());
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
    old_entries.swap(m_entries);
  }

  const std::vector<std::string> excluded_paths(Host::GetStringListSetting("GameList", "ExcludedPaths"));
  const std::vector<std::string> dirs(Host::GetStringListSetting("GameList", "Paths"));
  const std::vector<std::string> recursive_dirs(Host::GetStringListSetting("GameList", "RecursivePaths"));

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

      ScanDirectory(dir.c_str(), false, only_cache, excluded_paths, progress);
      progress->SetProgressValue(++directory_counter);
    }
    for (const std::string& dir : recursive_dirs)
    {
      if (progress->IsCancelled())
        break;

      ScanDirectory(dir.c_str(), true, only_cache, excluded_paths, progress);
      progress->SetProgressValue(++directory_counter);
    }
  }

  // don't need unused cache entries
  CloseCacheFileStream();
  m_cache_map.clear();
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

std::string GameList::GetNewCoverImagePathForEntry(const Entry* entry, const char* new_filename)
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
  std::string sanitized_name(entry->title);
  Path::SanitizeFileName(sanitized_name);

  std::string name;
  if (sanitized_name != entry->title)
    name = fmt::format("{}{}", entry->serial, extension);
  else
    name = fmt::format("{}{}", entry->title, extension);

  return Path::Combine(EmuFolders::Cache, name);
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
