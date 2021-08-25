#include "game_list.h"
#include "common/assert.h"
#include "common/byte_stream.h"
#include "common/cd_image.h"
#include "common/file_system.h"
#include "common/iso_reader.h"
#include "common/log.h"
#include "common/make_array.h"
#include "common/progress_callback.h"
#include "common/string_util.h"
#include "core/bios.h"
#include "core/host_interface.h"
#include "core/psf_loader.h"
#include "core/settings.h"
#include "core/system.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <string_view>
#include <tinyxml2.h>
#include <utility>
Log_SetChannel(GameList);

GameList::GameList() = default;

GameList::~GameList() = default;

const char* GameList::EntryTypeToString(GameListEntryType type)
{
  static std::array<const char*, static_cast<int>(GameListEntryType::Count)> names = {
    {"Disc", "PSExe", "Playlist", "PSF"}};
  return names[static_cast<int>(type)];
}

const char* GameList::EntryCompatibilityRatingToString(GameListCompatibilityRating rating)
{
  static std::array<const char*, static_cast<int>(GameListCompatibilityRating::Count)> names = {
    {"Unknown", "DoesntBoot", "CrashesInIntro", "CrashesInGame", "GraphicalAudioIssues", "NoIssues"}};
  return names[static_cast<int>(rating)];
}

const char* GameList::GetGameListCompatibilityRatingString(GameListCompatibilityRating rating)
{
  static constexpr std::array<const char*, static_cast<size_t>(GameListCompatibilityRating::Count)> names = {
    {TRANSLATABLE("GameListCompatibilityRating", "Unknown"),
     TRANSLATABLE("GameListCompatibilityRating", "Doesn't Boot"),
     TRANSLATABLE("GameListCompatibilityRating", "Crashes In Intro"),
     TRANSLATABLE("GameListCompatibilityRating", "Crashes In-Game"),
     TRANSLATABLE("GameListCompatibilityRating", "Graphical/Audio Issues"),
     TRANSLATABLE("GameListCompatibilityRating", "No Issues")}};
  return (rating >= GameListCompatibilityRating::Unknown && rating < GameListCompatibilityRating::Count) ?
           names[static_cast<int>(rating)] :
           "";
}

bool GameList::IsScannableFilename(const std::string& path)
{
  // we don't scan bin files because they'll duplicate
  std::string::size_type pos = path.rfind('.');
  if (pos != std::string::npos && StringUtil::Strcasecmp(path.c_str() + pos, ".bin") == 0)
    return false;

  return System::IsLoadableFilename(path.c_str());
}

bool GameList::GetExeListEntry(const std::string& path, GameListEntry* entry)
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
  entry->code.clear();
  entry->title = FileSystem::StripExtension(display_name);
  entry->region = BIOS::GetPSExeDiscRegion(header);
  entry->total_size = ZeroExtend64(file_size);
  entry->type = GameListEntryType::PSExe;
  entry->compatibility_rating = GameListCompatibilityRating::Unknown;

  return true;
}

bool GameList::GetPsfListEntry(const std::string& path, GameListEntry* entry)
{
  // we don't need to walk the library chain here - the top file is enough
  PSFLoader::File file;
  if (!file.Load(path.c_str()))
    return false;

  entry->code.clear();
  entry->region = file.GetRegion();
  entry->total_size = static_cast<u32>(file.GetProgramData().size());
  entry->type = GameListEntryType::PSF;
  entry->compatibility_rating = GameListCompatibilityRating::Unknown;

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
    entry->title += FileSystem::StripExtension(display_name);
  }

  return true;
}

bool GameList::GetGameListEntry(const std::string& path, GameListEntry* entry)
{
  if (System::IsExeFileName(path.c_str()))
    return GetExeListEntry(path.c_str(), entry);
  if (System::IsPsfFileName(path.c_str()))
    return GetPsfListEntry(path.c_str(), entry);

  std::unique_ptr<CDImage> cdi = CDImage::Open(path.c_str(), nullptr);
  if (!cdi)
    return false;

  entry->path = path;
  entry->total_size = static_cast<u64>(CDImage::RAW_SECTOR_SIZE) * static_cast<u64>(cdi->GetLBACount());
  entry->type = GameListEntryType::Disc;
  entry->compatibility_rating = GameListCompatibilityRating::Unknown;

  // try the database first
  LoadDatabase();
  GameDatabaseEntry dbentry;
  if (!m_database.GetEntryForDisc(cdi.get(), &dbentry))
  {
    // no game code, so use the filename title
    entry->code = System::GetGameCodeForImage(cdi.get(), true);
    entry->title = FileSystem::GetFileTitleFromPath(path);
    entry->compatibility_rating = GameListCompatibilityRating::Unknown;
    entry->release_date = 0;
    entry->min_players = 0;
    entry->max_players = 0;
    entry->min_blocks = 0;
    entry->max_blocks = 0;
    entry->supported_controllers = ~0u;
  }
  else
  {
    // pull from database
    entry->code = std::move(dbentry.serial);
    entry->title = std::move(dbentry.title);
    entry->genre = std::move(dbentry.genre);
    entry->publisher = std::move(dbentry.publisher);
    entry->developer = std::move(dbentry.developer);
    entry->release_date = dbentry.release_date;
    entry->min_players = static_cast<u8>(dbentry.min_players);
    entry->max_players = static_cast<u8>(dbentry.max_players);
    entry->min_blocks = static_cast<u8>(dbentry.min_blocks);
    entry->max_blocks = static_cast<u8>(dbentry.max_blocks);
    entry->supported_controllers = dbentry.supported_controllers_mask;
  }

  // region detection
  entry->region = System::GetRegionFromSystemArea(cdi.get());
  if (entry->region == DiscRegion::Other)
    entry->region = System::GetRegionForCode(entry->code);

  if (!entry->code.empty())
  {
    const GameListCompatibilityEntry* compatibility_entry = GetCompatibilityEntryForCode(entry->code);
    if (compatibility_entry)
      entry->compatibility_rating = compatibility_entry->compatibility_rating;
    else
      Log_WarningPrintf("'%s' (%s) not found in compatibility list", entry->code.c_str(), entry->title.c_str());

    if (!m_game_settings_load_tried)
      LoadGameSettings();
    const GameSettings::Entry* settings = m_game_settings.GetEntry(entry->code);
    if (settings)
      entry->settings = *settings;
  }

  if (cdi->HasSubImages())
  {
    entry->type = GameListEntryType::Playlist;

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

bool GameList::GetGameListEntryFromCache(const std::string& path, GameListEntry* entry)
{
  auto iter = m_cache_map.find(path);
  if (iter == m_cache_map.end())
    return false;

  *entry = std::move(iter->second);
  m_cache_map.erase(iter);
  return true;
}

void GameList::LoadCache()
{
  if (m_cache_filename.empty())
    return;

  std::unique_ptr<ByteStream> stream =
    FileSystem::OpenFile(m_cache_filename.c_str(), BYTESTREAM_OPEN_READ | BYTESTREAM_OPEN_STREAMED);
  if (!stream)
    return;

  if (!LoadEntriesFromCache(stream.get()))
  {
    Log_WarningPrintf("Deleting corrupted cache file '%s'", m_cache_filename.c_str());
    stream.reset();
    m_cache_map.clear();
    DeleteCacheFile();
    return;
  }
}

static bool ReadString(ByteStream* stream, std::string* dest)
{
  u32 size;
  if (!stream->Read2(&size, sizeof(size)))
    return false;

  dest->resize(size);
  if (!stream->Read2(dest->data(), size))
    return false;

  return true;
}

static bool ReadU8(ByteStream* stream, u8* dest)
{
  return stream->Read2(dest, sizeof(u8));
}

static bool ReadU32(ByteStream* stream, u32* dest)
{
  return stream->Read2(dest, sizeof(u32));
}

static bool ReadU64(ByteStream* stream, u64* dest)
{
  return stream->Read2(dest, sizeof(u64));
}

static bool WriteString(ByteStream* stream, const std::string& str)
{
  const u32 size = static_cast<u32>(str.size());
  return (stream->Write2(&size, sizeof(size)) && (size == 0 || stream->Write2(str.data(), size)));
}

static bool WriteU8(ByteStream* stream, u8 dest)
{
  return stream->Write2(&dest, sizeof(u8));
}

static bool WriteU32(ByteStream* stream, u32 dest)
{
  return stream->Write2(&dest, sizeof(u32));
}

static bool WriteU64(ByteStream* stream, u64 dest)
{
  return stream->Write2(&dest, sizeof(u64));
}

bool GameList::LoadEntriesFromCache(ByteStream* stream)
{
  u32 file_signature, file_version;
  if (!ReadU32(stream, &file_signature) || !ReadU32(stream, &file_version) ||
      file_signature != GAME_LIST_CACHE_SIGNATURE || file_version != GAME_LIST_CACHE_VERSION)
  {
    Log_WarningPrintf("Game list cache is corrupted");
    return false;
  }

  while (stream->GetPosition() != stream->GetSize())
  {
    std::string path;
    GameListEntry ge;

    u8 type;
    u8 region;
    u8 compatibility_rating;

    if (!ReadU8(stream, &type) || !ReadU8(stream, &region) || !ReadString(stream, &path) ||
        !ReadString(stream, &ge.code) || !ReadString(stream, &ge.title) || !ReadString(stream, &ge.genre) ||
        !ReadString(stream, &ge.publisher) || !ReadString(stream, &ge.developer) || !ReadU64(stream, &ge.total_size) ||
        !ReadU64(stream, &ge.last_modified_time) || !ReadU64(stream, &ge.release_date) ||
        !ReadU32(stream, &ge.supported_controllers) || !ReadU8(stream, &ge.min_players) ||
        !ReadU8(stream, &ge.max_players) || !ReadU8(stream, &ge.min_blocks) || !ReadU8(stream, &ge.max_blocks) ||
        !ReadU8(stream, &compatibility_rating) || region >= static_cast<u8>(DiscRegion::Count) ||
        type >= static_cast<u8>(GameListEntryType::Count) ||
        compatibility_rating >= static_cast<u8>(GameListCompatibilityRating::Count))
    {
      Log_WarningPrintf("Game list cache entry is corrupted");
      return false;
    }

    ge.path = path;
    ge.region = static_cast<DiscRegion>(region);
    ge.type = static_cast<GameListEntryType>(type);
    ge.compatibility_rating = static_cast<GameListCompatibilityRating>(compatibility_rating);

    if (!ge.settings.LoadFromStream(stream))
    {
      Log_WarningPrintf("Game list cache entry is corrupted (settings)");
      return false;
    }

    auto iter = m_cache_map.find(ge.path);
    if (iter != m_cache_map.end())
      iter->second = std::move(ge);
    else
      m_cache_map.emplace(std::move(path), std::move(ge));
  }

  return true;
}

bool GameList::OpenCacheForWriting()
{
  if (m_cache_filename.empty())
    return false;

  Assert(!m_cache_write_stream);
  m_cache_write_stream =
    FileSystem::OpenFile(m_cache_filename.c_str(), BYTESTREAM_OPEN_CREATE | BYTESTREAM_OPEN_WRITE |
                                                     BYTESTREAM_OPEN_APPEND | BYTESTREAM_OPEN_STREAMED);
  if (!m_cache_write_stream || !m_cache_write_stream->SeekToEnd())
  {
    m_cache_write_stream.reset();
    return false;
  }

  if (m_cache_write_stream->GetPosition() == 0)
  {
    // new cache file, write header
    if (!WriteU32(m_cache_write_stream.get(), GAME_LIST_CACHE_SIGNATURE) ||
        !WriteU32(m_cache_write_stream.get(), GAME_LIST_CACHE_VERSION))
    {
      Log_ErrorPrintf("Failed to write game list cache header");
      m_cache_write_stream.reset();
      FileSystem::DeleteFile(m_cache_filename.c_str());
      return false;
    }
  }

  return true;
}

bool GameList::WriteEntryToCache(const GameListEntry* entry, ByteStream* stream)
{
  bool result = true;
  result &= WriteU8(stream, static_cast<u8>(entry->type));
  result &= WriteU8(stream, static_cast<u8>(entry->region));
  result &= WriteString(stream, entry->path);
  result &= WriteString(stream, entry->code);
  result &= WriteString(stream, entry->title);
  result &= WriteString(stream, entry->genre);
  result &= WriteString(stream, entry->publisher);
  result &= WriteString(stream, entry->developer);
  result &= WriteU64(stream, entry->total_size);
  result &= WriteU64(stream, entry->last_modified_time);
  result &= WriteU64(stream, entry->release_date);
  result &= WriteU32(stream, entry->supported_controllers);
  result &= WriteU8(stream, entry->min_players);
  result &= WriteU8(stream, entry->max_players);
  result &= WriteU8(stream, entry->min_blocks);
  result &= WriteU8(stream, entry->max_blocks);
  result &= WriteU8(stream, static_cast<u8>(entry->compatibility_rating));
  result &= entry->settings.SaveToStream(stream);
  return result;
}

void GameList::FlushCacheFileStream()
{
  if (!m_cache_write_stream)
    return;

  m_cache_write_stream->Flush();
}

void GameList::CloseCacheFileStream()
{
  if (!m_cache_write_stream)
    return;

  m_cache_write_stream->Commit();
  m_cache_write_stream.reset();
}

void GameList::RewriteCacheFile()
{
  CloseCacheFileStream();
  DeleteCacheFile();
  if (OpenCacheForWriting())
  {
    for (const auto& it : m_entries)
    {
      if (!WriteEntryToCache(&it, m_cache_write_stream.get()))
      {
        Log_ErrorPrintf("Failed to write '%s' to new cache file", it.title.c_str());
        break;
      }
    }

    CloseCacheFileStream();
  }
}

void GameList::DeleteCacheFile()
{
  Assert(!m_cache_write_stream);
  if (!FileSystem::FileExists(m_cache_filename.c_str()))
    return;

  if (FileSystem::DeleteFile(m_cache_filename.c_str()))
    Log_InfoPrintf("Deleted game list cache '%s'", m_cache_filename.c_str());
  else
    Log_WarningPrintf("Failed to delete game list cache '%s'", m_cache_filename.c_str());
}

void GameList::ScanDirectory(const char* path, bool recursive, ProgressCallback* progress)
{
  Log_DevPrintf("Scanning %s%s", path, recursive ? " (recursively)" : "");

  progress->PushState();
  progress->SetFormattedStatusText("Scanning directory '%s'%s...", path, recursive ? " (recursively)" : "");

  FileSystem::FindResultsArray files;
  FileSystem::FindFiles(path, "*",
                        recursive ? (FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES | FILESYSTEM_FIND_RECURSIVE) :
                                    (FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES),
                        &files);

  progress->SetProgressRange(static_cast<u32>(files.size()));
  progress->SetProgressValue(0);

  for (FILESYSTEM_FIND_DATA& ffd : files)
  {
    progress->IncrementProgressValue();

    if (!IsScannableFilename(ffd.FileName) || IsPathExcluded(ffd.FileName) || GetEntryForPath(ffd.FileName.c_str()))
      continue;

    const u64 modified_time = ffd.ModificationTime.AsUnixTimestamp();
    if (AddFileFromCache(ffd.FileName, modified_time))
      continue;

    // ownership of fp is transferred
    progress->SetFormattedStatusText("Scanning '%s'...", FileSystem::GetDisplayNameFromPath(ffd.FileName).c_str());
    ScanFile(std::move(ffd.FileName), modified_time);
  }

  progress->SetProgressValue(static_cast<u32>(files.size()));
  progress->PopState();
}

bool GameList::AddFileFromCache(const std::string& path, u64 timestamp)
{
  if (std::any_of(m_entries.begin(), m_entries.end(),
                  [&path](const GameListEntry& other) { return other.path == path; }))
  {
    // already exists
    return true;
  }

  GameListEntry entry;
  if (!GetGameListEntryFromCache(path, &entry) || entry.last_modified_time != timestamp)
    return false;

  m_entries.push_back(std::move(entry));
  return true;
}

bool GameList::ScanFile(std::string path, u64 timestamp)
{
  Log_DevPrintf("Scanning '%s'...", path.c_str());

  GameListEntry entry;
  if (!GetGameListEntry(path, &entry))
    return false;

  entry.path = std::move(path);
  entry.last_modified_time = timestamp;

  if (m_cache_write_stream || OpenCacheForWriting())
  {
    if (!WriteEntryToCache(&entry, m_cache_write_stream.get()))
      Log_WarningPrintf("Failed to write entry '%s' to cache", entry.path.c_str());
  }

  m_entries.push_back(std::move(entry));
  return true;
}

void GameList::AddDirectory(std::string path, bool recursive)
{
  auto iter = std::find_if(m_search_directories.begin(), m_search_directories.end(),
                           [&path](const DirectoryEntry& de) { return de.path == path; });
  if (iter != m_search_directories.end())
  {
    iter->recursive = recursive;
    return;
  }

  m_search_directories.push_back({path, recursive});
}

const GameListEntry* GameList::GetEntryForPath(const char* path) const
{
  const size_t path_length = std::strlen(path);
  for (const GameListEntry& entry : m_entries)
  {
    if (entry.path.size() == path_length && StringUtil::Strcasecmp(entry.path.c_str(), path) == 0)
      return &entry;
  }

  return nullptr;
}

GameListEntry* GameList::GetMutableEntryForPath(const char* path)
{
  const size_t path_length = std::strlen(path);
  for (GameListEntry& entry : m_entries)
  {
    if (entry.path.size() == path_length && StringUtil::Strcasecmp(entry.path.c_str(), path) == 0)
      return &entry;
  }

  return nullptr;
}

const GameListCompatibilityEntry* GameList::GetCompatibilityEntryForCode(const std::string& code) const
{
  if (!m_compatibility_list_load_tried)
    const_cast<GameList*>(this)->LoadCompatibilityList();

  auto iter = m_compatibility_list.find(code);
  return (iter != m_compatibility_list.end()) ? &iter->second : nullptr;
}

bool GameList::GetDatabaseEntryForCode(const std::string_view& code, GameDatabaseEntry* entry)
{
  LoadDatabase();
  return m_database.GetEntryForCode(code, entry);
}

bool GameList::GetDatabaseEntryForDisc(CDImage* image, GameDatabaseEntry* entry)
{
  LoadDatabase();
  return m_database.GetEntryForDisc(image, entry);
}

bool GameList::IsPathExcluded(const std::string& path) const
{
  return (std::find(m_excluded_paths.begin(), m_excluded_paths.end(), path) != m_excluded_paths.end());
}

void GameList::SetSearchDirectoriesFromSettings(SettingsInterface& si)
{
  m_search_directories.clear();
  m_excluded_paths = si.GetStringList("GameList", "ExcludedPaths");

  std::vector<std::string> dirs = si.GetStringList("GameList", "Paths");
  for (std::string& dir : dirs)
    m_search_directories.push_back({std::move(dir), false});

  dirs = si.GetStringList("GameList", "RecursivePaths");
  for (std::string& dir : dirs)
    m_search_directories.push_back({std::move(dir), true});
}

void GameList::Refresh(bool invalidate_cache, bool invalidate_database, ProgressCallback* progress /* = nullptr */)
{
  m_game_list_loaded = true;

  if (!progress)
    progress = ProgressCallback::NullProgressCallback;

  if (invalidate_cache)
    DeleteCacheFile();
  else
    LoadCache();

  if (invalidate_database)
    ClearDatabase();

  m_entries.clear();

  if (!m_search_directories.empty())
  {
    progress->SetProgressRange(static_cast<u32>(m_search_directories.size()));
    progress->SetProgressValue(0);

    for (u32 i = 0; i < static_cast<u32>(m_search_directories.size()); i++)
    {
      const DirectoryEntry& de = m_search_directories[i];
      ScanDirectory(de.path.c_str(), de.recursive, progress);
      progress->SetProgressValue(i + 1);
    }
  }

  // don't need unused cache entries
  CloseCacheFileStream();
  m_cache_map.clear();

  // we don't need to keep the db around anymore, it's quick enough to re-parse if needed anyway
  ClearDatabase();
}

void GameList::UpdateCompatibilityEntry(GameListCompatibilityEntry new_entry, bool save_to_list /*= true*/)
{
  auto iter = m_compatibility_list.find(new_entry.code.c_str());
  if (iter != m_compatibility_list.end())
  {
    iter->second = std::move(new_entry);
  }
  else
  {
    std::string key(new_entry.code);
    iter = m_compatibility_list.emplace(std::move(key), std::move(new_entry)).first;
  }

  auto game_list_it = std::find_if(m_entries.begin(), m_entries.end(),
                                   [&iter](const GameListEntry& ge) { return (ge.code == iter->second.code); });
  if (game_list_it != m_entries.end() && game_list_it->compatibility_rating != iter->second.compatibility_rating)
  {
    game_list_it->compatibility_rating = iter->second.compatibility_rating;
    RewriteCacheFile();
  }

  if (save_to_list)
    SaveCompatibilityDatabaseForEntry(&iter->second);
}

void GameList::LoadDatabase()
{
  if (m_database_load_tried)
    return;

  m_database_load_tried = true;
  m_database.Load();
}

void GameList::ClearDatabase()
{
  m_database.Unload();
  m_database_load_tried = false;
}

class GameList::CompatibilityListVisitor final : public tinyxml2::XMLVisitor
{
public:
  CompatibilityListVisitor(CompatibilityMap& database) : m_database(database) {}

  static std::string FixupSerial(const std::string_view str)
  {
    std::string ret;
    ret.reserve(str.length());
    for (size_t i = 0; i < str.length(); i++)
    {
      if (str[i] == '.' || str[i] == '#')
        continue;
      else if (str[i] == ',')
        break;
      else if (str[i] == '_' || str[i] == ' ')
        ret.push_back('-');
      else
        ret.push_back(static_cast<char>(std::toupper(str[i])));
    }

    return ret;
  }

  bool VisitEnter(const tinyxml2::XMLElement& element, const tinyxml2::XMLAttribute* firstAttribute) override
  {
    // recurse into gamelist
    if (StringUtil::Strcasecmp(element.Name(), "compatibility-list") == 0)
      return true;

    if (StringUtil::Strcasecmp(element.Name(), "entry") != 0)
      return false;

    const char* attr = element.Attribute("code");
    std::string code(attr ? attr : "");
    attr = element.Attribute("title");
    std::string title(attr ? attr : "");
    attr = element.Attribute("region");
    std::optional<DiscRegion> region = Settings::ParseDiscRegionName(attr ? attr : "");
    const int compatibility = element.IntAttribute("compatibility");

    const tinyxml2::XMLElement* upscaling_elem = element.FirstChildElement("upscaling-issues");
    const tinyxml2::XMLElement* version_tested_elm = element.FirstChildElement("version-tested");
    const tinyxml2::XMLElement* comments_elem = element.FirstChildElement("comments");
    const char* upscaling = upscaling_elem ? upscaling_elem->GetText() : nullptr;
    const char* version_tested = version_tested_elm ? version_tested_elm->GetText() : nullptr;
    const char* comments = comments_elem ? comments_elem->GetText() : nullptr;
    if (code.empty() || !region.has_value() || compatibility < 0 ||
        compatibility >= static_cast<int>(GameListCompatibilityRating::Count))
    {
      Log_ErrorPrintf("Missing child node at line %d", element.GetLineNum());
      return false;
    }

    auto iter = m_database.find(code);
    if (iter != m_database.end())
    {
      Log_WarningPrintf("Duplicate game code in compatibility list: '%s'", code.c_str());
      m_database.erase(iter);
    }

    GameListCompatibilityEntry entry;
    entry.code = code;
    entry.title = title;
    entry.region = region.value();
    entry.compatibility_rating = static_cast<GameListCompatibilityRating>(compatibility);

    if (upscaling)
      entry.upscaling_issues = upscaling;
    if (version_tested)
      entry.version_tested = version_tested;
    if (comments)
      entry.comments = comments;

    m_database.emplace(std::move(code), std::move(entry));
    return false;
  }

private:
  CompatibilityMap& m_database;
};

void GameList::LoadCompatibilityList()
{
  if (m_compatibility_list_load_tried)
    return;

  m_compatibility_list_load_tried = true;

  // list we ship with
  {
    std::unique_ptr<ByteStream> file =
      g_host_interface->OpenPackageFile("database/compatibility.xml", BYTESTREAM_OPEN_READ | BYTESTREAM_OPEN_STREAMED);
    if (file)
    {
      LoadCompatibilityListFromXML(FileSystem::ReadStreamToString(file.get()));
    }
    else
    {
      Log_ErrorPrintf("Failed to load compatibility.xml from package");
    }
  }

  // user's list
  if (!m_user_compatibility_list_filename.empty() && FileSystem::FileExists(m_user_compatibility_list_filename.c_str()))
  {
    std::unique_ptr<ByteStream> file =
      FileSystem::OpenFile(m_user_compatibility_list_filename.c_str(), BYTESTREAM_OPEN_READ | BYTESTREAM_OPEN_STREAMED);
    if (file)
      LoadCompatibilityListFromXML(FileSystem::ReadStreamToString(file.get()));
  }
}

bool GameList::LoadCompatibilityListFromXML(const std::string& xml)
{
  tinyxml2::XMLDocument doc;
  tinyxml2::XMLError error = doc.Parse(xml.c_str(), xml.size());
  if (error != tinyxml2::XML_SUCCESS)
  {
    Log_ErrorPrintf("Failed to parse compatibility list: %s", tinyxml2::XMLDocument::ErrorIDToName(error));
    return false;
  }

  const tinyxml2::XMLElement* datafile_elem = doc.FirstChildElement("compatibility-list");
  if (!datafile_elem)
  {
    Log_ErrorPrintf("Failed to get compatibility-list element");
    return false;
  }

  CompatibilityListVisitor visitor(m_compatibility_list);
  datafile_elem->Accept(&visitor);
  Log_InfoPrintf("Loaded %zu entries from compatibility list", m_compatibility_list.size());
  return true;
}

static void InitElementForCompatibilityEntry(tinyxml2::XMLDocument* doc, tinyxml2::XMLElement* entry_elem,
                                             const GameListCompatibilityEntry* entry)
{
  entry_elem->SetAttribute("code", entry->code.c_str());
  entry_elem->SetAttribute("title", entry->title.c_str());
  entry_elem->SetAttribute("region", Settings::GetDiscRegionName(entry->region));
  entry_elem->SetAttribute("compatibility", static_cast<int>(entry->compatibility_rating));

  tinyxml2::XMLElement* elem = entry_elem->FirstChildElement("compatibility");
  if (!elem)
  {
    elem = doc->NewElement("compatibility");
    entry_elem->InsertEndChild(elem);
  }
  elem->SetText(GameList::GetGameListCompatibilityRatingString(entry->compatibility_rating));

  if (!entry->upscaling_issues.empty())
  {
    elem = entry_elem->FirstChildElement("upscaling-issues");
    if (!entry->upscaling_issues.empty())
    {
      if (!elem)
      {
        elem = doc->NewElement("upscaling-issues");
        entry_elem->InsertEndChild(elem);
      }
      elem->SetText(entry->upscaling_issues.c_str());
    }
    else
    {
      if (elem)
        entry_elem->DeleteChild(elem);
    }
  }

  if (!entry->version_tested.empty())
  {
    elem = entry_elem->FirstChildElement("version-tested");
    if (!entry->version_tested.empty())
    {
      if (!elem)
      {
        elem = doc->NewElement("version-tested");
        entry_elem->InsertEndChild(elem);
      }
      elem->SetText(entry->version_tested.c_str());
    }
    else
    {
      if (elem)
        entry_elem->DeleteChild(elem);
    }
  }

  if (!entry->comments.empty())
  {
    elem = entry_elem->FirstChildElement("comments");
    if (!entry->comments.empty())
    {
      if (!elem)
      {
        elem = doc->NewElement("comments");
        entry_elem->InsertEndChild(elem);
      }
      elem->SetText(entry->comments.c_str());
    }
    else
    {
      if (elem)
        entry_elem->DeleteChild(elem);
    }
  }
}

bool GameList::SaveCompatibilityDatabaseForEntry(const GameListCompatibilityEntry* entry)
{
  if (m_user_compatibility_list_filename.empty())
    return false;

  tinyxml2::XMLDocument doc;
  tinyxml2::XMLError error;
  tinyxml2::XMLElement* root_elem;
  auto fp = FileSystem::OpenManagedCFile(m_user_compatibility_list_filename.c_str(), "rb");
  if (fp)
  {
    error = doc.LoadFile(fp.get());
    if (error != tinyxml2::XML_SUCCESS)
    {
      Log_ErrorPrintf("Failed to parse compatibility list '%s': %s", m_user_compatibility_list_filename.c_str(),
                      tinyxml2::XMLDocument::ErrorIDToName(error));
      return false;
    }

    root_elem = doc.FirstChildElement("compatibility-list");
    if (!root_elem)
    {
      Log_ErrorPrintf("Failed to get compatibility-list element in '%s'", m_user_compatibility_list_filename.c_str());
      return false;
    }
  }
  else
  {
    root_elem = doc.NewElement("compatibility-list");
    doc.InsertEndChild(root_elem);
  }

  tinyxml2::XMLElement* current_entry_elem = root_elem->FirstChildElement();
  while (current_entry_elem)
  {
    const char* existing_code = current_entry_elem->Attribute("code");
    if (existing_code && StringUtil::Strcasecmp(entry->code.c_str(), existing_code) == 0)
    {
      // update the existing element
      InitElementForCompatibilityEntry(&doc, current_entry_elem, entry);
      break;
    }

    current_entry_elem = current_entry_elem->NextSiblingElement();
  }

  if (!current_entry_elem)
  {
    // not found, insert
    tinyxml2::XMLElement* entry_elem = doc.NewElement("entry");
    root_elem->InsertEndChild(entry_elem);
    InitElementForCompatibilityEntry(&doc, entry_elem, entry);
  }

  fp.reset();
  fp = FileSystem::OpenManagedCFile(m_user_compatibility_list_filename.c_str(), "wb");
  if (!fp)
  {
    Log_ErrorPrintf("Failed to open file '%s'", m_user_compatibility_list_filename.c_str());
    return false;
  }

  error = doc.SaveFile(fp.get());
  if (error != tinyxml2::XML_SUCCESS)
  {
    Log_ErrorPrintf("Failed to update compatibility list '%s': %s", m_user_compatibility_list_filename.c_str(),
                    tinyxml2::XMLDocument::ErrorIDToName(error));
    return false;
  }

  Log_InfoPrintf("Updated compatibility list '%s'", m_user_compatibility_list_filename.c_str());
  return true;
}

std::string GameList::ExportCompatibilityEntry(const GameListCompatibilityEntry* entry)
{
  tinyxml2::XMLDocument doc;
  tinyxml2::XMLElement* root_elem = doc.NewElement("compatibility-list");
  doc.InsertEndChild(root_elem);

  tinyxml2::XMLElement* entry_elem = doc.NewElement("entry");
  root_elem->InsertEndChild(entry_elem);
  InitElementForCompatibilityEntry(&doc, entry_elem, entry);

  tinyxml2::XMLPrinter printer;
  // doc.Print(&printer);
  entry_elem->Accept(&printer);
  return std::string(printer.CStr(), printer.CStrSize());
}

void GameList::LoadGameSettings()
{
  if (m_game_settings_load_tried)
    return;

  m_game_settings_load_tried = true;

  // list we ship with
  {
    std::unique_ptr<ByteStream> file =
      g_host_interface->OpenPackageFile("database/gamesettings.ini", BYTESTREAM_OPEN_READ | BYTESTREAM_OPEN_STREAMED);
    if (file)
    {
      m_game_settings.Load(FileSystem::ReadStreamToString(file.get()));
    }
    else
    {
      Log_ErrorPrintf("Failed to load compatibility.xml from package");
    }
  }

  // user's list
  if (!m_user_game_settings_filename.empty() && FileSystem::FileExists(m_user_game_settings_filename.c_str()))
  {
    std::unique_ptr<ByteStream> file =
      FileSystem::OpenFile(m_user_game_settings_filename.c_str(), BYTESTREAM_OPEN_READ | BYTESTREAM_OPEN_STREAMED);
    if (file)
      m_game_settings.Load(FileSystem::ReadStreamToString(file.get()));
  }
}

const GameSettings::Entry* GameList::GetGameSettings(const std::string& filename, const std::string& game_code)
{
  const GameListEntry* entry = GetMutableEntryForPath(filename.c_str());
  if (entry)
    return &entry->settings;

  if (!m_game_settings_load_tried)
    LoadGameSettings();

  return m_game_settings.GetEntry(game_code);
}

const GameSettings::Entry* GameList::GetGameSettingsForCode(const std::string& game_code)
{
  if (!m_game_settings_load_tried)
    LoadGameSettings();

  return m_game_settings.GetEntry(game_code);
}

void GameList::UpdateGameSettings(const std::string& filename, const std::string& game_code,
                                  const std::string& game_title, const GameSettings::Entry& new_entry,
                                  bool save_to_list /* = true */)
{
  GameListEntry* entry = GetMutableEntryForPath(filename.c_str());
  if (entry)
  {
    entry->settings = new_entry;
    RewriteCacheFile();
  }

  if (save_to_list)
    m_game_settings.SetEntry(game_code, game_title, new_entry, m_user_game_settings_filename.c_str());
}

std::string GameList::GetCoverImagePathForEntry(const GameListEntry* entry) const
{
  return GetCoverImagePath(entry->path, entry->code, entry->title);
}

std::string GameList::GetCoverImagePath(const std::string& path, const std::string& code,
                                        const std::string& title) const
{
  static constexpr auto extensions = make_array("jpg", "jpeg", "png", "webp");

  PathString cover_path;
  for (const char* extension : extensions)
  {
    // use the file title if it differs (e.g. modded games)
    const std::string_view file_title(FileSystem::GetFileTitleFromPath(path));
    if (!file_title.empty() && title != file_title)
    {
      cover_path.Clear();
      cover_path.AppendString(g_host_interface->GetUserDirectory().c_str());
      cover_path.AppendCharacter(FS_OSPATH_SEPARATOR_CHARACTER);
      cover_path.AppendString("covers");
      cover_path.AppendCharacter(FS_OSPATH_SEPARATOR_CHARACTER);
      cover_path.AppendString(file_title);
      cover_path.AppendCharacter('.');
      cover_path.AppendString(extension);
      if (FileSystem::FileExists(cover_path))
        return std::string(cover_path.GetCharArray());
    }

    // try the title
    if (!title.empty())
    {
      cover_path.Format("%s" FS_OSPATH_SEPARATOR_STR "covers" FS_OSPATH_SEPARATOR_STR "%s.%s",
                        g_host_interface->GetUserDirectory().c_str(), title.c_str(), extension);
      if (FileSystem::FileExists(cover_path))
        return std::string(cover_path.GetCharArray());
    }

    // then the code
    if (!code.empty())
    {
      cover_path.Format("%s" FS_OSPATH_SEPARATOR_STR "covers" FS_OSPATH_SEPARATOR_STR "%s.%s",
                        g_host_interface->GetUserDirectory().c_str(), code.c_str(), extension);
      if (FileSystem::FileExists(cover_path))
        return std::string(cover_path.GetCharArray());
    }
  }

  return std::string();
}

std::string GameList::GetNewCoverImagePathForEntry(const GameListEntry* entry, const char* new_filename) const
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

  return g_host_interface->GetUserDirectoryRelativePath("covers" FS_OSPATH_SEPARATOR_STR "%s%s", entry->title.c_str(),
                                                        extension);
}

size_t GameListEntry::GetReleaseDateString(char* buffer, size_t buffer_size) const
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
