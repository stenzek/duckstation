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

  virtual void OnSystemCreated() override;
  virtual void OnSystemPaused(bool paused) override;

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
  virtual bool AddButtonToInputMap(const std::string& binding, const std::string_view& device,
                                   const std::string_view& button, InputButtonHandler handler);
  virtual bool AddAxisToInputMap(const std::string& binding, const std::string_view& device,
                                 const std::string_view& axis, InputAxisHandler handler);

  HotkeyInfoList m_hotkeys;

  // input key maps
  std::map<HostKeyCode, InputButtonHandler> m_keyboard_input_handlers;
};
