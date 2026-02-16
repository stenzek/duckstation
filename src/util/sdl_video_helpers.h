// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "gpu_device.h"

#include "common/error.h"
#include "common/log.h"

#include <SDL3/SDL.h>

#include <algorithm>

namespace SDLVideoHelpers {

inline std::vector<GPUDevice::ExclusiveFullscreenMode> GetFullscreenModeList()
{
  int display_count = 0;
  const SDL_DisplayID* const displays = SDL_GetDisplays(&display_count);
  if (display_count <= 0)
  {
    GENERIC_LOG(Log::Channel::SDL, Log::Level::Error, Log::Color::Default, "SDL_GetDisplays() returned no displays: {}",
                SDL_GetError());
    return {};
  }

  std::vector<GPUDevice::ExclusiveFullscreenMode> modes;
  for (int i = 0; i < display_count; i++)
  {
    int dm_count = 0;
    const SDL_DisplayMode* const* const dms = SDL_GetFullscreenDisplayModes(displays[i], &dm_count);
    if (dm_count <= 0)
    {
      GENERIC_LOG(Log::Channel::SDL, Log::Level::Error, Log::Color::Default,
                  "SDL_GetFullscreenDisplayModes() returned no modes for display {}: {}", displays[i], SDL_GetError());
      continue;
    }

    modes.reserve(modes.size() + static_cast<size_t>(dm_count));

    for (int j = 0; j < dm_count; j++)
    {
      const SDL_DisplayMode* const dm = dms[j];
      const GPUDevice::ExclusiveFullscreenMode mode{static_cast<u32>(dm->w), static_cast<u32>(dm->h), dm->refresh_rate};
      if (std::ranges::find(modes, mode) == modes.end())
        modes.push_back(mode);
    }
  }

  return modes;
}

} // namespace SDLVideoHelpers
