#include "netplay.h"
#include "common/byte_stream.h"
#include "common/gpu_texture.h"
#include "common/log.h"
#include "common/memory_settings_interface.h"
#include "common/string_util.h"
#include "common/timer.h"
#include "digital_controller.h"
#include "ggponet.h"
#include "enet/enet.h"
#include "host.h"
#include "host_settings.h"
#include "pad.h"
#include "spu.h"
#include "system.h"
#include <bitset>
#include <gsl/span>
#include <deque>
#include <xxhash.h>
Log_SetChannel(Netplay);

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#endif

namespace Netplay {

using SaveStateBuffer = std::unique_ptr<System::MemorySaveState>;

struct Input
{
  u32 button_data;
};

static bool InitializeEnet();

static bool NpAdvFrameCb(void* ctx, int flags);
static bool NpSaveFrameCb(void* ctx, unsigned char** buffer, int* len, int* checksum, int frame);
static bool NpLoadFrameCb(void* ctx, unsigned char* buffer, int len, int rb_frames, int frame_to_load);
static bool NpBeginGameCb(void* ctx, const char* game_name);
static void NpFreeBuffCb(void* ctx, void* buffer, int frame);
static bool NpOnEventCb(void* ctx, GGPOEvent* ev);

static GGPOPlayerHandle PlayerIdToGGPOHandle(s32 player_id);
static Input ReadLocalInput();
static GGPOErrorCode AddLocalInput(Netplay::Input input);
static GGPOErrorCode SyncInput(Input inputs[2], int* disconnect_flags);
static void SetInputs(Input inputs[2]);

static void SetSettings();

static bool CreateSystem(std::string game_path);

// ENet
static void ShutdownEnetHost();
static s32 GetFreePlayerId();
static s32 GetPlayerIdForPeer(const ENetPeer* peer);
static bool ConnectToLowerPeers(gsl::span<const ENetAddress> peer_addresses);
static bool WaitForPeerConnections();
static void HandleEnetEvent(const ENetEvent* event);
static void PollEnet(Common::Timer::Value until_time);
static void HandleControlPacket(s32 player_id, const ENetPacket* pkt);

// l = local, r = remote
static s32 Start(s32 lhandle, u16 lport, const std::string& raddr, u16 rport, s32 ldelay, u32 pred, std::string game_path);

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
static void GenerateDesyncReport(s32 desync_frame);

//////////////////////////////////////////////////////////////////////////
// Variables
//////////////////////////////////////////////////////////////////////////

static MemorySettingsInterface s_settings_overlay;


/// Enet
static ENetHost* s_enet_host;
static std::array<ENetPeer*, MAX_PLAYERS> s_enet_peers;
static s32 s_player_id;

static GGPOPlayerHandle s_local_handle = GGPO_INVALID_HANDLE;
static GGPONetworkStats s_last_net_stats{};
static GGPOSession* s_ggpo = nullptr;

static std::deque<SaveStateBuffer> s_save_buffer_pool;

static std::array<std::array<float, 32>, NUM_CONTROLLER_AND_CARD_PORTS> s_net_input;

/// Frame timing. We manage our own frame pacing here, because we need to constantly adjust.
static float s_target_speed = 1.0f;
static Common::Timer::Value s_frame_period = 0;
static Common::Timer::Value s_next_frame_time = 0;
static s32 s_next_timesync_recovery_frame = -1;

} // namespace Netplay

// Netplay Impl

bool Netplay::CreateSystem(std::string game_path)
{
  // close system if its already running
  if (System::IsValid())
    System::ShutdownSystem(false);

  // fast boot the selected game and wait for the other player
  auto param = SystemBootParameters(std::move(game_path));
  param.override_fast_boot = true;
  return System::BootSystem(param);
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
    if (s_enet_peers[i])
    {
      enet_peer_reset(s_enet_peers[i]);
      s_enet_peers[i] = nullptr;
    }
  }

  enet_host_destroy(s_enet_host);
  s_enet_host = nullptr;
}

GGPOPlayerHandle PlayerIdToGGPOHandle(s32 player_id)
{
  return player_id + 1;
}

s32 Netplay::GetPlayerIdForPeer(const ENetPeer* peer)
{
  for (s32 i = 0; i < MAX_PLAYERS; i++)
  {
    if (s_enet_peers[i] == peer)
      return i;
  }

  return -1;
}

s32 Netplay::GetFreePlayerId()
{
  for (s32 i = 0; i < MAX_PLAYERS; i++)
  {
    if (i != s_player_id && !s_enet_peers[i])
      return i;
  }

  return -1;
}

void Netplay::HandleEnetEvent(const ENetEvent* event)
{
  switch (event->type)
  {
    case ENET_EVENT_TYPE_CONNECT:
    {
      // skip when it's one we set up ourselves, we're handling it in ConnectToLowerPeers().
      if (GetPlayerIdForPeer(event->peer) >= 0)
        return;

      // TODO: the player ID should either come from the packet (for the non-first player),
      // or be auto-assigned as below, for the connection to the first host
      const s32 new_player_id = GetFreePlayerId();
      Log_DevPrintf("Enet connect event: New client with id %d", new_player_id);

      if (new_player_id < 0)
      {
        Log_ErrorPrintf("No free slots, disconnecting client");
        enet_peer_disconnect(event->peer, 1);
        return;
      }

      s_enet_peers[new_player_id] = event->peer;
    }
    break;

    case ENET_EVENT_TYPE_DISCONNECT:
    {
      const s32 player_id = GetPlayerIdForPeer(event->peer);
      if (player_id < 0)
        return;

      // TODO: This one's gonna get kinda tricky... who do we orphan when they disconnect?
      Log_WarningPrintf("ENet player %d disconnected", player_id);
      Host::OnNetplayMessage(fmt::format("*** DISCONNECTED PLAYER {} ***", player_id));
      ggpo_disconnect_player(s_ggpo, PlayerIdToGGPOHandle(player_id));
      s_enet_peers[player_id] = nullptr;
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
          HandleControlPacket(player_id, event->packet);
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

bool Netplay::ConnectToLowerPeers(gsl::span<const ENetAddress> peer_addresses)
{
  for (size_t i = 0; i < peer_addresses.size(); i++)
  {
    char ipstr[32];
    if (enet_address_get_host_ip(&peer_addresses[i], ipstr, std::size(ipstr)) != 0)
      ipstr[0] = 0;
    Log_DevPrintf("Starting connection to peer %u at %s:%u", i, ipstr, peer_addresses[i].port);

    DebugAssert(i != s_player_id);
    s_enet_peers[i] = enet_host_connect(s_enet_host, &peer_addresses[i], NUM_ENET_CHANNELS, 0);
    if (!s_enet_peers[i])
    {
      Log_ErrorPrintf("enet_host_connect() for peer %u failed", i);
      return false;
    }
  }

  return true;
}

bool Netplay::WaitForPeerConnections()
{
  static constexpr float MAX_CONNECT_TIME = 30.0f;
  Common::Timer timeout;

  const u32 clients_to_connect = MAX_PLAYERS - 1;

  for (;;)
  {
    // TODO: Handle early shutdown/cancel request.
    u32 num_connected_peers = 0;
    for (s32 i = 0; i < MAX_PLAYERS; i++)
    {
      if (i != s_player_id && s_enet_peers[i] && s_enet_peers[i]->state == ENET_PEER_STATE_CONNECTED)
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

void Netplay::HandleControlPacket(s32 player_id, const ENetPacket* pkt)
{
  // TODO
  Log_ErrorPrintf("Unhandled control packet from player %d of size %zu", player_id, pkt->dataLength);
}

s32 Netplay::Start(s32 lhandle, u16 lport, const std::string& raddr, u16 rport, s32 ldelay, u32 pred, std::string game_path)
{
  SetSettings();
  if (!InitializeEnet())
    return -1;

  ENetAddress host_address;
  host_address.host = ENET_HOST_ANY;
  host_address.port = lport;
  s_enet_host = enet_host_create(&host_address, MAX_PLAYERS - 1, NUM_ENET_CHANNELS, 0, 0);
  if (!s_enet_host)
  {
    Log_ErrorPrintf("Failed to create enet host.");
    return -1;
  }

  // Connect to any lower-ID'ed hosts.
  // Eventually we'll assign these IDs as players connect, and everyone not starting it will get their ID sent back
  s_player_id = lhandle - 1;

  std::array<ENetAddress, MAX_PLAYERS> peer_addresses;
  const u32 num_peer_addresses = s_player_id;
  DebugAssert(num_peer_addresses == 0 || num_peer_addresses == 1);
  if (num_peer_addresses == 1)
  {
    // TODO: rewrite this when we support more players
    const u32 other_player_id = (lhandle == 1) ? 1 : 0;
    if (enet_address_set_host_ip(&peer_addresses[other_player_id], raddr.c_str()) != 0)
    {
      Log_ErrorPrintf("Failed to parse host: '%s'", raddr.c_str());
      ShutdownEnetHost();
      return -1;
    }

    peer_addresses[other_player_id].port = rport;
  }

  // Create system.
  if (!CreateSystem(std::move(game_path)))
  {
    Log_ErrorPrintf("Failed to create system.");
    ShutdownEnetHost();
    return -1;
  }
  InitializeFramePacing();

  // Connect to all peers.
  if ((num_peer_addresses > 0 &&
       !ConnectToLowerPeers(gsl::span<const ENetAddress>(peer_addresses).subspan(0, num_peer_addresses))) ||
      !WaitForPeerConnections())
  {
    // System shutdown cleans up enet.
    return -1;
  }

  /*
  TODO: since saving every frame during rollback loses us time to do actual gamestate iterations it might be better to
  hijack the update / save / load cycle to only save every confirmed frame only saving when actually needed.
  */
  GGPOSessionCallbacks cb{};

  cb.advance_frame = NpAdvFrameCb;
  cb.save_game_state = NpSaveFrameCb;
  cb.load_game_state = NpLoadFrameCb;
  cb.begin_game = NpBeginGameCb;
  cb.free_buffer = NpFreeBuffCb;
  cb.on_event = NpOnEventCb;

  GGPOErrorCode result;

  result = ggpo_start_session(&s_ggpo, &cb, "Duckstation-Netplay", MAX_PLAYERS, sizeof(Netplay::Input), lport, MAX_ROLLBACK_FRAMES);
  if (!GGPO_SUCCEEDED(result))
  {
    Log_ErrorPrintf("ggpo_start_session() failed: %d", result);
    return -1;
  }

  ggpo_set_disconnect_timeout(s_ggpo, 2000);
  ggpo_set_disconnect_notify_start(s_ggpo, 1000);

  for (s32 i = 0; i < MAX_PLAYERS; i++)
  {
    if (!s_enet_peers[i] && i != s_player_id)
      continue;

    // This is *awful*. Need to merge the player IDs, enough of this indices-start-at-1 rubbish.
    const GGPOPlayerHandle expected_handle = PlayerIdToGGPOHandle(i);

    GGPOPlayer player = { sizeof(GGPOPlayer) };
    GGPOPlayerHandle got_handle;
    player.player_num = expected_handle;
    if (i == s_player_id)
    {
      player.type = GGPO_PLAYERTYPE_LOCAL;
      result = ggpo_add_player(s_ggpo, &player, &got_handle);
      if (GGPO_SUCCEEDED(result))
        s_local_handle = got_handle;
    }
    else
    {
      player.type = GGPO_PLAYERTYPE_REMOTE;
      player.u.remote.peer = s_enet_peers[i];
      result = ggpo_add_player(s_ggpo, &player, &got_handle);
    }

    if (!GGPO_SUCCEEDED(result))
    {
      Log_ErrorPrintf("Failed to add player %d", i);
      return -1;
    }

    Assert(expected_handle == got_handle);
  }

  ggpo_set_frame_delay(s_ggpo, s_local_handle, ldelay);

  return result;
}

void Netplay::CloseSession()
{
  Assert(IsActive());

  ggpo_close_session(s_ggpo);
  s_ggpo = nullptr;
  s_save_buffer_pool.clear();
  s_local_handle = GGPO_INVALID_HANDLE;

  ShutdownEnetHost();

  // Restore original settings.
  Host::Internal::SetNetplaySettingsLayer(nullptr);
  System::ApplySettings(false);
}

bool Netplay::IsActive()
{
  return s_ggpo != nullptr;
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
  // not sure its needed but enabled for now... TODO
  si.SetBoolValue("GPU", "UseSoftwareRendererForReadbacks", true);

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
  for (;;)
  {
    // TODO: make better, we can tell this function to stall until the next frame
    PollEnet(0);
    ggpo_network_idle(s_ggpo);
    PollEnet(0);

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

void Netplay::GenerateDesyncReport(s32 desync_frame) 
{
  std::string path = "\\netplaylogs\\desync_frame_" + std::to_string(desync_frame) + "_p" +
                     std::to_string(s_local_handle) + "_" + System::GetRunningSerial() + "_.txt";
  std::string filename = EmuFolders::Dumps + path;

  std::unique_ptr<ByteStream> stream =
    ByteStream::OpenFile(filename.c_str(), BYTESTREAM_OPEN_CREATE | BYTESTREAM_OPEN_WRITE | BYTESTREAM_OPEN_TRUNCATE |
                                             BYTESTREAM_OPEN_ATOMIC_UPDATE | BYTESTREAM_OPEN_STREAMED);
  if (!stream)
  {
    Log_VerbosePrint("desync log creation failed to create stream");
    return;
  }

  if (!ByteStream::WriteBinaryToStream(stream.get(),
                                       s_save_buffer_pool.back().get()->state_stream.get()->GetMemoryPointer(),
                                       s_save_buffer_pool.back().get()->state_stream.get()->GetMemorySize()))
  {
    Log_VerbosePrint("desync log creation failed to write the stream");
    stream->Discard();
    return;
  }
 /* stream->Write(s_save_buffer_pool.back().get()->state_stream.get()->GetMemoryPointer(),
                s_save_buffer_pool.back().get()->state_stream.get()->GetMemorySize());*/

  stream->Commit();

  Log_VerbosePrintf("desync log created for frame %d", desync_frame);
}


void Netplay::AdvanceFrame()
{
  ggpo_advance_frame(s_ggpo, 0);
}

void Netplay::RunFrame()
{
  // housekeeping
  // TODO: get rid of double polling
  PollEnet(0);
  ggpo_network_idle(s_ggpo);
  PollEnet(0);
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

GGPOPlayerHandle Netplay::PlayerIdToGGPOHandle(s32 player_id)
{
  return player_id + 1;
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

void Netplay::SendMsg(const char* msg)
{
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
  int result = Netplay::Start(local_handle, local_port, remote_addr, remote_port, input_delay, MAX_ROLLBACK_FRAMES, std::move(game_path));
  // notify that the session failed
  if (result != GGPO_OK)
  {
    Log_ErrorPrintf("Failed to Create Netplay Session! Error: %d", result);
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
  while (System::IsRunning())
  {
    Netplay::RunFrame();

    // this can shut us down
    Host::PumpMessagesOnCPUThread();
    if (!System::IsValid())
      break;

    System::PresentFrame();
    System::UpdatePerformanceCounters();

    Throttle();
  }
}

bool Netplay::NpBeginGameCb(void* ctx, const char* game_name)
{
  SPU::SetAudioOutputMuted(true);
  // Fast Forward to Game Start if needed.
  while (System::GetInternalFrameNumber() < 2)
    System::RunFrame();
  SPU::SetAudioOutputMuted(false);
  // Set Initial Frame Pacing
  InitializeFramePacing();
  return true;
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
  // min size is 2 because otherwise the desync logger doesnt have enough time to dump the state.
  if (s_save_buffer_pool.size() < 2)
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
  const u32 state_size = our_buffer.get()->state_stream.get()->GetMemorySize();
  unsigned char* state = reinterpret_cast<unsigned char*>(our_buffer.get()->state_stream.get()->GetMemoryPointer());
  GenerateChecksumForFrame(checksum, frame, state, state_size);

  *len = sizeof(System::MemorySaveState);
  *buffer = reinterpret_cast<unsigned char*>(our_buffer.release());

  return true;
}

bool Netplay::NpLoadFrameCb(void* ctx, unsigned char* buffer, int len, int rb_frames, int frame_to_load)
{
  // Disable Audio For upcoming rollback
  SPU::SetAudioOutputMuted(true);

  return System::LoadMemoryState(*reinterpret_cast<const System::MemorySaveState*>(buffer));
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
      Host::OnNetplayMessage(fmt::format("Netplay Synchronzing: {}/{}", ev->u.synchronizing.count, ev->u.synchronizing.total));
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
      Host::OnNetplayMessage(fmt::format("Desync Detected: Current Frame: {}, Desync Frame: {}, Diff: {}, L:{}, R:{}", CurrentFrame(),
              ev->u.desync.nFrameOfDesync, CurrentFrame() - ev->u.desync.nFrameOfDesync, ev->u.desync.ourCheckSum,
              ev->u.desync.remoteChecksum));
      GenerateDesyncReport(ev->u.desync.nFrameOfDesync);
      break;
    default:
      Host::OnNetplayMessage(fmt::format("Netplay Event Code: {}", static_cast<int>(ev->code)));
      break;
  }

  return true;
}
