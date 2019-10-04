#pragma once
#include "types.h"

class StateWrapper;

class PadDevice
{
public:
  PadDevice();
  virtual ~PadDevice();

  virtual void Reset();
  virtual bool DoState(StateWrapper& sw);

  // Resets all state for the transferring to/from the device.
  virtual void ResetTransferState();

  // Returns the value of ACK, as well as filling out_data.
  virtual bool Transfer(const u8 data_in, u8* data_out);
};

