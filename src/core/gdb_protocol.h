#pragma once
#include <string_view>

namespace GDBProtocol
{

  bool IsPacketInterrupt(const std::string_view& data);
  bool IsPacketContinue(const std::string_view& data);

  bool IsPacketComplete(const std::string_view& data);
  std::string ProcessPacket(const std::string_view& data);

} // namespace GDBProtocol
