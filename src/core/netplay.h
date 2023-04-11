#pragma once

#include "types.h"
#include <string>

namespace Netplay {

enum : u32
{
  // Maximum number of emulated controllers.
  MAX_PLAYERS = 2,
};

void StartNetplaySession(s32 local_handle, u16 local_port, std::string& remote_addr, u16 remote_port, s32 input_delay,
                         std::string game_path);
void StopNetplaySession();

bool IsActive();

/// Runs the VM and netplay loop. when the netplay loop cancels it switches to normal execute mode.
void ExecuteNetplay();

void CollectInput(u32 slot, u32 bind, float value);

void SendMsg(const char* msg);

s32 GetPing();
u32 GetMaxPrediction();

} // namespace Netplay
