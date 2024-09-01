// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "types.h"

#include <array>
#include <span>

class MD5Digest
{
public:
  static constexpr u32 DIGEST_SIZE = 16;

  MD5Digest();

  void Update(const void* pData, u32 cbData);
  void Update(std::span<const u8> data);
  void Final(std::span<u8, DIGEST_SIZE> digest);
  void Reset();

  static std::array<u8, DIGEST_SIZE> HashData(std::span<const u8> data);

private:
  u32 buf[4];
  u32 bits[2];
  u8 in[64];
};
