// SPDX-FileCopyrightText: 2023 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "host.h"
#include "types.h"

#include "fmt/format.h"

namespace Netplay {

enum class SessionState
{
  Inactive,
  Initializing,
  Connecting,
  Resetting,
  Running,
  ClosingSession,
};

enum class ControlMessage : u32
{
  // host->player
  ConnectResponse,
  Reset,
  ResumeSession,
  PlayerJoined,
  DropPlayer,
  CloseSession,

  // player->host
  ConnectRequest,
  ResetComplete,
  ResetRequest,

  // bi-directional
  SetNickname,
  ChatMessage,
};

enum class DropPlayerReason : u32
{
  ConnectTimeout,
  DisconnectedFromHost,
};

#pragma pack(push, 1)
struct ControlMessageHeader
{
  ControlMessage type;
  u32 size;
};

struct ConnectRequestMessage
{
  enum class Mode
  {
    Player,
    Spectator,
  };

  ControlMessageHeader header;

  Mode mode;
  s32 requested_player_id;
  char nickname[MAX_NICKNAME_LENGTH];
  char session_password[MAX_SESSION_PASSWORD_LENGTH];

  std::string_view GetNickname() const
  {
    const size_t len = strnlen(nickname, std::size(nickname));
    return std::string_view(nickname, len);
  }

  std::string_view GetSessionPassword() const
  {
    const size_t len = strnlen(session_password, std::size(session_password));
    return std::string_view(session_password, len);
  }

  static ControlMessage MessageType() { return ControlMessage::ConnectRequest; }
};

struct ConnectResponseMessage
{
  enum class Result : u32
  {
    Success = 0,
    ServerFull,
    PlayerIDInUse,
    SessionClosed,
  };

  ControlMessageHeader header;

  Result result;
  s32 player_id;

  static ControlMessage MessageType() { return ControlMessage::ConnectResponse; }
};

struct ResetMessage
{
  struct PlayerAddress
  {
    u32 host;
    u16 port;
    s16 controller_port; // -1 if not present
    char nickname[MAX_NICKNAME_LENGTH];

    std::string_view GetNickname() const
    {
      const size_t len = strnlen(nickname, std::size(nickname));
      return std::string_view(nickname, len);
    }
  };

  ControlMessageHeader header;
  u32 cookie;
  s32 num_players;
  PlayerAddress players[MAX_PLAYERS];
  u32 state_data_size;
  // state_data_size bytes of state data follows

  static ControlMessage MessageType() { return ControlMessage::Reset; }
};

struct ResetCompleteMessage
{
  ControlMessageHeader header;

  u32 cookie;

  static ControlMessage MessageType() { return ControlMessage::ResetComplete; }
};

struct ResumeSessionMessage
{
  ControlMessageHeader header;

  static ControlMessage MessageType() { return ControlMessage::ResumeSession; }
};

struct PlayerJoinedMessage
{
  ControlMessageHeader header;
  s32 player_id;

  static ControlMessage MessageType() { return ControlMessage::PlayerJoined; }
};

struct DropPlayerMessage
{
  ControlMessageHeader header;
  DropPlayerReason reason;
  s32 player_id;

  static ControlMessage MessageType() { return ControlMessage::DropPlayer; }
};

struct ResetRequestMessage
{
  enum class Reason : u32
  {
    ConnectionLost,
  };

  ControlMessageHeader header;
  Reason reason;
  s32 causing_player_id;

  std::string ReasonToString() const
  {
    switch (reason)
    {
      case Reason::ConnectionLost:
        return fmt::format(Host::TranslateString("Netplay", "Connection lost to player {}.").GetCharArray(),
                           causing_player_id);
      default:
        return "Unknown";
    }
  }

  static ControlMessage MessageType() { return ControlMessage::ResetRequest; }
};

struct CloseSessionMessage
{
  enum class Reason : u32
  {
    HostRequest,
    HostShutdown,
  };

  ControlMessageHeader header;
  Reason reason;

  std::string ReasonToString() const
  {
    switch (reason)
    {
      case Reason::HostRequest:
        return Host::TranslateStdString("Netplay", "Session closed due to host request.");

      case Reason::HostShutdown:
        return Host::TranslateStdString("Netplay", "Session closed due to host shutdown.");

      default:
        return "Unknown";
    }
  }

  static ControlMessage MessageType() { return ControlMessage::CloseSession; }
};

struct SetNicknameMessage
{
  ControlMessageHeader header;

  static ControlMessage MessageType() { return ControlMessage::SetNickname; }
};

struct ChatMessage
{
  ControlMessageHeader header;

  std::string_view GetMessage() const
  {
    return (header.size > sizeof(ChatMessage)) ?
             std::string_view(reinterpret_cast<const char*>(this) + sizeof(ChatMessage),
                              header.size - sizeof(ChatMessage)) :
             std::string_view();
  }

  static ControlMessage MessageType() { return ControlMessage::ChatMessage; }
};
#pragma pack(pop)

} // namespace Netplay