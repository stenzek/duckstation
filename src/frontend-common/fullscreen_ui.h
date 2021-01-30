#pragma once
#include "common/types.h"

class CommonHostInterface;
class SettingsInterface;
struct Settings;

namespace FrontendCommon {
enum class ControllerNavigationButton : u32;
}

namespace FullscreenUI {
enum class MainWindowType
{
  None,
  Landing,
  GameList,
  Settings,
  QuickMenu,
  MoreQuickMenu
};

enum class SettingsPage
{
  InterfaceSettings,
  GameListSettings,
  ConsoleSettings,
  EmulationSettings,
  BIOSSettings,
  ControllerSettings,
  HotkeySettings,
  MemoryCardSettings,
  DisplaySettings,
  EnhancementSettings,
  AudioSettings,
  AdvancedSettings,
  Count
};

bool Initialize(CommonHostInterface* host_interface, SettingsInterface* settings_interface);
bool HasActiveWindow();
void SystemCreated();
void SystemDestroyed();
void SystemPaused(bool paused);
void OpenQuickMenu();
void CloseQuickMenu();
void Shutdown();
void Render();

void EnsureGameListLoaded();

Settings& GetSettingsCopy();
void SaveAndApplySettings();
void SetDebugMenuEnabled(bool enabled, bool save_to_ini = false);

/// Only ImGuiNavInput_Activate, ImGuiNavInput_Cancel, and DPad should be forwarded.
/// Returns true if the UI consumed the event, and it should not execute the normal handler.
bool SetControllerNavInput(FrontendCommon::ControllerNavigationButton button, bool value);

/// Forwards the controller navigation to ImGui for fullscreen navigation. Call before NewFrame().
void SetImGuiNavInputs();

} // namespace FullscreenUI
