// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "achievements.h"
#include "cpu_code_cache.h"
#include "cpu_core.h"
#include "cpu_pgxp.h"
#include "fullscreen_ui.h"
#include "gpu.h"
#include "host.h"
#include "imgui_overlays.h"
#include "settings.h"
#include "spu.h"
#include "system.h"
#include "texture_replacements.h"

#include "util/gpu_device.h"
#include "util/input_manager.h"
#include "util/postprocessing.h"

#include "common/file_system.h"

#include "IconsFontAwesome5.h"

#include <cmath>

void Settings::SetDefaultHotkeyConfig(SettingsInterface& si)
{
  si.ClearSection("Hotkeys");

#ifndef __ANDROID__
  si.SetStringValue("Hotkeys", "FastForward", "Keyboard/Tab");
  si.SetStringValue("Hotkeys", "TogglePause", "Keyboard/Space");
  si.SetStringValue("Hotkeys", "Screenshot", "Keyboard/F10");
  si.SetStringValue("Hotkeys", "ToggleFullscreen", "Keyboard/F11");

  si.SetStringValue("Hotkeys", "OpenPauseMenu", "Keyboard/Escape");
  si.SetStringValue("Hotkeys", "LoadSelectedSaveState", "Keyboard/F1");
  si.SetStringValue("Hotkeys", "SaveSelectedSaveState", "Keyboard/F2");
  si.SetStringValue("Hotkeys", "SelectPreviousSaveStateSlot", "Keyboard/F3");
  si.SetStringValue("Hotkeys", "SelectNextSaveStateSlot", "Keyboard/F4");
#endif
}

static void HotkeyModifyResolutionScale(s32 increment)
{
  const u32 new_resolution_scale = std::clamp<u32>(
    static_cast<u32>(static_cast<s32>(g_settings.gpu_resolution_scale) + increment), 1, GPU::MAX_RESOLUTION_SCALE);
  if (new_resolution_scale == g_settings.gpu_resolution_scale)
    return;

  const Settings old_settings = g_settings;
  g_settings.gpu_resolution_scale = new_resolution_scale;

  if (System::IsValid())
  {
    g_gpu->RestoreDeviceContext();
    g_gpu->UpdateSettings(old_settings);
    System::ClearMemorySaveStates();
  }
}

static void HotkeyLoadStateSlot(bool global, s32 slot)
{
  if (!System::IsValid())
    return;

  if (!global && System::GetGameSerial().empty())
  {
    Host::AddKeyedOSDMessage("LoadState", TRANSLATE_NOOP("OSDMessage", "Cannot load state for game without serial."),
                             Host::OSD_ERROR_DURATION);
    return;
  }

  std::string path(global ? System::GetGlobalSaveStateFileName(slot) :
                            System::GetGameSaveStateFileName(System::GetGameSerial(), slot));
  if (!FileSystem::FileExists(path.c_str()))
  {
    Host::AddKeyedOSDMessage("LoadState",
                             fmt::format(TRANSLATE_NOOP("OSDMessage", "No save state found in slot {}."), slot),
                             Host::OSD_INFO_DURATION);
    return;
  }

  System::LoadState(path.c_str());
}

static void HotkeySaveStateSlot(bool global, s32 slot)
{
  if (!System::IsValid())
    return;

  if (!global && System::GetGameSerial().empty())
  {
    Host::AddKeyedOSDMessage("LoadState", TRANSLATE_NOOP("OSDMessage", "Cannot save state for game without serial."),
                             Host::OSD_ERROR_DURATION);
    return;
  }

  std::string path(global ? System::GetGlobalSaveStateFileName(slot) :
                            System::GetGameSaveStateFileName(System::GetGameSerial(), slot));
  System::SaveState(path.c_str(), g_settings.create_save_state_backups);
}

#ifndef __ANDROID__

static bool CanPause()
{
  static constexpr const float PAUSE_INTERVAL = 3.0f;
  static Common::Timer::Value s_last_pause_time = 0;

  if (!Achievements::IsHardcoreModeActive() || System::IsPaused())
    return true;

  const Common::Timer::Value time = Common::Timer::GetCurrentValue();
  const float delta = static_cast<float>(Common::Timer::ConvertValueToSeconds(time - s_last_pause_time));
  if (delta < PAUSE_INTERVAL)
  {
    Host::AddIconOSDMessage(
      "PauseCooldown", ICON_FA_CLOCK,
      fmt::format(TRANSLATE_FS("Hotkeys", "You cannot pause until another {:.1f} seconds have passed."),
                  PAUSE_INTERVAL - delta),
      Host::OSD_QUICK_DURATION);
    return false;
  }

  Host::RemoveKeyedOSDMessage("PauseCooldown");
  s_last_pause_time = time;

  return true;
}

#endif

BEGIN_HOTKEY_LIST(g_common_hotkeys)
#ifndef __ANDROID__
DEFINE_HOTKEY("OpenPauseMenu", TRANSLATE_NOOP("Hotkeys", "General"), TRANSLATE_NOOP("Hotkeys", "Open Pause Menu"),
              [](s32 pressed) {
                if (!pressed && CanPause())
                  FullscreenUI::OpenPauseMenu();
              })
#endif

DEFINE_HOTKEY("FastForward", TRANSLATE_NOOP("Hotkeys", "General"), TRANSLATE_NOOP("Hotkeys", "Fast Forward"),
              [](s32 pressed) {
                if (pressed < 0)
                  return;
                System::SetFastForwardEnabled(pressed > 0);
              })

DEFINE_HOTKEY("ToggleFastForward", TRANSLATE_NOOP("Hotkeys", "General"),
              TRANSLATE_NOOP("Hotkeys", "Toggle Fast Forward"), [](s32 pressed) {
                if (!pressed)
                  System::SetFastForwardEnabled(!System::IsFastForwardEnabled());
              })

DEFINE_HOTKEY("Turbo", TRANSLATE_NOOP("Hotkeys", "General"), TRANSLATE_NOOP("Hotkeys", "Turbo"), [](s32 pressed) {
  if (pressed < 0)
    return;
  System::SetTurboEnabled(pressed > 0);
})

DEFINE_HOTKEY("ToggleTurbo", TRANSLATE_NOOP("Hotkeys", "General"), TRANSLATE_NOOP("Hotkeys", "Toggle Turbo"),
              [](s32 pressed) {
                if (!pressed)
                  System::SetTurboEnabled(!System::IsTurboEnabled());
              })

#ifndef __ANDROID__
DEFINE_HOTKEY("ToggleFullscreen", TRANSLATE_NOOP("Hotkeys", "General"), TRANSLATE_NOOP("Hotkeys", "Toggle Fullscreen"),
              [](s32 pressed) {
                if (!pressed)
                  Host::SetFullscreen(!Host::IsFullscreen());
              })

DEFINE_HOTKEY("TogglePause", TRANSLATE_NOOP("Hotkeys", "General"), TRANSLATE_NOOP("Hotkeys", "Toggle Pause"),
              [](s32 pressed) {
                if (!pressed && CanPause())
                  System::PauseSystem(!System::IsPaused());
              })

DEFINE_HOTKEY("PowerOff", TRANSLATE_NOOP("Hotkeys", "General"), TRANSLATE_NOOP("Hotkeys", "Power Off System"),
              [](s32 pressed) {
                if (!pressed && CanPause())
                  Host::RequestSystemShutdown(true, g_settings.save_state_on_exit);
              })
#endif

DEFINE_HOTKEY("Screenshot", TRANSLATE_NOOP("Hotkeys", "General"), TRANSLATE_NOOP("Hotkeys", "Save Screenshot"),
              [](s32 pressed) {
                if (!pressed)
                  System::SaveScreenshot();
              })

#ifndef __ANDROID__
DEFINE_HOTKEY("OpenAchievements", TRANSLATE_NOOP("Hotkeys", "General"),
              TRANSLATE_NOOP("Hotkeys", "Open Achievement List"), [](s32 pressed) {
                if (!pressed && CanPause())
                  FullscreenUI::OpenAchievementsWindow();
              })

DEFINE_HOTKEY("OpenLeaderboards", TRANSLATE_NOOP("Hotkeys", "General"),
              TRANSLATE_NOOP("Hotkeys", "Open Leaderboard List"), [](s32 pressed) {
                if (!pressed && CanPause())
                  FullscreenUI::OpenLeaderboardsWindow();
              })
#endif

DEFINE_HOTKEY("Reset", TRANSLATE_NOOP("Hotkeys", "System"), TRANSLATE_NOOP("Hotkeys", "Reset System"), [](s32 pressed) {
  if (!pressed)
    Host::RunOnCPUThread(System::ResetSystem);
})

DEFINE_HOTKEY("ChangeDisc", TRANSLATE_NOOP("Hotkeys", "System"), TRANSLATE_NOOP("Hotkeys", "Change Disc"),
              [](s32 pressed) {
                if (!pressed && System::IsValid() && System::HasMediaSubImages())
                {
                  const u32 current = System::GetMediaSubImageIndex();
                  const u32 next = (current + 1) % System::GetMediaSubImageCount();
                  if (current != next)
                    Host::RunOnCPUThread([next]() { System::SwitchMediaSubImage(next); });
                }
              })

DEFINE_HOTKEY("SwapMemoryCards", TRANSLATE_NOOP("Hotkeys", "System"),
              TRANSLATE_NOOP("Hotkeys", "Swap Memory Card Slots"), [](s32 pressed) {
                if (!pressed)
                  System::SwapMemoryCards();
              })

#ifndef __ANDROID__
DEFINE_HOTKEY("FrameStep", TRANSLATE_NOOP("Hotkeys", "System"), TRANSLATE_NOOP("Hotkeys", "Frame Step"),
              [](s32 pressed) {
                if (!pressed)
                  System::DoFrameStep();
              })
#endif

DEFINE_HOTKEY("Rewind", TRANSLATE_NOOP("Hotkeys", "System"), TRANSLATE_NOOP("Hotkeys", "Rewind"), [](s32 pressed) {
  if (pressed < 0)
    return;
  System::SetRewindState(pressed > 0);
})

#ifndef __ANDROID__
DEFINE_HOTKEY("ToggleCheats", TRANSLATE_NOOP("Hotkeys", "System"), TRANSLATE_NOOP("Hotkeys", "Toggle Cheats"),
              [](s32 pressed) {
                if (!pressed)
                  System::DoToggleCheats();
              })
#else
DEFINE_HOTKEY("TogglePatchCodes", TRANSLATE_NOOP("Hotkeys", "System"), TRANSLATE_NOOP("Hotkeys", "Toggle Patch Codes"),
              [](s32 pressed) {
                if (!pressed)
                  System::DoToggleCheats();
              })
#endif

DEFINE_HOTKEY("ToggleOverclocking", TRANSLATE_NOOP("Hotkeys", "System"),
              TRANSLATE_NOOP("Hotkeys", "Toggle Clock Speed Control (Overclocking)"), [](s32 pressed) {
                if (!pressed && System::IsValid())
                {
                  g_settings.cpu_overclock_enable = !g_settings.cpu_overclock_enable;
                  g_settings.UpdateOverclockActive();
                  System::UpdateOverclock();

                  if (g_settings.cpu_overclock_enable)
                  {
                    const u32 percent = g_settings.GetCPUOverclockPercent();
                    const double clock_speed =
                      ((static_cast<double>(System::MASTER_CLOCK) * static_cast<double>(percent)) / 100.0) / 1000000.0;
                    Host::AddKeyedFormattedOSDMessage(
                      "ToggleOverclocking", 5.0f,
                      TRANSLATE("OSDMessage", "CPU clock speed control enabled (%u%% / %.3f MHz)."), percent,
                      clock_speed);
                  }
                  else
                  {
                    Host::AddKeyedFormattedOSDMessage(
                      "ToggleOverclocking", 5.0f,
                      TRANSLATE("OSDMessage", "CPU clock speed control disabled (%.3f MHz)."),
                      static_cast<double>(System::MASTER_CLOCK) / 1000000.0);
                  }
                }
              })

DEFINE_HOTKEY("IncreaseEmulationSpeed", TRANSLATE_NOOP("Hotkeys", "System"),
              TRANSLATE_NOOP("Hotkeys", "Increase Emulation Speed"), [](s32 pressed) {
                if (!pressed && System::IsValid())
                {
                  g_settings.emulation_speed += 0.1f;
                  System::UpdateSpeedLimiterState();
                  Host::AddKeyedFormattedOSDMessage("EmulationSpeedChange", 5.0f,
                                                    TRANSLATE("OSDMessage", "Emulation speed set to %u%%."),
                                                    static_cast<u32>(std::lround(g_settings.emulation_speed * 100.0f)));
                }
              })

DEFINE_HOTKEY("DecreaseEmulationSpeed", TRANSLATE_NOOP("Hotkeys", "System"),
              TRANSLATE_NOOP("Hotkeys", "Decrease Emulation Speed"), [](s32 pressed) {
                if (!pressed && System::IsValid())
                {
                  g_settings.emulation_speed = std::max(g_settings.emulation_speed - 0.1f, 0.1f);
                  System::UpdateSpeedLimiterState();
                  Host::AddKeyedFormattedOSDMessage("EmulationSpeedChange", 5.0f,
                                                    TRANSLATE("OSDMessage", "Emulation speed set to %u%%."),
                                                    static_cast<u32>(std::lround(g_settings.emulation_speed * 100.0f)));
                }
              })

DEFINE_HOTKEY("ResetEmulationSpeed", TRANSLATE_NOOP("Hotkeys", "System"),
              TRANSLATE_NOOP("Hotkeys", "Reset Emulation Speed"), [](s32 pressed) {
                if (!pressed && System::IsValid())
                {
                  g_settings.emulation_speed = Host::GetFloatSettingValue("Main", "EmulationSpeed", 1.0f);
                  System::UpdateSpeedLimiterState();
                  Host::AddKeyedFormattedOSDMessage("EmulationSpeedChange", 5.0f,
                                                    TRANSLATE("OSDMessage", "Emulation speed set to %u%%."),
                                                    static_cast<u32>(std::lround(g_settings.emulation_speed * 100.0f)));
                }
              })

DEFINE_HOTKEY("ToggleSoftwareRendering", TRANSLATE_NOOP("Hotkeys", "Graphics"),
              TRANSLATE_NOOP("Hotkeys", "Toggle Software Rendering"), [](s32 pressed) {
                if (!pressed && System::IsValid())
                  System::ToggleSoftwareRendering();
              })

DEFINE_HOTKEY("TogglePGXP", TRANSLATE_NOOP("Hotkeys", "Graphics"), TRANSLATE_NOOP("Hotkeys", "Toggle PGXP"),
              [](s32 pressed) {
                if (!pressed && System::IsValid())
                {
                  Settings old_settings = g_settings;
                  g_settings.gpu_pgxp_enable = !g_settings.gpu_pgxp_enable;
                  g_gpu->RestoreDeviceContext();
                  g_gpu->UpdateSettings(old_settings);
                  System::ClearMemorySaveStates();
                  Host::AddKeyedOSDMessage("TogglePGXP",
                                           g_settings.gpu_pgxp_enable ?
                                             TRANSLATE_STR("OSDMessage", "PGXP is now enabled.") :
                                             TRANSLATE_STR("OSDMessage", "PGXP is now disabled."),
                                           5.0f);

                  if (g_settings.gpu_pgxp_enable)
                    CPU::PGXP::Initialize();
                  else
                    CPU::PGXP::Shutdown();

                  // we need to recompile all blocks if pgxp is toggled on/off
                  CPU::CodeCache::Reset();

                  // need to swap interpreters
                  System::InterruptExecution();
                }
              })

DEFINE_HOTKEY("IncreaseResolutionScale", TRANSLATE_NOOP("Hotkeys", "Graphics"),
              TRANSLATE_NOOP("Hotkeys", "Increase Resolution Scale"), [](s32 pressed) {
                if (!pressed && System::IsValid())
                  HotkeyModifyResolutionScale(1);
              })

DEFINE_HOTKEY("DecreaseResolutionScale", TRANSLATE_NOOP("Hotkeys", "Graphics"),
              TRANSLATE_NOOP("Hotkeys", "Decrease Resolution Scale"), [](s32 pressed) {
                if (!pressed && System::IsValid())
                  HotkeyModifyResolutionScale(-1);
              })

DEFINE_HOTKEY("TogglePostProcessing", TRANSLATE_NOOP("Hotkeys", "Graphics"),
              TRANSLATE_NOOP("Hotkeys", "Toggle Post-Processing"), [](s32 pressed) {
                if (!pressed && System::IsValid())
                  PostProcessing::Toggle();
              })

DEFINE_HOTKEY("ReloadPostProcessingShaders", TRANSLATE_NOOP("Hotkeys", "Graphics"),
              TRANSLATE_NOOP("Hotkeys", "Reload Post Processing Shaders"), [](s32 pressed) {
                if (!pressed && System::IsValid())
                  PostProcessing::ReloadShaders();
              })

DEFINE_HOTKEY("ReloadTextureReplacements", TRANSLATE_NOOP("Hotkeys", "Graphics"),
              TRANSLATE_NOOP("Hotkeys", "Reload Texture Replacements"), [](s32 pressed) {
                if (!pressed && System::IsValid())
                {
                  Host::AddKeyedOSDMessage("ReloadTextureReplacements",
                                           TRANSLATE_STR("OSDMessage", "Texture replacements reloaded."), 10.0f);
                  g_texture_replacements.Reload();
                }
              })

DEFINE_HOTKEY("ToggleWidescreen", TRANSLATE_NOOP("Hotkeys", "Graphics"), TRANSLATE_NOOP("Hotkeys", "Toggle Widescreen"),
              [](s32 pressed) {
                if (!pressed)
                  System::ToggleWidescreen();
              })

DEFINE_HOTKEY("TogglePGXPDepth", TRANSLATE_NOOP("Hotkeys", "Graphics"),
              TRANSLATE_NOOP("Hotkeys", "Toggle PGXP Depth Buffer"), [](s32 pressed) {
                if (!pressed && System::IsValid())
                {
                  if (!g_settings.gpu_pgxp_enable)
                    return;

                  const Settings old_settings = g_settings;
                  g_settings.gpu_pgxp_depth_buffer = !g_settings.gpu_pgxp_depth_buffer;

                  g_gpu->RestoreDeviceContext();
                  g_gpu->UpdateSettings(old_settings);
                  System::ClearMemorySaveStates();
                  Host::AddKeyedOSDMessage("TogglePGXPDepth",
                                           g_settings.gpu_pgxp_depth_buffer ?
                                             TRANSLATE_STR("OSDMessage", "PGXP Depth Buffer is now enabled.") :
                                             TRANSLATE_STR("OSDMessage", "PGXP Depth Buffer is now disabled."),
                                           5.0f);
                }
              })

DEFINE_HOTKEY("TogglePGXPCPU", TRANSLATE_NOOP("Hotkeys", "Graphics"), TRANSLATE_NOOP("Hotkeys", "Toggle PGXP CPU Mode"),
              [](s32 pressed) {
                if (pressed && System::IsValid())
                {
                  if (!g_settings.gpu_pgxp_enable)
                    return;

                  const Settings old_settings = g_settings;
                  g_settings.gpu_pgxp_cpu = !g_settings.gpu_pgxp_cpu;

                  g_gpu->RestoreDeviceContext();
                  g_gpu->UpdateSettings(old_settings);
                  System::ClearMemorySaveStates();
                  Host::AddKeyedOSDMessage("TogglePGXPCPU",
                                           g_settings.gpu_pgxp_cpu ?
                                             TRANSLATE_STR("OSDMessage", "PGXP CPU mode is now enabled.") :
                                             TRANSLATE_STR("OSDMessage", "PGXP CPU mode is now disabled."),
                                           5.0f);

                  CPU::PGXP::Shutdown();
                  CPU::PGXP::Initialize();

                  // we need to recompile all blocks if pgxp is toggled on/off
                  CPU::CodeCache::Reset();

                  // need to swap interpreters
                  System::InterruptExecution();
                }
              })

DEFINE_HOTKEY("AudioMute", TRANSLATE_NOOP("Hotkeys", "Audio"), TRANSLATE_NOOP("Hotkeys", "Toggle Mute"),
              [](s32 pressed) {
                if (!pressed && System::IsValid())
                {
                  g_settings.audio_output_muted = !g_settings.audio_output_muted;
                  const s32 volume = System::GetAudioOutputVolume();
                  SPU::GetOutputStream()->SetOutputVolume(volume);
                  if (g_settings.audio_output_muted)
                  {
                    Host::AddIconOSDMessage("AudioControlHotkey", ICON_FA_VOLUME_MUTE,
                                            TRANSLATE_STR("OSDMessage", "Volume: Muted"), 5.0f);
                  }
                  else
                  {
                    Host::AddIconOSDMessage("AudioControlHotkey", ICON_FA_VOLUME_UP,
                                            fmt::format(TRANSLATE_FS("OSDMessage", "Volume: {}%"), volume), 5.0f);
                  }
                }
              })
DEFINE_HOTKEY("AudioCDAudioMute", TRANSLATE_NOOP("Hotkeys", "Audio"), TRANSLATE_NOOP("Hotkeys", "Toggle CD Audio Mute"),
              [](s32 pressed) {
                if (!pressed && System::IsValid())
                {
                  g_settings.cdrom_mute_cd_audio = !g_settings.cdrom_mute_cd_audio;
                  Host::AddIconOSDMessage(
                    "AudioControlHotkey", g_settings.cdrom_mute_cd_audio ? ICON_FA_VOLUME_MUTE : ICON_FA_VOLUME_UP,
                    g_settings.cdrom_mute_cd_audio ? TRANSLATE_STR("OSDMessage", "CD Audio Muted.") :
                                                     TRANSLATE_STR("OSDMessage", "CD Audio Unmuted."),
                    2.0f);
                }
              })
DEFINE_HOTKEY("AudioVolumeUp", TRANSLATE_NOOP("Hotkeys", "Audio"), TRANSLATE_NOOP("Hotkeys", "Volume Up"),
              [](s32 pressed) {
                if (!pressed && System::IsValid())
                {
                  g_settings.audio_output_muted = false;

                  const s32 volume = std::min<s32>(System::GetAudioOutputVolume() + 10, 100);
                  g_settings.audio_output_volume = volume;
                  g_settings.audio_fast_forward_volume = volume;
                  SPU::GetOutputStream()->SetOutputVolume(volume);
                  Host::AddIconOSDMessage("AudioControlHotkey", ICON_FA_VOLUME_UP,
                                          fmt::format(TRANSLATE_FS("OSDMessage", "Volume: {}%"), volume), 5.0f);
                }
              })
DEFINE_HOTKEY("AudioVolumeDown", TRANSLATE_NOOP("Hotkeys", "Audio"), TRANSLATE_NOOP("Hotkeys", "Volume Down"),
              [](s32 pressed) {
                if (!pressed && System::IsValid())
                {
                  g_settings.audio_output_muted = false;

                  const s32 volume = std::max<s32>(System::GetAudioOutputVolume() - 10, 0);
                  g_settings.audio_output_volume = volume;
                  g_settings.audio_fast_forward_volume = volume;
                  SPU::GetOutputStream()->SetOutputVolume(volume);
                  Host::AddIconOSDMessage("AudioControlHotkey", ICON_FA_VOLUME_DOWN,
                                          fmt::format(TRANSLATE_FS("OSDMessage", "Volume: {}%"), volume), 5.0f);
                }
              })

// NOTE: All save/load state hotkeys are deferred, because it can trigger setting reapply, which reloads bindings.
DEFINE_HOTKEY("LoadSelectedSaveState", TRANSLATE_NOOP("Hotkeys", "Save States"),
              TRANSLATE_NOOP("Hotkeys", "Load From Selected Slot"), [](s32 pressed) {
                if (!pressed)
                  Host::RunOnCPUThread(SaveStateSelectorUI::LoadCurrentSlot);
              })
DEFINE_HOTKEY("SaveSelectedSaveState", TRANSLATE_NOOP("Hotkeys", "Save States"),
              TRANSLATE_NOOP("Hotkeys", "Save To Selected Slot"), [](s32 pressed) {
                if (!pressed)
                  Host::RunOnCPUThread(SaveStateSelectorUI::SaveCurrentSlot);
              })
DEFINE_HOTKEY("SelectPreviousSaveStateSlot", TRANSLATE_NOOP("Hotkeys", "Save States"),
              TRANSLATE_NOOP("Hotkeys", "Select Previous Save Slot"), [](s32 pressed) {
                if (!pressed)
                  Host::RunOnCPUThread([]() { SaveStateSelectorUI::SelectPreviousSlot(true); });
              })
DEFINE_HOTKEY("SelectNextSaveStateSlot", TRANSLATE_NOOP("Hotkeys", "Save States"),
              TRANSLATE_NOOP("Hotkeys", "Select Next Save Slot"), [](s32 pressed) {
                if (!pressed)
                  Host::RunOnCPUThread([]() { SaveStateSelectorUI::SelectNextSlot(true); });
              })
DEFINE_HOTKEY("SaveStateAndSelectNextSlot", TRANSLATE_NOOP("Hotkeys", "Save States"),
              TRANSLATE_NOOP("Hotkeys", "Save State and Select Next Slot"), [](s32 pressed) {
                if (!pressed && System::IsValid())
                {
                  SaveStateSelectorUI::SaveCurrentSlot();
                  SaveStateSelectorUI::SelectNextSlot(false);
                }
              })

DEFINE_HOTKEY("UndoLoadState", TRANSLATE_NOOP("Hotkeys", "Save States"), TRANSLATE_NOOP("Hotkeys", "Undo Load State"),
              [](s32 pressed) {
                if (!pressed)
                  Host::RunOnCPUThread(System::UndoLoadState);
              })

#define MAKE_LOAD_STATE_HOTKEY(global, slot, name)                                                                     \
  DEFINE_HOTKEY(global ? "LoadGameState" #slot : "LoadGlobalState" #slot, TRANSLATE_NOOP("Hotkeys", "Save States"),    \
                name, [](s32 pressed) {                                                                                \
                  if (!pressed)                                                                                        \
                    Host::RunOnCPUThread([]() { HotkeyLoadStateSlot(global, slot); });                                 \
                })
#define MAKE_SAVE_STATE_HOTKEY(global, slot, name)                                                                     \
  DEFINE_HOTKEY(global ? "SaveGameState" #slot : "SaveGlobalState" #slot, TRANSLATE_NOOP("Hotkeys", "Save States"),    \
                name, [](s32 pressed) {                                                                                \
                  if (!pressed)                                                                                        \
                    Host::RunOnCPUThread([]() { HotkeySaveStateSlot(global, slot); });                                 \
                })

MAKE_LOAD_STATE_HOTKEY(false, 1, TRANSLATE_NOOP("Hotkeys", "Load Game State 1"))
MAKE_SAVE_STATE_HOTKEY(false, 1, TRANSLATE_NOOP("Hotkeys", "Save Game State 1"))
MAKE_LOAD_STATE_HOTKEY(false, 2, TRANSLATE_NOOP("Hotkeys", "Load Game State 2"))
MAKE_SAVE_STATE_HOTKEY(false, 2, TRANSLATE_NOOP("Hotkeys", "Save Game State 2"))
MAKE_LOAD_STATE_HOTKEY(false, 3, TRANSLATE_NOOP("Hotkeys", "Load Game State 3"))
MAKE_SAVE_STATE_HOTKEY(false, 3, TRANSLATE_NOOP("Hotkeys", "Save Game State 3"))
MAKE_LOAD_STATE_HOTKEY(false, 4, TRANSLATE_NOOP("Hotkeys", "Load Game State 4"))
MAKE_SAVE_STATE_HOTKEY(false, 4, TRANSLATE_NOOP("Hotkeys", "Save Game State 4"))
MAKE_LOAD_STATE_HOTKEY(false, 5, TRANSLATE_NOOP("Hotkeys", "Load Game State 5"))
MAKE_SAVE_STATE_HOTKEY(false, 5, TRANSLATE_NOOP("Hotkeys", "Save Game State 5"))
MAKE_LOAD_STATE_HOTKEY(false, 6, TRANSLATE_NOOP("Hotkeys", "Load Game State 6"))
MAKE_SAVE_STATE_HOTKEY(false, 6, TRANSLATE_NOOP("Hotkeys", "Save Game State 6"))
MAKE_LOAD_STATE_HOTKEY(false, 7, TRANSLATE_NOOP("Hotkeys", "Load Game State 7"))
MAKE_SAVE_STATE_HOTKEY(false, 7, TRANSLATE_NOOP("Hotkeys", "Save Game State 7"))
MAKE_LOAD_STATE_HOTKEY(false, 8, TRANSLATE_NOOP("Hotkeys", "Load Game State 8"))
MAKE_SAVE_STATE_HOTKEY(false, 8, TRANSLATE_NOOP("Hotkeys", "Save Game State 8"))
MAKE_LOAD_STATE_HOTKEY(false, 9, TRANSLATE_NOOP("Hotkeys", "Load Game State 9"))
MAKE_SAVE_STATE_HOTKEY(false, 9, TRANSLATE_NOOP("Hotkeys", "Save Game State 9"))
MAKE_LOAD_STATE_HOTKEY(false, 10, TRANSLATE_NOOP("Hotkeys", "Load Game State 10"))
MAKE_SAVE_STATE_HOTKEY(false, 10, TRANSLATE_NOOP("Hotkeys", "Save Game State 10"))

MAKE_LOAD_STATE_HOTKEY(true, 1, TRANSLATE_NOOP("Hotkeys", "Load Global State 1"))
MAKE_SAVE_STATE_HOTKEY(true, 1, TRANSLATE_NOOP("Hotkeys", "Save Global State 1"))
MAKE_LOAD_STATE_HOTKEY(true, 2, TRANSLATE_NOOP("Hotkeys", "Load Global State 2"))
MAKE_SAVE_STATE_HOTKEY(true, 2, TRANSLATE_NOOP("Hotkeys", "Save Global State 2"))
MAKE_LOAD_STATE_HOTKEY(true, 3, TRANSLATE_NOOP("Hotkeys", "Load Global State 3"))
MAKE_SAVE_STATE_HOTKEY(true, 3, TRANSLATE_NOOP("Hotkeys", "Save Global State 3"))
MAKE_LOAD_STATE_HOTKEY(true, 4, TRANSLATE_NOOP("Hotkeys", "Load Global State 4"))
MAKE_SAVE_STATE_HOTKEY(true, 4, TRANSLATE_NOOP("Hotkeys", "Save Global State 4"))
MAKE_LOAD_STATE_HOTKEY(true, 5, TRANSLATE_NOOP("Hotkeys", "Load Global State 5"))
MAKE_SAVE_STATE_HOTKEY(true, 5, TRANSLATE_NOOP("Hotkeys", "Save Global State 5"))
MAKE_LOAD_STATE_HOTKEY(true, 6, TRANSLATE_NOOP("Hotkeys", "Load Global State 6"))
MAKE_SAVE_STATE_HOTKEY(true, 6, TRANSLATE_NOOP("Hotkeys", "Save Global State 6"))
MAKE_LOAD_STATE_HOTKEY(true, 7, TRANSLATE_NOOP("Hotkeys", "Load Global State 7"))
MAKE_SAVE_STATE_HOTKEY(true, 7, TRANSLATE_NOOP("Hotkeys", "Save Global State 7"))
MAKE_LOAD_STATE_HOTKEY(true, 8, TRANSLATE_NOOP("Hotkeys", "Load Global State 8"))
MAKE_SAVE_STATE_HOTKEY(true, 8, TRANSLATE_NOOP("Hotkeys", "Save Global State 8"))
MAKE_LOAD_STATE_HOTKEY(true, 9, TRANSLATE_NOOP("Hotkeys", "Load Global State 9"))
MAKE_SAVE_STATE_HOTKEY(true, 9, TRANSLATE_NOOP("Hotkeys", "Save Global State 9"))
MAKE_LOAD_STATE_HOTKEY(true, 10, TRANSLATE_NOOP("Hotkeys", "Load Global State 10"))
MAKE_SAVE_STATE_HOTKEY(true, 10, TRANSLATE_NOOP("Hotkeys", "Save Global State 10"))

#undef MAKE_SAVE_STATE_HOTKEY
#undef MAKE_LOAD_STATE_HOTKEY

END_HOTKEY_LIST()
