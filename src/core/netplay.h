#pragma once

#include "types.h"
#include <string>

namespace Netplay {

enum : s32
{
  // Maximum number of emulated controllers.
  MAX_PLAYERS = 2,
  // Maximum netplay prediction frames
  MAX_ROLLBACK_FRAMES = 8,
};

enum : u8
{
  ENET_CHANNEL_CONTROL = 0,
  ENET_CHANNEL_GGPO = 1,
  ENET_CHANNEL_SESSION = 2,

  NUM_ENET_CHANNELS,
};

void StartNetplaySession(s32 local_handle, u16 local_port, std::string& remote_addr, u16 remote_port, s32 input_delay,
                         std::string game_path);
void StopNetplaySession();

bool IsActive();

/// Frees up resources associated with the current netplay session.
/// Should only be called by System::ShutdownSystem().
void CloseSession();

/// Runs the VM and netplay loop. when the netplay loop cancels it switches to normal execute mode.
void ExecuteNetplay();

void CollectInput(u32 slot, u32 bind, float value);

void SendMsg(std::string msg);

s32 GetPing();
u32 GetMaxPrediction();

/// Updates the throttle period, call when target emulation speed changes.
void UpdateThrottlePeriod();

} // namespace Netplay
