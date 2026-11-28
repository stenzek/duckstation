// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "ddgo_controller.h"
#include "system.h"

#include "util/state_wrapper.h"
#include "util/translation.h"

#include "IconsPromptFont.h"

#include "common/assert.h"
#include "common/bitutils.h"
#include "common/settings_interface.h"

DDGoController::DDGoController(u32 index) : Controller(index, ControllerType::DDGoController)
{
}

DDGoController::~DDGoController() = default;

void DDGoController::Reset()
{
  ResetTransferState();
  m_power_transition_frames_remaining = 0;
  UpdatePowerBits();
  m_brake_transition_frames_remaining = 0;
  UpdateBrakeBits();
}

bool DDGoController::DoState(StateWrapper& sw, bool apply_input_state)
{
  if (sw.GetVersion() >= 105 && !Controller::DoState(sw, apply_input_state))
    return false;

  if (sw.IsWriting() || apply_input_state)
  {
    sw.Do(&m_button_state);
    sw.Do(&m_power_level);
    sw.Do(&m_power_transition_frames_remaining);
    sw.Do(&m_brake_level);
    sw.Do(&m_brake_transition_frames_remaining);
    if (sw.IsReading())
    {
      UpdatePowerBits();
      UpdateBrakeBits();
    }
  }
  else
  {
    sw.SkipBytes(sizeof(m_button_state) + sizeof(m_power_level) + sizeof(m_power_transition_frames_remaining) +
                 sizeof(m_brake_level) + sizeof(m_brake_transition_frames_remaining));
  }

  if (sw.GetVersion() < 105)
    SkipLegacyTransferState(sw);

  return true;
}

float DDGoController::GetBindState(u32 index) const
{
  if (index >= static_cast<u32>(Bind::VirtualButtonStart))
  {
    if (index < static_cast<u32>(Bind::VirtualBrakeReleased))
      return BoolToFloat(m_power_level == (index - static_cast<u32>(Bind::VirtualPowerOff)));
    else
      return BoolToFloat(m_brake_level == (index - static_cast<u32>(Bind::VirtualBrakeReleased)));
  }

  // don't show the buttons set by the level
  static constexpr u16 REPORT_MASK = (1u << static_cast<u16>(Bind::Start)) | (1u << static_cast<u16>(Bind::Select)) |
                                     (1u << static_cast<u16>(Bind::A)) | (1u << static_cast<u16>(Bind::B)) |
                                     (1u << static_cast<u16>(Bind::C));
  return static_cast<float>((((m_button_state ^ 0xFFFFu) & REPORT_MASK) >> index) & 1u);
}

void DDGoController::SetBindState(u32 index, float value)
{
  if (index == static_cast<u32>(Bind::Power))
  {
    value = (value < m_analog_deadzone) ? 0.0f : (value * m_analog_sensitivity);
    SetPowerLevel(std::min(static_cast<u32>(value * MAX_POWER_LEVEL), MAX_POWER_LEVEL));
    return;
  }
  else if (index == static_cast<u32>(Bind::Brake))
  {
    value = (value < m_analog_deadzone) ? 0.0f : (value * m_analog_sensitivity);
    SetBrakeLevel(std::min(static_cast<u32>(value * MAX_BRAKE_LEVEL), MAX_BRAKE_LEVEL));
    return;
  }
  else if (index >= static_cast<u32>(Bind::VirtualButtonStart))
  {
    // vbutton, only handle press
    if (value < 0.5f)
      return;

    if (index < static_cast<u32>(Bind::VirtualBrakeReleased))
      SetPowerLevel(index - static_cast<u32>(Bind::VirtualPowerOff));
    else
      SetBrakeLevel(index - static_cast<u32>(Bind::VirtualBrakeReleased));

    return;
  }

  SetButtonPressed(m_button_state, u16(1) << static_cast<u8>(index), (value >= 0.5f));
}

u32 DDGoController::GetButtonStateBits() const
{
  return m_button_state ^ 0xFFFF;
}

u8 DDGoController::GetIDByte() const
{
  return 0x41;
}

bool DDGoController::BeginCommand(u8 command)
{
  if (command != 0x42)
    return false;

  m_tx_buffer[2] = Truncate8(m_button_state & BUTTON_MASK);
  m_tx_buffer[3] = Truncate8((m_button_state & BUTTON_MASK) >> 8);
  return true;
}

void DDGoController::EndCommand()
{
  // handle transition time, after the poll so that it's seen for the full count
  if (m_power_transition_frames_remaining > 0)
  {
    m_power_transition_frames_remaining--;
    UpdatePowerBits();
  }

  if (m_brake_transition_frames_remaining > 0)
  {
    m_brake_transition_frames_remaining--;
    UpdateBrakeBits();
  }
}

void DDGoController::SetPowerLevel(u32 level)
{
  DebugAssert(level <= MAX_POWER_LEVEL);
  if (m_power_level == level)
    return;

  m_power_level = Truncate8(level);
  m_power_transition_frames_remaining = m_power_transition_frames;
  UpdatePowerBits();
  System::SetRunaheadReplayFlag(false);
}

void DDGoController::UpdatePowerBits()
{
#define POWER_BITS(b0, b1, b2)                                                                                         \
  static_cast<u16>(~(((b0) ? (1u << static_cast<u32>(Bind::PowerBit0)) : 0u) |                                         \
                     ((b1) ? (1u << static_cast<u32>(Bind::PowerBit1)) : 0u) |                                         \
                     ((b2) ? (1u << static_cast<u32>(Bind::PowerBit2)) : 0u)) &                                        \
                   POWER_MASK)

  static constexpr std::array<u16, MAX_POWER_LEVEL + 2> POWER_TABLE = {{
    POWER_BITS(0, 1, 1), // N
    POWER_BITS(1, 0, 1), // P1
    POWER_BITS(0, 0, 1), // P2
    POWER_BITS(1, 1, 0), // P3
    POWER_BITS(0, 1, 0), // P4
    POWER_BITS(1, 0, 0), // P5
    POWER_BITS(0, 0, 0), // Transition
  }};

#undef POWER_BITS

  const u32 idx = (m_power_transition_frames_remaining > 0) ? (MAX_POWER_LEVEL + 1) : m_power_level;
  m_button_state = (m_button_state & ~POWER_MASK) | POWER_TABLE[idx];
}

void DDGoController::SetBrakeLevel(u32 level)
{
  DebugAssert(level <= MAX_BRAKE_LEVEL);
  if (m_brake_level == level)
    return;

  m_brake_level = Truncate8(level);
  m_brake_transition_frames_remaining = m_brake_transition_frames;
  UpdateBrakeBits();
  System::SetRunaheadReplayFlag(false);
}

void DDGoController::UpdateBrakeBits()
{
#define BRAKE_BITS(b0, b1, b2, b3)                                                                                     \
  static_cast<u16>(~(((b0) ? (1u << static_cast<u32>(Bind::BrakeBit0)) : 0u) |                                         \
                     ((b1) ? (1u << static_cast<u32>(Bind::BrakeBit1)) : 0u) |                                         \
                     ((b2) ? (1u << static_cast<u32>(Bind::BrakeBit2)) : 0u) |                                         \
                     ((b3) ? (1u << static_cast<u32>(Bind::BrakeBit3)) : 0u)) &                                        \
                   BRAKE_MASK)

  static constexpr std::array<u16, MAX_BRAKE_LEVEL + 2> BRAKE_TABLE = {{
    BRAKE_BITS(0, 1, 1, 1), // Released
    BRAKE_BITS(1, 0, 1, 1), // B1
    BRAKE_BITS(0, 0, 1, 1), // B2
    BRAKE_BITS(1, 1, 0, 1), // B3
    BRAKE_BITS(0, 1, 0, 1), // B4
    BRAKE_BITS(1, 0, 0, 1), // B5
    BRAKE_BITS(0, 0, 0, 1), // B6
    BRAKE_BITS(1, 1, 1, 0), // B7
    BRAKE_BITS(0, 1, 1, 0), // B8
    BRAKE_BITS(0, 0, 0, 0), // Emergency
    BRAKE_BITS(1, 1, 1, 1), // Transition
  }};

#undef BRAKE_BITS

  const u32 idx = (m_brake_transition_frames_remaining > 0) ? (MAX_BRAKE_LEVEL + 1) : m_brake_level;
  m_button_state = (m_button_state & ~BRAKE_MASK) | BRAKE_TABLE[idx];
}

void DDGoController::LoadSettings(const SettingsInterface& si, const char* section, bool initial)
{
  m_analog_deadzone = std::clamp(si.GetFloatValue(section, "AnalogDeadzone", DEFAULT_STICK_DEADZONE), 0.0f, 1.0f);
  m_analog_sensitivity =
    std::clamp(si.GetFloatValue(section, "AnalogSensitivity", DEFAULT_STICK_SENSITIVITY), 0.01f, 3.0f);
  m_power_transition_frames = static_cast<u8>(si.GetIntValue(section, "PowerTransitionFrames", 10));
  m_brake_transition_frames = static_cast<u8>(si.GetIntValue(section, "BrakeTransitionFrames", 10));
}

std::unique_ptr<DDGoController> DDGoController::Create(u32 index)
{
  return std::make_unique<DDGoController>(index);
}

static const Controller::ControllerBindingInfo s_binding_info[] = {
// clang-format off
#define BUTTON(name, display_name, icon_name, image_element, button, genb) \
  {name, display_name, icon_name, image_element, static_cast<u32>(button), InputBindingInfo::Type::Button, genb}
#define AXIS(name, display_name, icon_name, image_element, axis, genb) \
  {name, display_name, icon_name, image_element, static_cast<u32>(axis), InputBindingInfo::Type::HalfAxis, genb}

  BUTTON("Select", TRANSLATE_NOOP("DDGoController", "Select"), ICON_PF_SELECT_SHARE, "Select", DDGoController::Bind::Select, GenericInputBinding::Select),
  BUTTON("Start", TRANSLATE_NOOP("DDGoController", "Start"), ICON_PF_START, "Start", DDGoController::Bind::Start, GenericInputBinding::Start),
  BUTTON("A", TRANSLATE_NOOP("DDGoController", "A"), ICON_PF_BUTTON_A, "A", DDGoController::Bind::A, GenericInputBinding::Square),
  BUTTON("B", TRANSLATE_NOOP("DDGoController", "B"), ICON_PF_BUTTON_B, "B", DDGoController::Bind::B, GenericInputBinding::Cross),
  BUTTON("C", TRANSLATE_NOOP("DDGoController", "C"), ICON_PF_BUTTON_C, "C", DDGoController::Bind::C, GenericInputBinding::Circle),
  BUTTON("VirtualPowerOff", TRANSLATE_NOOP("DDGoController", "Power Off"), ICON_PF_KEY_N, "Power", DDGoController::Bind::VirtualPowerOff, GenericInputBinding::Unknown),
  BUTTON("VirtualPower1", TRANSLATE_NOOP("DDGoController", "Power 1"), ICON_PF_1, "Power", DDGoController::Bind::VirtualPower1, GenericInputBinding::Unknown),
  BUTTON("VirtualPower2", TRANSLATE_NOOP("DDGoController", "Power 2"), ICON_PF_2, "Power", DDGoController::Bind::VirtualPower2, GenericInputBinding::Unknown),
  BUTTON("VirtualPower3", TRANSLATE_NOOP("DDGoController", "Power 3"), ICON_PF_3, "Power", DDGoController::Bind::VirtualPower3, GenericInputBinding::Unknown),
  BUTTON("VirtualPower4", TRANSLATE_NOOP("DDGoController", "Power 4"), ICON_PF_4, "Power", DDGoController::Bind::VirtualPower4, GenericInputBinding::Unknown),
  BUTTON("VirtualPower5", TRANSLATE_NOOP("DDGoController", "Power 5"), ICON_PF_5, "Power", DDGoController::Bind::VirtualPower5, GenericInputBinding::Unknown),
  BUTTON("VirtualBrakeReleased", TRANSLATE_NOOP("DDGoController", "Brake Released"), ICON_PF_KEY_R, "Brake", DDGoController::Bind::VirtualBrakeReleased, GenericInputBinding::Unknown),
  BUTTON("VirtualBrake1", TRANSLATE_NOOP("DDGoController", "Brake 1"), ICON_PF_1, "Brake", DDGoController::Bind::VirtualBrake1, GenericInputBinding::Unknown),
  BUTTON("VirtualBrake2", TRANSLATE_NOOP("DDGoController", "Brake 2"), ICON_PF_2, "Brake", DDGoController::Bind::VirtualBrake2, GenericInputBinding::Unknown),
  BUTTON("VirtualBrake3", TRANSLATE_NOOP("DDGoController", "Brake 3"), ICON_PF_3, "Brake", DDGoController::Bind::VirtualBrake3, GenericInputBinding::Unknown),
  BUTTON("VirtualBrake4", TRANSLATE_NOOP("DDGoController", "Brake 4"), ICON_PF_4, "Brake", DDGoController::Bind::VirtualBrake4, GenericInputBinding::Unknown),
  BUTTON("VirtualBrake5", TRANSLATE_NOOP("DDGoController", "Brake 5"), ICON_PF_5, "Brake", DDGoController::Bind::VirtualBrake5, GenericInputBinding::Unknown),
  BUTTON("VirtualBrake6", TRANSLATE_NOOP("DDGoController", "Brake 6"), ICON_PF_6, "Brake", DDGoController::Bind::VirtualBrake6, GenericInputBinding::Unknown),
  BUTTON("VirtualBrake7", TRANSLATE_NOOP("DDGoController", "Brake 7"), ICON_PF_7, "Brake", DDGoController::Bind::VirtualBrake7, GenericInputBinding::Unknown),
  BUTTON("VirtualBrake8", TRANSLATE_NOOP("DDGoController", "Brake 8"), ICON_PF_8, "Brake", DDGoController::Bind::VirtualBrake8, GenericInputBinding::Unknown),
  BUTTON("VirtualBrakeEmergency", TRANSLATE_NOOP("DDGoController", "Brake Emergency"), ICON_PF_KEY_E, "Brake", DDGoController::Bind::VirtualBrakeEmergency, GenericInputBinding::Unknown),

  AXIS("Power", TRANSLATE_NOOP("DDGoController", "Power"), ICON_PF_KEY_P, "Power", DDGoController::Bind::Power, GenericInputBinding::L2),
  AXIS("Brake", TRANSLATE_NOOP("DDGoController", "Brake"), ICON_PF_KEY_B, "Brake", DDGoController::Bind::Brake, GenericInputBinding::R2),
// clang-format on

#undef AXIS
#undef BUTTON
};

static const SettingInfo s_settings[] = {
  {SettingInfo::Type::Float, "AnalogDeadzone", TRANSLATE_NOOP("DDGoController", "Analog Deadzone"),
   TRANSLATE_NOOP("DDGoController",
                  "Sets the analog stick deadzone, i.e. the fraction of the stick movement which will be ignored."),
   "0", "0", "1", "0.01", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Float, "AnalogSensitivity", TRANSLATE_NOOP("DDGoController", "Analog Sensitivity"),
   TRANSLATE_NOOP(
     "DDGoController",
     "Sets the analog stick axis scaling factor. A value between 130% and 140% is recommended when using recent "
     "controllers, e.g. DualShock 4, Xbox One Controller."),
   "1.33", "0.01", "2", "0.01", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Integer, "PowerTransitionFrames", TRANSLATE_NOOP("DDGoController", "Power Transition Frames"),
   TRANSLATE_NOOP("DDGoController", "Sets the number of frames that the controller will report the "
                                    "transitioning/inbetween state when changing power level."),
   "10", "0", "255", "1", "%d", nullptr, 1.0f},
  {SettingInfo::Type::Integer, "BrakeTransitionFrames", TRANSLATE_NOOP("DDGoController", "Brake Transition Frames"),
   TRANSLATE_NOOP("DDGoController", "Sets the number of frames that the controller will report the "
                                    "transitioning/inbetween state when changing brake level."),
   "10", "0", "255", "1", "%d", nullptr, 1.0f}};

const Controller::ControllerInfo DDGoController::INFO = {ControllerType::DDGoController,
                                                         "DDGoController",
                                                         TRANSLATE_NOOP("ControllerType", "Densha de Go! Controller"),
                                                         ICON_PF_FIGHT_STICK_JOYSTICK,
                                                         "images/controllers/ddgo_controller.svg",
                                                         s_binding_info,
                                                         s_settings};
