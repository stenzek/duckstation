#pragma once

#ifndef _NETPLAY_H
#define _NETPLAY_H

#include <array>
#include <ggponet.h>
#include <stdint.h>
#include <string.h>
#include <string>

#include "common/timer.h"
#include "digital_controller.h"
#include "types.h"

// C GGPO Event Callbacks. Should be defined in system.cpp
extern "C" {
bool NpAdvFrameCb(void* ctx, int flags);
bool NpSaveFrameCb(void* ctx, uint8_t** buffer, int* len, int* checksum, int frame);
bool NpLoadFrameCb(void* ctx, uint8_t* buffer, int len, int rb_frames, int frame_to_load);
bool NpBeginGameCb(void* ctx, const char* game_name);
void NpFreeBuffCb(void* ctx, void* buffer);
bool NpOnEventCb(void* ctx, GGPOEvent* ev);
}

namespace Netplay {

struct Input
{
  uint32_t button_data;
};

struct LoopTimer
{
public:
  void Init(uint32_t fps, uint32_t frames_to_spread_wait);
  void OnGGPOTimeSyncEvent(float frames_ahead);
  // Call every loop, to get the amount of time the current iteration of gameloop should take
  int32_t UsToWaitThisLoop();

private:
  float m_last_advantage = 0.0f;
  int32_t m_us_per_game_loop = 0;
  int32_t m_us_ahead = 0;
  int32_t m_us_extra_to_wait = 0;
  int32_t m_frames_to_spread_wait = 0;
  int32_t m_wait_count = 0;
};

class Session
{
public:
  Session();
  ~Session();
  // l = local, r = remote
  static int32_t Start(int32_t lhandle, uint16_t lport, std::string& raddr, uint16_t rport, int32_t ldelay,
                       uint32_t pred);

  static void Close();
  static bool IsActive();
  static void RunIdle();
 
  static void AdvanceFrame(uint16_t checksum = 0);
  static void RunFrame(int32_t& waitTime);
  static int32_t CurrentFrame();

  static void CollectInput(uint32_t slot, uint32_t bind, float value);
  static Netplay::Input ReadLocalInput();

  static std::string& GetGamePath();
  static void SetGamePath(std::string& path);
  static void SendMsg(const char* msg);

  static GGPOErrorCode SyncInput(Netplay::Input inputs[2], int* disconnect_flags);
  static GGPOErrorCode AddLocalInput(Netplay::Input input);
  static GGPONetworkStats& GetNetStats(int32_t handle);
  static int32_t GetPing();
  static uint32_t GetMaxPrediction();
  static GGPOPlayerHandle GetLocalHandle();
  static void SetInputs(Netplay::Input inputs[2]);

  static Netplay::LoopTimer* GetTimer();
  static uint16_t Fletcher16(uint8_t* data, int count);

private:
  Netplay::LoopTimer m_timer;
  std::string m_game_path;
  uint32_t m_max_pred = 0;

  GGPOPlayerHandle m_local_handle = GGPO_INVALID_HANDLE;
  GGPONetworkStats m_last_net_stats{};
  GGPOSession* p_ggpo = nullptr;

  std::array<std::array<float, 32>, NUM_CONTROLLER_AND_CARD_PORTS> m_net_input;
};

} // namespace Netplay

// Netplay Instance
static Netplay::Session s_net_session = Netplay::Session();

#endif // !_NETPLAY_H
