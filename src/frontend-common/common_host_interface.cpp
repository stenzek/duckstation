#include "common_host_interface.h"
#include "common/assert.h"
#include "common/audio_stream.h"
#include "common/log.h"
#include "common/string_util.h"
#include "core/controller.h"
#include "core/game_list.h"
#include "core/gpu.h"
#include "core/system.h"
#include "sdl_audio_stream.h"
#include "sdl_controller_interface.h"
#include <cstring>
Log_SetChannel(CommonHostInterface);

CommonHostInterface::CommonHostInterface() : HostInterface()
{
  RegisterGeneralHotkeys();
  RegisterGraphicsHotkeys();
  RegisterSaveStateHotkeys();
}

CommonHostInterface::~CommonHostInterface() = default;

void CommonHostInterface::SetFullscreen(bool enabled) {}

void CommonHostInterface::ToggleFullscreen() {}

std::unique_ptr<AudioStream> CommonHostInterface::CreateAudioStream(AudioBackend backend)
{
  switch (backend)
  {
    case AudioBackend::Null:
      return AudioStream::CreateNullAudioStream();

    case AudioBackend::Cubeb:
      return AudioStream::CreateCubebAudioStream();

    case AudioBackend::SDL:
      return SDLAudioStream::Create();

    default:
      return nullptr;
  }
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
  si.SetStringValue("Controller1", "ButtonTriangle", "Keyboard/8");
  si.SetStringValue("Controller1", "ButtonCross", "Keyboard/2");
  si.SetStringValue("Controller1", "ButtonSquare", "Keyboard/4");
  si.SetStringValue("Controller1", "ButtonCircle", "Keyboard/6");
  si.SetStringValue("Controller1", "ButtonL1", "Keyboard/Q");
  si.SetStringValue("Controller1", "ButtonL2", "Keyboard/1");
  si.SetStringValue("Controller1", "ButtonR1", "Keyboard/E");
  si.SetStringValue("Controller1", "ButtonR2", "Keyboard/3");
  si.SetStringValue("Hotkeys", "FastForward", "Keyboard/Tab");
  si.SetStringValue("Hotkeys", "PowerOff", "Keyboard/Escape");
  si.SetStringValue("Hotkeys", "TogglePause", "Keyboard/Pause");
  si.SetStringValue("Hotkeys", "ToggleFullscreen", "Keyboard/Alt+Return");
}

#if 0
void CommonHostInterface::refreshGameList(bool invalidate_cache /* = false */, bool invalidate_database /* = false */)
{
  std::lock_guard<std::mutex> lock(m_qsettings_mutex);
  QtSettingsInterface si(m_qsettings);
  m_game_list->SetSearchDirectoriesFromSettings(si);
  m_game_list->Refresh(invalidate_cache, invalidate_database);
  emit gameListRefreshed();
}
#endif

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
  g_sdl_controller_interface.ClearControllerBindings();

  UpdateControllerInputMap(si);
  UpdateHotkeyInputMap(si);
}

void CommonHostInterface::UpdateControllerInputMap(SettingsInterface& si)
{
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
        AddButtonToInputMap(binding, [this, controller_index, button_code](bool pressed) {
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
        AddAxisToInputMap(binding, [this, controller_index, axis_code](float value) {
          if (!m_system)
            return;

          Controller* controller = m_system->GetController(controller_index);
          if (controller)
            controller->SetAxisState(axis_code, value);
        });
      }
    }
  }
}

void CommonHostInterface::UpdateHotkeyInputMap(SettingsInterface& si)
{
  for (const HotkeyInfo& hi : m_hotkeys)
  {
    const std::vector<std::string> bindings = si.GetStringList("Hotkeys", hi.name);
    for (const std::string& binding : bindings)
      AddButtonToInputMap(binding, hi.handler);
  }
}

void CommonHostInterface::AddButtonToInputMap(const std::string& binding, InputButtonHandler handler)
{
  const std::string::size_type slash_pos = binding.find('/');
  if (slash_pos == std::string::npos)
  {
    Log_WarningPrintf("Malformed button binding: '%s'", binding.c_str());
    return;
  }

  const auto device = std::string_view(binding).substr(0, slash_pos);
  const auto button = std::string_view(binding).substr(slash_pos + 1);
  if (device == "Keyboard")
  {
    std::optional<int> key_id = GetHostKeyCode(button);
    if (!key_id.has_value())
    {
      Log_WarningPrintf("Unknown keyboard key in binding '%s'", binding.c_str());
      return;
    }

    m_keyboard_input_handlers.emplace(key_id.value(), std::move(handler));
  }
  else if (device == "Controller")
  {
    const std::optional<int> controller_index = StringUtil::FromChars<int>(device.substr(10));
    if (!controller_index || *controller_index < 0)
    {
      Log_WarningPrintf("Invalid controller index in button binding '%s'", binding.c_str());
      return;
    }

    if (button.find_first_of("Button") == 0)
    {
      const std::optional<int> button_index = StringUtil::FromChars<int>(button.substr(6));
      if (!button_index ||
          !g_sdl_controller_interface.BindControllerButton(*controller_index, *button_index, std::move(handler)))
      {
        Log_WarningPrintf("Failed to bind controller button '%s' to button", binding.c_str());
        return;
      }
    }
    else if (button.find_first_of("+Axis") == 0 || button.find_first_of("-Axis"))
    {
      const std::optional<int> axis_index = StringUtil::FromChars<int>(button.substr(5));
      const bool positive = (button[0] == '+');
      if (!axis_index || !g_sdl_controller_interface.BindControllerAxisToButton(*controller_index, *axis_index,
                                                                                positive, std::move(handler)))
      {
        Log_WarningPrintf("Failed to bind controller axis '%s' to button", binding.c_str());
        return;
      }
    }
    else
    {
      Log_WarningPrintf("Malformed controller binding '%s' in button", binding.c_str());
      return;
    }
  }
  else
  {
    Log_WarningPrintf("Unknown input device in button binding '%s'", binding.c_str());
    return;
  }
}

void CommonHostInterface::AddAxisToInputMap(const std::string& binding, InputAxisHandler handler)
{
  const std::string::size_type slash_pos = binding.find('/');
  if (slash_pos == std::string::npos)
  {
    Log_WarningPrintf("Malformed axis binding: '%s'", binding.c_str());
    return;
  }

  const auto device = std::string_view(binding).substr(0, slash_pos);
  const auto axis = std::string_view(binding).substr(slash_pos + 1);
  if (device == "Controller")
  {
    const std::optional<int> controller_index = StringUtil::FromChars<int>(device.substr(10));
    if (!controller_index || *controller_index < 0)
    {
      Log_WarningPrintf("Invalid controller index in axis binding '%s'", binding.c_str());
      return;
    }

    if (axis.find_first_of("Axis") == 0)
    {
      const std::optional<int> axis_index = StringUtil::FromChars<int>(axis.substr(4));
      if (!axis_index ||
          !g_sdl_controller_interface.BindControllerAxis(*controller_index, *axis_index, std::move(handler)))
      {
        Log_WarningPrintf("Failed to bind controller axis '%s' to axi", binding.c_str());
        return;
      }
    }
    else
    {
      Log_WarningPrintf("Malformed controller binding '%s' in button", binding.c_str());
      return;
    }
  }
  else
  {
    Log_WarningPrintf("Unknown input device in axis binding '%s'", binding.c_str());
    return;
  }
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
                     ToggleFullscreen();
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
                     if (m_settings.confim_power_off)
                     {
                       SetFullscreen(false);

                       SmallString confirmation_message("Are you sure you want to stop emulation?");
                       if (m_settings.save_state_on_exit)
                         confirmation_message.AppendString("\n\nThe current state will be saved.");

                       if (!ConfirmMessage(confirmation_message))
                       {
                         if (m_settings.display_fullscreen)
                           SetFullscreen(true);

                         m_system->ResetPerformanceCounters();
                         return;
                       }
                     }

                     PowerOffSystem();
                   }
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
