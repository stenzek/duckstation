#include "digital_controller.h"
#include "common/assert.h"
#include "host.h"
#include "system.h"
#include "util/state_wrapper.h"

DigitalController::DigitalController(u32 index) : Controller(index) {}

DigitalController::~DigitalController() = default;

ControllerType DigitalController::GetType() const
{
  return ControllerType::DigitalController;
}

void DigitalController::Reset()
{
  m_transfer_state = TransferState::Idle;
}

bool DigitalController::DoState(StateWrapper& sw, bool apply_input_state)
{
  if (!Controller::DoState(sw, apply_input_state))
    return false;

  u16 button_state = m_button_state;
  sw.Do(&button_state);
  if (apply_input_state)
    m_button_state = button_state;

  sw.Do(&m_transfer_state);
  return true;
}

float DigitalController::GetBindState(u32 index) const
{
  if (index < static_cast<u32>(Button::Count))
  {
    return static_cast<float>(((m_button_state >> index) & 1u) ^ 1u);
  }
  else
  {
    return 0.0f;
  }
}

void DigitalController::SetBindState(u32 index, float value)
{
  if (index >= static_cast<u32>(Button::Count))
    return;

  const bool pressed = (value >= 0.5f);
  const u16 bit = u16(1) << static_cast<u8>(index);
  if (pressed)
  {
    if (m_button_state & bit)
      System::SetRunaheadReplayFlag();

    m_button_state &= ~bit;
  }
  else
  {
    if (!(m_button_state & bit))
      System::SetRunaheadReplayFlag();

    m_button_state |= bit;
  }
}

u32 DigitalController::GetButtonStateBits() const
{
  return m_button_state ^ 0xFFFF;
}

void DigitalController::ResetTransferState()
{
  m_transfer_state = TransferState::Idle;
}

bool DigitalController::Transfer(const u8 data_in, u8* data_out)
{
  static constexpr u16 ID = 0x5A41;

  switch (m_transfer_state)
  {
    case TransferState::Idle:
    {
      *data_out = 0xFF;

      if (data_in == 0x01)
      {
        m_transfer_state = TransferState::Ready;
        return true;
      }
      return false;
    }

    case TransferState::Ready:
    {
      if (data_in == 0x42)
      {
        *data_out = Truncate8(ID);
        m_transfer_state = TransferState::IDMSB;
        return true;
      }

      *data_out = 0xFF;
      return false;
    }

    case TransferState::IDMSB:
    {
      *data_out = Truncate8(ID >> 8);
      m_transfer_state = TransferState::ButtonsLSB;
      return true;
    }

    case TransferState::ButtonsLSB:
    {
      *data_out = Truncate8(m_button_state) & GetButtonsLSBMask();
      m_transfer_state = TransferState::ButtonsMSB;
      return true;
    }

    case TransferState::ButtonsMSB:
      *data_out = Truncate8(m_button_state >> 8);
      m_transfer_state = TransferState::Idle;
      return false;

    default:
    {
      UnreachableCode();
      return false;
    }
  }
}

std::unique_ptr<DigitalController> DigitalController::Create(u32 index)
{
  return std::make_unique<DigitalController>(index);
}

static const Controller::ControllerBindingInfo s_binding_info[] = {
#define BUTTON(name, display_name, button, genb)                                                                       \
  {                                                                                                                    \
    name, display_name, static_cast<u32>(button), Controller::ControllerBindingType::Button, genb                      \
  }

  BUTTON("Up", "D-Pad Up", DigitalController::Button::Up, GenericInputBinding::DPadUp),
  BUTTON("Right", "D-Pad Right", DigitalController::Button::Right, GenericInputBinding::DPadRight),
  BUTTON("Down", "D-Pad Down", DigitalController::Button::Down, GenericInputBinding::DPadDown),
  BUTTON("Left", "D-Pad Left", DigitalController::Button::Left, GenericInputBinding::DPadLeft),
  BUTTON("Triangle", "Triangle", DigitalController::Button::Triangle, GenericInputBinding::Triangle),
  BUTTON("Circle", "Circle", DigitalController::Button::Circle, GenericInputBinding::Circle),
  BUTTON("Cross", "Cross", DigitalController::Button::Cross, GenericInputBinding::Cross),
  BUTTON("Square", "Square", DigitalController::Button::Square, GenericInputBinding::Square),
  BUTTON("Select", "Select", DigitalController::Button::Select, GenericInputBinding::Select),
  BUTTON("Start", "Start", DigitalController::Button::Start, GenericInputBinding::Start),
  BUTTON("L1", "L1", DigitalController::Button::L1, GenericInputBinding::L1),
  BUTTON("R1", "R1", DigitalController::Button::R1, GenericInputBinding::R1),
  BUTTON("L2", "L2", DigitalController::Button::L2, GenericInputBinding::L2),
  BUTTON("R2", "R2", DigitalController::Button::R2, GenericInputBinding::R2),

#undef BUTTON
};

static const SettingInfo s_settings[] = {
  {SettingInfo::Type::Boolean, "ForcePopnControllerMode",
   TRANSLATABLE("DigitalController", "Force Pop'n Controller Mode"),
   TRANSLATABLE("DigitalController", "Forces the Digital Controller to act as a Pop'n Controller."), "false"}};

const Controller::ControllerInfo DigitalController::INFO = {ControllerType::DigitalController,
                                                            "DigitalController",
                                                            TRANSLATABLE("ControllerType", "Digital Controller"),
                                                            s_binding_info,
                                                            countof(s_binding_info),
                                                            s_settings,
                                                            countof(s_settings),
                                                            Controller::VibrationCapabilities::NoVibration};

void DigitalController::LoadSettings(SettingsInterface& si, const char* section)
{
  Controller::LoadSettings(si, section);
  m_popn_controller_mode = si.GetBoolValue(section, "ForcePopnControllerMode", false);
}

u8 DigitalController::GetButtonsLSBMask() const
{
  constexpr u8 popn_controller_mask =
    static_cast<u8>(~(u8(1) << static_cast<u8>(Button::Right) | u8(1) << static_cast<u8>(Button::Down) |
                      u8(1) << static_cast<u8>(Button::Left)));
  return m_popn_controller_mode ? popn_controller_mask : 0xFF;
}
