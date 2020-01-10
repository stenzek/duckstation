#include "digital_controller.h"
#include "common/assert.h"

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

void DigitalController::SetAxisState(s32 axis_code, float value) {}

void DigitalController::SetButtonState(Button button, bool pressed)
{
  if (pressed)
    m_button_state &= ~(u16(1) << static_cast<u8>(button));
  else
    m_button_state |= u16(1) << static_cast<u8>(button);
}

void DigitalController::SetButtonState(s32 button_code, bool pressed)
{
  if (button_code < 0 || button_code >= static_cast<s32>(Button::Count))
    return;

  SetButtonState(static_cast<Button>(button_code), pressed);
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
      // ack when sent 0x01, send ID for 0x42
      if (data_in == 0x42)
      {
        *data_out = Truncate8(ID);
        m_transfer_state = TransferState::IDMSB;
        return true;
      }
      else
      {
        *data_out = 0xFF;
        return (data_in == 0x01);
      }
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
#define B(n)                                                                                                           \
  {                                                                                                                    \
#n, static_cast < s32>(Button::n)                                                                                  \
  }
  return {B(Up),    B(Down),   B(Left),   B(Right), B(Select), B(Start), B(Triangle),
          B(Cross), B(Circle), B(Square), B(L1),    B(L2),     B(R1),    B(R2)};
#undef B
}
