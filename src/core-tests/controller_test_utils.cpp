// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "controller_test_utils.h"

#include "core/save_state_version.h"

#include "util/ini_settings_interface.h"
#include "util/state_wrapper.h"

#include <span>

namespace ControllerTestUtils {

float RawHalfAxis(u8 raw)
{
  return (static_cast<float>(raw) + 0.5f) / 255.0f;
}

std::vector<u8> DeviceReply(u8 id, u16 buttons, std::initializer_list<u8> extra)
{
  std::vector<u8> ret = {0xFF, id, 0x5A, Truncate8(buttons), Truncate8(buttons >> 8)};
  ret.insert(ret.end(), extra);
  return ret;
}

std::vector<u8> PollReply(u16 buttons)
{
  return DeviceReply(0x41, buttons);
}

std::vector<u8> AnalogPollReply(u16 buttons, u8 rx, u8 ry, u8 lx, u8 ly)
{
  return DeviceReply(0x73, buttons, {rx, ry, lx, ly});
}

Block DigitalBlock(u16 buttons)
{
  return {0x41, 0x5A, Truncate8(buttons), Truncate8(buttons >> 8), 0xFF, 0xFF, 0xFF, 0xFF};
}

Block AnalogBlock(u16 buttons)
{
  return {0x73, 0x5A, Truncate8(buttons), Truncate8(buttons >> 8), 0x7F, 0x7F, 0x7F, 0x7F};
}

std::vector<u8> EnterConfigModeCommand(u8 first)
{
  return {first, 0x43, 0x00, 0x01, 0x00};
}

std::vector<u8> ExitConfigModeCommand(u8 first)
{
  return {first, 0x43, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
}

std::vector<u8> SetAnalogModeCommand(u8 first, u8 analog, u8 lock)
{
  return {first, 0x44, 0x00, analog, lock, 0x00, 0x00, 0x00, 0x00};
}

std::vector<u8> ConfigCommand(u8 command, u8 param)
{
  return {0x01, command, 0x00, param, 0x00, 0x00, 0x00, 0x00, 0x00};
}

std::vector<u8> ConfigReply(std::initializer_list<u8> data)
{
  std::vector<u8> ret = {0xFF, 0xF3, 0x5A};
  ret.insert(ret.end(), data);
  ret.resize(9, 0x00);
  return ret;
}

std::vector<u8> RumblePoll(std::initializer_list<u8> motor_bytes, size_t length)
{
  std::vector<u8> ret = {0x01, 0x42, 0x00};
  ret.insert(ret.end(), motor_bytes);
  ret.resize(length, 0x00);
  return ret;
}

std::vector<u8> MapMotorsCommand(std::initializer_list<u8> mapping)
{
  std::vector<u8> ret = {0x01, 0x4D, 0x00};
  ret.insert(ret.end(), mapping);
  ret.resize(9, 0xFF);
  return ret;
}

std::vector<u8> TransferBytes(Controller& controller, const std::vector<u8>& tx, std::vector<bool>* acks)
{
  std::vector<u8> rx;
  if (acks)
    acks->clear();

  for (const u8 value : tx)
  {
    u8 data_out = 0;
    const bool ack = controller.Transfer(value, &data_out);
    rx.push_back(data_out);
    if (acks)
      acks->push_back(ack);
  }

  return rx;
}

std::vector<u8> TransferCommand(Controller& controller, const std::vector<u8>& tx)
{
  std::vector<u8> rx;
  for (size_t i = 0; i < tx.size(); i++)
  {
    u8 data_out = 0;
    const bool ack = controller.Transfer(tx[i], &data_out);
    rx.push_back(data_out);

    const bool expected_ack = (i + 1) < tx.size();
    EXPECT_EQ(ack, expected_ack) << "byte " << i;
    if (ack != expected_ack)
      break;
  }

  return rx;
}

void EnterAnalogMode(Controller& controller)
{
  EXPECT_EQ(TransferCommand(controller, EnterConfigModeCommand()), PollReply(NO_BUTTONS));
  EXPECT_EQ(TransferCommand(controller, SetAnalogModeCommand()), ANALOG_CONFIG_REPLY);
  EXPECT_EQ(TransferCommand(controller, ExitConfigModeCommand()), ANALOG_CONFIG_REPLY);
}

void ExpectRejectedBytes(Controller& controller, std::initializer_list<u8> bad_commands, u8 bad_command_reply,
                         const std::vector<u8>& poll, const std::vector<u8>& poll_reply)
{
  for (const u8 first : {0x00, 0x02, 0x42, 0x81, 0xFF})
  {
    EXPECT_EQ(TransferCommand(controller, {first}), std::vector<u8>{0xFF}) << "first byte 0x" << std::hex << u32(first);
    EXPECT_EQ(TransferCommand(controller, poll), poll_reply) << "after first byte 0x" << std::hex << u32(first);
  }

  for (const u8 command : bad_commands)
  {
    EXPECT_EQ(TransferCommand(controller, {0x01, command}), (std::vector<u8>{0xFF, bad_command_reply}))
      << "command 0x" << std::hex << u32(command);
    controller.ResetTransferState();
    EXPECT_EQ(TransferCommand(controller, poll), poll_reply) << "after command 0x" << std::hex << u32(command);
  }
}

std::vector<u8> SaveState(Controller& controller)
{
  std::vector<u8> data(1024);
  StateWrapper sw(std::span<u8>(data), StateWrapper::Mode::Write, SAVE_STATE_VERSION);
  EXPECT_TRUE(controller.DoState(sw, true));
  EXPECT_FALSE(sw.HasError());
  data.resize(sw.GetPosition());
  return data;
}

void LoadState(Controller& controller, const std::vector<u8>& data, bool apply_input_state, u32 version)
{
  StateWrapper sw(std::span<const u8>(data), StateWrapper::Mode::Read, version);
  EXPECT_TRUE(controller.DoState(sw, apply_input_state));
  EXPECT_FALSE(sw.HasError());
  EXPECT_EQ(sw.GetPosition(), data.size());
}

void LoadDefaultSettings(Controller& controller)
{
  INISettingsInterface si;
  controller.LoadSettings(si, "Pad1", true);
}

const Controller::ControllerBindingInfo* FindBinding(const Controller::ControllerInfo& info, std::string_view name)
{
  for (const Controller::ControllerBindingInfo& binding : info.bindings)
  {
    if (name == binding.name)
      return &binding;
  }

  return nullptr;
}

bool HasSetting(const Controller::ControllerInfo& info, std::string_view name)
{
  for (const SettingInfo& setting : info.settings)
  {
    if (name == setting.name)
      return true;
  }

  return false;
}

u32 GetBindIndex(const Controller::ControllerInfo& info, std::string_view name)
{
  const Controller::ControllerBindingInfo* binding = FindBinding(info, name);
  EXPECT_NE(binding, nullptr) << name;
  return binding ? binding->bind_index : 0xFFFFFFFFu;
}

void ControllerSettingsTest::SetUp()
{
  m_old_settings = g_settings;
  g_settings.apply_compatibility_settings = true;
}

void ControllerSettingsTest::TearDown()
{
  g_settings = m_old_settings;
}

void ControllerSettingsTest::SetCanStartInAnalogMode(bool enabled)
{
  g_settings.apply_compatibility_settings = !enabled;
}

void PadTest::SetUpTestSuite()
{
  TimingEvents::Initialize();
  System::UpdateOverclock();
  DeactivateAllEvents();
}

void PadTest::SetUp()
{
  CPU::g_state.cop0_regs.sr.bits = 0;
  CPU::g_state.pending_ticks = 0;
  DeactivateAllEvents();
  TimingEvents::Reset();
  InterruptController::Reset();

  // Time can't advance without something to count down to.
  m_idle_event.Activate();

  Pad::Initialize();
  for (u32 i = 0; i < NUM_MULTITAPS; i++)
    Pad::SetMultitapEnabled(i, false);

  Pad::WriteRegister(JOY_MODE, BIOS_JOY_MODE);
  Pad::WriteRegister(JOY_BAUD, BIOS_JOY_BAUD);
}

void PadTest::TearDown()
{
  Pad::Shutdown();
  for (u32 i = 0; i < NUM_MULTITAPS; i++)
    Pad::SetMultitapEnabled(i, false);
  DeactivateAllEvents();
}

void PadTest::DeactivateAllEvents()
{
  while (TimingEvent* event = *TimingEvents::GetHeadEventPtr())
    event->Deactivate();
}

void PadTest::Run(u32 ticks)
{
  for (u32 i = 0; i < ticks; i++)
  {
    CPU::AddPendingTicks(1);
    if (CPU::GetPendingTicks() >= CPU::g_state.downcount)
      TimingEvents::RunEvents();
  }
}

bool PadTest::RXReady()
{
  return (Pad::ReadRegister(JOY_STAT) & STAT_RXFIFONEMPTY) != 0;
}

bool PadTest::PadIRQPending()
{
  return (InterruptController::ReadRegister(0x00) & PAD_IRQ_BIT) != 0;
}

void PadTest::PlugController(u32 index)
{
  Pad::SetController(index, DigitalController::Create(index, ControllerType::DigitalController));
}

void PadTest::PlugAnalogController(u32 index)
{
  Pad::SetController(index, AnalogController::Create(index));
}

MemoryCard* PadTest::PlugMemoryCard(u32 index)
{
  Pad::SetMemoryCard(index, MemoryCard::Create(index));
  return Pad::GetMemoryCard(index);
}

void PadTest::Press(u32 index, DigitalController::Button button)
{
  Pad::GetController(index)->SetBindState(static_cast<u32>(button), 1.0f);
}

void PadTest::Select(u32 slot, u32 extra_bits)
{
  m_ctrl = CTRL_TXEN | CTRL_SELECT | extra_bits | (slot ? CTRL_SLOT2 : 0u);
  Pad::WriteRegister(JOY_CTRL, m_ctrl);
}

void PadTest::Deselect()
{
  Pad::WriteRegister(JOY_CTRL, 0);
}

void PadTest::AckIRQ()
{
  Pad::WriteRegister(JOY_CTRL, m_ctrl | CTRL_ACK);
  InterruptController::WriteRegister(0x00, ~PAD_IRQ_BIT);
}

PadTest::Reply PadTest::Exchange(u8 value)
{
  Pad::WriteRegister(JOY_DATA, value);
  EXPECT_EQ(RunUntil(RXReady, TRANSFER_TICKS * 2), TRANSFER_TICKS) << "TX of 0x" << std::hex << u32(value);

  Reply reply;
  reply.data = Truncate8(Pad::ReadRegister(JOY_DATA));
  reply.ack_ticks = RunUntil(PadIRQPending, ACK_WINDOW);
  reply.ack = (reply.ack_ticks != NOT_REACHED);
  AckIRQ();
  return reply;
}

std::vector<u8> PadTest::Command(const std::vector<u8>& tx)
{
  std::vector<u8> rx;
  m_ack_ticks.clear();
  for (size_t i = 0; i < tx.size(); i++)
  {
    const Reply reply = Exchange(tx[i]);
    rx.push_back(reply.data);
    if (reply.ack)
      m_ack_ticks.push_back(reply.ack_ticks);

    const bool expected_ack = (i + 1) < tx.size();
    EXPECT_EQ(reply.ack, expected_ack) << "byte " << i;
    if (reply.ack != expected_ack)
      break;
  }

  EXPECT_FALSE(Pad::IsTransmitting());
  return rx;
}

void PadTest::EnterAnalogMode(u32 slot, u8 first)
{
  Select(slot);
  EXPECT_EQ(Command(EnterConfigModeCommand(first)), PollReply(NO_BUTTONS));
  Deselect();
  Select(slot);
  EXPECT_EQ(Command(SetAnalogModeCommand(first)), ANALOG_CONFIG_REPLY);
  Deselect();
  Select(slot);
  EXPECT_EQ(Command(ExitConfigModeCommand(first)), ANALOG_CONFIG_REPLY);
  Deselect();
}

} // namespace ControllerTestUtils
