#include "playstation_mouse.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/state_wrapper.h"
#include "gpu.h"
#include "host_display.h"
#include "host_interface.h"
#include "system.h"
#include <array>
Log_SetChannel(PlayStationMouse);

PlayStationMouse::PlayStationMouse()
{
  m_last_host_position_x = g_host_interface->GetDisplay()->GetMousePositionX();
  m_last_host_position_y = g_host_interface->GetDisplay()->GetMousePositionY();
}

PlayStationMouse::~PlayStationMouse() = default;

ControllerType PlayStationMouse::GetType() const
{
  return ControllerType::PlayStationMouse;
}

std::optional<s32> PlayStationMouse::GetAxisCodeByName(std::string_view axis_name) const
{
  return StaticGetAxisCodeByName(axis_name);
}

std::optional<s32> PlayStationMouse::GetButtonCodeByName(std::string_view button_name) const
{
  return StaticGetButtonCodeByName(button_name);
}

void PlayStationMouse::Reset()
{
  m_transfer_state = TransferState::Idle;
}

bool PlayStationMouse::DoState(StateWrapper& sw, bool apply_input_state)
{
  if (!Controller::DoState(sw, apply_input_state))
    return false;

  u16 button_state = m_button_state;
  u8 delta_x = m_delta_x;
  u8 delta_y = m_delta_y;
  sw.Do(&button_state);
  sw.Do(&delta_x);
  sw.Do(&delta_y);
  if (apply_input_state)
  {
    m_button_state = button_state;
    m_delta_x = delta_x;
    m_delta_y = delta_y;
  }

  sw.Do(&m_transfer_state);
  return true;
}

bool PlayStationMouse::GetButtonState(s32 button_code) const
{
  if (button_code < 0 || button_code >= static_cast<s32>(Button::Count))
    return false;

  const u16 bit = u16(1) << static_cast<u8>(button_code);
  return ((m_button_state & bit) == 0);
}

void PlayStationMouse::SetButtonState(Button button, bool pressed)
{
  static constexpr std::array<u8, static_cast<size_t>(Button::Count)> indices = {{11, 10}};
  if (pressed)
    m_button_state &= ~(u16(1) << indices[static_cast<u8>(button)]);
  else
    m_button_state |= u16(1) << indices[static_cast<u8>(button)];
}

void PlayStationMouse::SetButtonState(s32 button_code, bool pressed)
{
  if (button_code < 0 || button_code >= static_cast<s32>(Button::Count))
    return;

  SetButtonState(static_cast<Button>(button_code), pressed);
}

void PlayStationMouse::ResetTransferState()
{
  m_transfer_state = TransferState::Idle;
}

bool PlayStationMouse::Transfer(const u8 data_in, u8* data_out)
{
  static constexpr u16 ID = 0x5A12;

  switch (m_transfer_state)
  {
    case TransferState::Idle:
    {
      *data_out = 0xFF;

      if (data_in == 0x01)
      {
        m_transfer_state = TransferState::Ready;
        return true;
      }
      return false;
    }

    case TransferState::Ready:
    {
      if (data_in == 0x42)
      {
        *data_out = Truncate8(ID);
        m_transfer_state = TransferState::IDMSB;
        return true;
      }

      *data_out = 0xFF;
      return false;
    }

    case TransferState::IDMSB:
    {
      *data_out = Truncate8(ID >> 8);
      m_transfer_state = TransferState::ButtonsLSB;
      return true;
    }

    case TransferState::ButtonsLSB:
    {
      *data_out = Truncate8(m_button_state);
      m_transfer_state = TransferState::ButtonsMSB;
      return true;
    }

    case TransferState::ButtonsMSB:
    {
      *data_out = Truncate8(m_button_state >> 8);
      m_transfer_state = TransferState::DeltaX;
      return true;
    }

    case TransferState::DeltaX:
    {
      UpdatePosition();
      *data_out = static_cast<u8>(m_delta_x);
      m_transfer_state = TransferState::DeltaY;
      return true;
    }

    case TransferState::DeltaY:
    {
      *data_out = static_cast<u8>(m_delta_y);
      m_transfer_state = TransferState::Idle;
      return false;
    }

    default:
    {
      UnreachableCode();
      return false;
    }
  }
}

void PlayStationMouse::UpdatePosition()
{
  // get screen coordinates
  const HostDisplay* display = g_host_interface->GetDisplay();
  const s32 mouse_x = display->GetMousePositionX();
  const s32 mouse_y = display->GetMousePositionY();
  const s32 delta_x = mouse_x - m_last_host_position_x;
  const s32 delta_y = mouse_y - m_last_host_position_y;
  m_last_host_position_x = mouse_x;
  m_last_host_position_y = mouse_y;

  if (delta_x != 0 || delta_y != 0)
    Log_DevPrintf("dx=%d, dy=%d", delta_x, delta_y);

  m_delta_x = static_cast<s8>(std::clamp<s32>(delta_x, std::numeric_limits<s8>::min(), std::numeric_limits<s8>::max()));
  m_delta_y = static_cast<s8>(std::clamp<s32>(delta_y, std::numeric_limits<s8>::min(), std::numeric_limits<s8>::max()));
}

std::unique_ptr<PlayStationMouse> PlayStationMouse::Create()
{
  return std::make_unique<PlayStationMouse>();
}

std::optional<s32> PlayStationMouse::StaticGetAxisCodeByName(std::string_view button_name)
{
  return std::nullopt;
}

std::optional<s32> PlayStationMouse::StaticGetButtonCodeByName(std::string_view button_name)
{
#define BUTTON(name)                                                                                                   \
  if (button_name == #name)                                                                                            \
  {                                                                                                                    \
    return static_cast<s32>(ZeroExtend32(static_cast<u8>(Button::name)));                                              \
  }

  BUTTON(Left);
  BUTTON(Right);

  return std::nullopt;

#undef BUTTON
}

Controller::AxisList PlayStationMouse::StaticGetAxisNames()
{
  return {};
}

Controller::ButtonList PlayStationMouse::StaticGetButtonNames()
{
  return {{TRANSLATABLE("PlayStationMouse", "Left"), static_cast<s32>(Button::Left)},
          {TRANSLATABLE("PlayStationMouse", "Right"), static_cast<s32>(Button::Right)}};
}

u32 PlayStationMouse::StaticGetVibrationMotorCount()
{
  return 0;
}

Controller::SettingList PlayStationMouse::StaticGetSettings()
{
  static constexpr std::array<SettingInfo, 1> settings = {{
    {SettingInfo::Type::Boolean, "RelativeMouseMode", TRANSLATABLE("PlayStationMouse", "Relative Mouse Mode"),
     TRANSLATABLE("PlayStationMouse", "Locks the mouse cursor to the window, use for FPS games."), "false"},
  }};

  return SettingList(settings.begin(), settings.end());
}

void PlayStationMouse::LoadSettings(const char* section)
{
  Controller::LoadSettings(section);

  m_use_relative_mode = g_host_interface->GetBoolSettingValue(section, "RelativeMouseMode");
}

bool PlayStationMouse::GetSoftwareCursor(const Common::RGBA8Image** image, float* image_scale, bool* relative_mode)
{
  *relative_mode = m_use_relative_mode;
  return m_use_relative_mode;
}
