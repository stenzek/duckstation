#pragma once
#include "common/types.h"
#include <string>

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
  Achievements,
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
  AchievementsSetings,
  AdvancedSettings,
  Count
};

bool Initialize(CommonHostInterface* host_interface);
bool HasActiveWindow();
void SystemCreated();
void SystemDestroyed();
void SystemPaused(bool paused);
void OpenQuickMenu();
void CloseQuickMenu();
void Shutdown();
void Render();

bool IsBindingInput();
bool HandleKeyboardBinding(const char* keyName, bool pressed);

bool InvalidateCachedTexture(const std::string& path);

// Returns true if the message has been dismissed.
bool DrawErrorWindow(const char* message);
bool DrawConfirmWindow(const char* message, bool* result);

void QueueGameListRefresh();
void EnsureGameListLoaded();

Settings& GetSettingsCopy();
void SaveAndApplySettings();
void SetDebugMenuAllowed(bool allowed);

/// Only ImGuiNavInput_Activate, ImGuiNavInput_Cancel, and DPad should be forwarded.
/// Returns true if the UI consumed the event, and it should not execute the normal handler.
bool SetControllerNavInput(FrontendCommon::ControllerNavigationButton button, bool value);

/// Forwards the controller navigation to ImGui for fullscreen navigation. Call before NewFrame().
void SetImGuiNavInputs();

} // namespace FullscreenUI
