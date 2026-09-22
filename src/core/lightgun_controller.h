// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "controller.h"

#include <array>
#include <string>
#include <utility>

class LightgunController : public Controller
{
public:
  ~LightgunController() override;

  bool DoState(StateWrapper& sw, bool apply_input_state) override;

  void LoadSettings(const SettingsInterface& si, const char* section, bool initial) override;

  float GetBindState(u32 index) const override;
  void SetBindState(u32 index, float value) override;
  u32 GetButtonStateBits() const override;

protected:
  struct Position
  {
    float window_x;
    float window_y;
    float display_x;
    float display_y;
    u32 tick;
    u32 line;
    bool valid;
  };

  enum class Binding : u8
  {
    Trigger = 0,
    Button1 = 1,
    Button2 = 2,
    ShootOffscreen = 3,
    ButtonCount = 4,

    RelativeLeft = 4,
    RelativeRight = 5,
    RelativeUp = 6,
    RelativeDown = 7,
    BindingCount = 8,
  };

  LightgunController(u32 index, const std::array<u8, 4>& button_indices);

  virtual void SetShootOffscreen(bool pressed) = 0;

  Position GetPosition() const;

  // buttons are active low
  u16 m_button_state = UINT16_C(0xFFFF);

private:
  // 0..1, not -1..1.
  std::pair<float, float> GetAbsolutePositionFromRelativeAxes() const;
  bool CanUseSoftwareCursor() const;
  u32 GetSoftwarePointerIndex() const;
  void UpdateSoftwarePointerPosition();

  std::array<u8, 4> m_button_indices;
  bool m_has_relative_binds = false;
  float m_x_scale = 1.0f;
  float m_relative_pos[4] = {};
  u8 m_cursor_index = 0;

  float m_cursor_scale = 1.0f;
  u32 m_cursor_color = 0xFFFFFFFFu;
  std::string m_cursor_path;
};
