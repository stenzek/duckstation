#include "namco_guncon.h"
#include "common/assert.h"
#include "common/log.h"
#include "gpu.h"
#include "host_display.h"
#include "host_interface.h"
#include "resources.h"
#include "system.h"
#include "util/state_wrapper.h"
#include <array>
Log_SetChannel(NamcoGunCon);

NamcoGunCon::NamcoGunCon() = default;

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

bool NamcoGunCon::DoState(StateWrapper& sw, bool apply_input_state)
{
  if (!Controller::DoState(sw, apply_input_state))
    return false;

  u16 button_state = m_button_state;
  u16 position_x = m_position_x;
  u16 position_y = m_position_y;
  sw.Do(&button_state);
  sw.Do(&position_x);
  sw.Do(&position_y);
  if (apply_input_state)
  {
    m_button_state = button_state;
    m_position_x = position_x;
    m_position_y = position_y;
  }

  sw.Do(&m_transfer_state);
  return true;
}

bool NamcoGunCon::GetButtonState(s32 button_code) const
{
  if (button_code < 0 || button_code > static_cast<s32>(Button::B))
    return false;

  const u16 bit = u16(1) << static_cast<u8>(button_code);
  return ((m_button_state & bit) == 0);
}

void NamcoGunCon::SetButtonState(Button button, bool pressed)
{
  if (button == Button::ShootOffscreen)
  {
    if (m_shoot_offscreen != pressed)
    {
      m_shoot_offscreen = pressed;
      SetButtonState(Button::Trigger, pressed);
    }

    return;
  }

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
  // get screen coordinates
  const HostDisplay* display = g_host_interface->GetDisplay();
  const s32 mouse_x = display->GetMousePositionX();
  const s32 mouse_y = display->GetMousePositionY();

  // are we within the active display area?
  u32 tick, line;
  if (mouse_x < 0 || mouse_y < 0 ||
      !g_gpu->ConvertScreenCoordinatesToBeamTicksAndLines(mouse_x, mouse_y, m_x_scale, &tick, &line) ||
      m_shoot_offscreen)
  {
    Log_DebugPrintf("Lightgun out of range for window coordinates %d,%d", mouse_x, mouse_y);
    m_position_x = 0x01;
    m_position_y = 0x0A;
    return;
  }

  // 8MHz units for X = 44100*768*11/7 = 53222400 / 8000000 = 6.6528
  const double divider = static_cast<double>(g_gpu->GetCRTCFrequency()) / 8000000.0;
  m_position_x = static_cast<u16>(static_cast<float>(tick) / static_cast<float>(divider));
  m_position_y = static_cast<u16>(line);
  Log_DebugPrintf("Lightgun window coordinates %d,%d -> tick %u line %u 8mhz ticks %u", mouse_x, mouse_y, tick, line,
                  m_position_x);
}

std::unique_ptr<NamcoGunCon> NamcoGunCon::Create()
{
  return std::make_unique<NamcoGunCon>();
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
  BUTTON(ShootOffscreen);
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
  return {{TRANSLATABLE("NamcoGunCon", "Trigger"), static_cast<s32>(Button::Trigger)},
          {TRANSLATABLE("NamcoGunCon", "ShootOffscreen"), static_cast<s32>(Button::ShootOffscreen)},
          {TRANSLATABLE("NamcoGunCon", "A"), static_cast<s32>(Button::A)},
          {TRANSLATABLE("NamcoGunCon", "B"), static_cast<s32>(Button::B)}};
}

u32 NamcoGunCon::StaticGetVibrationMotorCount()
{
  return 0;
}

Controller::SettingList NamcoGunCon::StaticGetSettings()
{
  static constexpr std::array<SettingInfo, 3> settings = {
    {{SettingInfo::Type::Path, "CrosshairImagePath", TRANSLATABLE("NamcoGunCon", "Crosshair Image Path"),
      TRANSLATABLE("NamcoGunCon", "Path to an image to use as a crosshair/cursor.")},
     {SettingInfo::Type::Float, "CrosshairScale", TRANSLATABLE("NamcoGunCon", "Crosshair Image Scale"),
      TRANSLATABLE("NamcoGunCon", "Scale of crosshair image on screen."), "1.0", "0.0001", "100.0", "0.10"},
     {SettingInfo::Type::Float, "XScale", TRANSLATABLE("NamcoGunCon", "X Scale"),
      TRANSLATABLE("NamcoGunCon", "Scales X coordinates relative to the center of the screen."), "1.0", "0.01", "2.0",
      "0.01"}}};

  return SettingList(settings.begin(), settings.end());
}

void NamcoGunCon::LoadSettings(const char* section)
{
  Controller::LoadSettings(section);

  std::string path = g_host_interface->GetStringSettingValue(section, "CrosshairImagePath");
  if (path != m_crosshair_image_path)
  {
    m_crosshair_image_path = std::move(path);
    if (m_crosshair_image_path.empty() ||
        !m_crosshair_image.LoadFromFile(m_crosshair_image_path.c_str()))
    {
      m_crosshair_image.Invalidate();
    }
  }

#ifndef __ANDROID__
  if (!m_crosshair_image.IsValid())
  {
    m_crosshair_image.SetPixels(Resources::CROSSHAIR_IMAGE_WIDTH, Resources::CROSSHAIR_IMAGE_HEIGHT,
                                Resources::CROSSHAIR_IMAGE_DATA.data());
  }
#endif

  m_crosshair_image_scale = g_host_interface->GetFloatSettingValue(section, "CrosshairScale", 1.0f);

  m_x_scale = g_host_interface->GetFloatSettingValue(section, "XScale", 1.0f);
}

bool NamcoGunCon::GetSoftwareCursor(const Common::RGBA8Image** image, float* image_scale, bool* relative_mode)
{
  if (!m_crosshair_image.IsValid())
    return false;

  *image = &m_crosshair_image;
  *image_scale = m_crosshair_image_scale;
  *relative_mode = false;
  return true;
}
