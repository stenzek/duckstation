#pragma once
#include "SDL.h"
#include "input_source.h"
#include <array>
#include <functional>
#include <mutex>
#include <vector>

class SettingsInterface;

class SDLInputSource final : public InputSource
{
public:
  SDLInputSource();
  ~SDLInputSource();

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

  bool ProcessSDLEvent(const SDL_Event* event);

private:
  enum : int
  {
    MAX_NUM_AXES = 7,
    MAX_NUM_BUTTONS = 16,
  };

  struct ControllerData
  {
    SDL_Haptic* haptic;
    SDL_GameController* game_controller;
    u16 rumble_intensity[2];
    int haptic_left_right_effect;
    int joystick_id;
    int player_id;
    bool use_game_controller_rumble;
  };

  using ControllerDataVector = std::vector<ControllerData>;

  bool InitializeSubsystem();
  void ShutdownSubsystem();
  void LoadSettings(SettingsInterface& si);
  void SetHints();

  ControllerDataVector::iterator GetControllerDataForJoystickId(int id);
  ControllerDataVector::iterator GetControllerDataForPlayerId(int id);
  int GetFreePlayerId() const;

  bool OpenGameController(int index);
  bool CloseGameController(int joystick_index);
  bool HandleControllerAxisEvent(const SDL_ControllerAxisEvent* event);
  bool HandleControllerButtonEvent(const SDL_ControllerButtonEvent* event);
  void SendRumbleUpdate(ControllerData* cd);

  ControllerDataVector m_controllers;

  bool m_sdl_subsystem_initialized = false;
  bool m_controller_enhanced_mode = false;
};
