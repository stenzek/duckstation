#pragma once
#include "types.h"
#include <string>

class SHA1Digest
{
public:
  enum : u32
  {
    DIGEST_SIZE = 20
  };

  SHA1Digest();

  void Update(const void* data, u32 len);
  void Final(u8 digest[DIGEST_SIZE]);
  void Reset();

  static std::string DigestToString(const u8 digest[DIGEST_SIZE]);

private:
  u32 state[5];
  u32 count[2];
  u8 buffer[64];
};
