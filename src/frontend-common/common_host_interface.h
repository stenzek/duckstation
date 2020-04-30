#pragma once
#include "common/string.h"
#include "core/host_interface.h"
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

class ControllerInterface;

namespace FrontendCommon {
class SaveStateSelectorUI;
}

class CommonHostInterface : public HostInterface
{
public:
  friend ControllerInterface;

  using HostKeyCode = s32;
  using HostMouseButton = s32;

  using InputButtonHandler = std::function<void(bool)>;
  using InputAxisHandler = std::function<void(float)>;
  using ControllerRumbleCallback = std::function<void(const float*, u32)>;

  struct HotkeyInfo
  {
    String category;
    String name;
    String display_name;
    InputButtonHandler handler;
  };

  using HotkeyInfoList = std::vector<HotkeyInfo>;

  /// Returns the name of the frontend.
  virtual const char* GetFrontendName() const = 0;

  virtual bool Initialize() override;
  virtual void Shutdown() override;

  virtual bool BootSystem(const SystemBootParameters& parameters) override;
  virtual void PowerOffSystem() override;

  /// Returns a list of all available hotkeys.
  ALWAYS_INLINE const HotkeyInfoList& GetHotkeyInfoList() const { return m_hotkeys; }

  /// Access to current controller interface.
  ALWAYS_INLINE ControllerInterface* GetControllerInterface() const { return m_controller_interface.get(); }

  /// Returns true if running in batch mode, i.e. exit after emulation.
  ALWAYS_INLINE bool InBatchMode() const { return m_batch_mode; }

  /// Parses command line parameters for all frontends.
  bool ParseCommandLineParameters(int argc, char* argv[], std::unique_ptr<SystemBootParameters>* out_boot_params);

protected:
  CommonHostInterface();
  ~CommonHostInterface();

  /// Request the frontend to exit.
  virtual void RequestExit() = 0;

  /// Executes per-frame tasks such as controller polling.
  virtual void PollAndUpdate();

  virtual bool IsFullscreen() const;
  virtual bool SetFullscreen(bool enabled);

  virtual std::unique_ptr<AudioStream> CreateAudioStream(AudioBackend backend) override;
  virtual std::unique_ptr<ControllerInterface> CreateControllerInterface();

  virtual void OnSystemCreated() override;
  virtual void OnSystemPaused(bool paused) override;
  virtual void OnSystemDestroyed() override;
  virtual void OnRunningGameChanged() override;
  virtual void OnControllerTypeChanged(u32 slot) override;
  virtual void DrawImGuiWindows() override;

  virtual void SetDefaultSettings(SettingsInterface& si) override;
  virtual void ApplySettings(SettingsInterface& si) override;

  virtual std::optional<HostKeyCode> GetHostKeyCode(const std::string_view key_code) const;

  virtual bool AddButtonToInputMap(const std::string& binding, const std::string_view& device,
                                   const std::string_view& button, InputButtonHandler handler);
  virtual bool AddAxisToInputMap(const std::string& binding, const std::string_view& device,
                                 const std::string_view& axis, InputAxisHandler handler);
  virtual bool AddRumbleToInputMap(const std::string& binding, u32 controller_index, u32 num_motors);

  /// Reloads the input map from config. Callable from controller interface.
  virtual void UpdateInputMap() = 0;

  /// Returns a path where an input profile with the specified name would be saved.
  std::string GetPathForInputProfile(const char* name) const;

  /// Returns a list of all input profiles. first - name, second - path
  std::vector<std::pair<std::string, std::string>> GetInputProfileList() const;

  /// Applies the specified input profile.
  void ApplyInputProfile(const char* profile_path, SettingsInterface& si);

  /// Saves the current input configuration to the specified profile name.
  bool SaveInputProfile(const char* profile_path, SettingsInterface& si);

  void RegisterHotkey(String category, String name, String display_name, InputButtonHandler handler);
  bool HandleHostKeyEvent(HostKeyCode code, bool pressed);
  bool HandleHostMouseEvent(HostMouseButton button, bool pressed);
  void UpdateInputMap(SettingsInterface& si);

  void AddControllerRumble(u32 controller_index, u32 num_motors, ControllerRumbleCallback callback);
  void UpdateControllerRumble();
  void StopControllerRumble();

  std::unique_ptr<ControllerInterface> m_controller_interface;

private:
  void RegisterGeneralHotkeys();
  void RegisterGraphicsHotkeys();
  void RegisterSaveStateHotkeys();
  void UpdateControllerInputMap(SettingsInterface& si);
  void UpdateHotkeyInputMap(SettingsInterface& si);
  void ClearAllControllerBindings(SettingsInterface& si);

#ifdef WITH_DISCORD_PRESENCE
  void SetDiscordPresenceEnabled(bool enabled);
  void InitializeDiscordPresence();
  void ShutdownDiscordPresence();
  void UpdateDiscordPresence();
  void PollDiscordPresence();
#endif

  HotkeyInfoList m_hotkeys;

  std::unique_ptr<FrontendCommon::SaveStateSelectorUI> m_save_state_selector_ui;

  // input key maps
  std::map<HostKeyCode, InputButtonHandler> m_keyboard_input_handlers;
  std::map<HostMouseButton, InputButtonHandler> m_mouse_input_handlers;

  // controller vibration motors/rumble
  struct ControllerRumbleState
  {
    enum : u32
    {
      MAX_MOTORS = 2
    };

    u32 controller_index;
    u32 num_motors;
    std::array<float, MAX_MOTORS> last_strength;
    ControllerRumbleCallback update_callback;
  };
  std::vector<ControllerRumbleState> m_controller_vibration_motors;

  // running in batch mode? i.e. exit after stopping emulation
  bool m_batch_mode = false;

#ifdef WITH_DISCORD_PRESENCE
  // discord rich presence
  bool m_discord_presence_enabled = false;
  bool m_discord_presence_active = false;
#endif
};
