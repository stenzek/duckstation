// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once
#include <string_view>

namespace GDBProtocol
{

  bool IsPacketInterrupt(const std::string_view& data);
  bool IsPacketContinue(const std::string_view& data);

  bool IsPacketComplete(const std::string_view& data);
  std::string ProcessPacket(const std::string_view& data);

} // namespace GDBProtocol
