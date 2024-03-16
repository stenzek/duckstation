// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once
#include "types.h"

class StateWrapper;

namespace InterruptController {

static constexpr u32 NUM_IRQS = 11;

enum class IRQ : u32
{
  VBLANK = 0, // IRQ0 - VBLANK
  GPU = 1,    // IRQ1 - GPU via GP0(1Fh)
  CDROM = 2,  // IRQ2 - CDROM
  DMA = 3,    // IRQ3 - DMA
  TMR0 = 4,   // IRQ4 - TMR0 - Sysclk or Dotclk
  TMR1 = 5,   // IRQ5 - TMR1 - Sysclk Hblank
  TMR2 = 6,   // IRQ6 - TMR2 - Sysclk or Sysclk / 8
  PAD = 7,    // IRQ7 - Controller and Memory Card Byte Received
  SIO = 8,    // IRQ8 - SIO
  SPU = 9,    // IRQ9 - SPU
  IRQ10 = 10, // IRQ10 - Lightpen interrupt, PIO

  MaxCount
};

void Reset();
bool DoState(StateWrapper& sw);

void SetLineState(IRQ irq, bool state);

u32 ReadRegister(u32 offset);
void WriteRegister(u32 offset, u32 value);

} // namespace InterruptController
