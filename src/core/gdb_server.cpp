// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com> and contributors.
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
#include "common/thirdparty/SmallVector.h"

#include "util/sockets.h"

#include <optional>

LOG_CHANNEL(GDBServer);

namespace GDBServer {

namespace {
class ClientSocket final : public BufferedStreamSocket
{
public:
  ClientSocket(SocketMultiplexer& multiplexer, SocketDescriptor descriptor);
  ~ClientSocket() override;

  void OnSystemPaused();
  void OnSystemResumed();

  void SendReplyWithAck(std::string_view reply = std::string_view());

protected:
  void OnConnected() override;
  void OnDisconnected(const Error& error) override;
  void OnRead() override;

private:
  void SendPacket(std::string_view sv);

  bool m_seen_resume = false;
};
} // namespace

static u8 ComputeChecksum(std::string_view str);

static bool Cmd$_questionMark(ClientSocket* client, std::string_view data);
static bool Cmd$g(ClientSocket* client, std::string_view data);
static bool Cmd$G(ClientSocket* client, std::string_view data);
static bool Cmd$m(ClientSocket* client, std::string_view data);
static bool Cmd$M(ClientSocket* client, std::string_view data);
static bool Cmd$s(ClientSocket* client, std::string_view data);
static bool Cmd$z1(ClientSocket* client, std::string_view data);
static bool Cmd$Z1(ClientSocket* client, std::string_view data);
static bool Cmd$vMustReplyEmpty(ClientSocket* client, std::string_view data);
static bool Cmd$qSupported(ClientSocket* client, std::string_view data);

static bool IsPacketAck(std::string_view data);
static bool IsPacketInterrupt(std::string_view data);
static bool IsPacketContinue(std::string_view data);

static bool IsPacketComplete(std::string_view data);
static bool ProcessPacket(ClientSocket* socket, std::string_view data);

/// yikes, lots of stack space
using LargeReplyPacket = SmallStackString<768>;

/// Number of registers in GDB remote protocol for MIPS III.
constexpr int NUM_GDB_REGISTERS = 73;

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

/// List of all GDB remote protocol packets supported by us.
static constexpr std::pair<std::string_view, bool (*)(ClientSocket*, std::string_view)> COMMANDS[] = {
  {"?", Cmd$_questionMark},
  {"g", Cmd$g},
  {"G", Cmd$G},
  {"m", Cmd$m},
  {"M", Cmd$M},
  {"s", Cmd$s},
  {"z0,", Cmd$z1},
  {"Z0,", Cmd$Z1},
  {"z1,", Cmd$z1},
  {"Z1,", Cmd$Z1},
  {"vMustReplyEmpty", Cmd$vMustReplyEmpty},
  {"qSupported", Cmd$qSupported},
};

static std::shared_ptr<ListenSocket> s_gdb_listen_socket;
static std::vector<std::shared_ptr<ClientSocket>> s_gdb_clients;

} // namespace GDBServer

u8 GDBServer::ComputeChecksum(std::string_view str)
{
  u8 checksum = 0;
  for (char c : str)
    checksum = (checksum + c) % 256;

  return checksum;
}

/// Get stop reason.
bool GDBServer::Cmd$_questionMark(ClientSocket* client, std::string_view data)
{
  client->SendReplyWithAck("S02");
  return true;
}

/// Get general registers.
bool GDBServer::Cmd$g(ClientSocket* client, std::string_view data)
{
  LargeReplyPacket reply;

  for (const u32* reg : REGISTERS)
  {
    // Data is in host order (little endian).
    reply.append_format("{:02x}{:02x}{:02x}{:02x}", *reg & 0xFFu, (*reg >> 8) & 0xFFu, (*reg >> 16) & 0xFFu,
                        (*reg >> 24));
  }

  // Pad with dummy data (FP registers stuff).
  for (int i = 0; i < NUM_GDB_REGISTERS - static_cast<int>(REGISTERS.size()); i++)
    reply.append("00000000");

  client->SendReplyWithAck(reply);
  return true;
}

/// Set general registers.
bool GDBServer::Cmd$G(ClientSocket* client, std::string_view data)
{
  if (data.size() == NUM_GDB_REGISTERS * 8)
  {
    size_t offset = 0;

    for (u32* reg : REGISTERS)
    {
      // Data is in host order (little endian).
      const std::string_view tex_value = data.substr(offset, 8);
      std::array<u8, 4> le_value;
      if (StringUtil::DecodeHex(le_value, tex_value) == 4)
      {
        *reg = ZeroExtend32(le_value[0]) | (ZeroExtend32(le_value[1]) << 8) | (ZeroExtend32(le_value[2]) << 16) |
               (ZeroExtend32(le_value[3]) << 16);
      }
      else
      {
        ERROR_LOG("Invalid register set value: {}", tex_value);
      }

      offset += 8;
    }
  }
  else
  {
    ERROR_LOG("Wrong payload size for 'G' command, expected {} got {}", NUM_GDB_REGISTERS * 8, data.size());
  }

  client->SendReplyWithAck();
  return true;
}

/// Get memory.
bool GDBServer::Cmd$m(ClientSocket* client, std::string_view data)
{
  // address,length
  std::string_view caret = data;
  std::optional<VirtualMemoryAddress> address;
  std::optional<u32> length;
  if (!(address = StringUtil::FromChars<VirtualMemoryAddress>(caret, 16, &caret)).has_value() || caret.empty() ||
      caret[0] != ',' || !(length = StringUtil::FromChars<u32>(caret.substr(1), 16)).has_value())
  {
    ERROR_LOG("Invalid packet: {}", data);
    return false;
  }

  // large enough for most requests
  llvm::SmallVector<u8, 128> buffer;
  buffer.resize_for_overwrite(length.value());
  if (!CPU::SafeReadMemoryBytes(address.value(), buffer.data(), length.value()))
  {
    ERROR_LOG("Failed to read {} bytes from address 0x{:08X}", buffer.size(), address.value());
    client->SendReplyWithAck("E00");
    return true;
  }

  SmallString reply;
  reply.append_hex(buffer.data(), buffer.size());
  client->SendReplyWithAck(reply);
  return true;
}

/// Set memory.
bool GDBServer::Cmd$M(ClientSocket* client, std::string_view data)
{
  // address,length:data
  std::string_view caret = data;
  std::optional<VirtualMemoryAddress> address;
  std::optional<u32> length;
  if (!(address = StringUtil::FromChars<VirtualMemoryAddress>(caret, 16, &caret)).has_value() || caret.empty() ||
      caret[0] != ',' || !(length = StringUtil::FromChars<u32>(caret.substr(1), 16, &caret)).has_value() ||
      caret.empty() || caret[0] != ':')
  {
    ERROR_LOG("Invalid packet: {}", data);
    return false;
  }

  // remove ':'
  caret = caret.substr(1);
  if (length.value() != (caret.size() / 2))
  {
    ERROR_LOG("Invalid length in packet {}", data);
    return false;
  }

  // large enough for most requests
  llvm::SmallVector<u8, 128> buffer;
  buffer.resize_for_overwrite(length.value());
  if (!StringUtil::DecodeHex(buffer, caret))
  {
    ERROR_LOG("Invalid hex in packet {}", data);
    return false;
  }

  if (!CPU::SafeWriteMemoryBytes(address.value(), buffer))
  {
    ERROR_LOG("Failed to write {} bytes to {}", buffer.size(), address.value());
    client->SendReplyWithAck("E00");
    return true;
  }

  client->SendReplyWithAck("OK");
  return true;
}

/// Single step.
bool GDBServer::Cmd$s(ClientSocket* client, std::string_view data)
{
  System::SingleStepCPU();
  client->SendReplyWithAck("OK");
  return true;
}

/// Remove hardware breakpoint.
bool GDBServer::Cmd$z1(ClientSocket* client, std::string_view data)
{
  const std::optional<VirtualMemoryAddress> address = StringUtil::FromChars<VirtualMemoryAddress>(data, 16);
  if (address.has_value())
  {
    CPU::RemoveBreakpoint(CPU::BreakpointType::Execute, *address);
    client->SendReplyWithAck("OK");
    return true;
  }
  else
  {
    ERROR_LOG("Invalid address to remove hw breakpoint: ", data);
    client->SendReplyWithAck();
    return false;
  }
}

/// Insert hardware breakpoint.
bool GDBServer::Cmd$Z1(ClientSocket* client, std::string_view data)
{
  const std::optional<VirtualMemoryAddress> address = StringUtil::FromChars<VirtualMemoryAddress>(data, 16);
  if (address)
  {
    CPU::AddBreakpoint(CPU::BreakpointType::Execute, *address, false);
    client->SendReplyWithAck("OK");
    return true;
  }
  else
  {
    ERROR_LOG("Invalid address to insert hw breakpoint: ", data);
    return false;
  }
}

bool GDBServer::Cmd$vMustReplyEmpty(ClientSocket* client, std::string_view data)
{
  client->SendReplyWithAck();
  return true;
}

bool GDBServer::Cmd$qSupported(ClientSocket* client, std::string_view data)
{
  client->SendReplyWithAck();
  return true;
}

bool GDBServer::IsPacketAck(std::string_view data)
{
  DebugAssert(data.size() >= 1);
  return (data[0] == '+' || data[0] == '-');
}

bool GDBServer::IsPacketInterrupt(std::string_view data)
{
  DebugAssert(data.size() >= 1);
  return (data[data.size() - 1] == '\003');
}

bool GDBServer::IsPacketContinue(std::string_view data)
{
  return (data.size() >= 5) && (data.substr(data.size() - 5) == "$c#63");
}

bool GDBServer::IsPacketComplete(std::string_view data)
{
  return ((data.size() == 1) && (data[0] == '\003')) || ((data.size() > 3) && (*(data.end() - 3) == '#'));
}

bool GDBServer::ProcessPacket(ClientSocket* client, std::string_view data)
{
  // Validate packet.
  if ((data.size() < 4) || (data[0] != '$') || (data[data.size() - 3] != '#'))
  {
    ERROR_LOG("Invalid packet: {}", data);
    return false;
  }

  // Verify checksum.
  const std::string_view request = data.substr(1, data.size() - 4);
  const u8 packet_checksum = StringUtil::FromChars<u8>(data.substr(data.size() - 2, 2), 16).value_or(0);
  const u8 computed_checksum = ComputeChecksum(request);
  if (packet_checksum != computed_checksum)
  {
    ERROR_LOG("Incorrect checksum, expected 0x{:02x} got 0x{:02x} for '{}'", computed_checksum, packet_checksum, data);
    return false;
  }

  // Try to invoke packet command.
  for (const auto& command : COMMANDS)
  {
    if (request.starts_with(command.first))
    {
      DEV_LOG("Processing command '{}'", command.first);

      // Invoke command, remove command name from payload.
      return command.second(client, request.substr(command.first.size()));
    }
  }

  // Don't bail out on unknown command
  WARNING_LOG("Failed to process packet '{}'", request);
  client->SendReplyWithAck({});
  return true;
}

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

      if (GDBServer::IsPacketAck(current_packet))
      {
        // Eat ACKs.
        if (current_packet[0] == '-')
          ERROR_LOG("Received negative ack");

        packet_complete = true;
        break;
      }
      else if (GDBServer::IsPacketInterrupt(current_packet))
      {
        DEV_LOG("{} > Interrupt request", GetRemoteAddress().ToString());
        System::PauseSystem(true);
        packet_complete = true;
        break;
      }
      else if (GDBServer::IsPacketContinue(current_packet))
      {
        DEV_LOG("{} > Continue request", GetRemoteAddress().ToString());
        System::PauseSystem(false);
        packet_complete = true;
        break;
      }
      else if (GDBServer::IsPacketComplete(current_packet))
      {
        // TODO: Make this not copy.
        DEV_LOG("{} > {}", GetRemoteAddress().ToString(), current_packet);
        if (!ProcessPacket(this, current_packet))
          SendPacket("-");

        packet_complete = true;
        break;
      }
    }

    if (!packet_complete)
    {
      WARNING_LOG(
        "Incomplete packet, got {} bytes: {}", buffer.size() - buffer_offset,
        std::string_view(reinterpret_cast<const char*>(buffer.data() + buffer_offset), buffer.size() - buffer_offset));
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
  GDBServer::ProcessPacket(this, "$?#3f");
}

void GDBServer::ClientSocket::OnSystemResumed()
{
  m_seen_resume = true;

  // Send ack, in case GDB sent a continue request.
  SendPacket("+");
}

void GDBServer::ClientSocket::SendReplyWithAck(std::string_view reply)
{
  SendPacket(SmallString::from_format("+${}#{:02x}", reply, ComputeChecksum(reply)));
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
