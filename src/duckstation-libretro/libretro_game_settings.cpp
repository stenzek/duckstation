#include "libretro_game_settings.h"

std::unique_ptr<GameSettings::Entry> GetSettingsForGame(const std::string& game_code)
{
  std::unique_ptr<GameSettings::Entry> gs = std::make_unique<GameSettings::Entry>();

  if (game_code == "SLUS-00530")
  {
    gs->AddTrait(GameSettings::Trait::ForcePGXPCPUMode);
    return gs;
  }

  if (game_code == "SLUS-00634")
  {
    gs->AddTrait(GameSettings::Trait::ForcePGXPCPUMode);
    return gs;
  }

  if (game_code == "SLUS-00077")
  {
    gs->AddTrait(GameSettings::Trait::DisableUpscaling);
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SLPM-87089")
  {
    gs->AddTrait(GameSettings::Trait::ForceInterlacing);
    return gs;
  }

  if (game_code == "SLPS-03336")
  {
    gs->AddTrait(GameSettings::Trait::ForceInterlacing);
    return gs;
  }

  if (game_code == "SLUS-01260")
  {
    gs->AddTrait(GameSettings::Trait::ForceSoftwareRenderer);
    gs->AddTrait(GameSettings::Trait::ForceInterlacing);
    return gs;
  }

  if (game_code == "SLES-01211")
  {
    gs->AddTrait(GameSettings::Trait::ForceSoftwareRenderer);
    gs->AddTrait(GameSettings::Trait::ForceInterlacing);
    return gs;
  }

  if (game_code == "SLUS-01261")
  {
    gs->AddTrait(GameSettings::Trait::ForceSoftwareRenderer);
    gs->AddTrait(GameSettings::Trait::ForceInterlacing);
    return gs;
  }

  if (game_code == "SLES-02466")
  {
    gs->AddTrait(GameSettings::Trait::ForceSoftwareRenderer);
    gs->AddTrait(GameSettings::Trait::ForceInterlacing);
    return gs;
  }

  if (game_code == "SLES-00259")
  {
    gs->AddTrait(GameSettings::Trait::ForceSoftwareRenderer);
    gs->AddTrait(GameSettings::Trait::ForceInterlacing);
    return gs;
  }

  if (game_code == "SLES-00606")
  {
    gs->AddTrait(GameSettings::Trait::ForceSoftwareRenderer);
    gs->AddTrait(GameSettings::Trait::ForceInterlacing);
    return gs;
  }

  if (game_code == "SLUS-00639")
  {
    gs->AddTrait(GameSettings::Trait::ForceSoftwareRenderer);
    gs->AddTrait(GameSettings::Trait::ForceInterlacing);
    return gs;
  }

  if (game_code == "SLUS-90039")
  {
    gs->AddTrait(GameSettings::Trait::ForceSoftwareRenderer);
    gs->AddTrait(GameSettings::Trait::ForceInterlacing);
    return gs;
  }

  if (game_code == "SLUS-00337")
  {
    gs->AddTrait(GameSettings::Trait::ForceInterlacing);
    return gs;
  }

  if (game_code == "SLUS-00606")
  {
    gs->AddTrait(GameSettings::Trait::ForceInterlacing);
    return gs;
  }

  if (game_code == "SLPS-03553")
  {
    gs->AddTrait(GameSettings::Trait::ForceInterlacing);
    return gs;
  }

  if (game_code == "SLPS-01211")
  {
    gs->AddTrait(GameSettings::Trait::ForceInterlacing);
    return gs;
  }

  if (game_code == "SLUS-00656")
  {
    gs->AddTrait(GameSettings::Trait::ForceInterlacing);
    return gs;
  }

  if (game_code == "SLUS-00952")
  {
    gs->AddTrait(GameSettings::Trait::ForceInterlacing);
    return gs;
  }

  if (game_code == "SLUS-01222")
  {
    gs->display_active_start_offset = 64;
    gs->display_active_end_offset = 68;
    return gs;
  }

  if (game_code == "SLUS-00297")
  {
    gs->AddTrait(GameSettings::Trait::DisableUpscaling);
    gs->AddTrait(GameSettings::Trait::DisablePGXP);
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SCUS-94350")
  {
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SCUS-94900")
  {
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "PCPX-96085")
  {
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SLUS-00590")
  {
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SLUS-00403")
  {
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SCUS-94300")
  {
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SLUS-00214")
  {
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SLUS-00204")
  {
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SLUS-00006")
  {
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SLUS-00213")
  {
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SCES-00344")
  {
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SLUS-00355")
  {
    gs->AddTrait(GameSettings::Trait::DisableUpscaling);
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SLUS-00331")
  {
    gs->AddTrait(GameSettings::Trait::DisableUpscaling);
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SLUS-00106")
  {
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SLUS-00005")
  {
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SLUS-01265")
  {
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SLUS-00601")
  {
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SLPS-00435")
  {
    gs->AddTrait(GameSettings::Trait::ForceRecompilerICache);
    return gs;
  }

  if (game_code == "SLUS-00388")
  {
    gs->AddTrait(GameSettings::Trait::ForceRecompilerICache);
    return gs;
  }

  if (game_code == "SCES-02834")
  {
    gs->AddTrait(GameSettings::Trait::ForceRecompilerICache);
    return gs;
  }

  if (game_code == "SLUS-00870")
  {
    gs->AddTrait(GameSettings::Trait::ForceInterpreter);
    return gs;
  }

  if (game_code == "SLUS-00183")
  {
    gs->AddTrait(GameSettings::Trait::ForceRecompilerICache);
    return gs;
  }

  if (game_code == "SLES-00483")
  {
    gs->AddTrait(GameSettings::Trait::ForceInterlacing);
    return gs;
  }

  if (game_code == "SLPS-02361")
  {
    gs->AddTrait(GameSettings::Trait::ForcePGXPVertexCache);
    return gs;
  }

  if (game_code == "SLPM-86023")
  {
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SLUS-00067")
  {
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SLES-00524")
  {
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SLPS-00712")
  {
    gs->AddTrait(GameSettings::Trait::ForceRecompilerICache);
    return gs;
  }

  if (game_code == "SLPS-01434")
  {
    gs->AddTrait(GameSettings::Trait::ForceInterlacing);
    return gs;
  }

  if (game_code == "SLUS-00684")
  {
    gs->AddTrait(GameSettings::Trait::ForceInterpreter);
    return gs;
  }

  if (game_code == "SLPS-02459")
  {
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SLPM-86750")
  {
    gs->AddTrait(GameSettings::Trait::ForceInterlacing);
    return gs;
  }

  if (game_code == "SLPS-02120")
  {
    gs->AddTrait(GameSettings::Trait::ForceInterlacing);
    return gs;
  }

  if (game_code == "SLUS-00102")
  {
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SLUS-00152")
  {
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SLUS-00603")
  {
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SLUS-00348")
  {
    gs->AddTrait(GameSettings::Trait::DisableUpscaling);
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SLUS-00042")
  {
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SLUS-00561")
  {
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SLUS-00035")
  {
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SLUS-00057")
  {
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SLUS-00014")
  {
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SCUS-94403")
  {
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SLUS-00549")
  {
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SLUS-00240")
  {
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SLUS-00027")
  {
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SLUS-00119")
  {
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SLUS-00224")
  {
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SLUS-00453")
  {
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SLUS-00753")
  {
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SLUS-00811")
  {
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SLUS-00208")
  {
    gs->display_active_start_offset = -62;
    gs->display_active_end_offset = 72;
    return gs;
  }

  if (game_code == "SLPS-01762")
  {
    gs->AddTrait(GameSettings::Trait::DisablePGXPCulling);
    return gs;
  }

  if (game_code == "SLPS-01567")
  {
    gs->display_active_start_offset = -62;
    gs->display_active_end_offset = 51;
    return gs;
  }

  if (game_code == "SLPS-00360")
  {
    gs->display_active_start_offset = -62;
    gs->display_active_end_offset = 72;
    return gs;
  }

  if (game_code == "SCES-02835")
  {
    gs->AddTrait(GameSettings::Trait::ForceInterpreter);
    gs->AddTrait(GameSettings::Trait::ForcePGXPCPUMode);
    return gs;
  }

  if (game_code == "SCES-02104")
  {
    gs->AddTrait(GameSettings::Trait::ForceInterpreter);
    gs->AddTrait(GameSettings::Trait::ForcePGXPCPUMode);
    return gs;
  }

  if (game_code == "SCES-01438")
  {
    gs->AddTrait(GameSettings::Trait::DisablePGXPCulling);
    gs->AddTrait(GameSettings::Trait::ForcePGXPCPUMode);
    return gs;
  }

  if (game_code == "SCUS-94467")
  {
    gs->AddTrait(GameSettings::Trait::ForcePGXPCPUMode);
    return gs;
  }

  if (game_code == "SCUS-94425")
  {
    gs->AddTrait(GameSettings::Trait::ForcePGXPCPUMode);
    return gs;
  }

  if (game_code == "SCPS-10085")
  {
    gs->AddTrait(GameSettings::Trait::ForcePGXPCPUMode);
    return gs;
  }

  if (game_code == "SCUS-94228")
  {
    gs->AddTrait(GameSettings::Trait::DisablePGXPCulling);
    gs->AddTrait(GameSettings::Trait::ForcePGXPCPUMode);
    return gs;
  }

  if (game_code == "SCUS-94290")
  {
    gs->AddTrait(GameSettings::Trait::ForcePGXPCPUMode);
    return gs;
  }

  if (game_code == "SLUS-01138")
  {
    gs->dma_max_slice_ticks = 200;
    gs->gpu_max_run_ahead = 1;
    return gs;
  }

  if (game_code == "SLPS-02376")
  {
    gs->dma_max_slice_ticks = 100;
    gs->gpu_max_run_ahead = 1;
    return gs;
  }

  if (game_code == "SLUS-00282")
  {
    gs->dma_max_slice_ticks = 200;
    gs->gpu_max_run_ahead = 1;
    return gs;
  }

  if (game_code == "SLUS-00022")
  {
    gs->AddTrait(GameSettings::Trait::DisableUpscaling);
    return gs;
  }

  if (game_code == "SLUS-00292")
  {
    gs->AddTrait(GameSettings::Trait::ForceRecompilerICache);
    return gs;
  }

  if (game_code == "SLUS-00522")
  {
    gs->dma_max_slice_ticks = 200;
    return gs;
  }

  if (game_code == "SLES-00469")
  {
    gs->dma_max_slice_ticks = 100;
    return gs;
  }

  if (game_code == "SLPS-01163")
  {
    gs->dma_max_slice_ticks = 100;
    return gs;
  }

  if (game_code == "SLUS-00498")
  {
    gs->dma_max_slice_ticks = 100;
    return gs;
  }

  if (game_code == "SLPS-00433")
  {
    gs->dma_max_slice_ticks = 100;
    return gs;
  }

  if (game_code == "SLUS-01029")
  {
    gs->AddTrait(GameSettings::Trait::DisableAnalogModeForcing);
    return gs;
  }

  if (game_code == "SLUS-00506")
  {
    gs->dma_max_slice_ticks = 100;
    return gs;
  }

  if (game_code == "SLES-00704")
  {
    gs->dma_max_slice_ticks = 100;
    return gs;
  }

  if (game_code == "SLPS-01399")
  {
    gs->dma_max_slice_ticks = 100;
    return gs;
  }

  if (game_code == "SLUS-00232")
  {
    gs->dma_max_slice_ticks = 100;
    return gs;
  }

  if (game_code == "SLES-00526")
  {
    gs->dma_max_slice_ticks = 100;
    return gs;
  }

  if (game_code == "SLED-00570")
  {
    gs->dma_max_slice_ticks = 100;
    return gs;
  }

  return {};
}
