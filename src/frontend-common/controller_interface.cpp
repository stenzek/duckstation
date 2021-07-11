#include "controller_interface.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/string_util.h"
#include "core/controller.h"
#include "core/system.h"
#include <cmath>
Log_SetChannel(ControllerInterface);

ControllerInterface::ControllerInterface() = default;

ControllerInterface::~ControllerInterface() = default;

bool ControllerInterface::Initialize(CommonHostInterface* host_interface)
{
  m_host_interface = host_interface;
  return true;
}

void ControllerInterface::Shutdown()
{
  m_host_interface = nullptr;
}

std::optional<int> ControllerInterface::GetControllerIndex(const std::string_view& device)
{
  if (!StringUtil::StartsWith(device, "Controller"))
    return std::nullopt;

  const std::optional<int> controller_index = StringUtil::FromChars<int>(device.substr(10));
  if (!controller_index || *controller_index < 0)
  {
    Log_WarningPrintf("Invalid controller index in button binding '%*s'", static_cast<int>(device.length()),
                      device.data());
    return std::nullopt;
  }

  return controller_index;
}

void ControllerInterface::SetHook(Hook::Callback callback)
{
  std::unique_lock<std::mutex> lock(m_event_intercept_mutex);
  Assert(!m_event_intercept_callback);
  m_event_intercept_callback = std::move(callback);
}

void ControllerInterface::ClearHook()
{
  std::unique_lock<std::mutex> lock(m_event_intercept_mutex);
  if (m_event_intercept_callback)
    m_event_intercept_callback = {};
}

bool ControllerInterface::HasHook()
{
  std::unique_lock<std::mutex> lock(m_event_intercept_mutex);
  return (bool)m_event_intercept_callback;
}

bool ControllerInterface::DoEventHook(Hook::Type type, int controller_index, int button_or_axis_number,
                                      std::variant<float, std::string_view> value, bool track_history)
{
  std::unique_lock<std::mutex> lock(m_event_intercept_mutex);
  if (!m_event_intercept_callback)
    return false;

  const Hook ei{type, controller_index, button_or_axis_number, std::move(value), track_history};
  const Hook::CallbackResult action = m_event_intercept_callback(ei);
  if (action == Hook::CallbackResult::StopMonitoring)
    m_event_intercept_callback = {};

  return true;
}

void ControllerInterface::OnControllerConnected(int host_id)
{
  Log_InfoPrintf("Host controller %d connected, updating input map", host_id);
  m_host_interface->UpdateInputMap();
}

void ControllerInterface::OnControllerDisconnected(int host_id)
{
  Log_InfoPrintf("Host controller %d disconnected, updating input map", host_id);
  m_host_interface->UpdateInputMap();
}

void ControllerInterface::ClearBindings() {}

bool ControllerInterface::BindControllerAxis(int controller_index, int axis_number, AxisSide axis_side,
                                             AxisCallback callback)
{
  return false;
}

bool ControllerInterface::BindControllerButton(int controller_index, int button_number, ButtonCallback callback)
{
  return false;
}

bool ControllerInterface::BindControllerAxisToButton(int controller_index, int axis_number, bool direction,
                                                     ButtonCallback callback)
{
  return false;
}

static constexpr std::array<const char*, static_cast<u32>(ControllerInterface::Backend::Count)> s_backend_names = {{
  TRANSLATABLE("ControllerInterface", "None"),
#ifdef WITH_SDL2
  TRANSLATABLE("ControllerInterface", "SDL"),
#endif
#ifdef _WIN32
  TRANSLATABLE("ControllerInterface", "XInput"),
#endif
#ifdef WITH_DINPUT
  TRANSLATABLE("ControllerInterface", "DInput"),
#endif
#ifdef ANDROID
  // Deliberately not translated as it's not exposed to users.
  "Android",
#endif
#ifdef WITH_EVDEV
  TRANSLATABLE("ControllerInterface", "Evdev"),
#endif
}};

std::optional<ControllerInterface::Backend> ControllerInterface::ParseBackendName(const char* name)
{
  for (u32 i = 0; i < static_cast<u32>(s_backend_names.size()); i++)
  {
    if (StringUtil::Strcasecmp(name, s_backend_names[i]) == 0)
      return static_cast<Backend>(i);
  }

  return std::nullopt;
}

const char* ControllerInterface::GetBackendName(Backend type)
{
  return s_backend_names[static_cast<u32>(type)];
}

ControllerInterface::Backend ControllerInterface::GetDefaultBackend()
{
#ifdef WITH_SDL2
  return Backend::SDL;
#else
#ifdef _WIN32
  return Backend::XInput;
#else
  return Backend::None;
#endif
#endif
}

#ifdef WITH_SDL2
#include "sdl_controller_interface.h"
#endif
#ifdef _WIN32
#include "xinput_controller_interface.h"
#endif
#ifdef WITH_DINPUT
#include "dinput_controller_interface.h"
#endif
#ifdef WITH_EVDEV
#include "evdev_controller_interface.h"
#endif

std::unique_ptr<ControllerInterface> ControllerInterface::Create(Backend type)
{
#ifdef WITH_SDL2
  if (type == Backend::SDL)
    return std::make_unique<SDLControllerInterface>();
#endif
#ifdef _WIN32
  if (type == Backend::XInput)
    return std::make_unique<XInputControllerInterface>();
#endif
#ifdef WITH_DINPUT
  if (type == Backend::DInput)
    return std::make_unique<DInputControllerInterface>();
#endif
#ifdef WITH_EVDEV
  if (type == Backend::Evdev)
    return std::make_unique<EvdevControllerInterface>();
#endif

  return {};
}
