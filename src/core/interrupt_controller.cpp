#include "interrupt_controller.h"
#include "common/log.h"
#include "cpu_core.h"
#include "util/state_wrapper.h"
Log_SetChannel(InterruptController);

InterruptController g_interrupt_controller;

InterruptController::InterruptController() = default;

InterruptController::~InterruptController() = default;

void InterruptController::Initialize()
{
  Reset();
}

void InterruptController::Shutdown() {}

void InterruptController::Reset()
{
  m_interrupt_status_register = 0;
  m_interrupt_mask_register = DEFAULT_INTERRUPT_MASK;
}

bool InterruptController::DoState(StateWrapper& sw)
{
  sw.Do(&m_interrupt_status_register);
  sw.Do(&m_interrupt_mask_register);

  return !sw.HasError();
}

void InterruptController::InterruptRequest(IRQ irq)
{
  const u32 bit = (u32(1) << static_cast<u32>(irq));
  m_interrupt_status_register |= bit;
  UpdateCPUInterruptRequest();
}

u32 InterruptController::ReadRegister(u32 offset)
{
  switch (offset)
  {
    case 0x00: // I_STATUS
      return m_interrupt_status_register;

    case 0x04: // I_MASK
      return m_interrupt_mask_register;

    default:
      Log_ErrorPrintf("Invalid read at offset 0x%08X", offset);
      return UINT32_C(0xFFFFFFFF);
  }
}

void InterruptController::WriteRegister(u32 offset, u32 value)
{
  switch (offset)
  {
    case 0x00: // I_STATUS
    {
      if ((m_interrupt_status_register & ~value) != 0)
        Log_DebugPrintf("Clearing bits 0x%08X", (m_interrupt_status_register & ~value));

      m_interrupt_status_register = m_interrupt_status_register & (value & REGISTER_WRITE_MASK);
      UpdateCPUInterruptRequest();
    }
    break;

    case 0x04: // I_MASK
    {
      Log_DebugPrintf("Interrupt mask <- 0x%08X", value);
      m_interrupt_mask_register = value & REGISTER_WRITE_MASK;
      UpdateCPUInterruptRequest();
    }
    break;

    default:
      Log_ErrorPrintf("Invalid write at offset 0x%08X", offset);
      break;
  }
}

void InterruptController::UpdateCPUInterruptRequest()
{
  // external interrupts set bit 10 only?
  if ((m_interrupt_status_register & m_interrupt_mask_register) != 0)
    CPU::SetExternalInterrupt(2);
  else
    CPU::ClearExternalInterrupt(2);
}
