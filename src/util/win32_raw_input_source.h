// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once
#include "common/windows_headers.h"
#include "input_source.h"
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

  bool Initialize(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) override;
  void UpdateSettings(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) override;
  bool ReloadDevices() override;
  void Shutdown() override;

  void PollEvents() override;
  std::vector<std::pair<std::string, std::string>> EnumerateDevices() override;
  std::vector<InputBindingKey> EnumerateMotors() override;
  bool GetGenericBindingMapping(std::string_view device, GenericInputBindingMapping* mapping) override;
  void UpdateMotorState(InputBindingKey key, float intensity) override;
  void UpdateMotorState(InputBindingKey large_key, InputBindingKey small_key, float large_intensity,
                        float small_intensity) override;

  std::optional<InputBindingKey> ParseKeyString(std::string_view device, std::string_view binding) override;
  TinyString ConvertKeyToString(InputBindingKey key) override;
  TinyString ConvertKeyToIcon(InputBindingKey key) override;

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
