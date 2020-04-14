#include "common_host_interface.h"
#include "common/assert.h"
#include "common/audio_stream.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/string_util.h"
#include "controller_interface.h"
#include "core/controller.h"
#include "core/game_list.h"
#include "core/gpu.h"
#include "core/system.h"
#ifdef WITH_SDL2
#include "sdl_audio_stream.h"
#include "sdl_controller_interface.h"
#endif
#include "ini_settings_interface.h"
#include <cstdio>
#include <cstring>
Log_SetChannel(CommonHostInterface);

CommonHostInterface::CommonHostInterface() = default;

CommonHostInterface::~CommonHostInterface() = default;

bool CommonHostInterface::Initialize()
{
  if (!HostInterface::Initialize())
    return false;

  // Change to the user directory so that all default/relative paths in the config are after this.
  if (!FileSystem::SetWorkingDirectory(m_user_directory.c_str()))
    Log_ErrorPrintf("Failed to set working directory to '%s'", m_user_directory.c_str());

  RegisterGeneralHotkeys();
  RegisterGraphicsHotkeys();
  RegisterSaveStateHotkeys();

  m_controller_interface = CreateControllerInterface();
  if (m_controller_interface && !m_controller_interface->Initialize(this))
  {
    Log_WarningPrintf("Failed to initialize controller bindings are not possible.");
    m_controller_interface.reset();
  }
  else if (!m_controller_interface)
  {
    Log_WarningPrintf("No controller interface created, controller bindings are not possible.");
  }

  return true;
}

void CommonHostInterface::Shutdown()
{
  HostInterface::Shutdown();

  m_system.reset();
  m_audio_stream.reset();
  if (m_display)
    ReleaseHostDisplay();

  if (m_controller_interface)
  {
    m_controller_interface->Shutdown();
    m_controller_interface.reset();
  }
}

bool CommonHostInterface::BootSystem(const SystemBootParameters& parameters)
{
  if (!HostInterface::BootSystem(parameters))
  {
    // if in batch mode, exit immediately if booting failed
    if (m_batch_mode)
      RequestExit();

    return false;
  }

  // enter fullscreen if requested in the parameters
  if ((parameters.override_fullscreen.has_value() && *parameters.override_fullscreen) ||
      (!parameters.override_fullscreen.has_value() && m_settings.start_fullscreen))
  {
    SetFullscreen(true);
  }

  return true;
}

void CommonHostInterface::PowerOffSystem()
{
  HostInterface::PowerOffSystem();

  // TODO: Do we want to move the resume state saving here?

  if (m_batch_mode)
    RequestExit();
}

static void PrintCommandLineVersion(const char* frontend_name)
{
  std::fprintf(stderr, "%s Version <TODO>\n", frontend_name);
  std::fprintf(stderr, "https://github.com/stenzek/duckstation\n");
  std::fprintf(stderr, "\n");
}

static void PrintCommandLineHelp(const char* progname, const char* frontend_name)
{
  PrintCommandLineVersion(frontend_name);
  std::fprintf(stderr, "Usage: %s [parameters] [--] [boot filename]\n", progname);
  std::fprintf(stderr, "\n");
  std::fprintf(stderr, "  -help: Displays this information and exits.\n");
  std::fprintf(stderr, "  -version: Displays version information and exits.\n");
  std::fprintf(stderr, "  -batch: Enables batch mode (exits after powering off).\n");
  std::fprintf(stderr, "  -fastboot: Force fast boot for provided filename.\n");
  std::fprintf(stderr, "  -slowboot: Force slow boot for provided filename.\n");
  std::fprintf(stderr, "  -resume: Load resume save state. If a boot filename is provided,\n"
                       "    that game's resume state will be loaded, otherwise the most\n"
                       "    recent resume save state will be loaded.\n");
  std::fprintf(stderr, "  -state <index>: Loads specified save state by index. If a boot\n"
                       "    filename is provided, a per-game state will be loaded, otherwise\n"
                       "    a global state will be loaded.\n");
  std::fprintf(stderr, "  -statefile <filename>: Loads state from the specified filename.\n"
                       "    No boot filename is required with this option.\n");
  std::fprintf(stderr, "  -fullscreen: Enters fullscreen mode immediately after starting.\n");
  std::fprintf(stderr, "  -nofullscreen: Prevents fullscreen mode from triggering if enabled.\n");
  std::fprintf(stderr, "  -portable: Forces \"portable mode\", data in same directory.\n");
  std::fprintf(stderr, "  --: Signals that no more arguments will follow and the remaining\n"
                       "    parameters make up the filename. Use when the filename contains\n"
                       "    spaces or starts with a dash.\n");
  std::fprintf(stderr, "\n");
}

bool CommonHostInterface::ParseCommandLineParameters(int argc, char* argv[],
                                                     std::unique_ptr<SystemBootParameters>* out_boot_params)
{
  std::optional<bool> force_fast_boot;
  std::optional<bool> force_fullscreen;
  std::optional<s32> state_index;
  std::string state_filename;
  std::string boot_filename;
  bool no_more_args = false;

  for (int i = 1; i < argc; i++)
  {
    if (!no_more_args)
    {
#define CHECK_ARG(str) !std::strcmp(argv[i], str)
#define CHECK_ARG_PARAM(str) (!std::strcmp(argv[i], str) && ((i + 1) < argc))

      if (CHECK_ARG("-help"))
      {
        PrintCommandLineHelp(argv[0], GetFrontendName());
        return false;
      }
      else if (CHECK_ARG("-version"))
      {
        PrintCommandLineVersion(GetFrontendName());
        return false;
      }
      else if (CHECK_ARG("-batch"))
      {
        Log_InfoPrintf("Enabling batch mode.");
        m_batch_mode = true;
        continue;
      }
      else if (CHECK_ARG("-fastboot"))
      {
        Log_InfoPrintf("Forcing fast boot.");
        force_fast_boot = true;
        continue;
      }
      else if (CHECK_ARG("-slowboot"))
      {
        Log_InfoPrintf("Forcing slow boot.");
        force_fast_boot = false;
        continue;
      }
      else if (CHECK_ARG("-resume"))
      {
        state_index = -1;
        continue;
      }
      else if (CHECK_ARG_PARAM("-state"))
      {
        state_index = std::atoi(argv[++i]);
        continue;
      }
      else if (CHECK_ARG_PARAM("-statefile"))
      {
        state_filename = argv[++i];
        continue;
      }
      else if (CHECK_ARG("-fullscreen"))
      {
        Log_InfoPrintf("Going fullscreen after booting.");
        force_fullscreen = true;
        continue;
      }
      else if (CHECK_ARG("-nofullscreen"))
      {
        Log_InfoPrintf("Preventing fullscreen after booting.");
        force_fullscreen = false;
        continue;
      }
      else if (CHECK_ARG("-portable"))
      {
        Log_InfoPrintf("Using portable mode.");
        SetUserDirectoryToProgramDirectory();
        continue;
      }
      else if (CHECK_ARG_PARAM("-resume"))
      {
        state_index = -1;
        continue;
      }
      else if (CHECK_ARG("--"))
      {
        no_more_args = true;
        continue;
      }
      else if (argv[i][0] == '-')
      {
        Log_ErrorPrintf("Unknown parameter: '%s'", argv[i]);
        return false;
      }

#undef CHECK_ARG
#undef CHECK_ARG_PARAM
    }

    if (!boot_filename.empty())
      boot_filename += ' ';
    boot_filename += argv[i];
  }

  if (state_index.has_value() || !boot_filename.empty() || !state_filename.empty())
  {
    // init user directory early since we need it for save states
    SetUserDirectory();

    if (state_index.has_value() && !state_filename.empty())
    {
      // if a save state is provided, whether a boot filename was provided determines per-game/local
      if (boot_filename.empty())
      {
        // loading a global state. if this is -1, we're loading the most recent resume state
        if (*state_index < 0)
          state_filename = GetMostRecentResumeSaveStatePath();
        else
          state_filename = GetGlobalSaveStateFileName(*state_index);

        if (state_filename.empty() || !FileSystem::FileExists(state_filename.c_str()))
        {
          Log_ErrorPrintf("Could not find file for global save state %d", *state_index);
          return false;
        }
      }
      else
      {
        // find the game id, and get its save state path
        std::string game_code = m_game_list->GetGameCodeForPath(boot_filename.c_str());
        if (game_code.empty())
        {
          Log_WarningPrintf("Could not identify game code for '%s', cannot load save state %d.", boot_filename.c_str(),
                            *state_index);
        }
        else
        {
          state_filename = GetGameSaveStateFileName(game_code.c_str(), *state_index);
          if (state_filename.empty() || !FileSystem::FileExists(state_filename.c_str()))
          {
            Log_ErrorPrintf("Could not find file for game '%s' save state %d", game_code.c_str(), *state_index);
            return false;
          }
        }
      }
    }

    std::unique_ptr<SystemBootParameters> boot_params = std::make_unique<SystemBootParameters>();
    boot_params->filename = std::move(boot_filename);
    boot_params->state_filename = std::move(state_filename);
    boot_params->override_fast_boot = std::move(force_fast_boot);
    boot_params->override_fullscreen = std::move(force_fullscreen);
    *out_boot_params = std::move(boot_params);
  }

  return true;
}

bool CommonHostInterface::IsFullscreen() const
{
  return false;
}

bool CommonHostInterface::SetFullscreen(bool enabled)
{
  return false;
}

std::unique_ptr<AudioStream> CommonHostInterface::CreateAudioStream(AudioBackend backend)
{
  switch (backend)
  {
    case AudioBackend::Null:
      return AudioStream::CreateNullAudioStream();

    case AudioBackend::Cubeb:
      return AudioStream::CreateCubebAudioStream();

#ifdef WITH_SDL2
    case AudioBackend::SDL:
      return SDLAudioStream::Create();
#endif

    default:
      return nullptr;
  }
}

std::unique_ptr<ControllerInterface> CommonHostInterface::CreateControllerInterface()
{
  // In the future we might want to use different controller interfaces.
#ifdef WITH_SDL2
  return std::make_unique<SDLControllerInterface>();
#else
  return nullptr;
#endif
}

void CommonHostInterface::OnSystemCreated()
{
  HostInterface::OnSystemCreated();
}

void CommonHostInterface::OnSystemPaused(bool paused)
{
  HostInterface::OnSystemPaused(paused);

  if (paused)
  {
    if (IsFullscreen())
      SetFullscreen(false);

    StopControllerRumble();
  }
}

void CommonHostInterface::OnSystemDestroyed()
{
  HostInterface::OnSystemDestroyed();

  StopControllerRumble();
}

void CommonHostInterface::OnControllerTypeChanged(u32 slot)
{
  HostInterface::OnControllerTypeChanged(slot);

  UpdateInputMap();
}

void CommonHostInterface::SetDefaultSettings(SettingsInterface& si)
{
  HostInterface::SetDefaultSettings(si);

  si.SetStringValue("Controller1", "ButtonUp", "Keyboard/W");
  si.SetStringValue("Controller1", "ButtonDown", "Keyboard/S");
  si.SetStringValue("Controller1", "ButtonLeft", "Keyboard/A");
  si.SetStringValue("Controller1", "ButtonRight", "Keyboard/D");
  si.SetStringValue("Controller1", "ButtonSelect", "Keyboard/Backspace");
  si.SetStringValue("Controller1", "ButtonStart", "Keyboard/Return");
  si.SetStringValue("Controller1", "ButtonTriangle", "Keyboard/Keypad+8");
  si.SetStringValue("Controller1", "ButtonCross", "Keyboard/Keypad+2");
  si.SetStringValue("Controller1", "ButtonSquare", "Keyboard/Keypad+4");
  si.SetStringValue("Controller1", "ButtonCircle", "Keyboard/Keypad+6");
  si.SetStringValue("Controller1", "ButtonL1", "Keyboard/Q");
  si.SetStringValue("Controller1", "ButtonL2", "Keyboard/1");
  si.SetStringValue("Controller1", "ButtonR1", "Keyboard/E");
  si.SetStringValue("Controller1", "ButtonR2", "Keyboard/3");
  si.SetStringValue("Hotkeys", "FastForward", "Keyboard/Tab");
  si.SetStringValue("Hotkeys", "TogglePause", "Keyboard/Pause");
  si.SetStringValue("Hotkeys", "ToggleFullscreen", "Keyboard/Alt+Return");
  si.SetStringValue("Hotkeys", "PowerOff", "Keyboard/Escape");
  si.SetStringValue("Hotkeys", "Screenshot", "Keyboard/F10");
  si.SetStringValue("Hotkeys", "IncreaseResolutionScale", "Keyboard/PageUp");
  si.SetStringValue("Hotkeys", "DecreaseResolutionScale", "Keyboard/PageDown");
  si.SetStringValue("Hotkeys", "ToggleSoftwareRendering", "Keyboard/End");
}

std::optional<CommonHostInterface::HostKeyCode>
CommonHostInterface::GetHostKeyCode(const std::string_view key_code) const
{
  return std::nullopt;
}

void CommonHostInterface::RegisterHotkey(String category, String name, String display_name, InputButtonHandler handler)
{
  m_hotkeys.push_back(HotkeyInfo{std::move(category), std::move(name), std::move(display_name), std::move(handler)});
}

bool CommonHostInterface::HandleHostKeyEvent(HostKeyCode key, bool pressed)
{
  const auto iter = m_keyboard_input_handlers.find(key);
  if (iter == m_keyboard_input_handlers.end())
    return false;

  iter->second(pressed);
  return true;
}

void CommonHostInterface::UpdateInputMap(SettingsInterface& si)
{
  m_keyboard_input_handlers.clear();
  if (m_controller_interface)
    m_controller_interface->ClearBindings();

  UpdateControllerInputMap(si);
  UpdateHotkeyInputMap(si);
}

void CommonHostInterface::AddControllerRumble(u32 controller_index, u32 num_motors, ControllerRumbleCallback callback)
{
  ControllerRumbleState rumble;
  rumble.controller_index = 0;
  rumble.num_motors = std::min<u32>(num_motors, ControllerRumbleState::MAX_MOTORS);
  rumble.last_strength.fill(0.0f);
  rumble.update_callback = std::move(callback);
  m_controller_vibration_motors.push_back(std::move(rumble));
}

void CommonHostInterface::UpdateControllerRumble()
{
  DebugAssert(m_system);

  for (ControllerRumbleState& rumble : m_controller_vibration_motors)
  {
    Controller* controller = m_system->GetController(rumble.controller_index);
    if (!controller)
      continue;

    bool changed = false;
    for (u32 i = 0; i < rumble.num_motors; i++)
    {
      const float strength = controller->GetVibrationMotorStrength(i);
      changed |= (strength != rumble.last_strength[i]);
      rumble.last_strength[i] = strength;
    }

    if (changed)
      rumble.update_callback(rumble.last_strength.data(), rumble.num_motors);
  }
}

void CommonHostInterface::StopControllerRumble()
{
  for (ControllerRumbleState& rumble : m_controller_vibration_motors)
  {
    bool changed = true;
    for (u32 i = 0; i < rumble.num_motors; i++)
    {
      changed |= (rumble.last_strength[i] != 0.0f);
      rumble.last_strength[i] = 0.0f;
    }

    if (changed)
      rumble.update_callback(rumble.last_strength.data(), rumble.num_motors);
  }
}

static bool SplitBinding(const std::string& binding, std::string_view* device, std::string_view* sub_binding)
{
  const std::string::size_type slash_pos = binding.find('/');
  if (slash_pos == std::string::npos)
  {
    Log_WarningPrintf("Malformed binding: '%s'", binding.c_str());
    return false;
  }

  *device = std::string_view(binding).substr(0, slash_pos);
  *sub_binding = std::string_view(binding).substr(slash_pos + 1);
  return true;
}

void CommonHostInterface::UpdateControllerInputMap(SettingsInterface& si)
{
  StopControllerRumble();
  m_controller_vibration_motors.clear();

  for (u32 controller_index = 0; controller_index < 2; controller_index++)
  {
    const ControllerType ctype = m_settings.controller_types[controller_index];
    if (ctype == ControllerType::None)
      continue;

    const auto category = TinyString::FromFormat("Controller%u", controller_index + 1);
    const auto button_names = Controller::GetButtonNames(ctype);
    for (const auto& it : button_names)
    {
      const std::string& button_name = it.first;
      const s32 button_code = it.second;

      const std::vector<std::string> bindings =
        si.GetStringList(category, TinyString::FromFormat("Button%s", button_name.c_str()));
      for (const std::string& binding : bindings)
      {
        std::string_view device, button;
        if (!SplitBinding(binding, &device, &button))
          continue;

        AddButtonToInputMap(binding, device, button, [this, controller_index, button_code](bool pressed) {
          if (!m_system)
            return;

          Controller* controller = m_system->GetController(controller_index);
          if (controller)
            controller->SetButtonState(button_code, pressed);
        });
      }
    }

    const auto axis_names = Controller::GetAxisNames(ctype);
    for (const auto& it : axis_names)
    {
      const std::string& axis_name = it.first;
      const s32 axis_code = it.second;

      const std::vector<std::string> bindings =
        si.GetStringList(category, TinyString::FromFormat("Axis%s", axis_name.c_str()));
      for (const std::string& binding : bindings)
      {
        std::string_view device, axis;
        if (!SplitBinding(binding, &device, &axis))
          continue;

        AddAxisToInputMap(binding, device, axis, [this, controller_index, axis_code](float value) {
          if (!m_system)
            return;

          Controller* controller = m_system->GetController(controller_index);
          if (controller)
            controller->SetAxisState(axis_code, value);
        });
      }
    }

    const u32 num_motors = Controller::GetVibrationMotorCount(ctype);
    if (num_motors > 0)
    {
      const std::vector<std::string> bindings = si.GetStringList(category, TinyString::FromFormat("Rumble"));
      for (const std::string& binding : bindings)
        AddRumbleToInputMap(binding, controller_index, num_motors);
    }
  }
}

void CommonHostInterface::UpdateHotkeyInputMap(SettingsInterface& si)
{
  for (const HotkeyInfo& hi : m_hotkeys)
  {
    const std::vector<std::string> bindings = si.GetStringList("Hotkeys", hi.name);
    for (const std::string& binding : bindings)
    {
      std::string_view device, button;
      if (!SplitBinding(binding, &device, &button))
        continue;

      AddButtonToInputMap(binding, device, button, hi.handler);
    }
  }
}

bool CommonHostInterface::AddButtonToInputMap(const std::string& binding, const std::string_view& device,
                                              const std::string_view& button, InputButtonHandler handler)
{
  if (device == "Keyboard")
  {
    std::optional<int> key_id = GetHostKeyCode(button);
    if (!key_id.has_value())
    {
      Log_WarningPrintf("Unknown keyboard key in binding '%s'", binding.c_str());
      return false;
    }

    m_keyboard_input_handlers.emplace(key_id.value(), std::move(handler));
    return true;
  }

  if (StringUtil::StartsWith(device, "Controller"))
  {
    if (!m_controller_interface)
    {
      Log_ErrorPrintf("No controller interface set, cannot bind '%s'", binding.c_str());
      return false;
    }

    const std::optional<int> controller_index = StringUtil::FromChars<int>(device.substr(10));
    if (!controller_index || *controller_index < 0)
    {
      Log_WarningPrintf("Invalid controller index in button binding '%s'", binding.c_str());
      return false;
    }

    if (StringUtil::StartsWith(button, "Button"))
    {
      const std::optional<int> button_index = StringUtil::FromChars<int>(button.substr(6));
      if (!button_index ||
          !m_controller_interface->BindControllerButton(*controller_index, *button_index, std::move(handler)))
      {
        Log_WarningPrintf("Failed to bind controller button '%s' to button", binding.c_str());
        return false;
      }

      return true;
    }
    else if (StringUtil::StartsWith(button, "+Axis") || StringUtil::StartsWith(button, "-Axis"))
    {
      const std::optional<int> axis_index = StringUtil::FromChars<int>(button.substr(5));
      const bool positive = (button[0] == '+');
      if (!axis_index || !m_controller_interface->BindControllerAxisToButton(*controller_index, *axis_index, positive,
                                                                             std::move(handler)))
      {
        Log_WarningPrintf("Failed to bind controller axis '%s' to button", binding.c_str());
        return false;
      }

      return true;
    }

    Log_WarningPrintf("Malformed controller binding '%s' in button", binding.c_str());
    return false;
  }

  Log_WarningPrintf("Unknown input device in button binding '%s'", binding.c_str());
  return false;
}

bool CommonHostInterface::AddAxisToInputMap(const std::string& binding, const std::string_view& device,
                                            const std::string_view& axis, InputAxisHandler handler)
{
  if (StringUtil::StartsWith(device, "Controller"))
  {
    if (!m_controller_interface)
    {
      Log_ErrorPrintf("No controller interface set, cannot bind '%s'", binding.c_str());
      return false;
    }

    const std::optional<int> controller_index = StringUtil::FromChars<int>(device.substr(10));
    if (!controller_index || *controller_index < 0)
    {
      Log_WarningPrintf("Invalid controller index in axis binding '%s'", binding.c_str());
      return false;
    }

    if (StringUtil::StartsWith(axis, "Axis"))
    {
      const std::optional<int> axis_index = StringUtil::FromChars<int>(axis.substr(4));
      if (!axis_index ||
          !m_controller_interface->BindControllerAxis(*controller_index, *axis_index, std::move(handler)))
      {
        Log_WarningPrintf("Failed to bind controller axis '%s' to axi", binding.c_str());
        return false;
      }

      return true;
    }

    Log_WarningPrintf("Malformed controller binding '%s' in button", binding.c_str());
    return false;
  }

  Log_WarningPrintf("Unknown input device in axis binding '%s'", binding.c_str());
  return false;
}

bool CommonHostInterface::AddRumbleToInputMap(const std::string& binding, u32 controller_index, u32 num_motors)
{
  if (StringUtil::StartsWith(binding, "Controller"))
  {
    if (!m_controller_interface)
    {
      Log_ErrorPrintf("No controller interface set, cannot bind '%s'", binding.c_str());
      return false;
    }

    const std::optional<int> host_controller_index = StringUtil::FromChars<int>(binding.substr(10));
    if (!host_controller_index || *host_controller_index < 0)
    {
      Log_WarningPrintf("Invalid controller index in rumble binding '%s'", binding.c_str());
      return false;
    }

    AddControllerRumble(controller_index, num_motors,
                        std::bind(&ControllerInterface::SetControllerRumbleStrength, m_controller_interface.get(),
                                  host_controller_index.value(), std::placeholders::_1, std::placeholders::_2));

    return true;
  }

  Log_WarningPrintf("Unknown input device in rumble binding '%s'", binding.c_str());
  return false;
}

void CommonHostInterface::RegisterGeneralHotkeys()
{
  RegisterHotkey(StaticString("General"), StaticString("FastForward"), StaticString("Toggle Fast Forward"),
                 [this](bool pressed) {
                   m_speed_limiter_temp_disabled = pressed;
                   HostInterface::UpdateSpeedLimiterState();
                 });

  RegisterHotkey(StaticString("General"), StaticString("ToggleFullscreen"), StaticString("Toggle Fullscreen"),
                 [this](bool pressed) {
                   if (!pressed)
                     SetFullscreen(!IsFullscreen());
                 });

  RegisterHotkey(StaticString("General"), StaticString("TogglePause"), StaticString("Toggle Pause"),
                 [this](bool pressed) {
                   if (!pressed)
                     PauseSystem(!m_paused);
                 });

  RegisterHotkey(StaticString("General"), StaticString("PowerOff"), StaticString("Power Off System"),
                 [this](bool pressed) {
                   if (!pressed && m_system)
                   {
                     if (m_settings.confim_power_off && !m_batch_mode)
                     {
                       SmallString confirmation_message("Are you sure you want to stop emulation?");
                       if (m_settings.save_state_on_exit)
                         confirmation_message.AppendString("\n\nThe current state will be saved.");

                       if (!ConfirmMessage(confirmation_message))
                       {
                         m_system->ResetPerformanceCounters();
                         return;
                       }
                     }

                     PowerOffSystem();
                   }
                 });

  RegisterHotkey(StaticString("General"), StaticString("Screenshot"), StaticString("Save Screenshot"),
                 [this](bool pressed) {
                   if (!pressed && m_system)
                     SaveScreenshot();
                 });
}

void CommonHostInterface::RegisterGraphicsHotkeys()
{
  RegisterHotkey(StaticString("Graphics"), StaticString("ToggleSoftwareRendering"),
                 StaticString("Toggle Software Rendering"), [this](bool pressed) {
                   if (!pressed)
                     ToggleSoftwareRendering();
                 });

  RegisterHotkey(StaticString("Graphics"), StaticString("IncreaseResolutionScale"),
                 StaticString("Increase Resolution Scale"), [this](bool pressed) {
                   if (!pressed)
                     ModifyResolutionScale(1);
                 });

  RegisterHotkey(StaticString("Graphics"), StaticString("DecreaseResolutionScale"),
                 StaticString("Decrease Resolution Scale"), [this](bool pressed) {
                   if (!pressed)
                     ModifyResolutionScale(-1);
                 });
}

void CommonHostInterface::RegisterSaveStateHotkeys()
{
  for (u32 global_i = 0; global_i < 2; global_i++)
  {
    const bool global = ConvertToBoolUnchecked(global_i);
    const u32 count = global ? GLOBAL_SAVE_STATE_SLOTS : PER_GAME_SAVE_STATE_SLOTS;
    for (u32 slot = 1; slot <= count; slot++)
    {
      RegisterHotkey(StaticString("Save States"),
                     TinyString::FromFormat("Load%sState%u", global ? "Global" : "Game", slot),
                     TinyString::FromFormat("Load %s State %u", global ? "Global" : "Game", slot),
                     [this, global, slot](bool pressed) {
                       if (!pressed)
                         LoadState(global, slot);
                     });
      RegisterHotkey(StaticString("Save States"),
                     TinyString::FromFormat("Save%sState%u", global ? "Global" : "Game", slot),
                     TinyString::FromFormat("Save %s State %u", global ? "Global" : "Game", slot),
                     [this, global, slot](bool pressed) {
                       if (!pressed)
                         SaveState(global, slot);
                     });
    }
  }
}

std::vector<std::pair<std::string, std::string>> CommonHostInterface::GetInputProfileList() const
{
  FileSystem::FindResultsArray results;
  FileSystem::FindFiles(GetUserDirectoryRelativePath("inputprofiles").c_str(), "*.ini",
                        FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_RELATIVE_PATHS, &results);

  std::vector<std::pair<std::string, std::string>> profile_names;
  profile_names.reserve(results.size());
  for (auto& it : results)
  {
    if (it.FileName.size() < 4)
      continue;

    std::string profile_name = it.FileName.substr(0, it.FileName.length() - 4);
    std::string full_filename = GetUserDirectoryRelativePath("inputprofiles/%s", it.FileName.c_str());
    profile_names.emplace_back(std::move(profile_name), std::move(full_filename));
  }

  return profile_names;
}

void CommonHostInterface::ClearAllControllerBindings(SettingsInterface& si)
{
  for (u32 controller_index = 1; controller_index <= NUM_CONTROLLER_AND_CARD_PORTS; controller_index++)
  {
    const ControllerType ctype = m_settings.controller_types[controller_index - 1];
    if (ctype == ControllerType::None)
      continue;

    const auto section_name = TinyString::FromFormat("Controller%u", controller_index);

    si.DeleteValue(section_name, "Type");

    for (const auto& button : Controller::GetButtonNames(ctype))
      si.DeleteValue(section_name, button.first.c_str());

    for (const auto& axis : Controller::GetAxisNames(ctype))
      si.DeleteValue(section_name, axis.first.c_str());

    if (Controller::GetVibrationMotorCount(ctype) > 0)
      si.DeleteValue(section_name, "Rumble");
  }
}

void CommonHostInterface::ApplyInputProfile(const char* profile_path, SettingsInterface& si)
{
  // clear bindings for all controllers
  ClearAllControllerBindings(si);

  INISettingsInterface profile(profile_path);

  for (u32 controller_index = 1; controller_index <= NUM_CONTROLLER_AND_CARD_PORTS; controller_index++)
  {
    const auto section_name = TinyString::FromFormat("Controller%u", controller_index);
    const std::string ctype_str = profile.GetStringValue(section_name, "Type");
    if (ctype_str.empty())
      continue;

    std::optional<ControllerType> ctype = Settings::ParseControllerTypeName(ctype_str.c_str());
    if (!ctype)
    {
      Log_ErrorPrintf("Invalid controller type in profile: '%s'", ctype_str.c_str());
      return;
    }

    m_settings.controller_types[controller_index - 1] = *ctype;
    HostInterface::OnControllerTypeChanged(controller_index - 1);

    si.SetStringValue(section_name, "Type", Settings::GetControllerTypeName(*ctype));

    for (const auto& button : Controller::GetButtonNames(*ctype))
    {
      const auto key_name = TinyString::FromFormat("Button%s", button.first.c_str());
      si.DeleteValue(section_name, key_name);
      const std::vector<std::string> bindings = profile.GetStringList(section_name, key_name);
      for (const std::string& binding : bindings)
        si.AddToStringList(section_name, key_name, binding.c_str());
    }

    for (const auto& axis : Controller::GetAxisNames(*ctype))
    {
      const auto key_name = TinyString::FromFormat("Axis%s", axis.first.c_str());
      si.DeleteValue(section_name, axis.first.c_str());
      const std::vector<std::string> bindings = profile.GetStringList(section_name, key_name);
      for (const std::string& binding : bindings)
        si.AddToStringList(section_name, key_name, binding.c_str());
    }

    si.DeleteValue(section_name, "Rumble");
    const std::string rumble_value = profile.GetStringValue(section_name, "Rumble");
    if (!rumble_value.empty())
      si.SetStringValue(section_name, "Rumble", rumble_value.c_str());
  }

  UpdateInputMap(si);

  if (m_system)
    m_system->UpdateControllers();

  ReportFormattedMessage("Loaded input profile from '%s'", profile_path);
}

bool CommonHostInterface::SaveInputProfile(const char* profile_path, SettingsInterface& si)
{
  if (FileSystem::FileExists(profile_path))
  {
    if (!FileSystem::DeleteFile(profile_path))
    {
      Log_ErrorPrintf("Failed to delete existing input profile '%s' when saving", profile_path);
      return false;
    }
  }

  INISettingsInterface profile(profile_path);

  for (u32 controller_index = 1; controller_index <= NUM_CONTROLLER_AND_CARD_PORTS; controller_index++)
  {
    const ControllerType ctype = m_settings.controller_types[controller_index - 1];
    if (ctype == ControllerType::None)
      continue;

    const auto section_name = TinyString::FromFormat("Controller%u", controller_index);

    profile.SetStringValue(section_name, "Type", Settings::GetControllerTypeName(ctype));

    for (const auto& button : Controller::GetButtonNames(ctype))
    {
      const auto key_name = TinyString::FromFormat("Button%s", button.first.c_str());
      const std::vector<std::string> bindings = si.GetStringList(section_name, key_name);
      for (const std::string& binding : bindings)
        profile.AddToStringList(section_name, key_name, binding.c_str());
    }

    for (const auto& axis : Controller::GetAxisNames(ctype))
    {
      const auto key_name = TinyString::FromFormat("Axis%s", axis.first.c_str());
      const std::vector<std::string> bindings = si.GetStringList(section_name, key_name);
      for (const std::string& binding : bindings)
        profile.AddToStringList(section_name, key_name, binding.c_str());
    }

    const std::string rumble_value = si.GetStringValue(section_name, "Rumble");
    if (!rumble_value.empty())
      profile.SetStringValue(section_name, "Rumble", rumble_value.c_str());
  }

  profile.Save();
  return true;
}
