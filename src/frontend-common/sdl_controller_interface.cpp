#include "sdl_controller_interface.h"
#include "common/assert.h"
#include "common/log.h"
#include "core/controller.h"
#include "core/host_interface.h"
#include "core/system.h"
#include "sdl_initializer.h"
#include <SDL.h>
#include <cmath>
Log_SetChannel(SDLControllerInterface);

SDLControllerInterface::SDLControllerInterface() = default;

SDLControllerInterface::~SDLControllerInterface()
{
  Assert(m_controllers.empty());
}

bool SDLControllerInterface::Initialize(CommonHostInterface* host_interface)
{
  if (!ControllerInterface::Initialize(host_interface))
    return false;

  FrontendCommon::EnsureSDLInitialized();

  if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC) < 0)
  {
    Log_ErrorPrintf("SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC) failed");
    return false;
  }

  // we should open the controllers as the connected events come in, so no need to do any more here
  m_sdl_subsystem_initialized = true;
  return true;
}

void SDLControllerInterface::Shutdown()
{
  while (!m_controllers.empty())
    CloseGameController(m_controllers.begin()->joystick_id, false);

  if (m_sdl_subsystem_initialized)
  {
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC);
    m_sdl_subsystem_initialized = false;
  }

  ControllerInterface::Shutdown();
}

void SDLControllerInterface::PollEvents()
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
      CloseGameController(event->cdevice.which, true);
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

SDLControllerInterface::ControllerDataVector::iterator
SDLControllerInterface::GetControllerDataForController(void* controller)
{
  return std::find_if(m_controllers.begin(), m_controllers.end(),
                      [controller](const ControllerData& cd) { return cd.controller == controller; });
}

SDLControllerInterface::ControllerDataVector::iterator SDLControllerInterface::GetControllerDataForJoystickId(int id)
{
  return std::find_if(m_controllers.begin(), m_controllers.end(),
                      [id](const ControllerData& cd) { return cd.joystick_id == id; });
}

SDLControllerInterface::ControllerDataVector::iterator SDLControllerInterface::GetControllerDataForPlayerId(int id)
{
  return std::find_if(m_controllers.begin(), m_controllers.end(),
                      [id](const ControllerData& cd) { return cd.player_id == id; });
}

bool SDLControllerInterface::OpenGameController(int index)
{
  SDL_GameController* gcontroller = SDL_GameControllerOpen(index);
  SDL_Joystick* joystick = gcontroller ? SDL_GameControllerGetJoystick(gcontroller) : nullptr;
  if (!gcontroller || !joystick)
  {
    Log_WarningPrintf("Failed to open controller %d", index);
    if (gcontroller)
      SDL_GameControllerClose(gcontroller);

    return false;
  }

  int joystick_id = SDL_JoystickInstanceID(joystick);
#if SDL_VERSION_ATLEAST(2, 0, 9)
  int player_id = SDL_GameControllerGetPlayerIndex(gcontroller);
#else
  int player_id = -1;
#endif
  if (player_id < 0)
  {
    Log_WarningPrintf("Controller %d (joystick %d) returned player ID %d. Setting to zero, but this may cause issues "
                      "if you try to use multiple controllers.",
                      index, joystick_id, player_id);
    player_id = 0;
  }

  Log_InfoPrintf("Opened controller %d (instance id %d, player id %d): %s", index, joystick_id, player_id,
                 SDL_GameControllerName(gcontroller));

  ControllerData cd = {};
  cd.controller = gcontroller;
  cd.player_id = player_id;
  cd.joystick_id = joystick_id;

  SDL_Haptic* haptic = SDL_HapticOpenFromJoystick(joystick);
  if (SDL_HapticRumbleSupported(haptic) && SDL_HapticRumbleInit(haptic) == 0)
    cd.haptic = haptic;
  else if (haptic)
    SDL_HapticClose(haptic);

  if (cd.haptic)
    Log_InfoPrintf("Rumble is supported on '%s'", SDL_GameControllerName(gcontroller));
  else
    Log_WarningPrintf("Rumble is not supported on '%s'", SDL_GameControllerName(gcontroller));

  m_controllers.push_back(std::move(cd));
  OnControllerConnected(player_id);
  return true;
}

bool SDLControllerInterface::CloseGameController(int joystick_index, bool notify)
{
  auto it = GetControllerDataForJoystickId(joystick_index);
  if (it == m_controllers.end())
    return false;

  const int player_id = it->player_id;

  if (it->haptic)
    SDL_HapticClose(static_cast<SDL_Haptic*>(it->haptic));

  SDL_GameControllerClose(static_cast<SDL_GameController*>(it->controller));
  m_controllers.erase(it);

  if (notify)
    OnControllerDisconnected(player_id);
  return true;
}

void SDLControllerInterface::ClearBindings()
{
  for (auto& it : m_controllers)
  {
    for (AxisCallback& ac : it.axis_mapping)
      ac = {};
    for (ButtonCallback& bc : it.button_mapping)
      bc = {};
  }
}

bool SDLControllerInterface::BindControllerAxis(int controller_index, int axis_number, AxisCallback callback)
{
  auto it = GetControllerDataForPlayerId(controller_index);
  if (it == m_controllers.end())
    return false;

  if (axis_number < 0 || axis_number >= MAX_NUM_AXISES)
    return false;

  it->axis_mapping[axis_number] = std::move(callback);
  return true;
}

bool SDLControllerInterface::BindControllerButton(int controller_index, int button_number, ButtonCallback callback)
{
  auto it = GetControllerDataForPlayerId(controller_index);
  if (it == m_controllers.end())
    return false;

  if (button_number < 0 || button_number >= MAX_NUM_BUTTONS)
    return false;

  it->button_mapping[button_number] = std::move(callback);
  return true;
}

bool SDLControllerInterface::BindControllerAxisToButton(int controller_index, int axis_number, bool direction,
                                                        ButtonCallback callback)
{
  auto it = GetControllerDataForPlayerId(controller_index);
  if (it == m_controllers.end())
    return false;

  if (axis_number < 0 || axis_number >= MAX_NUM_AXISES)
    return false;

  it->axis_button_mapping[axis_number][BoolToUInt8(direction)] = std::move(callback);
  return true;
}

bool SDLControllerInterface::HandleControllerAxisEvent(const SDL_Event* ev)
{
  // TODO: Make deadzone customizable.
  static constexpr float deadzone = 8192.0f / 32768.0f;

  const float value = static_cast<float>(ev->caxis.value) / (ev->caxis.value < 0 ? 32768.0f : 32767.0f);
  Log_DebugPrintf("controller %d axis %d %d %f", ev->caxis.which, ev->caxis.axis, ev->caxis.value, value);

  auto it = GetControllerDataForJoystickId(ev->caxis.which);
  if (it == m_controllers.end())
    return false;

  if (DoEventHook(Hook::Type::Axis, it->player_id, ev->caxis.axis, value))
    return true;

  const AxisCallback& cb = it->axis_mapping[ev->caxis.axis];
  if (cb)
  {
    cb(value);
    return true;
  }

  // set the other direction to false so large movements don't leave the opposite on
  const bool outside_deadzone = (std::abs(value) >= deadzone);
  const bool positive = (value >= 0.0f);
  const ButtonCallback& other_button_cb = it->axis_button_mapping[ev->caxis.axis][BoolToUInt8(!positive)];
  const ButtonCallback& button_cb = it->axis_button_mapping[ev->caxis.axis][BoolToUInt8(positive)];
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

  auto it = GetControllerDataForJoystickId(ev->caxis.which);
  if (it == m_controllers.end())
    return false;

  const bool pressed = (ev->cbutton.state == SDL_PRESSED);
  if (DoEventHook(Hook::Type::Button, it->player_id, ev->cbutton.button, pressed ? 1.0f : 0.0f))
    return true;

  const ButtonCallback& cb = it->button_mapping[ev->cbutton.button];
  if (!cb)
    return false;

  cb(pressed);
  return true;
}

void SDLControllerInterface::UpdateControllerRumble()
{
  for (auto& cd : m_controllers)
  {
    // TODO: FIXME proper binding
    if (!cd.haptic || cd.player_id < 0 || cd.player_id >= 2)
      continue;

    float new_strength = 0.0f;
    Controller* controller = GetController(cd.player_id);
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
