// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gdb_server.h"

#ifdef ENABLE_GDB_SERVER

#include "bus.h"
#include "cpu_code_cache.h"
#include "cpu_core.h"
#include "cpu_core_private.h"
#include "system.h"

#include "common/assert.h"
#include "common/log.h"
#include "common/small_string.h"
#include "common/string_util.h"
#include "common/thirdparty/SmallVector.h"

#include "util/sockets.h"

#include <limits>
#include <optional>
#include <utility>

LOG_CHANNEL(GDBServer);

namespace GDBServer {

static constexpr u8 GDB_SIGNAL_INTERRUPT = 2;
static constexpr u8 GDB_SIGNAL_TRAP = 5;
static constexpr u32 MAX_PACKET_SIZE = 16384;
static constexpr u32 MAX_FRAMED_PACKET_SIZE = MAX_PACKET_SIZE + 4;
static constexpr u32 MAX_MEMORY_READ_SIZE = MAX_PACKET_SIZE / 2;
static constexpr u32 MAX_WATCHPOINT_LENGTH = 256;

namespace {

enum class GDBBreakpointType : u8
{
  Software,
  Hardware,
  Write,
  Read,
  Access,
};

struct BreakpointKey
{
  CPU::BreakpointType type;
  VirtualMemoryAddress address;

  bool operator==(const BreakpointKey&) const = default;
};

struct ManagedBreakpoint
{
  BreakpointKey key;
  u32 reference_count;
  bool added_by_gdb;
};

struct ClientBreakpoint
{
  GDBBreakpointType type;
  VirtualMemoryAddress address;
  u32 kind;

  bool operator==(const ClientBreakpoint&) const = default;
};

class ClientSocket final : public BufferedStreamSocket
{
public:
  ClientSocket(SocketMultiplexer& multiplexer, SocketDescriptor descriptor);
  ~ClientSocket() override;

  void OnBreakpointHit(CPU::BreakpointType type, VirtualMemoryAddress address);
  void OnSystemPaused(u8 signal);
  void OnSystemResumed();

  void SendAck();
  void SendReply(std::string_view reply = std::string_view());
  void SendLastStopReplyWithAck();
  void SendReplyWithAck(std::string_view reply = std::string_view());
  void ResendLastReply();

  void AddBreakpoint(GDBBreakpointType type, VirtualMemoryAddress address, u32 kind);
  void RemoveBreakpoint(GDBBreakpointType type, VirtualMemoryAddress address, u32 kind);
  void ClearBreakpoints();

protected:
  void OnConnected() override;
  void OnDisconnected(const Error& error) override;
  void OnRead() override;

private:
  void SendPacket(std::string_view sv);

  bool m_seen_resume = false;
  SmallString m_last_stop_reply;
  SmallString m_last_reply;
  std::vector<ClientBreakpoint> m_breakpoints;
};

} // namespace

static u8 ComputeChecksum(std::string_view str);

static bool Cmd$_questionMark(ClientSocket* client, std::string_view data);
static bool Cmd$g(ClientSocket* client, std::string_view data);
static bool Cmd$G(ClientSocket* client, std::string_view data);
static bool Cmd$H(ClientSocket* client, std::string_view data);
static bool Cmd$m(ClientSocket* client, std::string_view data);
static bool Cmd$M(ClientSocket* client, std::string_view data);
static bool Cmd$c(ClientSocket* client, std::string_view data);
static bool Cmd$C(ClientSocket* client, std::string_view data);
static bool Cmd$s(ClientSocket* client, std::string_view data);
static bool Cmd$S(ClientSocket* client, std::string_view data);
template<bool add_breakpoint>
static bool Cmd$z(ClientSocket* client, std::string_view data);
static bool Cmd$vMustReplyEmpty(ClientSocket* client, std::string_view data);
static bool Cmd$qSupported(ClientSocket* client, std::string_view data);

static bool ParseOptionalAddress(std::string_view data, std::optional<VirtualMemoryAddress>* address);
static bool ParseSignalAndOptionalAddress(std::string_view data, u8* signal,
                                          std::optional<VirtualMemoryAddress>* address);
static void InvalidateMemoryWrite(VirtualMemoryAddress address, u32 length);
static CPU::BreakpointCallbackAction OnBreakpointHit(CPU::BreakpointType type, VirtualMemoryAddress pc,
                                                     VirtualMemoryAddress address);

static bool IsPacketAck(std::string_view data);
static bool IsPacketInterrupt(std::string_view data);

static bool IsPacketComplete(std::string_view data);
static bool ProcessPacket(ClientSocket* socket, std::string_view data);

/// yikes, lots of stack space
using LargeReplyPacket = SmallStackString<768>;

/// Number of registers in GDB remote protocol for MIPS III.
static constexpr int NUM_GDB_REGISTERS = 73;

/// List of GDB remote protocol registers for MIPS III (excluding FP).
static constexpr std::array<u32*, 38> REGISTERS{
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
  {"H", Cmd$H},
  {"m", Cmd$m},
  {"M", Cmd$M},
  {"c", Cmd$c},
  {"C", Cmd$C},
  {"s", Cmd$s},
  {"S", Cmd$S},
  {"z", Cmd$z<false>},
  {"Z", Cmd$z<true>},
  {"vMustReplyEmpty", Cmd$vMustReplyEmpty},
  {"qSupported", Cmd$qSupported},
};

namespace {
struct Locals
{
  std::unique_ptr<SocketMultiplexer> multiplexer;
  std::shared_ptr<ListenSocket> listen_socket;
  std::vector<std::shared_ptr<ClientSocket>> clients;
  std::vector<ManagedBreakpoint> breakpoints;
  u8 pending_stop_signal = GDB_SIGNAL_TRAP;
  bool resume_on_last_disconnect = false;
};
} // namespace

ALIGN_TO_CACHE_LINE static Locals s_locals;

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
  if (!data.empty())
  {
    client->SendReplyWithAck("E01");
    return true;
  }

  client->SendLastStopReplyWithAck();
  return true;
}

/// Get general registers.
bool GDBServer::Cmd$g(ClientSocket* client, std::string_view data)
{
  if (!data.empty())
  {
    client->SendReplyWithAck("E01");
    return true;
  }

  LargeReplyPacket reply;

  for (const u32* reg : REGISTERS)
  {
    // Data is in host order (little endian).
    reply.append_format("{:02x}{:02x}{:02x}{:02x}", *reg & 0xFFu, (*reg >> 8) & 0xFFu, (*reg >> 16) & 0xFFu,
                        (*reg >> 24));
  }

  // Pad with dummy data (FP registers stuff).
  for (int i = 0; i < NUM_GDB_REGISTERS - static_cast<int>(REGISTERS.size()); i++)
    reply.append("xxxxxxxx");

  client->SendReplyWithAck(reply);
  return true;
}

/// Set general registers.
bool GDBServer::Cmd$G(ClientSocket* client, std::string_view data)
{
  std::array<u8, NUM_GDB_REGISTERS * sizeof(u32)> bytes;
  if (data.size() != bytes.size() * 2 || StringUtil::DecodeHex(bytes, data) != bytes.size())
  {
    ERROR_LOG("Invalid payload for 'G' command, expected {} hex digits", bytes.size() * 2);
    client->SendReplyWithAck("E01");
    return true;
  }

  std::array<u32, NUM_GDB_REGISTERS> values;
  for (size_t i = 0; i < values.size(); i++)
  {
    const size_t offset = i * sizeof(u32);
    values[i] = ZeroExtend32(bytes[offset]) | (ZeroExtend32(bytes[offset + 1]) << 8) |
                (ZeroExtend32(bytes[offset + 2]) << 16) | (ZeroExtend32(bytes[offset + 3]) << 24);
  }

  const u32 new_pc = values[37];
  if ((new_pc & 3u) != 0)
  {
    ERROR_LOG("Invalid PC 0x{:08X} in 'G' command", new_pc);
    client->SendReplyWithAck("E01");
    return true;
  }

  CPU::g_state.regs.r[0] = 0;
  for (size_t i = 1; i < 32; i++)
    CPU::g_state.regs.r[i] = values[i];

  CPU::g_state.cop0_regs.sr.bits = (CPU::g_state.cop0_regs.sr.bits & ~CPU::Cop0Registers::SR::WRITE_MASK) |
                                   (values[32] & CPU::Cop0Registers::SR::WRITE_MASK);
  CPU::UpdateMemoryPointers();

  CPU::g_state.regs.lo = values[33];
  CPU::g_state.regs.hi = values[34];

  CPU::g_state.cop0_regs.cause.bits = (CPU::g_state.cop0_regs.cause.bits & ~CPU::Cop0Registers::CAUSE::WRITE_MASK) |
                                      (values[36] & CPU::Cop0Registers::CAUSE::WRITE_MASK);
  CPU::CheckForPendingInterrupt();

  if (new_pc != CPU::g_state.pc)
    CPU::SetPC(new_pc);

  client->SendReplyWithAck("OK");
  return true;
}

/// Thread operations, ignored.
bool GDBServer::Cmd$H(ClientSocket* client, std::string_view data)
{
  WARNING_LOG("Ignoring thread command '{}'", data);
  client->SendReplyWithAck("OK");
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
      caret[0] != ',' || !(length = StringUtil::FromChars<u32>(caret.substr(1), 16, &caret)).has_value() ||
      !caret.empty())
  {
    ERROR_LOG("Invalid packet: {}", data);
    client->SendReplyWithAck("E01");
    return true;
  }

  if (length.value() > MAX_MEMORY_READ_SIZE)
  {
    ERROR_LOG("Memory read of {} bytes exceeds maximum of {}", length.value(), MAX_MEMORY_READ_SIZE);
    client->SendReplyWithAck("E01");
    return true;
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
    client->SendReplyWithAck("E01");
    return true;
  }

  // remove ':'
  caret = caret.substr(1);
  if (length.value() != (caret.size() / 2))
  {
    ERROR_LOG("Invalid length in packet {}", data);
    client->SendReplyWithAck("E01");
    return true;
  }

  // large enough for most requests
  llvm::SmallVector<u8, 128> buffer;
  buffer.resize_for_overwrite(length.value());
  if (StringUtil::DecodeHex(buffer, caret) != length.value())
  {
    ERROR_LOG("Invalid hex in packet {}", data);
    client->SendReplyWithAck("E01");
    return true;
  }

  if (!CPU::SafeWriteMemoryBytes(address.value(), buffer))
  {
    ERROR_LOG("Failed to write {} bytes to {}", buffer.size(), address.value());
    client->SendReplyWithAck("E00");
    return true;
  }

  InvalidateMemoryWrite(address.value(), length.value());
  client->SendReplyWithAck("OK");
  return true;
}

/// Continue.
bool GDBServer::Cmd$c(ClientSocket* client, std::string_view data)
{
  std::optional<VirtualMemoryAddress> address;
  if (!ParseOptionalAddress(data, &address))
  {
    ERROR_LOG("Invalid continue address: {}", data);
    client->SendReplyWithAck("E01");
    return true;
  }

  client->SendAck();
  if (address.has_value() && address.value() != CPU::g_state.pc)
    CPU::SetPC(address.value());
  System::PauseSystem(false);
  return true;
}

/// Continue with signal.
bool GDBServer::Cmd$C(ClientSocket* client, std::string_view data)
{
  u8 signal;
  std::optional<VirtualMemoryAddress> address;
  if (!ParseSignalAndOptionalAddress(data, &signal, &address))
  {
    ERROR_LOG("Invalid continue-with-signal packet: {}", data);
    client->SendReplyWithAck("E01");
    return true;
  }

  WARNING_LOG("Ignoring signal 0x{:02X} in continue packet", signal);
  client->SendAck();
  if (address.has_value() && address.value() != CPU::g_state.pc)
    CPU::SetPC(address.value());
  System::PauseSystem(false);
  return true;
}

/// Single step.
bool GDBServer::Cmd$s(ClientSocket* client, std::string_view data)
{
  std::optional<VirtualMemoryAddress> address;
  if (!ParseOptionalAddress(data, &address))
  {
    ERROR_LOG("Invalid single-step address: {}", data);
    client->SendReplyWithAck("E01");
    return true;
  }

  client->SendAck();
  if (address.has_value() && address.value() != CPU::g_state.pc)
    CPU::SetPC(address.value());
  System::SingleStepCPU();
  return true;
}

/// Single step with signal.
bool GDBServer::Cmd$S(ClientSocket* client, std::string_view data)
{
  u8 signal;
  std::optional<VirtualMemoryAddress> address;
  if (!ParseSignalAndOptionalAddress(data, &signal, &address))
  {
    ERROR_LOG("Invalid single-step-with-signal packet: {}", data);
    client->SendReplyWithAck("E01");
    return true;
  }

  WARNING_LOG("Ignoring signal 0x{:02X} in single-step packet", signal);
  client->SendAck();
  if (address.has_value() && address.value() != CPU::g_state.pc)
    CPU::SetPC(address.value());
  System::SingleStepCPU();
  return true;
}

/// Remove hardware breakpoint (z).
/// Insert hardware breakpoint (Z).
template<bool add_breakpoint>
bool GDBServer::Cmd$z(ClientSocket* client, std::string_view data)
{
  std::string_view caret = data;
  std::optional<u32> bptype;
  std::optional<VirtualMemoryAddress> bpaddr;
  std::optional<u32> bpkind;

  // type,addr,kind
  if (!(bptype = StringUtil::FromChars<u32>(caret, 10, &caret)) || caret.empty() || caret[0] != ',' ||
      !(bpaddr = StringUtil::FromChars<VirtualMemoryAddress>(caret.substr(1), 16, &caret)).has_value() ||
      caret.empty() || caret[0] != ',' ||
      !(bpkind = StringUtil::FromChars<u32>(caret.substr(1), 16, &caret)).has_value() || !caret.empty())
  {
    ERROR_LOG("Invalid {} hw breakpoint packet: {}", add_breakpoint ? "add" : "remove", data);
    client->SendReplyWithAck("E01");
    return true;
  }

  if (bptype.value() > static_cast<u32>(GDBBreakpointType::Access))
  {
    ERROR_LOG("Unknown breakpoint type {}", bptype.value());
    client->SendReplyWithAck();
    return true;
  }

  const GDBBreakpointType type = static_cast<GDBBreakpointType>(bptype.value());
  if (type == GDBBreakpointType::Software || type == GDBBreakpointType::Hardware)
  {
    if (bpkind.value() != sizeof(CPU::Instruction))
    {
      ERROR_LOG("Unsupported breakpoint kind {}", bpkind.value());
      client->SendReplyWithAck("E01");
      return true;
    }
  }
  else if (bpkind.value() == 0 || bpkind.value() > MAX_WATCHPOINT_LENGTH ||
           bpaddr.value() > (std::numeric_limits<VirtualMemoryAddress>::max() - (bpkind.value() - 1)))
  {
    ERROR_LOG("Invalid watchpoint length {} at 0x{:08X}", bpkind.value(), bpaddr.value());
    client->SendReplyWithAck("E01");
    return true;
  }

  if constexpr (add_breakpoint)
    client->AddBreakpoint(type, bpaddr.value(), bpkind.value());
  else
    client->RemoveBreakpoint(type, bpaddr.value(), bpkind.value());

  client->SendReplyWithAck("OK");
  return true;
}

template bool GDBServer::Cmd$z<false>(ClientSocket* client, std::string_view data);
template bool GDBServer::Cmd$z<true>(ClientSocket* client, std::string_view data);

bool GDBServer::Cmd$vMustReplyEmpty(ClientSocket* client, std::string_view data)
{
  client->SendReplyWithAck();
  return true;
}

bool GDBServer::Cmd$qSupported(ClientSocket* client, std::string_view data)
{
  client->SendReplyWithAck(TinyString::from_format("PacketSize={:x}", MAX_PACKET_SIZE));
  return true;
}

bool GDBServer::ParseOptionalAddress(std::string_view data, std::optional<VirtualMemoryAddress>* address)
{
  address->reset();
  if (data.empty())
    return true;

  std::string_view end;
  *address = StringUtil::FromChars<VirtualMemoryAddress>(data, 16, &end);
  return address->has_value() && end.empty() && ((address->value() & 3u) == 0);
}

bool GDBServer::ParseSignalAndOptionalAddress(std::string_view data, u8* signal,
                                              std::optional<VirtualMemoryAddress>* address)
{
  std::array<u8, 1> signal_bytes;
  if (data.size() < 2 || StringUtil::DecodeHex(signal_bytes, data.substr(0, 2)) != signal_bytes.size())
    return false;

  *signal = signal_bytes[0];
  data.remove_prefix(2);
  if (data.empty())
  {
    address->reset();
    return true;
  }

  return data[0] == ';' && data.size() > 1 && ParseOptionalAddress(data.substr(1), address) && address->has_value();
}

void GDBServer::InvalidateMemoryWrite(VirtualMemoryAddress address, u32 length)
{
  for (u32 offset = 0; offset < length;)
  {
    const VirtualMemoryAddress current_address = address + offset;
    CPU::InvalidateICacheAt(current_address);
    offset += std::min(length - offset, CPU::ICACHE_LINE_SIZE - (current_address & (CPU::ICACHE_LINE_SIZE - 1)));
  }

  for (u32 offset = 0; offset < length;)
  {
    const PhysicalMemoryAddress physical_address = (address + offset) & CPU::KSEG_MASK;
    if (Bus::IsRAMAddress(physical_address))
    {
      const u32 page = Bus::GetRAMCodePageIndex(physical_address);
      if (Bus::IsRAMCodePage(page))
        CPU::CodeCache::InvalidateBlocksWithPageIndex(page);
    }

    offset += std::min(length - offset, HOST_PAGE_SIZE - (physical_address & (HOST_PAGE_SIZE - 1)));
  }
}

CPU::BreakpointCallbackAction GDBServer::OnBreakpointHit(CPU::BreakpointType type, VirtualMemoryAddress,
                                                         VirtualMemoryAddress address)
{
  for (auto& it : s_locals.clients)
    it->OnBreakpointHit(type, address);

  return s_locals.clients.empty() ? CPU::BreakpointCallbackAction::Continue : CPU::BreakpointCallbackAction::Pause;
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
  std::array<u8, 1> packet_checksum;
  if (StringUtil::DecodeHex(packet_checksum, data.substr(data.size() - 2, 2)) != 1)
  {
    ERROR_LOG("Invalid checksum in packet '{}'", data);
    return false;
  }

  const u8 computed_checksum = ComputeChecksum(request);
  if (packet_checksum[0] != computed_checksum)
  {
    ERROR_LOG("Incorrect checksum, expected 0x{:02x} got 0x{:02x} for '{}'", computed_checksum, packet_checksum[0],
              data);
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
  m_last_stop_reply.format("S{:02x}", GDB_SIGNAL_INTERRUPT);
}

GDBServer::ClientSocket::~ClientSocket() = default;

void GDBServer::ClientSocket::AddBreakpoint(GDBBreakpointType type, VirtualMemoryAddress address, u32 kind)
{
  const ClientBreakpoint client_breakpoint{type, address, kind};
  if (std::ranges::find(m_breakpoints, client_breakpoint) != m_breakpoints.end())
    return;

  const auto acquire_breakpoint = [](CPU::BreakpointType cpu_type, VirtualMemoryAddress cpu_address) {
    const BreakpointKey key{cpu_type, cpu_address};
    const auto it = std::ranges::find_if(s_locals.breakpoints,
                                         [&key](const ManagedBreakpoint& breakpoint) { return breakpoint.key == key; });
    if (it != s_locals.breakpoints.end())
    {
      it->reference_count++;
      return;
    }

    const bool added_by_gdb = CPU::AddBreakpointWithCallback(cpu_type, cpu_address, &GDBServer::OnBreakpointHit);
    s_locals.breakpoints.push_back({key, 1, added_by_gdb});
  };

  switch (type)
  {
    case GDBBreakpointType::Software:
    case GDBBreakpointType::Hardware:
      acquire_breakpoint(CPU::BreakpointType::Execute, address);
      break;

    case GDBBreakpointType::Write:
      for (u32 offset = 0; offset < kind; offset++)
        acquire_breakpoint(CPU::BreakpointType::Write, address + offset);
      break;

    case GDBBreakpointType::Read:
      for (u32 offset = 0; offset < kind; offset++)
        acquire_breakpoint(CPU::BreakpointType::Read, address + offset);
      break;

    case GDBBreakpointType::Access:
      for (u32 offset = 0; offset < kind; offset++)
      {
        acquire_breakpoint(CPU::BreakpointType::Read, address + offset);
        acquire_breakpoint(CPU::BreakpointType::Write, address + offset);
      }
      break;
  }

  m_breakpoints.push_back(client_breakpoint);
}

void GDBServer::ClientSocket::RemoveBreakpoint(GDBBreakpointType type, VirtualMemoryAddress address, u32 kind)
{
  const ClientBreakpoint client_breakpoint{type, address, kind};
  const auto client_it = std::ranges::find(m_breakpoints, client_breakpoint);
  if (client_it == m_breakpoints.end())
    return;

  const auto release_breakpoint = [](CPU::BreakpointType cpu_type, VirtualMemoryAddress cpu_address) {
    const BreakpointKey key{cpu_type, cpu_address};
    const auto it = std::ranges::find_if(s_locals.breakpoints,
                                         [&key](const ManagedBreakpoint& breakpoint) { return breakpoint.key == key; });
    DebugAssert(it != s_locals.breakpoints.end());
    if (--it->reference_count > 0)
      return;

    if (it->added_by_gdb)
      CPU::RemoveBreakpoint(cpu_type, cpu_address);
    s_locals.breakpoints.erase(it);
  };

  switch (type)
  {
    case GDBBreakpointType::Software:
    case GDBBreakpointType::Hardware:
      release_breakpoint(CPU::BreakpointType::Execute, address);
      break;

    case GDBBreakpointType::Write:
      for (u32 offset = 0; offset < kind; offset++)
        release_breakpoint(CPU::BreakpointType::Write, address + offset);
      break;

    case GDBBreakpointType::Read:
      for (u32 offset = 0; offset < kind; offset++)
        release_breakpoint(CPU::BreakpointType::Read, address + offset);
      break;

    case GDBBreakpointType::Access:
      for (u32 offset = 0; offset < kind; offset++)
      {
        release_breakpoint(CPU::BreakpointType::Read, address + offset);
        release_breakpoint(CPU::BreakpointType::Write, address + offset);
      }
      break;
  }

  m_breakpoints.erase(client_it);
}

void GDBServer::ClientSocket::ClearBreakpoints()
{
  while (!m_breakpoints.empty())
  {
    const ClientBreakpoint& breakpoint = m_breakpoints.back();
    RemoveBreakpoint(breakpoint.type, breakpoint.address, breakpoint.kind);
  }
}

void GDBServer::ClientSocket::OnConnected()
{
  INFO_LOG("Client {} connected.", GetRemoteAddress().ToString());

  if (Error error; !SetNagleBuffering(false, &error)) [[unlikely]]
    ERROR_LOG("Failed to disable nagle buffering for {}: {}", GetRemoteAddress().ToString(), error.GetDescription());

  const size_t previous_clients = s_locals.clients.size();
  if (previous_clients == 0)
    s_locals.resume_on_last_disconnect = System::IsRunning();

  m_seen_resume = System::IsPaused();
  System::PauseSystem(true);

  s_locals.clients.push_back(std::static_pointer_cast<ClientSocket>(shared_from_this()));

  if (previous_clients == 0)
    Host::OnGDBServerActiveClientsChanged(true);
}

void GDBServer::ClientSocket::OnDisconnected(const Error& error)
{
  INFO_LOG("Client {} disconnected: {}", GetRemoteAddress().ToString(), error.GetDescription());

  ClearBreakpoints();

  const auto iter = std::find_if(s_locals.clients.begin(), s_locals.clients.end(),
                                 [this](const std::shared_ptr<ClientSocket>& rhs) { return (rhs.get() == this); });
  if (iter == s_locals.clients.end())
  {
    ERROR_LOG("Unknown GDB client disconnected? This should never happen.");
    return;
  }

  s_locals.clients.erase(iter);
  if (s_locals.clients.empty())
  {
    const bool resume_system = std::exchange(s_locals.resume_on_last_disconnect, false);
    if (resume_system)
      System::PauseSystem(false);

    Host::OnGDBServerActiveClientsChanged(false);
  }
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
      if (current_packet_size > MAX_FRAMED_PACKET_SIZE)
      {
        ERROR_LOG("Closing GDB client after oversized packet.");
        Close();
        return;
      }

      const std::string_view current_packet(reinterpret_cast<const char*>(buffer.data() + buffer_offset),
                                            current_packet_size);

      if (GDBServer::IsPacketAck(current_packet))
      {
        if (current_packet[0] == '-')
          ResendLastReply();

        packet_complete = true;
        break;
      }
      else if (GDBServer::IsPacketInterrupt(current_packet))
      {
        DEV_LOG("{} > Interrupt request", GetRemoteAddress().ToString());
        s_locals.pending_stop_signal = GDB_SIGNAL_INTERRUPT;
        System::PauseSystem(true);
        s_locals.pending_stop_signal = GDB_SIGNAL_TRAP;
        packet_complete = true;
        break;
      }
      else if (GDBServer::IsPacketComplete(current_packet))
      {
        DEBUG_LOG("{} > {}", GetRemoteAddress().ToString(), current_packet);
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

  DEBUG_LOG("Send reply: {}", sv);
  if (size_t written = Write(sv.data(), sv.length(), false); written != sv.length())
  {
    ERROR_LOG("Only wrote {} of {} bytes.", written, sv.length());
    Close();
  }
}

void GDBServer::ClientSocket::OnBreakpointHit(CPU::BreakpointType type, VirtualMemoryAddress address)
{
  if (!m_seen_resume)
    return;

  const PhysicalMemoryAddress hit_address = CPU::VirtualAddressToPhysical(address);
  const auto it = std::ranges::find_if(m_breakpoints, [&type, &address, &hit_address](const ClientBreakpoint& bp) {
    if (bp.type == GDBBreakpointType::Software || bp.type == GDBBreakpointType::Hardware)
      return false;
    if (bp.type == GDBBreakpointType::Read && type != CPU::BreakpointType::Read)
      return false;
    if (bp.type == GDBBreakpointType::Write && type != CPU::BreakpointType::Write)
      return false;

    for (u32 offset = 0; offset < bp.kind; offset++)
    {
      if (CPU::VirtualAddressToPhysical(bp.address + offset) == hit_address)
        return true;
    }
    return false;
  });

  if (it != m_breakpoints.end())
  {
    // Prevent the OnSystemPaused() from sending a reply.
    m_seen_resume = false;

    const char* const reason =
      (it->type == GDBBreakpointType::Access) ? "awatch" : ((type == CPU::BreakpointType::Read) ? "rwatch" : "watch");
    m_last_stop_reply.format("T{:02x}{}:{:08x};", s_locals.pending_stop_signal, reason, address);
    SendReply(m_last_stop_reply);
    return;
  }
}

void GDBServer::ClientSocket::OnSystemPaused(u8 signal)
{
  if (!m_seen_resume)
    return;

  m_seen_resume = false;

  m_last_stop_reply.format("S{:02x}", signal);
  SendReply(m_last_stop_reply);
}

void GDBServer::ClientSocket::OnSystemResumed()
{
  m_seen_resume = true;
}

void GDBServer::ClientSocket::SendAck()
{
  SendPacket("+");
}

void GDBServer::ClientSocket::SendReply(std::string_view reply)
{
  m_last_reply.format("${}#{:02x}", reply, ComputeChecksum(reply));
  SendPacket(m_last_reply);
}

void GDBServer::ClientSocket::SendLastStopReplyWithAck()
{
  SendReplyWithAck(m_last_stop_reply);
}

void GDBServer::ClientSocket::SendReplyWithAck(std::string_view reply)
{
  SendAck();
  SendReply(reply);
}

void GDBServer::ClientSocket::ResendLastReply()
{
  if (m_last_reply.empty())
  {
    WARNING_LOG("Received negative acknowledgement without a previous reply.");
    return;
  }

  SendPacket(m_last_reply);
}

bool GDBServer::Initialize(u16 port)
{
  Error error;
  Assert(!s_locals.listen_socket);

  const std::optional<SocketAddress> address =
    SocketAddress::Parse(SocketAddress::Type::IPv4, "127.0.0.1", port, &error);
  if (!address.has_value())
  {
    ERROR_LOG("Failed to parse address: {}", error.GetDescription());
    return false;
  }

  if (!(s_locals.multiplexer = SocketMultiplexer::Create(&error)))
  {
    ERROR_LOG("Failed to create socket multiplexer: {}", error.GetDescription());
    return false;
  }

  s_locals.listen_socket = s_locals.multiplexer->CreateListenSocket<ClientSocket>(address.value(), &error);
  if (!s_locals.listen_socket)
  {
    ERROR_LOG("Failed to create listen socket: {}", error.GetDescription());
    s_locals.multiplexer.reset();
    return false;
  }

  INFO_LOG("GDB server is now listening on {}.", address->ToString());
  return true;
}

bool GDBServer::HasAnyClients()
{
  return !s_locals.clients.empty();
}

void GDBServer::Poll(u32 timeout_ms)
{
  if (s_locals.multiplexer)
    s_locals.multiplexer->PollEventsWithTimeout(timeout_ms);
}

void GDBServer::Shutdown()
{
  if (!s_locals.listen_socket)
    return;

  INFO_LOG("Disconnecting {} GDB clients...", s_locals.clients.size());
  while (!s_locals.clients.empty())
  {
    // maintain a reference so we don't delete while in scope
    std::shared_ptr<ClientSocket> client = s_locals.clients.back();
    client->Close();
  }

  INFO_LOG("Stopping GDB server.");
  s_locals.listen_socket->Close();
  s_locals.listen_socket.reset();
  s_locals.multiplexer.reset();
}

void GDBServer::OnSystemPaused()
{
  for (auto& it : s_locals.clients)
    it->OnSystemPaused(s_locals.pending_stop_signal);
}

void GDBServer::OnSystemResumed()
{
  for (auto& it : s_locals.clients)
    it->OnSystemResumed();
}

#endif // ENABLE_GDB_SERVER
