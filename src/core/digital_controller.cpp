#include "digital_controller.h"
#include "common/assert.h"
#include "host_interface.h"
#include "system.h"
#include "util/state_wrapper.h"

DigitalController::DigitalController() = default;

DigitalController::~DigitalController() = default;

ControllerType DigitalController::GetType() const
{
  return ControllerType::DigitalController;
}

std::optional<s32> DigitalController::GetAxisCodeByName(std::string_view axis_name) const
{
  return StaticGetAxisCodeByName(axis_name);
}

std::optional<s32> DigitalController::GetButtonCodeByName(std::string_view button_name) const
{
  return StaticGetButtonCodeByName(button_name);
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

bool DigitalController::GetButtonState(s32 button_code) const
{
  if (button_code < 0 || button_code >= static_cast<s32>(Button::Count))
    return false;

  const u16 bit = u16(1) << static_cast<u8>(button_code);
  return ((m_button_state & bit) == 0);
}

void DigitalController::SetButtonState(Button button, bool pressed)
{
  const u16 bit = u16(1) << static_cast<u8>(button);
  if (pressed)
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

void DigitalController::SetButtonState(s32 button_code, bool pressed)
{
  if (button_code < 0 || button_code >= static_cast<s32>(Button::Count))
    return;

  SetButtonState(static_cast<Button>(button_code), pressed);
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
      *data_out = Truncate8(m_button_state) & GetButtonsLSBMask();
      m_transfer_state = TransferState::ButtonsMSB;
      return true;
    }

    case TransferState::ButtonsMSB:
      *data_out = Truncate8(m_button_state >> 8);
      m_transfer_state = TransferState::Idle;
      return false;

    default:
    {
      UnreachableCode();
      return false;
    }
  }
}

std::unique_ptr<DigitalController> DigitalController::Create()
{
  return std::make_unique<DigitalController>();
}

std::optional<s32> DigitalController::StaticGetAxisCodeByName(std::string_view button_name)
{
  return std::nullopt;
}

std::optional<s32> DigitalController::StaticGetButtonCodeByName(std::string_view button_name)
{
#define BUTTON(name)                                                                                                   \
  if (button_name == #name)                                                                                            \
  {                                                                                                                    \
    return static_cast<s32>(ZeroExtend32(static_cast<u8>(Button::name)));                                              \
  }

  BUTTON(Select);
  BUTTON(L3);
  BUTTON(R3);
  BUTTON(Start);
  BUTTON(Up);
  BUTTON(Right);
  BUTTON(Down);
  BUTTON(Left);
  BUTTON(L2);
  BUTTON(R2);
  BUTTON(L1);
  BUTTON(R1);
  BUTTON(Triangle);
  BUTTON(Circle);
  BUTTON(Cross);
  BUTTON(Square);

  return std::nullopt;

#undef BUTTON
}

Controller::AxisList DigitalController::StaticGetAxisNames()
{
  return {};
}

Controller::ButtonList DigitalController::StaticGetButtonNames()
{
  return {{TRANSLATABLE("DigitalController", "Up"), static_cast<s32>(Button::Up)},
          {TRANSLATABLE("DigitalController", "Down"), static_cast<s32>(Button::Down)},
          {TRANSLATABLE("DigitalController", "Left"), static_cast<s32>(Button::Left)},
          {TRANSLATABLE("DigitalController", "Right"), static_cast<s32>(Button::Right)},
          {TRANSLATABLE("DigitalController", "Select"), static_cast<s32>(Button::Select)},
          {TRANSLATABLE("DigitalController", "Start"), static_cast<s32>(Button::Start)},
          {TRANSLATABLE("DigitalController", "Triangle"), static_cast<s32>(Button::Triangle)},
          {TRANSLATABLE("DigitalController", "Cross"), static_cast<s32>(Button::Cross)},
          {TRANSLATABLE("DigitalController", "Circle"), static_cast<s32>(Button::Circle)},
          {TRANSLATABLE("DigitalController", "Square"), static_cast<s32>(Button::Square)},
          {TRANSLATABLE("DigitalController", "L1"), static_cast<s32>(Button::L1)},
          {TRANSLATABLE("DigitalController", "L2"), static_cast<s32>(Button::L2)},
          {TRANSLATABLE("DigitalController", "R1"), static_cast<s32>(Button::R1)},
          {TRANSLATABLE("DigitalController", "R2"), static_cast<s32>(Button::R2)}};
}

u32 DigitalController::StaticGetVibrationMotorCount()
{
  return 0;
}

Controller::SettingList DigitalController::StaticGetSettings()
{
  static constexpr std::array<SettingInfo, 1> settings = {
    {{SettingInfo::Type::Boolean, "ForcePopnControllerMode",
      TRANSLATABLE("DigitalController", "Force Pop'n Controller Mode"),
      TRANSLATABLE("DigitalController", "Forces the Digital Controller to act as a Pop'n Controller."), "false"}}};
  return SettingList(settings.begin(), settings.end());
}

void DigitalController::LoadSettings(const char* section)
{
  Controller::LoadSettings(section);
  m_popn_controller_mode = g_host_interface->GetBoolSettingValue(section, "ForcePopnControllerMode", false);
}

u8 DigitalController::GetButtonsLSBMask() const
{
  constexpr u8 popn_controller_mask =
    static_cast<u8>(~(u8(1) << static_cast<u8>(Button::Right) | u8(1) << static_cast<u8>(Button::Down) |
                      u8(1) << static_cast<u8>(Button::Left)));
  return m_popn_controller_mode ? popn_controller_mask : 0xFF;
}
