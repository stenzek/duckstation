// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "controller.h"
#include "timing_event.h"

#include <memory>

class TimingEvent;

class Justifier final : public Controller
{
public:
  enum class Binding : u8
  {
    Trigger = 0,
    Start = 1,
    Back = 2,
    ShootOffscreen = 3,
    ButtonCount = 4,

    RelativeLeft = 4,
    RelativeRight = 5,
    RelativeUp = 6,
    RelativeDown = 7,
    BindingCount = 8,
  };

  static const Controller::ControllerInfo INFO;

  Justifier(u32 index);
  ~Justifier() override;

  static std::unique_ptr<Justifier> Create(u32 index);

  ControllerType GetType() const override;

  void Reset() override;
  bool DoState(StateWrapper& sw, bool apply_input_state) override;

  void LoadSettings(SettingsInterface& si, const char* section, bool initial) override;

  float GetBindState(u32 index) const override;
  void SetBindState(u32 index, float value) override;

  void ResetTransferState() override;
  bool Transfer(const u8 data_in, u8* data_out) override;

private:
  bool IsTriggerPressed() const;
  void UpdatePosition();
  void UpdateIRQEvent();
  void IRQEvent();

  std::pair<float, float> GetAbsolutePositionFromRelativeAxes() const;
  bool CanUseSoftwareCursor() const;
  u32 GetSoftwarePointerIndex() const;
  void UpdateSoftwarePointerPosition();

  enum class TransferState : u8
  {
    Idle,
    IDMSB,
    ButtonsLSB,
    ButtonsMSB,
    XLSB,
    XMSB,
    YLSB,
    YMSB
  };

  static constexpr s8 DEFAULT_FIRST_LINE_OFFSET = -12;
  static constexpr s8 DEFAULT_LAST_LINE_OFFSET = -6;
  static constexpr s16 DEFAULT_TICK_OFFSET = 50;
  static constexpr u8 DEFAULT_OFFSCREEN_OOB_FRAMES = 5;
  static constexpr u8 DEFAULT_OFFSCREEN_TRIGGER_FRAMES = 5;
  static constexpr u8 DEFAULT_OFFSCREEN_RELEASE_FRAMES = 5;

  s8 m_first_line_offset = 0;
  s8 m_last_line_offset = 0;
  s16 m_tick_offset = 0;

  u8 m_offscreen_oob_frames = 0;
  u8 m_offscreen_trigger_frames = 0;
  u8 m_offscreen_release_frames = 0;

  u16 m_irq_first_line = 0;
  u16 m_irq_last_line = 0;
  u16 m_irq_tick = 0;

  // buttons are active low
  u16 m_button_state = UINT16_C(0xFFFF);
  u8 m_shoot_offscreen = 0;
  bool m_position_valid = false;

  TransferState m_transfer_state = TransferState::Idle;

  TimingEvent m_irq_event;

  bool m_has_relative_binds = false;
  u8 m_cursor_index = 0;
  float m_relative_pos[4] = {};

  std::string m_cursor_path;
  float m_cursor_scale = 1.0f;
  u32 m_cursor_color = 0xFFFFFFFFu;
  float m_x_scale = 1.0f;
};
