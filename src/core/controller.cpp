#include "controller.h"
#include "analog_controller.h"
#include "analog_joystick.h"
#include "digital_controller.h"
#include "fmt/format.h"
#include "guncon.h"
#include "negcon.h"
#include "playstation_mouse.h"
#include "util/state_wrapper.h"

static const Controller::ControllerInfo s_none_info = {ControllerType::None,
                                                       "None",
                                                       "Not Connected",
                                                       nullptr,
                                                       0,
                                                       nullptr,
                                                       0,
                                                       Controller::VibrationCapabilities::NoVibration};

static const Controller::ControllerInfo* s_controller_info[] = {
  &s_none_info,  &DigitalController::INFO, &AnalogController::INFO, &AnalogJoystick::INFO, &NeGcon::INFO,
  &GunCon::INFO, &PlayStationMouse::INFO,
};

Controller::Controller(u32 index) : m_index(index) {}

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

float Controller::GetBindState(u32 index) const
{
  return 0.0f;
}

void Controller::SetBindState(u32 index, float value) {}

u32 Controller::GetButtonStateBits() const
{
  return 0;
}

std::optional<u32> Controller::GetAnalogInputBytes() const
{
  return std::nullopt;
}

void Controller::LoadSettings(SettingsInterface& si, const char* section) {}

bool Controller::GetSoftwareCursor(const Common::RGBA8Image** image, float* image_scale, bool* relative_mode)
{
  return false;
}

std::unique_ptr<Controller> Controller::Create(ControllerType type, u32 index)
{
  switch (type)
  {
    case ControllerType::DigitalController:
      return DigitalController::Create(index);

    case ControllerType::AnalogController:
      return AnalogController::Create(index);

    case ControllerType::AnalogJoystick:
      return AnalogJoystick::Create(index);

    case ControllerType::GunCon:
      return GunCon::Create(index);

    case ControllerType::PlayStationMouse:
      return PlayStationMouse::Create(index);

    case ControllerType::NeGcon:
      return NeGcon::Create(index);

    case ControllerType::None:
    default:
      return {};
  }
}

const char* Controller::GetDefaultPadType(u32 pad)
{
  return (pad == 0) ? "DigitalController" : "None";
}

const Controller::ControllerInfo* Controller::GetControllerInfo(ControllerType type)
{
  for (const ControllerInfo* info : s_controller_info)
  {
    if (type == info->type)
      return info;
  }

  return nullptr;
}

const Controller::ControllerInfo* Controller::GetControllerInfo(const std::string_view& name)
{
  for (const ControllerInfo* info : s_controller_info)
  {
    if (name == info->name)
      return info;
  }

  return nullptr;
}

std::vector<std::pair<std::string, std::string>> Controller::GetControllerTypeNames()
{
  std::vector<std::pair<std::string, std::string>> ret;
  for (const ControllerInfo* info : s_controller_info)
    ret.emplace_back(info->name, info->display_name);

  return ret;
}

std::vector<std::string> Controller::GetControllerBinds(const std::string_view& type)
{
  std::vector<std::string> ret;

  const ControllerInfo* info = GetControllerInfo(type);
  if (info)
  {
    for (u32 i = 0; i < info->num_bindings; i++)
    {
      const ControllerBindingInfo& bi = info->bindings[i];
      if (bi.type == ControllerBindingType::Unknown || bi.type == ControllerBindingType::Motor)
        continue;

      ret.emplace_back(info->bindings[i].name);
    }
  }

  return ret;
}

std::vector<std::string> Controller::GetControllerBinds(ControllerType type)
{
  std::vector<std::string> ret;

  const ControllerInfo* info = GetControllerInfo(type);
  if (info)
  {
    for (u32 i = 0; i < info->num_bindings; i++)
    {
      const ControllerBindingInfo& bi = info->bindings[i];
      if (bi.type == ControllerBindingType::Unknown || bi.type == ControllerBindingType::Motor)
        continue;

      ret.emplace_back(info->bindings[i].name);
    }
  }

  return ret;
}

std::optional<u32> Controller::GetBindIndex(ControllerType type, const std::string_view& bind_name)
{
  const ControllerInfo* info = GetControllerInfo(type);
  if (!info)
    return std::nullopt;

  for (u32 i = 0; i < info->num_bindings; i++)
  {
    if (bind_name == info->bindings[i].name)
      return i;
  }

  return std::nullopt;
}

Controller::VibrationCapabilities Controller::GetControllerVibrationCapabilities(const std::string_view& type)
{
  const ControllerInfo* info = GetControllerInfo(type);
  return info ? info->vibration_caps : VibrationCapabilities::NoVibration;
}

std::tuple<u32, u32> Controller::ConvertPadToPortAndSlot(u32 index)
{
  if (index > 4)                          // [5,6,7]
    return std::make_tuple(1, index - 4); // 2B,2C,2D
  else if (index > 1)                     // [2,3,4]
    return std::make_tuple(0, index - 1); // 1B,1C,1D
  else                                    // [0,1]
    return std::make_tuple(index, 0);     // 1A,2A
}

u32 Controller::ConvertPortAndSlotToPad(u32 port, u32 slot)
{
  if (slot == 0)
    return port;
  else if (port == 0) // slot=[0,1]
    return slot + 1;  // 2,3,4
  else
    return slot + 4; // 5,6,7
}

bool Controller::PadIsMultitapSlot(u32 index)
{
  return (index >= 2);
}

bool Controller::PortAndSlotIsMultitap(u32 port, u32 slot)
{
  return (slot != 0);
}

std::string Controller::GetSettingsSection(u32 pad)
{
  return fmt::format("Pad{}", pad + 1u);
}
