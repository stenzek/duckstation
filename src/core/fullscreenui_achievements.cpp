// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "achievements_private.h"
#include "fullscreenui_private.h"
#include "gpu_thread.h"
#include "system.h"

#include "common/assert.h"
#include "common/log.h"
#include "common/timer.h"

#include "IconsEmoji.h"
#include "IconsPromptFont.h"

LOG_CHANNEL(FullscreenUI);

#ifndef __ANDROID__

namespace FullscreenUI {

static constexpr const char* ACHEIVEMENT_DETAILS_URL_TEMPLATE = "https://retroachievements.org/achievement/{}";
static constexpr const char* PROFILE_DETAILS_URL_TEMPLATE = "https://retroachievements.org/user/{}";

static constexpr u32 LEADERBOARD_NEARBY_ENTRIES_TO_FETCH = 10;
static constexpr u32 LEADERBOARD_ALL_FETCH_SIZE = 20;

// How long the last progress update is shown in the pause menu.
static constexpr float PAUSE_MENU_PROGRESS_DISPLAY_TIME = 60.0f;

namespace {

struct PauseMenuAchievementInfo
{
  std::string title;
  std::string description;
  std::string badge_path;
  u32 achievement_id;
  float measured_percent;
};

struct PauseMenuMeasuredAchievementInfo : PauseMenuAchievementInfo
{
  std::string measured_progress;
};

struct PauseMenuTimedMeasuredAchievementInfo : PauseMenuMeasuredAchievementInfo
{
  // can't use imgui deltatime here because this is only updated when paused
  Timer::Value show_time;
};

} // namespace

static const std::string& GetCachedAchievementBadgePath(const rc_client_achievement_t* achievement, bool locked);

template<typename T>
static void CachePauseMenuAchievementInfo(const rc_client_achievement_t* achievement, std::optional<T>& value);

static void DrawAchievement(const rc_client_achievement_t* cheevo);

static void LeaderboardFetchNearbyCallback(int result, const char* error_message,
                                           rc_client_leaderboard_entry_list_t* list, rc_client_t* client,
                                           void* callback_userdata);
static void LeaderboardFetchAllCallback(int result, const char* error_message, rc_client_leaderboard_entry_list_t* list,
                                        rc_client_t* client, void* callback_userdata);

static bool OpenLeaderboardById(u32 leaderboard_id);
static void FetchNextLeaderboardEntries();
static void CloseLeaderboard();
static void DrawLeaderboardListEntry(const rc_client_leaderboard_t* lboard);
static void DrawLeaderboardEntry(const rc_client_leaderboard_entry_t& entry, u32 index, bool is_self,
                                 float rank_column_width, float name_column_width, float time_column_width,
                                 float column_spacing);

namespace {

struct AchievementsLocals
{
  rc_client_achievement_list_t* achievement_list = nullptr;
  std::vector<std::tuple<const void*, std::string, bool>> achievement_badge_paths;

  std::optional<PauseMenuAchievementInfo> most_recent_unlock;
  std::optional<PauseMenuMeasuredAchievementInfo> achievement_nearest_completion;
  std::optional<PauseMenuTimedMeasuredAchievementInfo> most_recent_progress_update;

  rc_client_leaderboard_list_t* leaderboard_list = nullptr;
  const rc_client_leaderboard_t* open_leaderboard = nullptr;
  rc_client_async_handle_t* leaderboard_fetch_handle = nullptr;
  std::vector<rc_client_leaderboard_entry_list_t*> leaderboard_entry_lists;
  std::vector<std::pair<const rc_client_leaderboard_entry_t*, std::string>> leaderboard_user_icon_paths;
  rc_client_leaderboard_entry_list_t* leaderboard_nearby_entries;
  bool is_showing_all_leaderboard_entries = false;
};

} // namespace

ALIGN_TO_CACHE_LINE static AchievementsLocals s_achievements_locals;

} // namespace FullscreenUI

void FullscreenUI::ClearAchievementsState()
{
  // NOTE: can be called on the CPU thread. don't mess with any GPU thread state
  // will already be held if we're clearing as a result of achievements shutting down

  const auto lock = Achievements::GetLock();

  CloseLeaderboard();

  s_achievements_locals.achievement_badge_paths = {};

  s_achievements_locals.leaderboard_user_icon_paths = {};
  s_achievements_locals.leaderboard_entry_lists = {};
  if (s_achievements_locals.leaderboard_list)
  {
    rc_client_destroy_leaderboard_list(s_achievements_locals.leaderboard_list);
    s_achievements_locals.leaderboard_list = nullptr;
  }

  if (s_achievements_locals.achievement_list)
  {
    rc_client_destroy_achievement_list(s_achievements_locals.achievement_list);
    s_achievements_locals.achievement_list = nullptr;
  }

  s_achievements_locals.most_recent_unlock.reset();
  s_achievements_locals.achievement_nearest_completion.reset();
}

const std::string& FullscreenUI::GetCachedAchievementBadgePath(const rc_client_achievement_t* achievement, bool locked)
{
  for (const auto& [l_cheevo, l_path, l_state] : s_achievements_locals.achievement_badge_paths)
  {
    if (l_cheevo == achievement && l_state == locked)
      return l_path;
  }

  std::string path = Achievements::GetAchievementBadgePath(achievement, locked);
  return std::get<1>(s_achievements_locals.achievement_badge_paths.emplace_back(achievement, std::move(path), locked));
}

template<typename T>
void FullscreenUI::CachePauseMenuAchievementInfo(const rc_client_achievement_t* achievement, std::optional<T>& value)
{
  if (!achievement)
  {
    value.reset();
    return;
  }

  if (!value.has_value())
    value.emplace();

  // have to take a copy because with RAIntegration the achievement pointer does not persist
  value->title = achievement->title;
  value->description = achievement->description;
  value->badge_path = Achievements::GetAchievementBadgePath(achievement, false);
  value->measured_percent = achievement->measured_percent;
  value->achievement_id = achievement->id;

  if constexpr (std::is_base_of_v<PauseMenuMeasuredAchievementInfo, T>)
    value->measured_progress = achievement->measured_progress;
  if constexpr (std::is_same_v<PauseMenuTimedMeasuredAchievementInfo, T>)
    value->show_time = Timer::GetCurrentValue();
}

void FullscreenUI::UpdateAchievementsRecentUnlockAndAlmostThere()
{
  const auto lock = Achievements::GetLock();
  if (!Achievements::HasActiveGame())
  {
    s_achievements_locals.most_recent_unlock.reset();
    s_achievements_locals.achievement_nearest_completion.reset();
    return;
  }

  rc_client_achievement_list_t* const achievements =
    rc_client_create_achievement_list(Achievements::GetClient(), RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE_AND_UNOFFICIAL,
                                      RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS);
  if (!achievements)
  {
    s_achievements_locals.most_recent_unlock.reset();
    s_achievements_locals.achievement_nearest_completion.reset();
    return;
  }

  const rc_client_achievement_t* most_recent_unlock = nullptr;
  const rc_client_achievement_t* nearest_completion = nullptr;

  for (u32 i = 0; i < achievements->num_buckets; i++)
  {
    const rc_client_achievement_bucket_t& bucket = achievements->buckets[i];
    for (u32 j = 0; j < bucket.num_achievements; j++)
    {
      const rc_client_achievement_t* achievement = bucket.achievements[j];

      if (achievement->state == RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED)
      {
        if (!most_recent_unlock || achievement->unlock_time > most_recent_unlock->unlock_time)
          most_recent_unlock = achievement;
      }
      else
      {
        // find the achievement with the greatest normalized progress, but skip anything below 80%,
        // matching the rc_client definition of "almost there"
        const float percent_cutoff = 80.0f;
        if (achievement->measured_percent >= percent_cutoff &&
            (!nearest_completion || achievement->measured_percent > nearest_completion->measured_percent))
        {
          nearest_completion = achievement;
        }
      }
    }
  }

  CachePauseMenuAchievementInfo(most_recent_unlock, s_achievements_locals.most_recent_unlock);
  CachePauseMenuAchievementInfo(nearest_completion, s_achievements_locals.achievement_nearest_completion);

  rc_client_destroy_achievement_list(achievements);
}

void FullscreenUI::UpdateAchievementsLastProgressUpdate(const rc_client_achievement_t* achievement)
{
  CachePauseMenuAchievementInfo(achievement, s_achievements_locals.most_recent_progress_update);
}

void FullscreenUI::DrawAchievementsPauseMenuOverlays(float start_pos_y)
{
  if (!Achievements::HasActiveGame() || !Achievements::HasAchievements())
    return;

  const auto lock = Achievements::GetLock();
  rc_client_t* const client = Achievements::GetClient();

  const ImVec2& display_size = ImGui::GetIO().DisplaySize;
  const float box_margin = LayoutScale(10.0f);
  const float box_width = LayoutScale(450.0f);
  const float box_padding = LayoutScale(15.0f);
  const float box_content_width = box_width - box_padding - box_padding;
  const float box_rounding = LayoutScale(20.0f);
  const u32 box_background_color = ImGui::GetColorU32(ModAlpha(UIStyle.BackgroundColor, 0.8f));
  const ImU32 box_title_text_color =
    ImGui::GetColorU32(DarkerColor(UIStyle.BackgroundTextColor, 0.9f)) | IM_COL32_A_MASK;
  const ImU32 title_text_color = ImGui::GetColorU32(UIStyle.BackgroundTextColor) | IM_COL32_A_MASK;
  const ImU32 text_color =
    ImGui::GetColorU32(DarkerColor(DarkerColor(UIStyle.BackgroundTextColor, 0.9f))) | IM_COL32_A_MASK;
  const float paragraph_spacing = LayoutScale(10.0f);
  const float text_spacing = LayoutScale(2.0f);

  const float progress_height = LayoutScale(20.0f);
  const float progress_rounding = LayoutScale(5.0f);
  const float badge_size = LayoutScale(32.0f);
  const float badge_text_width = box_content_width - badge_size - (text_spacing * 3.0f);
  const bool disconnected = rc_client_is_disconnected(client);
  const int pending_count = disconnected ? rc_client_get_award_achievement_pending_count(client) : 0;

  ImDrawList* dl = ImGui::GetBackgroundDrawList();

  float box_height = box_padding + box_padding + UIStyle.MediumFontSize + paragraph_spacing + progress_height +
                     ((pending_count > 0) ? (paragraph_spacing + UIStyle.MediumFontSize) : 0.0f);

  ImVec2 box_min = ImVec2(display_size.x - box_width - box_margin, start_pos_y + box_margin);
  ImVec2 box_max = ImVec2(box_min.x + box_width, box_min.y + box_height);
  ImVec2 text_pos = ImVec2(box_min.x + box_padding, box_min.y + box_padding);
  ImVec2 text_size;
  TinyString buffer;

  dl->AddRectFilled(box_min, box_max, box_background_color, box_rounding);

  // title
  {
    const rc_client_user_game_summary_t& summary = Achievements::GetGameSummary();

    buffer.format(ICON_EMOJI_UNLOCKED " {}",
                  TRANSLATE_DISAMBIG_SV("Achievements", "Achievements Unlocked", "Pause Menu"));
    dl->AddText(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight, text_pos, box_title_text_color,
                IMSTR_START_END(buffer));
    const float unlocked_fraction =
      static_cast<float>(summary.num_unlocked_achievements) / static_cast<float>(summary.num_core_achievements);
    buffer.format("{}%", static_cast<u32>(std::round(unlocked_fraction * 100.0f)));
    text_size = UIStyle.Font->CalcTextSizeA(UIStyle.MediumFontSize, UIStyle.BoldFontWeight, FLT_MAX, 0.0f,
                                            IMSTR_START_END(buffer));
    dl->AddText(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight,
                ImVec2(text_pos.x + (box_content_width - text_size.x), text_pos.y), text_color,
                IMSTR_START_END(buffer));
    text_pos.y += UIStyle.MediumFontSize + paragraph_spacing;

    const ImRect progress_bb(text_pos, text_pos + ImVec2(box_content_width, progress_height));
    dl->AddRectFilled(progress_bb.Min, progress_bb.Max, ImGui::GetColorU32(UIStyle.PrimaryDarkColor),
                      progress_rounding);
    if (summary.num_unlocked_achievements > 0)
    {
      ImGui::RenderRectFilledRangeH(dl, progress_bb, ImGui::GetColorU32(DarkerColor(UIStyle.SecondaryColor)), 0.0f,
                                    unlocked_fraction, progress_rounding);
    }

    buffer.format("{}/{}", summary.num_unlocked_achievements, summary.num_core_achievements);
    text_size = UIStyle.Font->CalcTextSizeA(UIStyle.MediumFontSize, UIStyle.BoldFontWeight, FLT_MAX, 0.0f,
                                            IMSTR_START_END(buffer));
    dl->AddText(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight,
                ImVec2(progress_bb.Min.x + ((progress_bb.Max.x - progress_bb.Min.x) / 2.0f) - (text_size.x / 2.0f),
                       progress_bb.Min.y + ((progress_bb.Max.y - progress_bb.Min.y) / 2.0f) - (text_size.y / 2.0f)),
                ImGui::GetColorU32(UIStyle.PrimaryTextColor), IMSTR_START_END(buffer));
    text_pos.y += progress_height;

    if (pending_count > 0)
    {
      text_pos.y += paragraph_spacing;
      buffer.format(ICON_EMOJI_WARNING " {}",
                    TRANSLATE_PLURAL_SSTR("Achievements", "%n unlocks have not been confirmed by the server.",
                                          "Pause Menu", pending_count));
      dl->AddText(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight, text_pos, title_text_color,
                  IMSTR_START_END(buffer));
      text_pos.y += UIStyle.MediumFontSize;
    }
  }

  const auto draw_achievement_in_box =
    [&box_margin, &box_width, &box_padding, &box_rounding, &box_content_width, &box_background_color, &box_min,
     &box_max, &badge_text_width, &dl, &box_title_text_color, &title_text_color, &text_color, &paragraph_spacing,
     &text_spacing, &progress_rounding, &text_pos,
     &badge_size](std::string_view box_title, std::string_view title, std::string_view description,
                  const std::string& badge_path, std::string_view measured_progress, float measured_percent) {
      const ImVec2 description_size =
        description.empty() ? ImVec2(0.0f, 0.0f) :
                              UIStyle.Font->CalcTextSizeA(UIStyle.MediumSmallFontSize, UIStyle.NormalFontWeight,
                                                          FLT_MAX, badge_text_width, IMSTR_START_END(description));

      const float box_height = box_padding + box_padding + UIStyle.MediumFontSize + paragraph_spacing +
                               std::max((title.empty() ? 0.0f : UIStyle.MediumSmallFontSize) +
                                          (description.empty() ? 0.0f : (text_spacing + description_size.y)),
                                        badge_size);

      box_min = ImVec2(box_min.x, box_max.y + box_margin);
      box_max = ImVec2(box_min.x + box_width, box_min.y + box_height);
      text_pos = ImVec2(box_min.x + box_padding, box_min.y + box_padding);

      dl->AddRectFilled(box_min, box_max, box_background_color, box_rounding);

      ImVec4 clip_rect = ImVec4(text_pos.x, text_pos.y, text_pos.x + box_content_width, box_max.y);
      dl->AddText(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight, text_pos, box_title_text_color,
                  IMSTR_START_END(box_title), 0.0f, &clip_rect);

      if (!measured_progress.empty())
      {
        const float progress_width = LayoutScale(100.0f);
        const float progress_height = UIStyle.MediumFontSize;
        const ImRect progress_bb(ImVec2(text_pos.x + box_content_width - progress_width, text_pos.y),
                                 ImVec2(text_pos.x + box_content_width, text_pos.y + progress_height));
        dl->AddRectFilled(progress_bb.Min, progress_bb.Max, ImGui::GetColorU32(UIStyle.PrimaryDarkColor),
                          progress_rounding);
        if (measured_percent > 0.0f)
        {
          ImGui::RenderRectFilledRangeH(dl, progress_bb, ImGui::GetColorU32(DarkerColor(UIStyle.SecondaryColor)), 0.0f,
                                        measured_percent * 0.01f, progress_rounding);
        }

        const ImVec2 measured_progress_size =
          UIStyle.Font->CalcTextSizeA(UIStyle.MediumSmallFontSize, UIStyle.BoldFontWeight, FLT_MAX, badge_text_width,
                                      IMSTR_START_END(measured_progress));

        dl->AddText(
          UIStyle.Font, UIStyle.MediumSmallFontSize, UIStyle.BoldFontWeight,
          ImVec2(
            progress_bb.Min.x + ((progress_bb.Max.x - progress_bb.Min.x) / 2.0f) - (measured_progress_size.x / 2.0f),
            progress_bb.Min.y + ((progress_bb.Max.y - progress_bb.Min.y) / 2.0f) - (measured_progress_size.y / 2.0f)),
          ImGui::GetColorU32(UIStyle.PrimaryTextColor), IMSTR_START_END(measured_progress));
      }

      text_pos.y += UIStyle.MediumFontSize + paragraph_spacing;

      const ImVec2 image_max = ImVec2(text_pos.x + badge_size, text_pos.y + badge_size);
      ImVec2 badge_text_pos = ImVec2(image_max.x + (text_spacing * 3.0f), text_pos.y);
      clip_rect = ImVec4(badge_text_pos.x, badge_text_pos.y, badge_text_pos.x + badge_text_width, box_max.y);

      GPUTexture* badge_tex = GetCachedTextureAsync(badge_path);
      dl->AddImage(badge_tex, text_pos, image_max);

      if (!title.empty())
      {
        dl->AddText(UIStyle.Font, UIStyle.MediumSmallFontSize, UIStyle.BoldFontWeight, badge_text_pos, title_text_color,
                    IMSTR_START_END(title), 0.0f, &clip_rect);
        badge_text_pos.y += UIStyle.MediumSmallFontSize;
      }

      if (!description.empty())
      {
        badge_text_pos.y += text_spacing;
        dl->AddText(UIStyle.Font, UIStyle.MediumSmallFontSize, UIStyle.NormalFontWeight, badge_text_pos, text_color,
                    IMSTR_START_END(description), badge_text_width, &clip_rect);
        badge_text_pos.y += description_size.y;
      }
    };

  const auto get_achievement_height = [&badge_size, &badge_text_width, &text_spacing](std::string_view description) {
    const ImVec2 description_size =
      description.empty() ? ImVec2(0.0f, 0.0f) :
                            UIStyle.Font->CalcTextSizeA(UIStyle.MediumSmallFontSize, UIStyle.NormalFontWeight, FLT_MAX,
                                                        badge_text_width, IMSTR_START_END(description));
    const float text_height = UIStyle.MediumSmallFontSize + text_spacing + description_size.y;
    return std::max(text_height, badge_size);
  };

  const auto draw_achievement_with_summary = [&box_max, &badge_text_width, &dl, &title_text_color, &text_color,
                                              &text_spacing, &text_pos,
                                              &badge_size](std::string_view title, std::string_view description,
                                                           const std::string& badge_path) {
    const ImVec2 image_max = ImVec2(text_pos.x + badge_size, text_pos.y + badge_size);
    ImVec2 badge_text_pos = ImVec2(image_max.x + (text_spacing * 3.0f), text_pos.y);
    const ImVec4 clip_rect = ImVec4(badge_text_pos.x, badge_text_pos.y, badge_text_pos.x + badge_text_width, box_max.y);
    ImVec2 text_size = description.empty() ?
                         ImVec2(0.0f, 0.0f) :
                         UIStyle.Font->CalcTextSizeA(UIStyle.MediumSmallFontSize, UIStyle.NormalFontWeight, FLT_MAX,
                                                     badge_text_width, IMSTR_START_END(description));

    GPUTexture* badge_tex = GetCachedTextureAsync(badge_path);
    dl->AddImage(badge_tex, text_pos, image_max);

    if (!title.empty())
    {
      dl->AddText(UIStyle.Font, UIStyle.MediumSmallFontSize, UIStyle.BoldFontWeight, badge_text_pos, title_text_color,
                  IMSTR_START_END(title), 0.0f, &clip_rect);
      badge_text_pos.y += UIStyle.MediumSmallFontSize + text_spacing;
    }

    if (!description.empty())
    {
      dl->AddText(UIStyle.Font, UIStyle.MediumSmallFontSize, UIStyle.NormalFontWeight, badge_text_pos, text_color,
                  IMSTR_START_END(description), badge_text_width, &clip_rect);
      badge_text_pos.y += text_size.y;
    }

    text_pos.y = badge_text_pos.y;
  };

  if (s_achievements_locals.most_recent_unlock.has_value())
  {
    buffer.format(ICON_FA_LOCK_OPEN " {}", TRANSLATE_DISAMBIG_SV("Achievements", "Most Recent", "Pause Menu"));
    draw_achievement_in_box(buffer, s_achievements_locals.most_recent_unlock->title,
                            s_achievements_locals.most_recent_unlock->description,
                            s_achievements_locals.most_recent_unlock->badge_path, {}, 0.0f);

    // extra spacing if we have two
    text_pos.y += s_achievements_locals.achievement_nearest_completion ? (paragraph_spacing + paragraph_spacing) : 0.0f;
  }

  // don't duplicate nearest completion if it was also the most recent progress update
  if (s_achievements_locals.achievement_nearest_completion.has_value() &&
      (!s_achievements_locals.most_recent_progress_update ||
       s_achievements_locals.most_recent_progress_update->achievement_id !=
         s_achievements_locals.achievement_nearest_completion->achievement_id))
  {
    buffer.format(ICON_FA_GAUGE_HIGH " {}", TRANSLATE_DISAMBIG_SV("Achievements", "Nearest Completion", "Pause Menu"));
    draw_achievement_in_box(buffer, s_achievements_locals.achievement_nearest_completion->title,
                            s_achievements_locals.achievement_nearest_completion->description,
                            s_achievements_locals.achievement_nearest_completion->badge_path,
                            s_achievements_locals.achievement_nearest_completion->measured_progress,
                            s_achievements_locals.achievement_nearest_completion->measured_percent);
    text_pos.y += paragraph_spacing;
  }

  if (s_achievements_locals.most_recent_progress_update.has_value())
  {
    if (Timer::ConvertValueToSeconds(Timer::GetCurrentValue() -
                                     s_achievements_locals.most_recent_progress_update->show_time) <
        PAUSE_MENU_PROGRESS_DISPLAY_TIME)
    {
      buffer.format(ICON_FA_RULER_HORIZONTAL " {}",
                    TRANSLATE_DISAMBIG_SV("Achievements", "Last Progress Update", "Pause Menu"));
      draw_achievement_in_box(buffer, s_achievements_locals.most_recent_progress_update->title,
                              s_achievements_locals.most_recent_progress_update->description,
                              s_achievements_locals.most_recent_progress_update->badge_path,
                              s_achievements_locals.most_recent_progress_update->measured_progress,
                              s_achievements_locals.most_recent_progress_update->measured_percent);
      text_pos.y += paragraph_spacing;
    }
    else
    {
      s_achievements_locals.most_recent_progress_update.reset();
    }
  }

  // Challenge indicators

  if (const std::span<const Achievements::ActiveChallengeIndicator> challenge_indicators =
        Achievements::GetActiveChallengeIndicators();
      !challenge_indicators.empty())
  {
    box_height = box_padding + box_padding + UIStyle.MediumFontSize;
    for (size_t i = 0; i < challenge_indicators.size(); i++)
    {
      const Achievements::ActiveChallengeIndicator& indicator = challenge_indicators[i];
      box_height += paragraph_spacing + get_achievement_height(indicator.achievement->description) +
                    ((i == (challenge_indicators.size() - 1)) ? 0.0f : paragraph_spacing);
    }

    box_min = ImVec2(box_min.x, box_max.y + box_margin);
    box_max = ImVec2(box_min.x + box_width, box_min.y + box_height);
    text_pos = ImVec2(box_min.x + box_padding, box_min.y + box_padding);

    dl->AddRectFilled(box_min, box_max, box_background_color, box_rounding);

    buffer.format(ICON_FA_STOPWATCH " {}",
                  TRANSLATE_DISAMBIG_SV("Achievements", "Active Challenge Achievements", "Pause Menu"));
    dl->AddText(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight, text_pos, box_title_text_color,
                IMSTR_START_END(buffer));
    text_pos.y += UIStyle.MediumFontSize;

    for (const Achievements::ActiveChallengeIndicator& indicator : challenge_indicators)
    {
      text_pos.y += paragraph_spacing;
      draw_achievement_with_summary(indicator.achievement->title, indicator.achievement->description,
                                    indicator.badge_path);
      text_pos.y += paragraph_spacing;
    }
  }
}

void FullscreenUI::OpenAchievementsWindow()
{
  // NOTE: Called from CPU thread.
  if (!System::IsValid())
    return;

  const auto lock = Achievements::GetLock();
  if (!Achievements::IsActive() || !Achievements::HasAchievements())
  {
    ShowToast(std::string(), Achievements::IsActive() ? FSUI_STR("This game has no achievements.") :
                                                        FSUI_STR("Achievements are not enabled."));
    return;
  }

  GPUThread::RunOnThread([]() {
    Initialize();

    PauseForMenuOpen(false);
    ForceKeyNavEnabled();

    BeginTransition(SHORT_TRANSITION_TIME, &SwitchToAchievements);
  });
}

void FullscreenUI::SwitchToAchievements()
{
  const auto lock = Achievements::GetLock();
  if (!Achievements::HasAchievements())
  {
    ClosePauseMenuImmediately();
    return;
  }

  s_achievements_locals.achievement_badge_paths = {};

  if (s_achievements_locals.achievement_list)
    rc_client_destroy_achievement_list(s_achievements_locals.achievement_list);
  s_achievements_locals.achievement_list = rc_client_create_achievement_list(
    Achievements::GetClient(), RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE_AND_UNOFFICIAL,
    RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS /*RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE*/);
  if (!s_achievements_locals.achievement_list)
  {
    ERROR_LOG("rc_client_create_achievement_list() returned null");
    ClosePauseMenuImmediately();
    return;
  }

  // sort unlocked achievements by unlock time
  for (size_t i = 0; i < s_achievements_locals.achievement_list->num_buckets; i++)
  {
    const rc_client_achievement_bucket_t* bucket = &s_achievements_locals.achievement_list->buckets[i];
    if (bucket->bucket_type == RC_CLIENT_ACHIEVEMENT_BUCKET_UNLOCKED)
    {
      std::sort(bucket->achievements, bucket->achievements + bucket->num_achievements,
                [](const rc_client_achievement_t* a, const rc_client_achievement_t* b) {
                  return a->unlock_time > b->unlock_time;
                });
    }
  }

  SwitchToMainWindow(MainWindowType::Achievements);
}

void FullscreenUI::DrawAchievementsWindow()
{
  const auto lock = Achievements::GetLock();

  // achievements can get turned off via the main UI
  if (!s_achievements_locals.achievement_list)
  {
    ReturnToPreviousWindow();
    return;
  }

  static constexpr float alpha = 0.8f;
  static constexpr float heading_alpha = 0.95f;

  const rc_client_user_game_summary_t& summary = Achievements::GetGameSummary();
  const float heading_height_unscaled = ((summary.beaten_time > 0 || summary.completed_time) ? 122.0f : 102.0f) +
                                        ((summary.num_unsupported_achievements > 0) ? 20.0f : 0.0f);

  const ImVec4 background = ModAlpha(UIStyle.BackgroundColor, alpha);
  const ImVec4 heading_background = ModAlpha(UIStyle.BackgroundColor, heading_alpha);
  const ImVec2 display_size = ImGui::GetIO().DisplaySize;
  const float heading_height = LayoutScale(heading_height_unscaled);
  bool close_window = false;

  if (BeginFullscreenWindow(ImVec2(), ImVec2(display_size.x, heading_height), "achievements_heading",
                            heading_background, 0.0f, ImVec2(10.0f, 10.0f),
                            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration |
                              ImGuiWindowFlags_NoScrollWithMouse))
  {
    const ImVec2 pos = ImGui::GetCursorScreenPos() + ImGui::GetStyle().FramePadding;
    const float spacing = LayoutScale(LAYOUT_MENU_ITEM_TITLE_SUMMARY_SPACING);
    const float image_size = LayoutScale(75.0f);

    if (const std::string& path = Achievements::GetGameIconPath(); !path.empty())
    {
      GPUTexture* badge = GetCachedTextureAsync(path);
      if (badge)
      {
        ImGui::GetWindowDrawList()->AddImage(badge, pos, pos + ImVec2(image_size, image_size), ImVec2(0.0f, 0.0f),
                                             ImVec2(1.0f, 1.0f), IM_COL32(255, 255, 255, 255));
      }
    }

    float left = pos.x + image_size + LayoutScale(10.0f);
    float right = pos.x + GetMenuButtonAvailableWidth();
    float top = pos.y;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    SmallString text;
    ImVec2 text_size;

    close_window = (FloatingButton(ICON_FA_SQUARE_XMARK, 10.0f, 10.0f, 1.0f, 0.0f, true) || WantsToCloseMenu());

    const ImRect title_bb(ImVec2(left, top), ImVec2(right, top + UIStyle.LargeFontSize));
    text.assign(Achievements::GetGameTitle());

    if (rc_client_get_hardcore_enabled(Achievements::GetClient()))
      text.append(TRANSLATE_SV("Achievements", " (Hardcore Mode)"));

    top += UIStyle.LargeFontSize + spacing;

    RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, title_bb.Min, title_bb.Max,
                              ImGui::GetColorU32(ImGuiCol_Text), text, nullptr, ImVec2(0.0f, 0.0f), 0.0f, &title_bb);

    const ImRect summary_bb(ImVec2(left, top), ImVec2(right, top + UIStyle.MediumFontSize));
    if (summary.num_core_achievements > 0)
    {
      text.assign(ICON_EMOJI_UNLOCKED " ");
      if (summary.num_unlocked_achievements == summary.num_core_achievements)
      {
        text.append(TRANSLATE_PLURAL_SSTR("Achievements", "You have unlocked all achievements and earned %n points!",
                                          "Point count", summary.points_unlocked));
      }
      else
      {
        text.append_format(
          TRANSLATE_FS("Achievements",
                       "You have unlocked {0} of {1} achievements, earning {2} of {3} possible points."),
          summary.num_unlocked_achievements, summary.num_core_achievements, summary.points_unlocked,
          summary.points_core);
      }
    }
    else
    {
      text.format(ICON_FA_BAN " {}", TRANSLATE_SV("Achievements", "This game has no achievements."));
    }

    top += UIStyle.MediumFontSize + spacing;

    RenderShadowedTextClipped(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight, summary_bb.Min,
                              summary_bb.Max, ImGui::GetColorU32(DarkerColor(ImGui::GetStyle().Colors[ImGuiCol_Text])),
                              text, nullptr, ImVec2(0.0f, 0.0f), 0.0f, &summary_bb);

    if (summary.num_unsupported_achievements)
    {
      text.format("{} {}", ICON_EMOJI_WARNING,
                  TRANSLATE_PLURAL_SSTR("Achievements",
                                        "%n achievements are not supported by DuckStation and cannot be unlocked.",
                                        "Unsupported achievement count", summary.num_unsupported_achievements));

      const ImRect unsupported_bb(ImVec2(left, top), ImVec2(right, top + UIStyle.MediumFontSize));
      RenderShadowedTextClipped(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight, unsupported_bb.Min,
                                unsupported_bb.Max,
                                ImGui::GetColorU32(DarkerColor(ImGui::GetStyle().Colors[ImGuiCol_Text])), text, nullptr,
                                ImVec2(0.0f, 0.0f), 0.0f, &unsupported_bb);

      top += UIStyle.MediumFontSize + spacing;
    }

    if (summary.beaten_time > 0 || summary.completed_time > 0)
    {
      text.assign(ICON_EMOJI_CHECKMARK_BUTTON " ");

      if (summary.beaten_time > 0)
      {
        const std::string beaten_time =
          Host::FormatNumber(Host::NumberFormatType::ShortDate, static_cast<s64>(summary.beaten_time));
        if (summary.completed_time > 0)
        {
          const std::string completion_time =
            Host::FormatNumber(Host::NumberFormatType::ShortDate, static_cast<s64>(summary.completed_time));
          text.append_format(TRANSLATE_FS("Achievements", "Game was beaten on {0}, and completed on {1}."), beaten_time,
                             completion_time);
        }
        else
        {
          text.append_format(TRANSLATE_FS("Achievements", "Game was beaten on {0}."), beaten_time);
        }
      }
      else
      {
        const std::string completion_time =
          Host::FormatNumber(Host::NumberFormatType::ShortDate, static_cast<s64>(summary.completed_time));
        text.append_format(TRANSLATE_FS("Achievements", "Game was completed on {0}."), completion_time);
      }

      const ImRect beaten_bb(ImVec2(left, top), ImVec2(right, top + UIStyle.MediumFontSize));
      RenderShadowedTextClipped(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight, beaten_bb.Min,
                                beaten_bb.Max, ImGui::GetColorU32(DarkerColor(ImGui::GetStyle().Colors[ImGuiCol_Text])),
                                text, nullptr, ImVec2(0.0f, 0.0f), 0.0f, &beaten_bb);

      top += UIStyle.MediumFontSize + spacing;
    }

    if (summary.num_core_achievements > 0)
    {
      const float progress_height = LayoutScale(20.0f);
      const float progress_rounding = LayoutScale(5.0f);
      const ImRect progress_bb(ImVec2(left, top), ImVec2(right, top + progress_height));
      const float fraction =
        static_cast<float>(summary.num_unlocked_achievements) / static_cast<float>(summary.num_core_achievements);
      dl->AddRectFilled(progress_bb.Min, progress_bb.Max, ImGui::GetColorU32(UIStyle.PrimaryDarkColor),
                        progress_rounding);
      if (summary.num_unlocked_achievements > 0)
      {
        ImGui::RenderRectFilledRangeH(dl, progress_bb, ImGui::GetColorU32(UIStyle.SecondaryColor), 0.0f, fraction,
                                      progress_rounding);
      }

      text.format("{}%", static_cast<u32>(std::round(fraction * 100.0f)));
      text_size = UIStyle.Font->CalcTextSizeA(UIStyle.MediumFontSize, UIStyle.BoldFontWeight, FLT_MAX, 0.0f,
                                              IMSTR_START_END(text));
      const ImVec2 text_pos(progress_bb.Min.x + ((progress_bb.Max.x - progress_bb.Min.x) / 2.0f) - (text_size.x / 2.0f),
                            progress_bb.Min.y + ((progress_bb.Max.y - progress_bb.Min.y) / 2.0f) -
                              (text_size.y / 2.0f));
      dl->AddText(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight, text_pos,
                  ImGui::GetColorU32(UIStyle.PrimaryTextColor), IMSTR_START_END(text));
      // top += progress_height + spacing;
    }
  }
  EndFullscreenWindow();

  // See note in FullscreenUI::DrawSettingsWindow().
  if (IsFocusResetFromWindowChange())
    ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));

  if (BeginFullscreenWindow(ImVec2(0.0f, heading_height),
                            ImVec2(display_size.x, display_size.y - heading_height - LayoutScale(LAYOUT_FOOTER_HEIGHT)),
                            "achievements", background, 0.0f,
                            ImVec2(LAYOUT_MENU_WINDOW_X_PADDING, LAYOUT_MENU_WINDOW_Y_PADDING), 0))
  {
    static bool buckets_collapsed[NUM_RC_CLIENT_ACHIEVEMENT_BUCKETS] = {};
    static constexpr std::pair<const char*, const char*> bucket_names[NUM_RC_CLIENT_ACHIEVEMENT_BUCKETS] = {
      {ICON_FA_TRIANGLE_EXCLAMATION, TRANSLATE_NOOP("Achievements", "Unknown")},
      {ICON_FA_LOCK, TRANSLATE_NOOP("Achievements", "Locked")},
      {ICON_FA_UNLOCK, TRANSLATE_NOOP("Achievements", "Unlocked")},
      {ICON_FA_TRIANGLE_EXCLAMATION, TRANSLATE_NOOP("Achievements", "Unsupported")},
      {ICON_FA_CIRCLE_QUESTION, TRANSLATE_NOOP("Achievements", "Unofficial")},
      {ICON_FA_UNLOCK, TRANSLATE_NOOP("Achievements", "Recently Unlocked")},
      {ICON_FA_STOPWATCH, TRANSLATE_NOOP("Achievements", "Active Challenges")},
      {ICON_FA_RULER_HORIZONTAL, TRANSLATE_NOOP("Achievements", "Almost There")},
      {ICON_FA_TRIANGLE_EXCLAMATION, TRANSLATE_NOOP("Achievements", "Unsynchronized")},
    };

    ResetFocusHere();
    BeginMenuButtons();

    for (u32 bucket_type : {RC_CLIENT_ACHIEVEMENT_BUCKET_ACTIVE_CHALLENGE,
                            RC_CLIENT_ACHIEVEMENT_BUCKET_RECENTLY_UNLOCKED, RC_CLIENT_ACHIEVEMENT_BUCKET_ALMOST_THERE,
                            RC_CLIENT_ACHIEVEMENT_BUCKET_UNLOCKED, RC_CLIENT_ACHIEVEMENT_BUCKET_LOCKED,
                            RC_CLIENT_ACHIEVEMENT_BUCKET_UNOFFICIAL, RC_CLIENT_ACHIEVEMENT_BUCKET_UNSUPPORTED})
    {
      for (u32 bucket_idx = 0; bucket_idx < s_achievements_locals.achievement_list->num_buckets; bucket_idx++)
      {
        const rc_client_achievement_bucket_t& bucket = s_achievements_locals.achievement_list->buckets[bucket_idx];
        if (bucket.bucket_type != bucket_type)
          continue;

        DebugAssert(bucket.bucket_type < NUM_RC_CLIENT_ACHIEVEMENT_BUCKETS);

        // TODO: Once subsets are supported, this will need to change.
        bool& bucket_collapsed = buckets_collapsed[bucket.bucket_type];
        bucket_collapsed ^= MenuHeadingButton(
          TinyString::from_format("{} {}", bucket_names[bucket.bucket_type].first,
                                  Host::TranslateToStringView("Achievements", bucket_names[bucket.bucket_type].second)),
          bucket_collapsed ? ICON_FA_CHEVRON_DOWN : ICON_FA_CHEVRON_UP, UIStyle.MediumLargeFontSize);
        if (!bucket_collapsed)
        {
          for (u32 i = 0; i < bucket.num_achievements; i++)
            DrawAchievement(bucket.achievements[i]);
        }
      }
    }

    EndMenuButtons();
  }
  EndFullscreenWindow();

  SetFullscreenStatusText(std::array{
    std::make_pair(ICON_PF_ACHIEVEMENTS_MISSABLE, TRANSLATE_SV("Achievements", "Missable")),
    std::make_pair(ICON_PF_ACHIEVEMENTS_PROGRESSION, TRANSLATE_SV("Achievements", "Progression")),
    std::make_pair(ICON_PF_ACHIEVEMENTS_WIN, TRANSLATE_SV("Achievements", "Win Condition")),
    std::make_pair(ICON_FA_LOCK, TRANSLATE_SV("Achievements", "Locked")),
    std::make_pair(ICON_FA_UNLOCK, TRANSLATE_SV("Achievements", "Unlocked")),
  });

  if (IsGamepadInputSource())
  {
    SetFullscreenFooterText(
      std::array{std::make_pair(ICON_PF_XBOX_DPAD_UP_DOWN, TRANSLATE_SV("Achievements", "Change Selection")),
                 std::make_pair(ICON_PF_BUTTON_A, TRANSLATE_SV("Achievements", "View Details")),
                 std::make_pair(ICON_PF_BUTTON_B, TRANSLATE_SV("Achievements", "Back"))});
  }
  else
  {
    SetFullscreenFooterText(
      std::array{std::make_pair(ICON_PF_ARROW_UP ICON_PF_ARROW_DOWN, TRANSLATE_SV("Achievements", "Change Selection")),
                 std::make_pair(ICON_PF_ENTER, TRANSLATE_SV("Achievements", "View Details")),
                 std::make_pair(ICON_PF_ESC, TRANSLATE_SV("Achievements", "Back"))});
  }

  if (close_window)
    ReturnToPreviousWindow();
}

void FullscreenUI::DrawAchievement(const rc_client_achievement_t* cheevo)
{
  static constexpr float progress_height_unscaled = 20.0f;
  static constexpr float progress_spacing_unscaled = 5.0f;
  static constexpr float progress_rounding_unscaled = 5.0f;

  const float spacing = LayoutScale(LAYOUT_MENU_ITEM_TITLE_SUMMARY_SPACING);
  const u32 text_color = ImGui::GetColorU32(UIStyle.SecondaryTextColor);
  const u32 summary_color = ImGui::GetColorU32(DarkerColor(UIStyle.SecondaryTextColor));
  const u32 rarity_color = ImGui::GetColorU32(DarkerColor(DarkerColor(UIStyle.SecondaryTextColor)));

  const ImVec2 image_size = LayoutScale(50.0f, 50.0f);
  const bool is_unlocked = (cheevo->state == RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED);
  const std::string_view measured_progress(cheevo->measured_progress);
  const bool is_measured = !is_unlocked && !measured_progress.empty();
  const float unlock_rarity_height = spacing + UIStyle.MediumFontSize;
  const ImVec2 points_template_size = UIStyle.Font->CalcTextSizeA(
    UIStyle.MediumFontSize, UIStyle.NormalFontWeight, FLT_MAX, 0.0f, TRANSLATE("Achievements", "XXX points"));
  const float avail_width = GetMenuButtonAvailableWidth();
  const size_t summary_length = std::strlen(cheevo->description);
  const float summary_wrap_width = (avail_width - (image_size.x + spacing + spacing) - points_template_size.x);
  const ImVec2 summary_text_size =
    UIStyle.Font->CalcTextSizeA(UIStyle.MediumFontSize, UIStyle.NormalFontWeight, FLT_MAX, summary_wrap_width,
                                cheevo->description, cheevo->description + summary_length);

  const float content_height = UIStyle.LargeFontSize + spacing + summary_text_size.y + unlock_rarity_height +
                               LayoutScale(is_measured ? progress_height_unscaled : 0.0f) +
                               LayoutScale(LAYOUT_MENU_ITEM_EXTRA_HEIGHT);
  ImRect bb;
  bool visible, hovered;
  const bool clicked =
    MenuButtonFrame(TinyString::from_format("chv_{}", cheevo->id), content_height, true, &bb, &visible, &hovered);
  if (!visible)
    return;

  const std::string& badge_path =
    GetCachedAchievementBadgePath(cheevo, cheevo->state != RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED);

  if (!badge_path.empty())
  {
    GPUTexture* badge = GetCachedTextureAsync(badge_path);
    if (badge)
    {
      const ImRect image_bb = CenterImage(ImRect(bb.Min, bb.Min + image_size), badge);
      ImGui::GetWindowDrawList()->AddImage(badge, image_bb.Min, image_bb.Max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                                           IM_COL32(255, 255, 255, 255));
    }
  }

  SmallString text;

  const float midpoint = bb.Min.y + UIStyle.LargeFontSize + spacing;
  text = TRANSLATE_PLURAL_SSTR("Achievements", "%n points", "Achievement points", cheevo->points);
  const ImVec2 points_size =
    UIStyle.Font->CalcTextSizeA(UIStyle.MediumFontSize, UIStyle.NormalFontWeight, FLT_MAX, 0.0f, IMSTR_START_END(text));
  const float points_template_start = bb.Max.x - points_template_size.x;
  const float points_start = points_template_start + ((points_template_size.x - points_size.x) * 0.5f);

  std::string_view right_icon_text;
  switch (cheevo->type)
  {
    case RC_CLIENT_ACHIEVEMENT_TYPE_MISSABLE:
      right_icon_text = ICON_PF_ACHIEVEMENTS_MISSABLE; // Missable
      break;

    case RC_CLIENT_ACHIEVEMENT_TYPE_PROGRESSION:
      right_icon_text = ICON_PF_ACHIEVEMENTS_PROGRESSION; // Progression
      break;

    case RC_CLIENT_ACHIEVEMENT_TYPE_WIN:
      right_icon_text = ICON_PF_ACHIEVEMENTS_WIN; // Win Condition
      break;

      // Just use the lock for standard achievements.
    case RC_CLIENT_ACHIEVEMENT_TYPE_STANDARD:
    default:
      right_icon_text = is_unlocked ? ICON_FA_UNLOCK : ICON_FA_LOCK;
      break;
  }

  const ImVec2 right_icon_size = UIStyle.Font->CalcTextSizeA(UIStyle.LargeFontSize, UIStyle.BoldFontWeight, FLT_MAX,
                                                             0.0f, IMSTR_START_END(right_icon_text));

  const float text_start_x = bb.Min.x + image_size.x + LayoutScale(15.0f);
  const ImRect title_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(points_start, midpoint));
  const ImRect summary_bb(ImVec2(text_start_x, midpoint), ImVec2(points_start, midpoint + summary_text_size.y));
  const ImRect unlock_rarity_bb(summary_bb.Min.x, summary_bb.Max.y + spacing, summary_bb.Max.x,
                                summary_bb.Max.y + unlock_rarity_height);
  const ImRect points_bb(ImVec2(points_start, midpoint), bb.Max);
  const ImRect lock_bb(ImVec2(points_template_start + ((points_template_size.x - right_icon_size.x) * 0.5f), bb.Min.y),
                       ImVec2(bb.Max.x, midpoint));

  RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, title_bb.Min, title_bb.Max,
                            text_color, cheevo->title, nullptr, ImVec2(0.0f, 0.0f), 0.0f, &title_bb);
  RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, lock_bb.Min, lock_bb.Max,
                            text_color, right_icon_text, &right_icon_size, ImVec2(0.0f, 0.0f), 0.0f, &lock_bb);
  RenderShadowedTextClipped(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.NormalFontWeight, points_bb.Min,
                            points_bb.Max, summary_color, text, &points_size, ImVec2(0.0f, 0.0f), 0.0f, &points_bb);

  if (cheevo->description && summary_length > 0)
  {
    RenderShadowedTextClipped(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.NormalFontWeight, summary_bb.Min,
                              summary_bb.Max, summary_color, std::string_view(cheevo->description, summary_length),
                              &summary_text_size, ImVec2(0.0f, 0.0f), summary_wrap_width, &summary_bb);
  }

  // display hc if hc is active
  const float rarity_to_display =
    rc_client_get_hardcore_enabled(Achievements::GetClient()) ? cheevo->rarity_hardcore : cheevo->rarity;

  if (is_unlocked)
  {
    const std::string date =
      Host::FormatNumber(Host::NumberFormatType::LongDateTime, static_cast<s64>(cheevo->unlock_time));
    text.format(TRANSLATE_FS("Achievements", "Unlocked: {} | {:.1f}% of players have this achievement"), date,
                rarity_to_display);

    RenderShadowedTextClipped(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.NormalFontWeight, unlock_rarity_bb.Min,
                              unlock_rarity_bb.Max, rarity_color, text, nullptr, ImVec2(0.0f, 0.0f), 0.0f,
                              &unlock_rarity_bb);
  }
  else
  {
    text.format(TRANSLATE_FS("Achievements", "{:.1f}% of players have this achievement"), rarity_to_display);
    RenderShadowedTextClipped(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.NormalFontWeight, unlock_rarity_bb.Min,
                              unlock_rarity_bb.Max, rarity_color, text, nullptr, ImVec2(0.0f, 0.0f), 0.0f,
                              &unlock_rarity_bb);
  }

  if (!is_unlocked && is_measured)
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float progress_height = LayoutScale(progress_height_unscaled);
    const float progress_spacing = LayoutScale(progress_spacing_unscaled);
    const float progress_rounding = LayoutScale(progress_rounding_unscaled);
    const ImRect progress_bb(summary_bb.Min.x, unlock_rarity_bb.Max.y + progress_spacing,
                             summary_bb.Max.x - progress_spacing,
                             unlock_rarity_bb.Max.y + progress_spacing + progress_height);
    const float fraction = cheevo->measured_percent * 0.01f;
    dl->AddRectFilled(progress_bb.Min, progress_bb.Max, ImGui::GetColorU32(UIStyle.PrimaryDarkColor),
                      progress_rounding);
    ImGui::RenderRectFilledRangeH(dl, progress_bb, ImGui::GetColorU32(UIStyle.SecondaryColor), 0.0f, fraction,
                                  progress_rounding);

    const ImVec2 text_size = UIStyle.Font->CalcTextSizeA(UIStyle.MediumFontSize, UIStyle.NormalFontWeight, FLT_MAX,
                                                         0.0f, IMSTR_START_END(measured_progress));
    const ImVec2 text_pos(progress_bb.Min.x + ((progress_bb.Max.x - progress_bb.Min.x) / 2.0f) - (text_size.x / 2.0f),
                          progress_bb.Min.y + ((progress_bb.Max.y - progress_bb.Min.y) / 2.0f) - (text_size.y / 2.0f));
    dl->AddText(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.NormalFontWeight, text_pos,
                ImGui::GetColorU32(UIStyle.PrimaryTextColor), IMSTR_START_END(measured_progress));
  }

  if (clicked)
  {
    const SmallString url = SmallString::from_format(fmt::runtime(ACHEIVEMENT_DETAILS_URL_TEMPLATE), cheevo->id);
    INFO_LOG("Opening achievement details: {}", url);
    Host::OpenURL(url);
  }
}

void FullscreenUI::OpenLeaderboardsWindow()
{
  if (!System::IsValid())
    return;

  const auto lock = Achievements::GetLock();
  if (!Achievements::IsActive() || !Achievements::HasLeaderboards())
  {
    ShowToast(std::string(), Achievements::IsActive() ? FSUI_STR("This game has no leaderboards.") :
                                                        FSUI_STR("Achievements are not enabled."));
    return;
  }

  GPUThread::RunOnThread([]() {
    Initialize();

    PauseForMenuOpen(false);
    ForceKeyNavEnabled();

    BeginTransition(SHORT_TRANSITION_TIME, &SwitchToLeaderboards);
  });
}

void FullscreenUI::SwitchToLeaderboards()
{
  const auto lock = Achievements::GetLock();
  if (!Achievements::HasLeaderboards())
  {
    ClosePauseMenuImmediately();
    return;
  }

  s_achievements_locals.achievement_badge_paths = {};
  CloseLeaderboard();
  if (s_achievements_locals.leaderboard_list)
    rc_client_destroy_leaderboard_list(s_achievements_locals.leaderboard_list);
  s_achievements_locals.leaderboard_list =
    rc_client_create_leaderboard_list(Achievements::GetClient(), RC_CLIENT_LEADERBOARD_LIST_GROUPING_NONE);
  if (!s_achievements_locals.leaderboard_list)
  {
    ERROR_LOG("rc_client_create_leaderboard_list() returned null");
    ClosePauseMenuImmediately();
    return;
  }

  SwitchToMainWindow(MainWindowType::Leaderboards);
}

void FullscreenUI::DrawLeaderboardsWindow()
{
  static constexpr float alpha = 0.8f;
  static constexpr float heading_alpha = 0.95f;
  static constexpr float heading_height_unscaled = 110.0f;
  static constexpr float tab_height_unscaled = 50.0f;

  const auto lock = Achievements::GetLock();
  if (!s_achievements_locals.leaderboard_list)
  {
    ReturnToPreviousWindow();
    return;
  }

  const bool is_leaderboard_open = (s_achievements_locals.open_leaderboard != nullptr);
  bool close_leaderboard_on_exit = false;

  SmallString text;

  const ImVec4 background = ModAlpha(UIStyle.BackgroundColor, alpha);
  const ImVec4 heading_background = ModAlpha(UIStyle.BackgroundColor, heading_alpha);
  const ImVec2 display_size = ImGui::GetIO().DisplaySize;
  const u32 text_color = ImGui::GetColorU32(ImGuiCol_Text);
  const float spacing = LayoutScale(10.0f);
  const float spacing_small = ImFloor(spacing * 0.5f);
  float heading_height = LayoutScale(heading_height_unscaled);
  if (is_leaderboard_open)
  {
    // tabs
    heading_height += spacing * 2.0f + LayoutScale(tab_height_unscaled) + spacing * 2.0f;

    // Add space for a legend - spacing + 1 line of text + spacing + line
    heading_height += UIStyle.LargeFontSize;
  }

  const float rank_column_width =
    UIStyle.Font
      ->CalcTextSizeA(UIStyle.LargeFontSize, UIStyle.BoldFontWeight, std::numeric_limits<float>::max(), -1.0f, "99999")
      .x;
  const float name_column_width = UIStyle.Font
                                    ->CalcTextSizeA(UIStyle.LargeFontSize, UIStyle.BoldFontWeight,
                                                    std::numeric_limits<float>::max(), -1.0f, "WWWWWWWWWWWWWWWWWWWWWW")
                                    .x;
  const float time_column_width = UIStyle.Font
                                    ->CalcTextSizeA(UIStyle.LargeFontSize, UIStyle.BoldFontWeight,
                                                    std::numeric_limits<float>::max(), -1.0f, "WWWWWWWWWWW")
                                    .x;
  const float column_spacing = spacing * 2.0f;

  if (BeginFullscreenWindow(ImVec2(), ImVec2(display_size.x, heading_height), "leaderboards_heading",
                            heading_background, 0.0f, ImVec2(10.0f, 10.0f),
                            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration |
                              ImGuiWindowFlags_NoScrollWithMouse))
  {
    const ImVec2 heading_pos = ImGui::GetCursorScreenPos() + ImGui::GetStyle().FramePadding;
    const float image_size = LayoutScale(85.0f);

    if (const std::string& icon = Achievements::GetGameIconPath(); !icon.empty())
    {
      GPUTexture* badge = GetCachedTextureAsync(icon);
      if (badge)
      {
        ImGui::GetWindowDrawList()->AddImage(badge, heading_pos, heading_pos + ImVec2(image_size, image_size),
                                             ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), IM_COL32(255, 255, 255, 255));
      }
    }

    float left = heading_pos.x + image_size + spacing;
    float right = heading_pos.x + GetMenuButtonAvailableWidth();
    float top = heading_pos.y;

    if (!is_leaderboard_open)
    {
      if (FloatingButton(ICON_FA_SQUARE_XMARK, 10.0f, 10.0f, 1.0f, 0.0f, true) || WantsToCloseMenu())
      {
        ReturnToPreviousWindow();
      }
    }
    else
    {
      if (FloatingButton(ICON_FA_SQUARE_CARET_LEFT, 10.0f, 10.0f, 1.0f, 0.0f, true) || WantsToCloseMenu())
      {
        close_leaderboard_on_exit = true;
      }
    }

    const ImRect title_bb(ImVec2(left, top), ImVec2(right, top + UIStyle.LargeFontSize));
    text.assign(Achievements::GetGameTitle());

    top += UIStyle.LargeFontSize + spacing_small;

    RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, title_bb.Min, title_bb.Max,
                              text_color, text, nullptr, ImVec2(0.0f, 0.0f), 0.0f, &title_bb);

    u32 summary_color;
    if (is_leaderboard_open)
    {
      const ImRect subtitle_bb(ImVec2(left, top), ImVec2(right, top + UIStyle.LargeFontSize));
      text.assign(s_achievements_locals.open_leaderboard->title);

      top += UIStyle.LargeFontSize + spacing_small;

      RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, subtitle_bb.Min,
                                subtitle_bb.Max,
                                ImGui::GetColorU32(DarkerColor(ImGui::GetStyle().Colors[ImGuiCol_Text])), text, nullptr,
                                ImVec2(0.0f, 0.0f), 0.0f, &subtitle_bb);

      text.assign(s_achievements_locals.open_leaderboard->description);
      summary_color = ImGui::GetColorU32(DarkerColor(DarkerColor(ImGui::GetStyle().Colors[ImGuiCol_Text])));
    }
    else
    {
      u32 count = 0;
      for (u32 i = 0; i < s_achievements_locals.leaderboard_list->num_buckets; i++)
        count += s_achievements_locals.leaderboard_list->buckets[i].num_leaderboards;
      text = TRANSLATE_PLURAL_SSTR("Achievements", "This game has %n leaderboards.", "Leaderboard count", count);
      summary_color = ImGui::GetColorU32(DarkerColor(ImGui::GetStyle().Colors[ImGuiCol_Text]));
    }

    const ImRect summary_bb(ImVec2(left, top), ImVec2(right, top + UIStyle.MediumFontSize));
    top += UIStyle.MediumFontSize + spacing_small;

    RenderShadowedTextClipped(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight, summary_bb.Min,
                              summary_bb.Max, summary_color, text, nullptr, ImVec2(0.0f, 0.0f), 0.0f, &summary_bb);

    if (!is_leaderboard_open && !Achievements::IsHardcoreModeActive())
    {
      const ImRect hardcore_warning_bb(ImVec2(left, top), ImVec2(right, top + UIStyle.MediumFontSize));
      top += UIStyle.MediumFontSize + spacing_small;

      text.format(
        ICON_EMOJI_WARNING " {}",
        TRANSLATE_SV("Achievements",
                     "Submitting scores is disabled because hardcore mode is off. Leaderboards are read-only."));

      RenderShadowedTextClipped(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight, hardcore_warning_bb.Min,
                                hardcore_warning_bb.Max,
                                ImGui::GetColorU32(DarkerColor(DarkerColor(ImGui::GetStyle().Colors[ImGuiCol_Text]))),
                                text, nullptr, ImVec2(0.0f, 0.0f), 0.0f, &hardcore_warning_bb);
    }

    if (is_leaderboard_open)
    {
      const float avail_width = GetMenuButtonAvailableWidth();
      const float tab_width = avail_width * 0.2f;
      const float tab_spacing = LayoutScale(20.0f);
      const float tab_left_padding = (avail_width - ((tab_width * 2.0f) + tab_spacing)) * 0.5f;
      ImGui::SetCursorScreenPos(ImVec2(heading_pos.x + tab_left_padding, top + spacing * 2.0f));
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                          LayoutScale(LAYOUT_MENU_WINDOW_X_PADDING, LAYOUT_MENU_WINDOW_Y_PADDING));

      if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, false) ||
          ImGui::IsKeyPressed(ImGuiKey_NavGamepadTweakSlow, false) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) ||
          ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, false) ||
          ImGui::IsKeyPressed(ImGuiKey_NavGamepadTweakFast, false) || ImGui::IsKeyPressed(ImGuiKey_RightArrow, false))
      {
        s_achievements_locals.is_showing_all_leaderboard_entries =
          !s_achievements_locals.is_showing_all_leaderboard_entries;
        QueueResetFocus(FocusResetType::ViewChanged);
      }

      for (const bool show_all : {false, true})
      {
        const std::string_view title =
          show_all ? TRANSLATE_SV("Achievements", "Show Best") : TRANSLATE_SV("Achievements", "Show Nearby");
        if (NavTab(title, s_achievements_locals.is_showing_all_leaderboard_entries == show_all, true, tab_width))
        {
          s_achievements_locals.is_showing_all_leaderboard_entries = show_all;
          QueueResetFocus(FocusResetType::ViewChanged);
        }

        if (!show_all)
          ImGui::SetCursorPosX(ImGui::GetCursorPosX() + tab_spacing);
      }

      ImGui::PopStyleVar();

      ImGui::SetCursorPos(ImVec2(0.0f, ImGui::GetCursorPosY() + LayoutScale(tab_height_unscaled) + spacing * 2.0f));

      ImVec2 column_heading_pos = ImGui::GetCursorScreenPos();
      float end_x = column_heading_pos.x + ImGui::GetContentRegionAvail().x;

      // add padding from the window below, don't want the menu items butted up against the edge
      column_heading_pos.x += LayoutScale(LAYOUT_MENU_WINDOW_X_PADDING);
      end_x -= LayoutScale(LAYOUT_MENU_WINDOW_X_PADDING);

      // and the padding for the frame itself
      column_heading_pos.x += LayoutScale(LAYOUT_MENU_BUTTON_X_PADDING);
      end_x -= LayoutScale(LAYOUT_MENU_BUTTON_X_PADDING);

      const u32 heading_color = ImGui::GetColorU32(DarkerColor(ImGui::GetStyle().Colors[ImGuiCol_Text]));

      const float midpoint = column_heading_pos.y + UIStyle.LargeFontSize + LayoutScale(4.0f);
      float text_start_x = column_heading_pos.x;

      const ImRect rank_bb(ImVec2(text_start_x, column_heading_pos.y), ImVec2(end_x, midpoint));
      RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, rank_bb.Min, rank_bb.Max,
                                heading_color, TRANSLATE_SV("Achievements", "Rank"), nullptr, ImVec2(0.0f, 0.0f), 0.0f,
                                &rank_bb);
      text_start_x += rank_column_width + column_spacing;

      const ImRect user_bb(ImVec2(text_start_x, column_heading_pos.y), ImVec2(end_x, midpoint));
      RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, user_bb.Min, user_bb.Max,
                                heading_color, TRANSLATE_SV("Achievements", "Name"), nullptr, ImVec2(0.0f, 0.0f), 0.0f,
                                &user_bb);
      text_start_x += name_column_width + column_spacing;

      static const char* value_headings[NUM_RC_CLIENT_LEADERBOARD_FORMATS] = {
        TRANSLATE_NOOP("Achievements", "Time"),
        TRANSLATE_NOOP("Achievements", "Score"),
        TRANSLATE_NOOP("Achievements", "Value"),
      };

      const ImRect score_bb(ImVec2(text_start_x, column_heading_pos.y), ImVec2(end_x, midpoint));
      RenderShadowedTextClipped(
        UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, score_bb.Min, score_bb.Max, heading_color,
        Host::TranslateToStringView("Achievements",
                                    value_headings[std::min<u8>(s_achievements_locals.open_leaderboard->format,
                                                                NUM_RC_CLIENT_LEADERBOARD_FORMATS - 1)]),
        nullptr, ImVec2(0.0f, 0.0f), 0.0f, &score_bb);
      text_start_x += time_column_width + column_spacing;

      const ImRect date_bb(ImVec2(text_start_x, column_heading_pos.y), ImVec2(end_x, midpoint));
      RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, date_bb.Min, date_bb.Max,
                                heading_color, TRANSLATE_SV("Achievements", "Date Submitted"), nullptr,
                                ImVec2(0.0f, 0.0f), 0.0f, &date_bb);

      const float line_thickness = LayoutScale(1.0f);
      const float line_padding = LayoutScale(5.0f);
      const ImVec2 line_start(column_heading_pos.x, column_heading_pos.y + UIStyle.LargeFontSize + line_padding);
      const ImVec2 line_end(end_x, line_start.y);
      ImGui::GetWindowDrawList()->AddLine(line_start, line_end, ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                          line_thickness);

      // keep imgui happy
      ImGui::Dummy(ImVec2(end_x - column_heading_pos.x, column_heading_pos.y - line_end.y));
    }
  }
  EndFullscreenWindow();

  // See note in FullscreenUI::DrawSettingsWindow().
  if (IsFocusResetFromWindowChange())
    ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));

  if (!is_leaderboard_open)
  {
    if (BeginFullscreenWindow(
          ImVec2(0.0f, heading_height),
          ImVec2(display_size.x, display_size.y - heading_height - LayoutScale(LAYOUT_FOOTER_HEIGHT)), "leaderboards",
          background, 0.0f, ImVec2(LAYOUT_MENU_WINDOW_X_PADDING, LAYOUT_MENU_WINDOW_Y_PADDING), 0))
    {
      ResetFocusHere();
      BeginMenuButtons();

      for (u32 bucket_index = 0; bucket_index < s_achievements_locals.leaderboard_list->num_buckets; bucket_index++)
      {
        const rc_client_leaderboard_bucket_t& bucket = s_achievements_locals.leaderboard_list->buckets[bucket_index];
        for (u32 i = 0; i < bucket.num_leaderboards; i++)
          DrawLeaderboardListEntry(bucket.leaderboards[i]);
      }

      EndMenuButtons();
    }
    EndFullscreenWindow();

    if (IsGamepadInputSource())
    {
      SetFullscreenFooterText(
        std::array{std::make_pair(ICON_PF_XBOX_DPAD_UP_DOWN, TRANSLATE_SV("Achievements", "Change Selection")),
                   std::make_pair(ICON_PF_BUTTON_A, TRANSLATE_SV("Achievements", "Open Leaderboard")),
                   std::make_pair(ICON_PF_BUTTON_B, TRANSLATE_SV("Achievements", "Back"))});
    }
    else
    {
    }

    SetFullscreenFooterText(
      std::array{std::make_pair(ICON_PF_ARROW_UP ICON_PF_ARROW_DOWN, TRANSLATE_SV("Achievements", "Change Selection")),
                 std::make_pair(ICON_PF_ENTER, TRANSLATE_SV("Achievements", "Open Leaderboard")),
                 std::make_pair(ICON_PF_ESC, TRANSLATE_SV("Achievements", "Back"))});
  }
  else
  {
    if (BeginFullscreenWindow(
          ImVec2(0.0f, heading_height),
          ImVec2(display_size.x, display_size.y - heading_height - LayoutScale(LAYOUT_FOOTER_HEIGHT)), "leaderboard",
          background, 0.0f, ImVec2(LAYOUT_MENU_WINDOW_X_PADDING, LAYOUT_MENU_WINDOW_Y_PADDING), 0))
    {
      BeginMenuButtons();
      ResetFocusHere();

      if (!s_achievements_locals.is_showing_all_leaderboard_entries)
      {
        if (s_achievements_locals.leaderboard_nearby_entries)
        {
          for (u32 i = 0; i < s_achievements_locals.leaderboard_nearby_entries->num_entries; i++)
          {
            DrawLeaderboardEntry(s_achievements_locals.leaderboard_nearby_entries->entries[i], i,
                                 static_cast<s32>(i) == s_achievements_locals.leaderboard_nearby_entries->user_index,
                                 rank_column_width, name_column_width, time_column_width, column_spacing);
          }
        }
        else
        {
          const ImVec2 pos_min(0.0f, heading_height);
          const ImVec2 pos_max(display_size.x, display_size.y);
          RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, pos_min, pos_max,
                                    text_color,
                                    TRANSLATE_SV("Achievements", "Downloading leaderboard data, please wait..."),
                                    nullptr, ImVec2(0.5f, 0.5f), 0.0f);
        }
      }
      else
      {
        for (const rc_client_leaderboard_entry_list_t* list : s_achievements_locals.leaderboard_entry_lists)
        {
          for (u32 i = 0; i < list->num_entries; i++)
          {
            DrawLeaderboardEntry(list->entries[i], i, static_cast<s32>(i) == list->user_index, rank_column_width,
                                 name_column_width, time_column_width, column_spacing);
          }
        }

        bool visible;
        text.format(ICON_FA_HOURGLASS_HALF " {}", TRANSLATE_SV("Achievements", "Loading..."));
        MenuButtonWithVisibilityQuery(text, text, {}, {}, &visible, false);
        if (visible && !s_achievements_locals.leaderboard_fetch_handle)
          FetchNextLeaderboardEntries();
      }

      EndMenuButtons();
    }
    EndFullscreenWindow();

    if (IsGamepadInputSource())
    {
      SetFullscreenFooterText(
        std::array{std::make_pair(ICON_PF_XBOX_DPAD_LEFT_RIGHT, TRANSLATE_SV("Achievements", "Change Page")),
                   std::make_pair(ICON_PF_XBOX_DPAD_UP_DOWN, TRANSLATE_SV("Achievements", "Change Selection")),
                   std::make_pair(ICON_PF_BUTTON_A, TRANSLATE_SV("Achievements", "View Profile")),
                   std::make_pair(ICON_PF_BUTTON_B, TRANSLATE_SV("Achievements", "Back"))});
    }
    else
    {
      SetFullscreenFooterText(std::array{
        std::make_pair(ICON_PF_ARROW_LEFT ICON_PF_ARROW_RIGHT, TRANSLATE_SV("Achievements", "Change Page")),
        std::make_pair(ICON_PF_ARROW_UP ICON_PF_ARROW_DOWN, TRANSLATE_SV("Achievements", "Change Selection")),
        std::make_pair(ICON_PF_ENTER, TRANSLATE_SV("Achievements", "View Profile")),
        std::make_pair(ICON_PF_ESC, TRANSLATE_SV("Achievements", "Back"))});
    }
  }

  if (close_leaderboard_on_exit)
  {
    BeginTransition([]() {
      CloseLeaderboard();
      QueueResetFocus(FocusResetType::ViewChanged);
    });
  }
}

void FullscreenUI::DrawLeaderboardEntry(const rc_client_leaderboard_entry_t& entry, u32 index, bool is_self,
                                        float rank_column_width, float name_column_width, float time_column_width,
                                        float column_spacing)
{
  ImRect bb;
  bool visible, hovered;
  bool pressed = MenuButtonFrame(entry.user, UIStyle.LargeFontSize, true, &bb, &visible, &hovered);
  if (!visible)
    return;

  const float midpoint = bb.Min.y + UIStyle.LargeFontSize + LayoutScale(4.0f);
  float text_start_x = bb.Min.x;
  SmallString text;

  text.format("{}", entry.rank);

  const u32 text_color =
    is_self ? IM_COL32(255, 242, 0, 255) :
              ImGui::GetColorU32(((index % 2) == 0) ? DarkerColor(ImGui::GetStyle().Colors[ImGuiCol_Text]) :
                                                      ImGui::GetStyle().Colors[ImGuiCol_Text]);

  const ImRect rank_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
  RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, rank_bb.Min, rank_bb.Max,
                            text_color, text, nullptr, ImVec2(0.0f, 0.0f), 0.0f, &rank_bb);
  text_start_x += rank_column_width + column_spacing;

  const float icon_size = bb.Max.y - bb.Min.y;
  const ImRect icon_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
  GPUTexture* icon_tex = nullptr;
  if (auto it = std::find_if(s_achievements_locals.leaderboard_user_icon_paths.begin(),
                             s_achievements_locals.leaderboard_user_icon_paths.end(),
                             [&entry](const auto& it) { return it.first == &entry; });
      it != s_achievements_locals.leaderboard_user_icon_paths.end())
  {
    if (!it->second.empty())
      icon_tex = GetCachedTextureAsync(it->second);
  }
  else
  {
    std::string path = Achievements::GetLeaderboardUserBadgePath(&entry);
    if (!path.empty())
    {
      icon_tex = GetCachedTextureAsync(path);
      s_achievements_locals.leaderboard_user_icon_paths.emplace_back(&entry, std::move(path));
    }
  }
  if (icon_tex)
  {
    const ImRect fit_icon_bb = CenterImage(ImRect(icon_bb.Min, icon_bb.Min + ImVec2(icon_size, icon_size)), icon_tex);
    ImGui::GetWindowDrawList()->AddImage(reinterpret_cast<ImTextureID>(icon_tex), fit_icon_bb.Min, fit_icon_bb.Max);
  }

  const ImRect user_bb(ImVec2(text_start_x + column_spacing + icon_size, bb.Min.y), ImVec2(bb.Max.x, midpoint));
  RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, user_bb.Min, user_bb.Max,
                            text_color, entry.user, nullptr, ImVec2(0.0f, 0.0f), 0.0f, &user_bb);
  text_start_x += name_column_width + column_spacing;

  const ImRect score_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
  RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, score_bb.Min, score_bb.Max,
                            text_color, entry.display, nullptr, ImVec2(0.0f, 0.0f), 0.0f, &score_bb);
  text_start_x += time_column_width + column_spacing;

  const ImRect time_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));

  const std::string submit_time =
    Host::FormatNumber(Host::NumberFormatType::LongDateTime, static_cast<s64>(entry.submitted));
  RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, time_bb.Min, time_bb.Max,
                            text_color, submit_time, nullptr, ImVec2(0.0f, 0.0f), 0.0f, &time_bb);

  if (pressed)
  {
    const SmallString url = SmallString::from_format(fmt::runtime(PROFILE_DETAILS_URL_TEMPLATE), entry.user);
    INFO_LOG("Opening profile details: {}", url);
    Host::OpenURL(url);
  }
}
void FullscreenUI::DrawLeaderboardListEntry(const rc_client_leaderboard_t* lboard)
{
  SmallString title;
  title.format("{}##{}", lboard->title, lboard->id);

  std::string_view summary;
  if (lboard->description && lboard->description[0] != '\0')
    summary = lboard->description;

  if (MenuButton(title, summary))
    BeginTransition([id = lboard->id]() { OpenLeaderboardById(id); });
}

bool FullscreenUI::OpenLeaderboardById(u32 leaderboard_id)
{
  const auto lock = Achievements::GetLock();
  if (!Achievements::IsActive())
    return false;

  rc_client_t* const client = Achievements::GetClient();
  const rc_client_leaderboard_t* lb = rc_client_get_leaderboard_info(client, leaderboard_id);
  if (!lb)
    return false;

  DEV_LOG("Opening leaderboard '{}' ({})", lb->title, lb->id);

  CloseLeaderboard();

  s_achievements_locals.open_leaderboard = lb;
  s_achievements_locals.is_showing_all_leaderboard_entries = false;
  s_achievements_locals.leaderboard_fetch_handle = rc_client_begin_fetch_leaderboard_entries_around_user(
    client, lb->id, LEADERBOARD_NEARBY_ENTRIES_TO_FETCH, LeaderboardFetchNearbyCallback, nullptr);
  QueueResetFocus(FocusResetType::Other);
  return true;
}

void FullscreenUI::LeaderboardFetchNearbyCallback(int result, const char* error_message,
                                                  rc_client_leaderboard_entry_list_t* list, rc_client_t* client,
                                                  void* callback_userdata)
{
  // should be already locked
  s_achievements_locals.leaderboard_fetch_handle = nullptr;

  if (result != RC_OK)
  {
    ShowToast(TRANSLATE("Achievements", "Leaderboard download failed"), error_message);
    CloseLeaderboard();
    return;
  }

  if (s_achievements_locals.leaderboard_nearby_entries)
    rc_client_destroy_leaderboard_entry_list(s_achievements_locals.leaderboard_nearby_entries);
  s_achievements_locals.leaderboard_nearby_entries = list;
  QueueResetFocus(FocusResetType::Other);
}

void FullscreenUI::LeaderboardFetchAllCallback(int result, const char* error_message,
                                               rc_client_leaderboard_entry_list_t* list, rc_client_t* client,
                                               void* callback_userdata)
{
  // should be already locked
  s_achievements_locals.leaderboard_fetch_handle = nullptr;

  if (result != RC_OK)
  {
    ShowToast(TRANSLATE("Achievements", "Leaderboard download failed"), error_message);
    CloseLeaderboard();
    return;
  }

  if (s_achievements_locals.leaderboard_entry_lists.empty())
    QueueResetFocus(FocusResetType::Other);

  s_achievements_locals.leaderboard_entry_lists.push_back(list);
}

void FullscreenUI::FetchNextLeaderboardEntries()
{
  u32 start = 1;
  for (rc_client_leaderboard_entry_list_t* list : s_achievements_locals.leaderboard_entry_lists)
    start += list->num_entries;

  DEV_LOG("Fetching entries {} to {}", start, start + LEADERBOARD_ALL_FETCH_SIZE);

  rc_client_t* const client = Achievements::GetClient();
  if (s_achievements_locals.leaderboard_fetch_handle)
    rc_client_abort_async(client, s_achievements_locals.leaderboard_fetch_handle);
  s_achievements_locals.leaderboard_fetch_handle =
    rc_client_begin_fetch_leaderboard_entries(client, s_achievements_locals.open_leaderboard->id, start,
                                              LEADERBOARD_ALL_FETCH_SIZE, LeaderboardFetchAllCallback, nullptr);
}

void FullscreenUI::CloseLeaderboard()
{
  s_achievements_locals.leaderboard_user_icon_paths.clear();

  for (auto iter = s_achievements_locals.leaderboard_entry_lists.rbegin();
       iter != s_achievements_locals.leaderboard_entry_lists.rend(); ++iter)
    rc_client_destroy_leaderboard_entry_list(*iter);
  s_achievements_locals.leaderboard_entry_lists.clear();

  if (s_achievements_locals.leaderboard_nearby_entries)
  {
    rc_client_destroy_leaderboard_entry_list(s_achievements_locals.leaderboard_nearby_entries);
    s_achievements_locals.leaderboard_nearby_entries = nullptr;
  }

  if (s_achievements_locals.leaderboard_fetch_handle)
  {
    rc_client_abort_async(Achievements::GetClient(), s_achievements_locals.leaderboard_fetch_handle);
    s_achievements_locals.leaderboard_fetch_handle = nullptr;
  }

  s_achievements_locals.open_leaderboard = nullptr;
}

#endif // __ANDROID__
