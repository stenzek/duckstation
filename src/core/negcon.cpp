// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "negcon.h"
#include "controller_helpers.h"

#include "util/state_wrapper.h"
#include "util/translation.h"

#include "common/assert.h"
#include "common/bitutils.h"

#include "IconsPromptFont.h"

NeGcon::NeGcon(u32 index) : NegConBase(index, static_cast<u32>(Button::Count))
{
}

NeGcon::~NeGcon() = default;

ControllerType NeGcon::GetType() const
{
  return ControllerType::NeGcon;
}

void NeGcon::Reset()
{
  m_transfer_state = TransferState::Idle;
}

bool NeGcon::DoState(StateWrapper& sw, bool apply_input_state)
{
  u16 button_state = m_button_state;
  sw.Do(&button_state);
  if (apply_input_state)
    m_button_state = button_state;

  sw.Do(&m_transfer_state, TransferState::AnalogL);
  return true;
}

void NeGcon::ResetTransferState()
{
  m_transfer_state = TransferState::Idle;
}

bool NeGcon::Transfer(const u8 data_in, u8* data_out)
{
  static constexpr u16 ID = 0x5A23;

  switch (m_transfer_state)
  {
    case TransferState::Idle:
    {
      *data_out = 0xFF;

      if (data_in == 0x01)
      {
        m_transfer_state = TransferState::Ready;
        return true;
      }
      return false;
    }

    case TransferState::Ready:
    {
      if (data_in == 0x42)
      {
        *data_out = Truncate8(ID);
        m_transfer_state = TransferState::IDMSB;
        return true;
      }

      *data_out = 0xFF;
      return false;
    }

    case TransferState::IDMSB:
    {
      *data_out = Truncate8(ID >> 8);
      m_transfer_state = TransferState::ButtonsLSB;
      return true;
    }

    case TransferState::ButtonsLSB:
    {
      *data_out =
        Truncate8(m_disable_socd ? ControllerHelpers::RemoveOpposingDirections(m_button_state) : m_button_state);
      m_transfer_state = TransferState::ButtonsMSB;
      return true;
    }

    case TransferState::ButtonsMSB:
    {
      *data_out = Truncate8(m_button_state >> 8);
      m_transfer_state = TransferState::AnalogSteering;
      return true;
    }

    case TransferState::AnalogSteering:
    {
      *data_out = Truncate8(m_axis_state[static_cast<u8>(Axis::Steering)]);
      m_transfer_state = TransferState::AnalogI;
      return true;
    }

    case TransferState::AnalogI:
    {
      *data_out = Truncate8(m_axis_state[static_cast<u8>(Axis::I)]);
      m_transfer_state = TransferState::AnalogII;
      return true;
    }

    case TransferState::AnalogII:
    {
      *data_out = Truncate8(m_axis_state[static_cast<u8>(Axis::II)]);
      m_transfer_state = TransferState::AnalogL;
      return true;
    }

    case TransferState::AnalogL:
    {
      *data_out = Truncate8(m_axis_state[static_cast<u8>(Axis::L)]);
      m_transfer_state = TransferState::Idle;
      return false;
    }

    default:
    {
      UnreachableCode();
      return false;
    }
  }
}

std::unique_ptr<NeGcon> NeGcon::Create(u32 index)
{
  return std::make_unique<NeGcon>(index);
}

static const Controller::ControllerBindingInfo s_binding_info[] = {
#define BUTTON(name, display_name, icon_name, button, genb)                                                            \
  {name, display_name, icon_name, static_cast<u32>(button), InputBindingInfo::Type::Button, genb}
#define AXIS(name, display_name, icon_name, halfaxis, genb)                                                            \
  {name,                                                                                                               \
   display_name,                                                                                                       \
   icon_name,                                                                                                          \
   static_cast<u32>(NeGcon::Button::Count) + static_cast<u32>(halfaxis),                                               \
   InputBindingInfo::Type::HalfAxis,                                                                                   \
   genb}

  // clang-format off
  BUTTON("Up", TRANSLATE_NOOP("NeGcon", "D-Pad Up"), ICON_PF_DPAD_UP, NeGcon::Button::Up, GenericInputBinding::DPadUp),
  BUTTON("Right", TRANSLATE_NOOP("NeGcon", "D-Pad Right"), ICON_PF_DPAD_RIGHT, NeGcon::Button::Right, GenericInputBinding::DPadRight),
  BUTTON("Down", TRANSLATE_NOOP("NeGcon", "D-Pad Down"), ICON_PF_DPAD_DOWN, NeGcon::Button::Down, GenericInputBinding::DPadDown),
  BUTTON("Left", TRANSLATE_NOOP("NeGcon", "D-Pad Left"), ICON_PF_DPAD_LEFT, NeGcon::Button::Left, GenericInputBinding::DPadLeft),
  BUTTON("Start", TRANSLATE_NOOP("NeGcon", "Start"), ICON_PF_START, NeGcon::Button::Start, GenericInputBinding::Start),
  BUTTON("A", TRANSLATE_NOOP("NeGcon", "A Button"), ICON_PF_BUTTON_A, NeGcon::Button::A, GenericInputBinding::Circle),
  BUTTON("B", TRANSLATE_NOOP("NeGcon", "B Button"), ICON_PF_BUTTON_B, NeGcon::Button::B, GenericInputBinding::Triangle),
  AXIS("I", TRANSLATE_NOOP("NeGcon", "I Button"), ICON_PF_BUTTON_ALT_1, NeGcon::HalfAxis::I, GenericInputBinding::R2),
  AXIS("II", TRANSLATE_NOOP("NeGcon", "II Button"), ICON_PF_BUTTON_ALT_2, NeGcon::HalfAxis::II, GenericInputBinding::L2),
  AXIS("L", TRANSLATE_NOOP("NeGcon", "Left Trigger"), ICON_PF_LEFT_TRIGGER_LT, NeGcon::HalfAxis::L, GenericInputBinding::L1),
  BUTTON("R", TRANSLATE_NOOP("NeGcon", "Right Trigger"), ICON_PF_RIGHT_TRIGGER_RT, NeGcon::Button::R, GenericInputBinding::R1),
  AXIS("SteeringLeft", TRANSLATE_NOOP("NeGcon", "Steering (Twist) Left"), ICON_PF_ANALOG_LEFT, NeGcon::HalfAxis::SteeringLeft, GenericInputBinding::LeftStickLeft),
  AXIS("SteeringRight", TRANSLATE_NOOP("NeGcon", "Steering (Twist) Right"), ICON_PF_ANALOG_RIGHT, NeGcon::HalfAxis::SteeringRight, GenericInputBinding::LeftStickRight),
// clang-format on

#undef AXIS
#undef BUTTON
};

static constexpr SettingInfo s_settings[] = {
  {SettingInfo::Type::Boolean, "DisableSOCD",
   TRANSLATE_NOOP("NeGcon", "Disable Simultaneous Opposing Cardinal Directions"),
   TRANSLATE_NOOP("NeGcon", "Prevents concurrent left/right or up/down inputs from being presented to the game."),
   "false", nullptr, nullptr, nullptr, nullptr, nullptr, 0.0f},
  {SettingInfo::Type::Float, "SteeringDeadzone", TRANSLATE_NOOP("NeGcon", "Steering Axis Deadzone"),
   TRANSLATE_NOOP("NeGcon", "Sets deadzone for steering axis."), "0", "0", "0.99", "0.01", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Float, "SteeringSaturation", TRANSLATE_NOOP("NeGcon", "Steering Axis Saturation"),
   TRANSLATE_NOOP("NeGcon", "Sets saturation for steering axis."), "1", "0.01", "1", "0.01", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Float, "SteeringLinearity", TRANSLATE_NOOP("NeGcon", "Steering Axis Linearity"),
   TRANSLATE_NOOP("NeGcon", "Sets linearity for steering axis."), "0", "-2", "2", "0.05", "%.2f", nullptr, 1.0f},
  {SettingInfo::Type::Float, "SteeringScaling", TRANSLATE_NOOP("NeGcon", "Steering Scaling"),
   TRANSLATE_NOOP("NeGcon", "Sets scaling for steering axis."), "1", "0.01", "10", "0.01", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Float, "IDeadzone", TRANSLATE_NOOP("NeGcon", "I Button Deadzone"),
   TRANSLATE_NOOP("NeGcon", "Sets deadzone for button I."), "0", "0", "0.99", "0.01", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Float, "ISaturation", TRANSLATE_NOOP("NeGcon", "I Button Saturation"),
   TRANSLATE_NOOP("NeGcon", "Sets saturation for button I."), "1", "0.01", "1", "0.01", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Float, "ILinearity", TRANSLATE_NOOP("NeGcon", "I Button Linearity"),
   TRANSLATE_NOOP("NeGcon", "Sets linearity for button I."), "0", "-2", "2", "0.01", "%.2f", nullptr, 1.0f},
  {SettingInfo::Type::Float, "IScaling", TRANSLATE_NOOP("NeGcon", "I Scaling"),
   TRANSLATE_NOOP("NeGcon", "Sets scaling for button I."), "1", "0.01", "10", "0.01", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Float, "IIDeadzone", TRANSLATE_NOOP("NeGcon", "II Button Deadzone"),
   TRANSLATE_NOOP("NeGcon", "Sets deadzone for button II."), "0", "0", "0.99", "0.01", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Float, "IISaturation", TRANSLATE_NOOP("NeGcon", "II Button Saturation"),
   TRANSLATE_NOOP("NeGcon", "Sets saturation for button II."), "1", "0.01", "1", "0.01", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Float, "IILinearity", TRANSLATE_NOOP("NeGcon", "II Button Linearity"),
   TRANSLATE_NOOP("NeGcon", "Sets linearity for button II."), "0", "-2", "2", "0.01", "%.2f", nullptr, 1.0f},
  {SettingInfo::Type::Float, "IIScaling", TRANSLATE_NOOP("NeGcon", "II Scaling"),
   TRANSLATE_NOOP("NeGcon", "Sets scaling for button II."), "1", "0.01", "10", "0.01", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Float, "LDeadzone", TRANSLATE_NOOP("NeGcon", "Left Trigger Deadzone"),
   TRANSLATE_NOOP("NeGcon", "Sets deadzone for left trigger."), "0", "0", "0.99", "0.01", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Float, "LSaturation", TRANSLATE_NOOP("NeGcon", "Left Trigger Saturation"),
   TRANSLATE_NOOP("NeGcon", "Sets saturation for left trigger."), "1", "0.01", "1", "0.01", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Float, "LLinearity", TRANSLATE_NOOP("NeGcon", "Left Trigger Linearity"),
   TRANSLATE_NOOP("NeGcon", "Sets linearity for left trigger."), "0", "-2", "2", "0.01", "%.2f", nullptr, 1.0f},
  {SettingInfo::Type::Float, "LScaling", TRANSLATE_NOOP("NeGcon", "Left Trigger Scaling"),
   TRANSLATE_NOOP("NeGcon", "Sets scaling for left trigger."), "1", "0.01", "10", "0.01", "%.0f%%", nullptr, 100.0f},
};

const Controller::ControllerInfo NeGcon::INFO = {ControllerType::NeGcon,
                                                 "NeGcon",
                                                 TRANSLATE_NOOP("ControllerType", "NeGcon"),
                                                 ICON_PF_STEERING_WHEEL,
                                                 "images/controllers/negcon.svg",
                                                 s_binding_info,
                                                 s_settings};
