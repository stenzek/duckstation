// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once
#include "types.h"

class MD5Digest
{
public:
  MD5Digest();

  void Update(const void* pData, u32 cbData);
  void Final(u8 Digest[16]);
  void Reset();

private:
  u32 buf[4];
  u32 bits[2];
  u8 in[64];
};
