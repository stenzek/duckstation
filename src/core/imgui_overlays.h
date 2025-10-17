// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "util/imgui_manager.h"

#include <string>

class SettingsInterface;

class GPUBackend;

namespace ImGuiManager {

inline constexpr const char* LOGO_IMAGE_NAME = "images/duck.png";

void UpdateInputOverlay();
void RenderTextOverlays(const GPUBackend* gpu);
bool AreAnyDebugWindowsEnabled(const SettingsInterface& si);
bool IsSPUDebugWindowEnabled();
void RenderDebugWindows();
bool UpdateDebugWindowConfig();
void DestroyAllDebugWindows();

void RenderOverlayWindows();
void DestroyOverlayTextures();
void RenderRewindSelector();

} // namespace ImGuiManager

namespace SaveStateSelectorUI {

inline constexpr float DEFAULT_OPEN_TIME = 7.5f;

bool IsOpen();
void Open(float open_time = DEFAULT_OPEN_TIME);
void RefreshList();
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
