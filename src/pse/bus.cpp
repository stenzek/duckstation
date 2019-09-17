#include "bus.h"
#include "YBaseLib/ByteStream.h"
#include "YBaseLib/Log.h"
#include "YBaseLib/String.h"
#include "cdrom.h"
#include "common/state_wrapper.h"
#include "cpu_core.h"
#include "cpu_disasm.h"
#include "dma.h"
#include "gpu.h"
#include "interrupt_controller.h"
#include <cstdio>
Log_SetChannel(Bus);

// Offset and value remapping for (w32) registers from nocash docs.
void FixupUnalignedWordAccessW32(u32& offset, u32& value)
{
  const u32 byte_offset = offset & u32(3);
  offset &= ~u32(3);
  value <<= byte_offset * 8;
}

Bus::Bus() = default;

Bus::~Bus() = default;

bool Bus::Initialize(CPU::Core* cpu, DMA* dma, InterruptController* interrupt_controller, GPU* gpu, CDROM* cdrom)
{
  if (!LoadBIOS())
    return false;

  m_cpu = cpu;
  m_dma = dma;
  m_interrupt_controller = interrupt_controller;
  m_gpu = gpu;
  m_cdrom = cdrom;
  return true;
}

void Bus::Reset()
{
  m_ram.fill(static_cast<u8>(0));
}

bool Bus::DoState(StateWrapper& sw)
{
  sw.DoBytes(m_ram.data(), m_ram.size());
  sw.DoBytes(m_bios.data(), m_bios.size());
  sw.Do(&m_tty_line_buffer);
  return !sw.HasError();
}

bool Bus::ReadByte(PhysicalMemoryAddress cpu_address, PhysicalMemoryAddress bus_address, u8* value)
{
  u32 temp = 0;
  const bool result = DispatchAccess<MemoryAccessType::Read, MemoryAccessSize::Byte>(cpu_address, bus_address, temp);
  *value = Truncate8(temp);
  return result;
}

bool Bus::ReadHalfWord(PhysicalMemoryAddress cpu_address, PhysicalMemoryAddress bus_address, u16* value)
{
  u32 temp = 0;
  const bool result =
    DispatchAccess<MemoryAccessType::Read, MemoryAccessSize::HalfWord>(cpu_address, bus_address, temp);
  *value = Truncate16(temp);
  return result;
}

bool Bus::ReadWord(PhysicalMemoryAddress cpu_address, PhysicalMemoryAddress bus_address, u32* value)
{
  return DispatchAccess<MemoryAccessType::Read, MemoryAccessSize::Word>(cpu_address, bus_address, *value);
}

bool Bus::WriteByte(PhysicalMemoryAddress cpu_address, PhysicalMemoryAddress bus_address, u8 value)
{
  u32 temp = ZeroExtend32(value);
  return DispatchAccess<MemoryAccessType::Read, MemoryAccessSize::Byte>(cpu_address, bus_address, temp);
}

bool Bus::WriteHalfWord(PhysicalMemoryAddress cpu_address, PhysicalMemoryAddress bus_address, u16 value)
{
  u32 temp = ZeroExtend32(value);
  return DispatchAccess<MemoryAccessType::Read, MemoryAccessSize::HalfWord>(cpu_address, bus_address, temp);
}

bool Bus::WriteWord(PhysicalMemoryAddress cpu_address, PhysicalMemoryAddress bus_address, u32 value)
{
  return DispatchAccess<MemoryAccessType::Write, MemoryAccessSize::Word>(cpu_address, bus_address, value);
}

void Bus::PatchBIOS(u32 address, u32 value, u32 mask /*= UINT32_C(0xFFFFFFFF)*/)
{
  const u32 phys_address = address & UINT32_C(0x1FFFFFFF);
  const u32 offset = phys_address - BIOS_BASE;
  Assert(phys_address >= BIOS_BASE && offset < BIOS_SIZE);

  u32 existing_value;
  std::memcpy(&existing_value, &m_bios[offset], sizeof(existing_value));
  u32 new_value = (existing_value & ~mask) | value;
  std::memcpy(&m_bios[offset], &new_value, sizeof(new_value));

  SmallString old_disasm, new_disasm;
  CPU::DisassembleInstruction(&old_disasm, address, existing_value);
  CPU::DisassembleInstruction(&new_disasm, address, new_value);
  Log_InfoPrintf("BIOS-Patch 0x%08X (+0x%X): 0x%08X %s -> %08X %s", address, offset, existing_value,
                 old_disasm.GetCharArray(), new_value, new_disasm.GetCharArray());
}

bool Bus::LoadBIOS()
{
  std::FILE* fp = std::fopen("SCPH1001.BIN", "rb");
  if (!fp)
    return false;

  std::fseek(fp, 0, SEEK_END);
  const u32 size = static_cast<u32>(std::ftell(fp));
  std::fseek(fp, 0, SEEK_SET);

  if (size != m_bios.size())
  {
    Log_ErrorPrintf("BIOS image mismatch, expecting %u bytes, got %u bytes", static_cast<u32>(m_bios.size()), size);
    std::fclose(fp);
    return false;
  }

  if (std::fread(m_bios.data(), 1, m_bios.size(), fp) != m_bios.size())
  {
    Log_ErrorPrintf("Failed to read BIOS image");
    std::fclose(fp);
    return false;
  }

  std::fclose(fp);

#if 1
  // Patch to enable TTY.
  PatchBIOS(BIOS_BASE + 0x6F0C, 0x24010001);
  PatchBIOS(BIOS_BASE + 0x6F14, 0xAF81A9C0);
#endif

  return true;
}

bool Bus::DoInvalidAccess(MemoryAccessType type, MemoryAccessSize size, PhysicalMemoryAddress cpu_address,
                          PhysicalMemoryAddress bus_address, u32& value)
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

  str.AppendFormattedString(" at address 0x%08X (virtual address 0x%08X)", bus_address, cpu_address);
  if (type == MemoryAccessType::Write)
    str.AppendFormattedString(" (value 0x%08X)", value);

  Log_ErrorPrint(str);
  if (type == MemoryAccessType::Read)
    value = UINT32_C(0xFFFFFFFF);

  return true;
}

bool Bus::ReadExpansionRegion2(MemoryAccessSize size, u32 offset, u32& value)
{
  offset &= EXP2_MASK;

  // rx/tx buffer empty
  if (offset == 0x21)
  {
    value = 0x04 | 0x08;
    return true;
  }

  return DoInvalidAccess(MemoryAccessType::Read, size, EXP2_BASE | offset, EXP2_BASE | offset, value);
}

bool Bus::WriteExpansionRegion2(MemoryAccessSize size, u32 offset, u32 value)
{
  offset &= EXP2_MASK;

  if (offset == 0x23)
  {
    if (value == '\r')
      return true;

    if (value == '\n')
    {
      if (!m_tty_line_buffer.IsEmpty())
        Log_InfoPrintf("TTY: %s", m_tty_line_buffer.GetCharArray());
      m_tty_line_buffer.Clear();
    }
    else
    {
      m_tty_line_buffer.AppendCharacter(Truncate8(value));
    }

    return true;
  }

  if (offset == 0x41)
  {
    Log_WarningPrintf("BIOS POST status: %02X", value & UINT32_C(0x0F));
    return true;
  }

  return DoInvalidAccess(MemoryAccessType::Write, size, EXP2_BASE | offset, EXP2_BASE | offset, value);
}

bool Bus::DoReadCDROM(MemoryAccessSize size, u32 offset, u32& value)
{
  // TODO: Splitting of half/word reads.
  Assert(size == MemoryAccessSize::Byte);
  value = ZeroExtend32(m_cdrom->ReadRegister(offset));
  return true;
}

bool Bus::DoWriteCDROM(MemoryAccessSize size, u32 offset, u32 value)
{
  // TODO: Splitting of half/word reads.
  Assert(size == MemoryAccessSize::Byte);
  m_cdrom->WriteRegister(offset, Truncate8(value));
  return true;
}

bool Bus::DoReadGPU(MemoryAccessSize size, u32 offset, u32& value)
{
  Assert(size == MemoryAccessSize::Word);
  value = m_gpu->ReadRegister(offset);
  return true;
}

bool Bus::DoWriteGPU(MemoryAccessSize size, u32 offset, u32 value)
{
  Assert(size == MemoryAccessSize::Word);
  m_gpu->WriteRegister(offset, value);
  return true;
}

bool Bus::DoReadInterruptController(MemoryAccessSize size, u32 offset, u32& value)
{
  FixupUnalignedWordAccessW32(offset, value);
  value = m_interrupt_controller->ReadRegister(offset);
  return true;
}

bool Bus::DoWriteInterruptController(MemoryAccessSize size, u32 offset, u32 value)
{
  FixupUnalignedWordAccessW32(offset, value);
  m_interrupt_controller->WriteRegister(offset, value);
  return true;
}

bool Bus::ReadSPU(MemoryAccessSize size, u32 offset, u32& value)
{
  if (offset == 0x1AE)
  {
    value = 0;
    return true;
  }

  // return DoInvalidAccess(MemoryAccessType::Write, size, SPU_BASE | offset, SPU_BASE | offset, value);
  value = 0;
  return true;
}

bool Bus::WriteSPU(MemoryAccessSize size, u32 offset, u32 value)
{
  // Transfer FIFO
  if (offset == 0x1A8)
    return true;

  // SPUCNT
  if (offset == 0x1AA)
    return true;

  // return DoInvalidAccess(MemoryAccessType::Write, size, SPU_BASE | offset, SPU_BASE | offset, value);
  return true;
}

bool Bus::DoReadDMA(MemoryAccessSize size, u32 offset, u32& value)
{
  Assert(size == MemoryAccessSize::Word);
  value = m_dma->ReadRegister(offset);
  return true;
}

bool Bus::DoWriteDMA(MemoryAccessSize size, u32 offset, u32 value)
{
  Assert(size == MemoryAccessSize::Word);
  m_dma->WriteRegister(offset, value);
  return true;
}
