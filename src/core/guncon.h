#pragma once
#include "controller.h"
#include <memory>
#include <optional>
#include <string_view>

class GunCon final : public Controller
{
public:
  enum class Button : u8
  {
    Trigger = 0,
    A = 1,
    B = 2,
    ShootOffscreen = 3,
    Count
  };

  static const Controller::ControllerInfo INFO;

  GunCon(u32 index);
  ~GunCon() override;

  static std::unique_ptr<GunCon> Create(u32 index);

  ControllerType GetType() const override;

  void Reset() override;
  bool DoState(StateWrapper& sw, bool apply_input_state) override;
  
  void LoadSettings(SettingsInterface& si, const char* section) override;
  bool GetSoftwareCursor(const Common::RGBA8Image** image, float* image_scale, bool* relative_mode) override;

  float GetBindState(u32 index) const override;
  void SetBindState(u32 index, float value) override;

  void ResetTransferState() override;
  bool Transfer(const u8 data_in, u8* data_out) override;

private:
  void UpdatePosition();

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

  Common::RGBA8Image m_crosshair_image;
  std::string m_crosshair_image_path;
  float m_crosshair_image_scale = 1.0f;
  float m_x_scale = 1.0f;

  // buttons are active low
  u16 m_button_state = UINT16_C(0xFFFF);
  u16 m_position_x = 0;
  u16 m_position_y = 0;
  bool m_shoot_offscreen = false;

  TransferState m_transfer_state = TransferState::Idle;
};
