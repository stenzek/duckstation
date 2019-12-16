#include "system.h"
#include "YBaseLib/AutoReleasePtr.h"
#include "YBaseLib/Log.h"
#include "bios.h"
#include "bus.h"
#include "cdrom.h"
#include "common/state_wrapper.h"
#include "controller.h"
#include "cpu_code_cache.h"
#include "cpu_core.h"
#include "dma.h"
#include "game_list.h"
#include "gpu.h"
#include "host_display.h"
#include "host_interface.h"
#include "interrupt_controller.h"
#include "mdec.h"
#include "memory_card.h"
#include "pad.h"
#include "sio.h"
#include "spu.h"
#include "timers.h"
#include <cstdio>
#include <imgui.h>
Log_SetChannel(System);

System::System(HostInterface* host_interface) : m_host_interface(host_interface)
{
  m_cpu = std::make_unique<CPU::Core>();
  m_cpu_code_cache = std::make_unique<CPU::CodeCache>();
  m_bus = std::make_unique<Bus>();
  m_dma = std::make_unique<DMA>();
  m_interrupt_controller = std::make_unique<InterruptController>();
  m_cdrom = std::make_unique<CDROM>();
  m_pad = std::make_unique<Pad>();
  m_timers = std::make_unique<Timers>();
  m_spu = std::make_unique<SPU>();
  m_mdec = std::make_unique<MDEC>();
  m_sio = std::make_unique<SIO>();
  m_region = host_interface->GetSettings().region;
  m_cpu_execution_mode = host_interface->GetSettings().cpu_execution_mode;
}

System::~System() = default;

bool System::IsPSExe(const char* filename)
{
  const StaticString filename_str(filename);
  return filename_str.EndsWith(".psexe", false) || filename_str.EndsWith(".exe", false);
}

std::unique_ptr<System> System::Create(HostInterface* host_interface)
{
  std::unique_ptr<System> system(new System(host_interface));
  if (!system->CreateGPU())
    return {};

  return system;
}

bool System::RecreateGPU()
{
  // save current state
  AutoReleasePtr<ByteStream> state_stream = ByteStream_CreateGrowableMemoryStream();
  StateWrapper sw(state_stream, StateWrapper::Mode::Write);
  const bool state_valid = m_gpu->DoState(sw);
  if (!state_valid)
    Log_ErrorPrintf("Failed to save old GPU state when switching renderers");

  // create new renderer
  m_gpu.reset();
  if (!CreateGPU())
  {
    Panic("Failed to recreate GPU");
    return false;
  }

  if (state_valid)
  {
    state_stream->SeekAbsolute(0);
    sw.SetMode(StateWrapper::Mode::Read);
    m_gpu->DoState(sw);
  }

  return true;
}

bool System::Boot(const char* filename)
{
  // Load CD image up and detect region.
  std::unique_ptr<CDImage> media;
  bool exe_boot = false;
  if (filename)
  {
    exe_boot = IsPSExe(filename);
    if (exe_boot)
    {
      if (m_region == ConsoleRegion::Auto)
      {
        Log_InfoPrintf("Defaulting to NTSC-U region for executable.");
        m_region = ConsoleRegion::NTSC_U;
      }
    }
    else
    {
      Log_InfoPrintf("Loading CD image '%s'...", filename);
      media = CDImage::Open(filename);
      if (!media)
      {
        m_host_interface->ReportError(SmallString::FromFormat("Failed to load CD image '%s'", filename));
        return false;
      }

      if (m_region == ConsoleRegion::Auto)
      {
        std::optional<ConsoleRegion> detected_region = GameList::GetRegionForImage(media.get());
        if (detected_region)
        {
          m_region = detected_region.value();
          Log_InfoPrintf("Auto-detected %s region for '%s'", Settings::GetConsoleRegionName(m_region), filename);
        }
        else
        {
          m_region = ConsoleRegion::NTSC_U;
          Log_WarningPrintf("Could not determine region for CD. Defaulting to %s.",
                            Settings::GetConsoleRegionName(m_region));
        }
      }
    }
  }
  else
  {
    // Default to NTSC for BIOS boot.
    if (m_region == ConsoleRegion::Auto)
      m_region = ConsoleRegion::NTSC_U;
  }

  // Load BIOS image.
  std::optional<BIOS::Image> bios_image = m_host_interface->GetBIOSImage(m_region);
  if (!bios_image)
  {
    m_host_interface->ReportError(
      TinyString::FromFormat("Failed to load %s BIOS", Settings::GetConsoleRegionName(m_region)));
    return false;
  }

  // Component setup.
  InitializeComponents();
  UpdateControllers();
  UpdateMemoryCards();
  Reset();

  // Enable tty by patching bios.
  const BIOS::Hash bios_hash = BIOS::GetHash(*bios_image);
  if (GetSettings().bios_patch_tty_enable)
    BIOS::PatchBIOSEnableTTY(*bios_image, bios_hash);

  // Load EXE late after BIOS.
  if (exe_boot && !LoadEXE(filename, *bios_image))
  {
    m_host_interface->ReportError(SmallString::FromFormat("Failed to load EXE file '%s'", filename));
    return false;
  }

  // Insert CD, and apply fastboot patch if enabled.
  m_cdrom->InsertMedia(std::move(media));
  if (m_cdrom->HasMedia() && GetSettings().bios_patch_fast_boot)
    BIOS::PatchBIOSFastBoot(*bios_image, bios_hash);

  // Load the patched BIOS up.
  m_bus->SetBIOS(*bios_image);

  // Good to go.
  return true;
}

void System::InitializeComponents()
{
  m_cpu->Initialize(m_bus.get());
  m_cpu_code_cache->Initialize(this, m_cpu.get(), m_bus.get(), m_cpu_execution_mode == CPUExecutionMode::Recompiler);
  m_bus->Initialize(m_cpu.get(), m_cpu_code_cache.get(), m_dma.get(), m_interrupt_controller.get(), m_gpu.get(),
                    m_cdrom.get(), m_pad.get(), m_timers.get(), m_spu.get(), m_mdec.get(), m_sio.get());

  m_dma->Initialize(this, m_bus.get(), m_interrupt_controller.get(), m_gpu.get(), m_cdrom.get(), m_spu.get(),
                    m_mdec.get());

  m_interrupt_controller->Initialize(m_cpu.get());

  m_cdrom->Initialize(this, m_dma.get(), m_interrupt_controller.get(), m_spu.get());
  m_pad->Initialize(this, m_interrupt_controller.get());
  m_timers->Initialize(this, m_interrupt_controller.get());
  m_spu->Initialize(this, m_dma.get(), m_interrupt_controller.get());
  m_mdec->Initialize(this, m_dma.get());
}

bool System::CreateGPU()
{
  switch (m_host_interface->GetSettings().gpu_renderer)
  {
    case GPURenderer::HardwareOpenGL:
      m_gpu = m_host_interface->GetDisplay()->GetRenderAPI() == HostDisplay::RenderAPI::OpenGLES ?
                GPU::CreateHardwareOpenGLESRenderer() :
                GPU::CreateHardwareOpenGLRenderer();
      break;

#ifdef WIN32
    case GPURenderer::HardwareD3D11:
      m_gpu = GPU::CreateHardwareD3D11Renderer();
      break;
#else
    case GPURenderer::HardwareD3D11:
      break;
#endif

    case GPURenderer::Software:
    default:
      m_gpu = GPU::CreateSoftwareRenderer();
      break;
  }

  if (!m_gpu || !m_gpu->Initialize(m_host_interface->GetDisplay(), this, m_dma.get(), m_interrupt_controller.get(),
                                   m_timers.get()))
  {
    Log_ErrorPrintf("Failed to initialize GPU, falling back to software");
    m_gpu.reset();
    m_host_interface->GetSettings().gpu_renderer = GPURenderer::Software;
    m_gpu = GPU::CreateSoftwareRenderer();
    if (!m_gpu->Initialize(m_host_interface->GetDisplay(), this, m_dma.get(), m_interrupt_controller.get(),
                           m_timers.get()))
    {
      return false;
    }
  }

  m_bus->SetGPU(m_gpu.get());
  m_dma->SetGPU(m_gpu.get());
  return true;
}

bool System::DoState(StateWrapper& sw)
{
  if (!sw.DoMarker("System"))
    return false;

  sw.Do(&m_frame_number);
  sw.Do(&m_internal_frame_number);
  sw.Do(&m_global_tick_counter);

  if (!sw.DoMarker("CPU") || !m_cpu->DoState(sw))
    return false;

  if (sw.IsReading())
    m_cpu_code_cache->Flush();

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

  if (!sw.DoMarker("SPU") || !m_spu->DoState(sw))
    return false;

  if (!sw.DoMarker("MDEC") || !m_mdec->DoState(sw))
    return false;

  if (!sw.DoMarker("SIO") || !m_sio->DoState(sw))
    return false;

  return !sw.HasError();
}

void System::Reset()
{
  m_cpu->Reset();
  m_cpu_code_cache->Flush();
  m_bus->Reset();
  m_dma->Reset();
  m_interrupt_controller->Reset();
  m_gpu->Reset();
  m_cdrom->Reset();
  m_pad->Reset();
  m_timers->Reset();
  m_spu->Reset();
  m_mdec->Reset();
  m_sio->Reset();
  m_frame_number = 1;
  m_internal_frame_number = 0;
  m_global_tick_counter = 0;
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
  // Duplicated to avoid branch in the while loop, as the downcount can be quite low at times.
  u32 current_frame_number = m_frame_number;
  if (m_cpu_execution_mode == CPUExecutionMode::Interpreter)
  {
    while (current_frame_number == m_frame_number)
    {
      m_cpu->Execute();
      Synchronize();
    }
  }
  else
  {
    while (current_frame_number == m_frame_number)
    {
      m_cpu_code_cache->Execute();
      Synchronize();
    }
  }
}

bool System::LoadEXE(const char* filename, std::vector<u8>& bios_image)
{
  std::FILE* fp = std::fopen(filename, "rb");
  if (!fp)
    return false;

  std::fseek(fp, 0, SEEK_END);
  const u32 file_size = static_cast<u32>(std::ftell(fp));
  std::fseek(fp, 0, SEEK_SET);

  BIOS::PSEXEHeader header;
  if (std::fread(&header, sizeof(header), 1, fp) != 1 || !BIOS::IsValidPSExeHeader(header, file_size))
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
    std::vector<u32> data_words((header.file_size + 3) / 4);
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
  const u32 r_pc = header.initial_pc;
  const u32 r_gp = header.initial_gp;
  const u32 r_sp = header.initial_sp_base + header.initial_sp_offset;
  const u32 r_fp = header.initial_sp_base + header.initial_sp_offset;
  return BIOS::PatchBIOSForEXE(bios_image, r_pc, r_gp, r_sp, r_fp);
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
  const TickCount pending_ticks = m_cpu->GetPendingTicks();
  if (pending_ticks == 0)
    return;

  m_cpu->ResetPendingTicks();
  m_cpu->ResetDowncount();

  m_global_tick_counter += static_cast<u32>(pending_ticks);

  m_gpu->Execute(pending_ticks);
  m_timers->Execute(pending_ticks);
  m_cdrom->Execute(pending_ticks);
  m_pad->Execute(pending_ticks);
  m_spu->Execute(pending_ticks);
  m_mdec->Execute(pending_ticks);
  m_dma->Execute(pending_ticks);
}

void System::SetDowncount(TickCount downcount)
{
  m_cpu->SetDowncount(downcount);
}

void System::StallCPU(TickCount ticks)
{
  m_cpu->AddPendingTicks(ticks);
}

Controller* System::GetController(u32 slot) const
{
  return m_pad->GetController(slot);
}

void System::UpdateControllers()
{
  const Settings& settings = m_host_interface->GetSettings();
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++) {
    m_pad->SetController(i, nullptr);

    const ControllerType type = settings.controller_types[i];
    if (type != ControllerType::None) {
      std::unique_ptr<Controller> controller = Controller::Create(type);
      if (controller)
        m_pad->SetController(i, std::move(controller));
    }
  }
}

void System::UpdateMemoryCards()
{
  const Settings& settings = m_host_interface->GetSettings();
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    m_pad->SetMemoryCard(i, nullptr);
    std::unique_ptr<MemoryCard> card = MemoryCard::Open(this, settings.memory_card_paths[i]);
    if (card)
      m_pad->SetMemoryCard(i, std::move(card));
  }
}

void System::UpdateCPUExecutionMode()
{
  m_cpu_execution_mode = GetSettings().cpu_execution_mode;
  m_cpu_code_cache->Flush();
  m_cpu_code_cache->SetUseRecompiler(m_cpu_execution_mode == CPUExecutionMode::Recompiler);
}

bool System::HasMedia() const
{
  return m_cdrom->HasMedia();
}

bool System::InsertMedia(const char* path)
{
  std::unique_ptr<CDImage> image = CDImage::Open(path);
  if (!image)
    return false;

  m_cdrom->InsertMedia(std::move(image));
  return true;
}

void System::RemoveMedia()
{
  m_cdrom->RemoveMedia();
}
