// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

class Error;
struct WindowInfo;

namespace CocoaTools {
/// Creates metal layer on specified window surface.
void* CreateMetalLayer(const WindowInfo& wi, Error* error);

/// Destroys metal layer on specified window surface.
void DestroyMetalLayer(const WindowInfo& wi, void* layer);
} // namespace CocoaTools
