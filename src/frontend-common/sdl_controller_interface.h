#pragma once
#include "controller_interface.h"
#include "core/types.h"
#include <array>
#include <functional>
#include <mutex>
#include <vector>

class SDLControllerInterface final : public ControllerInterface
{
public:
  SDLControllerInterface();
  ~SDLControllerInterface();

  Backend GetBackend() const override;
  bool Initialize(CommonHostInterface* host_interface) override;
  void Shutdown() override;

  /// Returns the path of the optional game controller database file.
  std::string GetGameControllerDBFileName() const;

  // Removes all bindings. Call before setting new bindings.
  void ClearBindings() override;

  // Binding to events. If a binding for this axis/button already exists, returns false.
  bool BindControllerAxis(int controller_index, int axis_number, AxisSide axis_side, AxisCallback callback) override;
  bool BindControllerButton(int controller_index, int button_number, ButtonCallback callback) override;
  bool BindControllerAxisToButton(int controller_index, int axis_number, bool direction,
                                  ButtonCallback callback) override;
  bool BindControllerHatToButton(int controller_index, int hat_number, std::string_view hat_position,
                                 ButtonCallback callback) override;
  bool BindControllerButtonToAxis(int controller_index, int button_number, AxisCallback callback) override;

  // Changing rumble strength.
  u32 GetControllerRumbleMotorCount(int controller_index) override;
  void SetControllerRumbleStrength(int controller_index, const float* strengths, u32 num_motors) override;

  // Set deadzone that will be applied on axis-to-button mappings
  bool SetControllerDeadzone(int controller_index, float size = 0.25f) override;

  void PollEvents() override;

  bool ProcessSDLEvent(const union SDL_Event* event);

private:
  enum : int
  {
    MAX_NUM_AXES = 7,
    MAX_NUM_BUTTONS = 16,
  };

  struct ControllerData
  {
    void* haptic;
    void* game_controller;
    int haptic_left_right_effect;
    int joystick_id;
    int player_id;
    bool use_game_controller_rumble;

    float deadzone = 0.25f;

    // TODO: Turn to vectors to support arbitrary amounts of buttons and axes (for Joysticks)
    // Preferably implement a simple "flat map", an ordered view over a vector
    std::array<std::array<AxisCallback, 3>, MAX_NUM_AXES> axis_mapping;
    std::array<ButtonCallback, MAX_NUM_BUTTONS> button_mapping;
    std::array<std::array<ButtonCallback, 2>, MAX_NUM_AXES> axis_button_mapping;
    std::array<AxisCallback, MAX_NUM_BUTTONS> button_axis_mapping;
    std::vector<std::array<ButtonCallback, 4>> hat_button_mapping;

    ALWAYS_INLINE bool IsGameController() const { return (game_controller != nullptr); }
  };

  using ControllerDataVector = std::vector<ControllerData>;

  ControllerDataVector::iterator GetControllerDataForJoystickId(int id);
  ControllerDataVector::iterator GetControllerDataForPlayerId(int id);
  int GetFreePlayerId() const;

  bool OpenGameController(int index);
  bool CloseGameController(int joystick_index, bool notify);
  bool HandleControllerAxisEvent(const struct SDL_ControllerAxisEvent* event);
  bool HandleControllerButtonEvent(const struct SDL_ControllerButtonEvent* event);

  bool OpenJoystick(int index);
  bool HandleJoystickAxisEvent(const struct SDL_JoyAxisEvent* event);
  bool HandleJoystickButtonEvent(const struct SDL_JoyButtonEvent* event);
  bool HandleJoystickHatEvent(const struct SDL_JoyHatEvent* event);

  ControllerDataVector m_controllers;

  std::mutex m_event_intercept_mutex;
  Hook::Callback m_event_intercept_callback;

  bool m_sdl_subsystem_initialized = false;
};
