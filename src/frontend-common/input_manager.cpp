#include "input_manager.h"
#include "common/assert.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"
#include "common/timer.h"
#include "core/controller.h"
#include "core/host.h"
#include "core/system.h"
#include "imgui_manager.h"
#include "input_source.h"

#include "fmt/core.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <variant>
#include <vector>

Log_SetChannel(InputManager);

// ------------------------------------------------------------------------
// Constants
// ------------------------------------------------------------------------

enum : u32
{
  MAX_KEYS_PER_BINDING = 4,
  MAX_MOTORS_PER_PAD = 2,
  FIRST_EXTERNAL_INPUT_SOURCE = static_cast<u32>(InputSourceType::Pointer) + 1u,
  LAST_EXTERNAL_INPUT_SOURCE = static_cast<u32>(InputSourceType::Count),
};

// ------------------------------------------------------------------------
// Event Handler Type
// ------------------------------------------------------------------------
// This class acts as an adapter to convert from normalized values to
// binary values when the callback is a binary/button handler. That way
// you don't need to convert float->bool in your callbacks.
using InputEventHandler = std::variant<InputAxisEventHandler, InputButtonEventHandler>;

// ------------------------------------------------------------------------
// Binding Type
// ------------------------------------------------------------------------
// This class tracks both the keys which make it up (for chords), as well
// as the state of all buttons. For button callbacks, it's fired when
// all keys go active, and for axis callbacks, when all are active and
// the value changes.

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
    u64 last_update_time;
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
  u32 toggle_frequency;     ///< Interval at which the buttons will be toggled, if not 0.
  u32 toggle_counter;       ///< When this counter reaches zero, buttons will be toggled.
  bool toggle_state;        ///< Current state for turbo.
  bool trigger_state;       ///< Whether the macro button is active.
};

// ------------------------------------------------------------------------
// Forward Declarations (for static qualifier)
// ------------------------------------------------------------------------
namespace InputManager {
static std::optional<InputBindingKey> ParseHostKeyboardKey(const std::string_view& source,
                                                           const std::string_view& sub_binding);
static std::optional<InputBindingKey> ParsePointerKey(const std::string_view& source,
                                                      const std::string_view& sub_binding);

static std::vector<std::string_view> SplitChord(const std::string_view& binding);
static bool SplitBinding(const std::string_view& binding, std::string_view* source, std::string_view* sub_binding);
static void AddBindings(const std::vector<std::string>& bindings, const InputEventHandler& handler);
static bool ParseBindingAndGetSource(const std::string_view& binding, InputBindingKey* key, InputSource** source);

static bool IsAxisHandler(const InputEventHandler& handler);

static void AddHotkeyBindings(SettingsInterface& si);
static void AddPadBindings(SettingsInterface& si, const std::string& section, u32 pad,
                           const Controller::ControllerInfo* cinfo);
static void UpdateContinuedVibration();
static void GenerateRelativeMouseEvents();

static bool DoEventHook(InputBindingKey key, float value);
static bool PreprocessEvent(InputBindingKey key, float value, GenericInputBinding generic_key);

static void LoadMacroButtonConfig(SettingsInterface& si, const std::string& section, u32 pad,
                                  const Controller::ControllerInfo* cinfo);
static void SetMacroButtonState(u32 pad, u32 index, bool state);
static void ApplyMacroButton(u32 pad, const MacroButton& mb);
static void UpdateMacroButtons();
} // namespace InputManager

// ------------------------------------------------------------------------
// Local Variables
// ------------------------------------------------------------------------

// This is a multimap containing any binds related to the specified key.
using BindingMap = std::unordered_multimap<InputBindingKey, std::shared_ptr<InputBinding>, InputBindingKeyHash>;
using VibrationBindingArray = std::vector<PadVibrationBinding>;
static BindingMap s_binding_map;
static VibrationBindingArray s_pad_vibration_array;
static std::mutex s_binding_map_write_lock;

// Hooks/intercepting (for setting bindings)
static std::mutex m_event_intercept_mutex;
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
static std::array<float, static_cast<u8>(InputPointerAxis::Count)> s_pointer_axis_scale;

// ------------------------------------------------------------------------
// Binding Parsing
// ------------------------------------------------------------------------

std::vector<std::string_view> InputManager::SplitChord(const std::string_view& binding)
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

bool InputManager::SplitBinding(const std::string_view& binding, std::string_view* source,
                                std::string_view* sub_binding)
{
  const std::string_view::size_type slash_pos = binding.find('/');
  if (slash_pos == std::string_view::npos)
  {
    Log_WarningPrintf("Malformed binding: '%.*s'", static_cast<int>(binding.size()), binding.data());
    return false;
  }

  *source = std::string_view(binding).substr(0, slash_pos);
  *sub_binding = std::string_view(binding).substr(slash_pos + 1);
  return true;
}

std::optional<InputBindingKey> InputManager::ParseInputBindingKey(const std::string_view& binding)
{
  std::string_view source, sub_binding;
  if (!SplitBinding(binding, &source, &sub_binding))
    return std::nullopt;

  // lameee, string matching
  if (StringUtil::StartsWith(source, "Keyboard"))
  {
    return ParseHostKeyboardKey(source, sub_binding);
  }
  else if (StringUtil::StartsWith(source, "Pointer"))
  {
    return ParsePointerKey(source, sub_binding);
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

bool InputManager::ParseBindingAndGetSource(const std::string_view& binding, InputBindingKey* key, InputSource** source)
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

std::string InputManager::ConvertInputBindingKeyToString(InputBindingKey key)
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
                         key.negative ? '-' : '+');
    }
  }
  else if (key.source_type < InputSourceType::Count && s_input_sources[static_cast<u32>(key.source_type)])
  {
    return s_input_sources[static_cast<u32>(key.source_type)]->ConvertKeyToString(key);
  }

  return {};
}

std::string InputManager::ConvertInputBindingKeysToString(const InputBindingKey* keys, size_t num_keys)
{
  std::stringstream ss;
  for (size_t i = 0; i < num_keys; i++)
  {
    const std::string keystr(ConvertInputBindingKeyToString(keys[i]));
    if (keystr.empty())
      return std::string();

    if (i > 0)
      ss << " & ";

    ss << keystr;
  }

  return ss.str();
}

void InputManager::AddBindings(const std::vector<std::string>& bindings, const InputEventHandler& handler)
{
  for (const std::string& binding : bindings)
  {
    std::shared_ptr<InputBinding> ibinding;
    const std::vector<std::string_view> chord_bindings(SplitChord(binding));

    for (const std::string_view& chord_binding : chord_bindings)
    {
      std::optional<InputBindingKey> key = ParseInputBindingKey(chord_binding);
      if (!key.has_value())
      {
        Log_ErrorPrintf("Invalid binding: '%s'", binding.c_str());
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
        Log_ErrorPrintf("Too many chord parts, max is %u (%s)", MAX_KEYS_PER_BINDING, binding.c_str());
        ibinding.reset();
        break;
      }

      ibinding->keys[ibinding->num_keys] = key.value();
      ibinding->full_mask |= (static_cast<u8>(1) << ibinding->num_keys);
      ibinding->num_keys++;
    }

    if (!ibinding)
      continue;

    // plop it in the input map for all the keys
    for (u32 i = 0; i < ibinding->num_keys; i++)
      s_binding_map.emplace(ibinding->keys[i].MaskDirection(), ibinding);
  }
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

// ------------------------------------------------------------------------
// Bind Encoders
// ------------------------------------------------------------------------

static std::array<const char*, static_cast<u32>(InputSourceType::Count)> s_input_class_names = {{
  "Keyboard",
  "Mouse",
#ifdef _WIN32
  "DInput",
  "XInput",
#endif
#if defined(_WIN32)
  "RawInput",
#endif
#ifdef WITH_SDL2
  "SDL",
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

std::optional<InputSourceType> InputManager::ParseInputSourceString(const std::string_view& str)
{
  for (u32 i = 0; i < static_cast<u32>(InputSourceType::Count); i++)
  {
    if (str == s_input_class_names[i])
      return static_cast<InputSourceType>(i);
  }

  return std::nullopt;
}

std::optional<InputBindingKey> InputManager::ParseHostKeyboardKey(const std::string_view& source,
                                                                  const std::string_view& sub_binding)
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

std::optional<InputBindingKey> InputManager::ParsePointerKey(const std::string_view& source,
                                                             const std::string_view& sub_binding)
{
  const std::optional<s32> pointer_index = StringUtil::FromChars<s32>(source.substr(8));
  if (!pointer_index.has_value() || pointer_index.value() < 0)
    return std::nullopt;

  InputBindingKey key = {};
  key.source_type = InputSourceType::Pointer;
  key.source_index = static_cast<u32>(pointer_index.value());

  if (StringUtil::StartsWith(sub_binding, "Button"))
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
    if (StringUtil::StartsWith(sub_binding, s_pointer_axis_names[i]))
    {
      key.source_subtype = InputSubclass::PointerAxis;
      key.data = i;

      const std::string_view dir_part(sub_binding.substr(std::strlen(s_pointer_axis_names[i])));
      if (dir_part == "+")
        key.negative = false;
      else if (dir_part == "-")
        key.negative = true;
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

// ------------------------------------------------------------------------
// Binding Enumeration
// ------------------------------------------------------------------------

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

void InputManager::AddHotkeyBindings(SettingsInterface& si)
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

void InputManager::AddPadBindings(SettingsInterface& si, const std::string& section, u32 pad_index,
                                  const Controller::ControllerInfo* cinfo)
{
  for (u32 i = 0; i < cinfo->num_bindings; i++)
  {
    const Controller::ControllerBindingInfo& bi = cinfo->bindings[i];
    const std::vector<std::string> bindings(si.GetStringList(section.c_str(), bi.name));
    if (!bindings.empty())
    {
      AddBindings(bindings, InputAxisEventHandler{[pad_index, bind_index = bi.bind_index](float value) {
                    if (!System::IsRunning())
                      return;

                    Controller* c = System::GetController(pad_index);
                    if (c)
                      c->SetBindState(bind_index, value);
                  }});
    }
  }

  for (u32 macro_button_index = 0; macro_button_index < NUM_MACRO_BUTTONS_PER_CONTROLLER; macro_button_index++)
  {
    const std::vector<std::string> bindings(
      si.GetStringList(section.c_str(), fmt::format("Macro{}", macro_button_index + 1u).c_str()));
    if (!bindings.empty())
    {
      AddBindings(bindings, InputButtonEventHandler{[pad_index, macro_button_index](bool state) {
                    if (!System::IsRunning())
                      return;

                    SetMacroButtonState(pad_index, macro_button_index, state);
                  }});
    }
  }

  if (cinfo->vibration_caps != Controller::VibrationCapabilities::NoVibration)
  {
    PadVibrationBinding vib;
    vib.pad_index = pad_index;

    bool has_any_bindings = false;
    switch (cinfo->vibration_caps)
    {
      case Controller::VibrationCapabilities::LargeSmallMotors:
      {
        if (const std::string large_binding(si.GetStringValue(section.c_str(), "LargeMotor")); !large_binding.empty())
          has_any_bindings |= ParseBindingAndGetSource(large_binding, &vib.motors[0].binding, &vib.motors[0].source);
        if (const std::string small_binding(si.GetStringValue(section.c_str(), "SmallMotor")); !small_binding.empty())
          has_any_bindings |= ParseBindingAndGetSource(small_binding, &vib.motors[1].binding, &vib.motors[1].source);
      }
      break;

      case Controller::VibrationCapabilities::SingleMotor:
      {
        if (const std::string binding(si.GetStringValue(section.c_str(), "Motor")); !binding.empty())
          has_any_bindings |= ParseBindingAndGetSource(binding, &vib.motors[0].binding, &vib.motors[0].source);
      }
      break;

      default:
        break;
    }

    if (has_any_bindings)
      s_pad_vibration_array.push_back(std::move(vib));
  }
}

// ------------------------------------------------------------------------
// Event Handling
// ------------------------------------------------------------------------

bool InputManager::HasAnyBindingsForKey(InputBindingKey key)
{
  std::unique_lock lock(s_binding_map_write_lock);
  return (s_binding_map.find(key.MaskDirection()) != s_binding_map.end());
}

bool InputManager::HasAnyBindingsForSource(InputBindingKey key)
{
  std::unique_lock lock(s_binding_map_write_lock);
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
      const bool negative = binding->keys[i].negative;
      const bool new_state = (negative ? (value < 0.0f) : (value > 0.0f));

      // invert if we're negative, since the handler expects 0..1
      const float value_to_pass = (negative ? ((value < 0.0f) ? -value : 0.0f) : (value > 0.0f) ? value : 0.0f);

      // axes are fired regardless of a state change, unless they're zero
      // (but going from not-zero to zero will still fire, because of the full state)
      // for buttons, we can use the state of the last chord key, because it'll be 1 on press,
      // and 0 on release (when the full state changes).
      if (IsAxisHandler(binding->handler))
      {
        if (value_to_pass >= 0.0f)
          std::get<InputAxisEventHandler>(binding->handler)(value_to_pass);
      }
      else if (binding->num_keys >= min_num_keys)
      {
        // update state based on whether the whole chord was activated
        const u8 new_mask = (new_state ? (binding->current_mask | bit) : (binding->current_mask & ~bit));
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
  for (u32 device = 0; device < MAX_POINTER_DEVICES; device++)
  {
    for (u32 axis = 0; axis < static_cast<u32>(static_cast<u8>(InputPointerAxis::Count)); axis++)
    {
      PointerAxisState& state = s_pointer_state[device][axis];
      const float delta = static_cast<float>(state.delta.exchange(0, std::memory_order_acquire)) / 65536.0f;
      const float unclamped_value = delta * s_pointer_axis_scale[axis];

      const InputBindingKey key(MakePointerAxisKey(device, static_cast<InputPointerAxis>(axis)));
      if (axis >= static_cast<u32>(InputPointerAxis::WheelX) &&
          ImGuiManager::ProcessPointerAxisEvent(key, unclamped_value))
      {
        continue;
      }

      const float value = std::clamp(unclamped_value, -1.0f, 1.0f);
      if (value != state.last_value)
      {
        state.last_value = value;
        InvokeEvents(key, value, GenericInputBinding::Unknown);
      }
    }
  }
}

void InputManager::UpdatePointerAbsolutePosition(u32 index, float x, float y)
{
  const float dx = x - std::exchange(s_host_pointer_positions[index][static_cast<u8>(InputPointerAxis::X)], x);
  const float dy = y - std::exchange(s_host_pointer_positions[index][static_cast<u8>(InputPointerAxis::Y)], y);

  if (dx != 0.0f)
    UpdatePointerRelativeDelta(index, InputPointerAxis::X, dx);
  if (dy != 0.0f)
    UpdatePointerRelativeDelta(index, InputPointerAxis::Y, dy);

  ImGuiManager::UpdateMousePosition(x, y);
}

void InputManager::UpdatePointerRelativeDelta(u32 index, InputPointerAxis axis, float d, bool raw_input)
{
  if (raw_input != IsUsingRawInput())
    return;

  s_pointer_state[index][static_cast<u8>(axis)].delta.fetch_add(static_cast<s32>(d * 65536.0f),
                                                                std::memory_order_release);
}

bool InputManager::IsUsingRawInput()
{
#if defined(_WIN32) && !defined(_UWP)
  return static_cast<bool>(s_input_sources[static_cast<u32>(InputSourceType::RawInput)]);
#else
  return false;
#endif
}

bool InputManager::HasPointerAxisBinds()
{
  std::unique_lock lock(s_binding_map_write_lock);
  for (const auto& it : s_binding_map)
  {
    const InputBindingKey& key = it.first;
    if (key.source_type == InputSourceType::Pointer && key.source_subtype == InputSubclass::PointerAxis &&
        key.data >= static_cast<u32>(InputPointerAxis::X) && key.data <= static_cast<u32>(InputPointerAxis::Y))
    {
      return true;
    }
  }

  return false;
}

void InputManager::SetDefaultConfig(SettingsInterface& si)
{
  si.ClearSection("InputSources");
  si.SetBoolValue("InputSources", "SDL", true);
  si.SetBoolValue("InputSources", "SDLControllerEnhancedMode", false);
  si.SetBoolValue("InputSources", "XInput", false);
  si.SetBoolValue("InputSources", "RawInput", false);
}

void InputManager::ClearPortBindings(SettingsInterface& si, u32 port)
{
  const std::string section(Controller::GetSettingsSection(port));
  const std::string type(si.GetStringValue(section.c_str(), "Type", Controller::GetDefaultPadType(port)));

  const Controller::ControllerInfo* info = Controller::GetControllerInfo(type);
  if (!info)
    return;

  for (u32 i = 0; i < info->num_bindings; i++)
    si.DeleteValue(section.c_str(), info->bindings[i].name);
}

void InputManager::CopyConfiguration(SettingsInterface* dest_si, const SettingsInterface& src_si,
                                     bool copy_pad_config /*= true*/, bool copy_pad_bindings /*= true*/,
                                     bool copy_hotkey_bindings /*= true*/)
{
  if (copy_pad_config)
    dest_si->CopyStringValue(src_si, "ControllerPorts", "MultitapMode");

  for (u32 port = 0; port < NUM_CONTROLLER_AND_CARD_PORTS; port++)
  {
    const std::string section(Controller::GetSettingsSection(port));
    const std::string type(src_si.GetStringValue(section.c_str(), "Type", Controller::GetDefaultPadType(port)));
    if (copy_pad_config)
      dest_si->SetStringValue(section.c_str(), "Type", type.c_str());

    const Controller::ControllerInfo* info = Controller::GetControllerInfo(type);
    if (!info)
      return;

    if (copy_pad_bindings)
    {
      for (u32 i = 0; i < info->num_bindings; i++)
      {
        const Controller::ControllerBindingInfo& bi = info->bindings[i];
        dest_si->CopyStringListValue(src_si, section.c_str(), bi.name);
      }

#if 0
      for (u32 i = 0; i < NUM_MACRO_BUTTONS_PER_CONTROLLER; i++)
      {
        dest_si->CopyStringListValue(src_si, section.c_str(), fmt::format("Macro{}", i + 1).c_str());
        dest_si->CopyStringValue(src_si, section.c_str(), fmt::format("Macro{}Binds", i + 1).c_str());
        dest_si->CopyUIntValue(src_si, section.c_str(), fmt::format("Macro{}Frequency", i + 1).c_str());
      }
#endif
    }

    if (copy_pad_config)
    {
      dest_si->CopyFloatValue(src_si, section.c_str(), "AxisScale");

      if (info->vibration_caps != Controller::VibrationCapabilities::NoVibration)
      {
        dest_si->CopyFloatValue(src_si, section.c_str(), "LargeMotorScale");
        dest_si->CopyFloatValue(src_si, section.c_str(), "SmallMotorScale");
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
                                const char* bind_name)
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
    Log_InfoPrintf("(MapController) Map %s/%s to '%s'", section.c_str(), bind_name, found_mapping->c_str());
    si.SetStringValue(section.c_str(), bind_name, found_mapping->c_str());
    return 1;
  }
  else
  {
    si.DeleteValue(section.c_str(), bind_name);
    return 0;
  }
}

bool InputManager::MapController(SettingsInterface& si, u32 controller,
                                 const std::vector<std::pair<GenericInputBinding, std::string>>& mapping)
{
  const std::string section(Controller::GetSettingsSection(controller));
  const std::string type(si.GetStringValue(section.c_str(), "Type", Controller::GetDefaultPadType(controller)));
  const Controller::ControllerInfo* info = Controller::GetControllerInfo(type);
  if (!info)
    return false;

  u32 num_mappings = 0;
  for (u32 i = 0; i < info->num_bindings; i++)
  {
    const Controller::ControllerBindingInfo& bi = info->bindings[i];
    if (bi.generic_mapping == GenericInputBinding::Unknown)
      continue;

    num_mappings += TryMapGenericMapping(si, section, mapping, bi.generic_mapping, bi.name);
  }
  if (info->vibration_caps == Controller::VibrationCapabilities::LargeSmallMotors)
  {
    num_mappings += TryMapGenericMapping(si, section, mapping, GenericInputBinding::SmallMotor, "SmallMotor");
    num_mappings += TryMapGenericMapping(si, section, mapping, GenericInputBinding::LargeMotor, "LargeMotor");
  }
  else if (info->vibration_caps == Controller::VibrationCapabilities::SingleMotor)
  {
    if (TryMapGenericMapping(si, section, mapping, GenericInputBinding::LargeMotor, "Motor") == 0)
      num_mappings += TryMapGenericMapping(si, section, mapping, GenericInputBinding::SmallMotor, "Motor");
    else
      num_mappings++;
  }

  return (num_mappings > 0);
}

std::vector<std::string> InputManager::GetInputProfileNames()
{
  FileSystem::FindResultsArray results;
  FileSystem::FindFiles(EmuFolders::InputProfiles.c_str(), "*.ini",
                        FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES | FILESYSTEM_FIND_RELATIVE_PATHS,
                        &results);

  std::vector<std::string> ret;
  ret.reserve(results.size());
  for (FILESYSTEM_FIND_DATA& fd : results)
    ret.emplace_back(Path::GetFileTitle(fd.FileName));
  return ret;
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
        large_motor.last_update_time = Common::Timer::GetCurrentValue();
        large_motor.source->UpdateMotorState(large_motor.binding, report_intensity);
      }
    }
    else if (large_motor.source == small_motor.source)
    {
      // both motors are bound to the same source, do an optimal update
      large_motor.last_update_time = Common::Timer::GetCurrentValue();
      large_motor.source->UpdateMotorState(large_motor.binding, small_motor.binding, large_or_single_motor_intensity,
                                           small_motor_intensity);
    }
    else
    {
      // update motors independently
      if (large_motor.source && large_motor.last_intensity != large_or_single_motor_intensity)
      {
        large_motor.last_update_time = Common::Timer::GetCurrentValue();
        large_motor.source->UpdateMotorState(large_motor.binding, large_or_single_motor_intensity);
      }
      if (small_motor.source && small_motor.last_intensity != small_motor_intensity)
      {
        small_motor.last_update_time = Common::Timer::GetCurrentValue();
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
  const u64 current_time = Common::Timer::GetCurrentValue();
  for (PadVibrationBinding& pad : s_pad_vibration_array)
  {
    if (pad.AreMotorsCombined())
    {
      // motors are combined
      PadVibrationBinding::Motor& large_motor = pad.motors[0];
      if (!large_motor.source)
        continue;

      // so only check the first one
      const double dt = Common::Timer::ConvertValueToSeconds(current_time - large_motor.last_update_time);
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

        const double dt = Common::Timer::ConvertValueToSeconds(current_time - motor.last_update_time);
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

void InputManager::LoadMacroButtonConfig(SettingsInterface& si, const std::string& section, u32 pad,
                                         const Controller::ControllerInfo* cinfo)
{
  s_macro_buttons[pad] = {};
  if (cinfo->num_bindings == 0)
    return;

  for (u32 i = 0; i < NUM_MACRO_BUTTONS_PER_CONTROLLER; i++)
  {
    std::string binds_string;
    if (!si.GetStringValue(section.c_str(), fmt::format("Macro{}Binds", i + 1u).c_str(), &binds_string))
      continue;

    const u32 frequency = si.GetUIntValue(section.c_str(), fmt::format("Macro{}Frequency", i + 1u).c_str(), 0u);

    // convert binds
    std::vector<u32> bind_indices;
    std::vector<std::string_view> buttons_split(StringUtil::SplitString(binds_string, '&', true));
    if (buttons_split.empty())
      continue;
    for (const std::string_view& button : buttons_split)
    {
      const Controller::ControllerBindingInfo* binding = nullptr;
      for (u32 j = 0; j < cinfo->num_bindings; j++)
      {
        if (button == cinfo->bindings[j].name)
        {
          binding = &cinfo->bindings[j];
          break;
        }
      }
      if (!binding)
      {
        Log_DevPrintf("Invalid bind '%.*s' in macro button %u for pad %u", static_cast<int>(button.size()),
                      button.data(), pad, i);
        continue;
      }

      bind_indices.push_back(binding->bind_index);
    }
    if (bind_indices.empty())
      continue;

    s_macro_buttons[pad][i].buttons = std::move(bind_indices);
    s_macro_buttons[pad][i].toggle_frequency = frequency;
  }
}

void InputManager::SetMacroButtonState(u32 pad, u32 index, bool state)
{
  if (pad >= NUM_CONTROLLER_AND_CARD_PORTS || index >= NUM_MACRO_BUTTONS_PER_CONTROLLER)
    return;

  MacroButton& mb = s_macro_buttons[pad][index];
  if (mb.buttons.empty() || mb.trigger_state == state)
    return;

  mb.toggle_counter = mb.toggle_frequency;
  mb.trigger_state = state;
  if (mb.toggle_state != state)
  {
    mb.toggle_state = state;
    ApplyMacroButton(pad, mb);
  }
}

void InputManager::ApplyMacroButton(u32 pad, const MacroButton& mb)
{
  Controller* const controller = System::GetController(pad);
  if (!controller)
    return;

  const float value = mb.toggle_state ? 1.0f : 0.0f;
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
  std::unique_lock<std::mutex> lock(m_event_intercept_mutex);
  DebugAssert(!m_event_intercept_callback);
  m_event_intercept_callback = std::move(callback);
}

void InputManager::RemoveHook()
{
  std::unique_lock<std::mutex> lock(m_event_intercept_mutex);
  if (m_event_intercept_callback)
    m_event_intercept_callback = {};
}

bool InputManager::HasHook()
{
  std::unique_lock<std::mutex> lock(m_event_intercept_mutex);
  return (bool)m_event_intercept_callback;
}

bool InputManager::DoEventHook(InputBindingKey key, float value)
{
  std::unique_lock<std::mutex> lock(m_event_intercept_mutex);
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

void InputManager::ReloadBindings(SettingsInterface& si, SettingsInterface& binding_si)
{
  PauseVibration();

  std::unique_lock lock(s_binding_map_write_lock);

  s_binding_map.clear();
  s_pad_vibration_array.clear();

  // Hotkeys use the base configuration, except if the custom hotkeys option is enabled.
  const bool use_profile_hotkeys = si.GetBoolValue("ControllerPorts", "UseProfileHotkeyBindings", false);
  AddHotkeyBindings(use_profile_hotkeys ? binding_si : si);

  // If there's an input profile, we load pad bindings from it alone, rather than
  // falling back to the base configuration.
  for (u32 pad = 0; pad < NUM_CONTROLLER_AND_CARD_PORTS; pad++)
  {
    const Controller::ControllerInfo* cinfo = Controller::GetControllerInfo(g_settings.controller_types[pad]);
    if (!cinfo || cinfo->type == ControllerType::None)
      continue;

    // NOTE: Macros can be overridden per-game.
    const std::string section(Controller::GetSettingsSection(pad));
    AddPadBindings(binding_si, section, pad, cinfo);
    LoadMacroButtonConfig(si, section, pad, cinfo);
  }

  for (u32 axis = 0; axis < static_cast<u32>(InputPointerAxis::Count); axis++)
  {
    // From lilypad: 1 mouse pixel = 1/8th way down.
    const float default_scale = (axis <= static_cast<u32>(InputPointerAxis::Y)) ? 8.0f : 1.0f;
    const float invert =
      si.GetBoolValue("Pad", fmt::format("Pointer{}Invert", s_pointer_axis_names[axis]).c_str(), false) ? -1.0f : 1.0f;
    s_pointer_axis_scale[axis] =
      invert / std::max(si.GetFloatValue("Pad", fmt::format("Pointer{}Scale", s_pointer_axis_names[axis]).c_str(),
                                         default_scale),
                        1.0f);
  }
}

// ------------------------------------------------------------------------
// Source Management
// ------------------------------------------------------------------------

void InputManager::CloseSources()
{
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

std::vector<std::pair<std::string, std::string>> InputManager::EnumerateDevices()
{
  std::vector<std::pair<std::string, std::string>> ret;

  ret.emplace_back("Keyboard", "Keyboard");
  ret.emplace_back("Mouse", "Mouse");

  for (u32 i = FIRST_EXTERNAL_INPUT_SOURCE; i < LAST_EXTERNAL_INPUT_SOURCE; i++)
  {
    if (s_input_sources[i])
    {
      std::vector<std::pair<std::string, std::string>> devs(s_input_sources[i]->EnumerateDevices());
      if (ret.empty())
        ret = std::move(devs);
      else
        std::move(devs.begin(), devs.end(), std::back_inserter(ret));
    }
  }

  return ret;
}

std::vector<InputBindingKey> InputManager::EnumerateMotors()
{
  std::vector<InputBindingKey> ret;

  for (u32 i = FIRST_EXTERNAL_INPUT_SOURCE; i < LAST_EXTERNAL_INPUT_SOURCE; i++)
  {
    if (s_input_sources[i])
    {
      std::vector<InputBindingKey> devs(s_input_sources[i]->EnumerateMotors());
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

static bool GetInternalGenericBindingMapping(const std::string_view& device, GenericInputBindingMapping* mapping)
{
  if (device == "Keyboard")
  {
    GetKeyboardGenericBindingMapping(mapping);
    return true;
  }

  return false;
}

GenericInputBindingMapping InputManager::GetGenericBindingMapping(const std::string_view& device)
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

static void UpdateInputSourceState(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock,
                                   InputSourceType type, std::unique_ptr<InputSource> (*factory_function)(),
                                   bool default_state)
{
  const bool enabled = si.GetBoolValue("InputSources", InputManager::InputSourceToString(type), default_state);
  if (enabled)
  {
    if (s_input_sources[static_cast<u32>(type)])
    {
      s_input_sources[static_cast<u32>(type)]->UpdateSettings(si, settings_lock);
    }
    else
    {
      std::unique_ptr<InputSource> source(factory_function());
      if (!source->Initialize(si, settings_lock))
      {
        Log_ErrorPrintf("(InputManager) Source '%s' failed to initialize.", InputManager::InputSourceToString(type));
        return;
      }

      s_input_sources[static_cast<u32>(type)] = std::move(source);
    }
  }
  else
  {
    if (s_input_sources[static_cast<u32>(type)])
    {
      s_input_sources[static_cast<u32>(type)]->Shutdown();
      s_input_sources[static_cast<u32>(type)].reset();
    }
  }
}

void InputManager::ReloadSources(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
#ifdef _WIN32
  UpdateInputSourceState(si, settings_lock, InputSourceType::DInput, &InputSource::CreateDInputSource, false);
  UpdateInputSourceState(si, settings_lock, InputSourceType::XInput, &InputSource::CreateXInputSource, false);
  UpdateInputSourceState(si, settings_lock, InputSourceType::RawInput, &InputSource::CreateWin32RawInputSource, false);
#endif
#ifdef WITH_SDL2
  UpdateInputSourceState(si, settings_lock, InputSourceType::SDL, &InputSource::CreateSDLSource, true);
#endif
}
