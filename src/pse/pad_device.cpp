#include "pad_device.h"
#include "common/state_wrapper.h"

PadDevice::PadDevice() = default;

PadDevice::~PadDevice() = default;

void PadDevice::Reset() {}

bool PadDevice::DoState(StateWrapper& sw)
{
  return !sw.HasError();
}

void PadDevice::ResetTransferState() {}

bool PadDevice::Transfer(const u8 data_in, u8* data_out)
{
  *data_out = 0xFF;
  return false;
}
