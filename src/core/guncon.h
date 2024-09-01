// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "controller.h"

#include <memory>

class GunCon final : public Controller
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

  GunCon(u32 index);
  ~GunCon() override;

  static std::unique_ptr<GunCon> Create(u32 index);

  ControllerType GetType() const override;

  void Reset() override;
  bool DoState(StateWrapper& sw, bool apply_input_state) override;

  void LoadSettings(SettingsInterface& si, const char* section, bool initial) override;

  float GetBindState(u32 index) const override;
  void SetBindState(u32 index, float value) override;

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

  void UpdatePosition();

  // 0..1, not -1..1.
  std::pair<float, float> GetAbsolutePositionFromRelativeAxes() const;
  bool CanUseSoftwareCursor() const;
  u32 GetSoftwarePointerIndex() const;
  void UpdateSoftwarePointerPosition();

  std::string m_cursor_path;
  float m_cursor_scale = 1.0f;
  u32 m_cursor_color = 0xFFFFFFFFu;
  float m_x_scale = 1.0f;

  float m_relative_pos[4] = {};

  // buttons are active low
  u16 m_button_state = UINT16_C(0xFFFF);
  u16 m_position_x = 0;
  u16 m_position_y = 0;
  bool m_shoot_offscreen = false;
  bool m_has_relative_binds = false;
  u8 m_cursor_index = 0;

  TransferState m_transfer_state = TransferState::Idle;
};
