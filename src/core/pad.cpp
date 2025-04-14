// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

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

#include "IconsEmoji.h"
#include "IconsFontAwesome5.h"
#include "fmt/format.h"

#include <array>
#include <memory>

LOG_CHANNEL(Pad);

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

union JOYCTRLRegister
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

union JOYSTATRegister
{
  u32 bits;

  BitField<u32, bool, 0, 1> TXRDY;
  BitField<u32, bool, 1, 1> RXFIFONEMPTY;
  BitField<u32, bool, 2, 1> TXDONE;
  BitField<u32, bool, 7, 1> ACKINPUT;
  BitField<u32, bool, 9, 1> INTR;
  BitField<u32, u32, 11, 21> TMR;
};

union JOYMODERegister
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
static void TriggerIRQ(const char* type);

static bool DoStateController(StateWrapper& sw, u32 i);
static bool DoStateMemcard(StateWrapper& sw, u32 i, bool is_memory_state);
static MemoryCard* GetDummyMemcard();
static void BackupMemoryCardState();
static void RestoreMemoryCardState();

namespace {

struct PadState
{
  std::array<std::unique_ptr<Controller>, NUM_CONTROLLER_AND_CARD_PORTS> controllers;
  std::array<std::unique_ptr<MemoryCard>, NUM_CONTROLLER_AND_CARD_PORTS> memory_cards;

  std::array<Multitap, NUM_MULTITAPS> multitaps;

  TimingEvent transfer_event{"Pad Serial Transfer", 1, 1, &Pad::TransferEvent, nullptr};
  State state = State::Idle;

  JOYSTATRegister JOY_STAT = {};
  JOYCTRLRegister JOY_CTRL = {};
  JOYMODERegister JOY_MODE = {};
  u16 JOY_BAUD = 0;

  ActiveDevice active_device = ActiveDevice::None;
  u8 receive_buffer = 0;
  u8 transmit_buffer = 0;
  u8 transmit_value = 0;
  bool receive_buffer_full = false;
  bool transmit_buffer_full = false;

  u32 last_memory_card_transfer_frame = 0;
  DynamicHeapArray<u8> memory_card_backup;
  std::unique_ptr<MemoryCard> dummy_card;
};
} // namespace

ALIGN_TO_CACHE_LINE static PadState s_state;

} // namespace Pad

void Pad::Initialize()
{
  Reset();
}

void Pad::Shutdown()
{
  s_state.memory_card_backup.deallocate();

  s_state.transfer_event.Deactivate();

  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    s_state.controllers[i].reset();
    s_state.memory_cards[i].reset();
  }
}

void Pad::Reset()
{
  SoftReset();

  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    if (s_state.controllers[i])
      s_state.controllers[i]->Reset();

    if (s_state.memory_cards[i])
      s_state.memory_cards[i]->Reset();
  }

  for (u32 i = 0; i < NUM_MULTITAPS; i++)
    s_state.multitaps[i].Reset();
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
  const ControllerType controller_type =
    s_state.controllers[i] ? s_state.controllers[i]->GetType() : ControllerType::None;
  ControllerType controller_type_in_state = controller_type;

  // Data type change...
  u32 state_controller_type_value = static_cast<u32>(controller_type_in_state);
  sw.Do(&state_controller_type_value);
  controller_type_in_state = static_cast<ControllerType>(state_controller_type_value);

  if (controller_type != controller_type_in_state)
  {
    const Controller::ControllerInfo& state_cinfo = Controller::GetControllerInfo(controller_type_in_state);
    Assert(sw.IsReading());

    DEV_LOG("Controller type mismatch in slot {}: state={}({}) ui={}({})", i + 1u, state_cinfo.name,
            static_cast<unsigned>(controller_type_in_state), Controller::GetControllerInfo(controller_type).name,
            static_cast<unsigned>(controller_type));

    Host::AddIconOSDWarning(
      fmt::format("PadTypeMismatch{}", i), ICON_EMOJI_WARNING,
      fmt::format(TRANSLATE_FS("OSDMessage",
                               "Save state contains controller type {0} in port {1}.\n       Leaving {2} connected."),
                  state_cinfo.GetDisplayName(), i + 1u,
                  Controller::GetControllerInfo(controller_type).GetDisplayName()),
      Host::OSD_WARNING_DURATION);

    if (s_state.controllers[i])
      s_state.controllers[i]->Reset();
  }

  // Still need to consume the state. If we saved the size, this would be better, since we could just skip over it.
  if (controller_type_in_state == ControllerType::None)
    return true;

  if (!sw.DoMarker("Controller"))
    return false;
  if (const auto& controller = s_state.controllers[i]; controller && controller->GetType() == controller_type_in_state)
    return controller->DoState(sw, g_settings.load_devices_from_save_states);
  else if (const auto dummy = Controller::Create(controller_type_in_state, i); dummy)
    return dummy->DoState(sw, g_settings.load_devices_from_save_states);

  return true;
}

bool Pad::DoStateMemcard(StateWrapper& sw, u32 i, bool is_memory_state)
{
  bool card_present_in_state = static_cast<bool>(s_state.memory_cards[i]);

  sw.Do(&card_present_in_state);

  if (card_present_in_state && !s_state.memory_cards[i] && g_settings.load_devices_from_save_states)
  {
    Host::AddIconOSDMessage(
      fmt::format("CardLoadWarning{}", i), ICON_FA_SD_CARD,
      fmt::format(
        TRANSLATE_FS("OSDMessage", "Memory card {} present in save state but not in system. Creating temporary card."),
        i + 1u),
      Host::OSD_ERROR_DURATION);
    s_state.memory_cards[i] = MemoryCard::Create();
  }

  MemoryCard* card_ptr = s_state.memory_cards[i].get();
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

  if (card_ptr != s_state.memory_cards[i].get())
  {
    if (s_state.memory_cards[i])
    {
      if (s_state.memory_cards[i]->GetData() == card_ptr->GetData())
      {
        DEV_LOG("Card {} data matches, copying state", i + 1u);
        s_state.memory_cards[i]->CopyState(card_ptr);
      }
      else
      {
        Host::AddIconOSDMessage(
          fmt::format("CardLoadWarning{}", i), ICON_FA_SD_CARD,
          fmt::format(
            TRANSLATE_FS("OSDMessage",
                         "Memory card {} from save state does not match current card data. Simulating replugging."),
            i + 1u),
          Host::OSD_WARNING_DURATION);

        WARNING_LOG("Memory card {} data mismatch. Using current data via instant-replugging.", i + 1u);
        s_state.memory_cards[i]->Reset();
      }
    }
    else
    {
      Host::AddIconOSDMessage(
        fmt::format("CardLoadWarning{}", i), ICON_FA_SD_CARD,
        fmt::format(
          TRANSLATE_FS("OSDMessage", "Memory card {} present in save state but not in system. Ignoring card."), i + 1u),
        Host::OSD_ERROR_DURATION);
    }

    return true;
  }

  if (!card_present_in_state && s_state.memory_cards[i])
  {
    if (g_settings.load_devices_from_save_states)
    {
      Host::AddIconOSDMessage(
        fmt::format("CardLoadWarning{}", i), ICON_FA_SD_CARD,
        fmt::format(
          TRANSLATE_FS("OSDMessage", "Memory card {} present in system but not in save state. Removing card."), i + 1u),
        Host::OSD_ERROR_DURATION);
      s_state.memory_cards[i].reset();
    }
    else
    {
      Host::AddIconOSDMessage(
        fmt::format("CardLoadWarning{}", i), ICON_FA_SD_CARD,
        fmt::format(
          TRANSLATE_FS("OSDMessage", "Memory card {} present in system but not in save state. Replugging card."),
          i + 1u),
        Host::OSD_WARNING_DURATION);
      s_state.memory_cards[i]->Reset();
    }
  }

  return true;
}

MemoryCard* Pad::GetDummyMemcard()
{
  if (!s_state.dummy_card)
    s_state.dummy_card = MemoryCard::Create();
  return s_state.dummy_card.get();
}

void Pad::BackupMemoryCardState()
{
  DEV_LOG("Backing up memory card state.");

  if (s_state.memory_card_backup.empty())
    s_state.memory_card_backup.resize(MemoryCard::STATE_SIZE * NUM_CONTROLLER_AND_CARD_PORTS);

  StateWrapper sw(s_state.memory_card_backup.span(), StateWrapper::Mode::Write, SAVE_STATE_VERSION);
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    if (s_state.memory_cards[i])
      s_state.memory_cards[i]->DoState(sw);
  }
}

void Pad::RestoreMemoryCardState()
{
  DebugAssert(!s_state.memory_card_backup.empty());

  VERBOSE_LOG("Restoring backed up memory card state.");

  StateWrapper sw(s_state.memory_card_backup.cspan(), StateWrapper::Mode::Read, SAVE_STATE_VERSION);
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    if (s_state.memory_cards[i])
      s_state.memory_cards[i]->DoState(sw);
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
    const u32 frames_since_transfer = frame_number - s_state.last_memory_card_transfer_frame;
    const u32 prev_transfer_frame = s_state.last_memory_card_transfer_frame;
    bool state_has_memcards = false;

    sw.Do(&s_state.last_memory_card_transfer_frame);

    // If there's been a transfer within the last N_ROLLBACK frames, include the memory card state when saving.
    state_has_memcards = (frames_since_transfer <= GetMaximumRollbackFrames());
    sw.Do(&state_has_memcards);

    if (sw.IsReading())
    {
      // If no transfers have occurred, no need to reload state.
      if (s_state.last_memory_card_transfer_frame != frame_number &&
          s_state.last_memory_card_transfer_frame == prev_transfer_frame)
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
        if (s_state.memory_cards[i])
        {
          MemoryCard* const mc = process_memcard_state ? s_state.memory_cards[i].get() : dummy_card;
          mc->DoState(sw);
        }
      }
    }

    // Always save controller state.
    for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
    {
      if (s_state.controllers[i])
      {
        // Ignore input state, use the current. I think we want this?
        s_state.controllers[i]->DoState(sw, false);
      }
    }
  }
  else
  {
    for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
    {
      if ((sw.GetVersion() < 50) && (i >= 2)) [[unlikely]]
      {
        // loading from old savestate which only had max 2 controllers.
        // honoring load_devices_from_save_states in this case seems debatable, but might as well...
        if (s_state.controllers[i])
        {
          if (g_settings.load_devices_from_save_states)
            s_state.controllers[i].reset();
          else
            s_state.controllers[i]->Reset();
        }

        if (s_state.memory_cards[i])
        {
          if (g_settings.load_devices_from_save_states)
            s_state.memory_cards[i].reset();
          else
            s_state.memory_cards[i]->Reset();
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

  if (sw.GetVersion() >= 50) [[unlikely]]
  {
    for (u32 i = 0; i < NUM_MULTITAPS; i++)
    {
      if (!s_state.multitaps[i].DoState(sw))
        return false;
    }
  }

  sw.Do(&s_state.state);
  sw.Do(&s_state.JOY_CTRL.bits);
  sw.Do(&s_state.JOY_STAT.bits);
  sw.Do(&s_state.JOY_MODE.bits);
  sw.Do(&s_state.JOY_BAUD);
  sw.Do(&s_state.receive_buffer);
  sw.Do(&s_state.transmit_buffer);
  sw.Do(&s_state.receive_buffer_full);
  sw.Do(&s_state.transmit_buffer_full);

  if (sw.IsReading() && IsTransmitting())
    s_state.transfer_event.Activate();

  return !sw.HasError();
}

Controller* Pad::GetController(u32 slot)
{
  return s_state.controllers[slot].get();
}

void Pad::SetController(u32 slot, std::unique_ptr<Controller> dev)
{
  s_state.controllers[slot] = std::move(dev);
}

MemoryCard* Pad::GetMemoryCard(u32 slot)
{
  return s_state.memory_cards[slot].get();
}

void Pad::SetMemoryCard(u32 slot, std::unique_ptr<MemoryCard> dev)
{
  INFO_LOG("Memory card slot {}: {}", slot,
           dev ? (dev->GetPath().empty() ? "<no file configured>" : dev->GetPath().c_str()) : "<unplugged>");

  s_state.memory_cards[slot] = std::move(dev);
}

std::unique_ptr<MemoryCard> Pad::RemoveMemoryCard(u32 slot)
{
  std::unique_ptr<MemoryCard> ret = std::move(s_state.memory_cards[slot]);
  if (ret)
    ret->Reset();
  return ret;
}

Multitap* Pad::GetMultitap(u32 slot)
{
  return &s_state.multitaps[slot];
}

void Pad::TriggerIRQ(const char* type)
{
  DEBUG_LOG("Triggering {} interrupt", type);
  s_state.JOY_STAT.INTR = true;
  InterruptController::SetLineState(InterruptController::IRQ::PAD, true);
}

u32 Pad::ReadRegister(u32 offset)
{
  switch (offset)
  {
    case 0x00: // JOY_DATA
    {
      if (IsTransmitting())
        s_state.transfer_event.InvokeEarly();

      const u8 value = s_state.receive_buffer_full ? s_state.receive_buffer : 0xFF;
      DEBUG_LOG("JOY_DATA (R) -> 0x{:02X}{}", value, s_state.receive_buffer_full ? "" : "(EMPTY)");
      s_state.receive_buffer_full = false;
      UpdateJoyStat();

      return (ZeroExtend32(value) | (ZeroExtend32(value) << 8) | (ZeroExtend32(value) << 16) |
              (ZeroExtend32(value) << 24));
    }

    case 0x04: // JOY_STAT
    {
      if (IsTransmitting())
        s_state.transfer_event.InvokeEarly();

      const u32 bits = s_state.JOY_STAT.bits;
      s_state.JOY_STAT.ACKINPUT = false;
      return bits;
    }

    case 0x08: // JOY_MODE
      return ZeroExtend32(s_state.JOY_MODE.bits);

    case 0x0A: // JOY_CTRL
      return ZeroExtend32(s_state.JOY_CTRL.bits);

    case 0x0E: // JOY_BAUD
      return ZeroExtend32(s_state.JOY_BAUD);

    [[unlikely]] default:
      ERROR_LOG("Unknown register read: 0x{:X}", offset);
      return UINT32_C(0xFFFFFFFF);
  }
}

void Pad::WriteRegister(u32 offset, u32 value)
{
  switch (offset)
  {
    case 0x00: // JOY_DATA
    {
      DEBUG_LOG("JOY_DATA (W) <- 0x{:02X}", value);

      if (s_state.transmit_buffer_full)
        WARNING_LOG("TX FIFO overrun");

      s_state.transmit_buffer = Truncate8(value);
      s_state.transmit_buffer_full = true;

      if (s_state.JOY_CTRL.TXINTEN)
        TriggerIRQ("TX");

      if (!IsTransmitting() && CanTransfer())
        BeginTransfer();

      return;
    }

    case 0x0A: // JOY_CTRL
    {
      DEBUG_LOG("JOY_CTRL <- 0x{:04X}", value);

      s_state.JOY_CTRL.bits = Truncate16(value);
      if (s_state.JOY_CTRL.RESET)
        SoftReset();

      if (s_state.JOY_CTRL.ACK)
      {
        // reset stat bits
        s_state.JOY_STAT.INTR = false;
        InterruptController::SetLineState(InterruptController::IRQ::PAD, false);
      }

      if (!s_state.JOY_CTRL.SELECT)
        ResetDeviceTransferState();

      if (!s_state.JOY_CTRL.SELECT || !s_state.JOY_CTRL.TXEN)
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
      DEBUG_LOG("JOY_MODE <- 0x{:08X}", value);
      s_state.JOY_MODE.bits = Truncate16(value);
      return;
    }

    case 0x0E:
    {
      DEBUG_LOG("JOY_BAUD <- 0x{:08X}", value);
      s_state.JOY_BAUD = Truncate16(value);
      return;
    }

    [[unlikely]] default:
    {
      ERROR_LOG("Unknown register write: 0x{:X} <- 0x{:08X}", offset, value);
      return;
    }
  }
}

bool Pad::IsTransmitting()
{
  return s_state.state != State::Idle;
}

bool Pad::CanTransfer()
{
  return s_state.transmit_buffer_full && s_state.JOY_CTRL.SELECT && s_state.JOY_CTRL.TXEN;
}

TickCount Pad::GetTransferTicks()
{
  return static_cast<TickCount>(ZeroExtend32(s_state.JOY_BAUD) * 8);
}

void Pad::SoftReset()
{
  if (IsTransmitting())
    EndTransfer();

  s_state.JOY_CTRL.bits = 0;
  s_state.JOY_STAT.bits = 0;
  s_state.JOY_MODE.bits = 0;
  s_state.receive_buffer = 0;
  s_state.receive_buffer_full = false;
  s_state.transmit_buffer = 0;
  s_state.transmit_buffer_full = false;
  ResetDeviceTransferState();
  UpdateJoyStat();
}

void Pad::UpdateJoyStat()
{
  s_state.JOY_STAT.RXFIFONEMPTY = s_state.receive_buffer_full;
  s_state.JOY_STAT.TXDONE = !s_state.transmit_buffer_full && s_state.state != State::Transmitting;
  s_state.JOY_STAT.TXRDY = !s_state.transmit_buffer_full;
}

void Pad::TransferEvent(void*, TickCount ticks, TickCount ticks_late)
{
  if (s_state.state == State::Transmitting)
    DoTransfer(ticks_late);
  else
    DoACK();
}

void Pad::BeginTransfer()
{
  DebugAssert(s_state.state == State::Idle && CanTransfer());
  DEBUG_LOG("Starting transfer");

  s_state.JOY_CTRL.RXEN = true;
  s_state.transmit_value = s_state.transmit_buffer;
  s_state.transmit_buffer_full = false;

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

  s_state.state = State::Transmitting;
  s_state.transfer_event.SetPeriodAndSchedule(GetTransferTicks());
}

void Pad::DoTransfer(TickCount ticks_late)
{
  DEBUG_LOG("Transferring slot {}", s_state.JOY_CTRL.SLOT.GetValue());

  const u8 device_index = s_state.multitaps[0].IsEnabled() ? 4u : s_state.JOY_CTRL.SLOT;
  Controller* const controller = s_state.controllers[device_index].get();
  MemoryCard* const memory_card = s_state.memory_cards[device_index].get();

  // set rx?
  s_state.JOY_CTRL.RXEN = true;

  const u8 data_out = s_state.transmit_value;

  u8 data_in = 0xFF;
  bool ack = false;

  switch (s_state.active_device)
  {
    case ActiveDevice::None:
    {
      if (s_state.multitaps[s_state.JOY_CTRL.SLOT].IsEnabled())
      {
        if ((ack = s_state.multitaps[s_state.JOY_CTRL.SLOT].Transfer(data_out, &data_in)) == true)
        {
          TRACE_LOG("Active device set to tap {}, sent 0x{:02X}, received 0x{:02X}",
                    static_cast<int>(s_state.JOY_CTRL.SLOT), data_out, data_in);
          s_state.active_device = ActiveDevice::Multitap;
        }
      }
      else
      {
        if (!controller || (ack = controller->Transfer(data_out, &data_in)) == false)
        {
          if (!memory_card || (ack = memory_card->Transfer(data_out, &data_in)) == false)
          {
            // nothing connected to this port
            TRACE_LOG("Nothing connected or ACK'ed");
          }
          else
          {
            // memory card responded, make it the active device until non-ack
            TRACE_LOG("Transfer to memory card, data_out=0x{:02X}, data_in=0x{:02X}", data_out, data_in);
            s_state.active_device = ActiveDevice::MemoryCard;

            // back up memory card state in case we roll back to before this transfer begun
            const u32 frame_number = System::GetFrameNumber();

            // consider u32 overflow case
            if (ShouldAvoidSavingToState() &&
                (frame_number - s_state.last_memory_card_transfer_frame) > GetMaximumRollbackFrames())
              BackupMemoryCardState();

            s_state.last_memory_card_transfer_frame = frame_number;
          }
        }
        else
        {
          // controller responded, make it the active device until non-ack
          TRACE_LOG("Transfer to controller, data_out=0x{:02X}, data_in=0x{:02X}", data_out, data_in);
          s_state.active_device = ActiveDevice::Controller;
        }
      }
    }
    break;

    case ActiveDevice::Controller:
    {
      if (controller)
      {
        ack = controller->Transfer(data_out, &data_in);
        TRACE_LOG("Transfer to controller, data_out=0x{:02X}, data_in=0x{:02X}", data_out, data_in);
      }
    }
    break;

    case ActiveDevice::MemoryCard:
    {
      if (memory_card)
      {
        s_state.last_memory_card_transfer_frame = System::GetFrameNumber();
        ack = memory_card->Transfer(data_out, &data_in);
        TRACE_LOG("Transfer to memory card, data_out=0x{:02X}, data_in=0x{:02X}", data_out, data_in);
      }
    }
    break;

    case ActiveDevice::Multitap:
    {
      if (s_state.multitaps[s_state.JOY_CTRL.SLOT].IsEnabled())
      {
        ack = s_state.multitaps[s_state.JOY_CTRL.SLOT].Transfer(data_out, &data_in);
        TRACE_LOG("Transfer tap {}, sent 0x{:02X}, received 0x{:02X}, acked: {}",
                  static_cast<int>(s_state.JOY_CTRL.SLOT), data_out, data_in, ack ? "true" : "false");
      }
    }
    break;
  }

  s_state.receive_buffer = data_in;
  s_state.receive_buffer_full = true;
  if (s_state.JOY_CTRL.RXINTEN)
    TriggerIRQ("TX");

  // device no longer active?
  if (!ack)
  {
    s_state.active_device = ActiveDevice::None;
    EndTransfer();
  }
  else
  {
    const bool memcard_transfer = s_state.active_device == ActiveDevice::MemoryCard ||
                                  (s_state.active_device == ActiveDevice::Multitap &&
                                   s_state.multitaps[s_state.JOY_CTRL.SLOT].IsReadingMemoryCard());

    const TickCount ack_timer = GetACKTicks(memcard_transfer);
    DEBUG_LOG("Delaying ACK for {} ticks", ack_timer);
    s_state.state = State::WaitingForACK;
    s_state.transfer_event.SetPeriodAndSchedule(ack_timer);
  }

  UpdateJoyStat();
}

void Pad::DoACK()
{
  s_state.JOY_STAT.ACKINPUT = true;

  if (s_state.JOY_CTRL.ACKINTEN)
    TriggerIRQ("ACK");

  EndTransfer();
  UpdateJoyStat();

  if (CanTransfer())
    BeginTransfer();
}

void Pad::EndTransfer()
{
  DebugAssert(s_state.state == State::Transmitting || s_state.state == State::WaitingForACK);
  DEBUG_LOG("Ending transfer");

  s_state.state = State::Idle;
  s_state.transfer_event.Deactivate();
}

void Pad::ResetDeviceTransferState()
{
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    if (s_state.controllers[i])
      s_state.controllers[i]->ResetTransferState();
    if (s_state.memory_cards[i])
      s_state.memory_cards[i]->ResetTransferState();
  }

  for (u32 i = 0; i < NUM_MULTITAPS; i++)
    s_state.multitaps[i].ResetTransferState();

  s_state.active_device = ActiveDevice::None;
}
