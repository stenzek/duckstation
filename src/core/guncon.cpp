// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "guncon.h"
#include "gpu.h"

#include "util/state_wrapper.h"
#include "util/translation.h"

#include "common/assert.h"
#include "common/gsvector_formatter.h"
#include "common/log.h"
#include "common/path.h"
#include "common/settings_interface.h"

#include "IconsPromptFont.h"

#include <array>

LOG_CHANNEL(Controller);

static constexpr std::array<u8, static_cast<size_t>(GunCon::Binding::ButtonCount)> s_button_indices = {{13, 3, 14, 0}};

GunCon::GunCon(u32 index) : LightgunController(index, s_button_indices)
{
}

GunCon::~GunCon() = default;

ControllerType GunCon::GetType() const
{
  return ControllerType::GunCon;
}

void GunCon::Reset()
{
  m_transfer_state = TransferState::Idle;
}

bool GunCon::DoState(StateWrapper& sw, bool apply_input_state)
{
  if (!LightgunController::DoState(sw, apply_input_state))
    return false;

  u16 position_x = m_position_x;
  u16 position_y = m_position_y;
  sw.Do(&position_x);
  sw.Do(&position_y);
  if (apply_input_state)
  {
    m_position_x = position_x;
    m_position_y = position_y;
  }

  sw.Do(&m_transfer_state, TransferState::YMSB);
  return true;
}

void GunCon::SetShootOffscreen(bool pressed)
{
  if (m_shoot_offscreen != pressed)
  {
    m_shoot_offscreen = pressed;
    SetBindState(static_cast<u32>(Binding::Trigger), pressed);
  }
}

void GunCon::ResetTransferState()
{
  m_transfer_state = TransferState::Idle;
}

bool GunCon::Transfer(const u8 data_in, u8* data_out)
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
      UnreachableCode();
  }
}

void GunCon::UpdatePosition()
{
  const Position pos = GetPosition();
  const GSVector2 display_pos(pos.display_x, pos.display_y);

  // are we within the active display area?
  s32 offset_tick, offset_line;
  if (!pos.valid || (offset_tick = static_cast<s32>(pos.tick) + m_tick_offset) < 0 ||
      (offset_line = static_cast<s32>(pos.line) + m_line_offset) < 0 || m_shoot_offscreen)
  {
    DEV_LOG("Lightgun out of range for window coordinates {:.0f},{:.0f}", pos.window_x, pos.window_y);
    m_position_x = 0x01;
    m_position_y = 0x0A;
    return;
  }

  // 8MHz units for X = 44100*768*11/7 = 53222400 / 8000000 = 6.6528
  const double divider = static_cast<double>(GPU::GetCRTCFrequency()) / 8000000.0;
  m_position_x = static_cast<u16>(static_cast<float>(offset_tick) / static_cast<float>(divider));
  m_position_y = static_cast<u16>(offset_line);
  DEV_LOG("Lightgun window coordinates {} -> tick {} line {} 8mhz ticks {}", display_pos, offset_tick, offset_line,
          m_position_x);
}

std::unique_ptr<GunCon> GunCon::Create(u32 index)
{
  return std::make_unique<GunCon>(index);
}

static const Controller::ControllerBindingInfo s_binding_info[] = {
#define BUTTON(name, display_name, icon_name, binding, genb)                                                           \
  {name, display_name, icon_name, static_cast<u32>(binding), InputBindingInfo::Type::Button, genb}
#define HALFAXIS(name, display_name, icon_name, binding, genb)                                                         \
  {name, display_name, icon_name, static_cast<u32>(binding), InputBindingInfo::Type::HalfAxis, genb}

  // clang-format off
  {"Pointer", TRANSLATE_NOOP("GunCon", "Pointer/Aiming"), ICON_PF_MOUSE, static_cast<u32>(GunCon::Binding::ButtonCount), InputBindingInfo::Type::Pointer, GenericInputBinding::Unknown},
  BUTTON("Trigger", TRANSLATE_NOOP("GunCon", "Trigger"), ICON_PF_CROSS, GunCon::Binding::Trigger, GenericInputBinding::R2),
  BUTTON("ShootOffscreen", TRANSLATE_NOOP("GunCon", "Shoot Offscreen"), nullptr, GunCon::Binding::ShootOffscreen, GenericInputBinding::L2),
  BUTTON("A", TRANSLATE_NOOP("GunCon", "A"), ICON_PF_BUTTON_A, GunCon::Binding::A, GenericInputBinding::Cross),
  BUTTON("B", TRANSLATE_NOOP("GunCon", "B"), ICON_PF_BUTTON_B, GunCon::Binding::B, GenericInputBinding::Circle),

  HALFAXIS("RelativeLeft", TRANSLATE_NOOP("GunCon", "Relative Left"), ICON_PF_ANALOG_LEFT, GunCon::Binding::RelativeLeft, GenericInputBinding::Unknown),
  HALFAXIS("RelativeRight", TRANSLATE_NOOP("GunCon", "Relative Right"), ICON_PF_ANALOG_RIGHT, GunCon::Binding::RelativeRight, GenericInputBinding::Unknown),
  HALFAXIS("RelativeUp", TRANSLATE_NOOP("GunCon", "Relative Up"), ICON_PF_ANALOG_UP, GunCon::Binding::RelativeUp, GenericInputBinding::Unknown),
  HALFAXIS("RelativeDown", TRANSLATE_NOOP("GunCon", "Relative Down"), ICON_PF_ANALOG_DOWN, GunCon::Binding::RelativeDown, GenericInputBinding::Unknown),
// clang-format on

#undef BUTTON
};

static constexpr const char* DEFAULT_CROSSHAIR_PATH = "images" FS_OSPATH_SEPARATOR_STR "crosshair.png";

static const SettingInfo s_settings[] = {
  {SettingInfo::Type::Path, "CrosshairImagePath", TRANSLATE_NOOP("GunCon", "Crosshair Image Path"),
   TRANSLATE_NOOP("GunCon", "Path to an image to use as a crosshair/cursor."), DEFAULT_CROSSHAIR_PATH, nullptr, nullptr,
   nullptr, nullptr, nullptr, 0.0f},
  {SettingInfo::Type::Float, "CrosshairScale", TRANSLATE_NOOP("GunCon", "Crosshair Image Scale"),
   TRANSLATE_NOOP("GunCon", "Scale of crosshair image on screen."), "1", "0.0001", "100", "0.1", "%.0f%%", nullptr,
   100.0f},
  {SettingInfo::Type::String, "CrosshairColor", TRANSLATE_NOOP("GunCon", "Cursor Color"),
   TRANSLATE_NOOP("GunCon", "Applies a color to the chosen crosshair images, can be used for multiple players. Specify "
                            "in HTML/CSS format (e.g. #aabbcc)"),
   "#ffffff", nullptr, nullptr, nullptr, nullptr, nullptr, 0.0f},
  {SettingInfo::Type::Float, "XScale", TRANSLATE_NOOP("GunCon", "X Scale"),
   TRANSLATE_NOOP("GunCon", "Scales X coordinates relative to the center of the screen."), "1.0", "0.01", "2.0", "0.01",
   "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Integer, "GunConLineOffset", TRANSLATE_NOOP("GunCon", "Line Offset"),
   TRANSLATE_NOOP("GunCon", "Offset applied to lightgun vertical position."), "0", "-128", "127", "1", "%u", nullptr,
   0.0f},
  {SettingInfo::Type::Integer, "GunConTickOffset", TRANSLATE_NOOP("GunCon", "Tick Offset"),
   TRANSLATE_NOOP("GunCon", "Offset applied to lightgun horizontal position."), "-140", "-1000", "1000", "1", "%u",
   nullptr, 0.0f},
};

const Controller::ControllerInfo GunCon::INFO = {ControllerType::GunCon,
                                                 "GunCon",
                                                 TRANSLATE_NOOP("ControllerType", "GunCon"),
                                                 ICON_PF_LIGHT_GUN,
                                                 "images/controllers/guncon.svg",
                                                 s_binding_info,
                                                 s_settings};

void GunCon::LoadSettings(const SettingsInterface& si, const char* section, bool initial)
{
  LightgunController::LoadSettings(si, section, initial);

  // NOTE: Settings are prefixed to avoid conflicting with Justifier, since stupid me in 2020 thought it was a good idea
  // to have all controllers sharing the same configuration section.
  m_line_offset = static_cast<s8>(std::clamp<int>(si.GetIntValue(section, "GunConLineOffset", DEFAULT_LINE_OFFSET),
                                                  std::numeric_limits<s8>::min(), std::numeric_limits<s8>::max()));
  m_tick_offset = static_cast<s16>(std::clamp<int>(si.GetIntValue(section, "GunConTickOffset", DEFAULT_TICK_OFFSET),
                                                   std::numeric_limits<s16>::min(), std::numeric_limits<s16>::max()));
}
