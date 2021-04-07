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

  PlayStationMouse();
  ~PlayStationMouse() override;

  static std::unique_ptr<PlayStationMouse> Create();
  static std::optional<s32> StaticGetAxisCodeByName(std::string_view button_name);
  static std::optional<s32> StaticGetButtonCodeByName(std::string_view button_name);
  static AxisList StaticGetAxisNames();
  static ButtonList StaticGetButtonNames();
  static u32 StaticGetVibrationMotorCount();
  static SettingList StaticGetSettings();

  ControllerType GetType() const override;
  std::optional<s32> GetAxisCodeByName(std::string_view axis_name) const override;
  std::optional<s32> GetButtonCodeByName(std::string_view button_name) const override;

  void Reset() override;
  bool DoState(StateWrapper& sw, bool apply_input_state) override;

  bool GetButtonState(s32 button_code) const override;
  void SetButtonState(s32 button_code, bool pressed) override;

  void ResetTransferState() override;
  bool Transfer(const u8 data_in, u8* data_out) override;

  void SetButtonState(Button button, bool pressed);

  void LoadSettings(const char* section) override;
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
