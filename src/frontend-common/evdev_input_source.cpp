#include "evdev_input_source.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/string_util.h"
#include "core/host.h"
#include "fmt/format.h"
#include "input_manager.h"
#include <cmath>
#include <cstdlib>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <tuple>
#include <unistd.h>

#ifdef __linux__
#include <alloca.h>
#endif

Log_SetChannel(EvdevInputSource);

static GenericInputBinding GetGenericBindingForButton(int button_id)
{
  switch (button_id)
  {
    case BTN_A:
      return GenericInputBinding::Cross;
    case BTN_B:
      return GenericInputBinding::Circle;
    case BTN_X:
      return GenericInputBinding::Square;
    case BTN_Y:
      return GenericInputBinding::Triangle;
    case BTN_SELECT:
      return GenericInputBinding::Select;
    case BTN_START:
      return GenericInputBinding::Start;
    case BTN_MODE:
      return GenericInputBinding::System;
    case BTN_TL:
      return GenericInputBinding::L1;
    case BTN_TR:
      return GenericInputBinding::R1;
    case BTN_TL2:
      return GenericInputBinding::L2;
    case BTN_TR2:
      return GenericInputBinding::R2;
    case BTN_THUMBL:
      return GenericInputBinding::L3;
    case BTN_THUMBR:
      return GenericInputBinding::R3;
    case BTN_DPAD_LEFT:
      return GenericInputBinding::DPadLeft;
    case BTN_DPAD_RIGHT:
      return GenericInputBinding::DPadRight;
    case BTN_DPAD_UP:
      return GenericInputBinding::DPadUp;
    case BTN_DPAD_DOWN:
      return GenericInputBinding::DPadDown;
    default:
      return GenericInputBinding::Unknown;
  }
}

static std::tuple<GenericInputBinding, GenericInputBinding> GetGenericBindingForAxis(u32 axis)
{
  switch (axis)
  {
    case ABS_X:
      return std::make_tuple(GenericInputBinding::LeftStickLeft, GenericInputBinding::LeftStickRight);
    case ABS_Y:
      return std::make_tuple(GenericInputBinding::LeftStickUp, GenericInputBinding::LeftStickDown);
    case ABS_RX:
      return std::make_tuple(GenericInputBinding::RightStickLeft, GenericInputBinding::RightStickRight);
    case ABS_RY:
      return std::make_tuple(GenericInputBinding::RightStickUp, GenericInputBinding::RightStickDown);
    default:
      return std::make_tuple(GenericInputBinding::Unknown, GenericInputBinding::Unknown);
  }
}

static bool IsFullAxis(u32 axis)
{
  // ugh, so min isn't necessarily zero for full axes... :/
  return (axis >= ABS_X && axis <= ABS_RZ);
}

EvdevInputSource::EvdevInputSource() = default;

EvdevInputSource::~EvdevInputSource() = default;

bool EvdevInputSource::Initialize(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
  for (int index = 0; index < 1000; index++)
  {
    TinyString path;
    path.Format("/dev/input/event%d", index);

    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
    {
      if (errno == ENOENT)
        break;
      else
        continue;
    }

    struct libevdev* obj;
    if (libevdev_new_from_fd(fd, &obj) != 0)
    {
      Log_ErrorPrintf("libevdev_new_from_fd(%s) failed", path.GetCharArray());
      close(fd);
      continue;
    }

    ControllerData data(fd, obj);
    data.controller_id = static_cast<int>(m_controllers.size());
    if (InitializeController(index, &data))
      m_controllers.push_back(std::move(data));
  }

  return true;
}

void EvdevInputSource::UpdateSettings(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
  // noop
}

bool EvdevInputSource::ReloadDevices()
{
  return false;
}

void EvdevInputSource::Shutdown()
{
  // noop
}

EvdevInputSource::ControllerData::ControllerData(int fd_, struct libevdev* obj_) : obj(obj_), fd(fd_) {}

EvdevInputSource::ControllerData::ControllerData(ControllerData&& move)
  : obj(move.obj), fd(move.fd), controller_id(move.controller_id), num_motors(move.num_motors), deadzone(move.deadzone),
    uniq(std::move(move.uniq)), name(std::move(move.name)), axes(std::move(move.axes)), buttons(std::move(move.buttons))
{
  move.obj = nullptr;
  move.fd = -1;
}

EvdevInputSource::ControllerData::~ControllerData()
{
  if (obj)
    libevdev_free(obj);
  if (fd >= 0)
    close(fd);
}

EvdevInputSource::ControllerData& EvdevInputSource::ControllerData::operator=(EvdevInputSource::ControllerData&& move)
{
  if (obj)
    libevdev_free(obj);
  obj = move.obj;
  move.obj = nullptr;
  if (fd >= 0)
    close(fd);
  fd = move.fd;
  move.fd = -1;
  controller_id = move.controller_id;
  num_motors = move.num_motors;
  deadzone = move.deadzone;
  uniq = std::move(move.uniq);
  name = std::move(move.name);
  axes = std::move(move.axes);
  buttons = std::move(move.buttons);
  return *this;
}

EvdevInputSource::ControllerData* EvdevInputSource::GetControllerById(int id)
{
  for (ControllerData& cd : m_controllers)
  {
    if (cd.controller_id == id)
      return &cd;
  }

  return nullptr;
}

EvdevInputSource::ControllerData* EvdevInputSource::GetControllerByUniq(const std::string_view& uniq)
{
  for (ControllerData& cd : m_controllers)
  {
    if (uniq == cd.uniq)
      return &cd;
  }

  return nullptr;
}

bool EvdevInputSource::InitializeController(int index, ControllerData* cd)
{
  const char* name = libevdev_get_name(cd->obj);
  const char* uniq = libevdev_get_uniq(cd->obj);
  cd->name = name ? name : "Unknown";
  cd->uniq = uniq ? fmt::format("Evdev-{}", uniq) : fmt::format("Evdev-Unknown{}", index);

  // Sanitize the name a bit just in case..
  for (size_t i = 6; i < cd->uniq.length(); i++)
  {
    const char ch = cd->uniq[i];
    if (!(ch >= 'a' && ch <= 'z') && !(ch >= 'A' && ch <= 'Z') && !(ch >= '0' && ch <= '9') && ch != '_')
      cd->uniq[i] = '_';
  }

  Log_DevPrintf("Input %d device name: \"%s\" ('%s')", index, cd->name.c_str(), cd->uniq.c_str());
  Log_DevPrintf("Input %d device ID: bus %#x vendor %#x product %#x", index, libevdev_get_id_bustype(cd->obj),
                libevdev_get_id_vendor(cd->obj), libevdev_get_id_product(cd->obj));

  bool has_dpad = false;
  for (u32 key = 0; key < KEY_CNT; key++)
  {
    if (!libevdev_has_event_code(cd->obj, EV_KEY, key))
      continue;

    const char* button_name = libevdev_event_code_get_name(EV_KEY, key);
    Log_DebugPrintf("Key %d: %s", key, button_name ? button_name : "null");

    ControllerData::Button button;
    button.name = button_name ? std::string(button_name) : fmt::format("Button{}", key);
    button.id = key;
    button.generic = GetGenericBindingForButton(key);
    cd->buttons.push_back(std::move(button));

    if (key == BTN_DPAD_LEFT || key == BTN_DPAD_RIGHT || key == BTN_DPAD_UP || key == BTN_DPAD_DOWN)
      has_dpad = true;
  }

  // Prelookup axes to get the range of them.
  for (u32 axis = 0; axis <= ABS_TOOL_WIDTH; axis++)
  {
    if (!libevdev_has_event_code(cd->obj, EV_ABS, axis))
      continue;

    const s32 min = libevdev_get_abs_minimum(cd->obj, axis);
    const s32 max = libevdev_get_abs_maximum(cd->obj, axis);
    const char* axis_name = libevdev_event_code_get_name(EV_ABS, axis);
    Log_DebugPrintf("Axis %u: %s [%d-%d]", axis, axis_name ? axis_name : "null", min, max);

    ControllerData::Axis ad;
    ad.name = axis_name ? std::string(axis_name) : fmt::format("Button{}", axis);
    ad.id = axis;
    ad.min = min;
    ad.range = max - min;
    ad.neg_button = 0;
    ad.pos_button = 0;
    std::tie(ad.neg_generic, ad.pos_generic) = GetGenericBindingForAxis(axis);

    if (!has_dpad)
    {
      // map hat -> dpad
      if (axis == ABS_HAT0X)
      {
        Log_VerbosePrintf("Redirecting HAT0X to DPad left/right");
        ad.neg_button = BTN_DPAD_LEFT;
        ad.pos_button = BTN_DPAD_RIGHT;
        cd->buttons.emplace_back("BTN_DPAD_LEFT", BTN_DPAD_LEFT, GenericInputBinding::DPadLeft);
        cd->buttons.emplace_back("BTN_DPAD_RIGHT", BTN_DPAD_RIGHT, GenericInputBinding::DPadRight);
      }
      else if (axis == ABS_HAT0Y)
      {
        Log_VerbosePrintf("Redirecting HAT0Y to DPad up/down");
        ad.neg_button = BTN_DPAD_UP;
        ad.pos_button = BTN_DPAD_DOWN;
        cd->buttons.emplace_back("BTN_DPAD_UP", BTN_DPAD_UP, GenericInputBinding::DPadUp);
        cd->buttons.emplace_back("BTN_DPAD_DOWN", BTN_DPAD_DOWN, GenericInputBinding::DPadDown);
      }
    }

    cd->axes.push_back(std::move(ad));
  }

  // Heuristic borrowed from Dolphin's evdev controller interface - ignore bogus devices
  // which do have less than 2 axes and less than 8 buttons. Key count of 80 is probably a keyboard.
  // Axes with no buttons is probably a motion sensor.
  if ((cd->axes.size() < 2 && cd->buttons.size() < 8) || cd->buttons.size() > 80 || (cd->axes.size() >= 6 && cd->buttons.empty()))
  {
    Log_VerbosePrintf("Ignoring device %s with %zu axes and %zu buttons due to heuristic", name, cd->axes.size(),
                      cd->buttons.size());
    return false;
  }

  Log_InfoPrintf("Controller %d -> %s with %zu axes and %zu buttons", cd->controller_id, name, cd->axes.size(),
                 cd->buttons.size());
  return true;
}

std::vector<std::pair<std::string, std::string>> EvdevInputSource::EnumerateDevices()
{
  std::vector<std::pair<std::string, std::string>> ret;
  for (const ControllerData& cd : m_controllers)
    ret.emplace_back(cd.uniq, cd.name);

  return ret;
}

std::optional<InputBindingKey> EvdevInputSource::ParseKeyString(const std::string_view& device,
                                                                const std::string_view& binding)
{
  if (!StringUtil::StartsWith(device, "Evdev-") || binding.empty())
    return std::nullopt;

  const ControllerData* cd = GetControllerByUniq(device);
  if (!cd)
    return std::nullopt;

  InputBindingKey key = {};
  key.source_type = InputSourceType::Evdev;
  key.source_index = static_cast<u32>(cd->controller_id);

  if (binding[0] == '-' || binding[0] == '+')
  {
    const std::string_view abinding(binding.substr(1));
    for (const ControllerData::Axis& axis : cd->axes)
    {
      if (abinding == axis.name)
      {
        key.source_subtype = InputSubclass::ControllerAxis;
        key.negative = (binding[0] == '-');
        key.data = axis.id;
        return key;
      }
    }
  }
  else
  {
    for (const ControllerData::Button& button : cd->buttons)
    {
      if (binding == button.name)
      {
        key.source_subtype = InputSubclass::ControllerButton;
        key.data = button.id;
        return key;
      }
    }
  }

  return std::nullopt;
}

std::string EvdevInputSource::ConvertKeyToString(InputBindingKey key)
{
  std::string ret;

  if (key.source_type == InputSourceType::Evdev)
  {
    const ControllerData* cd = GetControllerById(key.source_index);
    if (cd)
    {
      if (key.source_subtype == InputSubclass::ControllerAxis)
      {
        for (const ControllerData::Axis& axis : cd->axes)
        {
          if (static_cast<u32>(axis.id) == key.data)
          {
            ret = fmt::format("{}/{}{}", cd->uniq, key.negative ? "-" : "+", axis.name);
            break;
          }
        }
      }
      else if (key.source_subtype == InputSubclass::ControllerButton)
      {
        for (const ControllerData::Button& button : cd->buttons)
        {
          if (static_cast<u32>(button.id) == key.data)
          {
            ret = fmt::format("{}/{}", cd->uniq, button.name);
            break;
          }
        }
      }
    }
  }

  return ret;
}

bool EvdevInputSource::GetGenericBindingMapping(const std::string_view& device, GenericInputBindingMapping* mapping)
{
  const ControllerData* cd = GetControllerByUniq(device);
  if (!cd)
    return false;

  for (const ControllerData::Button& button : cd->buttons)
  {
    if (button.generic != GenericInputBinding::Unknown)
      mapping->emplace_back(button.generic, fmt::format("{}/{}", cd->uniq, button.name));
  }

  for (const ControllerData::Axis& axis : cd->axes)
  {
    if (axis.neg_generic != GenericInputBinding::Unknown)
      mapping->emplace_back(axis.neg_generic, fmt::format("{}/-{}", cd->uniq, axis.name));
    if (axis.pos_generic != GenericInputBinding::Unknown)
      mapping->emplace_back(axis.pos_generic, fmt::format("{}/+{}", cd->uniq, axis.name));
  }

  return true;
}

std::vector<InputBindingKey> EvdevInputSource::EnumerateMotors()
{
  // noop
  return {};
}

void EvdevInputSource::UpdateMotorState(InputBindingKey key, float intensity)
{
  // noop
}

void EvdevInputSource::UpdateMotorState(InputBindingKey large_key, InputBindingKey small_key, float large_intensity,
                                        float small_intensity)
{
  // noop
}

void EvdevInputSource::PollEvents()
{
  if (m_controllers.empty())
    return;

  struct pollfd* fds = static_cast<struct pollfd*>(alloca(sizeof(struct pollfd) * m_controllers.size()));
  for (size_t i = 0; i < m_controllers.size(); i++)
  {
    fds[i].events = POLLIN;
    fds[i].fd = m_controllers[i].fd;
    fds[i].revents = 0;
  }

  if (poll(fds, static_cast<int>(m_controllers.size()), 0) <= 0)
    return;

  for (size_t i = 0; i < m_controllers.size(); i++)
  {
    if (fds[i].revents & POLLIN)
      HandleControllerEvents(&m_controllers[i]);
  }
}

void EvdevInputSource::HandleControllerEvents(ControllerData* cd)
{
  struct input_event ev;
  while (libevdev_next_event(cd->obj, LIBEVDEV_READ_FLAG_NORMAL, &ev) == 0)
  {
    switch (ev.type)
    {
      case EV_KEY:
      {
        // auto-repeat
        if (ev.value == 2)
          continue;

        const bool pressed = (ev.value == 1);
        Log_DebugPrintf("%s %s Key %d %s", cd->uniq.c_str(), cd->name.c_str(), ev.code, pressed ? "pressed" : "unpressed");
        InputManager::InvokeEvents(MakeGenericControllerButtonKey(InputSourceType::Evdev, cd->controller_id, ev.code),
                                   pressed ? 1.0f : 0.0f, GetGenericBindingForButton(ev.code));
      }
      break;

      case EV_ABS:
      {
        // axis
        Log_DebugPrintf("%s %s Axis %u %d", cd->uniq.c_str(), cd->name.c_str(), ev.code, ev.value);

        for (ControllerData::Axis& axis : cd->axes)
        {
          if (axis.id == ev.code)
          {
            const float norm_value = static_cast<float>(static_cast<s32>(ev.value) - static_cast<s32>(axis.min)) /
                                     static_cast<float>(axis.range);
            const float real_value = (axis.min < 0 || IsFullAxis(ev.code)) ? ((norm_value * 2.0f) - 1.0f) : norm_value;

            // hat -> dpad mapping
            static constexpr float MAPPING_DEADZONE = 0.5f;
            if (axis.neg_button != 0)
            {
              if (real_value <= -MAPPING_DEADZONE && axis.last_value > -MAPPING_DEADZONE)
              {
                // gone negative
                InputManager::InvokeEvents(
                  MakeGenericControllerButtonKey(InputSourceType::Evdev, cd->controller_id, axis.neg_button), 1.0f,
                  GetGenericBindingForButton(axis.neg_button));
              }
              else if (real_value > -MAPPING_DEADZONE && axis.last_value <= -MAPPING_DEADZONE)
              {
                // no longer negative
                InputManager::InvokeEvents(
                  MakeGenericControllerButtonKey(InputSourceType::Evdev, cd->controller_id, axis.neg_button), 0.0f,
                  GetGenericBindingForButton(axis.neg_button));
              }
              else if (real_value >= MAPPING_DEADZONE && axis.last_value < MAPPING_DEADZONE)
              {
                // gone positive
                InputManager::InvokeEvents(
                  MakeGenericControllerButtonKey(InputSourceType::Evdev, cd->controller_id, axis.pos_button), 1.0f,
                  GetGenericBindingForButton(axis.pos_button));
              }
              else if (real_value < MAPPING_DEADZONE && axis.last_value >= MAPPING_DEADZONE)
              {
                // no longer positive
                InputManager::InvokeEvents(
                  MakeGenericControllerButtonKey(InputSourceType::Evdev, cd->controller_id, axis.pos_button), 0.0f,
                  GetGenericBindingForButton(axis.pos_button));
              }
            }
            else if (axis.last_value != real_value)
            {
              const GenericInputBinding generic = (real_value < 0.0f) ? axis.neg_generic : axis.pos_generic;
              InputManager::InvokeEvents(
                MakeGenericControllerAxisKey(InputSourceType::Evdev, cd->controller_id, ev.code), real_value, generic);
            }

            axis.last_value = real_value;
            break;
          }
        }
      }
      break;

      default:
        break;
    }
  }
}

std::unique_ptr<InputSource> InputSource::CreateEvdevSource()
{
  return std::make_unique<EvdevInputSource>();
}
