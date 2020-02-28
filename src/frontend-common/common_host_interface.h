#pragma once
#include "core/host_interface.h"
#include "common/string.h"
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>
#include <optional>
#include <vector>

class CommonHostInterface : public HostInterface
{
public:
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

  /// Returns a list of all available hotkeys.
  const HotkeyInfoList& GetHotkeyInfoList() const { return m_hotkeys; }

protected:
  CommonHostInterface();
  ~CommonHostInterface();

  virtual void SetFullscreen(bool enabled);
  virtual void ToggleFullscreen();

  virtual std::unique_ptr<AudioStream> CreateAudioStream(AudioBackend backend) override;

  virtual void SetDefaultSettings(SettingsInterface& si) override;

  virtual std::optional<HostKeyCode> GetHostKeyCode(const std::string_view key_code) const;

  void RegisterHotkey(String category, String name, String display_name, InputButtonHandler handler);
  bool HandleHostKeyEvent(HostKeyCode code, bool pressed);
  void UpdateInputMap(SettingsInterface& si);

private:
  void RegisterGeneralHotkeys();
  void RegisterGraphicsHotkeys();
  void RegisterSaveStateHotkeys();
  void UpdateControllerInputMap(SettingsInterface& si);
  void UpdateHotkeyInputMap(SettingsInterface& si);
  void AddButtonToInputMap(const std::string& binding, InputButtonHandler handler);
  void AddAxisToInputMap(const std::string& binding, InputAxisHandler handler);

  HotkeyInfoList m_hotkeys;

  // input key maps
  std::map<HostKeyCode, InputButtonHandler> m_keyboard_input_handlers;
};
