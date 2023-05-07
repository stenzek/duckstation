// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "imgui_manager.h"

namespace ImGuiManager {
void RenderTextOverlays();
void RenderOverlayWindows();

void RenderNetplayOverlays();
void OpenNetplayChat();
}

namespace SaveStateSelectorUI {

static constexpr float DEFAULT_OPEN_TIME = 5.0f;

void Open(float open_time = DEFAULT_OPEN_TIME);
void RefreshList();
void DestroyTextures();
void Close(bool reset_slot = false);

void SelectNextSlot();
void SelectPreviousSlot();

void LoadCurrentSlot();
void SaveCurrentSlot();

} // namespace SaveStateSelectorUI
