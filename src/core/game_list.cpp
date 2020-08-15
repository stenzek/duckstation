#include "game_list.h"
#include "bios.h"
#include "common/assert.h"
#include "common/byte_stream.h"
#include "common/cd_image.h"
#include "common/file_system.h"
#include "common/iso_reader.h"
#include "common/log.h"
#include "common/progress_callback.h"
#include "common/string_util.h"
#include "settings.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
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

const char* GameList::EntryCompatibilityRatingToString(GameListCompatibilityRating rating)
{
  static std::array<const char*, static_cast<int>(GameListCompatibilityRating::Count)> names = {
    {"Unknown", "DoesntBoot", "CrashesInIntro", "CrashesInGame", "GraphicalAudioIssues", "NoIssues"}};
  return names[static_cast<int>(rating)];
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
    else if (ch == ' ' || (ch >= 0x09 && ch <= 0x0D))
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
  else if (prefix == "scps" || prefix == "slps" || prefix == "slpm" || prefix == "sczs" || prefix == "papx")
    return DiscRegion::NTSC_J;
  else if (prefix == "scus" || prefix == "slus")
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

bool GameList::IsPsfFileName(const char* path)
{
  const char* extension = std::strrchr(path, '.');
  return (extension && StringUtil::Strcasecmp(extension, ".psf") == 0);
}

bool GameList::IsM3UFileName(const char* path)
{
  const char* extension = std::strrchr(path, '.');
  return (extension && StringUtil::Strcasecmp(extension, ".m3u") == 0);
}

std::vector<std::string> GameList::ParseM3UFile(const char* path)
{
  std::ifstream ifs(path);
  if (!ifs.is_open())
  {
    Log_ErrorPrintf("Failed to open %s", path);
    return {};
  }

  std::vector<std::string> entries;
  std::string line;
  while (std::getline(ifs, line))
  {
    u32 start_offset = 0;
    while (start_offset < line.size() && std::isspace(line[start_offset]))
      start_offset++;

    // skip comments
    if (start_offset == line.size() || line[start_offset] == '#')
      continue;

    // strip ending whitespace
    u32 end_offset = static_cast<u32>(line.size()) - 1;
    while (std::isspace(line[end_offset]) && end_offset > start_offset)
      end_offset--;

    // anything?
    if (start_offset == end_offset)
      continue;

    std::string entry_path(line.begin() + start_offset, line.begin() + end_offset + 1);
    if (!FileSystem::IsAbsolutePath(entry_path))
    {
      SmallString absolute_path;
      FileSystem::BuildPathRelativeToFile(absolute_path, path, entry_path.c_str());
      entry_path = absolute_path;
    }

    Log_DevPrintf("Read path from m3u: '%s'", entry_path.c_str());
    entries.push_back(std::move(entry_path));
  }

  Log_InfoPrintf("Loaded %zu paths from m3u '%s'", entries.size(), path);
  return entries;
}

const char* GameList::GetGameListCompatibilityRatingString(GameListCompatibilityRating rating)
{
  static constexpr std::array<const char*, static_cast<size_t>(GameListCompatibilityRating::Count)> names = {
    {"Unknown", "Doesn't Boot", "Crashes In Intro", "Crashes In-Game", "Graphical/Audio Issues", "No Issues"}};
  return (rating >= GameListCompatibilityRating::Unknown && rating < GameListCompatibilityRating::Count) ?
           names[static_cast<int>(rating)] :
           "";
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

  std::FILE* fp = FileSystem::OpenCFile(path, "rb");
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
  entry->compatibility_rating = GameListCompatibilityRating::Unknown;

  return true;
}

bool GameList::GetM3UListEntry(const char* path, GameListEntry* entry)
{
  FILESYSTEM_STAT_DATA ffd;
  if (!FileSystem::StatFile(path, &ffd))
    return false;

  std::vector<std::string> entries = ParseM3UFile(path);
  if (entries.empty())
    return false;

  entry->code.clear();
  entry->title = GetTitleForPath(path);
  entry->path = path;
  entry->region = DiscRegion::Other;
  entry->total_size = 0;
  entry->last_modified_time = ffd.ModificationTime.AsUnixTimestamp();
  entry->type = GameListEntryType::Playlist;
  entry->compatibility_rating = GameListCompatibilityRating::Unknown;

  for (size_t i = 0; i < entries.size(); i++)
  {
    std::unique_ptr<CDImage> entry_image = CDImage::Open(entries[i].c_str());
    if (!entry_image)
    {
      Log_ErrorPrintf("Failed to open entry %zu ('%s') in playlist %s", i, entries[i].c_str(), path);
      return false;
    }

    entry->total_size += static_cast<u64>(CDImage::RAW_SECTOR_SIZE) * static_cast<u64>(entry_image->GetLBACount());

    if (entry->region == DiscRegion::Other)
      entry->region = GetRegionForImage(entry_image.get());

    if (entry->compatibility_rating == GameListCompatibilityRating::Unknown)
    {
      std::string code = GetGameCodeForImage(entry_image.get());
      const GameListCompatibilityEntry* compatibility_entry = GetCompatibilityEntryForCode(entry->code);
      if (compatibility_entry)
        entry->compatibility_rating = compatibility_entry->compatibility_rating;
      else
        Log_WarningPrintf("'%s' (%s) not found in compatibility list", entry->code.c_str(), entry->title.c_str());
    }
  }

  return true;
}

bool GameList::GetGameListEntry(const std::string& path, GameListEntry* entry)
{
  if (IsExeFileName(path.c_str()))
    return GetExeListEntry(path.c_str(), entry);
  if (IsM3UFileName(path.c_str()))
    return GetM3UListEntry(path.c_str(), entry);

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
  entry->compatibility_rating = GameListCompatibilityRating::Unknown;
  cdi.reset();

  if (entry->code.empty())
  {
    // no game code, so use the filename title
    entry->title = GetTitleForPath(path.c_str());
    entry->compatibility_rating = GameListCompatibilityRating::Unknown;
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

    const GameListCompatibilityEntry* compatibility_entry = GetCompatibilityEntryForCode(entry->code);
    if (compatibility_entry)
      entry->compatibility_rating = compatibility_entry->compatibility_rating;
    else
      Log_WarningPrintf("'%s' (%s) not found in compatibility list", entry->code.c_str(), entry->title.c_str());
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
    u8 compatibility_rating;

    if (!ReadString(stream, &path) || !ReadString(stream, &code) || !ReadString(stream, &title) ||
        !ReadU64(stream, &total_size) || !ReadU64(stream, &last_modified_time) || !ReadU8(stream, &region) ||
        region >= static_cast<u8>(DiscRegion::Count) || !ReadU8(stream, &type) ||
        type > static_cast<u8>(GameListEntryType::Playlist) || !ReadU8(stream, &compatibility_rating) ||
        compatibility_rating >= static_cast<u8>(GameListCompatibilityRating::Count))
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
    ge.compatibility_rating = static_cast<GameListCompatibilityRating>(compatibility_rating);

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
  result &= WriteU8(stream, static_cast<u8>(entry->compatibility_rating));
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
  FileSystem::FindFiles(path, "*", FILESYSTEM_FIND_FILES | (recursive ? FILESYSTEM_FIND_RECURSIVE : 0), &files);

  GameListEntry entry;
  progress->SetProgressRange(static_cast<u32>(files.size()));
  progress->SetProgressValue(0);

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
      const char* file_part_slash =
        std::max(std::strrchr(entry_path.c_str(), '/'), std::strrchr(entry_path.c_str(), '\\'));
      progress->SetFormattedStatusText("Scanning '%s'...",
                                       file_part_slash ? (file_part_slash + 1) : entry_path.c_str());
      progress->IncrementProgressValue();

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

  FlushCacheFileStream();

  progress->SetProgressValue(static_cast<u32>(files.size()));
  progress->PopState();
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

const GameListCompatibilityEntry* GameList::GetCompatibilityEntryForCode(const std::string& code) const
{
  if (!m_compatibility_list_load_tried)
    const_cast<GameList*>(this)->LoadCompatibilityList();

  auto iter = m_compatibility_list.find(code);
  return (iter != m_compatibility_list.end()) ? &iter->second : nullptr;
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

void GameList::Refresh(bool invalidate_cache, bool invalidate_database, ProgressCallback* progress /* = nullptr */)
{
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

    for (DirectoryEntry& de : m_search_directories)
    {
      ScanDirectory(de.path.c_str(), de.recursive, progress);
      progress->IncrementProgressValue();
    }
  }

  // don't need unused cache entries
  CloseCacheFileStream();
  m_cache_map.clear();
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
      Log_ErrorPrintf("Duplicate game code in compatibility list: '%s'", code.c_str());
      return false;
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
  if (m_compatibility_list_filename.empty())
    return;

  tinyxml2::XMLDocument doc;
  tinyxml2::XMLError error = doc.LoadFile(m_compatibility_list_filename.c_str());
  if (error != tinyxml2::XML_SUCCESS)
  {
    Log_ErrorPrintf("Failed to parse compatibility list '%s': %s", m_compatibility_list_filename.c_str(),
                    tinyxml2::XMLDocument::ErrorIDToName(error));
    return;
  }

  const tinyxml2::XMLElement* datafile_elem = doc.FirstChildElement("compatibility-list");
  if (!datafile_elem)
  {
    Log_ErrorPrintf("Failed to get compatibility-list element in '%s'", m_compatibility_list_filename.c_str());
    return;
  }

  CompatibilityListVisitor visitor(m_compatibility_list);
  datafile_elem->Accept(&visitor);
  Log_InfoPrintf("Loaded %zu entries from compatibility list '%s'", m_compatibility_list.size(),
                 m_compatibility_list_filename.c_str());
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

bool GameList::SaveCompatibilityDatabase()
{
  if (m_compatibility_list_filename.empty())
    return false;

  tinyxml2::XMLDocument doc;
  tinyxml2::XMLElement* root_elem = doc.NewElement("compatibility-list");
  doc.InsertEndChild(root_elem);

  for (const auto& it : m_compatibility_list)
  {
    const GameListCompatibilityEntry* entry = &it.second;
    tinyxml2::XMLElement* entry_elem = doc.NewElement("entry");
    root_elem->InsertEndChild(entry_elem);
    InitElementForCompatibilityEntry(&doc, entry_elem, entry);
  }

  tinyxml2::XMLError error = doc.SaveFile(m_compatibility_list_filename.c_str());
  if (error != tinyxml2::XML_SUCCESS)
  {
    Log_ErrorPrintf("Failed to save compatibility list '%s': %s", m_compatibility_list_filename.c_str(),
                    tinyxml2::XMLDocument::ErrorIDToName(error));
    return false;
  }

  Log_InfoPrintf("Saved %zu entries to compatibility list '%s'", m_compatibility_list.size(),
                 m_compatibility_list_filename.c_str());
  return true;
}

bool GameList::SaveCompatibilityDatabaseForEntry(const GameListCompatibilityEntry* entry)
{
  if (m_compatibility_list_filename.empty())
    return false;

  if (!FileSystem::FileExists(m_compatibility_list_filename.c_str()))
    return SaveCompatibilityDatabase();

  tinyxml2::XMLDocument doc;
  tinyxml2::XMLError error = doc.LoadFile(m_compatibility_list_filename.c_str());
  if (error != tinyxml2::XML_SUCCESS)
  {
    Log_ErrorPrintf("Failed to parse compatibility list '%s': %s", m_compatibility_list_filename.c_str(),
                    tinyxml2::XMLDocument::ErrorIDToName(error));
    return false;
  }

  tinyxml2::XMLElement* root_elem = doc.FirstChildElement("compatibility-list");
  if (!root_elem)
  {
    Log_ErrorPrintf("Failed to get compatibility-list element in '%s'", m_compatibility_list_filename.c_str());
    return false;
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

  error = doc.SaveFile(m_compatibility_list_filename.c_str());
  if (error != tinyxml2::XML_SUCCESS)
  {
    Log_ErrorPrintf("Failed to update compatibility list '%s': %s", m_compatibility_list_filename.c_str(),
                    tinyxml2::XMLDocument::ErrorIDToName(error));
    return false;
  }

  Log_InfoPrintf("Updated compatibility list '%s'", m_compatibility_list_filename.c_str());
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
