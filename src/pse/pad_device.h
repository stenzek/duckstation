#pragma once
#include "types.h"

class PadDevice
{
public:
  PadDevice();
  virtual ~PadDevice();

  // Returns the value of ACK, as well as filling out_data.
  virtual bool Transfer(const u8 data_in, u8* data_out);
};

