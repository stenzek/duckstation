#include "game_list.h"
#include "YBaseLib/CString.h"
#include "YBaseLib/FileSystem.h"
#include "YBaseLib/Log.h"
#include "common/cd_image.h"
#include "common/iso_reader.h"
#include <algorithm>
#include <cctype>
#include <tinyxml2.h>
#include <utility>
Log_SetChannel(GameList);

GameList::GameList() = default;

GameList::~GameList() = default;

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
  auto iter =
    std::find_if(lines.begin(), lines.end(), [](const auto& it) { return Y_stricmp(it.first.c_str(), "boot") == 0; });
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

  // TODO: PAPX?

  if (prefix == "sces" || prefix == "sced" || prefix == "sles" || prefix == "sled")
    return ConsoleRegion::PAL;
  else if (prefix == "scps" || prefix == "scpd" || prefix == "slps" || prefix == "slpd")
    return ConsoleRegion::NTSC_J;
  else if (prefix == "scus" || prefix == "slus")
    return ConsoleRegion::NTSC_U;
  else
    return std::nullopt;
}

void GameList::AddDirectory(const char* path, bool recursive)
{
  ScanDirectory(path, recursive);
}

bool GameList::GetGameListEntry(const char* path, GameListEntry* entry)
{
  std::unique_ptr<CDImage> cdi = CDImage::Open(path);
  if (!cdi)
    return false;

  entry->path = path;
  entry->code = GetGameCodeForImage(cdi.get());
  entry->total_size = static_cast<u64>(CDImage::RAW_SECTOR_SIZE) * static_cast<u64>(cdi->GetLBACount());
  cdi.reset();

  auto iter = m_database.find(entry->code);
  if (iter != m_database.end())
  {
    entry->title = iter->second.title;
    entry->region = iter->second.region;
  }
  else
  {
    Log_WarningPrintf("'%s' not found in database", entry->code.c_str());
    entry->title = entry->code;
    entry->region = GetRegionForCode(entry->code).value_or(ConsoleRegion::NTSC_U);
  }

  return true;
}

void GameList::ScanDirectory(const char* path, bool recursive)
{
  Log_DevPrintf("Scanning %s%s", path, recursive ? " (recursively)" : "");

  FileSystem::FindResultsArray files;
  FileSystem::FindFiles(path, "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_RECURSIVE, &files);

  GameListEntry entry;
  for (const FILESYSTEM_FIND_DATA& ffd : files)
  {
    Log_DebugPrintf("Trying '%s'...", ffd.FileName);

    // if this is a .bin, check if we have a .cue. if there is one, skip it
    const char* extension = std::strrchr(ffd.FileName, '.');
    if (extension && Y_stricmp(extension, ".bin") == 0)
    {
#if 0
      std::string temp(ffd.FileName, extension - ffd.FileName);
      temp += ".cue";
      if (std::any_of(files.begin(), files.end(),
                      [&temp](const FILESYSTEM_FIND_DATA& it) { return Y_stricmp(it.FileName, temp.c_str()) == 0; }))
      {
        Log_DebugPrintf("Skipping due to '%s' existing", temp.c_str());
        continue;
      }
#else
      continue;
#endif
    }

    // try opening the image
    if (GetGameListEntry(ffd.FileName, &entry))
    {
      m_entries.push_back(std::move(entry));
      entry = {};
    }
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
    if (Y_stricmp(element.Name(), "datafile") == 0)
      return true;

    if (Y_stricmp(element.Name(), "game") != 0)
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

bool GameList::ParseRedumpDatabase(const char* redump_dat_path)
{
  tinyxml2::XMLDocument doc;
  tinyxml2::XMLError error = doc.LoadFile(redump_dat_path);
  if (error != tinyxml2::XML_SUCCESS)
  {
    Log_ErrorPrintf("Failed to parse redump dat '%s': %s", redump_dat_path,
                    tinyxml2::XMLDocument::ErrorIDToName(error));
    return false;
  }

  const tinyxml2::XMLElement* datafile_elem = doc.FirstChildElement("datafile");
  if (!datafile_elem)
  {
    Log_ErrorPrintf("Failed to get datafile element in '%s'", redump_dat_path);
    return false;
  }

  RedumpDatVisitor visitor(m_database);
  datafile_elem->Accept(&visitor);
  Log_InfoPrintf("Loaded %zu entries from Redump.org database '%s'", m_database.size(), redump_dat_path);
  return true;
}
