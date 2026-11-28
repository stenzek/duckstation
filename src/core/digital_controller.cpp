// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "digital_controller.h"
#include "controller_helpers.h"
#include "system.h"

#include "util/state_wrapper.h"
#include "util/translation.h"

#include "IconsPromptFont.h"

#include "common/assert.h"
#include "common/bitutils.h"
#include "common/settings_interface.h"

DigitalController::DigitalController(u32 index, ControllerType type, u16 button_mask)
  : Controller(index, type), m_button_mask(button_mask)
{
}

DigitalController::~DigitalController() = default;

void DigitalController::Reset()
{
  ResetTransferState();
}

bool DigitalController::DoState(StateWrapper& sw, bool apply_input_state)
{
  if (sw.GetVersion() >= 105 && !Controller::DoState(sw, apply_input_state))
    return false;

  if (sw.IsWriting() || apply_input_state)
    sw.Do(&m_button_state);
  else
    sw.SkipBytes(sizeof(m_button_state));

  if (sw.GetVersion() < 105)
    SkipLegacyTransferState(sw);

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

  SetButtonPressed(m_button_state, u16(1) << static_cast<u8>(index), (value >= 0.5f));
}

u32 DigitalController::GetButtonStateBits() const
{
  return m_button_state ^ 0xFFFF;
}

u8 DigitalController::GetIDByte() const
{
  return 0x41;
}

bool DigitalController::BeginCommand(u8 command)
{
  if (command != 0x42)
    return false;

  // Can skip opposing directions in the MSB since they're all in the LSB.
  const u16 button_state = m_button_state & m_button_mask;
  m_tx_buffer[2] = Truncate8(
    (m_disable_socd ? ControllerHelpers::RemoveOpposingDirections(m_button_state) : m_button_state) & m_button_mask);
  m_tx_buffer[3] = Truncate8(button_state >> 8);
  return true;
}

void DigitalController::LoadSettings(const SettingsInterface& si, const char* section, bool initial)
{
  m_disable_socd = si.GetBoolValue(section, "DisableSOCD", false);
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

  return std::make_unique<DigitalController>(index, type, mask);
}

static constexpr SettingInfo s_settings[] = {
  {SettingInfo::Type::Boolean, "DisableSOCD",
   TRANSLATE_NOOP("DigitalController", "Disable Simultaneous Opposing Cardinal Directions"),
   TRANSLATE_NOOP("DigitalController",
                  "Prevents concurrent left/right or up/down inputs from being presented to the game."),
   "false", nullptr, nullptr, nullptr, nullptr, nullptr, 0.0f},
};

static const Controller::ControllerBindingInfo s_binding_info[] = {
#define BUTTON(name, display_name, icon_name, image_element, button, genb)                                             \
  {name, display_name, icon_name, image_element, static_cast<u32>(button), InputBindingInfo::Type::Button, genb}

  // clang-format off
  BUTTON("Up", TRANSLATE_NOOP("DigitalController", "D-Pad Up"), ICON_PF_DPAD_UP, "Up", DigitalController::Button::Up, GenericInputBinding::DPadUp),
  BUTTON("Right", TRANSLATE_NOOP("DigitalController", "D-Pad Right"), ICON_PF_DPAD_RIGHT, "Right", DigitalController::Button::Right, GenericInputBinding::DPadRight),
  BUTTON("Down", TRANSLATE_NOOP("DigitalController", "D-Pad Down"), ICON_PF_DPAD_DOWN, "Down", DigitalController::Button::Down, GenericInputBinding::DPadDown),
  BUTTON("Left", TRANSLATE_NOOP("DigitalController", "D-Pad Left"), ICON_PF_DPAD_LEFT, "Left", DigitalController::Button::Left, GenericInputBinding::DPadLeft),
  BUTTON("Triangle", TRANSLATE_NOOP("DigitalController", "Triangle"), ICON_PF_BUTTON_TRIANGLE, "Triangle", DigitalController::Button::Triangle, GenericInputBinding::Triangle),
  BUTTON("Circle", TRANSLATE_NOOP("DigitalController", "Circle"), ICON_PF_BUTTON_CIRCLE, "Circle", DigitalController::Button::Circle, GenericInputBinding::Circle),
  BUTTON("Cross", TRANSLATE_NOOP("DigitalController", "Cross"), ICON_PF_BUTTON_CROSS, "Cross", DigitalController::Button::Cross, GenericInputBinding::Cross),
  BUTTON("Square", TRANSLATE_NOOP("DigitalController", "Square"), ICON_PF_BUTTON_SQUARE, "Square", DigitalController::Button::Square, GenericInputBinding::Square),
  BUTTON("Select", TRANSLATE_NOOP("DigitalController", "Select"), ICON_PF_SELECT_SHARE, "Select", DigitalController::Button::Select, GenericInputBinding::Select),
  BUTTON("Start", TRANSLATE_NOOP("DigitalController", "Start"), ICON_PF_START, "Start", DigitalController::Button::Start, GenericInputBinding::Start),
  BUTTON("L1", TRANSLATE_NOOP("DigitalController", "L1"), ICON_PF_LEFT_SHOULDER_L1, "L1", DigitalController::Button::L1, GenericInputBinding::L1),
  BUTTON("R1", TRANSLATE_NOOP("DigitalController", "R1"), ICON_PF_RIGHT_SHOULDER_R1, "R1", DigitalController::Button::R1, GenericInputBinding::R1),
  BUTTON("L2", TRANSLATE_NOOP("DigitalController", "L2"), ICON_PF_LEFT_TRIGGER_L2, "L2", DigitalController::Button::L2, GenericInputBinding::L2),
  BUTTON("R2", TRANSLATE_NOOP("DigitalController", "R2"), ICON_PF_RIGHT_TRIGGER_R2, "R2", DigitalController::Button::R2, GenericInputBinding::R2),
// clang-format on

#undef BUTTON
};

const Controller::ControllerInfo DigitalController::INFO = {
  ControllerType::DigitalController,
  "DigitalController",
  TRANSLATE_NOOP("ControllerType", "Digital Controller"),
  ICON_PF_GAMEPAD_ALT,
  "images/controllers/digital_controller.svg",
  s_binding_info,
  s_settings,
};

static const Controller::ControllerBindingInfo s_popn_binding_info[] = {
#define BUTTON(name, display_name, icon_name, image_element, button, genb)                                             \
  {name, display_name, icon_name, image_element, static_cast<u32>(button), InputBindingInfo::Type::Button, genb}

  // clang-format off
  BUTTON("LeftWhite", TRANSLATE_NOOP("PopnController", "Left White"), ICON_PF_POPN_WL, "LeftWhite", DigitalController::Button::Triangle, GenericInputBinding::Triangle),
  BUTTON("LeftYellow", TRANSLATE_NOOP("PopnController", "Left Yellow"), ICON_PF_POPN_YL, "LeftYellow", DigitalController::Button::Circle, GenericInputBinding::Circle),
  BUTTON("LeftGreen", TRANSLATE_NOOP("PopnController", "Left Green"), ICON_PF_POPN_GL, "LeftGreen", DigitalController::Button::R1, GenericInputBinding::R1),
  BUTTON("LeftBlue", TRANSLATE_NOOP("PopnController", "Left Blue/Sel"), ICON_PF_POPN_BL, "LeftBlue", DigitalController::Button::Cross, GenericInputBinding::Cross),
  BUTTON("MiddleRed", TRANSLATE_NOOP("PopnController", "Middle Red/Okay"), ICON_PF_POPN_R, "MiddleRed", DigitalController::Button::L1, GenericInputBinding::L1),
  BUTTON("RightBlue", TRANSLATE_NOOP("PopnController", "Right Blue/Sel"), ICON_PF_POPN_BR, "RightBlue", DigitalController::Button::Square, GenericInputBinding::Square),
  BUTTON("RightGreen", TRANSLATE_NOOP("PopnController", "Right Green"), ICON_PF_POPN_GR, "RightGreen", DigitalController::Button::R2, GenericInputBinding::R2),
  BUTTON("RightYellow", TRANSLATE_NOOP("PopnController", "Right Yellow"), ICON_PF_POPN_YR, "RightYellow", DigitalController::Button::Up, GenericInputBinding::DPadUp),
  BUTTON("RightWhite", TRANSLATE_NOOP("PopnController", "Right White"), ICON_PF_POPN_WR, "RightWhite", DigitalController::Button::L2, GenericInputBinding::L2),
  BUTTON("Select", TRANSLATE_NOOP("PopnController", "Select"), ICON_PF_SELECT_SHARE, "Select", DigitalController::Button::Select, GenericInputBinding::Select),
  BUTTON("Start", TRANSLATE_NOOP("PopnController", "Start"), ICON_PF_START, "Start", DigitalController::Button::Start, GenericInputBinding::Start),
// clang-format on

#undef BUTTON
};

const Controller::ControllerInfo DigitalController::INFO_POPN = {
  ControllerType::PopnController,
  "PopnController",
  TRANSLATE_NOOP("ControllerType", "Pop'n Controller"),
  ICON_PF_POPN_CONTROLLER,
  "images/controllers/popn_controller.svg",
  s_popn_binding_info,
  {},
};
