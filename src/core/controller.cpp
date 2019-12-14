#include "controller.h"
#include "common/state_wrapper.h"
#include "digital_controller.h"

Controller::Controller() = default;

Controller::~Controller() = default;

void Controller::Reset() {}

bool Controller::DoState(StateWrapper& sw)
{
  return !sw.HasError();
}

void Controller::ResetTransferState() {}

bool Controller::Transfer(const u8 data_in, u8* data_out)
{
  *data_out = 0xFF;
  return false;
}

void Controller::SetButtonState(s32 button_code, bool pressed) {}

std::unique_ptr<Controller> Controller::Create(std::string_view type_name)
{
  if (type_name == "DigitalController")
    return DigitalController::Create();

  return {};
}

std::optional<s32> Controller::GetButtonCodeByName(std::string_view type_name, std::string_view button_name)
{
  if (type_name == "DigitalController")
    return DigitalController::GetButtonCodeByName(button_name);

  return {};
}
