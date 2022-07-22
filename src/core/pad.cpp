#include "pad.h"
#include "common/log.h"
#include "controller.h"
#include "host.h"
#include "interrupt_controller.h"
#include "memory_card.h"
#include "multitap.h"
#include "system.h"
#include "util/state_wrapper.h"
Log_SetChannel(Pad);

Pad g_pad;

Pad::Pad() = default;

Pad::~Pad() = default;

void Pad::Initialize()
{
  m_transfer_event = TimingEvents::CreateTimingEvent(
    "Pad Serial Transfer", 1, 1,
    [](void* param, TickCount ticks, TickCount ticks_late) { static_cast<Pad*>(param)->TransferEvent(ticks_late); },
    this, false);
  Reset();
}

void Pad::Shutdown()
{
  m_transfer_event.reset();

  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    m_controllers[i].reset();
    m_memory_cards[i].reset();
  }
}

void Pad::Reset()
{
  SoftReset();

  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    if (m_controllers[i])
      m_controllers[i]->Reset();

    if (m_memory_cards[i])
      m_memory_cards[i]->Reset();
  }

  for (u32 i = 0; i < NUM_MULTITAPS; i++)
    m_multitaps[i].Reset();
}

bool Pad::DoStateController(StateWrapper& sw, u32 i)
{
  ControllerType controller_type = m_controllers[i] ? m_controllers[i]->GetType() : ControllerType::None;
  ControllerType state_controller_type = controller_type;

  sw.Do(&state_controller_type);

  if (controller_type != state_controller_type)
  {
    Assert(sw.GetMode() == StateWrapper::Mode::Read);

    // UI notification portion is separated from emulation portion (intentional condition check redundancy)
    if (g_settings.load_devices_from_save_states)
    {
      Host::AddFormattedOSDMessage(
        10.0f,
        Host::TranslateString("OSDMessage",
                              "Save state contains controller type %s in port %u, but %s is used. Switching."),
        Settings::GetControllerTypeName(state_controller_type), i + 1u,
        Settings::GetControllerTypeName(controller_type));
    }
    else
    {
      Host::AddFormattedOSDMessage(
        10.0f, Host::TranslateString("OSDMessage", "Ignoring mismatched controller type %s in port %u."),
        Settings::GetControllerTypeName(state_controller_type), i + 1u);
    }

    // dev-friendly untranslated console log.
    Log_DevPrintf("Controller type mismatch in slot %u: state=%s(%u) ui=%s(%u) load_from_state=%s", i + 1u,
                  Settings::GetControllerTypeName(state_controller_type), static_cast<unsigned>(state_controller_type),
                  Settings::GetControllerTypeName(controller_type), static_cast<unsigned>(controller_type),
                  g_settings.load_devices_from_save_states ? "yes" : "no");

    if (g_settings.load_devices_from_save_states)
    {
      m_controllers[i].reset();
      if (state_controller_type != ControllerType::None)
        m_controllers[i] = Controller::Create(state_controller_type, i);
    }
    else
    {
      // mismatched controller states prevents us from loading the state into the user's preferred controller.
      // just doing a reset here is a little dodgy. If there's an active xfer on the state-saved controller
      // then who knows what might happen as the rest of the packet streams in. (possibly the SIO xfer will
      // timeout and the controller will just correct itself on the next frame's read attempt -- after all on
      // physical HW removing a controller is allowed and could happen in the middle of SIO comms)

      if (m_controllers[i])
        m_controllers[i]->Reset();
    }
  }

  // we still need to read/write the save state controller state even if the controller does not exist.
  // the marker is only expected for valid controller types.
  if (state_controller_type == ControllerType::None)
    return true;

  if (!sw.DoMarker("Controller"))
    return false;

  if (auto& controller = m_controllers[i]; controller && controller->GetType() == state_controller_type)
    return controller->DoState(sw, g_settings.load_devices_from_save_states);
  else if (auto dummy = Controller::Create(state_controller_type, i); dummy)
    return dummy->DoState(sw, g_settings.load_devices_from_save_states);

  return true;
}

bool Pad::DoStateMemcard(StateWrapper& sw, u32 i)
{
  bool card_present_in_state = static_cast<bool>(m_memory_cards[i]);

  sw.Do(&card_present_in_state);

  if (card_present_in_state && !m_memory_cards[i] && g_settings.load_devices_from_save_states)
  {
    Host::AddFormattedOSDMessage(
      20.0f,
      Host::TranslateString("OSDMessage",
                            "Memory card %u present in save state but not in system. Creating temporary card."),
      i + 1u);
    m_memory_cards[i] = MemoryCard::Create();
  }

  MemoryCard* card_ptr = m_memory_cards[i].get();
  std::unique_ptr<MemoryCard> card_from_state;

  if (card_present_in_state)
  {
    if (sw.IsReading() && !g_settings.load_devices_from_save_states)
    {
      // load memcard into a temporary: If the card datas match, take the one from the savestate
      // since it has other useful non-data state information. Otherwise take the user's card
      // and perform a re-plugging.

      card_from_state = std::make_unique<MemoryCard>();
      card_ptr = card_from_state.get();
    }

    if (!sw.DoMarker("MemoryCard") || !card_ptr->DoState(sw))
      return false;
  }

  if (sw.IsWriting())
    return true; // all done as far as writes concerned.

  if (card_from_state)
  {
    if (m_memory_cards[i])
    {
      if (m_memory_cards[i]->GetData() == card_from_state->GetData())
      {
        card_from_state->SetFilename(m_memory_cards[i]->GetFilename());
        m_memory_cards[i] = std::move(card_from_state);
      }
      else
      {
        Host::AddFormattedOSDMessage(
          20.0f,
          Host::TranslateString("OSDMessage",
                                "Memory card %u from save state does match current card data. Simulating replugging."),
          i + 1u);

        // this is a potentially serious issue - some games cache info from memcards and jumping around
        // with savestates can lead to card corruption on the next save attempts (and may not be obvious
        // until much later). One workaround is to forcibly eject the card for 30+ frames, long enough
        // for the game to decide it was removed and purge its cache. Once implemented, this could be
        // described as deferred re-plugging in the log.

        Log_WarningPrintf("Memory card %u data mismatch. Using current data via instant-replugging.", i + 1u);
        m_memory_cards[i]->Reset();
      }
    }
    else
    {
      Host::AddFormattedOSDMessage(
        20.0f,
        Host::TranslateString("OSDMessage", "Memory card %u present in save state but not in system. Ignoring card."),
        i + 1u);
    }

    return true;
  }

  if (!card_present_in_state && m_memory_cards[i])
  {
    if (g_settings.load_devices_from_save_states)
    {
      Host::AddFormattedOSDMessage(
        20.0f,
        Host::TranslateString("OSDMessage", "Memory card %u present in system but not in save state. Removing card."),
        i + 1u);
      m_memory_cards[i].reset();
    }
    else
    {
      Host::AddFormattedOSDMessage(
        20.0f,
        Host::TranslateString("OSDMessage", "Memory card %u present in system but not in save state. Replugging card."),
        i + 1u);
      m_memory_cards[i]->Reset();
    }
  }

  return true;
}

bool Pad::DoState(StateWrapper& sw)
{
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    if ((sw.GetVersion() < 50) && (i >= 2))
    {
      // loading from old savestate which only had max 2 controllers.
      // honoring load_devices_from_save_states in this case seems debatable, but might as well...
      if (m_controllers[i])
      {
        if (g_settings.load_devices_from_save_states)
          m_controllers[i].reset();
        else
          m_controllers[i]->Reset();
      }

      if (m_memory_cards[i])
      {
        if (g_settings.load_devices_from_save_states)
          m_memory_cards[i].reset();
        else
          m_memory_cards[i]->Reset();
      }

      // ... and make sure to skip trying to read controller_type / card_present flags which don't exist in old states.
      continue;
    }

    if (!DoStateController(sw, i))
      return false;

    if (!DoStateMemcard(sw, i))
      return false;
  }

  if (sw.GetVersion() >= 50)
  {
    for (u32 i = 0; i < NUM_MULTITAPS; i++)
    {
      if (!m_multitaps[i].DoState(sw))
        return false;
    }
  }

  sw.Do(&m_state);
  sw.Do(&m_JOY_CTRL.bits);
  sw.Do(&m_JOY_STAT.bits);
  sw.Do(&m_JOY_MODE.bits);
  sw.Do(&m_JOY_BAUD);
  sw.Do(&m_receive_buffer);
  sw.Do(&m_transmit_buffer);
  sw.Do(&m_receive_buffer_full);
  sw.Do(&m_transmit_buffer_full);

  if (sw.IsReading() && IsTransmitting())
    m_transfer_event->Activate();

  return !sw.HasError();
}

void Pad::SetController(u32 slot, std::unique_ptr<Controller> dev)
{
  m_controllers[slot] = std::move(dev);
}

void Pad::SetMemoryCard(u32 slot, std::unique_ptr<MemoryCard> dev)
{
  m_memory_cards[slot] = std::move(dev);
}

std::unique_ptr<MemoryCard> Pad::RemoveMemoryCard(u32 slot)
{
  std::unique_ptr<MemoryCard> ret = std::move(m_memory_cards[slot]);
  if (ret)
    ret->Reset();
  return ret;
}

u32 Pad::ReadRegister(u32 offset)
{
  switch (offset)
  {
    case 0x00: // JOY_DATA
    {
      if (IsTransmitting())
        m_transfer_event->InvokeEarly();

      const u8 value = m_receive_buffer_full ? m_receive_buffer : 0xFF;
      Log_DebugPrintf("JOY_DATA (R) -> 0x%02X%s", ZeroExtend32(value), m_receive_buffer_full ? "" : "(EMPTY)");
      m_receive_buffer_full = false;
      UpdateJoyStat();

      return (ZeroExtend32(value) | (ZeroExtend32(value) << 8) | (ZeroExtend32(value) << 16) |
              (ZeroExtend32(value) << 24));
    }

    case 0x04: // JOY_STAT
    {
      if (IsTransmitting())
        m_transfer_event->InvokeEarly();

      const u32 bits = m_JOY_STAT.bits;
      m_JOY_STAT.ACKINPUT = false;
      return bits;
    }

    case 0x08: // JOY_MODE
      return ZeroExtend32(m_JOY_MODE.bits);

    case 0x0A: // JOY_CTRL
      return ZeroExtend32(m_JOY_CTRL.bits);

    case 0x0E: // JOY_BAUD
      return ZeroExtend32(m_JOY_BAUD);

    default:
      Log_ErrorPrintf("Unknown register read: 0x%X", offset);
      return UINT32_C(0xFFFFFFFF);
  }
}

void Pad::WriteRegister(u32 offset, u32 value)
{
  switch (offset)
  {
    case 0x00: // JOY_DATA
    {
      Log_DebugPrintf("JOY_DATA (W) <- 0x%02X", value);

      if (m_transmit_buffer_full)
        Log_WarningPrint("TX FIFO overrun");

      m_transmit_buffer = Truncate8(value);
      m_transmit_buffer_full = true;

      if (!IsTransmitting() && CanTransfer())
        BeginTransfer();

      return;
    }

    case 0x0A: // JOY_CTRL
    {
      Log_DebugPrintf("JOY_CTRL <- 0x%04X", value);

      m_JOY_CTRL.bits = Truncate16(value);
      if (m_JOY_CTRL.RESET)
        SoftReset();

      if (m_JOY_CTRL.ACK)
      {
        // reset stat bits
        m_JOY_STAT.INTR = false;
      }

      if (!m_JOY_CTRL.SELECT)
        ResetDeviceTransferState();

      if (!m_JOY_CTRL.SELECT || !m_JOY_CTRL.TXEN)
      {
        if (IsTransmitting())
          EndTransfer();
      }
      else
      {
        if (!IsTransmitting() && CanTransfer())
          BeginTransfer();
      }

      UpdateJoyStat();
      return;
    }

    case 0x08: // JOY_MODE
    {
      Log_DebugPrintf("JOY_MODE <- 0x%08X", value);
      m_JOY_MODE.bits = Truncate16(value);
      return;
    }

    case 0x0E:
    {
      Log_DebugPrintf("JOY_BAUD <- 0x%08X", value);
      m_JOY_BAUD = Truncate16(value);
      return;
    }

    default:
      Log_ErrorPrintf("Unknown register write: 0x%X <- 0x%08X", offset, value);
      return;
  }
}

void Pad::SoftReset()
{
  if (IsTransmitting())
    EndTransfer();

  m_JOY_CTRL.bits = 0;
  m_JOY_STAT.bits = 0;
  m_JOY_MODE.bits = 0;
  m_receive_buffer = 0;
  m_receive_buffer_full = false;
  m_transmit_buffer = 0;
  m_transmit_buffer_full = false;
  ResetDeviceTransferState();
  UpdateJoyStat();
}

void Pad::UpdateJoyStat()
{
  m_JOY_STAT.RXFIFONEMPTY = m_receive_buffer_full;
  m_JOY_STAT.TXDONE = !m_transmit_buffer_full && m_state != State::Transmitting;
  m_JOY_STAT.TXRDY = !m_transmit_buffer_full;
}

void Pad::TransferEvent(TickCount ticks_late)
{
  if (m_state == State::Transmitting)
    DoTransfer(ticks_late);
  else
    DoACK();
}

void Pad::BeginTransfer()
{
  DebugAssert(m_state == State::Idle && CanTransfer());
  Log_DebugPrintf("Starting transfer");

  m_JOY_CTRL.RXEN = true;
  m_transmit_value = m_transmit_buffer;
  m_transmit_buffer_full = false;

  // The transfer or the interrupt must be delayed, otherwise the BIOS thinks there's no device detected.
  // It seems to do something resembling the following:
  //  1) Sets the control register up for transmitting, interrupt on ACK.
  //  2) Writes 0x01 to the TX FIFO.
  //  3) Delays for a bit.
  //  4) Writes ACK to the control register, clearing the interrupt flag.
  //  5) Clears IRQ7 in the interrupt controller.
  //  6) Waits until the RX FIFO is not empty, reads the first byte to $zero.
  //  7) Checks if the interrupt status register had IRQ7 set. If not, no device connected.
  //
  // Performing the transfer immediately will result in both the INTR bit and the bit in the interrupt
  // controller being discarded in (4)/(5), but this bit was set by the *new* transfer. Therefore, the
  // test in (7) will fail, and it won't send any more data. So, the transfer/interrupt must be delayed
  // until after (4) and (5) have been completed.

  m_state = State::Transmitting;
  m_transfer_event->SetPeriodAndSchedule(GetTransferTicks());
}

void Pad::DoTransfer(TickCount ticks_late)
{
  Log_DebugPrintf("Transferring slot %d", m_JOY_CTRL.SLOT.GetValue());

  const u8 device_index = m_multitaps[0].IsEnabled() ? 4u : m_JOY_CTRL.SLOT;
  Controller* const controller = m_controllers[device_index].get();
  MemoryCard* const memory_card = m_memory_cards[device_index].get();

  // set rx?
  m_JOY_CTRL.RXEN = true;

  const u8 data_out = m_transmit_value;

  u8 data_in = 0xFF;
  bool ack = false;

  switch (m_active_device)
  {
    case ActiveDevice::None:
    {
      if (m_multitaps[m_JOY_CTRL.SLOT].IsEnabled())
      {
        if ((ack = m_multitaps[m_JOY_CTRL.SLOT].Transfer(data_out, &data_in)) == true)
        {
          Log_TracePrintf("Active device set to tap %d, sent 0x%02X, received 0x%02X",
                          static_cast<int>(m_JOY_CTRL.SLOT), data_out, data_in);
          m_active_device = ActiveDevice::Multitap;
        }
      }
      else
      {
        if (!controller || (ack = controller->Transfer(data_out, &data_in)) == false)
        {
          if (!memory_card || (ack = memory_card->Transfer(data_out, &data_in)) == false)
          {
            // nothing connected to this port
            Log_TracePrintf("Nothing connected or ACK'ed");
          }
          else
          {
            // memory card responded, make it the active device until non-ack
            Log_TracePrintf("Transfer to memory card, data_out=0x%02X, data_in=0x%02X", data_out, data_in);
            m_active_device = ActiveDevice::MemoryCard;
          }
        }
        else
        {
          // controller responded, make it the active device until non-ack
          Log_TracePrintf("Transfer to controller, data_out=0x%02X, data_in=0x%02X", data_out, data_in);
          m_active_device = ActiveDevice::Controller;
        }
      }
    }
    break;

    case ActiveDevice::Controller:
    {
      if (controller)
      {
        ack = controller->Transfer(data_out, &data_in);
        Log_TracePrintf("Transfer to controller, data_out=0x%02X, data_in=0x%02X", data_out, data_in);
      }
    }
    break;

    case ActiveDevice::MemoryCard:
    {
      if (memory_card)
      {
        ack = memory_card->Transfer(data_out, &data_in);
        Log_TracePrintf("Transfer to memory card, data_out=0x%02X, data_in=0x%02X", data_out, data_in);
      }
    }
    break;

    case ActiveDevice::Multitap:
    {
      if (m_multitaps[m_JOY_CTRL.SLOT].IsEnabled())
      {
        ack = m_multitaps[m_JOY_CTRL.SLOT].Transfer(data_out, &data_in);
        Log_TracePrintf("Transfer tap %d, sent 0x%02X, received 0x%02X, acked: %s", static_cast<int>(m_JOY_CTRL.SLOT),
                        data_out, data_in, ack ? "true" : "false");
      }
    }
    break;
  }

  m_receive_buffer = data_in;
  m_receive_buffer_full = true;

  // device no longer active?
  if (!ack)
  {
    m_active_device = ActiveDevice::None;
    EndTransfer();
  }
  else
  {
    const bool memcard_transfer =
      m_active_device == ActiveDevice::MemoryCard ||
      (m_active_device == ActiveDevice::Multitap && m_multitaps[m_JOY_CTRL.SLOT].IsReadingMemoryCard());

    const TickCount ack_timer = GetACKTicks(memcard_transfer);
    Log_DebugPrintf("Delaying ACK for %d ticks", ack_timer);
    m_state = State::WaitingForACK;
    m_transfer_event->SetPeriodAndSchedule(ack_timer);
  }

  UpdateJoyStat();
}

void Pad::DoACK()
{
  m_JOY_STAT.ACKINPUT = true;

  if (m_JOY_CTRL.ACKINTEN)
  {
    Log_DebugPrintf("Triggering ACK interrupt");
    m_JOY_STAT.INTR = true;
    g_interrupt_controller.InterruptRequest(InterruptController::IRQ::IRQ7);
  }

  EndTransfer();
  UpdateJoyStat();

  if (CanTransfer())
    BeginTransfer();
}

void Pad::EndTransfer()
{
  DebugAssert(m_state == State::Transmitting || m_state == State::WaitingForACK);
  Log_DebugPrintf("Ending transfer");

  m_state = State::Idle;
  m_transfer_event->Deactivate();
}

void Pad::ResetDeviceTransferState()
{
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    if (m_controllers[i])
      m_controllers[i]->ResetTransferState();
    if (m_memory_cards[i])
      m_memory_cards[i]->ResetTransferState();
  }

  for (u32 i = 0; i < NUM_MULTITAPS; i++)
    m_multitaps[i].ResetTransferState();

  m_active_device = ActiveDevice::None;
}
