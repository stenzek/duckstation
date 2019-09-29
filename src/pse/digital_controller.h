#pragma once
#include "common/fifo_queue.h"
#include "pad_device.h"
#include <memory>

class DigitalController final : public PadDevice
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
    Square = 15
  };

  DigitalController();
  ~DigitalController() override;

  static std::shared_ptr<DigitalController> Create();

  void SetButtonState(Button button, bool pressed);

  void ResetTransferState() override;
  bool Transfer(const u8 data_in, u8* data_out) override;

private:
  void QueryState();

  // buttons are active low
  u16 m_button_state = UINT16_C(0xFFFF);

  InlineFIFOQueue<u8, 8> m_transfer_fifo;
};
