// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "controller.h"
#include "analog_controller.h"
#include "analog_joystick.h"
#include "ddgo_controller.h"
#include "digital_controller.h"
#include "game_database.h"
#include "guncon.h"
#include "host.h"
#include "jogcon.h"
#include "justifier.h"
#include "negcon.h"
#include "negcon_rumble.h"
#include "playstation_mouse.h"
#include "system.h"

#include "util/state_wrapper.h"

#include "IconsPromptFont.h"
#include "fmt/format.h"

static const Controller::ControllerInfo s_none_info = {
  ControllerType::None, "None", TRANSLATE_NOOP("ControllerType", "Not Connected"), ICON_PF_QUESTION, {}, {}};

static constexpr std::array<const Controller::ControllerInfo*, static_cast<size_t>(ControllerType::Count)>
  s_controller_info = {{
    &s_none_info,
    &DigitalController::INFO,
    &AnalogController::INFO,
    &AnalogJoystick::INFO,
    &GunCon::INFO,
    &PlayStationMouse::INFO,
    &NeGcon::INFO,
    &NeGconRumble::INFO,
    &Justifier::INFO,
    &DigitalController::INFO_POPN,
    &DDGoController::INFO,
    &JogCon::INFO,
  }};

const std::array<u32, NUM_CONTROLLER_AND_CARD_PORTS> Controller::PortDisplayOrder = {{0, 2, 3, 4, 1, 5, 6, 7}};

std::string_view Controller::ControllerInfo::GetDisplayName() const
{
  return Host::TranslateToStringView("ControllerType", display_name);
}

std::string_view Controller::ControllerInfo::GetBindingDisplayName(const ControllerBindingInfo& bi) const
{
  return Host::TranslateToStringView(name, bi.display_name);
}

Controller::Controller(u32 index) : m_index(index)
{
}

Controller::~Controller() = default;

void Controller::Reset()
{
}

bool Controller::DoState(StateWrapper& sw, bool apply_input_state)
{
  return !sw.HasError();
}

void Controller::ResetTransferState()
{
}

bool Controller::Transfer(const u8 data_in, u8* data_out)
{
  *data_out = 0xFF;
  return false;
}

float Controller::GetBindState(u32 index) const
{
  return 0.0f;
}

void Controller::SetBindState(u32 index, float value)
{
}

u32 Controller::GetButtonStateBits() const
{
  return 0;
}

float Controller::GetVibrationMotorState(u32 index) const
{
  return 0.0f;
}

bool Controller::InAnalogMode() const
{
  return false;
}

std::optional<u32> Controller::GetAnalogInputBytes() const
{
  return std::nullopt;
}

u32 Controller::GetInputOverlayIconColor() const
{
  return 0xFFFFFFFFu;
}

void Controller::LoadSettings(const SettingsInterface& si, const char* section, bool initial)
{
}

std::unique_ptr<Controller> Controller::Create(ControllerType type, u32 index)
{
  switch (type)
  {
    case ControllerType::DigitalController:
    case ControllerType::PopnController:
      return DigitalController::Create(index, type);

    case ControllerType::AnalogController:
      return AnalogController::Create(index);

    case ControllerType::AnalogJoystick:
      return AnalogJoystick::Create(index);

    case ControllerType::GunCon:
      return GunCon::Create(index);

    case ControllerType::Justifier:
      return Justifier::Create(index);

    case ControllerType::PlayStationMouse:
      return PlayStationMouse::Create(index);

    case ControllerType::NeGcon:
      return NeGcon::Create(index);

    case ControllerType::NeGconRumble:
      return NeGconRumble::Create(index);

    case ControllerType::DDGoController:
      return DDGoController::Create(index);

    case ControllerType::JogCon:
      return JogCon::Create(index);

    case ControllerType::None:
    default:
      return {};
  }
}

const Controller::ControllerInfo& Controller::GetControllerInfo(ControllerType type)
{
  DebugAssert(type < ControllerType::Count && s_controller_info[static_cast<size_t>(type)]);
  return *s_controller_info[static_cast<size_t>(type)];
}

const Controller::ControllerInfo* Controller::GetControllerInfo(std::string_view name)
{
  for (const ControllerInfo* info : s_controller_info)
  {
    if (name == info->name)
      return info;
  }

  return nullptr;
}

const std::array<const Controller::ControllerInfo*, static_cast<size_t>(ControllerType::Count)>&
Controller::GetControllerInfoList()
{
  return s_controller_info;
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

const char* Controller::GetPortDisplayName(u32 port, u32 slot, bool mtap)
{
  static constexpr const std::array<const char*, NUM_MULTITAPS> no_mtap_labels = {{"1", "2"}};
  static constexpr const std::array<std::array<const char*, NUM_CONTROLLER_AND_CARD_PORTS_PER_MULTITAP>, NUM_MULTITAPS>
    mtap_labels = {{{{"1A", "1B", "1C", "1D"}}, {{"2A", "2B", "2C", "2D"}}}};

  DebugAssert(port < 2 && slot < 4);
  return mtap ? mtap_labels[port][slot] : no_mtap_labels[port];
}

const char* Controller::GetPortDisplayName(u32 index)
{
  const auto& [port, slot] = ConvertPadToPortAndSlot(index);
  return GetPortDisplayName(port, slot, g_settings.IsMultitapPortEnabled(port));
}

std::string Controller::GetSettingsSection(u32 pad)
{
  return fmt::format("Pad{}", pad + 1u);
}

bool Controller::InCircularDeadzone(float deadzone, float pos_x, float pos_y)
{
  if (pos_x == 0.0f && pos_y == 0.0f)
    return false;

  // Compute the angle at the given position in the stick's square bounding box.
  const float theta = std::atan2(pos_y, pos_x);

  // Compute the position that the edge of the circle would be at, given the angle.
  const float dz_x = std::cos(theta) * deadzone;
  const float dz_y = std::sin(theta) * deadzone;

  // We're in the deadzone if our position is less than the circle edge.
  const bool in_x = (pos_x < 0.0f) ? (pos_x > dz_x) : (pos_x <= dz_x);
  const bool in_y = (pos_y < 0.0f) ? (pos_y > dz_y) : (pos_y <= dz_y);
  return (in_x && in_y);
}

bool Controller::CanStartInAnalogMode(ControllerType ctype)
{
  if (!g_settings.apply_compatibility_settings)
    return true;

  const GameDatabase::Entry* dbentry = System::GetGameDatabaseEntry();
  if (!dbentry)
    return false;

  return ((dbentry->supported_controllers & (1u << static_cast<u8>(ctype))) != 0 &&
          !dbentry->HasTrait(GameDatabase::Trait::DisableAutoAnalogMode));
}
