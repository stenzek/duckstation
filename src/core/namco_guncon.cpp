#include "namco_guncon.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/state_wrapper.h"
#include "gpu.h"
#include "imgui.h"
#include "system.h"
#include <array>
Log_SetChannel(NamcoGunCon);

NamcoGunCon::NamcoGunCon(System* system) : m_system(system) {}

NamcoGunCon::~NamcoGunCon() = default;

ControllerType NamcoGunCon::GetType() const
{
  return ControllerType::NamcoGunCon;
}

std::optional<s32> NamcoGunCon::GetAxisCodeByName(std::string_view axis_name) const
{
  return StaticGetAxisCodeByName(axis_name);
}

std::optional<s32> NamcoGunCon::GetButtonCodeByName(std::string_view button_name) const
{
  return StaticGetButtonCodeByName(button_name);
}

void NamcoGunCon::Reset()
{
  m_transfer_state = TransferState::Idle;
}

bool NamcoGunCon::DoState(StateWrapper& sw)
{
  if (!Controller::DoState(sw))
    return false;

  sw.Do(&m_button_state);
  sw.Do(&m_position_x);
  sw.Do(&m_position_y);
  sw.Do(&m_transfer_state);
  return true;
}

void NamcoGunCon::SetAxisState(s32 axis_code, float value) {}

void NamcoGunCon::SetButtonState(Button button, bool pressed)
{
  static constexpr std::array<u8, static_cast<size_t>(Button::Count)> indices = {{13, 3, 14}};
  if (pressed)
    m_button_state &= ~(u16(1) << indices[static_cast<u8>(button)]);
  else
    m_button_state |= u16(1) << indices[static_cast<u8>(button)];
}

void NamcoGunCon::SetButtonState(s32 button_code, bool pressed)
{
  if (button_code < 0 || button_code >= static_cast<s32>(Button::Count))
    return;

  SetButtonState(static_cast<Button>(button_code), pressed);
}

void NamcoGunCon::ResetTransferState()
{
  m_transfer_state = TransferState::Idle;
}

bool NamcoGunCon::Transfer(const u8 data_in, u8* data_out)
{
  static constexpr u16 ID = 0x5A63;

  switch (m_transfer_state)
  {
    case TransferState::Idle:
    {
      // ack when sent 0x01, send ID for 0x42
      if (data_in == 0x42)
      {
        *data_out = Truncate8(ID);
        m_transfer_state = TransferState::IDMSB;
        return true;
      }
      else
      {
        *data_out = 0xFF;
        return (data_in == 0x01);
      }
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
      m_transfer_state = TransferState::XLSB;
      return true;
    }

    case TransferState::XLSB:
    {
      UpdatePosition();
      *data_out = Truncate8(m_position_x);
      m_transfer_state = TransferState::XMSB;
      return true;
    }

    case TransferState::XMSB:
    {
      *data_out = Truncate8(m_position_x >> 8);
      m_transfer_state = TransferState::YLSB;
      return true;
    }

    case TransferState::YLSB:
    {
      *data_out = Truncate8(m_position_y);
      m_transfer_state = TransferState::YMSB;
      return true;
    }

    case TransferState::YMSB:
    {
      *data_out = Truncate8(m_position_y >> 8);
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

void NamcoGunCon::UpdatePosition()
{
  // get screen coordinates. TODO better way
  const s32 mouse_x = static_cast<s32>(ImGui::GetIO().MousePos.x);
  const s32 mouse_y = static_cast<s32>(ImGui::GetIO().MousePos.y);

  // are we within the active display area?
  u32 tick, line;
  if (mouse_x < 0 || mouse_y < 0 ||
      !m_system->GetGPU()->ConvertScreenCoordinatesToBeamTicksAndLines(mouse_x, mouse_y, &tick, &line))
  {
    Log_DebugPrintf("Lightgun out of range for window coordinates %d,%d", mouse_x, mouse_y);
    m_position_x = 0x01;
    m_position_y = 0x0A;
    return;
  }

  // 8MHz units for X = 44100*768*11/7 = 53222400 / 8000000 = 6.6528
  m_position_x = static_cast<u16>(static_cast<float>(tick) * (1.0f / 6.6528f));
  m_position_y = static_cast<u16>(line);
  Log_DebugPrintf("Lightgun window coordinates %d,%d -> tick %u line %u 8mhz ticks %u", tick, line, m_position_x);
}

std::unique_ptr<NamcoGunCon> NamcoGunCon::Create(System* system)
{
  return std::make_unique<NamcoGunCon>(system);
}

std::optional<s32> NamcoGunCon::StaticGetAxisCodeByName(std::string_view button_name)
{
  return std::nullopt;
}

std::optional<s32> NamcoGunCon::StaticGetButtonCodeByName(std::string_view button_name)
{
#define BUTTON(name)                                                                                                   \
  if (button_name == #name)                                                                                            \
  {                                                                                                                    \
    return static_cast<s32>(ZeroExtend32(static_cast<u8>(Button::name)));                                              \
  }

  BUTTON(Trigger);
  BUTTON(A);
  BUTTON(B);

  return std::nullopt;

#undef BUTTON
}

Controller::AxisList NamcoGunCon::StaticGetAxisNames()
{
  return {};
}

Controller::ButtonList NamcoGunCon::StaticGetButtonNames()
{
#define B(n)                                                                                                           \
  {                                                                                                                    \
#n, static_cast < s32>(Button::n)                                                                                  \
  }
  return {B(Trigger), B(A), B(B)};
#undef B
}

u32 NamcoGunCon::StaticGetVibrationMotorCount()
{
  return 0;
}
