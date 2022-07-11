#pragma once
#include "common/windows_headers.h"
#include "input_source.h"
#include <Xinput.h>
#include <array>
#include <functional>
#include <mutex>
#include <vector>

class SettingsInterface;

class XInputSource final : public InputSource
{
public:
  XInputSource();
  ~XInputSource();

  bool Initialize(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) override;
  void UpdateSettings(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) override;
  void Shutdown() override;

  void PollEvents() override;
  std::vector<std::pair<std::string, std::string>> EnumerateDevices() override;
  std::vector<InputBindingKey> EnumerateMotors() override;
  bool GetGenericBindingMapping(const std::string_view& device, GenericInputBindingMapping* mapping) override;
  void UpdateMotorState(InputBindingKey key, float intensity) override;
  void UpdateMotorState(InputBindingKey large_key, InputBindingKey small_key, float large_intensity,
                        float small_intensity) override;

  std::optional<InputBindingKey> ParseKeyString(const std::string_view& device,
                                                const std::string_view& binding) override;
  std::string ConvertKeyToString(InputBindingKey key) override;

private:
  enum : u32
  {
    NUM_CONTROLLERS = XUSER_MAX_COUNT, // 4
    NUM_BUTTONS = 15,
  };

  enum : u32
  {
    AXIS_LEFTX,
    AXIS_LEFTY,
    AXIS_RIGHTX,
    AXIS_RIGHTY,
    AXIS_LEFTTRIGGER,
    AXIS_RIGHTTRIGGER,
    NUM_AXES,
  };

  struct ControllerData
  {
    XINPUT_STATE last_state;
    XINPUT_VIBRATION last_vibration = {};
    bool connected = false;
    bool has_large_motor = false;
    bool has_small_motor = false;
  };

  using ControllerDataArray = std::array<ControllerData, NUM_CONTROLLERS>;

  void CheckForStateChanges(u32 index, const XINPUT_STATE& new_state);
  void HandleControllerConnection(u32 index);
  void HandleControllerDisconnection(u32 index);

  ControllerDataArray m_controllers;

  HMODULE m_xinput_module{};
  DWORD(WINAPI* m_xinput_get_state)(DWORD, XINPUT_STATE*);
  DWORD(WINAPI* m_xinput_set_state)(DWORD, XINPUT_VIBRATION*);
  DWORD(WINAPI* m_xinput_get_capabilities)(DWORD, DWORD, XINPUT_CAPABILITIES*);

  static const char* s_axis_names[NUM_AXES];
  static const char* s_button_names[NUM_BUTTONS];
  static const u16 s_button_masks[NUM_BUTTONS];
};
