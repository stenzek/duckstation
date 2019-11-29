#pragma once
#include "types.h"
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

class CDImage;

class GameList
{
public:
  GameList();
  ~GameList();

  static std::string GetGameCodeForImage(CDImage* cdi);
  static std::string GetGameCodeForPath(const char* image_path);
  static std::optional<ConsoleRegion> GetRegionForCode(std::string_view code);

  void AddDirectory(const char* path, bool recursive);

private:
  struct GameListEntry
  {
    std::string path;
    std::string code;
    std::string title;
    ConsoleRegion region;
  };

  bool GetGameListEntry(const char* path, GameListEntry* entry);

  void ScanDirectory(const char* path, bool recursive);

  std::vector<GameListEntry> m_entries;
};
