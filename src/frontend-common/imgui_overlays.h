#pragma once

#include "imgui_manager.h"

namespace ImGuiManager {
void RenderOverlays();
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
