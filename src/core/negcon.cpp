#include "negcon.h"
#include "common/assert.h"
#include "common/log.h"
#include "host_interface.h"
#include "util/state_wrapper.h"
#include <array>
#include <cmath>

NeGcon::NeGcon()
{
  m_axis_state.fill(0x00);
  m_axis_state[static_cast<u8>(Axis::Steering)] = 0x80;
}

NeGcon::~NeGcon() = default;

ControllerType NeGcon::GetType() const
{
  return ControllerType::NeGcon;
}

std::optional<s32> NeGcon::GetAxisCodeByName(std::string_view axis_name) const
{
  return StaticGetAxisCodeByName(axis_name);
}

std::optional<s32> NeGcon::GetButtonCodeByName(std::string_view button_name) const
{
  return StaticGetButtonCodeByName(button_name);
}

void NeGcon::Reset()
{
  m_transfer_state = TransferState::Idle;
}

bool NeGcon::DoState(StateWrapper& sw, bool apply_input_state)
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

float NeGcon::GetAxisState(s32 axis_code) const
{
  if (axis_code < 0 || axis_code >= static_cast<s32>(Axis::Count))
    return 0.0f;

  if (axis_code == static_cast<s32>(Axis::Steering))
    return (((static_cast<float>(m_axis_state[static_cast<s32>(Axis::Steering)]) / 255.0f) * 2.0f) - 1.0f);
  else
    return (static_cast<float>(m_axis_state[static_cast<s32>(axis_code)]) / 255.0f);
}

void NeGcon::SetAxisState(s32 axis_code, float value)
{
  if (axis_code < 0 || axis_code >= static_cast<s32>(Axis::Count))
    return;

  // Steering Axis: -1..1 -> 0..255
  if (axis_code == static_cast<s32>(Axis::Steering))
  {
    const float float_value =
      (std::abs(value) < m_steering_deadzone) ?
        0.0f :
        std::copysign((std::abs(value) - m_steering_deadzone) / (1.0f - m_steering_deadzone), value);
    const u8 u8_value = static_cast<u8>(std::clamp(std::round(((float_value + 1.0f) / 2.0f) * 255.0f), 0.0f, 255.0f));

    SetAxisState(static_cast<Axis>(axis_code), u8_value);

    return;
  }

  // I, II, L: -1..1 -> 0..255
  const u8 u8_value = static_cast<u8>(std::clamp(value * 255.0f, 0.0f, 255.0f));

  SetAxisState(static_cast<Axis>(axis_code), u8_value);
}

void NeGcon::SetAxisState(Axis axis, u8 value)
{
  m_axis_state[static_cast<u8>(axis)] = value;
}

bool NeGcon::GetButtonState(s32 button_code) const
{
  if (button_code < 0 || button_code >= static_cast<s32>(Button::Count))
    return false;

  const u16 bit = u16(1) << static_cast<u8>(button_code);
  return ((m_button_state & bit) == 0);
}

void NeGcon::SetButtonState(s32 button_code, bool pressed)
{
  if (button_code < 0 || button_code >= static_cast<s32>(Button::Count))
    return;

  SetButtonState(static_cast<Button>(button_code), pressed);
}

void NeGcon::SetButtonState(Button button, bool pressed)
{
  // Mapping of Button to index of corresponding bit in m_button_state
  static constexpr std::array<u8, static_cast<size_t>(Button::Count)> indices = {3, 4, 5, 6, 7, 11, 12, 13};

  if (pressed)
    m_button_state &= ~(u16(1) << indices[static_cast<u8>(button)]);
  else
    m_button_state |= u16(1) << indices[static_cast<u8>(button)];
}

u32 NeGcon::GetButtonStateBits() const
{
  return m_button_state ^ 0xFFFF;
}

std::optional<u32> NeGcon::GetAnalogInputBytes() const
{
  return m_axis_state[static_cast<size_t>(Axis::L)] << 24 | m_axis_state[static_cast<size_t>(Axis::II)] << 16 |
         m_axis_state[static_cast<size_t>(Axis::I)] << 8 | m_axis_state[static_cast<size_t>(Axis::Steering)];
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
      *data_out = Truncate8(m_button_state);
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

std::unique_ptr<NeGcon> NeGcon::Create()
{
  return std::make_unique<NeGcon>();
}

std::optional<s32> NeGcon::StaticGetAxisCodeByName(std::string_view axis_name)
{
#define AXIS(name)                                                                                                     \
  if (axis_name == #name)                                                                                              \
  {                                                                                                                    \
    return static_cast<s32>(ZeroExtend32(static_cast<u8>(Axis::name)));                                                \
  }

  AXIS(Steering);
  AXIS(I);
  AXIS(II);
  AXIS(L);

  return std::nullopt;

#undef AXIS
}

std::optional<s32> NeGcon::StaticGetButtonCodeByName(std::string_view button_name)
{
#define BUTTON(name)                                                                                                   \
  if (button_name == #name)                                                                                            \
  {                                                                                                                    \
    return static_cast<s32>(ZeroExtend32(static_cast<u8>(Button::name)));                                              \
  }

  BUTTON(Up);
  BUTTON(Down);
  BUTTON(Left);
  BUTTON(Right);
  BUTTON(A);
  BUTTON(B);
  BUTTON(R);
  BUTTON(Start);

  return std::nullopt;

#undef BUTTON
}

Controller::AxisList NeGcon::StaticGetAxisNames()
{
  return {{TRANSLATABLE("NeGcon", "Steering"), static_cast<s32>(Axis::Steering), AxisType::Full},
          {TRANSLATABLE("NeGcon", "I"), static_cast<s32>(Axis::I), AxisType::Half},
          {TRANSLATABLE("NeGcon", "II"), static_cast<s32>(Axis::II), AxisType::Half},
          {TRANSLATABLE("NeGcon", "L"), static_cast<s32>(Axis::L), AxisType::Half}};
}

Controller::ButtonList NeGcon::StaticGetButtonNames()
{
  return {{TRANSLATABLE("NeGcon", "Up"), static_cast<s32>(Button::Up)},
          {TRANSLATABLE("NeGcon", "Down"), static_cast<s32>(Button::Down)},
          {TRANSLATABLE("NeGcon", "Left"), static_cast<s32>(Button::Left)},
          {TRANSLATABLE("NeGcon", "Right"), static_cast<s32>(Button::Right)},
          {TRANSLATABLE("NeGcon", "A"), static_cast<s32>(Button::A)},
          {TRANSLATABLE("NeGcon", "B"), static_cast<s32>(Button::B)},
          {TRANSLATABLE("NeGcon", "R"), static_cast<s32>(Button::R)},
          {TRANSLATABLE("NeGcon", "Start"), static_cast<s32>(Button::Start)}};
}

u32 NeGcon::StaticGetVibrationMotorCount()
{
  return 0;
}

Controller::SettingList NeGcon::StaticGetSettings()
{
  static constexpr std::array<SettingInfo, 1> settings = {
    {{SettingInfo::Type::Float, "SteeringDeadzone", TRANSLATABLE("NeGcon", "Steering Axis Deadzone"),
      TRANSLATABLE("NeGcon", "Sets deadzone size for steering axis."), "0.00f", "0.00f", "0.99f", "0.01f"}}};

  return SettingList(settings.begin(), settings.end());
}

void NeGcon::LoadSettings(const char* section)
{
  Controller::LoadSettings(section);
  m_steering_deadzone = g_host_interface->GetFloatSettingValue(section, "SteeringDeadzone", 0.10f);
}
