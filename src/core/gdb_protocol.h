// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once
#include <string_view>

namespace GDBProtocol {
bool IsPacketInterrupt(std::string_view data);
bool IsPacketContinue(std::string_view data);

bool IsPacketComplete(std::string_view data);
std::string ProcessPacket(std::string_view data);
} // namespace GDBProtocol
