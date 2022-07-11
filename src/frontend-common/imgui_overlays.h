#pragma once

#include "imgui_manager.h"

namespace ImGuiManager {
void RenderOverlays();
}

namespace SaveStateSelectorUI {

static constexpr float DEFAULT_OPEN_TIME = 5.0f;

void Open(float open_time = DEFAULT_OPEN_TIME);
void Close();

void SelectNextSlot();
void SelectPreviousSlot();

void LoadCurrentSlot();
void SaveCurrentSlot();

} // namespace SaveStateSelectorUI
