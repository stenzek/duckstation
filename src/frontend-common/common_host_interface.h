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

class CommonHostInterface : public HostInterface
{
public:
  friend ControllerInterface;

  using HostKeyCode = s32;

  using InputButtonHandler = std::function<void(bool)>;
  using InputAxisHandler = std::function<void(float)>;

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

  virtual bool IsFullscreen() const;
  virtual bool SetFullscreen(bool enabled);

  virtual std::unique_ptr<AudioStream> CreateAudioStream(AudioBackend backend) override;
  virtual std::unique_ptr<ControllerInterface> CreateControllerInterface();

  virtual void OnSystemCreated() override;
  virtual void OnSystemPaused(bool paused) override;
  virtual void OnControllerTypeChanged(u32 slot) override;

  virtual void SetDefaultSettings(SettingsInterface& si) override;

  virtual std::optional<HostKeyCode> GetHostKeyCode(const std::string_view key_code) const;

  virtual bool AddButtonToInputMap(const std::string& binding, const std::string_view& device,
                                   const std::string_view& button, InputButtonHandler handler);
  virtual bool AddAxisToInputMap(const std::string& binding, const std::string_view& device,
                                 const std::string_view& axis, InputAxisHandler handler);

  /// Reloads the input map from config. Callable from controller interface.
  virtual void UpdateInputMap() = 0;

  void RegisterHotkey(String category, String name, String display_name, InputButtonHandler handler);
  bool HandleHostKeyEvent(HostKeyCode code, bool pressed);
  void UpdateInputMap(SettingsInterface& si);

  std::unique_ptr<ControllerInterface> m_controller_interface;

private:
  void RegisterGeneralHotkeys();
  void RegisterGraphicsHotkeys();
  void RegisterSaveStateHotkeys();
  void UpdateControllerInputMap(SettingsInterface& si);
  void UpdateHotkeyInputMap(SettingsInterface& si);

  HotkeyInfoList m_hotkeys;

  // input key maps
  std::map<HostKeyCode, InputButtonHandler> m_keyboard_input_handlers;

  // running in batch mode? i.e. exit after stopping emulation
  bool m_batch_mode = false;
};
