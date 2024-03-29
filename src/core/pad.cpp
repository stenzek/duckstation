// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "pad.h"
#include "controller.h"
#include "host.h"
#include "interrupt_controller.h"
#include "memory_card.h"
#include "multitap.h"
#include "save_state_version.h"
#include "system.h"
#include "types.h"

#include "util/imgui_manager.h"
#include "util/state_wrapper.h"

#include "common/bitfield.h"
#include "common/bitutils.h"
#include "common/fifo_queue.h"
#include "common/log.h"

#include "IconsFontAwesome5.h"

#include <array>
#include <memory>

Log_SetChannel(Pad);

namespace Pad {

enum class State : u32
{
  Idle,
  Transmitting,
  WaitingForACK
};

enum class ActiveDevice : u8
{
  None,
  Controller,
  MemoryCard,
  Multitap
};

union JOY_CTRL
{
  u16 bits;

  BitField<u16, bool, 0, 1> TXEN;
  BitField<u16, bool, 1, 1> SELECT;
  BitField<u16, bool, 2, 1> RXEN;
  BitField<u16, bool, 4, 1> ACK;
  BitField<u16, bool, 6, 1> RESET;
  BitField<u16, u8, 8, 2> RXIMODE;
  BitField<u16, bool, 10, 1> TXINTEN;
  BitField<u16, bool, 11, 1> RXINTEN;
  BitField<u16, bool, 12, 1> ACKINTEN;
  BitField<u16, u8, 13, 1> SLOT;
};

union JOY_STAT
{
  u32 bits;

  BitField<u32, bool, 0, 1> TXRDY;
  BitField<u32, bool, 1, 1> RXFIFONEMPTY;
  BitField<u32, bool, 2, 1> TXDONE;
  BitField<u32, bool, 7, 1> ACKINPUT;
  BitField<u32, bool, 9, 1> INTR;
  BitField<u32, u32, 11, 21> TMR;
};

union JOY_MODE
{
  u16 bits;

  BitField<u16, u8, 0, 2> reload_factor;
  BitField<u16, u8, 2, 2> character_length;
  BitField<u16, bool, 4, 1> parity_enable;
  BitField<u16, u8, 5, 1> parity_type;
  BitField<u16, u8, 8, 1> clk_polarity;
};

static bool CanTransfer();
static bool ShouldAvoidSavingToState();
static u32 GetMaximumRollbackFrames();

static TickCount GetTransferTicks();

// From @JaCzekanski
// ACK lasts ~96 ticks or approximately 2.84us at master clock (not implemented).
// ACK delay is between 6.8us-13.7us, or ~338 ticks at master clock for approximately 9.98us.
// Memory card responds faster, approximately 5us or ~170 ticks.
static constexpr TickCount GetACKTicks(bool memory_card)
{
  return memory_card ? 170 : 450;
}

static void SoftReset();
static void UpdateJoyStat();
static void TransferEvent(void*, TickCount ticks, TickCount ticks_late);
static void BeginTransfer();
static void DoTransfer(TickCount ticks_late);
static void DoACK();
static void EndTransfer();
static void ResetDeviceTransferState();

static bool DoStateController(StateWrapper& sw, u32 i);
static bool DoStateMemcard(StateWrapper& sw, u32 i, bool is_memory_state);
static MemoryCard* GetDummyMemcard();
static void BackupMemoryCardState();
static void RestoreMemoryCardState();

static std::array<std::unique_ptr<Controller>, NUM_CONTROLLER_AND_CARD_PORTS> s_controllers;
static std::array<std::unique_ptr<MemoryCard>, NUM_CONTROLLER_AND_CARD_PORTS> s_memory_cards;

static std::array<Multitap, NUM_MULTITAPS> s_multitaps;

static std::unique_ptr<TimingEvent> s_transfer_event;
static State s_state = State::Idle;

static JOY_CTRL s_JOY_CTRL = {};
static JOY_STAT s_JOY_STAT = {};
static JOY_MODE s_JOY_MODE = {};
static u16 s_JOY_BAUD = 0;

static ActiveDevice s_active_device = ActiveDevice::None;
static u8 s_receive_buffer = 0;
static u8 s_transmit_buffer = 0;
static u8 s_transmit_value = 0;
static bool s_receive_buffer_full = false;
static bool s_transmit_buffer_full = false;

static u32 s_last_memory_card_transfer_frame = 0;
static std::unique_ptr<GrowableMemoryByteStream> s_memory_card_backup;
static std::unique_ptr<MemoryCard> s_dummy_card;

} // namespace Pad

void Pad::Initialize()
{
  s_transfer_event = TimingEvents::CreateTimingEvent("Pad Serial Transfer", 1, 1, &Pad::TransferEvent, nullptr, false);
  Reset();
}

void Pad::Shutdown()
{
  s_memory_card_backup.reset();

  s_transfer_event.reset();

  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    s_controllers[i].reset();
    s_memory_cards[i].reset();
  }
}

void Pad::Reset()
{
  SoftReset();

  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    if (s_controllers[i])
      s_controllers[i]->Reset();

    if (s_memory_cards[i])
      s_memory_cards[i]->Reset();
  }

  for (u32 i = 0; i < NUM_MULTITAPS; i++)
    s_multitaps[i].Reset();
}

bool Pad::ShouldAvoidSavingToState()
{
  // Currently only runahead, will also be used for netplay.
  return g_settings.IsRunaheadEnabled();
}

u32 Pad::GetMaximumRollbackFrames()
{
  return g_settings.runahead_frames;
}

bool Pad::DoStateController(StateWrapper& sw, u32 i)
{
  ControllerType controller_type = s_controllers[i] ? s_controllers[i]->GetType() : ControllerType::None;
  ControllerType state_controller_type = controller_type;

  sw.Do(&state_controller_type);

  if (controller_type != state_controller_type)
  {
    Assert(sw.GetMode() == StateWrapper::Mode::Read);

    // UI notification portion is separated from emulation portion (intentional condition check redundancy)
    if (g_settings.load_devices_from_save_states)
    {
      Host::AddFormattedOSDMessage(
        10.0f, TRANSLATE("OSDMessage", "Save state contains controller type %s in port %u, but %s is used. Switching."),
        Settings::GetControllerTypeName(state_controller_type), i + 1u,
        Settings::GetControllerTypeName(controller_type));
    }
    else
    {
      Host::AddFormattedOSDMessage(10.0f, TRANSLATE("OSDMessage", "Ignoring mismatched controller type %s in port %u."),
                                   Settings::GetControllerTypeName(state_controller_type), i + 1u);
    }

    // dev-friendly untranslated console log.
    Log_DevPrintf("Controller type mismatch in slot %u: state=%s(%u) ui=%s(%u) load_from_state=%s", i + 1u,
                  Settings::GetControllerTypeName(state_controller_type), static_cast<unsigned>(state_controller_type),
                  Settings::GetControllerTypeName(controller_type), static_cast<unsigned>(controller_type),
                  g_settings.load_devices_from_save_states ? "yes" : "no");

    if (g_settings.load_devices_from_save_states)
    {
      s_controllers[i].reset();
      if (state_controller_type != ControllerType::None)
        s_controllers[i] = Controller::Create(state_controller_type, i);
    }
    else
    {
      // mismatched controller states prevents us from loading the state into the user's preferred controller.
      // just doing a reset here is a little dodgy. If there's an active xfer on the state-saved controller
      // then who knows what might happen as the rest of the packet streams in. (possibly the SIO xfer will
      // timeout and the controller will just correct itself on the next frame's read attempt -- after all on
      // physical HW removing a controller is allowed and could happen in the middle of SIO comms)

      if (s_controllers[i])
        s_controllers[i]->Reset();
    }
  }

  // we still need to read/write the save state controller state even if the controller does not exist.
  // the marker is only expected for valid controller types.
  if (state_controller_type == ControllerType::None)
    return true;

  if (!sw.DoMarker("Controller"))
    return false;

  if (auto& controller = s_controllers[i]; controller && controller->GetType() == state_controller_type)
    return controller->DoState(sw, g_settings.load_devices_from_save_states);
  else if (auto dummy = Controller::Create(state_controller_type, i); dummy)
    return dummy->DoState(sw, g_settings.load_devices_from_save_states);

  return true;
}

bool Pad::DoStateMemcard(StateWrapper& sw, u32 i, bool is_memory_state)
{
  bool card_present_in_state = static_cast<bool>(s_memory_cards[i]);

  sw.Do(&card_present_in_state);

  if (card_present_in_state && !s_memory_cards[i] && g_settings.load_devices_from_save_states)
  {
    Host::AddIconOSDMessage(
      fmt::format("card_load_warning_{}", i), ICON_FA_SD_CARD,
      fmt::format(
        TRANSLATE_FS("OSDMessage", "Memory card {} present in save state but not in system. Creating temporary card."),
        i + 1u),
      Host::OSD_ERROR_DURATION);
    s_memory_cards[i] = MemoryCard::Create();
  }

  MemoryCard* card_ptr = s_memory_cards[i].get();
  if (card_present_in_state)
  {
    if (sw.IsReading() && !g_settings.load_devices_from_save_states)
    {
      // load memcard into a temporary: If the card datas match, take the one from the savestate
      // since it has other useful non-data state information. Otherwise take the user's card
      // and perform a re-plugging.
      card_ptr = GetDummyMemcard();
    }

    if (!sw.DoMarker("MemoryCard") || !card_ptr->DoState(sw))
      return false;
  }

  if (sw.IsWriting())
    return true; // all done as far as writes concerned.

  if (card_ptr != s_memory_cards[i].get())
  {
    if (s_memory_cards[i])
    {
      if (s_memory_cards[i]->GetData() == card_ptr->GetData())
      {
        Log_DevFmt("Card {} data matches, copying state", i + 1u);
        s_memory_cards[i]->CopyState(card_ptr);
      }
      else
      {
        Host::AddIconOSDMessage(
          fmt::format("card_load_warning_{}", i), ICON_FA_SD_CARD,
          fmt::format(
            TRANSLATE_FS("OSDMessage",
                         "Memory card {} from save state does match current card data. Simulating replugging."),
            i + 1u),
          Host::OSD_WARNING_DURATION);

        // this is a potentially serious issue - some games cache info from memcards and jumping around
        // with savestates can lead to card corruption on the next save attempts (and may not be obvious
        // until much later). One workaround is to forcibly eject the card for 30+ frames, long enough
        // for the game to decide it was removed and purge its cache. Once implemented, this could be
        // described as deferred re-plugging in the log.

        Log_WarningFmt("Memory card {} data mismatch. Using current data via instant-replugging.", i + 1u);
        s_memory_cards[i]->Reset();
      }
    }
    else
    {
      Host::AddIconOSDMessage(
        fmt::format("card_load_warning_{}", i), ICON_FA_SD_CARD,
        fmt::format(
          TRANSLATE_FS("OSDMessage", "Memory card {} present in save state but not in system. Ignoring card."), i + 1u),
        Host::OSD_ERROR_DURATION);
    }

    return true;
  }

  if (!card_present_in_state && s_memory_cards[i])
  {
    if (g_settings.load_devices_from_save_states)
    {
      Host::AddIconOSDMessage(
        fmt::format("card_load_warning_{}", i), ICON_FA_SD_CARD,
        fmt::format(
          TRANSLATE_FS("OSDMessage", "Memory card {} present in system but not in save state. Removing card."), i + 1u),
        Host::OSD_ERROR_DURATION);
      s_memory_cards[i].reset();
    }
    else
    {
      Host::AddIconOSDMessage(
        fmt::format("card_load_warning_{}", i), ICON_FA_SD_CARD,
        fmt::format(
          TRANSLATE_FS("OSDMessage", "Memory card {} present in system but not in save state. Replugging card."),
          i + 1u),
        Host::OSD_WARNING_DURATION);
      s_memory_cards[i]->Reset();
    }
  }

  return true;
}

MemoryCard* Pad::GetDummyMemcard()
{
  if (!s_dummy_card)
    s_dummy_card = MemoryCard::Create();
  return s_dummy_card.get();
}

void Pad::BackupMemoryCardState()
{
  Log_DevPrintf("Backing up memory card state.");

  if (!s_memory_card_backup)
  {
    s_memory_card_backup =
      std::make_unique<GrowableMemoryByteStream>(nullptr, MemoryCard::STATE_SIZE * NUM_CONTROLLER_AND_CARD_PORTS);
  }

  s_memory_card_backup->SeekAbsolute(0);

  StateWrapper sw(s_memory_card_backup.get(), StateWrapper::Mode::Write, SAVE_STATE_VERSION);

  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    if (s_memory_cards[i])
      s_memory_cards[i]->DoState(sw);
  }
}

void Pad::RestoreMemoryCardState()
{
  DebugAssert(s_memory_card_backup);

  Log_VerbosePrintf("Restoring backed up memory card state.");

  s_memory_card_backup->SeekAbsolute(0);
  StateWrapper sw(s_memory_card_backup.get(), StateWrapper::Mode::Read, SAVE_STATE_VERSION);

  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    if (s_memory_cards[i])
      s_memory_cards[i]->DoState(sw);
  }
}

bool Pad::DoState(StateWrapper& sw, bool is_memory_state)
{
  if (is_memory_state && ShouldAvoidSavingToState())
  {
    // We do a bit of trickery for memory states here to avoid writing 128KB * num_cards to the state.
    // Profiling shows that the card write scan be up to 17% of overall CPU time, so it's definitely worth skipping.
    // However, we can't roll back past a transfer boundary, because that'll corrupt our cards. So, we have to be smart
    // about this.
    //
    // There's three main scenarios:
    //  (1) No transfers occurring before or after the rollback point.
    //  (2) A transfer was started before the rollback point.
    //  (3) A transfer was started after the rollback point.
    //
    // For (1), it's easy, we don't have to do anything. Just skip saving and continue on our merry way.
    //
    // For (2), we serialize the state whenever there's a transfer within the last N_ROLLBACK frames. Easy-ish.
    //
    // For (3), it gets messy. We didn't know that a transfer was going to start, and our rollback state doesn't
    // contain the state of the memory cards, because we were cheeky and skipped it. So, instead, we back up
    // the state of memory cards when any transfer begins, assuming it's not within the last N_ROLLBACK frames, in
    // DoTransfer(). That way, when we do have to roll back past this boundary, we can just restore the known good "pre
    // transfer" state. Any memory saves created after the transfer begun will go through the same path as (2), so we
    // don't risk corrupting that way.
    //
    // Hopefully that's everything.
    //
    bool process_memcard_state = true;

    const u32 frame_number = System::GetFrameNumber();
    const u32 frames_since_transfer = frame_number - s_last_memory_card_transfer_frame;
    const u32 prev_transfer_frame = s_last_memory_card_transfer_frame;
    bool state_has_memcards = false;

    sw.Do(&s_last_memory_card_transfer_frame);

    // If there's been a transfer within the last N_ROLLBACK frames, include the memory card state when saving.
    state_has_memcards = (frames_since_transfer <= GetMaximumRollbackFrames());
    sw.Do(&state_has_memcards);

    if (sw.IsReading())
    {
      // If no transfers have occurred, no need to reload state.
      if (s_last_memory_card_transfer_frame != frame_number && s_last_memory_card_transfer_frame == prev_transfer_frame)
      {
        process_memcard_state = false;
      }
      else if (!state_has_memcards)
      {
        // If the memory state doesn't have card data (i.e. rolling back past a transfer start), reload the backed up
        // state created when the transfer initially begun.
        RestoreMemoryCardState();
        process_memcard_state = false;
      }
    }

    // Still have to parse through the data if it's present.
    if (state_has_memcards)
    {
      MemoryCard* dummy_card = process_memcard_state ? nullptr : GetDummyMemcard();
      for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
      {
        if (s_memory_cards[i])
        {
          MemoryCard* const mc = process_memcard_state ? s_memory_cards[i].get() : dummy_card;
          mc->DoState(sw);
        }
      }
    }

    // Always save controller state.
    for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
    {
      if (s_controllers[i])
      {
        // Ignore input state, use the current. I think we want this?
        s_controllers[i]->DoState(sw, false);
      }
    }
  }
  else
  {
    for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
    {
      if ((sw.GetVersion() < 50) && (i >= 2))
      {
        // loading from old savestate which only had max 2 controllers.
        // honoring load_devices_from_save_states in this case seems debatable, but might as well...
        if (s_controllers[i])
        {
          if (g_settings.load_devices_from_save_states)
            s_controllers[i].reset();
          else
            s_controllers[i]->Reset();
        }

        if (s_memory_cards[i])
        {
          if (g_settings.load_devices_from_save_states)
            s_memory_cards[i].reset();
          else
            s_memory_cards[i]->Reset();
        }

        // and make sure to skip trying to read controller_type / card_present flags which don't exist in old states.
        continue;
      }

      if (!DoStateController(sw, i))
        return false;

      if (!DoStateMemcard(sw, i, is_memory_state))
        return false;
    }
  }

  if (sw.GetVersion() >= 50)
  {
    for (u32 i = 0; i < NUM_MULTITAPS; i++)
    {
      if (!s_multitaps[i].DoState(sw))
        return false;
    }
  }

  sw.Do(&s_state);
  sw.Do(&s_JOY_CTRL.bits);
  sw.Do(&s_JOY_STAT.bits);
  sw.Do(&s_JOY_MODE.bits);
  sw.Do(&s_JOY_BAUD);
  sw.Do(&s_receive_buffer);
  sw.Do(&s_transmit_buffer);
  sw.Do(&s_receive_buffer_full);
  sw.Do(&s_transmit_buffer_full);

  if (sw.IsReading() && IsTransmitting())
    s_transfer_event->Activate();

  return !sw.HasError();
}

Controller* Pad::GetController(u32 slot)
{
  return s_controllers[slot].get();
}

void Pad::SetController(u32 slot, std::unique_ptr<Controller> dev)
{
  s_controllers[slot] = std::move(dev);
}

MemoryCard* Pad::GetMemoryCard(u32 slot)
{
  return s_memory_cards[slot].get();
}

void Pad::SetMemoryCard(u32 slot, std::unique_ptr<MemoryCard> dev)
{
  Log_InfoPrintf("Memory card slot %u: %s", slot,
                 dev ? (dev->GetFilename().empty() ? "<no file configured>" : dev->GetFilename().c_str()) :
                       "<unplugged>");

  s_memory_cards[slot] = std::move(dev);
}

std::unique_ptr<MemoryCard> Pad::RemoveMemoryCard(u32 slot)
{
  std::unique_ptr<MemoryCard> ret = std::move(s_memory_cards[slot]);
  if (ret)
    ret->Reset();
  return ret;
}

Multitap* Pad::GetMultitap(u32 slot)
{
  return &s_multitaps[slot];
}

u32 Pad::ReadRegister(u32 offset)
{
  switch (offset)
  {
    case 0x00: // JOY_DATA
    {
      if (IsTransmitting())
        s_transfer_event->InvokeEarly();

      const u8 value = s_receive_buffer_full ? s_receive_buffer : 0xFF;
      Log_DebugPrintf("JOY_DATA (R) -> 0x%02X%s", ZeroExtend32(value), s_receive_buffer_full ? "" : "(EMPTY)");
      s_receive_buffer_full = false;
      UpdateJoyStat();

      return (ZeroExtend32(value) | (ZeroExtend32(value) << 8) | (ZeroExtend32(value) << 16) |
              (ZeroExtend32(value) << 24));
    }

    case 0x04: // JOY_STAT
    {
      if (IsTransmitting())
        s_transfer_event->InvokeEarly();

      const u32 bits = s_JOY_STAT.bits;
      s_JOY_STAT.ACKINPUT = false;
      return bits;
    }

    case 0x08: // JOY_MODE
      return ZeroExtend32(s_JOY_MODE.bits);

    case 0x0A: // JOY_CTRL
      return ZeroExtend32(s_JOY_CTRL.bits);

    case 0x0E: // JOY_BAUD
      return ZeroExtend32(s_JOY_BAUD);

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

      if (s_transmit_buffer_full)
        Log_WarningPrint("TX FIFO overrun");

      s_transmit_buffer = Truncate8(value);
      s_transmit_buffer_full = true;

      if (!IsTransmitting() && CanTransfer())
        BeginTransfer();

      return;
    }

    case 0x0A: // JOY_CTRL
    {
      Log_DebugPrintf("JOY_CTRL <- 0x%04X", value);

      s_JOY_CTRL.bits = Truncate16(value);
      if (s_JOY_CTRL.RESET)
        SoftReset();

      if (s_JOY_CTRL.ACK)
      {
        // reset stat bits
        s_JOY_STAT.INTR = false;
        InterruptController::SetLineState(InterruptController::IRQ::PAD, false);
      }

      if (!s_JOY_CTRL.SELECT)
        ResetDeviceTransferState();

      if (!s_JOY_CTRL.SELECT || !s_JOY_CTRL.TXEN)
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
      s_JOY_MODE.bits = Truncate16(value);
      return;
    }

    case 0x0E:
    {
      Log_DebugPrintf("JOY_BAUD <- 0x%08X", value);
      s_JOY_BAUD = Truncate16(value);
      return;
    }

    default:
      Log_ErrorPrintf("Unknown register write: 0x%X <- 0x%08X", offset, value);
      return;
  }
}

bool Pad::IsTransmitting()
{
  return s_state != State::Idle;
}

bool Pad::CanTransfer()
{
  return s_transmit_buffer_full && s_JOY_CTRL.SELECT && s_JOY_CTRL.TXEN;
}

TickCount Pad::GetTransferTicks()
{
  return static_cast<TickCount>(ZeroExtend32(s_JOY_BAUD) * 8);
}

void Pad::SoftReset()
{
  if (IsTransmitting())
    EndTransfer();

  s_JOY_CTRL.bits = 0;
  s_JOY_STAT.bits = 0;
  s_JOY_MODE.bits = 0;
  s_receive_buffer = 0;
  s_receive_buffer_full = false;
  s_transmit_buffer = 0;
  s_transmit_buffer_full = false;
  ResetDeviceTransferState();
  UpdateJoyStat();
}

void Pad::UpdateJoyStat()
{
  s_JOY_STAT.RXFIFONEMPTY = s_receive_buffer_full;
  s_JOY_STAT.TXDONE = !s_transmit_buffer_full && s_state != State::Transmitting;
  s_JOY_STAT.TXRDY = !s_transmit_buffer_full;
}

void Pad::TransferEvent(void*, TickCount ticks, TickCount ticks_late)
{
  if (s_state == State::Transmitting)
    DoTransfer(ticks_late);
  else
    DoACK();
}

void Pad::BeginTransfer()
{
  DebugAssert(s_state == State::Idle && CanTransfer());
  Log_DebugPrintf("Starting transfer");

  s_JOY_CTRL.RXEN = true;
  s_transmit_value = s_transmit_buffer;
  s_transmit_buffer_full = false;

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

  s_state = State::Transmitting;
  s_transfer_event->SetPeriodAndSchedule(GetTransferTicks());
}

void Pad::DoTransfer(TickCount ticks_late)
{
  Log_DebugPrintf("Transferring slot %d", s_JOY_CTRL.SLOT.GetValue());

  const u8 device_index = s_multitaps[0].IsEnabled() ? 4u : s_JOY_CTRL.SLOT;
  Controller* const controller = s_controllers[device_index].get();
  MemoryCard* const memory_card = s_memory_cards[device_index].get();

  // set rx?
  s_JOY_CTRL.RXEN = true;

  const u8 data_out = s_transmit_value;

  u8 data_in = 0xFF;
  bool ack = false;

  switch (s_active_device)
  {
    case ActiveDevice::None:
    {
      if (s_multitaps[s_JOY_CTRL.SLOT].IsEnabled())
      {
        if ((ack = s_multitaps[s_JOY_CTRL.SLOT].Transfer(data_out, &data_in)) == true)
        {
          Log_TracePrintf("Active device set to tap %d, sent 0x%02X, received 0x%02X",
                          static_cast<int>(s_JOY_CTRL.SLOT), data_out, data_in);
          s_active_device = ActiveDevice::Multitap;
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
            s_active_device = ActiveDevice::MemoryCard;

            // back up memory card state in case we roll back to before this transfer begun
            const u32 frame_number = System::GetFrameNumber();

            // consider u32 overflow case
            if (ShouldAvoidSavingToState() &&
                (frame_number - s_last_memory_card_transfer_frame) > GetMaximumRollbackFrames())
              BackupMemoryCardState();

            s_last_memory_card_transfer_frame = frame_number;
          }
        }
        else
        {
          // controller responded, make it the active device until non-ack
          Log_TracePrintf("Transfer to controller, data_out=0x%02X, data_in=0x%02X", data_out, data_in);
          s_active_device = ActiveDevice::Controller;
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
        s_last_memory_card_transfer_frame = System::GetFrameNumber();
        ack = memory_card->Transfer(data_out, &data_in);
        Log_TracePrintf("Transfer to memory card, data_out=0x%02X, data_in=0x%02X", data_out, data_in);
      }
    }
    break;

    case ActiveDevice::Multitap:
    {
      if (s_multitaps[s_JOY_CTRL.SLOT].IsEnabled())
      {
        ack = s_multitaps[s_JOY_CTRL.SLOT].Transfer(data_out, &data_in);
        Log_TracePrintf("Transfer tap %d, sent 0x%02X, received 0x%02X, acked: %s", static_cast<int>(s_JOY_CTRL.SLOT),
                        data_out, data_in, ack ? "true" : "false");
      }
    }
    break;
  }

  s_receive_buffer = data_in;
  s_receive_buffer_full = true;

  // device no longer active?
  if (!ack)
  {
    s_active_device = ActiveDevice::None;
    EndTransfer();
  }
  else
  {
    const bool memcard_transfer =
      s_active_device == ActiveDevice::MemoryCard ||
      (s_active_device == ActiveDevice::Multitap && s_multitaps[s_JOY_CTRL.SLOT].IsReadingMemoryCard());

    const TickCount ack_timer = GetACKTicks(memcard_transfer);
    Log_DebugPrintf("Delaying ACK for %d ticks", ack_timer);
    s_state = State::WaitingForACK;
    s_transfer_event->SetPeriodAndSchedule(ack_timer);
  }

  UpdateJoyStat();
}

void Pad::DoACK()
{
  s_JOY_STAT.ACKINPUT = true;

  if (s_JOY_CTRL.ACKINTEN)
  {
    Log_DebugPrintf("Triggering ACK interrupt");
    s_JOY_STAT.INTR = true;
    InterruptController::SetLineState(InterruptController::IRQ::PAD, true);
  }

  EndTransfer();
  UpdateJoyStat();

  if (CanTransfer())
    BeginTransfer();
}

void Pad::EndTransfer()
{
  DebugAssert(s_state == State::Transmitting || s_state == State::WaitingForACK);
  Log_DebugPrintf("Ending transfer");

  s_state = State::Idle;
  s_transfer_event->Deactivate();
}

void Pad::ResetDeviceTransferState()
{
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    if (s_controllers[i])
      s_controllers[i]->ResetTransferState();
    if (s_memory_cards[i])
      s_memory_cards[i]->ResetTransferState();
  }

  for (u32 i = 0; i < NUM_MULTITAPS; i++)
    s_multitaps[i].ResetTransferState();

  s_active_device = ActiveDevice::None;
}
