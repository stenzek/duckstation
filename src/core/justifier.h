// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "lightgun_controller.h"
#include "timing_event.h"

#include <memory>

class TimingEvent;

class Justifier final : public LightgunController
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

  void LoadSettings(const SettingsInterface& si, const char* section, bool initial) override;

  void ResetTransferState() override;
  bool Transfer(const u8 data_in, u8* data_out) override;

private:
  void SetShootOffscreen(bool pressed) override;
  void UpdatePosition();
  void UpdateIRQEvent();
  void IRQEvent();

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

  u8 m_shoot_offscreen = 0;
  bool m_position_valid = false;
  bool m_irq_enabled = false;

  TransferState m_transfer_state = TransferState::Idle;

  TimingEvent m_irq_event;
};
