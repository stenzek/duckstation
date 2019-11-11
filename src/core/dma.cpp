#include "dma.h"
#include "YBaseLib/Log.h"
#include "bus.h"
#include "cdrom.h"
#include "common/state_wrapper.h"
#include "gpu.h"
#include "interrupt_controller.h"
#include "mdec.h"
#include "spu.h"
#include "system.h"
Log_SetChannel(DMA);

DMA::DMA() = default;

DMA::~DMA() = default;

void DMA::Initialize(System* system, Bus* bus, InterruptController* interrupt_controller, GPU* gpu, CDROM* cdrom,
                     SPU* spu, MDEC* mdec)
{
  m_system = system;
  m_bus = bus;
  m_interrupt_controller = interrupt_controller;
  m_gpu = gpu;
  m_cdrom = cdrom;
  m_spu = spu;
  m_mdec = mdec;
  m_transfer_buffer.resize(32);
}

void DMA::Reset()
{
  m_transfer_in_progress = false;
  std::memset(&m_state, 0, sizeof(m_state));
  m_DPCR.bits = 0x07654321;
  m_DICR.bits = 0;
}

bool DMA::DoState(StateWrapper& sw)
{
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
        state.base_address = value & ADDRESS_MASK;
        Log_TracePrintf("DMA channel %u base address <- 0x%08X", channel_index, state.base_address);
        return;
      }
      case 0x04:
      {
        Log_TracePrintf("DMA channel %u block control <- 0x%08X", channel_index, value);
        state.block_control.bits = value;
        Transfer();
        return;
      }

      case 0x08:
      {
        state.channel_control.bits = (state.channel_control.bits & ~ChannelState::ChannelControl::WRITE_MASK) |
                                     (value & ChannelState::ChannelControl::WRITE_MASK);
        Log_TracePrintf("DMA channel %u channel control <- 0x%08X", channel_index, state.channel_control.bits);
        Transfer();
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
        Transfer();
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
  if (request)
    Transfer();
}

bool DMA::CanTransferChannel(Channel channel) const
{
  if (!m_DPCR.GetMasterEnable(channel))
    return false;

  const ChannelState& cs = m_state[static_cast<u32>(channel)];
  if (!cs.channel_control.enable_busy)
    return false;

  if (!cs.request && channel != Channel::OTC)
    return false;

  if (cs.channel_control.sync_mode == SyncMode::Manual && !cs.channel_control.start_trigger)
    return false;

  return true;
}

bool DMA::CanRunAnyChannels() const
{
  for (u32 i = 0; i < NUM_CHANNELS; i++)
  {
    if (CanTransferChannel(static_cast<Channel>(i)))
      return true;
  }

  return false;
}

void DMA::UpdateIRQ()
{
  m_DICR.UpdateMasterFlag();
  if (m_DICR.master_flag)
  {
    Log_TracePrintf("Firing DMA master interrupt");
    m_interrupt_controller->InterruptRequest(InterruptController::IRQ::DMA);
  }
}

void DMA::Transfer()
{
  if (m_transfer_in_progress)
    return;

  // prevent recursive calls
  m_transfer_in_progress = true;

  // keep going until all transfers are done. one channel can start others (e.g. MDEC)
  for (;;)
  {
    bool any_channels_active = false;

    for (u32 i = 0; i < NUM_CHANNELS; i++)
    {
      const Channel channel = static_cast<Channel>(i);
      if (CanTransferChannel(channel))
      {
        TransferChannel(channel);
        any_channels_active = true;
      }
    }

    if (!any_channels_active)
      break;
  }

  m_transfer_in_progress = false;
}

void DMA::TransferChannel(Channel channel)
{
  ChannelState& cs = m_state[static_cast<u32>(channel)];
  const bool copy_to_device = cs.channel_control.copy_to_device;

  // start/trigger bit is cleared on beginning of transfer
  cs.channel_control.start_trigger = false;

  PhysicalMemoryAddress current_address = (cs.base_address & ~UINT32_C(3)) & ADDRESS_MASK;
  const PhysicalMemoryAddress increment = cs.channel_control.address_step_reverse ? static_cast<u32>(-4) : UINT32_C(4);
  switch (cs.channel_control.sync_mode)
  {
    case SyncMode::Manual:
    {
      const u32 word_count = cs.block_control.manual.GetWordCount();
      Log_DebugPrintf("DMA%u: Copying %u words %s 0x%08X", static_cast<u32>(channel), word_count,
                      copy_to_device ? "from" : "to", current_address);
      if (copy_to_device)
        TransferMemoryToDevice(channel, current_address, increment, word_count);
      else
        TransferDeviceToMemory(channel, current_address, increment, word_count);
    }
    break;

    case SyncMode::LinkedList:
    {
      if (!copy_to_device)
      {
        Panic("Linked list not implemented for DMA reads");
      }
      else
      {
        Log_DebugPrintf("DMA%u: Copying linked list starting at 0x%08X to device", static_cast<u32>(channel),
                        current_address);

        for (;;)
        {
          u32 header;
          m_bus->DispatchAccess<MemoryAccessType::Read, MemoryAccessSize::Word>(current_address, header);

          const u32 word_count = header >> 24;
          const u32 next_address = header & UINT32_C(0xFFFFFF);
          Log_TracePrintf(" .. linked list entry at 0x%08X size=%u(%u words) next=0x%08X", current_address,
                          word_count * UINT32_C(4), word_count, next_address);
          current_address += sizeof(header);

          if (word_count > 0)
            TransferMemoryToDevice(channel, current_address, 4, word_count);

          if (next_address & UINT32_C(0x800000))
            break;

          current_address = next_address & ADDRESS_MASK;
        }
      }
    }
    break;

    case SyncMode::Request:
    {
      Log_DebugPrintf("DMA%u: Copying %u blocks of size %u (%u total words) %s 0x%08X", static_cast<u32>(channel),
                      cs.block_control.request.GetBlockCount(), cs.block_control.request.GetBlockSize(),
                      cs.block_control.request.GetBlockCount() * cs.block_control.request.GetBlockSize(),
                      copy_to_device ? "from" : "to", current_address);

      const u32 block_size = cs.block_control.request.GetBlockSize();
      u32 blocks_remaining = cs.block_control.request.block_count;

      if (copy_to_device)
      {
        do
        {
          blocks_remaining--;
          TransferMemoryToDevice(channel, current_address, increment, block_size);
          current_address = (current_address + (increment * block_size)) & ADDRESS_MASK;
        } while (cs.request && blocks_remaining > 0);
      }
      else
      {
        do
        {
          blocks_remaining--;
          TransferDeviceToMemory(channel, current_address, increment, block_size);
          current_address = (current_address + (increment * block_size)) & ADDRESS_MASK;
        } while (cs.request && blocks_remaining > 0);
      }

      cs.base_address = current_address;
      cs.block_control.request.block_count = blocks_remaining;

      // finish transfer later if the request was cleared
      if (blocks_remaining > 0)
        return;
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
}

void DMA::TransferMemoryToDevice(Channel channel, u32 address, u32 increment, u32 word_count)
{
  // Read from memory. Wrap-around?
  if (m_transfer_buffer.size() < word_count)
    m_transfer_buffer.resize(word_count);

  if (increment > 0 && ((address + (increment * word_count)) & ADDRESS_MASK) > address)
  {
    m_bus->ReadWords(address, m_transfer_buffer.data(), word_count);
  }
  else
  {
    for (u32 i = 0; i < word_count; i++)
    {
      m_bus->DispatchAccess<MemoryAccessType::Read, MemoryAccessSize::Word>(address, m_transfer_buffer[i]);
      address = (address + increment) & ADDRESS_MASK;
    }
  }

  switch (channel)
  {
    case Channel::GPU:
      m_gpu->DMAWrite(m_transfer_buffer.data(), word_count);
      break;

    case Channel::SPU:
      m_spu->DMAWrite(m_transfer_buffer.data(), word_count);
      break;

    case Channel::MDECin:
      m_mdec->DMAWrite(m_transfer_buffer.data(), word_count);
      break;

    case Channel::CDROM:
    case Channel::MDECout:
    case Channel::PIO:
    default:
      Panic("Unhandled DMA channel for device write");
      break;
  }
}

void DMA::TransferDeviceToMemory(Channel channel, u32 address, u32 increment, u32 word_count)
{
  if (m_transfer_buffer.size() < word_count)
    m_transfer_buffer.resize(word_count);

  // Read from device.
  switch (channel)
  {
    case Channel::OTC:
    {
      // clear ordering table
      // this always goes in reverse, so we can generate values in reverse order and write it forwards
      if (((address - (4 * word_count)) & ADDRESS_MASK) < address)
      {
        const u32 end_address = (address - (4 * (word_count - 1))) & ADDRESS_MASK;

        u32 value = end_address;
        m_transfer_buffer[0] = UINT32_C(0xFFFFFF);
        for (u32 i = 1; i < word_count; i++)
        {
          m_transfer_buffer[i] = value;
          value = (value + 4) & ADDRESS_MASK;
        }

        m_bus->WriteWords(end_address, m_transfer_buffer.data(), word_count);
      }
      else
      {
        for (u32 i = 0; i < word_count; i++)
        {
          u32 value = (i == word_count - 1) ? UINT32_C(0xFFFFFFF) : ((address - 4) & ADDRESS_MASK);
          m_bus->DispatchAccess<MemoryAccessType::Write, MemoryAccessSize::Word>(address, value);
          address = (address - 4) & ADDRESS_MASK;
        }
      }

      return;
    }
    break;

    case Channel::GPU:
      m_gpu->DMARead(m_transfer_buffer.data(), word_count);
      break;

    case Channel::CDROM:
      m_cdrom->DMARead(m_transfer_buffer.data(), word_count);
      break;

    case Channel::SPU:
      m_spu->DMARead(m_transfer_buffer.data(), word_count);
      break;

    case Channel::MDECout:
      m_mdec->DMARead(m_transfer_buffer.data(), word_count);
      break;

    case Channel::MDECin:
    case Channel::PIO:
    default:
      Panic("Unhandled DMA channel for device read");
      std::fill_n(m_transfer_buffer.begin(), word_count, UINT32_C(0xFFFFFFFF));
      break;
  }

  if (increment > 0 && ((address + (increment * word_count)) & ADDRESS_MASK) > address)
  {
    m_bus->WriteWords(address, m_transfer_buffer.data(), word_count);
  }
  else
  {
    for (u32 i = 0; i < word_count; i++)
    {
      m_bus->DispatchAccess<MemoryAccessType::Write, MemoryAccessSize::Word>(address, m_transfer_buffer[i]);
      address = (address + increment) & ADDRESS_MASK;
    }
  }
}
