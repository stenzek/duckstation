// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "sdl_input_source.h"
#include "input_manager.h"

#include "core/host.h"
#include "core/settings.h"

#include "common/assert.h"
#include "common/bitutils.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"

#include "IconsPromptFont.h"
#include "fmt/format.h"

#include <cmath>

#ifdef __APPLE__
#include <dispatch/dispatch.h>
#endif

LOG_CHANNEL(SDL);

static constexpr const char* CONTROLLER_DB_FILENAME = "gamecontrollerdb.txt";

static constexpr std::array<const char*, SDL_GAMEPAD_AXIS_COUNT> s_sdl_axis_names = {{
  "LeftX",        // SDL_GAMEPAD_AXIS_LEFTX
  "LeftY",        // SDL_GAMEPAD_AXIS_LEFTY
  "RightX",       // SDL_GAMEPAD_AXIS_RIGHTX
  "RightY",       // SDL_GAMEPAD_AXIS_RIGHTY
  "LeftTrigger",  // SDL_GAMEPAD_AXIS_LEFT_TRIGGER
  "RightTrigger", // SDL_GAMEPAD_AXIS_RIGHT_TRIGGER
}};
static constexpr std::array<std::array<const char*, 2>, SDL_GAMEPAD_AXIS_COUNT> s_sdl_axis_icons = {{
  {{ICON_PF_LEFT_ANALOG_LEFT, ICON_PF_LEFT_ANALOG_RIGHT}},   // SDL_GAMEPAD_AXIS_LEFTX
  {{ICON_PF_LEFT_ANALOG_UP, ICON_PF_LEFT_ANALOG_DOWN}},      // SDL_GAMEPAD_AXIS_LEFTY
  {{ICON_PF_RIGHT_ANALOG_LEFT, ICON_PF_RIGHT_ANALOG_RIGHT}}, // SDL_GAMEPAD_AXIS_RIGHTX
  {{ICON_PF_RIGHT_ANALOG_UP, ICON_PF_RIGHT_ANALOG_DOWN}},    // SDL_GAMEPAD_AXIS_RIGHTY
  {{nullptr, ICON_PF_LEFT_TRIGGER_LT}},                      // SDL_GAMEPAD_AXIS_LEFT_TRIGGER
  {{nullptr, ICON_PF_RIGHT_TRIGGER_RT}},                     // SDL_GAMEPAD_AXIS_RIGHT_TRIGGER
}};
static constexpr std::array<std::array<GenericInputBinding, 2>, SDL_GAMEPAD_AXIS_COUNT>
  s_sdl_generic_binding_axis_mapping = {{
    {{GenericInputBinding::LeftStickLeft, GenericInputBinding::LeftStickRight}},   // SDL_GAMEPAD_AXIS_LEFTX
    {{GenericInputBinding::LeftStickUp, GenericInputBinding::LeftStickDown}},      // SDL_GAMEPAD_AXIS_LEFTY
    {{GenericInputBinding::RightStickLeft, GenericInputBinding::RightStickRight}}, // SDL_GAMEPAD_AXIS_RIGHTX
    {{GenericInputBinding::RightStickUp, GenericInputBinding::RightStickDown}},    // SDL_GAMEPAD_AXIS_RIGHTY
    {{GenericInputBinding::Unknown, GenericInputBinding::L2}},                     // SDL_GAMEPAD_AXIS_LEFT_TRIGGER
    {{GenericInputBinding::Unknown, GenericInputBinding::R2}},                     // SDL_GAMEPAD_AXIS_RIGHT_TRIGGER
  }};

static constexpr std::array<const char*, SDL_GAMEPAD_BUTTON_COUNT> s_sdl_button_names = {{
  "A",             // SDL_GAMEPAD_BUTTON_SOUTH
  "B",             // SDL_GAMEPAD_BUTTON_EAST
  "X",             // SDL_GAMEPAD_BUTTON_WEST
  "Y",             // SDL_GAMEPAD_BUTTON_NORTH
  "Back",          // SDL_GAMEPAD_BUTTON_BACK
  "Guide",         // SDL_GAMEPAD_BUTTON_GUIDE
  "Start",         // SDL_GAMEPAD_BUTTON_START
  "LeftStick",     // SDL_GAMEPAD_BUTTON_LEFT_STICK
  "RightStick",    // SDL_GAMEPAD_BUTTON_RIGHT_STICK
  "LeftShoulder",  // SDL_GAMEPAD_BUTTON_LEFT_SHOULDER
  "RightShoulder", // SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER
  "DPadUp",        // SDL_GAMEPAD_BUTTON_DPAD_UP
  "DPadDown",      // SDL_GAMEPAD_BUTTON_DPAD_DOWN
  "DPadLeft",      // SDL_GAMEPAD_BUTTON_DPAD_LEFT
  "DPadRight",     // SDL_GAMEPAD_BUTTON_DPAD_RIGHT
  "Misc1",         // SDL_GAMEPAD_BUTTON_MISC1
  "RightPaddle1",  // SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1
  "LeftPaddle1",   // SDL_GAMEPAD_BUTTON_LEFT_PADDLE1
  "RightPaddle2",  // SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2
  "LeftPaddle2",   // SDL_GAMEPAD_BUTTON_LEFT_PADDLE2
  "Touchpad",      // SDL_GAMEPAD_BUTTON_TOUCHPAD
  "Misc2",         // SDL_GAMEPAD_BUTTON_MISC2
  "Misc3",         // SDL_GAMEPAD_BUTTON_MISC3
  "Misc4",         // SDL_GAMEPAD_BUTTON_MISC4
  "Misc5",         // SDL_GAMEPAD_BUTTON_MISC5
  "Misc6",         // SDL_GAMEPAD_BUTTON_MISC6
}};
static constexpr std::array<const char*, SDL_GAMEPAD_BUTTON_COUNT> s_sdl_button_icons = {{
  ICON_PF_BUTTON_A,           // SDL_GAMEPAD_BUTTON_SOUTH
  ICON_PF_BUTTON_B,           // SDL_GAMEPAD_BUTTON_EAST
  ICON_PF_BUTTON_X,           // SDL_GAMEPAD_BUTTON_WEST
  ICON_PF_BUTTON_Y,           // SDL_GAMEPAD_BUTTON_NORTH
  ICON_PF_SHARE_CAPTURE,      // SDL_GAMEPAD_BUTTON_BACK
  ICON_PF_XBOX,               // SDL_GAMEPAD_BUTTON_GUIDE
  ICON_PF_BURGER_MENU,        // SDL_GAMEPAD_BUTTON_START
  ICON_PF_LEFT_ANALOG_CLICK,  // SDL_GAMEPAD_BUTTON_LEFT_STICK
  ICON_PF_RIGHT_ANALOG_CLICK, // SDL_GAMEPAD_BUTTON_RIGHT_STICK
  ICON_PF_LEFT_SHOULDER_LB,   // SDL_GAMEPAD_BUTTON_LEFT_SHOULDER
  ICON_PF_RIGHT_SHOULDER_RB,  // SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER
  ICON_PF_XBOX_DPAD_UP,       // SDL_GAMEPAD_BUTTON_DPAD_UP
  ICON_PF_XBOX_DPAD_DOWN,     // SDL_GAMEPAD_BUTTON_DPAD_DOWN
  ICON_PF_XBOX_DPAD_LEFT,     // SDL_GAMEPAD_BUTTON_DPAD_LEFT
  ICON_PF_XBOX_DPAD_RIGHT,    // SDL_GAMEPAD_BUTTON_DPAD_RIGHT
  nullptr,                    // SDL_GAMEPAD_BUTTON_MISC1
  nullptr,                    // SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1
  nullptr,                    // SDL_GAMEPAD_BUTTON_LEFT_PADDLE1
  nullptr,                    // SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2
  nullptr,                    // SDL_GAMEPAD_BUTTON_LEFT_PADDLE2
  ICON_PF_DUALSHOCK_TOUCHPAD, // SDL_GAMEPAD_BUTTON_TOUCHPAD
  nullptr,                    // SDL_GAMEPAD_BUTTON_MISC2
  nullptr,                    // SDL_GAMEPAD_BUTTON_MISC3
  nullptr,                    // SDL_GAMEPAD_BUTTON_MISC4
  nullptr,                    // SDL_GAMEPAD_BUTTON_MISC5
  nullptr,                    // SDL_GAMEPAD_BUTTON_MISC6
}};
static constexpr std::array<GenericInputBinding, SDL_GAMEPAD_BUTTON_COUNT> s_sdl_generic_binding_button_mapping = {{
  GenericInputBinding::Cross,     // SDL_GAMEPAD_BUTTON_SOUTH
  GenericInputBinding::Circle,    // SDL_GAMEPAD_BUTTON_EAST
  GenericInputBinding::Square,    // SDL_GAMEPAD_BUTTON_WEST
  GenericInputBinding::Triangle,  // SDL_GAMEPAD_BUTTON_NORTH
  GenericInputBinding::Select,    // SDL_GAMEPAD_BUTTON_BACK
  GenericInputBinding::System,    // SDL_GAMEPAD_BUTTON_GUIDE
  GenericInputBinding::Start,     // SDL_GAMEPAD_BUTTON_START
  GenericInputBinding::L3,        // SDL_GAMEPAD_BUTTON_LEFT_STICK
  GenericInputBinding::R3,        // SDL_GAMEPAD_BUTTON_RIGHT_STICK
  GenericInputBinding::L1,        // SDL_GAMEPAD_BUTTON_LEFT_SHOULDER
  GenericInputBinding::R1,        // SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER
  GenericInputBinding::DPadUp,    // SDL_GAMEPAD_BUTTON_DPAD_UP
  GenericInputBinding::DPadDown,  // SDL_GAMEPAD_BUTTON_DPAD_DOWN
  GenericInputBinding::DPadLeft,  // SDL_GAMEPAD_BUTTON_DPAD_LEFT
  GenericInputBinding::DPadRight, // SDL_GAMEPAD_BUTTON_DPAD_RIGHT
  GenericInputBinding::Unknown,   // SDL_GAMEPAD_BUTTON_MISC1
  GenericInputBinding::Unknown,   // SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1
  GenericInputBinding::Unknown,   // SDL_GAMEPAD_BUTTON_LEFT_PADDLE1
  GenericInputBinding::Unknown,   // SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2
  GenericInputBinding::Unknown,   // SDL_GAMEPAD_BUTTON_LEFT_PADDLE2
  GenericInputBinding::Unknown,   // SDL_GAMEPAD_BUTTON_TOUCHPAD
  GenericInputBinding::Unknown,   // SDL_GAMEPAD_BUTTON_MISC2
  GenericInputBinding::Unknown,   // SDL_GAMEPAD_BUTTON_MISC3
  GenericInputBinding::Unknown,   // SDL_GAMEPAD_BUTTON_MISC4
  GenericInputBinding::Unknown,   // SDL_GAMEPAD_BUTTON_MISC5
  GenericInputBinding::Unknown,   // SDL_GAMEPAD_BUTTON_MISC6
}};

static constexpr std::array<const char*, 4> s_sdl_hat_direction_names = {{
  // clang-format off
  "North",
  "East",
  "South",
  "West",
  // clang-format on
}};

static constexpr std::array<const char*, 4> s_sdl_default_led_colors = {{
  "0000ff", // SDL-0
  "ff0000", // SDL-1
  "00ff00", // SDL-2
  "ffff00", // SDL-3
}};

#ifdef _WIN32
static constexpr bool SDL_DEFAULT_XBOX_HIDAPI = false;
#else
static constexpr bool SDL_DEFAULT_XBOX_HIDAPI = true;
#endif

static constexpr const SettingInfo s_sdl_advanced_settings_info[] = {
  {SettingInfo::Type::Boolean, "SDLJoystickXboxHIDAPI", TRANSLATE_NOOP("SDLInputSource", "Enable XBox HIDAPI Driver"),
   TRANSLATE_NOOP("SDLInputSource", "Enables the HIDAPI driver for XBox controllers."),
   SDL_DEFAULT_XBOX_HIDAPI ? "true" : "false", nullptr, nullptr, nullptr, nullptr, nullptr, 0.0f},
#if defined(_WIN32)
  {SettingInfo::Type::Boolean, "SDLJoystickRawInput", TRANSLATE_NOOP("SDLInputSource", "Enable Raw Input Drivers"),
   TRANSLATE_NOOP("SDLInputSource",
                  "Enables raw input joystick drivers which can improve handling of XInput-capable devices."),
   "false", nullptr, nullptr, nullptr, nullptr, nullptr, 0.0f},
  {SettingInfo::Type::Boolean, "SDLJoystickDirectInput", TRANSLATE_NOOP("SDLInputSource", "Enable DirectInput Driver"),
   TRANSLATE_NOOP("SDLInputSource", "Enables compatibility with DirectInput controllers."), "true", nullptr, nullptr,
   nullptr, nullptr, nullptr, 0.0f},
  {SettingInfo::Type::Boolean, "SDLJoystickXInput", TRANSLATE_NOOP("SDLInputSource", "Enable XInput Driver"),
   TRANSLATE_NOOP("SDLInputSource", "Enables compatibility with XInput controllers."), "true", nullptr, nullptr,
   nullptr, nullptr, nullptr, 0.0f},
  {SettingInfo::Type::Boolean, "SDLJoystickWGI", TRANSLATE_NOOP("SDLInputSource", "Enable WGI Driver"),
   TRANSLATE_NOOP("SDLInputSource", "Enables compatibility with Windows.Gaming.Input controllers."), "true", nullptr,
   nullptr, nullptr, nullptr, nullptr, 0.0f},
  {SettingInfo::Type::Boolean, "SDLJoystickGameInput", TRANSLATE_NOOP("SDLInputSource", "Enable GameInput Driver"),
   TRANSLATE_NOOP("SDLInputSource", "Enables compatibility with GameInput controllers."), "false", nullptr, nullptr,
   nullptr, nullptr, nullptr, 0.0f},
#elif defined(__APPLE__)
  {SettingInfo::Type::Boolean, "SDLIOKitDriver", TRANSLATE_NOOP("SDLInputSource", "Enable IOKit Driver"),
   TRANSLATE_NOOP("SDLInputSource", "Allows the use of IOKit for controller handling."), "true", nullptr, nullptr,
   nullptr, nullptr, nullptr, 0.0f},
  {SettingInfo::Type::Boolean, "SDLMFIDriver", TRANSLATE_NOOP("SDLInputSource", "Enable MFI Driver"),
   TRANSLATE_NOOP("SDLInputSource", "Allows the use of GCController/MFI for controller handling."), "true", nullptr,
   nullptr, nullptr, nullptr, nullptr, 0.0f},
#else
  {SettingInfo::Type::Boolean, "SDLJoystickLinuxDigitalHats",
   TRANSLATE_NOOP("SDLInputSource", "Force Digital Hat Inputs"),
   TRANSLATE_NOOP("SDLInputSource", "Forces joysticks to always treat 'hat' axis inputs (ABS_HAT0X - ABS_HAT3Y) as "
                                    "8-way digital hats without checking whether they may be analog."),
   "false", nullptr, nullptr, nullptr, nullptr, nullptr, 0.0f},
#endif
};

static void SetControllerRGBLED(SDL_Gamepad* gp, u32 color)
{
  SDL_SetGamepadLED(gp, (color >> 16) & 0xff, (color >> 8) & 0xff, color & 0xff);
}

static void SDLLogCallback(void* userdata, int category, SDL_LogPriority priority, const char* message)
{
  static constexpr Log::Level priority_map[SDL_LOG_PRIORITY_COUNT] = {
    Log::Level::Debug,   // SDL_LOG_PRIORITY_INVALID
    Log::Level::Trace,   // SDL_LOG_PRIORITY_TRACE
    Log::Level::Verbose, // SDL_LOG_PRIORITY_VERBOSE
    Log::Level::Debug,   // SDL_LOG_PRIORITY_DEBUG
    Log::Level::Info,    // SDL_LOG_PRIORITY_INFO
    Log::Level::Warning, // SDL_LOG_PRIORITY_WARN
    Log::Level::Error,   // SDL_LOG_PRIORITY_ERROR
    Log::Level::Error,   // SDL_LOG_PRIORITY_CRITICAL
  };

  Log::FastWrite(Log::Channel::SDL, priority_map[priority], message);
}

bool SDLInputSource::ALLOW_EVENT_POLLING = true;

SDLInputSource::SDLInputSource() = default;

SDLInputSource::~SDLInputSource()
{
  Assert(m_controllers.empty());
}

bool SDLInputSource::Initialize(const SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
  LoadSettings(si);
  settings_lock.unlock();
  SetHints();
  bool result = InitializeSubsystem();
  settings_lock.lock();
  return result;
}

void SDLInputSource::UpdateSettings(const SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
  const bool old_controller_touchpad_as_pointer = m_controller_touchpad_as_pointer;
  const u8 old_advanced_options_bits = m_advanced_options_bits;

  LoadSettings(si);

  if (m_advanced_options_bits != old_advanced_options_bits ||
      m_controller_touchpad_as_pointer != old_controller_touchpad_as_pointer)
  {
    settings_lock.unlock();
    ShutdownSubsystem();
    SetHints();
    InitializeSubsystem();
    settings_lock.lock();
  }
}

bool SDLInputSource::ReloadDevices()
{
  // We'll get a GC added/removed event here.
  PollEvents();
  return false;
}

void SDLInputSource::Shutdown()
{
  ShutdownSubsystem();
}

void SDLInputSource::LoadSettings(const SettingsInterface& si)
{
  for (u32 i = 0; i < MAX_LED_COLORS; i++)
  {
    const u32 color = GetRGBForPlayerId(si, i);
    if (m_led_colors[i] == color)
      continue;

    m_led_colors[i] = color;

    const auto it = GetControllerDataForPlayerId(i);
    if (it == m_controllers.end() || !it->gamepad || !it->has_led)
      continue;

    SetControllerRGBLED(it->gamepad, color);
  }

  m_controller_enhanced_mode = si.GetBoolValue("InputSources", "SDLControllerEnhancedMode", false);
  m_controller_ps5_player_led = si.GetBoolValue("InputSources", "SDLPS5PlayerLED", false);
  m_controller_touchpad_as_pointer = si.GetBoolValue("InputSources", "SDLTouchpadAsPointer", false);
  m_sdl_hints = si.GetKeyValueList("SDLHints");

  m_joystick_xbox_hidapi = si.GetBoolValue("InputSources", "SDLJoystickXboxHIDAPI", SDL_DEFAULT_XBOX_HIDAPI);
#if defined(_WIN32)
  m_joystick_rawinput = si.GetBoolValue("InputSources", "SDLJoystickRawInput", false);
  m_joystick_directinput = si.GetBoolValue("InputSources", "SDLJoystickDirectInput", true);
  m_joystick_xinput = si.GetBoolValue("InputSources", "SDLJoystickXInput", true);
  m_joystick_wgi = si.GetBoolValue("InputSources", "SDLJoystickWGI", true);
  m_joystick_gameinput = si.GetBoolValue("InputSources", "SDLJoystickGameInput", false);
#elif defined(__APPLE__)
  m_enable_iokit_driver = si.GetBoolValue("InputSources", "SDLIOKitDriver", true);
  m_enable_mfi_driver = si.GetBoolValue("InputSources", "SDLMFIDriver", true);
#else
  m_joystick_force_hat_input = si.GetBoolValue("InputSources", "SDLJoystickLinuxDigitalHats", false);
#endif
}

void InputSource::CopySDLSourceSettings(SettingsInterface* dest_si, const SettingsInterface& src_si)
{
  for (u32 i = 0; i < SDLInputSource::MAX_LED_COLORS; i++)
    dest_si->CopyStringValue(src_si, "SDLExtra", TinyString::from_format("Player{}LED", i).c_str());

  dest_si->CopyBoolValue(src_si, "InputSources", "SDLControllerEnhancedMode");
  dest_si->CopyBoolValue(src_si, "InputSources", "SDLPS5PlayerLED");
  dest_si->CopyBoolValue(src_si, "InputSources", "SDLTouchpadAsPointer");
  dest_si->CopySection(src_si, "SDLHints");

  for (const SettingInfo& si : s_sdl_advanced_settings_info)
    si.CopyValue(dest_si, src_si, "InputSources");
}

u32 SDLInputSource::GetRGBForPlayerId(const SettingsInterface& si, u32 player_id)
{
  return ParseRGBForPlayerId(si.GetStringValue("SDLExtra", TinyString::from_format("Player{}LED", player_id).c_str(),
                                               s_sdl_default_led_colors[player_id]),
                             player_id);
}

u32 SDLInputSource::ParseRGBForPlayerId(std::string_view str, u32 player_id)
{
  if (player_id >= MAX_LED_COLORS)
    return 0;

  const u32 default_color = StringUtil::FromChars<u32>(s_sdl_default_led_colors[player_id], 16).value_or(0);
  const u32 color = StringUtil::FromChars<u32>(str, 16).value_or(default_color);

  return color;
}

std::span<const SettingInfo> SDLInputSource::GetAdvancedSettingsInfo()
{
  return s_sdl_advanced_settings_info;
}

void SDLInputSource::SetHints()
{
  if (const std::string upath = Path::Combine(EmuFolders::DataRoot, CONTROLLER_DB_FILENAME);
      FileSystem::FileExists(upath.c_str()))
  {
    INFO_LOG("Using Controller DB from user directory: '{}'", upath);
    SDL_SetHint(SDL_HINT_GAMECONTROLLERCONFIG_FILE, upath.c_str());
  }
  else if (const std::string rpath = EmuFolders::GetOverridableResourcePath(CONTROLLER_DB_FILENAME);
           FileSystem::FileExists(rpath.c_str()))
  {
    INFO_LOG("Using Controller DB from resources.");
    SDL_SetHint(SDL_HINT_GAMECONTROLLERCONFIG_FILE, rpath.c_str());
  }
  else
  {
    ERROR_LOG("Controller DB not found, it should be named '{}'", CONTROLLER_DB_FILENAME);
  }

  SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, m_controller_enhanced_mode ? "1" : "0");
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_PLAYER_LED, m_controller_ps5_player_led ? "1" : "0");
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_WII, "1");
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS3, "1");

  INFO_LOG("XBox HIDAPI is {}.", m_joystick_xbox_hidapi ? "enabled" : "disabled");
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX, m_joystick_xbox_hidapi ? "1" : "0");

#if defined(_WIN32)
  INFO_LOG("RawInput is {}, DirectInput is {}, XInput is {}, WGI is {}, GameInput is {}.",
           m_joystick_rawinput ? "enabled" : "disabled", m_joystick_directinput ? "enabled" : "disabled",
           m_joystick_xinput ? "enabled" : "disabled", m_joystick_wgi ? "enabled" : "disabled",
           m_joystick_gameinput ? "enabled" : "disabled");
  SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, m_joystick_rawinput ? "1" : "0");
  SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT, m_joystick_rawinput ? "1" : "0");
  SDL_SetHint(SDL_HINT_XINPUT_ENABLED, m_joystick_xinput ? "1" : "0");
  SDL_SetHint(SDL_HINT_JOYSTICK_WGI, m_joystick_wgi ? "1" : "0");
  SDL_SetHint(SDL_HINT_JOYSTICK_GAMEINPUT, m_joystick_gameinput ? "1" : "0");
#elif defined(__APPLE__)
  INFO_LOG("IOKit is {}, MFI is {}.", m_enable_iokit_driver ? "enabled" : "disabled",
           m_enable_mfi_driver ? "enabled" : "disabled");
  SDL_SetHint(SDL_HINT_JOYSTICK_IOKIT, m_enable_iokit_driver ? "1" : "0");
  SDL_SetHint(SDL_HINT_JOYSTICK_MFI, m_enable_mfi_driver ? "1" : "0");
#else
  SDL_SetHint(SDL_HINT_JOYSTICK_LINUX_DIGITAL_HATS, m_joystick_force_hat_input ? "1" : "0");
#endif

  for (const std::pair<std::string, std::string>& hint : m_sdl_hints)
    SDL_SetHint(hint.first.c_str(), hint.second.c_str());
}

bool SDLInputSource::InitializeSubsystem()
{
  if (!SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD | SDL_INIT_HAPTIC))
  {
    ERROR_LOG("SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD | SDL_INIT_HAPTIC) failed");
    return false;
  }

  SDL_SetLogOutputFunction(SDLLogCallback, nullptr);
#if defined(_DEBUG) || defined(_DEVEL)
  SDL_SetLogPriorities(SDL_LOG_PRIORITY_DEBUG);
#else
  SDL_SetLogPriorities(SDL_LOG_PRIORITY_INFO);
#endif

  // we should open the controllers as the connected events come in, so no need to do any more here
  m_sdl_subsystem_initialized = true;

  int mapping_count = 0;
  SDL_free(SDL_GetGamepadMappings(&mapping_count));
  INFO_LOG("{} controller mappings are loaded.", mapping_count);

  return true;
}

void SDLInputSource::ShutdownSubsystem()
{
  while (!m_controllers.empty())
    CloseDevice(m_controllers.begin()->joystick_id);

  if (m_sdl_subsystem_initialized)
  {
    SDL_QuitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD | SDL_INIT_HAPTIC);
    m_sdl_subsystem_initialized = false;
  }
}

void SDLInputSource::PollEvents()
{
  if (!ALLOW_EVENT_POLLING)
    return;

  for (;;)
  {
    SDL_Event ev;
    if (SDL_PollEvent(&ev))
      ProcessSDLEvent(&ev);
    else
      break;
  }
}

InputManager::DeviceList SDLInputSource::EnumerateDevices()
{
  InputManager::DeviceList ret;

  for (const ControllerData& cd : m_controllers)
  {
    std::string id = fmt::format("SDL-{}", cd.player_id);

    const InputBindingKey key = MakeGenericControllerDeviceKey(InputSourceType::SDL, cd.player_id);
    const char* name = cd.gamepad ? SDL_GetGamepadName(cd.gamepad) : SDL_GetJoystickName(cd.joystick);
    if (name)
      ret.emplace_back(key, std::move(id), name);
    else
      ret.emplace_back(key, std::move(id), "Unknown Device");
  }

  return ret;
}

bool SDLInputSource::ContainsDevice(std::string_view device) const
{
  return device.starts_with("SDL-");
}

std::optional<InputBindingKey> SDLInputSource::ParseKeyString(std::string_view device, std::string_view binding)
{
  if (!device.starts_with("SDL-") || binding.empty())
    return std::nullopt;

  const std::optional<s32> player_id = StringUtil::FromChars<s32>(device.substr(4));
  if (!player_id.has_value() || player_id.value() < 0)
    return std::nullopt;

  InputBindingKey key = {};
  key.source_type = InputSourceType::SDL;
  key.source_index = static_cast<u32>(player_id.value());

  if (binding.ends_with("Motor"))
  {
    key.source_subtype = InputSubclass::ControllerMotor;
    if (binding == "LargeMotor")
    {
      key.data = 0;
      return key;
    }
    else if (binding == "SmallMotor")
    {
      key.data = 1;
      return key;
    }
    else
    {
      return std::nullopt;
    }
  }
  else if (binding.ends_with("Haptic"))
  {
    key.source_subtype = InputSubclass::ControllerHaptic;
    key.data = 0;
    return key;
  }
  else if (binding[0] == '+' || binding[0] == '-')
  {
    // likely an axis
    std::string_view axis_name(binding.substr(1));

    if (axis_name.starts_with("Axis"))
    {
      std::string_view end;
      if (auto value = StringUtil::FromChars<u32>(axis_name.substr(4), 10, &end))
      {
        key.source_subtype = InputSubclass::ControllerAxis;
        key.data = *value + static_cast<u32>(std::size(s_sdl_axis_names));
        key.modifier = (binding[0] == '-') ? InputModifier::Negate : InputModifier::None;
        key.invert = (end == "~");
        return key;
      }
    }

    if (!axis_name.empty() && axis_name.back() == '~')
    {
      axis_name = axis_name.substr(0, axis_name.size() - 1);
      key.invert = true;
    }

    for (u32 i = 0; i < std::size(s_sdl_axis_names); i++)
    {
      if (axis_name == s_sdl_axis_names[i])
      {
        // found an axis!
        key.source_subtype = InputSubclass::ControllerAxis;
        key.data = i;
        key.modifier = (binding[0] == '-') ? InputModifier::Negate : InputModifier::None;
        return key;
      }
    }
  }
  else if (binding.starts_with("FullAxis"))
  {
    std::string_view end;
    if (auto value = StringUtil::FromChars<u32>(binding.substr(8), 10, &end))
    {
      key.source_subtype = InputSubclass::ControllerAxis;
      key.data = *value + static_cast<u32>(std::size(s_sdl_axis_names));
      key.modifier = InputModifier::FullAxis;
      key.invert = (end == "~");
      return key;
    }
  }
  else if (binding.starts_with("Hat"))
  {
    std::string_view hat_dir;
    if (auto value = StringUtil::FromChars<u32>(binding.substr(3), 10, &hat_dir); value.has_value() && !hat_dir.empty())
    {
      for (u8 dir = 0; dir < static_cast<u8>(std::size(s_sdl_hat_direction_names)); dir++)
      {
        if (hat_dir == s_sdl_hat_direction_names[dir])
        {
          key.source_subtype = InputSubclass::ControllerHat;
          key.data = value.value() * static_cast<u32>(std::size(s_sdl_hat_direction_names)) + dir;
          return key;
        }
      }
    }
  }
  else
  {
    // must be a button
    if (binding.starts_with("Button"))
    {
      if (auto value = StringUtil::FromChars<u32>(binding.substr(6)))
      {
        key.source_subtype = InputSubclass::ControllerButton;
        key.data = *value + static_cast<u32>(std::size(s_sdl_button_names));
        return key;
      }
    }
    for (u32 i = 0; i < std::size(s_sdl_button_names); i++)
    {
      if (binding == s_sdl_button_names[i])
      {
        key.source_subtype = InputSubclass::ControllerButton;
        key.data = i;
        return key;
      }
    }
  }

  // unknown axis/button
  return std::nullopt;
}

TinyString SDLInputSource::ConvertKeyToString(InputBindingKey key)
{
  TinyString ret;

  if (key.source_type == InputSourceType::SDL)
  {
    if (key.source_subtype == InputSubclass::ControllerAxis)
    {
      const char* modifier =
        (key.modifier == InputModifier::FullAxis ? "Full" : (key.modifier == InputModifier::Negate ? "-" : "+"));
      if (key.data < std::size(s_sdl_axis_names))
      {
        ret.format("SDL-{}/{}{}{}", static_cast<u32>(key.source_index), modifier, s_sdl_axis_names[key.data],
                   key.invert ? "~" : "");
      }
      else
      {
        ret.format("SDL-{}/{}Axis{}{}", static_cast<u32>(key.source_index), modifier,
                   key.data - static_cast<u32>(std::size(s_sdl_axis_names)), key.invert ? "~" : "");
      }
    }
    else if (key.source_subtype == InputSubclass::ControllerButton)
    {
      if (key.data < std::size(s_sdl_button_names))
      {
        ret.format("SDL-{}/{}", static_cast<u32>(key.source_index), s_sdl_button_names[key.data]);
      }
      else
      {
        ret.format("SDL-{}/Button{}", static_cast<u32>(key.source_index),
                   key.data - static_cast<u32>(std::size(s_sdl_button_names)));
      }
    }
    else if (key.source_subtype == InputSubclass::ControllerHat)
    {
      const u32 hat_index = key.data / static_cast<u32>(std::size(s_sdl_hat_direction_names));
      const u32 hat_direction = key.data % static_cast<u32>(std::size(s_sdl_hat_direction_names));
      ret.format("SDL-{}/Hat{}{}", static_cast<u32>(key.source_index), hat_index,
                 s_sdl_hat_direction_names[hat_direction]);
    }
    else if (key.source_subtype == InputSubclass::ControllerMotor)
    {
      ret.format("SDL-{}/{}Motor", static_cast<u32>(key.source_index), key.data ? "Large" : "Small");
    }
    else if (key.source_subtype == InputSubclass::ControllerHaptic)
    {
      ret.format("SDL-{}/Haptic", static_cast<u32>(key.source_index));
    }
  }

  return ret;
}

TinyString SDLInputSource::ConvertKeyToIcon(InputBindingKey key, InputManager::BindingIconMappingFunction mapper)
{
  TinyString ret;

  if (key.source_type == InputSourceType::SDL)
  {
    if (key.source_subtype == InputSubclass::ControllerAxis)
    {
      if (key.data < std::size(s_sdl_axis_icons) && key.modifier != InputModifier::FullAxis)
      {
        ret.format("SDL-{}  {}", static_cast<u32>(key.source_index),
                   mapper(s_sdl_axis_icons[key.data][key.modifier == InputModifier::None]));
      }
    }
    else if (key.source_subtype == InputSubclass::ControllerButton)
    {
      if (key.data < std::size(s_sdl_button_icons) && s_sdl_button_icons[key.data])
        ret.format("SDL-{}  {}", static_cast<u32>(key.source_index), mapper(s_sdl_button_icons[key.data]));
    }
  }

  return ret;
}

bool SDLInputSource::IsHandledInputEvent(const SDL_Event* ev)
{
  switch (ev->type)
  {
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
    case SDL_EVENT_JOYSTICK_ADDED:
    case SDL_EVENT_JOYSTICK_REMOVED:
    case SDL_EVENT_JOYSTICK_AXIS_MOTION:
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
    case SDL_EVENT_JOYSTICK_BUTTON_UP:
    case SDL_EVENT_JOYSTICK_HAT_MOTION:
      return true;

    default:
      return false;
  }
}

bool SDLInputSource::ProcessSDLEvent(const SDL_Event* event)
{
  switch (event->type)
  {
    case SDL_EVENT_GAMEPAD_ADDED:
    {
      INFO_LOG("Controller {} inserted", event->gdevice.which);
      OpenDevice(event->gdevice.which, true);
      return true;
    }

    case SDL_EVENT_GAMEPAD_REMOVED:
    {
      INFO_LOG("Controller {} removed", event->gdevice.which);
      CloseDevice(event->gdevice.which);
      return true;
    }

    case SDL_EVENT_JOYSTICK_ADDED:
    {
      // Let gamepad handle.. well.. gamepads.
      if (SDL_IsGamepad(event->jdevice.which))
        return false;

      INFO_LOG("Joystick {} inserted", event->jdevice.which);
      OpenDevice(event->jdevice.which, false);
      return true;
    }
    break;

    case SDL_EVENT_JOYSTICK_REMOVED:
    {
      if (auto it = GetControllerDataForJoystickId(event->jdevice.which); it != m_controllers.end() && it->gamepad)
        return false;

      INFO_LOG("Joystick {} removed", event->jdevice.which);
      CloseDevice(event->jdevice.which);
      return true;
    }

    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
      return HandleGamepadAxisMotionEvent(&event->gaxis);

    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
      return HandleGamepadButtonEvent(&event->gbutton);

    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
      return HandleGamepadTouchpadEvent(&event->gtouchpad);

    case SDL_EVENT_JOYSTICK_AXIS_MOTION:
      return HandleJoystickAxisEvent(&event->jaxis);

    case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
    case SDL_EVENT_JOYSTICK_BUTTON_UP:
      return HandleJoystickButtonEvent(&event->jbutton);

    case SDL_EVENT_JOYSTICK_HAT_MOTION:
      return HandleJoystickHatEvent(&event->jhat);

    default:
      return false;
  }
}

SDL_Joystick* SDLInputSource::GetJoystickForDevice(std::string_view device)
{
  if (!device.starts_with("SDL-"))
    return nullptr;

  const std::optional<s32> player_id = StringUtil::FromChars<s32>(device.substr(4));
  if (!player_id.has_value() || player_id.value() < 0)
    return nullptr;

  auto it = GetControllerDataForPlayerId(player_id.value());
  if (it == m_controllers.end())
    return nullptr;

  return it->joystick;
}

SDLInputSource::ControllerDataVector::iterator SDLInputSource::GetControllerDataForJoystickId(int id)
{
  return std::find_if(m_controllers.begin(), m_controllers.end(),
                      [id](const ControllerData& cd) { return cd.joystick_id == id; });
}

SDLInputSource::ControllerDataVector::iterator SDLInputSource::GetControllerDataForPlayerId(int id)
{
  return std::find_if(m_controllers.begin(), m_controllers.end(),
                      [id](const ControllerData& cd) { return cd.player_id == id; });
}

int SDLInputSource::GetFreePlayerId() const
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

bool SDLInputSource::OpenDevice(int index, bool is_gamecontroller)
{
  SDL_Gamepad* gamepad;
  SDL_Joystick* joystick;

  if (is_gamecontroller)
  {
    gamepad = SDL_OpenGamepad(index);
    joystick = gamepad ? SDL_GetGamepadJoystick(gamepad) : nullptr;
  }
  else
  {
    gamepad = nullptr;
    joystick = SDL_OpenJoystick(index);
  }

  if (!gamepad && !joystick)
  {
    ERROR_LOG("Failed to open controller {}", index);
    if (gamepad)
      SDL_CloseGamepad(gamepad);

    return false;
  }

  const int joystick_id = SDL_GetJoystickID(joystick);
  int player_id = gamepad ? SDL_GetGamepadPlayerIndex(gamepad) : SDL_GetJoystickPlayerIndex(joystick);
  if (player_id < 0 || GetControllerDataForPlayerId(player_id) != m_controllers.end())
  {
    const int free_player_id = GetFreePlayerId();
    WARNING_LOG("Controller {} (joystick {}) returned player ID {}, which is invalid or in use. Using ID {} instead.",
                index, joystick_id, player_id, free_player_id);
    player_id = free_player_id;
  }

  const char* name = gamepad ? SDL_GetGamepadName(gamepad) : SDL_GetJoystickName(joystick);
  if (!name)
    name = "Unknown Device";

  const SDL_PropertiesID properties = gamepad ? SDL_GetGamepadProperties(gamepad) : SDL_GetJoystickProperties(joystick);

  VERBOSE_LOG("Opened {} {} (instance id {}, player id {}): {}", is_gamecontroller ? "game controller" : "joystick",
              index, joystick_id, player_id, name);

  ControllerData cd = {};
  cd.player_id = player_id;
  cd.joystick_id = joystick_id;
  cd.haptic_left_right_effect = -1;
  cd.gamepad = gamepad;
  cd.joystick = joystick;
  cd.last_touch_x = 0.0f;
  cd.last_touch_y = 0.0f;

  const u32 num_axes = static_cast<u32>(std::max(SDL_GetNumJoystickAxes(joystick), 0));
  const u32 num_buttons = static_cast<u32>(std::max(SDL_GetNumJoystickButtons(joystick), 0));
  const u32 num_hats = static_cast<u32>(std::max(SDL_GetNumJoystickHats(joystick), 0));

  VERBOSE_LOG("Controller {} has {} axes, {} buttons and {} hats", player_id, num_axes, num_buttons, num_hats);

  cd.last_hat_state.resize(static_cast<size_t>(num_hats), u8(0));

  if (gamepad)
  {
    static constexpr auto map_desc = [](const SDL_GamepadBinding* binding) -> const char* {
      if (binding->output_type == SDL_GAMEPAD_BINDTYPE_BUTTON &&
          static_cast<u32>(binding->output.button) < SDL_GAMEPAD_BUTTON_COUNT)
      {
        return s_sdl_button_names[static_cast<u32>(binding->output.button)];
      }
      else if (binding->output_type == SDL_GAMEPAD_BINDTYPE_AXIS &&
               static_cast<u32>(binding->output.axis.axis) < SDL_GAMEPAD_AXIS_COUNT)
      {
        return s_sdl_axis_names[static_cast<u32>(binding->output.axis.axis)];
      }
      else
      {
        return "Unknown";
      }
    };

    // reserve the already-mapped gamepad inputs/outputs so that we don't duplicate events
    cd.joy_axis_used_in_gc.resize(num_axes, false);
    cd.joy_button_used_in_gc.resize(num_buttons, false);
    cd.joy_hat_used_in_gc.resize(num_hats, false);

    int binding_count = 0;
    SDL_GamepadBinding** const bindings = SDL_GetGamepadBindings(gamepad, &binding_count);
    for (int i = 0; i < binding_count; i++)
    {
      const SDL_GamepadBinding* binding = bindings[i];
      if (binding->input_type == SDL_GAMEPAD_BINDTYPE_BUTTON)
      {
        const u32 joy_button_index = static_cast<u32>(binding->input.button);
        if (joy_button_index < num_buttons && !cd.joy_button_used_in_gc[joy_button_index])
        {
          DEV_LOG("Controller {} button {} is mapped to gamepad {}", player_id, joy_button_index, map_desc(binding));
          cd.joy_button_used_in_gc[joy_button_index] = true;
        }
      }
      else if (binding->input_type == SDL_GAMEPAD_BINDTYPE_AXIS)
      {
        const u32 joy_axis_index = static_cast<u32>(binding->output.axis.axis);
        if (joy_axis_index < num_axes && !cd.joy_axis_used_in_gc[joy_axis_index])
        {
          DEV_LOG("Controller {} axis {} is mapped to gamepad {}", player_id, joy_axis_index, map_desc(binding));
          cd.joy_axis_used_in_gc[joy_axis_index] = true;
        }
      }
      else if (binding->input_type == SDL_GAMEPAD_BINDTYPE_HAT)
      {
        const u32 joy_hat_index = static_cast<u32>(binding->input.hat.hat);
        if (joy_hat_index < num_hats && !cd.joy_hat_used_in_gc[joy_hat_index])
        {
          DEV_LOG("Controller {} hat {} is mapped to gamepad {}", player_id, joy_hat_index, map_desc(binding));
          cd.joy_hat_used_in_gc[joy_hat_index] = true;
        }
      }
    }
    SDL_free(bindings);
  }

  cd.use_gamepad_rumble = (gamepad && SDL_GetBooleanProperty(properties, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false));
  if (cd.use_gamepad_rumble)
  {
    VERBOSE_LOG("Rumble is supported on '{}' via gamepad", name);
  }
  else
  {
    SDL_Haptic* haptic = SDL_OpenHapticFromJoystick(joystick);
    if (haptic)
    {
      SDL_HapticEffect ef = {};
      ef.leftright.type = SDL_HAPTIC_LEFTRIGHT;
      ef.leftright.length = 1000;

      int ef_id = SDL_CreateHapticEffect(haptic, &ef);
      if (ef_id >= 0)
      {
        cd.haptic = haptic;
        cd.haptic_left_right_effect = ef_id;
      }
      else
      {
        ERROR_LOG("Failed to create haptic left/right effect: {}", SDL_GetError());
        if (SDL_HapticRumbleSupported(haptic) && SDL_InitHapticRumble(haptic) != 0)
        {
          cd.haptic = haptic;
        }
        else
        {
          ERROR_LOG("No haptic rumble supported: {}", SDL_GetError());
          SDL_CloseHaptic(haptic);
        }
      }
    }

    if (cd.haptic)
      VERBOSE_LOG("Rumble is supported on '{}' via haptic", name);
  }

  if (!cd.haptic && !cd.use_gamepad_rumble)
    VERBOSE_LOG("Rumble is not supported on '{}'", name);

  cd.has_led = (gamepad && SDL_GetBooleanProperty(properties, SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN, false));
  if (cd.has_led && player_id >= 0 && static_cast<u32>(player_id) < MAX_LED_COLORS)
    SetControllerRGBLED(gamepad, m_led_colors[player_id]);

  m_controllers.push_back(std::move(cd));

  InputManager::OnInputDeviceConnected(MakeGenericControllerDeviceKey(InputSourceType::SDL, player_id),
                                       fmt::format("SDL-{}", player_id), name);
  return true;
}

bool SDLInputSource::CloseDevice(int joystick_index)
{
  auto it = GetControllerDataForJoystickId(joystick_index);
  if (it == m_controllers.end())
    return false;

  InputManager::OnInputDeviceDisconnected(MakeGenericControllerDeviceKey(InputSourceType::SDL, it->player_id),
                                          fmt::format("SDL-{}", it->player_id));

  if (it->haptic)
    SDL_CloseHaptic(it->haptic);

  if (it->gamepad)
    SDL_CloseGamepad(it->gamepad);
  else
    SDL_CloseJoystick(it->joystick);

  m_controllers.erase(it);
  return true;
}

static float NormalizeS16(s16 value)
{
  return static_cast<float>(value) / (value < 0 ? 32768.0f : 32767.0f);
}

bool SDLInputSource::HandleGamepadAxisMotionEvent(const SDL_GamepadAxisEvent* ev)
{
  auto it = GetControllerDataForJoystickId(ev->which);
  if (it == m_controllers.end())
    return false;

  const InputBindingKey key(MakeGenericControllerAxisKey(InputSourceType::SDL, it->player_id, ev->axis));
  const GenericInputBinding generic_key = (ev->axis < s_sdl_generic_binding_axis_mapping.size()) ?
                                            s_sdl_generic_binding_axis_mapping[ev->axis][ev->value >= 0] :
                                            GenericInputBinding::Unknown;
  InputManager::InvokeEvents(key, NormalizeS16(ev->value), generic_key);
  return true;
}

bool SDLInputSource::HandleGamepadButtonEvent(const SDL_GamepadButtonEvent* ev)
{
  auto it = GetControllerDataForJoystickId(ev->which);
  if (it == m_controllers.end())
    return false;

  const InputBindingKey key(MakeGenericControllerButtonKey(InputSourceType::SDL, it->player_id, ev->button));
  const GenericInputBinding generic_key = (ev->button < s_sdl_generic_binding_button_mapping.size()) ?
                                            s_sdl_generic_binding_button_mapping[ev->button] :
                                            GenericInputBinding::Unknown;
  InputManager::InvokeEvents(key, static_cast<float>(BoolToUInt32(ev->down)), generic_key);
  return true;
}

bool SDLInputSource::HandleGamepadTouchpadEvent(const SDL_GamepadTouchpadEvent* ev)
{
  // More than one touchpad?
  if (ev->touchpad != 0 || !m_controller_touchpad_as_pointer)
    return false;

  auto it = GetControllerDataForJoystickId(ev->which);
  if (it == m_controllers.end())
    return false;

  // Limited by InputManager pointers.
  const u32 pointer_index = static_cast<u32>(it->player_id);
  if (pointer_index >= InputManager::MAX_POINTER_DEVICES)
    return false;

  // Only looking at the first finger for motion for now.
  if (ev->finger == 0)
  {
    // If down event, reset the position.
    if (ev->type == SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN)
    {
      it->last_touch_x = ev->x;
      it->last_touch_y = ev->y;
    }

    const auto& [win_width, win_height] = InputManager::GetDisplayWindowSize();
    const float rel_x = (ev->x - std::exchange(it->last_touch_x, ev->x)) * win_width;
    const float rel_y = (ev->y - std::exchange(it->last_touch_y, ev->y)) * win_height;
    if (!InputManager::IsRelativeMouseModeActive())
    {
      const auto& [current_x, current_y] = InputManager::GetPointerAbsolutePosition(pointer_index);
      InputManager::UpdatePointerAbsolutePosition(pointer_index, current_x + rel_x, current_y + rel_y);
    }
    else
    {
      if (rel_x != 0.0f)
        InputManager::UpdatePointerRelativeDelta(pointer_index, InputPointerAxis::X, rel_x);
      if (rel_y != 0.0f)
        InputManager::UpdatePointerRelativeDelta(pointer_index, InputPointerAxis::Y, rel_y);
    }
  }

  // If down/up event, fire the clicked handler.
  if (ev->type == SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN || ev->type == SDL_EVENT_GAMEPAD_TOUCHPAD_UP)
  {
    const InputBindingKey key(InputManager::MakePointerButtonKey(pointer_index, static_cast<u32>(ev->finger)));
    InputManager::InvokeEvents(key, (ev->type == SDL_EVENT_GAMEPAD_TOUCHPAD_UP) ? 0.0f : ev->pressure);
  }

  return true;
}

bool SDLInputSource::HandleJoystickAxisEvent(const SDL_JoyAxisEvent* ev)
{
  auto it = GetControllerDataForJoystickId(ev->which);
  if (it == m_controllers.end())
    return false;
  if (ev->axis < it->joy_axis_used_in_gc.size() && it->joy_axis_used_in_gc[ev->axis])
    return false;                                                            // Will get handled by GC event
  const u32 axis = ev->axis + static_cast<u32>(std::size(s_sdl_axis_names)); // Ensure we don't conflict with GC axes
  const InputBindingKey key(MakeGenericControllerAxisKey(InputSourceType::SDL, it->player_id, axis));
  InputManager::InvokeEvents(key, NormalizeS16(ev->value));
  return true;
}

bool SDLInputSource::HandleJoystickButtonEvent(const SDL_JoyButtonEvent* ev)
{
  auto it = GetControllerDataForJoystickId(ev->which);
  if (it == m_controllers.end())
    return false;
  if (ev->button < it->joy_button_used_in_gc.size() && it->joy_button_used_in_gc[ev->button])
    return false; // Will get handled by GC event
  const u32 button =
    ev->button + static_cast<u32>(std::size(s_sdl_button_names)); // Ensure we don't conflict with GC buttons
  const InputBindingKey key(MakeGenericControllerButtonKey(InputSourceType::SDL, it->player_id, button));
  InputManager::InvokeEvents(key, static_cast<float>(BoolToUInt32(ev->down)));
  return true;
}

bool SDLInputSource::HandleJoystickHatEvent(const SDL_JoyHatEvent* ev)
{
  auto it = GetControllerDataForJoystickId(ev->which);
  if (it == m_controllers.end() || ev->hat >= it->last_hat_state.size())
    return false;
  if (ev->hat < it->joy_hat_used_in_gc.size() && it->joy_hat_used_in_gc[ev->hat])
    return false; // Will get handled by GC event

  const unsigned long last_direction = it->last_hat_state[ev->hat];
  it->last_hat_state[ev->hat] = ev->value;

  unsigned long changed_direction = last_direction ^ ev->value;
  while (changed_direction != 0)
  {
    const u32 pos = CountTrailingZeros(changed_direction);

    const unsigned long mask = (1u << pos);
    changed_direction &= ~mask;

    const InputBindingKey key(MakeGenericControllerHatKey(InputSourceType::SDL, it->player_id, ev->hat,
                                                          static_cast<u8>(pos),
                                                          static_cast<u32>(std::size(s_sdl_hat_direction_names))));
    InputManager::InvokeEvents(key, (last_direction & mask) ? 0.0f : 1.0f);
  }

  return true;
}

InputManager::VibrationMotorList SDLInputSource::EnumerateVibrationMotors(std::optional<InputBindingKey> for_device)
{
  InputManager::VibrationMotorList ret;

  if (for_device.has_value() && for_device->source_type != InputSourceType::SDL)
    return ret;

  InputBindingKey key = {};
  key.source_type = InputSourceType::SDL;

  for (ControllerData& cd : m_controllers)
  {
    if (for_device.has_value() && for_device->source_index != static_cast<u32>(cd.player_id))
      continue;

    key.source_index = cd.player_id;

    if (cd.use_gamepad_rumble || cd.haptic_left_right_effect)
    {
      // two motors
      key.source_subtype = InputSubclass::ControllerMotor;
      key.data = 0;
      ret.push_back(key);
      key.data = 1;
      ret.push_back(key);
    }
    else if (cd.haptic)
    {
      // haptic effect
      key.source_subtype = InputSubclass::ControllerHaptic;
      key.data = 0;
      ret.push_back(key);
    }
  }

  return ret;
}

bool SDLInputSource::GetGenericBindingMapping(std::string_view device, GenericInputBindingMapping* mapping)
{
  if (!device.starts_with("SDL-"))
    return false;

  const std::optional<s32> player_id = StringUtil::FromChars<s32>(device.substr(4));
  if (!player_id.has_value() || player_id.value() < 0)
    return false;

  ControllerDataVector::iterator it = GetControllerDataForPlayerId(player_id.value());
  if (it == m_controllers.end())
    return false;

  if (it->gamepad)
  {
    // assume all buttons are present.
    const s32 pid = player_id.value();
    for (u32 i = 0; i < std::size(s_sdl_generic_binding_axis_mapping); i++)
    {
      const GenericInputBinding negative = s_sdl_generic_binding_axis_mapping[i][0];
      const GenericInputBinding positive = s_sdl_generic_binding_axis_mapping[i][1];
      if (negative != GenericInputBinding::Unknown)
        mapping->emplace_back(negative, fmt::format("SDL-{}/-{}", pid, s_sdl_axis_names[i]));

      if (positive != GenericInputBinding::Unknown)
        mapping->emplace_back(positive, fmt::format("SDL-{}/+{}", pid, s_sdl_axis_names[i]));
    }
    for (u32 i = 0; i < std::size(s_sdl_generic_binding_button_mapping); i++)
    {
      const GenericInputBinding binding = s_sdl_generic_binding_button_mapping[i];
      if (binding != GenericInputBinding::Unknown)
        mapping->emplace_back(binding, fmt::format("SDL-{}/{}", pid, s_sdl_button_names[i]));
    }

    if (it->use_gamepad_rumble || it->haptic_left_right_effect)
    {
      mapping->emplace_back(GenericInputBinding::SmallMotor, fmt::format("SDL-{}/SmallMotor", pid));
      mapping->emplace_back(GenericInputBinding::LargeMotor, fmt::format("SDL-{}/LargeMotor", pid));
    }
    else
    {
      mapping->emplace_back(GenericInputBinding::SmallMotor, fmt::format("SDL-{}/Haptic", pid));
      mapping->emplace_back(GenericInputBinding::LargeMotor, fmt::format("SDL-{}/Haptic", pid));
    }

    return true;
  }
  else
  {
    // joysticks have arbitrary axis numbers, so automapping isn't going to work here.
    return false;
  }
}

void SDLInputSource::UpdateMotorState(InputBindingKey key, float intensity)
{
  if (key.source_subtype != InputSubclass::ControllerMotor && key.source_subtype != InputSubclass::ControllerHaptic)
    return;

  auto it = GetControllerDataForPlayerId(key.source_index);
  if (it == m_controllers.end())
    return;

  it->rumble_intensity[key.data] = static_cast<u16>(intensity * 65535.0f);
  SendRumbleUpdate(&(*it));
}

void SDLInputSource::UpdateMotorState(InputBindingKey large_key, InputBindingKey small_key, float large_intensity,
                                      float small_intensity)
{
  if (large_key.source_index != small_key.source_index || large_key.source_subtype != InputSubclass::ControllerMotor ||
      small_key.source_subtype != InputSubclass::ControllerMotor)
  {
    // bonkers config where they're mapped to different controllers... who would do such a thing?
    UpdateMotorState(large_key, large_intensity);
    UpdateMotorState(small_key, small_intensity);
    return;
  }

  auto it = GetControllerDataForPlayerId(large_key.source_index);
  if (it == m_controllers.end())
    return;

  it->rumble_intensity[large_key.data] = static_cast<u16>(large_intensity * 65535.0f);
  it->rumble_intensity[small_key.data] = static_cast<u16>(small_intensity * 65535.0f);
  SendRumbleUpdate(&(*it));
}

void SDLInputSource::SendRumbleUpdate(ControllerData* cd)
{
  // we'll update before this duration is elapsed
  static constexpr u32 DURATION = 65535; // SDL_MAX_RUMBLE_DURATION_MS

  if (cd->use_gamepad_rumble)
  {
    SDL_RumbleGamepad(cd->gamepad, cd->rumble_intensity[0], cd->rumble_intensity[1], DURATION);
    return;
  }

  if (cd->haptic_left_right_effect >= 0)
  {
    if ((static_cast<u32>(cd->rumble_intensity[0]) + static_cast<u32>(cd->rumble_intensity[1])) > 0)
    {
      SDL_HapticEffect ef;
      ef.type = SDL_HAPTIC_LEFTRIGHT;
      ef.leftright.large_magnitude = cd->rumble_intensity[0];
      ef.leftright.small_magnitude = cd->rumble_intensity[1];
      ef.leftright.length = DURATION;
      SDL_UpdateHapticEffect(cd->haptic, cd->haptic_left_right_effect, &ef);
      SDL_RunHapticEffect(cd->haptic, cd->haptic_left_right_effect, SDL_HAPTIC_INFINITY);
    }
    else
    {
      SDL_StopHapticEffect(cd->haptic, cd->haptic_left_right_effect);
    }
  }
  else
  {
    const float strength =
      static_cast<float>(std::max(cd->rumble_intensity[0], cd->rumble_intensity[1])) * (1.0f / 65535.0f);
    if (strength > 0.0f)
      SDL_PlayHapticRumble(cd->haptic, strength, DURATION);
    else
      SDL_StopHapticRumble(cd->haptic);
  }
}

std::unique_ptr<InputSource> InputSource::CreateSDLSource()
{
  return std::make_unique<SDLInputSource>();
}

std::unique_ptr<ForceFeedbackDevice> SDLInputSource::CreateForceFeedbackDevice(std::string_view device, Error* error)
{
  SDL_Joystick* joystick = GetJoystickForDevice(device);
  if (!joystick)
  {
    Error::SetStringFmt(error, "No SDL_Joystick for {}", device);
    return nullptr;
  }

  SDL_Haptic* haptic = SDL_OpenHapticFromJoystick(joystick);
  if (!haptic)
  {
    Error::SetStringFmt(error, "Haptic is not supported on {} ({})", device, SDL_GetJoystickName(joystick));
    return nullptr;
  }

  return std::unique_ptr<SDLForceFeedbackDevice>(new SDLForceFeedbackDevice(joystick, haptic));
}

SDLForceFeedbackDevice::SDLForceFeedbackDevice(SDL_Joystick* joystick, SDL_Haptic* haptic) : m_haptic(haptic)
{
  std::memset(&m_constant_effect, 0, sizeof(m_constant_effect));
}

SDLForceFeedbackDevice::~SDLForceFeedbackDevice()
{
  if (m_haptic)
  {
    DestroyEffects();

    SDL_CloseHaptic(m_haptic);
    m_haptic = nullptr;
  }
}

void SDLForceFeedbackDevice::CreateEffects(SDL_Joystick* joystick)
{
  constexpr u32 length = 10000; // 10 seconds since NFS games seem to not issue new commands while rotating.

  const u32 features = SDL_GetHapticFeatures(m_haptic);
  if (features & SDL_HAPTIC_CONSTANT)
  {
    m_constant_effect.type = SDL_HAPTIC_CONSTANT;
    m_constant_effect.constant.direction.type = SDL_HAPTIC_STEERING_AXIS;
    m_constant_effect.constant.length = length;

    m_constant_effect_id = SDL_CreateHapticEffect(m_haptic, &m_constant_effect);
    if (m_constant_effect_id < 0)
      ERROR_LOG("SDL_HapticNewEffect() for constant failed: {}", SDL_GetError());
  }
  else
  {
    WARNING_LOG("Constant effect is not supported on '{}'", SDL_GetJoystickName(joystick));
  }
}

void SDLForceFeedbackDevice::DestroyEffects()
{
  if (m_constant_effect_id >= 0)
  {
    if (m_constant_effect_running)
    {
      SDL_StopHapticEffect(m_haptic, m_constant_effect_id);
      m_constant_effect_running = false;
    }
    SDL_DestroyHapticEffect(m_haptic, m_constant_effect_id);
    m_constant_effect_id = -1;
  }
}

template<typename T>
[[maybe_unused]] static u16 ClampU16(T val)
{
  return static_cast<u16>(std::clamp<T>(val, 0, 65535));
}

template<typename T>
[[maybe_unused]] static u16 ClampS16(T val)
{
  return static_cast<s16>(std::clamp<T>(val, -32768, 32767));
}

void SDLForceFeedbackDevice::SetConstantForce(s32 level)
{
  if (m_constant_effect_id < 0)
    return;

  const s16 new_level = ClampS16(level);
  if (m_constant_effect.constant.level != new_level)
  {
    m_constant_effect.constant.level = new_level;
    if (SDL_UpdateHapticEffect(m_haptic, m_constant_effect_id, &m_constant_effect) != 0)
      ERROR_LOG("SDL_HapticUpdateEffect() for constant failed: {}", SDL_GetError());
  }

  if (!m_constant_effect_running)
  {
    if (SDL_RunHapticEffect(m_haptic, m_constant_effect_id, SDL_HAPTIC_INFINITY) == 0)
      m_constant_effect_running = true;
    else
      ERROR_LOG("SDL_HapticRunEffect() for constant failed: {}", SDL_GetError());
  }
}

void SDLForceFeedbackDevice::DisableForce(Effect force)
{
  switch (force)
  {
    case Effect::Constant:
    {
      if (m_constant_effect_running)
      {
        SDL_StopHapticEffect(m_haptic, m_constant_effect_id);
        m_constant_effect_running = false;
      }
    }
    break;

    default:
      break;
  }
}
