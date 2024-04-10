// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "dma.h"
#include "bus.h"
#include "cdrom.h"
#include "cpu_code_cache.h"
#include "cpu_core.h"
#include "gpu.h"
#include "host.h"
#include "imgui.h"
#include "interrupt_controller.h"
#include "mdec.h"
#include "pad.h"
#include "spu.h"
#include "system.h"

#include "util/imgui_manager.h"
#include "util/state_wrapper.h"

#include "common/bitfield.h"
#include "common/log.h"
#include "common/string_util.h"

#include "fmt/format.h"

#include <array>
#include <memory>
#include <vector>

Log_SetChannel(DMA);

namespace DMA {
namespace {

enum class SyncMode : u32
{
  Manual = 0,
  Request = 1,
  LinkedList = 2,
  Reserved = 3
};

static constexpr PhysicalMemoryAddress BASE_ADDRESS_MASK = UINT32_C(0x00FFFFFF);
static constexpr PhysicalMemoryAddress TRANSFER_ADDRESS_MASK = UINT32_C(0x00FFFFFC);
static constexpr PhysicalMemoryAddress LINKED_LIST_TERMINATOR = UINT32_C(0x00FFFFFF);

static constexpr TickCount LINKED_LIST_HEADER_READ_TICKS = 10;
static constexpr TickCount LINKED_LIST_BLOCK_SETUP_TICKS = 5;
static constexpr TickCount SLICE_SIZE_WHEN_TRANSMITTING_PAD = 10;

struct ChannelState
{
  u32 base_address = 0;

  union BlockControl
  {
    u32 bits;
    union
    {
      BitField<u32, u32, 0, 16> word_count;

      u32 GetWordCount() const { return (word_count == 0) ? 0x10000 : word_count; }
    } manual;
    union
    {
      BitField<u32, u32, 0, 16> block_size;
      BitField<u32, u32, 16, 16> block_count;

      u32 GetBlockSize() const { return (block_size == 0) ? 0x10000 : block_size; }
      u32 GetBlockCount() const { return (block_count == 0) ? 0x10000 : block_count; }
    } request;
  } block_control = {};

  union ChannelControl
  {
    u32 bits;
    BitField<u32, bool, 0, 1> copy_to_device;
    BitField<u32, bool, 1, 1> address_step_reverse;
    BitField<u32, bool, 8, 1> chopping_enable;
    BitField<u32, SyncMode, 9, 2> sync_mode;
    BitField<u32, u32, 16, 3> chopping_dma_window_size;
    BitField<u32, u32, 20, 3> chopping_cpu_window_size;
    BitField<u32, bool, 24, 1> enable_busy;
    BitField<u32, bool, 28, 1> start_trigger;

    static constexpr u32 WRITE_MASK = 0b01110001'01110111'00000111'00000011;
  } channel_control = {};

  bool request = false;
};

union DPCR
{
  u32 bits;

  BitField<u32, u8, 0, 3> MDECin_priority;
  BitField<u32, bool, 3, 1> MDECin_master_enable;
  BitField<u32, u8, 4, 3> MDECout_priority;
  BitField<u32, bool, 7, 1> MDECout_master_enable;
  BitField<u32, u8, 8, 3> GPU_priority;
  BitField<u32, bool, 10, 1> GPU_master_enable;
  BitField<u32, u8, 12, 3> CDROM_priority;
  BitField<u32, bool, 15, 1> CDROM_master_enable;
  BitField<u32, u8, 16, 3> SPU_priority;
  BitField<u32, bool, 19, 1> SPU_master_enable;
  BitField<u32, u8, 20, 3> PIO_priority;
  BitField<u32, bool, 23, 1> PIO_master_enable;
  BitField<u32, u8, 24, 3> OTC_priority;
  BitField<u32, bool, 27, 1> OTC_master_enable;
  BitField<u32, u8, 28, 3> priority_offset;
  BitField<u32, bool, 31, 1> unused;

  ALWAYS_INLINE u8 GetPriority(Channel channel) const { return ((bits >> (static_cast<u8>(channel) * 4)) & u32(3)); }
  ALWAYS_INLINE bool GetMasterEnable(Channel channel) const
  {
    return ConvertToBoolUnchecked((bits >> (static_cast<u8>(channel) * 4 + 3)) & u32(1));
  }
};

static constexpr u32 DICR_WRITE_MASK = 0b00000000'11111111'10000000'00111111;
static constexpr u32 DICR_RESET_MASK = 0b01111111'00000000'00000000'00000000;
union DICR
{
  u32 bits;

  BitField<u32, bool, 15, 1> bus_error;
  BitField<u32, bool, 16, 1> MDECin_irq_enable;
  BitField<u32, bool, 17, 1> MDECout_irq_enable;
  BitField<u32, bool, 18, 1> GPU_irq_enable;
  BitField<u32, bool, 19, 1> CDROM_irq_enable;
  BitField<u32, bool, 20, 1> SPU_irq_enable;
  BitField<u32, bool, 21, 1> PIO_irq_enable;
  BitField<u32, bool, 22, 1> OTC_irq_enable;
  BitField<u32, bool, 23, 1> master_enable;
  BitField<u32, bool, 24, 1> MDECin_irq_flag;
  BitField<u32, bool, 25, 1> MDECout_irq_flag;
  BitField<u32, bool, 26, 1> GPU_irq_flag;
  BitField<u32, bool, 27, 1> CDROM_irq_flag;
  BitField<u32, bool, 28, 1> SPU_irq_flag;
  BitField<u32, bool, 29, 1> PIO_irq_flag;
  BitField<u32, bool, 30, 1> OTC_irq_flag;
  BitField<u32, bool, 31, 1> master_flag;

  ALWAYS_INLINE bool GetIRQEnabled(Channel channel) const
  {
    return ConvertToBoolUnchecked((bits >> (static_cast<u8>(channel) + 16)) & 1u);
  }

  ALWAYS_INLINE bool GetIRQFlag(Channel channel) const
  {
    return ConvertToBoolUnchecked((bits >> (static_cast<u8>(channel) + 24)) & 1u);
  }

  ALWAYS_INLINE void SetIRQFlag(Channel channel) { bits |= (1u << (static_cast<u8>(channel) + 24)); }

  ALWAYS_INLINE bool ShouldSetIRQFlag(Channel channel)
  {
    // bus errors trigger IRQ unconditionally, completion requires the master flag to be enabled
    return ConvertToBoolUnchecked(((bits >> (static_cast<u8>(channel) + 16)) & ((bits >> 23) & 1u)));
  }

  ALWAYS_INLINE void UpdateMasterFlag()
  {
    master_flag =
      (((bits & (1u << 15)) != 0u) ||                                             // bus error, or
       (((bits & (1u << 23)) != 0u) != 0u && (bits & (0b1111111u << 24)) != 0u)); // master enable + irq on any channel
  }
};
} // namespace

static void ClearState();

// is everything enabled for a channel to operate?
static bool CanTransferChannel(Channel channel, bool ignore_halt);
static bool IsTransferHalted();
static void UpdateIRQ();

static void HaltTransfer(TickCount duration);
static void UnhaltTransfer(void*, TickCount ticks, TickCount ticks_late);

template<Channel channel>
static bool TransferChannel();

static bool IsLinkedListTerminator(PhysicalMemoryAddress address);
static bool CheckForBusError(Channel channel, ChannelState& cs, PhysicalMemoryAddress address, u32 size);
static void CompleteTransfer(Channel channel, ChannelState& cs);

// from device -> memory
template<Channel channel>
static TickCount TransferDeviceToMemory(u32 address, u32 increment, u32 word_count);

// from memory -> device
template<Channel channel>
static TickCount TransferMemoryToDevice(u32 address, u32 increment, u32 word_count);

static TickCount GetMaxSliceTicks();

// configuration
static TickCount s_max_slice_ticks = 1000;
static TickCount s_halt_ticks = 100;

static std::vector<u32> s_transfer_buffer;
static std::unique_ptr<TimingEvent> s_unhalt_event;
static TickCount s_halt_ticks_remaining = 0;

static std::array<ChannelState, NUM_CHANNELS> s_state;
static DPCR s_DPCR = {};
static DICR s_DICR = {};

static constexpr std::array<bool (*)(), NUM_CHANNELS> s_channel_transfer_functions = {{
  &TransferChannel<Channel::MDECin>,
  &TransferChannel<Channel::MDECout>,
  &TransferChannel<Channel::GPU>,
  &TransferChannel<Channel::CDROM>,
  &TransferChannel<Channel::SPU>,
  &TransferChannel<Channel::PIO>,
  &TransferChannel<Channel::OTC>,
}};

[[maybe_unused]] static constexpr std::array<const char*, NUM_CHANNELS> s_channel_names = {
  {"MDECin", "MDECout", "GPU", "CDROM", "SPU", "PIO", "OTC"}};

}; // namespace DMA

template<>
struct fmt::formatter<DMA::Channel> : fmt::formatter<fmt::string_view>
{
  auto format(DMA::Channel channel, fmt::format_context& ctx) const
  {
    return formatter<fmt::string_view>::format(DMA::s_channel_names[static_cast<u32>(channel)], ctx);
  }
};

void DMA::Initialize()
{
  s_max_slice_ticks = g_settings.dma_max_slice_ticks;
  s_halt_ticks = g_settings.dma_halt_ticks;

  s_unhalt_event =
    TimingEvents::CreateTimingEvent("DMA Transfer Unhalt", 1, s_max_slice_ticks, &DMA::UnhaltTransfer, nullptr, false);
  Reset();
}

void DMA::Shutdown()
{
  ClearState();
  s_unhalt_event.reset();
}

void DMA::Reset()
{
  ClearState();
  s_unhalt_event->Deactivate();
}

void DMA::ClearState()
{
  for (u32 i = 0; i < NUM_CHANNELS; i++)
  {
    ChannelState& cs = s_state[i];
    cs.base_address = 0;
    cs.block_control.bits = 0;
    cs.channel_control.bits = 0;
    cs.request = false;
  }

  s_DPCR.bits = 0x07654321;
  s_DICR.bits = 0;

  s_halt_ticks_remaining = 0;
}

bool DMA::DoState(StateWrapper& sw)
{
  sw.Do(&s_halt_ticks_remaining);

  for (u32 i = 0; i < NUM_CHANNELS; i++)
  {
    ChannelState& cs = s_state[i];
    sw.Do(&cs.base_address);
    sw.Do(&cs.block_control.bits);
    sw.Do(&cs.channel_control.bits);
    sw.Do(&cs.request);
  }

  sw.Do(&s_DPCR.bits);
  sw.Do(&s_DICR.bits);

  if (sw.IsReading())
  {
    if (s_halt_ticks_remaining > 0)
      s_unhalt_event->SetIntervalAndSchedule(s_halt_ticks_remaining);
    else
      s_unhalt_event->Deactivate();
  }

  return !sw.HasError();
}

u32 DMA::ReadRegister(u32 offset)
{
  const u32 channel_index = offset >> 4;
  if (channel_index < 7)
  {
    switch (offset & UINT32_C(0x0F))
    {
      case 0x00:
      {
        Log_TraceFmt("DMA[{}] base address -> 0x{:08X}", static_cast<Channel>(channel_index),
                     s_state[channel_index].base_address);
        return s_state[channel_index].base_address;
      }
      case 0x04:
      {
        Log_TraceFmt("DMA[{}] block control -> 0x{:08X}", static_cast<Channel>(channel_index),
                     s_state[channel_index].block_control.bits);
        return s_state[channel_index].block_control.bits;
      }
      case 0x08:
      {
        Log_TraceFmt("DMA[{}] channel control -> 0x{:08X}", static_cast<Channel>(channel_index),
                     s_state[channel_index].channel_control.bits);
        return s_state[channel_index].channel_control.bits;
      }
      default:
        break;
    }
  }
  else
  {
    if (offset == 0x70)
    {
      Log_TraceFmt("DPCR -> 0x{:08X}", s_DPCR.bits);
      return s_DPCR.bits;
    }
    else if (offset == 0x74)
    {
      Log_TraceFmt("DICR -> 0x{:08X}", s_DICR.bits);
      return s_DICR.bits;
    }
  }

  Log_ErrorFmt("Unhandled register read: {:02X}", offset);
  return UINT32_C(0xFFFFFFFF);
}

void DMA::WriteRegister(u32 offset, u32 value)
{
  const u32 channel_index = offset >> 4;
  if (channel_index < 7)
  {
    ChannelState& state = s_state[channel_index];
    switch (offset & UINT32_C(0x0F))
    {
      case 0x00:
      {
        state.base_address = value & BASE_ADDRESS_MASK;
        Log_TraceFmt("DMA channel {} base address <- 0x{:08X}", static_cast<Channel>(channel_index),
                     state.base_address);
        return;
      }
      case 0x04:
      {
        Log_TraceFmt("DMA channel {} block control <- 0x{:08X}", static_cast<Channel>(channel_index), value);
        state.block_control.bits = value;
        return;
      }

      case 0x08:
      {
        // HACK: Due to running DMA in slices, we can't wait for the current halt time to finish before running the
        // first block of a new channel. This affects games like FF8, where they kick a SPU transfer while a GPU
        // transfer is happening, and the SPU transfer gets delayed until the GPU transfer unhalts and finishes, and
        // breaks the interrupt.
        const bool ignore_halt = !state.channel_control.enable_busy && (value & (1u << 24));

        state.channel_control.bits = (state.channel_control.bits & ~ChannelState::ChannelControl::WRITE_MASK) |
                                     (value & ChannelState::ChannelControl::WRITE_MASK);
        Log_TraceFmt("DMA channel {} channel control <- 0x{:08X}", static_cast<Channel>(channel_index),
                     state.channel_control.bits);

        // start/trigger bit must be enabled for OTC
        if (static_cast<Channel>(channel_index) == Channel::OTC)
          SetRequest(static_cast<Channel>(channel_index), state.channel_control.start_trigger);

        if (CanTransferChannel(static_cast<Channel>(channel_index), ignore_halt))
        {
          if (static_cast<Channel>(channel_index) != Channel::OTC &&
              state.channel_control.sync_mode == SyncMode::Manual && state.channel_control.chopping_enable)
          {
            // Figure out how roughly many CPU cycles it'll take for the transfer to complete, and delay the transfer.
            // Needed for Lagnacure Legend, which sets DICR to enable interrupts after CHCR to kickstart the transfer.
            // This has an artificial 500 cycle cap, setting it too high causes Namco Museum Vol. 4 and a couple of
            // other games to crash... so clearly something is missing here.
            const u32 block_words = (1u << state.channel_control.chopping_dma_window_size);
            const u32 cpu_cycles_per_block = (1u << state.channel_control.chopping_cpu_window_size);
            const u32 blocks = state.block_control.manual.word_count / block_words;
            const TickCount delay_cycles = std::min(static_cast<TickCount>(cpu_cycles_per_block * blocks), 500);
            if (delay_cycles > 1 && true)
            {
              Log_DevFmt("Delaying {} transfer by {} cycles due to chopping", static_cast<Channel>(channel_index),
                         delay_cycles);
              HaltTransfer(delay_cycles);
            }
            else
            {
              s_channel_transfer_functions[channel_index]();
            }
          }
          else
          {
            s_channel_transfer_functions[channel_index]();
          }
        }
        return;
      }

      default:
        break;
    }
  }
  else
  {
    switch (offset)
    {
      case 0x70:
      {
        Log_TraceFmt("DPCR <- 0x{:08X}", value);
        s_DPCR.bits = value;

        for (u32 i = 0; i < NUM_CHANNELS; i++)
        {
          if (CanTransferChannel(static_cast<Channel>(i), false))
          {
            if (!s_channel_transfer_functions[i]())
              break;
          }
        }

        return;
      }

      case 0x74:
      {
        Log_TraceFmt("DICR <- 0x{:08X}", value);
        s_DICR.bits = (s_DICR.bits & ~DICR_WRITE_MASK) | (value & DICR_WRITE_MASK);
        s_DICR.bits = s_DICR.bits & ~(value & DICR_RESET_MASK);
        UpdateIRQ();
        return;
      }

      default:
        break;
    }
  }

  Log_ErrorFmt("Unhandled register write: {:02X} <- {:08X}", offset, value);
}

void DMA::SetRequest(Channel channel, bool request)
{
  ChannelState& cs = s_state[static_cast<u32>(channel)];
  if (cs.request == request)
    return;

  cs.request = request;
  if (CanTransferChannel(channel, false))
    s_channel_transfer_functions[static_cast<u32>(channel)]();
}

void DMA::SetMaxSliceTicks(TickCount ticks)
{
  s_max_slice_ticks = ticks;
}

void DMA::SetHaltTicks(TickCount ticks)
{
  s_halt_ticks = ticks;
}

ALWAYS_INLINE_RELEASE bool DMA::CanTransferChannel(Channel channel, bool ignore_halt)
{
  if (!s_DPCR.GetMasterEnable(channel))
    return false;

  const ChannelState& cs = s_state[static_cast<u32>(channel)];
  if (!cs.channel_control.enable_busy)
    return false;

  if (cs.channel_control.sync_mode != SyncMode::Manual && (IsTransferHalted() && !ignore_halt))
    return false;

  return cs.request;
}

bool DMA::IsTransferHalted()
{
  return s_unhalt_event->IsActive();
}

void DMA::UpdateIRQ()
{
  [[maybe_unused]] const auto old_dicr = s_DICR;
  s_DICR.UpdateMasterFlag();
  if (!old_dicr.master_flag && s_DICR.master_flag)
    Log_TracePrintf("Firing DMA master interrupt");
  InterruptController::SetLineState(InterruptController::IRQ::DMA, s_DICR.master_flag);
}

ALWAYS_INLINE_RELEASE bool DMA::IsLinkedListTerminator(PhysicalMemoryAddress address)
{
  return ((address & LINKED_LIST_TERMINATOR) == LINKED_LIST_TERMINATOR);
}

ALWAYS_INLINE_RELEASE bool DMA::CheckForBusError(Channel channel, ChannelState& cs, PhysicalMemoryAddress address,
                                                 u32 size)
{
  // Relying on a transfer partially happening at the end of RAM, then hitting a bus error would be pretty silly.
  if ((address + size) > Bus::RAM_8MB_SIZE) [[unlikely]]
  {
    Log_DebugFmt("DMA bus error on channel {} at address 0x{:08X} size {}", channel, address, size);
    cs.channel_control.enable_busy = false;
    s_DICR.bus_error = true;
    s_DICR.SetIRQFlag(channel);
    UpdateIRQ();
    return true;
  }

  return false;
}

ALWAYS_INLINE_RELEASE void DMA::CompleteTransfer(Channel channel, ChannelState& cs)
{
  // start/busy bit is cleared on end of transfer
  Log_DebugFmt("DMA transfer for channel {} complete", channel);
  cs.channel_control.enable_busy = false;
  if (s_DICR.ShouldSetIRQFlag(channel))
  {
    Log_DebugFmt("Setting DMA interrupt for channel {}", channel);
    s_DICR.SetIRQFlag(channel);
    UpdateIRQ();
  }
}

TickCount DMA::GetMaxSliceTicks()
{
  const TickCount max = Pad::IsTransmitting() ? SLICE_SIZE_WHEN_TRANSMITTING_PAD : s_max_slice_ticks;
  if (!TimingEvents::IsRunningEvents())
    return max;

  const u32 current_ticks = TimingEvents::GetGlobalTickCounter();
  const u32 max_ticks = TimingEvents::GetEventRunTickCounter() + static_cast<u32>(max);
  return std::clamp(static_cast<TickCount>(max_ticks - current_ticks), 0, max);
}

template<DMA::Channel channel>
bool DMA::TransferChannel()
{
  ChannelState& cs = s_state[static_cast<u32>(channel)];

  const bool copy_to_device = cs.channel_control.copy_to_device;

  // start/trigger bit is cleared on beginning of transfer
  cs.channel_control.start_trigger = false;

  PhysicalMemoryAddress current_address = cs.base_address;
  const PhysicalMemoryAddress increment = cs.channel_control.address_step_reverse ? static_cast<u32>(-4) : UINT32_C(4);
  switch (cs.channel_control.sync_mode)
  {
    case SyncMode::Manual:
    {
      const u32 word_count = cs.block_control.manual.GetWordCount();
      Log_DebugFmt("DMA[{}]: Copying {} words {} 0x{:08X}", channel, word_count, copy_to_device ? "from" : "to",
                   current_address);

      const PhysicalMemoryAddress transfer_addr = current_address & TRANSFER_ADDRESS_MASK;
      if (CheckForBusError(channel, cs, transfer_addr, word_count * sizeof(u32))) [[unlikely]]
        return true;

      TickCount used_ticks;
      if (copy_to_device)
        used_ticks = TransferMemoryToDevice<channel>(transfer_addr, increment, word_count);
      else
        used_ticks = TransferDeviceToMemory<channel>(transfer_addr, increment, word_count);

      CPU::AddPendingTicks(used_ticks);
      CompleteTransfer(channel, cs);
      return true;
    }

    case SyncMode::LinkedList:
    {
      if (!copy_to_device)
      {
        Panic("Linked list not implemented for DMA reads");
        return true;
      }

      Log_DebugFmt("DMA[{}]: Copying linked list starting at 0x{:08X} to device", channel, current_address);

      // Prove to the compiler that nothing's going to modify these.
      const u8* const ram_ptr = Bus::g_ram;
      const u32 mask = Bus::g_ram_mask;

      const TickCount slice_ticks = GetMaxSliceTicks();
      TickCount remaining_ticks = slice_ticks;
      while (cs.request && remaining_ticks > 0)
      {
        u32 header;
        PhysicalMemoryAddress transfer_addr = current_address & TRANSFER_ADDRESS_MASK;
        if (CheckForBusError(channel, cs, current_address, sizeof(header))) [[unlikely]]
        {
          cs.base_address = current_address;
          return true;
        }

        std::memcpy(&header, &ram_ptr[transfer_addr & mask], sizeof(header));
        const u32 word_count = header >> 24;
        const u32 next_address = header & 0x00FFFFFFu;
        Log_TraceFmt(" .. linked list entry at 0x{:08X} size={}({} words) next=0x{:08X}", current_address,
                     word_count * 4, word_count, next_address);

        const TickCount setup_ticks = (word_count > 0) ?
                                        (LINKED_LIST_HEADER_READ_TICKS + LINKED_LIST_BLOCK_SETUP_TICKS) :
                                        LINKED_LIST_HEADER_READ_TICKS;
        CPU::AddPendingTicks(setup_ticks);
        remaining_ticks -= setup_ticks;

        if (word_count > 0)
        {
          const TickCount block_ticks = TransferMemoryToDevice<channel>(transfer_addr + sizeof(header), 4, word_count);
          CPU::AddPendingTicks(block_ticks);
          remaining_ticks -= block_ticks;
        }

        current_address = next_address;
        if (IsLinkedListTerminator(current_address))
        {
          // Terminator is 24 bits, so is MADR, so it'll always be 0xFFFFFF.
          cs.base_address = LINKED_LIST_TERMINATOR;
          CompleteTransfer(channel, cs);
          return true;
        }
      }

      cs.base_address = current_address;
      if (cs.request)
      {
        // stall the transfer for a bit if we ran for too long
        HaltTransfer(s_halt_ticks);
        return false;
      }
      else
      {
        // linked list not yet complete
        return true;
      }
    }

    case SyncMode::Request:
    {
      Log_DebugFmt("DMA[{}]: Copying {} blocks of size {} ({} total words) {} 0x{:08X}", channel,
                   cs.block_control.request.GetBlockCount(), cs.block_control.request.GetBlockSize(),
                   cs.block_control.request.GetBlockCount() * cs.block_control.request.GetBlockSize(),
                   copy_to_device ? "from" : "to", current_address);

      const u32 block_size = cs.block_control.request.GetBlockSize();
      u32 blocks_remaining = cs.block_control.request.GetBlockCount();
      TickCount ticks_remaining = GetMaxSliceTicks();

      if (copy_to_device)
      {
        do
        {
          const PhysicalMemoryAddress transfer_addr = current_address & TRANSFER_ADDRESS_MASK;
          if (CheckForBusError(channel, cs, transfer_addr, block_size * increment)) [[unlikely]]
          {
            cs.base_address = current_address;
            cs.block_control.request.block_count = blocks_remaining;
            return true;
          }

          const TickCount ticks = TransferMemoryToDevice<channel>(transfer_addr, increment, block_size);
          CPU::AddPendingTicks(ticks);

          ticks_remaining -= ticks;
          blocks_remaining--;

          current_address = (transfer_addr + (increment * block_size));
        } while (cs.request && blocks_remaining > 0 && ticks_remaining > 0);
      }
      else
      {
        do
        {
          const PhysicalMemoryAddress transfer_addr = current_address & TRANSFER_ADDRESS_MASK;
          if (CheckForBusError(channel, cs, transfer_addr, block_size * increment)) [[unlikely]]
          {
            cs.base_address = current_address;
            cs.block_control.request.block_count = blocks_remaining;
            return true;
          }

          const TickCount ticks = TransferDeviceToMemory<channel>(transfer_addr, increment, block_size);
          CPU::AddPendingTicks(ticks);

          ticks_remaining -= ticks;
          blocks_remaining--;

          current_address = (transfer_addr + (increment * block_size));
        } while (cs.request && blocks_remaining > 0 && ticks_remaining > 0);
      }

      cs.base_address = current_address;
      cs.block_control.request.block_count = blocks_remaining;

      // finish transfer later if the request was cleared
      if (blocks_remaining > 0)
      {
        if (cs.request)
        {
          // we got halted
          if (!s_unhalt_event->IsActive())
            HaltTransfer(s_halt_ticks);

          return false;
        }

        return true;
      }

      CompleteTransfer(channel, cs);
      return true;
    }

    default:
      Panic("Unimplemented sync mode");
  }

  UnreachableCode();
}

void DMA::HaltTransfer(TickCount duration)
{
  s_halt_ticks_remaining += duration;
  Log_DebugPrintf("Halting DMA for %d ticks", s_halt_ticks_remaining);
  if (s_unhalt_event->IsActive())
    return;

  DebugAssert(!s_unhalt_event->IsActive());
  s_unhalt_event->SetIntervalAndSchedule(s_halt_ticks_remaining);
}

void DMA::UnhaltTransfer(void*, TickCount ticks, TickCount ticks_late)
{
  Log_DebugPrintf("Resuming DMA after %d ticks, %d ticks late", ticks, -(s_halt_ticks_remaining - ticks));
  s_halt_ticks_remaining -= ticks;
  s_unhalt_event->Deactivate();

  // TODO: Use channel priority. But doing it in ascending order is probably good enough.
  // Main thing is that OTC happens after GPU, because otherwise it'll wipe out the LL.
  for (u32 i = 0; i < NUM_CHANNELS; i++)
  {
    if (CanTransferChannel(static_cast<Channel>(i), false))
    {
      if (!s_channel_transfer_functions[i]())
        return;
    }
  }

  // We didn't run too long, so reset timer.
  s_halt_ticks_remaining = 0;
}

template<DMA::Channel channel>
TickCount DMA::TransferMemoryToDevice(u32 address, u32 increment, u32 word_count)
{
  const u32 mask = Bus::g_ram_mask;
#ifdef _DEBUG
  if ((address & mask) != address)
    Log_DebugFmt("DMA TO {} from masked RAM address 0x{:08X} => 0x{:08X}", channel, address, (address & mask));
#endif

  address &= mask;

  const u32* src_pointer = reinterpret_cast<u32*>(Bus::g_ram + address);
  if constexpr (channel != Channel::GPU)
  {
    if (static_cast<s32>(increment) < 0 || ((address + (increment * word_count)) & mask) <= address) [[unlikely]]
    {
      // Use temp buffer if it's wrapping around
      if (s_transfer_buffer.size() < word_count)
        s_transfer_buffer.resize(word_count);
      src_pointer = s_transfer_buffer.data();

      u8* ram_pointer = Bus::g_ram;
      for (u32 i = 0; i < word_count; i++)
      {
        std::memcpy(&s_transfer_buffer[i], &ram_pointer[address], sizeof(u32));
        address = (address + increment) & mask;
      }
    }
  }

  switch (channel)
  {
    case Channel::GPU:
    {
      if (g_gpu->BeginDMAWrite()) [[likely]]
      {
        u8* ram_pointer = Bus::g_ram;
        for (u32 i = 0; i < word_count; i++)
        {
          u32 value;
          std::memcpy(&value, &ram_pointer[address], sizeof(u32));
          g_gpu->DMAWrite(address, value);
          address = (address + increment) & mask;
        }
        g_gpu->EndDMAWrite();
      }
    }
    break;

    case Channel::SPU:
      SPU::DMAWrite(src_pointer, word_count);
      break;

    case Channel::MDECin:
      MDEC::DMAWrite(src_pointer, word_count);
      break;

    case Channel::CDROM:
    case Channel::MDECout:
    case Channel::PIO:
    default:
      Log_ErrorPrintf("Unhandled DMA channel %u for device write", static_cast<u32>(channel));
      break;
  }

  return Bus::GetDMARAMTickCount(word_count);
}

template<DMA::Channel channel>
TickCount DMA::TransferDeviceToMemory(u32 address, u32 increment, u32 word_count)
{
  const u32 mask = Bus::g_ram_mask;
#ifdef _DEBUG
  if ((address & mask) != address)
    Log_DebugFmt("DMA FROM {} to masked RAM address 0x{:08X} => 0x{:08X}", channel, address, (address & mask));
#endif

  // TODO: This might not be correct for OTC.
  address &= mask;

  if constexpr (channel == Channel::OTC)
  {
    // clear ordering table
    u8* ram_pointer = Bus::g_ram;
    const u32 word_count_less_1 = word_count - 1;
    for (u32 i = 0; i < word_count_less_1; i++)
    {
      u32 next = ((address - 4) & mask);
      std::memcpy(&ram_pointer[address], &next, sizeof(next));
      address = next;
    }

    const u32 terminator = UINT32_C(0xFFFFFF);
    std::memcpy(&ram_pointer[address], &terminator, sizeof(terminator));
    return Bus::GetDMARAMTickCount(word_count);
  }

  u32* dest_pointer = reinterpret_cast<u32*>(&Bus::g_ram[address]);
  if (static_cast<s32>(increment) < 0 || ((address + (increment * word_count)) & mask) <= address) [[unlikely]]
  {
    // Use temp buffer if it's wrapping around
    if (s_transfer_buffer.size() < word_count)
      s_transfer_buffer.resize(word_count);
    dest_pointer = s_transfer_buffer.data();
  }

  // Read from device.
  switch (channel)
  {
    case Channel::GPU:
      g_gpu->DMARead(dest_pointer, word_count);
      break;

    case Channel::CDROM:
      CDROM::DMARead(dest_pointer, word_count);
      break;

    case Channel::SPU:
      SPU::DMARead(dest_pointer, word_count);
      break;

    case Channel::MDECout:
      MDEC::DMARead(dest_pointer, word_count);
      break;

    default:
      Log_ErrorPrintf("Unhandled DMA channel %u for device read", static_cast<u32>(channel));
      std::fill_n(dest_pointer, word_count, UINT32_C(0xFFFFFFFF));
      break;
  }

  if (dest_pointer == s_transfer_buffer.data()) [[unlikely]]
  {
    u8* ram_pointer = Bus::g_ram;
    for (u32 i = 0; i < word_count; i++)
    {
      std::memcpy(&ram_pointer[address], &s_transfer_buffer[i], sizeof(u32));
      address = (address + increment) & mask;
    }
  }

  return Bus::GetDMARAMTickCount(word_count);
}

void DMA::DrawDebugStateWindow()
{
  static constexpr u32 NUM_COLUMNS = 10;
  static constexpr std::array<const char*, NUM_COLUMNS> column_names = {
    {"#", "Req", "Direction", "Chopping", "Mode", "Busy", "Enable", "Priority", "IRQ", "Flag"}};
  static constexpr std::array<const char*, 4> sync_mode_names = {{"Manual", "Request", "LinkedList", "Reserved"}};

  const float framebuffer_scale = Host::GetOSDScale();

  ImGui::SetNextWindowSize(ImVec2(850.0f * framebuffer_scale, 250.0f * framebuffer_scale), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("DMA State", nullptr))
  {
    ImGui::End();
    return;
  }

  ImGui::Columns(NUM_COLUMNS);
  ImGui::SetColumnWidth(0, 100.0f * framebuffer_scale);
  ImGui::SetColumnWidth(1, 50.0f * framebuffer_scale);
  ImGui::SetColumnWidth(2, 100.0f * framebuffer_scale);
  ImGui::SetColumnWidth(3, 150.0f * framebuffer_scale);
  ImGui::SetColumnWidth(4, 80.0f * framebuffer_scale);
  ImGui::SetColumnWidth(5, 80.0f * framebuffer_scale);
  ImGui::SetColumnWidth(6, 80.0f * framebuffer_scale);
  ImGui::SetColumnWidth(7, 80.0f * framebuffer_scale);
  ImGui::SetColumnWidth(8, 80.0f * framebuffer_scale);
  ImGui::SetColumnWidth(9, 80.0f * framebuffer_scale);

  for (const char* title : column_names)
  {
    ImGui::TextUnformatted(title);
    ImGui::NextColumn();
  }

  const ImVec4 active(1.0f, 1.0f, 1.0f, 1.0f);
  const ImVec4 inactive(0.5f, 0.5f, 0.5f, 1.0f);

  for (u32 i = 0; i < NUM_CHANNELS; i++)
  {
    const ChannelState& cs = s_state[i];

    ImGui::TextColored(cs.channel_control.enable_busy ? active : inactive, "%u[%s]", i, s_channel_names[i]);
    ImGui::NextColumn();
    ImGui::TextColored(cs.request ? active : inactive, cs.request ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::Text("%s%s", cs.channel_control.copy_to_device ? "FromRAM" : "ToRAM",
                cs.channel_control.address_step_reverse ? " Addr+" : " Addr-");
    ImGui::NextColumn();
    ImGui::TextColored(cs.channel_control.chopping_enable ? active : inactive, "%s/%u/%u",
                       cs.channel_control.chopping_enable ? "Yes" : "No",
                       cs.channel_control.chopping_cpu_window_size.GetValue(),
                       cs.channel_control.chopping_dma_window_size.GetValue());
    ImGui::NextColumn();
    ImGui::Text("%s", sync_mode_names[static_cast<u8>(cs.channel_control.sync_mode.GetValue())]);
    ImGui::NextColumn();
    ImGui::TextColored(cs.channel_control.enable_busy ? active : inactive, "%s%s",
                       cs.channel_control.enable_busy ? "Busy" : "Idle",
                       cs.channel_control.start_trigger ? " (Trigger)" : "");
    ImGui::NextColumn();
    ImGui::TextColored(s_DPCR.GetMasterEnable(static_cast<Channel>(i)) ? active : inactive,
                       s_DPCR.GetMasterEnable(static_cast<Channel>(i)) ? "Enabled" : "Disabled");
    ImGui::NextColumn();
    ImGui::TextColored(s_DPCR.GetMasterEnable(static_cast<Channel>(i)) ? active : inactive, "%u",
                       s_DPCR.GetPriority(static_cast<Channel>(i)));
    ImGui::NextColumn();
    ImGui::TextColored(s_DICR.GetIRQEnabled(static_cast<Channel>(i)) ? active : inactive,
                       s_DICR.GetIRQEnabled(static_cast<Channel>(i)) ? "Enabled" : "Disabled");
    ImGui::NextColumn();
    ImGui::TextColored(s_DICR.GetIRQFlag(static_cast<Channel>(i)) ? active : inactive,
                       s_DICR.GetIRQFlag(static_cast<Channel>(i)) ? "IRQ" : "");
    ImGui::NextColumn();
  }

  ImGui::Columns(1);
  ImGui::End();
}

// Instantiate channel functions.
template TickCount DMA::TransferDeviceToMemory<DMA::Channel::MDECin>(u32 address, u32 increment, u32 word_count);
template TickCount DMA::TransferMemoryToDevice<DMA::Channel::MDECin>(u32 address, u32 increment, u32 word_count);
template bool DMA::TransferChannel<DMA::Channel::MDECin>();
template TickCount DMA::TransferDeviceToMemory<DMA::Channel::MDECout>(u32 address, u32 increment, u32 word_count);
template TickCount DMA::TransferMemoryToDevice<DMA::Channel::MDECout>(u32 address, u32 increment, u32 word_count);
template bool DMA::TransferChannel<DMA::Channel::MDECout>();
template TickCount DMA::TransferDeviceToMemory<DMA::Channel::GPU>(u32 address, u32 increment, u32 word_count);
template TickCount DMA::TransferMemoryToDevice<DMA::Channel::GPU>(u32 address, u32 increment, u32 word_count);
template bool DMA::TransferChannel<DMA::Channel::GPU>();
template TickCount DMA::TransferDeviceToMemory<DMA::Channel::CDROM>(u32 address, u32 increment, u32 word_count);
template TickCount DMA::TransferMemoryToDevice<DMA::Channel::CDROM>(u32 address, u32 increment, u32 word_count);
template bool DMA::TransferChannel<DMA::Channel::CDROM>();
template TickCount DMA::TransferDeviceToMemory<DMA::Channel::SPU>(u32 address, u32 increment, u32 word_count);
template TickCount DMA::TransferMemoryToDevice<DMA::Channel::SPU>(u32 address, u32 increment, u32 word_count);
template bool DMA::TransferChannel<DMA::Channel::SPU>();
template TickCount DMA::TransferDeviceToMemory<DMA::Channel::PIO>(u32 address, u32 increment, u32 word_count);
template TickCount DMA::TransferMemoryToDevice<DMA::Channel::PIO>(u32 address, u32 increment, u32 word_count);
template bool DMA::TransferChannel<DMA::Channel::PIO>();
template TickCount DMA::TransferDeviceToMemory<DMA::Channel::OTC>(u32 address, u32 increment, u32 word_count);
template TickCount DMA::TransferMemoryToDevice<DMA::Channel::OTC>(u32 address, u32 increment, u32 word_count);
template bool DMA::TransferChannel<DMA::Channel::OTC>();
