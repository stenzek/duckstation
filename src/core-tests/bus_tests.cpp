// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "cpu_test_harness.h"

#include "core/cpu_code_cache.h"
#include "core/cpu_core_private.h"
#include "core/save_state_version.h"
#include "core/system.h"

#include "util/state_wrapper.h"

#include "common/error.h"

#include <array>

namespace {

struct BusMemory : public testing::Test
{
protected:
  void SetUp() override
  {
    m_old_settings = g_settings;
    g_settings = Settings{};
    g_settings.cpu_execution_mode = CPUExecutionMode::Interpreter;
    g_settings.cpu_fastmem_mode = CPUFastmemMode::Disabled;

    Error error;
    ASSERT_TRUE(CPU::CodeCache::ProcessStartup(&error)) << error.GetDescription();
    ASSERT_TRUE(Bus::AllocateMemory(false, &error)) << error.GetDescription();
    Bus::Initialize();
    CPU::CodeCache::Initialize();
    CPU::Initialize();
    Bus::Reset();
    CPU::Reset();
  }

  void TearDown() override
  {
    CPU::CodeCache::Shutdown();
    CPU::Shutdown();
    Bus::Shutdown();
    Bus::ReleaseMemory();
    CPU::CodeCache::ProcessShutdown();
    g_settings = m_old_settings;
  }

  static void SetWindow(u32 value)
  {
    // Use KSEG1 so the register remains accessible with cache isolation enabled.
    CPU::GetMemoryWriteHandler(0xBF801060, MemoryAccessSize::Word)(0xBF801060, value);
  }

  static u32 ReadWord(u32 address) { return CPU::GetMemoryReadHandler(address, MemoryAccessSize::Word)(address); }

  static void WriteWord(u32 address, u32 value)
  {
    CPU::GetMemoryWriteHandler(address, MemoryAccessSize::Word)(address, value);
  }

  static bool SaveBus(std::vector<u8>* data)
  {
    const CPURAMSize size = Settings::GetCPURAMSizeForBytes(g_bus.ram_size).value();
    data->resize(System::GetMaxSaveStateSize(size, false));
    StateWrapper sw(std::span<u8>(*data), StateWrapper::Mode::Write, SAVE_STATE_VERSION);
    const bool result = Bus::DoState(sw);
    data->resize(sw.GetPosition());
    return result;
  }

  Settings m_old_settings;
};

} // namespace

TEST_F(BusMemory, DRAMWindowsMapRAMAndRejectUnmappedAccesses)
{
  // DRAM_CTRL bits 8..11, from the bank sizes in the hardware reference. Include asymmetric banks.
  static constexpr std::array<u32, 16> window_sizes_mb = {1, 1, 4, 4, 2, 5, 5, 8, 2, 2, 8, 8, 4, 10, 10, 16};
  for (const CPUFastmemMode mode : CPUTestHarness::FASTMEM_MODES)
  {
    SCOPED_TRACE(Settings::GetCPUFastmemModeName(mode));
    for (const CPURAMSize size :
         {CPURAMSize::Default, CPURAMSize::Expanded4MB, CPURAMSize::Expanded8MB, CPURAMSize::Expanded16MB})
    {
      SCOPED_TRACE(Settings::GetCPURAMSizeName(size));
      CPUTestHarness::ResetMemory(size, mode);
      for (u32 window = 0; window < window_sizes_mb.size(); window++)
      {
        SCOPED_TRACE(window);
        SetWindow((window << 8) | 0x88);
        const u32 mapped_size = window_sizes_mb[window] * 1048576;
        ASSERT_EQ(g_bus.ram_mapped_size, mapped_size);

        for (const u32 segment : {0u, 0x80000000u, 0xA0000000u})
        {
          SCOPED_TRACE(segment);
          for (u32 offset = 0; offset < mapped_size; offset += 1048576)
          {
            const u32 address = segment | (offset + 0x20000);
            SCOPED_TRACE(address);
            const u32 value = 0x12340000 | (offset / 1048576);
            CPU::g_state.bus_error = false;
            WriteWord(address, value);
            EXPECT_FALSE(CPU::g_state.bus_error);
            EXPECT_EQ(ReadWord(address), value);
            EXPECT_FALSE(CPU::g_state.bus_error);
            EXPECT_EQ(CPUTestHarness::Program::ReadRAM(address), value);
          }

          // Check the last mapped word and both kinds of access immediately outside the window.
          WriteWord(segment | (mapped_size - sizeof(u32)), 0xABCDEF01);
          EXPECT_EQ(ReadWord(segment | (mapped_size - sizeof(u32))), 0xABCDEF01u);
          CPU::g_state.bus_error = false;
          ReadWord(segment | mapped_size);
          EXPECT_TRUE(CPU::g_state.bus_error);
          CPU::g_state.bus_error = false;
          WriteWord(segment | mapped_size, 0);
          EXPECT_TRUE(CPU::g_state.bus_error);
          CPU::g_state.bus_error = false;
        }
      }
    }
  }
}

TEST_F(BusMemory, CacheIsolationPreservesKSEG1RAMWindow)
{
  for (const CPUFastmemMode mode : CPUTestHarness::FASTMEM_MODES)
  {
    SCOPED_TRACE(Settings::GetCPUFastmemModeName(mode));
    for (const CPURAMSize size :
         {CPURAMSize::Default, CPURAMSize::Expanded4MB, CPURAMSize::Expanded8MB, CPURAMSize::Expanded16MB})
    {
      SCOPED_TRACE(Settings::GetCPURAMSizeName(size));
      CPUTestHarness::ResetMemory(size, mode);
      CPU::g_state.cop0_regs.sr.Isc = true;
      CPU::UpdateMemoryPointers();
      for (u32 window = 0; window < 16; window++)
      {
        SCOPED_TRACE(window);
        SetWindow((window << 8) | 0x88);
        CPU::g_state.bus_error = false;
        WriteWord(0xA0020000, 0x12345678);
        EXPECT_EQ(ReadWord(0xA0020000), 0x12345678u);
        EXPECT_FALSE(CPU::g_state.bus_error);
        ReadWord(0xA0000000 | g_bus.ram_mapped_size);
        EXPECT_TRUE(CPU::g_state.bus_error);
        CPU::g_state.bus_error = false;
      }
      CPU::g_state.cop0_regs.sr.Isc = false;
      CPU::UpdateMemoryPointers();
    }
  }
}

TEST_F(BusMemory, MemoryRegionsRecognizeRAMMirrors)
{
  for (const CPURAMSize size : {CPURAMSize::Default, CPURAMSize::Expanded4MB, CPURAMSize::Expanded8MB})
  {
    SCOPED_TRACE(Settings::GetCPURAMSizeName(size));
    CPUTestHarness::ResetMemory(size, CPUFastmemMode::Disabled);
    for (u32 address = 0x10000; address < g_bus.ram_mapped_size; address += 0x200000)
    {
      SCOPED_TRACE(address);
      EXPECT_TRUE(Bus::GetMemoryRegionForAddress(address).has_value());
    }
  }
}

TEST_F(BusMemory, SearchMemoryFromRAMMirror)
{
  static constexpr std::array<u8, 4> pattern = {0x8A, 0xE3, 0x5F, 0xC2};
  static constexpr std::array<u8, 16> absent_pattern = {0x8A, 0xE3, 0x5F, 0xC2, 0x77, 0x91, 0x43, 0xB6,
                                                        0xEF, 0x12, 0x39, 0xA5, 0xDC, 0x68, 0x04, 0xF1};
  WriteWord(0x1000, 0xC25FE38A);

  // Mirrors search from the unmirrored address, and must not read past the end of RAM.
  EXPECT_EQ(Bus::SearchMemory(0x600000, pattern.data(), nullptr, static_cast<u32>(pattern.size())), 0x1000u);
  EXPECT_FALSE(Bus::SearchMemory(0x601004, absent_pattern.data(), nullptr, static_cast<u32>(absent_pattern.size()))
                 .has_value());
}

TEST_F(BusMemory, StateRestoresInstalledRAMAndWindowIndependentlyOfSettings)
{
  // This covers Bus serialization. System's file-save buffer allocation additionally requires a running VM.
  for (const CPUFastmemMode mode : CPUTestHarness::FASTMEM_MODES)
  {
    SCOPED_TRACE(Settings::GetCPUFastmemModeName(mode));
    for (const CPURAMSize size :
         {CPURAMSize::Default, CPURAMSize::Expanded4MB, CPURAMSize::Expanded8MB, CPURAMSize::Expanded16MB})
    {
      SCOPED_TRACE(Settings::GetCPURAMSizeName(size));
      CPUTestHarness::ResetMemory(size, mode);
      SetWindow(0xF88);
      const u32 ram_size = g_bus.ram_size;
      WriteWord(0x10000, 0x11223344);
      WriteWord(ram_size - sizeof(u32), 0x55667788);
      std::vector<u8> data;
      ASSERT_TRUE(SaveBus(&data));

      CPUTestHarness::ResetMemory(CPURAMSize::Default, mode);
      StateWrapper sw(std::span<const u8>(data), StateWrapper::Mode::Read, SAVE_STATE_VERSION);
      ASSERT_TRUE(Bus::DoState(sw));
      EXPECT_EQ(sw.GetPosition(), data.size());
      EXPECT_EQ(g_settings.cpu_ram_size, CPURAMSize::Default);
      EXPECT_EQ(g_bus.ram_size, ram_size);
      EXPECT_EQ(g_bus.ram_mask, ram_size - 1);
      EXPECT_EQ(g_bus.ram_mapped_size, 16u * 1048576);
      EXPECT_EQ(ReadWord(0x10000), 0x11223344u);
      EXPECT_EQ(ReadWord(ram_size - sizeof(u32)), 0x55667788u);

      // A subsequent Bus serialization must use the loaded machine size, which may differ from the setting.
      std::vector<u8> saved_again;
      ASSERT_TRUE(SaveBus(&saved_again));
      EXPECT_EQ(saved_again, data);
    }
  }
}

TEST_F(BusMemory, COP0ExecutionBreakpointRangeFollowsInstalledRAM)
{
  // The interpreter always uses the debug dispatcher before the game starts.
  g_settings.cpu_execution_mode = CPUExecutionMode::CachedInterpreter;
  const auto uses_debug_dispatcher_for_breakpoint = [](u32 bpc) {
    CPU::g_state.cop0_regs.BPC = bpc;
    CPU::g_state.cop0_regs.BPCM = 0xFFFFFFFFu;
    CPU::g_state.cop0_regs.dcic.debug_enable = true;
    CPU::g_state.cop0_regs.dcic.trap_enable = true;
    CPU::g_state.cop0_regs.dcic.kernel_debug_enable = true;
    CPU::g_state.cop0_regs.dcic.execution_breakpoint_enable = true;
    CPU::UpdateDebugDispatcherFlag();
    return CPU::g_state.using_debug_dispatcher;
  };

  // 2MB RAM keeps the original 8MB range.
  CPUTestHarness::ResetMemory(CPURAMSize::Default, CPUFastmemMode::Disabled);
  EXPECT_TRUE(uses_debug_dispatcher_for_breakpoint(0x80010000));
  EXPECT_FALSE(uses_debug_dispatcher_for_breakpoint(0x80800000));

  // Keep non-code addresses filtered rather than forcing normal execution into the debug interpreter.
  for (const u32 bpc : {0x81000000u, 0xBF800000u, 0xBF801000u})
    EXPECT_FALSE(uses_debug_dispatcher_for_breakpoint(bpc)) << std::hex << bpc;
#if 0 // EXP1 check is disabled in IsCop0ExecutionBreakpointUnmasked().
  EXPECT_TRUE(uses_debug_dispatcher_for_breakpoint(0xBF000000u));
#endif

  // Loading expanded RAM from a state must update the flag, since CPU state is restored first.
  CPUTestHarness::ResetMemory(CPURAMSize::Expanded16MB, CPUFastmemMode::Disabled);
  std::vector<u8> data;
  ASSERT_TRUE(SaveBus(&data));
  CPUTestHarness::ResetMemory(CPURAMSize::Default, CPUFastmemMode::Disabled);
  ASSERT_FALSE(uses_debug_dispatcher_for_breakpoint(0x80800000));
  StateWrapper sw(std::span<const u8>(data), StateWrapper::Mode::Read, SAVE_STATE_VERSION);
  ASSERT_TRUE(Bus::DoState(sw));
  EXPECT_TRUE(CPU::g_state.using_debug_dispatcher);
}

TEST_F(BusMemory, ForceFullRAMMapping)
{
  static constexpr u32 one_mb = 1048576;
  for (const CPUFastmemMode mode : CPUTestHarness::FASTMEM_MODES)
  {
    SCOPED_TRACE(Settings::GetCPUFastmemModeName(mode));

    // The retail BIOS window only covers 8MB of 16MB.
    g_settings.cpu_force_full_ram_mapping = false;
    CPUTestHarness::ResetMemory(CPURAMSize::Expanded16MB, mode);
    ASSERT_EQ(g_bus.ram_mapped_size, 8 * one_mb);
    CPU::g_state.bus_error = false;
    ReadWord(0x80900000);
    EXPECT_TRUE(CPU::g_state.bus_error);

    // Enabling maps the remainder without changing the register.
    g_settings.cpu_force_full_ram_mapping = true;
    Bus::UpdateMappedRAMSize();
    EXPECT_EQ(g_bus.ram_mapped_size, 16 * one_mb);
    EXPECT_EQ(ReadWord(0xBF801060), 0xB88u);
    for (const u32 segment : {0u, 0x80000000u, 0xA0000000u})
    {
      SCOPED_TRACE(segment);
      CPU::g_state.bus_error = false;
      WriteWord(segment | 0x900000, 0x12345678 + segment);
      EXPECT_EQ(ReadWord(segment | 0x900000), 0x12345678 + segment);
      EXPECT_EQ(ReadWord(segment | (16 * one_mb - sizeof(u32))), 0u);
      EXPECT_FALSE(CPU::g_state.bus_error);
    }

    // Applies on reset, and still honours a larger window than installed.
    Bus::Reset();
    EXPECT_EQ(g_bus.ram_mapped_size, 16 * one_mb);
    CPUTestHarness::ResetMemory(CPURAMSize::Default, mode);
    EXPECT_EQ(g_bus.ram_mapped_size, 8 * one_mb);
    SetWindow(0x888);
    EXPECT_EQ(g_bus.ram_mapped_size, 2 * one_mb);
    CPUTestHarness::ResetMemory(CPURAMSize::Expanded8MB, mode);
    SetWindow(0x888);
    EXPECT_EQ(g_bus.ram_mapped_size, 8 * one_mb);

    // Disabling restores the game's window.
    g_settings.cpu_force_full_ram_mapping = false;
    Bus::UpdateMappedRAMSize();
    EXPECT_EQ(g_bus.ram_mapped_size, 2 * one_mb);
    CPU::g_state.bus_error = false;
    ReadWord(0x80200000);
    EXPECT_TRUE(CPU::g_state.bus_error);

    // Loading a state with a different RAM size must recompute the mapping, even if the register is unchanged.
    g_settings.cpu_force_full_ram_mapping = true;
    CPUTestHarness::ResetMemory(CPURAMSize::Expanded16MB, mode);
    std::vector<u8> data;
    ASSERT_TRUE(SaveBus(&data));
    CPUTestHarness::ResetMemory(CPURAMSize::Default, mode);
    ASSERT_EQ(g_bus.ram_mapped_size, 8 * one_mb);
    StateWrapper sw(std::span<const u8>(data), StateWrapper::Mode::Read, SAVE_STATE_VERSION);
    ASSERT_TRUE(Bus::DoState(sw));
    EXPECT_EQ(g_bus.ram_mapped_size, 16 * one_mb);
    CPU::g_state.bus_error = false;
    WriteWord(0x80900000, 0xCAFEF00D);
    EXPECT_EQ(ReadWord(0x80900000), 0xCAFEF00Du);
    EXPECT_FALSE(CPU::g_state.bus_error);

    g_settings.cpu_force_full_ram_mapping = false;
  }
}

TEST_F(BusMemory, ResetRestoresDefaultWindow)
{
  for (const CPUFastmemMode mode : CPUTestHarness::FASTMEM_MODES)
  {
    SCOPED_TRACE(Settings::GetCPUFastmemModeName(mode));
    CPUTestHarness::ResetMemory(CPURAMSize::Default, mode);
    SetWindow(0x088);
    ASSERT_EQ(g_bus.ram_mapped_size, 1048576u);
    Bus::Reset();
    EXPECT_EQ(g_bus.ram_mapped_size, 8u * 1048576);
    CPU::g_state.bus_error = false;
    WriteWord(0x80610000, 0x12345678);
    EXPECT_EQ(ReadWord(0x80010000), 0x12345678u);
    EXPECT_FALSE(CPU::g_state.bus_error);
  }
}

TEST_F(BusMemory, ResetRestoresWindowAfterReallocatingMemoryWithoutVM)
{
  for (const CPUFastmemMode mode : CPUTestHarness::FASTMEM_MODES)
  {
    SCOPED_TRACE(Settings::GetCPUFastmemModeName(mode));
    CPUTestHarness::ResetMemory(CPURAMSize::Default, mode);
    ASSERT_EQ(g_bus.ram_mapped_size, 8u * 1048576);
    ASSERT_FALSE(System::IsValid());
    Bus::Shutdown();
    Error error;
    ASSERT_TRUE(Bus::ReallocateMemoryMap(false, &error)) << error.GetDescription();
    Bus::Initialize();
    Bus::Reset();
    EXPECT_EQ(g_bus.ram_mapped_size, 8u * 1048576);
    CPU::g_state.bus_error = false;
    WriteWord(0x80610000, 0x12345678);
    EXPECT_EQ(ReadWord(0x80010000), 0x12345678u);
    EXPECT_FALSE(CPU::g_state.bus_error);
  }
}
