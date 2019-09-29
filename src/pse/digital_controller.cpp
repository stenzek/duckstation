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
  m_transfer_fifo.Clear();
}

bool DigitalController::Transfer(const u8 data_in, u8* data_out)
{
  bool ack;

  switch (data_in)
  {
    case 0x01: // tests if the controller is present
    {
      Log_DebugPrintf("Access");

      // response is hi-z
      *data_out = 0xFF;
      ack = true;
    }
    break;

    case 0x42: // query state
    {
      Log_DebugPrintf("Query state");
      QueryState();
      [[fallthrough]];
    }

    default: // sending response
    {
      if (m_transfer_fifo.IsEmpty())
      {
        Log_WarningPrint("FIFO empty on read");
        *data_out = 0xFF;
        ack = false;
      }
      else
      {
        *data_out = m_transfer_fifo.Pop();
        ack = !m_transfer_fifo.IsEmpty();
      }
    }
    break;
  }

  Log_DebugPrintf("Transfer, data_in=0x%02X, data_out=0x%02X, ack=%s", data_in, *data_out, ack ? "true" : "false");
  return ack;
}

void DigitalController::QueryState()
{
  constexpr u16 ID = 0x5A41;

  m_transfer_fifo.Clear();

  m_transfer_fifo.Push(Truncate8(ID));
  m_transfer_fifo.Push(Truncate8(ID >> 8));

  m_transfer_fifo.Push(Truncate8(m_button_state));      // Digital switches low
  m_transfer_fifo.Push(Truncate8(m_button_state >> 8)); // Digital switches high
}

std::shared_ptr<DigitalController> DigitalController::Create()
{
  return std::make_shared<DigitalController>();
}

