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

bool DMA::Initialize(System* system, Bus* bus, InterruptController* interrupt_controller, GPU* gpu, CDROM* cdrom,
                     SPU* spu, MDEC* mdec)
{
  m_system = system;
  m_bus = bus;
  m_interrupt_controller = interrupt_controller;
  m_gpu = gpu;
  m_cdrom = cdrom;
  m_spu = spu;
  m_mdec = mdec;
  return true;
}

void DMA::Reset()
{
  m_transfer_ticks = 0;
  m_transfer_in_progress = false;
  m_state = {};
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
        return m_state[channel_index].base_address;
      case 0x04:
        return m_state[channel_index].block_control.bits;
      case 0x08:
        return m_state[channel_index].channel_control.bits;
      default:
        break;
    }
  }
  else
  {
    if (offset == 0x70)
      return m_DPCR.bits;
    else if (offset == 0x74)
      return m_DICR.bits;
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
        m_DICR.bits = (m_DICR.bits & ~DICR_RESET_MASK) & (value ^ DICR_RESET_MASK);
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

  PhysicalMemoryAddress current_address = cs.base_address & ~UINT32_C(3);
  const PhysicalMemoryAddress increment = cs.channel_control.address_step_reverse ? static_cast<u32>(-4) : UINT32_C(4);
  switch (cs.channel_control.sync_mode)
  {
    case SyncMode::Manual:
    {
      const u32 word_count = cs.block_control.manual.GetWordCount();
      Log_DebugPrintf("DMA%u: Copying %u words %s 0x%08X", static_cast<u32>(channel), word_count,
                      copy_to_device ? "from" : "to", current_address);
      if (copy_to_device)
      {
        u32 words_remaining = word_count;
        do
        {
          words_remaining--;

          u32 value = 0;
          m_bus->DispatchAccess<MemoryAccessType::Read, MemoryAccessSize::Word>(current_address, value);
          DMAWrite(channel, value, current_address, words_remaining);

          current_address = (current_address + increment) & ADDRESS_MASK;
        } while (words_remaining > 0);
      }
      else
      {
        u32 words_remaining = word_count;
        do
        {
          words_remaining--;

          u32 value = DMARead(channel, current_address, words_remaining);
          m_bus->DispatchAccess<MemoryAccessType::Write, MemoryAccessSize::Word>(current_address, value);

          current_address = (current_address + increment) & ADDRESS_MASK;
        } while (words_remaining > 0);
      }
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
          {
            u32 words_remaining = word_count;
            do
            {
              words_remaining--;

              u32 memory_value = 0;
              m_bus->DispatchAccess<MemoryAccessType::Read, MemoryAccessSize::Word>(current_address, memory_value);
              DMAWrite(channel, memory_value, current_address, words_remaining);
              current_address = (current_address + UINT32_C(4)) & ADDRESS_MASK;
            } while (words_remaining > 0);
          }

          if (next_address & UINT32_C(0x800000))
            break;

          current_address = next_address & ADDRESS_MASK;
        }
      }
    }
    break;

    case SyncMode::Request:
    {
      Log_DebugPrintf("DMA%u: Copying %u blocks of size %u %s 0x%08X", static_cast<u32>(channel),
                      cs.block_control.request.GetBlockCount(), cs.block_control.request.GetBlockSize(),
                      copy_to_device ? "from" : "to", current_address);

      u32 blocks_remaining = cs.block_control.request.block_count;

      if (copy_to_device)
      {
        do
        {
          blocks_remaining--;

          u32 words_remaining = cs.block_control.request.block_size;
          do
          {
            words_remaining--;

            u32 value = 0;
            m_bus->DispatchAccess<MemoryAccessType::Read, MemoryAccessSize::Word>(current_address, value);
            DMAWrite(channel, value, current_address, words_remaining);

            current_address = (current_address + increment) & ADDRESS_MASK;
          } while (words_remaining > 0);
        } while (cs.request && blocks_remaining > 0);
      }
      else
      {
        do
        {
          blocks_remaining--;

          u32 words_remaining = cs.block_control.request.block_size;
          do
          {
            words_remaining--;

            u32 value = DMARead(channel, current_address, words_remaining);
            m_bus->DispatchAccess<MemoryAccessType::Write, MemoryAccessSize::Word>(current_address, value);

            current_address = (current_address + increment) & ADDRESS_MASK;
          } while (words_remaining > 0);
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
    Log_TracePrintf("Set DMA interrupt for channel %u", static_cast<u32>(channel));
    m_DICR.SetIRQFlag(channel);
    m_DICR.UpdateMasterFlag();
    if (m_DICR.master_flag)
    {
      Log_TracePrintf("Firing DMA interrupt");
      m_interrupt_controller->InterruptRequest(InterruptController::IRQ::DMA);
    }
  }
}

u32 DMA::DMARead(Channel channel, PhysicalMemoryAddress dst_address, u32 remaining_words)
{
  switch (channel)
  {
    case Channel::OTC:
      // clear ordering table
      return (remaining_words == 0) ? UINT32_C(0xFFFFFF) : ((dst_address - UINT32_C(4)) & ADDRESS_MASK);

    case Channel::GPU:
      return m_gpu->DMARead();

    case Channel::CDROM:
      return m_cdrom->DMARead();

    case Channel::SPU:
      return m_spu->DMARead();

    case Channel::MDECout:
      return m_mdec->DMARead();

    case Channel::MDECin:
    case Channel::PIO:
    default:
      Panic("Unhandled DMA channel read");
      return UINT32_C(0xFFFFFFFF);
  }
}

void DMA::DMAWrite(Channel channel, u32 value, PhysicalMemoryAddress src_address, u32 remaining_words)
{
  switch (channel)
  {
    case Channel::GPU:
      m_gpu->DMAWrite(value);
      return;

    case Channel::SPU:
      m_spu->DMAWrite(value);
      break;

    case Channel::MDECin:
      m_mdec->DMAWrite(value);
      break;

    case Channel::MDECout:
    case Channel::CDROM:
    case Channel::PIO:
    case Channel::OTC:
    default:
      Panic("Unhandled DMA channel write");
      break;
  }
}
