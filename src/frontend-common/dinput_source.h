#pragma once
#define DIRECTINPUT_VERSION 0x0800
#include "common/windows_headers.h"
#include "input_source.h"
#include "core/types.h"
#include <array>
#include <dinput.h>
#include <functional>
#include <mutex>
#include <vector>
#include <wrl/client.h>

class DInputSource final : public InputSource
{
public:
  enum HAT_DIRECTION : u32
  {
    HAT_DIRECTION_UP = 0,
    HAT_DIRECTION_DOWN = 1,
    HAT_DIRECTION_LEFT = 2,
    HAT_DIRECTION_RIGHT = 3,
    NUM_HAT_DIRECTIONS = 4,
  };

  enum : u32
  {
    MAX_NUM_BUTTONS = 32,
  };

  DInputSource();
  ~DInputSource() override;

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
  template<typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  struct ControllerData
  {
    ComPtr<IDirectInputDevice8> device;
    DIJOYSTATE last_state = {};
    std::vector<u32> axis_offsets;
    u32 num_buttons = 0;

    // NOTE: We expose hats as num_buttons + (hat_index * 4) + direction.
    u32 num_hats = 0;

    bool needs_poll = true;
  };

  using ControllerDataArray = std::vector<ControllerData>;

  static std::array<bool, NUM_HAT_DIRECTIONS> GetHatButtons(DWORD hat);
  static std::string GetDeviceIdentifier(u32 index);

  void AddDevices(HWND toplevel_window);
  bool AddDevice(ControllerData& cd, HWND toplevel_window, const char* name);

  void CheckForStateChanges(size_t index, const DIJOYSTATE& new_state);

  ControllerDataArray m_controllers;

  HMODULE m_dinput_module{};
  LPCDIDATAFORMAT m_joystick_data_format{};
  ComPtr<IDirectInput8> m_dinput;
};
