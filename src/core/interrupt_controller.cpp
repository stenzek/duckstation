// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "interrupt_controller.h"
#include "cpu_core.h"

#include "util/state_wrapper.h"

#include "common/log.h"

Log_SetChannel(InterruptController);

namespace InterruptController {

static constexpr u32 REGISTER_WRITE_MASK = (u32(1) << NUM_IRQS) - 1;
static constexpr u32 DEFAULT_INTERRUPT_MASK = 0;

static void UpdateCPUInterruptRequest();

static u32 s_interrupt_status_register = 0;
static u32 s_interrupt_mask_register = DEFAULT_INTERRUPT_MASK;
static u32 s_interrupt_line_state = 0;

[[maybe_unused]] static constexpr std::array<const char*, static_cast<size_t>(IRQ::MaxCount)> s_irq_names = {
  {"VBLANK", "GPU", "CDROM", "DMA", "TMR0", "TMR1", "TMR2", "PAD", "SIO", "SPU", "IRQ10"}};

} // namespace InterruptController

void InterruptController::Reset()
{
  s_interrupt_status_register = 0;
  s_interrupt_mask_register = DEFAULT_INTERRUPT_MASK;
  s_interrupt_line_state = 0;
}

bool InterruptController::DoState(StateWrapper& sw)
{
  sw.Do(&s_interrupt_status_register);
  sw.Do(&s_interrupt_mask_register);
  sw.DoEx(&s_interrupt_line_state, 63, s_interrupt_status_register);

  return !sw.HasError();
}

void InterruptController::SetLineState(IRQ irq, bool state)
{
  // Interupts are edge-triggered, so only set the flag in the status register on a 0-1 transition.
  const u32 bit = (1u << static_cast<u32>(irq));
  const u32 prev_state = s_interrupt_line_state;
  s_interrupt_line_state = (s_interrupt_line_state & ~bit) | (state ? bit : 0u);
  if (s_interrupt_line_state == prev_state)
    return;

#ifdef _DEBUG
  if (!(prev_state & bit) && state)
    Log_DebugFmt("{} IRQ triggered", s_irq_names[static_cast<size_t>(irq)]);
  else if ((prev_state & bit) && !state)
    Log_DebugFmt("{} IRQ line inactive", s_irq_names[static_cast<size_t>(irq)]);
#endif

  s_interrupt_status_register |= (state ? (prev_state ^ s_interrupt_line_state) : 0u) & s_interrupt_line_state;
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
#ifdef _DEBUG
      const u32 cleared_bits = (s_interrupt_status_register & ~value);
      for (u32 i = 0; i < static_cast<u32>(IRQ::MaxCount); i++)
      {
        if (cleared_bits & (1u << i))
          Log_DebugFmt("{} IRQ cleared", s_irq_names[i]);
      }
#endif

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

ALWAYS_INLINE_RELEASE void InterruptController::UpdateCPUInterruptRequest()
{
  const bool state = (s_interrupt_status_register & s_interrupt_mask_register) != 0;
  CPU::SetIRQRequest(state);
}
