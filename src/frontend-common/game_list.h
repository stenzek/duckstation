#pragma once
#include "core/types.h"
#include "game_database.h"
#include "game_settings.h"
#include "util/cd_image.h"
#include <ctime>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

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

struct GameListEntry
{
  GameListEntryType type = GameListEntryType::Disc;
  DiscRegion region = DiscRegion::Other;

  std::string path;
  std::string code;
  std::string title;
  std::string genre;
  std::string publisher;
  std::string developer;
  u64 total_size = 0;
  u64 last_modified_time = 0;

  u64 release_date = 0;
  u32 supported_controllers = ~static_cast<u32>(0);
  u8 min_players = 1;
  u8 max_players = 1;
  u8 min_blocks = 0;
  u8 max_blocks = 0;

  GameListCompatibilityRating compatibility_rating = GameListCompatibilityRating::Unknown;
  GameSettings::Entry settings;

  size_t GetReleaseDateString(char* buffer, size_t buffer_size) const;
};

struct GameListCompatibilityEntry
{
  std::string code;
  std::string title;
  std::string version_tested;
  std::string upscaling_issues;
  std::string comments;
  DiscRegion region = DiscRegion::Other;
  GameListCompatibilityRating compatibility_rating = GameListCompatibilityRating::Unknown;
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

  static const char* EntryTypeToString(GameListEntryType type);
  static const char* EntryCompatibilityRatingToString(GameListCompatibilityRating rating);

  /// Returns a string representation of a compatibility level.
  static const char* GetGameListCompatibilityRatingString(GameListCompatibilityRating rating);

  static bool IsScannableFilename(const std::string& path);

  const EntryList& GetEntries() const { return m_entries; }
  const u32 GetEntryCount() const { return static_cast<u32>(m_entries.size()); }
  const std::vector<DirectoryEntry>& GetSearchDirectories() const { return m_search_directories; }
  const u32 GetSearchDirectoryCount() const { return static_cast<u32>(m_search_directories.size()); }
  const bool IsGameListLoaded() const { return m_game_list_loaded; }

  const GameListEntry* GetEntryForPath(const char* path) const;
  const GameListCompatibilityEntry* GetCompatibilityEntryForCode(const std::string& code) const;
  bool GetDatabaseEntryForCode(const std::string_view& code, GameDatabaseEntry* entry);
  bool GetDatabaseEntryForDisc(CDImage* image, GameDatabaseEntry* entry);
  bool IsPathExcluded(const std::string& path) const;

  void SetCacheFilename(std::string filename) { m_cache_filename = std::move(filename); }
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
  const GameSettings::Entry* GetGameSettingsForCode(const std::string& game_code);
  void UpdateGameSettings(const std::string& filename, const std::string& game_code, const std::string& game_title,
                          const GameSettings::Entry& new_entry, bool save_to_list = true);

  std::string GetCoverImagePathForEntry(const GameListEntry* entry) const;
  std::string GetCoverImagePath(const std::string& path, const std::string& code, const std::string& title) const;
  std::string GetNewCoverImagePathForEntry(const GameListEntry* entry, const char* new_filename) const;

private:
  enum : u32
  {
    GAME_LIST_CACHE_SIGNATURE = 0x45434C47,
    GAME_LIST_CACHE_VERSION = 31
  };

  using CacheMap = std::unordered_map<std::string, GameListEntry>;
  using CompatibilityMap = std::unordered_map<std::string, GameListCompatibilityEntry>;

  class RedumpDatVisitor;
  class CompatibilityListVisitor;

  GameListEntry* GetMutableEntryForPath(const char* path);

  static bool GetExeListEntry(const std::string& path, GameListEntry* entry);
  static bool GetPsfListEntry(const std::string& path, GameListEntry* entry);

  bool GetGameListEntry(const std::string& path, GameListEntry* entry);
  bool GetGameListEntryFromCache(const std::string& path, GameListEntry* entry);
  void ScanDirectory(const char* path, bool recursive, ProgressCallback* progress);
  bool AddFileFromCache(const std::string& path, std::time_t timestamp);
  bool ScanFile(std::string path, std::time_t timestamp);

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

  EntryList m_entries;
  CacheMap m_cache_map;
  GameDatabase m_database;
  CompatibilityMap m_compatibility_list;
  GameSettings::Database m_game_settings;
  std::unique_ptr<ByteStream> m_cache_write_stream;

  std::vector<DirectoryEntry> m_search_directories;
  std::vector<std::string> m_excluded_paths;
  std::string m_cache_filename;
  std::string m_user_compatibility_list_filename;
  std::string m_user_game_settings_filename;
  bool m_database_load_tried = false;
  bool m_compatibility_list_load_tried = false;
  bool m_game_settings_load_tried = false;
  bool m_game_list_loaded = false;
};
