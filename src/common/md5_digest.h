#pragma once
#include "types.h"

// based heavily on this implementation:
// http://www.fourmilab.ch/md5/

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
