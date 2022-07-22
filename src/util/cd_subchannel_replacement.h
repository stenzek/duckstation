#pragma once
#include "cd_image.h"
#include "common/types.h"
#include <array>
#include <cstdio>
#include <unordered_map>

class CDSubChannelReplacement
{
public:
  CDSubChannelReplacement();
  ~CDSubChannelReplacement();

  u32 GetReplacementSectorCount() const { return static_cast<u32>(m_replacement_subq.size()); }

  bool LoadSBI(const char* path);
  bool LoadSBIFromImagePath(const char* image_path);

  /// Adds a sector to the replacement map.
  void AddReplacementSubChannelQ(u32 lba, const CDImage::SubChannelQ& subq);

  /// Returns the replacement subchannel data for the specified position (in BCD).
  bool GetReplacementSubChannelQ(u8 minute_bcd, u8 second_bcd, u8 frame_bcd, CDImage::SubChannelQ* subq) const;

  /// Returns the replacement subchannel data for the specified sector.
  bool GetReplacementSubChannelQ(u32 lba, CDImage::SubChannelQ* subq) const;

private:
  using ReplacementMap = std::unordered_map<u32, CDImage::SubChannelQ>;

  ReplacementMap m_replacement_subq;
};
