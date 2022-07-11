#include "evdev_controller_interface.h"
#include "common/assert.h"
#include "common/file_system.h"
#include "common/log.h"
#include "core/controller.h"
#include "core/system.h"
#include <cmath>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#ifdef __linux__
#include <alloca.h>
#endif

#if 0

Log_SetChannel(EvdevControllerInterface);

EvdevControllerInterface::EvdevControllerInterface() = default;

EvdevControllerInterface::~EvdevControllerInterface() = default;

ControllerInterface::Backend EvdevControllerInterface::GetBackend() const
{
  return ControllerInterface::Backend::Evdev;
}

bool EvdevControllerInterface::Initialize(CommonHostInterface* host_interface)
{
  for (int index = 0; index < 1000; index++)
  {
    TinyString path;
    path.Format("/dev/input/event%d", index);

    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
      break;

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

  if (!ControllerInterface::Initialize(host_interface))
    return false;

  return true;
}

void EvdevControllerInterface::Shutdown()
{
  ControllerInterface::Shutdown();
}

EvdevControllerInterface::ControllerData::ControllerData(int fd_, struct libevdev* obj_) : obj(obj_), fd(fd_) {}

EvdevControllerInterface::ControllerData::ControllerData(ControllerData&& move)
  : obj(move.obj), fd(move.fd), controller_id(move.controller_id), num_motors(move.num_motors), deadzone(move.deadzone),
    axes(std::move(move.axes)), buttons(std::move(move.buttons))
{
  move.obj = nullptr;
  move.fd = -1;
}

EvdevControllerInterface::ControllerData::~ControllerData()
{
  if (obj)
    libevdev_free(obj);
  if (fd >= 0)
    close(fd);
}

EvdevControllerInterface::ControllerData&
EvdevControllerInterface::ControllerData::operator=(EvdevControllerInterface::ControllerData&& move)
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
  axes = std::move(move.axes);
  buttons = std::move(move.buttons);
  return *this;
}

EvdevControllerInterface::ControllerData* EvdevControllerInterface::GetControllerById(int id)
{
  for (ControllerData& cd : m_controllers)
  {
    if (cd.controller_id == id)
      return &cd;
  }

  return nullptr;
}

bool EvdevControllerInterface::InitializeController(int index, ControllerData* cd)
{
  const char* name = libevdev_get_name(cd->obj);
  Log_DevPrintf("Input %d device name: \"%s\"", index, name);
  Log_DevPrintf("Input %d device ID: bus %#x vendor %#x product %#x", index, libevdev_get_id_bustype(cd->obj),
                libevdev_get_id_vendor(cd->obj), libevdev_get_id_product(cd->obj));

  for (u32 key = 0; key < KEY_CNT; key++)
  {
    if (!libevdev_has_event_code(cd->obj, EV_KEY, key))
      continue;

    const char* button_name = libevdev_event_code_get_name(EV_KEY, key);
    Log_DevPrintf("Key %d: %s -> Button %zu", key, button_name ? button_name : "null", cd->buttons.size());

    ControllerData::Button button;
    button.id = key;
    cd->buttons.push_back(std::move(button));
  }

  for (u32 axis = 0; axis <= ABS_TOOL_WIDTH; axis++)
  {
    if (!libevdev_has_event_code(cd->obj, EV_ABS, axis))
      continue;

    const s32 min = libevdev_get_abs_minimum(cd->obj, axis);
    const s32 max = libevdev_get_abs_maximum(cd->obj, axis);
    const char* axis_name = libevdev_event_code_get_name(EV_ABS, axis);
    Log_DevPrintf("Axis %u: %s -> Axis %zu [%d-%d]", axis, axis_name ? axis_name : "null", cd->axes.size(), min, max);

    ControllerData::Axis ad;
    ad.id = axis;
    ad.min = min;
    ad.range = max - min;
    cd->axes.push_back(std::move(ad));
  }

  // Heuristic borrowed from Dolphin's evdev controller interface - ignore bogus devices
  // which do have less than 2 axes and less than 8 buttons.
  if (cd->axes.size() < 2 && cd->buttons.size() < 8)
  {
    Log_VerbosePrintf("Ignoring device %s with %zu axes and %zu buttons due to heuristic", name, cd->axes.size(),
                      cd->buttons.size());
    return false;
  }

  Log_InfoPrintf("Controller %d -> %s with %zu axes and %zu buttons", cd->controller_id, name, cd->axes.size(),
                 cd->buttons.size());
  return true;
}

void EvdevControllerInterface::HandleControllerEvents(ControllerData* cd)
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
        Log_DevPrintf("Key %d %s", ev.code, pressed ? "pressed" : "unpressed");

        for (u32 i = 0; i < static_cast<u32>(cd->buttons.size()); i++)
        {
          if (cd->buttons[i].id == ev.code)
          {
            HandleButtonEvent(cd, i, ev.code, pressed);
            break;
          }
        }
      }
      break;

      case EV_ABS:
      {
        // axis
        Log_DebugPrintf("Axis %u %d", ev.code, ev.value);

        for (u32 i = 0; i < static_cast<u32>(cd->axes.size()); i++)
        {
          if (cd->axes[i].id == ev.code)
          {
            HandleAxisEvent(cd, i, ev.value);
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

void EvdevControllerInterface::PollEvents()
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

void EvdevControllerInterface::ClearBindings()
{
  for (ControllerData& cd : m_controllers)
  {
    for (ControllerData::Button& btn : cd.buttons)
    {
      btn.callback = {};
      btn.axis_callback = {};
    }
    for (ControllerData::Axis& axis : cd.axes)
    {
      axis.callback = {};
      axis.button_callback = {};
    }
  }
}

bool EvdevControllerInterface::BindControllerAxis(int controller_index, int axis_number, AxisSide axis_side,
                                                  AxisCallback callback)
{
  ControllerData* cd = GetControllerById(controller_index);
  if (!cd || static_cast<u32>(axis_number) >= cd->axes.size())
    return false;

  cd->axes[axis_number].callback[axis_side] = std::move(callback);
  return true;
}

bool EvdevControllerInterface::BindControllerButton(int controller_index, int button_number, ButtonCallback callback)
{
  ControllerData* cd = GetControllerById(controller_index);
  if (!cd || static_cast<u32>(button_number) >= cd->buttons.size())
    return false;

  cd->buttons[button_number].callback = std::move(callback);
  return true;
}

bool EvdevControllerInterface::BindControllerAxisToButton(int controller_index, int axis_number, bool direction,
                                                          ButtonCallback callback)
{
  ControllerData* cd = GetControllerById(controller_index);
  if (!cd || static_cast<u32>(axis_number) >= cd->axes.size())
    return false;

  cd->axes[axis_number].button_callback[BoolToUInt8(direction)] = std::move(callback);
  return true;
}

bool EvdevControllerInterface::BindControllerHatToButton(int controller_index, int hat_number,
                                                         std::string_view hat_position, ButtonCallback callback)
{
  // Hats don't exist in XInput
  return false;
}

bool EvdevControllerInterface::BindControllerButtonToAxis(int controller_index, int button_number,
                                                          AxisCallback callback)
{
  ControllerData* cd = GetControllerById(controller_index);
  if (!cd || static_cast<u32>(button_number) >= cd->buttons.size())
    return false;

  cd->buttons[button_number].axis_callback = std::move(callback);
  return true;
}

bool EvdevControllerInterface::HandleAxisEvent(ControllerData* cd, u32 axis, s32 value)
{
  const ControllerData::Axis& ad = cd->axes[axis];
  float f_value = (static_cast<float>(value - ad.min) / static_cast<float>(ad.range));
  if (ad.min < 0)
    f_value = (f_value * 2.0f) - 1.0f;

  Log_DevPrintf("controller %u axis %u %d %f range %d", cd->controller_id, axis, value, f_value, ad.range);

  if (DoEventHook(Hook::Type::Axis, cd->controller_id, axis, f_value))
    return true;

  const AxisCallback& cb = ad.callback[AxisSide::Full];
  if (cb)
  {
    cb(f_value);
    return true;
  }
  else
  {
    const AxisCallback& positive_cb = ad.callback[AxisSide::Positive];
    const AxisCallback& negative_cb = ad.callback[AxisSide::Negative];
    if (positive_cb || negative_cb)
    {
      if (positive_cb)
        positive_cb((f_value < 0.0f) ? 0.0f : f_value);
      if (negative_cb)
        negative_cb((f_value >= 0.0f) ? 0.0f : -f_value);

      return true;
    }
  }

  // set the other direction to false so large movements don't leave the opposite on
  const bool outside_deadzone = (std::abs(f_value) >= cd->deadzone);
  const bool positive = (f_value >= 0.0f);
  const ButtonCallback& other_button_cb = ad.button_callback[BoolToUInt8(!positive)];
  const ButtonCallback& button_cb = ad.button_callback[BoolToUInt8(positive)];
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

static FrontendCommon::ControllerNavigationButton MapEventButtonToNavigationButton(int button_id)
{
  switch (button_id)
  {
    case BTN_A:
      return FrontendCommon::ControllerNavigationButton::Activate;
    case BTN_B:
      return FrontendCommon::ControllerNavigationButton::Cancel;
    case BTN_TL:
      return FrontendCommon::ControllerNavigationButton::LeftShoulder;
    case BTN_TR:
      return FrontendCommon::ControllerNavigationButton::RightShoulder;
    case BTN_DPAD_LEFT:
      return FrontendCommon::ControllerNavigationButton::DPadLeft;
    case BTN_DPAD_RIGHT:
      return FrontendCommon::ControllerNavigationButton::DPadRight;
    case BTN_DPAD_UP:
      return FrontendCommon::ControllerNavigationButton::DPadUp;
    case BTN_DPAD_DOWN:
      return FrontendCommon::ControllerNavigationButton::DPadDown;
    default:
      return FrontendCommon::ControllerNavigationButton::Count;
  }
}

bool EvdevControllerInterface::HandleButtonEvent(ControllerData* cd, u32 button, int button_id, bool pressed)
{
  Log_DevPrintf("controller %d button %u %s", cd->controller_id, button, pressed ? "pressed" : "released");

  if (DoEventHook(Hook::Type::Button, cd->controller_id, button, pressed ? 1.0f : 0.0f))
    return true;

  const FrontendCommon::ControllerNavigationButton nav_button = MapEventButtonToNavigationButton(button_id);
  if (nav_button < FrontendCommon::ControllerNavigationButton::Count)
    m_host_interface->SetControllerNavigationButtonState(nav_button, pressed);

  if (m_host_interface->IsControllerNavigationActive())
  {
    // UI consumed the event
    return true;
  }

  const ButtonCallback& cb = cd->buttons[button].callback;
  if (cb)
  {
    cb(pressed);
    return true;
  }

  // Assume a half-axis, i.e. in 0..1 range
  const AxisCallback& axis_cb = cd->buttons[button].axis_callback;
  if (axis_cb)
  {
    axis_cb(pressed ? 1.0f : 0.0f);
  }
  return true;
}

u32 EvdevControllerInterface::GetControllerRumbleMotorCount(int controller_index)
{
  ControllerData* cd = GetControllerById(controller_index);
  return cd ? cd->num_motors : 0;
}

void EvdevControllerInterface::SetControllerRumbleStrength(int controller_index, const float* strengths, u32 num_motors)
{
  ControllerData* cd = GetControllerById(controller_index);
  if (!cd)
    return;

  /*  XINPUT_VIBRATION vib;
    vib.wLeftMotorSpeed = static_cast<u16>(strengths[0] * 65535.0f);
    vib.wRightMotorSpeed = static_cast<u16>(strengths[1] * 65535.0f);
    m_xinput_set_state(static_cast<u32>(controller_index), &vib);*/
}

bool EvdevControllerInterface::SetControllerDeadzone(int controller_index, float size /* = 0.25f */)
{
  ControllerData* cd = GetControllerById(controller_index);
  if (!cd)
    return false;

  cd->deadzone = std::clamp(std::abs(size), 0.01f, 0.99f);
  Log_InfoPrintf("Controller %d deadzone size set to %f", controller_index, cd->deadzone);
  return true;
}

#endif
