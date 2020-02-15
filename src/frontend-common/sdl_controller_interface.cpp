#include "sdl_controller_interface.h"
#include "common/assert.h"
#include "common/log.h"
#include "core/controller.h"
#include "core/host_interface.h"
#include "core/system.h"
#include "sdl_initializer.h"
#include <cmath>
#include <SDL.h>
Log_SetChannel(SDLControllerInterface);

SDLControllerInterface g_sdl_controller_interface;

SDLControllerInterface::SDLControllerInterface() = default;

SDLControllerInterface::~SDLControllerInterface()
{
  Assert(m_controllers.empty());
}

bool SDLControllerInterface::Initialize(HostInterface* host_interface)
{
  FrontendCommon::EnsureSDLInitialized();

  if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC) < 0)
  {
    Log_ErrorPrintf("SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC) failed");
    return false;
  }

  // we should open the controllers as the connected events come in, so no need to do any more here
  m_host_interface = host_interface;
  m_initialized = true;
  return true;
}

void SDLControllerInterface::Shutdown()
{
  while (!m_controllers.empty())
    CloseGameController(m_controllers.begin()->first);

  if (m_initialized)
  {
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC);
    m_initialized = false;
  }

  m_host_interface = nullptr;
}

void SDLControllerInterface::PumpSDLEvents()
{
  for (;;)
  {
    SDL_Event ev;
    if (SDL_PollEvent(&ev))
      ProcessSDLEvent(&ev);
    else
      break;
  }
}

bool SDLControllerInterface::ProcessSDLEvent(const SDL_Event* event)
{
  switch (event->type)
  {
    case SDL_CONTROLLERDEVICEADDED:
    {
      Log_InfoPrintf("Controller %d inserted", event->cdevice.which);
      OpenGameController(event->cdevice.which);
      return true;
    }

    case SDL_CONTROLLERDEVICEREMOVED:
    {
      Log_InfoPrintf("Controller %d removed", event->cdevice.which);
      CloseGameController(event->cdevice.which);
      return true;
    }

    case SDL_CONTROLLERAXISMOTION:
      return HandleControllerAxisEvent(event);

    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
      return HandleControllerButtonEvent(event);

    default:
      return false;
  }
}

System* SDLControllerInterface::GetSystem() const
{
  return m_host_interface->GetSystem();
}

Controller* SDLControllerInterface::GetController(u32 slot) const
{
  System* system = GetSystem();
  return system ? system->GetController(slot) : nullptr;
}

void SDLControllerInterface::SetHook(Hook::Callback callback)
{
  std::unique_lock<std::mutex> lock(m_event_intercept_mutex);
  Assert(!m_event_intercept_callback);
  m_event_intercept_callback = std::move(callback);
}

void SDLControllerInterface::ClearHook()
{
  std::unique_lock<std::mutex> lock(m_event_intercept_mutex);
  if (m_event_intercept_callback)
    m_event_intercept_callback = {};
}

bool SDLControllerInterface::DoEventHook(Hook::Type type, int controller_index, int button_or_axis_number, float value)
{
  std::unique_lock<std::mutex> lock(m_event_intercept_mutex);
  if (!m_event_intercept_callback)
    return false;

  const Hook ei{type, controller_index, button_or_axis_number, value};
  const Hook::CallbackResult action = m_event_intercept_callback(ei);
  if (action == Hook::CallbackResult::StopMonitoring)
    m_event_intercept_callback = {};

  return true;
}

bool SDLControllerInterface::OpenGameController(int index)
{
  if (m_controllers.find(index) != m_controllers.end())
    CloseGameController(index);

  SDL_GameController* gcontroller = SDL_GameControllerOpen(index);
  if (!gcontroller)
  {
    Log_WarningPrintf("Failed to open controller %d", index);
    return false;
  }

  Log_InfoPrintf("Opened controller %d: %s", index, SDL_GameControllerName(gcontroller));

  ControllerData cd = {};
  cd.controller = gcontroller;

  SDL_Joystick* joystick = SDL_GameControllerGetJoystick(gcontroller);
  if (joystick)
  {
    SDL_Haptic* haptic = SDL_HapticOpenFromJoystick(joystick);
    if (SDL_HapticRumbleSupported(haptic) && SDL_HapticRumbleInit(haptic) == 0)
      cd.haptic = haptic;
    else
      SDL_HapticClose(haptic);
  }

  if (cd.haptic)
    Log_InfoPrintf("Rumble is supported on '%s'", SDL_GameControllerName(gcontroller));
  else
    Log_WarningPrintf("Rumble is not supported on '%s'", SDL_GameControllerName(gcontroller));

  m_controllers.emplace(index, cd);
  return true;
}

void SDLControllerInterface::CloseGameControllers()
{
  while (!m_controllers.empty())
    CloseGameController(m_controllers.begin()->first);
}

bool SDLControllerInterface::CloseGameController(int index)
{
  auto it = m_controllers.find(index);
  if (it == m_controllers.end())
    return false;

  if (it->second.haptic)
    SDL_HapticClose(static_cast<SDL_Haptic*>(it->second.haptic));

  SDL_GameControllerClose(static_cast<SDL_GameController*>(it->second.controller));
  m_controllers.erase(it);
  return true;
}

void SDLControllerInterface::ClearControllerBindings()
{
  for (auto& it : m_controllers)
  {
    for (AxisCallback& ac : it.second.axis_mapping)
      ac = {};
    for (ButtonCallback& bc : it.second.button_mapping)
      bc = {};
  }
}

bool SDLControllerInterface::BindControllerAxis(int controller_index, int axis_number, AxisCallback callback)
{
  auto it = m_controllers.find(controller_index);
  if (it == m_controllers.end())
    return false;

  if (axis_number < 0 || axis_number >= MAX_NUM_AXISES)
    return false;

  it->second.axis_mapping[axis_number] = std::move(callback);
  return true;
}

bool SDLControllerInterface::BindControllerButton(int controller_index, int button_number, ButtonCallback callback)
{
  auto it = m_controllers.find(controller_index);
  if (it == m_controllers.end())
    return false;

  if (button_number < 0 || button_number >= MAX_NUM_BUTTONS)
    return false;

  it->second.button_mapping[button_number] = std::move(callback);
  return true;
}

bool SDLControllerInterface::BindControllerAxisToButton(int controller_index, int axis_number, bool direction,
                                                        ButtonCallback callback)
{
  auto it = m_controllers.find(controller_index);
  if (it == m_controllers.end())
    return false;

  if (axis_number < 0 || axis_number >= MAX_NUM_AXISES)
    return false;

  it->second.axis_button_mapping[axis_number][BoolToUInt8(direction)] = std::move(callback);
  return true;
}

void SDLControllerInterface::SetDefaultBindings()
{
  ClearControllerBindings();

  const ControllerType type = m_host_interface->GetSettings().controller_types[0];
  if (type == ControllerType::None || m_controllers.empty())
    return;

  const int first_controller_index = m_controllers.begin()->first;

#define SET_AXIS_MAP(axis, name)                                                                                       \
  do                                                                                                                   \
  {                                                                                                                    \
    std::optional<s32> code = Controller::GetAxisCodeByName(type, name);                                               \
    if (code)                                                                                                          \
    {                                                                                                                  \
      const s32 code_value = code.value();                                                                             \
      BindControllerAxis(first_controller_index, axis, [this, code_value](float value) {                               \
        Controller* controller = GetController(0);                                                                     \
        if (controller)                                                                                                \
          controller->SetAxisState(code_value, value);                                                                 \
      });                                                                                                              \
    }                                                                                                                  \
  } while (0)

#define SET_BUTTON_MAP(button, name)                                                                                   \
  do                                                                                                                   \
  {                                                                                                                    \
    std::optional<s32> code = Controller::GetButtonCodeByName(type, name);                                             \
    if (code)                                                                                                          \
    {                                                                                                                  \
      const s32 code_value = code.value();                                                                             \
      BindControllerButton(first_controller_index, button, [this, code_value](bool pressed) {                          \
        Controller* controller = GetController(0);                                                                     \
        if (controller)                                                                                                \
          controller->SetButtonState(code_value, pressed);                                                             \
      });                                                                                                              \
    }                                                                                                                  \
  } while (0)

#define SET_AXIS_BUTTON_MAP(axis, direction, name)                                                                     \
  do                                                                                                                   \
  {                                                                                                                    \
    std::optional<s32> code = Controller::GetButtonCodeByName(type, name);                                             \
    if (code)                                                                                                          \
    {                                                                                                                  \
      const s32 code_value = code.value();                                                                             \
      BindControllerAxisToButton(first_controller_index, axis, direction, [this, code_value](bool pressed) {           \
        Controller* controller = GetController(0);                                                                     \
        if (controller)                                                                                                \
          controller->SetButtonState(code_value, pressed);                                                             \
      });                                                                                                              \
    }                                                                                                                  \
  } while (0)

  SET_AXIS_MAP(SDL_CONTROLLER_AXIS_LEFTX, "LeftX");
  SET_AXIS_MAP(SDL_CONTROLLER_AXIS_LEFTY, "LeftY");
  SET_AXIS_MAP(SDL_CONTROLLER_AXIS_RIGHTX, "RightX");
  SET_AXIS_MAP(SDL_CONTROLLER_AXIS_RIGHTY, "RightY");
  SET_AXIS_MAP(SDL_CONTROLLER_AXIS_TRIGGERLEFT, "LeftTrigger");
  SET_AXIS_MAP(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, "RightTrigger");

  SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_DPAD_UP, "Up");
  SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_DPAD_DOWN, "Down");
  SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_DPAD_LEFT, "Left");
  SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, "Right");
  SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_Y, "Triangle");
  SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_A, "Cross");
  SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_X, "Square");
  SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_B, "Circle");
  SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, "L1");
  SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, "R1");
  SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_LEFTSTICK, "L3");
  SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_RIGHTSTICK, "R3");
  SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_START, "Start");
  SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_BACK, "Select");

  // fallback axis -> button mappings
  SET_AXIS_BUTTON_MAP(SDL_CONTROLLER_AXIS_LEFTX, false, "Left");
  SET_AXIS_BUTTON_MAP(SDL_CONTROLLER_AXIS_LEFTX, true, "Right");
  SET_AXIS_BUTTON_MAP(SDL_CONTROLLER_AXIS_LEFTY, false, "Up");
  SET_AXIS_BUTTON_MAP(SDL_CONTROLLER_AXIS_LEFTY, true, "Down");
  SET_AXIS_BUTTON_MAP(SDL_CONTROLLER_AXIS_TRIGGERLEFT, true, "L2");
  SET_AXIS_BUTTON_MAP(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, true, "R2");

#undef SET_AXIS_MAP
#undef SET_BUTTON_MAP
#undef SET_AXIS_BUTTON_MAP

  // TODO: L2/R2 -> buttons
}

bool SDLControllerInterface::HandleControllerAxisEvent(const SDL_Event* ev)
{
  Log_DebugPrintf("controller %d axis %d %d", ev->caxis.which, ev->caxis.axis, ev->caxis.value);

  // TODO: Make deadzone customizable.
  static constexpr float deadzone = 8192.0f / 32768.0f;

  const float value = static_cast<float>(ev->caxis.value) / (ev->caxis.value < 0 ? 32768.0f : 32767.0f);
  const bool outside_deadzone = (std::abs(value) >= deadzone);

  // only send monitor events if it's outside of the deadzone, otherwise it's really hard to bind
  if (outside_deadzone && DoEventHook(Hook::Type::Axis, ev->caxis.which, ev->caxis.axis, value))
    return true;

  auto it = m_controllers.find(ev->caxis.which);
  if (it == m_controllers.end())
    return false;

  const ControllerData& cd = it->second;
  const AxisCallback& cb = cd.axis_mapping[ev->caxis.axis];
  if (cb)
  {
    cb(value);
    return true;
  }

  // set the other direction to false so large movements don't leave the opposite on
  const bool positive = (value >= 0.0f);
  const ButtonCallback& other_button_cb = cd.axis_button_mapping[ev->caxis.axis][BoolToUInt8(!positive)];
  const ButtonCallback& button_cb = cd.axis_button_mapping[ev->caxis.axis][BoolToUInt8(positive)];
  if (button_cb)
  {
    button_cb(outside_deadzone);
    if (other_button_cb)
      other_button_cb(false);
    return true;
  }
  else if (other_button_cb)
  {
    other_button_cb(false);
    return true;
  }
  else
  {
    return false;
  }
}

bool SDLControllerInterface::HandleControllerButtonEvent(const SDL_Event* ev)
{
  Log_DebugPrintf("controller %d button %d %s", ev->cbutton.which, ev->cbutton.button,
                  ev->cbutton.state == SDL_PRESSED ? "pressed" : "released");

  const bool pressed = (ev->cbutton.state == SDL_PRESSED);
  if (DoEventHook(Hook::Type::Button, ev->cbutton.which, ev->cbutton.button, pressed ? 1.0f : 0.0f))
    return true;

  auto it = m_controllers.find(ev->caxis.which);
  if (it == m_controllers.end())
    return false;

  const ButtonCallback& cb = it->second.button_mapping[ev->cbutton.button];
  if (!cb)
    return false;

  cb(pressed);
  return true;
}

void SDLControllerInterface::UpdateControllerRumble()
{
  for (auto& it : m_controllers)
  {
    ControllerData& cd = it.second;
    if (!cd.haptic)
      continue;

    float new_strength = 0.0f;
    Controller* controller = GetController(cd.controller_index);
    if (controller)
    {
      const u32 motor_count = controller->GetVibrationMotorCount();
      for (u32 i = 0; i < motor_count; i++)
        new_strength = std::max(new_strength, controller->GetVibrationMotorStrength(i));
    }

    if (cd.last_rumble_strength == new_strength)
      continue;

    if (new_strength > 0.01f)
      SDL_HapticRumblePlay(static_cast<SDL_Haptic*>(cd.haptic), new_strength, 100000);
    else
      SDL_HapticRumbleStop(static_cast<SDL_Haptic*>(cd.haptic));

    cd.last_rumble_strength = new_strength;
  }
}
