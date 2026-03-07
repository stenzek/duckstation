// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once
#include "common/windows_headers.h"
#include "input_source.h"
#include <Xinput.h>
#include <array>
#include <functional>
#include <mutex>
#include <vector>

class SettingsInterface;

// https://github.com/nefarius/DsHidMini/blob/master/include/DsHidMini/ScpTypes.h
struct SCP_EXTN
{
  FLOAT SCP_UP;
  FLOAT SCP_RIGHT;
  FLOAT SCP_DOWN;
  FLOAT SCP_LEFT;

  FLOAT SCP_LX;
  FLOAT SCP_LY;

  FLOAT SCP_L1;
  FLOAT SCP_L2;
  FLOAT SCP_L3;

  FLOAT SCP_RX;
  FLOAT SCP_RY;

  FLOAT SCP_R1;
  FLOAT SCP_R2;
  FLOAT SCP_R3;

  FLOAT SCP_T;
  FLOAT SCP_C;
  FLOAT SCP_X;
  FLOAT SCP_S;

  FLOAT SCP_SELECT;
  FLOAT SCP_START;

  FLOAT SCP_PS;
};

class XInputSource final : public InputSource
{
public:
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

  XInputSource();
  ~XInputSource();

  bool Initialize(const SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) override;
  void UpdateSettings(const SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) override;
  bool ReloadDevices() override;
  void Shutdown() override;

  void PollEvents() override;
  std::optional<float> GetCurrentValue(InputBindingKey key) override;
  InputManager::DeviceList EnumerateDevices() override;
  InputManager::DeviceEffectList EnumerateEffects(std::optional<InputBindingInfo::Type> type,
                                                  std::optional<InputBindingKey> for_device) override;
  u32 GetPollableDeviceCount() const override;
  bool GetGenericBindingMapping(std::string_view device, GenericInputBindingMapping* mapping) override;
  void UpdateMotorState(InputBindingKey key, float intensity) override;
  void UpdateMotorState(InputBindingKey large_key, InputBindingKey small_key, float large_intensity,
                        float small_intensity) override;
  void UpdateLEDState(InputBindingKey key, float intensity) override;

  bool ContainsDevice(std::string_view device) const override;
  std::optional<InputBindingKey> ParseKeyString(std::string_view device, std::string_view binding) override;
  TinyString ConvertKeyToString(InputBindingKey key) override;
  TinyString ConvertKeyToDisplayString(InputBindingKey key, bool allow_icon,
                                       InputManager::BindingIconMappingFunction mapper) override;
  void SetSubclassPollDeviceList(InputSubclass subclass, const std::span<const InputBindingKey>* devices) override;
  std::unique_ptr<ForceFeedbackDevice> CreateForceFeedbackDevice(std::string_view device, Error* error) override;

private:
  union ControllerState
  {
    XINPUT_STATE xinput;
    SCP_EXTN scp_extn;
  };

  struct ControllerData
  {
    ControllerState last_state;
    XINPUT_VIBRATION last_vibration;
    bool connected;
    bool has_large_motor;
    bool has_small_motor;
  };

  using ControllerDataArray = std::array<ControllerData, NUM_CONTROLLERS>;

  bool UseSCPExtn() const;
  DWORD GetControllerState(u32 index, ControllerState* state);

  void CheckForStateChanges(u32 index, const ControllerState& new_state);
  void HandleControllerConnection(u32 index, const ControllerState& state);
  void HandleControllerDisconnection(u32 index);

  static std::string GetDeviceIdentifier(u32 index);
  static std::string GetDeviceName(u32 index);

  ControllerDataArray m_controllers = {};

  HMODULE m_xinput_module{};
  DWORD(WINAPI* m_xinput_get_state)(DWORD, XINPUT_STATE*) = nullptr;
  DWORD(WINAPI* m_xinput_set_state)(DWORD, XINPUT_VIBRATION*) = nullptr;
  DWORD(WINAPI* m_xinput_get_capabilities)(DWORD, DWORD, XINPUT_CAPABILITIES*) = nullptr;
  DWORD(WINAPI* m_xinput_get_extended)(DWORD, SCP_EXTN*) = nullptr;
};
