#include "pad_device.h"

PadDevice::PadDevice() = default;

PadDevice::~PadDevice() = default;

void PadDevice::ResetTransferState() {}

bool PadDevice::Transfer(const u8 data_in, u8* data_out)
{
  *data_out = 0xFF;
  return false;
}
