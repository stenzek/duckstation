// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "controller.h"
#include "analog_controller.h"
#include "analog_joystick.h"
#include "ddgo_controller.h"
#include "digital_controller.h"
#include "game_database.h"
#include "guncon.h"
#include "jogcon.h"
#include "justifier.h"
#include "negcon.h"
#include "negcon_rumble.h"
#include "playstation_mouse.h"
#include "system.h"

#include "util/state_wrapper.h"
#include "util/translation.h"

#include "common/assert.h"
#include "common/log.h"

#include "IconsPromptFont.h"
#include "fmt/format.h"

LOG_CHANNEL(Controller);

static const Controller::ControllerInfo s_none_info = {ControllerType::None,
                                                       "None",
                                                       TRANSLATE_NOOP("ControllerType", "Not Connected"),
                                                       ICON_PF_NO_CONTROLLER,
                                                       nullptr,
                                                       {},
                                                       {}};

static constexpr std::array<const Controller::ControllerInfo*, static_cast<size_t>(ControllerType::Count)>
  s_controller_info = {{
    &s_none_info,
    &DigitalController::INFO,
    &AnalogController::INFO,
    &AnalogJoystick::INFO,
    &GunCon::INFO,
    &PlayStationMouse::INFO,
    &NeGcon::INFO,
    &NeGconRumble::INFO,
    &Justifier::INFO,
    &DigitalController::INFO_POPN,
    &DDGoController::INFO,
    &JogCon::INFO,
  }};

const std::array<u32, NUM_CONTROLLER_AND_CARD_PORTS> Controller::PortDisplayOrder = {{0, 2, 3, 4, 1, 5, 6, 7}};

std::string_view Controller::ControllerInfo::GetDisplayName() const
{
  return Host::TranslateToStringView("ControllerType", display_name);
}

std::string_view Controller::ControllerInfo::GetBindingDisplayName(const ControllerBindingInfo& bi) const
{
  return Host::TranslateToStringView(name, bi.display_name);
}

Controller::Controller(u32 index, ControllerType type) : m_index(index), m_type(type)
{
}

Controller::~Controller() = default;

std::optional<u32> Controller::GetAnalogInputBytes() const
{
  return std::nullopt;
}

bool Controller::DoState(StateWrapper& sw, bool apply_input_state)
{
  sw.Do(&m_transfer_state, TransferState::Command);
  sw.Do(&m_command_step);
  sw.Do(&m_response_length);
  sw.DoBytes(m_rx_buffer.data(), m_rx_buffer.size());
  sw.DoBytes(m_tx_buffer.data(), m_tx_buffer.size());
  if (sw.IsReading() && m_transfer_state == TransferState::Command &&
      (m_command_step >= m_response_length || m_response_length > MAX_RESPONSE_LENGTH))
  {
    Controller::ResetTransferState();
  }

  return !sw.HasError();
}

void Controller::SkipLegacyTransferState(StateWrapper& sw)
{
  sw.SkipBytes(sizeof(TransferState)); // transfer_state
  if (sw.IsReading())
    Controller::ResetTransferState();
}

void Controller::ResetTransferState()
{
  m_transfer_state = TransferState::Idle;
  m_command_step = 0;
}

bool Controller::Transfer(const u8 data_in, u8* data_out)
{
  switch (m_transfer_state)
  {
    case TransferState::Idle:
    {
      *data_out = 0xFF;

      if (data_in == 0x01)
      {
        DEBUG_LOG("ACK controller access");
        m_transfer_state = TransferState::Ready;
        return true;
      }

      DEV_LOG("Unknown data_in = 0x{:02X}", data_in);
      return false;
    }

    case TransferState::Ready:
    {
      m_rx_buffer.fill(0x00);
      m_tx_buffer.fill(0x00);

      // The ID is sent while the command byte is received, so it's still there for unknown commands.
      const u8 id = GetIDByte();
      *data_out = id;

      m_rx_buffer[0] = data_in;
      m_tx_buffer[0] = id;
      m_tx_buffer[1] = 0x5A;
      if (!BeginCommand(data_in))
        return false;

      m_response_length = static_cast<u8>(((id & 0xF) + 1) * 2);
      m_command_step = 1;
      m_transfer_state = TransferState::Command;
      return true;
    }

    case TransferState::Command:
    {
      m_rx_buffer[m_command_step] = data_in;
      ProcessCommandByte(m_command_step, data_in);
      *data_out = m_tx_buffer[m_command_step];

      if (++m_command_step < m_response_length)
        return true;

      EndCommand();

      DEBUG_LOG("Rx: {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x}", m_rx_buffer[0], m_rx_buffer[1],
                m_rx_buffer[2], m_rx_buffer[3], m_rx_buffer[4], m_rx_buffer[5], m_rx_buffer[6], m_rx_buffer[7]);
      DEBUG_LOG("Tx: {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x}", m_tx_buffer[0], m_tx_buffer[1],
                m_tx_buffer[2], m_tx_buffer[3], m_tx_buffer[4], m_tx_buffer[5], m_tx_buffer[6], m_tx_buffer[7]);

      m_transfer_state = TransferState::Idle;
      m_command_step = 0;
      return false;
    }

      DefaultCaseIsUnreachable();
  }
}

void Controller::ProcessCommandByte(u32 step, u8 data_in)
{
}

void Controller::EndCommand()
{
}

void Controller::SetButtonPressed(u16& state, u16 bit, bool pressed)
{
  if (pressed == ((state & bit) == 0))
    return;

  System::SetRunaheadReplayFlag(false);
  if (pressed)
    state &= ~bit;
  else
    state |= bit;
}

std::unique_ptr<Controller> Controller::Create(ControllerType type, u32 index)
{
  switch (type)
  {
    case ControllerType::DigitalController:
    case ControllerType::PopnController:
      return DigitalController::Create(index, type);

    case ControllerType::AnalogController:
      return AnalogController::Create(index);

    case ControllerType::AnalogJoystick:
      return AnalogJoystick::Create(index);

    case ControllerType::GunCon:
      return GunCon::Create(index);

    case ControllerType::Justifier:
      return Justifier::Create(index);

    case ControllerType::PlayStationMouse:
      return PlayStationMouse::Create(index);

    case ControllerType::NeGcon:
      return NeGcon::Create(index);

    case ControllerType::NeGconRumble:
      return NeGconRumble::Create(index);

    case ControllerType::DDGoController:
      return DDGoController::Create(index);

    case ControllerType::JogCon:
      return JogCon::Create(index);

    case ControllerType::None:
    default:
      return {};
  }
}

const Controller::ControllerInfo& Controller::GetControllerInfo(ControllerType type)
{
  DebugAssert(type < ControllerType::Count && s_controller_info[static_cast<size_t>(type)]);
  return *s_controller_info[static_cast<size_t>(type)];
}

const Controller::ControllerInfo* Controller::GetControllerInfo(std::string_view name)
{
  for (const ControllerInfo* info : s_controller_info)
  {
    if (name == info->name)
      return info;
  }

  return nullptr;
}

const std::array<const Controller::ControllerInfo*, static_cast<size_t>(ControllerType::Count)>&
Controller::GetControllerInfoList()
{
  return s_controller_info;
}

std::tuple<u32, u32> Controller::ConvertPadToPortAndSlot(u32 index)
{
  if (index > 4)                          // [5,6,7]
    return std::make_tuple(1, index - 4); // 2B,2C,2D
  else if (index > 1)                     // [2,3,4]
    return std::make_tuple(0, index - 1); // 1B,1C,1D
  else                                    // [0,1]
    return std::make_tuple(index, 0);     // 1A,2A
}

u32 Controller::ConvertPortAndSlotToPad(u32 port, u32 slot)
{
  if (slot == 0)
    return port;
  else if (port == 0) // slot=[0,1]
    return slot + 1;  // 2,3,4
  else
    return slot + 4; // 5,6,7
}

bool Controller::PadIsMultitapSlot(u32 index)
{
  return (index >= 2);
}

bool Controller::PortAndSlotIsMultitap(u32 port, u32 slot)
{
  return (slot != 0);
}

const char* Controller::GetPortDisplayName(u32 port, u32 slot, bool mtap)
{
  static constexpr const std::array<const char*, NUM_MULTITAPS> no_mtap_labels = {{"1", "2"}};
  static constexpr const std::array<std::array<const char*, NUM_CONTROLLER_AND_CARD_PORTS_PER_MULTITAP>, NUM_MULTITAPS>
    mtap_labels = {{{{"1A", "1B", "1C", "1D"}}, {{"2A", "2B", "2C", "2D"}}}};

  DebugAssert(port < 2 && slot < 4);
  return mtap ? mtap_labels[port][slot] : no_mtap_labels[port];
}

const char* Controller::GetPortDisplayName(u32 index, MultitapMode mode)
{
  const auto& [port, slot] = ConvertPadToPortAndSlot(index);
  return GetPortDisplayName(port, slot, IsMultitapEnabledOnPort(port, mode));
}

bool Controller::IsMultitapEnabledOnPort(u32 port, MultitapMode mode)
{
  return (port == 0) ? (mode == MultitapMode::Port1Only || mode == MultitapMode::BothPorts) :
                       (mode == MultitapMode::Port2Only || mode == MultitapMode::BothPorts);
}

std::array<bool, 2> Controller::GetMultitapEnabledPorts(MultitapMode mode)
{
  return {(mode == MultitapMode::Port1Only || mode == MultitapMode::BothPorts),
          (mode == MultitapMode::Port2Only || mode == MultitapMode::BothPorts)};
}

std::string Controller::GetSettingsSection(u32 pad)
{
  return fmt::format("Pad{}", pad + 1u);
}

bool Controller::CanStartInAnalogMode(ControllerType ctype)
{
  if (!g_settings.apply_compatibility_settings)
    return true;

  const GameDatabase::Entry* dbentry = System::GetGameDatabaseEntry();
  if (!dbentry)
    return false;

  return ((dbentry->supported_controllers & (1u << static_cast<u8>(ctype))) != 0 &&
          !dbentry->HasTrait(GameDatabase::Trait::DisableAutoAnalogMode));
}
