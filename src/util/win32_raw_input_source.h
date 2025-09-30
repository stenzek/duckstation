// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "input_source.h"

#include "common/windows_headers.h"

#include <array>
#include <functional>
#include <mutex>
#include <vector>

class SettingsInterface;

class Win32RawInputSource final : public InputSource
{
public:
  Win32RawInputSource();
  ~Win32RawInputSource();

  bool Initialize(const SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) override;
  void UpdateSettings(const SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) override;
  bool ReloadDevices() override;
  void Shutdown() override;

  void PollEvents() override;
  std::optional<float> GetCurrentValue(InputBindingKey key) override;
  InputManager::DeviceList EnumerateDevices() override;
  InputManager::DeviceEffectList EnumerateEffects(std::optional<InputBindingInfo::Type> type,
                                                  std::optional<InputBindingKey> for_device) override;
  bool GetGenericBindingMapping(std::string_view device, GenericInputBindingMapping* mapping) override;
  void UpdateMotorState(InputBindingKey key, float intensity) override;
  void UpdateMotorState(InputBindingKey large_key, InputBindingKey small_key, float large_intensity,
                        float small_intensity) override;
  void UpdateLEDState(InputBindingKey key, float intensity) override;

  bool ContainsDevice(std::string_view device) const override;
  std::optional<InputBindingKey> ParseKeyString(std::string_view device, std::string_view binding) override;
  TinyString ConvertKeyToString(InputBindingKey key) override;
  TinyString ConvertKeyToIcon(InputBindingKey key, InputManager::BindingIconMappingFunction mapper) override;

  std::unique_ptr<ForceFeedbackDevice> CreateForceFeedbackDevice(std::string_view device, Error* error) override;

private:
  struct MouseState
  {
    HANDLE device;
    u32 button_state;
    s32 last_x;
    s32 last_y;
  };

  static bool RegisterDummyClass();
  static LRESULT CALLBACK DummyWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

  static std::string GetMouseDeviceName(u32 index);

  bool CreateDummyWindow();
  void DestroyDummyWindow();
  bool OpenDevices();
  void CloseDevices();

  bool ProcessRawInputEvent(const RAWINPUT* event);

  HWND m_dummy_window = {};

  std::vector<MouseState> m_mice;
};
