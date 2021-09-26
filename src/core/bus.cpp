#include "bus.h"
#include "cdrom.h"
#include "common/align.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/make_array.h"
#include "common/state_wrapper.h"
#include "cpu_code_cache.h"
#include "cpu_core.h"
#include "cpu_core_private.h"
#include "cpu_disasm.h"
#include "dma.h"
#include "gpu.h"
#include "host_interface.h"
#include "interrupt_controller.h"
#include "mdec.h"
#include "pad.h"
#include "sio.h"
#include "spu.h"
#include "timers.h"
#include <cstdio>
#include <tuple>
#include <utility>
Log_SetChannel(Bus);

namespace Bus {

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

std::bitset<RAM_8MB_CODE_PAGE_COUNT> m_ram_code_bits{};
u32 m_ram_code_page_count = 0;
u8* g_ram = nullptr; // 2MB RAM
u32 g_ram_size = 0;
u32 g_ram_mask = 0;
u8 g_bios[BIOS_SIZE]{}; // 512K BIOS ROM

static std::array<TickCount, 3> m_exp1_access_time = {};
static std::array<TickCount, 3> m_exp2_access_time = {};
static std::array<TickCount, 3> m_bios_access_time = {};
static std::array<TickCount, 3> m_cdrom_access_time = {};
static std::array<TickCount, 3> m_spu_access_time = {};

static std::vector<u8> m_exp1_rom;

static MEMCTRL m_MEMCTRL = {};
static u32 m_ram_size_reg = 0;

static std::string m_tty_line_buffer;

static Common::MemoryArena m_memory_arena;

static CPUFastmemMode m_fastmem_mode = CPUFastmemMode::Disabled;

#ifdef WITH_MMAP_FASTMEM
static u8* m_fastmem_base = nullptr;
static std::vector<Common::MemoryArena::View> m_fastmem_ram_views;
static std::vector<Common::MemoryArena::View> m_fastmem_reserved_views;
#endif

static u8** m_fastmem_lut = nullptr;
static constexpr auto m_fastmem_ram_mirrors =
  make_array(0x00000000u, 0x00200000u, 0x00400000u, 0x00600000u, 0x80000000u, 0x80200000u, 0x80400000u, 0x80600000u,
             0xA0000000u, 0xA0200000u, 0xA0400000u, 0xA0600000u);

static std::tuple<TickCount, TickCount, TickCount> CalculateMemoryTiming(MEMDELAY mem_delay, COMDELAY common_delay);
static void RecalculateMemoryTimings();

static bool AllocateMemory(bool enable_8mb_ram);
static void ReleaseMemory();

static void SetCodePageFastmemProtection(u32 page_index, bool writable);

#define FIXUP_HALFWORD_OFFSET(size, offset) ((size >= MemoryAccessSize::HalfWord) ? (offset) : ((offset) & ~1u))
#define FIXUP_HALFWORD_READ_VALUE(size, offset, value)                                                                 \
  ((size >= MemoryAccessSize::HalfWord) ? (value) : ((value) >> (((offset)&u32(1)) * 8u)))
#define FIXUP_HALFWORD_WRITE_VALUE(size, offset, value)                                                                \
  ((size >= MemoryAccessSize::HalfWord) ? (value) : ((value) << (((offset)&u32(1)) * 8u)))

#define FIXUP_WORD_OFFSET(size, offset) ((size == MemoryAccessSize::Word) ? (offset) : ((offset) & ~3u))
#define FIXUP_WORD_READ_VALUE(size, offset, value)                                                                     \
  ((size == MemoryAccessSize::Word) ? (value) : ((value) >> (((offset)&3u) * 8)))
#define FIXUP_WORD_WRITE_VALUE(size, offset, value)                                                                    \
  ((size == MemoryAccessSize::Word) ? (value) : ((value) << (((offset)&3u) * 8)))

bool Initialize()
{
  if (!AllocateMemory(g_settings.enable_8mb_ram))
  {
    g_host_interface->ReportError("Failed to allocate memory");
    return false;
  }

  Reset();
  return true;
}

void Shutdown()
{
  std::free(m_fastmem_lut);
  m_fastmem_lut = nullptr;

#ifdef WITH_MMAP_FASTMEM
  m_fastmem_base = nullptr;
  m_fastmem_ram_views.clear();
#endif

  CPU::g_state.fastmem_base = nullptr;
  m_fastmem_mode = CPUFastmemMode::Disabled;

  ReleaseMemory();
}

void Reset()
{
  std::memset(g_ram, 0, g_ram_size);
  m_MEMCTRL.exp1_base = 0x1F000000;
  m_MEMCTRL.exp2_base = 0x1F802000;
  m_MEMCTRL.exp1_delay_size.bits = 0x0013243F;
  m_MEMCTRL.exp3_delay_size.bits = 0x00003022;
  m_MEMCTRL.bios_delay_size.bits = 0x0013243F;
  m_MEMCTRL.spu_delay_size.bits = 0x200931E1;
  m_MEMCTRL.cdrom_delay_size.bits = 0x00020843;
  m_MEMCTRL.exp2_delay_size.bits = 0x00070777;
  m_MEMCTRL.common_delay.bits = 0x00031125;
  m_ram_size_reg = UINT32_C(0x00000B88);
  m_ram_code_bits = {};
  RecalculateMemoryTimings();
}

bool DoState(StateWrapper& sw)
{
  u32 ram_size = g_ram_size;
  sw.DoEx(&ram_size, 52, static_cast<u32>(RAM_2MB_SIZE));
  if (ram_size != g_ram_size)
  {
    const bool using_8mb_ram = (ram_size == RAM_8MB_SIZE);
    ReleaseMemory();
    if (!AllocateMemory(using_8mb_ram))
      return false;

    UpdateFastmemViews(m_fastmem_mode);
    CPU::UpdateFastmemBase();
  }

  sw.Do(&m_exp1_access_time);
  sw.Do(&m_exp2_access_time);
  sw.Do(&m_bios_access_time);
  sw.Do(&m_cdrom_access_time);
  sw.Do(&m_spu_access_time);
  sw.DoBytes(g_ram, g_ram_size);
  sw.DoBytes(g_bios, BIOS_SIZE);
  sw.DoArray(m_MEMCTRL.regs, countof(m_MEMCTRL.regs));
  sw.Do(&m_ram_size_reg);
  sw.Do(&m_tty_line_buffer);
  return !sw.HasError();
}

void SetExpansionROM(std::vector<u8> data)
{
  m_exp1_rom = std::move(data);
}

void SetBIOS(const std::vector<u8>& image)
{
  if (image.size() != static_cast<u32>(BIOS_SIZE))
  {
    Panic("Incorrect BIOS image size");
    return;
  }

  std::memcpy(g_bios, image.data(), BIOS_SIZE);
}

std::tuple<TickCount, TickCount, TickCount> CalculateMemoryTiming(MEMDELAY mem_delay, COMDELAY common_delay)
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

void RecalculateMemoryTimings()
{
  std::tie(m_bios_access_time[0], m_bios_access_time[1], m_bios_access_time[2]) =
    CalculateMemoryTiming(m_MEMCTRL.bios_delay_size, m_MEMCTRL.common_delay);
  std::tie(m_cdrom_access_time[0], m_cdrom_access_time[1], m_cdrom_access_time[2]) =
    CalculateMemoryTiming(m_MEMCTRL.cdrom_delay_size, m_MEMCTRL.common_delay);
  std::tie(m_spu_access_time[0], m_spu_access_time[1], m_spu_access_time[2]) =
    CalculateMemoryTiming(m_MEMCTRL.spu_delay_size, m_MEMCTRL.common_delay);

  Log_TracePrintf("BIOS Memory Timing: %u bit bus, byte=%d, halfword=%d, word=%d",
                  m_MEMCTRL.bios_delay_size.data_bus_16bit ? 16 : 8, m_bios_access_time[0] + 1,
                  m_bios_access_time[1] + 1, m_bios_access_time[2] + 1);
  Log_TracePrintf("CDROM Memory Timing: %u bit bus, byte=%d, halfword=%d, word=%d",
                  m_MEMCTRL.cdrom_delay_size.data_bus_16bit ? 16 : 8, m_cdrom_access_time[0] + 1,
                  m_cdrom_access_time[1] + 1, m_cdrom_access_time[2] + 1);
  Log_TracePrintf("SPU Memory Timing: %u bit bus, byte=%d, halfword=%d, word=%d",
                  m_MEMCTRL.spu_delay_size.data_bus_16bit ? 16 : 8, m_spu_access_time[0] + 1, m_spu_access_time[1] + 1,
                  m_spu_access_time[2] + 1);
}

bool AllocateMemory(bool enable_8mb_ram)
{
  if (!m_memory_arena.Create(MEMORY_ARENA_SIZE, true, false))
  {
    Log_ErrorPrint("Failed to create memory arena");
    return false;
  }

  // Create the base views.
  const u32 ram_size = enable_8mb_ram ? RAM_8MB_SIZE : RAM_2MB_SIZE;
  const u32 ram_mask = enable_8mb_ram ? RAM_8MB_MASK : RAM_2MB_MASK;
  g_ram = static_cast<u8*>(m_memory_arena.CreateViewPtr(MEMORY_ARENA_RAM_OFFSET, ram_size, true, false));
  if (!g_ram)
  {
    Log_ErrorPrintf("Failed to create base views of memory (%u bytes RAM)", ram_size);
    return false;
  }

  g_ram_mask = ram_mask;
  g_ram_size = ram_size;
  m_ram_code_page_count = enable_8mb_ram ? RAM_8MB_CODE_PAGE_COUNT : RAM_2MB_CODE_PAGE_COUNT;

  Log_InfoPrintf("RAM is %u bytes at %p", g_ram_size, g_ram);
  return true;
}

void ReleaseMemory()
{
  if (g_ram)
  {
    m_memory_arena.ReleaseViewPtr(g_ram, g_ram_size);
    g_ram = nullptr;
    g_ram_mask = 0;
    g_ram_size = 0;
  }

  m_memory_arena.Destroy();
}

static ALWAYS_INLINE u32 FastmemAddressToLUTPageIndex(u32 address)
{
  return address >> 12;
}

static ALWAYS_INLINE_RELEASE void SetLUTFastmemPage(u32 address, u8* ptr, bool writable)
{
  m_fastmem_lut[FastmemAddressToLUTPageIndex(address)] = ptr;
  m_fastmem_lut[FASTMEM_LUT_NUM_PAGES + FastmemAddressToLUTPageIndex(address)] = writable ? ptr : nullptr;
}

CPUFastmemMode GetFastmemMode()
{
  return m_fastmem_mode;
}

u8* GetFastmemBase()
{
#ifdef WITH_MMAP_FASTMEM
  if (m_fastmem_mode == CPUFastmemMode::MMap)
    return m_fastmem_base;
#endif
  if (m_fastmem_mode == CPUFastmemMode::LUT)
    return reinterpret_cast<u8*>(m_fastmem_lut);

  return nullptr;
}

void UpdateFastmemViews(CPUFastmemMode mode)
{
#ifndef WITH_MMAP_FASTMEM
  Assert(mode != CPUFastmemMode::MMap);
#else
  m_fastmem_ram_views.clear();
  m_fastmem_reserved_views.clear();
#endif

  m_fastmem_mode = mode;
  if (mode == CPUFastmemMode::Disabled)
  {
#ifdef WITH_MMAP_FASTMEM
    m_fastmem_base = nullptr;
#endif
    std::free(m_fastmem_lut);
    m_fastmem_lut = nullptr;
    return;
  }

#ifdef WITH_MMAP_FASTMEM
  if (mode == CPUFastmemMode::MMap)
  {
    std::free(m_fastmem_lut);
    m_fastmem_lut = nullptr;

    if (!m_fastmem_base)
    {
      m_fastmem_base = static_cast<u8*>(m_memory_arena.FindBaseAddressForMapping(FASTMEM_REGION_SIZE));
      if (!m_fastmem_base)
      {
        Log_ErrorPrint("Failed to find base address for fastmem");
        return;
      }

      Log_InfoPrintf("Fastmem base: %p", m_fastmem_base);
    }

    auto MapRAM = [](u32 base_address) {
      u8* map_address = m_fastmem_base + base_address;
      auto view = m_memory_arena.CreateView(MEMORY_ARENA_RAM_OFFSET, g_ram_size, true, false, map_address);
      if (!view)
      {
        Log_ErrorPrintf("Failed to map RAM at fastmem area %p (offset 0x%08X)", map_address, g_ram_size);
        return;
      }

      // mark all pages with code as non-writable
      for (u32 i = 0; i < m_ram_code_page_count; i++)
      {
        if (m_ram_code_bits[i])
        {
          u8* page_address = map_address + (i * HOST_PAGE_SIZE);
          if (!m_memory_arena.SetPageProtection(page_address, HOST_PAGE_SIZE, true, false, false))
          {
            Log_ErrorPrintf("Failed to write-protect code page at %p", page_address);
            return;
          }
        }
      }

      m_fastmem_ram_views.push_back(std::move(view.value()));
    };

    auto ReserveRegion = [](u32 start_address, u32 end_address_inclusive) {
    // We don't reserve memory regions on Android because the app could be subject to address space size limitations.
#ifndef __ANDROID__
      Assert(end_address_inclusive >= start_address);
      u8* map_address = m_fastmem_base + start_address;
      auto view = m_memory_arena.CreateReservedView(end_address_inclusive - start_address + 1, map_address);
      if (!view)
      {
        Log_ErrorPrintf("Failed to map reserved region %p (size 0x%08X)", map_address,
                        end_address_inclusive - start_address + 1);
        return;
      }

      m_fastmem_reserved_views.push_back(std::move(view.value()));
#endif
    };

    // KUSEG - cached
    MapRAM(0x00000000);
    ReserveRegion(0x00000000 + g_ram_size, 0x80000000 - 1);

    // KSEG0 - cached
    MapRAM(0x80000000);
    ReserveRegion(0x80000000 + g_ram_size, 0xA0000000 - 1);

    // KSEG1 - uncached
    MapRAM(0xA0000000);
    ReserveRegion(0xA0000000 + g_ram_size, 0xFFFFFFFF);

    return;
  }
#endif

#ifdef WITH_MMAP_FASTMEM
  m_fastmem_base = nullptr;
#endif

  if (!m_fastmem_lut)
  {
    m_fastmem_lut = static_cast<u8**>(std::calloc(FASTMEM_LUT_NUM_SLOTS, sizeof(u8*)));
    Assert(m_fastmem_lut);

    Log_InfoPrintf("Fastmem base (software): %p", m_fastmem_lut);
  }

  auto MapRAM = [](u32 base_address) {
    for (u32 address = 0; address < g_ram_size; address += HOST_PAGE_SIZE)
    {
      SetLUTFastmemPage(base_address + address, &g_ram[address],
                        !m_ram_code_bits[FastmemAddressToLUTPageIndex(address)]);
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

bool CanUseFastmemForAddress(VirtualMemoryAddress address)
{
  const PhysicalMemoryAddress paddr = address & CPU::PHYSICAL_MEMORY_ADDRESS_MASK;

  switch (m_fastmem_mode)
  {
#ifdef WITH_MMAP_FASTMEM
    case CPUFastmemMode::MMap:
    {
      // Currently since we don't map the mirrors, don't use fastmem for them.
      // This is because the swapping of page code bits for SMC is too expensive.
      return (paddr < RAM_MIRROR_END);
    }
#endif

    case CPUFastmemMode::LUT:
      return (paddr < g_ram_size);

    case CPUFastmemMode::Disabled:
    default:
      return false;
  }
}

bool IsRAMCodePage(u32 index)
{
  return m_ram_code_bits[index];
}

void SetRAMCodePage(u32 index)
{
  if (m_ram_code_bits[index])
    return;

  // protect fastmem pages
  m_ram_code_bits[index] = true;
  SetCodePageFastmemProtection(index, false);
}

void ClearRAMCodePage(u32 index)
{
  if (!m_ram_code_bits[index])
    return;

  // unprotect fastmem pages
  m_ram_code_bits[index] = false;
  SetCodePageFastmemProtection(index, true);
}

void SetCodePageFastmemProtection(u32 page_index, bool writable)
{
#ifdef WITH_MMAP_FASTMEM
  if (m_fastmem_mode == CPUFastmemMode::MMap)
  {
    // unprotect fastmem pages
    for (const auto& view : m_fastmem_ram_views)
    {
      u8* page_address = static_cast<u8*>(view.GetBasePointer()) + (page_index * HOST_PAGE_SIZE);
      if (!m_memory_arena.SetPageProtection(page_address, HOST_PAGE_SIZE, true, writable, false))
      {
        Log_ErrorPrintf("Failed to %s code page %u (0x%08X) @ %p", writable ? "unprotect" : "protect", page_index,
                        page_index * static_cast<u32>(HOST_PAGE_SIZE), page_address);
      }
    }

    return;
  }
#endif

  if (m_fastmem_mode == CPUFastmemMode::LUT)
  {
    // mirrors...
    const u32 ram_address = page_index * HOST_PAGE_SIZE;
    for (u32 mirror_start : m_fastmem_ram_mirrors)
      SetLUTFastmemPage(mirror_start + ram_address, &g_ram[ram_address], writable);
  }
}

void ClearRAMCodePageFlags()
{
  m_ram_code_bits.reset();

#ifdef WITH_MMAP_FASTMEM
  if (m_fastmem_mode == CPUFastmemMode::MMap)
  {
    // unprotect fastmem pages
    for (const auto& view : m_fastmem_ram_views)
    {
      if (!m_memory_arena.SetPageProtection(view.GetBasePointer(), view.GetMappingSize(), true, true, false))
      {
        Log_ErrorPrintf("Failed to unprotect code pages for fastmem view @ %p", view.GetBasePointer());
      }
    }
  }
#endif

  if (m_fastmem_mode == CPUFastmemMode::LUT)
  {
    for (u32 i = 0; i < m_ram_code_page_count; i++)
    {
      const u32 addr = (i * HOST_PAGE_SIZE);
      for (u32 mirror_start : m_fastmem_ram_mirrors)
        SetLUTFastmemPage(mirror_start + addr, &g_ram[addr], true);
    }
  }
}

bool IsCodePageAddress(PhysicalMemoryAddress address)
{
  return IsRAMAddress(address) ? m_ram_code_bits[(address & g_ram_mask) / HOST_PAGE_SIZE] : false;
}

bool HasCodePagesInRange(PhysicalMemoryAddress start_address, u32 size)
{
  if (!IsRAMAddress(start_address))
    return false;

  start_address = (start_address & g_ram_mask);

  const u32 end_address = start_address + size;
  while (start_address < end_address)
  {
    const u32 code_page_index = start_address / HOST_PAGE_SIZE;
    if (m_ram_code_bits[code_page_index])
      return true;

    start_address += HOST_PAGE_SIZE;
  }

  return false;
}

std::optional<MemoryRegion> GetMemoryRegionForAddress(PhysicalMemoryAddress address)
{
  if (address < RAM_2MB_SIZE)
    return MemoryRegion::RAM;
  else if (address < RAM_MIRROR_END)
    return static_cast<MemoryRegion>(static_cast<u32>(MemoryRegion::RAM) + (address / RAM_2MB_SIZE));
  else if (address >= EXP1_BASE && address < (EXP1_BASE + EXP1_SIZE))
    return MemoryRegion::EXP1;
  else if (address >= CPU::DCACHE_LOCATION && address < (CPU::DCACHE_LOCATION + CPU::DCACHE_SIZE))
    return MemoryRegion::Scratchpad;
  else if (address >= BIOS_BASE && address < (BIOS_BASE + BIOS_SIZE))
    return MemoryRegion::BIOS;

  return std::nullopt;
}

static constexpr std::array<std::pair<PhysicalMemoryAddress, PhysicalMemoryAddress>,
                            static_cast<u32>(MemoryRegion::Count)>
  s_code_region_ranges = {{
    {0, RAM_2MB_SIZE},
    {RAM_2MB_SIZE, RAM_2MB_SIZE * 2},
    {RAM_2MB_SIZE * 2, RAM_2MB_SIZE * 3},
    {RAM_2MB_SIZE * 3, RAM_MIRROR_END},
    {EXP1_BASE, EXP1_BASE + EXP1_SIZE},
    {CPU::DCACHE_LOCATION, CPU::DCACHE_LOCATION + CPU::DCACHE_SIZE},
    {BIOS_BASE, BIOS_BASE + BIOS_SIZE},
  }};

PhysicalMemoryAddress GetMemoryRegionStart(MemoryRegion region)
{
  return s_code_region_ranges[static_cast<u32>(region)].first;
}

PhysicalMemoryAddress GetMemoryRegionEnd(MemoryRegion region)
{
  return s_code_region_ranges[static_cast<u32>(region)].second;
}

u8* GetMemoryRegionPointer(MemoryRegion region)
{
  switch (region)
  {
    case MemoryRegion::RAM:
      return g_ram;

    case MemoryRegion::RAMMirror1:
      return (g_ram + (RAM_2MB_SIZE & g_ram_mask));

    case MemoryRegion::RAMMirror2:
      return (g_ram + ((RAM_2MB_SIZE * 2) & g_ram_mask));

    case MemoryRegion::RAMMirror3:
      return (g_ram + ((RAM_8MB_SIZE * 3) & g_ram_mask));

    case MemoryRegion::EXP1:
      return nullptr;

    case MemoryRegion::Scratchpad:
      return CPU::g_state.dcache.data();

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

std::optional<PhysicalMemoryAddress> SearchMemory(PhysicalMemoryAddress start_address, const u8* pattern,
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

static TickCount DoInvalidAccess(MemoryAccessType type, MemoryAccessSize size, PhysicalMemoryAddress address,
                                 u32& value)
{
  SmallString str;
  str.AppendString("Invalid bus ");
  if (size == MemoryAccessSize::Byte)
    str.AppendString("byte");
  if (size == MemoryAccessSize::HalfWord)
    str.AppendString("word");
  if (size == MemoryAccessSize::Word)
    str.AppendString("dword");
  str.AppendCharacter(' ');
  if (type == MemoryAccessType::Read)
    str.AppendString("read");
  else
    str.AppendString("write");

  str.AppendFormattedString(" at address 0x%08X", address);
  if (type == MemoryAccessType::Write)
    str.AppendFormattedString(" (value 0x%08X)", value);

  Log_ErrorPrint(str);
  if (type == MemoryAccessType::Read)
    value = UINT32_C(0xFFFFFFFF);

  return (type == MemoryAccessType::Read) ? 1 : 0;
}

template<MemoryAccessType type, MemoryAccessSize size, bool skip_redundant_writes>
ALWAYS_INLINE static TickCount DoRAMAccess(u32 offset, u32& value)
{
  offset &= g_ram_mask;
  if constexpr (type == MemoryAccessType::Read)
  {
    if constexpr (size == MemoryAccessSize::Byte)
    {
      value = ZeroExtend32(g_ram[offset]);
    }
    else if constexpr (size == MemoryAccessSize::HalfWord)
    {
      u16 temp;
      std::memcpy(&temp, &g_ram[offset], sizeof(u16));
      value = ZeroExtend32(temp);
    }
    else if constexpr (size == MemoryAccessSize::Word)
    {
      std::memcpy(&value, &g_ram[offset], sizeof(u32));
    }
  }
  else
  {
    const u32 page_index = offset / HOST_PAGE_SIZE;
    if constexpr (skip_redundant_writes)
    {
      if constexpr (size == MemoryAccessSize::Byte)
      {
        if (g_ram[offset] != Truncate8(value))
        {
          g_ram[offset] = Truncate8(value);
          if (m_ram_code_bits[page_index])
            CPU::CodeCache::InvalidateBlocksWithPageIndex(page_index);
        }
      }
      else if constexpr (size == MemoryAccessSize::HalfWord)
      {
        const u16 new_value = Truncate16(value);
        u16 old_value;
        std::memcpy(&old_value, &g_ram[offset], sizeof(old_value));
        if (old_value != new_value)
        {
          std::memcpy(&g_ram[offset], &new_value, sizeof(u16));
          if (m_ram_code_bits[page_index])
            CPU::CodeCache::InvalidateBlocksWithPageIndex(page_index);
        }
      }
      else if constexpr (size == MemoryAccessSize::Word)
      {
        u32 old_value;
        std::memcpy(&old_value, &g_ram[offset], sizeof(u32));
        if (old_value != value)
        {
          std::memcpy(&g_ram[offset], &value, sizeof(u32));
          if (m_ram_code_bits[page_index])
            CPU::CodeCache::InvalidateBlocksWithPageIndex(page_index);
        }
      }
    }
    else
    {
      if (m_ram_code_bits[page_index])
        CPU::CodeCache::InvalidateBlocksWithPageIndex(page_index);

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
  }

  return (type == MemoryAccessType::Read) ? RAM_READ_TICKS : 0;
}

template<MemoryAccessType type, MemoryAccessSize size>
ALWAYS_INLINE static TickCount DoBIOSAccess(u32 offset, u32& value)
{
  // TODO: Configurable mirroring.
  if constexpr (type == MemoryAccessType::Read)
  {
    offset &= UINT32_C(0x7FFFF);
    if constexpr (size == MemoryAccessSize::Byte)
    {
      value = ZeroExtend32(g_bios[offset]);
    }
    else if constexpr (size == MemoryAccessSize::HalfWord)
    {
      u16 temp;
      std::memcpy(&temp, &g_bios[offset], sizeof(u16));
      value = ZeroExtend32(temp);
    }
    else
    {
      std::memcpy(&value, &g_bios[offset], sizeof(u32));
    }
  }
  else
  {
    // Writes are ignored.
  }

  return m_bios_access_time[static_cast<u32>(size)];
}

template<MemoryAccessType type, MemoryAccessSize size>
static TickCount DoEXP1Access(u32 offset, u32& value)
{
  if constexpr (type == MemoryAccessType::Read)
  {
    if (m_exp1_rom.empty())
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
      if ((offset + transfer_size) > m_exp1_rom.size())
      {
        value = UINT32_C(0);
      }
      else
      {
        if constexpr (size == MemoryAccessSize::Byte)
        {
          value = ZeroExtend32(m_exp1_rom[offset]);
        }
        else if constexpr (size == MemoryAccessSize::HalfWord)
        {
          u16 halfword;
          std::memcpy(&halfword, &m_exp1_rom[offset], sizeof(halfword));
          value = ZeroExtend32(halfword);
        }
        else
        {
          std::memcpy(&value, &m_exp1_rom[offset], sizeof(value));
        }

        // Log_DevPrintf("EXP1 read: 0x%08X -> 0x%08X", EXP1_BASE | offset, value);
      }
    }

    return m_exp1_access_time[static_cast<u32>(size)];
  }
  else
  {
    Log_WarningPrintf("EXP1 write: 0x%08X <- 0x%08X", EXP1_BASE | offset, value);
    return 0;
  }
}

template<MemoryAccessType type, MemoryAccessSize size>
static TickCount DoEXP2Access(u32 offset, u32& value)
{
  if constexpr (type == MemoryAccessType::Read)
  {
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
      Log_WarningPrintf("EXP2 read: 0x%08X", EXP2_BASE | offset);
      value = UINT32_C(0xFFFFFFFF);
    }

    return m_exp2_access_time[static_cast<u32>(size)];
  }
  else
  {
    if (offset == 0x23 || offset == 0x80)
    {
      if (value == '\r')
      {
      }
      else if (value == '\n')
      {
        if (!m_tty_line_buffer.empty())
        {
          Log_InfoPrintf("TTY: %s", m_tty_line_buffer.c_str());
#ifdef _DEBUG
          if (CPU::IsTraceEnabled())
            CPU::WriteToExecutionLog("TTY: %s\n", m_tty_line_buffer.c_str());
#endif
        }
        m_tty_line_buffer.clear();
      }
      else
      {
        m_tty_line_buffer += static_cast<char>(Truncate8(value));
      }
    }
    else if (offset == 0x41 || offset == 0x42)
    {
      Log_DevPrintf("BIOS POST status: %02X", value & UINT32_C(0x0F));
    }
    else if (offset == 0x70)
    {
      Log_DevPrintf("BIOS POST2 status: %02X", value & UINT32_C(0x0F));
    }
    else
    {
      Log_WarningPrintf("EXP2 write: 0x%08X <- 0x%08X", EXP2_BASE | offset, value);
    }

    return 0;
  }
}

template<MemoryAccessType type>
ALWAYS_INLINE static TickCount DoEXP3Access(u32 offset, u32& value)
{
  if constexpr (type == MemoryAccessType::Read)
  {
    Log_WarningPrintf("EXP3 read: 0x%08X -> 0x%08X", offset, EXP3_BASE | offset);
    value = UINT32_C(0xFFFFFFFF);

    return 0;
  }
  else
  {
    if (offset == 0)
      Log_WarningPrintf("BIOS POST3 status: %02X", value & UINT32_C(0x0F));

    return 0;
  }
}

template<MemoryAccessType type>
ALWAYS_INLINE static TickCount DoUnknownEXPAccess(u32 address, u32& value)
{
  if constexpr (type == MemoryAccessType::Read)
  {
    Log_ErrorPrintf("Unknown EXP read: 0x%08X", address);
    return -1;
  }
  else
  {
    Log_WarningPrintf("Unknown EXP write: 0x%08X <- 0x%08X", address, value);
    return 0;
  }
}

template<MemoryAccessType type, MemoryAccessSize size>
ALWAYS_INLINE static TickCount DoMemoryControlAccess(u32 offset, u32& value)
{
  if constexpr (type == MemoryAccessType::Read)
  {
    value = m_MEMCTRL.regs[FIXUP_WORD_OFFSET(size, offset) / 4];
    value = FIXUP_WORD_READ_VALUE(size, offset, value);
    return 2;
  }
  else
  {
    const u32 index = FIXUP_WORD_OFFSET(size, offset) / 4;
    value = FIXUP_WORD_WRITE_VALUE(size, offset, value);

    const u32 write_mask = (index == 8) ? COMDELAY::WRITE_MASK : MEMDELAY::WRITE_MASK;
    const u32 new_value = (m_MEMCTRL.regs[index] & ~write_mask) | (value & write_mask);
    if (m_MEMCTRL.regs[index] != new_value)
    {
      m_MEMCTRL.regs[index] = new_value;
      RecalculateMemoryTimings();
    }
    return 0;
  }
}

template<MemoryAccessType type, MemoryAccessSize size>
ALWAYS_INLINE static TickCount DoMemoryControl2Access(u32 offset, u32& value)
{
  if constexpr (type == MemoryAccessType::Read)
  {
    if (offset == 0x00)
    {
      value = m_ram_size_reg;
    }
    else
    {
      return DoInvalidAccess(type, size, MEMCTRL2_BASE | offset, value);
    }

    return 2;
  }
  else
  {
    if (offset == 0x00)
    {
      m_ram_size_reg = value;
    }
    else
    {
      return DoInvalidAccess(type, size, MEMCTRL2_BASE | offset, value);
    }

    return 0;
  }
}

template<MemoryAccessType type, MemoryAccessSize size>
ALWAYS_INLINE static TickCount DoPadAccess(u32 offset, u32& value)
{
  if constexpr (type == MemoryAccessType::Read)
  {
    value = g_pad.ReadRegister(FIXUP_HALFWORD_OFFSET(size, offset));
    value = FIXUP_HALFWORD_READ_VALUE(size, offset, value);
    return 2;
  }
  else
  {
    g_pad.WriteRegister(FIXUP_HALFWORD_OFFSET(size, offset), FIXUP_HALFWORD_WRITE_VALUE(size, offset, value));
    return 0;
  }
}

template<MemoryAccessType type, MemoryAccessSize size>
ALWAYS_INLINE static TickCount DoSIOAccess(u32 offset, u32& value)
{
  if constexpr (type == MemoryAccessType::Read)
  {
    value = g_sio.ReadRegister(FIXUP_HALFWORD_OFFSET(size, offset));
    value = FIXUP_HALFWORD_READ_VALUE(size, offset, value);
    return 2;
  }
  else
  {
    g_sio.WriteRegister(FIXUP_HALFWORD_OFFSET(size, offset), FIXUP_HALFWORD_WRITE_VALUE(size, offset, value));
    return 0;
  }
}

template<MemoryAccessType type, MemoryAccessSize size>
ALWAYS_INLINE static TickCount DoCDROMAccess(u32 offset, u32& value)
{
  if constexpr (type == MemoryAccessType::Read)
  {
    switch (size)
    {
      case MemoryAccessSize::Word:
      {
        const u32 b0 = ZeroExtend32(g_cdrom.ReadRegister(offset));
        const u32 b1 = ZeroExtend32(g_cdrom.ReadRegister(offset + 1u));
        const u32 b2 = ZeroExtend32(g_cdrom.ReadRegister(offset + 2u));
        const u32 b3 = ZeroExtend32(g_cdrom.ReadRegister(offset + 3u));
        value = b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
      }

      case MemoryAccessSize::HalfWord:
      {
        const u32 lsb = ZeroExtend32(g_cdrom.ReadRegister(offset));
        const u32 msb = ZeroExtend32(g_cdrom.ReadRegister(offset + 1u));
        value = lsb | (msb << 8);
      }

      case MemoryAccessSize::Byte:
      default:
        value = ZeroExtend32(g_cdrom.ReadRegister(offset));
    }

    return m_cdrom_access_time[static_cast<u32>(size)];
  }
  else
  {
    switch (size)
    {
      case MemoryAccessSize::Word:
      {
        g_cdrom.WriteRegister(offset, Truncate8(value & 0xFFu));
        g_cdrom.WriteRegister(offset + 1u, Truncate8((value >> 8) & 0xFFu));
        g_cdrom.WriteRegister(offset + 2u, Truncate8((value >> 16) & 0xFFu));
        g_cdrom.WriteRegister(offset + 3u, Truncate8((value >> 24) & 0xFFu));
      }
      break;

      case MemoryAccessSize::HalfWord:
      {
        g_cdrom.WriteRegister(offset, Truncate8(value & 0xFFu));
        g_cdrom.WriteRegister(offset + 1u, Truncate8((value >> 8) & 0xFFu));
      }
      break;

      case MemoryAccessSize::Byte:
      default:
        g_cdrom.WriteRegister(offset, Truncate8(value));
        break;
    }

    return 0;
  }
}

template<MemoryAccessType type, MemoryAccessSize size>
ALWAYS_INLINE static TickCount DoGPUAccess(u32 offset, u32& value)
{
  if constexpr (type == MemoryAccessType::Read)
  {
    value = g_gpu->ReadRegister(FIXUP_WORD_OFFSET(size, offset));
    value = FIXUP_WORD_READ_VALUE(size, offset, value);
    return 2;
  }
  else
  {
    g_gpu->WriteRegister(FIXUP_WORD_OFFSET(size, offset), FIXUP_WORD_WRITE_VALUE(size, offset, value));
    return 0;
  }
}

template<MemoryAccessType type, MemoryAccessSize size>
ALWAYS_INLINE static TickCount DoMDECAccess(u32 offset, u32& value)
{
  if constexpr (type == MemoryAccessType::Read)
  {
    value = g_mdec.ReadRegister(FIXUP_WORD_OFFSET(size, offset));
    value = FIXUP_WORD_READ_VALUE(size, offset, value);
    return 2;
  }
  else
  {
    g_mdec.WriteRegister(FIXUP_WORD_OFFSET(size, offset), FIXUP_WORD_WRITE_VALUE(size, offset, value));
    return 0;
  }
}

template<MemoryAccessType type, MemoryAccessSize size>
ALWAYS_INLINE static TickCount DoAccessInterruptController(u32 offset, u32& value)
{
  if constexpr (type == MemoryAccessType::Read)
  {
    value = g_interrupt_controller.ReadRegister(FIXUP_WORD_OFFSET(size, offset));
    value = FIXUP_WORD_READ_VALUE(size, offset, value);
    return 2;
  }
  else
  {
    g_interrupt_controller.WriteRegister(FIXUP_WORD_OFFSET(size, offset), FIXUP_WORD_WRITE_VALUE(size, offset, value));
    return 0;
  }
}

template<MemoryAccessType type, MemoryAccessSize size>
ALWAYS_INLINE static TickCount DoAccessTimers(u32 offset, u32& value)
{
  if constexpr (type == MemoryAccessType::Read)
  {
    value = g_timers.ReadRegister(FIXUP_WORD_OFFSET(size, offset));
    value = FIXUP_WORD_READ_VALUE(size, offset, value);
    return 2;
  }
  else
  {
    g_timers.WriteRegister(FIXUP_WORD_OFFSET(size, offset), FIXUP_WORD_WRITE_VALUE(size, offset, value));
    return 0;
  }
}

template<MemoryAccessType type, MemoryAccessSize size>
ALWAYS_INLINE static TickCount DoAccessSPU(u32 offset, u32& value)
{
  if constexpr (type == MemoryAccessType::Read)
  {
    switch (size)
    {
      case MemoryAccessSize::Word:
      {
        // 32-bit reads are read as two 16-bit accesses.
        const u16 lsb = g_spu.ReadRegister(offset);
        const u16 msb = g_spu.ReadRegister(offset + 2);
        value = ZeroExtend32(lsb) | (ZeroExtend32(msb) << 16);
      }
      break;

      case MemoryAccessSize::HalfWord:
      {
        value = ZeroExtend32(g_spu.ReadRegister(offset));
      }
      break;

      case MemoryAccessSize::Byte:
      default:
      {
        const u16 value16 = g_spu.ReadRegister(FIXUP_HALFWORD_OFFSET(size, offset));
        value = FIXUP_HALFWORD_READ_VALUE(size, offset, value16);
      }
      break;
    }

    return m_spu_access_time[static_cast<u32>(size)];
  }
  else
  {
    // 32-bit writes are written as two 16-bit writes.
    // TODO: Ignore if address is not aligned.
    switch (size)
    {
      case MemoryAccessSize::Word:
      {
        DebugAssert(Common::IsAlignedPow2(offset, 2));
        g_spu.WriteRegister(offset, Truncate16(value));
        g_spu.WriteRegister(offset + 2, Truncate16(value >> 16));
        break;
      }

      case MemoryAccessSize::HalfWord:
      {
        DebugAssert(Common::IsAlignedPow2(offset, 2));
        g_spu.WriteRegister(offset, Truncate16(value));
        break;
      }

      case MemoryAccessSize::Byte:
      {
        g_spu.WriteRegister(FIXUP_HALFWORD_OFFSET(size, offset),
                            Truncate16(FIXUP_HALFWORD_READ_VALUE(size, offset, value)));
        break;
      }
    }

    return 0;
  }
}

template<MemoryAccessType type, MemoryAccessSize size>
ALWAYS_INLINE static TickCount DoDMAAccess(u32 offset, u32& value)
{
  if constexpr (type == MemoryAccessType::Read)
  {
    value = g_dma.ReadRegister(FIXUP_WORD_OFFSET(size, offset));
    value = FIXUP_WORD_READ_VALUE(size, offset, value);
    return 2;
  }
  else
  {
    g_dma.WriteRegister(FIXUP_WORD_OFFSET(size, offset), FIXUP_WORD_WRITE_VALUE(size, offset, value));
    return 0;
  }
}

} // namespace Bus

namespace CPU {

template<bool add_ticks, bool icache_read = false, u32 word_count = 1, bool raise_exceptions>
ALWAYS_INLINE_RELEASE bool DoInstructionRead(PhysicalMemoryAddress address, void* data)
{
  using namespace Bus;

  address &= PHYSICAL_MEMORY_ADDRESS_MASK;

  if (address < RAM_MIRROR_END)
  {
    std::memcpy(data, &g_ram[address & g_ram_mask], sizeof(u32) * word_count);
    if constexpr (add_ticks)
      g_state.pending_ticks += (icache_read ? 1 : RAM_READ_TICKS) * word_count;

    return true;
  }
  else if (address >= BIOS_BASE && address < (BIOS_BASE + BIOS_SIZE))
  {
    std::memcpy(data, &g_bios[(address - BIOS_BASE) & BIOS_MASK], sizeof(u32) * word_count);
    if constexpr (add_ticks)
      g_state.pending_ticks += m_bios_access_time[static_cast<u32>(MemoryAccessSize::Word)] * word_count;

    return true;
  }
  else
  {
    if (raise_exceptions)
      CPU::RaiseException(address, Cop0Registers::CAUSE::MakeValueForException(Exception::IBE, false, false, 0));

    std::memset(data, 0, sizeof(u32) * word_count);
    return false;
  }
}

TickCount GetInstructionReadTicks(VirtualMemoryAddress address)
{
  using namespace Bus;

  address &= PHYSICAL_MEMORY_ADDRESS_MASK;

  if (address < RAM_MIRROR_END)
  {
    return RAM_READ_TICKS;
  }
  else if (address >= BIOS_BASE && address < (BIOS_BASE + BIOS_SIZE))
  {
    return m_bios_access_time[static_cast<u32>(MemoryAccessSize::Word)];
  }
  else
  {
    return 0;
  }
}

TickCount GetICacheFillTicks(VirtualMemoryAddress address)
{
  using namespace Bus;

  address &= PHYSICAL_MEMORY_ADDRESS_MASK;

  if (address < RAM_MIRROR_END)
  {
    return 1 * ((ICACHE_LINE_SIZE - (address & (ICACHE_LINE_SIZE - 1))) / sizeof(u32));
  }
  else if (address >= BIOS_BASE && address < (BIOS_BASE + BIOS_SIZE))
  {
    return m_bios_access_time[static_cast<u32>(MemoryAccessSize::Word)] *
           ((ICACHE_LINE_SIZE - (address & (ICACHE_LINE_SIZE - 1))) / sizeof(u32));
  }
  else
  {
    return 0;
  }
}

void CheckAndUpdateICacheTags(u32 line_count, TickCount uncached_ticks)
{
  VirtualMemoryAddress current_pc = g_state.regs.pc & ICACHE_TAG_ADDRESS_MASK;
  if (IsCachedAddress(current_pc))
  {
    TickCount ticks = 0;
    TickCount cached_ticks_per_line = GetICacheFillTicks(current_pc);
    for (u32 i = 0; i < line_count; i++, current_pc += ICACHE_LINE_SIZE)
    {
      const u32 line = GetICacheLine(current_pc);
      if (g_state.icache_tags[line] != current_pc)
      {
        g_state.icache_tags[line] = current_pc;
        ticks += cached_ticks_per_line;
      }
    }

    g_state.pending_ticks += ticks;
  }
  else
  {
    g_state.pending_ticks += uncached_ticks;
  }
}

u32 FillICache(VirtualMemoryAddress address)
{
  const u32 line = GetICacheLine(address);
  u8* line_data = &g_state.icache_data[line * ICACHE_LINE_SIZE];
  u32 line_tag;
  switch ((address >> 2) & 0x03u)
  {
    case 0:
      DoInstructionRead<true, true, 4, false>(address & ~(ICACHE_LINE_SIZE - 1u), line_data);
      line_tag = GetICacheTagForAddress(address);
      break;
    case 1:
      DoInstructionRead<true, true, 3, false>(address & (~(ICACHE_LINE_SIZE - 1u) | 0x4), line_data + 0x4);
      line_tag = GetICacheTagForAddress(address) | 0x1;
      break;
    case 2:
      DoInstructionRead<true, true, 2, false>(address & (~(ICACHE_LINE_SIZE - 1u) | 0x8), line_data + 0x8);
      line_tag = GetICacheTagForAddress(address) | 0x3;
      break;
    case 3:
    default:
      DoInstructionRead<true, true, 1, false>(address & (~(ICACHE_LINE_SIZE - 1u) | 0xC), line_data + 0xC);
      line_tag = GetICacheTagForAddress(address) | 0x7;
      break;
  }
  g_state.icache_tags[line] = line_tag;

  const u32 offset = GetICacheLineOffset(address);
  u32 result;
  std::memcpy(&result, &line_data[offset], sizeof(result));
  return result;
}

void ClearICache()
{
  std::memset(g_state.icache_data.data(), 0, ICACHE_SIZE);
  g_state.icache_tags.fill(ICACHE_INVALID_BITS);
}

ALWAYS_INLINE_RELEASE static u32 ReadICache(VirtualMemoryAddress address)
{
  const u32 line = GetICacheLine(address);
  const u8* line_data = &g_state.icache_data[line * ICACHE_LINE_SIZE];
  const u32 offset = GetICacheLineOffset(address);
  u32 result;
  std::memcpy(&result, &line_data[offset], sizeof(result));
  return result;
}

ALWAYS_INLINE_RELEASE static void WriteICache(VirtualMemoryAddress address, u32 value)
{
  const u32 line = GetICacheLine(address);
  const u32 offset = GetICacheLineOffset(address);
  g_state.icache_tags[line] = GetICacheTagForAddress(address) | ICACHE_INVALID_BITS;
  std::memcpy(&g_state.icache_data[line * ICACHE_LINE_SIZE + offset], &value, sizeof(value));
}

static void WriteCacheControl(u32 value)
{
  Log_DevPrintf("Cache control <- 0x%08X", value);
  g_state.cache_control.bits = value;
}

template<MemoryAccessType type, MemoryAccessSize size>
ALWAYS_INLINE static TickCount DoScratchpadAccess(PhysicalMemoryAddress address, u32& value)
{
  const PhysicalMemoryAddress cache_offset = address & DCACHE_OFFSET_MASK;
  if constexpr (size == MemoryAccessSize::Byte)
  {
    if constexpr (type == MemoryAccessType::Read)
      value = ZeroExtend32(g_state.dcache[cache_offset]);
    else
      g_state.dcache[cache_offset] = Truncate8(value);
  }
  else if constexpr (size == MemoryAccessSize::HalfWord)
  {
    if constexpr (type == MemoryAccessType::Read)
    {
      u16 temp;
      std::memcpy(&temp, &g_state.dcache[cache_offset], sizeof(temp));
      value = ZeroExtend32(temp);
    }
    else
    {
      u16 temp = Truncate16(value);
      std::memcpy(&g_state.dcache[cache_offset], &temp, sizeof(temp));
    }
  }
  else if constexpr (size == MemoryAccessSize::Word)
  {
    if constexpr (type == MemoryAccessType::Read)
      std::memcpy(&value, &g_state.dcache[cache_offset], sizeof(value));
    else
      std::memcpy(&g_state.dcache[cache_offset], &value, sizeof(value));
  }

  return 0;
}

template<MemoryAccessType type, MemoryAccessSize size>
static ALWAYS_INLINE TickCount DoMemoryAccess(VirtualMemoryAddress address, u32& value)
{
  using namespace Bus;

  switch (address >> 29)
  {
    case 0x00: // KUSEG 0M-512M
    case 0x04: // KSEG0 - physical memory cached
    {
      if constexpr (type == MemoryAccessType::Write)
      {
        if (g_state.cop0_regs.sr.Isc)
        {
          WriteICache(address, value);
          return 0;
        }
      }

      address &= PHYSICAL_MEMORY_ADDRESS_MASK;
      if ((address & DCACHE_LOCATION_MASK) == DCACHE_LOCATION)
        return DoScratchpadAccess<type, size>(address, value);
    }
    break;

    case 0x01: // KUSEG 512M-1024M
    case 0x02: // KUSEG 1024M-1536M
    case 0x03: // KUSEG 1536M-2048M
    {
      // Above 512mb raises an exception.
      if constexpr (type == MemoryAccessType::Read)
        value = UINT32_C(0xFFFFFFFF);

      return -1;
    }

    case 0x05: // KSEG1 - physical memory uncached
    {
      address &= PHYSICAL_MEMORY_ADDRESS_MASK;
    }
    break;

    case 0x06: // KSEG2
    case 0x07: // KSEG2
    {
      if (address == 0xFFFE0130)
      {
        if constexpr (type == MemoryAccessType::Read)
          value = g_state.cache_control.bits;
        else
          WriteCacheControl(value);

        return 0;
      }
      else
      {
        if constexpr (type == MemoryAccessType::Read)
          value = UINT32_C(0xFFFFFFFF);

        return -1;
      }
    }
  }

  if (address < RAM_MIRROR_END)
  {
    return DoRAMAccess<type, size, false>(address, value);
  }
  else if (address >= BIOS_BASE && address < (BIOS_BASE + BIOS_SIZE))
  {
    return DoBIOSAccess<type, size>(static_cast<u32>(address - BIOS_BASE), value);
  }
  else if (address < EXP1_BASE)
  {
    return DoInvalidAccess(type, size, address, value);
  }
  else if (address < (EXP1_BASE + EXP1_SIZE))
  {
    return DoEXP1Access<type, size>(address & EXP1_MASK, value);
  }
  else if (address < MEMCTRL_BASE)
  {
    return DoInvalidAccess(type, size, address, value);
  }
  else if (address < (MEMCTRL_BASE + MEMCTRL_SIZE))
  {
    return DoMemoryControlAccess<type, size>(address & MEMCTRL_MASK, value);
  }
  else if (address < (PAD_BASE + PAD_SIZE))
  {
    return DoPadAccess<type, size>(address & PAD_MASK, value);
  }
  else if (address < (SIO_BASE + SIO_SIZE))
  {
    return DoSIOAccess<type, size>(address & SIO_MASK, value);
  }
  else if (address < (MEMCTRL2_BASE + MEMCTRL2_SIZE))
  {
    return DoMemoryControl2Access<type, size>(address & MEMCTRL2_MASK, value);
  }
  else if (address < (INTERRUPT_CONTROLLER_BASE + INTERRUPT_CONTROLLER_SIZE))
  {
    return DoAccessInterruptController<type, size>(address & INTERRUPT_CONTROLLER_MASK, value);
  }
  else if (address < (DMA_BASE + DMA_SIZE))
  {
    return DoDMAAccess<type, size>(address & DMA_MASK, value);
  }
  else if (address < (TIMERS_BASE + TIMERS_SIZE))
  {
    return DoAccessTimers<type, size>(address & TIMERS_MASK, value);
  }
  else if (address < CDROM_BASE)
  {
    return DoInvalidAccess(type, size, address, value);
  }
  else if (address < (CDROM_BASE + GPU_SIZE))
  {
    return DoCDROMAccess<type, size>(address & CDROM_MASK, value);
  }
  else if (address < (GPU_BASE + GPU_SIZE))
  {
    return DoGPUAccess<type, size>(address & GPU_MASK, value);
  }
  else if (address < (MDEC_BASE + MDEC_SIZE))
  {
    return DoMDECAccess<type, size>(address & MDEC_MASK, value);
  }
  else if (address < SPU_BASE)
  {
    return DoInvalidAccess(type, size, address, value);
  }
  else if (address < (SPU_BASE + SPU_SIZE))
  {
    return DoAccessSPU<type, size>(address & SPU_MASK, value);
  }
  else if (address < EXP2_BASE)
  {
    return DoInvalidAccess(type, size, address, value);
  }
  else if (address < (EXP2_BASE + EXP2_SIZE))
  {
    return DoEXP2Access<type, size>(address & EXP2_MASK, value);
  }
  else if (address < EXP3_BASE)
  {
    return DoUnknownEXPAccess<type>(address, value);
  }
  else if (address < (EXP3_BASE + EXP3_SIZE))
  {
    return DoEXP3Access<type>(address & EXP3_MASK, value);
  }
  else
  {
    return DoInvalidAccess(type, size, address, value);
  }
}

template<MemoryAccessType type, MemoryAccessSize size>
static bool DoAlignmentCheck(VirtualMemoryAddress address)
{
  if constexpr (size == MemoryAccessSize::HalfWord)
  {
    if (Common::IsAlignedPow2(address, 2))
      return true;
  }
  else if constexpr (size == MemoryAccessSize::Word)
  {
    if (Common::IsAlignedPow2(address, 4))
      return true;
  }
  else
  {
    return true;
  }

  g_state.cop0_regs.BadVaddr = address;
  RaiseException(type == MemoryAccessType::Read ? Exception::AdEL : Exception::AdES);
  return false;
}

bool FetchInstruction()
{
  DebugAssert(Common::IsAlignedPow2(g_state.regs.npc, 4));

  const PhysicalMemoryAddress address = g_state.regs.npc;
  switch (address >> 29)
  {
    case 0x00: // KUSEG 0M-512M
    case 0x04: // KSEG0 - physical memory cached
    {
#if 0
      DoInstructionRead<true, false, 1, false>(address, &g_state.next_instruction.bits);
#else
      if (CompareICacheTag(address))
        g_state.next_instruction.bits = ReadICache(address);
      else
        g_state.next_instruction.bits = FillICache(address);
#endif
    }
    break;

    case 0x05: // KSEG1 - physical memory uncached
    {
      if (!DoInstructionRead<true, false, 1, true>(address, &g_state.next_instruction.bits))
        return false;
    }
    break;

    case 0x01: // KUSEG 512M-1024M
    case 0x02: // KUSEG 1024M-1536M
    case 0x03: // KUSEG 1536M-2048M
    case 0x06: // KSEG2
    case 0x07: // KSEG2
    default:
    {
      CPU::RaiseException(Cop0Registers::CAUSE::MakeValueForException(Exception::IBE,
                                                                      g_state.current_instruction_in_branch_delay_slot,
                                                                      g_state.current_instruction_was_branch_taken, 0),
                          address);
      return false;
    }
  }

  g_state.regs.pc = g_state.regs.npc;
  g_state.regs.npc += sizeof(g_state.next_instruction.bits);
  return true;
}

bool FetchInstructionForInterpreterFallback()
{
  DebugAssert(Common::IsAlignedPow2(g_state.regs.npc, 4));

  const PhysicalMemoryAddress address = g_state.regs.npc;
  switch (address >> 29)
  {
    case 0x00: // KUSEG 0M-512M
    case 0x04: // KSEG0 - physical memory cached
    case 0x05: // KSEG1 - physical memory uncached
    {
      // We don't use the icache when doing interpreter fallbacks, because it's probably stale.
      if (!DoInstructionRead<false, false, 1, true>(address, &g_state.next_instruction.bits))
        return false;
    }
    break;

    case 0x01: // KUSEG 512M-1024M
    case 0x02: // KUSEG 1024M-1536M
    case 0x03: // KUSEG 1536M-2048M
    case 0x06: // KSEG2
    case 0x07: // KSEG2
    default:
    {
      CPU::RaiseException(Cop0Registers::CAUSE::MakeValueForException(Exception::IBE,
                                                                      g_state.current_instruction_in_branch_delay_slot,
                                                                      g_state.current_instruction_was_branch_taken, 0),
                          address);
      return false;
    }
  }

  g_state.regs.pc = g_state.regs.npc;
  g_state.regs.npc += sizeof(g_state.next_instruction.bits);
  return true;
}

bool SafeReadInstruction(VirtualMemoryAddress addr, u32* value)
{
  switch (addr >> 29)
  {
    case 0x00: // KUSEG 0M-512M
    case 0x04: // KSEG0 - physical memory cached
    case 0x05: // KSEG1 - physical memory uncached
    {
      // TODO: Check icache.
      return DoInstructionRead<false, false, 1, false>(addr, value);
    }

    case 0x01: // KUSEG 512M-1024M
    case 0x02: // KUSEG 1024M-1536M
    case 0x03: // KUSEG 1536M-2048M
    case 0x06: // KSEG2
    case 0x07: // KSEG2
    default:
    {
      return false;
    }
  }
}

bool ReadMemoryByte(VirtualMemoryAddress addr, u8* value)
{
  u32 temp = 0;
  const TickCount cycles = DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::Byte>(addr, temp);
  *value = Truncate8(temp);
  if (cycles < 0)
  {
    RaiseException(Exception::DBE);
    return false;
  }

  g_state.pending_ticks += cycles;
  return true;
}

bool ReadMemoryHalfWord(VirtualMemoryAddress addr, u16* value)
{
  if (!DoAlignmentCheck<MemoryAccessType::Read, MemoryAccessSize::HalfWord>(addr))
    return false;

  u32 temp = 0;
  const TickCount cycles = DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::HalfWord>(addr, temp);
  *value = Truncate16(temp);
  if (cycles < 0)
  {
    RaiseException(Exception::DBE);
    return false;
  }

  g_state.pending_ticks += cycles;
  return true;
}

bool ReadMemoryWord(VirtualMemoryAddress addr, u32* value)
{
  if (!DoAlignmentCheck<MemoryAccessType::Read, MemoryAccessSize::Word>(addr))
    return false;

  const TickCount cycles = DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::Word>(addr, *value);
  if (cycles < 0)
  {
    RaiseException(Exception::DBE);
    return false;
  }

  g_state.pending_ticks += cycles;
  return true;
}

bool WriteMemoryByte(VirtualMemoryAddress addr, u32 value)
{
  const TickCount cycles = DoMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::Byte>(addr, value);
  if (cycles < 0)
  {
    RaiseException(Exception::DBE);
    return false;
  }

  DebugAssert(cycles == 0);
  return true;
}

bool WriteMemoryHalfWord(VirtualMemoryAddress addr, u32 value)
{
  if (!DoAlignmentCheck<MemoryAccessType::Write, MemoryAccessSize::HalfWord>(addr))
    return false;

  const TickCount cycles = DoMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::HalfWord>(addr, value);
  if (cycles < 0)
  {
    RaiseException(Exception::DBE);
    return false;
  }

  DebugAssert(cycles == 0);
  return true;
}

bool WriteMemoryWord(VirtualMemoryAddress addr, u32 value)
{
  if (!DoAlignmentCheck<MemoryAccessType::Write, MemoryAccessSize::Word>(addr))
    return false;

  const TickCount cycles = DoMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::Word>(addr, value);
  if (cycles < 0)
  {
    RaiseException(Exception::DBE);
    return false;
  }

  DebugAssert(cycles == 0);
  return true;
}

template<MemoryAccessType type, MemoryAccessSize size>
static ALWAYS_INLINE bool DoSafeMemoryAccess(VirtualMemoryAddress address, u32& value)
{
  using namespace Bus;

  switch (address >> 29)
  {
    case 0x00: // KUSEG 0M-512M
    case 0x04: // KSEG0 - physical memory cached
    {
      address &= PHYSICAL_MEMORY_ADDRESS_MASK;
      if ((address & DCACHE_LOCATION_MASK) == DCACHE_LOCATION)
      {
        DoScratchpadAccess<type, size>(address, value);
        return true;
      }
    }
    break;

    case 0x01: // KUSEG 512M-1024M
    case 0x02: // KUSEG 1024M-1536M
    case 0x03: // KUSEG 1536M-2048M
    case 0x06: // KSEG2
    case 0x07: // KSEG2
    {
      // Above 512mb raises an exception.
      return false;
    }

    case 0x05: // KSEG1 - physical memory uncached
    {
      address &= PHYSICAL_MEMORY_ADDRESS_MASK;
    }
    break;
  }

  if (address < RAM_MIRROR_END)
  {
    DoRAMAccess<type, size, true>(address, value);
    return true;
  }
  if constexpr (type == MemoryAccessType::Read)
  {
    if (address >= BIOS_BASE && address < (BIOS_BASE + BIOS_SIZE))
    {
      DoBIOSAccess<type, size>(static_cast<u32>(address - BIOS_BASE), value);
      return true;
    }
  }
  return false;
}

bool SafeReadMemoryByte(VirtualMemoryAddress addr, u8* value)
{
  u32 temp = 0;
  if (!DoSafeMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::Byte>(addr, temp))
    return false;

  *value = Truncate8(temp);
  return true;
}

bool SafeReadMemoryHalfWord(VirtualMemoryAddress addr, u16* value)
{
  if ((addr & 1) == 0)
  {
    u32 temp = 0;
    if (!DoSafeMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::HalfWord>(addr, temp))
      return false;

    *value = Truncate16(temp);
    return true;
  }

  u8 low, high;
  if (!SafeReadMemoryByte(addr, &low) || !SafeReadMemoryByte(addr + 1, &high))
    return false;

  *value = (ZeroExtend16(high) << 8) | ZeroExtend16(low);
  return true;
}

bool SafeReadMemoryWord(VirtualMemoryAddress addr, u32* value)
{
  if ((addr & 3) == 0)
    return DoSafeMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::Word>(addr, *value);

  u16 low, high;
  if (!SafeReadMemoryHalfWord(addr, &low) || !SafeReadMemoryHalfWord(addr + 2, &high))
    return false;

  *value = (ZeroExtend32(high) << 16) | ZeroExtend32(low);
  return true;
}

bool SafeWriteMemoryByte(VirtualMemoryAddress addr, u8 value)
{
  u32 temp = ZeroExtend32(value);
  return DoSafeMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::Byte>(addr, temp);
}

bool SafeWriteMemoryHalfWord(VirtualMemoryAddress addr, u16 value)
{
  if ((addr & 1) == 0)
  {
    u32 temp = ZeroExtend32(value);
    return DoSafeMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::HalfWord>(addr, temp);
  }

  return SafeWriteMemoryByte(addr, Truncate8(value)) && SafeWriteMemoryByte(addr + 1, Truncate8(value >> 8));
}

bool SafeWriteMemoryWord(VirtualMemoryAddress addr, u32 value)
{
  if ((addr & 3) == 0)
    return DoSafeMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::Word>(addr, value);

  return SafeWriteMemoryHalfWord(addr, Truncate16(value >> 16)) &&
         SafeWriteMemoryHalfWord(addr + 2, Truncate16(value >> 16));
}

void* GetDirectReadMemoryPointer(VirtualMemoryAddress address, MemoryAccessSize size, TickCount* read_ticks)
{
  using namespace Bus;

  const u32 seg = (address >> 29);
  if (seg != 0 && seg != 4 && seg != 5)
    return nullptr;

  const PhysicalMemoryAddress paddr = address & PHYSICAL_MEMORY_ADDRESS_MASK;
  if (paddr < RAM_MIRROR_END)
  {
    if (read_ticks)
      *read_ticks = RAM_READ_TICKS;

    return &g_ram[paddr & g_ram_mask];
  }

  if ((paddr & DCACHE_LOCATION_MASK) == DCACHE_LOCATION)
  {
    if (read_ticks)
      *read_ticks = 0;

    return &g_state.dcache[paddr & DCACHE_OFFSET_MASK];
  }

  if (paddr >= BIOS_BASE && paddr < (BIOS_BASE + BIOS_SIZE))
  {
    if (read_ticks)
      *read_ticks = m_bios_access_time[static_cast<u32>(size)];

    return &g_bios[paddr & BIOS_MASK];
  }

  return nullptr;
}

void* GetDirectWriteMemoryPointer(VirtualMemoryAddress address, MemoryAccessSize size)
{
  using namespace Bus;

  const u32 seg = (address >> 29);
  if (seg != 0 && seg != 4 && seg != 5)
    return nullptr;

  const PhysicalMemoryAddress paddr = address & PHYSICAL_MEMORY_ADDRESS_MASK;

#if 0
  // Not enabled until we can protect code regions.
  if (paddr < RAM_MIRROR_END)
    return &g_ram[paddr & RAM_MASK];
#endif

  if ((paddr & DCACHE_LOCATION_MASK) == DCACHE_LOCATION)
    return &g_state.dcache[paddr & DCACHE_OFFSET_MASK];

  return nullptr;
}

namespace Recompiler::Thunks {

u64 ReadMemoryByte(u32 address)
{
  u32 temp;
  const TickCount cycles = DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::Byte>(address, temp);
  if (cycles < 0)
    return static_cast<u64>(-static_cast<s64>(Exception::DBE));

  g_state.pending_ticks += cycles;
  return ZeroExtend64(temp);
}

u64 ReadMemoryHalfWord(u32 address)
{
  if (!Common::IsAlignedPow2(address, 2))
  {
    g_state.cop0_regs.BadVaddr = address;
    return static_cast<u64>(-static_cast<s64>(Exception::AdEL));
  }

  u32 temp;
  const TickCount cycles = DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::HalfWord>(address, temp);
  if (cycles < 0)
    return static_cast<u64>(-static_cast<s64>(Exception::DBE));

  g_state.pending_ticks += cycles;
  return ZeroExtend64(temp);
}

u64 ReadMemoryWord(u32 address)
{
  if (!Common::IsAlignedPow2(address, 4))
  {
    g_state.cop0_regs.BadVaddr = address;
    return static_cast<u64>(-static_cast<s64>(Exception::AdEL));
  }

  u32 temp;
  const TickCount cycles = DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::Word>(address, temp);
  if (cycles < 0)
    return static_cast<u64>(-static_cast<s64>(Exception::DBE));

  g_state.pending_ticks += cycles;
  return ZeroExtend64(temp);
}

u32 WriteMemoryByte(u32 address, u32 value)
{
  const TickCount cycles = DoMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::Byte>(address, value);
  if (cycles < 0)
    return static_cast<u32>(Exception::DBE);

  DebugAssert(cycles == 0);
  return 0;
}

u32 WriteMemoryHalfWord(u32 address, u32 value)
{
  if (!Common::IsAlignedPow2(address, 2))
  {
    g_state.cop0_regs.BadVaddr = address;
    return static_cast<u32>(Exception::AdES);
  }

  const TickCount cycles = DoMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::HalfWord>(address, value);
  if (cycles < 0)
    return static_cast<u32>(Exception::DBE);

  DebugAssert(cycles == 0);
  return 0;
}

u32 WriteMemoryWord(u32 address, u32 value)
{
  if (!Common::IsAlignedPow2(address, 4))
  {
    g_state.cop0_regs.BadVaddr = address;
    return static_cast<u32>(Exception::AdES);
  }

  const TickCount cycles = DoMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::Word>(address, value);
  if (cycles < 0)
    return static_cast<u32>(Exception::DBE);

  DebugAssert(cycles == 0);
  return 0;
}

u32 UncheckedReadMemoryByte(u32 address)
{
  u32 temp;
  g_state.pending_ticks += DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::Byte>(address, temp);
  return temp;
}

u32 UncheckedReadMemoryHalfWord(u32 address)
{
  u32 temp;
  g_state.pending_ticks += DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::HalfWord>(address, temp);
  return temp;
}

u32 UncheckedReadMemoryWord(u32 address)
{
  u32 temp;
  g_state.pending_ticks += DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::Word>(address, temp);
  return temp;
}

void UncheckedWriteMemoryByte(u32 address, u32 value)
{
  g_state.pending_ticks += DoMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::Byte>(address, value);
}

void UncheckedWriteMemoryHalfWord(u32 address, u32 value)
{
  g_state.pending_ticks += DoMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::HalfWord>(address, value);
}

void UncheckedWriteMemoryWord(u32 address, u32 value)
{
  g_state.pending_ticks += DoMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::Word>(address, value);
}

} // namespace Recompiler::Thunks

} // namespace CPU
