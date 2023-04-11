#include "netplay.h"
#include "pad.h"
#include "spu.h"
#include "system.h"
#include <bitset>

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#endif

// Netplay Impl
Netplay::Session::Session() = default;

Netplay::Session::~Session()
{
  Close();
}

int32_t Netplay::Session::Start(int32_t lhandle, uint16_t lport, std::string& raddr, uint16_t rport, int32_t ldelay,
                                uint32_t pred)
{
  s_net_session.m_max_pred = pred;
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

  result = ggpo_start_session(&s_net_session.p_ggpo, &cb, "Duckstation-Netplay", 2, sizeof(Netplay::Input), lport,
                              s_net_session.m_max_pred);

  ggpo_set_disconnect_timeout(s_net_session.p_ggpo, 3000);
  ggpo_set_disconnect_notify_start(s_net_session.p_ggpo, 1000);

  for (int i = 1; i <= 2; i++)
  {
    GGPOPlayer player = {};
    GGPOPlayerHandle handle = 0;

    player.size = sizeof(GGPOPlayer);
    player.player_num = i;

    if (lhandle == i)
    {
      player.type = GGPOPlayerType::GGPO_PLAYERTYPE_LOCAL;
      result = ggpo_add_player(s_net_session.p_ggpo, &player, &handle);
      s_net_session.m_local_handle = handle;
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
      result = ggpo_add_player(s_net_session.p_ggpo, &player, &handle);
    }
  }
  ggpo_set_frame_delay(s_net_session.p_ggpo, s_net_session.m_local_handle, ldelay);

  return result;
}

void Netplay::Session::Close()
{
  ggpo_close_session(s_net_session.p_ggpo);
  s_net_session.p_ggpo = nullptr;
  s_net_session.m_local_handle = GGPO_INVALID_HANDLE;
  s_net_session.m_max_pred = 0;
}

bool Netplay::Session::IsActive()
{
  return s_net_session.p_ggpo != nullptr;
}

void Netplay::Session::RunIdle()
{
  ggpo_idle(s_net_session.p_ggpo);
}

void Netplay::Session::AdvanceFrame(uint16_t checksum)
{
  ggpo_advance_frame(s_net_session.p_ggpo, checksum);
}

void Netplay::Session::RunFrame(int32_t& waitTime)
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
      System::NetplayAdvanceFrame (inputs, disconnectFlags);
    }
    else
      RunIdle();
  }
  else
    RunIdle();

  waitTime = GetTimer()->UsToWaitThisLoop();
}

int32_t Netplay::Session::CurrentFrame()
{
  int32_t frame;
  ggpo_get_current_frame(s_net_session.p_ggpo, frame);
  return frame;
}

void Netplay::Session::CollectInput(uint32_t slot, uint32_t bind, float value)
{
  s_net_session.m_net_input[slot][bind] = value;
}

Netplay::Input Netplay::Session::ReadLocalInput()
{
  // get controller data of the first controller (0 internally)
  Netplay::Input inp{0};
  for (uint32_t i = 0; i < (uint32_t)DigitalController::Button::Count; i++)
  {
    if (s_net_session.m_net_input[0][i] >= 0.25f)
      inp.button_data |= 1 << i;
  }
  return inp;
}

std::string& Netplay::Session::GetGamePath()
{
  return s_net_session.m_game_path;
}

void Netplay::Session::SetGamePath(std::string& path)
{
  s_net_session.m_game_path = path;
}

void Netplay::Session::SendMsg(const char* msg)
{
  ggpo_client_chat(s_net_session.p_ggpo, msg);
}

GGPOErrorCode Netplay::Session::SyncInput(Netplay::Input inputs[2], int* disconnect_flags)
{
  return ggpo_synchronize_input(s_net_session.p_ggpo, inputs, sizeof(Netplay::Input) * 2, disconnect_flags);
}

GGPOErrorCode Netplay::Session::AddLocalInput(Netplay::Input input)
{
  return ggpo_add_local_input(s_net_session.p_ggpo, s_net_session.m_local_handle, &input, sizeof(Netplay::Input));
}

GGPONetworkStats& Netplay::Session::GetNetStats(int32_t handle)
{
  ggpo_get_network_stats(s_net_session.p_ggpo, handle, &s_net_session.m_last_net_stats);
  return s_net_session.m_last_net_stats;
}

int32_t Netplay::Session::GetPing()
{
  const int handle = GetLocalHandle() == 1 ? 2 : 1;
  ggpo_get_network_stats(s_net_session.p_ggpo, handle, &s_net_session.m_last_net_stats);
  return s_net_session.m_last_net_stats.network.ping;
}

uint32_t Netplay::Session::GetMaxPrediction()
{
  return s_net_session.m_max_pred;
}

GGPOPlayerHandle Netplay::Session::GetLocalHandle()
{
  return s_net_session.m_local_handle;
}

void Netplay::Session::SetInputs(Netplay::Input inputs[2])
{
  for (u32 i = 0; i < 2; i++)
  {
    auto cont = Pad::GetController(i);
    std::bitset<sizeof(u32) * 8> buttonBits(inputs[i].button_data);
    for (u32 j = 0; j < (u32)DigitalController::Button::Count; j++)
      cont->SetBindState(j, buttonBits.test(j) ? 1.0f : 0.0f);
  }
}

Netplay::LoopTimer* Netplay::Session::GetTimer()
{
  return &s_net_session.m_timer;
}

uint16_t Netplay::Session::Fletcher16(uint8_t* data, int count)
{
  uint16_t sum1 = 0;
  uint16_t sum2 = 0;
  int index;

  for (index = 0; index < count; ++index)
  {
    sum1 = (sum1 + data[index]) % 255;
    sum2 = (sum2 + sum1) % 255;
  }

  return (sum2 << 8) | sum1;
}

void Netplay::LoopTimer::Init(uint32_t fps, uint32_t frames_to_spread_wait)
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

int32_t Netplay::LoopTimer::UsToWaitThisLoop()
{
  int32_t timetoWait = m_us_per_game_loop;
  if (m_wait_count)
  {
    timetoWait += m_us_extra_to_wait;
    m_wait_count--;
    if (!m_wait_count)
      m_us_extra_to_wait = 0;
  }
  return timetoWait;
}
