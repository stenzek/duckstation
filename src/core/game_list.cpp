#include "game_list.h"
#include "YBaseLib/AutoReleasePtr.h"
#include "YBaseLib/BinaryReader.h"
#include "YBaseLib/BinaryWriter.h"
#include "YBaseLib/ByteStream.h"
#include "YBaseLib/FileSystem.h"
#include "YBaseLib/Log.h"
#include "bios.h"
#include "common/cd_image.h"
#include "common/iso_reader.h"
#include "settings.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>
#include <tinyxml2.h>
#include <utility>
Log_SetChannel(GameList);

#ifdef _MSC_VER
#define CASE_COMPARE _stricmp
#else
#define CASE_COMPARE strcasecmp
#endif

GameList::GameList() = default;

GameList::~GameList() = default;

const char* GameList::EntryTypeToString(GameList::EntryType type)
{
  static std::array<const char*, 2> names = {{"Disc", "PSExe"}};
  return names[static_cast<int>(type)];
}

std::string GameList::GetGameCodeForPath(const char* image_path)
{
  std::unique_ptr<CDImage> cdi = CDImage::Open(image_path);
  if (!cdi)
    return {};

  return GetGameCodeForImage(cdi.get());
}

std::string GameList::GetGameCodeForImage(CDImage* cdi)
{
  ISOReader iso;
  if (!iso.Open(cdi, 1))
    return {};

  // Read SYSTEM.CNF
  std::vector<u8> system_cnf_data;
  if (!iso.ReadFile("SYSTEM.CNF", &system_cnf_data))
    return {};

  // Parse lines
  std::vector<std::pair<std::string, std::string>> lines;
  std::pair<std::string, std::string> current_line;
  bool reading_value = false;
  for (size_t pos = 0; pos < system_cnf_data.size(); pos++)
  {
    const char ch = static_cast<char>(system_cnf_data[pos]);
    if (ch == '\r' || ch == '\n')
    {
      if (!current_line.first.empty())
      {
        lines.push_back(std::move(current_line));
        current_line = {};
        reading_value = false;
      }
    }
    else if (ch == ' ')
    {
      continue;
    }
    else if (ch == '=' && !reading_value)
    {
      reading_value = true;
    }
    else
    {
      if (reading_value)
        current_line.second.push_back(ch);
      else
        current_line.first.push_back(ch);
    }
  }

  if (!current_line.first.empty())
    lines.push_back(std::move(current_line));

  // Find the BOOT line
  auto iter = std::find_if(lines.begin(), lines.end(),
                           [](const auto& it) { return CASE_COMPARE(it.first.c_str(), "boot") == 0; });
  if (iter == lines.end())
    return {};

  // cdrom:\SCES_123.45;1
  std::string code = iter->second;
  std::string::size_type pos = code.rfind('\\');
  if (pos != std::string::npos)
  {
    code.erase(0, pos + 1);
  }
  else
  {
    // cdrom:SCES_123.45;1
    pos = code.rfind(':');
    if (pos != std::string::npos)
      code.erase(0, pos + 1);
  }

  pos = code.find(';');
  if (pos != std::string::npos)
    code.erase(pos);

  // SCES_123.45 -> SCES-12345
  for (pos = 0; pos < code.size();)
  {
    if (code[pos] == '.')
    {
      code.erase(pos, 1);
      continue;
    }

    if (code[pos] == '_')
      code[pos] = '-';
    else
      code[pos] = static_cast<char>(std::toupper(code[pos]));

    pos++;
  }

  return code;
}

std::optional<ConsoleRegion> GameList::GetRegionForCode(std::string_view code)
{
  std::string prefix;
  for (size_t pos = 0; pos < code.length(); pos++)
  {
    const int ch = std::tolower(code[pos]);
    if (ch < 'a' || ch > 'z')
      break;

    prefix.push_back(static_cast<char>(ch));
  }

  if (prefix == "sces" || prefix == "sced" || prefix == "sles" || prefix == "sled")
    return ConsoleRegion::PAL;
  else if (prefix == "scps" || prefix == "slps" || prefix == "slpm")
    return ConsoleRegion::NTSC_J;
  else if (prefix == "scus" || prefix == "slus" || prefix == "papx")
    return ConsoleRegion::NTSC_U;
  else
    return std::nullopt;
}

std::optional<ConsoleRegion> GameList::GetRegionFromSystemArea(CDImage* cdi)
{
  // The license code is on sector 4 of the disc.
  u8 sector[CDImage::DATA_SECTOR_SIZE];
  if (!cdi->Seek(1, 4) || cdi->Read(CDImage::ReadMode::DataOnly, 1, sector) != 1)
    return std::nullopt;

  static constexpr char ntsc_u_string[] = "          Licensed  by          Sony Computer Entertainment Amer  ica ";
  static constexpr char ntsc_j_string[] = "          Licensed  by          Sony Computer Entertainment Inc.";
  static constexpr char pal_string[] = "          Licensed  by          Sony Computer Entertainment Euro pe";

  // subtract one for the terminating null
  if (std::equal(ntsc_u_string, ntsc_u_string + countof(ntsc_u_string) - 1, sector))
    return ConsoleRegion::NTSC_U;
  else if (std::equal(ntsc_j_string, ntsc_j_string + countof(ntsc_j_string) - 1, sector))
    return ConsoleRegion::NTSC_J;
  else if (std::equal(pal_string, pal_string + countof(pal_string) - 1, sector))
    return ConsoleRegion::PAL;

  return std::nullopt;
}

std::optional<ConsoleRegion> GameList::GetRegionForImage(CDImage* cdi)
{
  std::optional<ConsoleRegion> system_area_region = GetRegionFromSystemArea(cdi);
  if (system_area_region)
    return system_area_region;

  std::string code = GetGameCodeForImage(cdi);
  if (code.empty())
    return std::nullopt;

  return GetRegionForCode(code);
}

std::optional<ConsoleRegion> GameList::GetRegionForPath(const char* image_path)
{
  std::unique_ptr<CDImage> cdi = CDImage::Open(image_path);
  if (!cdi)
    return {};

  return GetRegionForImage(cdi.get());
}

bool GameList::IsExeFileName(const char* path)
{
  const char* extension = std::strrchr(path, '.');
  return (extension && (CASE_COMPARE(extension, ".exe") == 0 || CASE_COMPARE(extension, ".psexe") == 0));
}

static std::string_view GetFileNameFromPath(const char* path)
{
  const char* filename_end = path + std::strlen(path);
  const char* filename_start = std::max(std::strrchr(path, '/'), std::strrchr(path, '\\'));
  if (!filename_start)
    return std::string_view(path, filename_end - path);
  else
    return std::string_view(filename_start + 1, filename_end - filename_start);
}

static std::string_view GetTitleForPath(const char* path)
{
  const char* extension = std::strrchr(path, '.');
  if (path == extension)
    return path;

  const char* path_end = path + std::strlen(path);
  const char* title_end = extension ? (extension - 1) : (path_end);
  const char* title_start = std::max(std::strrchr(path, '/'), std::strrchr(path, '\\'));
  if (!title_start || title_start == path)
    return std::string_view(path, title_end - title_start);
  else
    return std::string_view(title_start + 1, title_end - title_start);
}

bool GameList::GetExeListEntry(const char* path, GameListEntry* entry)
{
  FILESYSTEM_STAT_DATA ffd;
  if (!FileSystem::StatFile(path, &ffd))
    return false;

  std::FILE* fp = std::fopen(path, "rb");
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
    Log_DebugPrintf("%s is not a valid PS-EXE", path);
    return false;
  }

  const char* extension = std::strrchr(path, '.');
  if (!extension)
    return false;

  entry->code.clear();
  entry->title = GetFileNameFromPath(path);

  // no way to detect region...
  entry->path = path;
  entry->region = ConsoleRegion::NTSC_U;
  entry->total_size = ZeroExtend64(file_size);
  entry->last_modified_time = ffd.ModificationTime.AsUnixTimestamp();
  entry->type = EntryType::PSExe;

  return true;
}

bool GameList::GetGameListEntry(const std::string& path, GameListEntry* entry)
{
  if (IsExeFileName(path.c_str()))
    return GetExeListEntry(path.c_str(), entry);

  std::unique_ptr<CDImage> cdi = CDImage::Open(path.c_str());
  if (!cdi)
    return false;

  entry->path = path;
  entry->code = GetGameCodeForImage(cdi.get());
  entry->region =
    GetRegionFromSystemArea(cdi.get()).value_or(GetRegionForCode(entry->code).value_or(ConsoleRegion::NTSC_U));
  entry->total_size = static_cast<u64>(CDImage::RAW_SECTOR_SIZE) * static_cast<u64>(cdi->GetLBACount());
  entry->type = EntryType::Disc;
  cdi.reset();

  if (entry->code.empty())
  {
    // no game code, so use the filename title
    entry->title = GetTitleForPath(path.c_str());
  }
  else
  {
    LoadDatabase();

    auto iter = m_database.find(entry->code);
    if (iter != m_database.end())
    {
      entry->title = iter->second.title;
      entry->region = iter->second.region;
    }
    else
    {
      Log_WarningPrintf("'%s' not found in database", entry->code.c_str());
      entry->title = GetTitleForPath(path.c_str());
    }
  }

  FILESYSTEM_STAT_DATA ffd;
  if (!FileSystem::StatFile(path.c_str(), &ffd))
    return false;

  entry->last_modified_time = ffd.ModificationTime.AsUnixTimestamp();
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

  ByteStream* stream = FileSystem::OpenFile(m_cache_filename.c_str(), BYTESTREAM_OPEN_READ | BYTESTREAM_OPEN_STREAMED);
  if (!stream)
    return;

  if (!LoadEntriesFromCache(stream))
  {
    Log_WarningPrintf("Deleting corrupted cache file '%s'", m_cache_filename.c_str());
    stream->Release();
    m_cache_map.clear();
    DeleteCacheFile();
    return;
  }

  stream->Release();
}

bool GameList::LoadEntriesFromCache(ByteStream* stream)
{
  BinaryReader reader(stream);
  if (reader.ReadUInt32() != GAME_LIST_CACHE_SIGNATURE || reader.ReadUInt32() != GAME_LIST_CACHE_VERSION)
  {
    Log_WarningPrintf("Game list cache is corrupted");
    return false;
  }

  String path;
  TinyString code;
  SmallString title;
  u64 total_size;
  u64 last_modified_time;
  u8 region;
  u8 type;

  while (stream->GetPosition() != stream->GetSize())
  {
    if (!reader.SafeReadSizePrefixedString(&path) || !reader.SafeReadSizePrefixedString(&code) ||
        !reader.SafeReadSizePrefixedString(&title) || !reader.SafeReadUInt64(&total_size) ||
        !reader.SafeReadUInt64(&last_modified_time) || !reader.SafeReadUInt8(&region) ||
        region >= static_cast<u8>(ConsoleRegion::Count) || !reader.SafeReadUInt8(&type) ||
        type > static_cast<u8>(EntryType::PSExe))
    {
      Log_WarningPrintf("Game list cache entry is corrupted");
      return false;
    }

    GameListEntry ge;
    ge.path = path;
    ge.code = code;
    ge.title = title;
    ge.total_size = total_size;
    ge.last_modified_time = last_modified_time;
    ge.region = static_cast<ConsoleRegion>(region);
    ge.type = static_cast<EntryType>(type);

    auto iter = m_cache_map.find(ge.path);
    if (iter != m_cache_map.end())
      iter->second = std::move(ge);
    else
      m_cache_map.emplace(path, std::move(ge));
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
  if (!m_cache_write_stream)
    return false;

  if (m_cache_write_stream->GetPosition() == 0)
  {
    // new cache file, write header
    BinaryWriter writer(m_cache_write_stream);
    if (!writer.SafeWriteUInt32(GAME_LIST_CACHE_SIGNATURE) || !writer.SafeWriteUInt32(GAME_LIST_CACHE_VERSION))
    {
      Log_ErrorPrintf("Failed to write game list cache header");
      m_cache_write_stream->Release();
      m_cache_write_stream = nullptr;
      FileSystem::DeleteFile(m_cache_filename.c_str());
      return false;
    }
  }

  return true;
}

bool GameList::WriteEntryToCache(const GameListEntry* entry, ByteStream* stream)
{
  BinaryWriter writer(stream);
  bool result = writer.SafeWriteSizePrefixedString(entry->path.c_str());
  result &= writer.SafeWriteSizePrefixedString(entry->code.c_str());
  result &= writer.SafeWriteSizePrefixedString(entry->title.c_str());
  result &= writer.SafeWriteUInt64(entry->total_size);
  result &= writer.SafeWriteUInt64(entry->last_modified_time);
  result &= writer.SafeWriteUInt8(static_cast<u8>(entry->region));
  result &= writer.SafeWriteUInt8(static_cast<u8>(entry->type));
  return result;
}

void GameList::CloseCacheFileStream()
{
  if (!m_cache_write_stream)
    return;

  m_cache_write_stream->Commit();
  m_cache_write_stream->Release();
  m_cache_write_stream = nullptr;
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

void GameList::ScanDirectory(const char* path, bool recursive)
{
  Log_DevPrintf("Scanning %s%s", path, recursive ? " (recursively)" : "");

  FileSystem::FindResultsArray files;
  FileSystem::FindFiles(path, "*", FILESYSTEM_FIND_FILES | (recursive ? FILESYSTEM_FIND_RECURSIVE : 0), &files);

  GameListEntry entry;
  for (const FILESYSTEM_FIND_DATA& ffd : files)
  {
    // if this is a .bin, check if we have a .cue. if there is one, skip it
    const char* extension = std::strrchr(ffd.FileName, '.');
    if (extension && CASE_COMPARE(extension, ".bin") == 0)
    {
#if 0
      std::string temp(ffd.FileName, extension - ffd.FileName);
      temp += ".cue";
      if (std::any_of(files.begin(), files.end(),
                      [&temp](const FILESYSTEM_FIND_DATA& it) { return CASE_COMPARE(it.FileName, temp.c_str()) == 0; }))
      {
        Log_DebugPrintf("Skipping due to '%s' existing", temp.c_str());
        continue;
      }
#else
      continue;
#endif
    }

    std::string entry_path(ffd.FileName);
    if (std::any_of(m_entries.begin(), m_entries.end(),
                    [&entry_path](const GameListEntry& other) { return other.path == entry_path; }))
    {
      continue;
    }
    Log_DebugPrintf("Trying '%s'...", entry_path.c_str());

    // try opening the image
    if (!GetGameListEntryFromCache(entry_path, &entry) ||
        entry.last_modified_time != ffd.ModificationTime.AsUnixTimestamp())
    {
      if (GetGameListEntry(entry_path, &entry))
      {
        if (m_cache_write_stream || OpenCacheForWriting())
        {
          if (!WriteEntryToCache(&entry, m_cache_write_stream))
            Log_WarningPrintf("Failed to write entry '%s' to cache", entry.path.c_str());
        }
      }
      else
      {
        continue;
      }
    }

    m_entries.push_back(std::move(entry));
    entry = {};
  }
}

class RedumpDatVisitor final : public tinyxml2::XMLVisitor
{
public:
  RedumpDatVisitor(GameList::DatabaseMap& database) : m_database(database) {}

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
    if (CASE_COMPARE(element.Name(), "datafile") == 0)
      return true;

    if (CASE_COMPARE(element.Name(), "game") != 0)
      return false;

    const char* name = element.Attribute("name");
    if (!name)
      return false;

    const tinyxml2::XMLElement* serial_elem = element.FirstChildElement("serial");
    if (!serial_elem)
      return false;

    const char* serial_text = serial_elem->GetText();
    if (!serial_text)
      return false;

    // Handle entries like <serial>SCES-00984, SCES-00984#</serial>
    const char* start = serial_text;
    const char* end = std::strchr(start, ',');
    for (;;)
    {
      std::string code = FixupSerial(end ? std::string_view(start, end - start) : std::string_view(start));
      auto iter = m_database.find(code);
      if (iter == m_database.end())
      {
        GameList::GameDatabaseEntry gde;
        gde.code = std::move(code);
        gde.region = GameList::GetRegionForCode(gde.code).value_or(ConsoleRegion::NTSC_U);
        gde.title = name;
        m_database.emplace(gde.code, std::move(gde));
      }

      if (!end)
        break;

      start = end + 1;
      while (std::isspace(*start))
        start++;

      end = std::strchr(start, ',');
    }

    return false;
  }

private:
  GameList::DatabaseMap& m_database;
};

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

void GameList::SetPathsFromSettings(SettingsInterface& si)
{
  m_search_directories.clear();

  std::vector<std::string> dirs = si.GetStringList("GameList", "Paths");
  for (std::string& dir : dirs)
    m_search_directories.push_back({std::move(dir), false});

  dirs = si.GetStringList("GameList", "RecursivePaths");
  for (std::string& dir : dirs)
    m_search_directories.push_back({std::move(dir), true});

  m_database_filename = si.GetStringValue("GameList", "RedumpDatabasePath");
  m_cache_filename = si.GetStringValue("GameList", "CachePath");
}

void GameList::Refresh(bool invalidate_cache, bool invalidate_database)
{
  if (invalidate_cache)
    DeleteCacheFile();
  else
    LoadCache();

  if (invalidate_database)
    ClearDatabase();

  m_entries.clear();

  for (const DirectoryEntry& de : m_search_directories)
    ScanDirectory(de.path.c_str(), de.recursive);

  // don't need unused cache entries
  CloseCacheFileStream();
  m_cache_map.clear();
}

void GameList::LoadDatabase()
{
  if (m_database_load_tried)
    return;

  m_database_load_tried = true;
  if (m_database_filename.empty())
    return;

  tinyxml2::XMLDocument doc;
  tinyxml2::XMLError error = doc.LoadFile(m_database_filename.c_str());
  if (error != tinyxml2::XML_SUCCESS)
  {
    Log_ErrorPrintf("Failed to parse redump dat '%s': %s", m_database_filename.c_str(),
                    tinyxml2::XMLDocument::ErrorIDToName(error));
    return;
  }

  const tinyxml2::XMLElement* datafile_elem = doc.FirstChildElement("datafile");
  if (!datafile_elem)
  {
    Log_ErrorPrintf("Failed to get datafile element in '%s'", m_database_filename.c_str());
    return;
  }

  RedumpDatVisitor visitor(m_database);
  datafile_elem->Accept(&visitor);
  Log_InfoPrintf("Loaded %zu entries from Redump.org database '%s'", m_database.size(), m_database_filename.c_str());
}

void GameList::ClearDatabase()
{
  m_database.clear();
  m_database_load_tried = false;
}
