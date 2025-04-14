// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "achievements.h"
#include "cpu_code_cache.h"
#include "cpu_core.h"
#include "cpu_pgxp.h"
#include "fullscreen_ui.h"
#include "gpu.h"
#include "gpu_hw_texture_cache.h"
#include "gpu_presenter.h"
#include "gpu_thread.h"
#include "gte.h"
#include "host.h"
#include "imgui_overlays.h"
#include "settings.h"
#include "spu.h"
#include "system.h"

#include "util/gpu_device.h"
#include "util/input_manager.h"
#include "util/postprocessing.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/timer.h"

#include "IconsEmoji.h"
#include "IconsFontAwesome5.h"
#include "fmt/format.h"

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
  g_settings.gpu_resolution_scale = Truncate8(new_resolution_scale);

  if (System::IsValid())
  {
    GPUThread::UpdateSettings(true, false, false);
    System::ClearMemorySaveStates(true, false);
  }
}

static void HotkeyLoadStateSlot(bool global, s32 slot)
{
  if (!System::IsValid())
    return;

  if (!global && System::GetGameSerial().empty())
  {
    Host::AddKeyedOSDMessage("LoadState", TRANSLATE_STR("OSDMessage", "Cannot load state for game without serial."),
                             Host::OSD_ERROR_DURATION);
    return;
  }

  std::string path(global ? System::GetGlobalSaveStatePath(slot) :
                            System::GetGameSaveStatePath(System::GetGameSerial(), slot));
  if (!FileSystem::FileExists(path.c_str()))
  {
    Host::AddKeyedOSDMessage("LoadState",
                             fmt::format(TRANSLATE_FS("OSDMessage", "No save state found in slot {}."), slot),
                             Host::OSD_INFO_DURATION);
    return;
  }

  Error error;
  if (!System::LoadState(path.c_str(), &error, true, false))
  {
    Host::AddKeyedOSDMessage(
      "LoadState",
      fmt::format(TRANSLATE_FS("OSDMessage", "Failed to load state from slot {0}:\n{1}"), slot, error.GetDescription()),
      Host::OSD_ERROR_DURATION);
  }
}

static void HotkeySaveStateSlot(bool global, s32 slot)
{
  if (!System::IsValid())
    return;

  if (!global && System::GetGameSerial().empty())
  {
    Host::AddKeyedOSDMessage("SaveState", TRANSLATE_STR("OSDMessage", "Cannot save state for game without serial."),
                             Host::OSD_ERROR_DURATION);
    return;
  }

  std::string path(global ? System::GetGlobalSaveStatePath(slot) :
                            System::GetGameSaveStatePath(System::GetGameSerial(), slot));
  Error error;
  if (!System::SaveState(std::move(path), &error, g_settings.create_save_state_backups, false))
  {
    Host::AddIconOSDMessage(
      "SaveState", ICON_FA_EXCLAMATION_TRIANGLE,
      fmt::format(TRANSLATE_FS("OSDMessage", "Failed to save state to slot {0}:\n{1}"), slot, error.GetDescription()),
      Host::OSD_ERROR_DURATION);
  }
}

static void HotkeyToggleOSD()
{
  g_settings.display_show_fps ^= Host::GetBoolSettingValue("Display", "ShowFPS", false);
  g_settings.display_show_speed ^= Host::GetBoolSettingValue("Display", "ShowSpeed", false);
  g_settings.display_show_gpu_stats ^= Host::GetBoolSettingValue("Display", "ShowGPUStatistics", false);
  g_settings.display_show_resolution ^= Host::GetBoolSettingValue("Display", "ShowResolution", false);
  g_settings.display_show_latency_stats ^= Host::GetBoolSettingValue("Display", "ShowLatencyStatistics", false);
  g_settings.display_show_cpu_usage ^= Host::GetBoolSettingValue("Display", "ShowCPU", false);
  g_settings.display_show_gpu_usage ^= Host::GetBoolSettingValue("Display", "ShowGPU", false);
  g_settings.display_show_frame_times ^= Host::GetBoolSettingValue("Display", "ShowFrameTimes", false);
  g_settings.display_show_status_indicators ^= Host::GetBoolSettingValue("Display", "ShowStatusIndicators", true);
  g_settings.display_show_inputs ^= Host::GetBoolSettingValue("Display", "ShowInputs", false);
  g_settings.display_show_enhancements ^= Host::GetBoolSettingValue("Display", "ShowEnhancements", false);

  GPUThread::UpdateSettings(true, false, false);
}

#ifndef __ANDROID__

static bool CanPause()
{
  const u32 frames_until_pause_allowed = Achievements::GetPauseThrottleFrames();
  if (frames_until_pause_allowed == 0)
    return true;

  const float seconds = static_cast<float>(frames_until_pause_allowed) / System::GetVideoFrameRate();
  Host::AddIconOSDMessage("PauseCooldown", ICON_FA_CLOCK,
                          TRANSLATE_PLURAL_STR("Hotkeys", "You cannot pause until another %n second(s) have passed.",
                                               "", static_cast<int>(std::ceil(seconds))),
                          std::max(seconds, Host::OSD_QUICK_DURATION));
  return false;
}

#define DEFINE_NON_ANDROID_HOTKEY(name, category, display_name, handler)                                               \
  DEFINE_HOTKEY(name, category, display_name, handler)

#else

#define DEFINE_NON_ANDROID_HOTKEY(name, category, display_name, handler)

#endif

BEGIN_HOTKEY_LIST(g_common_hotkeys)

DEFINE_NON_ANDROID_HOTKEY("OpenPauseMenu", TRANSLATE_NOOP("Hotkeys", "Interface"),
                          TRANSLATE_NOOP("Hotkeys", "Open Pause Menu"), [](s32 pressed) {
                            if (!pressed && CanPause())
                              FullscreenUI::OpenPauseMenu();
                          })

DEFINE_NON_ANDROID_HOTKEY("OpenCheatsMenu", TRANSLATE_NOOP("Hotkeys", "Interface"),
                          TRANSLATE_NOOP("Hotkeys", "Open Cheat Settings"), [](s32 pressed) {
                            if (!pressed && CanPause())
                              FullscreenUI::OpenCheatsMenu();
                          })

DEFINE_NON_ANDROID_HOTKEY("OpenAchievements", TRANSLATE_NOOP("Hotkeys", "Interface"),
                          TRANSLATE_NOOP("Hotkeys", "Open Achievement List"), [](s32 pressed) {
                            if (!pressed && CanPause())
                              FullscreenUI::OpenAchievementsWindow();
                          })

DEFINE_NON_ANDROID_HOTKEY("OpenLeaderboards", TRANSLATE_NOOP("Hotkeys", "Interface"),
                          TRANSLATE_NOOP("Hotkeys", "Open Leaderboard List"), [](s32 pressed) {
                            if (!pressed && CanPause())
                              FullscreenUI::OpenLeaderboardsWindow();
                          })

DEFINE_NON_ANDROID_HOTKEY("Screenshot", TRANSLATE_NOOP("Hotkeys", "Interface"),
                          TRANSLATE_NOOP("Hotkeys", "Save Screenshot"), [](s32 pressed) {
                            if (!pressed)
                              System::SaveScreenshot();
                          })

DEFINE_NON_ANDROID_HOTKEY("TogglePause", TRANSLATE_NOOP("Hotkeys", "Interface"),
                          TRANSLATE_NOOP("Hotkeys", "Toggle Pause"), [](s32 pressed) {
                            if (!pressed && CanPause())
                              System::PauseSystem(!System::IsPaused());
                          })

DEFINE_NON_ANDROID_HOTKEY("ToggleFullscreen", TRANSLATE_NOOP("Hotkeys", "Interface"),
                          TRANSLATE_NOOP("Hotkeys", "Toggle Fullscreen"), [](s32 pressed) {
                            if (!pressed)
                              Host::SetFullscreen(!Host::IsFullscreen());
                          })

DEFINE_HOTKEY("FastForward", TRANSLATE_NOOP("Hotkeys", "System"), TRANSLATE_NOOP("Hotkeys", "Fast Forward (Hold)"),
              [](s32 pressed) {
                if (pressed < 0)
                  return;
                System::SetFastForwardEnabled(pressed > 0);
              })

DEFINE_HOTKEY("ToggleFastForward", TRANSLATE_NOOP("Hotkeys", "System"),
              TRANSLATE_NOOP("Hotkeys", "Fast Forward (Toggle)"), [](s32 pressed) {
                if (!pressed)
                  System::SetFastForwardEnabled(!System::IsFastForwardEnabled());
              })

DEFINE_HOTKEY("Turbo", TRANSLATE_NOOP("Hotkeys", "System"), TRANSLATE_NOOP("Hotkeys", "Turbo (Hold)"), [](s32 pressed) {
  if (pressed < 0)
    return;
  System::SetTurboEnabled(pressed > 0);
})

DEFINE_HOTKEY("ToggleTurbo", TRANSLATE_NOOP("Hotkeys", "System"), TRANSLATE_NOOP("Hotkeys", "Turbo (Toggle)"),
              [](s32 pressed) {
                if (!pressed)
                  System::SetTurboEnabled(!System::IsTurboEnabled());
              })

DEFINE_NON_ANDROID_HOTKEY("PowerOff", TRANSLATE_NOOP("Hotkeys", "System"),
                          TRANSLATE_NOOP("Hotkeys", "Power Off System"), [](s32 pressed) {
                            if (!pressed && CanPause())
                              Host::RequestSystemShutdown(true, g_settings.save_state_on_exit, true);
                          })

DEFINE_HOTKEY("Reset", TRANSLATE_NOOP("Hotkeys", "System"), TRANSLATE_NOOP("Hotkeys", "Reset System"), [](s32 pressed) {
  if (!pressed)
    Host::RunOnCPUThread(System::ResetSystem);
})

DEFINE_NON_ANDROID_HOTKEY("ChangeDisc", TRANSLATE_NOOP("Hotkeys", "System"), TRANSLATE_NOOP("Hotkeys", "Change Disc"),
                          [](s32 pressed) {
                            if (!pressed)
                              FullscreenUI::OpenDiscChangeMenu();
                          })

DEFINE_HOTKEY("Rewind", TRANSLATE_NOOP("Hotkeys", "System"), TRANSLATE_NOOP("Hotkeys", "Rewind"), [](s32 pressed) {
  if (pressed < 0)
    return;
  System::SetRewindState(pressed > 0);
})

DEFINE_NON_ANDROID_HOTKEY("FrameStep", TRANSLATE_NOOP("Hotkeys", "System"), TRANSLATE_NOOP("Hotkeys", "Frame Step"),
                          [](s32 pressed) {
                            if (!pressed)
                              System::DoFrameStep();
                          })

DEFINE_HOTKEY("SwapMemoryCards", TRANSLATE_NOOP("Hotkeys", "System"),
              TRANSLATE_NOOP("Hotkeys", "Swap Memory Card Slots"), [](s32 pressed) {
                if (!pressed)
                  System::SwapMemoryCards();
              })

DEFINE_NON_ANDROID_HOTKEY("ToggleMediaCapture", TRANSLATE_NOOP("Hotkeys", "System"),
                          TRANSLATE_NOOP("Hotkeys", "Toggle Media Capture"), [](s32 pressed) {
                            if (!pressed)
                            {
                              if (System::GetMediaCapture())
                                System::StopMediaCapture();
                              else
                                System::StartMediaCapture();
                            }
                          })

DEFINE_HOTKEY("ToggleOverclocking", TRANSLATE_NOOP("Hotkeys", "System"),
              TRANSLATE_NOOP("Hotkeys", "Toggle Clock Speed Control (Overclocking)"), [](s32 pressed) {
                if (!pressed && System::IsValid() && !Achievements::IsHardcoreModeActive())
                {
                  g_settings.cpu_overclock_enable = !g_settings.cpu_overclock_enable;
                  g_settings.UpdateOverclockActive();
                  System::UpdateOverclock();

                  if (g_settings.cpu_overclock_enable)
                  {
                    const u32 percent = g_settings.GetCPUOverclockPercent();
                    const double clock_speed =
                      ((static_cast<double>(System::MASTER_CLOCK) * static_cast<double>(percent)) / 100.0) / 1000000.0;
                    Host::AddIconOSDMessage(
                      "ToggleOverclocking", ICON_FA_TACHOMETER_ALT,
                      fmt::format(TRANSLATE_FS("OSDMessage", "CPU clock speed control enabled ({:.3f} MHz)."),
                                  clock_speed),
                      Host::OSD_QUICK_DURATION);
                  }
                  else
                  {
                    Host::AddIconOSDMessage(
                      "ToggleOverclocking", ICON_FA_TACHOMETER_ALT,
                      fmt::format(TRANSLATE_FS("OSDMessage", "CPU clock speed control disabled ({:.3f} MHz)."),
                                  static_cast<double>(System::MASTER_CLOCK) / 1000000.0),
                      Host::OSD_QUICK_DURATION);
                  }
                }
              })

DEFINE_HOTKEY("IncreaseEmulationSpeed", TRANSLATE_NOOP("Hotkeys", "System"),
              TRANSLATE_NOOP("Hotkeys", "Increase Emulation Speed"), [](s32 pressed) {
                if (!pressed && System::IsValid())
                {
                  g_settings.emulation_speed += 0.1f;
                  System::UpdateSpeedLimiterState();
                  Host::AddIconOSDMessage(
                    "EmulationSpeedChange", ICON_FA_TACHOMETER_ALT,
                    fmt::format(TRANSLATE_FS("OSDMessage", "Emulation speed set to {}%."),
                                static_cast<u32>(std::lround(g_settings.emulation_speed * 100.0f))),
                    Host::OSD_QUICK_DURATION);
                }
              })

DEFINE_HOTKEY("DecreaseEmulationSpeed", TRANSLATE_NOOP("Hotkeys", "System"),
              TRANSLATE_NOOP("Hotkeys", "Decrease Emulation Speed"), [](s32 pressed) {
                if (!pressed && System::IsValid())
                {
                  g_settings.emulation_speed =
                    std::max(g_settings.emulation_speed - 0.1f, Achievements::IsHardcoreModeActive() ? 1.0f : 0.1f);
                  System::UpdateSpeedLimiterState();
                  Host::AddIconOSDMessage(
                    "EmulationSpeedChange", ICON_FA_TACHOMETER_ALT,
                    fmt::format(TRANSLATE_FS("OSDMessage", "Emulation speed set to {}%."),
                                static_cast<u32>(std::lround(g_settings.emulation_speed * 100.0f))),
                    Host::OSD_QUICK_DURATION);
                }
              })

DEFINE_HOTKEY("ResetEmulationSpeed", TRANSLATE_NOOP("Hotkeys", "System"),
              TRANSLATE_NOOP("Hotkeys", "Reset Emulation Speed"), [](s32 pressed) {
                if (!pressed && System::IsValid())
                {
                  g_settings.emulation_speed = std::max(Host::GetFloatSettingValue("Main", "EmulationSpeed", 1.0f),
                                                        Achievements::IsHardcoreModeActive() ? 1.0f : 0.1f);
                  System::UpdateSpeedLimiterState();
                  Host::AddIconOSDMessage(
                    "EmulationSpeedChange", ICON_FA_TACHOMETER_ALT,
                    fmt::format(TRANSLATE_FS("OSDMessage", "Emulation speed set to {}%."),
                                static_cast<u32>(std::lround(g_settings.emulation_speed * 100.0f))),
                    Host::OSD_QUICK_DURATION);
                }
              })

DEFINE_HOTKEY("RotateClockwise", TRANSLATE_NOOP("Hotkeys", "Graphics"),
              TRANSLATE_NOOP("Hotkeys", "Rotate Display Clockwise"), [](s32 pressed) {
                if (!pressed)
                {
                  g_settings.display_rotation = static_cast<DisplayRotation>(
                    (static_cast<u8>(g_settings.display_rotation) + 1) % static_cast<u8>(DisplayRotation::Count));
                }
              })

DEFINE_HOTKEY("RotateCounterclockwise", TRANSLATE_NOOP("Hotkeys", "Graphics"),
              TRANSLATE_NOOP("Hotkeys", "Rotate Display Counterclockwise"), [](s32 pressed) {
                if (!pressed)
                {
                  g_settings.display_rotation =
                    (g_settings.display_rotation > static_cast<DisplayRotation>(0)) ?
                      static_cast<DisplayRotation>((static_cast<u8>(g_settings.display_rotation) - 1) %
                                                   static_cast<u8>(DisplayRotation::Count)) :
                      static_cast<DisplayRotation>(static_cast<u8>(DisplayRotation::Count) - 1);
                }
              })

DEFINE_HOTKEY("ToggleOSD", TRANSLATE_NOOP("Hotkeys", "Graphics"), TRANSLATE_NOOP("Hotkeys", "Toggle On-Screen Display"),
              [](s32 pressed) {
                if (!pressed)
                  HotkeyToggleOSD();
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
                  System::ClearMemorySaveStates(true, true);

                  // This is pretty yikes, if PGXP was off we'll have already disabled all the dependent settings.
                  g_settings.gpu_pgxp_enable = !g_settings.gpu_pgxp_enable;
                  {
                    const auto lock = Host::GetSettingsLock();
                    const SettingsInterface& si = *Host::GetSettingsInterface();
                    if (g_settings.gpu_pgxp_enable)
                      g_settings.LoadPGXPSettings(si);
                    g_settings.FixIncompatibleSettings(si, false);
                  }

                  GPUThread::UpdateSettings(true, false, false);

                  Host::AddKeyedOSDMessage("TogglePGXP",
                                           g_settings.gpu_pgxp_enable ?
                                             TRANSLATE_STR("OSDMessage", "PGXP is now enabled.") :
                                             TRANSLATE_STR("OSDMessage", "PGXP is now disabled."),
                                           Host::OSD_QUICK_DURATION);

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

DEFINE_HOTKEY("TogglePGXPDepth", TRANSLATE_NOOP("Hotkeys", "Graphics"),
              TRANSLATE_NOOP("Hotkeys", "Toggle PGXP Depth Buffer"), [](s32 pressed) {
                if (!pressed && System::IsValid())
                {
                  if (!g_settings.gpu_pgxp_enable)
                    return;

                  System::ClearMemorySaveStates(true, true);

                  g_settings.gpu_pgxp_depth_buffer = !g_settings.gpu_pgxp_depth_buffer;
                  GPUThread::UpdateSettings(true, false, false);

                  Host::AddIconOSDMessage("TogglePGXPDepth", ICON_FA_SITEMAP,
                                          g_settings.gpu_pgxp_depth_buffer ?
                                            TRANSLATE_STR("OSDMessage", "PGXP Depth Buffer is now enabled.") :
                                            TRANSLATE_STR("OSDMessage", "PGXP Depth Buffer is now disabled."),
                                          Host::OSD_QUICK_DURATION);
                }
              })

DEFINE_HOTKEY("ToggleWidescreen", TRANSLATE_NOOP("Hotkeys", "Graphics"), TRANSLATE_NOOP("Hotkeys", "Toggle Widescreen"),
              [](s32 pressed) {
                if (!pressed)
                  System::ToggleWidescreen();
              })

DEFINE_HOTKEY("TogglePostProcessing", TRANSLATE_NOOP("Hotkeys", "Graphics"),
              TRANSLATE_NOOP("Hotkeys", "Toggle Post-Processing"), [](s32 pressed) {
                if (!pressed && System::IsValid())
                  GPUPresenter::TogglePostProcessing();
              })

DEFINE_HOTKEY("ReloadPostProcessingShaders", TRANSLATE_NOOP("Hotkeys", "Graphics"),
              TRANSLATE_NOOP("Hotkeys", "Reload Post Processing Shaders"), [](s32 pressed) {
                if (!pressed && System::IsValid())
                  GPUPresenter::ReloadPostProcessingSettings(true, true, true);
              })

DEFINE_HOTKEY("ReloadTextureReplacements", TRANSLATE_NOOP("Hotkeys", "Graphics"),
              TRANSLATE_NOOP("Hotkeys", "Reload Texture Replacements"), [](s32 pressed) {
                if (!pressed && System::IsValid())
                  GPUThread::RunOnThread([]() { GPUTextureCache::ReloadTextureReplacements(true); });
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

DEFINE_HOTKEY("RecordSingleFrameGPUDump", TRANSLATE_NOOP("Hotkeys", "Graphics"),
              TRANSLATE_NOOP("Hotkeys", "Record Single Frame GPU Trace"), [](s32 pressed) {
                if (!pressed)
                  System::StartRecordingGPUDump(nullptr, 1);
              })

DEFINE_HOTKEY("RecordMultiFrameGPUDump", TRANSLATE_NOOP("Hotkeys", "Graphics"),
              TRANSLATE_NOOP("Hotkeys", "Record Multi-Frame GPU Trace"), [](s32 pressed) {
                if (pressed > 0)
                  System::StartRecordingGPUDump(nullptr, 0);
                else
                  System::StopRecordingGPUDump();
              })

// See gte.cpp.
#ifndef __ANDROID__

DEFINE_HOTKEY("FreecamToggle", TRANSLATE_NOOP("Hotkeys", "Free Camera"), TRANSLATE_NOOP("Hotkeys", "Freecam Toggle"),
              [](s32 pressed) {
                if (!pressed && !Achievements::IsHardcoreModeActive())
                  GTE::SetFreecamEnabled(!GTE::IsFreecamEnabled());
              })
DEFINE_HOTKEY("FreecamReset", TRANSLATE_NOOP("Hotkeys", "Free Camera"), TRANSLATE_NOOP("Hotkeys", "Freecam Reset"),
              [](s32 pressed) {
                if (!pressed && !Achievements::IsHardcoreModeActive())
                  GTE::ResetFreecam();
              })
DEFINE_HOTKEY("FreecamMoveLeft", TRANSLATE_NOOP("Hotkeys", "Free Camera"),
              TRANSLATE_NOOP("Hotkeys", "Freecam Move Left"), [](s32 pressed) {
                if (Achievements::IsHardcoreModeActive())
                  return;

                GTE::SetFreecamMoveAxis(0, std::max(static_cast<float>(pressed), 0.0f));
              })
DEFINE_HOTKEY("FreecamMoveRight", TRANSLATE_NOOP("Hotkeys", "Free Camera"),
              TRANSLATE_NOOP("Hotkeys", "Freecam Move Right"), [](s32 pressed) {
                if (Achievements::IsHardcoreModeActive())
                  return;

                GTE::SetFreecamMoveAxis(0, std::min(static_cast<float>(-pressed), 0.0f));
              })
DEFINE_HOTKEY("FreecamMoveUp", TRANSLATE_NOOP("Hotkeys", "Free Camera"), TRANSLATE_NOOP("Hotkeys", "Freecam Move Up"),
              [](s32 pressed) {
                if (Achievements::IsHardcoreModeActive())
                  return;

                GTE::SetFreecamMoveAxis(1, std::max(static_cast<float>(pressed), 0.0f));
              })
DEFINE_HOTKEY("FreecamMoveDown", TRANSLATE_NOOP("Hotkeys", "Free Camera"),
              TRANSLATE_NOOP("Hotkeys", "Freecam Move Down"), [](s32 pressed) {
                if (Achievements::IsHardcoreModeActive())
                  return;

                GTE::SetFreecamMoveAxis(1, std::min(static_cast<float>(-pressed), 0.0f));
              })
DEFINE_HOTKEY("FreecamMoveForward", TRANSLATE_NOOP("Hotkeys", "Free Camera"),
              TRANSLATE_NOOP("Hotkeys", "Freecam Move Forward"), [](s32 pressed) {
                if (Achievements::IsHardcoreModeActive())
                  return;

                GTE::SetFreecamMoveAxis(2, std::min(static_cast<float>(-pressed), 0.0f));
              })
DEFINE_HOTKEY("FreecamMoveBackward", TRANSLATE_NOOP("Hotkeys", "Free Camera"),
              TRANSLATE_NOOP("Hotkeys", "Freecam Move Backward"), [](s32 pressed) {
                if (Achievements::IsHardcoreModeActive())
                  return;

                GTE::SetFreecamMoveAxis(2, std::max(static_cast<float>(pressed), 0.0f));
              })
DEFINE_HOTKEY("FreecamRotateLeft", TRANSLATE_NOOP("Hotkeys", "Free Camera"),
              TRANSLATE_NOOP("Hotkeys", "Freecam Rotate Left"), [](s32 pressed) {
                if (Achievements::IsHardcoreModeActive())
                  return;

                GTE::SetFreecamRotateAxis(1, std::max(static_cast<float>(pressed), 0.0f));
              })
DEFINE_HOTKEY("FreecamRotateRight", TRANSLATE_NOOP("Hotkeys", "Free Camera"),
              TRANSLATE_NOOP("Hotkeys", "Freecam Rotate Right"), [](s32 pressed) {
                if (Achievements::IsHardcoreModeActive())
                  return;

                GTE::SetFreecamRotateAxis(1, std::min(static_cast<float>(-pressed), 0.0f));
              })
DEFINE_HOTKEY("FreecamRotateForward", TRANSLATE_NOOP("Hotkeys", "Free Camera"),
              TRANSLATE_NOOP("Hotkeys", "Freecam Rotate Forward"), [](s32 pressed) {
                if (Achievements::IsHardcoreModeActive())
                  return;

                GTE::SetFreecamRotateAxis(0, std::min(static_cast<float>(-pressed), 0.0f));
              })
DEFINE_HOTKEY("FreecamRotateBackward", TRANSLATE_NOOP("Hotkeys", "Free Camera"),
              TRANSLATE_NOOP("Hotkeys", "Freecam Rotate Backward"), [](s32 pressed) {
                if (Achievements::IsHardcoreModeActive())
                  return;

                GTE::SetFreecamRotateAxis(0, std::max(static_cast<float>(pressed), 0.0f));
              })
DEFINE_HOTKEY("FreecamRollLeft", TRANSLATE_NOOP("Hotkeys", "Free Camera"),
              TRANSLATE_NOOP("Hotkeys", "Freecam Roll Left"), [](s32 pressed) {
                if (Achievements::IsHardcoreModeActive())
                  return;

                GTE::SetFreecamRotateAxis(2, std::min(static_cast<float>(-pressed), 0.0f));
              })
DEFINE_HOTKEY("FreecamRollRight", TRANSLATE_NOOP("Hotkeys", "Free Camera"),
              TRANSLATE_NOOP("Hotkeys", "Freecam Roll Right"), [](s32 pressed) {
                if (Achievements::IsHardcoreModeActive())
                  return;

                GTE::SetFreecamRotateAxis(2, std::max(static_cast<float>(pressed), 0.0f));
              })

#endif // __ANDROID__

DEFINE_HOTKEY("AudioMute", TRANSLATE_NOOP("Hotkeys", "Audio"), TRANSLATE_NOOP("Hotkeys", "Toggle Mute"),
              [](s32 pressed) {
                if (!pressed && System::IsValid())
                {
                  g_settings.audio_output_muted = !g_settings.audio_output_muted;
                  const s32 volume = System::GetAudioOutputVolume();
                  SPU::GetOutputStream()->SetOutputVolume(volume);
                  if (g_settings.audio_output_muted)
                  {
                    Host::AddIconOSDMessage("AudioControlHotkey", ICON_EMOJI_MUTED_SPEAKER,
                                            TRANSLATE_STR("OSDMessage", "Volume: Muted"), 5.0f);
                  }
                  else
                  {
                    Host::AddIconOSDMessage("AudioControlHotkey", ICON_EMOJI_MEDIUM_VOLUME_SPEAKER,
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
                    "AudioControlHotkey",
                    g_settings.cdrom_mute_cd_audio ? ICON_EMOJI_MUTED_SPEAKER : ICON_EMOJI_MEDIUM_VOLUME_SPEAKER,
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

                  const u8 volume =
                    Truncate8(std::min<s32>(static_cast<s32>(System::GetAudioOutputVolume()) + 10, 200));
                  g_settings.audio_output_volume = volume;
                  g_settings.audio_fast_forward_volume = volume;
                  SPU::GetOutputStream()->SetOutputVolume(volume);
                  Host::AddIconOSDMessage("AudioControlHotkey", ICON_EMOJI_HIGH_VOLUME_SPEAKER,
                                          fmt::format(TRANSLATE_FS("OSDMessage", "Volume: {}%"), volume), 5.0f);
                }
              })
DEFINE_HOTKEY("AudioVolumeDown", TRANSLATE_NOOP("Hotkeys", "Audio"), TRANSLATE_NOOP("Hotkeys", "Volume Down"),
              [](s32 pressed) {
                if (!pressed && System::IsValid())
                {
                  g_settings.audio_output_muted = false;

                  const u8 volume = Truncate8(std::max<s32>(static_cast<s32>(System::GetAudioOutputVolume()) - 10, 0));
                  g_settings.audio_output_volume = volume;
                  g_settings.audio_fast_forward_volume = volume;
                  SPU::GetOutputStream()->SetOutputVolume(volume);
                  Host::AddIconOSDMessage("AudioControlHotkey", ICON_EMOJI_MEDIUM_VOLUME_SPEAKER,
                                          fmt::format(TRANSLATE_FS("OSDMessage", "Volume: {}%"), volume), 5.0f);
                }
              })

// NOTE: All save/load state hotkeys are deferred, because it can trigger setting reapply, which reloads bindings.
DEFINE_HOTKEY("LoadSelectedSaveState", TRANSLATE_NOOP("Hotkeys", "Save States"),
              TRANSLATE_NOOP("Hotkeys", "Load From Selected Slot"), [](s32 pressed) {
                if (!pressed)
                  GPUThread::RunOnThread(SaveStateSelectorUI::LoadCurrentSlot);
              })
DEFINE_HOTKEY("SaveSelectedSaveState", TRANSLATE_NOOP("Hotkeys", "Save States"),
              TRANSLATE_NOOP("Hotkeys", "Save To Selected Slot"), [](s32 pressed) {
                if (!pressed)
                  GPUThread::RunOnThread(SaveStateSelectorUI::SaveCurrentSlot);
              })
DEFINE_HOTKEY("SelectPreviousSaveStateSlot", TRANSLATE_NOOP("Hotkeys", "Save States"),
              TRANSLATE_NOOP("Hotkeys", "Select Previous Save Slot"), [](s32 pressed) {
                if (!pressed)
                  GPUThread::RunOnThread([]() { SaveStateSelectorUI::SelectPreviousSlot(true); });
              })
DEFINE_HOTKEY("SelectNextSaveStateSlot", TRANSLATE_NOOP("Hotkeys", "Save States"),
              TRANSLATE_NOOP("Hotkeys", "Select Next Save Slot"), [](s32 pressed) {
                if (!pressed)
                  GPUThread::RunOnThread([]() { SaveStateSelectorUI::SelectNextSlot(true); });
              })
DEFINE_HOTKEY("SaveStateAndSelectNextSlot", TRANSLATE_NOOP("Hotkeys", "Save States"),
              TRANSLATE_NOOP("Hotkeys", "Save State and Select Next Slot"), [](s32 pressed) {
                if (!pressed && System::IsValid())
                {
                  GPUThread::RunOnThread([]() {
                    SaveStateSelectorUI::SaveCurrentSlot();
                    SaveStateSelectorUI::SelectNextSlot(false);
                  });
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

// clang-format off
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
// clang-format on

#undef MAKE_SAVE_STATE_HOTKEY
#undef MAKE_LOAD_STATE_HOTKEY

DEFINE_HOTKEY("TogglePGXPCPU", TRANSLATE_NOOP("Hotkeys", "Debugging"),
              TRANSLATE_NOOP("Hotkeys", "Toggle PGXP CPU Mode"), [](s32 pressed) {
                if (pressed && System::IsValid())
                {
                  if (!g_settings.gpu_pgxp_enable)
                    return;

                  System::ClearMemorySaveStates(true, true);

                  // GPU thread is unchanged
                  g_settings.gpu_pgxp_cpu = !g_settings.gpu_pgxp_cpu;

                  Host::AddIconOSDMessage("TogglePGXPCPU", ICON_FA_BEZIER_CURVE,
                                          g_settings.gpu_pgxp_cpu ?
                                            TRANSLATE_STR("OSDMessage", "PGXP CPU mode is now enabled.") :
                                            TRANSLATE_STR("OSDMessage", "PGXP CPU mode is now disabled."),
                                          Host::OSD_QUICK_DURATION);

                  CPU::PGXP::Shutdown();
                  CPU::PGXP::Initialize();

                  // we need to recompile all blocks if pgxp is toggled on/off
                  CPU::CodeCache::Reset();

                  // need to swap interpreters
                  System::InterruptExecution();
                }
              })
DEFINE_HOTKEY("TogglePGXPPreserveProjPrecision", TRANSLATE_NOOP("Hotkeys", "Debugging"),
              TRANSLATE_NOOP("Hotkeys", "Toggle PGXP Preserve Projection Precision"), [](s32 pressed) {
                if (pressed && System::IsValid())
                {
                  if (!g_settings.gpu_pgxp_enable)
                    return;

                  // GPU thread is unchanged
                  g_settings.gpu_pgxp_preserve_proj_fp = !g_settings.gpu_pgxp_preserve_proj_fp;

                  Host::AddIconOSDMessage(
                    "TogglePGXPPreserveProjPrecision", ICON_FA_DRAW_POLYGON,
                    g_settings.gpu_pgxp_preserve_proj_fp ?
                      TRANSLATE_STR("OSDMessage", "PGXP Preserve Projection Precision is now enabled.") :
                      TRANSLATE_STR("OSDMessage", "PGXP Preserve Projection Precision is now disabled."),
                    Host::OSD_QUICK_DURATION);
                }
              })
DEFINE_HOTKEY("ToggleVRAMView", TRANSLATE_NOOP("Hotkeys", "Debugging"), TRANSLATE_NOOP("Hotkeys", "Toggle VRAM View"),
              [](s32 pressed) {
                if (pressed && System::IsValid())
                {
                  if (!g_settings.gpu_pgxp_enable || Achievements::IsHardcoreModeActive())
                    return;

                  g_settings.gpu_show_vram = !g_settings.gpu_show_vram;
                  GPUThread::UpdateSettings(true, false, false);

                  Host::AddIconOSDMessage("ToggleVRAMView", ICON_FA_FILE_ALT,
                                          g_settings.gpu_show_vram ?
                                            TRANSLATE_STR("OSDMessage", "Now showing VRAM.") :
                                            TRANSLATE_STR("OSDMessage", "Now showing display."),
                                          Host::OSD_QUICK_DURATION);
                }
              })

END_HOTKEY_LIST()
