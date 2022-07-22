#include "playstation_mouse.h"
#include "common/assert.h"
#include "common/log.h"
#include "gpu.h"
#include "host.h"
#include "host_display.h"
#include "system.h"
#include "util/state_wrapper.h"
#include <array>
Log_SetChannel(PlayStationMouse);

static constexpr std::array<u8, static_cast<size_t>(PlayStationMouse::Button::Count)> s_button_indices = {{11, 10}};

PlayStationMouse::PlayStationMouse(u32 index) : Controller(index)
{
  m_last_host_position_x = g_host_display->GetMousePositionX();
  m_last_host_position_y = g_host_display->GetMousePositionY();
}

PlayStationMouse::~PlayStationMouse() = default;

ControllerType PlayStationMouse::GetType() const
{
  return ControllerType::PlayStationMouse;
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

float PlayStationMouse::GetBindState(u32 index) const
{
  if (index >= s_button_indices.size())
    return 0.0f;

  const u32 bit = s_button_indices[index];
  return static_cast<float>(((m_button_state >> bit) & 1u) ^ 1u);
}

void PlayStationMouse::SetBindState(u32 index, float value)
{
  if (index > s_button_indices.size())
    return;

  if (value >= 0.5f)
    m_button_state &= ~(u16(1) << s_button_indices[index]);
  else
    m_button_state |= u16(1) << s_button_indices[index];
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
  const s32 mouse_x = g_host_display->GetMousePositionX();
  const s32 mouse_y = g_host_display->GetMousePositionY();
  const s32 delta_x = mouse_x - m_last_host_position_x;
  const s32 delta_y = mouse_y - m_last_host_position_y;
  m_last_host_position_x = mouse_x;
  m_last_host_position_y = mouse_y;

  if (delta_x != 0 || delta_y != 0)
    Log_DevPrintf("dx=%d, dy=%d", delta_x, delta_y);

  m_delta_x = static_cast<s8>(std::clamp<s32>(delta_x, std::numeric_limits<s8>::min(), std::numeric_limits<s8>::max()));
  m_delta_y = static_cast<s8>(std::clamp<s32>(delta_y, std::numeric_limits<s8>::min(), std::numeric_limits<s8>::max()));
}

std::unique_ptr<PlayStationMouse> PlayStationMouse::Create(u32 index)
{
  return std::make_unique<PlayStationMouse>(index);
}

static const Controller::ControllerBindingInfo s_binding_info[] = {
#define BUTTON(name, display_name, button, genb)                                                                       \
  {                                                                                                                    \
    name, display_name, static_cast<u32>(button), Controller::ControllerBindingType::Button, genb                      \
  }

  BUTTON("Left", "Left Button", PlayStationMouse::Button::Left, GenericInputBinding::Cross),
  BUTTON("Right", "Right Button", PlayStationMouse::Button::Right, GenericInputBinding::Circle),

#undef BUTTON
};

static const SettingInfo s_settings[] = {
  {SettingInfo::Type::Boolean, "RelativeMouseMode", TRANSLATABLE("PlayStationMouse", "Relative Mouse Mode"),
   TRANSLATABLE("PlayStationMouse", "Locks the mouse cursor to the window, use for FPS games."), "false"},
};

const Controller::ControllerInfo PlayStationMouse::INFO = {ControllerType::PlayStationMouse,
                                                           "PlayStationMouse",
                                                           TRANSLATABLE("ControllerType", "PlayStation Mouse"),
                                                           s_binding_info,
                                                           countof(s_binding_info),
                                                           s_settings,
                                                           countof(s_settings),
                                                           Controller::VibrationCapabilities::NoVibration};

void PlayStationMouse::LoadSettings(SettingsInterface& si, const char* section)
{
  Controller::LoadSettings(si, section);

  m_use_relative_mode = si.GetBoolValue(section, "RelativeMouseMode", false);
}

bool PlayStationMouse::GetSoftwareCursor(const Common::RGBA8Image** image, float* image_scale, bool* relative_mode)
{
  *relative_mode = m_use_relative_mode;
  return m_use_relative_mode;
}
