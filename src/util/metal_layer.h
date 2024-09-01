// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

struct WindowInfo;

namespace CocoaTools {
/// Creates metal layer on specified window surface.
bool CreateMetalLayer(WindowInfo* wi);

/// Destroys metal layer on specified window surface.
void DestroyMetalLayer(WindowInfo* wi);
} // namespace CocoaTools
