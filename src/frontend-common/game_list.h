#pragma once
#include "core/types.h"
#include "game_settings.h"
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class CDImage;
class ByteStream;
class ProgressCallback;

class SettingsInterface;

enum class GameListEntryType
{
  Disc,
  PSExe,
  Playlist,
  PSF,
  Count
};

enum class GameListCompatibilityRating
{
  Unknown = 0,
  DoesntBoot = 1,
  CrashesInIntro = 2,
  CrashesInGame = 3,
  GraphicalAudioIssues = 4,
  NoIssues = 5,
  Count,
};

struct GameListDatabaseEntry
{
  std::string code;
  std::string title;
  DiscRegion region;
};

struct GameListEntry
{
  std::string path;
  std::string code;
  std::string title;
  u64 total_size;
  u64 last_modified_time;
  DiscRegion region;
  GameListEntryType type;
  GameListCompatibilityRating compatibility_rating;
  GameSettings::Entry settings;
};

struct GameListCompatibilityEntry
{
  std::string code;
  std::string title;
  std::string version_tested;
  std::string upscaling_issues;
  std::string comments;
  DiscRegion region;
  GameListCompatibilityRating compatibility_rating;
};

class GameList
{
public:
  using EntryList = std::vector<GameListEntry>;

  struct DirectoryEntry
  {
    std::string path;
    bool recursive;
  };

  GameList();
  ~GameList();

  /// Returns true if the filename is a PlayStation executable we can inject.
  static bool IsExeFileName(const char* path);

  /// Returns true if the filename is a Portable Sound Format file we can uncompress/load.
  static bool IsPsfFileName(const char* path);

  /// Returns true if the filename is a M3U Playlist we can handle.
  static bool IsM3UFileName(const char* path);

  static const char* EntryTypeToString(GameListEntryType type);
  static const char* EntryCompatibilityRatingToString(GameListCompatibilityRating rating);

  /// Returns a string representation of a compatibility level.
  static const char* GetGameListCompatibilityRatingString(GameListCompatibilityRating rating);

  static bool IsScannableFilename(const std::string& path);

  const EntryList& GetEntries() const { return m_entries; }
  const u32 GetEntryCount() const { return static_cast<u32>(m_entries.size()); }
  const std::vector<DirectoryEntry>& GetSearchDirectories() const { return m_search_directories; }
  const u32 GetSearchDirectoryCount() const { return static_cast<u32>(m_search_directories.size()); }

  const GameListEntry* GetEntryForPath(const char* path) const;
  const GameListDatabaseEntry* GetDatabaseEntryForCode(const std::string& code) const;
  const GameListCompatibilityEntry* GetCompatibilityEntryForCode(const std::string& code) const;

  void SetCacheFilename(std::string filename) { m_cache_filename = std::move(filename); }
  void SetUserDatabaseFilename(std::string filename) { m_user_database_filename = std::move(filename); }
  void SetUserCompatibilityListFilename(std::string filename)
  {
    m_user_compatibility_list_filename = std::move(filename);
  }
  void SetUserGameSettingsFilename(std::string filename) { m_user_game_settings_filename = std::move(filename); }
  void SetSearchDirectoriesFromSettings(SettingsInterface& si);

  void AddDirectory(std::string path, bool recursive);
  void Refresh(bool invalidate_cache, bool invalidate_database, ProgressCallback* progress = nullptr);

  void UpdateCompatibilityEntry(GameListCompatibilityEntry new_entry, bool save_to_list = true);

  static std::string ExportCompatibilityEntry(const GameListCompatibilityEntry* entry);

  const GameSettings::Entry* GetGameSettings(const std::string& filename, const std::string& game_code);
  void UpdateGameSettings(const std::string& filename, const std::string& game_code, const std::string& game_title,
                          const GameSettings::Entry& new_entry, bool save_to_list = true);

  std::string GetCoverImagePathForEntry(const GameListEntry* entry) const;
  std::string GetNewCoverImagePathForEntry(const GameListEntry* entry, const char* new_filename) const;

private:
  enum : u32
  {
    GAME_LIST_CACHE_SIGNATURE = 0x45434C47,
    GAME_LIST_CACHE_VERSION = 24
  };

  using DatabaseMap = std::unordered_map<std::string, GameListDatabaseEntry>;
  using CacheMap = std::unordered_map<std::string, GameListEntry>;
  using CompatibilityMap = std::unordered_map<std::string, GameListCompatibilityEntry>;

  class RedumpDatVisitor;
  class CompatibilityListVisitor;

  GameListEntry* GetMutableEntryForPath(const char* path);

  static bool GetExeListEntry(const char* path, GameListEntry* entry);
  static bool GetPsfListEntry(const char* path, GameListEntry* entry);
  bool GetM3UListEntry(const char* path, GameListEntry* entry);

  bool GetGameListEntry(const std::string& path, GameListEntry* entry);
  bool GetGameListEntryFromCache(const std::string& path, GameListEntry* entry);
  void ScanDirectory(const char* path, bool recursive, ProgressCallback* progress);

  void LoadCache();
  bool LoadEntriesFromCache(ByteStream* stream);
  bool OpenCacheForWriting();
  bool WriteEntryToCache(const GameListEntry* entry, ByteStream* stream);
  void FlushCacheFileStream();
  void CloseCacheFileStream();
  void RewriteCacheFile();
  void DeleteCacheFile();

  void LoadDatabase();
  void ClearDatabase();

  void LoadCompatibilityList();
  bool LoadCompatibilityListFromXML(const std::string& xml);
  bool SaveCompatibilityDatabaseForEntry(const GameListCompatibilityEntry* entry);

  void LoadGameSettings();

  DatabaseMap m_database;
  EntryList m_entries;
  CacheMap m_cache_map;
  CompatibilityMap m_compatibility_list;
  GameSettings::Database m_game_settings;
  std::unique_ptr<ByteStream> m_cache_write_stream;

  std::vector<DirectoryEntry> m_search_directories;
  std::string m_cache_filename;
  std::string m_user_database_filename;
  std::string m_user_compatibility_list_filename;
  std::string m_user_game_settings_filename;
  bool m_database_load_tried = false;
  bool m_compatibility_list_load_tried = false;
  bool m_game_settings_load_tried = false;
};
