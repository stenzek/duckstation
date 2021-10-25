#pragma once
#include "common/cd_image_hasher.h"
#include "core/types.h"
#include <map>
#include <string>
#include <string_view>
#include <vector>

class CDImage;

struct GameDatabaseEntry
{
  std::string serial;
  std::string title;
  std::string genre;
  std::string developer;
  std::string publisher;
  u64 release_date;
  u32 min_players;
  u32 max_players;
  u32 min_blocks;
  u32 max_blocks;
  u32 supported_controllers_mask;
};

class GameDatabase
{
public:
  GameDatabase();
  ~GameDatabase();

  bool Load();
  void Unload();

  bool GetEntryForDisc(CDImage* image, GameDatabaseEntry* entry) const;

  bool GetEntryForCode(const std::string_view& code, GameDatabaseEntry* entry) const;

  // Map of track hashes for image verification
  struct TrackData
  {
    TrackData(std::vector<std::string> codes, std::string revisionString, uint32_t revision)
      : codes(codes), revisionString(revisionString), revision(revision)
    {
    }

    friend bool operator==(const TrackData& left, const TrackData& right)
    {
      // 'revisionString' is deliberately ignored in comparisons as it's redundant with comparing 'revision'! Do not
      // change!
      return left.codes == right.codes && left.revision == right.revision;
    }

    std::vector<std::string> codes;
    std::string revisionString;
    uint32_t revision;
  };
  using TrackHashesMap = std::multimap<CDImageHasher::Hash, TrackData>;
  TrackHashesMap GetTrackHashesMap() const;

private:
  void* m_json = nullptr;
};
