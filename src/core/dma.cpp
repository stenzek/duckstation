#include "dma.h"
#include "bus.h"
#include "cdrom.h"
#include "common/log.h"
#include "common/state_wrapper.h"
#include "common/string_util.h"
#include "cpu_code_cache.h"
#include "cpu_core.h"
#include "gpu.h"
#include "interrupt_controller.h"
#include "mdec.h"
#include "spu.h"
#include "system.h"
Log_SetChannel(DMA);

DMA g_dma;

DMA::DMA() = default;

DMA::~DMA() = default;

void DMA::Initialize()
{
  m_max_slice_ticks = g_settings.dma_max_slice_ticks;
  m_halt_ticks = g_settings.dma_halt_ticks;

  m_transfer_buffer.resize(32);
  m_unhalt_event = TimingEvents::CreateTimingEvent("DMA Transfer Unhalt", 1, m_max_slice_ticks,
                                                   std::bind(&DMA::UnhaltTransfer, this, std::placeholders::_1), false);

  Reset();
}

void DMA::Shutdown()
{
  m_unhalt_event.reset();
}

void DMA::Reset()
{
  for (u32 i = 0; i < NUM_CHANNELS; i++)
  {
    ChannelState& cs = m_state[i];
    cs.base_address = 0;
    cs.block_control.bits = 0;
    cs.channel_control.bits = 0;
    cs.request = false;
  }

  m_DPCR.bits = 0x07654321;
  m_DICR.bits = 0;

  m_halt_ticks_remaining = 0;
  m_unhalt_event->Deactivate();
}

bool DMA::DoState(StateWrapper& sw)
{
  sw.Do(&m_halt_ticks_remaining);

  for (u32 i = 0; i < NUM_CHANNELS; i++)
  {
    ChannelState& cs = m_state[i];
    sw.Do(&cs.base_address);
    sw.Do(&cs.block_control.bits);
    sw.Do(&cs.channel_control.bits);
    sw.Do(&cs.request);
  }

  sw.Do(&m_DPCR.bits);
  sw.Do(&m_DICR.bits);

  if (sw.IsReading())
  {
    if (m_halt_ticks_remaining > 0)
      m_unhalt_event->SetIntervalAndSchedule(m_halt_ticks_remaining);
    else
      m_unhalt_event->Deactivate();
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
        Log_TracePrintf("DMA%u base address -> 0x%08X", channel_index, m_state[channel_index].base_address);
        return m_state[channel_index].base_address;
      }
      case 0x04:
      {
        Log_TracePrintf("DMA%u block control -> 0x%08X", channel_index, m_state[channel_index].block_control.bits);
        return m_state[channel_index].block_control.bits;
      }
      case 0x08:
      {
        Log_TracePrintf("DMA%u channel control -> 0x%08X", channel_index, m_state[channel_index].channel_control.bits);
        return m_state[channel_index].channel_control.bits;
      }
      default:
        break;
    }
  }
  else
  {
    if (offset == 0x70)
    {
      Log_TracePrintf("DPCR -> 0x%08X", m_DPCR.bits);
      return m_DPCR.bits;
    }
    else if (offset == 0x74)
    {
      Log_TracePrintf("DPCR -> 0x%08X", m_DPCR.bits);
      return m_DICR.bits;
    }
  }

  Log_ErrorPrintf("Unhandled register read: %02X", offset);
  return UINT32_C(0xFFFFFFFF);
}

void DMA::WriteRegister(u32 offset, u32 value)
{
  const u32 channel_index = offset >> 4;
  if (channel_index < 7)
  {
    ChannelState& state = m_state[channel_index];
    switch (offset & UINT32_C(0x0F))
    {
      case 0x00:
      {
        state.base_address = value & BASE_ADDRESS_MASK;
        Log_TracePrintf("DMA channel %u base address <- 0x%08X", channel_index, state.base_address);
        return;
      }
      case 0x04:
      {
        Log_TracePrintf("DMA channel %u block control <- 0x%08X", channel_index, value);
        state.block_control.bits = value;
        return;
      }

      case 0x08:
      {
        state.channel_control.bits = (state.channel_control.bits & ~ChannelState::ChannelControl::WRITE_MASK) |
                                     (value & ChannelState::ChannelControl::WRITE_MASK);
        Log_TracePrintf("DMA channel %u channel control <- 0x%08X", channel_index, state.channel_control.bits);

        // start/trigger bit must be enabled for OTC
        if (static_cast<Channel>(channel_index) == Channel::OTC)
          SetRequest(static_cast<Channel>(channel_index), state.channel_control.start_trigger);

        if (CanTransferChannel(static_cast<Channel>(channel_index)))
          TransferChannel(static_cast<Channel>(channel_index));
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
        Log_TracePrintf("DPCR <- 0x%08X", value);
        m_DPCR.bits = value;

        for (u32 i = 0; i < NUM_CHANNELS; i++)
        {
          if (CanTransferChannel(static_cast<Channel>(i)))
          {
            if (!TransferChannel(static_cast<Channel>(i)))
              break;
          }
        }

        return;
      }

      case 0x74:
      {
        Log_TracePrintf("DCIR <- 0x%08X", value);
        m_DICR.bits = (m_DICR.bits & ~DICR_WRITE_MASK) | (value & DICR_WRITE_MASK);
        m_DICR.bits = m_DICR.bits & ~(value & DICR_RESET_MASK);
        m_DICR.UpdateMasterFlag();
        return;
      }

      default:
        break;
    }
  }

  Log_ErrorPrintf("Unhandled register write: %02X <- %08X", offset, value);
}

void DMA::SetRequest(Channel channel, bool request)
{
  ChannelState& cs = m_state[static_cast<u32>(channel)];
  if (cs.request == request)
    return;

  cs.request = request;
  if (CanTransferChannel(channel))
    TransferChannel(channel);
}

bool DMA::CanTransferChannel(Channel channel) const
{
  if (!m_DPCR.GetMasterEnable(channel))
    return false;

  const ChannelState& cs = m_state[static_cast<u32>(channel)];
  if (!cs.channel_control.enable_busy)
    return false;

  if (cs.channel_control.sync_mode != SyncMode::Manual && IsTransferHalted())
    return false;

  return cs.request;
}

bool DMA::IsTransferHalted() const
{
  return m_unhalt_event->IsActive();
}

void DMA::UpdateIRQ()
{
  m_DICR.UpdateMasterFlag();
  if (m_DICR.master_flag)
  {
    Log_TracePrintf("Firing DMA master interrupt");
    g_interrupt_controller.InterruptRequest(InterruptController::IRQ::DMA);
  }
}

bool DMA::TransferChannel(Channel channel)
{
  ChannelState& cs = m_state[static_cast<u32>(channel)];

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
      Log_DebugPrintf("DMA%u: Copying %u words %s 0x%08X", static_cast<u32>(channel), word_count,
                      copy_to_device ? "from" : "to", current_address & ADDRESS_MASK);

      TickCount used_ticks;
      if (copy_to_device)
        used_ticks = TransferMemoryToDevice(channel, current_address & ADDRESS_MASK, increment, word_count);
      else
        used_ticks = TransferDeviceToMemory(channel, current_address & ADDRESS_MASK, increment, word_count);

      CPU::AddPendingTicks(used_ticks);
    }
    break;

    case SyncMode::LinkedList:
    {
      TickCount used_ticks = 0;
      if (!copy_to_device)
      {
        Panic("Linked list not implemented for DMA reads");
        return true;
      }

      Log_DebugPrintf("DMA%u: Copying linked list starting at 0x%08X to device", static_cast<u32>(channel),
                      current_address & ADDRESS_MASK);

      u8* ram_pointer = Bus::g_ram;
      bool halt_transfer = false;
      while (cs.request)
      {
        u32 header;
        std::memcpy(&header, &ram_pointer[current_address & ADDRESS_MASK], sizeof(header));
        used_ticks++;

        const u32 word_count = header >> 24;
        const u32 next_address = header & UINT32_C(0x00FFFFFF);
        Log_TracePrintf(" .. linked list entry at 0x%08X size=%u(%u words) next=0x%08X", current_address & ADDRESS_MASK,
                        word_count * UINT32_C(4), word_count, next_address);
        if (word_count > 0)
        {
          used_ticks +=
            TransferMemoryToDevice(channel, (current_address + sizeof(header)) & ADDRESS_MASK, 4, word_count);
        }
        else if ((current_address & ADDRESS_MASK) == (next_address & ADDRESS_MASK))
        {
          current_address = next_address;
          halt_transfer = true;
          break;
        }

        current_address = next_address;
        if (current_address & UINT32_C(0x800000))
          break;

        if (used_ticks >= m_max_slice_ticks)
        {
          halt_transfer = true;
          break;
        }
      }

      cs.base_address = current_address;
      CPU::AddPendingTicks(used_ticks);

      if (current_address & UINT32_C(0x800000))
        break;

      if (halt_transfer)
      {
        // stall the transfer for a bit if we ran for too long
        HaltTransfer(m_halt_ticks);
        return false;
      }
      else
      {
        // linked list not yet complete
        return true;
      }
    }
    break;

    case SyncMode::Request:
    {
      Log_DebugPrintf("DMA%u: Copying %u blocks of size %u (%u total words) %s 0x%08X", static_cast<u32>(channel),
                      cs.block_control.request.GetBlockCount(), cs.block_control.request.GetBlockSize(),
                      cs.block_control.request.GetBlockCount() * cs.block_control.request.GetBlockSize(),
                      copy_to_device ? "from" : "to", current_address & ADDRESS_MASK);

      const u32 block_size = cs.block_control.request.GetBlockSize();
      u32 blocks_remaining = cs.block_control.request.GetBlockCount();
      TickCount used_ticks = 0;

      if (copy_to_device)
      {
        do
        {
          blocks_remaining--;
          used_ticks += TransferMemoryToDevice(channel, current_address & ADDRESS_MASK, increment, block_size);
          current_address = (current_address + (increment * block_size));
        } while (cs.request && blocks_remaining > 0);
      }
      else
      {
        do
        {
          blocks_remaining--;
          used_ticks += TransferDeviceToMemory(channel, current_address & ADDRESS_MASK, increment, block_size);
          current_address = (current_address + (increment * block_size));
        } while (cs.request && blocks_remaining > 0);
      }

      cs.base_address = current_address & BASE_ADDRESS_MASK;
      cs.block_control.request.block_count = blocks_remaining;
      CPU::AddPendingTicks(used_ticks);

      // finish transfer later if the request was cleared
      if (blocks_remaining > 0)
        return true;
    }
    break;

    default:
      Panic("Unimplemented sync mode");
      break;
  }

  // start/busy bit is cleared on end of transfer
  cs.channel_control.enable_busy = false;
  if (m_DICR.IsIRQEnabled(channel))
  {
    Log_DebugPrintf("Set DMA interrupt for channel %u", static_cast<u32>(channel));
    m_DICR.SetIRQFlag(channel);
    UpdateIRQ();
  }

  return true;
}

void DMA::HaltTransfer(TickCount duration)
{
  m_halt_ticks_remaining += duration;
  Log_DebugPrintf("Halting DMA for %d ticks", m_halt_ticks_remaining);

  DebugAssert(!m_unhalt_event->IsActive());
  m_unhalt_event->SetIntervalAndSchedule(m_halt_ticks_remaining);
}

void DMA::UnhaltTransfer(TickCount ticks)
{
  Log_DebugPrintf("Resuming DMA after %d ticks, %d ticks late", ticks, -(m_halt_ticks_remaining - ticks));
  m_halt_ticks_remaining -= ticks;
  m_unhalt_event->Deactivate();

  // TODO: Use channel priority. But doing it in ascending order is probably good enough.
  // Main thing is that OTC happens after GPU, because otherwise it'll wipe out the LL.
  for (u32 i = 0; i < NUM_CHANNELS; i++)
  {
    if (CanTransferChannel(static_cast<Channel>(i)))
    {
      if (!TransferChannel(static_cast<Channel>(i)))
        return;
    }
  }

  // We didn't run too long, so reset timer.
  m_halt_ticks_remaining = 0;
}

TickCount DMA::TransferMemoryToDevice(Channel channel, u32 address, u32 increment, u32 word_count)
{
  const u32* src_pointer = reinterpret_cast<u32*>(Bus::g_ram + address);
  if (channel != Channel::GPU &&
      (static_cast<s32>(increment) < 0 || ((address + (increment * word_count)) & ADDRESS_MASK) <= address))
  {
    // Use temp buffer if it's wrapping around
    if (m_transfer_buffer.size() < word_count)
      m_transfer_buffer.resize(word_count);
    src_pointer = m_transfer_buffer.data();

    u8* ram_pointer = Bus::g_ram;
    for (u32 i = 0; i < word_count; i++)
    {
      std::memcpy(&m_transfer_buffer[i], &ram_pointer[address], sizeof(u32));
      address = (address + increment) & ADDRESS_MASK;
    }
  }

  switch (channel)
  {
    case Channel::GPU:
    {
      if (g_gpu->BeginDMAWrite())
      {
        u8* ram_pointer = Bus::g_ram;
        for (u32 i = 0; i < word_count; i++)
        {
          u32 value;
          std::memcpy(&value, &ram_pointer[address], sizeof(u32));
          g_gpu->DMAWrite(address, value);
          address = (address + increment) & ADDRESS_MASK;
        }
        g_gpu->EndDMAWrite();
      }
    }
    break;

    case Channel::SPU:
      g_spu.DMAWrite(src_pointer, word_count);
      break;

    case Channel::MDECin:
      g_mdec.DMAWrite(src_pointer, word_count);
      break;

    case Channel::CDROM:
    case Channel::MDECout:
    case Channel::PIO:
    default:
      Panic("Unhandled DMA channel for device write");
      break;
  }

  return Bus::GetDMARAMTickCount(word_count);
}

TickCount DMA::TransferDeviceToMemory(Channel channel, u32 address, u32 increment, u32 word_count)
{
  if (channel == Channel::OTC)
  {
    // clear ordering table
    u8* ram_pointer = Bus::g_ram;
    const u32 word_count_less_1 = word_count - 1;
    for (u32 i = 0; i < word_count_less_1; i++)
    {
      u32 value = ((address - 4) & ADDRESS_MASK);
      std::memcpy(&ram_pointer[address], &value, sizeof(value));
      address = (address - 4) & ADDRESS_MASK;
    }

    const u32 terminator = UINT32_C(0xFFFFFF);
    std::memcpy(&ram_pointer[address], &terminator, sizeof(terminator));
    CPU::CodeCache::InvalidateCodePages(address, word_count);
    return Bus::GetDMARAMTickCount(word_count);
  }

  u32* dest_pointer = reinterpret_cast<u32*>(&Bus::g_ram[address]);
  if (static_cast<s32>(increment) < 0 || ((address + (increment * word_count)) & ADDRESS_MASK) <= address)
  {
    // Use temp buffer if it's wrapping around
    if (m_transfer_buffer.size() < word_count)
      m_transfer_buffer.resize(word_count);
    dest_pointer = m_transfer_buffer.data();
  }

  // Read from device.
  switch (channel)
  {
    case Channel::GPU:
      g_gpu->DMARead(dest_pointer, word_count);
      break;

    case Channel::CDROM:
      g_cdrom.DMARead(dest_pointer, word_count);
      break;

    case Channel::SPU:
      g_spu.DMARead(dest_pointer, word_count);
      break;

    case Channel::MDECout:
      g_mdec.DMARead(dest_pointer, word_count);
      break;

    default:
      Panic("Unhandled DMA channel for device read");
      std::fill_n(dest_pointer, word_count, UINT32_C(0xFFFFFFFF));
      break;
  }

  if (dest_pointer == m_transfer_buffer.data())
  {
    u8* ram_pointer = Bus::g_ram;
    for (u32 i = 0; i < word_count; i++)
    {
      std::memcpy(&ram_pointer[address], &m_transfer_buffer[i], sizeof(u32));
      address = (address + increment) & ADDRESS_MASK;
    }
  }

  CPU::CodeCache::InvalidateCodePages(address, word_count);
  return Bus::GetDMARAMTickCount(word_count);
}
