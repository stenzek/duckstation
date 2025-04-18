// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "input_manager.h"
#include "imgui_manager.h"
#include "input_source.h"

#include "core/controller.h"
#include "core/host.h"
#include "core/system.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"
#include "common/timer.h"

#include "IconsPromptFont.h"

#include "fmt/core.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <variant>
#include <vector>

LOG_CHANNEL(InputManager);

namespace InputManager {

// ------------------------------------------------------------------------
// Constants
// ------------------------------------------------------------------------

enum : u32
{
  FIRST_EXTERNAL_INPUT_SOURCE = static_cast<u32>(InputSourceType::Pointer) + 1u,
  LAST_EXTERNAL_INPUT_SOURCE = static_cast<u32>(InputSourceType::Count),
};

// ------------------------------------------------------------------------
// Binding Type
// ------------------------------------------------------------------------
// This class tracks both the keys which make it up (for chords), as well
// as the state of all buttons. For button callbacks, it's fired when
// all keys go active, and for axis callbacks, when all are active and
// the value changes.

namespace {

struct InputBinding
{
  InputBindingKey keys[MAX_KEYS_PER_BINDING] = {};
  InputEventHandler handler;
  u8 num_keys = 0;
  u8 full_mask = 0;
  u8 current_mask = 0;
};

struct PadVibrationBinding
{
  struct Motor
  {
    InputBindingKey binding;
    Timer::Value last_update_time;
    InputSource* source;
    float last_intensity;
  };

  u32 pad_index = 0;
  Motor motors[MAX_MOTORS_PER_PAD] = {};

  /// Returns true if the two motors are bound to the same host motor.
  ALWAYS_INLINE bool AreMotorsCombined() const { return motors[0].binding == motors[1].binding; }

  /// Returns the intensity when both motors are combined.
  ALWAYS_INLINE float GetCombinedIntensity() const
  {
    return std::max(motors[0].last_intensity, motors[1].last_intensity);
  }
};

struct MacroButton
{
  std::vector<u32> buttons; ///< Buttons to activate.
  u16 toggle_frequency;     ///< Interval at which the buttons will be toggled, if not 0.
  u16 toggle_counter;       ///< When this counter reaches zero, buttons will be toggled.
  bool toggle_state;        ///< Current state for turbo.
  bool trigger_state;       ///< Whether the macro button is active.
  bool trigger_toggle;      ///< Whether the macro is trigged by holding or press.
  u8 trigger_pressure;      ///< Pressure to apply when macro is active.
};

} // namespace

// ------------------------------------------------------------------------
// Forward Declarations (for static qualifier)
// ------------------------------------------------------------------------
static std::optional<InputBindingKey> ParseHostKeyboardKey(std::string_view source, std::string_view sub_binding);
static std::optional<InputBindingKey> ParsePointerKey(std::string_view source, std::string_view sub_binding);
static std::optional<InputBindingKey> ParseSensorKey(std::string_view source, std::string_view sub_binding);

static std::vector<std::string_view> SplitChord(std::string_view binding);
static bool SplitBinding(std::string_view binding, std::string_view* source, std::string_view* sub_binding);
static void PrettifyInputBindingPart(std::string_view binding, BindingIconMappingFunction mapper, SmallString& ret,
                                     bool& changed);
static void AddBindings(const std::vector<std::string>& bindings, const InputEventHandler& handler);
static void UpdatePointerCount();

static bool IsAxisHandler(const InputEventHandler& handler);
static float ApplySingleBindingScale(float sensitivity, float deadzone, float value);

static void AddHotkeyBindings(const SettingsInterface& si);
static void AddPadBindings(const SettingsInterface& si, const std::string& section, u32 pad,
                           const Controller::ControllerInfo& cinfo);
static void UpdateContinuedVibration();
static void GenerateRelativeMouseEvents();

static bool DoEventHook(InputBindingKey key, float value);
static bool PreprocessEvent(InputBindingKey key, float value, GenericInputBinding generic_key);
static bool ProcessEvent(InputBindingKey key, float value, bool skip_button_handlers);

static void LoadMacroButtonConfig(const SettingsInterface& si, const std::string& section, u32 pad,
                                  const Controller::ControllerInfo& cinfo);
static void ApplyMacroButton(u32 pad, const MacroButton& mb);
static void UpdateMacroButtons();

static void UpdateInputSourceState(const SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock,
                                   InputSourceType type, std::unique_ptr<InputSource> (*factory_function)());

// ------------------------------------------------------------------------
// Local Variables
// ------------------------------------------------------------------------

// This is a multimap containing any binds related to the specified key.
using BindingMap = std::unordered_multimap<InputBindingKey, std::shared_ptr<InputBinding>, InputBindingKeyHash>;
using VibrationBindingArray = std::vector<PadVibrationBinding>;
static BindingMap s_binding_map;
static VibrationBindingArray s_pad_vibration_array;
static std::recursive_mutex s_mutex;

// Hooks/intercepting (for setting bindings)
static InputInterceptHook::Callback m_event_intercept_callback;

// Input sources. Keyboard/mouse don't exist here.
static std::array<std::unique_ptr<InputSource>, static_cast<u32>(InputSourceType::Count)> s_input_sources;

// Macro buttons.
static std::array<std::array<MacroButton, InputManager::NUM_MACRO_BUTTONS_PER_CONTROLLER>,
                  NUM_CONTROLLER_AND_CARD_PORTS>
  s_macro_buttons;

// ------------------------------------------------------------------------
// Hotkeys
// ------------------------------------------------------------------------

static const HotkeyInfo* const s_hotkey_list[] = {g_common_hotkeys, g_host_hotkeys};

// ------------------------------------------------------------------------
// Tracking host mouse movement and turning into relative events
// 4 axes: pointer left/right, wheel vertical/horizontal. Last/Next/Normalized.
// ------------------------------------------------------------------------
static constexpr const std::array<const char*, static_cast<u8>(InputPointerAxis::Count)> s_pointer_axis_names = {
  {"X", "Y", "WheelX", "WheelY"}};
static constexpr const std::array<const char*, 3> s_pointer_button_names = {
  {"LeftButton", "RightButton", "MiddleButton"}};
static constexpr const std::array<const char*, 3> s_sensor_accelerometer_names = {{"Turn", "Tilt", "Rotate"}};

struct PointerAxisState
{
  std::atomic<s32> delta;
  float last_value;
};
static std::array<std::array<float, static_cast<u8>(InputPointerAxis::Count)>, InputManager::MAX_POINTER_DEVICES>
  s_host_pointer_positions;
static std::array<std::array<PointerAxisState, static_cast<u8>(InputPointerAxis::Count)>,
                  InputManager::MAX_POINTER_DEVICES>
  s_pointer_state;
static u32 s_pointer_count = 0;
static std::array<float, static_cast<u8>(InputPointerAxis::Count)> s_pointer_axis_scale;

using PointerMoveCallback = std::function<void(InputBindingKey key, float value)>;
static std::vector<std::pair<u32, PointerMoveCallback>> s_pointer_move_callbacks;

// Window size, used for clamping the mouse position in raw input modes.
static std::array<float, 2> s_window_size = {};
static bool s_relative_mouse_mode = false;
static bool s_relative_mouse_mode_active = false;
static bool s_hide_host_mouse_cursor = false;
static bool s_hide_host_mouse_cusor_active = false;

} // namespace InputManager

// ------------------------------------------------------------------------
// Binding Parsing
// ------------------------------------------------------------------------

std::vector<std::string_view> InputManager::SplitChord(std::string_view binding)
{
  std::vector<std::string_view> parts;

  // under an if for RVO
  if (!binding.empty())
  {
    std::string_view::size_type last = 0;
    std::string_view::size_type next;
    while ((next = binding.find('&', last)) != std::string_view::npos)
    {
      if (last != next)
      {
        std::string_view part(StringUtil::StripWhitespace(binding.substr(last, next - last)));
        if (!part.empty())
          parts.push_back(std::move(part));
      }
      last = next + 1;
    }
    if (last < (binding.size() - 1))
    {
      std::string_view part(StringUtil::StripWhitespace(binding.substr(last)));
      if (!part.empty())
        parts.push_back(std::move(part));
    }
  }

  return parts;
}

bool InputManager::SplitBinding(std::string_view binding, std::string_view* source, std::string_view* sub_binding)
{
  const std::string_view::size_type slash_pos = binding.find('/');
  if (slash_pos == std::string_view::npos)
  {
    WARNING_LOG("Malformed binding: '{}'", binding);
    return false;
  }

  *source = std::string_view(binding).substr(0, slash_pos);
  *sub_binding = std::string_view(binding).substr(slash_pos + 1);
  return true;
}

std::optional<InputBindingKey> InputManager::ParseInputBindingKey(std::string_view binding)
{
  std::string_view source, sub_binding;
  if (!SplitBinding(binding, &source, &sub_binding))
    return std::nullopt;

  // lameee, string matching
  if (source.starts_with("Keyboard"))
  {
    return ParseHostKeyboardKey(source, sub_binding);
  }
  else if (source.starts_with("Pointer"))
  {
    return ParsePointerKey(source, sub_binding);
  }
  else if (source.starts_with("Sensor"))
  {
    return ParseSensorKey(source, sub_binding);
  }
  else
  {
    for (u32 i = FIRST_EXTERNAL_INPUT_SOURCE; i < LAST_EXTERNAL_INPUT_SOURCE; i++)
    {
      if (s_input_sources[i])
      {
        std::optional<InputBindingKey> key = s_input_sources[i]->ParseKeyString(source, sub_binding);
        if (key.has_value())
          return key;
      }
    }
  }

  return std::nullopt;
}

bool InputManager::ParseBindingAndGetSource(std::string_view binding, InputBindingKey* key, InputSource** source)
{
  std::string_view source_string, sub_binding;
  if (!SplitBinding(binding, &source_string, &sub_binding))
    return false;

  for (u32 i = FIRST_EXTERNAL_INPUT_SOURCE; i < LAST_EXTERNAL_INPUT_SOURCE; i++)
  {
    if (s_input_sources[i])
    {
      std::optional<InputBindingKey> parsed_key = s_input_sources[i]->ParseKeyString(source_string, sub_binding);
      if (parsed_key.has_value())
      {
        *key = parsed_key.value();
        *source = s_input_sources[i].get();
        return true;
      }
    }
  }

  return false;
}

std::string InputManager::ConvertInputBindingKeyToString(InputBindingInfo::Type binding_type, InputBindingKey key)
{
  if (binding_type == InputBindingInfo::Type::Pointer || binding_type == InputBindingInfo::Type::RelativePointer ||
      binding_type == InputBindingInfo::Type::Device)
  {
    // pointer and device bindings don't have a data part
    if (key.source_type == InputSourceType::Pointer)
    {
      return GetPointerDeviceName(key.source_index);
    }
    else if (key.source_type < InputSourceType::Count && s_input_sources[static_cast<u32>(key.source_type)])
    {
      // This assumes that it always follows the Type/Binding form.
      std::string keystr(s_input_sources[static_cast<u32>(key.source_type)]->ConvertKeyToString(key));
      std::string::size_type pos = keystr.find('/');
      if (pos != std::string::npos)
        keystr.erase(pos);
      return keystr;
    }
  }
  else
  {
    if (key.source_type == InputSourceType::Keyboard)
    {
      const std::optional<std::string> str(ConvertHostKeyboardCodeToString(key.data));
      if (str.has_value() && !str->empty())
        return fmt::format("Keyboard/{}", str->c_str());
    }
    else if (key.source_type == InputSourceType::Pointer)
    {
      if (key.source_subtype == InputSubclass::PointerButton)
      {
        if (key.data < s_pointer_button_names.size())
          return fmt::format("Pointer-{}/{}", u32{key.source_index}, s_pointer_button_names[key.data]);
        else
          return fmt::format("Pointer-{}/Button{}", u32{key.source_index}, key.data);
      }
      else if (key.source_subtype == InputSubclass::PointerAxis)
      {
        return fmt::format("Pointer-{}/{}{:c}", u32{key.source_index}, s_pointer_axis_names[key.data],
                           key.modifier == InputModifier::Negate ? '-' : '+');
      }
    }
    else if (key.source_type < InputSourceType::Count && s_input_sources[static_cast<u32>(key.source_type)])
    {
      return std::string(s_input_sources[static_cast<u32>(key.source_type)]->ConvertKeyToString(key));
    }
  }

  return {};
}

std::string InputManager::ConvertInputBindingKeysToString(InputBindingInfo::Type binding_type,
                                                          const InputBindingKey* keys, size_t num_keys)
{
  // can't have a chord of devices/pointers
  if (binding_type == InputBindingInfo::Type::Pointer || binding_type == InputBindingInfo::Type::RelativePointer ||
      binding_type == InputBindingInfo::Type::Device)
  {
    // so only take the first
    if (num_keys > 0)
      return ConvertInputBindingKeyToString(binding_type, keys[0]);
  }

  std::stringstream ss;
  for (size_t i = 0; i < num_keys; i++)
  {
    const std::string keystr(ConvertInputBindingKeyToString(binding_type, keys[i]));
    if (keystr.empty())
      return std::string();

    if (i > 0)
      ss << " & ";

    ss << keystr;
  }

  return std::move(ss).str();
}

bool InputManager::PrettifyInputBinding(SmallStringBase& binding, BindingIconMappingFunction mapper /*= nullptr*/)
{
  if (binding.empty())
    return false;

  mapper = mapper ? mapper : [](std::string_view v) { return v; };

  const std::string_view binding_view = binding.view();

  SmallString ret;
  bool changed = false;

  std::string_view::size_type last = 0;
  std::string_view::size_type next;
  while ((next = binding_view.find('&', last)) != std::string_view::npos)
  {
    if (last != next)
    {
      const std::string_view part = StringUtil::StripWhitespace(binding_view.substr(last, next - last));
      if (!part.empty())
      {
        if (!ret.empty())
          ret.append(" + ");
        PrettifyInputBindingPart(part, mapper, ret, changed);
      }
    }
    last = next + 1;
  }
  if (last < (binding_view.size() - 1))
  {
    const std::string_view part = StringUtil::StripWhitespace(binding_view.substr(last));
    if (!part.empty())
    {
      if (!ret.empty())
        ret.append(" + ");
      PrettifyInputBindingPart(part, mapper, ret, changed);
    }
  }

  if (changed)
    binding = ret;

  return changed;
}

void InputManager::PrettifyInputBindingPart(const std::string_view binding, BindingIconMappingFunction mapper,
                                            SmallString& ret, bool& changed)
{
  std::string_view source, sub_binding;
  if (!SplitBinding(binding, &source, &sub_binding))
    return;

  // lameee, string matching
  if (source.starts_with("Keyboard"))
  {
    std::optional<InputBindingKey> key = ParseHostKeyboardKey(source, sub_binding);
    const char* icon = key.has_value() ? ConvertHostKeyboardCodeToIcon(key->data) : nullptr;
    if (icon)
    {
      ret.append(icon);
      changed = true;
      return;
    }
  }
  else if (source.starts_with("Pointer"))
  {
    const std::optional<InputBindingKey> key = ParsePointerKey(source, sub_binding);
    if (key.has_value())
    {
      if (key->source_subtype == InputSubclass::PointerButton)
      {
        static constexpr const char* button_icons[] = {
          ICON_PF_MOUSE_BUTTON_1, ICON_PF_MOUSE_BUTTON_2, ICON_PF_MOUSE_BUTTON_3,
          ICON_PF_MOUSE_BUTTON_4, ICON_PF_MOUSE_BUTTON_5,
        };
        if (key->data < std::size(button_icons))
        {
          ret.append(button_icons[key->data]);
          changed = true;
          return;
        }
      }
    }
  }
  else if (source.starts_with("Sensor"))
  {
  }
  else
  {
    for (u32 i = FIRST_EXTERNAL_INPUT_SOURCE; i < LAST_EXTERNAL_INPUT_SOURCE; i++)
    {
      if (s_input_sources[i])
      {
        std::optional<InputBindingKey> key = s_input_sources[i]->ParseKeyString(source, sub_binding);
        if (key.has_value())
        {
          const TinyString icon = s_input_sources[i]->ConvertKeyToIcon(key.value(), mapper);
          if (!icon.empty())
          {
            ret.append(icon);
            changed = true;
            return;
          }

          break;
        }
      }
    }
  }

  ret.append(binding);
}

void InputManager::AddBindings(const std::vector<std::string>& bindings, const InputEventHandler& handler)
{
  for (const std::string& binding : bindings)
    AddBinding(binding, handler);
}

void InputManager::AddBinding(std::string_view binding, const InputEventHandler& handler)
{
  std::shared_ptr<InputBinding> ibinding;
  const std::vector<std::string_view> chord_bindings(SplitChord(binding));

  for (const std::string_view& chord_binding : chord_bindings)
  {
    std::optional<InputBindingKey> key = ParseInputBindingKey(chord_binding);
    if (!key.has_value())
    {
      ERROR_LOG("Invalid binding: '{}'", binding);
      ibinding.reset();
      break;
    }

    if (!ibinding)
    {
      ibinding = std::make_shared<InputBinding>();
      ibinding->handler = handler;
    }

    if (ibinding->num_keys == MAX_KEYS_PER_BINDING)
    {
      ERROR_LOG("Too many chord parts, max is {} ({})", static_cast<unsigned>(MAX_KEYS_PER_BINDING), binding.size());
      ibinding.reset();
      break;
    }

    ibinding->keys[ibinding->num_keys] = key.value();
    ibinding->full_mask |= (static_cast<u8>(1) << ibinding->num_keys);
    ibinding->num_keys++;
  }

  if (!ibinding)
    return;

  // plop it in the input map for all the keys
  for (u32 i = 0; i < ibinding->num_keys; i++)
    s_binding_map.emplace(ibinding->keys[i].MaskDirection(), ibinding);
}

void InputManager::AddVibrationBinding(u32 pad_index, const InputBindingKey* motor_0_binding,
                                       InputSource* motor_0_source, const InputBindingKey* motor_1_binding,
                                       InputSource* motor_1_source)
{
  PadVibrationBinding vib;
  vib.pad_index = pad_index;
  if (motor_0_binding)
  {
    vib.motors[0].binding = *motor_0_binding;
    vib.motors[0].source = motor_0_source;
  }
  if (motor_1_binding)
  {
    vib.motors[1].binding = *motor_1_binding;
    vib.motors[1].source = motor_1_source;
  }
  s_pad_vibration_array.push_back(std::move(vib));
}

// ------------------------------------------------------------------------
// Key Decoders
// ------------------------------------------------------------------------

InputBindingKey InputManager::MakeHostKeyboardKey(u32 key_code)
{
  InputBindingKey key = {};
  key.source_type = InputSourceType::Keyboard;
  key.data = key_code;
  return key;
}

InputBindingKey InputManager::MakePointerButtonKey(u32 index, u32 button_index)
{
  InputBindingKey key = {};
  key.source_index = index;
  key.source_type = InputSourceType::Pointer;
  key.source_subtype = InputSubclass::PointerButton;
  key.data = button_index;
  return key;
}

InputBindingKey InputManager::MakePointerAxisKey(u32 index, InputPointerAxis axis)
{
  InputBindingKey key = {};
  key.data = static_cast<u32>(axis);
  key.source_index = index;
  key.source_type = InputSourceType::Pointer;
  key.source_subtype = InputSubclass::PointerAxis;
  return key;
}

InputBindingKey InputManager::MakeSensorAxisKey(InputSubclass sensor, u32 axis)
{
  InputBindingKey key = {};
  key.data = static_cast<u32>(axis);
  key.source_index = 0;
  key.source_type = InputSourceType::Sensor;
  key.source_subtype = sensor;
  return key;
}

// ------------------------------------------------------------------------
// Bind Encoders
// ------------------------------------------------------------------------

static std::array<const char*, static_cast<u32>(InputSourceType::Count)> s_input_class_names = {{
  "Keyboard",
  "Pointer",
  "Sensor",
#ifdef _WIN32
  "DInput",
  "XInput",
  "RawInput",
#endif
#ifdef ENABLE_SDL
  "SDL",
#endif
#ifdef __ANDROID__
  "Android",
#endif
}};

InputSource* InputManager::GetInputSourceInterface(InputSourceType type)
{
  return s_input_sources[static_cast<u32>(type)].get();
}

const char* InputManager::InputSourceToString(InputSourceType clazz)
{
  return s_input_class_names[static_cast<u32>(clazz)];
}

bool InputManager::GetInputSourceDefaultEnabled(InputSourceType type)
{
  switch (type)
  {
    case InputSourceType::Keyboard:
    case InputSourceType::Pointer:
      return true;

#ifdef _WIN32
    case InputSourceType::DInput:
      return false;

    case InputSourceType::XInput:
      return false;

    case InputSourceType::RawInput:
      return false;
#endif

#ifdef ENABLE_SDL
    case InputSourceType::SDL:
      return true;
#endif

#ifdef __ANDROID__
    case InputSourceType::Android:
      return true;
#endif

    default:
      return false;
  }
}

std::optional<InputSourceType> InputManager::ParseInputSourceString(std::string_view str)
{
  for (u32 i = 0; i < static_cast<u32>(InputSourceType::Count); i++)
  {
    if (str == s_input_class_names[i])
      return static_cast<InputSourceType>(i);
  }

  return std::nullopt;
}

std::optional<InputBindingKey> InputManager::ParseHostKeyboardKey(std::string_view source, std::string_view sub_binding)
{
  if (source != "Keyboard")
    return std::nullopt;

  const std::optional<s32> code = ConvertHostKeyboardStringToCode(sub_binding);
  if (!code.has_value())
    return std::nullopt;

  InputBindingKey key = {};
  key.source_type = InputSourceType::Keyboard;
  key.data = static_cast<u32>(code.value());
  return key;
}

std::optional<InputBindingKey> InputManager::ParsePointerKey(std::string_view source, std::string_view sub_binding)
{
  const std::optional<s32> pointer_index = StringUtil::FromChars<s32>(source.substr(8));
  if (!pointer_index.has_value() || pointer_index.value() < 0)
    return std::nullopt;

  InputBindingKey key = {};
  key.source_type = InputSourceType::Pointer;
  key.source_index = static_cast<u32>(pointer_index.value());

  if (sub_binding.starts_with("Button"))
  {
    const std::optional<s32> button_number = StringUtil::FromChars<s32>(sub_binding.substr(6));
    if (!button_number.has_value() || button_number.value() < 0)
      return std::nullopt;

    key.source_subtype = InputSubclass::PointerButton;
    key.data = static_cast<u32>(button_number.value());
    return key;
  }

  for (u32 i = 0; i < s_pointer_axis_names.size(); i++)
  {
    if (sub_binding.starts_with(s_pointer_axis_names[i]))
    {
      key.source_subtype = InputSubclass::PointerAxis;
      key.data = i;

      const std::string_view dir_part(sub_binding.substr(std::strlen(s_pointer_axis_names[i])));
      if (dir_part == "+")
        key.modifier = InputModifier::None;
      else if (dir_part == "-")
        key.modifier = InputModifier::Negate;
      else
        return std::nullopt;

      return key;
    }
  }

  for (u32 i = 0; i < s_pointer_button_names.size(); i++)
  {
    if (sub_binding == s_pointer_button_names[i])
    {
      key.source_subtype = InputSubclass::PointerButton;
      key.data = i;
      return key;
    }
  }

  return std::nullopt;
}

std::optional<u32> InputManager::GetIndexFromPointerBinding(std::string_view source)
{
  if (!source.starts_with("Pointer-"))
    return std::nullopt;

  const std::optional<s32> pointer_index = StringUtil::FromChars<s32>(source.substr(8));
  if (!pointer_index.has_value() || pointer_index.value() < 0)
    return std::nullopt;

  return static_cast<u32>(pointer_index.value());
}

std::string InputManager::GetPointerDeviceName(u32 pointer_index)
{
  return fmt::format("Pointer-{}", pointer_index);
}

std::optional<InputBindingKey> InputManager::ParseSensorKey(std::string_view source, std::string_view sub_binding)
{
  if (source != "Sensor")
    return std::nullopt;

  InputBindingKey key = {};
  key.source_type = InputSourceType::Sensor;
  key.source_index = 0;

  for (u32 i = 0; i < s_sensor_accelerometer_names.size(); i++)
  {
    if (sub_binding.starts_with(s_sensor_accelerometer_names[i]))
    {
      key.source_subtype = InputSubclass::SensorAccelerometer;
      key.data = i;

      const std::string_view dir_part(sub_binding.substr(std::strlen(s_sensor_accelerometer_names[i])));
      if (dir_part == "+")
        key.modifier = InputModifier::None;
      else if (dir_part == "-")
        key.modifier = InputModifier::Negate;
      else
        return std::nullopt;

      return key;
    }
  }

  return std::nullopt;
}

// ------------------------------------------------------------------------
// Binding Enumeration
// ------------------------------------------------------------------------

float InputManager::ApplySingleBindingScale(float scale, float deadzone, float value)
{
  const float svalue = std::clamp(value * scale, 0.0f, 1.0f);
  return (deadzone > 0.0f && svalue < deadzone) ? 0.0f : svalue;
}

std::vector<const HotkeyInfo*> InputManager::GetHotkeyList()
{
  std::vector<const HotkeyInfo*> ret;
  for (const HotkeyInfo* hotkey_list : s_hotkey_list)
  {
    for (const HotkeyInfo* hotkey = hotkey_list; hotkey->name != nullptr; hotkey++)
      ret.push_back(hotkey);
  }
  return ret;
}

void InputManager::AddHotkeyBindings(const SettingsInterface& si)
{
  for (const HotkeyInfo* hotkey_list : s_hotkey_list)
  {
    for (const HotkeyInfo* hotkey = hotkey_list; hotkey->name != nullptr; hotkey++)
    {
      const std::vector<std::string> bindings(si.GetStringList("Hotkeys", hotkey->name));
      if (bindings.empty())
        continue;

      AddBindings(bindings, InputButtonEventHandler{hotkey->handler});
    }
  }
}

void InputManager::AddPadBindings(const SettingsInterface& si, const std::string& section, u32 pad_index,
                                  const Controller::ControllerInfo& cinfo)
{
  bool vibration_binding_valid = false;
  PadVibrationBinding vibration_binding = {};
  vibration_binding.pad_index = pad_index;

  for (const Controller::ControllerBindingInfo& bi : cinfo.bindings)
  {
    const std::vector<std::string> bindings(si.GetStringList(section.c_str(), bi.name));

    switch (bi.type)
    {
      case InputBindingInfo::Type::Button:
      case InputBindingInfo::Type::HalfAxis:
      case InputBindingInfo::Type::Axis:
      {
        if (!bindings.empty())
        {
          const float sensitivity =
            si.GetFloatValue(section.c_str(), TinyString::from_format("{}Scale", bi.name), 1.0f);
          const float deadzone =
            si.GetFloatValue(section.c_str(), TinyString::from_format("{}Deadzone", bi.name), 0.0f);
          AddBindings(bindings, InputAxisEventHandler{[pad_index, bind_index = bi.bind_index, sensitivity,
                                                       deadzone](float value) {
                        if (!System::IsValid())
                          return;

                        Controller* c = System::GetController(pad_index);
                        if (c)
                          c->SetBindState(bind_index, ApplySingleBindingScale(sensitivity, deadzone, value));
                      }});
        }
      }
      break;

      case InputBindingInfo::Type::RelativePointer:
      {
        auto cb = [pad_index, base = bi.bind_index](InputBindingKey key, float value) {
          if (!System::IsValid())
            return;

          Controller* c = System::GetController(pad_index);
          if (c)
            c->SetBindState(base + key.data, value);
        };

        // bind pointer 0 by default
        if (bindings.empty())
        {
          s_pointer_move_callbacks.emplace_back(0, std::move(cb));
        }
        else
        {
          for (const std::string& binding : bindings)
          {
            const std::optional<u32> key(GetIndexFromPointerBinding(binding));
            if (!key.has_value())
              continue;

            s_pointer_move_callbacks.emplace_back(key.value(), cb);
          }
        }
      }
      break;

      case InputBindingInfo::Type::Motor:
      {
        DebugAssert(bi.bind_index < std::size(vibration_binding.motors));
        if (bindings.empty())
          continue;

        if (bindings.size() > 1)
          WARNING_LOG("More than one vibration motor binding for {}:{}", pad_index, bi.name);

        vibration_binding_valid |=
          ParseBindingAndGetSource(bindings.front(), &vibration_binding.motors[bi.bind_index].binding,
                                   &vibration_binding.motors[bi.bind_index].source);
      }
      break;

      case InputBindingInfo::Type::Pointer:
      case InputBindingInfo::Type::Device:
        // handled in device
        break;

      default:
        ERROR_LOG("Unhandled binding info type {}", static_cast<u32>(bi.type));
        break;
    }
  }

  if (vibration_binding_valid)
    s_pad_vibration_array.push_back(std::move(vibration_binding));

  for (u32 macro_button_index = 0; macro_button_index < NUM_MACRO_BUTTONS_PER_CONTROLLER; macro_button_index++)
  {
    const std::vector<std::string> bindings(
      si.GetStringList(section.c_str(), TinyString::from_format("Macro{}", macro_button_index + 1u).c_str()));
    if (!bindings.empty())
    {
      const float deadzone = si.GetFloatValue(
        section.c_str(), TinyString::from_format("Macro{}Deadzone", macro_button_index + 1).c_str(), 0.0f);
      for (const std::string& binding : bindings)
      {
        // We currently can't use chords with a deadzone.
        if (binding.find('&') != std::string::npos || deadzone == 0.0f)
        {
          if (deadzone != 0.0f)
            WARNING_LOG("Chord binding {} not supported with trigger deadzone {}.", binding, deadzone);

          AddBinding(binding, InputButtonEventHandler{[pad_index, macro_button_index](bool state) {
                       if (!System::IsValid())
                         return;

                       SetMacroButtonState(pad_index, macro_button_index, state);
                     }});
        }
        else
        {
          AddBindings(bindings, InputAxisEventHandler{[pad_index, macro_button_index, deadzone](float value) {
                        if (!System::IsValid())
                          return;

                        const bool state = (value > deadzone);
                        SetMacroButtonState(pad_index, macro_button_index, state);
                      }});
        }
      }
    }
  }
}

// ------------------------------------------------------------------------
// Event Handling
// ------------------------------------------------------------------------

bool InputManager::HasAnyBindingsForKey(InputBindingKey key)
{
  std::unique_lock lock(s_mutex);
  return (s_binding_map.find(key.MaskDirection()) != s_binding_map.end());
}

bool InputManager::HasAnyBindingsForSource(InputBindingKey key)
{
  std::unique_lock lock(s_mutex);
  for (const auto& it : s_binding_map)
  {
    const InputBindingKey& okey = it.first;
    if (okey.source_type == key.source_type && okey.source_index == key.source_index &&
        okey.source_subtype == key.source_subtype)
    {
      return true;
    }
  }

  return false;
}

bool InputManager::IsAxisHandler(const InputEventHandler& handler)
{
  return std::holds_alternative<InputAxisEventHandler>(handler);
}

bool InputManager::InvokeEvents(InputBindingKey key, float value, GenericInputBinding generic_key)
{
  if (DoEventHook(key, value))
    return true;

  // If imgui ate the event, don't fire our handlers.
  const bool skip_button_handlers = PreprocessEvent(key, value, generic_key);
  return ProcessEvent(key, value, skip_button_handlers);
}

bool InputManager::ProcessEvent(InputBindingKey key, float value, bool skip_button_handlers)
{
  // find all the bindings associated with this key
  const InputBindingKey masked_key = key.MaskDirection();
  const auto range = s_binding_map.equal_range(masked_key);
  if (range.first == s_binding_map.end())
    return false;

  // Now we can actually fire/activate bindings.
  u32 min_num_keys = 0;
  for (auto it = range.first; it != range.second; ++it)
  {
    InputBinding* binding = it->second.get();

    // find the key which matches us
    for (u32 i = 0; i < binding->num_keys; i++)
    {
      if (binding->keys[i].MaskDirection() != masked_key)
        continue;

      const u8 bit = static_cast<u8>(1) << i;
      const bool negative = binding->keys[i].modifier == InputModifier::Negate;
      const bool new_state = (negative ? (value < 0.0f) : (value > 0.0f));

      float value_to_pass = 0.0f;
      switch (binding->keys[i].modifier)
      {
        case InputModifier::None:
          if (value > 0.0f)
            value_to_pass = value;
          break;
        case InputModifier::Negate:
          if (value < 0.0f)
            value_to_pass = -value;
          break;
        case InputModifier::FullAxis:
          value_to_pass = value * 0.5f + 0.5f;
          break;
      }

      // handle inverting, needed for some wheels.
      value_to_pass = binding->keys[i].invert ? (1.0f - value_to_pass) : value_to_pass;

      // axes are fired regardless of a state change, unless they're zero
      // (but going from not-zero to zero will still fire, because of the full state)
      // for buttons, we can use the state of the last chord key, because it'll be 1 on press,
      // and 0 on release (when the full state changes).
      if (IsAxisHandler(binding->handler))
      {
        if (value_to_pass >= 0.0f && (!skip_button_handlers || value_to_pass == 0.0f))
          std::get<InputAxisEventHandler>(binding->handler)(value_to_pass);
      }
      else if (binding->num_keys >= min_num_keys)
      {
        // update state based on whether the whole chord was activated
        const u8 new_mask =
          ((new_state && !skip_button_handlers) ? (binding->current_mask | bit) : (binding->current_mask & ~bit));
        const bool prev_full_state = (binding->current_mask == binding->full_mask);
        const bool new_full_state = (new_mask == binding->full_mask);
        binding->current_mask = new_mask;

        // Workaround for multi-key bindings that share the same keys.
        if (binding->num_keys > 1 && new_full_state && prev_full_state != new_full_state && range.first != range.second)
        {
          // Because the binding map isn't ordered, we could iterate in the order of Shift+F1 and then
          // F1, which would mean that F1 wouldn't get cancelled and still activate. So, to handle this
          // case, we skip activating any future bindings with a fewer number of keys.
          min_num_keys = std::max<u32>(min_num_keys, binding->num_keys);

          // Basically, if we bind say, F1 and Shift+F1, and press shift and then F1, we'll fire bindings
          // for both F1 and Shift+F1, when we really only want to fire the binding for Shift+F1. So,
          // when we activate a multi-key chord (key press), we go through the binding map for all the
          // other keys in the chord, and cancel them if they have a shorter chord. If they're longer,
          // they could still activate and take precedence over us, so we leave them alone.
          for (u32 j = 0; j < binding->num_keys; j++)
          {
            const auto range2 = s_binding_map.equal_range(binding->keys[j].MaskDirection());
            for (auto it2 = range2.first; it2 != range2.second; ++it2)
            {
              InputBinding* other_binding = it2->second.get();
              if (other_binding == binding || IsAxisHandler(other_binding->handler) ||
                  other_binding->num_keys >= binding->num_keys)
              {
                continue;
              }

              // We only need to cancel the binding if it was fully active before. Which in the above
              // case of Shift+F1 / F1, it will be.
              if (other_binding->current_mask == other_binding->full_mask)
                std::get<InputButtonEventHandler>(other_binding->handler)(-1);

              // Zero out the current bits so that we don't release this binding, if the other part
              // of the chord releases first.
              other_binding->current_mask = 0;
            }
          }
        }

        if (prev_full_state != new_full_state && binding->num_keys >= min_num_keys)
        {
          const s32 pressed = skip_button_handlers ? -1 : static_cast<s32>(value_to_pass > 0.0f);
          std::get<InputButtonEventHandler>(binding->handler)(pressed);
        }
      }

      // bail out, since we shouldn't have the same key twice in the chord
      break;
    }
  }

  return true;
}

void InputManager::ClearBindStateFromSource(InputBindingKey key)
{
  // Why are we doing it this way? Because any of the bindings could cause a reload and invalidate our iterators :(.
  // Axis handlers should be fine, so we'll do those as a first pass.
  for (const auto& [match_key, binding] : s_binding_map)
  {
    if (key.source_type != match_key.source_type || key.source_subtype != match_key.source_subtype ||
        key.source_index != match_key.source_index || !IsAxisHandler(binding->handler))
    {
      continue;
    }

    for (u32 i = 0; i < binding->num_keys; i++)
    {
      if (binding->keys[i].MaskDirection() != match_key)
        continue;

      std::get<InputAxisEventHandler>(binding->handler)(0.0f);
      break;
    }
  }

  // Now go through the button handlers, and pick them off.
  bool matched;
  do
  {
    matched = false;

    for (const auto& [match_key, binding] : s_binding_map)
    {
      if (key.source_type != match_key.source_type || key.source_subtype != match_key.source_subtype ||
          key.source_index != match_key.source_index || IsAxisHandler(binding->handler))
      {
        continue;
      }

      for (u32 i = 0; i < binding->num_keys; i++)
      {
        if (binding->keys[i].MaskDirection() != match_key)
          continue;

        // Skip if we weren't pressed.
        const u8 bit = static_cast<u8>(1) << i;
        if ((binding->current_mask & bit) == 0)
          continue;

        // Only fire handler if we're changing from active state.
        const u8 current_mask = binding->current_mask;
        binding->current_mask &= ~bit;

        if (current_mask == binding->full_mask)
        {
          std::get<InputButtonEventHandler>(binding->handler)(-1);
          matched = true;
          break;
        }
      }

      // Need to start again, might've reloaded.
      if (matched)
        break;
    }
  } while (matched);
}

bool InputManager::PreprocessEvent(InputBindingKey key, float value, GenericInputBinding generic_key)
{
  // does imgui want the event?
  if (key.source_type == InputSourceType::Keyboard)
  {
    if (ImGuiManager::ProcessHostKeyEvent(key, value))
      return true;
  }
  else if (key.source_type == InputSourceType::Pointer && key.source_subtype == InputSubclass::PointerButton)
  {
    if (ImGuiManager::ProcessPointerButtonEvent(key, value))
      return true;
  }
  else if (generic_key != GenericInputBinding::Unknown)
  {
    if (ImGuiManager::ProcessGenericInputEvent(generic_key, value) && value != 0.0f)
      return true;
  }

  return false;
}

void InputManager::GenerateRelativeMouseEvents()
{
  const bool system_running = System::IsRunning();

  for (u32 device = 0; device < s_pointer_count; device++)
  {
    for (u32 axis = 0; axis < static_cast<u32>(static_cast<u8>(InputPointerAxis::Count)); axis++)
    {
      PointerAxisState& state = s_pointer_state[device][axis];
      const int deltai = state.delta.load(std::memory_order_acquire);
      state.delta.fetch_sub(deltai, std::memory_order_release);
      const float delta = static_cast<float>(deltai) / 65536.0f;
      const float unclamped_value = delta * s_pointer_axis_scale[axis];
      const float value = std::clamp(unclamped_value, -1.0f, 1.0f);

      const InputBindingKey key(MakePointerAxisKey(device, static_cast<InputPointerAxis>(axis)));
      if (device == 0 && axis >= static_cast<u32>(InputPointerAxis::WheelX) && delta != 0.0f &&
          ImGuiManager::ProcessPointerAxisEvent(key, delta))
      {
        continue;
      }

      // only generate axis-bound events when it hasn't changed
      if (value != state.last_value)
      {
        state.last_value = value;
        if (system_running)
          InvokeEvents(key, value, GenericInputBinding::Unknown);
      }

      // and pointer events only when it hasn't moved
      if (delta != 0.0f && system_running)
      {
        for (const std::pair<u32, PointerMoveCallback>& pmc : s_pointer_move_callbacks)
        {
          if (pmc.first == device)
            pmc.second(key, delta);
        }
      }
    }
  }
}

void InputManager::UpdatePointerCount()
{
  if (!IsUsingRawInput())
  {
    s_pointer_count = 1;
    return;
  }

#ifdef _WIN32
  InputSource* ris = GetInputSourceInterface(InputSourceType::RawInput);
  DebugAssert(ris);

  s_pointer_count = 0;
  for (const auto& [key, identifier, device_name] : ris->EnumerateDevices())
  {
    if (key.source_type == InputSourceType::Pointer)
      s_pointer_count++;
  }
#endif
}

u32 InputManager::GetPointerCount()
{
  return s_pointer_count;
}

std::pair<float, float> InputManager::GetPointerAbsolutePosition(u32 index)
{
  DebugAssert(index < s_host_pointer_positions.size());
  return std::make_pair(s_host_pointer_positions[index][static_cast<u8>(InputPointerAxis::X)],
                        s_host_pointer_positions[index][static_cast<u8>(InputPointerAxis::Y)]);
}

void InputManager::UpdatePointerAbsolutePosition(u32 index, float x, float y, bool raw_input)
{
  if (index >= MAX_POINTER_DEVICES || (s_relative_mouse_mode_active && !raw_input)) [[unlikely]]
    return;

  const float dx = x - std::exchange(s_host_pointer_positions[index][static_cast<u8>(InputPointerAxis::X)], x);
  const float dy = y - std::exchange(s_host_pointer_positions[index][static_cast<u8>(InputPointerAxis::Y)], y);

  if (dx != 0.0f)
  {
    s_pointer_state[index][static_cast<u8>(InputPointerAxis::X)].delta.fetch_add(static_cast<s32>(dx * 65536.0f),
                                                                                 std::memory_order_acq_rel);
  }
  if (dy != 0.0f)
  {
    s_pointer_state[index][static_cast<u8>(InputPointerAxis::Y)].delta.fetch_add(static_cast<s32>(dy * 65536.0f),
                                                                                 std::memory_order_acq_rel);
  }

  if (index == 0)
    ImGuiManager::UpdateMousePosition(x, y);
}

void InputManager::ResetPointerRelativeDelta(u32 index)
{
  if (index >= MAX_POINTER_DEVICES || s_relative_mouse_mode_active) [[unlikely]]
    return;

  s_pointer_state[index][static_cast<u8>(InputPointerAxis::X)].delta.store(0, std::memory_order_release);
  s_pointer_state[index][static_cast<u8>(InputPointerAxis::Y)].delta.store(0, std::memory_order_release);
}

void InputManager::UpdatePointerRelativeDelta(u32 index, InputPointerAxis axis, float d, bool raw_input)
{
  if (index >= MAX_POINTER_DEVICES || (axis < InputPointerAxis::WheelX && !s_relative_mouse_mode_active))
    return;

  s_host_pointer_positions[index][static_cast<u8>(axis)] += d;
  s_pointer_state[index][static_cast<u8>(axis)].delta.fetch_add(static_cast<s32>(d * 65536.0f),
                                                                std::memory_order_release);

  // We need to clamp the position ourselves in relative mode.
  if (axis <= InputPointerAxis::Y)
  {
    s_host_pointer_positions[index][static_cast<u8>(axis)] =
      std::clamp(s_host_pointer_positions[index][static_cast<u8>(axis)], 0.0f, s_window_size[static_cast<u8>(axis)]);

    // Imgui also needs to be updated, since the absolute position won't be set above.
    if (index == 0)
      ImGuiManager::UpdateMousePosition(s_host_pointer_positions[0][0], s_host_pointer_positions[0][1]);
  }
}

void InputManager::UpdateRelativeMouseMode()
{
  // Check for relative mode bindings, and enable if there's anything using it.
  // Raw input needs to force relative mode/clipping, because it's now disconnected from the system pointer.
  bool has_relative_mode_bindings = !s_pointer_move_callbacks.empty() || IsUsingRawInput();
  if (!has_relative_mode_bindings)
  {
    for (const auto& it : s_binding_map)
    {
      const InputBindingKey& key = it.first;
      if (key.source_type == InputSourceType::Pointer && key.source_subtype == InputSubclass::PointerAxis &&
          key.data >= static_cast<u32>(InputPointerAxis::X) && key.data <= static_cast<u32>(InputPointerAxis::Y))
      {
        has_relative_mode_bindings = true;
        break;
      }
    }
  }

  const bool hide_mouse_cursor = has_relative_mode_bindings || ImGuiManager::HasSoftwareCursor(0);
  if (s_relative_mouse_mode == has_relative_mode_bindings && s_hide_host_mouse_cursor == hide_mouse_cursor)
    return;

#ifndef __ANDROID__
  s_relative_mouse_mode = has_relative_mode_bindings;
  s_hide_host_mouse_cursor = hide_mouse_cursor;
#endif

  UpdateHostMouseMode();
}

void InputManager::UpdateHostMouseMode()
{
  const bool can_change = System::IsRunning();
  const bool wanted_relative_mouse_mode = (s_relative_mouse_mode && can_change);
  const bool wanted_hide_host_mouse_cursor = (s_hide_host_mouse_cursor && can_change);
  if (wanted_relative_mouse_mode == s_relative_mouse_mode_active &&
      wanted_hide_host_mouse_cursor == s_hide_host_mouse_cusor_active)
  {
    return;
  }

  s_relative_mouse_mode_active = wanted_relative_mouse_mode;
  s_hide_host_mouse_cusor_active = wanted_hide_host_mouse_cursor;
  Host::SetMouseMode(wanted_relative_mouse_mode, wanted_hide_host_mouse_cursor);
}

bool InputManager::IsRelativeMouseModeActive()
{
  return s_relative_mouse_mode_active;
}

bool InputManager::IsUsingRawInput()
{
#if defined(_WIN32)
  return static_cast<bool>(s_input_sources[static_cast<u32>(InputSourceType::RawInput)]);
#else
  return false;
#endif
}

void InputManager::SetDisplayWindowSize(float width, float height)
{
  s_window_size[0] = width;
  s_window_size[1] = height;
}

std::pair<float, float> InputManager::GetDisplayWindowSize()
{
  return std::make_pair(s_window_size[0], s_window_size[1]);
}

void InputManager::SetDefaultSourceConfig(SettingsInterface& si)
{
  si.ClearSection("InputSources");
  si.SetBoolValue("InputSources", "SDL", true);
  si.SetBoolValue("InputSources", "SDLControllerEnhancedMode", false);
  si.SetBoolValue("InputSources", "SDLPS5PlayerLED", false);
  si.SetBoolValue("InputSources", "XInput", false);
  si.SetBoolValue("InputSources", "RawInput", false);
}

void InputManager::ClearPortBindings(SettingsInterface& si, u32 port)
{
  const std::string section = Controller::GetSettingsSection(port);
  const TinyString type = si.GetTinyStringValue(
    section.c_str(), "Type", Controller::GetControllerInfo(Settings::GetDefaultControllerType(port)).name);

  const Controller::ControllerInfo* info = Controller::GetControllerInfo(type);
  if (!info)
    return;

  for (const Controller::ControllerBindingInfo& bi : info->bindings)
    si.DeleteValue(section.c_str(), bi.name);
}

void InputManager::CopyConfiguration(SettingsInterface* dest_si, const SettingsInterface& src_si,
                                     bool copy_pad_config /*= true*/, bool copy_source_config /*= true*/,
                                     bool copy_pad_bindings /*= true*/, bool copy_hotkey_bindings /*= true*/)
{
  if (copy_pad_config)
    dest_si->CopyStringValue(src_si, "ControllerPorts", "MultitapMode");

  if (copy_source_config)
  {
    for (u32 type = 0; type < static_cast<u32>(InputSourceType::Count); type++)
    {
      dest_si->CopyBoolValue(src_si, "InputSources",
                             InputManager::InputSourceToString(static_cast<InputSourceType>(type)));
    }

#ifdef ENABLE_SDL
    // I hate this, but there isn't a better location for it...
    if (dest_si->GetBoolValue("InputSources", "SDL"))
      InputSource::CopySDLSourceSettings(dest_si, src_si);
#endif
  }

  for (u32 port = 0; port < NUM_CONTROLLER_AND_CARD_PORTS; port++)
  {
    if (Controller::PadIsMultitapSlot(port))
    {
      const auto [mt_port, mt_slot] = Controller::ConvertPadToPortAndSlot(port);
      if (!g_settings.IsMultitapPortEnabled(mt_port))
        continue;
    }

    const std::string section(Controller::GetSettingsSection(port));
    const TinyString type = src_si.GetTinyStringValue(
      section.c_str(), "Type", Controller::GetControllerInfo(Settings::GetDefaultControllerType(port)).name);
    if (copy_pad_config)
      dest_si->SetStringValue(section.c_str(), "Type", type.c_str());

    const Controller::ControllerInfo* info = Controller::GetControllerInfo(type);
    if (!info)
      return;

    if (copy_pad_bindings)
    {
      for (const Controller::ControllerBindingInfo& bi : info->bindings)
      {
        dest_si->CopyStringListValue(src_si, section.c_str(), bi.name);
        dest_si->CopyFloatValue(src_si, section.c_str(), TinyString::from_format("{}Scale", bi.name));
        dest_si->CopyFloatValue(src_si, section.c_str(), TinyString::from_format("{}Deadzone", bi.name));
      }

      for (u32 i = 0; i < NUM_MACRO_BUTTONS_PER_CONTROLLER; i++)
      {
        dest_si->CopyStringListValue(src_si, section.c_str(), TinyString::from_format("Macro{}", i + 1));
        dest_si->CopyStringValue(src_si, section.c_str(), TinyString::from_format("Macro{}Binds", i + 1));
        dest_si->CopyFloatValue(src_si, section.c_str(), TinyString::from_format("Macro{}Deadzone", i + 1));
        dest_si->CopyFloatValue(src_si, section.c_str(), TinyString::from_format("Macro{}Pressure", i + 1));
        dest_si->CopyUIntValue(src_si, section.c_str(), TinyString::from_format("Macro{}Frequency", i + 1));
        dest_si->CopyBoolValue(src_si, section.c_str(), TinyString::from_format("Macro{}Toggle", i + 1));
      }
    }

    if (copy_pad_config)
    {
      for (const SettingInfo& csi : info->settings)
      {
        switch (csi.type)
        {
          case SettingInfo::Type::Boolean:
            dest_si->CopyBoolValue(src_si, section.c_str(), csi.name);
            break;
          case SettingInfo::Type::Integer:
          case SettingInfo::Type::IntegerList:
            dest_si->CopyIntValue(src_si, section.c_str(), csi.name);
            break;
          case SettingInfo::Type::Float:
            dest_si->CopyFloatValue(src_si, section.c_str(), csi.name);
            break;
          case SettingInfo::Type::String:
          case SettingInfo::Type::Path:
            dest_si->CopyStringValue(src_si, section.c_str(), csi.name);
            break;
          default:
            break;
        }
      }
    }
  }

  if (copy_hotkey_bindings)
  {
    std::vector<const HotkeyInfo*> hotkeys(InputManager::GetHotkeyList());
    for (const HotkeyInfo* hki : hotkeys)
      dest_si->CopyStringListValue(src_si, "Hotkeys", hki->name);
  }
}

static u32 TryMapGenericMapping(SettingsInterface& si, const std::string& section,
                                const GenericInputBindingMapping& mapping, GenericInputBinding generic_name,
                                const char* bind_name, bool clear_existing_mappings)
{
  // find the mapping it corresponds to
  const std::string* found_mapping = nullptr;
  for (const std::pair<GenericInputBinding, std::string>& it : mapping)
  {
    if (it.first == generic_name)
    {
      found_mapping = &it.second;
      break;
    }
  }

  if (found_mapping)
  {
    INFO_LOG("Map {}/{} to '{}'", section, bind_name, *found_mapping);
    if (clear_existing_mappings)
      si.SetStringValue(section.c_str(), bind_name, found_mapping->c_str());
    else
      si.AddToStringList(section.c_str(), bind_name, found_mapping->c_str());

    return 1;
  }
  else
  {
    if (clear_existing_mappings)
      si.DeleteValue(section.c_str(), bind_name);

    return 0;
  }
}

bool InputManager::MapController(SettingsInterface& si, u32 controller,
                                 const std::vector<std::pair<GenericInputBinding, std::string>>& mapping,
                                 bool clear_existing_mappings)
{
  const std::string section = Controller::GetSettingsSection(controller);
  const TinyString type = si.GetTinyStringValue(
    section.c_str(), "Type", Controller::GetControllerInfo(Settings::GetDefaultControllerType(controller)).name);
  const Controller::ControllerInfo* info = Controller::GetControllerInfo(type);
  if (!info)
    return false;

  u32 num_mappings = 0;
  for (const Controller::ControllerBindingInfo& bi : info->bindings)
  {
    if (bi.generic_mapping == GenericInputBinding::Unknown)
      continue;

    u32 mappings_added =
      TryMapGenericMapping(si, section, mapping, bi.generic_mapping, bi.name, clear_existing_mappings);

    // try to map to small motor if we tried big motor
    if (mappings_added == 0 && bi.generic_mapping == GenericInputBinding::LargeMotor)
    {
      mappings_added +=
        TryMapGenericMapping(si, section, mapping, GenericInputBinding::SmallMotor, bi.name, clear_existing_mappings);
    }

    num_mappings += mappings_added;
  }

  return (num_mappings > 0);
}

std::string InputManager::GetPhysicalDeviceForController(SettingsInterface& si, u32 controller)
{
  std::string ret;

  const std::string section = Controller::GetSettingsSection(controller);
  const TinyString type = si.GetTinyStringValue(
    section.c_str(), "Type", Controller::GetControllerInfo(Settings::GetDefaultControllerType(controller)).name);
  const Controller::ControllerInfo* info = Controller::GetControllerInfo(type);
  if (info)
  {
    for (const Controller::ControllerBindingInfo& bi : info->bindings)
    {
      for (const std::string& binding : si.GetStringList(section.c_str(), bi.name))
      {
        std::string_view source, sub_binding;
        if (!SplitBinding(binding, &source, &sub_binding))
          continue;

        if (ret.empty())
        {
          ret = source;
          continue;
        }

        if (ret != source)
        {
          ret = TRANSLATE_STR("InputManager", "Multiple Devices");
          return ret;
        }
      }
    }
  }

  if (ret.empty())
    ret = TRANSLATE_STR("InputManager", "None");

  return ret;
}

std::vector<std::string> InputManager::GetInputProfileNames()
{
  FileSystem::FindResultsArray results;
  FileSystem::FindFiles(EmuFolders::InputProfiles.c_str(), "*.ini",
                        FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES | FILESYSTEM_FIND_RELATIVE_PATHS |
                          FILESYSTEM_FIND_SORT_BY_NAME,
                        &results);

  std::vector<std::string> ret;
  ret.reserve(results.size());
  for (FILESYSTEM_FIND_DATA& fd : results)
    ret.emplace_back(Path::GetFileTitle(fd.FileName));

  return ret;
}

void InputManager::OnInputDeviceConnected(InputBindingKey key, std::string_view identifier,
                                          std::string_view device_name)
{
  INFO_LOG("Device '{}' connected: '{}'", identifier, device_name);
  Host::OnInputDeviceConnected(key, identifier, device_name);
}

void InputManager::OnInputDeviceDisconnected(InputBindingKey key, std::string_view identifier)
{
  INFO_LOG("Device '{}' disconnected", identifier);
  Host::OnInputDeviceDisconnected(key, identifier);
}

std::unique_ptr<ForceFeedbackDevice> InputManager::CreateForceFeedbackDevice(const std::string_view device,
                                                                             Error* error)
{
  for (u32 i = FIRST_EXTERNAL_INPUT_SOURCE; i < LAST_EXTERNAL_INPUT_SOURCE; i++)
  {
    if (s_input_sources[i] && s_input_sources[i]->ContainsDevice(device))
      return s_input_sources[i]->CreateForceFeedbackDevice(device, error);
  }

  Error::SetStringFmt(error, "No input source matched device '{}'", device);
  return {};
}

// ------------------------------------------------------------------------
// Vibration
// ------------------------------------------------------------------------

void InputManager::SetPadVibrationIntensity(u32 pad_index, float large_or_single_motor_intensity,
                                            float small_motor_intensity)
{
  for (PadVibrationBinding& pad : s_pad_vibration_array)
  {
    if (pad.pad_index != pad_index)
      continue;

    PadVibrationBinding::Motor& large_motor = pad.motors[0];
    PadVibrationBinding::Motor& small_motor = pad.motors[1];
    if (large_motor.last_intensity == large_or_single_motor_intensity &&
        small_motor.last_intensity == small_motor_intensity)
      continue;

    if (pad.AreMotorsCombined())
    {
      // if the motors are combined, we need to adjust to the maximum of both
      const float report_intensity = std::max(large_or_single_motor_intensity, small_motor_intensity);
      if (large_motor.source)
      {
        large_motor.last_update_time = Timer::GetCurrentValue();
        large_motor.source->UpdateMotorState(large_motor.binding, report_intensity);
      }
    }
    else if (large_motor.source == small_motor.source)
    {
      // both motors are bound to the same source, do an optimal update
      large_motor.last_update_time = Timer::GetCurrentValue();
      large_motor.source->UpdateMotorState(large_motor.binding, small_motor.binding, large_or_single_motor_intensity,
                                           small_motor_intensity);
    }
    else
    {
      // update motors independently
      if (large_motor.source && large_motor.last_intensity != large_or_single_motor_intensity)
      {
        large_motor.last_update_time = Timer::GetCurrentValue();
        large_motor.source->UpdateMotorState(large_motor.binding, large_or_single_motor_intensity);
      }
      if (small_motor.source && small_motor.last_intensity != small_motor_intensity)
      {
        small_motor.last_update_time = Timer::GetCurrentValue();
        small_motor.source->UpdateMotorState(small_motor.binding, small_motor_intensity);
      }
    }

    large_motor.last_intensity = large_or_single_motor_intensity;
    small_motor.last_intensity = small_motor_intensity;
  }
}

void InputManager::PauseVibration()
{
  for (PadVibrationBinding& binding : s_pad_vibration_array)
  {
    for (u32 motor_index = 0; motor_index < MAX_MOTORS_PER_PAD; motor_index++)
    {
      PadVibrationBinding::Motor& motor = binding.motors[motor_index];
      if (!motor.source || motor.last_intensity == 0.0f)
        continue;

      // we deliberately don't zero the intensity here, so it can resume later
      motor.last_update_time = 0;
      motor.source->UpdateMotorState(motor.binding, 0.0f);
    }
  }
}

void InputManager::UpdateContinuedVibration()
{
  // update vibration intensities, so if the game does a long effect, it continues
  const u64 current_time = Timer::GetCurrentValue();
  for (PadVibrationBinding& pad : s_pad_vibration_array)
  {
    if (pad.AreMotorsCombined())
    {
      // motors are combined
      PadVibrationBinding::Motor& large_motor = pad.motors[0];
      if (!large_motor.source)
        continue;

      // so only check the first one
      const double dt = Timer::ConvertValueToSeconds(current_time - large_motor.last_update_time);
      if (dt < VIBRATION_UPDATE_INTERVAL_SECONDS)
        continue;

      // but take max of both motors for the intensity
      const float intensity = pad.GetCombinedIntensity();
      if (intensity == 0.0f)
        continue;

      large_motor.last_update_time = current_time;
      large_motor.source->UpdateMotorState(large_motor.binding, intensity);
    }
    else
    {
      // independent motor control
      for (u32 i = 0; i < MAX_MOTORS_PER_PAD; i++)
      {
        PadVibrationBinding::Motor& motor = pad.motors[i];
        if (!motor.source || motor.last_intensity == 0.0f)
          continue;

        const double dt = Timer::ConvertValueToSeconds(current_time - motor.last_update_time);
        if (dt < VIBRATION_UPDATE_INTERVAL_SECONDS)
          continue;

        // re-notify the source of the continued effect
        motor.last_update_time = current_time;
        motor.source->UpdateMotorState(motor.binding, motor.last_intensity);
      }
    }
  }
}

// ------------------------------------------------------------------------
// Macros
// ------------------------------------------------------------------------

void InputManager::LoadMacroButtonConfig(const SettingsInterface& si, const std::string& section, u32 pad,
                                         const Controller::ControllerInfo& cinfo)
{
  s_macro_buttons[pad] = {};
  if (cinfo.bindings.empty())
    return;

  for (u32 i = 0; i < NUM_MACRO_BUTTONS_PER_CONTROLLER; i++)
  {
    std::string binds_string;
    if (!si.GetStringValue(section.c_str(), TinyString::from_format("Macro{}Binds", i + 1u), &binds_string))
      continue;

    const u32 frequency =
      std::min<u32>(si.GetUIntValue(section.c_str(), TinyString::from_format("Macro{}Frequency", i + 1u), 0u),
                    std::numeric_limits<u16>::max());
    const u8 pressure = static_cast<u8>(
      std::clamp(si.GetFloatValue(section.c_str(), TinyString::from_format("Macro{}Pressure", i + 1u), 1.0f), 0.0f,
                 1.0f) *
      255.0f);
    const bool toggle = si.GetBoolValue(section.c_str(), TinyString::from_format("Macro{}Toggle", i + 1u), false);

    // convert binds
    std::vector<u32> bind_indices;
    std::vector<std::string_view> buttons_split(StringUtil::SplitString(binds_string, '&', true));
    if (buttons_split.empty())
      continue;
    for (const std::string_view& button : buttons_split)
    {
      const Controller::ControllerBindingInfo* binding = nullptr;
      for (const Controller::ControllerBindingInfo& bi : cinfo.bindings)
      {
        if (button == bi.name)
        {
          binding = &bi;
          break;
        }
      }
      if (!binding)
      {
        DEV_LOG("Invalid bind '{}' in macro button {} for pad {}", button, pad, i);
        continue;
      }

      bind_indices.push_back(binding->bind_index);
    }
    if (bind_indices.empty())
      continue;

    MacroButton& macro = s_macro_buttons[pad][i];
    macro.buttons = std::move(bind_indices);
    macro.toggle_frequency = static_cast<u16>(frequency);
    macro.trigger_toggle = toggle;
    macro.trigger_pressure = pressure;
  }
}

void InputManager::SetMacroButtonState(u32 pad, u32 index, bool state)
{
  if (pad >= NUM_CONTROLLER_AND_CARD_PORTS || index >= NUM_MACRO_BUTTONS_PER_CONTROLLER)
    return;

  MacroButton& mb = s_macro_buttons[pad][index];
  if (mb.buttons.empty())
    return;

  const bool trigger_state = (mb.trigger_toggle ? (state ? !mb.trigger_state : mb.trigger_state) : state);
  if (mb.trigger_state == trigger_state)
    return;

  mb.toggle_counter = mb.toggle_frequency;
  mb.trigger_state = trigger_state;
  if (mb.toggle_state != trigger_state)
  {
    mb.toggle_state = trigger_state;
    ApplyMacroButton(pad, mb);
  }
}

void InputManager::ApplyMacroButton(u32 pad, const MacroButton& mb)
{
  Controller* const controller = System::GetController(pad);
  if (!controller)
    return;

  const float value = static_cast<float>(mb.toggle_state ? mb.trigger_pressure : 0) * (1.0f / 255.0f);
  for (const u32 btn : mb.buttons)
    controller->SetBindState(btn, value);
}

void InputManager::UpdateMacroButtons()
{
  for (u32 pad = 0; pad < NUM_CONTROLLER_AND_CARD_PORTS; pad++)
  {
    for (u32 index = 0; index < NUM_MACRO_BUTTONS_PER_CONTROLLER; index++)
    {
      MacroButton& mb = s_macro_buttons[pad][index];
      if (!mb.trigger_state || mb.toggle_frequency == 0)
        continue;

      mb.toggle_counter--;
      if (mb.toggle_counter > 0)
        continue;

      mb.toggle_counter = mb.toggle_frequency;
      mb.toggle_state = !mb.toggle_state;
      ApplyMacroButton(pad, mb);
    }
  }
}

// ------------------------------------------------------------------------
// Hooks/Event Intercepting
// ------------------------------------------------------------------------

void InputManager::SetHook(InputInterceptHook::Callback callback)
{
  std::unique_lock lock(s_mutex);
  DebugAssert(!m_event_intercept_callback);
  m_event_intercept_callback = std::move(callback);
}

void InputManager::RemoveHook()
{
  std::unique_lock lock(s_mutex);
  if (m_event_intercept_callback)
    m_event_intercept_callback = {};
}

bool InputManager::HasHook()
{
  std::unique_lock lock(s_mutex);
  return (bool)m_event_intercept_callback;
}

bool InputManager::DoEventHook(InputBindingKey key, float value)
{
  std::unique_lock lock(s_mutex);
  if (!m_event_intercept_callback)
    return false;

  const InputInterceptHook::CallbackResult action = m_event_intercept_callback(key, value);
  if (action >= InputInterceptHook::CallbackResult::RemoveHookAndStopProcessingEvent)
    m_event_intercept_callback = {};

  return (action == InputInterceptHook::CallbackResult::RemoveHookAndStopProcessingEvent ||
          action == InputInterceptHook::CallbackResult::StopProcessingEvent);
}

// ------------------------------------------------------------------------
// Binding Updater
// ------------------------------------------------------------------------

void InputManager::ReloadBindings(const SettingsInterface& binding_si, const SettingsInterface& hotkey_binding_si)
{
  PauseVibration();

  std::unique_lock lock(s_mutex);

  s_binding_map.clear();
  s_pad_vibration_array.clear();
  s_pointer_move_callbacks.clear();

  Host::AddFixedInputBindings(binding_si);

  // Hotkeys use the base configuration, except if the custom hotkeys option is enabled.
  AddHotkeyBindings(hotkey_binding_si);

  // If there's an input profile, we load pad bindings from it alone, rather than
  // falling back to the base configuration.
  for (u32 pad = 0; pad < NUM_CONTROLLER_AND_CARD_PORTS; pad++)
  {
    if (g_settings.controller_types[pad] == ControllerType::None)
      continue;

    const Controller::ControllerInfo& cinfo = Controller::GetControllerInfo(g_settings.controller_types[pad]);
    const std::string section(Controller::GetSettingsSection(pad));
    AddPadBindings(binding_si, section, pad, cinfo);
    LoadMacroButtonConfig(binding_si, section, pad, cinfo);
  }

  for (u32 axis = 0; axis < static_cast<u32>(InputPointerAxis::Count); axis++)
  {
    // From lilypad: 1 mouse pixel = 1/8th way down.
    const float default_scale = (axis <= static_cast<u32>(InputPointerAxis::Y)) ? 8.0f : 1.0f;
    s_pointer_axis_scale[axis] =
      1.0f / std::max(binding_si.GetFloatValue(
                        "ControllerPorts",
                        TinyString::from_format("Pointer{}Scale", s_pointer_axis_names[axis]).c_str(), default_scale),
                      1.0f);
  }

  UpdateRelativeMouseMode();
}

// ------------------------------------------------------------------------
// Source Management
// ------------------------------------------------------------------------

bool InputManager::ReloadDevices()
{
  std::unique_lock lock(s_mutex);

  bool changed = false;

  for (u32 i = FIRST_EXTERNAL_INPUT_SOURCE; i < LAST_EXTERNAL_INPUT_SOURCE; i++)
  {
    if (s_input_sources[i])
      changed |= s_input_sources[i]->ReloadDevices();
  }

  UpdatePointerCount();

  return changed;
}

void InputManager::CloseSources()
{
  std::unique_lock lock(s_mutex);

  for (u32 i = FIRST_EXTERNAL_INPUT_SOURCE; i < LAST_EXTERNAL_INPUT_SOURCE; i++)
  {
    if (s_input_sources[i])
    {
      s_input_sources[i]->Shutdown();
      s_input_sources[i].reset();
    }
  }
}

void InputManager::PollSources()
{
  for (u32 i = FIRST_EXTERNAL_INPUT_SOURCE; i < LAST_EXTERNAL_INPUT_SOURCE; i++)
  {
    if (s_input_sources[i])
      s_input_sources[i]->PollEvents();
  }

  GenerateRelativeMouseEvents();

  if (System::GetState() == System::State::Running)
  {
    UpdateMacroButtons();
    if (!s_pad_vibration_array.empty())
      UpdateContinuedVibration();
  }
}

InputManager::DeviceList InputManager::EnumerateDevices()
{
  std::unique_lock lock(s_mutex);

  DeviceList ret;

  InputBindingKey keyboard_key = {};
  keyboard_key.source_type = InputSourceType::Keyboard;
  InputBindingKey mouse_key = {};
  mouse_key.source_type = InputSourceType::Pointer;

  ret.emplace_back(keyboard_key, "Keyboard", TRANSLATE_STR("InputManager", "Keyboard"));
  ret.emplace_back(mouse_key, GetPointerDeviceName(0), TRANSLATE_STR("InputManager", "Mouse"));

  for (u32 i = FIRST_EXTERNAL_INPUT_SOURCE; i < LAST_EXTERNAL_INPUT_SOURCE; i++)
  {
    if (s_input_sources[i])
    {
      DeviceList devs = s_input_sources[i]->EnumerateDevices();
      if (ret.empty())
        ret = std::move(devs);
      else
        std::move(devs.begin(), devs.end(), std::back_inserter(ret));
    }
  }

  return ret;
}

InputManager::VibrationMotorList InputManager::EnumerateVibrationMotors(std::optional<InputBindingKey> for_device)
{
  std::unique_lock lock(s_mutex);

  VibrationMotorList ret;

  for (u32 i = FIRST_EXTERNAL_INPUT_SOURCE; i < LAST_EXTERNAL_INPUT_SOURCE; i++)
  {
    if (s_input_sources[i])
    {
      VibrationMotorList devs = s_input_sources[i]->EnumerateVibrationMotors(for_device);
      if (ret.empty())
        ret = std::move(devs);
      else
        std::move(devs.begin(), devs.end(), std::back_inserter(ret));
    }
  }

  return ret;
}

static void GetKeyboardGenericBindingMapping(std::vector<std::pair<GenericInputBinding, std::string>>* mapping)
{
  mapping->emplace_back(GenericInputBinding::DPadUp, "Keyboard/Up");
  mapping->emplace_back(GenericInputBinding::DPadRight, "Keyboard/Right");
  mapping->emplace_back(GenericInputBinding::DPadDown, "Keyboard/Down");
  mapping->emplace_back(GenericInputBinding::DPadLeft, "Keyboard/Left");
  mapping->emplace_back(GenericInputBinding::LeftStickUp, "Keyboard/W");
  mapping->emplace_back(GenericInputBinding::LeftStickRight, "Keyboard/D");
  mapping->emplace_back(GenericInputBinding::LeftStickDown, "Keyboard/S");
  mapping->emplace_back(GenericInputBinding::LeftStickLeft, "Keyboard/A");
  mapping->emplace_back(GenericInputBinding::RightStickUp, "Keyboard/T");
  mapping->emplace_back(GenericInputBinding::RightStickRight, "Keyboard/H");
  mapping->emplace_back(GenericInputBinding::RightStickDown, "Keyboard/G");
  mapping->emplace_back(GenericInputBinding::RightStickLeft, "Keyboard/F");
  mapping->emplace_back(GenericInputBinding::Start, "Keyboard/Return");
  mapping->emplace_back(GenericInputBinding::Select, "Keyboard/Backspace");
  mapping->emplace_back(GenericInputBinding::Triangle, "Keyboard/I");
  mapping->emplace_back(GenericInputBinding::Circle, "Keyboard/L");
  mapping->emplace_back(GenericInputBinding::Cross, "Keyboard/K");
  mapping->emplace_back(GenericInputBinding::Square, "Keyboard/J");
  mapping->emplace_back(GenericInputBinding::L1, "Keyboard/Q");
  mapping->emplace_back(GenericInputBinding::L2, "Keyboard/1");
  mapping->emplace_back(GenericInputBinding::L3, "Keyboard/2");
  mapping->emplace_back(GenericInputBinding::R1, "Keyboard/E");
  mapping->emplace_back(GenericInputBinding::R2, "Keyboard/3");
  mapping->emplace_back(GenericInputBinding::R3, "Keyboard/4");
}

static bool GetInternalGenericBindingMapping(std::string_view device, GenericInputBindingMapping* mapping)
{
  if (device == "Keyboard")
  {
    GetKeyboardGenericBindingMapping(mapping);
    return true;
  }

  return false;
}

GenericInputBindingMapping InputManager::GetGenericBindingMapping(std::string_view device)
{
  GenericInputBindingMapping mapping;

  if (!GetInternalGenericBindingMapping(device, &mapping))
  {
    for (u32 i = FIRST_EXTERNAL_INPUT_SOURCE; i < LAST_EXTERNAL_INPUT_SOURCE; i++)
    {
      if (s_input_sources[i] && s_input_sources[i]->GetGenericBindingMapping(device, &mapping))
        break;
    }
  }

  return mapping;
}

bool InputManager::IsInputSourceEnabled(const SettingsInterface& si, InputSourceType type)
{
#ifdef __ANDROID__
  // Force Android source to always be enabled so nobody accidentally breaks it via ini.
  if (type == InputSourceType::Android)
    return true;
#endif

  return si.GetBoolValue("InputSources", InputSourceToString(type), GetInputSourceDefaultEnabled(type));
}

void InputManager::UpdateInputSourceState(const SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock,
                                          InputSourceType type, std::unique_ptr<InputSource> (*factory_function)())
{
  const bool enabled = IsInputSourceEnabled(si, type);
  std::unique_ptr<InputSource>& source = s_input_sources[static_cast<u32>(type)];
  if (enabled)
  {
    if (source)
    {
      source->UpdateSettings(si, settings_lock);
    }
    else
    {
      source = factory_function();
      if (!source || !source->Initialize(si, settings_lock))
      {
        ERROR_LOG("Source '{}' failed to initialize.", InputSourceToString(type));
        if (source)
          source->Shutdown();
        source.reset();
        return;
      }
    }
  }
  else
  {
    if (source)
    {
      source->Shutdown();
      source.reset();
    }
  }
}

void InputManager::ReloadSources(const SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
  std::unique_lock lock(s_mutex);

#ifdef _WIN32
  UpdateInputSourceState(si, settings_lock, InputSourceType::DInput, &InputSource::CreateDInputSource);
  UpdateInputSourceState(si, settings_lock, InputSourceType::XInput, &InputSource::CreateXInputSource);
  UpdateInputSourceState(si, settings_lock, InputSourceType::RawInput, &InputSource::CreateWin32RawInputSource);
#endif
#ifdef ENABLE_SDL
  UpdateInputSourceState(si, settings_lock, InputSourceType::SDL, &InputSource::CreateSDLSource);
#endif
#ifdef __ANDROID__
  UpdateInputSourceState(si, settings_lock, InputSourceType::Android, &InputSource::CreateAndroidSource);
#endif

  UpdatePointerCount();
}

ForceFeedbackDevice::~ForceFeedbackDevice()
{
}
