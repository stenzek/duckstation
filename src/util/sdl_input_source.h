// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "input_source.h"

#include <SDL3/SDL.h>

#include <array>
#include <functional>
#include <mutex>
#include <vector>

class SettingsInterface;

class SDLInputSource final : public InputSource
{
public:
  static constexpr u32 MAX_LED_COLORS = 4;

  SDLInputSource();
  ~SDLInputSource();

  bool Initialize(const SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) override;
  void UpdateSettings(const SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) override;
  bool ReloadDevices() override;
  void Shutdown() override;

  void PollEvents() override;
  InputManager::DeviceList EnumerateDevices() override;
  InputManager::VibrationMotorList EnumerateVibrationMotors(std::optional<InputBindingKey> for_device) override;
  bool GetGenericBindingMapping(std::string_view device, GenericInputBindingMapping* mapping) override;
  void UpdateMotorState(InputBindingKey key, float intensity) override;
  void UpdateMotorState(InputBindingKey large_key, InputBindingKey small_key, float large_intensity,
                        float small_intensity) override;

  bool ContainsDevice(std::string_view device) const override;
  std::optional<InputBindingKey> ParseKeyString(std::string_view device, std::string_view binding) override;
  TinyString ConvertKeyToString(InputBindingKey key) override;
  TinyString ConvertKeyToIcon(InputBindingKey key, InputManager::BindingIconMappingFunction mapper) override;

  std::unique_ptr<ForceFeedbackDevice> CreateForceFeedbackDevice(std::string_view device, Error* error) override;

  bool ProcessSDLEvent(const SDL_Event* event);

  SDL_Joystick* GetJoystickForDevice(std::string_view device);

  static u32 GetRGBForPlayerId(const SettingsInterface& si, u32 player_id);
  static u32 ParseRGBForPlayerId(std::string_view str, u32 player_id);

  static bool IsHandledInputEvent(const SDL_Event* ev);

  static void CopySettings(SettingsInterface& dest_si, const SettingsInterface& src_si);

  static bool ALLOW_EVENT_POLLING;

private:
  struct ControllerData
  {
    SDL_Haptic* haptic;
    SDL_Gamepad* gamepad;
    SDL_Joystick* joystick;
    u16 rumble_intensity[2];
    int haptic_left_right_effect;
    int joystick_id;
    int player_id;
    float last_touch_x;
    float last_touch_y;
    bool use_gamepad_rumble : 1;
    bool has_led : 1;

    // Used to disable Joystick controls that are used in GameController inputs so we don't get double events
    std::vector<bool> joy_button_used_in_gc;
    std::vector<bool> joy_axis_used_in_gc;
    std::vector<bool> joy_hat_used_in_gc;

    // Track last hat state so we can send "unpressed" events.
    std::vector<u8> last_hat_state;
  };

  using ControllerDataVector = std::vector<ControllerData>;

  bool InitializeSubsystem();
  void ShutdownSubsystem();
  void LoadSettings(const SettingsInterface& si);
  void SetHints();

  ControllerDataVector::iterator GetControllerDataForJoystickId(int id);
  ControllerDataVector::iterator GetControllerDataForPlayerId(int id);
  int GetFreePlayerId() const;

  bool OpenDevice(int index, bool is_gamecontroller);
  bool CloseDevice(int joystick_index);
  bool HandleGamepadAxisMotionEvent(const SDL_GamepadAxisEvent* ev);
  bool HandleGamepadButtonEvent(const SDL_GamepadButtonEvent* ev);
  bool HandleGamepadTouchpadEvent(const SDL_GamepadTouchpadEvent* ev);
  bool HandleJoystickAxisEvent(const SDL_JoyAxisEvent* ev);
  bool HandleJoystickButtonEvent(const SDL_JoyButtonEvent* ev);
  bool HandleJoystickHatEvent(const SDL_JoyHatEvent* ev);
  void SendRumbleUpdate(ControllerData* cd);

  ControllerDataVector m_controllers;

  std::array<u32, MAX_LED_COLORS> m_led_colors{};
  std::vector<std::pair<std::string, std::string>> m_sdl_hints;

  bool m_sdl_subsystem_initialized = false;
  bool m_controller_enhanced_mode = false;
  bool m_controller_ps5_player_led = false;
  bool m_controller_touchpad_as_pointer = false;

#ifdef __APPLE__
  bool m_enable_iokit_driver = false;
  bool m_enable_mfi_driver = false;
#endif
};

class SDLForceFeedbackDevice : public ForceFeedbackDevice
{
public:
  SDLForceFeedbackDevice(SDL_Joystick* joystick, SDL_Haptic* haptic);
  ~SDLForceFeedbackDevice() override;

  void SetConstantForce(s32 level) override;
  void DisableForce(Effect force) override;

private:
  void CreateEffects(SDL_Joystick* joystick);
  void DestroyEffects();

  SDL_Haptic* m_haptic = nullptr;

  SDL_HapticEffect m_constant_effect;
  int m_constant_effect_id = -1;
  bool m_constant_effect_running = false;
};
