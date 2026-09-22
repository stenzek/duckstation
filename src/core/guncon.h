// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "lightgun_controller.h"

#include <memory>

class GunCon final : public LightgunController
{
public:
  enum class Binding : u8
  {
    Trigger = 0,
    A = 1,
    B = 2,
    ShootOffscreen = 3,
    ButtonCount = 4,

    RelativeLeft = 4,
    RelativeRight = 5,
    RelativeUp = 6,
    RelativeDown = 7,
    BindingCount = 8,
  };

  static const Controller::ControllerInfo INFO;

  explicit GunCon(u32 index);
  ~GunCon() override;

  static std::unique_ptr<GunCon> Create(u32 index);

  ControllerType GetType() const override;

  void Reset() override;
  bool DoState(StateWrapper& sw, bool apply_input_state) override;

  void LoadSettings(const SettingsInterface& si, const char* section, bool initial) override;

  void ResetTransferState() override;
  bool Transfer(const u8 data_in, u8* data_out) override;

private:
  enum class TransferState : u8
  {
    Idle,
    Ready,
    IDMSB,
    ButtonsLSB,
    ButtonsMSB,
    XLSB,
    XMSB,
    YLSB,
    YMSB
  };

  static constexpr s8 DEFAULT_LINE_OFFSET = 0;
  static constexpr s16 DEFAULT_TICK_OFFSET = -140;

  void SetShootOffscreen(bool pressed) override;
  void UpdatePosition();

  s16 m_tick_offset = DEFAULT_TICK_OFFSET;
  s8 m_line_offset = DEFAULT_LINE_OFFSET;

  u16 m_position_x = 0;
  u16 m_position_y = 0;
  bool m_shoot_offscreen = false;

  TransferState m_transfer_state = TransferState::Idle;
};
