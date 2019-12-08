#include "pad_device.h"
#include "common/state_wrapper.h"
#include "digital_controller.h"

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

std::shared_ptr<PadDevice> PadDevice::Create(std::string_view type_name)
{
  if (type_name == "DigitalController")
    return DigitalController::Create();

  return {};
}

std::optional<s32> PadDevice::GetButtonCodeByName(std::string_view type_name, std::string_view button_name)
{
  if (type_name == "DigitalController")
    return DigitalController::GetButtonCodeByName(button_name);

  return {};
}
