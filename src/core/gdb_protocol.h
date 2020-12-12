#pragma once
#include <string_view>

namespace GDBProtocol
{

  bool IsPacketComplete(const std::string_view& data);
  std::string ProcessPacket(const std::string_view& data);

} // namespace GDBProtocol
