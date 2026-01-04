// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include <string_view>

namespace SoundEffectManager {

/// Returns true if a stream has been created.
bool IsInitialized();

/// Ensures the audio effect manager has been created.
void EnsureInitialized();

/// Closes the audio effect manager.
void Shutdown();

/// Asynchronously queues an audio effect.
/// The path is assumed to be a resource name.
/// This effect will be cached to avoid disk I/O.
void EnqueueSoundEffect(std::string_view name);

/// Streams a WAV file for playback.
/// This will bypass the cache and read directly from disk.
void StreamSoundEffect(std::string path);

/// Stops all currently playing sound effects.
void StopAll();

/// Returns true if any sound effects are currently playing.
bool HasAnyActiveEffects();

} // namespace SoundEffectManager
