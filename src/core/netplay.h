#pragma once

#include "types.h"
#include <string>

namespace Netplay {

enum : s32
{
  // Maximum number of emulated controllers.
  MAX_PLAYERS = 2,

  // Maximum number of spectators allowed to watch the session.
  MAX_SPECTATORS = 4,

  // Maximum netplay prediction frames
  MAX_ROLLBACK_FRAMES = 8,

  // Maximum length of a nickname
  MAX_NICKNAME_LENGTH = 128,

  // Maximum name of password for session
  MAX_SESSION_PASSWORD_LENGTH = 128,
};

enum : u8
{
  ENET_CHANNEL_CONTROL = 0,
  ENET_CHANNEL_GGPO = 1,

  NUM_ENET_CHANNELS,
};

bool CreateSession(std::string nickname, s32 port, s32 max_players, std::string password, int inputdelay, bool traversal);
bool JoinSession(std::string nickname, const std::string& hostname, s32 port, std::string password, bool spectating,
                 int inputdelay, bool traversal, const std::string& hostcode);

bool IsActive();

/// Frees up resources associated with the current netplay session.
/// Should only be called by System::ShutdownSystem().
void SystemDestroyed();

/// Runs the VM and netplay loop. when the netplay loop cancels it switches to normal execute mode.
void ExecuteNetplay();

void CollectInput(u32 slot, u32 bind, float value);

void SendChatMessage(const std::string_view& msg);

s32 GetPing();
u32 GetMaxPrediction();
std::string_view GetHostCode();

/// Updates the throttle period, call when target emulation speed changes.
void UpdateThrottlePeriod();

void ToggleDesyncNotifications();

} // namespace Netplay
