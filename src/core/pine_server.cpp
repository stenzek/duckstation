// SPDX-FileCopyrightText: 2002-2024 PCSX2 Dev Team, Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0
//
// NOTE: File has been rewritten completely compared to the original, only the enums remain.
//

#include "pine_server.h"
#include "cpu_core.h"
#include "host.h"
#include "settings.h"
#include "system.h"

#include "scmversion/scmversion.h"

#include "util/sockets.h"

#include "common/assert.h"
#include "common/binary_reader_writer.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/small_string.h"

#include "fmt/format.h"

Log_SetChannel(PINEServer);

namespace PINEServer {
static std::shared_ptr<ListenSocket> s_listen_socket;

#ifndef _WIN32
static std::string s_socket_path;
#endif

/**
 * Maximum memory used by an IPC message request.
 * Equivalent to 50,000 Write64 requests.
 */
static constexpr u32 MAX_IPC_SIZE = 650000;

/**
 * Maximum memory used by an IPC message reply.
 * Equivalent to 50,000 Read64 replies.
 */
static constexpr u32 MAX_IPC_RETURN_SIZE = 450000;

/**
 * IPC Command messages opcodes.
 * A list of possible operations possible by the IPC.
 * Each one of them is what we call an "opcode" and is the first
 * byte sent by the IPC to differentiate between commands.
 */
enum IPCCommand : u8
{
  MsgRead8 = 0,           /**< Read 8 bit value from memory. */
  MsgRead16 = 1,          /**< Read 16 bit value from memory. */
  MsgRead32 = 2,          /**< Read 32 bit value from memory. */
  MsgRead64 = 3,          /**< Read 64 bit value from memory. */
  MsgWrite8 = 4,          /**< Write 8 bit value from memory. */
  MsgWrite16 = 5,         /**< Write 16 bit value from memory. */
  MsgWrite32 = 6,         /**< Write 32 bit value from memory. */
  MsgWrite64 = 7,         /**< Write 64 bit value from memory. */
  MsgVersion = 8,         /**< Returns PCSX2 version. */
  MsgSaveState = 9,       /**< Saves a savestate. */
  MsgLoadState = 0xA,     /**< Loads a savestate. */
  MsgTitle = 0xB,         /**< Returns the game title. */
  MsgID = 0xC,            /**< Returns the game ID. */
  MsgUUID = 0xD,          /**< Returns the game UUID. */
  MsgGameVersion = 0xE,   /**< Returns the game verion. */
  MsgStatus = 0xF,        /**< Returns the emulator status. */
  MsgReadBytes = 0x20,    /**< Reads range of bytes from memory. */
  MsgWriteBytes = 0x21,   /**< Writes range of bytes to memory. */
  MsgUnimplemented = 0xFF /**< Unimplemented IPC message. */
};

/**
 * Emulator status enum.
 * A list of possible emulator statuses.
 */
enum class EmuStatus : u32
{
  Running = 0, /**< Game is running */
  Paused = 1,  /**< Game is paused */
  Shutdown = 2 /**< Game is shutdown */
};

/**
 * IPC result codes.
 * A list of possible result codes the IPC can send back.
 * Each one of them is what we call an "opcode" or "tag" and is the
 * first byte sent by the IPC to differentiate between results.
 */
using IPCStatus = u8;
static constexpr IPCStatus IPC_OK = 0;      /**< IPC command successfully completed. */
static constexpr IPCStatus IPC_FAIL = 0xFF; /**< IPC command failed to complete. */

namespace {
class PINESocket final : public BufferedStreamSocket
{
public:
  PINESocket(SocketMultiplexer& multiplexer, SocketDescriptor descriptor);
  ~PINESocket() override;

protected:
  void OnConnected() override;
  void OnDisconnected(const Error& error) override;
  void OnRead() override;
  void OnWrite() override;

private:
  void ProcessCommandsInBuffer();
  bool HandleCommand(IPCCommand command, BinarySpanReader rdbuf);

  bool BeginReply(BinarySpanWriter& wrbuf, size_t required_bytes);
  bool EndReply(const BinarySpanWriter& sw);

  bool SendErrorReply();
};
} // namespace
} // namespace PINEServer

bool PINEServer::IsRunning()
{
  return static_cast<bool>(s_listen_socket);
}

bool PINEServer::Initialize(u16 slot)
{
  Error error;
  std::optional<SocketAddress> address;
#ifdef _WIN32
  address = SocketAddress::Parse(SocketAddress::Type::IPv4, "127.0.0.1", slot, &error);
#else
#ifdef __APPLE__
  const char* runtime_dir = std::getenv("TMPDIR");
#else
  const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR");
#endif
  // fallback in case macOS or other OSes don't implement the XDG base spec
  runtime_dir = runtime_dir ? runtime_dir : "/tmp";

  std::string socket_path;
  if (slot != Settings::DEFAULT_PINE_SLOT)
    socket_path = fmt::format("{}/duckstation.sock.{}", runtime_dir, slot);
  else
    socket_path = fmt::format("{}/duckstation.sock", runtime_dir);

  // we unlink the socket so that when releasing this thread the socket gets
  // freed even if we didn't close correctly the loop
  FileSystem::DeleteFile(socket_path.c_str());

  address = SocketAddress::Parse(SocketAddress::Type::Unix, socket_path.c_str(), 0, &error);
#endif

  if (!address.has_value())
  {
    ERROR_LOG("PINE: Failed to resolve listen address: {}", error.GetDescription());
    return false;
  }

  SocketMultiplexer* multiplexer = System::GetSocketMultiplexer();
  if (!multiplexer)
    return false;

  s_listen_socket = multiplexer->CreateListenSocket<PINESocket>(address.value(), &error);
  if (!s_listen_socket)
  {
    ERROR_LOG("PINE: Failed to create listen socket: {}", error.GetDescription());
    System::ReleaseSocketMultiplexer();
    return false;
  }

#ifndef _WIN32
  s_socket_path = std::move(socket_path);
#endif

  return true;
}

void PINEServer::Shutdown()
{
  // also closes the listener
  if (s_listen_socket)
  {
    s_listen_socket->Close();
    s_listen_socket.reset();
    System::ReleaseSocketMultiplexer();
  }

  // unlink the socket so nobody tries to connect to something no longer existent
#ifndef _WIN32
  if (!s_socket_path.empty())
  {
    FileSystem::DeleteFile(s_socket_path.c_str());
    s_socket_path = {};
  }
#endif
}

PINEServer::PINESocket::PINESocket(SocketMultiplexer& multiplexer, SocketDescriptor descriptor)
  : BufferedStreamSocket(multiplexer, descriptor, MAX_IPC_SIZE, MAX_IPC_RETURN_SIZE)
{
}

PINEServer::PINESocket::~PINESocket() = default;

void PINEServer::PINESocket::OnConnected()
{
  INFO_LOG("New client at {} connected.", GetRemoteAddress().ToString());

  Error error;
  if (GetLocalAddress().IsIPAddress() && !SetNagleBuffering(false, &error))
    ERROR_LOG("Failed to disable nagle buffering: {}", error.GetDescription());
}

void PINEServer::PINESocket::OnDisconnected(const Error& error)
{
  INFO_LOG("Client {} disconnected: {}", GetRemoteAddress().ToString(), error.GetDescription());
}

void PINEServer::PINESocket::OnRead()
{
  ProcessCommandsInBuffer();
}

void PINEServer::PINESocket::OnWrite()
{
  ProcessCommandsInBuffer();
}

void PINEServer::PINESocket::ProcessCommandsInBuffer()
{
  std::span<const u8> rdbuf = AcquireReadBuffer();
  if (rdbuf.empty())
    return;

  size_t position = 0;
  size_t remaining = rdbuf.size();
  while (remaining >= sizeof(u32))
  {
    u32 packet_size;
    std::memcpy(&packet_size, &rdbuf[position], sizeof(u32));
    if (packet_size > MAX_IPC_SIZE || packet_size < 5)
    {
      ERROR_LOG("PINE: Received invalid packet size {}", packet_size);
      Close();
      return;
    }

    // whole thing received yet yet?
    if (packet_size > remaining)
      break;

    const IPCCommand command = static_cast<IPCCommand>(rdbuf[position + sizeof(u32)]);
    if (!HandleCommand(command, BinarySpanReader(rdbuf.subspan(position + sizeof(u32) + sizeof(u8),
                                                               packet_size - sizeof(u32) - sizeof(u8)))))
    {
      // Out of write buffer space, abort.
      break;
    }

    position += packet_size;
    remaining -= packet_size;
  }

  ReleaseReadBuffer(position);
  ReleaseWriteBuffer(0, true);
}

bool PINEServer::PINESocket::HandleCommand(IPCCommand command, BinarySpanReader rdbuf)
{
  // example IPC messages: MsgRead/Write
  // refer to the client doc for more info on the format
  //         IPC Message event (1 byte)
  //         |  Memory address (4 byte)
  //         |  |           argument (VLE)
  //         |  |           |
  // format: XX YY YY YY YY ZZ ZZ ZZ ZZ
  //        reply code: 00 = OK, FF = NOT OK
  //        |  return value (VLE)
  //        |  |
  // reply: XX ZZ ZZ ZZ ZZ

  BinarySpanWriter reply;
  switch (command)
  {
    case MsgRead8:
    {
      if (!rdbuf.CheckRemaining(sizeof(PhysicalMemoryAddress)) || !System::IsValid())
        return SendErrorReply();
      else if (!BeginReply(reply, sizeof(u8))) [[unlikely]]
        return false;

      const PhysicalMemoryAddress addr = rdbuf.ReadU32();
      u8 res = 0;
      reply << (CPU::SafeReadMemoryByte(addr, &res) ? IPC_OK : IPC_FAIL);
      reply << res;
      return EndReply(reply);
    }

    case MsgRead16:
    {
      if (!rdbuf.CheckRemaining(sizeof(PhysicalMemoryAddress)) || !System::IsValid())
        return SendErrorReply();
      else if (!BeginReply(reply, sizeof(u16))) [[unlikely]]
        return false;

      const PhysicalMemoryAddress addr = rdbuf.ReadU32();
      u16 res = 0;
      reply << (CPU::SafeReadMemoryHalfWord(addr, &res) ? IPC_OK : IPC_FAIL);
      reply << res;
      return EndReply(reply);
    }

    case MsgRead32:
    {
      if (!rdbuf.CheckRemaining(sizeof(PhysicalMemoryAddress)) || !System::IsValid())
        return SendErrorReply();
      else if (!BeginReply(reply, sizeof(u32))) [[unlikely]]
        return false;

      const PhysicalMemoryAddress addr = rdbuf.ReadU32();
      u32 res = 0;
      reply << (CPU::SafeReadMemoryWord(addr, &res) ? IPC_OK : IPC_FAIL);
      reply << res;
      return EndReply(reply);
    }

    case MsgRead64:
    {
      if (!rdbuf.CheckRemaining(sizeof(PhysicalMemoryAddress)) || !System::IsValid())
        return SendErrorReply();
      else if (!BeginReply(reply, sizeof(u64))) [[unlikely]]
        return false;

      const PhysicalMemoryAddress addr = rdbuf.ReadU32();
      u32 res_low = 0, res_high = 0;
      reply << ((!CPU::SafeReadMemoryWord(addr, &res_low) || !CPU::SafeReadMemoryWord(addr + sizeof(u32), &res_high)) ?
                  IPC_FAIL :
                  IPC_OK);
      reply << ((ZeroExtend64(res_high) << 32) | ZeroExtend64(res_low));
      return EndReply(reply);
    }

    case MsgReadBytes:
    {
      if (!rdbuf.CheckRemaining(sizeof(PhysicalMemoryAddress) + sizeof(u32)) || !System::IsValid())
        return SendErrorReply();

      const PhysicalMemoryAddress addr = rdbuf.ReadU32();
      const u32 num_bytes = rdbuf.ReadU32();
      if (num_bytes == 0) [[unlikely]]
        return SendErrorReply();

      if (!BeginReply(reply, num_bytes)) [[unlikely]]
        return false;

      const auto data = reply.GetRemainingSpan(sizeof(IPCStatus) + num_bytes);
      if (!CPU::SafeReadMemoryBytes(addr, data.data() + sizeof(IPCStatus), num_bytes)) [[unlikely]]
      {
        reply << IPC_FAIL;
      }
      else
      {
        reply << IPC_OK;
        reply.IncrementPosition(num_bytes);
      }

      return EndReply(reply);
    }

    case MsgWrite8:
    {
      // Don't do the actual write until we have space for the response, otherwise we might do it twice when we come
      // back around.
      if (!rdbuf.CheckRemaining(sizeof(PhysicalMemoryAddress) + sizeof(u8)) || !System::IsValid())
        return SendErrorReply();
      else if (!BeginReply(reply, 0)) [[unlikely]]
        return false;

      const PhysicalMemoryAddress addr = rdbuf.ReadU32();
      const u8 value = rdbuf.ReadU8();
      reply << (CPU::SafeWriteMemoryByte(addr, value) ? IPC_OK : IPC_FAIL);
      return EndReply(reply);
    }

    case MsgWrite16:
    {
      if (!rdbuf.CheckRemaining(sizeof(PhysicalMemoryAddress) + sizeof(u16)) || !System::IsValid())
        return SendErrorReply();
      else if (!BeginReply(reply, 0)) [[unlikely]]
        return false;

      const PhysicalMemoryAddress addr = rdbuf.ReadU32();
      const u16 value = rdbuf.ReadU16();
      reply << (CPU::SafeWriteMemoryHalfWord(addr, value) ? IPC_OK : IPC_FAIL);
      return EndReply(reply);
    }

    case MsgWrite32:
    {
      if (!rdbuf.CheckRemaining(sizeof(PhysicalMemoryAddress) + sizeof(u32)) || !System::IsValid())
        return SendErrorReply();
      else if (!BeginReply(reply, 0)) [[unlikely]]
        return false;

      const PhysicalMemoryAddress addr = rdbuf.ReadU32();
      const u32 value = rdbuf.ReadU32();
      reply << (CPU::SafeWriteMemoryWord(addr, value) ? IPC_OK : IPC_FAIL);
      return EndReply(reply);
    }

    case MsgWrite64:
    {
      if (!rdbuf.CheckRemaining(sizeof(PhysicalMemoryAddress) + sizeof(u64)) || !System::IsValid())
        return SendErrorReply();
      else if (!BeginReply(reply, 0)) [[unlikely]]
        return false;

      const PhysicalMemoryAddress addr = rdbuf.ReadU32();
      const u64 value = rdbuf.ReadU64();
      reply << ((!CPU::SafeWriteMemoryWord(addr, Truncate32(value)) ||
                 !CPU::SafeWriteMemoryWord(addr + sizeof(u32), Truncate32(value >> 32))) ?
                  IPC_FAIL :
                  IPC_OK);
      return EndReply(reply);
    }

    case MsgWriteBytes:
    {
      if (!rdbuf.CheckRemaining(sizeof(PhysicalMemoryAddress) + sizeof(u32)) || !System::IsValid())
        return SendErrorReply();

      const PhysicalMemoryAddress addr = rdbuf.ReadU32();
      const u32 num_bytes = rdbuf.ReadU32();
      if (num_bytes == 0 || !rdbuf.CheckRemaining(num_bytes)) [[unlikely]]
        return SendErrorReply();

      if (!BeginReply(reply, 0)) [[unlikely]]
        return false;

      const auto data = rdbuf.GetRemainingSpan(num_bytes);
      reply << (CPU::SafeWriteMemoryBytes(addr, data.data(), num_bytes) ? IPC_OK : IPC_FAIL);
      return EndReply(reply);
    }

    case MsgVersion:
    {
      const TinyString version = TinyString::from_format("DuckStation {}", g_scm_tag_str);
      if (!BeginReply(reply, version.length() + 1)) [[unlikely]]
        return false;

      reply << IPC_OK << version;
      return EndReply(reply);
    }

    case MsgSaveState:
    {
      if (!rdbuf.CheckRemaining(sizeof(u8)) || !System::IsValid())
        return SendErrorReply();

      const std::string& serial = System::GetGameSerial();
      if (!serial.empty())
        return SendErrorReply();

      if (!BeginReply(reply, 0)) [[unlikely]]
        return false;

      std::string state_filename = System::GetGameSaveStateFileName(serial, rdbuf.ReadU8());
      Host::RunOnCPUThread([state_filename = std::move(state_filename)] {
        Error error;
        if (!System::SaveState(state_filename.c_str(), &error, false))
          ERROR_LOG("PINE: Save state failed: {}", error.GetDescription());
      });

      reply << IPC_OK;
      return EndReply(reply);
    }

    case MsgLoadState:
    {
      if (!rdbuf.CheckRemaining(sizeof(u8)) || !System::IsValid())
        return SendErrorReply();

      const std::string& serial = System::GetGameSerial();
      if (!serial.empty())
        return SendErrorReply();

      std::string state_filename = System::GetGameSaveStateFileName(serial, rdbuf.ReadU8());
      if (!FileSystem::FileExists(state_filename.c_str()))
        return SendErrorReply();

      if (!BeginReply(reply, 0)) [[unlikely]]
        return false;

      Host::RunOnCPUThread([state_filename = std::move(state_filename)] {
        Error error;
        if (!System::LoadState(state_filename.c_str(), &error, true))
          ERROR_LOG("PINE: Load state failed: {}", error.GetDescription());
      });

      reply << IPC_OK;
      return EndReply(reply);
    }

    case MsgTitle:
    {
      if (!System::IsValid())
        return SendErrorReply();

      const std::string& name = System::GetGameTitle();
      if (!BeginReply(reply, name.length() + 1)) [[unlikely]]
        return false;

      reply << IPC_OK << name;
      return EndReply(reply);
    }

    case MsgID:
    {
      if (!System::IsValid())
        return SendErrorReply();

      const std::string& serial = System::GetGameSerial();
      if (!BeginReply(reply, serial.length() + 1)) [[unlikely]]
        return false;

      reply << IPC_OK << serial;
      return EndReply(reply);
    }

    case MsgUUID:
    {
      if (!System::IsValid())
        return SendErrorReply();

      const TinyString crc = TinyString::from_format("{:016x}", System::GetGameHash());
      if (!BeginReply(reply, crc.length() + 1)) [[unlikely]]
        return false;

      reply << IPC_OK << crc;
      return EndReply(reply);
    }

    case MsgGameVersion:
    {
      ERROR_LOG("PINE: MsgGameVersion not supported.");
      return SendErrorReply();
    }

    case MsgStatus:
    {
      EmuStatus status;
      switch (System::GetState())
      {
        case System::State::Running:
          status = EmuStatus::Running;
          break;
        case System::State::Paused:
          status = EmuStatus::Paused;
          break;
        default:
          status = EmuStatus::Shutdown;
          break;
      }

      if (!BeginReply(reply, sizeof(u32))) [[unlikely]]
        return false;

      reply << IPC_OK << static_cast<u32>(status);
      return EndReply(reply);
    }

    default:
    {
      ERROR_LOG("PINE: Unhandled IPC command {:02X}", static_cast<u8>(command));
      return SendErrorReply();
    }
  }
}

bool PINEServer::PINESocket::BeginReply(BinarySpanWriter& wrbuf, size_t required_bytes)
{
  wrbuf = (AcquireWriteBuffer(sizeof(u32) + sizeof(IPCStatus) + required_bytes, false));
  if (!wrbuf.IsValid()) [[unlikely]]
    return false;

  wrbuf << static_cast<u32>(0); // size placeholder
  return true;
}

bool PINEServer::PINESocket::EndReply(const BinarySpanWriter& sw)
{
  DebugAssert(sw.IsValid());
  const size_t total_size = sw.GetBufferWritten();
  std::memcpy(&sw.GetSpan()[0], &total_size, sizeof(u32));
  ReleaseWriteBuffer(sw.GetBufferWritten(), false);
  return true;
}

bool PINEServer::PINESocket::SendErrorReply()
{
  BinarySpanWriter reply;
  if (!BeginReply(reply, 0)) [[unlikely]]
    return false;

  reply << IPC_FAIL;
  return EndReply(reply);
}
