// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "bus.h"
#include "cdrom.h"
#include "cpu_code_cache.h"
#include "cpu_core.h"
#include "cpu_core_private.h"
#include "cpu_disasm.h"
#include "dma.h"
#include "gpu.h"
#include "host.h"
#include "interrupt_controller.h"
#include "mdec.h"
#include "pad.h"
#include "settings.h"
#include "sio.h"
#include "spu.h"
#include "system.h"
#include "timers.h"
#include "timing_event.h"

#include "util/state_wrapper.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/error.h"
#include "common/intrin.h"
#include "common/log.h"
#include "common/memmap.h"

#include <cstdio>
#include <tuple>
#include <utility>

Log_SetChannel(Bus);

// TODO: Get rid of page code bits, instead use page faults to track SMC.

// Exports for external debugger access
#ifndef __ANDROID__
namespace Exports {

extern "C" {
#ifdef _WIN32
_declspec(dllexport) uintptr_t RAM;
_declspec(dllexport) u32 RAM_SIZE, RAM_MASK;
#else
__attribute__((visibility("default"), used)) uintptr_t RAM;
__attribute__((visibility("default"), used)) u32 RAM_SIZE, RAM_MASK;
#endif
}

} // namespace Exports
#endif

namespace Bus {

namespace {
union MEMDELAY
{
  u32 bits;

  BitField<u32, u8, 4, 4> access_time; // cycles
  BitField<u32, bool, 8, 1> use_com0_time;
  BitField<u32, bool, 9, 1> use_com1_time;
  BitField<u32, bool, 10, 1> use_com2_time;
  BitField<u32, bool, 11, 1> use_com3_time;
  BitField<u32, bool, 12, 1> data_bus_16bit;
  BitField<u32, u8, 16, 5> memory_window_size;

  static constexpr u32 WRITE_MASK = 0b10101111'00011111'11111111'11111111;
};

union COMDELAY
{
  u32 bits;

  BitField<u32, u8, 0, 4> com0;
  BitField<u32, u8, 4, 4> com1;
  BitField<u32, u8, 8, 4> com2;
  BitField<u32, u8, 12, 4> com3;
  BitField<u32, u8, 16, 2> comunk;

  static constexpr u32 WRITE_MASK = 0b00000000'00000011'11111111'11111111;
};

union MEMCTRL
{
  u32 regs[MEMCTRL_REG_COUNT];

  struct
  {
    u32 exp1_base;
    u32 exp2_base;
    MEMDELAY exp1_delay_size;
    MEMDELAY exp3_delay_size;
    MEMDELAY bios_delay_size;
    MEMDELAY spu_delay_size;
    MEMDELAY cdrom_delay_size;
    MEMDELAY exp2_delay_size;
    COMDELAY common_delay;
  };
};
} // namespace

static void* s_shmem_handle = nullptr;

std::bitset<RAM_8MB_CODE_PAGE_COUNT> g_ram_code_bits{};
u8* g_ram = nullptr;
u8* g_unprotected_ram = nullptr;
u32 g_ram_size = 0;
u32 g_ram_mask = 0;
u8* g_bios = nullptr;
void** g_memory_handlers = nullptr;
void** g_memory_handlers_isc = nullptr;

std::array<TickCount, 3> g_exp1_access_time = {};
std::array<TickCount, 3> g_exp2_access_time = {};
std::array<TickCount, 3> g_bios_access_time = {};
std::array<TickCount, 3> g_cdrom_access_time = {};
std::array<TickCount, 3> g_spu_access_time = {};

static std::vector<u8> s_exp1_rom;

static MEMCTRL s_MEMCTRL = {};
static u32 s_ram_size_reg = 0;

static std::string s_tty_line_buffer;

static CPUFastmemMode s_fastmem_mode = CPUFastmemMode::Disabled;

#ifdef ENABLE_MMAP_FASTMEM
static SharedMemoryMappingArea s_fastmem_arena;
static std::vector<std::pair<u8*, size_t>> s_fastmem_ram_views;
#endif

static u8** s_fastmem_lut = nullptr;

static void SetRAMSize(bool enable_8mb_ram);

static std::tuple<TickCount, TickCount, TickCount> CalculateMemoryTiming(MEMDELAY mem_delay, COMDELAY common_delay);
static void RecalculateMemoryTimings();

static u8* GetLUTFastmemPointer(u32 address, u8* ram_ptr);

static void SetRAMPageWritable(u32 page_index, bool writable);

static void SetHandlers();

template<typename FP>
static FP* OffsetHandlerArray(void** handlers, MemoryAccessSize size, MemoryAccessType type);
} // namespace Bus

namespace MemoryMap {
static constexpr size_t RAM_OFFSET = 0;
static constexpr size_t RAM_SIZE = Bus::RAM_8MB_SIZE;
static constexpr size_t BIOS_OFFSET = RAM_OFFSET + RAM_SIZE;
static constexpr size_t BIOS_SIZE = Bus::BIOS_SIZE;
static constexpr size_t LUT_OFFSET = BIOS_OFFSET + BIOS_SIZE;
static constexpr size_t LUT_SIZE = (sizeof(void*) * Bus::MEMORY_LUT_SLOTS) * 2; // normal and isolated
static constexpr size_t TOTAL_SIZE = LUT_OFFSET + LUT_SIZE;
} // namespace MemoryMap

#define FIXUP_HALFWORD_OFFSET(size, offset) ((size >= MemoryAccessSize::HalfWord) ? (offset) : ((offset) & ~1u))
#define FIXUP_HALFWORD_READ_VALUE(size, offset, value)                                                                 \
  ((size >= MemoryAccessSize::HalfWord) ? (value) : ((value) >> (((offset) & u32(1)) * 8u)))
#define FIXUP_HALFWORD_WRITE_VALUE(size, offset, value)                                                                \
  ((size >= MemoryAccessSize::HalfWord) ? (value) : ((value) << (((offset) & u32(1)) * 8u)))

#define FIXUP_WORD_OFFSET(size, offset) ((size == MemoryAccessSize::Word) ? (offset) : ((offset) & ~3u))
#define FIXUP_WORD_READ_VALUE(size, offset, value)                                                                     \
  ((size == MemoryAccessSize::Word) ? (value) : ((value) >> (((offset) & 3u) * 8)))
#define FIXUP_WORD_WRITE_VALUE(size, offset, value)                                                                    \
  ((size == MemoryAccessSize::Word) ? (value) : ((value) << (((offset) & 3u) * 8)))

bool Bus::AllocateMemory()
{
  Error error;
  s_shmem_handle =
    MemMap::CreateSharedMemory(MemMap::GetFileMappingName("duckstation").c_str(), MemoryMap::TOTAL_SIZE, &error);
  if (!s_shmem_handle)
  {
#ifndef __linux__
    error.AddSuffix("\nYou may need to close some programs to free up additional memory.");
#else
    error.AddSuffix(
      "\nYou may need to close some programs to free up additional memory, or increase the size of /dev/shm.");
#endif

    Host::ReportFatalError("Memory Allocation Failed", error.GetDescription());
    return false;
  }

  g_ram = static_cast<u8*>(MemMap::MapSharedMemory(s_shmem_handle, MemoryMap::RAM_OFFSET, nullptr, MemoryMap::RAM_SIZE,
                                                   PageProtect::ReadWrite));
  g_unprotected_ram = static_cast<u8*>(MemMap::MapSharedMemory(s_shmem_handle, MemoryMap::RAM_OFFSET, nullptr,
                                                               MemoryMap::RAM_SIZE, PageProtect::ReadWrite));
  if (!g_ram || !g_unprotected_ram)
  {
    Host::ReportFatalError("Memory Allocation Failed", "Failed to map memory for RAM");
    ReleaseMemory();
    return false;
  }

  Log_VerboseFmt("RAM is mapped at {}.", static_cast<void*>(g_ram));

  g_bios = static_cast<u8*>(MemMap::MapSharedMemory(s_shmem_handle, MemoryMap::BIOS_OFFSET, nullptr,
                                                    MemoryMap::BIOS_SIZE, PageProtect::ReadWrite));
  if (!g_bios)
  {
    Host::ReportFatalError("Memory Allocation Failed", "Failed to map memory for BIOS");
    ReleaseMemory();
    return false;
  }

  Log_VerboseFmt("BIOS is mapped at {}.", static_cast<void*>(g_bios));

  g_memory_handlers = static_cast<void**>(MemMap::MapSharedMemory(s_shmem_handle, MemoryMap::LUT_OFFSET, nullptr,
                                                                  MemoryMap::LUT_SIZE, PageProtect::ReadWrite));
  if (!g_memory_handlers)
  {
    Host::ReportFatalError("Memory Allocation Failed", "Failed to map memory for LUTs");
    ReleaseMemory();
    return false;
  }

  Log_VerboseFmt("LUTs are mapped at {}.", static_cast<void*>(g_memory_handlers));
  g_memory_handlers_isc = g_memory_handlers + MEMORY_LUT_SLOTS;
  SetHandlers();

#ifdef ENABLE_MMAP_FASTMEM
  if (!s_fastmem_arena.Create(FASTMEM_ARENA_SIZE))
  {
    // TODO: maybe make this non-fatal?
    Host::ReportFatalError("Memory Allocation Failed", "Failed to create fastmem arena");
    ReleaseMemory();
    return false;
  }

  Log_InfoPrintf("Fastmem base: %p", s_fastmem_arena.BasePointer());
#endif

#ifndef __ANDROID__
  Exports::RAM = reinterpret_cast<uintptr_t>(g_unprotected_ram);
#endif

  return true;
}

void Bus::ReleaseMemory()
{
#ifndef __ANDROID__
  Exports::RAM = 0;
  Exports::RAM_SIZE = 0;
  Exports::RAM_MASK = 0;
#endif

#ifdef ENABLE_MMAP_FASTMEM
  DebugAssert(s_fastmem_ram_views.empty());
  s_fastmem_arena.Destroy();
#endif

  std::free(s_fastmem_lut);
  s_fastmem_lut = nullptr;

  g_memory_handlers_isc = nullptr;
  if (g_memory_handlers)
  {
    MemMap::UnmapSharedMemory(g_memory_handlers, MemoryMap::LUT_SIZE);
    g_memory_handlers = nullptr;
  }

  if (g_bios)
  {
    MemMap::UnmapSharedMemory(g_bios, MemoryMap::BIOS_SIZE);
    g_bios = nullptr;
  }

  if (g_unprotected_ram)
  {
    MemMap::UnmapSharedMemory(g_unprotected_ram, MemoryMap::RAM_SIZE);
    g_unprotected_ram = nullptr;
  }

  if (g_ram)
  {
    MemMap::UnmapSharedMemory(g_ram, MemoryMap::RAM_SIZE);
    g_ram = nullptr;
  }

  if (s_shmem_handle)
  {
    MemMap::DestroySharedMemory(s_shmem_handle);
    s_shmem_handle = nullptr;
  }
}

bool Bus::Initialize()
{
  SetRAMSize(g_settings.enable_8mb_ram);
  Reset();
  return true;
}

void Bus::SetRAMSize(bool enable_8mb_ram)
{
  g_ram_size = enable_8mb_ram ? RAM_8MB_SIZE : RAM_2MB_SIZE;
  g_ram_mask = enable_8mb_ram ? RAM_8MB_MASK : RAM_2MB_MASK;

#ifndef __ANDROID__
  Exports::RAM_SIZE = g_ram_size;
  Exports::RAM_MASK = g_ram_mask;
#endif
}

void Bus::Shutdown()
{
  UpdateFastmemViews(CPUFastmemMode::Disabled);
  CPU::g_state.fastmem_base = nullptr;

  g_ram_mask = 0;
  g_ram_size = 0;

#ifndef __ANDROID__
  Exports::RAM = 0;
  Exports::RAM_SIZE = 0;
  Exports::RAM_MASK = 0;
#endif
}

void Bus::Reset()
{
  std::memset(g_ram, 0, g_ram_size);
  s_MEMCTRL.exp1_base = 0x1F000000;
  s_MEMCTRL.exp2_base = 0x1F802000;
  s_MEMCTRL.exp1_delay_size.bits = 0x0013243F;
  s_MEMCTRL.exp3_delay_size.bits = 0x00003022;
  s_MEMCTRL.bios_delay_size.bits = 0x0013243F;
  s_MEMCTRL.spu_delay_size.bits = 0x200931E1;
  s_MEMCTRL.cdrom_delay_size.bits = 0x00020843;
  s_MEMCTRL.exp2_delay_size.bits = 0x00070777;
  s_MEMCTRL.common_delay.bits = 0x00031125;
  s_ram_size_reg = UINT32_C(0x00000B88);
  g_ram_code_bits = {};
  RecalculateMemoryTimings();
}

void Bus::AddTTYCharacter(char ch)
{
  if (ch == '\r')
  {
  }
  else if (ch == '\n')
  {
    if (!s_tty_line_buffer.empty())
    {
      Log::Writef("TTY", "", LOGLEVEL_INFO, "\033[1;34m%s\033[0m", s_tty_line_buffer.c_str());
#ifdef _DEBUG
      if (CPU::IsTraceEnabled())
        CPU::WriteToExecutionLog("TTY: %s\n", s_tty_line_buffer.c_str());
#endif
    }
    s_tty_line_buffer.clear();
  }
  else
  {
    s_tty_line_buffer += ch;
  }
}

void Bus::AddTTYString(const std::string_view& str)
{
  for (char ch : str)
    AddTTYCharacter(ch);
}

bool Bus::DoState(StateWrapper& sw)
{
  u32 ram_size = g_ram_size;
  sw.DoEx(&ram_size, 52, static_cast<u32>(RAM_2MB_SIZE));
  if (ram_size != g_ram_size)
  {
    const bool using_8mb_ram = (ram_size == RAM_8MB_SIZE);
    SetRAMSize(using_8mb_ram);
    UpdateFastmemViews(s_fastmem_mode);
    CPU::UpdateMemoryPointers();
  }

  sw.Do(&g_exp1_access_time);
  sw.Do(&g_exp2_access_time);
  sw.Do(&g_bios_access_time);
  sw.Do(&g_cdrom_access_time);
  sw.Do(&g_spu_access_time);
  sw.DoBytes(g_ram, g_ram_size);

  if (sw.GetVersion() < 58)
  {
    Log_WarningPrint("Overwriting loaded BIOS with old save state.");
    sw.DoBytes(g_bios, BIOS_SIZE);
  }

  sw.DoArray(s_MEMCTRL.regs, countof(s_MEMCTRL.regs));
  sw.Do(&s_ram_size_reg);
  sw.Do(&s_tty_line_buffer);
  return !sw.HasError();
}

void Bus::SetExpansionROM(std::vector<u8> data)
{
  s_exp1_rom = std::move(data);
}

std::tuple<TickCount, TickCount, TickCount> Bus::CalculateMemoryTiming(MEMDELAY mem_delay, COMDELAY common_delay)
{
  // from nocash spec
  s32 first = 0, seq = 0, min = 0;
  if (mem_delay.use_com0_time)
  {
    first += s32(common_delay.com0) - 1;
    seq += s32(common_delay.com0) - 1;
  }
  if (mem_delay.use_com2_time)
  {
    first += s32(common_delay.com2);
    seq += s32(common_delay.com2);
  }
  if (mem_delay.use_com3_time)
  {
    min = s32(common_delay.com3);
  }
  if (first < 6)
    first++;

  first = first + s32(mem_delay.access_time) + 2;
  seq = seq + s32(mem_delay.access_time) + 2;

  if (first < (min + 6))
    first = min + 6;
  if (seq < (min + 2))
    seq = min + 2;

  const TickCount byte_access_time = first;
  const TickCount halfword_access_time = mem_delay.data_bus_16bit ? first : (first + seq);
  const TickCount word_access_time = mem_delay.data_bus_16bit ? (first + seq) : (first + seq + seq + seq);
  return std::tie(std::max(byte_access_time - 1, 0), std::max(halfword_access_time - 1, 0),
                  std::max(word_access_time - 1, 0));
}

void Bus::RecalculateMemoryTimings()
{
  std::tie(g_bios_access_time[0], g_bios_access_time[1], g_bios_access_time[2]) =
    CalculateMemoryTiming(s_MEMCTRL.bios_delay_size, s_MEMCTRL.common_delay);
  std::tie(g_cdrom_access_time[0], g_cdrom_access_time[1], g_cdrom_access_time[2]) =
    CalculateMemoryTiming(s_MEMCTRL.cdrom_delay_size, s_MEMCTRL.common_delay);
  std::tie(g_spu_access_time[0], g_spu_access_time[1], g_spu_access_time[2]) =
    CalculateMemoryTiming(s_MEMCTRL.spu_delay_size, s_MEMCTRL.common_delay);

  Log_TracePrintf("BIOS Memory Timing: %u bit bus, byte=%d, halfword=%d, word=%d",
                  s_MEMCTRL.bios_delay_size.data_bus_16bit ? 16 : 8, g_bios_access_time[0] + 1,
                  g_bios_access_time[1] + 1, g_bios_access_time[2] + 1);
  Log_TracePrintf("CDROM Memory Timing: %u bit bus, byte=%d, halfword=%d, word=%d",
                  s_MEMCTRL.cdrom_delay_size.data_bus_16bit ? 16 : 8, g_cdrom_access_time[0] + 1,
                  g_cdrom_access_time[1] + 1, g_cdrom_access_time[2] + 1);
  Log_TracePrintf("SPU Memory Timing: %u bit bus, byte=%d, halfword=%d, word=%d",
                  s_MEMCTRL.spu_delay_size.data_bus_16bit ? 16 : 8, g_spu_access_time[0] + 1, g_spu_access_time[1] + 1,
                  g_spu_access_time[2] + 1);
}

CPUFastmemMode Bus::GetFastmemMode()
{
  return s_fastmem_mode;
}

void* Bus::GetFastmemBase(bool isc)
{
#ifdef ENABLE_MMAP_FASTMEM
  if (s_fastmem_mode == CPUFastmemMode::MMap)
    return isc ? nullptr : s_fastmem_arena.BasePointer();
#endif
  if (s_fastmem_mode == CPUFastmemMode::LUT)
    return reinterpret_cast<u8*>(s_fastmem_lut + (isc ? (FASTMEM_LUT_SIZE * sizeof(void*)) : 0));

  return nullptr;
}

u8* Bus::GetLUTFastmemPointer(u32 address, u8* ram_ptr)
{
  return ram_ptr - address;
}

void Bus::UpdateFastmemViews(CPUFastmemMode mode)
{
#ifndef ENABLE_MMAP_FASTMEM
  Assert(mode != CPUFastmemMode::MMap);
#else
  for (const auto& it : s_fastmem_ram_views)
    s_fastmem_arena.Unmap(it.first, it.second);
  s_fastmem_ram_views.clear();
#endif

  s_fastmem_mode = mode;
  if (mode == CPUFastmemMode::Disabled)
    return;

#ifdef ENABLE_MMAP_FASTMEM
  if (mode == CPUFastmemMode::MMap)
  {
    auto MapRAM = [](u32 base_address) {
      u8* map_address = s_fastmem_arena.BasePointer() + base_address;
      if (!s_fastmem_arena.Map(s_shmem_handle, 0, map_address, g_ram_size, PageProtect::ReadWrite))
      {
        Log_ErrorPrintf("Failed to map RAM at fastmem area %p (offset 0x%08X)", map_address, g_ram_size);
        return;
      }

      // mark all pages with code as non-writable
      for (u32 i = 0; i < static_cast<u32>(g_ram_code_bits.size()); i++)
      {
        if (g_ram_code_bits[i])
        {
          u8* page_address = map_address + (i * HOST_PAGE_SIZE);
          if (!MemMap::MemProtect(page_address, HOST_PAGE_SIZE, PageProtect::ReadOnly))
          {
            Log_ErrorPrintf("Failed to write-protect code page at %p", page_address);
            s_fastmem_arena.Unmap(map_address, g_ram_size);
            return;
          }
        }
      }

      s_fastmem_ram_views.emplace_back(map_address, g_ram_size);
    };

    // KUSEG - cached
    MapRAM(0x00000000);

    // KSEG0 - cached
    MapRAM(0x80000000);

    // KSEG1 - uncached
    MapRAM(0xA0000000);

    return;
  }
#endif

  if (!s_fastmem_lut)
  {
    s_fastmem_lut = static_cast<u8**>(std::malloc(sizeof(u8*) * FASTMEM_LUT_SLOTS));
    Assert(s_fastmem_lut);

    Log_InfoPrintf("Fastmem base (software): %p", s_fastmem_lut);
  }

  // This assumes the top 4KB of address space is not mapped. It shouldn't be on any sane OSes.
  for (u32 i = 0; i < FASTMEM_LUT_SLOTS; i++)
    s_fastmem_lut[i] = GetLUTFastmemPointer(i << FASTMEM_LUT_PAGE_SHIFT, nullptr);

  auto MapRAM = [](u32 base_address) {
    u8* ram_ptr = g_ram + (base_address & g_ram_mask);
    for (u32 address = 0; address < g_ram_size; address += FASTMEM_LUT_PAGE_SIZE)
    {
      const u32 lut_index = (base_address + address) >> FASTMEM_LUT_PAGE_SHIFT;
      s_fastmem_lut[lut_index] = GetLUTFastmemPointer(base_address + address, ram_ptr);
      ram_ptr += FASTMEM_LUT_PAGE_SIZE;
    }
  };

  // KUSEG - cached
  MapRAM(0x00000000);
  MapRAM(0x00200000);
  MapRAM(0x00400000);
  MapRAM(0x00600000);

  // KSEG0 - cached
  MapRAM(0x80000000);
  MapRAM(0x80200000);
  MapRAM(0x80400000);
  MapRAM(0x80600000);

  // KSEG1 - uncached
  MapRAM(0xA0000000);
  MapRAM(0xA0200000);
  MapRAM(0xA0400000);
  MapRAM(0xA0600000);
}

bool Bus::CanUseFastmemForAddress(VirtualMemoryAddress address)
{
  const PhysicalMemoryAddress paddr = address & CPU::PHYSICAL_MEMORY_ADDRESS_MASK;

  switch (s_fastmem_mode)
  {
#ifdef ENABLE_MMAP_FASTMEM
    case CPUFastmemMode::MMap:
    {
      // Currently since we don't map the mirrors, don't use fastmem for them.
      // This is because the swapping of page code bits for SMC is too expensive.
      return (paddr < g_ram_size);
    }
#endif

    case CPUFastmemMode::LUT:
      return (paddr < RAM_MIRROR_END);

    case CPUFastmemMode::Disabled:
    default:
      return false;
  }
}

bool Bus::IsRAMCodePage(u32 index)
{
  return g_ram_code_bits[index];
}

void Bus::SetRAMCodePage(u32 index)
{
  if (g_ram_code_bits[index])
    return;

  // protect fastmem pages
  g_ram_code_bits[index] = true;
  SetRAMPageWritable(index, false);
}

void Bus::ClearRAMCodePage(u32 index)
{
  if (!g_ram_code_bits[index])
    return;

  // unprotect fastmem pages
  g_ram_code_bits[index] = false;
  SetRAMPageWritable(index, true);
}

void Bus::SetRAMPageWritable(u32 page_index, bool writable)
{
  if (!MemMap::MemProtect(&g_ram[page_index * HOST_PAGE_SIZE], HOST_PAGE_SIZE,
                          writable ? PageProtect::ReadWrite : PageProtect::ReadOnly)) [[unlikely]]
  {
    Log_ErrorFmt("Failed to set RAM host page {} ({}) to {}", page_index,
                 reinterpret_cast<const void*>(&g_ram[page_index * HOST_PAGE_SIZE]),
                 writable ? "read-write" : "read-only");
  }

#ifdef ENABLE_MMAP_FASTMEM
  if (s_fastmem_mode == CPUFastmemMode::MMap)
  {
    const PageProtect protect = writable ? PageProtect::ReadWrite : PageProtect::ReadOnly;

    // unprotect fastmem pages
    for (const auto& it : s_fastmem_ram_views)
    {
      u8* page_address = it.first + (page_index * HOST_PAGE_SIZE);
      if (!MemMap::MemProtect(page_address, HOST_PAGE_SIZE, protect)) [[unlikely]]
      {
        Log_ErrorPrintf("Failed to %s code page %u (0x%08X) @ %p", writable ? "unprotect" : "protect", page_index,
                        page_index * static_cast<u32>(HOST_PAGE_SIZE), page_address);
      }
    }

    return;
  }
#endif
}

void Bus::ClearRAMCodePageFlags()
{
  g_ram_code_bits.reset();

  if (!MemMap::MemProtect(g_ram, RAM_8MB_SIZE, PageProtect::ReadWrite))
    Log_ErrorPrint("Failed to restore RAM protection to read-write.");

#ifdef ENABLE_MMAP_FASTMEM
  if (s_fastmem_mode == CPUFastmemMode::MMap)
  {
    // unprotect fastmem pages
    for (const auto& it : s_fastmem_ram_views)
    {
      if (!MemMap::MemProtect(it.first, it.second, PageProtect::ReadWrite))
      {
        Log_ErrorPrintf("Failed to unprotect code pages for fastmem view @ %p", it.first);
      }
    }
  }
#endif
}

bool Bus::IsCodePageAddress(PhysicalMemoryAddress address)
{
  return IsRAMAddress(address) ? g_ram_code_bits[(address & g_ram_mask) / HOST_PAGE_SIZE] : false;
}

bool Bus::HasCodePagesInRange(PhysicalMemoryAddress start_address, u32 size)
{
  if (!IsRAMAddress(start_address))
    return false;

  start_address = (start_address & g_ram_mask);

  const u32 end_address = start_address + size;
  while (start_address < end_address)
  {
    const u32 code_page_index = start_address / HOST_PAGE_SIZE;
    if (g_ram_code_bits[code_page_index])
      return true;

    start_address += HOST_PAGE_SIZE;
  }

  return false;
}

std::optional<Bus::MemoryRegion> Bus::GetMemoryRegionForAddress(PhysicalMemoryAddress address)
{
  if (address < RAM_2MB_SIZE)
    return MemoryRegion::RAM;
  else if (address < RAM_MIRROR_END)
    return static_cast<MemoryRegion>(static_cast<u32>(MemoryRegion::RAM) + (address / RAM_2MB_SIZE));
  else if (address >= EXP1_BASE && address < (EXP1_BASE + EXP1_SIZE))
    return MemoryRegion::EXP1;
  else if (address >= CPU::SCRATCHPAD_ADDR && address < (CPU::SCRATCHPAD_ADDR + CPU::SCRATCHPAD_SIZE))
    return MemoryRegion::Scratchpad;
  else if (address >= BIOS_BASE && address < (BIOS_BASE + BIOS_SIZE))
    return MemoryRegion::BIOS;

  return std::nullopt;
}

static constexpr std::array<std::pair<PhysicalMemoryAddress, PhysicalMemoryAddress>,
                            static_cast<u32>(Bus::MemoryRegion::Count)>
  s_code_region_ranges = {{
    {0, Bus::RAM_2MB_SIZE},
    {Bus::RAM_2MB_SIZE, Bus::RAM_2MB_SIZE * 2},
    {Bus::RAM_2MB_SIZE * 2, Bus::RAM_2MB_SIZE * 3},
    {Bus::RAM_2MB_SIZE * 3, Bus::RAM_MIRROR_END},
    {Bus::EXP1_BASE, Bus::EXP1_BASE + Bus::EXP1_SIZE},
    {CPU::SCRATCHPAD_ADDR, CPU::SCRATCHPAD_ADDR + CPU::SCRATCHPAD_SIZE},
    {Bus::BIOS_BASE, Bus::BIOS_BASE + Bus::BIOS_SIZE},
  }};

PhysicalMemoryAddress Bus::GetMemoryRegionStart(MemoryRegion region)
{
  return s_code_region_ranges[static_cast<u32>(region)].first;
}

PhysicalMemoryAddress Bus::GetMemoryRegionEnd(MemoryRegion region)
{
  return s_code_region_ranges[static_cast<u32>(region)].second;
}

u8* Bus::GetMemoryRegionPointer(MemoryRegion region)
{
  switch (region)
  {
    case MemoryRegion::RAM:
      return g_unprotected_ram;

    case MemoryRegion::RAMMirror1:
      return (g_unprotected_ram + (RAM_2MB_SIZE & g_ram_mask));

    case MemoryRegion::RAMMirror2:
      return (g_unprotected_ram + ((RAM_2MB_SIZE * 2) & g_ram_mask));

    case MemoryRegion::RAMMirror3:
      return (g_unprotected_ram + ((RAM_8MB_SIZE * 3) & g_ram_mask));

    case MemoryRegion::EXP1:
      return nullptr;

    case MemoryRegion::Scratchpad:
      return CPU::g_state.scratchpad.data();

    case MemoryRegion::BIOS:
      return g_bios;

    default:
      return nullptr;
  }
}

static ALWAYS_INLINE_RELEASE bool MaskedMemoryCompare(const u8* pattern, const u8* mask, u32 pattern_length,
                                                      const u8* mem)
{
  if (!mask)
    return std::memcmp(mem, pattern, pattern_length) == 0;

  for (u32 i = 0; i < pattern_length; i++)
  {
    if ((mem[i] & mask[i]) != (pattern[i] & mask[i]))
      return false;
  }

  return true;
}

std::optional<PhysicalMemoryAddress> Bus::SearchMemory(PhysicalMemoryAddress start_address, const u8* pattern,
                                                       const u8* mask, u32 pattern_length)
{
  std::optional<MemoryRegion> region = GetMemoryRegionForAddress(start_address);
  if (!region.has_value())
    return std::nullopt;

  PhysicalMemoryAddress current_address = start_address;
  MemoryRegion current_region = region.value();
  while (current_region != MemoryRegion::Count)
  {
    const u8* mem = GetMemoryRegionPointer(current_region);
    const PhysicalMemoryAddress region_start = GetMemoryRegionStart(current_region);
    const PhysicalMemoryAddress region_end = GetMemoryRegionEnd(current_region);

    if (mem)
    {
      PhysicalMemoryAddress region_offset = current_address - region_start;
      PhysicalMemoryAddress bytes_remaining = region_end - current_address;
      while (bytes_remaining >= pattern_length)
      {
        if (MaskedMemoryCompare(pattern, mask, pattern_length, mem + region_offset))
          return region_start + region_offset;

        region_offset++;
        bytes_remaining--;
      }
    }

    // skip RAM mirrors
    if (current_region == MemoryRegion::RAM)
      current_region = MemoryRegion::EXP1;
    else
      current_region = static_cast<MemoryRegion>(static_cast<int>(current_region) + 1);

    if (current_region != MemoryRegion::Count)
      current_address = GetMemoryRegionStart(current_region);
  }

  return std::nullopt;
}

#define BUS_CYCLES(n) CPU::g_state.pending_ticks += n

// TODO: Move handlers to own files for better inlining.
namespace Bus {
static void ClearHandlers(void** handlers);
static void SetHandlerForRegion(void** handlers, VirtualMemoryAddress address, u32 size,
                                MemoryReadHandler read_byte_handler, MemoryReadHandler read_halfword_handler,
                                MemoryReadHandler read_word_handler, MemoryWriteHandler write_byte_handler,
                                MemoryWriteHandler write_halfword_handler, MemoryWriteHandler write_word_handler);

// clang-format off
template<MemoryAccessSize size> static u32 UnknownReadHandler(VirtualMemoryAddress address);
template<MemoryAccessSize size> static void UnknownWriteHandler(VirtualMemoryAddress address, u32 value);
template<MemoryAccessSize size> static void IgnoreWriteHandler(VirtualMemoryAddress address, u32 value);
template<MemoryAccessSize size> static u32 UnmappedReadHandler(VirtualMemoryAddress address);
template<MemoryAccessSize size> static void UnmappedWriteHandler(VirtualMemoryAddress address, u32 value);

template<MemoryAccessSize size> static u32 RAMReadHandler(VirtualMemoryAddress address);
template<MemoryAccessSize size> static void RAMWriteHandler(VirtualMemoryAddress address, u32 value);

template<MemoryAccessSize size> static u32 BIOSReadHandler(VirtualMemoryAddress address);

template<MemoryAccessSize size> static u32 ScratchpadReadHandler(VirtualMemoryAddress address);
template<MemoryAccessSize size> static void ScratchpadWriteHandler(VirtualMemoryAddress address, u32 value);

template<MemoryAccessSize size> static u32 CacheControlReadHandler(VirtualMemoryAddress address);
template<MemoryAccessSize size> static void CacheControlWriteHandler(VirtualMemoryAddress address, u32 value);

template<MemoryAccessSize size> static u32 ICacheReadHandler(VirtualMemoryAddress address);
template<MemoryAccessSize size> static void ICacheWriteHandler(VirtualMemoryAddress address, u32 value);

template<MemoryAccessSize size> static u32 EXP1ReadHandler(VirtualMemoryAddress address);
template<MemoryAccessSize size> static void EXP1WriteHandler(VirtualMemoryAddress address, u32 value);
template<MemoryAccessSize size> static u32 EXP2ReadHandler(VirtualMemoryAddress address);
template<MemoryAccessSize size> static void EXP2WriteHandler(VirtualMemoryAddress address, u32 value);
template<MemoryAccessSize size> static u32 EXP3ReadHandler(VirtualMemoryAddress address);
template<MemoryAccessSize size> static void EXP3WriteHandler(VirtualMemoryAddress address, u32 value);

template<MemoryAccessSize size> static u32 HardwareReadHandler(VirtualMemoryAddress address);
template<MemoryAccessSize size> static void HardwareWriteHandler(VirtualMemoryAddress address, u32 value);

// clang-format on
} // namespace Bus

template<MemoryAccessSize size>
u32 Bus::UnknownReadHandler(VirtualMemoryAddress address)
{
  static constexpr const char* sizes[3] = {"byte", "halfword", "word"};
  Log_ErrorFmt("Invalid {} read at address 0x{:08X}, pc 0x{:08X}", sizes[static_cast<u32>(size)], address,
               CPU::g_state.pc);
  return 0xFFFFFFFFu;
}

template<MemoryAccessSize size>
void Bus::UnknownWriteHandler(VirtualMemoryAddress address, u32 value)
{
  static constexpr const char* sizes[3] = {"byte", "halfword", "word"};
  Log_ErrorFmt("Invalid {} write at address 0x{:08X}, value 0x{:08X}, pc 0x{:08X}", sizes[static_cast<u32>(size)],
               address, value, CPU::g_state.pc);
  CPU::g_state.bus_error = true;
}

template<MemoryAccessSize size>
void Bus::IgnoreWriteHandler(VirtualMemoryAddress address, u32 value)
{
  // noop
}

template<MemoryAccessSize size>
u32 Bus::UnmappedReadHandler(VirtualMemoryAddress address)
{
  CPU::g_state.bus_error = true;
  return UnknownReadHandler<size>(address);
}

template<MemoryAccessSize size>
void Bus::UnmappedWriteHandler(VirtualMemoryAddress address, u32 value)
{
  CPU::g_state.bus_error = true;
  UnknownWriteHandler<size>(address, value);
}

template<MemoryAccessSize size>
u32 Bus::RAMReadHandler(VirtualMemoryAddress address)
{
  BUS_CYCLES(RAM_READ_TICKS);

  const u32 offset = address & g_ram_mask;
  if constexpr (size == MemoryAccessSize::Byte)
  {
    return ZeroExtend32(g_ram[offset]);
  }
  else if constexpr (size == MemoryAccessSize::HalfWord)
  {
    u16 temp;
    std::memcpy(&temp, &g_ram[offset], sizeof(u16));
    return ZeroExtend32(temp);
  }
  else if constexpr (size == MemoryAccessSize::Word)
  {
    u32 value;
    std::memcpy(&value, &g_ram[offset], sizeof(u32));
    return value;
  }
}

template<MemoryAccessSize size>
void Bus::RAMWriteHandler(VirtualMemoryAddress address, u32 value)
{
  const u32 offset = address & g_ram_mask;

  if constexpr (size == MemoryAccessSize::Byte)
  {
    g_ram[offset] = Truncate8(value);
  }
  else if constexpr (size == MemoryAccessSize::HalfWord)
  {
    const u16 temp = Truncate16(value);
    std::memcpy(&g_ram[offset], &temp, sizeof(u16));
  }
  else if constexpr (size == MemoryAccessSize::Word)
  {
    std::memcpy(&g_ram[offset], &value, sizeof(u32));
  }
}

template<MemoryAccessSize size>
u32 Bus::BIOSReadHandler(VirtualMemoryAddress address)
{
  BUS_CYCLES(g_bios_access_time[static_cast<u32>(size)]);

  // TODO: Configurable mirroring.
  const u32 offset = address & UINT32_C(0x7FFFF);
  if constexpr (size == MemoryAccessSize::Byte)
  {
    return ZeroExtend32(g_bios[offset]);
  }
  else if constexpr (size == MemoryAccessSize::HalfWord)
  {
    u16 temp;
    std::memcpy(&temp, &g_bios[offset], sizeof(u16));
    return ZeroExtend32(temp);
  }
  else
  {
    u32 value;
    std::memcpy(&value, &g_bios[offset], sizeof(u32));
    return value;
  }
}

template<MemoryAccessSize size>
u32 Bus::ScratchpadReadHandler(VirtualMemoryAddress address)
{
  const PhysicalMemoryAddress cache_offset = address & MEMORY_LUT_PAGE_MASK;
  if (cache_offset >= CPU::SCRATCHPAD_SIZE) [[unlikely]]
    return UnknownReadHandler<size>(address);

  if constexpr (size == MemoryAccessSize::Byte)
  {
    return ZeroExtend32(CPU::g_state.scratchpad[cache_offset]);
  }
  else if constexpr (size == MemoryAccessSize::HalfWord)
  {
    u16 temp;
    std::memcpy(&temp, &CPU::g_state.scratchpad[cache_offset], sizeof(temp));
    return ZeroExtend32(temp);
  }
  else
  {
    u32 value;
    std::memcpy(&value, &CPU::g_state.scratchpad[cache_offset], sizeof(value));
    return value;
  }
}

template<MemoryAccessSize size>
void Bus::ScratchpadWriteHandler(VirtualMemoryAddress address, u32 value)
{
  const PhysicalMemoryAddress cache_offset = address & MEMORY_LUT_PAGE_MASK;
  if (cache_offset >= CPU::SCRATCHPAD_SIZE) [[unlikely]]
  {
    UnknownWriteHandler<size>(address, value);
    return;
  }

  if constexpr (size == MemoryAccessSize::Byte)
    CPU::g_state.scratchpad[cache_offset] = Truncate8(value);
  else if constexpr (size == MemoryAccessSize::HalfWord)
    std::memcpy(&CPU::g_state.scratchpad[cache_offset], &value, sizeof(u16));
  else if constexpr (size == MemoryAccessSize::Word)
    std::memcpy(&CPU::g_state.scratchpad[cache_offset], &value, sizeof(u32));
}

template<MemoryAccessSize size>
u32 Bus::CacheControlReadHandler(VirtualMemoryAddress address)
{
  if (address != 0xFFFE0130)
    return UnknownReadHandler<size>(address);

  return CPU::g_state.cache_control.bits;
}

template<MemoryAccessSize size>
void Bus::CacheControlWriteHandler(VirtualMemoryAddress address, u32 value)
{
  if (address != 0xFFFE0130)
    return UnknownWriteHandler<size>(address, value);

  Log_DevFmt("Cache control <- 0x{:08X}", value);
  CPU::g_state.cache_control.bits = value;
}

template<MemoryAccessSize size>
u32 Bus::ICacheReadHandler(VirtualMemoryAddress address)
{
  const u32 line = CPU::GetICacheLine(address);
  const u8* line_data = &CPU::g_state.icache_data[line * CPU::ICACHE_LINE_SIZE];
  const u32 offset = CPU::GetICacheLineOffset(address);
  u32 result;
  std::memcpy(&result, &line_data[offset], sizeof(result));
  return result;
}

template<MemoryAccessSize size>
void Bus::ICacheWriteHandler(VirtualMemoryAddress address, u32 value)
{
  const u32 line = CPU::GetICacheLine(address);
  const u32 offset = CPU::GetICacheLineOffset(address);
  CPU::g_state.icache_tags[line] = CPU::GetICacheTagForAddress(address) | CPU::ICACHE_INVALID_BITS;
  if constexpr (size == MemoryAccessSize::Byte)
    std::memcpy(&CPU::g_state.icache_data[line * CPU::ICACHE_LINE_SIZE + offset], &value, sizeof(u8));
  else if constexpr (size == MemoryAccessSize::HalfWord)
    std::memcpy(&CPU::g_state.icache_data[line * CPU::ICACHE_LINE_SIZE + offset], &value, sizeof(u16));
  else
    std::memcpy(&CPU::g_state.icache_data[line * CPU::ICACHE_LINE_SIZE + offset], &value, sizeof(u32));
}

template<MemoryAccessSize size>
u32 Bus::EXP1ReadHandler(VirtualMemoryAddress address)
{
  BUS_CYCLES(g_exp1_access_time[static_cast<u32>(size)]);

  const u32 offset = address & EXP1_MASK;
  u32 value;
  if (s_exp1_rom.empty())
  {
    // EXP1 not present.
    value = UINT32_C(0xFFFFFFFF);
  }
  else if (offset == 0x20018)
  {
    // Bit 0 - Action Replay On/Off
    value = UINT32_C(1);
  }
  else
  {
    const u32 transfer_size = u32(1) << static_cast<u32>(size);
    if ((offset + transfer_size) > s_exp1_rom.size())
    {
      value = UINT32_C(0);
    }
    else
    {
      if constexpr (size == MemoryAccessSize::Byte)
      {
        value = ZeroExtend32(s_exp1_rom[offset]);
      }
      else if constexpr (size == MemoryAccessSize::HalfWord)
      {
        u16 halfword;
        std::memcpy(&halfword, &s_exp1_rom[offset], sizeof(halfword));
        value = ZeroExtend32(halfword);
      }
      else
      {
        std::memcpy(&value, &s_exp1_rom[offset], sizeof(value));
      }

      // Log_DevPrintf("EXP1 read: 0x%08X -> 0x%08X", address, value);
    }
  }

  return value;
}

template<MemoryAccessSize size>
void Bus::EXP1WriteHandler(VirtualMemoryAddress address, u32 value)
{
  Log_WarningFmt("EXP1 write: 0x{:08X} <- 0x{:08X}", address, value);
}

template<MemoryAccessSize size>
u32 Bus::EXP2ReadHandler(VirtualMemoryAddress address)
{
  BUS_CYCLES(g_exp2_access_time[static_cast<u32>(size)]);

  const u32 offset = address & EXP2_MASK;
  u32 value;

  // rx/tx buffer empty
  if (offset == 0x21)
  {
    value = 0x04 | 0x08;
  }
  else if (offset >= 0x60 && offset <= 0x67)
  {
    // nocash expansion area
    value = UINT32_C(0xFFFFFFFF);
  }
  else
  {
    Log_WarningFmt("EXP2 read: 0x{:08X}", address);
    value = UINT32_C(0xFFFFFFFF);
  }

  return value;
}

template<MemoryAccessSize size>
void Bus::EXP2WriteHandler(VirtualMemoryAddress address, u32 value)
{
  const u32 offset = address & EXP2_MASK;
  if (offset == 0x23 || offset == 0x80)
  {
    AddTTYCharacter(static_cast<char>(value));
  }
  else if (offset == 0x41 || offset == 0x42)
  {
    Log_DevFmt("BIOS POST status: {:02X}", value & UINT32_C(0x0F));
  }
  else if (offset == 0x70)
  {
    Log_DevFmt("BIOS POST2 status: {:02X}", value & UINT32_C(0x0F));
  }
#if 0
  // TODO: Put behind configuration variable
  else if (offset == 0x81)
  {
    Log_WarningPrint("pcsx_debugbreak()");
    Host::ReportErrorAsync("Error", "pcsx_debugbreak()");
    System::PauseSystem(true);
    CPU::ExitExecution();
  }
  else if (offset == 0x82)
  {
    Log_WarningFmt("pcsx_exit() with status 0x{:02X}", value & UINT32_C(0xFF));
    Host::ReportErrorAsync("Error", fmt::format("pcsx_exit() with status 0x{:02X}", value & UINT32_C(0xFF)));
    System::ShutdownSystem(false);
    CPU::ExitExecution();
  }
#endif
  else
  {
    Log_WarningFmt("EXP2 write: 0x{:08X} <- 0x{:08X}", address, value);
  }
}

template<MemoryAccessSize size>
u32 Bus::EXP3ReadHandler(VirtualMemoryAddress address)
{
  Log_WarningFmt("EXP3 read: 0x{:08X}", address);
  return UINT32_C(0xFFFFFFFF);
}

template<MemoryAccessSize size>
void Bus::EXP3WriteHandler(VirtualMemoryAddress address, u32 value)
{
  const u32 offset = address & EXP3_MASK;
  if (offset == 0)
    Log_WarningFmt("BIOS POST3 status: {:02X}", value & UINT32_C(0x0F));
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// HARDWARE HANDLERS
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

namespace Bus::HWHandlers {
// clang-format off
template<MemoryAccessSize size> static u32 MemCtrlRead(PhysicalMemoryAddress address);
template<MemoryAccessSize size> static void MemCtrlWrite(PhysicalMemoryAddress address, u32 value);
template<MemoryAccessSize size> static u32 PADRead(PhysicalMemoryAddress address);
template<MemoryAccessSize size> static void PADWrite(PhysicalMemoryAddress address, u32 value);
template<MemoryAccessSize size> static u32 SIORead(PhysicalMemoryAddress address);
template<MemoryAccessSize size> static void SIOWrite(PhysicalMemoryAddress address, u32 value);
template<MemoryAccessSize size> static u32 MemCtrl2Read(PhysicalMemoryAddress address);
template<MemoryAccessSize size> static void MemCtrl2Write(PhysicalMemoryAddress address, u32 value);
template<MemoryAccessSize size> static u32 INTCRead(PhysicalMemoryAddress address);
template<MemoryAccessSize size> static void INTCWrite(PhysicalMemoryAddress address, u32 value);
template<MemoryAccessSize size> static u32 DMARead(PhysicalMemoryAddress address);
template<MemoryAccessSize size> static void DMAWrite(PhysicalMemoryAddress address, u32 value);
template<MemoryAccessSize size> static u32 TimersRead(PhysicalMemoryAddress address);
template<MemoryAccessSize size> static void TimersWrite(PhysicalMemoryAddress address, u32 value);
template<MemoryAccessSize size> static u32 CDROMRead(PhysicalMemoryAddress address);
template<MemoryAccessSize size> static void CDROMWrite(PhysicalMemoryAddress address, u32 value);
template<MemoryAccessSize size> static u32 GPURead(PhysicalMemoryAddress address);
template<MemoryAccessSize size> static void GPUWrite(PhysicalMemoryAddress address, u32 value);
template<MemoryAccessSize size> static u32 MDECRead(PhysicalMemoryAddress address);
template<MemoryAccessSize size> static void MDECWrite(PhysicalMemoryAddress address, u32 value);
template<MemoryAccessSize size> static u32 SPURead(PhysicalMemoryAddress address);
template<MemoryAccessSize size> static void SPUWrite(PhysicalMemoryAddress address, u32 value);
// clang-format on
} // namespace Bus::HWHandlers

template<MemoryAccessSize size>
u32 Bus::HWHandlers::MemCtrlRead(PhysicalMemoryAddress address)
{
  const u32 offset = address & MEMCTRL_MASK;

  u32 value = s_MEMCTRL.regs[FIXUP_WORD_OFFSET(size, offset) / 4];
  value = FIXUP_WORD_READ_VALUE(size, offset, value);
  BUS_CYCLES(2);
  return value;
}

template<MemoryAccessSize size>
void Bus::HWHandlers::MemCtrlWrite(PhysicalMemoryAddress address, u32 value)
{
  const u32 offset = address & MEMCTRL_MASK;
  const u32 index = FIXUP_WORD_OFFSET(size, offset) / 4;
  value = FIXUP_WORD_WRITE_VALUE(size, offset, value);

  const u32 write_mask = (index == 8) ? COMDELAY::WRITE_MASK : MEMDELAY::WRITE_MASK;
  const u32 new_value = (s_MEMCTRL.regs[index] & ~write_mask) | (value & write_mask);
  if (s_MEMCTRL.regs[index] != new_value)
  {
    s_MEMCTRL.regs[index] = new_value;
    RecalculateMemoryTimings();
  }
}

template<MemoryAccessSize size>
u32 Bus::HWHandlers::MemCtrl2Read(PhysicalMemoryAddress address)
{
  const u32 offset = address & MEMCTRL2_MASK;

  u32 value;
  if (offset == 0x00)
  {
    value = s_ram_size_reg;
  }
  else
  {
    return UnknownReadHandler<size>(address);
  }

  BUS_CYCLES(2);
  return value;
}

template<MemoryAccessSize size>
void Bus::HWHandlers::MemCtrl2Write(PhysicalMemoryAddress address, u32 value)
{
  const u32 offset = address & MEMCTRL2_MASK;

  if (offset == 0x00)
  {
    s_ram_size_reg = value;
  }
  else
  {
    return UnknownWriteHandler<size>(address, value);
  }
}

template<MemoryAccessSize size>
u32 Bus::HWHandlers::PADRead(PhysicalMemoryAddress address)
{
  const u32 offset = address & PAD_MASK;

  u32 value = Pad::ReadRegister(FIXUP_HALFWORD_OFFSET(size, offset));
  value = FIXUP_HALFWORD_READ_VALUE(size, offset, value);
  BUS_CYCLES(2);
  return value;
}

template<MemoryAccessSize size>
void Bus::HWHandlers::PADWrite(PhysicalMemoryAddress address, u32 value)
{
  const u32 offset = address & PAD_MASK;
  Pad::WriteRegister(FIXUP_HALFWORD_OFFSET(size, offset), FIXUP_HALFWORD_WRITE_VALUE(size, offset, value));
}

template<MemoryAccessSize size>
u32 Bus::HWHandlers::SIORead(PhysicalMemoryAddress address)
{
  const u32 offset = address & SIO_MASK;
  u32 value = SIO::ReadRegister(FIXUP_HALFWORD_OFFSET(size, offset));
  value = FIXUP_HALFWORD_READ_VALUE(size, offset, value);
  BUS_CYCLES(2);
  return value;
}

template<MemoryAccessSize size>
void Bus::HWHandlers::SIOWrite(PhysicalMemoryAddress address, u32 value)
{
  const u32 offset = address & SIO_MASK;
  SIO::WriteRegister(FIXUP_HALFWORD_OFFSET(size, offset), FIXUP_HALFWORD_WRITE_VALUE(size, offset, value));
}

template<MemoryAccessSize size>
u32 Bus::HWHandlers::CDROMRead(PhysicalMemoryAddress address)
{
  const u32 offset = address & CDROM_MASK;

  u32 value;
  switch (size)
  {
    case MemoryAccessSize::Word:
    {
      const u32 b0 = ZeroExtend32(CDROM::ReadRegister(offset));
      const u32 b1 = ZeroExtend32(CDROM::ReadRegister(offset + 1u));
      const u32 b2 = ZeroExtend32(CDROM::ReadRegister(offset + 2u));
      const u32 b3 = ZeroExtend32(CDROM::ReadRegister(offset + 3u));
      value = b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
    }

    case MemoryAccessSize::HalfWord:
    {
      const u32 lsb = ZeroExtend32(CDROM::ReadRegister(offset));
      const u32 msb = ZeroExtend32(CDROM::ReadRegister(offset + 1u));
      value = lsb | (msb << 8);
    }

    case MemoryAccessSize::Byte:
    default:
      value = ZeroExtend32(CDROM::ReadRegister(offset));
  }

  BUS_CYCLES(Bus::g_cdrom_access_time[static_cast<u32>(size)]);
  return value;
}

template<MemoryAccessSize size>
void Bus::HWHandlers::CDROMWrite(PhysicalMemoryAddress address, u32 value)
{
  const u32 offset = address & CDROM_MASK;
  switch (size)
  {
    case MemoryAccessSize::Word:
    {
      CDROM::WriteRegister(offset, Truncate8(value & 0xFFu));
      CDROM::WriteRegister(offset + 1u, Truncate8((value >> 8) & 0xFFu));
      CDROM::WriteRegister(offset + 2u, Truncate8((value >> 16) & 0xFFu));
      CDROM::WriteRegister(offset + 3u, Truncate8((value >> 24) & 0xFFu));
    }
    break;

    case MemoryAccessSize::HalfWord:
    {
      CDROM::WriteRegister(offset, Truncate8(value & 0xFFu));
      CDROM::WriteRegister(offset + 1u, Truncate8((value >> 8) & 0xFFu));
    }
    break;

    case MemoryAccessSize::Byte:
    default:
      CDROM::WriteRegister(offset, Truncate8(value));
      break;
  }
}

template<MemoryAccessSize size>
u32 Bus::HWHandlers::GPURead(PhysicalMemoryAddress address)
{
  const u32 offset = address & GPU_MASK;
  u32 value = g_gpu->ReadRegister(FIXUP_WORD_OFFSET(size, offset));
  value = FIXUP_WORD_READ_VALUE(size, offset, value);
  BUS_CYCLES(2);
  return value;
}

template<MemoryAccessSize size>
void Bus::HWHandlers::GPUWrite(PhysicalMemoryAddress address, u32 value)
{
  const u32 offset = address & GPU_MASK;
  g_gpu->WriteRegister(FIXUP_WORD_OFFSET(size, offset), FIXUP_WORD_WRITE_VALUE(size, offset, value));
}

template<MemoryAccessSize size>
u32 Bus::HWHandlers::MDECRead(PhysicalMemoryAddress address)
{
  const u32 offset = address & MDEC_MASK;
  u32 value = MDEC::ReadRegister(FIXUP_WORD_OFFSET(size, offset));
  value = FIXUP_WORD_READ_VALUE(size, offset, value);
  BUS_CYCLES(2);
  return value;
}

template<MemoryAccessSize size>
void Bus::HWHandlers::MDECWrite(PhysicalMemoryAddress address, u32 value)
{
  const u32 offset = address & MDEC_MASK;
  MDEC::WriteRegister(FIXUP_WORD_OFFSET(size, offset), FIXUP_WORD_WRITE_VALUE(size, offset, value));
}

template<MemoryAccessSize size>
u32 Bus::HWHandlers::INTCRead(PhysicalMemoryAddress address)
{
  const u32 offset = address & INTERRUPT_CONTROLLER_MASK;
  u32 value = InterruptController::ReadRegister(FIXUP_WORD_OFFSET(size, offset));
  value = FIXUP_WORD_READ_VALUE(size, offset, value);
  BUS_CYCLES(2);
  return value;
}

template<MemoryAccessSize size>
void Bus::HWHandlers::INTCWrite(PhysicalMemoryAddress address, u32 value)
{
  const u32 offset = address & INTERRUPT_CONTROLLER_MASK;
  InterruptController::WriteRegister(FIXUP_WORD_OFFSET(size, offset), FIXUP_WORD_WRITE_VALUE(size, offset, value));
}

template<MemoryAccessSize size>
u32 Bus::HWHandlers::TimersRead(PhysicalMemoryAddress address)
{
  const u32 offset = address & TIMERS_MASK;
  u32 value = Timers::ReadRegister(FIXUP_WORD_OFFSET(size, offset));
  value = FIXUP_WORD_READ_VALUE(size, offset, value);
  BUS_CYCLES(2);
  return value;
}

template<MemoryAccessSize size>
void Bus::HWHandlers::TimersWrite(PhysicalMemoryAddress address, u32 value)
{
  const u32 offset = address & TIMERS_MASK;
  Timers::WriteRegister(FIXUP_WORD_OFFSET(size, offset), FIXUP_WORD_WRITE_VALUE(size, offset, value));
}

template<MemoryAccessSize size>
u32 Bus::HWHandlers::SPURead(PhysicalMemoryAddress address)
{
  const u32 offset = address & SPU_MASK;
  u32 value;

  switch (size)
  {
    case MemoryAccessSize::Word:
    {
      // 32-bit reads are read as two 16-bit accesses.
      const u16 lsb = SPU::ReadRegister(offset);
      const u16 msb = SPU::ReadRegister(offset + 2);
      value = ZeroExtend32(lsb) | (ZeroExtend32(msb) << 16);
    }
    break;

    case MemoryAccessSize::HalfWord:
    {
      value = ZeroExtend32(SPU::ReadRegister(offset));
    }
    break;

    case MemoryAccessSize::Byte:
    default:
    {
      const u16 value16 = SPU::ReadRegister(FIXUP_HALFWORD_OFFSET(size, offset));
      value = FIXUP_HALFWORD_READ_VALUE(size, offset, value16);
    }
    break;
  }

  BUS_CYCLES(Bus::g_spu_access_time[static_cast<u32>(size)]);
  return value;
}

template<MemoryAccessSize size>
void Bus::HWHandlers::SPUWrite(PhysicalMemoryAddress address, u32 value)
{
  const u32 offset = address & SPU_MASK;

  // 32-bit writes are written as two 16-bit writes.
  // TODO: Ignore if address is not aligned.
  switch (size)
  {
    case MemoryAccessSize::Word:
    {
      DebugAssert(Common::IsAlignedPow2(offset, 2));
      SPU::WriteRegister(offset, Truncate16(value));
      SPU::WriteRegister(offset + 2, Truncate16(value >> 16));
      break;
    }

    case MemoryAccessSize::HalfWord:
    {
      DebugAssert(Common::IsAlignedPow2(offset, 2));
      SPU::WriteRegister(offset, Truncate16(value));
      break;
    }

    case MemoryAccessSize::Byte:
    {
      SPU::WriteRegister(FIXUP_HALFWORD_OFFSET(size, offset),
                         Truncate16(FIXUP_HALFWORD_READ_VALUE(size, offset, value)));
      break;
    }
  }
}

template<MemoryAccessSize size>
u32 Bus::HWHandlers::DMARead(PhysicalMemoryAddress address)
{
  const u32 offset = address & DMA_MASK;
  u32 value = DMA::ReadRegister(FIXUP_WORD_OFFSET(size, offset));
  value = FIXUP_WORD_READ_VALUE(size, offset, value);
  BUS_CYCLES(2);
  return value;
}

template<MemoryAccessSize size>
void Bus::HWHandlers::DMAWrite(PhysicalMemoryAddress address, u32 value)
{
  const u32 offset = address & DMA_MASK;
  DMA::WriteRegister(FIXUP_WORD_OFFSET(size, offset), FIXUP_WORD_WRITE_VALUE(size, offset, value));
}

#undef BUS_CYCLES

namespace Bus::HWHandlers {
// We index hardware registers by bits 15..8.
template<MemoryAccessType type, MemoryAccessSize size,
         typename RT = std::conditional_t<type == MemoryAccessType::Read, MemoryReadHandler, MemoryWriteHandler>>
static constexpr std::array<RT, 256> GetHardwareRegisterHandlerTable()
{
  std::array<RT, 256> ret = {};
  for (size_t i = 0; i < ret.size(); i++)
  {
    if constexpr (type == MemoryAccessType::Read)
      ret[i] = UnmappedReadHandler<size>;
    else
      ret[i] = UnmappedWriteHandler<size>;
  }

#if 0
  // Verifies no region has >1 handler, but doesn't compile on older GCC.
#define SET(raddr, rsize, read_handler, write_handler)                                                                 \
  static_assert(raddr >= 0x1F801000 && (raddr + rsize) <= 0x1F802000);                                                 \
  for (u32 taddr = raddr; taddr < (raddr + rsize); taddr += 16)                                                        \
  {                                                                                                                    \
    const u32 i = (taddr >> 4) & 0xFFu;                                                                                \
    if constexpr (type == MemoryAccessType::Read)                                                                      \
      ret[i] = (ret[i] == UnmappedReadHandler<size>) ? read_handler<size> : (abort(), read_handler<size>);             \
    else                                                                                                               \
      ret[i] = (ret[i] == UnmappedWriteHandler<size>) ? write_handler<size> : (abort(), write_handler<size>);          \
  }
#else
#define SET(raddr, rsize, read_handler, write_handler)                                                                 \
  static_assert(raddr >= 0x1F801000 && (raddr + rsize) <= 0x1F802000);                                                 \
  for (u32 taddr = raddr; taddr < (raddr + rsize); taddr += 16)                                                        \
  {                                                                                                                    \
    const u32 i = (taddr >> 4) & 0xFFu;                                                                                \
    if constexpr (type == MemoryAccessType::Read)                                                                      \
      ret[i] = read_handler<size>;                                                                                     \
    else                                                                                                               \
      ret[i] = write_handler<size>;                                                                                    \
  }
#endif

  SET(MEMCTRL_BASE, MEMCTRL_SIZE, MemCtrlRead, MemCtrlWrite);
  SET(PAD_BASE, PAD_SIZE, PADRead, PADWrite);
  SET(SIO_BASE, SIO_SIZE, SIORead, SIOWrite);
  SET(MEMCTRL2_BASE, MEMCTRL2_SIZE, MemCtrl2Read, MemCtrl2Write);
  SET(INTC_BASE, INTC_SIZE, INTCRead, INTCWrite);
  SET(DMA_BASE, DMA_SIZE, DMARead, DMAWrite);
  SET(TIMERS_BASE, TIMERS_SIZE, TimersRead, TimersWrite);
  SET(CDROM_BASE, CDROM_SIZE, CDROMRead, CDROMWrite);
  SET(GPU_BASE, GPU_SIZE, GPURead, GPUWrite);
  SET(MDEC_BASE, MDEC_SIZE, MDECRead, MDECWrite);
  SET(SPU_BASE, SPU_SIZE, SPURead, SPUWrite);

#undef SET

  return ret;
}
} // namespace Bus::HWHandlers

template<MemoryAccessSize size>
u32 Bus::HardwareReadHandler(VirtualMemoryAddress address)
{
  static constexpr const auto table = HWHandlers::GetHardwareRegisterHandlerTable<MemoryAccessType::Read, size>();
  const u32 table_index = (address >> 4) & 0xFFu;
  return table[table_index](address);
}

template<MemoryAccessSize size>
void Bus::HardwareWriteHandler(VirtualMemoryAddress address, u32 value)
{
  static constexpr const auto table = HWHandlers::GetHardwareRegisterHandlerTable<MemoryAccessType::Write, size>();
  const u32 table_index = (address >> 4) & 0xFFu;
  return table[table_index](address, value);
}

//////////////////////////////////////////////////////////////////////////

void Bus::SetHandlers()
{
  ClearHandlers(g_memory_handlers);
  ClearHandlers(g_memory_handlers_isc);

#define SET(table, start, size, read_handler, write_handler)                                                           \
  SetHandlerForRegion(table, start, size, read_handler<MemoryAccessSize::Byte>,                                        \
                      read_handler<MemoryAccessSize::HalfWord>, read_handler<MemoryAccessSize::Word>,                  \
                      write_handler<MemoryAccessSize::Byte>, write_handler<MemoryAccessSize::HalfWord>,                \
                      write_handler<MemoryAccessSize::Word>)
#define SETUC(start, size, read_handler, write_handler)                                                                \
  SET(g_memory_handlers, start, size, read_handler, write_handler);                                                    \
  SET(g_memory_handlers_isc, start, size, read_handler, write_handler)

  static constexpr u32 KUSEG = 0;
  static constexpr u32 KSEG0 = 0x80000000U;
  static constexpr u32 KSEG1 = 0xA0000000U;
  static constexpr u32 KSEG2 = 0xC0000000U;

  // KUSEG - Cached
  // Cache isolated appears to affect KUSEG+KSEG0.
  SET(g_memory_handlers, KUSEG | RAM_BASE, RAM_MIRROR_SIZE, RAMReadHandler, RAMWriteHandler);
  SET(g_memory_handlers, KUSEG | CPU::SCRATCHPAD_ADDR, 0x1000, ScratchpadReadHandler, ScratchpadWriteHandler);
  SET(g_memory_handlers, KUSEG | BIOS_BASE, BIOS_SIZE, BIOSReadHandler, IgnoreWriteHandler);
  SET(g_memory_handlers, KUSEG | EXP1_BASE, EXP1_SIZE, EXP1ReadHandler, EXP1WriteHandler);
  SET(g_memory_handlers, KUSEG | HW_BASE, HW_SIZE, HardwareReadHandler, HardwareWriteHandler);
  SET(g_memory_handlers, KUSEG | EXP2_BASE, EXP2_SIZE, EXP2ReadHandler, EXP2WriteHandler);
  SET(g_memory_handlers, KUSEG | EXP3_BASE, EXP3_SIZE, EXP3ReadHandler, EXP3WriteHandler);
  SET(g_memory_handlers_isc, KUSEG, 0x80000000, ICacheReadHandler, ICacheWriteHandler);

  // KSEG0 - Cached
  SET(g_memory_handlers, KSEG0 | RAM_BASE, RAM_MIRROR_SIZE, RAMReadHandler, RAMWriteHandler);
  SET(g_memory_handlers, KSEG0 | CPU::SCRATCHPAD_ADDR, 0x1000, ScratchpadReadHandler, ScratchpadWriteHandler);
  SET(g_memory_handlers, KSEG0 | BIOS_BASE, BIOS_SIZE, BIOSReadHandler, IgnoreWriteHandler);
  SET(g_memory_handlers, KSEG0 | EXP1_BASE, EXP1_SIZE, EXP1ReadHandler, EXP1WriteHandler);
  SET(g_memory_handlers, KSEG0 | HW_BASE, HW_SIZE, HardwareReadHandler, HardwareWriteHandler);
  SET(g_memory_handlers, KSEG0 | EXP2_BASE, EXP2_SIZE, EXP2ReadHandler, EXP2WriteHandler);
  SET(g_memory_handlers, KSEG0 | EXP3_BASE, EXP3_SIZE, EXP3ReadHandler, EXP3WriteHandler);
  SET(g_memory_handlers_isc, KSEG0, 0x20000000, ICacheReadHandler, ICacheWriteHandler);

  // KSEG1 - Uncached
  SETUC(KSEG1 | RAM_BASE, RAM_MIRROR_SIZE, RAMReadHandler, RAMWriteHandler);
  SETUC(KSEG1 | BIOS_BASE, BIOS_SIZE, BIOSReadHandler, IgnoreWriteHandler);
  SETUC(KSEG1 | EXP1_BASE, EXP1_SIZE, EXP1ReadHandler, EXP1WriteHandler);
  SETUC(KSEG1 | HW_BASE, HW_SIZE, HardwareReadHandler, HardwareWriteHandler);
  SETUC(KSEG1 | EXP2_BASE, EXP2_SIZE, EXP2ReadHandler, EXP2WriteHandler);
  SETUC(KSEG1 | EXP3_BASE, EXP3_SIZE, EXP3ReadHandler, EXP3WriteHandler);

  // KSEG2 - Uncached - 0xFFFE0130
  SETUC(KSEG2 | 0xFFFE0000, 0x1000, CacheControlReadHandler, CacheControlWriteHandler);

#undef SET
#undef SETUC
}

void Bus::ClearHandlers(void** handlers)
{
  for (u32 size = 0; size < 3; size++)
  {
    MemoryReadHandler* read_handlers =
      OffsetHandlerArray<MemoryReadHandler>(handlers, static_cast<MemoryAccessSize>(size), MemoryAccessType::Read);
    const MemoryReadHandler read_handler =
      (size == 0) ?
        UnmappedReadHandler<MemoryAccessSize::Byte> :
        ((size == 1) ? UnmappedReadHandler<MemoryAccessSize::HalfWord> : UnmappedReadHandler<MemoryAccessSize::Word>);
    MemsetPtrs(read_handlers, read_handler, MEMORY_LUT_SIZE);

    MemoryWriteHandler* write_handlers =
      OffsetHandlerArray<MemoryWriteHandler>(handlers, static_cast<MemoryAccessSize>(size), MemoryAccessType::Write);
    const MemoryWriteHandler write_handler =
      (size == 0) ?
        UnmappedWriteHandler<MemoryAccessSize::Byte> :
        ((size == 1) ? UnmappedWriteHandler<MemoryAccessSize::HalfWord> : UnmappedWriteHandler<MemoryAccessSize::Word>);
    MemsetPtrs(write_handlers, write_handler, MEMORY_LUT_SIZE);
  }
}

void Bus::SetHandlerForRegion(void** handlers, VirtualMemoryAddress address, u32 size,
                              MemoryReadHandler read_byte_handler, MemoryReadHandler read_halfword_handler,
                              MemoryReadHandler read_word_handler, MemoryWriteHandler write_byte_handler,
                              MemoryWriteHandler write_halfword_handler, MemoryWriteHandler write_word_handler)
{
  // Should be 4K aligned.
  DebugAssert(Common::IsAlignedPow2(size, MEMORY_LUT_PAGE_SIZE));

  const u32 start_page = (address / MEMORY_LUT_PAGE_SIZE);
  const u32 num_pages = ((size + (MEMORY_LUT_PAGE_SIZE - 1)) / MEMORY_LUT_PAGE_SIZE);

  for (u32 acc_size = 0; acc_size < 3; acc_size++)
  {
    MemoryReadHandler* read_handlers =
      OffsetHandlerArray<MemoryReadHandler>(handlers, static_cast<MemoryAccessSize>(acc_size), MemoryAccessType::Read) +
      start_page;
    const MemoryReadHandler read_handler =
      (acc_size == 0) ? read_byte_handler : ((acc_size == 1) ? read_halfword_handler : read_word_handler);
#if 0
    for (u32 i = 0; i < num_pages; i++)
    {
      DebugAssert((acc_size == 0 && read_handlers[i] == UnmappedReadHandler<MemoryAccessSize::Byte>) ||
                  (acc_size == 1 && read_handlers[i] == UnmappedReadHandler<MemoryAccessSize::HalfWord>) ||
                  (acc_size == 2 && read_handlers[i] == UnmappedReadHandler<MemoryAccessSize::Word>));

      read_handlers[i] = read_handler;
    }
#else
    MemsetPtrs(read_handlers, read_handler, num_pages);
#endif

    MemoryWriteHandler* write_handlers = OffsetHandlerArray<MemoryWriteHandler>(
                                           handlers, static_cast<MemoryAccessSize>(acc_size), MemoryAccessType::Write) +
                                         start_page;
    const MemoryWriteHandler write_handler =
      (acc_size == 0) ? write_byte_handler : ((acc_size == 1) ? write_halfword_handler : write_word_handler);
#if 0
    for (u32 i = 0; i < num_pages; i++)
    {
      DebugAssert((acc_size == 0 && write_handlers[i] == UnmappedWriteHandler<MemoryAccessSize::Byte>) ||
                  (acc_size == 1 && write_handlers[i] == UnmappedWriteHandler<MemoryAccessSize::HalfWord>) ||
                  (acc_size == 2 && write_handlers[i] == UnmappedWriteHandler<MemoryAccessSize::Word>));

      write_handlers[i] = write_handler;
    }
#else
    MemsetPtrs(write_handlers, write_handler, num_pages);
#endif
  }
}

void** Bus::GetMemoryHandlers(bool isolate_cache, bool swap_caches)
{
  if (!isolate_cache)
    return g_memory_handlers;

#ifdef _DEBUG
  if (swap_caches)
    Log_WarningPrint("Cache isolated and swapped, icache will be written instead of scratchpad?");
#endif

  return g_memory_handlers_isc;
}
