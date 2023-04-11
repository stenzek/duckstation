#include "netplay.h"
#include "common/byte_stream.h"
#include "common/gpu_texture.h"
#include "common/log.h"
#include "digital_controller.h"
#include "ggponet.h"
#include "pad.h"
#include "spu.h"
#include "system.h"
#include <bitset>
#include <deque>
Log_SetChannel(Netplay);

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#endif

namespace Netplay {

struct Input
{
  u32 button_data;
};

struct LoopTimer
{
public:
  void Init(u32 fps, u32 frames_to_spread_wait);
  void OnGGPOTimeSyncEvent(float frames_ahead);
  // Call every loop, to get the amount of time the current iteration of gameloop should take
  s32 UsToWaitThisLoop();

private:
  float m_last_advantage = 0.0f;
  s32 m_us_per_game_loop = 0;
  s32 m_us_ahead = 0;
  s32 m_us_extra_to_wait = 0;
  s32 m_frames_to_spread_wait = 0;
  s32 m_wait_count = 0;
};

static bool NpAdvFrameCb(void* ctx, int flags);
static bool NpSaveFrameCb(void* ctx, unsigned char** buffer, int* len, int* checksum, int frame);
static bool NpLoadFrameCb(void* ctx, unsigned char* buffer, int len, int rb_frames, int frame_to_load);
static bool NpBeginGameCb(void* ctx, const char* game_name);
static void NpFreeBuffCb(void* ctx, void* buffer);
static bool NpOnEventCb(void* ctx, GGPOEvent* ev);

static Input ReadLocalInput();
static GGPOErrorCode AddLocalInput(Netplay::Input input);
static GGPOErrorCode SyncInput(Input inputs[2], int* disconnect_flags);
static void SetInputs(Input inputs[2]);

static LoopTimer* GetTimer();

// l = local, r = remote
static s32 Start(s32 lhandle, u16 lport, std::string& raddr, u16 rport, s32 ldelay, u32 pred);
static void Close();
static void RunIdle();

static void AdvanceFrame(u16 checksum = 0);
static void RunFrame(s32& waitTime);

static void NetplayAdvanceFrame(Netplay::Input inputs[], int disconnect_flags);

static LoopTimer s_timer;
static std::string s_game_path;
static u32 s_max_pred = 0;

static GGPOPlayerHandle s_local_handle = GGPO_INVALID_HANDLE;
static GGPONetworkStats s_last_net_stats{};
static GGPOSession* s_ggpo = nullptr;

static std::deque<System::MemorySaveState> s_netplay_states;

static std::array<std::array<float, 32>, NUM_CONTROLLER_AND_CARD_PORTS> s_net_input;

} // namespace Netplay

// Netplay Impl

s32 Netplay::Start(s32 lhandle, u16 lport, std::string& raddr, u16 rport, s32 ldelay, u32 pred)
{
  s_max_pred = pred;
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

  result = ggpo_start_session(&s_ggpo, &cb, "Duckstation-Netplay", 2, sizeof(Netplay::Input), lport, s_max_pred);
  //result = ggpo_start_synctest(&s_ggpo, &cb, (char*)"asdf", 2, sizeof(Netplay::Input), 1);

  ggpo_set_disconnect_timeout(s_ggpo, 3000);
  ggpo_set_disconnect_notify_start(s_ggpo, 1000);

  for (int i = 1; i <= 2; i++)
  {
    GGPOPlayer player = {};
    GGPOPlayerHandle handle = 0;

    player.size = sizeof(GGPOPlayer);
    player.player_num = i;

    if (lhandle == i)
    {
      player.type = GGPOPlayerType::GGPO_PLAYERTYPE_LOCAL;
      result = ggpo_add_player(s_ggpo, &player, &handle);
      s_local_handle = handle;
    }
    else
    {
      player.type = GGPOPlayerType::GGPO_PLAYERTYPE_REMOTE;
#ifdef _WIN32
      strcpy_s(player.u.remote.ip_address, raddr.c_str());
#else
      strcpy(player.u.remote.ip_address, raddr.c_str());
#endif
      player.u.remote.port = rport;
      result = ggpo_add_player(s_ggpo, &player, &handle);
    }
  }
  ggpo_set_frame_delay(s_ggpo, s_local_handle, ldelay);

  return result;
}

void Netplay::Close()
{
  ggpo_close_session(s_ggpo);
  s_ggpo = nullptr;
  s_local_handle = GGPO_INVALID_HANDLE;
  s_max_pred = 0;
}

bool Netplay::IsActive()
{
  return s_ggpo != nullptr;
}

void Netplay::RunIdle()
{
  ggpo_idle(s_ggpo);
}

void Netplay::AdvanceFrame(u16 checksum)
{
  ggpo_advance_frame(s_ggpo, checksum);
}

void Netplay::RunFrame(s32& waitTime)
{
  // run game
  auto result = GGPO_OK;
  int disconnectFlags = 0;
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
    result = SyncInput(inputs, &disconnectFlags);
    if (GGPO_SUCCEEDED(result))
    {
      // enable again when rolling back done
      SPU::SetAudioOutputMuted(false);
      NetplayAdvanceFrame(inputs, disconnectFlags);
    }
    else
      RunIdle();
  }
  else
    RunIdle();

  waitTime = GetTimer()->UsToWaitThisLoop();
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

void Netplay::SendMsg(const char* msg)
{
  ggpo_client_chat(s_ggpo, msg);
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
  return s_max_pred;
}

void Netplay::SetInputs(Netplay::Input inputs[2])
{
  for (u32 i = 0; i < 2; i++)
  {
    auto cont = Pad::GetController(i);
    std::bitset<sizeof(u32) * 8> buttonBits(inputs[i].button_data);
    for (u32 j = 0; j < (u32)DigitalController::Button::Count; j++)
      cont->SetBindState(j, buttonBits.test(j) ? 1.0f : 0.0f);
  }
}

Netplay::LoopTimer* Netplay::GetTimer()
{
  return &s_timer;
}

void Netplay::LoopTimer::Init(u32 fps, u32 frames_to_spread_wait)
{
  m_us_per_game_loop = 1000000 / fps;
  m_us_ahead = 0;
  m_us_extra_to_wait = 0;
  m_frames_to_spread_wait = frames_to_spread_wait;
  m_last_advantage = 0.0f;
}

void Netplay::LoopTimer::OnGGPOTimeSyncEvent(float frames_ahead)
{
  m_last_advantage = (1000.0f * frames_ahead / 60.0f);
  m_last_advantage /= 2;
  if (m_last_advantage < 0)
  {
    int t = 0;
    t++;
  }
  m_us_extra_to_wait = (int)(m_last_advantage * 1000);
  if (m_us_extra_to_wait)
  {
    m_us_extra_to_wait /= m_frames_to_spread_wait;
    m_wait_count = m_frames_to_spread_wait;
  }
}

s32 Netplay::LoopTimer::UsToWaitThisLoop()
{
  s32 timetoWait = m_us_per_game_loop;
  if (m_wait_count)
  {
    timetoWait += m_us_extra_to_wait;
    m_wait_count--;
    if (!m_wait_count)
      m_us_extra_to_wait = 0;
  }
  return timetoWait;
}

void Netplay::StartNetplaySession(s32 local_handle, u16 local_port, std::string& remote_addr, u16 remote_port,
                                  s32 input_delay, std::string game_path)
{
  // dont want to start a session when theres already one going on.
  if (IsActive())
    return;
  // set game path for later loading during the begin game callback
  s_game_path = game_path;
  // set netplay timer
  const u32 fps = (System::GetRegion() == ConsoleRegion::PAL ? 50 : 60);
  GetTimer()->Init(fps, 180);
  // create session
  int result = Netplay::Start(local_handle, local_port, remote_addr, remote_port, input_delay, 8);
  if (result != GGPO_OK)
  {
    Log_ErrorPrintf("Failed to Create Netplay Session! Error: %d", result);
  }
}

void Netplay::StopNetplaySession()
{
  if (!IsActive())
    return;
  s_netplay_states.clear();
  Close();
}

void Netplay::NetplayAdvanceFrame(Netplay::Input inputs[], int disconnect_flags)
{
  Netplay::SetInputs(inputs);
  System::RunFrame();
  Netplay::AdvanceFrame();
}

void Netplay::ExecuteNetplay()
{
  // frame timing
  s32 timeToWait;
  std::chrono::steady_clock::time_point start, next, now;
  start = next = now = std::chrono::steady_clock::now();
  while (Netplay::IsActive() && System::IsRunning())
  {
    now = std::chrono::steady_clock::now();
    if (now >= next)
    {
      Netplay::RunFrame(timeToWait);
      next = now + std::chrono::microseconds(timeToWait);
      // s_next_frame_time += timeToWait;

      // this can shut us down
      Host::PumpMessagesOnCPUThread();
      if (!System::IsValid() || !Netplay::IsActive())
        break;

      System::PresentFrame();
      System::UpdatePerformanceCounters();
    }
  }
}

bool Netplay::NpBeginGameCb(void* ctx, const char* game_name)
{
  // close system if its already running
  if (System::IsValid())
    System::ShutdownSystem(false);
  // fast boot the selected game and wait for the other player
  auto param = SystemBootParameters(s_game_path);
  param.override_fast_boot = true;
  if (!System::BootSystem(param))
  {
    StopNetplaySession();
    return false;
  }
  // Fast Forward to Game Start
  SPU::SetAudioOutputMuted(true);
  while (System::GetInternalFrameNumber() < 2)
    System::RunFrame();
  SPU::SetAudioOutputMuted(false);
  return true;
}

bool Netplay::NpAdvFrameCb(void* ctx, int flags)
{
  Netplay::Input inputs[2] = {};
  int disconnectFlags;
  Netplay::SyncInput(inputs, &disconnectFlags);
  NetplayAdvanceFrame(inputs, disconnectFlags);
  return true;
}

bool Netplay::NpSaveFrameCb(void* ctx, uint8_t** buffer, int* len, int* checksum, int frame)
{
  bool result = false;
  // give ggpo something so it doesnt complain.
  u8 dummyData = 43;
  *len = sizeof(u8);
  *buffer = (unsigned char*)malloc(*len);
  if (!*buffer)
    return false;
  memcpy(*buffer, &dummyData, *len);
  // store state for later.
  int pred = Netplay::GetMaxPrediction();
  if (frame < pred && s_netplay_states.size() < pred)
  {
    System::MemorySaveState save;
    result = System::SaveMemoryState(&save);
    s_netplay_states.push_back(std::move(save));
  }
  else
  {
    // reuse streams
    result = System::SaveMemoryState(&s_netplay_states[frame % pred]);
  }
  return result;
}

bool Netplay::NpLoadFrameCb(void* ctx, uint8_t* buffer, int len, int rb_frames, int frame_to_load)
{
  // Disable Audio For upcoming rollback
  SPU::SetAudioOutputMuted(true);
  return System::LoadMemoryState(s_netplay_states[frame_to_load % Netplay::GetMaxPrediction()]);
}

bool Netplay::NpOnEventCb(void* ctx, GGPOEvent* ev)
{
  char buff[128];
  std::string msg;
  switch (ev->code)
  {
    case GGPOEventCode::GGPO_EVENTCODE_CONNECTED_TO_PEER:
      sprintf(buff, "Netplay Connected To Player: %d", ev->u.connected.player);
      msg = buff;
      break;
    case GGPOEventCode::GGPO_EVENTCODE_SYNCHRONIZING_WITH_PEER:
      sprintf(buff, "Netplay Synchronzing: %d/%d", ev->u.synchronizing.count, ev->u.synchronizing.total);
      msg = buff;
      break;
    case GGPOEventCode::GGPO_EVENTCODE_SYNCHRONIZED_WITH_PEER:
      sprintf(buff, "Netplay Synchronized With Player: %d", ev->u.synchronized.player);
      msg = buff;
      break;
    case GGPOEventCode::GGPO_EVENTCODE_DISCONNECTED_FROM_PEER:
      sprintf(buff, "Netplay Player: %d Disconnected", ev->u.disconnected.player);
      msg = buff;
      break;
    case GGPOEventCode::GGPO_EVENTCODE_RUNNING:
      msg = "Netplay Is Running";
      break;
    case GGPOEventCode::GGPO_EVENTCODE_CONNECTION_INTERRUPTED:
      sprintf(buff, "Netplay Player: %d Connection Interupted, Timeout: %d", ev->u.connection_interrupted.player,
              ev->u.connection_interrupted.disconnect_timeout);
      msg = buff;
      break;
    case GGPOEventCode::GGPO_EVENTCODE_CONNECTION_RESUMED:
      sprintf(buff, "Netplay Player: %d Connection Resumed", ev->u.connection_resumed.player);
      msg = buff;
      break;
    case GGPOEventCode::GGPO_EVENTCODE_CHAT:
      sprintf(buff, "%s", ev->u.chat.msg);
      msg = buff;
      break;
    case GGPOEventCode::GGPO_EVENTCODE_TIMESYNC:
      Netplay::GetTimer()->OnGGPOTimeSyncEvent(ev->u.timesync.frames_ahead);
      break;
    case GGPOEventCode::GGPO_EVENTCODE_DESYNC:
      sprintf(buff, "Netplay Desync Detected!: Frame: %d, L:%u, R:%u", ev->u.desync.nFrameOfDesync,
              ev->u.desync.ourCheckSum, ev->u.desync.remoteChecksum);
      msg = buff;
      break;
    default:
      sprintf(buff, "Netplay Event Code: %d", ev->code);
      msg = buff;
  }
  if (!msg.empty())
  {
    Host::OnNetplayMessage(msg);
    Log_InfoPrintf("%s", msg.c_str());
  }
  return true;
}

void Netplay::NpFreeBuffCb(void* ctx, void* buffer)
{
  free(buffer);
}