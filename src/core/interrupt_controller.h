#pragma once
#include "types.h"

class StateWrapper;

class InterruptController final
{
public:
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
    IRQ7 = 7,   // IRQ7 - Controller and Memory Card Byte Received
    SIO = 8,    // IRQ8 - SIO
    SPU = 9,    // IRQ9 - SPU
    IRQ10 = 10  // IRQ10 - Lightpen interrupt, PIO
  };


  InterruptController();
  ~InterruptController();

  void Initialize();
  void Shutdown();
  void Reset();
  bool DoState(StateWrapper& sw);

  // Should mirror CPU state.
  ALWAYS_INLINE bool GetIRQLineState() const { return (m_interrupt_status_register != 0); }

  // Interupts are edge-triggered, so if it is masked when TriggerInterrupt() is called, it will be lost.
  void InterruptRequest(IRQ irq);

  // I/O
  u32 ReadRegister(u32 offset);
  void WriteRegister(u32 offset, u32 value);

private:
  static constexpr u32 REGISTER_WRITE_MASK = (u32(1) << NUM_IRQS) - 1;
  static constexpr u32 DEFAULT_INTERRUPT_MASK = 0; //(u32(1) << NUM_IRQS) - 1;

  void UpdateCPUInterruptRequest();

  u32 m_interrupt_status_register = 0;
  u32 m_interrupt_mask_register = DEFAULT_INTERRUPT_MASK;
};

extern InterruptController g_interrupt_controller;