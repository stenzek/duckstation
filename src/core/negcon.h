// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "negcon_base.h"

#include <memory>

class NeGcon final : public NegConBase
{
public:
  static const Controller::ControllerInfo INFO;

  explicit NeGcon(u32 index);
  ~NeGcon() override;

  static std::unique_ptr<NeGcon> Create(u32 index);

  ControllerType GetType() const override;

  void Reset() override;
  bool DoState(StateWrapper& sw, bool apply_input_state) override;

  void ResetTransferState() override;
  bool Transfer(const u8 data_in, u8* data_out) override;

private:
  enum class TransferState : u8
  {
    Idle,
    Ready,
    IDMSB,
    ButtonsLSB,
    ButtonsMSB,
    AnalogSteering,
    AnalogI,
    AnalogII,
    AnalogL
  };

  TransferState m_transfer_state = TransferState::Idle;
};
