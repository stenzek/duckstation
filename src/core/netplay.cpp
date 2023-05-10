#include "netplay.h"
#include "common/byte_stream.h"
#include "common/file_system.h"
#include "common/gpu_texture.h"
#include "common/log.h"
#include "common/memory_settings_interface.h"
#include "common/path.h"
#include "common/string_util.h"
#include "common/timer.h"
#include "digital_controller.h"
#include "host.h"
#include "host_settings.h"
#include "pad.h"
#include "save_state_version.h"
#include "spu.h"
#include "system.h"
#include <bitset>
#include <deque>
#include <gsl/span>
#include <xxhash.h>
Log_SetChannel(Netplay);

#ifdef _WIN32
#include "common/windows_headers.h"
#pragma comment(lib, "ws2_32.lib")
#endif

// TODO: windows.h getting picked up somewhere here...
#include "enet/enet.h"
#include "ggponet.h"

namespace Netplay {

// TODO: Put this in a header..
enum class SessionState
{
  Inactive,
  Initializing,
  Connecting,
  Synchronizing,
  Running,
};

enum class ControlMessage : u32
{
  ConnectResponse,
  SynchronizeSession,
  SynchronizeComplete,
};

enum class SessionMessage : u32
{
  ChatMessage,
};

#pragma pack(push, 1)
struct ControlMessageHeader
{
  ControlMessage type;
  u32 size;
};

#pragma pack(push, 1)
struct SessionMessageHeader
{
  SessionMessage type;
  u32 size;
};

struct ControlConnectResponseMessage
{
  enum class Result : u32
  {
    Success = 0,
    ServerFull,
    PlayerIDInUse,
  };

  Result result;
  s32 player_id;

  static ControlMessage MessageType() { return ControlMessage::ConnectResponse; }
};

struct ControlSynchronizeSessionMessage
{
  struct PlayerAddress
  {
    u32 host;
    u16 port;
    s16 controller_port; // -1 if not present
  };

  ControlMessageHeader header;
  s32 num_players;
  PlayerAddress players[MAX_PLAYERS];
  u32 state_data_size;
  // state_data_size bytes of state data follows

  static ControlMessage MessageType() { return ControlMessage::SynchronizeSession; }
};

struct ControlSynchronizeCompleteMessage
{
  ControlMessageHeader header;

  static ControlMessage MessageType() { return ControlMessage::SynchronizeComplete; }
};

struct SessionChatMessage
{
  SessionMessageHeader header;

  u32 chat_message_size;

  static SessionMessage MessageType() { return SessionMessage::ChatMessage; }
};
#pragma pack(pop)

using SaveStateBuffer = std::unique_ptr<System::MemorySaveState>;

struct Input
{
  u32 button_data;
};

static bool NpAdvFrameCb(void* ctx, int flags);
static bool NpSaveFrameCb(void* ctx, unsigned char** buffer, int* len, int* checksum, int frame);
static bool NpLoadFrameCb(void* ctx, unsigned char* buffer, int len, int rb_frames, int frame_to_load);
static void NpFreeBuffCb(void* ctx, void* buffer, int frame);
static bool NpOnEventCb(void* ctx, GGPOEvent* ev);

static GGPOPlayerHandle PlayerIdToGGPOHandle(s32 player_id);
static Input ReadLocalInput();
static GGPOErrorCode AddLocalInput(Netplay::Input input);
static GGPOErrorCode SyncInput(Input inputs[2], int* disconnect_flags);
static void SetInputs(Input inputs[2]);

static void SetSettings();

static bool CreateSystem(std::string game_path);

// TODO: Fatal error and shutdown helper

// ENet
static bool InitializeEnet();
static void ShutdownEnetHost();
static std::string PeerAddressString(const ENetPeer* peer);
static void HandleEnetEvent(const ENetEvent* event);
static void PollEnet(Common::Timer::Value until_time);

// Player management
static bool IsHost();
static bool IsValidPlayerId(s32 player_id);
static s32 GetFreePlayerId();
static ENetPeer* GetPeerForPlayer(s32 player_id);
static s32 GetPlayerIdForPeer(const ENetPeer* peer);
static bool WaitForPeerConnections();

// Controlpackets
static void HandleControlMessage(s32 player_id, const ENetPacket* pkt);
static void HandleConnectResponseMessage(s32 player_id, const ENetPacket* pkt);
static void HandleSynchronizeSessionMessage(s32 player_id, const ENetPacket* pkt);
static void HandleSynchronizeCompleteMessage(s32 player_id, const ENetPacket* pkt);

// Sessionpackets
static void HandleSessionMessage(s32 player_id, const ENetPacket* pkt);
static void HandleSessionChatMessage(s32 player_id, const ENetPacket* pkt);

// l = local, r = remote
static bool CreateGGPOSession();
static void DestroyGGPOSession();
static bool Start(s32 lhandle, u16 lport, const std::string& raddr, u16 rport, s32 ldelay, u32 pred,
                  std::string game_path);

// Host functions.
static void HandlePeerConnectionAsHost(ENetPeer* peer, s32 claimed_player_id);
static void HandlePeerConnectionAsNonHost(ENetPeer* peer, s32 claimed_player_id);
static void HandlePeerDisconnectionAsHost(s32 player_id);
static void HandlePeerDisconnectionAsNonHost(s32 player_id);
static void Resynchronize();
static void CheckForCompleteResynchronize();

static void AdvanceFrame();
static void RunFrame();

static s32 CurrentFrame();

static void NetplayAdvanceFrame(Netplay::Input inputs[], int disconnect_flags);

/// Frame Pacing
static void InitializeFramePacing();
static void HandleTimeSyncEvent(float frame_delta, int update_interval);
static void Throttle();

// Desync Detection
static void GenerateChecksumForFrame(int* checksum, int frame, unsigned char* buffer, int buffer_size);

//////////////////////////////////////////////////////////////////////////
// Variables
//////////////////////////////////////////////////////////////////////////

static MemorySettingsInterface s_settings_overlay;
static SessionState s_state;

/// Enet
struct Peer
{
  ENetPeer* peer;
  GGPOPlayerHandle ggpo_handle;
};
static ENetHost* s_enet_host = nullptr;
static std::array<Peer, MAX_PLAYERS> s_peers;
static s32 s_host_player_id = 0;
static s32 s_player_id = 0;
static s32 s_num_players = 0;
static s32 s_synchronized_players = 0; // only valid on host

static GGPOPlayerHandle s_local_handle = GGPO_INVALID_HANDLE;
static s32 s_local_delay = 0;
static GGPONetworkStats s_last_net_stats{};
static GGPOSession* s_ggpo = nullptr;

static std::deque<SaveStateBuffer> s_save_buffer_pool;

static std::array<std::array<float, 32>, NUM_CONTROLLER_AND_CARD_PORTS> s_net_input;

/// Frame timing. We manage our own frame pacing here, because we need to constantly adjust.
static float s_target_speed = 1.0f;
static Common::Timer::Value s_frame_period = 0;
static Common::Timer::Value s_next_frame_time = 0;
static s32 s_next_timesync_recovery_frame = -1;

//////////////////////////////////////////////////////////////////////////
// Packet helpers
//////////////////////////////////////////////////////////////////////////

template<typename T>
struct PacketWrapper
{
  ENetPacket* pkt;

  ALWAYS_INLINE const T* operator->() const { return reinterpret_cast<const T*>(pkt->data); }
  ALWAYS_INLINE T* operator->() { return reinterpret_cast<T*>(pkt->data); }
};
template<typename T>
static PacketWrapper<T> NewWrappedPacket(u32 size = sizeof(T), u32 flags = 0)
{
  return PacketWrapper<T>{enet_packet_create(nullptr, size, flags)};
}
template<typename T>
static PacketWrapper<T> NewControlPacket(u32 size = sizeof(T), u32 flags = ENET_PACKET_FLAG_RELIABLE)
{
  PacketWrapper<T> ret = NewWrappedPacket<T>(size, flags);
  ControlMessageHeader* hdr = reinterpret_cast<ControlMessageHeader*>(ret.pkt->data);
  hdr->type = T::MessageType();
  hdr->size = size;
  return ret;
}
template<typename T>
static bool SendControlPacket(ENetPeer* peer, const PacketWrapper<T>& pkt)
{
  const int rc = enet_peer_send(peer, ENET_CHANNEL_CONTROL, pkt.pkt);
  if (rc != 0)
  {
    Log_ErrorPrintf("enet_peer_send() failed: %d", rc);
    enet_packet_destroy(pkt.pkt);
    return false;
  }

  return true;
}
template<typename T>
static bool SendControlPacket(s32 player_id, const PacketWrapper<T>& pkt)
{
  DebugAssert(player_id >= 0 && player_id < MAX_PLAYERS && s_peers[player_id].peer);
  return SendControlPacket<T>(s_peers[player_id].peer, pkt);
}
template<typename T>
static PacketWrapper<T> NewSessionPacket(u32 size = sizeof(T), u32 flags = ENET_PACKET_FLAG_RELIABLE)
{
  PacketWrapper<T> ret = NewWrappedPacket<T>(size, flags);
  SessionMessageHeader* hdr = reinterpret_cast<SessionMessageHeader*>(ret.pkt->data);
  hdr->type = T::MessageType();
  hdr->size = size;
  return ret;
}
template<typename T>
static bool SendSessionPacket(ENetPeer* peer, const PacketWrapper<T>& pkt)
{
  const int rc = enet_peer_send(peer, ENET_CHANNEL_SESSION, pkt.pkt);
  if (rc != 0)
  {
    Log_ErrorPrintf("enet_peer_send() failed: %d", rc);
    enet_packet_destroy(pkt.pkt);
    return false;
  }

  return true;
}
template<typename T>
static bool SendSessionPacket(s32 player_id, const PacketWrapper<T>& pkt)
{
  DebugAssert(player_id >= 0 && player_id < MAX_PLAYERS && s_peers[player_id].peer);
  return SendSessionPacket<T>(s_peers[player_id].peer, pkt);
}
} // namespace Netplay

// Netplay Impl

bool Netplay::Start(s32 lhandle, u16 lport, const std::string& raddr, u16 rport, s32 ldelay, u32 pred,
                    std::string game_path)
{
  s_state = SessionState::Initializing;

  SetSettings();

  if (!CreateSystem(std::move(game_path)))
  {
    Log_ErrorPrintf("Failed to create system.");
    return false;
  }

  if (!InitializeEnet())
  {
    Log_ErrorPrintf("Failed to initialize Enet.");
    return false;
  }

  // Create our "host" (which is basically just our port).
  ENetAddress server_address;
  server_address.host = ENET_HOST_ANY;
  server_address.port = lport;
  s_enet_host = enet_host_create(&server_address, MAX_PLAYERS - 1, NUM_ENET_CHANNELS, 0, 0);
  if (!s_enet_host)
  {
    Log_ErrorPrintf("Failed to create enet host.");
    return false;
  }

  s_host_player_id = 0;
  s_local_delay = ldelay;

  // If we're the host, we can just continue on our merry way, the others will join later.
  if (lhandle == 1)
  {
    // Starting session with a single player.
    s_player_id = 0;
    s_num_players = 1;
    s_synchronized_players = 1;

    if (!CreateGGPOSession())
    {
      Log_ErrorPrintf("Failed to create GGPO session for host.");
      return false;
    }

    Log_InfoPrintf("Netplay session started as host.");
    s_state = SessionState::Running;
    return true;
  }

  // for non-hosts, we don't know our player id yet until after we connect...
  s_player_id = -1;

  // Connect to host.
  ENetAddress host_address;
  host_address.port = rport;
  if (enet_address_set_host_ip(&host_address, raddr.c_str()) != 0)
  {
    Log_ErrorPrintf("Failed to parse host: '%s'", raddr.c_str());
    return false;
  }

  s_peers[s_host_player_id].peer =
    enet_host_connect(s_enet_host, &host_address, NUM_ENET_CHANNELS, static_cast<u32>(s_player_id));
  if (!s_peers[s_host_player_id].peer)
  {
    Log_ErrorPrintf("Failed to start connection to host.");
    return false;
  }

  // Wait until we're connected to the main host. They'll send us back state to load and a full player list.
  s_state = SessionState::Connecting;
  return true;
}

void Netplay::CloseSession()
{
  Assert(IsActive());

  DestroyGGPOSession();
  ShutdownEnetHost();

  // Restore original settings.
  Host::Internal::SetNetplaySettingsLayer(nullptr);
  System::ApplySettings(false);
}

bool Netplay::IsActive()
{
  return (s_state != SessionState::Inactive);
}

bool Netplay::CreateSystem(std::string game_path)
{
  // close system if its already running
  if (System::IsValid())
    System::ShutdownSystem(false);

  // fast boot the selected game and wait for the other player
  auto param = SystemBootParameters(std::move(game_path));
  param.override_fast_boot = true;
  if (!System::BootSystem(param))
    return false;

#if 1
  // Fast Forward to Game Start if needed.
  SPU::SetAudioOutputMuted(true);
  while (System::GetInternalFrameNumber() < 2)
    System::RunFrame();
  SPU::SetAudioOutputMuted(false);
#endif

  return true;
}

bool Netplay::InitializeEnet()
{
  static bool enet_initialized = false;
  int rc;
  if (!enet_initialized && (rc = enet_initialize()) != 0)
  {
    Log_ErrorPrintf("enet_initialize() returned %d", rc);
    return false;
  }

  std::atexit(enet_deinitialize);
  enet_initialized = true;
  return true;
}

void Netplay::ShutdownEnetHost()
{
  Log_DevPrint("Disconnecting all peers");

  // forcefully disconnect all peers
  // TODO: do we want to send disconnect requests and wait a bit?
  for (u32 i = 0; i < MAX_PLAYERS; i++)
  {
    if (s_peers[i].peer)
    {
      enet_peer_reset(s_peers[i].peer);
      s_peers[i].peer = nullptr;
    }
  }

  enet_host_destroy(s_enet_host);
  s_enet_host = nullptr;
}

std::string Netplay::PeerAddressString(const ENetPeer* peer)
{
  char buf[128];
  if (enet_address_get_host_ip(&peer->address, buf, std::size(buf)))
    buf[0] = 0;

  return fmt::format("{}:{}", buf, peer->address.port);
}

void Netplay::HandleEnetEvent(const ENetEvent* event)
{
  switch (event->type)
  {
    case ENET_EVENT_TYPE_CONNECT:
    {
      if (IsHost())
        HandlePeerConnectionAsHost(event->peer, static_cast<s32>(event->data));
      else
        HandlePeerConnectionAsNonHost(event->peer, static_cast<s32>(event->data));

      return;
    }
    break;

    case ENET_EVENT_TYPE_DISCONNECT:
    {
      const s32 player_id = GetPlayerIdForPeer(event->peer);
      if (s_state == SessionState::Connecting)
      {
        Assert(player_id == s_host_player_id);
        Panic("Failed to connect to host");
        return;
      }
      else if (s_state == SessionState::Synchronizing)
      {
        // let the timeout deal with it
        Log_DevPrintf("Ignoring disconnection from %d while synchronizing", player_id);
        return;
      }

      Log_WarningPrintf("ENet player %d disconnected", player_id);
      Host::OnNetplayMessage(fmt::format("*** DISCONNECTED PLAYER {} ***", player_id));

      Assert(IsValidPlayerId(player_id));
      if (IsHost())
        HandlePeerDisconnectionAsHost(player_id);
      else
        HandlePeerDisconnectionAsNonHost(player_id);
    }
    break;

    case ENET_EVENT_TYPE_RECEIVE:
    {
      const s32 player_id = GetPlayerIdForPeer(event->peer);
      if (player_id < 0)
      {
        Log_WarningPrintf("Received packet from unknown player");
        return;
      }

      if (event->channelID == ENET_CHANNEL_CONTROL)
      {
        HandleControlMessage(player_id, event->packet);
      }
      else if (event->channelID == ENET_CHANNEL_SESSION)
      {
        HandleSessionMessage(player_id, event->packet);
      }
      else if (event->channelID == ENET_CHANNEL_GGPO)
      {
        Log_TracePrintf("Received %zu ggpo bytes from player %d", event->packet->dataLength, player_id);
        const int rc = ggpo_handle_packet(s_ggpo, event->peer, event->packet);
        if (rc != GGPO_OK)
          Log_ErrorPrintf("Failed to process GGPO packet!");
      }
      else
      {
        Log_ErrorPrintf("Unexpected packet channel %u", event->channelID);
      }
    }
    break;

    default:
    {
      Log_WarningPrintf("Unhandled enet event %d", event->type);
    }
    break;
  }
}

void Netplay::PollEnet(Common::Timer::Value until_time)
{
  ENetEvent event;

  u64 current_time = Common::Timer::GetCurrentValue();

  for (;;)
  {
    const u32 enet_timeout = (current_time >= until_time) ?
                               0 :
                               static_cast<u32>(Common::Timer::ConvertValueToMilliseconds(until_time - current_time));

    // make sure s_enet_host exists
    Assert(s_enet_host);

    const int res = enet_host_service(s_enet_host, &event, enet_timeout);
    if (res > 0)
    {
      HandleEnetEvent(&event);

      // make sure we get all events
      current_time = Common::Timer::GetCurrentValue();
      continue;
    }
    // exit once we're nonblocking
    current_time = Common::Timer::GetCurrentValue();
    if (enet_timeout == 0 || current_time >= until_time)
      break;
  }
}

bool Netplay::IsHost()
{
  return s_player_id == s_host_player_id;
}

GGPOPlayerHandle Netplay::PlayerIdToGGPOHandle(s32 player_id)
{
  DebugAssert(player_id >= 0 && player_id < MAX_PLAYERS);
  return s_peers[player_id].ggpo_handle;
}

ENetPeer* Netplay::GetPeerForPlayer(s32 player_id)
{
  DebugAssert(player_id >= 0 && player_id < MAX_PLAYERS);
  return s_peers[player_id].peer;
}

s32 Netplay::GetPlayerIdForPeer(const ENetPeer* peer)
{
  for (s32 i = 0; i < MAX_PLAYERS; i++)
  {
    if (s_peers[i].peer == peer)
      return i;
  }

  return -1;
}

bool Netplay::IsValidPlayerId(s32 player_id)
{
  return s_player_id == player_id || (player_id >= 0 && player_id < MAX_PLAYERS && s_peers[player_id].peer);
}

s32 Netplay::GetFreePlayerId()
{
  for (s32 i = 0; i < MAX_PLAYERS; i++)
  {
    if (i != s_player_id && !s_peers[i].peer)
      return i;
  }

  return -1;
}

bool Netplay::WaitForPeerConnections()
{
  static constexpr float MAX_CONNECT_TIME = 30.0f;
  Common::Timer timeout;

  const s32 clients_to_connect = s_num_players - 1;
  Log_VerbosePrintf("Waiting for connection to %d peers", clients_to_connect);

  for (;;)
  {
    // TODO: Handle early shutdown/cancel request.
    s32 num_connected_peers = 0;
    for (s32 i = 0; i < MAX_PLAYERS; i++)
    {
      if (i != s_player_id && s_peers[i].peer && s_peers[i].peer->state == ENET_PEER_STATE_CONNECTED)
        num_connected_peers++;
    }
    if (num_connected_peers == clients_to_connect)
      break;

    if (timeout.GetTimeSeconds() >= MAX_CONNECT_TIME)
    {
      Log_ErrorPrintf("Peer connection timeout");
      return false;
    }

    Host::PumpMessagesOnCPUThread();
    Host::DisplayLoadingScreen("Connected to netplay peers", 0, clients_to_connect, num_connected_peers);

    const Common::Timer::Value poll_end_time =
      Common::Timer::GetCurrentValue() + Common::Timer::ConvertMillisecondsToValue(16);
    PollEnet(poll_end_time);
  }

  Log_InfoPrint("Peer connection complete.");
  return true;
}

bool Netplay::CreateGGPOSession()
{
  /*
  TODO: since saving every frame during rollback loses us time to do actual gamestate iterations it might be better to
  hijack the update / save / load cycle to only save every confirmed frame only saving when actually needed.
  */
  GGPOSessionCallbacks cb = {};
  cb.advance_frame = NpAdvFrameCb;
  cb.save_game_state = NpSaveFrameCb;
  cb.load_game_state = NpLoadFrameCb;
  cb.free_buffer = NpFreeBuffCb;
  cb.on_event = NpOnEventCb;

  ggpo_start_session(&s_ggpo, &cb, s_num_players, sizeof(Netplay::Input), MAX_ROLLBACK_FRAMES);

  int player_num = 1;
  for (s32 i = 0; i < MAX_PLAYERS; i++)
  {
    // slot filled?
    if (!s_peers[i].peer && i != s_player_id)
      continue;

    GGPOPlayer player = {sizeof(GGPOPlayer)};
    GGPOErrorCode result;
    player.player_num = player_num++;
    if (i == s_player_id)
    {
      player.type = GGPO_PLAYERTYPE_LOCAL;
      result = ggpo_add_player(s_ggpo, &player, &s_peers[i].ggpo_handle);
      if (GGPO_SUCCEEDED(result))
        s_local_handle = s_peers[i].ggpo_handle;
    }
    else
    {
      player.type = GGPO_PLAYERTYPE_REMOTE;
      player.u.remote.peer = s_peers[i].peer;
      result = ggpo_add_player(s_ggpo, &player, &s_peers[i].ggpo_handle);
    }

    if (!GGPO_SUCCEEDED(result))
    {
      Log_ErrorPrintf("Failed to add player %d", i);
      return false;
    }
  }

  ggpo_set_frame_delay(s_ggpo, s_local_handle, s_local_delay);
  InitializeFramePacing();
  return true;
}

void Netplay::DestroyGGPOSession()
{
  if (!s_ggpo)
    return;

  Log_DevPrintf("Destroying GGPO session...");
  ggpo_close_session(s_ggpo);
  s_ggpo = nullptr;
  s_save_buffer_pool.clear();
  s_local_handle = GGPO_INVALID_HANDLE;

  for (Peer& p : s_peers)
    p.ggpo_handle = GGPO_INVALID_HANDLE;
}

void Netplay::HandleControlMessage(s32 player_id, const ENetPacket* pkt)
{
  if (pkt->dataLength < sizeof(ControlMessageHeader))
  {
    Log_ErrorPrintf("Invalid control packet from player %d of size %zu", player_id, pkt->dataLength);
    return;
  }

  const ControlMessageHeader* hdr = reinterpret_cast<const ControlMessageHeader*>(pkt->data);
  switch (hdr->type)
  {
    case ControlMessage::ConnectResponse:
      HandleConnectResponseMessage(player_id, pkt);
      break;

    case ControlMessage::SynchronizeSession:
      HandleSynchronizeSessionMessage(player_id, pkt);
      break;

    case ControlMessage::SynchronizeComplete:
      HandleSynchronizeCompleteMessage(player_id, pkt);
      break;

    default:
      Log_ErrorPrintf("Unhandled control packet %u from player %d of size %zu", hdr->type, player_id, pkt->dataLength);
      break;
  }
}

void Netplay::HandlePeerConnectionAsHost(ENetPeer* peer, s32 claimed_player_id)
{
  Log_VerbosePrint(
    fmt::format("New host peer connection from {} claiming player ID {}", PeerAddressString(peer), claimed_player_id)
      .c_str());

  PacketWrapper<ControlConnectResponseMessage> response = NewControlPacket<ControlConnectResponseMessage>();
  response->player_id = -1;

  // Player is attempting to reconnect.
  // Hopefully both sides have disconnected completely before they reconnect.
  // If not, too bad.
  if (claimed_player_id >= 0 && IsValidPlayerId(claimed_player_id))
  {
    Log_ErrorPrintf("Player ID %d is already in use, rejecting connection.", claimed_player_id);
    response->result = ControlConnectResponseMessage::Result::PlayerIDInUse;
    SendControlPacket(peer, response);
    return;
  }

  // Any free slots?
  const s32 new_player_id = (claimed_player_id >= 0) ? claimed_player_id : GetFreePlayerId();
  if (new_player_id < 0)
  {
    Log_ErrorPrintf("Server full, rejecting connection.");
    response->result = ControlConnectResponseMessage::Result::ServerFull;
    SendControlPacket(peer, response);
    return;
  }

  Log_VerbosePrint(
    fmt::format("New connection from {} assigned to player ID {}", PeerAddressString(peer), new_player_id).c_str());
  response->result = ControlConnectResponseMessage::Result::Success;
  response->player_id = new_player_id;
  SendControlPacket(peer, response);

  // Set up their player slot.
  Assert(s_num_players < MAX_PLAYERS);
  s_peers[new_player_id].peer = peer;
  s_num_players++;

  // Force everyone to resynchronize with the new player.
  Resynchronize();
}

void Netplay::HandlePeerConnectionAsNonHost(ENetPeer* peer, s32 claimed_player_id)
{
  Log_VerbosePrint(
    fmt::format("New peer connection from {} claiming player ID {}", PeerAddressString(peer), claimed_player_id)
      .c_str());

  // shouldn't ever get a non-host connection without a valid ID
  if (claimed_player_id < 0 || claimed_player_id >= MAX_PLAYERS)
  {
    Log_ErrorPrintf("Invalid claimed_player_id %d", claimed_player_id);
    enet_peer_disconnect_now(peer, 0);
    return;
  }

  // the peer should match up, unless we're the one receiving the connection
  Assert(s_peers[claimed_player_id].peer == peer || claimed_player_id > s_player_id);
  if (s_peers[claimed_player_id].peer == peer)
  {
    // WaitForPeerConnections handles this case.
    // If this was the host, we still need to wait for the synchronization.
    Log_DevPrintf("Connection complete with player %d%s", claimed_player_id,
                  (claimed_player_id == s_host_player_id) ? "[HOST]" : "");
    return;
  }

  Log_DevPrintf("Connection received from peer %d", claimed_player_id);
  s_peers[claimed_player_id].peer = peer;
}

void Netplay::HandleConnectResponseMessage(s32 player_id, const ENetPacket* pkt)
{
  // TODO: error handling
  Assert(player_id == s_host_player_id && pkt->dataLength >= sizeof(ControlConnectResponseMessage));

  const ControlConnectResponseMessage* msg = reinterpret_cast<const ControlConnectResponseMessage*>(pkt->data);
  if (msg->result != ControlConnectResponseMessage::Result::Success)
  {
    Log_ErrorPrintf("Connection failed %u", msg->result);
    Panic("Connection failed");
    return;
  }

  // Still need to wait for synchronization
  Log_InfoPrintf("Connected to host, we were assigned player ID %d", msg->player_id);
  s_player_id = msg->player_id;
}

void Netplay::HandlePeerDisconnectionAsHost(s32 player_id)
{
  Log_InfoPrintf("Player %d disconnected from host, reclaiming their slot", player_id);

  Assert(s_peers[player_id].peer);
  enet_peer_disconnect_now(s_peers[player_id].peer, 0);
  s_peers[player_id].peer = nullptr;
  s_num_players--;

  // TODO: We *could* avoid the resync here and just tell all players to drop old mate
  Resynchronize();
}

void Netplay::HandlePeerDisconnectionAsNonHost(s32 player_id)
{
  Panic("Disconnected non server peer, FIXME");
}

void Netplay::Resynchronize()
{
  Assert(IsHost());

  Log_VerbosePrintf("Resynchronizing...");

  // Use the current system state, whatever that is.
  // TODO: This save state has the bloody path to the disc in it. We need a new save state format.
  GrowableMemoryByteStream state(nullptr, System::MAX_SAVE_STATE_SIZE);
  if (!System::SaveStateToStream(&state, 0, SAVE_STATE_HEADER::COMPRESSION_TYPE_ZSTD))
    Panic("Failed to save state...");

  const u32 state_data_size = static_cast<u32>(state.GetPosition());
  ControlSynchronizeSessionMessage header = {};
  header.header.type = ControlMessage::SynchronizeSession;
  header.header.size = sizeof(ControlSynchronizeSessionMessage) + state_data_size;
  header.state_data_size = state_data_size;

  // Fill in player info.
  header.num_players = s_num_players;
  for (s32 i = 0; i < MAX_PLAYERS; i++)
  {
    if (!IsValidPlayerId(i))
    {
      header.players[i].controller_port = -1;
      continue;
    }

    // TODO: This is wrong, so wrong....
    header.players[i].controller_port = static_cast<s16>(i);

    if (i == s_player_id)
    {
      // TODO: not valid... listening on any address.
      header.players[i].host = s_enet_host->address.host;
      header.players[i].port = s_enet_host->address.port;
    }
    else
    {
      header.players[i].host = s_peers[i].peer->address.host;
      header.players[i].port = s_peers[i].peer->address.port;
    }
  }

  // Distribute sync request to all peers, then wait for them to reload back.
  // Any GGPO packets will get dropped, since the session's gone temporarily.
  DestroyGGPOSession();
  s_state = SessionState::Synchronizing;
  s_synchronized_players = 1;

  for (s32 i = 0; i < MAX_PLAYERS; i++)
  {
    if (!s_peers[i].peer)
      continue;

    ENetPacket* pkt = enet_packet_create(nullptr, sizeof(header) + state_data_size, ENET_PACKET_FLAG_RELIABLE);
    std::memcpy(pkt->data, &header, sizeof(header));
    std::memcpy(pkt->data + sizeof(header), state.GetMemoryPointer(), state_data_size);
    const int rc = enet_peer_send(s_peers[i].peer, ENET_CHANNEL_CONTROL, pkt);
    if (rc != 0)
    {
      // TODO: probably just drop them.. but the others already know their address :/
      Log_ErrorPrintf("Failed to send synchronization request to player %d", i);
      Panic("Failed to send sync packet");
      enet_packet_destroy(pkt);
    }
  }

  // Do a full state reload on the host as well, that way everything (including the GPU)
  // has a clean slate, reducing the chance of desyncs. Looking at you, mister rec, somehow
  // having a different number of cycles.
  // CPU::CodeCache::Flush();
  state.SeekAbsolute(0);
  if (!System::LoadStateFromStream(&state, true))
    Panic("Failed to reload host state");

  // Might be done already if there's only one player.
  CheckForCompleteResynchronize();
}

void Netplay::HandleSynchronizeSessionMessage(s32 player_id, const ENetPacket* pkt)
{
  static bool is_synchronizing = false;
  if (is_synchronizing)
  {
    // TODO: this might happen if someone drops during sync...
    Panic("Double sync");
  }
  is_synchronizing = true;

  // TODO: This should check that the message only comes from the host/player 1.
  Assert(s_host_player_id == player_id);

  const ControlSynchronizeSessionMessage* msg = reinterpret_cast<const ControlSynchronizeSessionMessage*>(pkt->data);
  if (pkt->dataLength < sizeof(ControlSynchronizeSessionMessage) ||
      pkt->dataLength < (sizeof(ControlSynchronizeSessionMessage) + msg->state_data_size))
  {
    // TODO: Shut down the session
    Panic("Invalid synchronize session packet");
    return;
  }

  DestroyGGPOSession();

  // Make sure we're connected to all players.
  Assert(msg->num_players > 1);
  Log_DevPrintf("Checking connections");
  s_num_players = msg->num_players;
  s_state = SessionState::Synchronizing;
  for (s32 i = 0; i < MAX_PLAYERS; i++)
  {
    Peer& p = s_peers[i];
    if (msg->players[i].controller_port < 0)
    {
      // If we had a client here, it must've dropped or something..
      if (p.peer)
      {
        Log_WarningPrintf("Dropping connection to player %d", i);
        enet_peer_reset(p.peer);
        p.peer = nullptr;
      }

      continue;
    }

    // Can't connect to ourselves! Or the host, which may not contain a correct address, since it's from itself.
    if (i == s_player_id || i == s_host_player_id)
      continue;

    // The host should fall into the category where we can reuse.
    if (p.peer && p.peer->address.host == msg->players[i].host && p.peer->address.port == msg->players[i].port)
    {
      Log_DevPrintf("Preserving connection to player %d", i);
      continue;
    }

    Assert(i != s_host_player_id);
    if (p.peer)
    {
      enet_peer_reset(p.peer);
      p.peer = nullptr;
    }

    // If this player has a higher ID than us, then they're responsible for connecting to us, not the other way around.
    // i.e. player 2 is responsible for connecting to players 0 and 1, player 1 is responsible for connecting to player
    // 0.
    if (i > s_player_id)
    {
      Log_DevPrintf("Not connecting to player %d, as they have a higher ID than us (%d)", i);
      continue;
    }

    const ENetAddress address = {msg->players[i].host, msg->players[i].port};
    p.peer = enet_host_connect(s_enet_host, &address, NUM_ENET_CHANNELS, static_cast<u32>(s_player_id));
    if (!p.peer)
      Panic("Failed to connect to peer on resynchronize");
  }

  if (!WaitForPeerConnections())
  {
    // TODO: proper error handling here
    Panic("Failed to reconnect to all peers");
  }

  if (!CreateGGPOSession())
    Panic("Failed to create GGPO session");

  // Load state from packet.
  ReadOnlyMemoryByteStream stream(pkt->data + sizeof(ControlSynchronizeSessionMessage), msg->state_data_size);
  if (!System::LoadStateFromStream(&stream, true))
    Panic("Failed to load state from host");

  // We're done here.
  if (!SendControlPacket(player_id, NewControlPacket<ControlSynchronizeCompleteMessage>()))
    Panic("Failed to send sync complete control packet");

  s_state = SessionState::Running;
  is_synchronizing = false;
}

void Netplay::HandleSynchronizeCompleteMessage(s32 player_id, const ENetPacket* pkt)
{
  if (s_state != SessionState::Synchronizing || player_id == s_host_player_id)
  {
    Log_ErrorPrintf("Received unexpected synchronization complete from player %d", player_id);
    return;
  }

  // TODO: we should check that one player isn't sending multiple sync done packets...
  Log_DevPrintf("Player %d completed synchronization", player_id);
  s_synchronized_players++;
  CheckForCompleteResynchronize();
}

void Netplay::HandleSessionMessage(s32 player_id, const ENetPacket* pkt)
{
  if (pkt->dataLength < sizeof(SessionMessageHeader))
  {
    Log_ErrorPrintf("Invalid session packet from player %d of size %zu", player_id, pkt->dataLength);
    return;
  }

  const SessionMessageHeader* hdr = reinterpret_cast<const SessionMessageHeader*>(pkt->data);
  switch (hdr->type)
  {
    case SessionMessage::ChatMessage:
      HandleSessionChatMessage(player_id, pkt);
      break;

    default:
      Log_ErrorPrintf("Unhandled session packet %u from player %d of size %zu", hdr->type, player_id, pkt->dataLength);
      break;
  }
}

void Netplay::HandleSessionChatMessage(s32 player_id, const ENetPacket* pkt)
{
  const SessionChatMessage* msg = reinterpret_cast<const SessionChatMessage*>(pkt->data);
  if (pkt->dataLength < sizeof(SessionChatMessage) ||
      pkt->dataLength < (sizeof(SessionChatMessage) + msg->chat_message_size))
  {
    // invalid chat message. ignore.
    return;
  }

  std::string message(pkt->data + sizeof(SessionChatMessage),
                      pkt->data + sizeof(SessionChatMessage) + msg->chat_message_size);

  Host::OnNetplayMessage(fmt::format("Player {}: {}", player_id + 1, message));
}

void Netplay::CheckForCompleteResynchronize()
{
  if (s_synchronized_players == s_num_players)
  {
    Log_VerbosePrintf("All players synchronized, resuming!");
    if (!CreateGGPOSession())
      Panic("Failed to create GGPO session after sync");

    s_state = SessionState::Running;
  }
}

//////////////////////////////////////////////////////////////////////////
// Settings Overlay
//////////////////////////////////////////////////////////////////////////

void Netplay::SetSettings()
{
  MemorySettingsInterface& si = s_settings_overlay;

  si.Clear();
  for (u32 i = 0; i < MAX_PLAYERS; i++)
  {
    // Only digital pads supported for now.
    si.SetStringValue(Controller::GetSettingsSection(i).c_str(), "Type",
                      Settings::GetControllerTypeName(ControllerType::DigitalController));
  }

  // No runahead or rewind, that'd be a disaster.
  si.SetIntValue("Main", "RunaheadFrameCount", 0);
  si.SetBoolValue("Main", "RewindEnable", false);

  // no block linking, it degrades savestate loading performance
  si.SetBoolValue("CPU", "RecompilerBlockLinking", false);

  Host::Internal::SetNetplaySettingsLayer(&si);
  System::ApplySettings(false);
}

//////////////////////////////////////////////////////////////////////////
// Frame Pacing
//////////////////////////////////////////////////////////////////////////

void Netplay::InitializeFramePacing()
{
  // Start at 100% speed, adjust as soon as we get a timesync event.
  s_target_speed = 1.0f;
  UpdateThrottlePeriod();

  s_next_frame_time = Common::Timer::GetCurrentValue() + s_frame_period;
}

void Netplay::UpdateThrottlePeriod()
{
  s_frame_period =
    Common::Timer::ConvertSecondsToValue(1.0 / (static_cast<double>(System::GetThrottleFrequency()) * s_target_speed));
}

void Netplay::HandleTimeSyncEvent(float frame_delta, int update_interval)
{
  // only activate timesync if its worth correcting.
  if (std::abs(frame_delta) < 1.0f)
    return;
  // Distribute the frame difference over the next N * 0.75 frames.
  // only part of the interval time is used since we want to come back to normal speed.
  // otherwise we will keep spiraling into unplayable gameplay.
  float total_time = (frame_delta * s_frame_period) / 4;
  float mun_timesync_frames = update_interval * 0.75f;
  float added_time_per_frame = -(total_time / mun_timesync_frames);
  float iterations_per_frame = 1.0f / s_frame_period;

  s_target_speed = (s_frame_period + added_time_per_frame) * iterations_per_frame;
  s_next_timesync_recovery_frame = CurrentFrame() + static_cast<s32>(std::ceil(mun_timesync_frames));

  UpdateThrottlePeriod();

  Log_VerbosePrintf("TimeSync: %f frames %s, target speed %.4f%%", std::abs(frame_delta),
                    (frame_delta >= 0.0f ? "ahead" : "behind"), s_target_speed * 100.0f);
}

void Netplay::Throttle()
{
  // if the s_next_timesync_recovery_frame has been reached revert back to the normal throttle speed
  s32 current_frame = CurrentFrame();
  if (s_target_speed != 1.0f && current_frame >= s_next_timesync_recovery_frame)
  {
    s_target_speed = 1.0f;
    UpdateThrottlePeriod();

    Log_VerbosePrintf("TimeSync Recovery: frame %d, target speed %.4f%%", current_frame, s_target_speed * 100.0f);
  }

  s_next_frame_time += s_frame_period;

  // If we're running too slow, advance the next frame time based on the time we lost. Effectively skips
  // running those frames at the intended time, because otherwise if we pause in the debugger, we'll run
  // hundreds of frames when we resume.
  Common::Timer::Value current_time = Common::Timer::GetCurrentValue();
  if (current_time > s_next_frame_time)
  {
    const Common::Timer::Value diff = static_cast<s64>(current_time) - static_cast<s64>(s_next_frame_time);
    s_next_frame_time += (diff / s_frame_period) * s_frame_period;
    return;
  }
  // Poll at 2ms throughout the sleep.
  // This way the network traffic comes through as soon as possible.
  const Common::Timer::Value sleep_period = Common::Timer::ConvertMillisecondsToValue(1);
  while (s_state == SessionState::Running)
  {
    // TODO: make better, we can tell this function to stall until the next frame
    PollEnet(0);
    if (s_ggpo)
    {
      ggpo_network_idle(s_ggpo);
      PollEnet(0);
    }

    current_time = Common::Timer::GetCurrentValue();
    if (current_time >= s_next_frame_time)
      break;

    // Spin for the last millisecond.
    if ((s_next_frame_time - current_time) <= sleep_period)
      Common::Timer::BusyWait(s_next_frame_time - current_time);
    else
      Common::Timer::SleepUntil(current_time + sleep_period, false);
  }
}

void Netplay::GenerateChecksumForFrame(int* checksum, int frame, unsigned char* buffer, int buffer_size)
{
  const u32 sliding_window_size = 4096 * 4; // 4 pages.
  const u32 num_group_of_pages = buffer_size / sliding_window_size;
  const u32 start_position = (frame % num_group_of_pages) * sliding_window_size;
  *checksum = XXH32(buffer + start_position, sliding_window_size, frame);
  // Log_VerbosePrintf("Netplay Checksum: f:%d wf:%d c:%u", frame, frame % num_group_of_pages, *checksum);
}

void Netplay::AdvanceFrame()
{
  ggpo_advance_frame(s_ggpo, 0);
}

void Netplay::RunFrame()
{
  PollEnet(0);

  if (!s_ggpo)
    return;
  // housekeeping
  ggpo_network_idle(s_ggpo);
  ggpo_idle(s_ggpo);
  // run game
  auto result = GGPO_OK;
  int disconnect_flags = 0;
  Netplay::Input inputs[2] = {};
  // add local input
  if (s_local_handle != GGPO_INVALID_HANDLE)
  {
    auto inp = ReadLocalInput();
    result = AddLocalInput(inp);
  }
  // advance game
  if (GGPO_SUCCEEDED(result))
  {
    result = SyncInput(inputs, &disconnect_flags);
    if (GGPO_SUCCEEDED(result))
    {
      // enable again when rolling back done
      SPU::SetAudioOutputMuted(false);
      NetplayAdvanceFrame(inputs, disconnect_flags);
    }
  }
}

s32 Netplay::CurrentFrame()
{
  s32 current = -1;
  ggpo_get_current_frame(s_ggpo, current);
  return current;
}

void Netplay::CollectInput(u32 slot, u32 bind, float value)
{
  s_net_input[slot][bind] = value;
}

Netplay::Input Netplay::ReadLocalInput()
{
  // get controller data of the first controller (0 internally)
  Netplay::Input inp{0};
  for (u32 i = 0; i < (u32)DigitalController::Button::Count; i++)
  {
    if (s_net_input[0][i] >= 0.25f)
      inp.button_data |= 1 << i;
  }
  return inp;
}

void Netplay::SendMsg(std::string msg)
{
  SessionChatMessage header{};
  const size_t msg_size = msg.size();

  header.header.type = SessionMessage::ChatMessage;
  header.header.size = sizeof(SessionChatMessage) + msg_size;
  header.chat_message_size = msg_size;

  ENetPacket* pkt = enet_packet_create(nullptr, sizeof(header) + msg_size, ENET_PACKET_FLAG_RELIABLE);
  std::memcpy(pkt->data, &header, sizeof(header));
  std::memcpy(pkt->data + sizeof(header), msg.c_str(), msg_size);

  for (s32 i = 0; i < MAX_PLAYERS; i++)
  {
    if (!s_peers[i].peer)
      continue;

    const int err = enet_peer_send(s_peers[i].peer, ENET_CHANNEL_SESSION, pkt);
    if (err != 0)
    {
      // failed to send netplay message? just clean it up.
      Log_ErrorPrint("Failed to send netplay message");
      enet_packet_destroy(pkt);
    }
  }

  // add own netplay message locally to netplay messages
  Host::OnNetplayMessage(fmt::format("Player {}: {}", s_local_handle, msg));
}

GGPOErrorCode Netplay::SyncInput(Netplay::Input inputs[2], int* disconnect_flags)
{
  return ggpo_synchronize_input(s_ggpo, inputs, sizeof(Netplay::Input) * 2, disconnect_flags);
}

GGPOErrorCode Netplay::AddLocalInput(Netplay::Input input)
{
  return ggpo_add_local_input(s_ggpo, s_local_handle, &input, sizeof(Netplay::Input));
}

s32 Netplay::GetPing()
{
  const int handle = s_local_handle == 1 ? 2 : 1;
  ggpo_get_network_stats(s_ggpo, handle, &s_last_net_stats);
  return s_last_net_stats.network.ping;
}

u32 Netplay::GetMaxPrediction()
{
  return MAX_ROLLBACK_FRAMES;
}

void Netplay::SetInputs(Netplay::Input inputs[2])
{
  for (u32 i = 0; i < 2; i++)
  {
    auto cont = Pad::GetController(i);
    std::bitset<sizeof(u32) * 8> button_bits(inputs[i].button_data);
    for (u32 j = 0; j < (u32)DigitalController::Button::Count; j++)
      cont->SetBindState(j, button_bits.test(j) ? 1.0f : 0.0f);
  }
}

void Netplay::StartNetplaySession(s32 local_handle, u16 local_port, std::string& remote_addr, u16 remote_port,
                                  s32 input_delay, std::string game_path)
{
  // dont want to start a session when theres already one going on.
  if (IsActive())
    return;

  // create session
  if (!Netplay::Start(local_handle, local_port, remote_addr, remote_port, input_delay, MAX_ROLLBACK_FRAMES,
                      std::move(game_path)))
  {
    // this'll call back to us to shut everything netplay-related down
    Log_ErrorPrint("Failed to Create Netplay Session!");
    System::ShutdownSystem(false);
  }
  else
  {
    // Load savestate if available
    std::string save = EmuFolders::SaveStates + "/netplay/" + System::GetRunningSerial() + ".sav";
    System::LoadState(save.c_str());
  }
}

void Netplay::StopNetplaySession()
{
  if (!IsActive())
    return;

  // This will call back to us.
  System::ShutdownSystem(false);
}

void Netplay::NetplayAdvanceFrame(Netplay::Input inputs[], int disconnect_flags)
{
  Netplay::SetInputs(inputs);
  System::RunFrame();
  Netplay::AdvanceFrame();
}

void Netplay::ExecuteNetplay()
{
  while (s_state != SessionState::Inactive)
  {
    switch (s_state)
    {
      case SessionState::Connecting:
      {
        // still waiting for connection to host..
        // TODO: add a timeout here.
        PollEnet(Common::Timer::GetCurrentValue() + Common::Timer::ConvertMillisecondsToValue(16));
        Host::DisplayLoadingScreen("Connecting to host...");
        Host::PumpMessagesOnCPUThread();
        continue;
      }

      case SessionState::Synchronizing:
      {
        // only happens on host, when waiting for clients to report back
        PollEnet(Common::Timer::GetCurrentValue() + Common::Timer::ConvertMillisecondsToValue(16));
        Host::DisplayLoadingScreen("Netplay synchronizing", 0, s_synchronized_players, s_num_players);
        Host::PumpMessagesOnCPUThread();
        continue;
      }

      case SessionState::Running:
      {
        Netplay::RunFrame();

        // this can shut us down
        Host::PumpMessagesOnCPUThread();
        if (!System::IsValid())
          break;

        System::PresentFrame();
        System::UpdatePerformanceCounters();

        Throttle();
        continue;
      }

      default:
      case SessionState::Initializing:
      case SessionState::Inactive:
      {
        // shouldn't be here
        Panic("Invalid state");
        continue;
      }
    }
  }
}

bool Netplay::NpAdvFrameCb(void* ctx, int flags)
{
  Netplay::Input inputs[2] = {};
  int disconnect_flags;
  Netplay::SyncInput(inputs, &disconnect_flags);
  NetplayAdvanceFrame(inputs, disconnect_flags);
  return true;
}

bool Netplay::NpSaveFrameCb(void* ctx, unsigned char** buffer, int* len, int* checksum, int frame)
{
  SaveStateBuffer our_buffer;
  if (s_save_buffer_pool.empty())
  {
    our_buffer = std::make_unique<System::MemorySaveState>();
  }
  else
  {
    our_buffer = std::move(s_save_buffer_pool.front());
    s_save_buffer_pool.pop_front();
  }

  if (!System::SaveMemoryState(our_buffer.get()))
  {
    s_save_buffer_pool.push_front(std::move(our_buffer));
    return false;
  }

  // desync detection
  const u32 state_size = static_cast<u32>(our_buffer.get()->state_stream.get()->GetPosition());
  unsigned char* state = reinterpret_cast<unsigned char*>(our_buffer.get()->state_stream.get()->GetMemoryPointer());
  GenerateChecksumForFrame(checksum, frame, state, state_size);

#if 0
  if (frame > 100 && frame < 150 && s_num_players > 1)
  {
    std::string filename =
      Path::Combine(EmuFolders::Dumps, fmt::format("f{}_c{}_p{}.bin", frame, (u32)*checksum, s_local_handle));
    FileSystem::WriteBinaryFile(filename.c_str(), state, state_size);
  }

  Log_VerbosePrintf("Saved real frame %u as GGPO frame %d CS %u", System::GetFrameNumber(), frame, *checksum);
#endif

  *len = sizeof(System::MemorySaveState);
  *buffer = reinterpret_cast<unsigned char*>(our_buffer.release());

  return true;
}

bool Netplay::NpLoadFrameCb(void* ctx, unsigned char* buffer, int len, int rb_frames, int frame_to_load)
{
  // Disable Audio For upcoming rollback
  SPU::SetAudioOutputMuted(true);

  const u32 prev_frame = System::GetFrameNumber();
  if (!System::LoadMemoryState(*reinterpret_cast<const System::MemorySaveState*>(buffer)))
    return false;

#if 0
  Log_VerbosePrintf("Loaded real frame %u from GGPO frame %d [prev %u]", System::GetFrameNumber(), frame_to_load,
                    prev_frame);
#endif

  return true;
}

void Netplay::NpFreeBuffCb(void* ctx, void* buffer, int frame)
{
  // Log_VerbosePrintf("Reuse Buffer: %d", frame);
  SaveStateBuffer our_buffer(reinterpret_cast<System::MemorySaveState*>(buffer));
  s_save_buffer_pool.push_back(std::move(our_buffer));
}

bool Netplay::NpOnEventCb(void* ctx, GGPOEvent* ev)
{
  switch (ev->code)
  {
    case GGPOEventCode::GGPO_EVENTCODE_CONNECTED_TO_PEER:
      Host::OnNetplayMessage(fmt::format("Netplay Connected To Player: {}", ev->u.connected.player));
      break;
    case GGPOEventCode::GGPO_EVENTCODE_SYNCHRONIZING_WITH_PEER:
      Host::OnNetplayMessage(
        fmt::format("Netplay Synchronzing: {}/{}", ev->u.synchronizing.count, ev->u.synchronizing.total));
      break;
    case GGPOEventCode::GGPO_EVENTCODE_SYNCHRONIZED_WITH_PEER:
      Host::OnNetplayMessage(fmt::format("Netplay Synchronized With Player: {}", ev->u.synchronized.player));
      break;
    case GGPOEventCode::GGPO_EVENTCODE_RUNNING:
      Host::OnNetplayMessage("Netplay Is Running");
      break;
    case GGPOEventCode::GGPO_EVENTCODE_TIMESYNC:
      HandleTimeSyncEvent(ev->u.timesync.frames_ahead, ev->u.timesync.timeSyncPeriodInFrames);
      break;
    case GGPOEventCode::GGPO_EVENTCODE_DESYNC:
      Host::OnNetplayMessage(fmt::format("Desync Detected: Current Frame: {}, Desync Frame: {}, Diff: {}, L:{}, R:{}",
                                         CurrentFrame(), ev->u.desync.nFrameOfDesync,
                                         CurrentFrame() - ev->u.desync.nFrameOfDesync, ev->u.desync.ourCheckSum,
                                         ev->u.desync.remoteChecksum));
      break;
    default:
      Host::OnNetplayMessage(fmt::format("Netplay Event Code: {}", static_cast<int>(ev->code)));
      break;
  }

  return true;
}
