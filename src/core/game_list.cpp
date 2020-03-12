#include "game_list.h"
#include "bios.h"
#include "common/assert.h"
#include "common/byte_stream.h"
#include "common/cd_image.h"
#include "common/file_system.h"
#include "common/iso_reader.h"
#include "common/log.h"
#include "common/string_util.h"
#include "settings.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>
#include <tinyxml2.h>
#include <utility>
Log_SetChannel(GameList);

GameList::GameList() = default;

GameList::~GameList() = default;

const char* GameList::EntryTypeToString(GameListEntryType type)
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
                           [](const auto& it) { return StringUtil::Strcasecmp(it.first.c_str(), "boot") == 0; });
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

DiscRegion GameList::GetRegionForCode(std::string_view code)
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
    return DiscRegion::PAL;
  else if (prefix == "scps" || prefix == "slps" || prefix == "slpm")
    return DiscRegion::NTSC_J;
  else if (prefix == "scus" || prefix == "slus" || prefix == "papx")
    return DiscRegion::NTSC_U;
  else
    return DiscRegion::Other;
}

DiscRegion GameList::GetRegionFromSystemArea(CDImage* cdi)
{
  // The license code is on sector 4 of the disc.
  u8 sector[CDImage::DATA_SECTOR_SIZE];
  if (!cdi->Seek(1, 4) || cdi->Read(CDImage::ReadMode::DataOnly, 1, sector) != 1)
    return DiscRegion::Other;

  static constexpr char ntsc_u_string[] = "          Licensed  by          Sony Computer Entertainment Amer  ica ";
  static constexpr char ntsc_j_string[] = "          Licensed  by          Sony Computer Entertainment Inc.";
  static constexpr char pal_string[] = "          Licensed  by          Sony Computer Entertainment Euro pe";

  // subtract one for the terminating null
  if (std::equal(ntsc_u_string, ntsc_u_string + countof(ntsc_u_string) - 1, sector))
    return DiscRegion::NTSC_U;
  else if (std::equal(ntsc_j_string, ntsc_j_string + countof(ntsc_j_string) - 1, sector))
    return DiscRegion::NTSC_J;
  else if (std::equal(pal_string, pal_string + countof(pal_string) - 1, sector))
    return DiscRegion::PAL;
  else
    return DiscRegion::Other;
}

DiscRegion GameList::GetRegionForImage(CDImage* cdi)
{
  DiscRegion system_area_region = GetRegionFromSystemArea(cdi);
  if (system_area_region != DiscRegion::Other)
    return system_area_region;

  std::string code = GetGameCodeForImage(cdi);
  if (code.empty())
    return DiscRegion::Other;

  return GetRegionForCode(code);
}

std::optional<DiscRegion> GameList::GetRegionForPath(const char* image_path)
{
  std::unique_ptr<CDImage> cdi = CDImage::Open(image_path);
  if (!cdi)
    return {};

  return GetRegionForImage(cdi.get());
}

bool GameList::IsExeFileName(const char* path)
{
  const char* extension = std::strrchr(path, '.');
  return (extension &&
          (StringUtil::Strcasecmp(extension, ".exe") == 0 || StringUtil::Strcasecmp(extension, ".psexe") == 0));
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

std::string_view GameList::GetTitleForPath(const char* path)
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
  entry->region = DiscRegion::Other;
  entry->total_size = ZeroExtend64(file_size);
  entry->last_modified_time = ffd.ModificationTime.AsUnixTimestamp();
  entry->type = GameListEntryType::PSExe;

  return true;
}

bool GameList::GetGameListEntry(const std::string& path, GameListEntry* entry)
{
  if (IsExeFileName(path.c_str()))
    return GetExeListEntry(path.c_str(), entry);

  std::unique_ptr<CDImage> cdi = CDImage::Open(path.c_str());
  if (!cdi)
    return false;

  std::string code = GetGameCodeForImage(cdi.get());
  DiscRegion region = GetRegionFromSystemArea(cdi.get());
  if (region == DiscRegion::Other)
    region = GetRegionForCode(code);

  entry->path = path;
  entry->code = std::move(code);
  entry->region = region;
  entry->total_size = static_cast<u64>(CDImage::RAW_SECTOR_SIZE) * static_cast<u64>(cdi->GetLBACount());
  entry->type = GameListEntryType::Disc;
  cdi.reset();

  if (entry->code.empty())
  {
    // no game code, so use the filename title
    entry->title = GetTitleForPath(path.c_str());
  }
  else
  {
    const GameListDatabaseEntry* database_entry = GetDatabaseEntryForCode(entry->code);
    if (database_entry)
    {
      entry->title = database_entry->title;

      if (entry->region != database_entry->region)
        Log_WarningPrintf("Region mismatch between disc and database for '%s'", entry->code.c_str());
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
    std::string code;
    std::string title;
    u64 total_size;
    u64 last_modified_time;
    u8 region;
    u8 type;

    if (!ReadString(stream, &path) || !ReadString(stream, &code) || !ReadString(stream, &title) ||
        !ReadU64(stream, &total_size) || !ReadU64(stream, &last_modified_time) || !ReadU8(stream, &region) ||
        region >= static_cast<u8>(DiscRegion::Count) || !ReadU8(stream, &type) ||
        type > static_cast<u8>(GameListEntryType::PSExe))
    {
      Log_WarningPrintf("Game list cache entry is corrupted");
      return false;
    }

    GameListEntry ge;
    ge.path = path;
    ge.code = std::move(code);
    ge.title = std::move(title);
    ge.total_size = total_size;
    ge.last_modified_time = last_modified_time;
    ge.region = static_cast<DiscRegion>(region);
    ge.type = static_cast<GameListEntryType>(type);

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
  if (!m_cache_write_stream)
    return false;

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
  bool result = WriteString(stream, entry->path);
  result &= WriteString(stream, entry->code);
  result &= WriteString(stream, entry->title);
  result &= WriteU64(stream, entry->total_size);
  result &= WriteU64(stream, entry->last_modified_time);
  result &= WriteU8(stream, static_cast<u8>(entry->region));
  result &= WriteU8(stream, static_cast<u8>(entry->type));
  return result;
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
    const char* extension = std::strrchr(ffd.FileName.c_str(), '.');
    if (extension && StringUtil::Strcasecmp(extension, ".bin") == 0)
    {
#if 0
      std::string temp(ffd.FileName, extension - ffd.FileName);
      temp += ".cue";
      if (std::any_of(files.begin(), files.end(),
                      [&temp](const FILESYSTEM_FIND_DATA& it) { return StringUtil::Strcasecmp(it.FileName, temp.c_str()) == 0; }))
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
          if (!WriteEntryToCache(&entry, m_cache_write_stream.get()))
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

class GameList::RedumpDatVisitor final : public tinyxml2::XMLVisitor
{
public:
  RedumpDatVisitor(DatabaseMap& database) : m_database(database) {}

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
    if (StringUtil::Strcasecmp(element.Name(), "datafile") == 0)
      return true;

    if (StringUtil::Strcasecmp(element.Name(), "game") != 0)
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
        GameListDatabaseEntry gde;
        gde.code = std::move(code);
        gde.region = GameList::GetRegionForCode(gde.code);
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
  DatabaseMap& m_database;
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

const GameListDatabaseEntry* GameList::GetDatabaseEntryForCode(const std::string& code) const
{
  if (!m_database_load_tried)
    const_cast<GameList*>(this)->LoadDatabase();

  auto iter = m_database.find(code);
  return (iter != m_database.end()) ? &iter->second : nullptr;
}

void GameList::SetSearchDirectoriesFromSettings(SettingsInterface& si)
{
  m_search_directories.clear();

  std::vector<std::string> dirs = si.GetStringList("GameList", "Paths");
  for (std::string& dir : dirs)
    m_search_directories.push_back({std::move(dir), false});

  dirs = si.GetStringList("GameList", "RecursivePaths");
  for (std::string& dir : dirs)
    m_search_directories.push_back({std::move(dir), true});
}

bool GameList::IsDatabasePresent() const
{
  return FileSystem::FileExists(m_database_filename.c_str());
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
