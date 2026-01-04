// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "digital_controller.h"
#include "system.h"

#include "util/state_wrapper.h"
#include "util/translation.h"

#include "IconsPromptFont.h"

#include "common/assert.h"
#include "common/bitutils.h"

DigitalController::DigitalController(u32 index, u16 button_mask) : Controller(index), m_button_mask(button_mask)
{
}

DigitalController::~DigitalController() = default;

ControllerType DigitalController::GetType() const
{
  return ControllerType::DigitalController;
}

void DigitalController::Reset()
{
  m_transfer_state = TransferState::Idle;
}

bool DigitalController::DoState(StateWrapper& sw, bool apply_input_state)
{
  if (!Controller::DoState(sw, apply_input_state))
    return false;

  u16 button_state = m_button_state;
  sw.Do(&button_state);
  if (apply_input_state)
    m_button_state = button_state;

  sw.Do(&m_transfer_state);
  return true;
}

float DigitalController::GetBindState(u32 index) const
{
  if (index < static_cast<u32>(Button::Count))
  {
    return static_cast<float>(((m_button_state >> index) & 1u) ^ 1u);
  }
  else
  {
    return 0.0f;
  }
}

void DigitalController::SetBindState(u32 index, float value)
{
  if (index >= static_cast<u32>(Button::Count))
    return;

  const bool pressed = (value >= 0.5f);
  const u16 bit = u16(1) << static_cast<u8>(index);
  if (pressed)
  {
    if (m_button_state & bit)
      System::SetRunaheadReplayFlag(false);

    m_button_state &= ~bit;
  }
  else
  {
    if (!(m_button_state & bit))
      System::SetRunaheadReplayFlag(false);

    m_button_state |= bit;
  }
}

u32 DigitalController::GetButtonStateBits() const
{
  return m_button_state ^ 0xFFFF;
}

void DigitalController::ResetTransferState()
{
  m_transfer_state = TransferState::Idle;
}

bool DigitalController::Transfer(const u8 data_in, u8* data_out)
{
  static constexpr u16 ID = 0x5A41;

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
      *data_out = Truncate8(m_button_state & m_button_mask);
      m_transfer_state = TransferState::ButtonsMSB;
      return true;
    }

    case TransferState::ButtonsMSB:
      *data_out = Truncate8((m_button_state & m_button_mask) >> 8);
      m_transfer_state = TransferState::Idle;
      return false;

    default:
      UnreachableCode();
  }
}

std::unique_ptr<DigitalController> DigitalController::Create(u32 index, ControllerType type)
{
  // popn controller - right/down/left grounded
  static constexpr u16 POPN_BUTTON_MASK =
    static_cast<u16>(~((1u << static_cast<u8>(Button::Right)) | (1u << static_cast<u8>(Button::Down)) |
                       (1u << static_cast<u8>(Button::Left))));

  u16 mask = 0xFFFFu;
  if (type == ControllerType::PopnController)
    mask = POPN_BUTTON_MASK;

  return std::make_unique<DigitalController>(index, mask);
}

static const Controller::ControllerBindingInfo s_binding_info[] = {
#define BUTTON(name, display_name, icon_name, button, genb)                                                            \
  {name, display_name, icon_name, static_cast<u32>(button), InputBindingInfo::Type::Button, genb}

  // clang-format off
  BUTTON("Up", TRANSLATE_NOOP("DigitalController", "D-Pad Up"), ICON_PF_DPAD_UP, DigitalController::Button::Up, GenericInputBinding::DPadUp),
  BUTTON("Right", TRANSLATE_NOOP("DigitalController", "D-Pad Right"), ICON_PF_DPAD_RIGHT, DigitalController::Button::Right, GenericInputBinding::DPadRight),
  BUTTON("Down", TRANSLATE_NOOP("DigitalController", "D-Pad Down"), ICON_PF_DPAD_DOWN, DigitalController::Button::Down, GenericInputBinding::DPadDown),
  BUTTON("Left", TRANSLATE_NOOP("DigitalController", "D-Pad Left"), ICON_PF_DPAD_LEFT, DigitalController::Button::Left, GenericInputBinding::DPadLeft),
  BUTTON("Triangle", TRANSLATE_NOOP("DigitalController", "Triangle"), ICON_PF_BUTTON_TRIANGLE, DigitalController::Button::Triangle, GenericInputBinding::Triangle),
  BUTTON("Circle", TRANSLATE_NOOP("DigitalController", "Circle"), ICON_PF_BUTTON_CIRCLE, DigitalController::Button::Circle, GenericInputBinding::Circle),
  BUTTON("Cross", TRANSLATE_NOOP("DigitalController", "Cross"), ICON_PF_BUTTON_CROSS, DigitalController::Button::Cross, GenericInputBinding::Cross),
  BUTTON("Square", TRANSLATE_NOOP("DigitalController", "Square"), ICON_PF_BUTTON_SQUARE, DigitalController::Button::Square, GenericInputBinding::Square),
  BUTTON("Select", TRANSLATE_NOOP("DigitalController", "Select"), ICON_PF_SELECT_SHARE, DigitalController::Button::Select, GenericInputBinding::Select),
  BUTTON("Start", TRANSLATE_NOOP("DigitalController", "Start"), ICON_PF_START, DigitalController::Button::Start, GenericInputBinding::Start),
  BUTTON("L1", TRANSLATE_NOOP("DigitalController", "L1"), ICON_PF_LEFT_SHOULDER_L1, DigitalController::Button::L1, GenericInputBinding::L1),
  BUTTON("R1", TRANSLATE_NOOP("DigitalController", "R1"), ICON_PF_RIGHT_SHOULDER_R1, DigitalController::Button::R1, GenericInputBinding::R1),
  BUTTON("L2", TRANSLATE_NOOP("DigitalController", "L2"), ICON_PF_LEFT_TRIGGER_L2, DigitalController::Button::L2, GenericInputBinding::L2),
  BUTTON("R2", TRANSLATE_NOOP("DigitalController", "R2"), ICON_PF_RIGHT_TRIGGER_R2, DigitalController::Button::R2, GenericInputBinding::R2),
// clang-format on

#undef BUTTON
};

const Controller::ControllerInfo DigitalController::INFO = {ControllerType::DigitalController,
                                                            "DigitalController",
                                                            TRANSLATE_NOOP("ControllerType", "Digital Controller"),
                                                            ICON_PF_GAMEPAD_ALT,
                                                            s_binding_info,
                                                            {}};

static const Controller::ControllerBindingInfo s_popn_binding_info[] = {
#define BUTTON(name, display_name, icon_name, button, genb)                                                            \
  {name, display_name, icon_name, static_cast<u32>(button), InputBindingInfo::Type::Button, genb}

  // clang-format off
  BUTTON("LeftWhite", TRANSLATE_NOOP("PopnController", "Left White"), ICON_PF_POPN_WL, DigitalController::Button::Triangle, GenericInputBinding::Triangle),
  BUTTON("LeftYellow", TRANSLATE_NOOP("PopnController", "Left Yellow"), ICON_PF_POPN_YL, DigitalController::Button::Circle, GenericInputBinding::Circle),
  BUTTON("LeftGreen", TRANSLATE_NOOP("PopnController", "Left Green"), ICON_PF_POPN_GL, DigitalController::Button::R1, GenericInputBinding::R1),
  BUTTON("LeftBlue", TRANSLATE_NOOP("PopnController", "Left Blue/Sel"), ICON_PF_POPN_BL, DigitalController::Button::Cross, GenericInputBinding::Cross),
  BUTTON("MiddleRed", TRANSLATE_NOOP("PopnController", "Middle Red/Okay"), ICON_PF_POPN_R, DigitalController::Button::L1, GenericInputBinding::L1),
  BUTTON("RightBlue", TRANSLATE_NOOP("PopnController", "Right Blue/Sel"), ICON_PF_POPN_BR, DigitalController::Button::Square, GenericInputBinding::Square),
  BUTTON("RightGreen", TRANSLATE_NOOP("PopnController", "Right Green"), ICON_PF_POPN_GR, DigitalController::Button::R2, GenericInputBinding::R2),
  BUTTON("RightYellow", TRANSLATE_NOOP("PopnController", "Right Yellow"), ICON_PF_POPN_YR, DigitalController::Button::Up, GenericInputBinding::DPadUp),
  BUTTON("RightWhite", TRANSLATE_NOOP("PopnController", "Right White"), ICON_PF_POPN_WR, DigitalController::Button::L2, GenericInputBinding::L2),
  BUTTON("Select", TRANSLATE_NOOP("PopnController", "Select"), ICON_PF_SELECT_SHARE, DigitalController::Button::Select, GenericInputBinding::Select),
  BUTTON("Start", TRANSLATE_NOOP("PopnController", "Start"), ICON_PF_START, DigitalController::Button::Start, GenericInputBinding::Start),
// clang-format on

#undef BUTTON
};

const Controller::ControllerInfo DigitalController::INFO_POPN = {
  ControllerType::PopnController, "PopnController",    TRANSLATE_NOOP("ControllerType", "Pop'n Controller"),
  ICON_PF_POPN_CONTROLLER,        s_popn_binding_info, {}};
