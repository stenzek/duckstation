#pragma once
#include "cd_image.h"
#include "types.h"
#include <array>
#include <cstdio>
#include <unordered_map>

class CDSubChannelReplacement
{
public:
  enum : u32
  {
    SUBCHANNEL_Q_SIZE = 12,
  };

  using ReplacementData = std::array<u8, SUBCHANNEL_Q_SIZE>;

  CDSubChannelReplacement();
  ~CDSubChannelReplacement();

  u32 GetReplacementSectorCount() const { return static_cast<u32>(m_replacement_subq.size()); }

  bool LoadSBI(const char* path);

  /// Returns the replacement subchannel data for the specified position (in BCD).
  bool GetReplacementSubChannelQ(u8 minute_bcd, u8 second_bcd, u8 frame_bcd, ReplacementData& subq_data) const;

  /// Returns the replacement subchannel data for the specified sector.
  bool GetReplacementSubChannelQ(u32 lba, ReplacementData& subq_data) const;

private:
  using ReplacementMap = std::unordered_map<u32, ReplacementData>;

  ReplacementMap m_replacement_subq;
};
