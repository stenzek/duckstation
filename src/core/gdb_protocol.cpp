#include "gdb_protocol.h"
#include "bus.h"
#include "cpu_core_private.h"
#include "common/log.h"
#include "common/string_util.h"
#include "cpu_core.h"
#include "frontend-common/common_host_interface.h"
#include "system.h"
#include <functional>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <string>
Log_SetChannel(GDBProtocol);

namespace GDBProtocol
{

static u8* GetMemoryPointer(PhysicalMemoryAddress address, u32 length)
{
  auto region = Bus::GetMemoryRegionForAddress(address);
  if (region) {
    u8* data = GetMemoryRegionPointer(*region);
    if (data && (address + length <= GetMemoryRegionEnd(*region))) {
      return data + (address - GetMemoryRegionStart(*region));
    }
  }

  return nullptr;
}

static u8 ComputeChecksum(const std::string_view& str)
{
  u8 checksum = 0;
  for (char c : str) {
    checksum = (checksum + c) % 256;
  }
  return checksum;
}

static std::optional<std::string_view> DeserializePacket(const std::string_view& in)
{
  if ((in.size() < 4) || (in[0] != '$') || (in[in.size()-3] != '#')) {
    return std::nullopt;
  }
  std::string_view data = in.substr(1, in.size()-4);

  u8 packetChecksum = StringUtil::FromChars<u8>(in.substr(in.size()-2, 2), 16).value_or(0);
  u8 computedChecksum = ComputeChecksum(data);

  if (packetChecksum == computedChecksum) {
    return { data };
  }
  else {
    return std::nullopt;
  }
}

static std::string SerializePacket(const std::string_view& in)
{
  std::stringstream ss;
  ss << '$' << in << '#' << StringUtil::StdStringFromFormat("%02x", ComputeChecksum(in));
  return ss.str();
}

/// List of GDB remote protocol registers for MIPS III (excluding FP).
static const std::array<u32*, 38> REGISTERS {
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
  &CPU::g_state.regs.pc,
};

/// Number of registers in GDB remote protocol for MIPS III.
constexpr int NUM_GDB_REGISTERS = 73;

/// Get stop reason.
static std::optional<std::string> Cmd$_questionMark(const std::string_view& data)
{
  return { "S02" };
}

/// Get general registers.
static std::optional<std::string> Cmd$g(const std::string_view& data)
{
  std::stringstream ss;

  for (u32* reg : REGISTERS) {
    // Data is in host order (little endian).
    ss << StringUtil::EncodeHex(reinterpret_cast<u8*>(reg), 4);
  }

  // Pad with dummy data (FP registers stuff).
  for (int i = 0; i < NUM_GDB_REGISTERS - static_cast<int>(REGISTERS.size()); i++) {
    ss << "00000000";
  }

  return { ss.str() };
}

/// Set general registers.
static std::optional<std::string> Cmd$G(const std::string_view& data)
{
  if (data.size() == NUM_GDB_REGISTERS*8) {
    int offset = 0;

    for (u32* reg : REGISTERS) {
      // Data is in host order (little endian).
      auto value = StringUtil::DecodeHex({data.data()+offset, 8});
      if (value) {
        *reg = *reinterpret_cast<u32*>(&(*value)[0]);
      }
      offset += 8;
    }
  }
  else {
    Log_ErrorPrintf("Wrong payload size for 'G' command, expected %d got %zu", NUM_GDB_REGISTERS*8, data.size());
  }

  return { "" };
}

/// Get memory.
static std::optional<std::string> Cmd$m(const std::string_view& data)
{
  std::stringstream ss{std::string{data}};
  std::string dataAddress, dataLength;

  std::getline(ss, dataAddress, ',');
  std::getline(ss, dataLength, '\0');

  auto address = StringUtil::FromChars<VirtualMemoryAddress>(dataAddress, 16);
  auto length = StringUtil::FromChars<u32>(dataLength, 16);

  if (address && length) {
    PhysicalMemoryAddress phys_addr = *address & CPU::PHYSICAL_MEMORY_ADDRESS_MASK;
    u32 phys_length = *length;

    u8* ptr_data = GetMemoryPointer(phys_addr, phys_length);
    if (ptr_data) {
      return { StringUtil::EncodeHex(ptr_data, phys_length) };
    }
  }
  return { "E00" };
}

/// Set memory.
static std::optional<std::string> Cmd$M(const std::string_view& data)
{
  std::stringstream ss{std::string{data}};
  std::string dataAddress, dataLength, dataPayload;

  std::getline(ss, dataAddress, ',');
  std::getline(ss, dataLength, ':');
  std::getline(ss, dataPayload, '\0');

  auto address = StringUtil::FromChars<VirtualMemoryAddress>(dataAddress, 16);
  auto length = StringUtil::FromChars<u32>(dataLength, 16);
  auto payload = StringUtil::DecodeHex(dataPayload);

  if (address && length && payload && (payload->size() == *length)) {
    u32 phys_addr = *address & CPU::PHYSICAL_MEMORY_ADDRESS_MASK;
    u32 phys_length = *length;

    u8* ptr_data = GetMemoryPointer(phys_addr, phys_length);
    if (ptr_data) {
      memcpy(ptr_data, payload->data(), phys_length);
      return { "OK" };
    }
  }

  return { "E00" };
}

/// Remove hardware breakpoint.
static std::optional<std::string> Cmd$z1(const std::string_view& data)
{
  auto address = StringUtil::FromChars<VirtualMemoryAddress>(data, 16);
  if (address) {
    CPU::RemoveBreakpoint(*address);
    return { "OK" };
  }
  else {
    return std::nullopt;
  }
}

/// Insert hardware breakpoint.
static std::optional<std::string> Cmd$Z1(const std::string_view& data)
{
  auto address = StringUtil::FromChars<VirtualMemoryAddress>(data, 16);
  if (address) {
    CPU::AddBreakpoint(*address, false);
    return { "OK" };
  }
  else {
    return std::nullopt;
  }
}

static std::optional<std::string> Cmd$vMustReplyEmpty(const std::string_view& data)
{
  return { "" };
}

static std::optional<std::string> Cmd$qSupported(const std::string_view& data)
{
  return { "" };
}

/// List of all GDB remote protocol packets supported by us.
static const std::map<const char*, std::function<std::optional<std::string>(const std::string_view&)>> COMMANDS
{
  { "?", Cmd$_questionMark },
  { "g", Cmd$g },
  { "G", Cmd$G },
  { "m", Cmd$m },
  { "M", Cmd$M },
  { "z0,", Cmd$z1 },
  { "Z0,", Cmd$Z1 },
  { "z1,", Cmd$z1 },
  { "Z1,", Cmd$Z1 },
  { "vMustReplyEmpty", Cmd$vMustReplyEmpty },
  { "qSupported", Cmd$qSupported },
};

bool IsPacketInterrupt(const std::string_view& data)
{
  return (data.size() >= 1) && (data[data.size()-1] == '\003');
}

bool IsPacketContinue(const std::string_view& data)
{
  return (data.size() >= 5) && (data.substr(data.size()-5) == "$c#63");
}

bool IsPacketComplete(const std::string_view& data)
{
  return ((data.size() == 1) && (data[0] == '\003')) ||
    ((data.size() > 3) && (*(data.end()-3) == '#'));
}

std::string ProcessPacket(const std::string_view& data)
{
  std::string_view trimmedData = data;

  // Eat ACKs.
  while (!trimmedData.empty() && (trimmedData[0] == '+' || trimmedData[0] == '-')) {
    if (trimmedData[0] == '-') {
      Log_ErrorPrint("Received negative ack");
    }
    trimmedData = trimmedData.substr(1);
  }

  // Validate packet.
  auto packet = DeserializePacket(trimmedData);
  if (!packet) {
    Log_ErrorPrintf("Malformed packet '%*s'", static_cast<int>(trimmedData.size()), trimmedData.data());
    return "-";
  }

  std::optional<std::string> reply = { "" };

  // Try to invoke packet command.
  bool processed = false;
  for (const auto& command : COMMANDS) {
    if (StringUtil::StartsWith(packet->data(), command.first)) {
      Log_DebugPrintf("Processing command '%s'", command.first);

      // Invoke command, remove command name from payload.
      reply = command.second(packet->substr(strlen(command.first)));
      processed = true;
      break;
    }
  }

  if (!processed) {
    Log_WarningPrintf("Failed to process packet '%*s'", static_cast<int>(trimmedData.size()), trimmedData.data());
  }
  return reply ? "+"+SerializePacket(*reply) : "+";
}

} // namespace GDBProtocol
