#pragma once
#include "core/controller.h"
#include <array>

namespace FrontendCommon {

class InputOverlayUI
{
public:
  InputOverlayUI();
  ~InputOverlayUI();

  void Draw();

private:
  void UpdateNames();

  std::array<Controller::AxisList, NUM_CONTROLLER_AND_CARD_PORTS> m_axis_names;
  std::array<Controller::ButtonList, NUM_CONTROLLER_AND_CARD_PORTS> m_button_names;
  std::array<ControllerType, NUM_CONTROLLER_AND_CARD_PORTS> m_types{};
  u32 m_active_ports = 0;
};

} // namespace FrontendCommon