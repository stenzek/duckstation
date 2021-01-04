#include "controller.h"
#include "analog_controller.h"
#include "analog_joystick.h"
#include "common/state_wrapper.h"
#include "digital_controller.h"
#include "namco_guncon.h"
#include "negcon.h"
#include "playstation_mouse.h"

Controller::Controller() = default;

Controller::~Controller() = default;

void Controller::Reset() {}

bool Controller::DoState(StateWrapper& sw, bool apply_input_state)
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

u32 Controller::GetButtonStateBits() const
{
  return 0;
}

std::optional<u32> Controller::GetAnalogInputBytes() const
{
  return std::nullopt;
}

u32 Controller::GetVibrationMotorCount() const
{
  return 0;
}

float Controller::GetVibrationMotorStrength(u32 motor)
{
  return 0.0f;
}

void Controller::LoadSettings(const char* section) {}

bool Controller::GetSoftwareCursor(const Common::RGBA8Image** image, float* image_scale, bool* relative_mode)
{
  return false;
}

std::unique_ptr<Controller> Controller::Create(ControllerType type, u32 index)
{
  switch (type)
  {
    case ControllerType::DigitalController:
      return DigitalController::Create();

    case ControllerType::AnalogController:
      return AnalogController::Create(index);

    case ControllerType::AnalogJoystick:
      return AnalogJoystick::Create(index);

    case ControllerType::NamcoGunCon:
      return NamcoGunCon::Create();

    case ControllerType::PlayStationMouse:
      return PlayStationMouse::Create();

    case ControllerType::NeGcon:
      return NeGcon::Create();

    case ControllerType::None:
    default:
      return {};
  }
}

std::optional<s32> Controller::GetAxisCodeByName(std::string_view button_name) const
{
  return std::nullopt;
}

std::optional<s32> Controller::GetButtonCodeByName(std::string_view button_name) const
{
  return std::nullopt;
}

Controller::AxisList Controller::GetAxisNames(ControllerType type)
{
  switch (type)
  {
    case ControllerType::DigitalController:
      return DigitalController::StaticGetAxisNames();

    case ControllerType::AnalogController:
      return AnalogController::StaticGetAxisNames();

    case ControllerType::AnalogJoystick:
      return AnalogJoystick::StaticGetAxisNames();

    case ControllerType::NamcoGunCon:
      return NamcoGunCon::StaticGetAxisNames();

    case ControllerType::PlayStationMouse:
      return PlayStationMouse::StaticGetAxisNames();

    case ControllerType::NeGcon:
      return NeGcon::StaticGetAxisNames();

    case ControllerType::None:
    default:
      return {};
  }
}

Controller::ButtonList Controller::GetButtonNames(ControllerType type)
{
  switch (type)
  {
    case ControllerType::DigitalController:
      return DigitalController::StaticGetButtonNames();

    case ControllerType::AnalogController:
      return AnalogController::StaticGetButtonNames();

    case ControllerType::AnalogJoystick:
      return AnalogJoystick::StaticGetButtonNames();

    case ControllerType::NamcoGunCon:
      return NamcoGunCon::StaticGetButtonNames();

    case ControllerType::PlayStationMouse:
      return PlayStationMouse::StaticGetButtonNames();

    case ControllerType::NeGcon:
      return NeGcon::StaticGetButtonNames();

    case ControllerType::None:
    default:
      return {};
  }
}

u32 Controller::GetVibrationMotorCount(ControllerType type)
{
  switch (type)
  {
    case ControllerType::DigitalController:
      return DigitalController::StaticGetVibrationMotorCount();

    case ControllerType::AnalogController:
      return AnalogController::StaticGetVibrationMotorCount();

    case ControllerType::AnalogJoystick:
      return AnalogJoystick::StaticGetVibrationMotorCount();

    case ControllerType::NamcoGunCon:
      return NamcoGunCon::StaticGetVibrationMotorCount();

    case ControllerType::PlayStationMouse:
      return PlayStationMouse::StaticGetVibrationMotorCount();

    case ControllerType::NeGcon:
      return NeGcon::StaticGetVibrationMotorCount();

    case ControllerType::None:
    default:
      return 0;
  }
}

std::optional<s32> Controller::GetAxisCodeByName(ControllerType type, std::string_view axis_name)
{
  switch (type)
  {
    case ControllerType::DigitalController:
      return DigitalController::StaticGetAxisCodeByName(axis_name);

    case ControllerType::AnalogController:
      return AnalogController::StaticGetAxisCodeByName(axis_name);

    case ControllerType::AnalogJoystick:
      return AnalogJoystick::StaticGetAxisCodeByName(axis_name);

    case ControllerType::NamcoGunCon:
      return NamcoGunCon::StaticGetAxisCodeByName(axis_name);

    case ControllerType::PlayStationMouse:
      return PlayStationMouse::StaticGetAxisCodeByName(axis_name);

    case ControllerType::NeGcon:
      return NeGcon::StaticGetAxisCodeByName(axis_name);

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

    case ControllerType::AnalogController:
      return AnalogController::StaticGetButtonCodeByName(button_name);

    case ControllerType::AnalogJoystick:
      return AnalogJoystick::StaticGetButtonCodeByName(button_name);

    case ControllerType::NamcoGunCon:
      return NamcoGunCon::StaticGetButtonCodeByName(button_name);

    case ControllerType::PlayStationMouse:
      return PlayStationMouse::StaticGetButtonCodeByName(button_name);

    case ControllerType::NeGcon:
      return NeGcon::StaticGetButtonCodeByName(button_name);

    case ControllerType::None:
    default:
      return std::nullopt;
  }
}

Controller::SettingList Controller::GetSettings(ControllerType type)
{
  switch (type)
  {
    case ControllerType::DigitalController:
      return DigitalController::StaticGetSettings();

    case ControllerType::AnalogController:
      return AnalogController::StaticGetSettings();

    case ControllerType::AnalogJoystick:
      return AnalogJoystick::StaticGetSettings();

    case ControllerType::NamcoGunCon:
      return NamcoGunCon::StaticGetSettings();

    case ControllerType::NeGcon:
      return NeGcon::StaticGetSettings();

    case ControllerType::PlayStationMouse:
      return PlayStationMouse::StaticGetSettings();

    default:
      return {};
  }
}
