// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "controller.h"

#include <memory>

class DigitalController final : public Controller
{
public:
  enum class Button : u8
  {
    Select = 0,
    L3 = 1,
    R3 = 2,
    Start = 3,
    Up = 4,
    Right = 5,
    Down = 6,
    Left = 7,
    L2 = 8,
    R2 = 9,
    L1 = 10,
    R1 = 11,
    Triangle = 12,
    Circle = 13,
    Cross = 14,
    Square = 15,
    Count
  };

  static const Controller::ControllerInfo INFO;
  static const Controller::ControllerInfo INFO_POPN;
  static const Controller::ControllerInfo INFO_DDGO;

  DigitalController(u32 index, ControllerType type, u16 button_mask);
  ~DigitalController() override;

  static std::unique_ptr<DigitalController> Create(u32 index, ControllerType type);

  void Reset() override;
  bool DoState(StateWrapper& sw, bool apply_input_state) override;

  float GetBindState(u32 index) const override;
  void SetBindState(u32 index, float value) override;
  u32 GetButtonStateBits() const override;

  void LoadSettings(const SettingsInterface& si, const char* section, bool initial) override;

private:
  u8 GetIDByte() const override;
  bool BeginCommand(u8 command) override;

  // buttons are active low
  u16 m_button_state = UINT16_C(0xFFFF);
  u16 m_button_mask = UINT16_C(0xFFFF);

  bool m_disable_socd = false;
};
