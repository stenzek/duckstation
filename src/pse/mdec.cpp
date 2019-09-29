#include "mdec.h"
#include "YBaseLib/Log.h"
#include "common/state_wrapper.h"
#include "dma.h"
#include "interrupt_controller.h"
#include "system.h"
Log_SetChannel(MDEC);

MDEC::MDEC() = default;

MDEC::~MDEC() = default;

bool MDEC::Initialize(System* system, DMA* dma)
{
  m_system = system;
  m_dma = dma;
  return true;
}

void MDEC::Reset()
{
  SoftReset();
}

bool MDEC::DoState(StateWrapper& sw)
{
  sw.Do(&m_status_register.bits);
  sw.Do(&m_data_in_fifo);
  sw.Do(&m_data_out_fifo);

  return !sw.HasError();
}

u32 MDEC::ReadRegister(u32 offset)
{
  switch (offset)
  {
    case 0:
      return ReadDataRegister();

    case 4:
    {
      Log_DebugPrintf("MDEC status register -> 0x%08X", m_status_register.bits);
      return m_status_register.bits;
    }

    default:
    {
      Log_ErrorPrintf("Unknown MDEC register read: 0x%08X", offset);
      return UINT32_C(0xFFFFFFFF);
    }
  }
}

void MDEC::WriteRegister(u32 offset, u32 value)
{
  switch (offset)
  {
    case 0:
    {
      WriteCommandRegister(value);
      return;
    }

    case 4:
    {
      Log_DebugPrintf("MDEC control register <- 0x%08X", value);

      const ControlRegister cr{value};
      if (cr.reset)
        SoftReset();

      m_status_register.data_in_request = cr.enable_dma_in;
      m_status_register.data_out_request = cr.enable_dma_out;
      m_dma->SetRequest(DMA::Channel::MDECin, cr.enable_dma_in);
      m_dma->SetRequest(DMA::Channel::MDECout, cr.enable_dma_out);

      return;
    }

    default:
    {
      Log_ErrorPrintf("Unknown MDEC register write: 0x%08X <- 0x%08X", offset, value);
      return;
    }
  }
}

u32 MDEC::DMARead()
{
  return ReadDataRegister();
}

void MDEC::DMAWrite(u32 value)
{
  WriteCommandRegister(value);
}

void MDEC::SoftReset()
{
  m_status_register = {};
  m_data_in_fifo.Clear();
  m_data_out_fifo.Clear();

  UpdateStatusRegister();
}

void MDEC::UpdateStatusRegister()
{
  m_status_register.data_out_fifo_empty = m_data_out_fifo.IsEmpty();
  m_status_register.data_in_fifo_full = m_data_in_fifo.IsFull();
}

void MDEC::WriteCommandRegister(u32 value)
{
  Log_DebugPrintf("MDEC command/data register <- 0x%08X", value);

  m_data_in_fifo.Push(value);
  HandleCommand();
  UpdateStatusRegister();
}

u32 MDEC::ReadDataRegister()
{
  if (m_data_out_fifo.IsEmpty())
  {
    Log_WarningPrintf("MDEC data out FIFO empty on read");
    return UINT32_C(0xFFFFFFFF);
  }

  const u32 value = m_data_out_fifo.Pop();
  UpdateStatusRegister();
  return value;
}

void MDEC::HandleCommand()
{
  Log_DebugPrintf("MDEC command: 0x%08X", m_data_in_fifo.Peek(0));
}
