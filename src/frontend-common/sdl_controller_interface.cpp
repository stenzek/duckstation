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

int SDLControllerInterface::GetFreePlayerId() const
{
  for (int player_id = 0;; player_id++)
  {
    size_t i;
    for (i = 0; i < m_controllers.size(); i++)
    {
      if (m_controllers[i].player_id == player_id)
        break;
    }
    if (i == m_controllers.size())
      return player_id;
  }

  return 0;
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
  if (player_id < 0 || GetControllerDataForPlayerId(player_id) != m_controllers.end())
  {
    const int free_player_id = GetFreePlayerId();
    Log_WarningPrintf(
      "Controller %d (joystick %d) returned player ID %d, which is invalid or in use. Using ID %d instead.", index,
      joystick_id, player_id, free_player_id);
    player_id = free_player_id;
  }

  Log_InfoPrintf("Opened controller %d (instance id %d, player id %d): %s", index, joystick_id, player_id,
                 SDL_GameControllerName(gcontroller));

  ControllerData cd = {};
  cd.controller = gcontroller;
  cd.player_id = player_id;
  cd.joystick_id = joystick_id;
  cd.haptic_left_right_effect = -1;

  SDL_Haptic* haptic = SDL_HapticOpenFromJoystick(joystick);
  if (haptic)
  {
    SDL_HapticEffect ef = {};
    ef.leftright.type = SDL_HAPTIC_LEFTRIGHT;
    ef.leftright.length = 1000;

    int ef_id = SDL_HapticNewEffect(haptic, &ef);
    if (ef_id >= 0)
    {
      cd.haptic = haptic;
      cd.haptic_left_right_effect = ef_id;
    }
    else
    {
      Log_ErrorPrintf("Failed to create haptic left/right effect: %s", SDL_GetError());
      if (SDL_HapticRumbleSupported(haptic) && SDL_HapticRumbleInit(haptic) != 0)
      {
        cd.haptic = haptic;
      }
      else
      {
        Log_ErrorPrintf("No haptic rumble supported: %s", SDL_GetError());
        SDL_HapticClose(haptic);
      }
    }
  }

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
    // Apply axis scaling only when controller axis is mapped to an axis
    cb(std::clamp(it->axis_scale * value, -1.0f, 1.0f));
    return true;
  }

  // set the other direction to false so large movements don't leave the opposite on
  const bool outside_deadzone = (std::abs(value) >= it->deadzone);
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

u32 SDLControllerInterface::GetControllerRumbleMotorCount(int controller_index)
{
  auto it = GetControllerDataForPlayerId(controller_index);
  if (it == m_controllers.end())
    return 0;

  return (it->haptic_left_right_effect >= 0) ? 2 : (it->haptic ? 1 : 0);
}

void SDLControllerInterface::SetControllerRumbleStrength(int controller_index, const float* strengths, u32 num_motors)
{
  auto it = GetControllerDataForPlayerId(controller_index);
  if (it == m_controllers.end())
    return;

  // we'll update before this duration is elapsed
  static constexpr float MIN_STRENGTH = 0.01f;
  static constexpr u32 DURATION = 100000;

  SDL_Haptic* haptic = static_cast<SDL_Haptic*>(it->haptic);
  if (it->haptic_left_right_effect >= 0 && num_motors > 1)
  {
    if (strengths[0] >= MIN_STRENGTH || strengths[1] >= MIN_STRENGTH)
    {
      SDL_HapticEffect ef;
      ef.type = SDL_HAPTIC_LEFTRIGHT;
      ef.leftright.large_magnitude = static_cast<u32>(strengths[0] * 65535.0f);
      ef.leftright.small_magnitude = static_cast<u32>(strengths[1] * 65535.0f);
      ef.leftright.length = DURATION;
      SDL_HapticUpdateEffect(haptic, it->haptic_left_right_effect, &ef);
      SDL_HapticRunEffect(haptic, it->haptic_left_right_effect, SDL_HAPTIC_INFINITY);
    }
    else
    {
      SDL_HapticStopEffect(haptic, it->haptic_left_right_effect);
    }
  }
  else
  {
    float max_strength = 0.0f;
    for (u32 i = 0; i < num_motors; i++)
      max_strength = std::max(max_strength, strengths[i]);

    if (max_strength >= MIN_STRENGTH)
      SDL_HapticRumblePlay(haptic, max_strength, DURATION);
    else
      SDL_HapticRumbleStop(haptic);
  }
}

bool SDLControllerInterface::SetControllerAxisScale(int controller_index, float scale /* = 1.00f */)
{
  auto it = GetControllerDataForPlayerId(controller_index);
  if (it == m_controllers.end())
    return false;

  it->axis_scale = std::clamp(std::abs(scale), 0.01f, 1.50f);
  Log_InfoPrintf("Controller %d axis scale set to %f", controller_index, it->axis_scale);
  return true;
}

bool SDLControllerInterface::SetControllerDeadzone(int controller_index, float size /* = 0.25f */)
{
  auto it = GetControllerDataForPlayerId(controller_index);
  if (it == m_controllers.end())
    return false;

  it->deadzone = std::clamp(std::abs(size), 0.01f, 0.99f);
  Log_InfoPrintf("Controller %d deadzone size set to %f", controller_index, it->deadzone);
  return true;
}
