// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "justifier.h"
#include "gpu.h"
#include "host.h"
#include "interrupt_controller.h"
#include "system.h"

#include "util/imgui_manager.h"
#include "util/input_manager.h"
#include "util/state_wrapper.h"

#include "common/assert.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"

#include "IconsPromptFont.h"
#include <array>

LOG_CHANNEL(Controller);

// #define CHECK_TIMING 1
#ifdef CHECK_TIMING
static u32 s_irq_current_line;
#endif

static constexpr std::array<u8, static_cast<size_t>(Justifier::Binding::ButtonCount)> s_button_indices = {{15, 3, 14}};
static constexpr std::array<const char*, NUM_CONTROLLER_AND_CARD_PORTS> s_event_names = {
  {"Justifier IRQ P0", "Justifier IRQ P1", "Justifier IRQ P2", "Justifier IRQ P3", "Justifier IRQ P4",
   "Justifier IRQ P5", "Justifier IRQ P6", "Justifier IRQ P7"}};

Justifier::Justifier(u32 index)
  : Controller(index), m_irq_event(
                         s_event_names[index], 1, 1,
                         [](void* param, TickCount, TickCount) { static_cast<Justifier*>(param)->IRQEvent(); }, this)
{
}

Justifier::~Justifier()
{
  m_irq_event.Deactivate();

  if (!m_cursor_path.empty())
  {
    const u32 cursor_index = GetSoftwarePointerIndex();
    if (cursor_index < InputManager::MAX_SOFTWARE_CURSORS)
      ImGuiManager::ClearSoftwareCursor(cursor_index);
  }
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
  if (!Controller::DoState(sw, apply_input_state))
    return false;

  u16 irq_first_line = m_irq_first_line;
  u16 irq_last_line = m_irq_last_line;
  u16 irq_tick = m_irq_tick;
  u16 button_state = m_button_state;
  bool shoot_offscreen = m_shoot_offscreen;
  bool position_valid = m_position_valid;

  sw.Do(&irq_first_line);
  sw.Do(&irq_last_line);
  sw.Do(&irq_tick);
  sw.Do(&button_state);
  sw.Do(&shoot_offscreen);
  sw.Do(&position_valid);

  if (apply_input_state)
  {
    m_irq_first_line = irq_first_line;
    m_irq_last_line = irq_last_line;
    m_irq_tick = irq_tick;
    m_button_state = button_state;
    m_shoot_offscreen = shoot_offscreen;
    m_position_valid = position_valid;
  }

  sw.DoEx(&m_irq_enabled, 82, true);
  sw.Do(&m_transfer_state);

  if (sw.IsReading())
    UpdateIRQEvent();

  return true;
}

float Justifier::GetBindState(u32 index) const
{
  if (index >= s_button_indices.size())
    return 0.0f;

  const u32 bit = s_button_indices[index];
  return static_cast<float>(((m_button_state >> bit) & 1u) ^ 1u);
}

void Justifier::SetBindState(u32 index, float value)
{
  const bool pressed = (value >= 0.5f);
  if (index == static_cast<u32>(Binding::ShootOffscreen))
  {
    if (pressed)
      m_shoot_offscreen = m_shoot_offscreen ? m_shoot_offscreen : m_offscreen_oob_frames;

    return;
  }
  else if (index >= static_cast<u32>(Binding::ButtonCount))
  {
    if (index >= static_cast<u32>(Binding::BindingCount) || !m_has_relative_binds)
      return;

    if (m_relative_pos[index - static_cast<u32>(Binding::RelativeLeft)] != value)
    {
      m_relative_pos[index - static_cast<u32>(Binding::RelativeLeft)] = value;
      UpdateSoftwarePointerPosition();
    }

    return;
  }

  if (pressed)
    m_button_state &= ~(u16(1) << s_button_indices[static_cast<u8>(index)]);
  else
    m_button_state |= u16(1) << s_button_indices[static_cast<u8>(index)];
}

bool Justifier::IsTriggerPressed() const
{
  return ((m_button_state & (1u << 15)) != 0);
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
      return true;
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

  float display_x, display_y;
  const auto [window_x, window_y] = (m_has_relative_binds) ? GetAbsolutePositionFromRelativeAxes() :
                                                             InputManager::GetPointerAbsolutePosition(m_cursor_index);
  g_gpu.ConvertScreenCoordinatesToDisplayCoordinates(window_x, window_y, &display_x, &display_y);

  // are we within the active display area?
  u32 tick, line;
  if (display_x < 0 || display_y < 0 ||
      !g_gpu.ConvertDisplayCoordinatesToBeamTicksAndLines(display_x, display_y, m_x_scale, &tick, &line) ||
      m_shoot_offscreen)
  {
    DEV_LOG("Lightgun out of range for window coordinates {:.0f},{:.0f}", window_x, window_y);
    m_position_valid = false;
    UpdateIRQEvent();
    return;
  }

  m_position_valid = true;

  m_irq_tick = static_cast<u16>(static_cast<TickCount>(tick) +
                                System::ScaleTicksToOverclock(static_cast<TickCount>(m_tick_offset)));
  m_irq_first_line = static_cast<u16>(std::clamp<s32>(static_cast<s32>(line) + m_first_line_offset,
                                                      static_cast<s32>(g_gpu.GetCRTCActiveStartLine()),
                                                      static_cast<s32>(g_gpu.GetCRTCActiveEndLine())));
  m_irq_last_line = static_cast<u16>(std::clamp<s32>(static_cast<s32>(line) + m_last_line_offset,
                                                     static_cast<s32>(g_gpu.GetCRTCActiveStartLine()),
                                                     static_cast<s32>(g_gpu.GetCRTCActiveEndLine())));

  DEV_LOG("Lightgun window coordinates {},{} -> dpy {},{} -> tick {} line {} [{}-{}]", window_x, window_y, display_x,
          display_y, tick, line, m_irq_first_line, m_irq_last_line);

  UpdateIRQEvent();
}

void Justifier::UpdateIRQEvent()
{
  // TODO: Avoid deactivate and event sort.
  m_irq_event.Deactivate();

  if (!m_position_valid || !m_irq_enabled)
    return;

  u32 current_tick, current_line;
  g_gpu.GetBeamPosition(&current_tick, &current_line);

  u32 target_line;
  if (current_line < m_irq_first_line || current_line >= m_irq_last_line)
    target_line = m_irq_first_line;
  else
    target_line = current_line + 1;

  const TickCount ticks_until_pos = g_gpu.GetSystemTicksUntilTicksAndLine(m_irq_tick, target_line);
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

// TODO: Merge all this crap with guncon

std::pair<float, float> Justifier::GetAbsolutePositionFromRelativeAxes() const
{
  const float screen_rel_x = (((m_relative_pos[1] > 0.0f) ? m_relative_pos[1] : -m_relative_pos[0]) + 1.0f) * 0.5f;
  const float screen_rel_y = (((m_relative_pos[3] > 0.0f) ? m_relative_pos[3] : -m_relative_pos[2]) + 1.0f) * 0.5f;
  return std::make_pair(screen_rel_x * ImGuiManager::GetWindowWidth(), screen_rel_y * ImGuiManager::GetWindowHeight());
}

bool Justifier::CanUseSoftwareCursor() const
{
  return (InputManager::MAX_POINTER_DEVICES + m_index) < InputManager::MAX_SOFTWARE_CURSORS;
}

u32 Justifier::GetSoftwarePointerIndex() const
{
  return m_has_relative_binds ? (InputManager::MAX_POINTER_DEVICES + m_index) : m_cursor_index;
}

void Justifier::UpdateSoftwarePointerPosition()
{
  if (m_cursor_path.empty() || !CanUseSoftwareCursor())
    return;

  const auto& [window_x, window_y] = GetAbsolutePositionFromRelativeAxes();
  ImGuiManager::SetSoftwareCursorPosition(GetSoftwarePointerIndex(), window_x, window_y);
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

#ifndef __ANDROID__
static constexpr const char* DEFAULT_CROSSHAIR_PATH = "images" FS_OSPATH_SEPARATOR_STR "crosshair.png";
#else
static constexpr const char* DEFAULT_CROSSHAIR_PATH = "";
#endif

static const SettingInfo s_settings[] = {
  {SettingInfo::Type::Path, "CrosshairImagePath", TRANSLATE_NOOP("Justifier", "Crosshair Image Path"),
   TRANSLATE_NOOP("Justifier", "Path to an image to use as a crosshair/cursor."), DEFAULT_CROSSHAIR_PATH, nullptr,
   nullptr, nullptr, nullptr, nullptr, 0.0f},
  {SettingInfo::Type::Float, "CrosshairScale", TRANSLATE_NOOP("Justifier", "Crosshair Image Scale"),
   TRANSLATE_NOOP("Justifier", "Scale of crosshair image on screen."), "1.0", "0.0001", "100.0", "0.10", "%.0f%%",
   nullptr, 100.0f},
  {SettingInfo::Type::String, "CrosshairColor", TRANSLATE_NOOP("Justifier", "Cursor Color"),
   TRANSLATE_NOOP("Justifier",
                  "Applies a color to the chosen crosshair images, can be used for multiple players. Specify "
                  "in HTML/CSS format (e.g. #aabbcc)"),
   "#ffffff", nullptr, nullptr, nullptr, nullptr, nullptr, 0.0f},
  {SettingInfo::Type::Float, "XScale", TRANSLATE_NOOP("Justifier", "X Scale"),
   TRANSLATE_NOOP("Justifier", "Scales X coordinates relative to the center of the screen."), "1.0", "0.01", "2.0",
   "0.01", "%.0f%%", nullptr, 100.0f},
  {SettingInfo::Type::Integer, "FirstLineOffset", TRANSLATE_NOOP("Justifier", "Line Start Offset"),
   TRANSLATE_NOOP("Justifier",
                  "Offset applied to lightgun vertical position that the Justifier will first trigger on."),
   "-14", "-128", "127", "1", "%u", nullptr, 0.0f},
  {SettingInfo::Type::Integer, "LastLineOffset", TRANSLATE_NOOP("Justifier", "Line End Offset"),
   TRANSLATE_NOOP("Justifier", "Offset applied to lightgun vertical position that the Justifier will last trigger on."),
   "-8", "-128", "127", "1", "%u", nullptr, 0.0f},
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

const Controller::ControllerInfo Justifier::INFO = {
  ControllerType::Justifier, "Justifier",    TRANSLATE_NOOP("ControllerType", "Justifier"),
  ICON_PF_LIGHT_GUN,         s_binding_info, s_settings};

void Justifier::LoadSettings(const SettingsInterface& si, const char* section, bool initial)
{
  Controller::LoadSettings(si, section, initial);

  m_x_scale = si.GetFloatValue(section, "XScale", 1.0f);

  std::string cursor_path = si.GetStringValue(section, "CrosshairImagePath", DEFAULT_CROSSHAIR_PATH);
  const float cursor_scale = si.GetFloatValue(section, "CrosshairScale", 1.0f);
  u32 cursor_color = 0xFFFFFF;
  if (std::string cursor_color_str = si.GetStringValue(section, "CrosshairColor", ""); !cursor_color_str.empty())
  {
    // Strip the leading hash, if it's a CSS style colour.
    const std::optional<u32> cursor_color_opt(StringUtil::FromChars<u32>(
      cursor_color_str[0] == '#' ? std::string_view(cursor_color_str).substr(1) : std::string_view(cursor_color_str),
      16));
    if (cursor_color_opt.has_value())
    {
      cursor_color = cursor_color_opt.value();
      cursor_color = (cursor_color & 0x00FF00u) | ((cursor_color >> 16) & 0xFFu) | ((cursor_color & 0xFFu) << 16);
    }
  }

  const s32 prev_pointer_index = GetSoftwarePointerIndex();

  m_has_relative_binds = (si.ContainsValue(section, "RelativeLeft") || si.ContainsValue(section, "RelativeRight") ||
                          si.ContainsValue(section, "RelativeUp") || si.ContainsValue(section, "RelativeDown"));
  m_cursor_index =
    static_cast<u8>(InputManager::GetIndexFromPointerBinding(si.GetStringValue(section, "Pointer")).value_or(0));

  const s32 new_pointer_index = GetSoftwarePointerIndex();

  if (prev_pointer_index != new_pointer_index || m_cursor_path != cursor_path || m_cursor_scale != cursor_scale ||
      m_cursor_color != cursor_color)
  {
    if (!initial && prev_pointer_index != new_pointer_index &&
        static_cast<u32>(prev_pointer_index) < InputManager::MAX_SOFTWARE_CURSORS)
    {
      ImGuiManager::ClearSoftwareCursor(prev_pointer_index);
    }

    // Pointer changed, so need to update software cursor.
    const bool had_software_cursor = !m_cursor_path.empty();
    m_cursor_path = std::move(cursor_path);
    m_cursor_scale = cursor_scale;
    m_cursor_color = cursor_color;
    if (static_cast<u32>(new_pointer_index) < InputManager::MAX_SOFTWARE_CURSORS)
    {
      if (!m_cursor_path.empty())
      {
        std::string image_path;
#ifndef __ANDROID__
        if (!Path::IsAbsolute(m_cursor_path))
          image_path = Path::Combine(EmuFolders::Resources, m_cursor_path);
        else
          image_path = m_cursor_path;
#else
        image_path = m_cursor_path;
#endif

        ImGuiManager::SetSoftwareCursor(new_pointer_index, std::move(image_path), m_cursor_scale, m_cursor_color);
        if (m_has_relative_binds)
          UpdateSoftwarePointerPosition();
      }
      else if (had_software_cursor)
      {
        ImGuiManager::ClearSoftwareCursor(new_pointer_index);
      }
    }
  }

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
