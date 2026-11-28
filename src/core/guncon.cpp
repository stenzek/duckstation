// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "guncon.h"
#include "gpu.h"
#include "system.h"

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

GunCon::GunCon(u32 index) : LightgunController(index, ControllerType::GunCon, s_button_indices)
{
}

GunCon::~GunCon() = default;

void GunCon::Reset()
{
  ResetTransferState();
}

bool GunCon::DoState(StateWrapper& sw, bool apply_input_state)
{
  if ((sw.GetVersion() >= 105 && !Controller::DoState(sw, apply_input_state)) ||
      !LightgunController::DoState(sw, apply_input_state))
  {
    return false;
  }

  if (sw.IsWriting() || apply_input_state)
  {
    sw.Do(&m_position_x);
    sw.Do(&m_position_y);
  }
  else
  {
    sw.SkipBytes(sizeof(m_position_x) + sizeof(m_position_y));
  }

  if (sw.GetVersion() < 105)
    SkipLegacyTransferState(sw);

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

u8 GunCon::GetIDByte() const
{
  return 0x63;
}

bool GunCon::BeginCommand(u8 command)
{
  if (command != 0x42)
    return false;

  UpdatePosition();
  m_tx_buffer[2] = Truncate8(m_button_state);
  m_tx_buffer[3] = Truncate8(m_button_state >> 8);
  m_tx_buffer[4] = Truncate8(m_position_x);
  m_tx_buffer[5] = Truncate8(m_position_x >> 8);
  m_tx_buffer[6] = Truncate8(m_position_y);
  m_tx_buffer[7] = Truncate8(m_position_y >> 8);
  return true;
}

void GunCon::UpdatePosition()
{
  const Position pos = GetPosition();
  const GSVector2 display_pos(pos.display_x, pos.display_y);

  // are we within the active display area?
  s32 offset_tick, offset_line;
  TickCount remainder = 0;
  if (!pos.valid ||
      (offset_tick = System::UnscaleTicksToOverclock(static_cast<TickCount>(pos.tick), &remainder) + m_tick_offset) <
        0 ||
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
#define BUTTON(name, display_name, icon_name, image_element, binding, genb)                                            \
  {name, display_name, icon_name, image_element, static_cast<u32>(binding), InputBindingInfo::Type::Button, genb}
#define HALFAXIS(name, display_name, icon_name, binding, genb)                                                         \
  {name, display_name, icon_name, nullptr, static_cast<u32>(binding), InputBindingInfo::Type::HalfAxis, genb}

  // clang-format off
  {"Pointer", TRANSLATE_NOOP("GunCon", "Pointer/Aiming"), ICON_PF_MOUSE, nullptr, static_cast<u32>(GunCon::Binding::ButtonCount), InputBindingInfo::Type::Pointer, GenericInputBinding::Unknown},
  BUTTON("Trigger", TRANSLATE_NOOP("GunCon", "Trigger"), ICON_PF_CROSS, "Trigger", GunCon::Binding::Trigger, GenericInputBinding::R2),
  BUTTON("ShootOffscreen", TRANSLATE_NOOP("GunCon", "Shoot Offscreen"), nullptr, "Trigger", GunCon::Binding::ShootOffscreen, GenericInputBinding::L2),
  BUTTON("A", TRANSLATE_NOOP("GunCon", "A"), ICON_PF_BUTTON_A, nullptr, GunCon::Binding::A, GenericInputBinding::Cross),
  BUTTON("B", TRANSLATE_NOOP("GunCon", "B"), ICON_PF_BUTTON_B, "B", GunCon::Binding::B, GenericInputBinding::Circle),

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
   TRANSLATE_NOOP("GunCon", "Offset applied to lightgun vertical position."), "0", "-128", "127", "1", "%d", nullptr,
   0.0f},
  {SettingInfo::Type::Integer, "GunConTickOffset", TRANSLATE_NOOP("GunCon", "Tick Offset"),
   TRANSLATE_NOOP("GunCon", "Offset applied to lightgun horizontal position."), "-140", "-1000", "1000", "1", "%d",
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
