#pragma once
#include "controller.h"
#include <memory>
#include <optional>
#include <string_view>

class PlayStationMouse final : public Controller
{
public:
  enum class Button : u8
  {
    Left = 0,
    Right = 1,
    Count
  };

  static const Controller::ControllerInfo INFO;

  PlayStationMouse(u32 index);
  ~PlayStationMouse() override;

  static std::unique_ptr<PlayStationMouse> Create(u32 index);

  ControllerType GetType() const override;

  void Reset() override;
  bool DoState(StateWrapper& sw, bool apply_input_state) override;

  float GetBindState(u32 index) const override;
  void SetBindState(u32 index, float value) override;

  void ResetTransferState() override;
  bool Transfer(const u8 data_in, u8* data_out) override;

  void LoadSettings(SettingsInterface& si, const char* section) override;
  bool GetSoftwareCursor(const Common::RGBA8Image** image, float* image_scale, bool* relative_mode) override;

private:
  void UpdatePosition();

  enum class TransferState : u8
  {
    Idle,
    Ready,
    IDMSB,
    ButtonsLSB,
    ButtonsMSB,
    DeltaX,
    DeltaY
  };

  s32 m_last_host_position_x = 0;
  s32 m_last_host_position_y = 0;

  // buttons are active low
  u16 m_button_state = UINT16_C(0xFFFF);
  s8 m_delta_x = 0;
  s8 m_delta_y = 0;

  TransferState m_transfer_state = TransferState::Idle;

  bool m_use_relative_mode = false;
};
