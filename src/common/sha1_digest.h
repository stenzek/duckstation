// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "types.h"

#include <array>
#include <string>
#include <span>

class SHA1Digest
{
public:
  enum : u32
  {
    DIGEST_SIZE = 20
  };

  SHA1Digest();

  void Update(const void* data, size_t len);
  void Update(std::span<const u8> data);
  void Final(u8 digest[DIGEST_SIZE]);
  void Reset();

  static std::string DigestToString(const std::span<u8, DIGEST_SIZE> digest);

  static std::array<u8, DIGEST_SIZE> GetDigest(const void* data, size_t len);
  static std::array<u8, DIGEST_SIZE> GetDigest(std::span<const u8> data);

private:
  u32 state[5];
  u32 count[2];
  u8 buffer[64];
};
