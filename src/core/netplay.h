#pragma once

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
bool NpSaveFrameCb(void* ctx, unsigned char** buffer, int* len, int* checksum, int frame);
bool NpLoadFrameCb(void* ctx, unsigned char* buffer, int len, int rb_frames, int frame_to_load);
bool NpBeginGameCb(void* ctx, const char* game_name);
void NpFreeBuffCb(void* ctx, void* buffer);
bool NpOnEventCb(void* ctx, GGPOEvent* ev);
}

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

// l = local, r = remote
s32 Start(s32 lhandle, u16 lport, std::string& raddr, u16 rport, s32 ldelay, u32 pred);

void Close();
bool IsActive();
void RunIdle();

void AdvanceFrame(u16 checksum = 0);
void RunFrame(s32& waitTime);
s32 CurrentFrame();

void CollectInput(u32 slot, u32 bind, float value);
Netplay::Input ReadLocalInput();

std::string& GetGamePath();
void SetGamePath(std::string& path);
void SendMsg(const char* msg);

GGPOErrorCode SyncInput(Netplay::Input inputs[2], int* disconnect_flags);
GGPOErrorCode AddLocalInput(Netplay::Input input);
GGPONetworkStats& GetNetStats(s32 handle);
s32 GetPing();
u32 GetMaxPrediction();
GGPOPlayerHandle GetLocalHandle();
void SetInputs(Netplay::Input inputs[2]);

Netplay::LoopTimer* GetTimer();
u16 Fletcher16(uint8_t* data, int count);
} // namespace Netplay
