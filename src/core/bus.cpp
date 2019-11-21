#include "bus.h"
#include "YBaseLib/ByteStream.h"
#include "YBaseLib/Log.h"
#include "YBaseLib/MD5Digest.h"
#include "YBaseLib/String.h"
#include "cdrom.h"
#include "common/state_wrapper.h"
#include "cpu_code_cache.h"
#include "cpu_core.h"
#include "cpu_disasm.h"
#include "dma.h"
#include "gpu.h"
#include "interrupt_controller.h"
#include "mdec.h"
#include "pad.h"
#include "spu.h"
#include "timers.h"
#include <cstdio>
Log_SetChannel(Bus);

#define FIXUP_WORD_READ_OFFSET(offset) ((offset) & ~u32(3))
#define FIXUP_WORD_READ_VALUE(offset, value) ((value) >> (((offset)&u32(3)) * 8))

// Offset and value remapping for (w32) registers from nocash docs.
void FixupUnalignedWordAccessW32(u32& offset, u32& value)
{
  const u32 byte_offset = offset & u32(3);
  offset &= ~u32(3);
  value <<= byte_offset * 8;
}

Bus::Bus() = default;

Bus::~Bus() = default;

void Bus::Initialize(CPU::Core* cpu, CPU::CodeCache* cpu_code_cache, DMA* dma,
                     InterruptController* interrupt_controller, GPU* gpu, CDROM* cdrom, Pad* pad, Timers* timers,
                     SPU* spu, MDEC* mdec)
{
  m_cpu = cpu;
  m_cpu_code_cache = cpu_code_cache;
  m_dma = dma;
  m_interrupt_controller = interrupt_controller;
  m_gpu = gpu;
  m_cdrom = cdrom;
  m_pad = pad;
  m_timers = timers;
  m_spu = spu;
  m_mdec = mdec;
}

void Bus::Reset()
{
  m_ram.fill(static_cast<u8>(0));
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
  RecalculateMemoryTimings();
}

bool Bus::DoState(StateWrapper& sw)
{
  sw.Do(&m_exp1_access_time);
  sw.Do(&m_exp2_access_time);
  sw.Do(&m_bios_access_time);
  sw.Do(&m_cdrom_access_time);
  sw.Do(&m_spu_access_time);
  sw.DoBytes(m_ram.data(), m_ram.size());
  sw.DoBytes(m_bios.data(), m_bios.size());
  sw.DoArray(m_MEMCTRL.regs, countof(m_MEMCTRL.regs));
  sw.Do(&m_ram_size_reg);
  sw.Do(&m_tty_line_buffer);
  return !sw.HasError();
}

bool Bus::ReadByte(PhysicalMemoryAddress address, u8* value)
{
  u32 temp = 0;
  const bool result = DispatchAccess<MemoryAccessType::Read, MemoryAccessSize::Byte>(address, temp);
  *value = Truncate8(temp);
  return result;
}

bool Bus::ReadHalfWord(PhysicalMemoryAddress address, u16* value)
{
  u32 temp = 0;
  const bool result = DispatchAccess<MemoryAccessType::Read, MemoryAccessSize::HalfWord>(address, temp);
  *value = Truncate16(temp);
  return result;
}

bool Bus::ReadWord(PhysicalMemoryAddress address, u32* value)
{
  return DispatchAccess<MemoryAccessType::Read, MemoryAccessSize::Word>(address, *value);
}

bool Bus::WriteByte(PhysicalMemoryAddress address, u8 value)
{
  u32 temp = ZeroExtend32(value);
  return DispatchAccess<MemoryAccessType::Read, MemoryAccessSize::Byte>(address, temp);
}

bool Bus::WriteHalfWord(PhysicalMemoryAddress address, u16 value)
{
  u32 temp = ZeroExtend32(value);
  return DispatchAccess<MemoryAccessType::Read, MemoryAccessSize::HalfWord>(address, temp);
}

bool Bus::WriteWord(PhysicalMemoryAddress address, u32 value)
{
  return DispatchAccess<MemoryAccessType::Write, MemoryAccessSize::Word>(address, value);
}

TickCount Bus::ReadWords(PhysicalMemoryAddress address, u32* words, u32 word_count)
{
  if (address + (word_count * sizeof(u32)) > (RAM_BASE + RAM_SIZE))
  {
    // Not RAM, or RAM mirrors.
    TickCount total_ticks = 0;
    for (u32 i = 0; i < word_count; i++)
    {
      const TickCount ticks = DispatchAccess<MemoryAccessType::Read, MemoryAccessSize::Word>(address, words[i]);
      if (ticks < 0)
        return -1;

      total_ticks += ticks;
      address += sizeof(u32);
    }

    return total_ticks;
  }

  // DMA is using DRAM Hyper Page mode, allowing it to access DRAM rows at 1 clock cycle per word (effectively around 17
  // clks per 16 words, due to required row address loading, probably plus some further minimal overload due to refresh
  // cycles). This is making DMA much faster than CPU memory accesses (CPU DRAM access takes 1 opcode cycle plus 6
  // waitstates, ie. 7 cycles in total).
  std::memcpy(words, &m_ram[address], sizeof(u32) * word_count);
  return static_cast<TickCount>(word_count + ((word_count + 15) / 16));
}

TickCount Bus::WriteWords(PhysicalMemoryAddress address, const u32* words, u32 word_count)
{
  if (address + (word_count * sizeof(u32)) > (RAM_BASE + RAM_SIZE))
  {
    // Not RAM, or RAM mirrors.
    TickCount total_ticks = 0;
    for (u32 i = 0; i < word_count; i++)
    {
      u32 value = words[i];
      const TickCount ticks = DispatchAccess<MemoryAccessType::Write, MemoryAccessSize::Word>(address, value);
      if (ticks < 0)
        return -1;

      total_ticks += ticks;
      address += sizeof(u32);
    }

    return total_ticks;
  }

  std::memcpy(&m_ram[address], words, sizeof(u32) * word_count);
  return static_cast<TickCount>(word_count + ((word_count + 15) / 16));
}

void Bus::SetExpansionROM(std::vector<u8> data)
{
  m_exp1_rom = std::move(data);
}

void Bus::SetBIOS(const std::vector<u8>& image)
{
  if (image.size() != static_cast<u32>(BIOS_SIZE))
  {
    Panic("Incorrect BIOS image size");
    return;
  }

  std::copy(image.cbegin(), image.cend(), m_bios.begin());
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
  return std::tie(byte_access_time, halfword_access_time, word_access_time);
}

void Bus::RecalculateMemoryTimings()
{
  std::tie(m_bios_access_time[0], m_bios_access_time[1], m_bios_access_time[2]) =
    CalculateMemoryTiming(m_MEMCTRL.bios_delay_size, m_MEMCTRL.common_delay);
  std::tie(m_cdrom_access_time[0], m_cdrom_access_time[1], m_cdrom_access_time[2]) =
    CalculateMemoryTiming(m_MEMCTRL.cdrom_delay_size, m_MEMCTRL.common_delay);
  std::tie(m_spu_access_time[0], m_spu_access_time[1], m_spu_access_time[2]) =
    CalculateMemoryTiming(m_MEMCTRL.spu_delay_size, m_MEMCTRL.common_delay);

  Log_TracePrintf("BIOS Memory Timing: %u bit bus, byte=%d, halfword=%d, word=%d",
                  m_MEMCTRL.bios_delay_size.data_bus_16bit ? 16 : 8, m_bios_access_time[0], m_bios_access_time[1],
                  m_bios_access_time[2]);
  Log_TracePrintf("CDROM Memory Timing: %u bit bus, byte=%d, halfword=%d, word=%d",
                  m_MEMCTRL.cdrom_delay_size.data_bus_16bit ? 16 : 8, m_cdrom_access_time[0], m_cdrom_access_time[1],
                  m_cdrom_access_time[2]);
  Log_TracePrintf("SPU Memory Timing: %u bit bus, byte=%d, halfword=%d, word=%d",
                  m_MEMCTRL.spu_delay_size.data_bus_16bit ? 16 : 8, m_spu_access_time[0], m_spu_access_time[1],
                  m_spu_access_time[2]);
}

TickCount Bus::DoInvalidAccess(MemoryAccessType type, MemoryAccessSize size, PhysicalMemoryAddress address, u32& value)
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

  return 1;
}

u32 Bus::DoReadEXP1(MemoryAccessSize size, u32 offset)
{
  if (m_exp1_rom.empty())
  {
    // EXP1 not present.
    return UINT32_C(0xFFFFFFFF);
  }

  if (offset == 0x20018)
  {
    // Bit 0 - Action Replay On/Off
    return UINT32_C(1);
  }

  const u32 transfer_size = u32(1) << static_cast<u32>(size);
  if ((offset + transfer_size) > m_exp1_rom.size())
  {
    return UINT32_C(0);
  }

  u32 value;
  if (size == MemoryAccessSize::Byte)
  {
    value = ZeroExtend32(m_exp1_rom[offset]);
  }
  else if (size == MemoryAccessSize::HalfWord)
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
  return value;
}

void Bus::DoWriteEXP1(MemoryAccessSize size, u32 offset, u32 value)
{
  Log_WarningPrintf("EXP1 write: 0x%08X <- 0x%08X", EXP1_BASE | offset, value);
}

u32 Bus::DoReadEXP2(MemoryAccessSize size, u32 offset)
{
  // rx/tx buffer empty
  if (offset == 0x21)
  {
    return 0x04 | 0x08;
  }

  Log_WarningPrintf("EXP2 read: 0x%08X", EXP2_BASE | offset);
  return UINT32_C(0xFFFFFFFF);
}

void Bus::DoWriteEXP2(MemoryAccessSize size, u32 offset, u32 value)
{
  if (offset == 0x23)
  {
    if (value == '\r')
      return;

    if (value == '\n')
    {
      if (!m_tty_line_buffer.IsEmpty())
      {
        Log_InfoPrintf("TTY: %s", m_tty_line_buffer.GetCharArray());
#ifdef _DEBUG
        if (CPU::LOG_EXECUTION)
          CPU::WriteToExecutionLog("TTY: %s\n", m_tty_line_buffer.GetCharArray());
#endif
      }
      m_tty_line_buffer.Clear();
    }
    else
    {
      m_tty_line_buffer.AppendCharacter(Truncate8(value));
    }

    return;
  }

  if (offset == 0x41)
  {
    Log_WarningPrintf("BIOS POST status: %02X", value & UINT32_C(0x0F));
    return;
  }

  Log_WarningPrintf("EXP2 write: 0x%08X <- 0x%08X", EXP2_BASE | offset, value);
}

u32 Bus::DoReadMemoryControl(MemoryAccessSize size, u32 offset)
{
  u32 value = m_MEMCTRL.regs[offset / 4];
  FixupUnalignedWordAccessW32(offset, value);
  return value;
}

void Bus::DoWriteMemoryControl(MemoryAccessSize size, u32 offset, u32 value)
{
  FixupUnalignedWordAccessW32(offset, value);

  const u32 index = offset / 4;
  const u32 write_mask = (index == 8) ? COMDELAY::WRITE_MASK : MEMDELAY::WRITE_MASK;
  const u32 new_value = (m_MEMCTRL.regs[index] & ~write_mask) | (value & write_mask);
  if (m_MEMCTRL.regs[index] != new_value)
  {
    m_MEMCTRL.regs[index] = new_value;
    RecalculateMemoryTimings();
  }
}

u32 Bus::DoReadMemoryControl2(MemoryAccessSize size, u32 offset)
{
  if (offset == 0x00)
    return m_ram_size_reg;

  u32 value = 0;
  DoInvalidAccess(MemoryAccessType::Read, size, MEMCTRL2_BASE | offset, value);
  return value;
}

void Bus::DoWriteMemoryControl2(MemoryAccessSize size, u32 offset, u32 value)
{
  if (offset == 0x00)
  {
    m_ram_size_reg = value;
    return;
  }

  DoInvalidAccess(MemoryAccessType::Write, size, MEMCTRL2_BASE | offset, value);
}

u32 Bus::DoReadPad(MemoryAccessSize size, u32 offset)
{
  return m_pad->ReadRegister(offset);
}

void Bus::DoWritePad(MemoryAccessSize size, u32 offset, u32 value)
{
  m_pad->WriteRegister(offset, value);
}

u32 Bus::DoReadSIO(MemoryAccessSize size, u32 offset)
{
  Log_ErrorPrintf("SIO Read 0x%08X", offset);
  if (offset == 0x04)
    return 0x5;
  else
    return 0;
}

void Bus::DoWriteSIO(MemoryAccessSize size, u32 offset, u32 value)
{
  Log_ErrorPrintf("SIO Write 0x%08X <- 0x%08X", offset, value);
}

u32 Bus::DoReadCDROM(MemoryAccessSize size, u32 offset)
{
  // TODO: Splitting of half/word reads.
  Assert(size == MemoryAccessSize::Byte);
  return ZeroExtend32(m_cdrom->ReadRegister(offset));
}

void Bus::DoWriteCDROM(MemoryAccessSize size, u32 offset, u32 value)
{
  // TODO: Splitting of half/word reads.
  Assert(size == MemoryAccessSize::Byte);
  m_cdrom->WriteRegister(offset, Truncate8(value));
}

u32 Bus::DoReadGPU(MemoryAccessSize size, u32 offset)
{
  Assert(size == MemoryAccessSize::Word);
  return m_gpu->ReadRegister(offset);
}

void Bus::DoWriteGPU(MemoryAccessSize size, u32 offset, u32 value)
{
  Assert(size == MemoryAccessSize::Word);
  m_gpu->WriteRegister(offset, value);
}

u32 Bus::DoReadMDEC(MemoryAccessSize size, u32 offset)
{
  Assert(size == MemoryAccessSize::Word);
  return m_mdec->ReadRegister(offset);
}

void Bus::DoWriteMDEC(MemoryAccessSize size, u32 offset, u32 value)
{
  Assert(size == MemoryAccessSize::Word);
  m_mdec->WriteRegister(offset, value);
}

u32 Bus::DoReadInterruptController(MemoryAccessSize size, u32 offset)
{
  u32 value = m_interrupt_controller->ReadRegister(offset);
  FixupUnalignedWordAccessW32(offset, value);
  return value;
}

void Bus::DoWriteInterruptController(MemoryAccessSize size, u32 offset, u32 value)
{
  FixupUnalignedWordAccessW32(offset, value);
  m_interrupt_controller->WriteRegister(offset, value);
}

u32 Bus::DoReadTimers(MemoryAccessSize size, u32 offset)
{
  u32 value = m_timers->ReadRegister(offset);
  FixupUnalignedWordAccessW32(offset, value);
  return value;
}

void Bus::DoWriteTimers(MemoryAccessSize size, u32 offset, u32 value)
{
  FixupUnalignedWordAccessW32(offset, value);
  m_timers->WriteRegister(offset, value);
}

u32 Bus::DoReadSPU(MemoryAccessSize size, u32 offset)
{
  // 32-bit reads are read as two 16-bit accesses.
  if (size == MemoryAccessSize::Word)
  {
    const u16 lsb = m_spu->ReadRegister(offset);
    const u16 msb = m_spu->ReadRegister(offset + 2);
    return ZeroExtend32(lsb) | (ZeroExtend32(msb) << 16);
  }
  else
  {
    return ZeroExtend32(m_spu->ReadRegister(offset));
  }
}

void Bus::DoWriteSPU(MemoryAccessSize size, u32 offset, u32 value)
{
  // 32-bit writes are written as two 16-bit writes.
  // TODO: Ignore if address is not aligned.
  if (size == MemoryAccessSize::Word)
  {
    Assert(Common::IsAlignedPow2(offset, 2));
    m_spu->WriteRegister(offset, Truncate16(value));
    m_spu->WriteRegister(offset + 2, Truncate16(value >> 16));
    return;
  }

  Assert(Common::IsAlignedPow2(offset, 2));
  m_spu->WriteRegister(offset, Truncate16(value));
}

void Bus::DoInvalidateCodeCache(u32 page_index)
{
  m_cpu_code_cache->InvalidateBlocksWithPageIndex(page_index);
}

u32 Bus::DoReadDMA(MemoryAccessSize size, u32 offset)
{
  return FIXUP_WORD_READ_VALUE(offset, m_dma->ReadRegister(FIXUP_WORD_READ_OFFSET(offset)));
}

void Bus::DoWriteDMA(MemoryAccessSize size, u32 offset, u32 value)
{
  switch (size)
  {
    case MemoryAccessSize::Byte:
    case MemoryAccessSize::HalfWord:
    {
      // zero extend length register
      if ((offset & u32(0xF0)) < 7 && (offset & u32(0x0F)) == 0x4)
        value = ZeroExtend32(value);
      else
        FixupUnalignedWordAccessW32(offset, value);
    }

    default:
      break;
  }

  m_dma->WriteRegister(offset, value);
}
