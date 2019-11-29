#include "game_list.h"
#include "YBaseLib/CString.h"
#include "YBaseLib/FileSystem.h"
#include "YBaseLib/Log.h"
#include "common/cd_image.h"
#include "common/iso_reader.h"
#include <algorithm>
#include <cctype>
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
  if (pos == std::string::npos)
    return {};
  code.erase(0, pos + 1);
  pos = code.find(';');
  if (pos == std::string::npos)
    return {};

  code.erase(pos);

  // SCES_123.45 -> SCES-12345
  for (pos = 0; pos < code.size();)
  {
    if (code[pos] == '_')
      code[pos++] = '-';
    else if (code[pos] == '.')
      code.erase(pos, 1);
    else
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

  std::string game_code = GetGameCodeForImage(cdi.get());

  entry->path = path;
  entry->title = game_code;
  entry->code = game_code;
  entry->region = GetRegionForCode(game_code).value_or(ConsoleRegion::NTSC_U);
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
