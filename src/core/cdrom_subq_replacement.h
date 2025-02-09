// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "util/cd_image.h"

#include <cstdio>
#include <unordered_map>

class CDROMSubQReplacement
{
public:
  CDROMSubQReplacement();
  ~CDROMSubQReplacement();

  // NOTE: Can return true if no sbi is available, false means load/parse error.
  static bool LoadForImage(std::unique_ptr<CDROMSubQReplacement>* ret, CDImage* image, std::string_view serial,
                           std::string_view title, Error* error);

  size_t GetReplacementSectorCount() const { return m_replacement_subq.size(); }

  /// Returns the replacement subchannel data for the specified sector.
  const CDImage::SubChannelQ* GetReplacementSubQ(u32 lba) const;

private:
  using ReplacementMap = std::unordered_map<u32, CDImage::SubChannelQ>;

  static std::unique_ptr<CDROMSubQReplacement> LoadSBI(const std::string& path, std::FILE* fp, Error* error);
  static std::unique_ptr<CDROMSubQReplacement> LoadLSD(const std::string& path, std::FILE* fp, Error* error);

  ReplacementMap m_replacement_subq;
};
