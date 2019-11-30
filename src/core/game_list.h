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
  struct GameDatabaseEntry
  {
    std::string code;
    std::string title;
    ConsoleRegion region;
  };

  using DatabaseMap = std::unordered_map<std::string, GameDatabaseEntry>;

  struct GameListEntry
  {
    std::string path;
    std::string code;
    std::string title;
    ConsoleRegion region;
  };

  using EntryList = std::vector<GameListEntry>;

  GameList();
  ~GameList();

  static std::string GetGameCodeForImage(CDImage* cdi);
  static std::string GetGameCodeForPath(const char* image_path);
  static std::optional<ConsoleRegion> GetRegionForCode(std::string_view code);

  const DatabaseMap& GetDatabase() const { return m_database; }
  const EntryList& GetEntries() const { return m_entries; }

  void AddDirectory(const char* path, bool recursive);

  bool ParseRedumpDatabase(const char* redump_dat_path);

private:

  bool GetGameListEntry(const char* path, GameListEntry* entry);

  void ScanDirectory(const char* path, bool recursive);

  DatabaseMap m_database;
  EntryList m_entries;
};
