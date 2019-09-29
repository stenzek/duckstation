#include "system.h"
#include "YBaseLib/Log.h"
#include "bus.h"
#include "cdrom.h"
#include "common/state_wrapper.h"
#include "cpu_core.h"
#include "dma.h"
#include "gpu.h"
#include "interrupt_controller.h"
#include "mdec.h"
#include "pad.h"
#include "pad_device.h"
#include "spu.h"
#include "timers.h"
#include <cstdio>
Log_SetChannel(System);

System::System(HostInterface* host_interface) : m_host_interface(host_interface)
{
  m_cpu = std::make_unique<CPU::Core>();
  m_bus = std::make_unique<Bus>();
  m_dma = std::make_unique<DMA>();
  m_interrupt_controller = std::make_unique<InterruptController>();
  // m_gpu = std::make_unique<GPU>();
  m_gpu = GPU::CreateHardwareOpenGLRenderer();
  m_cdrom = std::make_unique<CDROM>();
  m_pad = std::make_unique<Pad>();
  m_timers = std::make_unique<Timers>();
  m_spu = std::make_unique<SPU>();
  m_mdec = std::make_unique<MDEC>();
}

System::~System() = default;

bool System::Initialize()
{
  if (!m_cpu->Initialize(m_bus.get()))
    return false;

  if (!m_bus->Initialize(m_cpu.get(), m_dma.get(), m_interrupt_controller.get(), m_gpu.get(), m_cdrom.get(),
                         m_pad.get(), m_timers.get(), m_spu.get(), m_mdec.get()))
  {
    return false;
  }

  if (!m_dma->Initialize(this, m_bus.get(), m_interrupt_controller.get(), m_gpu.get(), m_cdrom.get(), m_spu.get(),
                         m_mdec.get()))
  {
    return false;
  }

  if (!m_interrupt_controller->Initialize(m_cpu.get()))
    return false;

  if (!m_gpu->Initialize(this, m_dma.get(), m_interrupt_controller.get(), m_timers.get()))
    return false;

  if (!m_cdrom->Initialize(this, m_dma.get(), m_interrupt_controller.get()))
    return false;

  if (!m_pad->Initialize(this, m_interrupt_controller.get()))
    return false;

  if (!m_timers->Initialize(this, m_interrupt_controller.get()))
    return false;

  if (!m_spu->Initialize(this, m_dma.get(), m_interrupt_controller.get()))
    return false;

  if (!m_mdec->Initialize(this, m_dma.get()))
    return false;

  return true;
}

bool System::DoState(StateWrapper& sw)
{
  if (!sw.DoMarker("CPU") || !m_cpu->DoState(sw))
    return false;

  if (!sw.DoMarker("Bus") || !m_bus->DoState(sw))
    return false;

  if (!sw.DoMarker("DMA") || !m_dma->DoState(sw))
    return false;

  if (!sw.DoMarker("InterruptController") || !m_interrupt_controller->DoState(sw))
    return false;

  if (!sw.DoMarker("GPU") || !m_gpu->DoState(sw))
    return false;

  if (!sw.DoMarker("CDROM") || !m_cdrom->DoState(sw))
    return false;

  if (!sw.DoMarker("Pad") || !m_pad->DoState(sw))
    return false;

  if (!sw.DoMarker("Timers") || !m_timers->DoState(sw))
    return false;

  if (!sw.DoMarker("SPU") || !m_timers->DoState(sw))
    return false;

  if (!sw.DoMarker("MDEC") || !m_mdec->DoState(sw))
    return false;

  return !sw.HasError();
}

void System::Reset()
{
  m_cpu->Reset();
  m_bus->Reset();
  m_dma->Reset();
  m_interrupt_controller->Reset();
  m_gpu->Reset();
  m_cdrom->Reset();
  m_pad->Reset();
  m_timers->Reset();
  m_spu->Reset();
  m_mdec->Reset();
  m_frame_number = 1;
}

bool System::LoadState(ByteStream* state)
{
  StateWrapper sw(state, StateWrapper::Mode::Read);
  return DoState(sw);
}

bool System::SaveState(ByteStream* state)
{
  StateWrapper sw(state, StateWrapper::Mode::Write);
  return DoState(sw);
}

void System::RunFrame()
{
  u32 current_frame_number = m_frame_number;
  while (current_frame_number == m_frame_number)
  {
    m_cpu->Execute();
    Synchronize();
  }
}

void System::RenderUI()
{
  m_gpu->RenderUI();
}

bool System::LoadEXE(const char* filename)
{
#pragma pack(push, 1)
  struct EXEHeader
  {
    char id[8];            // 0x000-0x007 PS-X EXE
    char pad1[8];          // 0x008-0x00F
    u32 initial_pc;        // 0x010
    u32 initial_gp;        // 0x014
    u32 load_address;      // 0x018
    u32 file_size;         // 0x01C excluding 0x800-byte header
    u32 unk0;              // 0x020
    u32 unk1;              // 0x024
    u32 memfill_start;     // 0x028
    u32 memfill_size;      // 0x02C
    u32 initial_sp_base;   // 0x030
    u32 initial_sp_offset; // 0x034
    u32 reserved[5];       // 0x038-0x04B
    char marker[0x7B4];    // 0x04C-0x7FF
  };
  static_assert(sizeof(EXEHeader) == 0x800);
#pragma pack(pop)

  std::FILE* fp = std::fopen(filename, "rb");
  if (!fp)
    return false;

  EXEHeader header;
  if (std::fread(&header, sizeof(header), 1, fp) != 1)
  {
    std::fclose(fp);
    return false;
  }

  if (header.memfill_size > 0)
  {
    const u32 words_to_write = header.memfill_size / 4;
    u32 address = header.memfill_start & ~UINT32_C(3);
    for (u32 i = 0; i < words_to_write; i++)
    {
      m_cpu->SafeWriteMemoryWord(address, 0);
      address += sizeof(u32);
    }
  }

  if (header.file_size >= 4)
  {
    std::vector<u32> data_words(header.file_size / 4);
    if (std::fread(data_words.data(), header.file_size, 1, fp) != 1)
    {
      std::fclose(fp);
      return false;
    }

    const u32 num_words = header.file_size / 4;
    u32 address = header.load_address;
    for (u32 i = 0; i < num_words; i++)
    {
      m_cpu->SafeWriteMemoryWord(address, data_words[i]);
      address += sizeof(u32);
    }
  }

  std::fclose(fp);

  // patch the BIOS to jump to the executable directly
  {
    const u32 r_pc = header.load_address;
    const u32 r_gp = header.initial_gp;
    const u32 r_sp = header.initial_sp_base;
    const u32 r_fp = header.initial_sp_base + header.initial_sp_offset;

    // pc has to be done first because we can't load it in the delay slot
    m_bus->PatchBIOS(0xBFC06FF0, UINT32_C(0x3C080000) | r_pc >> 16);                // lui $t0, (r_pc >> 16)
    m_bus->PatchBIOS(0xBFC06FF4, UINT32_C(0x35080000) | (r_pc & UINT32_C(0xFFFF))); // ori $t0, $t0, (r_pc & 0xFFFF)
    m_bus->PatchBIOS(0xBFC06FF8, UINT32_C(0x3C1C0000) | r_gp >> 16);                // lui $gp, (r_gp >> 16)
    m_bus->PatchBIOS(0xBFC06FFC, UINT32_C(0x379C0000) | (r_gp & UINT32_C(0xFFFF))); // ori $gp, $gp, (r_gp & 0xFFFF)
    m_bus->PatchBIOS(0xBFC07000, UINT32_C(0x3C1D0000) | r_sp >> 16);                // lui $sp, (r_sp >> 16)
    m_bus->PatchBIOS(0xBFC07004, UINT32_C(0x37BD0000) | (r_sp & UINT32_C(0xFFFF))); // ori $sp, $sp, (r_sp & 0xFFFF)
    m_bus->PatchBIOS(0xBFC07008, UINT32_C(0x3C1E0000) | r_fp >> 16);                // lui $fp, (r_fp >> 16)
    m_bus->PatchBIOS(0xBFC0700C, UINT32_C(0x01000008));                             // jr $t0
    m_bus->PatchBIOS(0xBFC07010, UINT32_C(0x37DE0000) | (r_fp & UINT32_C(0xFFFF))); // ori $fp, $fp, (r_fp & 0xFFFF)
  }

  return true;
}

bool System::SetExpansionROM(const char* filename)
{
  std::FILE* fp = std::fopen(filename, "rb");
  if (!fp)
  {
    Log_ErrorPrintf("Failed to open '%s'", filename);
    return false;
  }

  std::fseek(fp, 0, SEEK_END);
  const u32 size = static_cast<u32>(std::ftell(fp));
  std::fseek(fp, 0, SEEK_SET);

  std::vector<u8> data(size);
  if (std::fread(data.data(), size, 1, fp) != 1)
  {
    Log_ErrorPrintf("Failed to read ROM data from '%s'", filename);
    std::fclose(fp);
    return false;
  }

  std::fclose(fp);

  Log_InfoPrintf("Loaded expansion ROM from '%s': %u bytes", filename, size);
  m_bus->SetExpansionROM(std::move(data));
  return true;
}

void System::Synchronize()
{
  m_cpu->ResetDowncount();

  const TickCount pending_ticks = m_cpu->GetPendingTicks();
  m_cpu->ResetPendingTicks();

  m_gpu->Execute(pending_ticks);
  m_timers->AddSystemTicks(pending_ticks);
  m_cdrom->Execute(pending_ticks);
  m_pad->Execute(pending_ticks);
  m_dma->Execute(pending_ticks);
}

void System::SetDowncount(TickCount downcount)
{
  m_cpu->SetDowncount(downcount);
}

void System::SetController(u32 slot, std::shared_ptr<PadDevice> dev)
{
  m_pad->SetController(slot, std::move(dev));
}

void System::SetMemoryCard(u32 slot, std::shared_ptr<PadDevice> dev)
{
  m_pad->SetMemoryCard(slot, std::move(dev));
}

bool System::HasMedia() const
{
  return m_cdrom->HasMedia();
}

bool System::InsertMedia(const char* path)
{
  return m_cdrom->InsertMedia(path);
}

void System::RemoveMedia()
{
  m_cdrom->RemoveMedia();
}
