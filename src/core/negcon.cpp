#include "negcon.h"
#include "common/assert.h"
#include "common/log.h"
#include "host.h"
#include "system.h"
#include "util/state_wrapper.h"
#include <array>
#include <cmath>

// Mapping of Button to index of corresponding bit in m_button_state
static constexpr std::array<u8, static_cast<size_t>(NeGcon::Button::Count)> s_button_indices = {3, 4,  5,  6,
                                                                                                7, 11, 12, 13};
NeGcon::NeGcon(u32 index) : Controller(index)
{
  m_axis_state.fill(0x00);
  m_axis_state[static_cast<u8>(Axis::Steering)] = 0x80;
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
  if (!Controller::DoState(sw, apply_input_state))
    return false;

  u16 button_state = m_button_state;
  sw.Do(&button_state);
  if (apply_input_state)
    m_button_state = button_state;

  sw.Do(&m_transfer_state);
  return true;
}

float NeGcon::GetBindState(u32 index) const
{
  if (index == (static_cast<u32>(Button::Count) + static_cast<u32>(HalfAxis::SteeringLeft)) ||
      index == (static_cast<u32>(Button::Count) + static_cast<u32>(HalfAxis::SteeringRight)))
  {
    return static_cast<float>(m_half_axis_state[index - static_cast<u32>(Button::Count)]) * (1.0f / 255.0f);
  }
  else if (index >= static_cast<u32>(Button::Count))
  {
    // less one because of the two steering axes
    const u32 sub_index = index - (static_cast<u32>(Button::Count) + 1);
    if (sub_index >= m_axis_state.size())
      return 0.0f;

    return static_cast<float>(m_axis_state[sub_index]) * (1.0f / 255.0f);
  }
  else
  {
    const u32 bit = s_button_indices[index];
    return static_cast<float>(((m_button_state >> bit) & 1u) ^ 1u);
  }
}

void NeGcon::SetBindState(u32 index, float value)
{
  // Steering Axis: -1..1 -> 0..255
  if (index == (static_cast<u32>(Button::Count) + static_cast<u32>(HalfAxis::SteeringLeft)) ||
      index == (static_cast<u32>(Button::Count) + static_cast<u32>(HalfAxis::SteeringRight)))
  {
    value = ApplyAnalogDeadzoneSensitivity(m_steering_deadzone, 1.0f, value);

    m_half_axis_state[index - static_cast<u32>(Button::Count)] =
      static_cast<u8>(std::clamp(value * 255.0f, 0.0f, 255.0f));

    // Merge left/right. Seems to be inverted.
    m_axis_state[static_cast<u32>(Axis::Steering)] =
      ((m_half_axis_state[1] != 0) ? (127u + ((m_half_axis_state[1] + 1u) / 2u)) :
                                     (127u - (m_half_axis_state[0] / 2u)));
  }
  else if (index >= static_cast<u32>(Button::Count))
  {
    // less one because of the two steering axes
    const u32 sub_index = index - (static_cast<u32>(Button::Count) + 1);
    if (sub_index >= m_axis_state.size())
      return;

    m_axis_state[sub_index] = static_cast<u8>(std::clamp(value * 255.0f, 0.0f, 255.0f));
  }
  else if (index < static_cast<u32>(Button::Count))
  {
    const u16 bit = u16(1) << s_button_indices[static_cast<u8>(index)];

    if (value >= 0.5f)
    {
      if (m_button_state & bit)
        System::SetRunaheadReplayFlag();

      m_button_state &= ~bit;
    }
    else
    {
      if (!(m_button_state & bit))
        System::SetRunaheadReplayFlag();

      m_button_state |= bit;
    }
  }
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

std::unique_ptr<NeGcon> NeGcon::Create(u32 index)
{
  return std::make_unique<NeGcon>(index);
}

static const Controller::ControllerBindingInfo s_binding_info[] = {
#define BUTTON(name, display_name, button, genb)                                                                       \
  {                                                                                                                    \
    name, display_name, static_cast<u32>(button), Controller::ControllerBindingType::Button, genb                      \
  }
#define AXIS(name, display_name, halfaxis, genb)                                                                       \
  {                                                                                                                    \
    name, display_name, static_cast<u32>(NeGcon::Button::Count) + static_cast<u32>(halfaxis),                          \
      Controller::ControllerBindingType::HalfAxis, genb                                                                \
  }

  BUTTON("Up", "D-Pad Up", NeGcon::Button::Up, GenericInputBinding::DPadUp),
  BUTTON("Right", "D-Pad Right", NeGcon::Button::Right, GenericInputBinding::DPadRight),
  BUTTON("Down", "D-Pad Down", NeGcon::Button::Down, GenericInputBinding::DPadDown),
  BUTTON("Left", "D-Pad Left", NeGcon::Button::Left, GenericInputBinding::DPadLeft),
  BUTTON("Start", "Start", NeGcon::Button::Start, GenericInputBinding::Start),
  BUTTON("A", "A Button", NeGcon::Button::A, GenericInputBinding::Circle),
  BUTTON("B", "B Button", NeGcon::Button::B, GenericInputBinding::Triangle),
  AXIS("I", "I Button", NeGcon::HalfAxis::I, GenericInputBinding::R2),
  AXIS("II", "II Button", NeGcon::HalfAxis::II, GenericInputBinding::L2),
  AXIS("L", "Left Trigger", NeGcon::HalfAxis::L, GenericInputBinding::L1),
  BUTTON("R", "Right Trigger", NeGcon::Button::R, GenericInputBinding::R1),
  AXIS("SteeringLeft", "Steering (Twist) Left", NeGcon::HalfAxis::SteeringLeft, GenericInputBinding::LeftStickLeft),
  AXIS("SteeringRight", "Steering (Twist) Right", NeGcon::HalfAxis::SteeringRight, GenericInputBinding::LeftStickRight),

#undef AXIS
#undef BUTTON
};

static const SettingInfo s_settings[] = {
  {SettingInfo::Type::Float, "SteeringDeadzone", TRANSLATABLE("NeGcon", "Steering Axis Deadzone"),
   TRANSLATABLE("NeGcon", "Sets deadzone size for steering axis."), "0.00f", "0.00f", "0.99f", "0.01f"}};

const Controller::ControllerInfo NeGcon::INFO = {ControllerType::NeGcon,
                                                 "NeGcon",
                                                 TRANSLATABLE("ControllerType", "NeGcon"),
                                                 s_binding_info,
                                                 countof(s_binding_info),
                                                 s_settings,
                                                 countof(s_settings),
                                                 Controller::VibrationCapabilities::NoVibration};

void NeGcon::LoadSettings(SettingsInterface& si, const char* section)
{
  Controller::LoadSettings(si, section);
  m_steering_deadzone = si.GetFloatValue(section, "SteeringDeadzone", 0.10f);
}
