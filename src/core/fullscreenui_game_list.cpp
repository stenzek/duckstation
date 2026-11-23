// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "achievements.h"
#include "core.h"
#include "fullscreenui_private.h"
#include "game_list.h"
#include "host.h"
#include "system.h"
#include "video_thread.h"

#include "util/gpu_device.h"
#include "util/gpu_texture.h"
#include "util/imgui_manager.h"

#include "common/bitutils.h"
#include "common/easing.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"
#include "common/threading.h"
#include "common/time_helpers.h"

#include "IconsEmoji.h"
#include "IconsPromptFont.h"

#include <cmath>
#include <cstring>
#include <numbers>

LOG_CHANNEL(FullscreenUI);

namespace FullscreenUI {

static void DrawGameList(const ImVec2& heading_size);
static void DrawGameGrid(const ImVec2& heading_size);
static void DrawGameCarousel(const ImVec2& heading_size);
static void DrawCarouselCoverCallback(const ImDrawList* parent_list, const ImDrawCmd* cmd, u32 base_vertex,
                                      u32 base_index);
static void SetGameListView(GameListView view);
static void SetGameListSortColumn(GameList::Column column);
static void HandleGameListActivate(const GameList::Entry* entry);
static void HandleGameListOptions(const GameList::Entry* entry);
static void HandleSelectDiscForDiscSet(const GameDatabase::DiscSetEntry* dsentry);
static void PopulateGameListEntryList();
static std::string_view GetKeyForGameListEntry(const GameList::Entry* entry);
static GPUTexture* GetTextureForGameListEntryType(GameList::EntryType type);
static GPUTexture* GetGameListCover(const GameList::Entry* entry, bool fallback_to_achievements_icon,
                                    bool fallback_to_icon, bool return_default_image);
static GPUTexture* GetGameListIcon(const GameList::Entry* entry);
static GPUTexture* GetGameListCoverTrophy(const GameList::Entry* entry, const ImVec2& image_size);
static void DrawGameListCover(const GameList::Entry* entry, bool fallback_to_achievements_icon, bool fallback_to_icon,
                              bool icon_style, bool show_localized_titles, ImDrawList* dl, const ImRect& rect);
static GPUTexture* GetGeneratedArtwork(std::string_view title, GameList::EntryType type, bool icon_style, u32 tex_size);
static void DoSetCoverImage(std::string entry_path);
static void DoSetCoverImage(std::string source_path, std::string existing_path, std::string new_path);

namespace {

struct CarouselPushConstants
{
  GSMatrix4x4 mvp;
  float color[4];
  float fade_start;
  float fade_scale;
  float corner_radius[2];
  float flip_y;
};

struct CarouselDrawData
{
  CarouselPushConstants push_constants;
  GPUTexture* texture;
};

struct GameListLocals
{
  // Lazily populated cover images.
  std::unordered_map<std::string, std::string> cover_image_map;
  std::unordered_map<std::string, std::string> icon_image_map;
  std::vector<const GameList::Entry*> game_list_sorted_entries;
  GameList::Column sort_column = GameList::DEFAULT_SORT_COLUMN;
  GameList::IconSource icon_source = GameList::DEFAULT_ICON_SOURCE;
  bool sort_reversed = GameList::DEFAULT_SORT_REVERSED;
  bool localized_titles = GameList::DEFAULT_LOCALIZED_TITLES;
  bool merge_disc_sets = GameList::DEFAULT_MERGE_DISC_SETS;
  GameListView view = DEFAULT_GAME_LIST_VIEW;
  bool carousel_caught = false;
  s32 carousel_index = 0;
  float carousel_position = 0.0f;
  float carousel_animation_start = 0.0f;
  float carousel_animation_time = 0.0f;
  s32 carousel_animation_target = 0;
  float carousel_wheel_accum = 0.0f;
  bool cover_titles = GameList::DEFAULT_COVER_TITLES;
  bool compact_mode = true;
  bool show_trophy_icons = true;
  float game_list_current_selection_timeout = 0.0f;
  std::string game_list_current_selection_path;
  char game_list_search_string[256];
  float search_expand_progress = 0.0f; // linear 0..1, eased when drawn
};

} // namespace

ALIGN_TO_CACHE_LINE static GameListLocals s_game_list_locals;

} // namespace FullscreenUI

FullscreenUI::FileSelectorFilters FullscreenUI::GetDiscImageFilters()
{
  return {"*.bin", "*.cue", "*.iso",   "*.img",    "*.chd", "*.ecm", "*.mds", "*.pbp",     "*.ccd",
          "*.cpe", "*.elf", "*.psexe", "*.ps-exe", "*.exe", "*.psx", "*.psf", "*.minipsf", "*.m3u"};
}

FullscreenUI::FileSelectorFilters FullscreenUI::GetImageFilters()
{
  return {"*.png", "*.jpg", "*.jpeg", "*.webp"};
}

void FullscreenUI::ClearGameListState()
{
  s_game_list_locals.icon_image_map.clear();
  s_game_list_locals.cover_image_map.clear();
  s_game_list_locals.game_list_sorted_entries = {};
}

static constexpr std::array<const char*, static_cast<size_t>(FullscreenUI::GameListView::MaxCount)>
  s_game_list_view_names = {{"Grid", "Carousel", "List"}};
static constexpr std::array<const char*, static_cast<size_t>(FullscreenUI::GameListView::MaxCount)>
  s_game_list_view_display_names = {{FSUI_NSTR("Grid"), FSUI_NSTR("Carousel"), FSUI_NSTR("List")}};

const char* FullscreenUI::GetGameListViewName(GameListView view)
{
  return (static_cast<size_t>(view) < s_game_list_view_names.size()) ?
           s_game_list_view_names[static_cast<size_t>(view)] :
           "";
}

const char* FullscreenUI::GetGameListViewDisplayName(GameListView view)
{
  return (static_cast<size_t>(view) < s_game_list_view_display_names.size()) ?
           Host::TranslateToCString(FSUI_TR_CONTEXT, s_game_list_view_display_names[static_cast<size_t>(view)]) :
           "";
}

std::optional<FullscreenUI::GameListView> FullscreenUI::ParseGameListViewName(std::string_view name)
{
  for (size_t i = 0; i < s_game_list_view_names.size(); i++)
  {
    if (name == s_game_list_view_names[i])
      return static_cast<GameListView>(i);
  }

  return std::nullopt;
}

void FullscreenUI::SetGameListView(GameListView view)
{
  s_game_list_locals.view = view;
  Core::SetBaseStringSettingValue(GameList::UI_SECTION_NAME, GameList::SETTING_KEY_FULLSCREENUI_VIEW,
                                  GetGameListViewName(view));
  Host::CommitBaseSettingChanges();
  QueueResetFocus(FocusResetType::ViewChanged);
}

void FullscreenUI::SetGameListSortColumn(GameList::Column column)
{
  s_game_list_locals.sort_column = column;
  Core::SetBaseStringSettingValue(GameList::UI_SECTION_NAME, GameList::SETTING_KEY_SORT_COLUMN,
                                  GameList::GetColumnName(column));
  Host::CommitBaseSettingChanges();
}

void FullscreenUI::DoSetCoverImage(std::string entry_path)
{
  OpenFileSelector(FSUI_ICONVSTR(ICON_FA_IMAGE, "Set Cover Image"), GetImageFilters(), EmuFolders::Covers,
                   [entry_path = std::move(entry_path)](std::string path) {
                     if (path.empty())
                       return;

                     const auto lock = GameList::GetLock();
                     const GameList::Entry* entry = GameList::GetEntryForPath(entry_path);
                     if (!entry)
                       return;

                     std::string existing_path = GameList::GetCoverImagePathForEntry(entry);
                     std::string new_path = GameList::GetNewCoverImagePathForEntry(entry, path.c_str(), false);
                     if (!existing_path.empty())
                     {
                       OpenConfirmMessageDialog(
                         ICON_EMOJI_WARNING, FSUI_ICONVSTR(ICON_FA_IMAGE, "Set Cover Image"),
                         FSUI_STR("A cover already exists for this game. Are you sure that you want to overwrite it?"),
                         [path = std::move(path), existing_path = std::move(existing_path),
                          new_path = std::move(new_path)](bool result) {
                           if (!result)
                             return;

                           DoSetCoverImage(std::move(path), std::move(existing_path), std::move(new_path));
                         });
                     }
                     else
                     {
                       DoSetCoverImage(std::move(path), std::move(existing_path), std::move(new_path));
                     }
                   });
}

void FullscreenUI::DoSetCoverImage(std::string source_path, std::string existing_path, std::string new_path)
{
  Error error;
  if (!existing_path.empty() && existing_path != new_path && FileSystem::FileExists(existing_path.c_str()))
  {
    if (!FileSystem::DeleteFile(existing_path.c_str(), &error))
    {
      ShowToast(OSDMessageType::Error, {},
                fmt::format(FSUI_FSTR("Failed to delete existing cover: {}"), error.GetDescription()));
      return;
    }
  }

  if (!FileSystem::CopyFilePath(source_path.c_str(), new_path.c_str(), true, &error))
  {
    ShowToast(OSDMessageType::Error, {}, fmt::format(FSUI_FSTR("Failed to copy cover: {}"), error.GetDescription()));
    return;
  }

  ShowToast(OSDMessageType::Quick, {}, FSUI_STR("Cover set."));

  // Ensure the old one wasn't cached.
  if (!existing_path.empty())
    InvalidateCachedTexture(existing_path);
  if (existing_path != new_path)
    InvalidateCachedTexture(new_path);
  s_game_list_locals.cover_image_map.clear();
}

void FullscreenUI::PopulateGameListEntryList()
{
  const std::string_view string_filter = s_game_list_locals.game_list_search_string;

  s_game_list_locals.game_list_sorted_entries.clear();
  s_game_list_locals.game_list_sorted_entries.reserve(GameList::GetEntryCount());
  for (const GameList::Entry& entry : GameList::GetEntries())
  {
    if (s_game_list_locals.merge_disc_sets)
    {
      if (entry.disc_set_member)
        continue;
    }
    else
    {
      if (entry.IsDiscSet())
        continue;
    }

    if (!string_filter.empty())
    {
      if (!((!entry.IsDiscSet() && StringUtil::ContainsNoCase(entry.path, string_filter)) ||
            StringUtil::ContainsNoCase(entry.serial, string_filter) ||
            StringUtil::ContainsNoCase(entry.GetDisplayTitle(true), string_filter) ||
            StringUtil::ContainsNoCase(entry.GetDisplayTitle(false), string_filter)))
      {
        continue;
      }
    }

    s_game_list_locals.game_list_sorted_entries.push_back(&entry);
  }

  std::ranges::sort(s_game_list_locals.game_list_sorted_entries, [](const GameList::Entry* lhs,
                                                                    const GameList::Entry* rhs) {
    return GameList::CompareEntryLessThan(s_game_list_locals.sort_reversed ? rhs : lhs,
                                          s_game_list_locals.sort_reversed ? lhs : rhs, s_game_list_locals.sort_column);
  });
}

void FullscreenUI::DrawGameListWindow()
{
  auto game_list_lock = GameList::GetLock();
  PopulateGameListEntryList();

  const ImGuiIO& io = ImGui::GetIO();
  const ImVec2 heading_size = ImVec2(io.DisplaySize.x, LayoutScale(LAYOUT_PAGE_HEADER_HEIGHT));

  if (BeginPageHeader("gamelist_header", GetPageBackgroundColor(), true))
  {
    if (PageHeaderBackButton())
    {
      BeginTransition(TransitionEffect::ZoomOut, DEFAULT_TRANSITION_TIME,
                      []() { SwitchToMainWindow(MainWindowType::Landing); });
    }

    PageHeaderTitle(FSUI_VSTR("Library"));

    ImGuiWindow* const window = ImGui::GetCurrentWindow();
    const float spacing = LayoutScale(8.0f);

    // view pills after the title
    const std::array<IconStackString, static_cast<size_t>(GameListView::MaxCount)> view_labels = {{
      IconStackString(ICON_FA_TABLE_CELLS_LARGE, FSUI_NSTR("Grid")),
      IconStackString(ICON_FA_IMAGES, FSUI_NSTR("Carousel")),
      IconStackString(ICON_FA_LIST, FSUI_NSTR("List")),
    }};

    // pills are the same height as the search box
    static constexpr const float& search_font_size = UIStyle.MediumFontSize;
    static constexpr const float& search_font_weight = UIStyle.BoldFontWeight;
    const ImVec2 search_padding = LayoutScale(LAYOUT_MENU_BUTTON_X_PADDING, 9.0f);
    const float pill_height = search_font_size + search_padding.y * 2.0f;
    const float pills_left = window->DC.CursorPos.x;
    ImGui::NewLine(); // otherwise the pills after the first are placed on the back button's line
    ImGui::SetCursorScreenPos(ImVec2(pills_left, window->Pos.y + ImFloor((heading_size.y - pill_height) * 0.5f)));
    for (size_t i = 0; i < view_labels.size(); i++)
    {
      const GameListView view = static_cast<GameListView>(i);
      const bool active = (view == s_game_list_locals.view);
      bool hovered;
      if (PillButton(window->GetID(GetGameListViewName(view)), view_labels[i], pill_height,
                     active ? ImGui::GetColorU32(UIStyle.BackgroundTextColor) : 0,
                     ImGui::GetColorU32(active ? UIStyle.SurfaceColor : UIStyle.MutedTextColor), 0, &hovered) &&
          !active)
      {
        BeginTransition([view]() { SetGameListView(view); });
      }
      ImGui::SameLine(0.0f, LayoutScale(4.0f));
    }
    const float view_hint_x = ImGui::GetItemRectMax().x + spacing;
    const float pills_right =
      view_hint_x + DrawInputHint(window->DrawList, ImVec2(view_hint_x, window->Pos.y), heading_size.y,
                                  GetInputHint(ICON_PF_LEFT_SHOULDER_LB ICON_PF_RIGHT_SHOULDER_RB, ICON_PF_F4));

    // right to left: sort order (with the key that changes it inside the pill), search (collapsed until used)
    const SmallString sort_label =
      SmallString::from_format("{}  {}  {}", GetInputHint(ICON_PF_RIGHT_ANALOG_CLICK, ICON_PF_F6),
                               GameList::GetColumnDisplayName(s_game_list_locals.sort_column),
                               s_game_list_locals.sort_reversed ? ICON_FA_ARROW_DOWN : ICON_FA_ARROW_UP);
    const float sort_left =
      window->WorkRect.Max.x - MeasurePill(sort_label, pill_height, UIStyle.MediumFontSize, UIStyle.BoldFontWeight).x;

    // stays open while focused or filtering, progress is linear so that reversing part way through doesn't jump
    static constexpr float SEARCH_ANIMATION_TIME = 0.1f;
    const ImGuiID search_id = window->GetID("##game_list_search");
    const bool search_has_filter = (s_game_list_locals.game_list_search_string[0] != '\0');
    const bool search_open = (GImGui->ActiveId == search_id || search_has_filter);
    const float search_step = UIStyle.Animations ? (io.DeltaTime / SEARCH_ANIMATION_TIME) : 1.0f;
    float& search_progress = s_game_list_locals.search_expand_progress;
    search_progress = std::clamp(search_progress + (search_open ? search_step : -search_step), 0.0f, 1.0f);

    const std::string_view search_key = GetInputHint(ICON_PF_BUTTON_X, ICON_PF_F3);
    const char* const search_hint = FSUI_CSTR("Search");
    const float search_collapsed_width =
      ImCeil(search_padding.x * 2.0f + MeasureText(search_font_size, search_font_weight, search_key).x +
             ImCeil(search_font_size * 0.5f) + MeasureText(search_font_size, search_font_weight, search_hint).x);

    // shrinks if it would run into the view pills
    const float search_expanded_width =
      std::max(search_collapsed_width, std::min(LayoutScale(250.0f), sort_left - pills_right - spacing * 3.0f));

    // whole pixels, otherwise the right edge shimmers while animating
    const float search_width =
      ImCeil(std::lerp(search_collapsed_width, search_expanded_width, Easing::OutExpo(search_progress)));
    const float search_x = sort_left - spacing - search_width;

    ImGui::NewLine();
    ImGui::SetCursorScreenPos(ImVec2(sort_left, window->Pos.y + ImFloor((heading_size.y - pill_height) * 0.5f)));
    bool sort_hovered;
    if (PillButton(window->GetID("sort"), sort_label, pill_height, ImGui::GetColorU32(UIStyle.SurfaceRaisedColor),
                   ImGui::GetColorU32(UIStyle.MutedTextColor), 0, &sort_hovered))
    {
      ChoiceDialogOptions options;
      options.reserve(static_cast<size_t>(GameList::Column::MaxCount));
      for (u32 i = 0; i < static_cast<u32>(GameList::Column::MaxCount); i++)
      {
        options.emplace_back(GameList::GetColumnDisplayName(static_cast<GameList::Column>(i)),
                             static_cast<u32>(s_game_list_locals.sort_column) == i);
      }
      OpenChoiceDialog(FSUI_ICONVSTR(ICON_FA_SORT, "Sort By"), false, std::move(options),
                       [](s32 index, const std::string& title, bool checked) {
                         if (index >= 0)
                           SetGameListSortColumn(static_cast<GameList::Column>(index));

                         CloseCurrentDialog();
                       });
    }

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, search_padding);

    // pill frame is drawn here to match the sort pill, so hide the input's own frame
    const ImVec2 search_pos = ImFloor(ImVec2(search_x, window->Pos.y + (heading_size.y - pill_height) * 0.5f));
    const ImVec2 search_max = search_pos + ImVec2(search_width, pill_height);
    window->DrawList->AddRectFilled(search_pos, search_max,
                                    ImGui::GetColorU32((GImGui->NavId == search_id) ?
                                                         DarkerColor(UIStyle.SurfaceRaisedColor, 1.3f) :
                                                         UIStyle.SurfaceRaisedColor),
                                    pill_height * 0.5f);

    static constexpr ImVec4 transparent = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    const ImVec4 search_text_color = search_open ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : UIStyle.MutedTextColor;
    ImGui::PushStyleColor(ImGuiCol_Button, transparent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, transparent);
    ImGui::PushStyleColor(ImGuiCol_Border, transparent);
    ImGui::PushStyleColor(ImGuiCol_Text, search_text_color);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, UIStyle.MutedTextColor);

    // leave room for the clear button at the end of the pill
    const float search_clear_width =
      MeasurePill(ICON_FA_XMARK, pill_height, UIStyle.MediumFontSize, UIStyle.BoldFontWeight).x;
    const float search_input_width =
      search_has_filter ? (search_width - search_clear_width + search_padding.x) : search_width;

    ImGui::SetCursorScreenPos(search_pos);

    const ImGuiInputTextFlags extra_flags =
      (ImGui::IsKeyPressed(ImGuiKey_NavGamepadMenu, false) || ImGui::IsKeyPressed(ImGuiKey_F3, false)) ?
        ImGuiInputTextFlags_AlwaysActivate :
        0;
    if (InputTextWithIcon("##game_list_search", search_open ? ICON_FA_MAGNIFYING_GLASS : search_key, search_hint,
                          s_game_list_locals.game_list_search_string,
                          sizeof(s_game_list_locals.game_list_search_string), search_input_width, search_font_size,
                          search_font_weight,
                          ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue | extra_flags) ||
        (ImGui::IsItemDeactivated() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
    {
      // Clear search on escape press, and don't let the same press close the game list.
      if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
      {
        s_game_list_locals.game_list_search_string[0] = '\0';
        CancelPendingMenuClose();
      }

      QueueResetFocus(FocusResetType::Other);
      ForceKeyNavEnabled();
    }

    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar();

    if (search_has_filter)
    {
      ImGui::SetCursorScreenPos(ImVec2(search_max.x - search_clear_width, search_pos.y));
      bool clear_hovered;
      if (PillButton(window->GetID("clear_search"), ICON_FA_XMARK, pill_height, 0,
                     ImGui::GetColorU32(UIStyle.MutedTextColor), 0, &clear_hovered))
      {
        s_game_list_locals.game_list_search_string[0] = '\0';
        QueueResetFocus(FocusResetType::Other);
      }

      // after the button, otherwise its hover background covers the border
      window->DrawList->AddRect(search_pos, search_max, ImGui::GetColorU32(UIStyle.AccentColor), pill_height * 0.5f,
                                LayoutScale(1.0f));
    }
  }
  EndPageHeader();

  switch (s_game_list_locals.view)
  {
    case GameListView::Carousel:
      DrawGameCarousel(heading_size);
      break;
    case GameListView::List:
      DrawGameList(heading_size);
      break;
    default:
      DrawGameGrid(heading_size);
      break;
  }

  // note: has to come afterwards
  if (!AreAnyDialogsOpen())
  {
    if (ImGui::IsKeyPressed(ImGuiKey_NavGamepadTweakSlow, false) ||
        ImGui::IsKeyPressed(ImGuiKey_NavGamepadTweakFast, false) || ImGui::IsKeyPressed(ImGuiKey_F4, false))
    {
      // L1 goes to the previous view, R1/F4 to the next
      static constexpr u32 count = static_cast<u32>(GameListView::MaxCount);
      const u32 step = ImGui::IsKeyPressed(ImGuiKey_NavGamepadTweakSlow, false) ? (count - 1) : 1;
      EnqueueSoundEffect(SFX_NAV_MOVE);
      BeginTransition([step]() {
        SetGameListView(static_cast<GameListView>((static_cast<u32>(s_game_list_locals.view) + step) % count));
      });
    }
    else if (ImGui::IsKeyPressed(ImGuiKey_GamepadR3, false) || ImGui::IsKeyPressed(ImGuiKey_F6, false))
    {
      // flips the order, then moves on to the next column
      EnqueueSoundEffect(SFX_NAV_MOVE);
      const bool reversed = !s_game_list_locals.sort_reversed;
      s_game_list_locals.sort_reversed = reversed;
      Core::SetBaseBoolSettingValue(GameList::UI_SECTION_NAME, GameList::SETTING_KEY_SORT_REVERSED, reversed);
      if (!reversed)
      {
        SetGameListSortColumn(static_cast<GameList::Column>((static_cast<u32>(s_game_list_locals.sort_column) + 1) %
                                                            static_cast<u32>(GameList::Column::MaxCount)));
      }
      else
      {
        Host::CommitBaseSettingChanges();
      }
    }
    else if (ImGui::IsKeyPressed(ImGuiKey_GamepadBack, false) || ImGui::IsKeyPressed(ImGuiKey_F2, false))
    {
      EnqueueSoundEffect(SFX_NAV_BACK);
      BeginTransition(TransitionEffect::ZoomIn, DEFAULT_TRANSITION_TIME, []() {
        // if we're opening to game list, use the main page, otherwise game list settings
        const SettingsPage page = ShouldOpenToGameList() ? SettingsPage::Interface : SettingsPage::GameList;
        FullscreenUI::SwitchToSettings(page);
      });
    }
    else if (ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false) || ImGui::IsKeyPressed(ImGuiKey_F5, false))
    {
      EnqueueSoundEffect(SFX_NAV_ACTIVATE);
      DoResume();
    }
  }

  if (IsGamepadInputSource())
  {
    SetFullscreenFooterText(std::array{std::make_pair(ICON_PF_XBOX_DPAD, FSUI_VSTR("Select Game")),
                                       std::make_pair(ICON_PF_BURGER_MENU, FSUI_VSTR("Resume Last Session")),
                                       std::make_pair(ICON_PF_SHARE_CAPTURE, FSUI_VSTR("Settings")),
                                       std::make_pair(ICON_PF_BUTTON_Y, FSUI_VSTR("Launch Options")),
                                       std::make_pair(ICON_PF_BUTTON_A, FSUI_VSTR("Start Game")),
                                       std::make_pair(ICON_PF_BUTTON_B, FSUI_VSTR("Back"))});
  }
  else
  {
    SetFullscreenFooterText(std::array{
      std::make_pair(ICON_PF_ARROW_UP ICON_PF_ARROW_DOWN ICON_PF_ARROW_LEFT ICON_PF_ARROW_RIGHT,
                     FSUI_VSTR("Select Game")),
      std::make_pair(ICON_PF_F1, FSUI_VSTR("Launch Options")),
      std::make_pair(ICON_PF_F2, FSUI_VSTR("Settings")),
      std::make_pair(ICON_PF_F5, FSUI_VSTR("Resume Last Session")),
      std::make_pair(ICON_PF_ENTER, FSUI_VSTR("Start Game")),
      std::make_pair(ICON_PF_ESC, FSUI_VSTR("Back")),
    });
  }
}

void FullscreenUI::DrawGameList(const ImVec2& heading_size)
{
  static constexpr auto to_mb = [](s64 size) { return static_cast<u32>((size + 1048575) / 1048576); };

  if (!BeginFullscreenColumns(nullptr, heading_size.y, true, true))
  {
    EndFullscreenColumns();
    return;
  }

  if (!AreAnyDialogsOpen() && WantsToCloseMenu())
  {
    BeginTransition(TransitionEffect::ZoomOut, DEFAULT_TRANSITION_TIME,
                    []() { SwitchToMainWindow(MainWindowType::Landing); });
  }

  auto game_list_lock = GameList::GetLock();
  const GameList::Entry* selected_entry = nullptr;
  PopulateGameListEntryList();

  if (IsFocusResetFromWindowChange())
    ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));

  if (BeginFullscreenColumnWindow(0.0f, -530.0f, "game_list_entries", GetTransparentBackgroundColor(),
                                  ImVec2(LAYOUT_MENU_WINDOW_X_PADDING, LAYOUT_MENU_WINDOW_Y_PADDING)))
  {
    const GameList::IconSource icon_source = s_game_list_locals.icon_source;
    const bool show_images = (icon_source != GameList::IconSource::None);
    const float image_size =
      show_images ? (s_game_list_locals.compact_mode ? UIStyle.LargeFontSize : LayoutScale(50.0f)) : 0.0f;
    const float row_image_padding = show_images ? LayoutScale(15.0f) : 0.0f;
    const float row_left_margin = image_size + row_image_padding;

    ResetFocusHere();

    BeginMenuButtons();

    SmallString summary;
    const u32 text_color = ImGui::GetColorU32(ImGui::GetStyle().Colors[ImGuiCol_Text]);
    const u32 subtitle_text_color = ImGui::GetColorU32(DarkerColor(ImGui::GetStyle().Colors[ImGuiCol_Text]));

    for (const GameList::Entry* entry : s_game_list_locals.game_list_sorted_entries)
    {
      if (!s_game_list_locals.compact_mode)
      {
        if (entry->serial.empty())
        {
          summary.format("{} \u2022 {} MB", Path::GetFileName(entry->path), to_mb(entry->file_size));
        }
        else
        {
          summary.format("{} \u2022 {} \u2022 {} MB", entry->serial, Path::GetFileName(entry->path),
                         to_mb(entry->file_size));
        }
      }

      const MenuButtonBounds mbb(entry->GetDisplayTitle(s_game_list_locals.localized_titles), {}, summary,
                                 row_left_margin);

      bool visible, hovered;
      bool pressed = MenuButtonFrame(GetKeyForGameListEntry(entry), true, mbb.frame_bb, &visible, &hovered);
      if (!visible)
        continue;

      if (show_images)
      {
        const ImRect image_rect(ImVec2(mbb.title_bb.Min.x - row_left_margin, mbb.title_bb.Min.y),
                                ImVec2(mbb.title_bb.Min.x - row_image_padding, mbb.title_bb.Min.y + image_size));
        DrawGameListIcon(entry, entry->GetDisplayTitle(s_game_list_locals.localized_titles), ImGui::GetWindowDrawList(),
                         image_rect, IM_COL32(255, 255, 255, 255));
      }

      RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, mbb.title_bb.Min,
                                mbb.title_bb.Max, text_color,
                                entry->GetDisplayTitle(s_game_list_locals.localized_titles), &mbb.title_size,
                                ImVec2(0.0f, 0.0f), mbb.title_size.x, &mbb.title_bb);

      if (!summary.empty())
      {
        RenderShadowedTextClipped(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.NormalFontWeight, mbb.summary_bb.Min,
                                  mbb.summary_bb.Max, subtitle_text_color, summary, &mbb.summary_size,
                                  ImVec2(0.0f, 0.0f), mbb.summary_size.x, &mbb.summary_bb);
      }

      if (pressed)
      {
        HandleGameListActivate(entry);
      }
      else
      {
        if (hovered)
          selected_entry = entry;

        if (selected_entry &&
            (ImGui::IsItemClicked(ImGuiMouseButton_Right) ||
             ImGui::IsKeyPressed(ImGuiKey_NavGamepadContextMenu, false) || ImGui::IsKeyPressed(ImGuiKey_F1, false)))
        {
          CancelPendingMenuClose();
          HandleGameListOptions(selected_entry);
        }
      }

      if (entry == s_game_list_locals.game_list_sorted_entries.front())
        ImGui::SetItemDefaultFocus();
    }

    EndMenuButtons();
  }
  SetWindowNavWrapping(false, true);
  EndFullscreenColumnWindow();

  // avoid clearing the selection for a couple of seconds when the mouse goes inbetween items
  static constexpr float ITEM_TIMEOUT = 1.0f;
  if (!selected_entry)
  {
    if (!s_game_list_locals.game_list_current_selection_path.empty())
    {
      // reset countdown if a dialog was open
      if (AreAnyDialogsOpen())
      {
        s_game_list_locals.game_list_current_selection_timeout = ITEM_TIMEOUT;
      }
      else
      {
        s_game_list_locals.game_list_current_selection_timeout -= ImGui::GetIO().DeltaTime;
        if (s_game_list_locals.game_list_current_selection_timeout <= 0.0f)
        {
          s_game_list_locals.game_list_current_selection_timeout = 0.0f;
          s_game_list_locals.game_list_current_selection_path.clear();
        }
      }
    }

    if (!s_game_list_locals.game_list_current_selection_path.empty())
      selected_entry = GameList::GetEntryForPath(s_game_list_locals.game_list_current_selection_path);
  }
  else
  {
    // reset countdown on new or current item
    if (s_game_list_locals.game_list_current_selection_path != selected_entry->path)
      s_game_list_locals.game_list_current_selection_path = selected_entry->path;
    s_game_list_locals.game_list_current_selection_timeout = ITEM_TIMEOUT;
  }

  static constexpr float info_window_width = 530.0f;
  if (BeginFullscreenColumnWindow(-info_window_width, 0.0f, "game_list_info",
                                  ModAlpha(UIStyle.PrimaryDarkColor, GetBackgroundAlpha())))
  {
    static constexpr float info_top_margin = 20.0f;
    static constexpr float cover_size = 320.0f;

    ImGui::SetCursorPos(LayoutScale((info_window_width - cover_size) / 2.0f, info_top_margin));
    const ImRect cover_rect(ImGui::GetCursorScreenPos(),
                            ImGui::GetCursorScreenPos() + LayoutScale(ImVec2(cover_size, cover_size)));
    if (selected_entry)
    {
      DrawGameListCover(selected_entry, false, false, false, s_game_list_locals.localized_titles,
                        ImGui::GetWindowDrawList(), cover_rect);
    }
    else
    {
      GPUTexture* const cover_texture = GetTextureForGameListEntryType(GameList::EntryType::MaxCount);
      const ImRect image_rect(CenterImage(cover_rect, ImVec2(static_cast<float>(cover_texture->GetWidth()),
                                                             static_cast<float>(cover_texture->GetHeight()))));
      ImGui::GetWindowDrawList()->AddImage(cover_texture, image_rect.Min, image_rect.Max);
    }

    const float work_width = ImGui::GetCurrentWindow()->WorkRect.GetWidth();
    static constexpr float field_margin_y = 4.0f;
    static constexpr float start_x = 50.0f;
    float text_y = info_top_margin + cover_size + info_top_margin;

    float text_width;

    PushPrimaryColor();
    ImGui::SetCursorPos(LayoutScale(start_x, text_y));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, LayoutScale(0.0f, field_margin_y));
    ImGui::BeginGroup();

    if (selected_entry)
    {
      const ImVec4 subtitle_text_color = DarkerColor(ImGui::GetStyle().Colors[ImGuiCol_Text]);
      const std::string_view title = selected_entry->GetDisplayTitle(s_game_list_locals.localized_titles);

      // title
      ImGui::PushFont(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight);
      text_width = ImGui::CalcTextSize(IMSTR_START_END(title), false, work_width).x;
      ImGui::SetCursorPosX((work_width - text_width) / 2.0f);
      ImGui::TextWrapped("%.*s", static_cast<int>(title.size()), title.data());
      ImGui::PopFont();

      ImGui::PushFont(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight);

      // developer
      if (selected_entry->dbentry && !selected_entry->dbentry->developer.empty())
      {
        text_width =
          ImGui::CalcTextSize(selected_entry->dbentry->developer.data(),
                              selected_entry->dbentry->developer.data() + selected_entry->dbentry->developer.length(),
                              false, work_width)
            .x;
        ImGui::SetCursorPosX((work_width - text_width) / 2.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, subtitle_text_color);
        ImGui::TextWrapped("%.*s", static_cast<int>(selected_entry->dbentry->developer.size()),
                           selected_entry->dbentry->developer.data());
        ImGui::PopStyleColor();
      }

      // code
      text_width = ImGui::CalcTextSize(selected_entry->serial.c_str(), nullptr, false, work_width).x;
      ImGui::SetCursorPosX((work_width - text_width) / 2.0f);
      ImGui::PushStyleColor(ImGuiCol_Text, DarkerColor(subtitle_text_color));
      ImGui::TextWrapped("%s", selected_entry->serial.c_str());
      ImGui::PopStyleColor();
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + LayoutScale(15.0f));

      ImGui::PopFont();
      ImGui::PushFont(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.NormalFontWeight);

      // region
      {
        const bool display_as_language = (selected_entry->dbentry && selected_entry->dbentry->HasAnyLanguage());
        const ImVec2 size = LayoutScale(23.0f, 16.0f);
        ImGui::PushFont(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight);
        TextUnformatted(
          FSUI_ICONVSTR(ICON_EMOJI_GLOBE, display_as_language ? FSUI_CSTR("Language: ") : FSUI_CSTR("Region: ")));
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::Image(GetCachedTexture(selected_entry->GetLanguageIconName(), size), size);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, subtitle_text_color);
        if (display_as_language)
        {
          ImGui::TextWrapped(" (%s, %s)", selected_entry->dbentry->GetLanguagesString().c_str(),
                             Settings::GetDiscRegionDisplayName(selected_entry->region));
        }
        else
        {
          ImGui::TextWrapped(" (%s)", Settings::GetDiscRegionDisplayName(selected_entry->region));
        }
        ImGui::PopStyleColor();
      }

      // genre
      if (selected_entry->dbentry)
      {
        if (!selected_entry->dbentry->genre.empty())
        {
          ImGui::PushFont(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight);
          TextUnformatted(FSUI_ICONVSTR(ICON_EMOJI_BOOKS, FSUI_VSTR("Genre: ")));
          ImGui::PopFont();
          ImGui::SameLine();
          ImGui::PushStyleColor(ImGuiCol_Text, subtitle_text_color);
          ImGui::TextUnformatted(selected_entry->dbentry->genre.data(),
                                 selected_entry->dbentry->genre.data() + selected_entry->dbentry->genre.length());
          ImGui::PopStyleColor();
        }

        if (selected_entry->dbentry->release_date != 0)
        {
          ImGui::PushFont(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight);
          TextUnformatted(FSUI_ICONVSTR(ICON_EMOJI_CALENDAR, FSUI_VSTR("Release Date: ")));
          ImGui::PopFont();
          ImGui::SameLine();
          ImGui::PushStyleColor(ImGuiCol_Text, subtitle_text_color);
          ImGui::TextUnformatted(selected_entry->GetReleaseDateString().c_str());
          ImGui::PopStyleColor();
        }
      }

      // achievements
      if (selected_entry->num_achievements > 0)
      {
        ImGui::PushFont(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight);
        TextUnformatted(FSUI_ICONVSTR(ICON_EMOJI_TROPHY, "Achievements: "));
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, subtitle_text_color);
        if (selected_entry->unlocked_achievements_hc > 0)
        {
          ImGui::Text("%u (%u) / %u", selected_entry->unlocked_achievements, selected_entry->unlocked_achievements_hc,
                      selected_entry->num_achievements);
        }
        else
        {
          ImGui::Text("%u / %u", selected_entry->unlocked_achievements, selected_entry->num_achievements);
        }
        ImGui::PopStyleColor();
      }

      // compatibility
      const ImVec2 compatibility_size = LayoutScale(88.0f, 16.0f);
      ImGui::PushFont(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight);
      TextUnformatted(FSUI_ICONSTR(ICON_EMOJI_STAR, FSUI_VSTR("Compatibility: ")));
      ImGui::PopFont();
      ImGui::SameLine();
      ImGui::Image(GetCachedTexture(selected_entry->GetCompatibilityIconFileName(), compatibility_size),
                   compatibility_size);
      ImGui::SameLine();
      ImGui::PushStyleColor(ImGuiCol_Text, subtitle_text_color);
      ImGui::Text(" (%s)", GameDatabase::GetCompatibilityRatingDisplayName(
                             (selected_entry && selected_entry->dbentry) ? selected_entry->dbentry->compatibility :
                                                                           GameDatabase::CompatibilityRating::Unknown));
      ImGui::PopStyleColor();

      // play time
      ImGui::PushFont(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight);
      TextUnformatted(FSUI_ICONSTR(ICON_EMOJI_HOURGLASS, FSUI_VSTR("Time Played: ")));
      ImGui::PopFont();
      ImGui::SameLine();
      ImGui::PushStyleColor(ImGuiCol_Text, subtitle_text_color);
      ImGui::TextUnformatted(Host::FormatTimespan(selected_entry->total_played_time, true).c_str());
      ImGui::PopStyleColor();
      ImGui::PushFont(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight);
      TextUnformatted(FSUI_ICONSTR(ICON_EMOJI_CLOCK_FIVE_OCLOCK, FSUI_CSTR("Last Played: ")));
      ImGui::PopFont();
      ImGui::SameLine();
      ImGui::PushStyleColor(ImGuiCol_Text, subtitle_text_color);
      ImGui::TextUnformatted(Host::FormatRelativeDate(selected_entry->last_played_time, false, true).c_str());
      ImGui::PopStyleColor();

      // size
      if (selected_entry->file_size >= 0)
      {
        ImGui::PushFont(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight);
        TextUnformatted(FSUI_ICONSTR(ICON_EMOJI_FILE_FOLDER_OPEN, FSUI_VSTR("Size: ")));
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, subtitle_text_color);
        ImGui::Text(FSUI_CSTR("%u MB"), to_mb(selected_entry->uncompressed_size));
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, DarkerColor(subtitle_text_color));
        ImGui::Text(FSUI_CSTR(" (%u MB on disk)"), to_mb(selected_entry->file_size));
        ImGui::PopStyleColor();
      }
      else
      {
        ImGui::PushFont(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight);
        ImGui::TextUnformatted(FSUI_CSTR("Unknown File Size"));
        ImGui::PopFont();
      }

      ImGui::PopFont();
    }
    else
    {
      // title
      const char* title = FSUI_CSTR("No Game Selected");
      ImGui::PushFont(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight);
      text_width = ImGui::CalcTextSize(title, nullptr, false, work_width).x;
      ImGui::SetCursorPosX((work_width - text_width) / 2.0f);
      ImGui::TextWrapped("%s", title);
      ImGui::PopFont();
    }

    ImGui::EndGroup();
    ImGui::PopStyleVar();
    PopPrimaryColor();
  }
  EndFullscreenColumnWindow();

  EndFullscreenColumns();
}

void FullscreenUI::DrawGameGrid(const ImVec2& heading_size)
{
  if (IsFocusResetFromWindowChange())
    ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));

  ImGuiIO& io = ImGui::GetIO();
  if (!BeginFullscreenWindow(
        ImVec2(0.0f, heading_size.y),
        ImVec2(io.DisplaySize.x, io.DisplaySize.y - heading_size.y - LayoutScale(LAYOUT_FOOTER_HEIGHT)), "game_grid",
        GetTransparentBackgroundColor(), 0.0f, ImVec2(LAYOUT_MENU_WINDOW_X_PADDING, LAYOUT_MENU_WINDOW_Y_PADDING)))
  {
    EndFullscreenWindow();
    return;
  }

  if (ImGui::IsWindowFocused() && WantsToCloseMenu())
  {
    BeginTransition(TransitionEffect::ZoomOut, DEFAULT_TRANSITION_TIME,
                    []() { SwitchToMainWindow(MainWindowType::Landing); });
  }

  ResetFocusHere();
  BeginMenuButtons(0, 0.0f, 15.0f, 15.0f, 20.0f, 20.0f);

  const ImGuiStyle& style = ImGui::GetStyle();

  const float title_font_size = UIStyle.MediumLargeFontSize;
  const float title_font_weight = UIStyle.NormalFontWeight;
  const float avail_width = ImGui::GetContentRegionAvail().x;
  const float title_spacing = LayoutScale(10.0f);
  const float item_width_with_spacing = std::floor(avail_width / 5.0f);
  const float item_width = item_width_with_spacing - style.ItemSpacing.x;
  const float image_width = item_width - (style.FramePadding.x * 2.0f);
  const float image_height = image_width;
  const ImVec2 image_size(image_width, image_height);
  const float base_item_height = (style.FramePadding.y * 2.0f) + image_height;
  const u32 grid_count_x = static_cast<u32>(std::floor(avail_width / item_width_with_spacing));

  // calculate padding to center it, the last item in the row doesn't need spacing
  const float x_padding = std::floor(
    (avail_width - ((item_width_with_spacing * static_cast<float>(grid_count_x)) - style.ItemSpacing.x)) * 0.5f);

  ImGuiWindow* const window = ImGui::GetCurrentWindow();
  ImDrawList* const dl = ImGui::GetWindowDrawList();
  SmallString draw_title;
  const u32 text_color = ImGui::GetColorU32(ImGuiCol_Text);

  u32 grid_x = 0;
  float row_item_height = base_item_height;
  if (!s_game_list_locals.game_list_sorted_entries.empty())
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + x_padding);

  // Prefetch two rows worth of covers.
  const ImVec2 prefetch_range =
    ImVec2(window->Pos.y + window->Scroll.y, window->Pos.y + window->Scroll.y + (window->ClipRect.GetHeight() * 2.0f));

  for (size_t entry_index = 0; entry_index < s_game_list_locals.game_list_sorted_entries.size(); entry_index++)
  {
    if (window->SkipItems)
      continue;

    // This is pretty annoying. If we don't use an equal sized item for each grid item, keyboard/gamepad navigation
    // tends to break when scrolling vertically - it goes left/right. Precompute the maximum item height for the row
    // first, and make all items the same size to work around this.
    const GameList::Entry* entry = s_game_list_locals.game_list_sorted_entries[entry_index];
    if (grid_x == 0 && s_game_list_locals.cover_titles)
    {
      row_item_height = 0.0f;

      const size_t row_entry_index_end =
        std::min(entry_index + grid_count_x, s_game_list_locals.game_list_sorted_entries.size());
      for (size_t row_entry_index = entry_index; row_entry_index < row_entry_index_end; row_entry_index++)
      {
        const GameList::Entry* row_entry = s_game_list_locals.game_list_sorted_entries[row_entry_index];
        const std::string_view row_title = row_entry->GetDisplayTitle(s_game_list_locals.localized_titles);
        const ImVec2 this_title_size = UIStyle.Font->CalcTextSizeA(title_font_size, title_font_weight, image_width,
                                                                   image_width, IMSTR_START_END(row_title));
        row_item_height = std::max(row_item_height, this_title_size.y);
      }

      row_item_height += title_spacing + base_item_height;
    }

    ImVec2 title_size;
    if (s_game_list_locals.cover_titles)
    {
      const std::string_view title = entry->GetDisplayTitle(s_game_list_locals.localized_titles);
      title_size = UIStyle.Font->CalcTextSizeA(title_font_size, title_font_weight, image_width, image_width,
                                               IMSTR_START_END(title));
    }

    const std::string_view item_key = GetKeyForGameListEntry(entry);
    const ImGuiID id = window->GetID(IMSTR_START_END(item_key));
    const ImVec2 pos(window->DC.CursorPos);
    const ImVec2 item_size(item_width, row_item_height);
    ImRect bb(pos, pos + item_size);
    ImGui::ItemSize(item_size);
    if (ImGui::ItemAdd(bb, id))
    {
      bool held;
      bool hovered;
      bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held, 0);
      if (hovered)
        DrawMenuButtonFrame(bb.Min, bb.Max, held);

      bb.Min += style.FramePadding;
      bb.Max -= style.FramePadding;

      DrawGameListCover(entry, false, false, false, s_game_list_locals.localized_titles, dl,
                        ImRect(bb.Min, bb.Min + image_size));

      if (s_game_list_locals.show_trophy_icons)
      {
        GPUTexture* const cover_trophy = GetGameListCoverTrophy(entry, image_size);
        if (cover_trophy)
        {
          const ImVec2 trophy_size =
            ImVec2(static_cast<float>(cover_trophy->GetWidth()), static_cast<float>(cover_trophy->GetHeight()));
          dl->AddImage(cover_trophy, bb.Min + image_size - trophy_size, bb.Min + image_size, ImVec2(0.0f, 0.0f),
                       ImVec2(1.0f, 1.0f), IM_COL32(255, 255, 255, 255));
        }
      }

      if (s_game_list_locals.cover_titles)
      {
        const ImRect title_bb(ImVec2(bb.Min.x, bb.Min.y + image_height + title_spacing), bb.Max);
        RenderMultiLineShadowedTextClipped(dl, UIStyle.Font, title_font_size, title_font_weight, title_bb.Min,
                                           title_bb.Max, text_color,
                                           entry->GetDisplayTitle(s_game_list_locals.localized_titles),
                                           LAYOUT_CENTER_ALIGN_TEXT, image_width, &title_bb);
      }

      if (pressed)
      {
        HandleGameListActivate(entry);
      }
      else if (hovered &&
               (ImGui::IsItemClicked(ImGuiMouseButton_Right) ||
                ImGui::IsKeyPressed(ImGuiKey_NavGamepadContextMenu, false) || ImGui::IsKeyPressed(ImGuiKey_F1, false)))
      {
        CancelPendingMenuClose();
        HandleGameListOptions(entry);
      }
    }
    else
    {
      if (pos.y >= prefetch_range.x && pos.y <= prefetch_range.y)
        GetGameListCover(entry, false, false, false);
    }

    if (entry == s_game_list_locals.game_list_sorted_entries.front())
      ImGui::SetItemDefaultFocus();
    else if (entry == s_game_list_locals.game_list_sorted_entries.back())
      break;

    grid_x++;
    if (grid_x == grid_count_x)
    {
      grid_x = 0;
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + x_padding);
    }
    else
    {
      ImGui::SameLine();
    }
  }

  EndMenuButtons();
  EndFullscreenWindow();
}

void FullscreenUI::DrawCarouselCoverCallback(const ImDrawList* parent_list, const ImDrawCmd* cmd, u32 base_vertex,
                                             u32 base_index)
{
  // callback data buffer isn't necessarily aligned
  CarouselDrawData data;
  std::memcpy(&data, cmd->UserCallbackData, sizeof(data));
  data.push_constants.mvp = ImGuiManager::GetCurrentProjectionMatrix() * data.push_constants.mvp;

  g_gpu_device->SetPipeline(GetCarouselPipeline());
  g_gpu_device->SetTextureSampler(0, data.texture, g_gpu_device->GetLinearSampler());
  g_gpu_device->DrawWithPushConstants(4, 0, &data.push_constants, sizeof(data.push_constants));
}

void FullscreenUI::DrawGameCarousel(const ImVec2& heading_size)
{
  ImGuiIO& io = ImGui::GetIO();
  if (!BeginFullscreenWindow(
        ImVec2(0.0f, heading_size.y),
        ImVec2(io.DisplaySize.x, io.DisplaySize.y - heading_size.y - LayoutScale(LAYOUT_FOOTER_HEIGHT)),
        "game_carousel", GetTransparentBackgroundColor(), 0.0f, ImVec2(),
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
  {
    EndFullscreenWindow();
    return;
  }

  if (ImGui::IsWindowFocused() && WantsToCloseMenu())
  {
    BeginTransition(TransitionEffect::ZoomOut, DEFAULT_TRANSITION_TIME,
                    []() { SwitchToMainWindow(MainWindowType::Landing); });
  }

  ImGuiWindow* const window = ImGui::GetCurrentWindow();
  ImDrawList* const dl = ImGui::GetWindowDrawList();
  const ImVec2 window_pos = window->Pos;
  const ImVec2 window_size = window->Size;
  const u32 text_color = ImGui::GetColorU32(ImGuiCol_Text);
  const auto& entries = s_game_list_locals.game_list_sorted_entries;
  if (entries.empty())
  {
    RenderShadowedTextClipped(dl, UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, window_pos,
                              window_pos + window_size, text_color, FSUI_VSTR("No games found."), nullptr,
                              ImVec2(0.5f, 0.5f));
    EndFullscreenWindow();
    return;
  }

  const s32 count = static_cast<s32>(entries.size());
  const float max_position = static_cast<float>(count - 1);
  s32& index = s_game_list_locals.carousel_index;
  float& position = s_game_list_locals.carousel_position;
  index = std::clamp(index, 0, count - 1);
  position = std::clamp(position, 0.0f, max_position);

  // Cover layout, in screen pixels. The camera looks at the middle of the centre cover.
  static constexpr float SIDE_ANGLE = 70.0f * static_cast<float>(std::numbers::pi / 180.0);
  static constexpr u32 MAX_SIDE_COVERS = 20;
  const float cover_size = std::min(window_size.y * 0.5f, LayoutScale(400.0f));
  const float centre_gap = cover_size * 0.75f;
  const float side_spacing = cover_size * 0.22f;
  const float side_depth = cover_size * 0.6f;
  const float focal_length = cover_size * 3.0f;
  const float centre_x = window_pos.x + window_size.x * 0.5f;
  const float floor_y = window_pos.y + window_size.y * 0.62f;
  const float centre_y = floor_y - cover_size * 0.5f;
  const float reflection_size = 0.35f;
  const u32 side_count = std::min(
    static_cast<u32>(std::ceil(std::max(window_size.x * 0.5f - centre_gap, 0.0f) / side_spacing)) + 2, MAX_SIDE_COVERS);

  // Generated artwork is rendered at the size of the centre cover.
  const u32 generated_artwork_size = static_cast<u32>(std::ceil(cover_size));
  const bool generated_artwork_flip = g_gpu_device->UsesLowerLeftOrigin();

  // One item covering the whole carousel, so that nav activation and mouse drag/clicks go through ImGui.
  ResetFocusHere();
  const ImGuiID id = window->GetID("carousel");
  const ImRect bb(window_pos, window_pos + window_size);
  ImGui::ItemSize(bb);
  bool hovered = false, held = false, pressed = false;
  if (ImGui::ItemAdd(bb, id))
    pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held, 0);
  ImGui::SetItemDefaultFocus();

  const bool can_navigate = ImGui::IsWindowFocused() && !AreAnyDialogsOpen();
  const s32 prev_index = index;
  if (can_navigate)
  {
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, true) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true))
      index--;
    else if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, true) || ImGui::IsKeyPressed(ImGuiKey_RightArrow, true))
      index++;
    else if (ImGui::IsKeyPressed(ImGuiKey_GamepadL2, true) || ImGui::IsKeyPressed(ImGuiKey_PageUp, true))
      index -= 10;
    else if (ImGui::IsKeyPressed(ImGuiKey_GamepadR2, true) || ImGui::IsKeyPressed(ImGuiKey_PageDown, true))
      index += 10;
    else if (ImGui::IsKeyPressed(ImGuiKey_Home, false))
      index = 0;
    else if (ImGui::IsKeyPressed(ImGuiKey_End, false))
      index = count - 1;

    // only keyboard/gamepad moves make a sound, not the mouse
    index = std::clamp(index, 0, count - 1);
    if (index != prev_index)
      EnqueueSoundEffect(SFX_NAV_MOVE);
  }

  if (ImGui::IsWindowHovered() && !AreAnyDialogsOpen())
  {
    s_game_list_locals.carousel_wheel_accum -= io.MouseWheel + io.MouseWheelH;
    const float steps = std::trunc(s_game_list_locals.carousel_wheel_accum);
    s_game_list_locals.carousel_wheel_accum -= steps;
    index += static_cast<s32>(steps);
  }

  // Pressing while it's moving stops it, without activating anything.
  if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
  {
    s_game_list_locals.carousel_caught = (std::abs(position - static_cast<float>(index)) > 0.5f);
    if (s_game_list_locals.carousel_caught)
      index = static_cast<s32>(std::round(position));
  }

  // Dragging moves the carousel directly. On release, a fling picks a cover further along, which is eased to like any
  // other move.
  const float drag_item_width = centre_gap * 0.5f;
  ImVec2 fling_distance;
  const bool dragging = GetWindowDrag(&fling_distance);
  if (dragging)
  {
    position = std::clamp(position - io.MouseDelta.x / drag_item_width, 0.0f, max_position);
    index = static_cast<s32>(std::round(position + fling_distance.x / drag_item_width));

    // animate from wherever it's released
    s_game_list_locals.carousel_animation_target = -1;
  }

  index = std::clamp(index, 0, count - 1);

  // Dragging sets the position directly. Changing the target part way through starts a new animation from the current
  // position.
  if (!UIStyle.Animations && !dragging)
  {
    position = static_cast<float>(index);
  }
  else if (!dragging)
  {
    static constexpr float ANIMATION_TIME = 0.5f;
    if (index != s_game_list_locals.carousel_animation_target)
    {
      s_game_list_locals.carousel_animation_target = index;
      s_game_list_locals.carousel_animation_start = position;
      s_game_list_locals.carousel_animation_time = 0.0f;
    }

    s_game_list_locals.carousel_animation_time += io.DeltaTime;
    const float t = s_game_list_locals.carousel_animation_time / ANIMATION_TIME;
    position = (t >= 1.0f) ?
                 static_cast<float>(index) :
                 std::lerp(s_game_list_locals.carousel_animation_start, static_cast<float>(index), Easing::OutExpo(t));
  }

  // Covers are drawn back-to-front, since there's no depth buffer.
  const s32 centre_index = static_cast<s32>(std::round(position));
  const s32 first_index = std::max(centre_index - static_cast<s32>(side_count), 0);
  const s32 last_index = std::min(centre_index + static_cast<s32>(side_count), count - 1);
  std::array<s32, MAX_SIDE_COVERS * 2 + 1> draw_order;
  u32 draw_count = 0;
  for (s32 i = first_index; i <= last_index; i++)
    draw_order[draw_count++] = i;
  std::sort(draw_order.begin(), draw_order.begin() + draw_count, [&](s32 lhs, s32 rhs) {
    return std::abs(static_cast<float>(lhs) - position) > std::abs(static_cast<float>(rhs) - position);
  });

  // Pixel-space perspective projection, the ImGui projection is applied in the callback.
  const GSMatrix4x4 projection(1.0f, 0.0f, centre_x / focal_length, centre_x, 0.0f, 1.0f, centre_y / focal_length,
                               centre_y, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f / focal_length, 1.0f);
  const float global_alpha = ImGui::GetStyle().Alpha;

  std::array<ImRect, MAX_SIDE_COVERS * 2 + 1> cover_rects;
  for (u32 draw_index = 0; draw_index < draw_count; draw_index++)
  {
    const s32 entry_index = draw_order[draw_index];
    const GameList::Entry* const entry = entries[entry_index];

    // empty rect, so it's not hit by the mouse
    ImRect& rect = cover_rects[draw_index];
    rect = ImRect(ImVec2(FLT_MAX, FLT_MAX), ImVec2(-FLT_MAX, -FLT_MAX));

    GPUTexture* texture = GetGameListCover(entry, false, false, false);
    const bool generated = !texture;
    if (generated)
    {
      texture = GetGeneratedArtwork(entry->GetDisplayTitle(s_game_list_locals.localized_titles), entry->type, false,
                                    generated_artwork_size);
      if (!texture)
        continue;
    }

    const float d = static_cast<float>(entry_index) - position;
    const float side_t = std::clamp(d, -1.0f, 1.0f);
    const float abs_d = std::abs(d);
    const float x = (abs_d <= 1.0f) ? (d * centre_gap) : (std::copysign(centre_gap + (abs_d - 1.0f) * side_spacing, d));
    const float z = std::abs(side_t) * side_depth;

    // fit cover into the square, bottom-aligned
    const float aspect = static_cast<float>(texture->GetWidth()) / static_cast<float>(texture->GetHeight());
    const float width = (aspect >= 1.0f) ? cover_size : (cover_size * aspect);
    const float height = (aspect >= 1.0f) ? (cover_size / aspect) : cover_size;

    const GSMatrix4x4 placement =
      projection * GSMatrix4x4::Translation(x, floor_y - centre_y, z) * GSMatrix4x4::RotationY(SIDE_ANGLE * side_t);
    const GSMatrix4x4 cover_mvp = placement * GSMatrix4x4(width, 0.0f, 0.0f, 0.0f, 0.0f, height, 0.0f, 0.0f, 0.0f, 0.0f,
                                                          1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    const GSMatrix4x4 reflection_mvp = placement * GSMatrix4x4(width, 0.0f, 0.0f, 0.0f, 0.0f, -height, 0.0f, 0.0f, 0.0f,
                                                               0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);

    // darken covers as they move to the side, and further with distance
    const float brightness = std::max(1.0f - 0.5f * std::min(abs_d, 1.0f) - 0.05f * std::max(abs_d - 1.0f, 0.0f), 0.2f);
    CarouselDrawData data = {};
    data.texture = texture;
    data.push_constants.mvp = reflection_mvp;
    data.push_constants.color[0] = data.push_constants.color[1] = data.push_constants.color[2] = brightness;
    data.push_constants.color[3] = 0.3f * global_alpha;
    data.push_constants.fade_start = 1.0f - reflection_size;
    data.push_constants.fade_scale = 1.0f / reflection_size;

    // corner radius is relative to the shorter side, so it's the same as generated artwork in pixels
    const float corner_radius = GameList::GeneratedArtwork::COVER_CORNER_RADIUS * std::min(width, height);
    data.push_constants.corner_radius[0] = corner_radius / width;
    data.push_constants.corner_radius[1] = corner_radius / height;
    data.push_constants.flip_y = (generated && generated_artwork_flip) ? 1.0f : 0.0f;
    dl->AddCallback(&DrawCarouselCoverCallback, &data, sizeof(data));

    data.push_constants.mvp = cover_mvp;
    data.push_constants.color[3] = global_alpha;
    data.push_constants.fade_scale = 0.0f;
    dl->AddCallback(&DrawCarouselCoverCallback, &data, sizeof(data));

    // projected bounds for hit testing
    for (const GSVector4& corner : {GSVector4(-0.5f, -1.0f, 0.0f, 1.0f), GSVector4(0.5f, -1.0f, 0.0f, 1.0f),
                                    GSVector4(-0.5f, 0.0f, 0.0f, 1.0f), GSVector4(0.5f, 0.0f, 0.0f, 1.0f)})
    {
      const GSVector4 projected = cover_mvp * corner;
      rect.Add(ImVec2(projected.x / projected.w, projected.y / projected.w));
    }
  }

  // Covers off-screen are prefetched so they're ready when scrolling.
  for (s32 i = std::max(first_index - 5, 0); i <= std::min(last_index + 5, count - 1); i++)
  {
    if (i < first_index || i > last_index)
      GetGameListCover(entries[i], false, false, false);
  }

  // Hit test front-to-back for mouse clicks.
  s32 mouse_index = -1;
  if (hovered)
  {
    for (u32 draw_index = draw_count; draw_index > 0; draw_index--)
    {
      if (cover_rects[draw_index - 1].Contains(io.MousePos))
      {
        mouse_index = draw_order[draw_index - 1];
        break;
      }
    }
  }

  const GameList::Entry* const selected_entry = entries[index];
  if (pressed && !(s_game_list_locals.carousel_caught && ImGui::IsMouseReleased(ImGuiMouseButton_Left)))
  {
    if (!ImGui::IsMouseReleased(ImGuiMouseButton_Left) || mouse_index == index)
    {
      HandleGameListActivate(selected_entry);
    }
    else if (mouse_index >= 0)
    {
      index = mouse_index;
    }
  }
  else if (hovered && mouse_index >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
  {
    // right-clicking empty space goes back, like the grid and list
    CancelPendingMenuClose();
    HandleGameListOptions(entries[mouse_index]);
  }
  else if (can_navigate &&
           (ImGui::IsKeyPressed(ImGuiKey_NavGamepadContextMenu, false) || ImGui::IsKeyPressed(ImGuiKey_F1, false)))
  {
    CancelPendingMenuClose();
    HandleGameListOptions(selected_entry);
  }

  // Trophy icon on the centre cover, once it's settled.
  if (s_game_list_locals.show_trophy_icons && std::abs(static_cast<float>(index) - position) < 0.05f &&
      cover_rects[draw_count - 1].Min.x < cover_rects[draw_count - 1].Max.x)
  {
    const ImRect& rect = cover_rects[draw_count - 1];
    GPUTexture* const cover_trophy = GetGameListCoverTrophy(selected_entry, rect.GetSize());
    if (cover_trophy)
    {
      const ImVec2 trophy_size =
        ImVec2(static_cast<float>(cover_trophy->GetWidth()), static_cast<float>(cover_trophy->GetHeight()));
      dl->AddImage(cover_trophy, rect.Max - trophy_size, rect.Max);
    }
  }

  // Details for the game either side of the carousel position, in a card at the bottom. Cards slide and fade with the
  // carousel position, so they follow the covers and are only fully visible once it settles.
  const float line_spacing = LayoutScale(4.0f);
  const float chip_height = LayoutScale(24.0f);
  const float panel_margin = LayoutScale(24.0f);
  const float panel_padding = LayoutScale(20.0f);
  const ImVec2 flag_size = LayoutScale(23.0f, 16.0f);
  const float flag_spacing = LayoutScale(6.0f);
  const float card_slide_distance = LayoutScale(100.0f);
  const auto draw_card = [&dl, &window_pos, &window_size, &global_alpha, &line_spacing, &chip_height, &panel_margin,
                          &panel_padding, &flag_size,
                          &flag_spacing](const GameList::Entry* entry, float card_x, float card_alpha) {
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, global_alpha * card_alpha);
    const GameDatabase::Entry* const dbentry = entry->dbentry;
    const std::string_view title = entry->GetDisplayTitle(s_game_list_locals.localized_titles);
    const u32 chip_color = ImGui::GetColorU32(UIStyle.SurfaceRaisedColor);
    const u32 image_color = ImGui::GetColorU32(IM_COL32(255, 255, 255, 255));

    InlineRun title_run(0.0f);
    title_run.Text(UIStyle.HeadingText.WithWeight(UIStyle.ExtraBoldFontWeight), title, InlineRun::Shrink);

    InlineRun developer_run(0.0f);
    if (dbentry)
    {
      if (!dbentry->developer.empty())
        developer_run.Text(UIStyle.MutedText, dbentry->developer, InlineRun::Shrink);

      std::optional<std::tm> release_date;
      if (dbentry->release_date != 0 &&
          (release_date = Common::LocalTime(static_cast<std::time_t>(dbentry->release_date))).has_value())
      {
        if (!dbentry->developer.empty())
          developer_run.Text(UIStyle.MutedText, " " ICON_EMOJI_MIDDLE_DOT " ");
        developer_run.TextFmt(UIStyle.MutedText, "{}", release_date->tm_year + 1900);
      }
    }

    // region pill has the flag before the text, so it's drawn once the line is laid out
    InlineRun chips_run(LayoutScale(10.0f));
    const std::string_view region_name = Settings::GetDiscRegionDisplayName(entry->region);
    const u32 region_item = chips_run.Space(
      MeasurePill(region_name, chip_height, UIStyle.SmallMutedText).x + flag_size.x + flag_spacing, chip_height);
    if (!entry->serial.empty())
      chips_run.Pill(entry->serial, UIStyle.SmallMutedText, chip_color, chip_height);
    if (entry->num_achievements > 0)
    {
      if (entry->unlocked_achievements_hc > 0)
      {
        chips_run.PillFmt(UIStyle.SmallMutedText, chip_color, chip_height, -1.0f, ICON_FA_TROPHY " {} ({}) / {}",
                          entry->unlocked_achievements, entry->unlocked_achievements_hc, entry->num_achievements);
      }
      else
      {
        chips_run.PillFmt(UIStyle.SmallMutedText, chip_color, chip_height, -1.0f, ICON_FA_TROPHY " {} / {}",
                          entry->unlocked_achievements, entry->num_achievements);
      }
    }

    InlineRun played_run(LayoutScale(10.0f));
    if (entry->total_played_time > 0)
    {
      played_run.PillFmt(UIStyle.SmallMutedText, chip_color, chip_height, -1.0f, ICON_FA_HOURGLASS_HALF " {}",
                         Host::FormatTimespan(entry->total_played_time, true));
      played_run.PillFmt(UIStyle.SmallMutedText, chip_color, chip_height, -1.0f, ICON_FA_CLOCK " {}",
                         Host::FormatRelativeDate(entry->last_played_time, false, true));
    }

    // Panel fits the text, lines that aren't present are dropped, and the icon is the height of the text.
    const bool has_developer_line = (developer_run.GetWidth() > 0.0f);
    const bool has_played_line = (played_run.GetWidth() > 0.0f);
    const float text_width =
      std::max({title_run.GetWidth(), developer_run.GetWidth(), chips_run.GetWidth(), played_run.GetWidth()});
    const float text_height = UIStyle.HeadingText.font_size + line_spacing + chip_height +
                              (has_developer_line ? (UIStyle.MutedText.font_size + line_spacing) : 0.0f) +
                              (has_played_line ? (chip_height + line_spacing) : 0.0f);
    const float icon_size = (s_game_list_locals.icon_source != GameList::IconSource::None) ? text_height : 0.0f;
    const float icon_spacing = (icon_size > 0.0f) ? LayoutScale(12.0f) : 0.0f;
    const float panel_width =
      std::min(text_width + icon_size + icon_spacing + panel_padding * 2.0f, window_size.x - panel_margin * 2.0f);
    const float panel_bottom = window_pos.y + window_size.y - panel_margin;
    const ImRect panel_rect(
      ImFloor(ImVec2(card_x - panel_width * 0.5f, panel_bottom - text_height - panel_padding * 2.0f)),
      ImFloor(ImVec2(card_x + panel_width * 0.5f, panel_bottom)));
    if (icon_size > 0.0f)
    {
      const ImVec2 icon_pos =
        ImFloor(ImVec2(panel_rect.Min.x + panel_padding, panel_rect.GetCenter().y - icon_size * 0.5f));
      const ImRect icon_rect(icon_pos, icon_pos + ImVec2(icon_size, icon_size));

      // The carousel shows the cover already, so it uses memory card icons instead.
      DrawGameListIcon(entry, title, dl, icon_rect, image_color,
                       (s_game_list_locals.icon_source == GameList::IconSource::Cover) ?
                         std::make_optional(GameList::IconSource::MemoryCard) :
                         std::nullopt);
    }

    const float line_left = panel_rect.Min.x + panel_padding + icon_size + icon_spacing;
    const float line_right = panel_rect.Max.x - panel_padding;
    float line_y = panel_rect.Min.y + panel_padding;
    const auto draw_line = [&dl, &line_left, &line_right, &line_y, &line_spacing](InlineRun& run, float height) {
      run.Draw(dl, ImRect(line_left, line_y, line_right, line_y + height));
      line_y += height + line_spacing;
    };
    draw_line(title_run, UIStyle.HeadingText.font_size);
    if (has_developer_line)
      draw_line(developer_run, UIStyle.MutedText.font_size);
    draw_line(chips_run, chip_height);
    if (has_played_line)
      draw_line(played_run, chip_height);

    const ImRect& region_rect = chips_run.GetItemRect(region_item);
    dl->AddRectFilled(region_rect.Min, region_rect.Max, chip_color, chip_height * 0.5f);
    const ImVec2 flag_pos =
      ImFloor(ImVec2(region_rect.Min.x + chip_height * 0.4f, region_rect.GetCenter().y - flag_size.y * 0.5f));
    dl->AddImage(GetCachedTexture(entry->GetLanguageIconName(), flag_size), flag_pos, flag_pos + flag_size,
                 ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), image_color);
    RenderText(dl,
               ImFloor(ImVec2(flag_pos.x + flag_size.x + flag_spacing,
                              region_rect.GetCenter().y - MeasureText(UIStyle.SmallMutedText, region_name).y * 0.5f)),
               UIStyle.SmallMutedText, region_name);

    ImGui::PopStyleVar();
  };

  // Full-width band behind the cards, sized for the tallest card so it doesn't change between games. It fades in
  // above the card, and is most opaque at the bottom where the text is.
  {
    const float max_text_height =
      UIStyle.HeadingText.font_size + UIStyle.MutedText.font_size + chip_height * 2.0f + line_spacing * 4.0f;
    const float band_bottom = io.DisplaySize.y;
    const float band_mid = window_pos.y + window_size.y - panel_margin - max_text_height - panel_padding * 2.0f;
    const float band_top = band_mid - LayoutScale(64.0f);
    const u32 band_top_color = ImGui::GetColorU32(ModAlpha(UIStyle.SurfaceColor, 0.0f));
    const u32 band_mid_color = ImGui::GetColorU32(ModAlpha(UIStyle.SurfaceColor, 0.75f));
    const u32 band_bottom_color = ImGui::GetColorU32(ModAlpha(UIStyle.SurfaceColor, 0.85f));
    // continues under the footer, so it isn't clipped to the window
    dl->PushClipRect(ImVec2(0.0f, 0.0f), io.DisplaySize, false);
    dl->AddRectFilledMultiColor(ImVec2(0.0f, band_top), ImVec2(io.DisplaySize.x, band_mid), band_top_color,
                                band_top_color, band_mid_color, band_mid_color);
    dl->AddRectFilledMultiColor(ImVec2(0.0f, band_mid), ImVec2(io.DisplaySize.x, band_bottom), band_mid_color,
                                band_mid_color, band_bottom_color, band_bottom_color);
    dl->PopClipRect();
  }

  const s32 first_card = static_cast<s32>(std::floor(position));
  for (s32 card_index = first_card; card_index <= std::min(first_card + 1, count - 1); card_index++)
  {
    const float card_offset = static_cast<float>(card_index) - position;
    const float card_alpha = 1.0f - std::abs(card_offset) * 2.0f;
    if (card_alpha > 0.0f)
      draw_card(entries[card_index], centre_x + card_offset * card_slide_distance, card_alpha);
  }

  EndFullscreenWindow(false, false);
}

void FullscreenUI::HandleGameListActivate(const GameList::Entry* entry)
{
  if (entry->IsDiscSet())
  {
    HandleSelectDiscForDiscSet(entry->dbentry->disc_set);
    return;
  }

  // launch game
  if (!OpenLoadStateSelectorForGameResume(entry))
    DoStartPath(entry->path);
}

void FullscreenUI::HandleGameListOptions(const GameList::Entry* entry)
{
  if (entry->IsDiscSet())
  {
    ChoiceDialogOptions options = {
      {FSUI_ICONSTR(ICON_FA_WRENCH, "Game Properties"), false},
      {FSUI_ICONSTR(ICON_FA_IMAGE, "Set Cover Image"), false},
      {FSUI_ICONSTR(ICON_FA_COMPACT_DISC, "Select Disc"), false},
    };

    const GameDatabase::DiscSetEntry* dsentry = entry->dbentry->disc_set;
    OpenChoiceDialog(entry->GetDisplayTitle(s_game_list_locals.localized_titles), false, std::move(options),
                     [dsentry](s32 index, const std::string& title, bool checked) mutable {
                       switch (index)
                       {
                         case 0: // Open Game Properties
                           BeginTransition([dsentry]() {
                             // shouldn't fail
                             const GameList::Entry* first_disc_entry = GameList::GetFirstDiscSetMember(dsentry);
                             if (!first_disc_entry)
                               return;

                             SwitchToGameSettingsForPath(first_disc_entry->path);
                           });
                           break;
                         case 1: // Set Cover Image
                           DoSetCoverImage(std::string(dsentry->GetSaveTitle()));
                           break;
                         case 2: // Select Disc
                           HandleSelectDiscForDiscSet(dsentry);
                           break;
                         default:
                           break;
                       }
                     });
  }
  else if (entry->IsGame())
  {
    ChoiceDialogOptions options = {
      {FSUI_ICONSTR(ICON_FA_WRENCH, "Game Properties"), false},
      {FSUI_ICONSTR(ICON_FA_FOLDER_OPEN, "Open Containing Directory"), false},
      {FSUI_ICONSTR(ICON_FA_IMAGE, "Set Cover Image"), false},
      {FSUI_ICONSTR(ICON_FA_PLAY, "Resume Game"), false},
      {FSUI_ICONSTR(ICON_FA_ARROW_ROTATE_LEFT, "Load State"), false},
      {FSUI_ICONSTR(ICON_FA_COMPACT_DISC, "Default Boot"), false},
      {FSUI_ICONSTR(ICON_FA_BOLT, "Fast Boot"), false},
      {FSUI_ICONSTR(ICON_FA_HOURGLASS, "Slow Boot"), false},
      {FSUI_ICONSTR(ICON_FA_DELETE_LEFT, "Reset Play Time"), false},
    };

    OpenChoiceDialog(entry->GetDisplayTitle(s_game_list_locals.localized_titles), false, std::move(options),
                     [entry_path = entry->path, entry_serial = entry->serial](s32 index, const std::string& title,
                                                                              bool checked) mutable {
                       switch (index)
                       {
                         case 0: // Open Game Properties
                           BeginTransition(
                             TransitionEffect::ZoomIn, DEFAULT_TRANSITION_TIME,
                             [entry_path = std::move(entry_path)]() { SwitchToGameSettingsForPath(entry_path); });
                           break;
                         case 1: // Open Containing Directory
                           ExitFullscreenAndOpenURL(Path::CreateFileURL(Path::GetDirectory(entry_path)));
                           break;
                         case 2: // Set Cover Image
                           DoSetCoverImage(std::move(entry_path));
                           break;
                         case 3: // Resume Game
                           DoStartPath(entry_path, System::GetGameSaveStatePath(entry_serial, -1));
                           break;
                         case 4: // Load State
                           OpenSaveStateSelector(entry_serial, entry_path, true);
                           break;
                         case 5: // Default Boot
                           DoStartPath(entry_path);
                           break;
                         case 6: // Fast Boot
                           DoStartPath(entry_path, {}, true);
                           break;
                         case 7: // Slow Boot
                           DoStartPath(entry_path, {}, false);
                           break;
                         case 8: // Reset Play Time
                           GameList::ClearPlayedTimeForSerial(entry_serial);
                           break;
                         default:
                           break;
                       }
                     });
  }
  else
  {
    ChoiceDialogOptions options = {
      {FSUI_ICONSTR(ICON_FA_FOLDER_OPEN, "Open Containing Directory"), false},
      {FSUI_ICONSTR(ICON_FA_IMAGE, "Set Cover Image"), false},
      {FSUI_ICONSTR(ICON_FA_COMPACT_DISC, "Default Boot"), false},
      {FSUI_ICONSTR(ICON_FA_BOLT, "Fast Boot"), false},
      {FSUI_ICONSTR(ICON_FA_HOURGLASS, "Slow Boot"), false},
    };

    OpenChoiceDialog(entry->GetDisplayTitle(s_game_list_locals.localized_titles), false, std::move(options),
                     [entry_path = entry->path](s32 index, const std::string& title, bool checked) mutable {
                       switch (index)
                       {
                         case 0: // Open Containing Directory
                           ExitFullscreenAndOpenURL(Path::CreateFileURL(Path::GetDirectory(entry_path)));
                           break;
                         case 1: // Set Cover Image
                           DoSetCoverImage(std::move(entry_path));
                           break;
                         case 2: // Default Boot
                           DoStartPath(entry_path);
                           break;
                         case 3: // Fast Boot
                           DoStartPath(entry_path, {}, true);
                           break;
                         case 4: // Slow Boot
                           DoStartPath(entry_path, {}, false);
                           break;
                         default:
                           break;
                       }
                     });
  }
}

void FullscreenUI::HandleSelectDiscForDiscSet(const GameDatabase::DiscSetEntry* dsentry)
{
  auto lock = GameList::GetLock();
  const std::vector<const GameList::Entry*> entries = GameList::GetDiscSetMembers(dsentry, true);
  if (entries.empty())
    return;

  ChoiceDialogOptions options;
  std::vector<std::string> paths;
  paths.reserve(entries.size());

  for (u32 i = 0; i < static_cast<u32>(entries.size()); i++)
  {
    const GameList::Entry* const entry = entries[i];
    std::string title = fmt::format(ICON_FA_COMPACT_DISC " {} {} \u2022 {}##{}", FSUI_VSTR("Disc"),
                                    entry->disc_set_index + 1, Path::GetFileName(entry->path), i);
    options.emplace_back(std::move(title), false);
    paths.push_back(entry->path);
  }
  options.emplace_back(FSUI_ICONVSTR(ICON_FA_SQUARE_XMARK, "Close Menu"), false);

  const GameList::Entry* dsgentry = GameList::GetEntryForPath(dsentry->GetSaveTitle());
  OpenChoiceDialog(fmt::format(FSUI_FSTR("Select Disc for {}"),
                               dsgentry ? dsgentry->GetDisplayTitle(s_game_list_locals.localized_titles) :
                                          dsentry->GetDisplayTitle(s_game_list_locals.localized_titles)),
                   false, std::move(options),
                   [paths = std::move(paths)](s32 index, const std::string& title, bool checked) {
                     if (static_cast<u32>(index) >= paths.size())
                       return;

                     auto lock = GameList::GetLock();
                     const GameList::Entry* entry = GameList::GetEntryForPath(paths[index]);
                     if (entry)
                       HandleGameListActivate(entry);
                   });
}

void FullscreenUI::SwitchToGameList()
{
  ReloadGameListSettings();

  s_game_list_locals.game_list_current_selection_path = {};
  s_game_list_locals.game_list_current_selection_timeout = 0.0f;
  s_game_list_locals.game_list_search_string[0] = '\0';
  s_game_list_locals.search_expand_progress = 0.0f;

  // Wipe icon map, because a new save might give us an icon.
  for (const auto& it : s_game_list_locals.icon_image_map)
  {
    if (!it.second.empty())
      InvalidateCachedTexture(it.second);
  }
  s_game_list_locals.icon_image_map.clear();

  SwitchToMainWindow(MainWindowType::GameList);
}

void FullscreenUI::ReloadGameListSettings()
{
  const GameList::IconSource icon_source =
    GameList::ParseIconSourceName(
      Core::GetBaseTinyStringSettingValue(GameList::UI_SECTION_NAME, GameList::SETTING_KEY_ICON_SOURCE,
                                          GameList::GetIconSourceName(GameList::DEFAULT_ICON_SOURCE)))
      .value_or(GameList::DEFAULT_ICON_SOURCE);
  if (s_game_list_locals.icon_source != icon_source)
  {
    // Icon paths depend on the source.
    s_game_list_locals.icon_source = icon_source;
    s_game_list_locals.icon_image_map.clear();
  }

  s_game_list_locals.sort_column =
    GameList::ParseColumnName(
      Core::GetBaseStringSettingValue(GameList::UI_SECTION_NAME, GameList::SETTING_KEY_SORT_COLUMN))
      .value_or(GameList::DEFAULT_SORT_COLUMN);
  s_game_list_locals.sort_reversed =
    Core::GetBaseBoolSettingValue(GameList::UI_SECTION_NAME, GameList::SETTING_KEY_SORT_REVERSED, false);
  s_game_list_locals.localized_titles = Core::GetBaseBoolSettingValue(
    GameList::UI_SECTION_NAME, GameList::SETTING_KEY_LOCALIZED_TITLES, GameList::DEFAULT_LOCALIZED_TITLES);
  s_game_list_locals.merge_disc_sets = Core::GetBaseBoolSettingValue(
    GameList::UI_SECTION_NAME, GameList::SETTING_KEY_MERGE_DISC_SETS, GameList::DEFAULT_MERGE_DISC_SETS);
  s_game_list_locals.view = ParseGameListViewName(Core::GetBaseStringSettingValue(
                                                    GameList::UI_SECTION_NAME, GameList::SETTING_KEY_FULLSCREENUI_VIEW))
                              .value_or(DEFAULT_GAME_LIST_VIEW);
  s_game_list_locals.cover_titles = Core::GetBaseBoolSettingValue(
    GameList::UI_SECTION_NAME, GameList::SETTING_KEY_COVER_TITLES, GameList::DEFAULT_COVER_TITLES);
  s_game_list_locals.compact_mode =
    Core::GetBaseBoolSettingValue(GameList::UI_SECTION_NAME, "GameListCompactMode", true);
  s_game_list_locals.show_trophy_icons =
    Core::GetBaseBoolSettingValue(GameList::UI_SECTION_NAME, "GameListGridTrophyIcons", true);
}

GPUTexture* FullscreenUI::GetGameListCover(const GameList::Entry* entry, bool fallback_to_achievements_icon,
                                           bool fallback_to_icon, bool return_default_image)
{
  // lookup and grab cover image
  auto cover_it = s_game_list_locals.cover_image_map.find(entry->path);
  if (cover_it == s_game_list_locals.cover_image_map.end())
  {
    std::string cover_path = GameList::GetCoverImagePathForEntry(entry);
    cover_it = s_game_list_locals.cover_image_map.emplace(entry->path, std::move(cover_path)).first;

    // try achievements image before memcard icon
    if (fallback_to_achievements_icon && cover_it->second.empty() && Achievements::IsActive())
    {
      if (VideoThread::GetGamePath() == entry->path)
      {
        const auto lock = Achievements::GetLock();
        cover_it->second = Achievements::GetCurrentGameBadgeURL();
      }
    }
  }

  // because memcard icons are crap res
  if (fallback_to_icon && cover_it->second.empty() && !entry->serial.empty() && entry->IsDiscOrDiscSet())
  {
    cover_it = s_game_list_locals.icon_image_map.find(entry->serial);
    if (cover_it == s_game_list_locals.icon_image_map.end())
    {
      // This is only a fallback for the missing cover, so still use memory card icons if the list doesn't.
      const GameList::IconSource icon_source = (s_game_list_locals.icon_source == GameList::IconSource::Achievements) ?
                                                 GameList::IconSource::Achievements :
                                                 GameList::IconSource::MemoryCard;
      std::string icon_path = GameList::GetGameIconPath(entry, icon_source);
      cover_it = s_game_list_locals.icon_image_map.emplace(entry->serial, std::move(icon_path)).first;
    }
  }

  GPUTexture* const tex = (!cover_it->second.empty()) ? GetCachedTextureAsync(cover_it->second) : nullptr;
  return tex ? tex : (return_default_image ? GetTextureForGameListEntryType(entry->type) : nullptr);
}

GPUTexture* FullscreenUI::GetGameListIcon(const GameList::Entry* entry)
{
  // We only do this for discs/disc sets for now.
  if (!entry->serial.empty() && entry->IsDiscOrDiscSet())
  {
    auto icon_it = s_game_list_locals.icon_image_map.find(entry->serial);
    if (icon_it == s_game_list_locals.icon_image_map.end())
    {
      // With covers configured only the carousel gets here, which shows the cover already, so use memory card icons.
      const GameList::IconSource icon_source = (s_game_list_locals.icon_source == GameList::IconSource::Cover) ?
                                                 GameList::IconSource::MemoryCard :
                                                 s_game_list_locals.icon_source;
      std::string icon_path = GameList::GetGameIconPath(entry, icon_source);
      icon_it = s_game_list_locals.icon_image_map.emplace(entry->serial, std::move(icon_path)).first;
    }

    if (!icon_it->second.empty())
      return GetCachedTextureAsync(icon_it->second);
  }

  // No icon, so the caller draws generated artwork instead.
  return nullptr;
}

GPUTexture* FullscreenUI::GetGameListCoverTrophy(const GameList::Entry* entry, const ImVec2& image_size)
{
  if (entry->num_achievements == 0)
    return nullptr;

  // this'll get re-scaled up, so undo layout scale
  const ImVec2 trophy_size = LayoutUnscale(image_size / 6.0f);

  GPUTexture* texture =
    GetCachedTextureAsync(entry->AreAchievementsMastered() ? "images/trophy-icon-star.svg" : "images/trophy-icon.svg",
                          static_cast<u32>(trophy_size.x), static_cast<u32>(trophy_size.y));

  // don't draw the placeholder, it's way too large
  return (texture == GetPlaceholderTexture()) ? nullptr : texture;
}

std::string_view FullscreenUI::GetKeyForGameListEntry(const GameList::Entry* entry)
{
  return entry->IsDiscSet() ? entry->GetDiscSetEntry()->GetSaveTitle() : std::string_view(entry->path);
}

GPUTexture* FullscreenUI::GetTextureForGameListEntryType(GameList::EntryType type)
{
  switch (type)
  {
    case GameList::EntryType::PSExe:
      return GetCachedTexture("fullscreenui/exe-file.png");

    case GameList::EntryType::Playlist:
      return GetCachedTexture("fullscreenui/playlist-file.png");

    case GameList::EntryType::PSF:
    case GameList::EntryType::AudioCD:
      return GetCachedTexture("fullscreenui/psf-file.png");

    case GameList::EntryType::Disc:
    default:
      return GetCachedTexture("fullscreenui/cdrom.png");
  }
}

void FullscreenUI::SetCoverCacheEntry(std::string path, std::string cover_path)
{
  s_game_list_locals.cover_image_map.insert_or_assign(std::move(path), std::move(cover_path));
}

void FullscreenUI::RemoveCoverCacheEntry(const std::string& path)
{
  if (path.empty())
    s_game_list_locals.cover_image_map.clear();
  else
    s_game_list_locals.cover_image_map.erase(path);
}

void FullscreenUI::InvalidateCoverCache(std::string path)
{
  if (!VideoThread::IsFullscreenUIRequested())
    return;

  VideoThread::RunOnThread([path = std::move(path)]() { RemoveCoverCacheEntry(path); });
}

void FullscreenUI::DrawGameListCover(const GameList::Entry* entry, bool fallback_to_achievements_icon,
                                     bool fallback_to_icon, bool icon_style, bool show_localized_titles, ImDrawList* dl,
                                     const ImRect& rect)
{
  GPUTexture* const cover_texture = GetGameListCover(entry, fallback_to_achievements_icon, fallback_to_icon, false);
  if (!cover_texture)
  {
    DrawGeneratedArtwork(dl, rect, entry->GetDisplayTitle(show_localized_titles), entry->type, icon_style,
                         IM_COL32(255, 255, 255, 255));
    return;
  }

  const ImRect image_rect = CenterImage(
    rect, ImVec2(static_cast<float>(cover_texture->GetWidth()), static_cast<float>(cover_texture->GetHeight())));
  dl->AddImage(cover_texture, image_rect.Min, image_rect.Max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
               IM_COL32(255, 255, 255, 255));
}

void FullscreenUI::DrawGameListIcon(const GameList::Entry* entry, std::string_view title, ImDrawList* dl,
                                    const ImRect& rect, u32 color,
                                    std::optional<GameList::IconSource> icon_source_override)
{
  const GameList::IconSource icon_source = icon_source_override.value_or(s_game_list_locals.icon_source);
  if (icon_source == GameList::IconSource::None)
    return;

  // Missing covers fall back to the achievements badge or memory card icon, then generated artwork.
  GPUTexture* const texture =
    (icon_source == GameList::IconSource::Cover) ? GetGameListCover(entry, true, true, false) : GetGameListIcon(entry);
  if (!texture)
  {
    DrawGeneratedArtwork(dl, rect, title, entry->type, true, color);
    return;
  }

  const ImRect image_rect =
    CenterImage(rect, ImVec2(static_cast<float>(texture->GetWidth()), static_cast<float>(texture->GetHeight())));

  // Memory card icons are tiny, so don't blur them when scaling up.
  const bool nearest = (texture->GetWidth() <= 32 && texture->GetHeight() <= 32);
  if (nearest)
    ImGuiManager::PushNearestSampler(dl, texture);

  dl->AddImage(texture, image_rect.Min, image_rect.Max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), color);

  if (nearest)
    ImGuiManager::PopNearestSampler(dl);
}

void FullscreenUI::DrawGeneratedArtwork(ImDrawList* dl, const ImRect& rect, std::string_view title,
                                        GameList::EntryType type, bool icon_style, u32 color)
{
  using namespace GameList::GeneratedArtwork;

  // Rendered at the size it's drawn at.
  GPUTexture* const tex = GetGeneratedArtwork(title, type, icon_style, static_cast<u32>(std::ceil(rect.GetWidth())));
  if (!tex)
    return;

  // Lower-left origin APIs render the texture upside down.
  const bool flip = g_gpu_device->UsesLowerLeftOrigin();
  dl->AddImageRounded(tex, rect.Min, rect.Max, ImVec2(0.0f, flip ? 1.0f : 0.0f), ImVec2(1.0f, flip ? 0.0f : 1.0f),
                      color, rect.GetWidth() * (icon_style ? ICON_CORNER_RADIUS : COVER_CORNER_RADIUS));
}

GPUTexture* FullscreenUI::GetGeneratedArtwork(std::string_view title, GameList::EntryType type, bool icon_style,
                                              u32 tex_size)
{
  using namespace GameList::GeneratedArtwork;

  // Rendered once per size, and cached alongside the covers.
  const SmallString key = SmallString::from_format("{}{}:{}:{}:{}", GENERATED_ARTWORK_KEY_PREFIX,
                                                   icon_style ? 'i' : 'c', tex_size, static_cast<u32>(type), title);
  if (GPUTexture* const tex = FindCachedTexture(key))
    return tex;

  // Same format as the swap chain, since that's what the ImGui pipeline is compiled for.
  GPUSwapChain* const swap_chain = g_gpu_device->GetMainSwapChain();
  if (!swap_chain)
    return nullptr;

  Error error;
  GPUDevice::AutoRecycleTexture rt =
    g_gpu_device->FetchAutoRecycleTexture(tex_size, tex_size, 1, 1, 1, GPUTexture::Type::RenderTarget,
                                          swap_chain->GetFormat(), GPUTexture::Flags::None, nullptr, 0, &error);
  std::unique_ptr<GPUTexture> tex =
    rt ? g_gpu_device->FetchTexture(tex_size, tex_size, 1, 1, 1, GPUTexture::Type::Texture, rt->GetFormat(),
                                    GPUTexture::Flags::None, nullptr, 0, &error) :
         nullptr;
  if (!tex)
  {
    ERROR_LOG("Failed to allocate {}x{} generated artwork texture: {}", tex_size, tex_size, error.GetDescription());
    return nullptr;
  }

  DEV_LOG("Generating {}x{} {} artwork for '{}'", tex_size, tex_size, icon_style ? "icon" : "cover", title);

  // Drawn square, the corners are rounded when the texture is drawn.
  const ImGuiIO& io = ImGui::GetIO();
  const float size = static_cast<float>(tex_size);
  const ImVec2 rt_size(size, size);
  ImDrawList dl(ImGui::GetDrawListSharedData());
  dl._ResetForNewFrame();
  dl.PushTexture(io.Fonts->TexRef);
  dl.PushClipRect(ImVec2(0.0f, 0.0f), rt_size);

  // Diagonal gradient, filled in white and then shaded.
  const auto [start_color, end_color] = GetColors(title);
  dl.AddRectFilled(ImVec2(0.0f, 0.0f), rt_size, IM_COL32(255, 255, 255, 255));
  ImGui::ShadeVertsLinearColorGradientKeepAlpha(&dl, 0, dl.VtxBuffer.Size, ImVec2(0.0f, 0.0f), rt_size, start_color,
                                                end_color);

  // Font sizes are kept to whole pixels, so the font atlas doesn't bake a size for every cover scale.
  ImFont* const font = UIStyle.Font;
  const float font_weight = UIStyle.BoldFontWeight;

  // Covers have a large faint monogram behind the title, which gives covers of different games distinct shapes.
  TinyString monogram = GetMonogram(title);
  if (!monogram.empty())
  {
    const float max_width = size * (icon_style ? ICON_MONOGRAM_MAX_WIDTH : COVER_MONOGRAM_MAX_WIDTH);
    float font_size = std::max(std::floor(size * (icon_style ? ICON_MONOGRAM_SIZE : COVER_MONOGRAM_SIZE)), 1.0f);
    ImVec2 text_size = font->CalcTextSizeA(font_size, font_weight, FLT_MAX, 0.0f, IMSTR_START_END(monogram));
    if (text_size.x > max_width)
    {
      font_size = std::max(std::floor(font_size * max_width / text_size.x), 1.0f);
      text_size = font->CalcTextSizeA(font_size, font_weight, FLT_MAX, 0.0f, IMSTR_START_END(monogram));
    }

    const float center_y = size * (icon_style ? 0.5f : COVER_MONOGRAM_CENTER_Y);
    dl.AddText(font, font_size, font_weight, ImVec2((size - text_size.x) * 0.5f, center_y - text_size.y * 0.5f),
               IM_COL32(255, 255, 255, icon_style ? 255 : COVER_MONOGRAM_ALPHA), IMSTR_START_END(monogram));
  }

  if (!icon_style)
  {
    // Long titles shrink until they fit in the lower half, and are cut off after a few lines.
    const float margin = size * MARGIN;
    const float wrap_width = size - margin * 2.0f;
    const float min_font_size = size * MIN_TITLE_SIZE;
    float font_size = std::max(std::floor(size * TITLE_SIZE), 1.0f);
    ImVec2 title_size = font->CalcTextSizeA(font_size, font_weight, FLT_MAX, wrap_width, IMSTR_START_END(title));
    while (title_size.y > size * MAX_TITLE_HEIGHT && font_size > min_font_size && font_size > 1.0f)
    {
      font_size = std::max(std::floor(font_size * TITLE_SHRINK_FACTOR), 1.0f);
      title_size = font->CalcTextSizeA(font_size, font_weight, FLT_MAX, wrap_width, IMSTR_START_END(title));
    }

    const float title_height = std::min(title_size.y, font_size * static_cast<float>(MAX_TITLE_LINES));
    const ImVec2 title_pos(margin, size - margin - title_height);
    const ImVec4 title_clip(title_pos.x, title_pos.y, size - margin, title_pos.y + title_height);
    dl.AddText(font, font_size, font_weight, title_pos, IM_COL32(255, 255, 255, 255), IMSTR_START_END(title),
               wrap_width, &title_clip);

    // Content that isn't a plain disc gets a badge with its type.
    const char* badge_icon;
    switch (type)
    {
        // clang-format off
      case GameList::EntryType::DiscSet: badge_icon = ICON_FA_LAYER_GROUP; break;
      case GameList::EntryType::AudioCD: badge_icon = ICON_FA_MUSIC; break;
      case GameList::EntryType::PSF: badge_icon = ICON_FA_MUSIC; break;
      case GameList::EntryType::PSExe: badge_icon = ICON_FA_GEAR; break;
      case GameList::EntryType::Playlist: badge_icon = ICON_FA_LIST; break;
      default: badge_icon = nullptr; break;
        // clang-format on
    }
    if (badge_icon)
    {
      const float radius = size * BADGE_RADIUS;
      const ImVec2 center(size - margin - radius, margin + radius);
      dl.AddCircleFilled(center, radius, IM_COL32(0, 0, 0, BADGE_ALPHA));

      const float icon_size = std::max(std::floor(radius * BADGE_ICON_SIZE), 1.0f);
      const ImVec2 icon_text_size = font->CalcTextSizeA(icon_size, UIStyle.NormalFontWeight, FLT_MAX, 0.0f, badge_icon);
      dl.AddText(font, icon_size, UIStyle.NormalFontWeight, center - icon_text_size * 0.5f,
                 IM_COL32(255, 255, 255, 255), badge_icon);
    }
  }

  // Textures includes the font atlas, so any glyphs baked above are uploaded before drawing.
  ImDrawData draw_data;
  draw_data.Valid = true;
  draw_data.DisplaySize = rt_size;
  draw_data.FramebufferScale = ImVec2(1.0f, 1.0f);
  draw_data.Textures = &io.Fonts->TexList;
  draw_data.AddDrawList(&dl);

  // The ImGui pipeline doesn't write alpha, so clear it to opaque for the texture to be sampled as such.
  g_gpu_device->ClearRenderTarget(rt.get(), 0xFF000000u);
  g_gpu_device->SetRenderTarget(rt.get());
  ImGuiManager::RenderDrawLists(&draw_data, tex_size, tex_size, WindowInfoPrerotation::Identity);

  g_gpu_device->CopyTextureRegion(tex.get(), 0, 0, 0, 0, rt.get(), 0, 0, 0, 0, tex_size, tex_size);
  tex->MakeReadyForSampling();

  return InsertCachedTexture(std::string(key.view()), std::move(tex));
}
