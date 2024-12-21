// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "dma.h"
#include "bus.h"
#include "cdrom.h"
#include "cpu_core.h"
#include "gpu.h"
#include "gpu_dump.h"
#include "imgui.h"
#include "interrupt_controller.h"
#include "mdec.h"
#include "pad.h"
#include "spu.h"
#include "system.h"
#include "timing_event.h"

#include "util/imgui_manager.h"
#include "util/state_wrapper.h"

#include "common/bitfield.h"
#include "common/log.h"
#include "common/string_util.h"

#include "fmt/format.h"

#include <array>
#include <memory>
#include <vector>

LOG_CHANNEL(DMA);

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

union DPCRRegister
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
union DICRRegister
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

static TickCount GetMaxSliceTicks(TickCount max_slice_size);

// configuration
namespace {
struct DMAState
{
  std::vector<u32> transfer_buffer;
  TimingEvent unhalt_event{"DMA Transfer Unhalt", 1, 1, &DMA::UnhaltTransfer, nullptr};
  TickCount halt_ticks_remaining = 0;

  std::array<ChannelState, NUM_CHANNELS> channels;
  DPCRRegister DPCR = {};
  DICRRegister DICR = {};
};
} // namespace

ALIGN_TO_CACHE_LINE static DMAState s_state;

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
  s_state.unhalt_event.SetInterval(g_settings.dma_halt_ticks);
  Reset();
}

void DMA::Shutdown()
{
  ClearState();
  s_state.unhalt_event.Deactivate();
}

void DMA::Reset()
{
  ClearState();
  s_state.unhalt_event.Deactivate();
}

void DMA::ClearState()
{
  for (u32 i = 0; i < NUM_CHANNELS; i++)
  {
    ChannelState& cs = s_state.channels[i];
    cs.base_address = 0;
    cs.block_control.bits = 0;
    cs.channel_control.bits = 0;
    cs.request = false;
  }

  s_state.DPCR.bits = 0x07654321;
  s_state.DICR.bits = 0;

  s_state.halt_ticks_remaining = 0;
}

bool DMA::DoState(StateWrapper& sw)
{
  sw.Do(&s_state.halt_ticks_remaining);

  for (u32 i = 0; i < NUM_CHANNELS; i++)
  {
    ChannelState& cs = s_state.channels[i];
    sw.Do(&cs.base_address);
    sw.Do(&cs.block_control.bits);
    sw.Do(&cs.channel_control.bits);
    sw.Do(&cs.request);
  }

  sw.Do(&s_state.DPCR.bits);
  sw.Do(&s_state.DICR.bits);

  if (sw.IsReading())
  {
    if (s_state.halt_ticks_remaining > 0)
      s_state.unhalt_event.SetIntervalAndSchedule(s_state.halt_ticks_remaining);
    else
      s_state.unhalt_event.Deactivate();
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
        TRACE_LOG("DMA[{}] base address -> 0x{:08X}", static_cast<Channel>(channel_index),
                  s_state.channels[channel_index].base_address);
        return s_state.channels[channel_index].base_address;
      }
      case 0x04:
      {
        TRACE_LOG("DMA[{}] block control -> 0x{:08X}", static_cast<Channel>(channel_index),
                  s_state.channels[channel_index].block_control.bits);
        return s_state.channels[channel_index].block_control.bits;
      }
      case 0x08:
      {
        TRACE_LOG("DMA[{}] channel control -> 0x{:08X}", static_cast<Channel>(channel_index),
                  s_state.channels[channel_index].channel_control.bits);
        return s_state.channels[channel_index].channel_control.bits;
      }
      default:
        break;
    }
  }
  else
  {
    if (offset == 0x70)
    {
      TRACE_LOG("DPCR -> 0x{:08X}", s_state.DPCR.bits);
      return s_state.DPCR.bits;
    }
    else if (offset == 0x74)
    {
      TRACE_LOG("DICR -> 0x{:08X}", s_state.DICR.bits);
      return s_state.DICR.bits;
    }
  }

  ERROR_LOG("Unhandled register read: {:02X}", offset);
  return UINT32_C(0xFFFFFFFF);
}

void DMA::WriteRegister(u32 offset, u32 value)
{
  const u32 channel_index = offset >> 4;
  if (channel_index < 7)
  {
    ChannelState& state = s_state.channels[channel_index];
    switch (offset & UINT32_C(0x0F))
    {
      case 0x00:
      {
        state.base_address = value & BASE_ADDRESS_MASK;
        TRACE_LOG("DMA channel {} base address <- 0x{:08X}", static_cast<Channel>(channel_index), state.base_address);
        return;
      }
      case 0x04:
      {
        TRACE_LOG("DMA channel {} block control <- 0x{:08X}", static_cast<Channel>(channel_index), value);
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
        TRACE_LOG("DMA channel {} channel control <- 0x{:08X}", static_cast<Channel>(channel_index),
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
              DEV_LOG("Delaying {} transfer by {} cycles due to chopping", static_cast<Channel>(channel_index),
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
        TRACE_LOG("DPCR <- 0x{:08X}", value);
        s_state.DPCR.bits = value;

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
        TRACE_LOG("DICR <- 0x{:08X}", value);
        s_state.DICR.bits = (s_state.DICR.bits & ~DICR_WRITE_MASK) | (value & DICR_WRITE_MASK);
        s_state.DICR.bits = s_state.DICR.bits & ~(value & DICR_RESET_MASK);
        UpdateIRQ();
        return;
      }

      default:
        break;
    }
  }

  ERROR_LOG("Unhandled register write: {:02X} <- {:08X}", offset, value);
}

void DMA::SetRequest(Channel channel, bool request)
{
  ChannelState& cs = s_state.channels[static_cast<u32>(channel)];
  if (cs.request == request)
    return;

  cs.request = request;
  if (CanTransferChannel(channel, false))
    s_channel_transfer_functions[static_cast<u32>(channel)]();
}

ALWAYS_INLINE_RELEASE bool DMA::CanTransferChannel(Channel channel, bool ignore_halt)
{
  if (!s_state.DPCR.GetMasterEnable(channel))
    return false;

  const ChannelState& cs = s_state.channels[static_cast<u32>(channel)];
  if (!cs.channel_control.enable_busy)
    return false;

  if (cs.channel_control.sync_mode != SyncMode::Manual && (IsTransferHalted() && !ignore_halt))
    return false;

  return cs.request;
}

bool DMA::IsTransferHalted()
{
  return s_state.unhalt_event.IsActive();
}

void DMA::UpdateIRQ()
{
  [[maybe_unused]] const auto old_dicr = s_state.DICR;
  s_state.DICR.UpdateMasterFlag();
  if (!old_dicr.master_flag && s_state.DICR.master_flag)
    TRACE_LOG("Firing DMA master interrupt");
  InterruptController::SetLineState(InterruptController::IRQ::DMA, s_state.DICR.master_flag);
}

ALWAYS_INLINE_RELEASE bool DMA::IsLinkedListTerminator(PhysicalMemoryAddress address)
{
  return ((address & LINKED_LIST_TERMINATOR) == LINKED_LIST_TERMINATOR);
}

ALWAYS_INLINE_RELEASE bool DMA::CheckForBusError(Channel channel, ChannelState& cs, PhysicalMemoryAddress address,
                                                 u32 size)
{
  // Relying on a transfer partially happening at the end of RAM, then hitting a bus error would be pretty silly.
  if ((address + size) >= Bus::g_ram_mapped_size) [[unlikely]]
  {
    DEBUG_LOG("DMA bus error on channel {} at address 0x{:08X} size {}", channel, address, size);
    cs.channel_control.enable_busy = false;
    s_state.DICR.bus_error = true;
    s_state.DICR.SetIRQFlag(channel);
    UpdateIRQ();
    return true;
  }

  return false;
}

ALWAYS_INLINE_RELEASE void DMA::CompleteTransfer(Channel channel, ChannelState& cs)
{
  // start/busy bit is cleared on end of transfer
  DEBUG_LOG("DMA transfer for channel {} complete", channel);
  cs.channel_control.enable_busy = false;
  if (s_state.DICR.ShouldSetIRQFlag(channel))
  {
    DEBUG_LOG("Setting DMA interrupt for channel {}", channel);
    s_state.DICR.SetIRQFlag(channel);
    UpdateIRQ();
  }
}

TickCount DMA::GetMaxSliceTicks(TickCount max_slice_size)
{
  const TickCount max = Pad::IsTransmitting() ? SLICE_SIZE_WHEN_TRANSMITTING_PAD : max_slice_size;
  if (!TimingEvents::IsRunningEvents())
    return max;

  const TickCount remaining_in_event_loop =
    static_cast<TickCount>(TimingEvents::GetEventRunTickCounter() - TimingEvents::GetGlobalTickCounter());
  return std::max<TickCount>(max - remaining_in_event_loop, 1);
}

template<DMA::Channel channel>
bool DMA::TransferChannel()
{
  ChannelState& cs = s_state.channels[static_cast<u32>(channel)];

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
      DEBUG_LOG("DMA[{}]: Copying {} words {} 0x{:08X}", channel, word_count, copy_to_device ? "from" : "to",
                current_address);

      const PhysicalMemoryAddress transfer_addr = current_address & TRANSFER_ADDRESS_MASK;
      if (CheckForBusError(channel, cs, transfer_addr, (word_count - 1) * increment)) [[unlikely]]
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

      DEBUG_LOG("DMA[{}]: Copying linked list starting at 0x{:08X} to device", channel, current_address);

      // Prove to the compiler that nothing's going to modify these.
      const u8* const ram_ptr = Bus::g_ram;
      const u32 mask = Bus::g_ram_mask;

      const TickCount slice_ticks = GetMaxSliceTicks(g_settings.dma_max_slice_ticks);
      TickCount remaining_ticks = slice_ticks;
      while (cs.request && remaining_ticks > 0)
      {
        u32 header;
        PhysicalMemoryAddress transfer_addr = current_address & TRANSFER_ADDRESS_MASK;
        if (CheckForBusError(channel, cs, transfer_addr, sizeof(header))) [[unlikely]]
        {
          cs.base_address = current_address;
          return true;
        }

        std::memcpy(&header, &ram_ptr[transfer_addr & mask], sizeof(header));
        const u32 word_count = header >> 24;
        const u32 next_address = header & 0x00FFFFFFu;
        TRACE_LOG(" .. linked list entry at 0x{:08X} size={}({} words) next=0x{:08X}", current_address, word_count * 4,
                  word_count, next_address);

        const TickCount setup_ticks = (word_count > 0) ?
                                        (LINKED_LIST_HEADER_READ_TICKS + LINKED_LIST_BLOCK_SETUP_TICKS) :
                                        LINKED_LIST_HEADER_READ_TICKS;
        CPU::AddPendingTicks(setup_ticks);
        remaining_ticks -= setup_ticks;

        if (word_count > 0)
        {
          if (CheckForBusError(channel, cs, transfer_addr, (word_count - 1) * increment)) [[unlikely]]
          {
            cs.base_address = current_address;
            return true;
          }

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
        HaltTransfer(g_settings.dma_halt_ticks);
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
      DEBUG_LOG("DMA[{}]: Copying {} blocks of size {} ({} total words) {} 0x{:08X}", channel,
                cs.block_control.request.GetBlockCount(), cs.block_control.request.GetBlockSize(),
                cs.block_control.request.GetBlockCount() * cs.block_control.request.GetBlockSize(),
                copy_to_device ? "from" : "to", current_address);

      const u32 block_size = cs.block_control.request.GetBlockSize();
      u32 blocks_remaining = cs.block_control.request.GetBlockCount();
      TickCount ticks_remaining = GetMaxSliceTicks(g_settings.dma_max_slice_ticks);

      if (copy_to_device)
      {
        do
        {
          const PhysicalMemoryAddress transfer_addr = current_address & TRANSFER_ADDRESS_MASK;
          if (CheckForBusError(channel, cs, transfer_addr, (block_size - 1) * increment)) [[unlikely]]
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
          if (CheckForBusError(channel, cs, transfer_addr, (block_size - 1) * increment)) [[unlikely]]
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
          if (!s_state.unhalt_event.IsActive())
            HaltTransfer(g_settings.dma_halt_ticks);

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
  s_state.halt_ticks_remaining += duration;
  DEBUG_LOG("Halting DMA for {} ticks", s_state.halt_ticks_remaining);
  if (s_state.unhalt_event.IsActive())
    return;

  DebugAssert(!s_state.unhalt_event.IsActive());
  s_state.unhalt_event.SetIntervalAndSchedule(s_state.halt_ticks_remaining);
}

void DMA::UnhaltTransfer(void*, TickCount ticks, TickCount ticks_late)
{
  DEBUG_LOG("Resuming DMA after {} ticks, {} ticks late", ticks, -(s_state.halt_ticks_remaining - ticks));
  s_state.halt_ticks_remaining -= ticks;
  s_state.unhalt_event.Deactivate();

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
  s_state.halt_ticks_remaining = 0;
}

template<DMA::Channel channel>
TickCount DMA::TransferMemoryToDevice(u32 address, u32 increment, u32 word_count)
{
  const u32 mask = Bus::g_ram_mask;
#if defined(_DEBUG) || defined(_DEVEL)
  if ((address & mask) != address)
    DEBUG_LOG("DMA TO {} from masked RAM address 0x{:08X} => 0x{:08X}", channel, address, (address & mask));
#endif

  address &= mask;

  const u32* src_pointer = reinterpret_cast<u32*>(Bus::g_ram + address);
  if constexpr (channel != Channel::GPU)
  {
    if (static_cast<s32>(increment) < 0 || ((address + (increment * word_count)) & mask) <= address) [[unlikely]]
    {
      // Use temp buffer if it's wrapping around
      if (s_state.transfer_buffer.size() < word_count)
        s_state.transfer_buffer.resize(word_count);
      src_pointer = s_state.transfer_buffer.data();

      u8* ram_pointer = Bus::g_ram;
      for (u32 i = 0; i < word_count; i++)
      {
        std::memcpy(&s_state.transfer_buffer[i], &ram_pointer[address], sizeof(u32));
        address = (address + increment) & mask;
      }
    }
  }

  switch (channel)
  {
    case Channel::GPU:
    {
      if (g_gpu.BeginDMAWrite()) [[likely]]
      {
        if (GPUDump::Recorder* dump = g_gpu.GetGPUDump()) [[unlikely]]
        {
          // No wraparound?
          dump->BeginGP0Packet(word_count);
          if (((address + (increment * (word_count - 1))) & mask) >= address) [[likely]]
          {
            dump->WriteWords(reinterpret_cast<const u32*>(&Bus::g_ram[address]), word_count);
          }
          else
          {
            u32 dump_address = address;
            for (u32 i = 0; i < word_count; i++)
            {
              u32 value;
              std::memcpy(&value, &Bus::g_ram[dump_address], sizeof(u32));
              dump->WriteWord(value);
              dump_address = (dump_address + increment) & mask;
            }
          }
          dump->EndGP0Packet();
        }

        u8* ram_pointer = Bus::g_ram;
        for (u32 i = 0; i < word_count; i++)
        {
          u32 value;
          std::memcpy(&value, &ram_pointer[address], sizeof(u32));
          g_gpu.DMAWrite(address, value);
          address = (address + increment) & mask;
        }
        g_gpu.EndDMAWrite();
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
      ERROR_LOG("Unhandled DMA channel {} for device write", static_cast<u32>(channel));
      break;
  }

  return Bus::GetDMARAMTickCount(word_count);
}

template<DMA::Channel channel>
TickCount DMA::TransferDeviceToMemory(u32 address, u32 increment, u32 word_count)
{
  const u32 mask = Bus::g_ram_mask;
#if defined(_DEBUG) || defined(_DEVEL)
  if ((address & mask) != address)
    DEBUG_LOG("DMA FROM {} to masked RAM address 0x{:08X} => 0x{:08X}", channel, address, (address & mask));
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
    if (s_state.transfer_buffer.size() < word_count)
      s_state.transfer_buffer.resize(word_count);
    dest_pointer = s_state.transfer_buffer.data();
  }

  // Read from device.
  switch (channel)
  {
    case Channel::GPU:
      g_gpu.DMARead(dest_pointer, word_count);
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
      ERROR_LOG("Unhandled DMA channel {} for device read", static_cast<u32>(channel));
      std::fill_n(dest_pointer, word_count, UINT32_C(0xFFFFFFFF));
      break;
  }

  if (dest_pointer == s_state.transfer_buffer.data()) [[unlikely]]
  {
    u8* ram_pointer = Bus::g_ram;
    for (u32 i = 0; i < word_count; i++)
    {
      std::memcpy(&ram_pointer[address], &s_state.transfer_buffer[i], sizeof(u32));
      address = (address + increment) & mask;
    }
  }

  return Bus::GetDMARAMTickCount(word_count);
}

void DMA::DrawDebugStateWindow(float scale)
{
  static constexpr std::array column_names = {"#",    "Req",    "Addr",     "Direction", "Chopping", "Mode",
                                              "Busy", "Enable", "Priority", "IRQ",       "Flag"};
  static constexpr std::array sync_mode_names = {"Manual", "Request", "LinkedList", "Reserved"};

  ImGui::Columns(static_cast<int>(column_names.size()));
  ImGui::SetColumnWidth(0, 80.0f * scale);
  ImGui::SetColumnWidth(1, 50.0f * scale);
  ImGui::SetColumnWidth(2, 80.0f * scale);
  ImGui::SetColumnWidth(3, 110.0f * scale);
  ImGui::SetColumnWidth(4, 100.0f * scale);
  ImGui::SetColumnWidth(5, 80.0f * scale);
  ImGui::SetColumnWidth(6, 80.0f * scale);
  ImGui::SetColumnWidth(7, 80.0f * scale);
  ImGui::SetColumnWidth(8, 60.0f * scale);
  ImGui::SetColumnWidth(9, 80.0f * scale);
  ImGui::SetColumnWidth(10, 80.0f * scale);

  for (const char* title : column_names)
  {
    ImGui::TextUnformatted(title);
    ImGui::NextColumn();
  }

  const ImVec4 active(1.0f, 1.0f, 1.0f, 1.0f);
  const ImVec4 inactive(0.5f, 0.5f, 0.5f, 1.0f);

  for (u32 i = 0; i < NUM_CHANNELS; i++)
  {
    const ChannelState& cs = s_state.channels[i];

    ImGui::TextColored(cs.channel_control.enable_busy ? active : inactive, "%u[%s]", i, s_channel_names[i]);
    ImGui::NextColumn();
    ImGui::TextColored(cs.request ? active : inactive, cs.request ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::TextColored(cs.request ? active : inactive, "%08X", cs.base_address);
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
    ImGui::TextColored(s_state.DPCR.GetMasterEnable(static_cast<Channel>(i)) ? active : inactive,
                       s_state.DPCR.GetMasterEnable(static_cast<Channel>(i)) ? "Enabled" : "Disabled");
    ImGui::NextColumn();
    ImGui::TextColored(s_state.DPCR.GetMasterEnable(static_cast<Channel>(i)) ? active : inactive, "%u",
                       s_state.DPCR.GetPriority(static_cast<Channel>(i)));
    ImGui::NextColumn();
    ImGui::TextColored(s_state.DICR.GetIRQEnabled(static_cast<Channel>(i)) ? active : inactive,
                       s_state.DICR.GetIRQEnabled(static_cast<Channel>(i)) ? "Enabled" : "Disabled");
    ImGui::NextColumn();
    ImGui::TextColored(s_state.DICR.GetIRQFlag(static_cast<Channel>(i)) ? active : inactive,
                       s_state.DICR.GetIRQFlag(static_cast<Channel>(i)) ? "IRQ" : "");
    ImGui::NextColumn();
  }

  ImGui::Columns(1);
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
