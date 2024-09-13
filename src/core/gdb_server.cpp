// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gdb_server.h"
#include "bus.h"
#include "cpu_core.h"
#include "cpu_core_private.h"
#include "system.h"

#include "common/assert.h"
#include "common/log.h"
#include "common/small_string.h"
#include "common/string_util.h"

#include "util/sockets.h"

#include <functional>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <string>

Log_SetChannel(GDBProtocol);

namespace GDBProtocol {
static bool IsPacketInterrupt(std::string_view data);
static bool IsPacketContinue(std::string_view data);

static bool IsPacketComplete(std::string_view data);
static std::string ProcessPacket(std::string_view data);
} // namespace GDBProtocol

namespace GDBProtocol {

static u8* GetMemoryPointer(PhysicalMemoryAddress address, u32 length)
{
  auto region = Bus::GetMemoryRegionForAddress(address);
  if (region)
  {
    u8* data = GetMemoryRegionPointer(*region);
    if (data && (address + length <= GetMemoryRegionEnd(*region)))
    {
      return data + (address - GetMemoryRegionStart(*region));
    }
  }

  return nullptr;
}

static u8 ComputeChecksum(std::string_view str)
{
  u8 checksum = 0;
  for (char c : str)
  {
    checksum = (checksum + c) % 256;
  }
  return checksum;
}

static std::optional<std::string_view> DeserializePacket(std::string_view in)
{
  if ((in.size() < 4) || (in[0] != '$') || (in[in.size() - 3] != '#'))
  {
    return std::nullopt;
  }
  std::string_view data = in.substr(1, in.size() - 4);

  u8 packetChecksum = StringUtil::FromChars<u8>(in.substr(in.size() - 2, 2), 16).value_or(0);
  u8 computedChecksum = ComputeChecksum(data);

  if (packetChecksum == computedChecksum)
  {
    return {data};
  }
  else
  {
    return std::nullopt;
  }
}

static std::string SerializePacket(std::string_view in)
{
  std::stringstream ss;
  ss << '$' << in << '#' << TinyString::from_format("{:02x}", ComputeChecksum(in));
  return ss.str();
}

/// List of GDB remote protocol registers for MIPS III (excluding FP).
static const std::array<u32*, 38> REGISTERS{
  &CPU::g_state.regs.r[0],
  &CPU::g_state.regs.r[1],
  &CPU::g_state.regs.r[2],
  &CPU::g_state.regs.r[3],
  &CPU::g_state.regs.r[4],
  &CPU::g_state.regs.r[5],
  &CPU::g_state.regs.r[6],
  &CPU::g_state.regs.r[7],
  &CPU::g_state.regs.r[8],
  &CPU::g_state.regs.r[9],
  &CPU::g_state.regs.r[10],
  &CPU::g_state.regs.r[11],
  &CPU::g_state.regs.r[12],
  &CPU::g_state.regs.r[13],
  &CPU::g_state.regs.r[14],
  &CPU::g_state.regs.r[15],
  &CPU::g_state.regs.r[16],
  &CPU::g_state.regs.r[17],
  &CPU::g_state.regs.r[18],
  &CPU::g_state.regs.r[19],
  &CPU::g_state.regs.r[20],
  &CPU::g_state.regs.r[21],
  &CPU::g_state.regs.r[22],
  &CPU::g_state.regs.r[23],
  &CPU::g_state.regs.r[24],
  &CPU::g_state.regs.r[25],
  &CPU::g_state.regs.r[26],
  &CPU::g_state.regs.r[27],
  &CPU::g_state.regs.r[28],
  &CPU::g_state.regs.r[29],
  &CPU::g_state.regs.r[30],
  &CPU::g_state.regs.r[31],

  &CPU::g_state.cop0_regs.sr.bits,
  &CPU::g_state.regs.lo,
  &CPU::g_state.regs.hi,
  &CPU::g_state.cop0_regs.BadVaddr,
  &CPU::g_state.cop0_regs.cause.bits,
  &CPU::g_state.pc,
};

/// Number of registers in GDB remote protocol for MIPS III.
constexpr int NUM_GDB_REGISTERS = 73;

/// Get stop reason.
static std::optional<std::string> Cmd$_questionMark(std::string_view data)
{
  return {"S02"};
}

/// Get general registers.
static std::optional<std::string> Cmd$g(std::string_view data)
{
  std::stringstream ss;

  for (u32* reg : REGISTERS)
  {
    // Data is in host order (little endian).
    ss << StringUtil::EncodeHex(reinterpret_cast<u8*>(reg), 4);
  }

  // Pad with dummy data (FP registers stuff).
  for (int i = 0; i < NUM_GDB_REGISTERS - static_cast<int>(REGISTERS.size()); i++)
  {
    ss << "00000000";
  }

  return {ss.str()};
}

/// Set general registers.
static std::optional<std::string> Cmd$G(std::string_view data)
{
  if (data.size() == NUM_GDB_REGISTERS * 8)
  {
    int offset = 0;

    for (u32* reg : REGISTERS)
    {
      // Data is in host order (little endian).
      auto value = StringUtil::DecodeHex({data.data() + offset, 8});
      if (value)
      {
        *reg = *reinterpret_cast<u32*>(&(*value)[0]);
      }
      offset += 8;
    }
  }
  else
  {
    ERROR_LOG("Wrong payload size for 'G' command, expected {} got {}", NUM_GDB_REGISTERS * 8, data.size());
  }

  return {""};
}

/// Get memory.
static std::optional<std::string> Cmd$m(std::string_view data)
{
  std::stringstream ss{std::string{data}};
  std::string dataAddress, dataLength;

  std::getline(ss, dataAddress, ',');
  std::getline(ss, dataLength, '\0');

  auto address = StringUtil::FromChars<VirtualMemoryAddress>(dataAddress, 16);
  auto length = StringUtil::FromChars<u32>(dataLength, 16);

  if (address && length)
  {
    PhysicalMemoryAddress phys_addr = *address & CPU::PHYSICAL_MEMORY_ADDRESS_MASK;
    u32 phys_length = *length;

    u8* ptr_data = GetMemoryPointer(phys_addr, phys_length);
    if (ptr_data)
    {
      return {StringUtil::EncodeHex(ptr_data, phys_length)};
    }
  }
  return {"E00"};
}

/// Set memory.
static std::optional<std::string> Cmd$M(std::string_view data)
{
  std::stringstream ss{std::string{data}};
  std::string dataAddress, dataLength, dataPayload;

  std::getline(ss, dataAddress, ',');
  std::getline(ss, dataLength, ':');
  std::getline(ss, dataPayload, '\0');

  auto address = StringUtil::FromChars<VirtualMemoryAddress>(dataAddress, 16);
  auto length = StringUtil::FromChars<u32>(dataLength, 16);
  auto payload = StringUtil::DecodeHex(dataPayload);

  if (address && length && payload && (payload->size() == *length))
  {
    u32 phys_addr = *address & CPU::PHYSICAL_MEMORY_ADDRESS_MASK;
    u32 phys_length = *length;

    u8* ptr_data = GetMemoryPointer(phys_addr, phys_length);
    if (ptr_data)
    {
      memcpy(ptr_data, payload->data(), phys_length);
      return {"OK"};
    }
  }

  return {"E00"};
}

/// Remove hardware breakpoint.
static std::optional<std::string> Cmd$z1(std::string_view data)
{
  auto address = StringUtil::FromChars<VirtualMemoryAddress>(data, 16);
  if (address)
  {
    CPU::RemoveBreakpoint(CPU::BreakpointType::Execute, *address);
    return {"OK"};
  }
  else
  {
    return std::nullopt;
  }
}

/// Insert hardware breakpoint.
static std::optional<std::string> Cmd$Z1(std::string_view data)
{
  auto address = StringUtil::FromChars<VirtualMemoryAddress>(data, 16);
  if (address)
  {
    CPU::AddBreakpoint(CPU::BreakpointType::Execute, *address, false);
    return {"OK"};
  }
  else
  {
    return std::nullopt;
  }
}

static std::optional<std::string> Cmd$vMustReplyEmpty(std::string_view data)
{
  return {""};
}

static std::optional<std::string> Cmd$qSupported(std::string_view data)
{
  return {""};
}

/// List of all GDB remote protocol packets supported by us.
static const std::map<const char*, std::function<std::optional<std::string>(std::string_view)>> COMMANDS{
  {"?", Cmd$_questionMark},
  {"g", Cmd$g},
  {"G", Cmd$G},
  {"m", Cmd$m},
  {"M", Cmd$M},
  {"z0,", Cmd$z1},
  {"Z0,", Cmd$Z1},
  {"z1,", Cmd$z1},
  {"Z1,", Cmd$Z1},
  {"vMustReplyEmpty", Cmd$vMustReplyEmpty},
  {"qSupported", Cmd$qSupported},
};

bool IsPacketInterrupt(std::string_view data)
{
  return (data.size() >= 1) && (data[data.size() - 1] == '\003');
}

bool IsPacketContinue(std::string_view data)
{
  return (data.size() >= 5) && (data.substr(data.size() - 5) == "$c#63");
}

bool IsPacketComplete(std::string_view data)
{
  return ((data.size() == 1) && (data[0] == '\003')) || ((data.size() > 3) && (*(data.end() - 3) == '#'));
}

std::string ProcessPacket(std::string_view data)
{
  std::string_view trimmedData = data;

  // Eat ACKs.
  while (!trimmedData.empty() && (trimmedData[0] == '+' || trimmedData[0] == '-'))
  {
    if (trimmedData[0] == '-')
    {
      ERROR_LOG("Received negative ack");
    }
    trimmedData = trimmedData.substr(1);
  }

  // Validate packet.
  auto packet = DeserializePacket(trimmedData);
  if (!packet)
  {
    ERROR_LOG("Malformed packet '{}'", trimmedData);
    return "-";
  }

  std::optional<std::string> reply = {""};

  // Try to invoke packet command.
  bool processed = false;
  for (const auto& command : COMMANDS)
  {
    if (packet->starts_with(command.first))
    {
      DEBUG_LOG("Processing command '{}'", command.first);

      // Invoke command, remove command name from payload.
      reply = command.second(packet->substr(strlen(command.first)));
      processed = true;
      break;
    }
  }

  if (!processed)
    WARNING_LOG("Failed to process packet '{}'", trimmedData);

  return reply ? "+" + SerializePacket(*reply) : "+";
}

} // namespace GDBProtocol

namespace GDBServer {

namespace {
class ClientSocket final : public BufferedStreamSocket
{
public:
  ClientSocket(SocketMultiplexer& multiplexer, SocketDescriptor descriptor);
  ~ClientSocket() override;

  void OnSystemPaused();
  void OnSystemResumed();

protected:
  void OnConnected() override;
  void OnDisconnected(const Error& error) override;
  void OnRead() override;

private:
  void SendPacket(std::string_view sv);

  bool m_seen_resume = false;
};
} // namespace

static std::shared_ptr<ListenSocket> s_gdb_listen_socket;
static std::vector<std::shared_ptr<ClientSocket>> s_gdb_clients;
} // namespace GDBServer

GDBServer::ClientSocket::ClientSocket(SocketMultiplexer& multiplexer, SocketDescriptor descriptor)
  : BufferedStreamSocket(multiplexer, descriptor, 65536, 65536)
{
}

GDBServer::ClientSocket::~ClientSocket() = default;

void GDBServer::ClientSocket::OnConnected()
{
  INFO_LOG("Client {} connected.", GetRemoteAddress().ToString());

  m_seen_resume = System::IsPaused();
  System::PauseSystem(true);

  s_gdb_clients.push_back(std::static_pointer_cast<ClientSocket>(shared_from_this()));
}

void GDBServer::ClientSocket::OnDisconnected(const Error& error)
{
  INFO_LOG("Client {} disconnected: {}", GetRemoteAddress().ToString(), error.GetDescription());

  const auto iter = std::find_if(s_gdb_clients.begin(), s_gdb_clients.end(),
                                 [this](const std::shared_ptr<ClientSocket>& rhs) { return (rhs.get() == this); });
  if (iter == s_gdb_clients.end())
  {
    ERROR_LOG("Unknown GDB client disconnected? This should never happen.");
    return;
  }

  s_gdb_clients.erase(iter);
}

void GDBServer::ClientSocket::OnRead()
{
  const std::span<const u8> buffer = AcquireReadBuffer();
  if (buffer.empty())
    return;

  size_t buffer_offset = 0;
  while (buffer_offset < buffer.size())
  {
    size_t current_packet_size = 1;
    bool packet_complete = false;
    for (; (buffer_offset + current_packet_size) <= buffer.size(); current_packet_size++)
    {
      const std::string_view current_packet(reinterpret_cast<const char*>(buffer.data() + buffer_offset),
                                            current_packet_size);

      if (GDBProtocol::IsPacketInterrupt(current_packet))
      {
        DEV_LOG("{} > Interrupt request", GetRemoteAddress().ToString());
        System::PauseSystem(true);
        packet_complete = true;
        break;
      }
      else if (GDBProtocol::IsPacketContinue(current_packet))
      {
        DEV_LOG("{} > Continue request", GetRemoteAddress().ToString());
        System::PauseSystem(false);
        packet_complete = true;
        break;
      }
      else if (GDBProtocol::IsPacketComplete(current_packet))
      {
        // TODO: Make this not copy.
        DEV_LOG("{} > {}", GetRemoteAddress().ToString(), current_packet);
        SendPacket(GDBProtocol::ProcessPacket(current_packet));
        packet_complete = true;
        break;
      }
    }

    if (!packet_complete)
    {
      WARNING_LOG("Incomplete packet, got {} bytes.", buffer.size() - buffer_offset);
      break;
    }
    else
    {
      buffer_offset += current_packet_size;
    }
  }

  ReleaseReadBuffer(buffer_offset);
}

void GDBServer::ClientSocket::SendPacket(std::string_view sv)
{
  if (sv.empty())
    return;

  WARNING_LOG("Write: {}", sv);
  if (size_t written = Write(sv.data(), sv.length()); written != sv.length())
    ERROR_LOG("Only wrote {} of {} bytes.", written, sv.length());
}

void GDBServer::ClientSocket::OnSystemPaused()
{
  if (!m_seen_resume)
    return;

  m_seen_resume = false;

  // Generate a stop reply packet, insert '?' command to generate it.
  SendPacket(GDBProtocol::ProcessPacket("$?#3f"));
}

void GDBServer::ClientSocket::OnSystemResumed()
{
  m_seen_resume = true;

  // Send ack, in case GDB sent a continue request.
  SendPacket("+");
}

bool GDBServer::Initialize(u16 port)
{
  Error error;
  Assert(!s_gdb_listen_socket);

  const std::optional<SocketAddress> address =
    SocketAddress::Parse(SocketAddress::Type::IPv4, "127.0.0.1", port, &error);
  if (!address.has_value())
  {
    ERROR_LOG("Failed to parse address: {}", error.GetDescription());
    return false;
  }

  SocketMultiplexer* multiplexer = System::GetSocketMultiplexer();
  if (!multiplexer)
    return false;

  s_gdb_listen_socket = multiplexer->CreateListenSocket<ClientSocket>(address.value(), &error);
  if (!s_gdb_listen_socket)
  {
    ERROR_LOG("Failed to create listen socket: {}", error.GetDescription());
    System::ReleaseSocketMultiplexer();
    return false;
  }

  INFO_LOG("GDB server is now listening on {}.", address->ToString());
  return true;
}

bool GDBServer::HasAnyClients()
{
  return !s_gdb_clients.empty();
}

void GDBServer::Shutdown()
{
  if (!s_gdb_listen_socket)
    return;

  INFO_LOG("Disconnecting {} GDB clients...", s_gdb_clients.size());
  while (!s_gdb_clients.empty())
  {
    // maintain a reference so we don't delete while in scope
    std::shared_ptr<ClientSocket> client = s_gdb_clients.back();
    client->Close();
  }

  INFO_LOG("Stopping GDB server.");
  s_gdb_listen_socket->Close();
  s_gdb_listen_socket.reset();
  System::ReleaseSocketMultiplexer();
}

void GDBServer::OnSystemPaused()
{
  for (auto& it : s_gdb_clients)
    it->OnSystemPaused();
}

void GDBServer::OnSystemResumed()
{
  for (auto& it : s_gdb_clients)
    it->OnSystemResumed();
}
