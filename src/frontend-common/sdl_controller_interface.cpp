#include "sdl_controller_interface.h"
#include "common/assert.h"
#include "common/file_system.h"
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

ControllerInterface::Backend SDLControllerInterface::GetBackend() const
{
  return ControllerInterface::Backend::SDL;
}

bool SDLControllerInterface::Initialize(CommonHostInterface* host_interface)
{
  if (!ControllerInterface::Initialize(host_interface))
    return false;

  FrontendCommon::EnsureSDLInitialized();

  const std::string gcdb_file_name = GetGameControllerDBFileName();
  if (!gcdb_file_name.empty())
  {
    Log_InfoPrintf("Loading game controller mappings from '%s'", gcdb_file_name.c_str());
    if (SDL_GameControllerAddMappingsFromFile(gcdb_file_name.c_str()) < 0)
    {
      Log_ErrorPrintf("SDL_GameControllerAddMappingsFromFile(%s) failed: %s", gcdb_file_name.c_str(), SDL_GetError());
    }
  }

  const bool ds4_rumble_enabled = host_interface->GetBoolSettingValue("Main", "ControllerEnhancedMode", false);
  if (ds4_rumble_enabled)
  {
    Log_InfoPrintf("Enabling PS4/PS5 enhanced mode.");
#if SDL_VERSION_ATLEAST(2, 0, 9)
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "true");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "true");
#endif
#if SDL_VERSION_ATLEAST(2, 0, 16)
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "true");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "true");
#endif
  }

  if (SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC) < 0)
  {
    Log_ErrorPrintf("SDL_InitSubSystem(SDL_INIT_JOYSTICK |SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC) failed");
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
    SDL_QuitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC);
    m_sdl_subsystem_initialized = false;
  }

  ControllerInterface::Shutdown();
}

std::string SDLControllerInterface::GetGameControllerDBFileName() const
{
  // prefer the userdir copy
  std::string filename(m_host_interface->GetUserDirectoryRelativePath("gamecontrollerdb.txt"));
  if (FileSystem::FileExists(filename.c_str()))
    return filename;

  filename =
    m_host_interface->GetProgramDirectoryRelativePath("database" FS_OSPATH_SEPARATOR_STR "gamecontrollerdb.txt");
  if (FileSystem::FileExists(filename.c_str()))
    return filename;

  return {};
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
      return HandleControllerAxisEvent(&event->caxis);

    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
      return HandleControllerButtonEvent(&event->cbutton);

    case SDL_JOYDEVICEADDED:
      if (SDL_IsGameController(event->jdevice.which))
        return true;

      Log_InfoPrintf("Joystick %d inserted", event->jdevice.which);
      OpenJoystick(event->jdevice.which);
      return true;

    case SDL_JOYAXISMOTION:
      return HandleJoystickAxisEvent(&event->jaxis);

    case SDL_JOYHATMOTION:
      return HandleJoystickHatEvent(&event->jhat);

    case SDL_JOYBUTTONDOWN:
    case SDL_JOYBUTTONUP:
      return HandleJoystickButtonEvent(&event->jbutton);

    default:
      return false;
  }
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
  cd.player_id = player_id;
  cd.joystick_id = joystick_id;
  cd.haptic_left_right_effect = -1;
  cd.game_controller = gcontroller;

#if SDL_VERSION_ATLEAST(2, 0, 9)
  cd.use_game_controller_rumble = (SDL_GameControllerRumble(gcontroller, 0, 0, 0) == 0);
#else
  cd.use_game_controller_rumble = false;
#endif

  if (cd.use_game_controller_rumble)
  {
    Log_InfoPrintf("Rumble is supported on '%s' via gamecontroller", SDL_GameControllerName(gcontroller));
  }
  else
  {
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
      Log_InfoPrintf("Rumble is supported on '%s' via haptic", SDL_GameControllerName(gcontroller));
  }

  if (!cd.haptic && !cd.use_game_controller_rumble)
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

  SDL_GameControllerClose(static_cast<SDL_GameController*>(it->game_controller));
  m_controllers.erase(it);

  if (notify)
    OnControllerDisconnected(player_id);
  return true;
}

bool SDLControllerInterface::OpenJoystick(int index)
{
  SDL_Joystick* joystick = SDL_JoystickOpen(index);
  if (!joystick)
  {
    Log_WarningPrintf("Failed to open joystick %d", index);

    return false;
  }

  int joystick_id = SDL_JoystickInstanceID(joystick);
#if SDL_VERSION_ATLEAST(2, 0, 9)
  int player_id = SDL_JoystickGetDevicePlayerIndex(index);
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

  const char* name = SDL_JoystickName(joystick);

  Log_InfoPrintf("Opened controller %d (instance id %d, player id %d): %s", index, joystick_id, player_id, name);

  ControllerData cd = {};
  cd.player_id = player_id;
  cd.joystick_id = joystick_id;
  cd.haptic_left_right_effect = -1;
  cd.game_controller = nullptr;
  cd.use_game_controller_rumble = false;

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
    Log_InfoPrintf("Rumble is supported on '%s'", name);
  else
    Log_WarningPrintf("Rumble is not supported on '%s'", name);

  m_controllers.push_back(std::move(cd));
  OnControllerConnected(player_id);
  return true;
}

bool SDLControllerInterface::HandleJoystickAxisEvent(const SDL_JoyAxisEvent* event)
{
  const float value = static_cast<float>(event->value) / (event->value < 0 ? 32768.0f : 32767.0f);
  Log_DebugPrintf("controller %d axis %d %d %f", event->which, event->axis, event->value, value);

  auto it = GetControllerDataForJoystickId(event->which);
  if (it == m_controllers.end() || it->IsGameController())
    return false;

  if (DoEventHook(Hook::Type::Axis, it->player_id, event->axis, value, true))
    return true;

  bool processed = false;

  const AxisCallback& cb = it->axis_mapping[event->axis][AxisSide::Full];
  if (cb)
  {
    cb(value);
    processed = true;
  }

  if (value > 0.0f)
  {
    const AxisCallback& hcb = it->axis_mapping[event->axis][AxisSide::Positive];
    if (hcb)
    {
      hcb(value);
      processed = true;
    }
  }
  else if (value < 0.0f)
  {
    const AxisCallback& hcb = it->axis_mapping[event->axis][AxisSide::Negative];
    if (hcb)
    {
      hcb(value);
      processed = true;
    }
  }

  if (processed)
    return true;

  // set the other direction to false so large movements don't leave the opposite on
  const bool outside_deadzone = (std::abs(value) >= it->deadzone);
  const bool positive = (value >= 0.0f);
  const ButtonCallback& other_button_cb = it->axis_button_mapping[event->axis][BoolToUInt8(!positive)];
  const ButtonCallback& button_cb = it->axis_button_mapping[event->axis][BoolToUInt8(positive)];
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

bool SDLControllerInterface::HandleJoystickButtonEvent(const SDL_JoyButtonEvent* event)
{
  Log_DebugPrintf("controller %d button %d %s", event->which, event->button,
                  event->state == SDL_PRESSED ? "pressed" : "released");

  auto it = GetControllerDataForJoystickId(event->which);
  if (it == m_controllers.end() || it->IsGameController())
    return false;

  const bool pressed = (event->state == SDL_PRESSED);
  if (DoEventHook(Hook::Type::Button, it->player_id, event->button, pressed ? 1.0f : 0.0f))
    return true;

  const ButtonCallback& cb = it->button_mapping[event->button];
  if (cb)
  {
    cb(pressed);
    return true;
  }

  const AxisCallback& axis_cb = it->button_axis_mapping[event->button];
  if (axis_cb)
  {
    axis_cb(pressed ? 1.0f : -1.0f);
    return true;
  }

  return false;
}

bool SDLControllerInterface::HandleJoystickHatEvent(const SDL_JoyHatEvent* event)
{
  Log_DebugPrintf("controller %d hat %d %d", event->which, event->hat, event->value);

  auto it = GetControllerDataForJoystickId(event->which);
  if (it == m_controllers.end() || it->IsGameController())
    return false;

  auto HatEventHook = [hat = event->hat, value = event->value, player_id = it->player_id, this](int hat_position) {
    if ((value & hat_position) == 0)
      return false;

    std::string_view position_str;
    switch (value)
    {
      case SDL_HAT_UP:
        position_str = "Up";
        break;
      case SDL_HAT_RIGHT:
        position_str = "Right";
        break;
      case SDL_HAT_DOWN:
        position_str = "Down";
        break;
      case SDL_HAT_LEFT:
        position_str = "Left";
        break;
      default:
        return false;
    }

    return DoEventHook(Hook::Type::Hat, player_id, hat, position_str);
  };

  if (event->value == SDL_HAT_CENTERED)
  {
    if (HatEventHook(SDL_HAT_CENTERED))
      return true;
  }
  else
  {
    // event->value can be a bitmask of multiple direction, so probe them all
    if (HatEventHook(SDL_HAT_UP) || HatEventHook(SDL_HAT_RIGHT) || HatEventHook(SDL_HAT_DOWN) ||
        HatEventHook(SDL_HAT_LEFT))
      return true;
  }

  bool processed = false;

  if (event->hat < it->hat_button_mapping.size())
  {
    if (const ButtonCallback& cb = it->hat_button_mapping[event->hat][0]; cb)
    {
      cb(event->value & SDL_HAT_UP);
      processed = true;
    }
    if (const ButtonCallback& cb = it->hat_button_mapping[event->hat][1]; cb)
    {
      cb(event->value & SDL_HAT_RIGHT);
      processed = true;
    }
    if (const ButtonCallback& cb = it->hat_button_mapping[event->hat][2]; cb)
    {
      cb(event->value & SDL_HAT_DOWN);
      processed = true;
    }
    if (const ButtonCallback& cb = it->hat_button_mapping[event->hat][3]; cb)
    {
      cb(event->value & SDL_HAT_LEFT);
      processed = true;
    }
  }

  return processed;
}

void SDLControllerInterface::ClearBindings()
{
  for (auto& it : m_controllers)
  {
    it.axis_mapping.fill({});
    it.button_mapping.fill({});
    it.axis_button_mapping.fill({});
    it.button_axis_mapping.fill({});
    it.hat_button_mapping.clear();
  }
}

bool SDLControllerInterface::BindControllerAxis(int controller_index, int axis_number, AxisSide axis_side,
                                                AxisCallback callback)
{
  auto it = GetControllerDataForPlayerId(controller_index);
  if (it == m_controllers.end())
    return false;

  if (axis_number < 0 || axis_number >= MAX_NUM_AXES)
    return false;

  it->axis_mapping[axis_number][axis_side] = std::move(callback);
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

  if (axis_number < 0 || axis_number >= MAX_NUM_AXES)
    return false;

  it->axis_button_mapping[axis_number][BoolToUInt8(direction)] = std::move(callback);
  return true;
}

bool SDLControllerInterface::BindControllerHatToButton(int controller_index, int hat_number,
                                                       std::string_view hat_position, ButtonCallback callback)
{
  auto it = GetControllerDataForPlayerId(controller_index);
  if (it == m_controllers.end())
    return false;

  size_t index;
  if (hat_position == "Up")
    index = 0;
  else if (hat_position == "Right")
    index = 1;
  else if (hat_position == "Down")
    index = 2;
  else if (hat_position == "Left")
    index = 3;
  else
    return false;

  // We need 4 entries per hat_number
  if (static_cast<int>(it->hat_button_mapping.size()) < hat_number + 1)
    it->hat_button_mapping.resize(hat_number + 1);

  it->hat_button_mapping[hat_number][index] = std::move(callback);
  return true;
}

bool SDLControllerInterface::BindControllerButtonToAxis(int controller_index, int button_number, AxisCallback callback)
{
  auto it = GetControllerDataForPlayerId(controller_index);
  if (it == m_controllers.end())
    return false;

  if (button_number < 0 || button_number >= MAX_NUM_BUTTONS)
    return false;

  it->button_axis_mapping[button_number] = std::move(callback);
  return true;
}

bool SDLControllerInterface::HandleControllerAxisEvent(const SDL_ControllerAxisEvent* ev)
{
  const float value = static_cast<float>(ev->value) / (ev->value < 0 ? 32768.0f : 32767.0f);
  Log_DebugPrintf("controller %d axis %d %d %f", ev->which, ev->axis, ev->value, value);

  auto it = GetControllerDataForJoystickId(ev->which);
  if (it == m_controllers.end())
    return false;

  if (DoEventHook(Hook::Type::Axis, it->player_id, ev->axis, value))
    return true;

  if (ev->axis >= MAX_NUM_AXES)
    return false;

  const AxisCallback& cb = it->axis_mapping[ev->axis][AxisSide::Full];
  if (cb)
  {
    cb(value);
    return true;
  }
  else
  {
    const AxisCallback& positive_cb = it->axis_mapping[ev->axis][AxisSide::Positive];
    const AxisCallback& negative_cb = it->axis_mapping[ev->axis][AxisSide::Negative];
    if (positive_cb || negative_cb)
    {
      if (positive_cb)
        positive_cb((value < 0.0f) ? 0.0f : value);
      if (negative_cb)
        negative_cb((value >= 0.0f) ? 0.0f : -value);

      return true;
    }
  }

  // set the other direction to false so large movements don't leave the opposite on
  const bool outside_deadzone = (std::abs(value) >= it->deadzone);
  const bool positive = (value >= 0.0f);
  const ButtonCallback& other_button_cb = it->axis_button_mapping[ev->axis][BoolToUInt8(!positive)];
  const ButtonCallback& button_cb = it->axis_button_mapping[ev->axis][BoolToUInt8(positive)];
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

bool SDLControllerInterface::HandleControllerButtonEvent(const SDL_ControllerButtonEvent* ev)
{
  Log_DebugPrintf("controller %d button %d %s", ev->which, ev->button,
                  ev->state == SDL_PRESSED ? "pressed" : "released");

  auto it = GetControllerDataForJoystickId(ev->which);
  if (it == m_controllers.end())
    return false;

  static constexpr std::array<FrontendCommon::ControllerNavigationButton, SDL_CONTROLLER_BUTTON_MAX>
    nav_button_mapping = {{
      FrontendCommon::ControllerNavigationButton::Activate,      // SDL_CONTROLLER_BUTTON_A
      FrontendCommon::ControllerNavigationButton::Cancel,        // SDL_CONTROLLER_BUTTON_B
      FrontendCommon::ControllerNavigationButton::Count,         // SDL_CONTROLLER_BUTTON_X
      FrontendCommon::ControllerNavigationButton::Count,         // SDL_CONTROLLER_BUTTON_Y
      FrontendCommon::ControllerNavigationButton::Count,         // SDL_CONTROLLER_BUTTON_BACK
      FrontendCommon::ControllerNavigationButton::Count,         // SDL_CONTROLLER_BUTTON_GUIDE
      FrontendCommon::ControllerNavigationButton::Count,         // SDL_CONTROLLER_BUTTON_START
      FrontendCommon::ControllerNavigationButton::Count,         // SDL_CONTROLLER_BUTTON_LEFTSTICK
      FrontendCommon::ControllerNavigationButton::Count,         // SDL_CONTROLLER_BUTTON_RIGHTSTICK
      FrontendCommon::ControllerNavigationButton::LeftShoulder,  // SDL_CONTROLLER_BUTTON_LEFTSHOULDER
      FrontendCommon::ControllerNavigationButton::RightShoulder, // SDL_CONTROLLER_BUTTON_RIGHTSHOULDER
      FrontendCommon::ControllerNavigationButton::DPadUp,        // SDL_CONTROLLER_BUTTON_DPAD_UP
      FrontendCommon::ControllerNavigationButton::DPadDown,      // SDL_CONTROLLER_BUTTON_DPAD_DOWN
      FrontendCommon::ControllerNavigationButton::DPadLeft,      // SDL_CONTROLLER_BUTTON_DPAD_LEFT
      FrontendCommon::ControllerNavigationButton::DPadRight,     // SDL_CONTROLLER_BUTTON_DPAD_RIGHT
    }};

  const bool pressed = (ev->state == SDL_PRESSED);
  if (DoEventHook(Hook::Type::Button, it->player_id, ev->button, pressed ? 1.0f : 0.0f))
    return true;

  if (ev->button < nav_button_mapping.size() &&
      nav_button_mapping[ev->button] != FrontendCommon::ControllerNavigationButton::Count)
  {
    m_host_interface->SetControllerNavigationButtonState(nav_button_mapping[ev->button], pressed);
  }

  if (m_host_interface->IsControllerNavigationActive())
  {
    // UI consumed the event
    return true;
  }

  if (ev->button >= MAX_NUM_BUTTONS)
    return false;

  const ButtonCallback& cb = it->button_mapping[ev->button];
  if (cb)
  {
    cb(pressed);
    return true;
  }

  const AxisCallback& axis_cb = it->button_axis_mapping[ev->button];
  if (axis_cb)
  {
    axis_cb(pressed ? 1.0f : -1.0f);
    return true;
  }

  return false;
}

u32 SDLControllerInterface::GetControllerRumbleMotorCount(int controller_index)
{
  auto it = GetControllerDataForPlayerId(controller_index);
  if (it == m_controllers.end())
    return 0;

  return (it->use_game_controller_rumble ? 2 : ((it->haptic_left_right_effect >= 0) ? 2 : (it->haptic ? 1 : 0)));
}

void SDLControllerInterface::SetControllerRumbleStrength(int controller_index, const float* strengths, u32 num_motors)
{
  auto it = GetControllerDataForPlayerId(controller_index);
  if (it == m_controllers.end())
    return;

  // we'll update before this duration is elapsed
  static constexpr u32 DURATION = 65535; // SDL_MAX_RUMBLE_DURATION_MS

#if SDL_VERSION_ATLEAST(2, 0, 9)
  if (it->use_game_controller_rumble)
  {
    const u16 large = static_cast<u16>(strengths[0] * 65535.0f);
    const u16 small = static_cast<u16>(strengths[1] * 65535.0f);
    SDL_GameControllerRumble(static_cast<SDL_GameController*>(it->game_controller), large, small, DURATION);
    return;
  }
#endif

  SDL_Haptic* haptic = static_cast<SDL_Haptic*>(it->haptic);
  if (it->haptic_left_right_effect >= 0 && num_motors > 1)
  {
    if (strengths[0] > 0.0f || strengths[1] > 0.0f)
    {
      SDL_HapticEffect ef;
      ef.type = SDL_HAPTIC_LEFTRIGHT;
      ef.leftright.large_magnitude = static_cast<u16>(strengths[0] * 65535.0f);
      ef.leftright.small_magnitude = static_cast<u16>(strengths[1] * 65535.0f);
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

    if (max_strength > 0.0f)
      SDL_HapticRumblePlay(haptic, max_strength, DURATION);
    else
      SDL_HapticRumbleStop(haptic);
  }
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
