// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

// Ports of psxdiag tests that pass on hardware, run through each CPU execution mode.

#include "core/bus.h"
#include "core/cpu_code_cache.h"
#include "core/cpu_core.h"
#include "core/cpu_core_private.h"
#include "core/dma.h"
#include "core/interrupt_controller.h"
#include "core/mips_encoder.h"
#include "core/pad.h"
#include "core/settings.h"
#include "core/sio.h"
#include "core/spu.h"
#include "core/system.h"
#include "core/timers.h"
#include "core/timing_event.h"

#include "common/error.h"

#include <gtest/gtest.h>

#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace Mips::Encoder;

// pcsx_debugbreak(), used to get out of the execution loop.
static constexpr u32 ADDR_EXIT = 0x1F802081u;

static constexpr u32 PROGRAM_BASE = 0x80010000u;
static constexpr u32 RESULTS_BASE = 0x80100000u;
static constexpr u32 RAM_TEST_ADDR = 0x80180000u;

/// Builds a program, results are stored to RAM because the recompiler doesn't write registers back when exiting.
class Program
{
public:
  void Emit(u32 inst) { m_code.push_back(inst); }

  void LoadImmediate(Reg reg, u32 value)
  {
    Emit(lui(reg, static_cast<u16>(value >> 16)));
    Emit(ori(reg, reg, static_cast<u16>(value)));
  }

  /// Stores the register to the next result slot, and returns the slot.
  u32 StoreResult(Reg reg)
  {
    const u32 slot = m_num_results++;
    Emit(lui(Reg::T9, static_cast<u16>(RESULTS_BASE >> 16)));
    Emit(sw(reg, static_cast<s16>(slot * sizeof(u32)), Reg::T9));
    return slot;
  }

  /// Executes the program the specified number of times, the first run warms up the icache.
  void Run(u32 runs = 1)
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

  u32 Result(u32 slot) const { return ReadRAM(RESULTS_BASE + slot * sizeof(u32)); }

  static u32 ReadRAM(u32 address)
  {
    u32 value;
    std::memcpy(&value, &g_bus.ram[address & g_bus.ram_mask], sizeof(value));
    return value;
  }

private:
  std::vector<u32> m_code;
  u32 m_num_results = 0;
};

class PsxDiag : public ::testing::TestWithParam<CPUExecutionMode>
{
protected:
  static void SetUpTestSuite()
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

  static void TearDownTestSuite()
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

  void SetUp() override
  {
    // The switch itself happens when the CPU is next executed.
    g_settings.cpu_execution_mode = GetParam();
  }

  static inline std::unique_ptr<TimingEvent> s_event;
};

} // namespace

//////////////////////////////////////////////////////////////////////////
// general_8_16_32_bit_writes
//////////////////////////////////////////////////////////////////////////

namespace {

enum class WriteType : u8
{
  MWok, // 8/16/32-bit writes all work
  MW32, // 8/16-bit writes latch the whole shifted word
  MW16, // 8-bit writes latch the shifted halfword, 32-bit writes only change the low halfword
  MWro, // writes are ignored
  MI16, // 8-bit writes to the high byte of each halfword are ignored
};

struct WriteTestRegister
{
  u32 address;
  u32 mask;
  u32 base;
  u32 force_mask;
  WriteType type;
  u32 garbage_mask;
};

static constexpr WriteTestRegister s_write_test_registers[] = {
  {RAM_TEST_ADDR, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, WriteType::MWok, 0xFFFFFFFFu}, // main ram
  {0x1F800100u, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, WriteType::MWok, 0xFFFFFFFFu},   // scratchpad
  {0x1F801004u, 0x00FFFFFFu, 0x1F000000u, 0xFFFFFFFFu, WriteType::MW32, 0xFFFFFFFFu},   // exp.2 base
  {0x1F801008u, 0xAF1FFFFFu, 0x00000000u, 0xFF05FFFFu, WriteType::MW32, 0xFFFFFFFFu},   // exp.1 delay/size
  {0x1F80100Cu, 0x2F1FFFFFu, 0x00000000u, 0xFF05FFFFu, WriteType::MW32, 0xFFFFFFFFu},   // exp.3 delay/size
  {0x1F801010u, 0x2F1FFFFFu, 0x00000000u, 0xFF05FFFFu, WriteType::MW32, 0xFFFFFFFFu},   // bios delay/size
  {0x1F801014u, 0x2F1FFFFFu, 0x00000000u, 0xFFFFFFFFu, WriteType::MW32, 0xFFFFFFFFu},   // spu delay/size
  {0x1F801018u, 0x2F1FFFFFu, 0x00000000u, 0xFFFFFFFFu, WriteType::MW32, 0xFFFFFFFFu},   // cdrom delay/size
  {0x1F80101Cu, 0x2F1FFFFFu, 0x00000000u, 0xFF05FFFFu, WriteType::MW32, 0xFFFFFFFFu},   // exp.2 delay/size
  {0x1F801020u, 0x0003FFFFu, 0x00000000u, 0xFFFFFFFFu, WriteType::MW32, 0xFFFFFFFFu},   // com delay
  {0x1F801048u, 0x3F2F013Fu, 0x00000000u, 0xFFBBFFFFu, WriteType::MW16, 0xFFFFFFFFu},   // joy mode/ctrl
  {0x1F80104Cu, 0xFFFF0000u, 0x00000000u, 0xFFFFFFFFu, WriteType::MW16, 0xFFFF0000u},   // joy unused/baud
  {0x1F801058u, 0x1FAF00FFu, 0x00000000u, 0xFFBFFFFFu, WriteType::MW16, 0xFF7FFFFFu},   // sio mode/ctrl
  {0x1F80105Cu, 0xFFFF0000u, 0x00000000u, 0xFFFFFFFFu, WriteType::MW16, 0xFFFF0000u},   // sio internal/baud
  {0x1F801074u, 0x000007FFu, 0x00000000u, 0xFFFFFFFFu, WriteType::MW32, 0x0000FFFFu},   // i_mask
  {0x1F801104u, 0x00001FFFu, 0x00000000u, 0xFFFFFFFFu, WriteType::MW32, 0x0000E3FFu},   // tmr0 mode
  {0x1F801108u, 0x0000FFFFu, 0x00000000u, 0xFFFFFFFFu, WriteType::MW32, 0x0000FFFFu},   // tmr0 target
  {0x1F801114u, 0x00001FFFu, 0x00000000u, 0xFFFFFFFFu, WriteType::MW32, 0x0000E3FFu},   // tmr1 mode
  {0x1F801118u, 0x0000FFFFu, 0x00000000u, 0xFFFFFFFFu, WriteType::MW32, 0x0000FFFFu},   // tmr1 target
  {0x1F801124u, 0x00001FFFu, 0x00000000u, 0xFFFFFFFFu, WriteType::MW32, 0x0000E3FFu},   // tmr2 mode
  {0x1F801128u, 0x0000FFFFu, 0x00000000u, 0xFFFFFFFFu, WriteType::MW32, 0x0000FFFFu},   // tmr2 target
  {0x1F801080u, 0x00FFFFFFu, 0x00000000u, 0xFFFFFFFFu, WriteType::MW32, 0xFFFFFFFFu},   // dma0 madr
  {0x1F801084u, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, WriteType::MWok, 0xFFFFFFFFu},   // dma0 bcr
  {0x1F801088u, 0x71770703u, 0x00000000u, 0xEEFFFFFFu, WriteType::MW32, 0xFFFFFFFFu},   // dma0 chcr
  {0x1F80108Cu, 0x71770703u, 0x00000000u, 0xEEFFFFFFu, WriteType::MW32, 0xFFFFFFFFu},   // dma0 chcr mirror
  {0x1F801090u, 0x00FFFFFFu, 0x00000000u, 0xFFFFFFFFu, WriteType::MW32, 0xFFFFFFFFu},   // dma1 madr
  {0x1F801094u, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, WriteType::MWok, 0xFFFFFFFFu},   // dma1 bcr
  {0x1F801098u, 0x71770703u, 0x00000000u, 0xEEFFFFFFu, WriteType::MW32, 0xFFFFFFFFu},   // dma1 chcr
  {0x1F80109Cu, 0x71770703u, 0x00000000u, 0xEEFFFFFFu, WriteType::MW32, 0xFFFFFFFFu},   // dma1 chcr mirror
  {0x1F8010A0u, 0x00FFFFFFu, 0x00000000u, 0xFFFFFFFFu, WriteType::MW32, 0xFFFFFFFFu},   // dma2 madr
  {0x1F8010A4u, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, WriteType::MWok, 0xFFFFFFFFu},   // dma2 bcr
  {0x1F8010A8u, 0x71770703u, 0x00000000u, 0xEEFFFFFFu, WriteType::MW32, 0xFFFFFFFFu},   // dma2 chcr
  {0x1F8010ACu, 0x71770703u, 0x00000000u, 0xEEFFFFFFu, WriteType::MW32, 0xFFFFFFFFu},   // dma2 chcr mirror
  {0x1F8010B0u, 0x00FFFFFFu, 0x00000000u, 0xFFFFFFFFu, WriteType::MW32, 0xFFFFFFFFu},   // dma3 madr
  {0x1F8010B4u, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, WriteType::MWok, 0xFFFFFFFFu},   // dma3 bcr
  {0x1F8010B8u, 0x71770703u, 0x00000000u, 0xEEFFFFFFu, WriteType::MW32, 0xFFFFFFFFu},   // dma3 chcr
  {0x1F8010BCu, 0x71770703u, 0x00000000u, 0xEEFFFFFFu, WriteType::MW32, 0xFFFFFFFFu},   // dma3 chcr mirror
  {0x1F8010C0u, 0x00FFFFFFu, 0x00000000u, 0xFFFFFFFFu, WriteType::MW32, 0xFFFFFFFFu},   // dma4 madr
  {0x1F8010C4u, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, WriteType::MWok, 0xFFFFFFFFu},   // dma4 bcr
  {0x1F8010C8u, 0x71770703u, 0x00000000u, 0xEEFFFFFFu, WriteType::MW32, 0xFFFFFFFFu},   // dma4 chcr
  {0x1F8010CCu, 0x71770703u, 0x00000000u, 0xEEFFFFFFu, WriteType::MW32, 0xFFFFFFFFu},   // dma4 chcr mirror
  {0x1F8010D0u, 0x00FFFFFFu, 0x00000000u, 0xFFFFFFFFu, WriteType::MW32, 0xFFFFFFFFu},   // dma5 madr
  {0x1F8010D4u, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, WriteType::MWok, 0xFFFFFFFFu},   // dma5 bcr
  {0x1F8010D8u, 0x71770703u, 0x00000000u, 0xEEFFFFFFu, WriteType::MW32, 0xFFFFFFFFu},   // dma5 chcr
  {0x1F8010DCu, 0x71770703u, 0x00000000u, 0xEEFFFFFFu, WriteType::MW32, 0xFFFFFFFFu},   // dma5 chcr mirror
  {0x1F8010E0u, 0x00FFFFFFu, 0x00000000u, 0xFFFFFFFFu, WriteType::MW32, 0xFFFFFFFFu},   // dma6 madr
  {0x1F8010E4u, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, WriteType::MWok, 0xFFFFFFFFu},   // dma6 bcr
  {0x1F8010E8u, 0x51000000u, 0x00000002u, 0xEEFFFFFFu, WriteType::MW32, 0xFFFFFFFFu},   // dma6 chcr
  {0x1F8010ECu, 0x51000000u, 0x00000002u, 0xEEFFFFFFu, WriteType::MW32, 0xFFFFFFFFu},   // dma6 chcr mirror
  {0x1F8010F0u, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, WriteType::MW32, 0xFFFFFFFFu},   // dpcr
  {0x1F8010F4u, 0x00FF803Fu, 0x00000000u, 0xFFFF7FFFu, WriteType::MW32, 0xFFFFFFFFu},   // dicr
  {0x1F8010F8u, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, WriteType::MWro, 0xFFFFFFFFu},   // dma unknown
  {0x1F8010FCu, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, WriteType::MWro, 0xFFFFFFFFu},   // dma unknown
  {0x1F801C00u, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, WriteType::MI16, 0xFFFFFFFFu},   // spu voices
  {0x1F801D70u, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, WriteType::MI16, 0xFFFFFFFFu},   // spu voices
  {0x1F801D80u, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, WriteType::MI16, 0xFFFFFFFFu},   // spu control
  {0x1F801DBCu, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, WriteType::MI16, 0xFFFFFFFFu},   // spu unused
  {0x1F801DC0u, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, WriteType::MI16, 0xFFFFFFFFu},   // spu reverb
  {0x1F801DFCu, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, WriteType::MI16, 0xFFFFFFFFu},   // spu reverb
  {0x1F801E60u, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, WriteType::MI16, 0xFFFFFFFFu},   // spu unused
  {0x1F801E7Cu, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, WriteType::MI16, 0xFFFFFFFFu},   // spu unused
  {0x1F801E80u, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, WriteType::MWro, 0xFFFFFFFFu},   // spu high-z
  {0x1F801FFCu, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, WriteType::MWro, 0xFFFFFFFFu},   // spu high-z
};

static constexpr u32 ROR(u32 value, u32 bits)
{
  return (bits == 0) ? value : ((value >> bits) | (value << (32 - bits)));
}

/// Builds the psxdiag sequence for one register, the checks are evaluated after the program runs.
class WriteTestProgram : public Program
{
public:
  using StoreFunction = u32 (*)(Reg, s16, Reg);

  void SetRegister(const WriteTestRegister& reg) { LoadImmediate(Reg::A0, reg.address); }

  void Store(StoreFunction op, s16 offset, u32 value)
  {
    LoadImmediate(Reg::A1, value);
    Emit(op(Reg::A1, offset, Reg::A0));
  }

  u32 Read()
  {
    Emit(lw(Reg::V0, 0, Reg::A0));
    Emit(nop());
    return StoreResult(Reg::V0);
  }

  /// Compares the readback against the expected value, which can depend on a value read before the writes.
  void Check(const WriteTestRegister& reg, const char* what, u32 garbage_mask, u32 old_slot,
             std::function<u32(u32)> expected)
  {
    m_checks.push_back({&reg, what, Read(), old_slot, garbage_mask, std::move(expected)});
  }

  void Restore(const WriteTestRegister& reg, u32 original)
  {
    SetRegister(reg);
    Store(&sh, 0, original);
    Store(&sh, 2, original >> 16);
    Store(&sw, 0, original);
  }

  void Verify(u32 r7) const
  {
    for (const CheckInfo& ci : m_checks)
    {
      const u32 old = (ci.old_slot != NO_SLOT) ? Result(ci.old_slot) : 0;
      EXPECT_EQ(Result(ci.got_slot) & ci.garbage_mask, ci.expected(old) & ci.garbage_mask)
        << ci.what << " to " << std::hex << ci.reg->address << " with " << r7;
    }
  }

  static constexpr u32 NO_SLOT = 0xFFFFFFFFu;

private:
  struct CheckInfo
  {
    const WriteTestRegister* reg;
    const char* what;
    u32 got_slot;
    u32 old_slot;
    u32 garbage_mask;
    std::function<u32(u32)> expected;
  };

  std::vector<CheckInfo> m_checks;
};

} // namespace

TEST_P(PsxDiag, Writes8_16_32)
{
  static constexpr u32 NO_SLOT = WriteTestProgram::NO_SLOT;

  std::vector<u32> original;
  {
    WriteTestProgram prog;
    std::vector<u32> slots;
    for (const WriteTestRegister& reg : s_write_test_registers)
    {
      prog.SetRegister(reg);
      slots.push_back(prog.Read());
    }
    prog.Run();
    for (const u32 slot : slots)
      original.push_back(prog.Result(slot));
  }

  // psxdiag reads the forced OR value from the forced mask slot, so the forced mask is always what gets written.
  for (const u32 r7 : {0x00000000u, 0x12345678u, 0xFFFFFFFFu})
  {
    for (size_t i = 0; i < std::size(s_write_test_registers); i++)
    {
      // One program per register, since timer 0/1 writes start the CRTC event.
      WriteTestProgram prog;
      const WriteTestRegister& reg = s_write_test_registers[i];
      const u32 g = reg.garbage_mask;
      prog.SetRegister(reg);
      if (reg.type == WriteType::MW16)
      {
        const u32 old_slot = prog.Read();
        prog.Store(&sw, 0, reg.force_mask & r7);
        prog.Check(reg, "32-bit MW16", g, old_slot,
                   [&reg, r7](u32 old) { return ((r7 & 0xFFFFu) | (old & 0xFFFF0000u)) & reg.mask; });
      }
      else if (reg.type == WriteType::MWro)
      {
        const u32 old_slot = prog.Read();
        prog.Store(&sw, 0, r7);
        prog.Check(reg, "32-bit MWro", g, old_slot, [](u32 old) { return old; });
      }
      else
      {
        const u32 v = (r7 & reg.force_mask) | reg.force_mask;
        prog.Store(&sw, 0, v);
        prog.Store(&sw, 0, v);
        prog.Check(reg, "32-bit", g, NO_SLOT, [&reg, v](u32) { return (v & reg.mask) | reg.base; });
      }
      prog.Restore(reg, original[i]);
      prog.Run();
      prog.Verify(r7);
    }
  }

  for (const u32 r7 : {0x00000000u, 0x12345678u, 0xFFFFFFFFu})
  {
    for (size_t i = 0; i < std::size(s_write_test_registers); i++)
    {
      // One program per register, since timer 0/1 writes start the CRTC event.
      WriteTestProgram prog;
      const WriteTestRegister& reg = s_write_test_registers[i];
      const u32 g = reg.garbage_mask;
      prog.SetRegister(reg);
      if (reg.type == WriteType::MWok || reg.type == WriteType::MI16)
      {
        prog.Store(&sw, 0, 0x11223344u);
        prog.Store(&sw, 0, 0x11223344u);
        prog.Store(&sh, 0, r7);
        prog.Store(&sh, 0, r7);
        prog.Check(reg, "16-bit low", 0xFFFFFFFFu, NO_SLOT, [r7](u32) { return 0x11220000u | (r7 & 0xFFFFu); });
        prog.Store(&sw, 0, 0x55667788u);
        prog.Store(&sw, 0, 0x55667788u);
        prog.Store(&sh, 2, r7);
        prog.Store(&sh, 2, r7);
        prog.Check(reg, "16-bit high", 0xFFFFFFFFu, NO_SLOT, [r7](u32) { return (r7 << 16) + 0x7788u; });
      }
      else if (reg.type == WriteType::MW16)
      {
        u32 old_slot = prog.Read();
        const u32 lo = reg.force_mask & r7;
        prog.Store(&sh, 0, lo);
        prog.Check(reg, "16-bit MW16 low", g, old_slot,
                   [&reg, lo](u32 old) { return ((lo & 0xFFFFu) | (old & 0xFFFF0000u)) & reg.mask; });
        old_slot = prog.Read();
        const u32 hi = ROR(reg.force_mask & r7, 16);
        prog.Store(&sh, 2, hi);
        prog.Check(reg, "16-bit MW16 high", g, old_slot,
                   [&reg, hi](u32 old) { return ((hi << 16) | (old & 0xFFFFu)) & reg.mask; });
      }
      else if (reg.type == WriteType::MWro)
      {
        const u32 old_slot = prog.Read();
        prog.Store(&sh, 0, r7);
        prog.Store(&sh, 2, r7);
        prog.Check(reg, "16-bit MWro", g, old_slot, [](u32 old) { return old; });
      }
      else if (reg.type == WriteType::MW32)
      {
        const u32 v = (r7 & reg.force_mask) | reg.force_mask;
        prog.Store(&sh, 0, v);
        prog.Check(reg, "16-bit MW32 +0", g, NO_SLOT, [&reg, v](u32) { return (v & reg.mask) | reg.base; });
        prog.Store(&sh, 2, ROR(v, 16));
        prog.Check(reg, "16-bit MW32 +2", g, NO_SLOT,
                   [&reg, v](u32) { return ((ROR(v, 16) << 16) & reg.mask) | reg.base; });
      }
      prog.Restore(reg, original[i]);
      prog.Run();
      prog.Verify(r7);
    }
  }

  for (const u32 r7 : {0x00000000u, 0x12345678u, 0xFFFFFFFFu})
  {
    for (size_t i = 0; i < std::size(s_write_test_registers); i++)
    {
      // One program per register, since timer 0/1 writes start the CRTC event.
      WriteTestProgram prog;
      const WriteTestRegister& reg = s_write_test_registers[i];
      const u32 g = reg.garbage_mask;
      prog.SetRegister(reg);
      if (reg.type == WriteType::MWok)
      {
        prog.Store(&sw, 0, 0x11223344u);
        prog.Store(&sb, 0, r7);
        prog.Check(reg, "8-bit low", 0xFFFFFFFFu, NO_SLOT, [r7](u32) { return 0x11223300u | (r7 & 0xFFu); });
        prog.Store(&sw, 0, 0x55667788u);
        prog.Store(&sb, 3, r7);
        prog.Check(reg, "8-bit high", 0xFFFFFFFFu, NO_SLOT, [r7](u32) { return 0x00667788u | (r7 << 24); });
      }
      else if (reg.type == WriteType::MI16)
      {
        u32 old_slot = prog.Read();
        const u32 lo = reg.force_mask & r7;
        prog.Store(&sb, 0, lo);
        prog.Store(&sb, 0, lo);
        prog.Store(&sb, 3, 0);
        prog.Store(&sb, 3, 0);
        prog.Check(reg, "8-bit MI16 low", g, old_slot,
                   [&reg, r7](u32 old) { return ((r7 & 0xFFFFu) | (old & 0xFFFF0000u)) & reg.mask; });
        old_slot = prog.Read();
        const u32 hi = ROR(ROR(reg.force_mask & r7, 8), 16);
        prog.Store(&sb, 2, hi);
        prog.Store(&sb, 2, hi);
        prog.Store(&sb, 1, 0);
        prog.Store(&sb, 1, 0);
        prog.Check(reg, "8-bit MI16 high", g, old_slot,
                   [&reg, hi](u32 old) { return ((hi << 16) | (old & 0xFFFFu)) & reg.mask; });
      }
      else if (reg.type == WriteType::MW16)
      {
        u32 old_slot = prog.Read();
        prog.Store(&sb, 0, reg.force_mask & r7);
        prog.Check(reg, "8-bit MW16 low", g, old_slot,
                   [&reg, r7](u32 old) { return ((r7 & 0xFFFFu) | (old & 0xFFFF0000u)) & reg.mask; });
        old_slot = prog.Read();
        const u32 hi = ROR(ROR(reg.force_mask & r7, 8), 16);
        prog.Store(&sb, 3, hi);
        prog.Check(reg, "8-bit MW16 high", g, old_slot,
                   [&reg, hi](u32 old) { return ((hi << 24) | (old & 0xFFFFu)) & reg.mask; });
      }
      else if (reg.type == WriteType::MW32)
      {
        const u32 v = (r7 & reg.force_mask) | reg.force_mask;
        for (const u32 offset : {0u, 1u, 2u, 3u})
        {
          prog.Store(&sb, static_cast<s16>(offset), ROR(v, offset * 8));
          prog.Check(reg, "8-bit MW32", g, NO_SLOT,
                     [&reg, v, offset](u32) { return ((ROR(v, offset * 8) << (offset * 8)) & reg.mask) | reg.base; });
        }
      }
      else if (reg.type == WriteType::MWro)
      {
        const u32 old_slot = prog.Read();
        for (const s16 offset : {0, 1, 2, 3})
          prog.Store(&sb, offset, r7);
        prog.Check(reg, "8-bit MWro", g, old_slot, [](u32 old) { return old; });
      }
      prog.Restore(reg, original[i]);
      prog.Run();
      prog.Verify(r7);
    }
  }
}

//////////////////////////////////////////////////////////////////////////
// general_cop_memaccess_jmp
//////////////////////////////////////////////////////////////////////////

TEST_P(PsxDiag, CopMemAccessJmp)
{
  const u32 old_sr = CPU::g_state.cop0_regs.sr.bits;

  // cop2 is skipped, since swc2 stores the GTE registers.
  for (const u32 cop : {0u, 1u, 3u})
  {
    const auto swc = [cop](u32 rt, s16 offset, Reg base) {
      return ((0x38u | cop) << 26) | (static_cast<u32>(base) << 21) | (rt << 16) | static_cast<u16>(offset);
    };
    const auto bc = [cop](bool cond, s16 offset) {
      return ((0x10u | cop) << 26) | (0x08u << 21) | (static_cast<u32>(cond) << 16) | static_cast<u16>(offset);
    };

    // Nothing drives the bus for swcN, so the next instruction gets stored.
    Program prog;
    prog.LoadImmediate(Reg::T0, 0x10000000u << cop);
    prog.Emit(mtc0(Reg::T0, 12));
    prog.Emit(lui(Reg::T1, static_cast<u16>(RAM_TEST_ADDR >> 16)));
    prog.Emit(swc(12, 0, Reg::T1));
    prog.Emit(swc(13, 4, Reg::T1));

    // bcNt doesn't branch, bcNf does.
    prog.Emit(ori(Reg::V0, Reg::R0, 0x5500));
    prog.Emit(bc(true, 2));
    prog.Emit(nop());
    prog.Emit(ori(Reg::V0, Reg::V0, 0x02));
    prog.Emit(bc(false, 2));
    prog.Emit(nop());
    prog.Emit(ori(Reg::V0, Reg::V0, 0x04));
    prog.Emit(mtc0(Reg::R0, 12));
    const u32 result = prog.StoreResult(Reg::V0);
    prog.Run();

    EXPECT_EQ(Program::ReadRAM(RAM_TEST_ADDR), swc(13, 4, Reg::T1)) << "swc" << cop;
    EXPECT_EQ(Program::ReadRAM(RAM_TEST_ADDR + 4), ori(Reg::V0, Reg::R0, 0x5500)) << "swc" << cop;
    EXPECT_EQ(prog.Result(result), 0x5502u) << "bc" << cop;
  }

  CPU::g_state.cop0_regs.sr.bits = old_sr;
}

//////////////////////////////////////////////////////////////////////////
// Not from psxdiag
//////////////////////////////////////////////////////////////////////////

TEST_P(PsxDiag, ChoppedManualDMARunsWhileGPUTransferIsSliced)
{
  const Settings old_settings = g_settings;
  g_settings.dma_max_slice_ticks = 32;
  g_settings.dma_halt_ticks = 16;
  DMA::Reset();

  // An empty GPU linked list that loops back to itself keeps exhausting its DMA slice without sending GPU commands.
  const u32 node_address = RAM_TEST_ADDR & g_bus.ram_mask;
  std::memcpy(&g_bus.ram[node_address], &node_address, sizeof(node_address));
  DMA::WriteRegister(0x70, 0);
  DMA::WriteRegister(0x20, node_address);
  DMA::WriteRegister(0x28, 0x01000401);
  DMA::SetRequest(DMA::Channel::GPU, true);
  DMA::SetRequest(DMA::Channel::SPU, true);
  DMA::WriteRegister(0x70, 0x00080800);

  // A chopped manual transfer is delayed by halting DMA. When the halt ends, the GPU runs first and halts again, which
  // must not keep the manual transfer waiting until the linked list ends (CDROM reads in Tondemo Crisis).
  DMA::WriteRegister(0x40, node_address + 0x1000);
  DMA::WriteRegister(0x44, 16);
  DMA::WriteRegister(0x48, 0x01100101);
  EXPECT_NE(DMA::ReadRegister(0x48) & (1u << 24), 0u);

  Program prog;
  prog.LoadImmediate(Reg::T0, 128);
  prog.Emit(addiu(Reg::T0, Reg::T0, -1));
  prog.Emit(bne(Reg::T0, Reg::R0, -8));
  prog.Emit(nop());
  prog.Run();

  EXPECT_NE(DMA::ReadRegister(0x28) & (1u << 24), 0u);
  EXPECT_EQ(DMA::ReadRegister(0x48) & (1u << 24), 0u);

  DMA::Reset();
  SPU::Reset();
  g_settings = old_settings;
}

TEST_P(PsxDiag, DoubleLoadDelayAcrossBlocks)
{
  // A second load to the same register discards the first one's value, even when the first one is in a branch delay
  // slot and the second one starts the next block.
  for (const bool taken : {false, true})
  {
    Program prog;
    prog.LoadImmediate(Reg::T0, RAM_TEST_ADDR);
    prog.LoadImmediate(Reg::T2, 0xAAAAAAAAu);
    prog.Emit(sw(Reg::T2, 0, Reg::T0));
    prog.LoadImmediate(Reg::T2, 0xBBBBBBBBu);
    prog.Emit(sw(Reg::T2, 4, Reg::T0));
    prog.LoadImmediate(Reg::T1, 0x12345678u);
    prog.LoadImmediate(Reg::A2, taken ? 0x12345678u : 0u);

    // Either way ends up at the instruction after the delay slot.
    prog.Emit(beq(Reg::A2, Reg::T1, 4));
    prog.Emit(lw(Reg::T1, 0, Reg::T0));
    prog.Emit(lw(Reg::T1, 4, Reg::T0));
    prog.Emit(addu(Reg::A0, Reg::T1, Reg::R0));
    prog.Emit(addu(Reg::A1, Reg::T1, Reg::R0));
    const u32 before = prog.StoreResult(Reg::A0);
    const u32 after = prog.StoreResult(Reg::A1);
    prog.Run();

    EXPECT_EQ(prog.Result(before), 0x12345678u) << (taken ? "taken" : "not taken");
    EXPECT_EQ(prog.Result(after), 0xBBBBBBBBu) << (taken ? "taken" : "not taken");
  }
}

INSTANTIATE_TEST_SUITE_P(ExecutionModes, PsxDiag,
                         ::testing::Values(CPUExecutionMode::Interpreter, CPUExecutionMode::CachedInterpreter,
                                           CPUExecutionMode::Recompiler),
                         [](const ::testing::TestParamInfo<CPUExecutionMode>& info) {
                           return std::string(Settings::GetCPUExecutionModeName(info.param));
                         });
