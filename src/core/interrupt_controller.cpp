// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "interrupt_controller.h"
#include "common/log.h"
#include "cpu_core.h"
#include "util/state_wrapper.h"
Log_SetChannel(InterruptController);

namespace InterruptController {

static constexpr u32 REGISTER_WRITE_MASK = (u32(1) << NUM_IRQS) - 1;
static constexpr u32 DEFAULT_INTERRUPT_MASK = 0; //(u32(1) << NUM_IRQS) - 1;

static void UpdateCPUInterruptRequest();

static u32 s_interrupt_status_register = 0;
static u32 s_interrupt_mask_register = DEFAULT_INTERRUPT_MASK;

} // namespace InterruptController

void InterruptController::Initialize()
{
  Reset();
}

void InterruptController::Shutdown() {}

void InterruptController::Reset()
{
  s_interrupt_status_register = 0;
  s_interrupt_mask_register = DEFAULT_INTERRUPT_MASK;
}

bool InterruptController::DoState(StateWrapper& sw)
{
  sw.Do(&s_interrupt_status_register);
  sw.Do(&s_interrupt_mask_register);

  return !sw.HasError();
}

bool InterruptController::GetIRQLineState()
{
  return (s_interrupt_status_register != 0);
}

void InterruptController::InterruptRequest(IRQ irq)
{
  const u32 bit = (u32(1) << static_cast<u32>(irq));
  s_interrupt_status_register |= bit;
  UpdateCPUInterruptRequest();
}

u32 InterruptController::ReadRegister(u32 offset)
{
  switch (offset)
  {
    case 0x00: // I_STATUS
      return s_interrupt_status_register;

    case 0x04: // I_MASK
      return s_interrupt_mask_register;

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
      if ((s_interrupt_status_register & ~value) != 0)
        Log_DebugPrintf("Clearing bits 0x%08X", (s_interrupt_status_register & ~value));

      s_interrupt_status_register = s_interrupt_status_register & (value & REGISTER_WRITE_MASK);
      UpdateCPUInterruptRequest();
    }
    break;

    case 0x04: // I_MASK
    {
      Log_DebugPrintf("Interrupt mask <- 0x%08X", value);
      s_interrupt_mask_register = value & REGISTER_WRITE_MASK;
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
  if ((s_interrupt_status_register & s_interrupt_mask_register) != 0)
    CPU::SetExternalInterrupt(2);
  else
    CPU::ClearExternalInterrupt(2);
}
