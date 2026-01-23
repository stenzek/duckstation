// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "achievements_private.h"
#include "fullscreenui_private.h"
#include "gpu_thread.h"
#include "host.h"
#include "system.h"

#include "util/gpu_texture.h"
#include "util/imgui_manager.h"
#include "util/translation.h"

#include "common/assert.h"
#include "common/easing.h"
#include "common/log.h"
#include "common/string_util.h"
#include "common/time_helpers.h"
#include "common/timer.h"

#include "IconsEmoji.h"
#include "IconsPromptFont.h"

LOG_CHANNEL(FullscreenUI);

#ifndef __ANDROID__

namespace FullscreenUI {

static constexpr const char* ACHEIVEMENT_DETAILS_URL_TEMPLATE = "https://retroachievements.org/achievement/{}";
static constexpr const char* PROFILE_DETAILS_URL_TEMPLATE = "https://retroachievements.org/user/{}";

static constexpr float WINDOW_ALPHA = 0.9f;
static constexpr float WINDOW_HEADING_ALPHA = 0.95f;

static constexpr u32 LEADERBOARD_NEARBY_ENTRIES_TO_FETCH = 20;
static constexpr u32 LEADERBOARD_ALL_FETCH_SIZE = 50;

// How long the last progress update is shown in the pause menu.
static constexpr float PAUSE_MENU_PROGRESS_DISPLAY_TIME = 60.0f;

// Notification animation times.
static constexpr float NOTIFICATION_APPEAR_ANIMATION_TIME = 0.2f;
static constexpr float NOTIFICATION_DISAPPEAR_ANIMATION_TIME = 0.5f;

static constexpr float CHALLENGE_INDICATOR_FADE_IN_TIME = 0.1f;
static constexpr float CHALLENGE_INDICATOR_FADE_OUT_TIME = 0.3f;

namespace {

struct Notification
{
  std::string key;
  std::string title;
  std::string text;
  std::string note;
  std::string badge_path;
  u64 start_time;
  u64 move_time;
  float duration;
  float target_y;
  float last_y;
  u16 min_width;
  AchievementNotificationNoteType note_type;
};

struct PauseMenuAchievementInfo
{
  std::string title;
  std::string description;
  std::string badge_path;
  u32 achievement_id;
  float measured_percent;
};

struct PauseMenuAchievementInfoWithPoints : PauseMenuAchievementInfo
{
  u32 points;
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

struct PauseMenuLeaderboardInfo
{
  std::string title;
  std::string description;
  std::string tracker_value;
  u32 leaderboard_id;
  u32 format;
};

} // namespace

static void DrawNotifications(NotificationLayout& layout);
static void DrawIndicators(NotificationLayout& layout);
static void UpdateAchievementOverlaysRunIdle();

static void AddSubsetInfo(const rc_client_subset_t* subset);
static bool IsCoreSubsetOpen();
static void SetCurrentSubsetID(u32 subset_id);
static void DrawSubsetSelector();
template<typename T>
static void CollectSubsetsFromList(const T* list, bool include_achievements, bool include_leaderboards);
template<typename T>
static bool IsBucketVisibleInCurrentSubset(const T& bucket);

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
static bool DrawLeaderboardEntry(const rc_client_leaderboard_entry_t& entry, u32 index, bool is_self,
                                 float rank_column_width, float name_column_width, float time_column_width,
                                 float column_spacing, std::time_t current_time, const std::tm& current_tm);
static void DrawLeaderboardLoadingIndicator(float pos_y, float avail_height, bool short_text);
static SmallString FormatRelativeTimestamp(std::time_t timestamp, std::time_t current_time, const std::tm& current_tm);

namespace {

struct SubsetInfo
{
  std::string full_name;
  std::string short_name;
  std::string badge_path;
  u32 subset_id;
  u32 num_leaderboards;
  rc_client_user_game_summary_t summary;
};

struct AchievementsLocals
{
  std::vector<Notification> notifications;

  // Shared by both achievements and leaderboards, TODO: add all filter
  std::vector<SubsetInfo> subset_info_list;
  const SubsetInfo* open_subset = nullptr;

  rc_client_achievement_list_t* achievement_list = nullptr;
  std::vector<std::tuple<const void*, std::string, bool>> achievement_badge_paths;

  std::optional<PauseMenuAchievementInfoWithPoints> most_recent_unlock;
  std::optional<PauseMenuMeasuredAchievementInfo> achievement_nearest_completion;
  std::optional<PauseMenuTimedMeasuredAchievementInfo> most_recent_progress_update;
  std::vector<PauseMenuLeaderboardInfo> active_leaderboards;

  rc_client_leaderboard_list_t* leaderboard_list = nullptr;
  const rc_client_leaderboard_t* open_leaderboard = nullptr;
  rc_client_async_handle_t* leaderboard_fetch_handle = nullptr;
  std::vector<rc_client_leaderboard_entry_list_t*> leaderboard_entry_lists;
  std::vector<std::pair<const rc_client_leaderboard_entry_t*, std::string>> leaderboard_user_icon_paths;
  rc_client_leaderboard_entry_list_t* leaderboard_nearby_entries;
  bool is_showing_all_leaderboard_entries = false;
  bool has_fetched_all_leaderboard_entries = false;

  u32 last_open_subset_id = 0;
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

  s_achievements_locals.notifications = {};

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

  s_achievements_locals.open_subset = nullptr;
  s_achievements_locals.subset_info_list.clear();
  s_achievements_locals.last_open_subset_id = 0;

  s_achievements_locals.most_recent_unlock.reset();
  s_achievements_locals.achievement_nearest_completion.reset();

  UpdateAchievementOverlaysRunIdle();
}

void FullscreenUI::DrawAchievementsOverlays()
{
  if (!Achievements::IsActive())
    return;

  const auto lock = Achievements::GetLock();

  NotificationLayout layout(g_settings.achievements_notification_location);
  DrawNotifications(layout);

  if (Achievements::HasActiveGame())
  {
    // need to group them together if they're in the same location
    if (g_settings.achievements_indicator_location != layout.GetLocation())
      layout = NotificationLayout(g_settings.achievements_indicator_location);

    DrawIndicators(layout);
  }
}

void FullscreenUI::AddAchievementNotification(std::string key, float duration, std::string image_path,
                                              std::string title, std::string text, std::string note,
                                              AchievementNotificationNoteType note_type, u16 min_width)
{
  const bool prev_had_notifications = s_achievements_locals.notifications.empty();
  const Timer::Value current_time = Timer::GetCurrentValue();

  if (!key.empty())
  {
    for (auto it = s_achievements_locals.notifications.begin(); it != s_achievements_locals.notifications.end(); ++it)
    {
      if (it->key == key)
      {
        it->duration = duration;
        it->title = std::move(title);
        it->text = std::move(text);
        it->note = std::move(note);
        it->badge_path = std::move(image_path);
        it->min_width = min_width;
        it->note_type = note_type;

        // Don't fade it in again
        const float time_passed = static_cast<float>(Timer::ConvertValueToSeconds(current_time - it->start_time));
        it->start_time =
          current_time - Timer::ConvertSecondsToValue(std::min(time_passed, NOTIFICATION_APPEAR_ANIMATION_TIME));
        return;
      }
    }
  }

  Notification notif;
  notif.key = std::move(key);
  notif.duration = duration;
  notif.title = std::move(title);
  notif.text = std::move(text);
  notif.note = std::move(note);
  notif.badge_path = std::move(image_path);
  notif.start_time = current_time;
  notif.move_time = current_time;
  notif.target_y = -1.0f;
  notif.last_y = -1.0f;
  notif.min_width = min_width;
  notif.note_type = note_type;
  s_achievements_locals.notifications.push_back(std::move(notif));

  if (!prev_had_notifications)
    UpdateAchievementOverlaysRunIdle();
}

void FullscreenUI::DrawNotifications(NotificationLayout& layout)
{
  if (s_achievements_locals.notifications.empty())
    return;

  static constexpr float MOVE_DURATION = 0.5f;
  const Timer::Value current_time = Timer::GetCurrentValue();

  const float horizontal_padding = FullscreenUI::LayoutScale(20.0f);
  const float vertical_padding = FullscreenUI::LayoutScale(15.0f);
  const float horizontal_spacing = FullscreenUI::LayoutScale(10.0f);
  const float larger_horizontal_spacing = FullscreenUI::LayoutScale(18.0f);
  const float vertical_spacing = FullscreenUI::LayoutScale(4.0f);
  const float badge_size = FullscreenUI::LayoutScale(48.0f);
  const float min_width = FullscreenUI::LayoutScale(200.0f);
  const float max_width = FullscreenUI::LayoutScale(600.0f);
  const float max_text_width = max_width - badge_size - (horizontal_padding * 2.0f) - horizontal_spacing;
  const float min_height = (vertical_padding * 2.0f) + badge_size;
  const float rounding = FullscreenUI::LayoutScale(20.0f);
  const float min_rounded_width = rounding * 2.0f;
  const float note_icon_padding = LayoutScale(30.0f);

  ImFont*& font = UIStyle.Font;
  const float& title_font_size = UIStyle.LargeFontSize;
  const float& title_font_weight = UIStyle.BoldFontWeight;
  const float& text_font_size = UIStyle.MediumFontSize;
  const float& text_font_weight = UIStyle.NormalFontWeight;
  const float& note_text_size = UIStyle.MediumFontSize;
  const float& note_text_weight = UIStyle.BoldFontWeight;
  const float& note_icon_size = UIStyle.LargeFontSize;

  const ImVec4 left_background_color = DarkerColor(UIStyle.ToastBackgroundColor, 1.3f);
  const ImVec4 right_background_color = DarkerColor(UIStyle.ToastBackgroundColor, 0.8f);
  ImDrawList* const dl = ImGui::GetForegroundDrawList();

  for (auto iter = s_achievements_locals.notifications.begin(); iter != s_achievements_locals.notifications.end();)
  {
    Notification& notif = *iter;
    const float time_passed = static_cast<float>(Timer::ConvertValueToSeconds(current_time - notif.start_time));
    if (time_passed >= notif.duration)
    {
      iter = s_achievements_locals.notifications.erase(iter);
      continue;
    }

    // place note to the right of the title
    GPUTexture* note_image = nullptr;
    float note_font_size, note_font_weight, note_offset_y, note_spacing;
    ImVec2 note_size;
    switch (notif.note_type)
    {
      case AchievementNotificationNoteType::Text:
        note_font_size = note_text_size;
        note_font_weight = note_text_weight;
        note_size = font->CalcTextSizeA(note_font_size, note_font_weight, FLT_MAX, 0.0f, IMSTR_START_END(notif.note));
        note_offset_y = 0.0f;
        note_spacing = larger_horizontal_spacing;
        break;

      case AchievementNotificationNoteType::IconText:
        note_font_size = note_icon_size;
        note_font_weight = UIStyle.NormalFontWeight;
        note_size = font->CalcTextSizeA(note_font_size, note_font_weight, FLT_MAX, 0.0f, IMSTR_START_END(notif.note));
        note_offset_y = 0.0f;
        note_spacing = note_icon_padding;
        break;

      case AchievementNotificationNoteType::Spinner:
        note_font_size = 0.0f;
        note_font_weight = 0.0f;
        note_size = ImVec2(note_text_size, note_text_size);
        note_offset_y = ImFloor((note_icon_size - note_text_size) * 0.5f);
        note_spacing = note_icon_padding;
        break;

      case AchievementNotificationNoteType::Image:
        note_font_size = 0.0f;
        note_font_weight = 0.0f;
        note_image = GetCachedTexture(notif.note, static_cast<u32>(note_text_size), static_cast<u32>(note_text_size));
        note_size = (note_image && note_image->GetWidth() > note_image->GetHeight()) ?
                      ImVec2(note_text_size * (static_cast<float>(note_image->GetWidth()) /
                                               static_cast<float>(note_image->GetHeight())),
                             note_text_size) :
                      ImVec2(note_text_size, note_text_size * (static_cast<float>(note_image->GetHeight()) /
                                                               static_cast<float>(note_image->GetWidth())));
        note_offset_y = ImFloor((note_icon_size - note_text_size) * 0.5f);
        note_spacing = note_icon_padding;
        break;

      case AchievementNotificationNoteType::None:
      default:
        note_font_size = 0.0f;
        note_font_weight = 0.0f;
        note_size = ImVec2();
        note_offset_y = 0.0f;
        note_spacing = 0.0f;
        break;
    }

    const ImVec2 title_size = font->CalcTextSizeA(title_font_size, title_font_weight, max_text_width - note_size.x,
                                                  max_text_width - note_size.x, IMSTR_START_END(notif.title));
    const ImVec2 text_size = font->CalcTextSizeA(text_font_size, text_font_weight, max_text_width, max_text_width,
                                                 IMSTR_START_END(notif.text));

    const float box_width = std::max((horizontal_padding * 2.0f) + badge_size + horizontal_spacing +
                                       ImCeil(std::max(title_size.x + note_spacing + note_size.x, text_size.x)),
                                     std::max(static_cast<float>(LayoutScale(notif.min_width)), min_width));
    const float box_height =
      std::max((vertical_padding * 2.0f) + ImCeil(title_size.y) + vertical_spacing + ImCeil(text_size.y), min_height);

    const auto& [expected_pos, opacity] =
      layout.GetNextPosition(box_width, box_height, time_passed, notif.duration, NOTIFICATION_APPEAR_ANIMATION_TIME,
                             NOTIFICATION_DISAPPEAR_ANIMATION_TIME, 0.2f);

    float actual_y;
    if (!layout.IsVerticalAnimation() || opacity == 1.0f)
    {
      actual_y = notif.last_y;
      if (notif.target_y != expected_pos.y)
      {
        notif.move_time = current_time;
        notif.target_y = expected_pos.y;
        notif.last_y = (notif.last_y < 0.0f) ? expected_pos.y : notif.last_y;
        actual_y = notif.last_y;
      }
      else if (actual_y != expected_pos.y)
      {
        const float time_since_move = static_cast<float>(Timer::ConvertValueToSeconds(current_time - notif.move_time));
        if (time_since_move >= MOVE_DURATION)
        {
          notif.move_time = current_time;
          notif.last_y = notif.target_y;
          actual_y = notif.last_y;
        }
        else
        {
          const float frac = Easing::OutExpo(time_since_move / MOVE_DURATION);
          actual_y = notif.last_y - ((notif.last_y - notif.target_y) * frac);
        }
      }
    }
    else
    {
      actual_y = expected_pos.y;
    }

    const ImVec2 box_min(expected_pos.x, actual_y);
    const ImVec2 box_max(box_min.x + box_width, box_min.y + box_height);
    const float background_opacity = opacity * 0.95f;

    DrawRoundedGradientRect(
      dl, box_min, box_max, ImGui::GetColorU32(ModAlpha(left_background_color, background_opacity)),
      ImGui::GetColorU32(ModAlpha(ImLerp(left_background_color, right_background_color,
                                         (box_width - min_rounded_width) / (max_width - min_rounded_width)),
                                  background_opacity)),
      rounding);

    const ImVec2 badge_min(box_min.x + horizontal_padding, box_min.y + vertical_padding);
    const ImVec2 badge_max(badge_min.x + badge_size, badge_min.y + badge_size);
    if (!notif.badge_path.empty())
    {
      GPUTexture* tex = GetCachedTexture(notif.badge_path, static_cast<u32>(badge_size), static_cast<u32>(badge_size));
      if (tex)
      {
        dl->AddImage(tex, badge_min, badge_max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                     ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, opacity)));
      }
    }

    const u32 title_col = ImGui::GetColorU32(ModAlpha(UIStyle.ToastTextColor, opacity));
    const u32 text_col = ImGui::GetColorU32(ModAlpha(DarkerColor(UIStyle.ToastTextColor), opacity));

    const ImVec2 title_pos = ImVec2(badge_max.x + horizontal_spacing, box_min.y + vertical_padding);
    const ImRect title_bb = ImRect(title_pos, title_pos + title_size);
    RenderShadowedTextClipped(dl, font, title_font_size, title_font_weight, title_bb.Min, title_bb.Max, title_col,
                              notif.title, &title_size, ImVec2(0.0f, 0.0f), max_text_width - note_size.x, &title_bb);

    const ImVec2 text_pos = ImVec2(badge_max.x + horizontal_spacing, title_bb.Max.y + vertical_spacing);
    const ImRect text_bb = ImRect(text_pos, text_pos + text_size);
    RenderShadowedTextClipped(dl, font, text_font_size, text_font_weight, text_bb.Min, text_bb.Max, text_col,
                              notif.text, &text_size, ImVec2(0.0f, 0.0f), max_text_width, &text_bb);

    const ImVec2 note_pos =
      ImVec2((box_min.x + box_width) - horizontal_padding - note_size.x, box_min.y + vertical_padding + note_offset_y);
    switch (notif.note_type)
    {
      case AchievementNotificationNoteType::Text:
      case AchievementNotificationNoteType::IconText:
      {
        const ImRect note_bb = ImRect(note_pos, note_pos + note_size);
        RenderShadowedTextClipped(dl, font, note_font_size, note_font_weight, note_bb.Min, note_bb.Max, title_col,
                                  notif.note, &note_size, ImVec2(0.0f, 0.0f), max_text_width, &note_bb);
      }
      break;

      case AchievementNotificationNoteType::Spinner:
      {
        DrawSpinner(dl, note_pos, title_col, note_size.x, LayoutScale(4.0f));
      }
      break;

      case AchievementNotificationNoteType::Image:
      {
        if (note_image)
        {
          const ImRect image_rect = CenterImage(note_size, note_image);
          dl->AddImage(note_image, note_pos + image_rect.Min, note_pos + image_rect.Max);
        }
      }
      break;

      case AchievementNotificationNoteType::None:
      default:
        break;
    }

    ++iter;
  }

  // cleared?
  if (s_achievements_locals.notifications.empty())
    GPUThread::SetRunIdleReason(GPUThread::RunIdleReason::AchievementOverlaysActive, false);
}

void FullscreenUI::DrawIndicators(NotificationLayout& layout)
{
  static constexpr float INDICATOR_WIDTH_COEFF = 0.3f;

  static constexpr const float& font_size = UIStyle.MediumFontSize;
  static constexpr const float& font_weight = UIStyle.BoldFontWeight;

  static constexpr float bg_opacity = 0.8f;

  const float spacing = LayoutScale(10.0f);
  const float padding = LayoutScale(10.0f);
  const float rounding = LayoutScale(10.0f);
  const ImVec2 image_size = LayoutScale(50.0f, 50.0f);
  const ImGuiIO& io = ImGui::GetIO();
  ImDrawList* dl = ImGui::GetBackgroundDrawList();

  if (std::vector<Achievements::ActiveChallengeIndicator>& indicators = Achievements::GetActiveChallengeIndicators();
      !indicators.empty() &&
      (g_settings.achievements_challenge_indicator_mode == AchievementChallengeIndicatorMode::PersistentIcon ||
       g_settings.achievements_challenge_indicator_mode == AchievementChallengeIndicatorMode::TemporaryIcon))
  {
    const bool use_time_remaining =
      (g_settings.achievements_challenge_indicator_mode == AchievementChallengeIndicatorMode::TemporaryIcon);
    const float x_advance = image_size.x + spacing;
    const float total_width = image_size.x + (static_cast<float>(indicators.size() - 1) * x_advance);
    ImVec2 current_position = layout.GetFixedPosition(total_width, image_size.y);

    for (auto it = indicators.begin(); it != indicators.end();)
    {
      Achievements::ActiveChallengeIndicator& indicator = *it;
      bool active = indicator.active;
      if (use_time_remaining)
      {
        indicator.time_remaining = std::max(indicator.time_remaining - io.DeltaTime, 0.0f);
        active = (indicator.time_remaining > 0.0f);
      }

      const float target_opacity = active ? 1.0f : 0.0f;
      const float rate = active ? CHALLENGE_INDICATOR_FADE_IN_TIME : -CHALLENGE_INDICATOR_FADE_OUT_TIME;
      indicator.opacity =
        (indicator.opacity != target_opacity) ? ImSaturate(indicator.opacity + (io.DeltaTime / rate)) : target_opacity;

      GPUTexture* badge = FullscreenUI::GetCachedTextureAsync(indicator.badge_path);
      if (badge)
      {
        dl->AddImage(badge, current_position, current_position + image_size, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                     ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, indicator.opacity)));
      }

      current_position.x += x_advance;

      if (!indicator.active && indicator.opacity <= 0.01f)
      {
        DEV_LOG("Remove challenge indicator");
        it = indicators.erase(it);
      }
      else
      {
        ++it;
      }
    }
  }

  if (std::optional<Achievements::AchievementProgressIndicator>& indicator = Achievements::GetActiveProgressIndicator();
      indicator.has_value())
  {
    indicator->time += indicator->active ? io.DeltaTime : -io.DeltaTime;

    const ImVec4 left_background_color = DarkerColor(UIStyle.ToastBackgroundColor, 1.3f);
    const ImVec4 right_background_color = DarkerColor(UIStyle.ToastBackgroundColor, 0.8f);
    const ImVec2 progress_image_size = LayoutScale(32.0f, 32.0f);
    const std::string_view text = indicator->achievement->measured_progress;
    const ImVec2 text_size = UIStyle.Font->CalcTextSizeA(font_size, font_weight, FLT_MAX, 0.0f, IMSTR_START_END(text));
    const float box_width = progress_image_size.x + text_size.x + spacing + padding * 2.0f;
    const float box_height = progress_image_size.y + padding * 2.0f;

    const auto& [box_min, opacity] = layout.GetNextPosition(
      box_width, box_height, indicator->active, indicator->time, Achievements::INDICATOR_FADE_IN_TIME,
      Achievements::INDICATOR_FADE_OUT_TIME, INDICATOR_WIDTH_COEFF);
    const ImVec2 box_max = box_min + ImVec2(box_width, box_height);

    DrawRoundedGradientRect(dl, box_min, box_max,
                            ImGui::GetColorU32(ModAlpha(left_background_color, opacity * bg_opacity)),
                            ImGui::GetColorU32(ModAlpha(right_background_color, opacity * bg_opacity)), rounding);

    GPUTexture* const badge = FullscreenUI::GetCachedTextureAsync(indicator->badge_path);
    if (badge)
    {
      const ImVec2 badge_pos = box_min + ImVec2(padding, padding);
      dl->AddImage(badge, badge_pos, badge_pos + progress_image_size, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                   ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, opacity)));
    }

    const ImVec2 text_pos =
      box_min + ImVec2(padding + progress_image_size.x + spacing, (box_max.y - box_min.y - text_size.y) * 0.5f);
    const ImRect text_clip_rect(text_pos, box_max);
    RenderShadowedTextClipped(dl, UIStyle.Font, font_size, font_weight, text_pos, box_max,
                              ImGui::GetColorU32(ModAlpha(UIStyle.ToastTextColor, opacity)), text, &text_size,
                              ImVec2(0.0f, 0.0f), 0.0f, &text_clip_rect);

    if (!indicator->active && opacity <= 0.01f)
    {
      DEV_LOG("Remove progress indicator");
      indicator.reset();
    }
  }

  if (std::vector<Achievements::LeaderboardTrackerIndicator>& trackers =
        Achievements::GetLeaderboardTrackerIndicators();
      !trackers.empty())
  {
    const ImVec4 left_background_color = DarkerColor(UIStyle.ToastBackgroundColor, 1.3f);
    const ImVec4 right_background_color = DarkerColor(UIStyle.ToastBackgroundColor, 0.8f);
    TinyString tstr;

    const auto measure_tracker = [&tstr](const Achievements::LeaderboardTrackerIndicator& indicator) {
      tstr.assign(ICON_FA_STOPWATCH " ");
      for (u32 i = 0; i < indicator.text.length(); i++)
      {
        // 8 is typically the widest digit
        if (indicator.text[i] >= '0' && indicator.text[i] <= '9')
          tstr.append('8');
        else
          tstr.append(indicator.text[i]);
      }

      return UIStyle.Font->CalcTextSizeA(font_size, font_weight, FLT_MAX, 0.0f, IMSTR_START_END(tstr));
    };

    const auto draw_tracker = [&padding, &rounding, &dl, &left_background_color, &right_background_color, &tstr,
                               &measure_tracker](Achievements::LeaderboardTrackerIndicator& indicator,
                                                 const ImVec2& pos, float opacity) {
      const ImVec2 size = measure_tracker(indicator);
      const float box_width = size.x + padding * 2.0f;
      const float box_height = size.y + padding * 2.0f;
      const ImRect box(pos, ImVec2(pos.x + box_width, pos.y + box_height));

      DrawRoundedGradientRect(dl, box.Min, box.Max,
                              ImGui::GetColorU32(ModAlpha(left_background_color, opacity * bg_opacity)),
                              ImGui::GetColorU32(ModAlpha(right_background_color, opacity * bg_opacity)), rounding);

      tstr.format(ICON_FA_STOPWATCH " {}", indicator.text);

      const u32 text_col = ImGui::GetColorU32(ModAlpha(UIStyle.ToastTextColor, opacity));
      RenderShadowedTextClipped(dl, UIStyle.Font, font_size, font_weight,
                                ImVec2(box.Min.x + padding, box.Min.y + padding), box.Max, text_col, tstr, nullptr,
                                ImVec2(0.0f, 0.0f), 0.0f, &box);

      return size.x;
    };

    // animations are not currently handled for more than one tracker... but this should be rare
    if (trackers.size() > 1)
    {
      float total_width = 0.0f;
      float max_height = 0.0f;
      for (const Achievements::LeaderboardTrackerIndicator& indicator : trackers)
      {
        const ImVec2 size = measure_tracker(indicator);
        total_width += ((total_width > 0.0f) ? spacing : 0.0f) + size.x + padding * 2.0f;
        max_height = std::max(max_height, size.y);
      }

      ImVec2 current_pos = layout.GetFixedPosition(total_width, max_height + padding * 2.0f);
      for (auto it = trackers.begin(); it != trackers.end();)
      {
        Achievements::LeaderboardTrackerIndicator& indicator = *it;
        indicator.time += indicator.active ? io.DeltaTime : -io.DeltaTime;

        current_pos.x += draw_tracker(indicator, current_pos, 1.0f);

        if (!indicator.active)
        {
          DEV_LOG("Remove tracker indicator");
          it = trackers.erase(it);
        }
        else
        {
          ++it;
        }
      }
    }
    else
    {
      // don't need to precalc size here either :D
      Achievements::LeaderboardTrackerIndicator& indicator = trackers.front();
      indicator.time += indicator.active ? io.DeltaTime : -io.DeltaTime;

      const ImVec2 size = measure_tracker(indicator);
      const float box_width = size.x + padding * 2.0f;
      const float box_height = size.y + padding * 2.0f;
      const auto& [box_pos, opacity] = layout.GetNextPosition(
        box_width, box_height, indicator.active, indicator.time, Achievements::INDICATOR_FADE_IN_TIME,
        Achievements::INDICATOR_FADE_OUT_TIME, INDICATOR_WIDTH_COEFF);
      draw_tracker(indicator, box_pos, opacity);

      if (!indicator.active && opacity <= 0.01f)
      {
        DEV_LOG("Remove tracker indicator");
        trackers.clear();
      }
    }
  }
}

void FullscreenUI::UpdateAchievementOverlaysRunIdle()
{
  // early out if we're already on the GPU thread
  if (GPUThread::IsOnThread())
  {
    GPUThread::SetRunIdleReason(GPUThread::RunIdleReason::AchievementOverlaysActive,
                                !s_achievements_locals.notifications.empty());
    return;
  }

  // need to check it again once we're executing on the gpu thread, it could've changed since
  GPUThread::RunOnThread([]() {
    bool is_active;
    {
      const auto lock = Achievements::GetLock();
      is_active = !s_achievements_locals.notifications.empty();
    }
    GPUThread::SetRunIdleReason(GPUThread::RunIdleReason::AchievementOverlaysActive, is_active);
  });
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

  if constexpr (std::is_base_of_v<PauseMenuAchievementInfoWithPoints, T>)
    value->points = achievement->points;
  if constexpr (std::is_base_of_v<PauseMenuMeasuredAchievementInfo, T>)
    value->measured_progress = achievement->measured_progress;
  if constexpr (std::is_same_v<PauseMenuTimedMeasuredAchievementInfo, T>)
    value->show_time = Timer::GetCurrentValue();
}

void FullscreenUI::UpdateAchievementsPauseScreenInfo()
{
  const auto lock = Achievements::GetLock();
  if (!Achievements::HasActiveGame())
  {
    s_achievements_locals.most_recent_unlock.reset();
    s_achievements_locals.achievement_nearest_completion.reset();
    s_achievements_locals.active_leaderboards.clear();
    return;
  }

  rc_client_achievement_list_t* const achievements =
    Achievements::HasAchievements() ?
      rc_client_create_achievement_list(Achievements::GetClient(), RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE_AND_UNOFFICIAL,
                                        RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS) :
      nullptr;
  if (achievements)
  {
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
  else
  {
    s_achievements_locals.most_recent_unlock.reset();
    s_achievements_locals.achievement_nearest_completion.reset();
  }

  rc_client_leaderboard_list_t* const leaderboards =
    Achievements::HasLeaderboards() ?
      rc_client_create_leaderboard_list(Achievements::GetClient(), RC_CLIENT_LEADERBOARD_LIST_GROUPING_NONE) :
      nullptr;
  if (leaderboards)
  {
    std::vector<PauseMenuLeaderboardInfo>& active_lbs = s_achievements_locals.active_leaderboards;
    size_t num_active_lbs = 0;

    for (u32 i = 0; i < leaderboards->num_buckets; i++)
    {
      const rc_client_leaderboard_bucket_t& bucket = leaderboards->buckets[i];
      for (u32 j = 0; j < bucket.num_leaderboards; j++)
      {
        const rc_client_leaderboard_t* leaderboard = bucket.leaderboards[j];
        if (leaderboard->state != RC_CLIENT_LEADERBOARD_STATE_TRACKING)
          continue;

        // avoid alloc if unnecessary
        if (num_active_lbs >= active_lbs.size() || active_lbs[num_active_lbs].leaderboard_id != leaderboard->id)
        {
          if (num_active_lbs < active_lbs.size())
            active_lbs.erase(active_lbs.begin() + num_active_lbs, active_lbs.end());

          PauseMenuLeaderboardInfo& lbinfo = active_lbs.emplace_back();
          lbinfo.title = leaderboard->title;
          if (leaderboard->description)
            lbinfo.description = leaderboard->description;
        }

        if (leaderboard->tracker_value)
          active_lbs[num_active_lbs].tracker_value = leaderboard->tracker_value;

        num_active_lbs++;
      }
    }

    // remove extras
    if (num_active_lbs < active_lbs.size())
      active_lbs.erase(active_lbs.begin() + num_active_lbs, active_lbs.end());

    rc_client_destroy_leaderboard_list(leaderboards);
  }
  else
  {
    s_achievements_locals.active_leaderboards.clear();
  }
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
      ImGui::RenderRectFilledInRangeH(
        dl, progress_bb, ImGui::GetColorU32(DarkerColor(UIStyle.SecondaryColor)), progress_bb.Min.x,
        progress_bb.Min.x + (unlocked_fraction * progress_bb.GetWidth()), progress_rounding);
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

  const auto draw_achievement_in_box = [&box_margin, &box_width, &box_padding, &box_rounding, &box_content_width,
                                        &box_background_color, &box_min, &box_max, &badge_text_width, &dl,
                                        &box_title_text_color, &title_text_color, &text_color, &paragraph_spacing,
                                        &text_spacing, &progress_rounding, &text_pos, &badge_size](
                                         std::string_view box_title, std::string_view title,
                                         std::string_view description, const std::string& badge_path,
                                         std::string_view measured_progress, float measured_percent, u32 points) {
    const ImVec2 description_size =
      description.empty() ? ImVec2(0.0f, 0.0f) :
                            UIStyle.Font->CalcTextSizeA(UIStyle.MediumSmallFontSize, UIStyle.NormalFontWeight, FLT_MAX,
                                                        badge_text_width, IMSTR_START_END(description));

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
        ImGui::RenderRectFilledInRangeH(
          dl, progress_bb, ImGui::GetColorU32(DarkerColor(UIStyle.SecondaryColor)), progress_bb.Min.x,
          progress_bb.Min.x + ((measured_percent * 0.01f) * progress_bb.GetWidth()), progress_rounding);
      }

      const ImVec2 measured_progress_size =
        UIStyle.Font->CalcTextSizeA(UIStyle.MediumSmallFontSize, UIStyle.BoldFontWeight, FLT_MAX, badge_text_width,
                                    IMSTR_START_END(measured_progress));

      dl->AddText(
        UIStyle.Font, UIStyle.MediumSmallFontSize, UIStyle.BoldFontWeight,
        ImVec2(progress_bb.Min.x + ((progress_bb.Max.x - progress_bb.Min.x) / 2.0f) - (measured_progress_size.x / 2.0f),
               progress_bb.Min.y + ((progress_bb.Max.y - progress_bb.Min.y) / 2.0f) -
                 (measured_progress_size.y / 2.0f)),
        ImGui::GetColorU32(UIStyle.PrimaryTextColor), IMSTR_START_END(measured_progress));
    }

    text_pos.y += UIStyle.MediumFontSize + paragraph_spacing;

    const ImVec2 image_max = ImVec2(text_pos.x + badge_size, text_pos.y + badge_size);
    GPUTexture* const badge_tex = GetCachedTextureAsync(badge_path);
    dl->AddImage(badge_tex, text_pos, image_max);

    TinyString points_text;
    float points_width = 0.0f;
    if (points > 0)
    {
      points_text.format(ICON_EMOJI_TROPHY " {}", points);
      points_width = UIStyle.Font
                       ->CalcTextSizeA(UIStyle.MediumSmallFontSize, UIStyle.BoldFontWeight, FLT_MAX, 0.0f,
                                       IMSTR_START_END(points_text))
                       .x;
    }

    ImVec2 badge_text_pos = ImVec2(image_max.x + (text_spacing * 3.0f), text_pos.y);

    if (!title.empty())
    {
      clip_rect =
        ImVec4(badge_text_pos.x, badge_text_pos.y, badge_text_pos.x + badge_text_width - points_width, box_max.y);

      dl->AddText(UIStyle.Font, UIStyle.MediumSmallFontSize, UIStyle.BoldFontWeight, badge_text_pos, title_text_color,
                  IMSTR_START_END(title), 0.0f, &clip_rect);

      if (points > 0)
      {
        clip_rect = ImVec4(clip_rect.z, clip_rect.y, clip_rect.z + points_width, clip_rect.w);
        dl->AddText(UIStyle.Font, UIStyle.MediumSmallFontSize, UIStyle.BoldFontWeight, ImVec2(clip_rect.x, clip_rect.y),
                    title_text_color, IMSTR_START_END(points_text), 0.0f, &clip_rect);
      }

      badge_text_pos.y += UIStyle.MediumSmallFontSize;
    }

    if (!description.empty())
    {
      clip_rect = ImVec4(badge_text_pos.x, badge_text_pos.y, badge_text_pos.x + badge_text_width, box_max.y);

      badge_text_pos.y += text_spacing;
      dl->AddText(UIStyle.Font, UIStyle.MediumSmallFontSize, UIStyle.NormalFontWeight, badge_text_pos, text_color,
                  IMSTR_START_END(description), badge_text_width, &clip_rect);
      badge_text_pos.y += description_size.y;
    }
  };

  const auto get_achievement_height = [&badge_size, &badge_text_width, &text_spacing](std::string_view description) {
    const ImVec2 description_size =
      description.empty() ? ImVec2() :
                            UIStyle.Font->CalcTextSizeA(UIStyle.MediumSmallFontSize, UIStyle.NormalFontWeight, FLT_MAX,
                                                        badge_text_width, IMSTR_START_END(description));
    const float text_height =
      UIStyle.MediumSmallFontSize + (description.empty() ? 0.0f : (text_spacing + description_size.y));
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
                         ImVec2() :
                         UIStyle.Font->CalcTextSizeA(UIStyle.MediumSmallFontSize, UIStyle.NormalFontWeight, FLT_MAX,
                                                     badge_text_width, IMSTR_START_END(description));

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
      badge_text_pos.y += text_size.y;
    }

    text_pos.y = badge_text_pos.y;
  };

  if (s_achievements_locals.most_recent_unlock.has_value())
  {
    buffer.format(ICON_FA_LOCK_OPEN " {}", TRANSLATE_DISAMBIG_SV("Achievements", "Most Recent", "Pause Menu"));
    draw_achievement_in_box(
      buffer, s_achievements_locals.most_recent_unlock->title, s_achievements_locals.most_recent_unlock->description,
      s_achievements_locals.most_recent_unlock->badge_path, {}, 0.0f, s_achievements_locals.most_recent_unlock->points);

    // extra spacing if we have two
    text_pos.y += s_achievements_locals.achievement_nearest_completion ? (paragraph_spacing + paragraph_spacing) : 0.0f;
  }

  // don't duplicate nearest completion if it was also the most recent progress update
  if (s_achievements_locals.achievement_nearest_completion.has_value() &&
      (!s_achievements_locals.most_recent_progress_update ||
       s_achievements_locals.most_recent_progress_update->achievement_id !=
         s_achievements_locals.achievement_nearest_completion->achievement_id))
  {
    buffer.format(ICON_FA_FLAG_CHECKERED " {}", TRANSLATE_DISAMBIG_SV("Achievements", "Nearest Completion", "Pause Menu"));
    draw_achievement_in_box(buffer, s_achievements_locals.achievement_nearest_completion->title,
                            s_achievements_locals.achievement_nearest_completion->description,
                            s_achievements_locals.achievement_nearest_completion->badge_path,
                            s_achievements_locals.achievement_nearest_completion->measured_progress,
                            s_achievements_locals.achievement_nearest_completion->measured_percent, 0);
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
                              s_achievements_locals.most_recent_progress_update->measured_percent, 0);
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

    buffer.format(ICON_FA_HAND_FIST " {}",
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

  // Leaderboards
  if (!s_achievements_locals.active_leaderboards.empty())
  {
    box_height = box_padding + box_padding + UIStyle.MediumFontSize;

    const std::string_view icon_template = Achievements::GetLeaderboardFormatIcon(RC_CLIENT_LEADERBOARD_FORMAT_TIME);
    const float leaderboard_icon_size =
      UIStyle.Font
        ->CalcTextSizeA(UIStyle.LargeFontSize, UIStyle.NormalFontWeight, FLT_MAX, 0.0f, IMSTR_START_END(icon_template))
        .x;
    const float leaderboard_icon_reserve = leaderboard_icon_size + (text_spacing * 3.0f);
    const float avail_text_width = box_content_width - leaderboard_icon_reserve;

    for (size_t i = 0; i < s_achievements_locals.active_leaderboards.size(); i++)
    {
      const PauseMenuLeaderboardInfo& lbinfo = s_achievements_locals.active_leaderboards[i];
      box_height += paragraph_spacing;

      const ImVec2 tracker_size = lbinfo.tracker_value.empty() ?
                                    ImVec2() :
                                    UIStyle.Font->CalcTextSizeA(UIStyle.MediumSmallFontSize, UIStyle.BoldFontWeight,
                                                                FLT_MAX, 0.0f, IMSTR_START_END(lbinfo.tracker_value));
      const float avail_title_width =
        avail_text_width - ((tracker_size.x > 0.0f) ? (tracker_size.x + text_spacing) : 0.0f);
      const ImVec2 title_size = UIStyle.Font->CalcTextSizeA(UIStyle.MediumSmallFontSize, UIStyle.BoldFontWeight,
                                                            FLT_MAX, avail_title_width, IMSTR_START_END(lbinfo.title));
      box_height += title_size.y;

      if (!lbinfo.description.empty())
      {
        const ImVec2 description_size =
          UIStyle.Font->CalcTextSizeA(UIStyle.MediumSmallFontSize, UIStyle.NormalFontWeight, FLT_MAX, avail_text_width,
                                      IMSTR_START_END(lbinfo.description));
        box_height += text_spacing + description_size.y;
      }

      box_height += ((i == (s_achievements_locals.active_leaderboards.size() - 1)) ? 0.0f : paragraph_spacing);
    }

    box_min = ImVec2(box_min.x, box_max.y + box_margin);
    box_max = ImVec2(box_min.x + box_width, box_min.y + box_height);
    text_pos = ImVec2(box_min.x + box_padding, box_min.y + box_padding);

    dl->AddRectFilled(box_min, box_max, box_background_color, box_rounding);

    buffer.format(ICON_FA_STOPWATCH " {}",
                  TRANSLATE_DISAMBIG_SV("Achievements", "Active Leaderboard Attempts", "Pause Menu"));
    dl->AddText(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.BoldFontWeight, text_pos, box_title_text_color,
                IMSTR_START_END(buffer));
    text_pos.y += UIStyle.MediumFontSize;

    const ImVec4 clip_rect = ImVec4(text_pos.x, text_pos.y, text_pos.x + box_content_width, text_pos.y + box_height);

    for (const PauseMenuLeaderboardInfo& lbinfo : s_achievements_locals.active_leaderboards)
    {
      text_pos.y += paragraph_spacing;

      const std::string_view icon = Achievements::GetLeaderboardFormatIcon(lbinfo.format);
      dl->AddText(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.NormalFontWeight, text_pos, title_text_color,
                  IMSTR_START_END(icon), 0.0f, &clip_rect);

      const ImVec2 tracker_size = lbinfo.tracker_value.empty() ?
                                    ImVec2() :
                                    UIStyle.Font->CalcTextSizeA(UIStyle.MediumSmallFontSize, UIStyle.BoldFontWeight,
                                                                FLT_MAX, 0.0f, IMSTR_START_END(lbinfo.tracker_value));
      const float avail_title_width =
        avail_text_width - ((tracker_size.x > 0.0f) ? (tracker_size.x + text_spacing) : 0.0f);
      const ImVec2 title_size = UIStyle.Font->CalcTextSizeA(UIStyle.MediumSmallFontSize, UIStyle.BoldFontWeight,
                                                            FLT_MAX, avail_title_width, IMSTR_START_END(lbinfo.title));

      dl->AddText(UIStyle.Font, UIStyle.MediumSmallFontSize, UIStyle.BoldFontWeight,
                  ImVec2(text_pos.x + leaderboard_icon_reserve, text_pos.y), title_text_color,
                  IMSTR_START_END(lbinfo.title), avail_title_width, &clip_rect);

      if (!lbinfo.tracker_value.empty())
      {
        dl->AddText(UIStyle.Font, UIStyle.MediumSmallFontSize, UIStyle.BoldFontWeight,
                    ImVec2(box_max.x - box_padding - tracker_size.x, text_pos.y), title_text_color,
                    IMSTR_START_END(lbinfo.tracker_value), 0.0f, &clip_rect);
      }

      text_pos.y += title_size.y;

      if (!lbinfo.description.empty())
      {
        text_pos.y += text_spacing;

        const ImVec2 description_size =
          UIStyle.Font->CalcTextSizeA(UIStyle.MediumSmallFontSize, UIStyle.NormalFontWeight, FLT_MAX, avail_text_width,
                                      IMSTR_START_END(lbinfo.description));
        dl->AddText(UIStyle.Font, UIStyle.MediumSmallFontSize, UIStyle.NormalFontWeight,
                    ImVec2(text_pos.x + leaderboard_icon_reserve, text_pos.y), text_color,
                    IMSTR_START_END(lbinfo.description), avail_text_width, &clip_rect);

        text_pos.y += description_size.y;
      }

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
    Host::AddIconOSDMessage(OSDMessageType::Info, "AchievementsUnavailable", Achievements::RA_LOGO_ICON_NAME,
                            TRANSLATE_STR("Achievements", "Achievements are not available."),
                            Achievements::IsActive() ?
                              TRANSLATE_STR("Achievements", "This game has no achievements.") :
                              TRANSLATE_STR("Achievements", "Achievements are disabled in settings."));
    return;
  }

  GPUThread::RunOnThread([]() {
    Initialize();

    PauseForMenuOpen(false);
    ForceKeyNavEnabled();
    EnqueueSoundEffect(SFX_NAV_ACTIVATE);

    BeginTransition(SHORT_TRANSITION_TIME, &SwitchToAchievements);
  });
}

void FullscreenUI::AddSubsetInfo(const rc_client_subset_t* subset)
{
  const std::string_view game_title = Achievements::GetGameTitle();

  SubsetInfo info;
  info.subset_id = subset->id;

  // is this the core subset?
  std::string_view subset_title = subset->title;
  if (game_title == subset_title)
  {
    info.full_name = subset_title;
    info.short_name = TRANSLATE_STR("Achievements", "Core");
  }
  else if (subset_title.starts_with(game_title))
  {
    info.full_name = subset_title;
    info.short_name = StringUtil::StripWhitespace(subset_title.substr(game_title.size()));
    if (info.short_name.starts_with("-"))
      info.short_name = StringUtil::StripWhitespace(info.short_name.substr(1));
    if (info.short_name.empty())
      info.short_name = StringUtil::ToChars(subset->id);
  }
  else
  {
    info.full_name = fmt::format(TRANSLATE_FS("Achievements", "{0} - {1}"), game_title, subset_title);
    info.short_name = subset_title;
  }

  info.badge_path = Achievements::GetSubsetBadgePath(subset);
  info.num_leaderboards = subset->num_leaderboards;

  info.summary = {};
  rc_client_get_user_subset_summary(Achievements::GetClient(), subset->id, &info.summary);

  s_achievements_locals.subset_info_list.push_back(std::move(info));
}

void FullscreenUI::DrawSubsetSelector()
{
  DebugAssert(s_achievements_locals.open_subset);

  const float& font_size = UIStyle.MediumFontSize;
  constexpr float font_weight = UIStyle.BoldFontWeight;
  constexpr float nav_x_padding = 12.0f;
  constexpr float nav_y_padding = 8.0f;

  float nav_width = 0.0f;
  for (const SubsetInfo& subset : s_achievements_locals.subset_info_list)
    nav_width += CalcFloatingNavBarButtonWidth(subset.short_name, font_size, font_size, font_weight, nav_x_padding);

  BeginFloatingNavBar(30.0f, 10.0f, nav_width, font_size, 1.0f, 0.0f, nav_x_padding, nav_y_padding);

  std::optional<u32> new_subset_id;

  for (size_t i = 0; i < s_achievements_locals.subset_info_list.size(); i++)
  {
    const SubsetInfo& subset = s_achievements_locals.subset_info_list[i];
    GPUTexture* badge = GetCachedTextureAsync(subset.badge_path);

    if (FloatingNavBarIcon(subset.short_name, badge, (&subset == s_achievements_locals.open_subset), font_size,
                           font_size, font_weight))
    {
      new_subset_id = subset.subset_id;
    }
  }

  if (!new_subset_id.has_value())
  {
    const size_t i = s_achievements_locals.open_subset - &s_achievements_locals.subset_info_list[0];

    if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, true) ||
        ImGui::IsKeyPressed(ImGuiKey_NavGamepadTweakSlow, true) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true))
    {
      EnqueueSoundEffect(SFX_NAV_MOVE);
      new_subset_id = (i == 0) ? s_achievements_locals.subset_info_list.back().subset_id :
                                 s_achievements_locals.subset_info_list[i - 1].subset_id;
    }
    else if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, true) ||
             ImGui::IsKeyPressed(ImGuiKey_NavGamepadTweakFast, true) || ImGui::IsKeyPressed(ImGuiKey_RightArrow, true))
    {
      EnqueueSoundEffect(SFX_NAV_MOVE);
      new_subset_id = ((i + 1) == s_achievements_locals.subset_info_list.size()) ?
                        s_achievements_locals.subset_info_list.front().subset_id :
                        s_achievements_locals.subset_info_list[i + 1].subset_id;
    }
  }

  EndFloatingNavBar();

  if (new_subset_id.has_value())
  {
    BeginTransition(DEFAULT_TRANSITION_TIME,
                    [new_subset_id = new_subset_id.value()]() { SetCurrentSubsetID(new_subset_id); });
  }
}

bool FullscreenUI::IsCoreSubsetOpen()
{
  return (!s_achievements_locals.open_subset ||
          s_achievements_locals.open_subset == &s_achievements_locals.subset_info_list[0]);
}

void FullscreenUI::SetCurrentSubsetID(u32 subset_id)
{
  for (const SubsetInfo& info : s_achievements_locals.subset_info_list)
  {
    if (info.subset_id == subset_id)
    {
      s_achievements_locals.open_subset = &info;
      s_achievements_locals.last_open_subset_id = subset_id;
      QueueResetFocus(FocusResetType::ViewChanged);
      return;
    }
  }
}

template<typename T>
void FullscreenUI::CollectSubsetsFromList(const T* list, bool include_achievements, bool include_leaderboards)
{
  s_achievements_locals.open_subset = nullptr;
  s_achievements_locals.subset_info_list.clear();

  // Prefer rc_client grabbing subsets if possible. Old external clients won't support this.
  rc_client_subset_list_t* subset_list = rc_client_create_subset_list(Achievements::GetClient());
  if (subset_list && subset_list->num_subsets > 0)
  {
    // If there is only a single subset, we don't want to show a selector.
    if (subset_list->num_subsets > 1)
    {
      for (u32 i = 0; i < subset_list->num_subsets; i++)
      {
        const rc_client_subset_t* subset = subset_list->subsets[i];
        if ((include_achievements && subset->num_achievements > 0) ||
            (include_leaderboards && subset->num_leaderboards > 0))
        {
          AddSubsetInfo(subset);
        }
      }
    }
  }
  else if (std::any_of(list->buckets, list->buckets + list->num_buckets,
                       [&list](const auto& it) { return it.subset_id != list->buckets[0].subset_id; }))
  {
    for (u32 bucket_idx = 0; bucket_idx < list->num_buckets; bucket_idx++)
    {
      const auto& bucket = list->buckets[bucket_idx];
      if (std::ranges::none_of(s_achievements_locals.subset_info_list,
                               [&bucket](const SubsetInfo& info) { return info.subset_id == bucket.subset_id; }))
      {
        const rc_client_subset_t* subset = rc_client_get_subset_info(Achievements::GetClient(), bucket.subset_id);
        if (subset)
          AddSubsetInfo(subset);
      }
    }
  }

  if (subset_list)
    rc_client_destroy_subset_list(subset_list);

  // hopefully the first will be core...
  if (!s_achievements_locals.subset_info_list.empty())
  {
    const auto it = std::ranges::find_if(s_achievements_locals.subset_info_list, [](const SubsetInfo& info) {
      return info.subset_id == s_achievements_locals.last_open_subset_id;
    });
    if (it != s_achievements_locals.subset_info_list.end())
      s_achievements_locals.open_subset = &(*it);
    else
      s_achievements_locals.open_subset = &s_achievements_locals.subset_info_list[0];
  }
}

template<typename T>
bool FullscreenUI::IsBucketVisibleInCurrentSubset(const T& bucket)
{
  // Show e.g. active challenges/leaderboards in all subsets.
  if (bucket.subset_id == 0)
    return true;

  if (s_achievements_locals.open_subset && bucket.subset_id != s_achievements_locals.open_subset->subset_id)
    return false;

  return true;
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
  s_achievements_locals.achievement_list =
    rc_client_create_achievement_list(Achievements::GetClient(), RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE_AND_UNOFFICIAL,
                                      RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_SUBSET_BUCKETS);
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

  CollectSubsetsFromList(s_achievements_locals.achievement_list, true, false);
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

  const rc_client_user_game_summary_t& summary =
    s_achievements_locals.open_subset ? s_achievements_locals.open_subset->summary : Achievements::GetGameSummary();
  const bool is_core_subset = IsCoreSubsetOpen();
  const float heading_height_unscaled = ((summary.beaten_time > 0 || summary.completed_time) ? 122.0f : 102.0f) +
                                        ((summary.num_unsupported_achievements > 0) ? 20.0f : 0.0f);

  const ImVec4 background = ModAlpha(UIStyle.BackgroundColor, WINDOW_ALPHA);
  const ImVec4 heading_background = ModAlpha(UIStyle.BackgroundColor, WINDOW_HEADING_ALPHA);
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

    if (s_achievements_locals.open_subset)
      DrawSubsetSelector();

    close_window = (FloatingButton(ICON_FA_XMARK, 10.0f, 10.0f, 1.0f, 0.0f, true) || WantsToCloseMenu());

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
      if (IsCoreSubsetOpen())
      {
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
        if (summary.num_unlocked_achievements == summary.num_core_achievements)
        {
          text.append(TRANSLATE_PLURAL_SSTR("Achievements",
                                            "You have unlocked all achievements in this subset and earned %n points!",
                                            "Point count", summary.points_unlocked));
        }
        else
        {
          text.append_format(
            TRANSLATE_FS(
              "Achievements",
              "You have unlocked {0} of {1} achievements in this subset, earning {2} of {3} possible points."),
            summary.num_unlocked_achievements, summary.num_core_achievements, summary.points_unlocked,
            summary.points_core);
        }
      }
    }
    else
    {
      text.format(ICON_FA_BAN " {}", is_core_subset ? TRANSLATE_SV("Achievements", "This game has no achievements.") :
                                                      TRANSLATE_SV("Achievements", "This subset has no achievements."));
    }

    top += UIStyle.MediumFontSize + spacing;

    RenderShadowedTextClipped(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.NormalFontWeight, summary_bb.Min,
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
          if (is_core_subset)
          {
            text.append_format(TRANSLATE_FS("Achievements", "Game was beaten on {0}, and completed on {1}."),
                               beaten_time, completion_time);
          }
          else
          {
            text.append_format(TRANSLATE_FS("Achievements", "Subset was beaten on {0}, and completed on {1}."),
                               beaten_time, completion_time);
          }
        }
        else
        {
          if (is_core_subset)
            text.append_format(TRANSLATE_FS("Achievements", "Game was beaten on {0}."), beaten_time);
          else
            text.append_format(TRANSLATE_FS("Achievements", "Subset was beaten on {0}."), beaten_time);
        }
      }
      else
      {
        const std::string completion_time =
          Host::FormatNumber(Host::NumberFormatType::ShortDate, static_cast<s64>(summary.completed_time));
        if (is_core_subset)
          text.append_format(TRANSLATE_FS("Achievements", "Game was completed on {0}."), completion_time);
        else
          text.append_format(TRANSLATE_FS("Achievements", "Subset was completed on {0}."), completion_time);
      }

      const ImRect beaten_bb(ImVec2(left, top), ImVec2(right, top + UIStyle.MediumFontSize));
      RenderShadowedTextClipped(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.NormalFontWeight, beaten_bb.Min,
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
        ImGui::RenderRectFilledInRangeH(dl, progress_bb, ImGui::GetColorU32(UIStyle.SecondaryColor), progress_bb.Min.x,
                                        progress_bb.Min.x + (fraction * progress_bb.GetWidth()), progress_rounding);
      }

      text.format("{}%", static_cast<u32>(std::round(fraction * 100.0f)));
      text_size = UIStyle.Font->CalcTextSizeA(UIStyle.MediumFontSize, UIStyle.NormalFontWeight, FLT_MAX, 0.0f,
                                              IMSTR_START_END(text));
      const ImVec2 text_pos(progress_bb.Min.x + ((progress_bb.Max.x - progress_bb.Min.x) / 2.0f) - (text_size.x / 2.0f),
                            progress_bb.Min.y + ((progress_bb.Max.y - progress_bb.Min.y) / 2.0f) -
                              (text_size.y / 2.0f));
      dl->AddText(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.NormalFontWeight, text_pos,
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
      {ICON_FA_CIRCLE_QUESTION, TRANSLATE_NOOP("Achievements", "Unknown")},
      {ICON_FA_LOCK, TRANSLATE_NOOP("Achievements", "Locked")},
      {ICON_FA_UNLOCK, TRANSLATE_NOOP("Achievements", "Unlocked")},
      {ICON_FA_TRIANGLE_EXCLAMATION, TRANSLATE_NOOP("Achievements", "Unsupported")},
      {ICON_FA_FLASK_VIAL, TRANSLATE_NOOP("Achievements", "Unofficial")},
      {ICON_FA_LOCK_OPEN, TRANSLATE_NOOP("Achievements", "Recently Unlocked")},
      {ICON_FA_HAND_FIST, TRANSLATE_NOOP("Achievements", "Active Challenges")},
      {ICON_FA_FLAG_CHECKERED, TRANSLATE_NOOP("Achievements", "Almost There")},
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
        if (bucket.bucket_type != bucket_type || !IsBucketVisibleInCurrentSubset(bucket))
          continue;

        DebugAssert(bucket.bucket_type < NUM_RC_CLIENT_ACHIEVEMENT_BUCKETS);

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

  if (IsGamepadInputSource())
  {
    if (s_achievements_locals.open_subset)
    {
      SetFullscreenFooterText(
        std::array{std::make_pair(ICON_PF_XBOX_DPAD_LEFT_RIGHT, TRANSLATE_SV("Achievements", "Change Subset")),
                   std::make_pair(ICON_PF_XBOX_DPAD_UP_DOWN, TRANSLATE_SV("Achievements", "Change Selection")),
                   std::make_pair(ICON_PF_BUTTON_A, TRANSLATE_SV("Achievements", "View Details")),
                   std::make_pair(ICON_PF_BUTTON_B, TRANSLATE_SV("Achievements", "Back"))});
    }
    else
    {
      SetFullscreenFooterText(
        std::array{std::make_pair(ICON_PF_XBOX_DPAD_UP_DOWN, TRANSLATE_SV("Achievements", "Change Selection")),
                   std::make_pair(ICON_PF_BUTTON_A, TRANSLATE_SV("Achievements", "View Details")),
                   std::make_pair(ICON_PF_BUTTON_B, TRANSLATE_SV("Achievements", "Back"))});
    }
  }
  else
  {
    if (s_achievements_locals.open_subset)
    {
      SetFullscreenFooterText(std::array{
        std::make_pair(ICON_PF_ARROW_LEFT ICON_PF_ARROW_RIGHT, TRANSLATE_SV("Achievements", "Change Subset")),
        std::make_pair(ICON_PF_ARROW_UP ICON_PF_ARROW_DOWN, TRANSLATE_SV("Achievements", "Change Selection")),
        std::make_pair(ICON_PF_ENTER, TRANSLATE_SV("Achievements", "View Details")),
        std::make_pair(ICON_PF_ESC, TRANSLATE_SV("Achievements", "Back"))});
    }
    else
    {
      SetFullscreenFooterText(std::array{
        std::make_pair(ICON_PF_ARROW_UP ICON_PF_ARROW_DOWN, TRANSLATE_SV("Achievements", "Change Selection")),
        std::make_pair(ICON_PF_ENTER, TRANSLATE_SV("Achievements", "View Details")),
        std::make_pair(ICON_PF_ESC, TRANSLATE_SV("Achievements", "Back"))});
    }
  }

  if (close_window)
    ReturnToPreviousWindow();
}

void FullscreenUI::DrawAchievement(const rc_client_achievement_t* cheevo)
{
  static constexpr const float progress_height_unscaled = 20.0f;
  static constexpr const float progress_rounding_unscaled = 5.0f;

  static constexpr const float& title_font_size = UIStyle.LargeFontSize;
  static constexpr const float& title_font_weight = UIStyle.BoldFontWeight;
  static constexpr const float& subtitle_font_size = UIStyle.MediumFontSize;
  static constexpr const float& subtitle_font_weight = UIStyle.NormalFontWeight;
  static constexpr const float& type_badge_font_size = UIStyle.MediumSmallFontSize;
  static constexpr const float& type_badge_font_weight = UIStyle.BoldFontWeight;

  const std::string_view title(cheevo->title);
  const std::string_view description = cheevo->description ? std::string_view(cheevo->description) : std::string_view();
  const std::string_view measured_progress(cheevo->measured_progress);
  const bool is_unlocked = (cheevo->state == RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED);
  const bool is_measured = (!is_unlocked && !measured_progress.empty());

  ImVec2 type_badge_padding;
  ImVec2 type_badge_size;
  float type_badge_spacing = 0.0f;
  float type_badge_rounding = 0.0f;
  ImU32 type_badge_bg_color = 0;
  TinyString type_badge_text;
  switch (cheevo->type)
  {
    case RC_CLIENT_ACHIEVEMENT_TYPE_MISSABLE:
      type_badge_text.format(ICON_PF_ACHIEVEMENTS_MISSABLE " {}", TRANSLATE_SV("Achievements", "Missable"));
      type_badge_bg_color = IM_COL32(205, 45, 32, 255);
      break;

    case RC_CLIENT_ACHIEVEMENT_TYPE_PROGRESSION:
      type_badge_text.format(ICON_PF_ACHIEVEMENTS_PROGRESSION " {}", TRANSLATE_SV("Achievements", "Progression"));
      type_badge_bg_color = IM_COL32(13, 71, 161, 255);
      break;

    case RC_CLIENT_ACHIEVEMENT_TYPE_WIN:
      type_badge_text.format(ICON_PF_ACHIEVEMENTS_PROGRESSION " {}", TRANSLATE_SV("Achievements", "Win Condition"));
      type_badge_bg_color = IM_COL32(50, 110, 30, 255);
      break;
  }
  if (!type_badge_text.empty())
  {
    type_badge_padding = LayoutScale(5.0f, 3.0f);
    type_badge_spacing = LayoutScale(10.0f);
    type_badge_rounding = LayoutScale(3.0f);
    type_badge_size = UIStyle.Font->CalcTextSizeA(type_badge_font_size, type_badge_font_weight, FLT_MAX, 0.0f,
                                                  IMSTR_START_END(type_badge_text));
    type_badge_size += type_badge_padding * 2.0f;
  }

  const ImVec2 image_size = LayoutScale(50.0f, 50.0f);
  const float image_right_padding = LayoutScale(15.0f);
  const float avail_width = GetMenuButtonAvailableWidth();
  const float spacing = LayoutScale(4.0f);
  const ImVec2 right_side_size = UIStyle.Font->CalcTextSizeA(UIStyle.MediumFontSize, UIStyle.NormalFontWeight, FLT_MAX,
                                                             0.0f, TRANSLATE("Achievements", "XXX points"));
  const float max_text_width =
    avail_width - (image_size.x + image_right_padding + (spacing * 2.0f) + right_side_size.x);
  const float max_title_width =
    max_text_width - (type_badge_text.empty() ? 0.0f : type_badge_size.x + type_badge_spacing);
  const ImVec2 title_size = UIStyle.Font->CalcTextSizeA(UIStyle.LargeFontSize, UIStyle.BoldFontWeight, FLT_MAX,
                                                        max_title_width, IMSTR_START_END(title));
  const ImVec2 description_size = description.empty() ?
                                    ImVec2() :
                                    UIStyle.Font->CalcTextSizeA(UIStyle.MediumFontSize, UIStyle.NormalFontWeight,
                                                                FLT_MAX, max_text_width, IMSTR_START_END(description));
  const float content_height = (title_size.y + spacing + description_size.y + spacing + UIStyle.MediumFontSize) +
                               (is_measured ? (spacing + LayoutScale(progress_height_unscaled)) : 0.0f) +
                               LayoutScale(LAYOUT_MENU_ITEM_EXTRA_HEIGHT);

  SmallString text;
  text.format("chv_{}", cheevo->id);

  ImRect bb;
  bool visible, hovered;
  const bool clicked = MenuButtonFrame(text, content_height, true, &bb, &visible, &hovered);
  if (!visible)
    return;

  ImDrawList* const dl = ImGui::GetWindowDrawList();

  if (const std::string& badge_path = GetCachedAchievementBadgePath(cheevo, !is_unlocked); !badge_path.empty())
  {
    GPUTexture* badge = GetCachedTextureAsync(badge_path);
    if (badge)
    {
      const ImRect image_bb = CenterImage(ImRect(bb.Min, bb.Min + image_size), badge);
      dl->AddImage(badge, image_bb.Min, image_bb.Max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                   IM_COL32(255, 255, 255, 255));
    }
  }

  // make it easier to compute bounding boxes...
  ImVec2 current_pos = ImVec2(bb.Min.x + image_size.x + image_right_padding, bb.Min.y);

  // -- Title --
  const ImRect title_bb(current_pos, current_pos + title_size);
  const u32 text_color = ImGui::GetColorU32(UIStyle.SecondaryTextColor);
  RenderShadowedTextClipped(dl, UIStyle.Font, title_font_size, title_font_weight, title_bb.Min, title_bb.Max,
                            text_color, title, &title_size, ImVec2(0.0f, 0.0f), max_title_width, &title_bb);
  current_pos.y += title_size.y + spacing;

  // -- Type Badge --
  if (!type_badge_text.empty())
  {
    const ImVec2 type_badge_pos(title_bb.Min.x + title_size.x + type_badge_spacing,
                                ImFloor(title_bb.Min.y + (title_font_size - type_badge_size.y) * 0.5f));
    dl->AddRectFilled(type_badge_pos, type_badge_pos + type_badge_size, type_badge_bg_color, type_badge_rounding);

    const ImVec2 type_badge_text_pos = type_badge_pos + type_badge_padding;
    const ImVec4 type_badge_text_clip = ImVec4(type_badge_pos.x, type_badge_pos.y, type_badge_pos.x + type_badge_size.x,
                                               type_badge_pos.y + type_badge_size.y);
    dl->AddText(UIStyle.Font, type_badge_font_size, type_badge_font_weight, type_badge_text_pos,
                IM_COL32(255, 255, 255, 255), IMSTR_START_END(type_badge_text), 0.0f, &type_badge_text_clip);
  }

  // -- Description --
  const u32 description_color = ImGui::GetColorU32(DarkerColor(UIStyle.SecondaryTextColor));
  if (!description.empty())
  {
    const ImRect description_bb(current_pos, current_pos + description_size);
    RenderShadowedTextClipped(dl, UIStyle.Font, subtitle_font_size, subtitle_font_weight, description_bb.Min,
                              description_bb.Max, description_color, description, &description_size, ImVec2(0.0f, 0.0f),
                              max_text_width, &description_bb);
    current_pos.y += description_size.y + spacing;
  }

  // -- Rarity --
  // display hc if hc is active
  const float rarity_to_display =
    rc_client_get_hardcore_enabled(Achievements::GetClient()) ? cheevo->rarity_hardcore : cheevo->rarity;
  const ImRect rarity_bb(current_pos, ImVec2(current_pos.x + max_text_width, current_pos.y + UIStyle.MediumFontSize));
  const u32 rarity_color = ImGui::GetColorU32(DarkerColor(DarkerColor(UIStyle.SecondaryTextColor)));
  if (is_unlocked)
  {
    const std::string date =
      Host::FormatNumber(Host::NumberFormatType::LongDateTime, static_cast<s64>(cheevo->unlock_time));
    text.format(TRANSLATE_FS("Achievements", "Unlocked: {} | {:.1f}% of players have this achievement"), date,
                rarity_to_display);

    RenderShadowedTextClipped(dl, UIStyle.Font, subtitle_font_size, subtitle_font_weight, rarity_bb.Min, rarity_bb.Max,
                              rarity_color, text, nullptr, ImVec2(0.0f, 0.0f), 0.0f, &rarity_bb);
  }
  else
  {
    text.format(TRANSLATE_FS("Achievements", "{:.1f}% of players have this achievement"), rarity_to_display);
    RenderShadowedTextClipped(dl, UIStyle.Font, subtitle_font_size, subtitle_font_weight, rarity_bb.Min, rarity_bb.Max,
                              rarity_color, text, nullptr, ImVec2(0.0f, 0.0f), 0.0f, &rarity_bb);
  }
  current_pos.y += UIStyle.MediumFontSize + spacing;

  if (is_measured)
  {
    const float progress_rounding = LayoutScale(progress_rounding_unscaled);
    const ImRect progress_bb(
      current_pos, ImVec2(current_pos.x + max_text_width, current_pos.y + LayoutScale(progress_height_unscaled)));
    const float fraction = cheevo->measured_percent * 0.01f;
    dl->AddRectFilled(progress_bb.Min, progress_bb.Max, ImGui::GetColorU32(UIStyle.PrimaryDarkColor),
                      progress_rounding);
    ImGui::RenderRectFilledInRangeH(dl, progress_bb, ImGui::GetColorU32(UIStyle.SecondaryColor), progress_bb.Min.x,
                                    progress_bb.Min.x + (fraction * progress_bb.GetWidth()), progress_rounding);

    const ImVec2 text_size = UIStyle.Font->CalcTextSizeA(subtitle_font_size, subtitle_font_weight, FLT_MAX, 0.0f,
                                                         IMSTR_START_END(measured_progress));
    const ImVec2 text_pos =
      ImFloor(ImVec2(progress_bb.Min.x + ((progress_bb.Max.x - progress_bb.Min.x) / 2.0f) - (text_size.x / 2.0f),
                     progress_bb.Min.y + ((progress_bb.Max.y - progress_bb.Min.y) / 2.0f) - (text_size.y / 2.0f)));
    dl->AddText(UIStyle.Font, subtitle_font_size, subtitle_font_weight, text_pos,
                ImGui::GetColorU32(UIStyle.PrimaryTextColor), IMSTR_START_END(measured_progress));
  }

  // right side items
  current_pos = ImVec2(bb.Max.x - right_side_size.x, bb.Min.y);

  // -- Lock Icon and Points --
  const std::string_view lock_text = is_unlocked ? ICON_EMOJI_UNLOCKED : ICON_FA_LOCK;
  const ImVec2 lock_size =
    UIStyle.Font->CalcTextSizeA(title_font_size, 0.0f, FLT_MAX, 0.0f, IMSTR_START_END(lock_text));
  const ImRect lock_bb(current_pos, ImVec2(bb.Max.x, current_pos.y + lock_size.y));
  RenderShadowedTextClipped(dl, UIStyle.Font, title_font_size, 0.0f, lock_bb.Min, lock_bb.Max, text_color, lock_text,
                            &lock_size, ImVec2(0.5f, 0.0f), 0.0f, &lock_bb);
  current_pos.y += lock_size.y + spacing;

  text = TRANSLATE_PLURAL_SSTR("Achievements", "%n points", "Achievement points", cheevo->points);
  const ImVec2 points_size =
    UIStyle.Font->CalcTextSizeA(subtitle_font_size, subtitle_font_weight, FLT_MAX, 0.0f, IMSTR_START_END(text));
  const ImRect points_bb(current_pos, ImVec2(bb.Max.x, current_pos.y + points_size.y));
  RenderShadowedTextClipped(dl, UIStyle.Font, subtitle_font_size, subtitle_font_weight, points_bb.Min, points_bb.Max,
                            description_color, text, &points_size, ImVec2(0.5f, 0.0f), 0.0f, &points_bb);

  if (clicked)
  {
    const std::string url = fmt::format(fmt::runtime(ACHEIVEMENT_DETAILS_URL_TEMPLATE), cheevo->id);
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
    Host::AddIconOSDMessage(OSDMessageType::Info, "LeaderboardsUnavailable", Achievements::RA_LOGO_ICON_NAME,
                            TRANSLATE_STR("Achievements", "Leaderboards are not available."),
                            Achievements::IsActive() ?
                              TRANSLATE_STR("Achievements", "This game has no leaderboards.") :
                              TRANSLATE_STR("Achievements", "Achievements are disabled in settings."));
    return;
  }

  GPUThread::RunOnThread([]() {
    Initialize();

    PauseForMenuOpen(false);
    ForceKeyNavEnabled();
    EnqueueSoundEffect(SFX_NAV_ACTIVATE);

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

  CollectSubsetsFromList(s_achievements_locals.leaderboard_list, false, true);
  SwitchToMainWindow(MainWindowType::Leaderboards);
}

void FullscreenUI::DrawLeaderboardsWindow()
{
  static constexpr float heading_height_unscaled = 102.0f;

  const auto lock = Achievements::GetLock();
  if (!s_achievements_locals.leaderboard_list)
  {
    ReturnToPreviousWindow();
    return;
  }

  const bool is_leaderboard_open = (s_achievements_locals.open_leaderboard != nullptr);
  bool close_leaderboard_on_exit = false;

  SmallString text;

  const ImVec4 background = ModAlpha(UIStyle.BackgroundColor, WINDOW_ALPHA);
  const ImVec4 heading_background = ModAlpha(UIStyle.BackgroundColor, WINDOW_HEADING_ALPHA);
  const ImVec2 display_size = ImGui::GetIO().DisplaySize;
  const u32 text_color = ImGui::GetColorU32(ImGuiCol_Text);
  const float spacing = LayoutScale(10.0f);
  const float spacing_small = ImFloor(spacing * 0.5f);
  const float heading_height = LayoutScale(heading_height_unscaled);

  if (BeginFullscreenWindow(ImVec2(), ImVec2(display_size.x, heading_height), "leaderboards_heading",
                            heading_background, 0.0f, ImVec2(10.0f, 10.0f),
                            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration |
                              ImGuiWindowFlags_NoScrollWithMouse))
  {
    const ImVec2 heading_pos = ImGui::GetCursorScreenPos() + ImGui::GetStyle().FramePadding;
    const float image_size = LayoutScale(75.0f);

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

    const ImRect title_bb(ImVec2(left, top), ImVec2(right, top + UIStyle.LargeFontSize));
    if (s_achievements_locals.open_subset)
      text.assign(s_achievements_locals.open_subset->full_name);
    else
      text.assign(Achievements::GetGameTitle());

    top += UIStyle.LargeFontSize + spacing_small;

    RenderShadowedTextClipped(UIStyle.Font, UIStyle.LargeFontSize, UIStyle.BoldFontWeight, title_bb.Min, title_bb.Max,
                              text_color, text, nullptr, ImVec2(0.0f, 0.0f), 0.0f, &title_bb);

    u32 summary_color;
    if (is_leaderboard_open)
    {
      const ImRect subtitle_bb(ImVec2(left, top), ImVec2(right, top + UIStyle.LargeFontSize));
      text.assign(s_achievements_locals.open_leaderboard->title);

      top += UIStyle.MediumLargeFontSize + spacing_small;

      RenderShadowedTextClipped(UIStyle.Font, UIStyle.MediumLargeFontSize, UIStyle.BoldFontWeight, subtitle_bb.Min,
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
      {
        const rc_client_leaderboard_bucket_t& bucket = s_achievements_locals.leaderboard_list->buckets[i];
        if (IsBucketVisibleInCurrentSubset(bucket))
          count += bucket.num_leaderboards;
      }

      if (IsCoreSubsetOpen())
        text = TRANSLATE_PLURAL_SSTR("Achievements", "This game has %n leaderboards.", "Leaderboard count", count);
      else
        text = TRANSLATE_PLURAL_SSTR("Achievements", "This subset has %n leaderboards.", "Leaderboard count", count);

      summary_color = ImGui::GetColorU32(DarkerColor(ImGui::GetStyle().Colors[ImGuiCol_Text]));
    }

    const ImRect summary_bb(ImVec2(left, top), ImVec2(right, top + UIStyle.MediumFontSize));
    top += UIStyle.MediumFontSize + spacing_small;

    RenderShadowedTextClipped(UIStyle.Font, UIStyle.MediumFontSize, UIStyle.NormalFontWeight, summary_bb.Min,
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

    if (!is_leaderboard_open)
    {
      if (s_achievements_locals.open_subset)
        DrawSubsetSelector();

      if (FloatingButton(ICON_FA_XMARK, 10.0f, 10.0f, 1.0f, 0.0f, true) || WantsToCloseMenu())
        ReturnToPreviousWindow();
    }
    else
    {
      const auto show_nearby_title = FullscreenUI::IconStackString(ICON_EMOJI_CLIPBOARD, FSUI_NSTR("Show Nearby"));
      const auto show_all_title = FullscreenUI::IconStackString(ICON_EMOJI_NOTEBOOK, FSUI_NSTR("Show All"));

      const float& nav_font_size = UIStyle.MediumFontSize;
      constexpr float nav_font_weight = UIStyle.BoldFontWeight;
      constexpr float nav_x_padding = 12.0f;
      constexpr float nav_y_padding = 8.0f;
      const float nav_width =
        CalcFloatingNavBarButtonWidth(show_nearby_title, 0.0f, nav_font_size, nav_font_weight, nav_x_padding) +
        CalcFloatingNavBarButtonWidth(show_all_title, 0.0f, nav_font_size, nav_font_weight, nav_x_padding);

      const ImVec2 saved_cursor_pos = ImGui::GetCursorPos();
      BeginFloatingNavBar(30.0f, 10.0f, nav_width, nav_font_size, 1.0f, 0.0f, nav_x_padding, nav_y_padding);

      const bool view_toggled =
        (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, false) ||
         ImGui::IsKeyPressed(ImGuiKey_NavGamepadTweakSlow, false) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) ||
         ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, false) ||
         ImGui::IsKeyPressed(ImGuiKey_NavGamepadTweakFast, false) || ImGui::IsKeyPressed(ImGuiKey_RightArrow, false));
      bool new_view = view_toggled ? !s_achievements_locals.is_showing_all_leaderboard_entries :
                                     s_achievements_locals.is_showing_all_leaderboard_entries;
      for (const bool show_all : {false, true})
      {
        if (FloatingNavBarIcon(show_all ? show_all_title.view() : show_nearby_title.view(), nullptr,
                               s_achievements_locals.is_showing_all_leaderboard_entries == show_all, 0.0f,
                               nav_font_size, nav_font_weight))
        {
          new_view = show_all;
        }
      }
      if (s_achievements_locals.is_showing_all_leaderboard_entries != new_view)
      {
        BeginTransition(DEFAULT_TRANSITION_TIME, [new_view]() {
          s_achievements_locals.is_showing_all_leaderboard_entries = new_view;
          QueueResetFocus(FocusResetType::ViewChanged);
        });
      }

      EndFloatingNavBar();
      ImGui::SetCursorPos(saved_cursor_pos);

      if (FloatingButton(ICON_FA_XMARK, 10.0f, 10.0f, 1.0f, 0.0f, true) || WantsToCloseMenu())
        close_leaderboard_on_exit = true;
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
        if (!IsBucketVisibleInCurrentSubset(bucket))
          continue;

        for (u32 i = 0; i < bucket.num_leaderboards; i++)
          DrawLeaderboardListEntry(bucket.leaderboards[i]);
      }

      EndMenuButtons();
    }
    EndFullscreenWindow();

    if (IsGamepadInputSource())
    {
      if (s_achievements_locals.open_subset)
      {
        SetFullscreenFooterText(
          std::array{std::make_pair(ICON_PF_XBOX_DPAD_LEFT_RIGHT, TRANSLATE_SV("Achievements", "Change Subset")),
                     std::make_pair(ICON_PF_XBOX_DPAD_UP_DOWN, TRANSLATE_SV("Achievements", "Change Selection")),
                     std::make_pair(ICON_PF_BUTTON_A, TRANSLATE_SV("Achievements", "Open Leaderboard")),
                     std::make_pair(ICON_PF_BUTTON_B, TRANSLATE_SV("Achievements", "Back"))});
      }
      else
      {
        SetFullscreenFooterText(
          std::array{std::make_pair(ICON_PF_XBOX_DPAD_UP_DOWN, TRANSLATE_SV("Achievements", "Change Selection")),
                     std::make_pair(ICON_PF_BUTTON_A, TRANSLATE_SV("Achievements", "Open Leaderboard")),
                     std::make_pair(ICON_PF_BUTTON_B, TRANSLATE_SV("Achievements", "Back"))});
      }
    }
    else
    {
      if (s_achievements_locals.open_subset)
      {
        SetFullscreenFooterText(std::array{
          std::make_pair(ICON_PF_ARROW_LEFT ICON_PF_ARROW_RIGHT, TRANSLATE_SV("Achievements", "Change Subset")),
          std::make_pair(ICON_PF_ARROW_UP ICON_PF_ARROW_DOWN, TRANSLATE_SV("Achievements", "Change Selection")),
          std::make_pair(ICON_PF_ENTER, TRANSLATE_SV("Achievements", "Open Leaderboard")),
          std::make_pair(ICON_PF_ESC, TRANSLATE_SV("Achievements", "Back"))});
      }
      else
      {
        SetFullscreenFooterText(std::array{
          std::make_pair(ICON_PF_ARROW_UP ICON_PF_ARROW_DOWN, TRANSLATE_SV("Achievements", "Change Selection")),
          std::make_pair(ICON_PF_ENTER, TRANSLATE_SV("Achievements", "Open Leaderboard")),
          std::make_pair(ICON_PF_ESC, TRANSLATE_SV("Achievements", "Back"))});
      }
    }
  }
  else
  {
    if (BeginFullscreenWindow(
          ImVec2(0.0f, heading_height),
          ImVec2(display_size.x, display_size.y - heading_height - LayoutScale(LAYOUT_FOOTER_HEIGHT)), "leaderboard",
          background, 0.0f, ImVec2(LAYOUT_MENU_WINDOW_X_PADDING, LAYOUT_MENU_WINDOW_Y_PADDING), 0))
    {
      const ImVec2 heading_start_pos = ImGui::GetCursorScreenPos();
      ImVec2 column_heading_pos = heading_start_pos;
      float end_x = column_heading_pos.x + ImGui::GetContentRegionAvail().x;

      // and the padding for the frame itself
      column_heading_pos.x += LayoutScale(LAYOUT_MENU_BUTTON_X_PADDING);
      end_x -= LayoutScale(LAYOUT_MENU_BUTTON_X_PADDING);

      const float rank_column_width = UIStyle.Font
                                        ->CalcTextSizeA(UIStyle.LargeFontSize, UIStyle.BoldFontWeight,
                                                        std::numeric_limits<float>::max(), -1.0f, "99999")
                                        .x;
      const float name_column_width =
        UIStyle.Font
          ->CalcTextSizeA(UIStyle.LargeFontSize, UIStyle.BoldFontWeight, std::numeric_limits<float>::max(), -1.0f,
                          "WWWWWWWWWWWWWWWWWWWWWW")
          .x;
      const float time_column_width = UIStyle.Font
                                        ->CalcTextSizeA(UIStyle.LargeFontSize, UIStyle.BoldFontWeight,
                                                        std::numeric_limits<float>::max(), -1.0f, "WWWWWWWWWWW")
                                        .x;
      const float column_spacing = spacing * 2.0f;
      const u32 heading_color = ImGui::GetColorU32(DarkerColor(ImGui::GetStyle().Colors[ImGuiCol_Text]));

      const float midpoint = column_heading_pos.y + UIStyle.MediumLargeFontSize + LayoutScale(4.0f);
      float text_start_x = column_heading_pos.x;

      const ImRect rank_bb(ImVec2(text_start_x, column_heading_pos.y), ImVec2(end_x, midpoint));
      RenderShadowedTextClipped(UIStyle.Font, UIStyle.MediumLargeFontSize, UIStyle.BoldFontWeight, rank_bb.Min,
                                rank_bb.Max, heading_color, TRANSLATE_SV("Achievements", "Rank"), nullptr,
                                ImVec2(0.0f, 0.0f), 0.0f, &rank_bb);
      text_start_x += rank_column_width + column_spacing;

      const ImRect user_bb(ImVec2(text_start_x, column_heading_pos.y), ImVec2(end_x, midpoint));
      RenderShadowedTextClipped(UIStyle.Font, UIStyle.MediumLargeFontSize, UIStyle.BoldFontWeight, user_bb.Min,
                                user_bb.Max, heading_color, TRANSLATE_SV("Achievements", "Name"), nullptr,
                                ImVec2(0.0f, 0.0f), 0.0f, &user_bb);
      text_start_x += name_column_width + column_spacing;

      static const char* value_headings[NUM_RC_CLIENT_LEADERBOARD_FORMATS] = {
        TRANSLATE_NOOP("Achievements", "Time"),
        TRANSLATE_NOOP("Achievements", "Score"),
        TRANSLATE_NOOP("Achievements", "Value"),
      };

      const ImRect score_bb(ImVec2(text_start_x, column_heading_pos.y), ImVec2(end_x, midpoint));
      RenderShadowedTextClipped(
        UIStyle.Font, UIStyle.MediumLargeFontSize, UIStyle.BoldFontWeight, score_bb.Min, score_bb.Max, heading_color,
        Host::TranslateToStringView("Achievements",
                                    value_headings[std::min<u8>(s_achievements_locals.open_leaderboard->format,
                                                                NUM_RC_CLIENT_LEADERBOARD_FORMATS - 1)]),
        nullptr, ImVec2(0.0f, 0.0f), 0.0f, &score_bb);
      text_start_x += time_column_width + column_spacing;

      const ImRect date_bb(ImVec2(text_start_x, column_heading_pos.y), ImVec2(end_x, midpoint));
      RenderShadowedTextClipped(UIStyle.Font, UIStyle.MediumLargeFontSize, UIStyle.BoldFontWeight, date_bb.Min,
                                date_bb.Max, heading_color, TRANSLATE_SV("Achievements", "Date Submitted"), nullptr,
                                ImVec2(0.0f, 0.0f), 0.0f, &date_bb);

      const float line_thickness = LayoutScale(1.0f);
      const float line_padding = LayoutScale(5.0f);
      const ImVec2 line_start(column_heading_pos.x, column_heading_pos.y + UIStyle.MediumLargeFontSize + line_padding);
      const ImVec2 line_end(end_x, line_start.y);
      ImGui::GetWindowDrawList()->AddLine(line_start, line_end, ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                          line_thickness);

      // keep imgui happy
      ImGui::Dummy(ImVec2(end_x - heading_start_pos.x, spacing + UIStyle.MediumLargeFontSize));

      BeginMenuButtons(0, 0.0f, LAYOUT_MENU_BUTTON_X_PADDING, 8.0f, 0.0f, 4.0f);
      ResetFocusHere();

      // for drawing time popups
      const std::time_t current_time = std::time(nullptr);
      std::optional<std::tm> current_tm = Common::LocalTime(current_time);
      if (!current_tm.has_value())
        current_tm = std::tm{};
      ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
      ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);

      u32 total_count = 0;
      u32 last_visible_count = 0;
      if (!s_achievements_locals.is_showing_all_leaderboard_entries)
      {
        if (s_achievements_locals.leaderboard_nearby_entries)
        {
          for (u32 i = 0; i < s_achievements_locals.leaderboard_nearby_entries->num_entries; i++)
          {
            total_count++;

            if (DrawLeaderboardEntry(s_achievements_locals.leaderboard_nearby_entries->entries[i], i,
                                     static_cast<s32>(i) ==
                                       s_achievements_locals.leaderboard_nearby_entries->user_index,
                                     rank_column_width, name_column_width, time_column_width, column_spacing,
                                     current_time, current_tm.value()))
            {
              last_visible_count = total_count;
            }
          }
        }
        else
        {
          DrawLeaderboardLoadingIndicator(heading_height, display_size.y - heading_height, false);
        }
      }
      else
      {
        for (const rc_client_leaderboard_entry_list_t* list : s_achievements_locals.leaderboard_entry_lists)
        {
          for (u32 i = 0; i < list->num_entries; i++)
          {
            total_count++;

            if (DrawLeaderboardEntry(list->entries[i], i, static_cast<s32>(i) == list->user_index, rank_column_width,
                                     name_column_width, time_column_width, column_spacing, current_time,
                                     current_tm.value()))
            {
              last_visible_count = total_count;
            }
          }
        }

        if (!s_achievements_locals.has_fetched_all_leaderboard_entries)
        {
          // if showing the last few, fetch the next batch
          if ((total_count - last_visible_count) <= 3 && !s_achievements_locals.leaderboard_fetch_handle)
            FetchNextLeaderboardEntries();

          // show the loading indicator in the bottom-right
          if (s_achievements_locals.leaderboard_fetch_handle)
          {
            const ImRect& win_rc = ImGui::GetCurrentWindow()->InnerRect;
            DrawLeaderboardLoadingIndicator(win_rc.Min.y, win_rc.GetHeight(), true);
          }
        }
      }

      ImGui::PopStyleVar(2);

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

bool FullscreenUI::DrawLeaderboardEntry(const rc_client_leaderboard_entry_t& entry, u32 index, bool is_self,
                                        float rank_column_width, float name_column_width, float time_column_width,
                                        float column_spacing, std::time_t current_time, const std::tm& current_tm)
{
  ImRect bb;
  bool visible, hovered;
  bool pressed = MenuButtonFrame(entry.user, UIStyle.MediumLargeFontSize, true, &bb, &visible, &hovered);
  if (!visible)
    return false;

  const float midpoint = bb.Min.y + UIStyle.MediumLargeFontSize + LayoutScale(4.0f);
  float text_start_x = bb.Min.x;
  SmallString text;

  text.format("{}", entry.rank);

  const u32 text_color =
    is_self ? IM_COL32(255, 242, 0, 255) :
              ImGui::GetColorU32(((index % 2) == 0) ? DarkerColor(ImGui::GetStyle().Colors[ImGuiCol_Text]) :
                                                      ImGui::GetStyle().Colors[ImGuiCol_Text]);

  const ImRect rank_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
  const float font_weight = is_self ? UIStyle.BoldFontWeight : UIStyle.NormalFontWeight;
  RenderShadowedTextClipped(UIStyle.Font, UIStyle.MediumLargeFontSize, font_weight, rank_bb.Min, rank_bb.Max,
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

  const float icon_spacing = LayoutScale(10.0f);
  const ImRect user_bb(ImVec2(text_start_x + icon_spacing + icon_size, bb.Min.y), ImVec2(bb.Max.x, midpoint));
  RenderShadowedTextClipped(UIStyle.Font, UIStyle.MediumLargeFontSize, font_weight, user_bb.Min, user_bb.Max,
                            text_color, entry.user, nullptr, ImVec2(0.0f, 0.0f), 0.0f, &user_bb);
  text_start_x += name_column_width + column_spacing;

  const ImRect score_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
  RenderShadowedTextClipped(UIStyle.Font, UIStyle.MediumLargeFontSize, font_weight, score_bb.Min, score_bb.Max,
                            text_color, entry.display, nullptr, ImVec2(0.0f, 0.0f), 0.0f, &score_bb);
  text_start_x += time_column_width + column_spacing;

  const ImRect time_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));

  const SmallString relative_time =
    FormatRelativeTimestamp(static_cast<std::time_t>(entry.submitted), current_time, current_tm);
  RenderShadowedTextClipped(UIStyle.Font, UIStyle.MediumLargeFontSize, font_weight, time_bb.Min, time_bb.Max,
                            text_color, relative_time, nullptr, ImVec2(0.0f, 0.0f), 0.0f, &time_bb);

  if (time_bb.Contains(ImGui::GetIO().MousePos) && ImGui::BeginItemTooltip())
  {
    const std::string submit_time =
      Host::FormatNumber(Host::NumberFormatType::LongDateTime, static_cast<s64>(entry.submitted));
    ImGui::PushFont(UIStyle.Font, UIStyle.MediumLargeFontSize, UIStyle.NormalFontWeight);
    ImGui::Text(ICON_EMOJI_CLOCK_FIVE_OCLOCK " %s", submit_time.c_str());
    ImGui::PopFont();
    ImGui::EndTooltip();
  }

  if (pressed)
  {
    const std::string url = fmt::format(fmt::runtime(PROFILE_DETAILS_URL_TEMPLATE), entry.user);
    INFO_LOG("Opening profile details: {}", url);
    Host::OpenURL(url);
  }

  return true;
}

void FullscreenUI::DrawLeaderboardLoadingIndicator(float pos_y, float avail_height, bool short_text)
{
  static constexpr const float& font_weight = UIStyle.BoldFontWeight;
  const float font_size = short_text ? UIStyle.MediumFontSize : UIStyle.MediumLargeFontSize;
  const u32 color = ImGui::GetColorU32(DarkerColor(ImGui::GetStyle().Colors[ImGuiCol_Text], 0.9f));
  const std::string_view text = short_text ?
                                  TRANSLATE_SV("Achievements", "Loading...") :
                                  TRANSLATE_SV("Achievements", "Downloading leaderboard data, please wait...");
  const ImVec2 text_size = UIStyle.Font->CalcTextSizeA(font_size, font_weight, FLT_MAX, 0.0f, IMSTR_START_END(text));
  const float spinner_size = font_size;
  const float spinner_spacing = short_text ? LayoutScale(12.0f) : LayoutScale(16.0f);
  const float total_width = spinner_size + spinner_spacing + text_size.x;
  const float display_width = ImGui::GetIO().DisplaySize.x;

  // position in right side of screen if short text, center otherwise
  const ImVec2 pos = short_text ?
                       ImVec2((display_width - total_width) - LayoutScale(25.0f),
                              pos_y + avail_height - font_size - LayoutScale(10.0f)) :
                       ImVec2((display_width - total_width) * 0.5f, pos_y + (avail_height - font_size) * 0.5f);

  // for short text, draw a background box
  if (short_text)
  {
    const ImVec2 padding = ImVec2(LayoutScale(10.0f), LayoutScale(6.0f));
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(pos.x - padding.x, pos.y - padding.y),
                                              ImVec2(pos.x + total_width + padding.x, pos.y + font_size + padding.y),
                                              ImGui::GetColorU32(ModAlpha(UIStyle.PopupBackgroundColor, 0.8f)),
                                              LayoutScale(LAYOUT_MENU_ITEM_BORDER_ROUNDING));
  }

  DrawSpinner(ImGui::GetWindowDrawList(), pos, color, spinner_size, LayoutScale(3.0f));
  const ImVec2 text_pos = ImVec2(pos.x + spinner_size + spinner_spacing, pos.y);
  RenderShadowedTextClipped(UIStyle.Font, font_size, UIStyle.BoldFontWeight, text_pos, text_pos + text_size, color,
                            text, &text_size);
}

SmallString FullscreenUI::FormatRelativeTimestamp(time_t timestamp, time_t current_time, const std::tm& current_tm)
{
  const s64 diff = static_cast<s64>(current_time) - static_cast<s64>(timestamp);

  constexpr s64 MINUTE = 60;
  constexpr s64 HOUR = 60 * MINUTE;
  constexpr s64 DAY = 24 * HOUR;
  constexpr s64 WEEK = 7 * DAY;

  if (diff < MINUTE)
    return SmallString(TRANSLATE_SV("Achievements", "Just now"));

  if (diff < HOUR)
  {
    const int minutes = static_cast<int>(diff / MINUTE);
    return TRANSLATE_PLURAL_SSTR("Achievements", "%n minutes ago", "Relative time", minutes);
  }

  if (diff < DAY)
  {
    const int hours = static_cast<int>(diff / HOUR);
    return TRANSLATE_PLURAL_SSTR("Achievements", "%n hours ago", "Relative time", hours);
  }

  if (diff < DAY * 2)
  {
    // Check if it's actually today vs yesterday
    const std::optional<std::tm> timestamp_tm = Common::LocalTime(timestamp);
    if (timestamp_tm.has_value() && timestamp_tm->tm_yday == current_tm.tm_yday &&
        timestamp_tm->tm_year == current_tm.tm_year)
    {
      return SmallString(TRANSLATE_SV("Achievements", "Today"));
    }

    return SmallString(TRANSLATE_SV("Achievements", "Yesterday"));
  }

  if (diff < WEEK)
  {
    const int days = static_cast<int>(diff / DAY);
    return TRANSLATE_PLURAL_SSTR("Achievements", "%n days ago", "Relative time", days);
  }

  const std::optional<std::tm> timestamp_tm = Common::LocalTime(timestamp);
  if (!timestamp_tm.has_value())
    return SmallString();

  const int year_diff = current_tm.tm_year - timestamp_tm->tm_year;
  const int month_diff = current_tm.tm_mon - timestamp_tm->tm_mon;
  const int total_months = year_diff * 12 + month_diff;

  if (total_months == 0)
  {
    // Less than a month - use weeks
    const int weeks = static_cast<int>(diff / WEEK);
    return TRANSLATE_PLURAL_SSTR("Achievements", "%n weeks ago", "Relative time", weeks);
  }

  if (total_months < 12)
    return TRANSLATE_PLURAL_SSTR("Achievements", "%n months ago", "Relative time", total_months);

  // For years, adjust if we haven't reached the anniversary yet
  int years = year_diff;
  if (current_tm.tm_mon < timestamp_tm->tm_mon ||
      (current_tm.tm_mon == timestamp_tm->tm_mon && current_tm.tm_mday < timestamp_tm->tm_mday))
  {
    years--;
  }

  // Edge case: less than a full year but more than 11 months
  if (years < 1)
    return TRANSLATE_PLURAL_SSTR("Achievements", "%n months ago", "Relative time", total_months);

  return TRANSLATE_PLURAL_SSTR("Achievements", "%n years ago", "Relative time", years);
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
  s_achievements_locals.has_fetched_all_leaderboard_entries = false;
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
    ShowToast(OSDMessageType::Error, TRANSLATE_STR("Achievements", "Leaderboard download failed"), error_message);
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
    ShowToast(OSDMessageType::Error, TRANSLATE_STR("Achievements", "Leaderboard download failed"), error_message);
    CloseLeaderboard();
    return;
  }

  if (s_achievements_locals.leaderboard_entry_lists.empty())
    QueueResetFocus(FocusResetType::Other);

  s_achievements_locals.leaderboard_entry_lists.push_back(list);

  // at the end if we don't have the request size full of entries
  s_achievements_locals.has_fetched_all_leaderboard_entries |= (list->num_entries < LEADERBOARD_ALL_FETCH_SIZE);
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
  {
    rc_client_destroy_leaderboard_entry_list(*iter);
  }
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
  s_achievements_locals.has_fetched_all_leaderboard_entries = false;
  s_achievements_locals.is_showing_all_leaderboard_entries = false;
}

#endif // __ANDROID__
