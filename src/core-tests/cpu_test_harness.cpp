// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "cpu_test_harness.h"

#include "core/cpu_code_cache.h"
#include "core/cpu_core.h"
#include "core/cpu_core_private.h"
#include "core/dma.h"
#include "core/interrupt_controller.h"
#include "core/pad.h"
#include "core/sio.h"
#include "core/spu.h"
#include "core/system.h"
#include "core/system_private.h"
#include "core/timers.h"
#include "core/timing_event.h"

#include "common/error.h"

using namespace Mips::Encoder;

namespace CPUTestHarness {

void Program::LoadImmediate(Reg reg, u32 value)
{
  Emit(lui(reg, static_cast<u16>(value >> 16)));
  Emit(ori(reg, reg, static_cast<u16>(value)));
}

u32 Program::StoreResult(Reg reg)
{
  const u32 slot = m_num_results++;
  Emit(lui(Reg::T9, static_cast<u16>(RESULTS_BASE >> 16)));
  Emit(sw(reg, static_cast<s16>(slot * sizeof(u32)), Reg::T9));
  return slot;
}

void Program::Run(u32 runs)
{
  std::vector<u32> code;
  code.push_back(lui(Reg::AT, static_cast<u16>(ADDR_EXIT >> 16)));
  code.insert(code.end(), m_code.begin(), m_code.end());
  code.push_back(sb(Reg::R0, static_cast<s16>(ADDR_EXIT & 0xFFFFu), Reg::AT));
  code.push_back(nop());
  code.push_back(nop());

  // Writes to timers 0/1 start the CRTC event, which can't run without a GPU.
  for (TimingEvent** event = TimingEvents::GetHeadEventPtr(); *event;)
  {
    if (std::strcmp((*event)->GetName(), "GPU CRTC Tick") == 0)
      (*event)->Deactivate();
    else
      event = &(*event)->next;
  }

  // Get rid of the previous program, this also makes the pages it was in writable again.
  CPU::CodeCache::InvalidateAllRAMBlocks();
  std::memcpy(&g_bus.ram[PROGRAM_BASE & g_bus.ram_mask], code.data(), code.size() * sizeof(u32));
  CPU::ClearICache();
  for (u32 i = 0; i < runs; i++)
  {
    CPU::SetPC(PROGRAM_BASE);
    CPU::Execute();
  }
}

void ExecutionModeTest::SetUpTestSuite()
{
  g_settings.cpu_execution_mode = CPUExecutionMode::Interpreter;
  g_settings.pcsx_expansion_region_enable = true;
  g_settings.audio_backend = AudioBackend::Null;

  Error error;
  ASSERT_TRUE(CPU::CodeCache::ProcessStartup(&error)) << error.GetDescription();
  ASSERT_TRUE(Bus::AllocateMemory(false, &error)) << error.GetDescription();
  Bus::Initialize();
  CPU::Initialize();
  TimingEvents::Initialize();
  System::UpdateOverclock();
  Bus::Reset();

  // Otherwise the interpreter stays on the debug dispatcher waiting for the entry point.
  System::SetGameStarted();
  CPU::Reset();

  InterruptController::Reset();
  Timers::Initialize();
  DMA::Initialize();
  SPU::Initialize();
  SPU::SetAudioOutputMuted(true);
  Pad::Initialize();
  SIO::Initialize();

  // The CPU needs something to count down to.
  s_event = std::make_unique<TimingEvent>("Test Event", 1000000, 1000000, [](void*, TickCount) {}, nullptr);
  s_event->Activate();
}

void ExecutionModeTest::TearDownTestSuite()
{
  s_event->Deactivate();
  s_event.reset();
  SIO::Shutdown();
  Pad::Shutdown();
  SPU::Shutdown();
  SPU::SetAudioOutputMuted(false);
  DMA::Shutdown();
  Timers::Shutdown();
  while (TimingEvent* event = *TimingEvents::GetHeadEventPtr())
    event->Deactivate();
  CPU::CodeCache::Shutdown();
  CPU::Shutdown();
  Bus::Shutdown();
  Bus::ReleaseMemory();
  CPU::CodeCache::ProcessShutdown();
}

} // namespace CPUTestHarness
