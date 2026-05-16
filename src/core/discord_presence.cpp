// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "discord_presence.h"

#ifdef ENABLE_DISCORD_PRESENCE

#include "achievements.h"
#include "game_database.h"
#include "system.h"

#include "common/dynamic_library.h"
#include "common/error.h"
#include "common/log.h"
#include "common/string_util.h"

#include "discord_rpc.h"

#include <ctime>

LOG_CHANNEL(DiscordPresence);

#define DISCORD_RPC_FUNCTIONS(X)                                                                                       \
  X(Discord_Initialize)                                                                                                \
  X(Discord_Shutdown)                                                                                                  \
  X(Discord_RunCallbacks)                                                                                              \
  X(Discord_UpdatePresence)                                                                                            \
  X(Discord_ClearPresence)

namespace DiscordPresence {

static bool OpenDiscordRPC(Error* error);
static void CloseDiscordRPC();

namespace {
struct Locals
{
  bool discord_presence_active;

  std::time_t session_start_time;
  std::string current_state;
  std::string current_image_url;

  DynamicLibrary rpc_library;

#define ADD_FUNC(F) decltype(&::F) rpc_##F;
  DISCORD_RPC_FUNCTIONS(ADD_FUNC)
#undef ADD_FUNC
};
} // namespace

static Locals s_locals;

} // namespace DiscordPresence

bool DiscordPresence::OpenDiscordRPC(Error* error)
{
  if (s_locals.rpc_library.IsOpen())
    return true;

  if (!s_locals.rpc_library.Open(DynamicLibrary::GetBundledLibraryPath("discord-rpc").c_str(), error))
  {
    Error::AddPrefix(error, "Failed to load discord-rpc: ");
    return false;
  }

  static const DynamicLibrary::SymbolTable symbols[] = {
#define SYMBOL_ENTRY(F) {#F, reinterpret_cast<void**>(&s_locals.rpc_##F)},
    DISCORD_RPC_FUNCTIONS(SYMBOL_ENTRY)
#undef SYMBOL_ENTRY
  };

  if (!s_locals.rpc_library.ResolveSymbols(symbols, std::size(symbols), error))
  {
    CloseDiscordRPC();
    return false;
  }

  return true;
}

void DiscordPresence::CloseDiscordRPC()
{
  if (!s_locals.rpc_library.IsOpen())
    return;

#define UNLOAD_FUNC(F) s_locals.rpc_##F = nullptr;
  DISCORD_RPC_FUNCTIONS(UNLOAD_FUNC)
#undef UNLOAD_FUNC

  s_locals.rpc_library.Close();
}

bool DiscordPresence::Initialize()
{
  if (s_locals.discord_presence_active)
    return true;

  Error error;
  if (!OpenDiscordRPC(&error))
  {
    ERROR_LOG("Failed to open discord-rpc: {}", error.GetDescription());
    return false;
  }

  DiscordEventHandlers handlers = {};
  s_locals.rpc_Discord_Initialize("705325712680288296", &handlers, 0, nullptr);
  s_locals.discord_presence_active = true;
  s_locals.session_start_time = std::time(nullptr);

  INFO_LOG("Discord Rich Presence initialized successfully");

  if (const auto lock = Achievements::GetLock(); Achievements::HasActiveGame())
    UpdateDetails(Achievements::GetCurrentGameBadgeURL(), Achievements::GetRichPresenceString());

  Update(true);
  return true;
}

void DiscordPresence::Shutdown()
{
  if (!s_locals.discord_presence_active)
    return;

  INFO_LOG("Shutting down Discord Rich Presence...");

  s_locals.discord_presence_active = false;

  s_locals.rpc_Discord_ClearPresence();
  s_locals.rpc_Discord_Shutdown();
  CloseDiscordRPC();
}

void DiscordPresence::Poll()
{
  if (!s_locals.discord_presence_active)
    return;

  s_locals.rpc_Discord_RunCallbacks();
}

void DiscordPresence::Update(bool is_new_session)
{
  if (!s_locals.discord_presence_active)
    return;

  if (is_new_session)
    s_locals.session_start_time = std::time(nullptr);

  // https://discord.com/developers/docs/rich-presence/how-to#updating-presence-update-presence-payload-fields
  DiscordRichPresence rp = {};
  rp.largeImageKey = s_locals.current_image_url.empty() ? "duckstation_logo" : s_locals.current_image_url.c_str();
  rp.largeImageText = "DuckStation PS1/PSX Emulator";
  rp.startTimestamp = s_locals.session_start_time;

  TinyString game_details("No Game Running");
  if (System::IsValidOrInitializing())
  {
    // Use disc set name if it's not a custom title.
    const bool custom_title = System::IsRunningGameCustomTitle();
    const GameDatabase::Entry* game_entry = System::GetGameDatabaseEntry();
    if (!custom_title && game_entry && game_entry->disc_set)
    {
      game_details = game_entry->disc_set->GetDisplayTitle(true);
    }
    else
    {
      if (const std::string& game_title = System::GetGameTitle(); !game_title.empty())
        game_details = game_title;
      else
        game_details = "Unknown Game";
    }
  }
  rp.details = game_details.c_str();
  rp.state = s_locals.current_state.empty() ? nullptr : s_locals.current_state.c_str();

  INFO_LOG("Updating: '{}' state='{}', badge='{}'", game_details, s_locals.current_state, rp.largeImageKey);

  s_locals.rpc_Discord_UpdatePresence(&rp);
}

void DiscordPresence::UpdateDetails(std::string_view badge_url, std::string_view state)
{
  if (badge_url.empty())
    s_locals.current_image_url = {};
  else if (s_locals.current_image_url != badge_url)
    s_locals.current_image_url = badge_url;

  if (state.empty())
    s_locals.current_state = {};
  else
    s_locals.current_state = StringUtil::Ellipsise(state, 128);

  Update(false);
}

#endif // ENABLE_DISCORD_PRESENCE