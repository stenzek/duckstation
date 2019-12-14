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

void Controller::SetAxisState(s32 axis_code, float value) {}

void Controller::SetButtonState(s32 button_code, bool pressed) {}

std::unique_ptr<Controller> Controller::Create(ControllerType type)
{
  switch (type)
  {
    return {};

    case ControllerType::DigitalController:
      return DigitalController::Create();

    case ControllerType::None:
    default:
      return {};
  }
}

std::optional<s32> Controller::GetAxisCodeByName(std::string_view button_name) const
{
  return std::nullopt;
}

std::optional<s32> Controller::GetAxisCodeByName(ControllerType type, std::string_view axis_name)
{
  switch (type)
  {
    case ControllerType::DigitalController:
      return DigitalController::StaticGetAxisCodeByName(axis_name);

    case ControllerType::None:
    default:
      return std::nullopt;
  }
}

std::optional<s32> Controller::GetButtonCodeByName(ControllerType type, std::string_view button_name)
{
  switch (type)
  {
    case ControllerType::DigitalController:
      return DigitalController::StaticGetButtonCodeByName(button_name);

    case ControllerType::None:
    default:
      return std::nullopt;
  }
}

std::optional<s32> Controller::GetButtonCodeByName(std::string_view button_name) const
{
  return std::nullopt;
}
