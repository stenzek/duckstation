#include "netplay.h"
#include "pad.h"
#include "spu.h"
#include "system.h"
#include <bitset>

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#endif

Netplay::LoopTimer s_timer;
std::string s_game_path;
u32 s_max_pred = 0;

GGPOPlayerHandle s_local_handle = GGPO_INVALID_HANDLE;
GGPONetworkStats s_last_net_stats{};
GGPOSession* s_ggpo = nullptr;

std::array<std::array<float, 32>, NUM_CONTROLLER_AND_CARD_PORTS> s_net_input;

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
  if (GetLocalHandle() != GGPO_INVALID_HANDLE)
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
      System::NetplayAdvanceFrame(inputs, disconnectFlags);
    }
    else
      RunIdle();
  }
  else
    RunIdle();

  waitTime = GetTimer()->UsToWaitThisLoop();
}

s32 Netplay::CurrentFrame()
{
  s32 frame;
  ggpo_get_current_frame(s_ggpo, frame);
  return frame;
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

std::string& Netplay::GetGamePath()
{
  return s_game_path;
}

void Netplay::SetGamePath(std::string& path)
{
  s_game_path = path;
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

GGPONetworkStats& Netplay::GetNetStats(s32 handle)
{
  ggpo_get_network_stats(s_ggpo, handle, &s_last_net_stats);
  return s_last_net_stats;
}

s32 Netplay::GetPing()
{
  const int handle = GetLocalHandle() == 1 ? 2 : 1;
  ggpo_get_network_stats(s_ggpo, handle, &s_last_net_stats);
  return s_last_net_stats.network.ping;
}

u32 Netplay::GetMaxPrediction()
{
  return s_max_pred;
}

GGPOPlayerHandle Netplay::GetLocalHandle()
{
  return s_local_handle;
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

u16 Netplay::Fletcher16(uint8_t* data, int count)
{
  u16 sum1 = 0;
  u16 sum2 = 0;
  int index;

  for (index = 0; index < count; ++index)
  {
    sum1 = (sum1 + data[index]) % 255;
    sum2 = (sum2 + sum1) % 255;
  }

  return (sum2 << 8) | sum1;
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
