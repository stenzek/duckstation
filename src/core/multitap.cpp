#include "multitap.h"
#include "common/log.h"
#include "common/types.h"
#include "controller.h"
#include "memory_card.h"
#include "pad.h"
#include "util/state_wrapper.h"
Log_SetChannel(Multitap);

Multitap::Multitap()
{
  Reset();
}

void Multitap::Reset()
{
  m_transfer_state = TransferState::Idle;
  m_selected_slot = 0;
  m_controller_transfer_step = 0;
  m_transfer_all_controllers = false;
  m_invalid_transfer_all_command = false;
  m_current_controller_done = false;
  m_transfer_buffer.fill(0xFF);
}

void Multitap::SetEnable(bool enable, u32 base_index)
{
  if (m_enabled != enable || m_base_index != base_index)
  {
    m_enabled = enable;
    m_base_index = base_index;
    Reset();
  }
}

bool Multitap::DoState(StateWrapper& sw)
{
  sw.Do(&m_transfer_state);
  sw.Do(&m_selected_slot);
  sw.Do(&m_controller_transfer_step);
  sw.Do(&m_invalid_transfer_all_command);
  sw.Do(&m_transfer_all_controllers);
  sw.Do(&m_current_controller_done);
  sw.Do(&m_transfer_buffer);

  return !sw.HasError();
}

void Multitap::ResetTransferState()
{
  m_transfer_state = TransferState::Idle;
  m_selected_slot = 0;
  m_controller_transfer_step = 0;
  m_current_controller_done = false;

  // Don't reset m_transfer_all_controllers here, since it's queued up for the next transfer sequence
  // Controller and memory card transfer resets are handled in the Pad class
}

bool Multitap::TransferController(u32 slot, const u8 data_in, u8* data_out) const
{
  Controller* const selected_controller = g_pad.GetController(m_base_index + slot);
  if (!selected_controller)
  {
    *data_out = 0xFF;
    return false;
  }

  return selected_controller->Transfer(data_in, data_out);
}

bool Multitap::TransferMemoryCard(u32 slot, const u8 data_in, u8* data_out) const
{
  MemoryCard* const selected_memcard = g_pad.GetMemoryCard(m_base_index + slot);
  if (!selected_memcard)
  {
    *data_out = 0xFF;
    return false;
  }

  return selected_memcard->Transfer(data_in, data_out);
}

bool Multitap::Transfer(const u8 data_in, u8* data_out)
{
  bool ack = false;
  switch (m_transfer_state)
  {
    case TransferState::Idle:
    {
      switch (data_in)
      {
        case 0x81:
        case 0x82:
        case 0x83:
        case 0x84:
        {
          m_selected_slot = (data_in & 0x0F) - 1u;
          ack = TransferMemoryCard(m_selected_slot, 0x81, data_out);

          if (ack)
            m_transfer_state = TransferState::MemoryCard;
        }
        break;

        case 0x01:
        case 0x02:
        case 0x03:
        case 0x04:
        {
          m_selected_slot = data_in - 1u;
          ack = TransferController(m_selected_slot, 0x01, data_out);

          if (ack)
          {
            m_transfer_state = TransferState::ControllerCommand;

            if (m_transfer_all_controllers)
            {
              // Send access byte to remaining controllers for this transfer mode
              u8 dummy_value;
              for (u32 i = 0; i < 4; i++)
              {
                if (i != m_selected_slot)
                  TransferController(i, 0x01, &dummy_value);
              }
            }
          }
        }
        break;

        default:
        {
          *data_out = 0xFF;
          ack = false;
        }
        break;
      }
    }
    break;

    case TransferState::MemoryCard:
    {
      ack = TransferMemoryCard(m_selected_slot, data_in, data_out);

      if (!ack)
      {
        Log_DevPrintf("Memory card transfer ended");
        m_transfer_state = TransferState::Idle;
      }
    }
    break;

    case TransferState::ControllerCommand:
    {
      if (m_controller_transfer_step == 0) // Command byte
      {
        if (m_transfer_all_controllers)
        {
          // Unknown if 0x42 is the only valid command byte here, but other tested command bytes cause early aborts
          *data_out = GetMultitapIDByte();
          m_invalid_transfer_all_command = (data_in != 0x42);
          ack = true;
        }
        else
        {
          ack = TransferController(m_selected_slot, data_in, data_out);
        }
        m_controller_transfer_step++;
      }
      else if (m_controller_transfer_step == 1) // Request byte
      {
        if (m_transfer_all_controllers)
        {
          *data_out = GetStatusByte();

          ack = !m_invalid_transfer_all_command;
          m_selected_slot = 0;
          m_transfer_state = TransferState::AllControllers;
        }
        else
        {
          ack = TransferController(m_selected_slot, 0x00, data_out);
          m_transfer_state = TransferState::SingleController;
        }

        // Queue up request for next transfer cycle (not sure if this is always queued on invalid commands)
        m_transfer_all_controllers = (data_in & 0x01);
        m_controller_transfer_step = 0;
      }
      else
      {
        UnreachableCode();
      }
    }
    break;

    case TransferState::SingleController:
    {
      // TODO: Check if the transfer buffer gets wiped when transitioning to/from this mode

      ack = TransferController(m_selected_slot, data_in, data_out);

      if (!ack)
      {
        Log_DevPrintf("Controller transfer ended");
        m_transfer_state = TransferState::Idle;
      }
    }
    break;

    case TransferState::AllControllers:
    {
      // In this mode, we transfer until reaching 8 bytes or the controller finishes its response (no ack is returned).
      // The hardware is probably either latching the controller info halfword count or waiting for a transfer timeout
      // (timeouts might be possible due to buffered responses in this mode, and if the controllers are transferred in
      // parallel rather than sequentially like we're doing here). We'll just simplify this and check the ack return
      // value since our controller implementations are deterministic.

      *data_out = m_transfer_buffer[m_controller_transfer_step];
      ack = true;

      if (m_current_controller_done)
        m_transfer_buffer[m_controller_transfer_step] = 0xFF;
      else
        m_current_controller_done =
          !TransferController(m_selected_slot, data_in, &m_transfer_buffer[m_controller_transfer_step]);

      m_controller_transfer_step++;
      if (m_controller_transfer_step % 8 == 0)
      {
        m_current_controller_done = false;
        m_selected_slot = (m_selected_slot + 1) % 4;
        if (m_selected_slot == 0)
          ack = false;
      }
    }
    break;

      DefaultCaseIsUnreachable();
  }
  return ack;
}
