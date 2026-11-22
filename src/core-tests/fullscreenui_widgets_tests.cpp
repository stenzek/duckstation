// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "core/fullscreenui_widgets.h"

#include "dyn_freetype.h"
#include "dyn_plutosvg.h"

#include "common/file_system.h"
#include "common/path.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <gtest/gtest.h>

namespace {

class FullscreenUIWidgets : public ::testing::Test
{
protected:
  static constexpr float DISPLAY_WIDTH = 1280.0f;
  static constexpr float DISPLAY_HEIGHT = 720.0f;
  static constexpr float PAGE_TOP = 100.0f;
  static constexpr float SEARCH_LEFT = 480.0f;
  static constexpr float SEARCH_WIDTH = 320.0f;
  static constexpr float EMPTY_HEADER_X = 900.0f;
  static constexpr float SEGMENTS_LEFT = 1000.0f;

  void SetUp() override
  {
    if (!g_dyn_freetype.Open(nullptr) || !g_dyn_plutosvg.Open(nullptr))
      GTEST_SKIP() << "FreeType is not available";

    // resources are copied next to the executable
    const std::string font_path = Path::Combine(Path::GetDirectory(FileSystem::GetProgramPath(nullptr)),
                                                "resources/fonts/Manrope-VariableFont_wght.ttf");
    std::optional<DynamicHeapArray<u8>> font_data = FileSystem::ReadBinaryFile(font_path.c_str());
    ASSERT_TRUE(font_data.has_value()) << font_path;

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigInputTrickleEventQueue = false;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

    // the atlas takes ownership
    const int font_size = static_cast<int>(font_data->size());
    void* const font_copy = IM_ALLOC(font_data->size());
    std::memcpy(font_copy, font_data->data(), font_data->size());

    using FullscreenUI::UIStyle;
    UIStyle.Font = io.Fonts->AddFontFromMemoryTTF(font_copy, font_size, FullscreenUI::LAYOUT_MEDIUM_FONT_SIZE);
    UIStyle.LayoutScale = 1.0f;
    UIStyle.RcpLayoutScale = 1.0f;
    UIStyle.LargeFontSize = FullscreenUI::LAYOUT_LARGE_FONT_SIZE;
    UIStyle.MediumFontSize = FullscreenUI::LAYOUT_MEDIUM_FONT_SIZE;
    UIStyle.MediumLargeFontSize = FullscreenUI::LAYOUT_MEDIUM_LARGE_FONT_SIZE;
    UIStyle.MediumSmallFontSize = FullscreenUI::LAYOUT_MEDIUM_SMALL_FONT_SIZE;
    UIStyle.TitleText = {UIStyle.MediumFontSize, UIStyle.BoldFontWeight, ImVec4(1.0f, 1.0f, 1.0f, 1.0f)};
    UIStyle.MutedText = {UIStyle.MediumFontSize, UIStyle.NormalFontWeight, ImVec4(1.0f, 1.0f, 1.0f, 0.6f)};
    m_has_context = true;

    // one frame to create the windows, then select the second game like a user navigating the page
    Frame();
    ImGui::FocusWindow(m_entries_window);
    Frame();
    ImGui::SetFocusID(m_game_id, m_entries_window);
    Frame();
    ASSERT_TRUE(IsPageFocused());
  }

  void TearDown() override
  {
    if (m_has_context)
      ImGui::DestroyContext();
    FullscreenUI::UIStyle.Font = nullptr;
  }

  /// Draws the header as the game list does, followed by a page with a child of items below it.
  void Frame()
  {
    using namespace FullscreenUI;

    ImGui::NewFrame();

    if (BeginPageHeader("header", ImVec4(0.0f, 0.0f, 0.0f, 1.0f), false))
    {
      m_back_pressed |= PageHeaderBackButton();
      PageHeaderTitle("Library");

      ImGuiWindow* const window = ImGui::GetCurrentWindow();
      ImGui::NewLine();
      ImGui::SetCursorScreenPos(ImVec2(SEGMENTS_LEFT, window->Pos.y + LayoutScale(8.0f)));
      BeginSegmentedControl("view");
      for (u32 i = 0; i < 2; i++)
      {
        m_segment_clicked |= Segment(i ? "Grid" : "List", i == 0);
        m_segment_rects[i] = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
      }
      EndSegmentedControl();
      m_segmented_control_rect = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

      ImGui::SetCursorScreenPos(ImVec2(SEARCH_LEFT, window->Pos.y + LayoutScale(8.0f)));
      ImGui::SetNextItemWidth(SEARCH_WIDTH);
      ImGui::InputText("##search", m_search, sizeof(m_search),
                       ImGui::IsKeyPressed(ImGuiKey_F3, false) ? ImGuiInputTextFlags_AlwaysActivate : 0);

      m_back_center = window->Pos + ImVec2(LayoutScale(LAYOUT_PAGE_X_PADDING) + LayoutScale(17.0f),
                                           LayoutScale(LAYOUT_PAGE_HEADER_HEIGHT) * 0.5f);
    }
    EndPageHeader();

    ImGui::SetNextWindowPos(ImVec2(0.0f, PAGE_TOP));
    ImGui::SetNextWindowSize(ImVec2(DISPLAY_WIDTH, DISPLAY_HEIGHT - PAGE_TOP));
    ImGui::Begin("page", nullptr, ImGuiWindowFlags_NoDecoration);
    ImGui::BeginChild("entries", ImVec2(750.0f, 0.0f), ImGuiChildFlags_NavFlattened | ImGuiChildFlags_NoNavCancel);
    m_entries_window = ImGui::GetCurrentWindow();
    ImGui::Button("Game 1");
    ImGui::Button("Game 2");
    m_game_id = ImGui::GetItemID();
    ImGui::EndChild();
    ImGui::End();

    ImGui::EndFrame();
  }

  void Click(const ImVec2& press_pos, const ImVec2& release_pos)
  {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(press_pos.x, press_pos.y);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    Frame();
    io.AddMousePosEvent(release_pos.x, release_pos.y);
    Frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    Frame();
    Frame();
  }

  bool IsPageFocused() const
  {
    const ImGuiWindow* const nav_window = GImGui->NavWindow;
    return (nav_window == m_entries_window && GImGui->NavId == m_game_id);
  }

  ImGuiWindow* m_entries_window = nullptr;
  ImGuiID m_game_id = 0;
  char m_search[64] = {};
  ImRect m_segment_rects[2];
  ImRect m_segmented_control_rect;
  ImVec2 m_back_center;
  bool m_back_pressed = false;
  bool m_segment_clicked = false;
  bool m_has_context = false;
};

} // namespace

TEST_F(FullscreenUIWidgets, SegmentsShareLineAfterBackButton)
{
  EXPECT_EQ(m_segment_rects[0].Min.y, m_segment_rects[1].Min.y);
  EXPECT_EQ(m_segment_rects[0].GetHeight(), FullscreenUI::LayoutScale(FullscreenUI::SEGMENTED_CONTROL_HEIGHT));

  const float frame_width = m_segment_rects[0].GetWidth() + m_segment_rects[1].GetWidth();
  const ImVec2 frame_size = FullscreenUI::GetSegmentedControlFrameSize(frame_width);
  EXPECT_EQ(m_segmented_control_rect.GetWidth(), frame_size.x);
  EXPECT_EQ(m_segmented_control_rect.GetHeight(), frame_size.y);
}

TEST_F(FullscreenUIWidgets, ClickingActiveSegmentKeepsPageFocus)
{
  const ImVec2 pos = m_segment_rects[0].GetCenter();
  Click(pos, pos);
  EXPECT_FALSE(m_segment_clicked);
  EXPECT_TRUE(IsPageFocused());
}

TEST_F(FullscreenUIWidgets, ClickingInactiveSegmentKeepsPageFocus)
{
  const ImVec2 pos = m_segment_rects[1].GetCenter();
  Click(pos, pos);
  EXPECT_TRUE(m_segment_clicked);
  EXPECT_TRUE(IsPageFocused());
}

TEST_F(FullscreenUIWidgets, BackButtonReleasedOutsideKeepsPageFocus)
{
  Click(m_back_center, ImVec2(EMPTY_HEADER_X, m_back_center.y));
  EXPECT_FALSE(m_back_pressed);
  EXPECT_TRUE(IsPageFocused());
}

TEST_F(FullscreenUIWidgets, BackButtonClickKeepsPageFocus)
{
  Click(m_back_center, m_back_center);
  EXPECT_TRUE(m_back_pressed);
  EXPECT_TRUE(IsPageFocused());
}

TEST_F(FullscreenUIWidgets, HeaderBackgroundClickKeepsPageFocus)
{
  const ImVec2 pos = ImVec2(EMPTY_HEADER_X, m_back_center.y);
  Click(pos, pos);
  EXPECT_TRUE(IsPageFocused());
}

TEST_F(FullscreenUIWidgets, SearchKeepsFocusWhileTyping)
{
  ImGuiIO& io = ImGui::GetIO();
  io.AddKeyEvent(ImGuiKey_F3, true);
  Frame();
  io.AddKeyEvent(ImGuiKey_F3, false);
  Frame();
  io.AddInputCharactersUTF8("Tekken");
  Frame();
  Frame();
  EXPECT_STREQ(m_search, "Tekken");
  EXPECT_NE(GImGui->ActiveId, 0u);
}
