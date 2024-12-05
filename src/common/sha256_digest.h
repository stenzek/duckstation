// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "types.h"

#include <array>
#include <span>
#include <string>

class SHA256Digest
{
public:
  enum : u32
  {
    DIGEST_SIZE = 32,
    BLOCK_SIZE = 64,
  };

  using Digest = std::array<u8, DIGEST_SIZE>;

  SHA256Digest();

  void Update(const void* data, size_t len);
  void Update(std::span<const u8> data);
  void Final(std::span<u8, DIGEST_SIZE> digest);
  Digest Final();
  void Reset();

  static std::string DigestToString(const std::span<const u8, DIGEST_SIZE> digest);

  static Digest GetDigest(const void* data, size_t len);
  static Digest GetDigest(std::span<const u8> data);

private:
  void TransformBlock();

  u64 m_bit_length = 0;
  std::array<u32, 8> m_state = {};
  u32 m_block_length = 0;
  std::array<u8, BLOCK_SIZE> m_block = {};
};
