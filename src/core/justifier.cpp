// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "justifier.h"
#include "gpu.h"
#include "interrupt_controller.h"
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

// #define CHECK_TIMING 1
#ifdef CHECK_TIMING
static u32 s_irq_current_line;
#endif

static constexpr std::array<u8, static_cast<size_t>(Justifier::Binding::ButtonCount)> s_button_indices = {
  {15, 3, 14, 0}};
static constexpr std::array<const char*, NUM_CONTROLLER_AND_CARD_PORTS> s_event_names = {
  {"Justifier IRQ P0", "Justifier IRQ P1", "Justifier IRQ P2", "Justifier IRQ P3", "Justifier IRQ P4",
   "Justifier IRQ P5", "Justifier IRQ P6", "Justifier IRQ P7"}};

Justifier::Justifier(u32 index)
  : LightgunController(index, s_button_indices),
    m_irq_event(
      s_event_names[index], 1, 1, [](void* param, TickCount) { static_cast<Justifier*>(param)->IRQEvent(); }, this)
{
}

Justifier::~Justifier()
{
  m_irq_event.Deactivate();
}

ControllerType Justifier::GetType() const
{
  return ControllerType::Justifier;
}

void Justifier::Reset()
{
  m_transfer_state = TransferState::Idle;
}

bool Justifier::DoState(StateWrapper& sw, bool apply_input_state)
{
  u16 irq_first_line = m_irq_first_line;
  u16 irq_last_line = m_irq_last_line;
  u16 irq_tick = m_irq_tick;
  bool shoot_offscreen = m_shoot_offscreen;
  bool position_valid = m_position_valid;

  sw.Do(&irq_first_line);
  sw.Do(&irq_last_line);
  sw.Do(&irq_tick);
  if (!LightgunController::DoState(sw, apply_input_state))
    return false;
  sw.Do(&shoot_offscreen);
  sw.Do(&position_valid);

  if (apply_input_state)
  {
    m_irq_first_line = irq_first_line;
    m_irq_last_line = irq_last_line;
    m_irq_tick = irq_tick;
    m_shoot_offscreen = shoot_offscreen;
    m_position_valid = position_valid;
  }

  sw.DoEx(&m_irq_enabled, 82, true);
  sw.Do(&m_transfer_state, TransferState::YMSB);

  if (sw.IsReading())
    UpdateIRQEvent();

  return true;
}

void Justifier::SetShootOffscreen(bool pressed)
{
  if (pressed)
    m_shoot_offscreen = m_shoot_offscreen ? m_shoot_offscreen : m_offscreen_oob_frames;
}

void Justifier::ResetTransferState()
{
  m_transfer_state = TransferState::Idle;
}

bool Justifier::Transfer(const u8 data_in, u8* data_out)
{
  static constexpr u16 ID = 0x5A31;

  switch (m_transfer_state)
  {
    case TransferState::Idle:
    {
      // ack when sent 0x01, send ID for 0x42
      if (data_in == 0x42)
      {
        *data_out = Truncate8(ID);
        m_transfer_state = TransferState::IDMSB;
        UpdatePosition();
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
      const bool new_irq_enabled = ((data_in & 0x10) == 0x10);
      if (new_irq_enabled != m_irq_enabled)
      {
        m_irq_enabled = new_irq_enabled;
        UpdateIRQEvent();
      }

      *data_out = Truncate8(m_button_state);
      m_transfer_state = TransferState::ButtonsMSB;
      return true;
    }

    case TransferState::ButtonsMSB:
    {
      *data_out = Truncate8(m_button_state >> 8);
      m_transfer_state = TransferState::Idle;
      return false;
    }

    default:
    {
      UnreachableCode();
    }
  }
}

void Justifier::UpdatePosition()
{
  if (m_shoot_offscreen > 0)
  {
    if (m_shoot_offscreen == m_offscreen_trigger_frames)
      SetBindState(static_cast<u32>(Binding::Trigger), 1.0f);
    else if (m_shoot_offscreen == m_offscreen_release_frames)
      SetBindState(static_cast<u32>(Binding::Trigger), 0.0f);

    m_shoot_offscreen--;
    m_position_valid = false;
    UpdateIRQEvent();
    return;
  }

  const Position pos = GetPosition();
  const GSVector2 display_pos(pos.display_x, pos.display_y);

  // are we within the active display area?
  if (!pos.valid || m_shoot_offscreen)
  {
    DEV_LOG("Lightgun out of range for window coordinates {:.0f},{:.0f}", pos.window_x, pos.window_y);
    m_position_valid = false;
    UpdateIRQEvent();
    return;
  }

  m_position_valid = true;

  m_irq_tick = static_cast<u16>(static_cast<TickCount>(pos.tick) +
                                System::ScaleTicksToOverclock(static_cast<TickCount>(m_tick_offset)));
  m_irq_first_line = static_cast<u16>(std::clamp<s32>(static_cast<s32>(pos.line) + m_first_line_offset,
                                                      static_cast<s32>(GPU::GetCRTCActiveStartLine()),
                                                      static_cast<s32>(GPU::GetCRTCActiveEndLine())));
  m_irq_last_line = static_cast<u16>(std::clamp<s32>(static_cast<s32>(pos.line) + m_last_line_offset,
                                                     static_cast<s32>(GPU::GetCRTCActiveStartLine()),
                                                     static_cast<s32>(GPU::GetCRTCActiveEndLine())));

  DEV_LOG("Lightgun window coordinates {},{} -> dpy {} -> tick {} line {} [{}-{}]", pos.window_x, pos.window_y,
          display_pos, pos.tick, pos.line, m_irq_first_line, m_irq_last_line);

  UpdateIRQEvent();
}

void Justifier::UpdateIRQEvent()
{
  // TODO: Avoid deactivate and event sort.
  m_irq_event.Deactivate();

  if (!m_position_valid || !m_irq_enabled)
    return;

  u32 current_tick, current_line;
  GPU::GetBeamPosition(&current_tick, &current_line);

  u32 target_line;
  if (current_line < m_irq_first_line || current_line >= m_irq_last_line)
    target_line = m_irq_first_line;
  else
    target_line = current_line + 1;

  const TickCount ticks_until_pos = GPU::GetSystemTicksUntilTicksAndLine(m_irq_tick, target_line);
  DEBUG_LOG("Triggering IRQ in {} ticks @ tick {} line {}", ticks_until_pos, m_irq_tick, target_line);
  m_irq_event.Schedule(ticks_until_pos);
}

void Justifier::IRQEvent()
{
#ifdef CHECK_TIMING
  u32 ticks, line;
  g_gpu->GetBeamPosition(&ticks, &line);

  const u32 expected_line = (s_irq_current_line == m_irq_last_line) ? m_irq_first_line : (s_irq_current_line + 1);
  if (line < expected_line)
    WARNING_LOG("IRQ event fired {} lines too early", expected_line - line);
  else if (line > expected_line)
    WARNING_LOG("IRQ event fired {} lines too late", line - expected_line);
  if (ticks < m_irq_tick)
    WARNING_LOG("IRQ event fired {} ticks too early", m_irq_tick - ticks);
  else if (ticks > m_irq_tick)
    WARNING_LOG("IRQ event fired {} ticks too late", ticks - m_irq_tick);
  s_irq_current_line = line;
#endif

  InterruptController::SetLineState(InterruptController::IRQ::IRQ10, true);
  InterruptController::SetLineState(InterruptController::IRQ::IRQ10, false);

  UpdateIRQEvent();
}

std::unique_ptr<Justifier> Justifier::Create(u32 index)
{
  return std::make_unique<Justifier>(index);
}

static const Controller::ControllerBindingInfo s_binding_info[] = {
#define BUTTON(name, display_name, icon_name, binding, genb)                                                           \
  {name, display_name, icon_name, static_cast<u32>(binding), InputBindingInfo::Type::Button, genb}
#define HALFAXIS(name, display_name, icon_name, binding, genb)                                                         \
  {name, display_name, icon_name, static_cast<u32>(binding), InputBindingInfo::Type::HalfAxis, genb}

  // clang-format off
  {"Pointer", TRANSLATE_NOOP("Justifier", "Pointer/Aiming"), ICON_PF_MOUSE, static_cast<u32>(Justifier::Binding::ButtonCount), InputBindingInfo::Type::Pointer, GenericInputBinding::Unknown},
  BUTTON("Trigger", TRANSLATE_NOOP("Justifier", "Trigger"), ICON_PF_CROSS, Justifier::Binding::Trigger, GenericInputBinding::R2),
  BUTTON("ShootOffscreen", TRANSLATE_NOOP("Justifier", "Shoot Offscreen"), nullptr, Justifier::Binding::ShootOffscreen, GenericInputBinding::L2),
  BUTTON("Start", TRANSLATE_NOOP("Justifier", "Start"), ICON_PF_START, Justifier::Binding::Start, GenericInputBinding::Cross),
  BUTTON("Back", TRANSLATE_NOOP("Justifier", "Back"), ICON_PF_BACK, Justifier::Binding::Back, GenericInputBinding::Circle),

  HALFAXIS("RelativeLeft", TRANSLATE_NOOP("Justifier", "Relative Left"), ICON_PF_ANALOG_LEFT, Justifier::Binding::RelativeLeft, GenericInputBinding::Unknown),
  HALFAXIS("RelativeRight", TRANSLATE_NOOP("Justifier", "Relative Right"), ICON_PF_ANALOG_RIGHT, Justifier::Binding::RelativeRight, GenericInputBinding::Unknown),
  HALFAXIS("RelativeUp", TRANSLATE_NOOP("Justifier", "Relative Up"), ICON_PF_ANALOG_UP, Justifier::Binding::RelativeUp, GenericInputBinding::Unknown),
  HALFAXIS("RelativeDown", TRANSLATE_NOOP("Justifier", "Relative Down"), ICON_PF_ANALOG_DOWN, Justifier::Binding::RelativeDown, GenericInputBinding::Unknown),
// clang-format on

#undef BUTTON
};

static constexpr const char* DEFAULT_CROSSHAIR_PATH = "images" FS_OSPATH_SEPARATOR_STR "crosshair.png";

static const SettingInfo s_settings[] = {
  {SettingInfo::Type::Path, "CrosshairImagePath", TRANSLATE_NOOP("Justifier", "Crosshair Image Path"),
   TRANSLATE_NOOP("Justifier", "Path to an image to use as a crosshair/cursor."), DEFAULT_CROSSHAIR_PATH, nullptr,
   nullptr, nullptr, nullptr, nullptr, 0.0f},
  {SettingInfo::Type::Float, "CrosshairScale", TRANSLATE_NOOP("Justifier", "Crosshair Image Scale"),
   TRANSLATE_NOOP("Justifier", "Scale of crosshair image on screen."), "1", "0.0001", "100", "0.1", "%.0f%%", nullptr,
   100.0f},
  {SettingInfo::Type::String, "CrosshairColor", TRANSLATE_NOOP("Justifier", "Cursor Color"),
   TRANSLATE_NOOP("Justifier",
                  "Applies a color to the chosen crosshair images, can be used for multiple players. Specify "
                  "in HTML/CSS format (e.g. #aabbcc)"),
   "#ffffff", nullptr, nullptr, nullptr, nullptr, nullptr, 0.0f},
  {SettingInfo::Type::Float, "XScale", TRANSLATE_NOOP("Justifier", "X Scale"),
   TRANSLATE_NOOP("Justifier", "Scales X coordinates relative to the center of the screen."), "1", "0.01", "2", "0.01",
   "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Integer, "FirstLineOffset", TRANSLATE_NOOP("Justifier", "Line Start Offset"),
   TRANSLATE_NOOP("Justifier",
                  "Offset applied to lightgun vertical position that the Justifier will first trigger on."),
   "-12", "-128", "127", "1", "%u", nullptr, 0.0f},
  {SettingInfo::Type::Integer, "LastLineOffset", TRANSLATE_NOOP("Justifier", "Line End Offset"),
   TRANSLATE_NOOP("Justifier", "Offset applied to lightgun vertical position that the Justifier will last trigger on."),
   "-6", "-128", "127", "1", "%u", nullptr, 0.0f},
  {SettingInfo::Type::Integer, "TickOffset", TRANSLATE_NOOP("Justifier", "Tick Offset"),
   TRANSLATE_NOOP("Justifier", "Offset applied to lightgun horizontal position that the Justifier will trigger on."),
   "50", "-1000", "1000", "1", "%u", nullptr, 0.0f},
  {SettingInfo::Type::Integer, "OffscreenOOBFrames", TRANSLATE_NOOP("Justifier", "Off-Screen Out-Of-Bounds Frames"),
   TRANSLATE_NOOP("Justifier", "Number of frames that the Justifier is pointed out-of-bounds for an off-screen shot."),
   "5", "0", "80", "1", "%u", nullptr, 0.0f},
  {SettingInfo::Type::Integer, "OffscreenTriggerFrames", TRANSLATE_NOOP("Justifier", "Off-Screen Trigger Frames"),
   TRANSLATE_NOOP("Justifier", "Number of frames that the trigger is held for an off-screen shot."), "5", "0", "80",
   "1", "%u", nullptr, 0.0f},
  {SettingInfo::Type::Integer, "OffscreenReleaseFrames", TRANSLATE_NOOP("Justifier", "Off-Screen Trigger Frames"),
   TRANSLATE_NOOP("Justifier", "Number of frames that the Justifier is pointed out-of-bounds after the trigger is "
                               "released, for an off-screen shot."),
   "5", "0", "80", "1", "%u", nullptr, 0.0f},
};

const Controller::ControllerInfo Justifier::INFO = {ControllerType::Justifier,
                                                    "Justifier",
                                                    TRANSLATE_NOOP("ControllerType", "Justifier"),
                                                    ICON_PF_LIGHT_GUN,
                                                    "images/controllers/justifier.svg",
                                                    s_binding_info,
                                                    s_settings};

void Justifier::LoadSettings(const SettingsInterface& si, const char* section, bool initial)
{
  LightgunController::LoadSettings(si, section, initial);

  m_first_line_offset =
    static_cast<s8>(std::clamp<int>(si.GetIntValue(section, "FirstLineOffset", DEFAULT_FIRST_LINE_OFFSET),
                                    std::numeric_limits<s8>::min(), std::numeric_limits<s8>::max()));
  m_last_line_offset =
    static_cast<s8>(std::clamp<int>(si.GetIntValue(section, "LastLineOffset", DEFAULT_LAST_LINE_OFFSET),
                                    std::numeric_limits<s8>::min(), std::numeric_limits<s8>::max()));
  m_tick_offset = static_cast<s16>(std::clamp<int>(si.GetIntValue(section, "TickOffset", DEFAULT_TICK_OFFSET),
                                                   std::numeric_limits<s16>::min(), std::numeric_limits<s16>::max()));

  const s8 offscreen_oob_frames =
    static_cast<s8>(std::clamp<int>(si.GetIntValue(section, "OffscreenOOBFrames", DEFAULT_OFFSCREEN_OOB_FRAMES),
                                    std::numeric_limits<s8>::min(), std::numeric_limits<s8>::max()));
  const s8 offscreen_trigger_frames =
    static_cast<s8>(std::clamp<int>(si.GetIntValue(section, "OffscreenTriggerFrames", DEFAULT_OFFSCREEN_TRIGGER_FRAMES),
                                    std::numeric_limits<s8>::min(), std::numeric_limits<s8>::max()));
  const s8 offscreen_release_frames =
    static_cast<s8>(std::clamp<int>(si.GetIntValue(section, "OffscreenReleaseFrames", DEFAULT_OFFSCREEN_RELEASE_FRAMES),
                                    std::numeric_limits<s8>::min(), std::numeric_limits<s8>::max()));
  m_offscreen_oob_frames = offscreen_oob_frames + offscreen_trigger_frames + offscreen_release_frames;
  m_offscreen_trigger_frames = m_offscreen_oob_frames - offscreen_trigger_frames;
  m_offscreen_release_frames = m_offscreen_trigger_frames - offscreen_release_frames;
}
