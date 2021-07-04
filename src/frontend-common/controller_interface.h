#pragma once
#include "common_host_interface.h"
#include "core/types.h"
#include <array>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string_view>
#include <variant>

class HostInterface;
class Controller;

class ControllerInterface
{
public:
  enum class Backend
  {
    None,
#ifdef WITH_SDL2
    SDL,
#endif
#ifdef _WIN32
    XInput,
#endif
#ifdef WITH_DINPUT
    DInput,
#endif
#ifdef ANDROID
    Android,
#endif
#ifdef WITH_EVDEV
    Evdev,
#endif
    Count
  };

  enum : int
  {
    NUM_HAT_DIRECTIONS = 4,
    HAT_DIRECTION_UP = 0,
    HAT_DIRECTION_DOWN = 1,
    HAT_DIRECTION_LEFT = 2,
    HAT_DIRECTION_RIGHT = 3,
  };

  enum AxisSide
  {
    Full,
    Positive,
    Negative
  };

  using AxisCallback = CommonHostInterface::InputAxisHandler;
  using ButtonCallback = CommonHostInterface::InputButtonHandler;

  ControllerInterface();
  virtual ~ControllerInterface();

  static std::optional<Backend> ParseBackendName(const char* name);
  static const char* GetBackendName(Backend type);
  static Backend GetDefaultBackend();
  static std::unique_ptr<ControllerInterface> Create(Backend type);

  virtual Backend GetBackend() const = 0;
  virtual bool Initialize(CommonHostInterface* host_interface);
  virtual void Shutdown();

  // Removes all bindings. Call before setting new bindings.
  virtual void ClearBindings() = 0;

  // Binding to events. If a binding for this axis/button already exists, returns false.
  virtual std::optional<int> GetControllerIndex(const std::string_view& device);
  virtual bool BindControllerAxis(int controller_index, int axis_number, AxisSide axis_side, AxisCallback callback) = 0;
  virtual bool BindControllerButton(int controller_index, int button_number, ButtonCallback callback) = 0;
  virtual bool BindControllerAxisToButton(int controller_index, int axis_number, bool direction,
                                          ButtonCallback callback) = 0;
  virtual bool BindControllerHatToButton(int controller_index, int hat_number, std::string_view hat_position,
                                         ButtonCallback callback) = 0;
  virtual bool BindControllerButtonToAxis(int controller_index, int button_number, AxisCallback callback) = 0;

  virtual void PollEvents() = 0;

  // Changing rumble strength.
  virtual u32 GetControllerRumbleMotorCount(int controller_index) = 0;
  virtual void SetControllerRumbleStrength(int controller_index, const float* strengths, u32 num_motors) = 0;

  // Set deadzone that will be applied on axis-to-button mappings
  virtual bool SetControllerDeadzone(int controller_index, float size) = 0;

  // Input monitoring for external access.
  struct Hook
  {
    enum class Type
    {
      Axis,
      Button,
      Hat // Only for joysticks
    };

    enum class CallbackResult
    {
      StopMonitoring,
      ContinueMonitoring
    };

    using Callback = std::function<CallbackResult(const Hook& ei)>;

    Type type;
    int controller_index;
    int button_or_axis_number;
    std::variant<float, std::string_view> value; // 0/1 for buttons, -1..1 for axes, hat direction name for hats
    bool track_history;                          // Track axis movement to spot inversion/half axes
  };
  void SetHook(Hook::Callback callback);
  void ClearHook();
  bool HasHook();

protected:
  bool DoEventHook(Hook::Type type, int controller_index, int button_or_axis_number,
                   std::variant<float, std::string_view> value, bool track_history = false);

  void OnControllerConnected(int host_id);
  void OnControllerDisconnected(int host_id);

  CommonHostInterface* m_host_interface = nullptr;

  std::mutex m_event_intercept_mutex;
  Hook::Callback m_event_intercept_callback;
};
