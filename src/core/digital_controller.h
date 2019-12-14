#pragma once
#include "controller.h"
#include <memory>
#include <optional>
#include <string_view>

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

  DigitalController();
  ~DigitalController() override;

  static std::unique_ptr<DigitalController> Create();
  static std::optional<s32> StaticGetButtonCodeByName(std::string_view button_name);

  ControllerType GetType() const override;
  std::optional<s32> GetButtonCodeByName(std::string_view button_name) const override;

  void SetButtonState(Button button, bool pressed);
  void SetButtonState(s32 button_code, bool pressed);

  void ResetTransferState() override;
  bool Transfer(const u8 data_in, u8* data_out) override;

private:
  enum class TransferState : u8
  {
    Idle,
    IDMSB,
    ButtonsLSB,
    ButtonsMSB
  };

  // buttons are active low
  u16 m_button_state = UINT16_C(0xFFFF);

  TransferState m_transfer_state = TransferState::Idle;
};
