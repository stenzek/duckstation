// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "analog_controller.h"
#include "controller_helpers.h"
#include "settings.h"
#include "system.h"

#include "util/imgui_manager.h"
#include "util/input_manager.h"
#include "util/state_wrapper.h"
#include "util/translation.h"

#include "common/bitutils.h"
#include "common/log.h"
#include "common/string_util.h"

#include "IconsFontAwesome.h"
#include "IconsPromptFont.h"
#include "fmt/format.h"

#include <cmath>

LOG_CHANNEL(Controller);

AnalogController::AnalogController(u32 index) : ConfigModeController(index, ControllerType::AnalogController)
{
  m_axis_state.fill(AXIS_CENTER);
}

AnalogController::~AnalogController() = default;

void AnalogController::Reset()
{
  ResetConfigState();

  if (m_force_analog_on_reset)
  {
    if (CanStartInAnalogMode(ControllerType::AnalogController))
      SetAnalogMode(true, false);
  }
}

bool AnalogController::DoState(StateWrapper& sw, bool apply_input_state)
{
  if (sw.GetVersion() < 105) [[unlikely]]
    return DoLegacyState(sw);

  if (!ConfigModeController::DoState(sw, apply_input_state))
    return false;

  if (sw.IsWriting() || apply_input_state)
  {
    sw.Do(&m_button_state);
    sw.Do(&m_axis_state);
  }
  else
  {
    sw.SkipBytes(sizeof(m_button_state) + sizeof(m_axis_state));
  }

  return !sw.HasError();
}

bool AnalogController::DoLegacyState(StateWrapper& sw)
{
  bool analog_mode = m_analog_mode;
  MotorState motor_state = m_motor_state;

  if (sw.GetVersion() < 76)
  {
    bool unused_legacy_rumble_unlocked = false;

    sw.Do(&analog_mode);
    sw.Do(&m_dualshock_enabled);
    sw.DoEx(&unused_legacy_rumble_unlocked, 44, false);
    sw.Do(&m_configuration_mode);
    sw.SkipBytes(sizeof(u8)); // command_param
    sw.DoEx(&m_status_byte, 55, static_cast<u8>(0x5A));

    // we're never applying input state from legacy states
    if (sw.GetVersion() >= 44)
      sw.SkipBytes(sizeof(m_button_state));

    sw.SkipBytes(sizeof(u8)); // command

    sw.DoEx(&m_rumble_config, 45, {0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu});
    if (sw.GetVersion() >= 45)
      sw.SkipBytes(sizeof(int) + sizeof(int)); // rumble_config_large_motor_index, rumble_config_small_motor_index
    sw.DoEx(&m_analog_toggle_queued, 45, false);
  }
  else
  {
    // command, step, length, buffer, buffer
    sw.SkipBytes(sizeof(u8) + sizeof(u8) + sizeof(u8) + MAX_RESPONSE_LENGTH + MAX_RESPONSE_LENGTH);
    sw.Do(&analog_mode);
    sw.Do(&m_analog_locked);
    sw.Do(&m_dualshock_enabled);
    sw.Do(&m_configuration_mode);
    sw.DoBytes(m_rumble_config.data(), m_rumble_config.size());
    sw.Do(&m_status_byte);

    // we're never applying input state from legacy states
    sw.SkipBytes(sizeof(m_axis_state) + sizeof(m_button_state));
  }

  sw.Do(&motor_state);

  if (sw.IsReading())
  {
    Controller::ResetTransferState();
    FinishLoadState(analog_mode, motor_state);
  }

  return !sw.HasError();
}

float AnalogController::GetBindState(u32 index) const
{
  if (index >= LED_BIND_START_INDEX)
    return BoolToFloat(index == LED_BIND_START_INDEX && m_analog_mode);
  else if (index >= MOTOR_BIND_START_INDEX)
    return GetMotorStrength(index - MOTOR_BIND_START_INDEX);
  else if (index >= HALFAXIS_BIND_START_INDEX)
    return static_cast<float>(m_half_axis_state[index - HALFAXIS_BIND_START_INDEX]) * (1.0f / 255.0f);
  else if (index < static_cast<u32>(Button::Analog))
    return static_cast<float>(((m_button_state >> index) & 1u) ^ 1u);
  else
    return 0.0f;
}

void AnalogController::SetBindState(u32 index, float value)
{
  if (index == static_cast<s32>(Button::Analog))
  {
    // analog toggle
    if (value >= m_button_deadzone)
      ToggleAnalogMode();

    return;
  }
  else if (index >= static_cast<u32>(Button::Count))
  {
    const u32 sub_index = index - static_cast<u32>(Button::Count);
    if (sub_index >= static_cast<u32>(m_half_axis_state.size()))
      return;

    const u8 u8_value = static_cast<u8>(std::clamp(value * m_analog_sensitivity * 255.0f, 0.0f, 255.0f));
    if (u8_value == m_half_axis_state[sub_index])
      return;

    m_half_axis_state[sub_index] = u8_value;

    const auto prev_axis_state = m_axis_state;

    // Both axes of the stick are updated, since the deadzone may have centred the other one.
    if (static_cast<HalfAxis>(sub_index) < HalfAxis::RLeft)
    {
      ControllerHelpers::UpdateStickAxes(
        m_half_axis_state[static_cast<size_t>(HalfAxis::LLeft)],
        m_half_axis_state[static_cast<size_t>(HalfAxis::LRight)], m_half_axis_state[static_cast<size_t>(HalfAxis::LUp)],
        m_half_axis_state[static_cast<size_t>(HalfAxis::LDown)], m_invert_left_stick, m_analog_deadzone,
        &m_axis_state[static_cast<u8>(Axis::LeftX)], &m_axis_state[static_cast<u8>(Axis::LeftY)]);
    }
    else
    {
      ControllerHelpers::UpdateStickAxes(
        m_half_axis_state[static_cast<size_t>(HalfAxis::RLeft)],
        m_half_axis_state[static_cast<size_t>(HalfAxis::RRight)], m_half_axis_state[static_cast<size_t>(HalfAxis::RUp)],
        m_half_axis_state[static_cast<size_t>(HalfAxis::RDown)], m_invert_right_stick, m_analog_deadzone,
        &m_axis_state[static_cast<u8>(Axis::RightX)], &m_axis_state[static_cast<u8>(Axis::RightY)]);
    }

    if (std::memcmp(m_axis_state.data(), prev_axis_state.data(), m_axis_state.size()) != 0)
      System::SetRunaheadReplayFlag(true);

    return;
  }

  SetButtonPressed(m_button_state, u16(1) << static_cast<u8>(index), (value >= m_button_deadzone));
}

u32 AnalogController::GetButtonStateBits() const
{
  // flip bits, native data is active low
  return m_button_state ^ 0xFFFF;
}

std::optional<u32> AnalogController::GetAnalogInputBytes() const
{
  return (m_axis_state[static_cast<u8>(Axis::LeftY)] << 24 | m_axis_state[static_cast<u8>(Axis::LeftX)] << 16 |
          m_axis_state[static_cast<u8>(Axis::RightY)] << 8 | m_axis_state[static_cast<u8>(Axis::RightX)]);
}

u32 AnalogController::GetMotorBindIndex(u32 motor) const
{
  return MOTOR_BIND_START_INDEX + motor;
}

u16 AnalogController::GetExtraButtonMask() const
{
  u16 mask = 0xFFFF;

  static constexpr u8 NEG_THRESHOLD = static_cast<u8>(128.0f - (127.0 * 0.5f));
  static constexpr u8 POS_THRESHOLD = static_cast<u8>(128.0f + (127.0 * 0.5f));

  if (m_analog_dpad_in_digital_mode && !m_analog_mode && !m_configuration_mode)
  {
    const bool left = (m_axis_state[static_cast<u8>(Axis::LeftX)] <= NEG_THRESHOLD);
    const bool right = (m_axis_state[static_cast<u8>(Axis::LeftX)] >= POS_THRESHOLD);
    const bool up = (m_axis_state[static_cast<u8>(Axis::LeftY)] <= NEG_THRESHOLD);
    const bool down = (m_axis_state[static_cast<u8>(Axis::LeftY)] >= POS_THRESHOLD);

    mask = ~((static_cast<u16>(left) << static_cast<u8>(Button::Left)) |
             (static_cast<u16>(right) << static_cast<u8>(Button::Right)) |
             (static_cast<u16>(up) << static_cast<u8>(Button::Up)) |
             (static_cast<u16>(down) << static_cast<u8>(Button::Down)));
  }

  if (m_analog_shoulder_buttons == 2 || (m_analog_shoulder_buttons == 1 && !m_analog_mode && !m_configuration_mode))
  {
    const bool left = (m_axis_state[static_cast<u8>(Axis::RightX)] <= NEG_THRESHOLD);
    const bool right = (m_axis_state[static_cast<u8>(Axis::RightX)] >= POS_THRESHOLD);

    mask &= ~((static_cast<u16>(left) << static_cast<u8>(Button::L1)) |
              (static_cast<u16>(right) << static_cast<u8>(Button::R1)));
  }

  if (m_analog_trigger_buttons == 2 || (m_analog_trigger_buttons == 1 && !m_analog_mode && !m_configuration_mode))
  {
    const bool left = (m_axis_state[static_cast<u8>(Axis::RightX)] <= NEG_THRESHOLD);
    const bool right = (m_axis_state[static_cast<u8>(Axis::RightX)] >= POS_THRESHOLD);

    mask &= ~((static_cast<u16>(left) << static_cast<u8>(Button::L2)) |
              (static_cast<u16>(right) << static_cast<u8>(Button::R2)));
  }

  return mask;
}

void AnalogController::Poll()
{
  // L3/R3 are only sent in analog and config mode.
  const u16 l3r3_released =
    (m_analog_mode || m_configuration_mode) ?
      0 :
      static_cast<u16>((1u << static_cast<u8>(Button::L3)) | (1u << static_cast<u8>(Button::R3)));
  const u16 button_mask = (m_button_state | l3r3_released) & GetExtraButtonMask();
  const u16 button_state = m_disable_socd ? ControllerHelpers::RemoveOpposingDirections(button_mask) : button_mask;
  m_tx_buffer[2] = Truncate8(button_state);
  m_tx_buffer[3] = Truncate8(button_state >> 8);

  if (m_analog_mode || m_configuration_mode)
  {
    m_tx_buffer[4] = m_axis_state[static_cast<u8>(Axis::RightX)];
    m_tx_buffer[5] = m_axis_state[static_cast<u8>(Axis::RightY)];
    m_tx_buffer[6] = m_axis_state[static_cast<u8>(Axis::LeftX)];
    m_tx_buffer[7] = m_axis_state[static_cast<u8>(Axis::LeftY)];
  }
}

std::unique_ptr<AnalogController> AnalogController::Create(u32 index)
{
  return std::make_unique<AnalogController>(index);
}

constinit const Controller::ControllerBindingInfo AnalogController::s_binding_info[] = {
#define BUTTON(name, display_name, icon_name, image_element, button, genb)                                             \
  {name, display_name, icon_name, image_element, static_cast<u32>(button), InputBindingInfo::Type::Button, genb}
#define AXIS(name, display_name, icon_name, image_element, halfaxis, genb)                                             \
  {name,                                                                                                               \
   display_name,                                                                                                       \
   icon_name,                                                                                                          \
   image_element,                                                                                                      \
   HALFAXIS_BIND_START_INDEX + static_cast<u32>(halfaxis),                                                             \
   InputBindingInfo::Type::HalfAxis,                                                                                   \
   genb}
#define MOTOR(name, display_name, icon_name, index, genb)                                                              \
  {name, display_name, icon_name, nullptr, MOTOR_BIND_START_INDEX + index, InputBindingInfo::Type::Motor, genb}
#define MODE_LED(name, display_name, icon_name, image_element, index, genb)                                            \
  {name, display_name, icon_name, image_element, LED_BIND_START_INDEX + index, InputBindingInfo::Type::LED, genb}

  // clang-format off
  BUTTON("Up", TRANSLATE_NOOP("AnalogController", "D-Pad Up"), ICON_PF_DPAD_UP, "Up", AnalogController::Button::Up, GenericInputBinding::DPadUp),
  BUTTON("Right", TRANSLATE_NOOP("AnalogController", "D-Pad Right"), ICON_PF_DPAD_RIGHT, "Right", AnalogController::Button::Right, GenericInputBinding::DPadRight),
  BUTTON("Down", TRANSLATE_NOOP("AnalogController", "D-Pad Down"), ICON_PF_DPAD_DOWN, "Down", AnalogController::Button::Down, GenericInputBinding::DPadDown),
  BUTTON("Left", TRANSLATE_NOOP("AnalogController", "D-Pad Left"), ICON_PF_DPAD_LEFT, "Left", AnalogController::Button::Left, GenericInputBinding::DPadLeft),
  BUTTON("Triangle", TRANSLATE_NOOP("AnalogController", "Triangle"), ICON_PF_BUTTON_TRIANGLE, "Triangle", AnalogController::Button::Triangle, GenericInputBinding::Triangle),
  BUTTON("Circle", TRANSLATE_NOOP("AnalogController", "Circle"), ICON_PF_BUTTON_CIRCLE, "Circle", AnalogController::Button::Circle, GenericInputBinding::Circle),
  BUTTON("Cross", TRANSLATE_NOOP("AnalogController", "Cross"), ICON_PF_BUTTON_CROSS, "Cross", AnalogController::Button::Cross, GenericInputBinding::Cross),
  BUTTON("Square", TRANSLATE_NOOP("AnalogController", "Square"), ICON_PF_BUTTON_SQUARE, "Square", AnalogController::Button::Square, GenericInputBinding::Square),
  BUTTON("Select", TRANSLATE_NOOP("AnalogController", "Select"), ICON_PF_SELECT_SHARE, "Select", AnalogController::Button::Select, GenericInputBinding::Select),
  BUTTON("Start", TRANSLATE_NOOP("AnalogController", "Start"),ICON_PF_START, "Start", AnalogController::Button::Start, GenericInputBinding::Start),
  BUTTON("Analog", TRANSLATE_NOOP("AnalogController", "Analog Toggle"), ICON_PF_ANALOG_LEFT_RIGHT, "Analog", AnalogController::Button::Analog, GenericInputBinding::System),
  BUTTON("L1", TRANSLATE_NOOP("AnalogController", "L1"), ICON_PF_LEFT_SHOULDER_L1, "L1", AnalogController::Button::L1, GenericInputBinding::L1),
  BUTTON("R1", TRANSLATE_NOOP("AnalogController", "R1"), ICON_PF_RIGHT_SHOULDER_R1, "R1", AnalogController::Button::R1, GenericInputBinding::R1),
  BUTTON("L2", TRANSLATE_NOOP("AnalogController", "L2"), ICON_PF_LEFT_TRIGGER_L2, "L2", AnalogController::Button::L2, GenericInputBinding::L2),
  BUTTON("R2", TRANSLATE_NOOP("AnalogController", "R2"), ICON_PF_RIGHT_TRIGGER_R2, "R2", AnalogController::Button::R2, GenericInputBinding::R2),
  BUTTON("L3", TRANSLATE_NOOP("AnalogController", "L3"), ICON_PF_LEFT_ANALOG_CLICK, "LStick", AnalogController::Button::L3, GenericInputBinding::L3),
  BUTTON("R3", TRANSLATE_NOOP("AnalogController", "R3"), ICON_PF_RIGHT_ANALOG_CLICK, "RStick", AnalogController::Button::R3, GenericInputBinding::R3),

  AXIS("LLeft", TRANSLATE_NOOP("AnalogController", "Left Stick Left"), ICON_PF_LEFT_ANALOG_LEFT, "LStick", AnalogController::HalfAxis::LLeft, GenericInputBinding::LeftStickLeft),
  AXIS("LRight", TRANSLATE_NOOP("AnalogController", "Left Stick Right"), ICON_PF_LEFT_ANALOG_RIGHT, "LStick", AnalogController::HalfAxis::LRight, GenericInputBinding::LeftStickRight),
  AXIS("LDown", TRANSLATE_NOOP("AnalogController", "Left Stick Down"), ICON_PF_LEFT_ANALOG_DOWN, "LStick", AnalogController::HalfAxis::LDown, GenericInputBinding::LeftStickDown),
  AXIS("LUp", TRANSLATE_NOOP("AnalogController", "Left Stick Up"), ICON_PF_LEFT_ANALOG_UP, "LStick", AnalogController::HalfAxis::LUp, GenericInputBinding::LeftStickUp),
  AXIS("RLeft", TRANSLATE_NOOP("AnalogController", "Right Stick Left"), ICON_PF_RIGHT_ANALOG_LEFT, "RStick", AnalogController::HalfAxis::RLeft, GenericInputBinding::RightStickLeft),
  AXIS("RRight", TRANSLATE_NOOP("AnalogController", "Right Stick Right"), ICON_PF_RIGHT_ANALOG_RIGHT, "RStick", AnalogController::HalfAxis::RRight, GenericInputBinding::RightStickRight),
  AXIS("RDown", TRANSLATE_NOOP("AnalogController", "Right Stick Down"), ICON_PF_RIGHT_ANALOG_DOWN, "RStick", AnalogController::HalfAxis::RDown, GenericInputBinding::RightStickDown),
  AXIS("RUp", TRANSLATE_NOOP("AnalogController", "Right Stick Up"), ICON_PF_RIGHT_ANALOG_UP, "RStick", AnalogController::HalfAxis::RUp, GenericInputBinding::RightStickUp),

  MOTOR("LargeMotor", TRANSLATE_NOOP("AnalogController", "Large Motor"), ICON_PF_VIBRATION_L, LargeMotor, GenericInputBinding::LargeMotor),
  MOTOR("SmallMotor", TRANSLATE_NOOP("AnalogController", "Small Motor"), ICON_PF_VIBRATION, SmallMotor, GenericInputBinding::SmallMotor),

  MODE_LED("AnalogLED", TRANSLATE_NOOP("AnalogController", "Analog LED"), ICON_PF_LED, "AnalogLED", 0, GenericInputBinding::ModeLED),

// clang-format on

#undef MOTOR
#undef AXIS
#undef BUTTON
#undef MODE_LED
};

static constexpr const char* s_invert_settings[] = {
  TRANSLATE_NOOP("AnalogController", "Not Inverted"), TRANSLATE_NOOP("AnalogController", "Invert Left/Right"),
  TRANSLATE_NOOP("AnalogController", "Invert Up/Down"),
  TRANSLATE_NOOP("AnalogController", "Invert Left/Right + Up/Down"), nullptr};

static constexpr const char* s_shoulder_settings[] = {
  TRANSLATE_NOOP("AnalogController", "Never"), TRANSLATE_NOOP("AnalogController", "Digital Mode Only"),
  TRANSLATE_NOOP("AnalogController", "Analog and Digital Modes"), nullptr};

static constexpr SettingInfo s_settings[] = {
  {SettingInfo::Type::Boolean, "ForceAnalogOnReset",
   TRANSLATE_NOOP("AnalogController", "Automatically Enable Analog Mode"),
   TRANSLATE_NOOP("AnalogController", "Forces the controller to analog mode when the game is started/restarted."),
   "true", nullptr, nullptr, nullptr, nullptr, nullptr, 0.0f},
  {SettingInfo::Type::Boolean, "AnalogDPadInDigitalMode",
   TRANSLATE_NOOP("AnalogController", "Use Left Analog for D-Pad in Digital Mode"),
   TRANSLATE_NOOP(
     "AnalogController",
     "Allows you to use the left analog stick to control the d-pad in digital mode, as well as the buttons."),
   "true", nullptr, nullptr, nullptr, nullptr, nullptr, 0.0f},
  {SettingInfo::Type::Boolean, "DisableSOCD",
   TRANSLATE_NOOP("AnalogController", "Disable Simultaneous Opposing Cardinal Directions"),
   TRANSLATE_NOOP("AnalogController",
                  "Prevents concurrent left/right or up/down inputs from being presented to the game."),
   "false", nullptr, nullptr, nullptr, nullptr, nullptr, 0.0f},
  {SettingInfo::Type::IntegerList, "AnalogShoulderButtons",
   TRANSLATE_NOOP("AnalogController", "Use Right Analog for Shoulder Buttons"),
   TRANSLATE_NOOP(
     "AnalogController",
     "Allows you to use the right analog stick to control the shoulder buttons (L1/R1), as well as the buttons."),
   "0", "0", "2", nullptr, nullptr, s_shoulder_settings, 0.0f},
  {SettingInfo::Type::IntegerList, "AnalogTriggerButtons",
   TRANSLATE_NOOP("AnalogController", "Use Right Analog for Trigger Buttons"),
   TRANSLATE_NOOP(
     "AnalogController",
     "Allows you to use the right analog stick to control the trigger buttons (L2/R2), as well as the buttons."),
   "0", "0", "2", nullptr, nullptr, s_shoulder_settings, 0.0f},
  {SettingInfo::Type::Float, "AnalogDeadzone", TRANSLATE_NOOP("AnalogController", "Analog Deadzone"),
   TRANSLATE_NOOP("AnalogController",
                  "Sets the analog stick deadzone, i.e. the fraction of the stick movement which will be ignored."),
   "0", "0", "1", "0.01", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Float, "AnalogSensitivity", TRANSLATE_NOOP("AnalogController", "Analog Sensitivity"),
   TRANSLATE_NOOP(
     "AnalogController",
     "Sets the analog stick axis scaling factor. A value between 130% and 140% is recommended when using recent "
     "controllers, e.g. DualShock 4, Xbox One Controller."),
   "1.33", "0.01", "2", "0.01", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Float, "ButtonDeadzone", TRANSLATE_NOOP("AnalogController", "Button/Trigger Deadzone"),
   TRANSLATE_NOOP("AnalogController", "Sets the deadzone for activating buttons/triggers, "
                                      "i.e. the fraction of the trigger which will be ignored."),
   "0.25", "0.01", "1", "0.01", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Integer, "LargeMotorVibrationBias",
   TRANSLATE_NOOP("AnalogController", "Large Motor Vibration Bias"),
   TRANSLATE_NOOP("AnalogController",
                  "Sets the bias value for the large vibration motor. If vibration in some games is too weak or not "
                  "functioning, try increasing this value. Negative values will decrease the intensity of vibration."),
   "8", "-255", "255", "1", "%d", nullptr, 1.0f},
  {SettingInfo::Type::Integer, "SmallMotorVibrationBias",
   TRANSLATE_NOOP("AnalogController", "Small Motor Vibration Bias"),
   TRANSLATE_NOOP("AnalogController",
                  "Sets the bias value for the small vibration motor. If vibration in some games is too weak or not "
                  "functioning, try increasing this value. Negative values will decrease the intensity of vibration."),
   "8", "-255", "255", "1", "%d", nullptr, 1.0f},
  {SettingInfo::Type::IntegerList, "InvertLeftStick", TRANSLATE_NOOP("AnalogController", "Invert Left Stick"),
   TRANSLATE_NOOP("AnalogController", "Inverts the direction of the left analog stick."), "0", "0", "3", nullptr,
   nullptr, s_invert_settings, 0.0f},
  {SettingInfo::Type::IntegerList, "InvertRightStick", TRANSLATE_NOOP("AnalogController", "Invert Right Stick"),
   TRANSLATE_NOOP("AnalogController", "Inverts the direction of the right analog stick."), "0", "0", "3", nullptr,
   nullptr, s_invert_settings, 0.0f},
};

const Controller::ControllerInfo AnalogController::INFO = {ControllerType::AnalogController,
                                                           "AnalogController",
                                                           TRANSLATE_NOOP("ControllerType", "Analog Controller"),
                                                           ICON_PF_GAMEPAD_ALT,
                                                           "images/controllers/analog_controller.svg",
                                                           s_binding_info,
                                                           s_settings};

void AnalogController::LoadSettings(const SettingsInterface& si, const char* section, bool initial)
{
  m_force_analog_on_reset = si.GetBoolValue(section, "ForceAnalogOnReset", true);
  m_analog_dpad_in_digital_mode = si.GetBoolValue(section, "AnalogDPadInDigitalMode", true);
  m_disable_socd = si.GetBoolValue(section, "DisableSOCD", false);
  m_analog_shoulder_buttons = static_cast<u8>(si.GetUIntValue(section, "AnalogShoulderButtons", 0u));
  m_analog_trigger_buttons = static_cast<u8>(si.GetUIntValue(section, "AnalogTriggerButtons", 0u));
  m_analog_deadzone = std::clamp(si.GetFloatValue(section, "AnalogDeadzone", DEFAULT_STICK_DEADZONE), 0.0f, 1.0f);
  m_analog_sensitivity =
    std::clamp(si.GetFloatValue(section, "AnalogSensitivity", DEFAULT_STICK_SENSITIVITY), 0.01f, 3.0f);
  m_button_deadzone = std::clamp(si.GetFloatValue(section, "ButtonDeadzone", DEFAULT_BUTTON_DEADZONE), 0.01f, 1.0f);
  m_vibration_bias[LargeMotor] =
    static_cast<s16>(std::clamp(si.GetIntValue(section, "LargeMotorVibrationBias", DEFAULT_VIBRATION_BIAS), -255, 255));
  m_vibration_bias[SmallMotor] =
    static_cast<s16>(std::clamp(si.GetIntValue(section, "SmallMotorVibrationBias", DEFAULT_VIBRATION_BIAS), -255, 255));
  m_invert_left_stick = static_cast<u8>(si.GetUIntValue(section, "InvertLeftStick", 0u));
  m_invert_right_stick = static_cast<u8>(si.GetUIntValue(section, "InvertRightStick", 0u));
}
