// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "util/imgui_manager.h"

#include <string>

namespace ImGuiManager {
void RenderTextOverlays();
void RenderDebugWindows();

void RenderOverlayWindows();
void DestroyOverlayTextures();
} // namespace ImGuiManager

namespace SaveStateSelectorUI {

static constexpr float DEFAULT_OPEN_TIME = 7.5f;

bool IsOpen();
void Open(float open_time = DEFAULT_OPEN_TIME);
void RefreshList(const std::string& serial);
void Clear();
void ClearList();
void Close();

void SelectNextSlot(bool open_selector);
void SelectPreviousSlot(bool open_selector);

s32 GetCurrentSlot();
bool IsCurrentSlotGlobal();
void LoadCurrentSlot();
void SaveCurrentSlot();

} // namespace SaveStateSelectorUI
