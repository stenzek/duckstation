// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "controller.h"

#include <memory>

class DDGoController final : public Controller
{
public:
  enum class Axis : u8
  {
    Count
  };

  enum class Bind : u8
  {
    Select = 0,
    Start = 3,
    A = 15,
    B = 14,
    C = 13,
    PowerBit0 = 12,
    PowerBit1 = 7,
    PowerBit2 = 5,
    BrakeBit0 = 10,
    BrakeBit1 = 8,
    BrakeBit2 = 11,
    BrakeBit3 = 9,

    // We have to sneak the power/brake axes in here because otherwise we go over the limit for the
    // input overlay, which is 32 bindings.
    Power = 1,
    Brake = 2,

    VirtualButtonStart = 16,
    VirtualPowerOff = VirtualButtonStart,
    VirtualPower1,
    VirtualPower2,
    VirtualPower3,
    VirtualPower4,
    VirtualPower5,
    VirtualBrakeReleased,
    VirtualBrake1,
    VirtualBrake2,
    VirtualBrake3,
    VirtualBrake4,
    VirtualBrake5,
    VirtualBrake6,
    VirtualBrake7,
    VirtualBrake8,
    VirtualBrakeEmergency,
    Count,
  };

  static const Controller::ControllerInfo INFO;

  explicit DDGoController(u32 index);
  ~DDGoController() override;

  static std::unique_ptr<DDGoController> Create(u32 index);

  ControllerType GetType() const override;

  void Reset() override;
  bool DoState(StateWrapper& sw, bool apply_input_state) override;

  float GetBindState(u32 index) const override;
  void SetBindState(u32 index, float value) override;
  u32 GetButtonStateBits() const override;

  void ResetTransferState() override;
  bool Transfer(const u8 data_in, u8* data_out) override;

  void LoadSettings(const SettingsInterface& si, const char* section, bool initial) override;

private:
  enum class TransferState : u8
  {
    Idle,
    Ready,
    IDMSB,
    ButtonsLSB,
    ButtonsMSB
  };

  static constexpr u16 BUTTON_MASK = static_cast<u16>(~((1u << 4) | (1u << 6)));

  static constexpr u16 POWER_MASK =
    static_cast<u16>((1u << static_cast<u32>(Bind::PowerBit0)) | (1u << static_cast<u32>(Bind::PowerBit1)) |
                     (1u << static_cast<u32>(Bind::PowerBit2)));

  static constexpr u16 BRAKE_MASK =
    static_cast<u16>((1u << static_cast<u32>(Bind::BrakeBit0)) | (1u << static_cast<u32>(Bind::BrakeBit1)) |
                     (1u << static_cast<u32>(Bind::BrakeBit2)) | (1u << static_cast<u32>(Bind::BrakeBit3)));

  static constexpr u32 MAX_POWER_LEVEL = 5;
  static constexpr u32 MAX_BRAKE_LEVEL = 9;

  void SetPowerLevel(u32 level);
  void UpdatePowerBits();

  void SetBrakeLevel(u32 level);
  void UpdateBrakeBits();

  // buttons are active low
  u16 m_button_state = UINT16_C(0xFFFF);

  TransferState m_transfer_state = TransferState::Idle;

  u8 m_power_level = 0;
  u8 m_brake_level = MAX_BRAKE_LEVEL;

  u8 m_power_transition_frames_remaining = 0;
  u8 m_brake_transition_frames_remaining = 0;

  u8 m_power_transition_frames = 10;
  u8 m_brake_transition_frames = 10;

  float m_analog_deadzone = 0.0f;
  float m_analog_sensitivity = 1.33f;
};
