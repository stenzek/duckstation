// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "achievements.h"
#include "core.h"
#include "fullscreenui_private.h"
#include "game_list.h"
#include "gpu_thread.h"
#include "system.h"

#include "util/gpu_texture.h"
#include "util/imgui_manager.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/path.h"
#include "common/string_util.h"

#include "IconsEmoji.h"
#include "IconsPromptFont.h"

#ifndef __ANDROID__

namespace FullscreenUI {

enum class GameListView : u8
{
  Grid,
  List,
  Count
};

static void DrawGameList(const ImVec2& heading_size);
static void DrawGameGrid(const ImVec2& heading_size);
static void HandleGameListActivate(const GameList::Entry* entry);
static void HandleGameListOptions(const GameList::Entry* entry);
static void HandleSelectDiscForDiscSet(const GameDatabase::DiscSetEntry* dsentry);
static void PopulateGameListEntryList();
static std::string_view GetKeyForGameListEntry(const GameList::Entry* entry);
static GPUTexture* GetCoverPlaceholderTexture();
static GPUTexture* GetTextureForGameListEntryType(GameList::EntryType type);
static GPUTexture* GetGameListCover(const GameList::Entry* entry, bool fallback_to_achievements_icon,
                                    bool fallback_to_icon, bool return_default_image);
static GPUTexture* GetGameListCoverTrophy(const GameList::Entry* entry, const ImVec2& image_size);
static void DrawGameListCover(const GameList::Entry* entry, bool fallback_to_achievements_icon, bool fallback_to_icon,
                              bool draw_on_placeholder, bool show_localized_titles, ImDrawList* dl, const ImRect& rect);
static void DoSetCoverImage(std::string entry_path);
static void DoSetCoverImage(std::string source_path, std::string existing_path, std::string new_path);

namespace {
struct GameListLocals
{
  // Lazily populated cover images.
  std::unordered_map<std::string, std::string> cover_image_map;
  std::unordered_map<std::string, std::string> icon_image_map;
  std::vector<const GameList::Entry*> game_list_sorted_entries;
  GameListView game_list_view = GameListView::Grid;
  float game_list_current_selection_timeout = 0.0f;
  std::string game_list_current_selection_path;
};

} // namespace

ALIGN_TO_CACHE_LINE static GameListLocals s_game_list_locals;

} // namespace FullscreenUI

FullscreenUI::FileSelectorFilters FullscreenUI::GetDiscImageFilters()
{
  return {"*.bin",   "*.cue",    "*.iso", "*.img", "*.chd", "*.ecm",     "*.mds", "*.cpe", "*.elf",
          "*.psexe", "*.ps-exe", "*.exe", "*.psx", "*.psf", "*.minipsf", "*.m3u", "*.pbp"};
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

void FullscreenUI::DoSetCoverImage(std::string entry_path)
{
  OpenFileSelector(
    FSUI_ICONVSTR(ICON_FA_IMAGE, "Set Cover Image"), false,
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
    },
    GetImageFilters(), EmuFolders::Covers);
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
  const s32 sort = Core::GetBaseIntSettingValue("Main", "FullscreenUIGameSort", 0);
  const bool reverse = Core::GetBaseBoolSettingValue("Main", "FullscreenUIGameSortReverse", false);
  const bool merge_disc_sets = Core::GetBaseBoolSettingValue("Main", "FullscreenUIMergeDiscSets", true);

  s_game_list_locals.game_list_sorted_entries.clear();
  s_game_list_locals.game_list_sorted_entries.reserve(GameList::GetEntryCount());
  for (const GameList::Entry& entry : GameList::GetEntries())
  {
    if (merge_disc_sets)
    {
      if (entry.disc_set_member)
        continue;
    }
    else
    {
      if (entry.IsDiscSet())
        continue;
    }

    s_game_list_locals.game_list_sorted_entries.push_back(&entry);
  }

  std::sort(s_game_list_locals.game_list_sorted_entries.begin(), s_game_list_locals.game_list_sorted_entries.end(),
            [sort, reverse](const GameList::Entry* lhs, const GameList::Entry* rhs) {
              switch (sort)
              {
                case 0: // Type
                {
                  const GameList::EntryType lst = lhs->GetSortType();
                  const GameList::EntryType rst = rhs->GetSortType();
                  if (lst != rst)
                    return reverse ? (lst > rst) : (lst < rst);
                }
                break;

                case 1: // Serial
                {
                  if (lhs->serial != rhs->serial)
                    return reverse ? (lhs->serial > rhs->serial) : (lhs->serial < rhs->serial);
                }
                break;

                case 2: // Title
                  break;

                case 3: // File Title
                {
                  const std::string_view lhs_title(Path::GetFileTitle(lhs->path));
                  const std::string_view rhs_title(Path::GetFileTitle(rhs->path));
                  const int res = StringUtil::Strncasecmp(lhs_title.data(), rhs_title.data(),
                                                          std::min(lhs_title.size(), rhs_title.size()));
                  if (res != 0)
                    return reverse ? (res > 0) : (res < 0);
                }
                break;

                case 4: // Time Played
                {
                  if (lhs->total_played_time != rhs->total_played_time)
                  {
                    return reverse ? (lhs->total_played_time > rhs->total_played_time) :
                                     (lhs->total_played_time < rhs->total_played_time);
                  }
                }
                break;

                case 5: // Last Played (reversed by default)
                {
                  if (lhs->last_played_time != rhs->last_played_time)
                  {
                    return reverse ? (lhs->last_played_time < rhs->last_played_time) :
                                     (lhs->last_played_time > rhs->last_played_time);
                  }
                }
                break;

                case 6: // File Size
                {
                  if (lhs->file_size != rhs->file_size)
                  {
                    return reverse ? (lhs->file_size > rhs->file_size) : (lhs->file_size < rhs->file_size);
                  }
                }
                break;

                case 7: // Uncompressed Size
                {
                  if (lhs->uncompressed_size != rhs->uncompressed_size)
                  {
                    return reverse ? (lhs->uncompressed_size > rhs->uncompressed_size) :
                                     (lhs->uncompressed_size < rhs->uncompressed_size);
                  }
                }
                break;

                case 8: // Achievements
                {
                  // sort by unlock percentage
                  const float unlock_lhs =
                    (lhs->num_achievements > 0) ?
                      (static_cast<float>(std::max(lhs->unlocked_achievements, lhs->unlocked_achievements_hc)) /
                       static_cast<float>(lhs->num_achievements)) :
                      0;
                  const float unlock_rhs =
                    (rhs->num_achievements > 0) ?
                      (static_cast<float>(std::max(rhs->unlocked_achievements, rhs->unlocked_achievements_hc)) /
                       static_cast<float>(rhs->num_achievements)) :
                      0;
                  if (std::abs(unlock_lhs - unlock_rhs) >= 0.0001f)
                    return reverse ? (unlock_lhs >= unlock_rhs) : (unlock_lhs < unlock_rhs);

                  // order by achievement count
                  if (lhs->num_achievements != rhs->num_achievements)
                    return reverse ? (rhs->num_achievements < lhs->num_achievements) :
                                     (lhs->num_achievements < rhs->num_achievements);
                }
              }

              // fallback to title when all else is equal
              const int res = StringUtil::CompareNoCase(lhs->GetSortTitle(), rhs->GetSortTitle());
              if (res != 0)
                return reverse ? (res > 0) : (res < 0);

              // fallback to path when all else is equal
              return reverse ? (lhs->path > rhs->path) : (lhs->path < rhs->path);
            });
}

void FullscreenUI::DrawGameListWindow()
{
  auto game_list_lock = GameList::GetLock();
  PopulateGameListEntryList();

  ImGuiIO& io = ImGui::GetIO();
  const ImVec2 heading_size = ImVec2(
    io.DisplaySize.x, UIStyle.LargeFontSize + (LayoutScale(LAYOUT_MENU_BUTTON_Y_PADDING) * 2.0f) + LayoutScale(2.0f));

  if (BeginFullscreenWindow(ImVec2(0.0f, 0.0f), heading_size, "gamelist_view",
                            MulAlpha(UIStyle.PrimaryColor, GetBackgroundAlpha())))
  {
    static constexpr const char* icons[] = {ICON_FA_TABLE_CELLS_LARGE, ICON_FA_LIST};
    static constexpr const char* titles[] = {FSUI_NSTR("Game Grid"), FSUI_NSTR("Game List")};
    static constexpr u32 count = static_cast<u32>(std::size(titles));

    BeginNavBar();

    if (NavButton(ICON_PF_NAVIGATION_BACK, true, true))
      BeginTransition([]() { SwitchToMainWindow(MainWindowType::Landing); });

    NavTitle(Host::TranslateToStringView(FSUI_TR_CONTEXT, titles[static_cast<u32>(s_game_list_locals.game_list_view)]));
    RightAlignNavButtons(count);

    for (u32 i = 0; i < count; i++)
    {
      if (NavButton(icons[i], static_cast<GameListView>(i) == s_game_list_locals.game_list_view, true))
      {
        BeginTransition([]() {
          s_game_list_locals.game_list_view =
            (s_game_list_locals.game_list_view == GameListView::Grid) ? GameListView::List : GameListView::Grid;
          QueueResetFocus(FocusResetType::ViewChanged);
        });
      }
    }

    EndNavBar();
  }

  EndFullscreenWindow();

  switch (s_game_list_locals.game_list_view)
  {
    case GameListView::Grid:
      DrawGameGrid(heading_size);
      break;
    case GameListView::List:
      DrawGameList(heading_size);
      break;
    default:
      break;
  }

  // note: has to come afterwards
  if (!AreAnyDialogsOpen())
  {
    if (ImGui::IsKeyPressed(ImGuiKey_NavGamepadMenu, false) || ImGui::IsKeyPressed(ImGuiKey_F4, false))
    {
      EnqueueSoundEffect(SFX_NAV_MOVE);
      BeginTransition([]() {
        s_game_list_locals.game_list_view =
          (s_game_list_locals.game_list_view == GameListView::Grid) ? GameListView::List : GameListView::Grid;
        QueueResetFocus(FocusResetType::ViewChanged);
      });
    }
    else if (ImGui::IsKeyPressed(ImGuiKey_GamepadBack, false) || ImGui::IsKeyPressed(ImGuiKey_F2, false))
    {
      EnqueueSoundEffect(SFX_NAV_BACK);
      BeginTransition(&SwitchToSettings);
    }
    else if (ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false) || ImGui::IsKeyPressed(ImGuiKey_F3, false))
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
                                       std::make_pair(ICON_PF_BUTTON_X, FSUI_VSTR("Change View")),
                                       std::make_pair(ICON_PF_BUTTON_Y, FSUI_VSTR("Launch Options")),
                                       std::make_pair(ICON_PF_BUTTON_A, FSUI_VSTR("Start Game")),
                                       std::make_pair(ICON_PF_BUTTON_B, FSUI_VSTR("Back"))});
  }
  else
  {
    SetFullscreenFooterText(std::array{
      std::make_pair(ICON_PF_ARROW_UP ICON_PF_ARROW_DOWN ICON_PF_ARROW_LEFT ICON_PF_ARROW_RIGHT,
                     FSUI_VSTR("Select Game")),
      std::make_pair(ICON_PF_F3, FSUI_VSTR("Resume Last Session")),
      std::make_pair(ICON_PF_F2, FSUI_VSTR("Settings")),
      std::make_pair(ICON_PF_F4, FSUI_VSTR("Change View")),
      std::make_pair(ICON_PF_F1, FSUI_VSTR("Launch Options")),
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
    BeginTransition([]() { SwitchToMainWindow(MainWindowType::Landing); });

  const bool compact_mode = Core::GetBaseBoolSettingValue("Main", "FullscreenUIGameListCompactMode", true);
  const bool show_localized_titles = GameList::ShouldShowLocalizedTitles();
  auto game_list_lock = GameList::GetLock();
  const GameList::Entry* selected_entry = nullptr;
  PopulateGameListEntryList();

  if (IsFocusResetFromWindowChange())
    ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));

  if (BeginFullscreenColumnWindow(0.0f, -530.0f, "game_list_entries", GetTransparentBackgroundColor(),
                                  ImVec2(LAYOUT_MENU_WINDOW_X_PADDING, LAYOUT_MENU_WINDOW_Y_PADDING)))
  {
    const float image_size = compact_mode ? UIStyle.LargeFontSize : LayoutScale(50.0f);
    const float row_image_padding = LayoutScale(compact_mode ? 15.0f : 15.0f);
    const float row_left_margin = image_size + row_image_padding;

    ResetFocusHere();

    BeginMenuButtons();

    SmallString summary;
    const u32 text_color = ImGui::GetColorU32(ImGui::GetStyle().Colors[ImGuiCol_Text]);
    const u32 subtitle_text_color = ImGui::GetColorU32(DarkerColor(ImGui::GetStyle().Colors[ImGuiCol_Text]));

    for (const GameList::Entry* entry : s_game_list_locals.game_list_sorted_entries)
    {
      if (!compact_mode)
      {
        if (entry->serial.empty())
          summary.format("{} | {} MB", Path::GetFileName(entry->path), to_mb(entry->file_size));
        else
          summary.format("{} | {} | {} MB", entry->serial, Path::GetFileName(entry->path), to_mb(entry->file_size));
      }

      const MenuButtonBounds mbb(entry->GetDisplayTitle(show_localized_titles), {}, summary, row_left_margin);

      bool visible, hovered;
      bool pressed = MenuButtonFrame(GetKeyForGameListEntry(entry), true, mbb.frame_bb, &visible, &hovered);
      if (!visible)
        continue;

      DrawGameListCover(entry, false, true, false, show_localized_titles, ImGui::GetWindowDrawList(),
                        ImRect(ImVec2(mbb.title_bb.Min.x - row_left_margin, mbb.title_bb.Min.y),
                               ImVec2(mbb.title_bb.Min.x - row_image_padding, mbb.title_bb.Min.y + image_size)));

      RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, mbb.title_bb.Min,
                                mbb.title_bb.Max, text_color, entry->GetDisplayTitle(show_localized_titles),
                                &mbb.title_size, ImVec2(0.0f, 0.0f), mbb.title_size.x, &mbb.title_bb);

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
            (ImGui::IsItemClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_NavGamepadInput, false) ||
             ImGui::IsKeyPressed(ImGuiKey_F1, false)))
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

    GPUTexture* const cover_texture = selected_entry ? GetGameListCover(selected_entry, false, false, true) :
                                                       GetTextureForGameListEntryType(GameList::EntryType::MaxCount);
    if (cover_texture)
    {
      const ImRect image_rect(CenterImage(
        LayoutScale(ImVec2(cover_size, cover_size)),
        ImVec2(static_cast<float>(cover_texture->GetWidth()), static_cast<float>(cover_texture->GetHeight()))));

      ImGui::SetCursorPos(LayoutScale((info_window_width - cover_size) / 2.0f, info_top_margin) + image_rect.Min);
      ImGui::Image(cover_texture, image_rect.GetSize());
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
      const std::string_view title = selected_entry->GetDisplayTitle(show_localized_titles);

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
        ImGui::PushFont(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight);
        TextUnformatted(
          FSUI_ICONVSTR(ICON_EMOJI_GLOBE, display_as_language ? FSUI_CSTR("Language: ") : FSUI_CSTR("Region: ")));
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::Image(GetCachedTexture(selected_entry->GetLanguageIconName(), 23, 16), LayoutScale(23.0f, 16.0f));
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, subtitle_text_color);
        if (display_as_language)
        {
          ImGui::TextWrapped(" (%s, %s)", selected_entry->dbentry->GetLanguagesString().c_str(),
                             Settings::GetDiscRegionName(selected_entry->region));
        }
        else
        {
          ImGui::TextWrapped(" (%s)", Settings::GetDiscRegionName(selected_entry->region));
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
      ImGui::PushFont(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight);
      TextUnformatted(FSUI_ICONSTR(ICON_EMOJI_STAR, FSUI_VSTR("Compatibility: ")));
      ImGui::PopFont();
      ImGui::SameLine();
      ImGui::Image(GetCachedTexture(selected_entry->GetCompatibilityIconFileName(), 88, 16), LayoutScale(88.0f, 16.0f));
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
      ImGui::TextUnformatted(GameList::FormatTimespan(selected_entry->total_played_time).c_str());
      ImGui::PopStyleColor();
      ImGui::PushFont(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight);
      TextUnformatted(FSUI_ICONSTR(ICON_EMOJI_CLOCK_FIVE_OCLOCK, FSUI_CSTR("Last Played: ")));
      ImGui::PopFont();
      ImGui::SameLine();
      ImGui::PushStyleColor(ImGuiCol_Text, subtitle_text_color);
      ImGui::TextUnformatted(GameList::FormatTimestamp(selected_entry->last_played_time).c_str());
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
    BeginTransition([]() { SwitchToMainWindow(MainWindowType::Landing); });

  ResetFocusHere();
  BeginMenuButtons(0, 0.0f, 15.0f, 15.0f, 20.0f, 20.0f);

  const ImGuiStyle& style = ImGui::GetStyle();
  const bool show_trophy_icons = Core::GetBaseBoolSettingValue("Main", "FullscreenUIShowTrophyIcons", true);
  const bool show_titles = Core::GetBaseBoolSettingValue("Main", "FullscreenUIShowGridTitles", true);
  const bool show_localized_titles = GameList::ShouldShowLocalizedTitles();

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

  for (size_t entry_index = 0; entry_index < s_game_list_locals.game_list_sorted_entries.size(); entry_index++)
  {
    if (window->SkipItems)
      continue;

    // This is pretty annoying. If we don't use an equal sized item for each grid item, keyboard/gamepad navigation
    // tends to break when scrolling vertically - it goes left/right. Precompute the maximum item height for the row
    // first, and make all items the same size to work around this.
    const GameList::Entry* entry = s_game_list_locals.game_list_sorted_entries[entry_index];
    if (grid_x == 0 && show_titles)
    {
      row_item_height = 0.0f;

      const size_t row_entry_index_end =
        std::min(entry_index + grid_count_x, s_game_list_locals.game_list_sorted_entries.size());
      for (size_t row_entry_index = entry_index; row_entry_index < row_entry_index_end; row_entry_index++)
      {
        const GameList::Entry* row_entry = s_game_list_locals.game_list_sorted_entries[row_entry_index];
        const std::string_view row_title = row_entry->GetDisplayTitle(show_localized_titles);
        const ImVec2 this_title_size = UIStyle.Font->CalcTextSizeA(title_font_size, title_font_weight, image_width,
                                                                   image_width, IMSTR_START_END(row_title));
        row_item_height = std::max(row_item_height, this_title_size.y);
      }

      row_item_height += title_spacing + base_item_height;
    }

    ImVec2 title_size;
    if (show_titles)
    {
      const std::string_view title = entry->GetDisplayTitle(show_localized_titles);
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

      DrawGameListCover(entry, false, false, true, show_localized_titles, dl, ImRect(bb.Min, bb.Min + image_size));

      if (show_trophy_icons)
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

      if (draw_title)
      {
        const ImRect title_bb(ImVec2(bb.Min.x, bb.Min.y + image_height + title_spacing), bb.Max);
        RenderMultiLineShadowedTextClipped(dl, UIStyle.Font, title_font_size, title_font_weight, title_bb.Min,
                                           title_bb.Max, text_color, entry->GetDisplayTitle(show_localized_titles),
                                           LAYOUT_CENTER_ALIGN_TEXT, image_width, &title_bb);
      }

      if (pressed)
      {
        HandleGameListActivate(entry);
      }
      else if (hovered &&
               (ImGui::IsItemClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_NavGamepadInput, false) ||
                ImGui::IsKeyPressed(ImGuiKey_F1, false)))
      {
        CancelPendingMenuClose();
        HandleGameListOptions(entry);
      }
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
  if (!entry->IsDiscSet())
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

    OpenChoiceDialog(
      entry->GetDisplayTitle(GameList::ShouldShowLocalizedTitles()), false, std::move(options),
      [entry_path = entry->path, entry_serial = entry->serial](s32 index, const std::string& title,
                                                               bool checked) mutable {
        switch (index)
        {
          case 0: // Open Game Properties
            BeginTransition([entry_path = std::move(entry_path)]() { SwitchToGameSettingsForPath(entry_path); });
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
            BeginTransition([entry_serial = std::move(entry_serial), entry_path = std::move(entry_path)]() {
              OpenSaveStateSelector(entry_serial, entry_path, true);
            });
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
      {FSUI_ICONSTR(ICON_FA_WRENCH, "Game Properties"), false},
      {FSUI_ICONSTR(ICON_FA_IMAGE, "Set Cover Image"), false},
      {FSUI_ICONSTR(ICON_FA_COMPACT_DISC, "Select Disc"), false},
    };

    const GameDatabase::DiscSetEntry* dsentry = entry->dbentry->disc_set;
    OpenChoiceDialog(entry->GetDisplayTitle(GameList::ShouldShowLocalizedTitles()), false, std::move(options),
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
    std::string title = fmt::format(ICON_FA_COMPACT_DISC " {} {} | {}##{}", FSUI_VSTR("Disc"),
                                    entry->disc_set_index + 1, Path::GetFileName(entry->path), i);
    options.emplace_back(std::move(title), false);
    paths.push_back(entry->path);
  }
  options.emplace_back(FSUI_ICONVSTR(ICON_FA_SQUARE_XMARK, "Close Menu"), false);

  const GameList::Entry* dsgentry = GameList::GetEntryForPath(dsentry->GetSaveTitle());
  const bool localized_titles = GameList::ShouldShowLocalizedTitles();
  OpenChoiceDialog(fmt::format(FSUI_FSTR("Select Disc for {}"), dsgentry ? dsgentry->GetDisplayTitle(localized_titles) :
                                                                           dsentry->GetDisplayTitle(localized_titles)),
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
  s_game_list_locals.game_list_view =
    static_cast<GameListView>(Core::GetBaseIntSettingValue("Main", "DefaultFullscreenUIGameView", 0));
  s_game_list_locals.game_list_current_selection_path = {};
  s_game_list_locals.game_list_current_selection_timeout = 0.0f;

  // Wipe icon map, because a new save might give us an icon.
  for (const auto& it : s_game_list_locals.icon_image_map)
  {
    if (!it.second.empty())
      InvalidateCachedTexture(it.second);
  }
  s_game_list_locals.icon_image_map.clear();

  SwitchToMainWindow(MainWindowType::GameList);
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
      const auto lock = Achievements::GetLock();
      if (Achievements::GetGamePath() == entry->path)
        cover_it->second = Achievements::GetGameIconPath();
    }
  }

  // because memcard icons are crap res
  if (fallback_to_icon && cover_it->second.empty() && !entry->serial.empty() && entry->IsDiscOrDiscSet())
  {
    cover_it = s_game_list_locals.icon_image_map.find(entry->serial);
    if (cover_it == s_game_list_locals.icon_image_map.end())
    {
      std::string icon_path = GameList::GetGameIconPath(entry);
      cover_it = s_game_list_locals.icon_image_map.emplace(entry->serial, std::move(icon_path)).first;
    }
  }

  GPUTexture* const tex = (!cover_it->second.empty()) ? GetCachedTextureAsync(cover_it->second) : nullptr;
  return tex ? tex : (return_default_image ? GetTextureForGameListEntryType(entry->type) : nullptr);
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
  return (texture == GetPlaceholderTexture().get()) ? nullptr : texture;
}

std::string_view FullscreenUI::GetKeyForGameListEntry(const GameList::Entry* entry)
{
  return entry->IsDiscSet() ? entry->GetDiscSetEntry()->GetSaveTitle() : std::string_view(entry->path);
}

GPUTexture* FullscreenUI::GetCoverPlaceholderTexture()
{
  return GetCachedTexture("images/cover-placeholder.png");
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
      return GetCachedTexture("fullscreenui/psf-file.png");

    case GameList::EntryType::Disc:
    default:
      return GetCachedTexture("fullscreenui/cdrom.png");
  }
}

GPUTexture* FullscreenUI::GetCoverForCurrentGame(const std::string& game_path)
{
  auto lock = GameList::GetLock();

  const GameList::Entry* entry = GameList::GetEntryForPath(game_path);
  if (!entry)
    return GetTextureForGameListEntryType(GameList::EntryType::Disc);

  return GetGameListCover(entry, true, true, true);
}

void FullscreenUI::SetCoverCacheEntry(std::string path, std::string cover_path)
{
  s_game_list_locals.cover_image_map.emplace(std::move(path), std::move(cover_path));
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
  if (!GPUThread::IsFullscreenUIRequested())
    return;

  GPUThread::RunOnThread([path = std::move(path)]() { RemoveCoverCacheEntry(path); });
}

void FullscreenUI::DrawGameListCover(const GameList::Entry* entry, bool fallback_to_achievements_icon,
                                     bool fallback_to_icon, bool draw_on_placeholder, bool show_localized_titles,
                                     ImDrawList* dl, const ImRect& rect)
{
  GPUTexture* const cover_texture =
    GetGameListCover(entry, fallback_to_achievements_icon, fallback_to_icon, !draw_on_placeholder);
  if (cover_texture)
  {
    // simple case, has cover
    const ImRect image_rect = CenterImage(
      rect, ImVec2(static_cast<float>(cover_texture->GetWidth()), static_cast<float>(cover_texture->GetHeight())));
    dl->AddImage(cover_texture, image_rect.Min, image_rect.Max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                 IM_COL32(255, 255, 255, 255));
    return;
  }

  // draw placeholder
  GPUTexture* const placeholder_texture = GetCoverPlaceholderTexture();
  const ImRect image_rect = CenterImage(rect, ImVec2(static_cast<float>(placeholder_texture->GetWidth()),
                                                     static_cast<float>(placeholder_texture->GetHeight())));
  dl->AddImage(placeholder_texture, image_rect.Min, image_rect.Max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
               IM_COL32(255, 255, 255, 255));

  // and the text
  const float& font_size = UIStyle.LargeFontSize;
  const float& font_weight = UIStyle.BoldFontWeight;
  const std::string_view title = entry->GetDisplayTitle(show_localized_titles);
  const ImVec2 title_size = UIStyle.Font->CalcTextSizeA(font_size, font_weight, image_rect.GetWidth(),
                                                        image_rect.GetWidth(), IMSTR_START_END(title));
  const ImVec2 title_offset = ImVec2(0.0f, std::max((image_rect.GetHeight() - title_size.y) * 0.5f, 0.0f));
  RenderMultiLineShadowedTextClipped(dl, UIStyle.Font, font_size, font_weight, image_rect.Min + title_offset,
                                     image_rect.Max, IM_COL32(255, 255, 255, 255), title, LAYOUT_CENTER_ALIGN_TEXT,
                                     image_rect.GetWidth(), &image_rect);
}

#endif // __ANDROID__
