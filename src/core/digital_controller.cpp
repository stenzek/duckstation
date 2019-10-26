#include "digital_controller.h"
#include "YBaseLib/Log.h"
Log_SetChannel(DigitalController);

DigitalController::DigitalController() = default;

DigitalController::~DigitalController() = default;

void DigitalController::SetButtonState(Button button, bool pressed)
{
  if (pressed)
    m_button_state &= ~(u16(1) << static_cast<u8>(button));
  else
    m_button_state |= u16(1) << static_cast<u8>(button);
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

std::shared_ptr<DigitalController> DigitalController::Create()
{
  return std::make_shared<DigitalController>();
}
