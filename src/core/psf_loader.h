#pragma once
#include "types.h"
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace PSFLoader {

#pragma pack(push, 1)
struct PSFHeader
{
  u8 id[3];
  u8 version;
  u32 reserved_area_size;
  u32 compressed_program_size;
  u32 program_crc32;
};
#pragma pack(pop)

class File
{
public:
  using TagMap = std::map<std::string, std::string>;
  using ProgramData = std::vector<u8>;

  ALWAYS_INLINE const ProgramData& GetProgramData() const { return m_program_data; }
  ALWAYS_INLINE const TagMap& GetTagMap() const { return m_tags; }
  ALWAYS_INLINE DiscRegion GetRegion() const { return m_region; }

  std::optional<std::string> GetTagString(const char* tag_name) const;
  std::optional<int> GetTagInt(const char* tag_name) const;
  std::optional<float> GetTagFloat(const char* tag_name) const;

  std::string GetTagString(const char* tag_name, const char* default_value) const;
  int GetTagInt(const char* tag_name, int default_value) const;
  float GetTagFloat(const char* tag_name, float default_value) const;

  bool Load(const char* path);

private:
  enum : u32
  {
    MAX_PROGRAM_SIZE = 2 * 1024 * 1024
  };

  ProgramData m_program_data;
  TagMap m_tags;
  DiscRegion m_region = DiscRegion::Other;
};

bool Load(const char* path);

} // namespace PSFLoader