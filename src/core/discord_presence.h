// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/types.h"

#include <string_view>

// Discord presence is available on all platforms except Android.
#ifndef __ANDROID__
#define ENABLE_DISCORD_PRESENCE
#endif

#ifdef ENABLE_DISCORD_PRESENCE

namespace DiscordPresence {

bool Initialize();
void Shutdown();
void Poll();

/// Called when rich presence changes.
void Update(bool is_new_session);

void UpdateDetails(std::string_view badge_url, std::string_view state);

} // namespace DiscordPresence

#endif // ENABLE_DISCORD_PRESENCE
