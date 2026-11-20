#include "rc_client_do_frame.h"

#include "rc_client_async.h"
#include "rc_client_game.h"
#include "rc_client_load.h"
#include "rc_client_internal.h" /* rc_client */
#include "rc_client_server.h"

#include "rc_api_request.h"
#include "rc_api_runtime.h"
#include "rc_consoles.h" /* rc_memory_regions_t */

#include "../runtime/rc_eval_state.h"
#include "../runtime/rc_format.h"
#include "../runtime/rc_lboard.h"
#include "../runtime/rc_modified_memref.h"
#include "../runtime/rc_richpresence.h"
#include "../runtime/rc_trigger.h"
#include "../runtime/rc_typed_value.h"

#include "../util/rc_compat.h"

#include <string.h> /* memset */

static void rc_client_reschedule_callback(rc_client_t* client, rc_client_scheduled_callback_data_t* callback, rc_clock_t when);

/* internals pulled from rc_runtime.c */
int rc_trigger_contains_memref(const struct rc_trigger_t* trigger, const struct rc_memref_t* memref);
int rc_value_contains_memref(const struct rc_value_t* value, const struct rc_memref_t* memref);
/* end rc_runtime.c internals */

/* ===== Achievements ===== */

typedef struct rc_client_award_achievement_callback_data_t
{
  uint32_t id;
  uint32_t retry_count;
  uint8_t hardcore;
  const char* game_hash;
  rc_clock_t unlock_time;
  rc_client_t* client;
  rc_client_scheduled_callback_data_t* scheduled_callback_data;
} rc_client_award_achievement_callback_data_t;

static void rc_client_award_achievement_server_call(rc_client_award_achievement_callback_data_t* ach_data);

static void rc_client_award_achievement_retry(rc_client_scheduled_callback_data_t* callback_data, rc_client_t* client, rc_clock_t now)
{
  rc_client_award_achievement_callback_data_t* ach_data =
    (rc_client_award_achievement_callback_data_t*)callback_data->data;

  (void)client;
  (void)now;

  rc_client_award_achievement_server_call(ach_data);
}

static void rc_client_award_achievement_check_mastery(rc_client_t* client, uint32_t achievement_id)
{
  rc_mutex_lock(&client->state.mutex);
  if (client->game) {
    rc_client_subset_info_t* subset;
    for (subset = client->game->subsets; subset; subset = subset->next) {
      if (subset->mastery == RC_CLIENT_MASTERY_STATE_NONE) {
        rc_client_achievement_info_t* scan = subset->achievements;
        rc_client_achievement_info_t* stop = scan + subset->public_.num_achievements;
        int found = 0;
        for (; scan < stop; ++scan) {
          if (scan->public_.id == achievement_id) {
            found = 1;
            break;
          }
        }

        if (found) {
          RC_CLIENT_LOG_INFO_FORMATTED(client, "%s %u %s",
            (subset->public_.id == client->game->public_.id) ? "Game" : "Subset",
            client->game->public_.id,
            client->state.hardcore ? "mastered" : "completed");

          subset->mastery = RC_CLIENT_MASTERY_STATE_PENDING;
          break;
        }
      }
    }
  }
  rc_mutex_unlock(&client->state.mutex);
}

static void rc_client_award_achievement_callback(const rc_api_server_response_t* server_response, void* callback_data)
{
  rc_client_award_achievement_callback_data_t* ach_data =
    (rc_client_award_achievement_callback_data_t*)callback_data;
  rc_api_award_achievement_response_t award_achievement_response;

  int result = rc_api_process_award_achievement_server_response(&award_achievement_response, server_response);
  const char* error_message = rc_client_server_error_message(&result, server_response->http_status_code, &award_achievement_response.response);

  if (error_message) {
    if (award_achievement_response.response.error_message && !rc_client_should_retry(server_response)) {
      /* actual error from server */
      RC_CLIENT_LOG_ERR_FORMATTED(ach_data->client, "Error awarding achievement %u: %s", ach_data->id, error_message);
      rc_client_raise_server_error_event(ach_data->client, "award_achievement", ach_data->id, result, award_achievement_response.response.error_message);
    }
    else if (ach_data->retry_count++ == 0) {
      /* first retry is immediate */
      RC_CLIENT_LOG_ERR_FORMATTED(ach_data->client, "Error awarding achievement %u: %s, retrying immediately", ach_data->id, error_message);
      rc_client_award_achievement_server_call(ach_data);
      return;
    }
    else {
      /* double wait time between each attempt until we hit a maximum delay of two minutes */
      /* 1s -> 2s -> 4s -> 8s -> 16s -> 32s -> 64s -> 120s -> 120s -> 120s ...*/
      const uint32_t delay = (ach_data->retry_count > 8) ? 120 : (1 << (ach_data->retry_count - 2));
      RC_CLIENT_LOG_ERR_FORMATTED(ach_data->client, "Error awarding achievement %u: %s, retrying in %u seconds", ach_data->id, error_message, delay);

      if (!ach_data->scheduled_callback_data) {
        ach_data->scheduled_callback_data = (rc_client_scheduled_callback_data_t*)calloc(1, sizeof(*ach_data->scheduled_callback_data));
        if (!ach_data->scheduled_callback_data) {
          RC_CLIENT_LOG_ERR_FORMATTED(ach_data->client, "Failed to allocate scheduled callback data for reattempt to unlock achievement %u", ach_data->id);
          rc_client_raise_server_error_event(ach_data->client, "award_achievement", ach_data->id, RC_OUT_OF_MEMORY, rc_error_str(RC_OUT_OF_MEMORY));
          return;
        }
        ach_data->scheduled_callback_data->callback = rc_client_award_achievement_retry;
        ach_data->scheduled_callback_data->data = ach_data;
        ach_data->scheduled_callback_data->related_id = ach_data->id;
      }

      ach_data->scheduled_callback_data->when =
        ach_data->client->callbacks.get_time_millisecs(ach_data->client) + delay * 1000;

      rc_client_schedule_callback(ach_data->client, ach_data->scheduled_callback_data);

      rc_client_update_disconnect_state(ach_data->client);
      return;
    }
  }
  else {
    ach_data->client->user.score = award_achievement_response.new_player_score;
    ach_data->client->user.score_casual = award_achievement_response.new_player_score_casual;

    if (award_achievement_response.awarded_achievement_id != ach_data->id) {
      RC_CLIENT_LOG_ERR_FORMATTED(ach_data->client, "Awarded achievement %u instead of %u", award_achievement_response.awarded_achievement_id, error_message);
    }
    else {
      if (award_achievement_response.response.error_message) {
        /* previously unlocked achievements are returned as a success with an error message */
        RC_CLIENT_LOG_INFO_FORMATTED(ach_data->client, "Achievement %u: %s", ach_data->id, award_achievement_response.response.error_message);
      }
      else if (ach_data->retry_count) {
        RC_CLIENT_LOG_INFO_FORMATTED(ach_data->client, "Achievement %u awarded after %u attempts, new score: %u",
          ach_data->id, ach_data->retry_count + 1,
          ach_data->hardcore ? award_achievement_response.new_player_score : award_achievement_response.new_player_score_casual);
      }
      else {
        RC_CLIENT_LOG_INFO_FORMATTED(ach_data->client, "Achievement %u awarded, new score: %u",
          ach_data->id,
          ach_data->hardcore ? award_achievement_response.new_player_score : award_achievement_response.new_player_score_casual);
      }

      if (award_achievement_response.achievements_remaining == 0)
        rc_client_award_achievement_check_mastery(ach_data->client, ach_data->id);
    }
  }

  if (ach_data->retry_count)
    rc_client_update_disconnect_state(ach_data->client);

  if (ach_data->scheduled_callback_data)
    free(ach_data->scheduled_callback_data);
  free(ach_data);
}

static void rc_client_award_achievement_server_call(rc_client_award_achievement_callback_data_t* ach_data)
{
  rc_api_award_achievement_request_t api_params;
  rc_api_request_t request;
  int result;

  memset(&api_params, 0, sizeof(api_params));
  api_params.username = ach_data->client->user.username;
  api_params.api_token = ach_data->client->user.token;
  api_params.achievement_id = ach_data->id;
  api_params.hardcore = ach_data->hardcore;
  api_params.game_hash = ach_data->game_hash;

  if (ach_data->retry_count) {
    const rc_clock_t now = ach_data->client->callbacks.get_time_millisecs(ach_data->client);
    api_params.seconds_since_unlock = (uint32_t)((now - ach_data->unlock_time) / 1000);
  }

  result = rc_api_init_award_achievement_request_hosted(&request, &api_params, &ach_data->client->state.host);
  if (result != RC_OK) {
    RC_CLIENT_LOG_ERR_FORMATTED(ach_data->client, "Error constructing unlock request for achievement %u: %s", ach_data->id, rc_error_str(result));
    free(ach_data);
    return;
  }

  ach_data->client->callbacks.server_call(&request, rc_client_award_achievement_callback, ach_data, ach_data->client);

  rc_api_destroy_request(&request);
}

static void rc_client_award_achievement(rc_client_t* client, rc_client_achievement_info_t* achievement)
{
  rc_client_award_achievement_callback_data_t* callback_data;

  rc_mutex_lock(&client->state.mutex);

  if (client->state.hardcore) {
    achievement->public_.unlock_time = achievement->unlock_time_hardcore = time(NULL);
    if (achievement->unlock_time_casual == 0)
      achievement->unlock_time_casual = achievement->unlock_time_hardcore;

    /* adjust score now - will get accurate score back from server */
    client->user.score += achievement->public_.points;
  }
  else {
    achievement->public_.unlock_time = achievement->unlock_time_casual = time(NULL);

    /* adjust score now - will get accurate score back from server */
    client->user.score_casual += achievement->public_.points;
  }

  achievement->public_.state = RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED;
  achievement->public_.unlocked |= (client->state.hardcore) ?
    RC_CLIENT_ACHIEVEMENT_UNLOCKED_BOTH : RC_CLIENT_ACHIEVEMENT_UNLOCKED_CASUAL;

  rc_mutex_unlock(&client->state.mutex);

  if (achievement->public_.id >= RC_CLIENT_ACHIEVEMENT_WARNING_ID) {
    RC_CLIENT_LOG_INFO_FORMATTED(client, "Unlocked warning achievement %u: %s", achievement->public_.id, achievement->public_.title);
    return;
  }

  if (client->callbacks.can_submit_achievement_unlock &&
    !client->callbacks.can_submit_achievement_unlock(achievement->public_.id, client)) {
    RC_CLIENT_LOG_INFO_FORMATTED(client, "Achievement %u unlock blocked by client", achievement->public_.id);
    return;
  }

  /* can't unlock unpromoted achievements on the server */
  if (achievement->public_.category != RC_CLIENT_ACHIEVEMENT_CATEGORY_PROMOTED) {
    RC_CLIENT_LOG_INFO_FORMATTED(client, "Unlocked unpromoted achievement %u: %s", achievement->public_.id, achievement->public_.title);
    return;
  }

  /* don't actually unlock achievements when spectating */
  if (client->state.spectator_mode != RC_CLIENT_SPECTATOR_MODE_OFF) {
    RC_CLIENT_LOG_INFO_FORMATTED(client, "Spectated achievement %u: %s", achievement->public_.id, achievement->public_.title);
    return;
  }

  callback_data = (rc_client_award_achievement_callback_data_t*)calloc(1, sizeof(*callback_data));
  if (!callback_data) {
    RC_CLIENT_LOG_ERR_FORMATTED(client, "Failed to allocate callback data for unlocking achievement %u", achievement->public_.id);
    rc_client_raise_server_error_event(client, "award_achievement", achievement->public_.id, RC_OUT_OF_MEMORY, rc_error_str(RC_OUT_OF_MEMORY));
    return;
  }
  callback_data->client = client;
  callback_data->id = achievement->public_.id;
  callback_data->hardcore = client->state.hardcore;
  callback_data->unlock_time = client->callbacks.get_time_millisecs(client);

  if (client->game) /* may be NULL if this gets called while unloading the game (from another thread - events are raised outside the lock) */
    callback_data->game_hash = client->game->public_.hash;

  RC_CLIENT_LOG_INFO_FORMATTED(client, "Awarding achievement %u: %s", achievement->public_.id, achievement->public_.title);
  rc_client_award_achievement_server_call(callback_data);
}

int rc_client_is_award_achievement_pending(const rc_client_t* client, uint32_t achievement_id)
{
  /* assume lock already held */
  rc_client_scheduled_callback_data_t* scheduled_callback = client->state.scheduled_callbacks;
  for (; scheduled_callback; scheduled_callback = scheduled_callback->next)
  {
    if (scheduled_callback->callback == rc_client_award_achievement_retry)
    {
      rc_client_award_achievement_callback_data_t* ach_data =
        (rc_client_award_achievement_callback_data_t*)scheduled_callback->data;
      if (ach_data->id == achievement_id)
        return 1;
    }
  }

  return 0;
}

int rc_client_get_award_achievement_pending_count(rc_client_t* client)
{
  rc_client_scheduled_callback_data_t* scheduled_callback;
  int count = 0;

  if (!client)
    return 0;

  rc_mutex_lock(&client->state.mutex);

  scheduled_callback = client->state.scheduled_callbacks;
  for (; scheduled_callback; scheduled_callback = scheduled_callback->next) {
    if (scheduled_callback->callback == rc_client_award_achievement_retry)
      count++;
  }

  rc_mutex_unlock(&client->state.mutex);

  return count;
}

static void rc_client_subset_reset_achievements(rc_client_subset_info_t* subset)
{
  rc_client_achievement_info_t* achievement = subset->achievements;
  rc_client_achievement_info_t* stop = achievement + subset->public_.num_achievements;

  for (; achievement < stop; ++achievement) {
    rc_trigger_t* trigger = achievement->trigger;
    if (!trigger || achievement->public_.state != RC_CLIENT_ACHIEVEMENT_STATE_ACTIVE)
      continue;

    if (trigger->state == RC_TRIGGER_STATE_PRIMED) {
      achievement->pending_events |= RC_CLIENT_ACHIEVEMENT_PENDING_EVENT_CHALLENGE_INDICATOR_HIDE;
      subset->pending_events |= RC_CLIENT_SUBSET_PENDING_EVENT_ACHIEVEMENT;
    }

    rc_reset_trigger(trigger);
  }
}

static void rc_client_reset_achievements(rc_client_t* client)
{
  rc_client_subset_info_t* subset;
  for (subset = client->game->subsets; subset; subset = subset->next)
    rc_client_subset_reset_achievements(subset);
}

int rc_client_set_unpromoted_achievement_unlock_state(rc_client_t* client, uint32_t id,
    uint8_t unlock_state, time_t unlock_time_casual, time_t unlock_time_hardcore)
{
  rc_client_subset_info_t* subset;
  rc_client_achievement_info_t* achievement = NULL;
  uint8_t active_bit;

  if (!client)
    return RC_INVALID_STATE;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client) {
    RC_CLIENT_LOG_ERR_FORMATTED(client, "Cannot set unpromoted achievement %u unlock state with external client", id);
    return RC_INVALID_STATE;
  }
#endif

  if (!client->game)
    return RC_NO_GAME_LOADED;

  if (unlock_state & ~RC_CLIENT_ACHIEVEMENT_UNLOCKED_BOTH)
    return RC_INVALID_VALUE;

  for (subset = client->game->subsets; subset; subset = subset->next) {
    rc_client_achievement_info_t* scan = subset->achievements;
    rc_client_achievement_info_t* stop = scan + subset->public_.num_achievements;
    for (; scan < stop; ++scan) {
      if (scan->public_.id == id) {
        achievement = scan;
        break;
      }
    }

    if (achievement)
      break;
  }

  if (!achievement || achievement->public_.category != RC_CLIENT_ACHIEVEMENT_CATEGORY_UNPROMOTED)
    return RC_NOT_FOUND;

  rc_mutex_lock(&client->state.mutex);

  if ((unlock_state & RC_CLIENT_ACHIEVEMENT_UNLOCKED_CASUAL) && !unlock_time_casual)
    unlock_time_casual = time(NULL);
  else if (!(unlock_state & RC_CLIENT_ACHIEVEMENT_UNLOCKED_CASUAL))
    unlock_time_casual = 0;

  if ((unlock_state & RC_CLIENT_ACHIEVEMENT_UNLOCKED_HARDCORE) && !unlock_time_hardcore)
    unlock_time_hardcore = time(NULL);
  else if (!(unlock_state & RC_CLIENT_ACHIEVEMENT_UNLOCKED_HARDCORE))
    unlock_time_hardcore = 0;

  achievement->public_.unlocked = unlock_state;
  achievement->unlock_time_casual = unlock_time_casual;
  achievement->unlock_time_hardcore = unlock_time_hardcore;
  achievement->public_.unlock_time = client->state.hardcore ? unlock_time_hardcore : unlock_time_casual;

  active_bit = client->state.encore_mode ? RC_CLIENT_ACHIEVEMENT_UNLOCKED_NONE :
    client->state.hardcore ? RC_CLIENT_ACHIEVEMENT_UNLOCKED_HARDCORE : RC_CLIENT_ACHIEVEMENT_UNLOCKED_CASUAL;

  if (achievement->public_.state != RC_CLIENT_ACHIEVEMENT_STATE_DISABLED) {
    if (unlock_state & active_bit) {
      achievement->public_.state = RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED;
      if (achievement->trigger)
        achievement->trigger->state = RC_TRIGGER_STATE_TRIGGERED;
    }
    else {
      if (achievement->trigger)
        rc_reset_trigger(achievement->trigger);
      achievement->public_.state = RC_CLIENT_ACHIEVEMENT_STATE_ACTIVE;
    }
  }

  rc_client_update_active_achievements(client->game);
  rc_mutex_unlock(&client->state.mutex);
  return RC_OK;
}

/* ===== Leaderboards ===== */

void rc_client_allocate_leaderboard_tracker(rc_client_game_info_t* game, rc_client_leaderboard_info_t* leaderboard)
{
  rc_client_leaderboard_tracker_info_t* tracker;
  rc_client_leaderboard_tracker_info_t* available_tracker = NULL;

  for (tracker = game->leaderboard_trackers; tracker; tracker = tracker->next) {
    if (tracker->reference_count == 0) {
      if (available_tracker == NULL && tracker->pending_events == RC_CLIENT_LEADERBOARD_TRACKER_PENDING_EVENT_NONE)
        available_tracker = tracker;

      continue;
    }

    if (tracker->value_djb2 != leaderboard->value_djb2 || tracker->format != leaderboard->format)
      continue;

    if (tracker->raw_value != leaderboard->value) {
      /* if the value comes from tracking hits, we can't assume the trackers started in the
       * same frame, so we can't share the tracker */
      if (tracker->value_from_hits)
        continue;

      /* value has changed. prepare an update event */
      tracker->raw_value = leaderboard->value;
      tracker->pending_events |= RC_CLIENT_LEADERBOARD_TRACKER_PENDING_EVENT_UPDATE;
      game->pending_events |= RC_CLIENT_GAME_PENDING_EVENT_LEADERBOARD_TRACKER;
    }

    /* attach to the existing tracker */
    ++tracker->reference_count;
    tracker->pending_events &= ~RC_CLIENT_LEADERBOARD_TRACKER_PENDING_EVENT_HIDE;
    leaderboard->tracker = tracker;
    leaderboard->public_.tracker_value = tracker->public_.display;
    return;
  }

  if (!available_tracker) {
    rc_client_leaderboard_tracker_info_t** next = &game->leaderboard_trackers;

    available_tracker = (rc_client_leaderboard_tracker_info_t*)rc_buffer_alloc(&game->buffer, sizeof(*available_tracker));
    memset(available_tracker, 0, sizeof(*available_tracker));
    available_tracker->public_.id = 1;

    for (tracker = *next; tracker; next = &tracker->next, tracker = *next)
      available_tracker->public_.id++;

    *next = available_tracker;
  }

  /* update the claimed tracker */
  available_tracker->reference_count = 1;
  available_tracker->value_djb2 = leaderboard->value_djb2;
  available_tracker->format = leaderboard->format;
  available_tracker->raw_value = leaderboard->value;
  available_tracker->pending_events = RC_CLIENT_LEADERBOARD_TRACKER_PENDING_EVENT_SHOW;
  available_tracker->value_from_hits = (uint8_t)rc_value_from_hits(&leaderboard->lboard->value);
  leaderboard->tracker = available_tracker;
  leaderboard->public_.tracker_value = available_tracker->public_.display;
  game->pending_events |= RC_CLIENT_GAME_PENDING_EVENT_LEADERBOARD_TRACKER;
}

void rc_client_release_leaderboard_tracker(rc_client_game_info_t* game, rc_client_leaderboard_info_t* leaderboard)
{
  rc_client_leaderboard_tracker_info_t* tracker = leaderboard->tracker;
  leaderboard->tracker = NULL;

  if (tracker && --tracker->reference_count == 0) {
    tracker->pending_events |= RC_CLIENT_LEADERBOARD_TRACKER_PENDING_EVENT_HIDE;
    game->pending_events |= RC_CLIENT_GAME_PENDING_EVENT_LEADERBOARD_TRACKER;
  }
}

void rc_client_update_leaderboard_tracker(rc_client_game_info_t* game, rc_client_leaderboard_info_t* leaderboard)
{
  rc_client_leaderboard_tracker_info_t* tracker = leaderboard->tracker;
  if (tracker && tracker->raw_value != leaderboard->value) {
    tracker->raw_value = leaderboard->value;
    tracker->pending_events |= RC_CLIENT_LEADERBOARD_TRACKER_PENDING_EVENT_UPDATE;
    game->pending_events |= RC_CLIENT_GAME_PENDING_EVENT_LEADERBOARD_TRACKER;
  }
}

typedef struct rc_client_submit_leaderboard_entry_callback_data_t
{
  uint32_t id;
  int32_t score;
  uint32_t retry_count;
  const char* game_hash;
  rc_clock_t submit_time;
  rc_client_t* client;
  rc_client_scheduled_callback_data_t* scheduled_callback_data;
} rc_client_submit_leaderboard_entry_callback_data_t;

static void rc_client_submit_leaderboard_entry_server_call(rc_client_submit_leaderboard_entry_callback_data_t* lboard_data);

static void rc_client_submit_leaderboard_entry_retry(rc_client_scheduled_callback_data_t* callback_data, rc_client_t* client, rc_clock_t now)
{
  rc_client_submit_leaderboard_entry_callback_data_t* lboard_data =
    (rc_client_submit_leaderboard_entry_callback_data_t*)callback_data->data;

  (void)client;
  (void)now;

  rc_client_submit_leaderboard_entry_server_call(lboard_data);
}

static void rc_client_raise_scoreboard_event(rc_client_submit_leaderboard_entry_callback_data_t* lboard_data,
  const rc_api_submit_lboard_entry_response_t* response)
{
  rc_client_leaderboard_scoreboard_t sboard;
  rc_client_event_t client_event;
  rc_client_subset_info_t* subset;
  rc_client_t* client = lboard_data->client;
  const rc_client_leaderboard_info_t* leaderboard = NULL;

  if (!client || !client->game)
    return;

  for (subset = client->game->subsets; subset; subset = subset->next) {
    leaderboard = rc_client_subset_get_leaderboard_info(subset, lboard_data->id);
    if (leaderboard != NULL)
      break;
  }
  if (leaderboard == NULL) {
    RC_CLIENT_LOG_ERR_FORMATTED(client, "Trying to raise scoreboard for unknown leaderboard %u", lboard_data->id);
    return;
  }

  memset(&sboard, 0, sizeof(sboard));
  sboard.leaderboard_id = lboard_data->id;
  rc_format_value(sboard.submitted_score, sizeof(sboard.submitted_score), response->submitted_score, leaderboard->format);
  rc_format_value(sboard.best_score, sizeof(sboard.best_score), response->best_score, leaderboard->format);
  sboard.new_rank = response->new_rank;
  sboard.num_entries = response->num_entries;
  sboard.num_top_entries = response->num_top_entries;
  if (sboard.num_top_entries > 0) {
    sboard.top_entries = (rc_client_leaderboard_scoreboard_entry_t*)calloc(
      response->num_top_entries, sizeof(rc_client_leaderboard_scoreboard_entry_t));
    if (sboard.top_entries != NULL) {
      uint32_t i;
      for (i = 0; i < response->num_top_entries; i++) {
        sboard.top_entries[i].username = response->top_entries[i].username;
        sboard.top_entries[i].rank = response->top_entries[i].rank;
        rc_format_value(sboard.top_entries[i].score, sizeof(sboard.top_entries[i].score), response->top_entries[i].score,
          leaderboard->format);
      }
    }
  }

  memset(&client_event, 0, sizeof(client_event));
  client_event.type = RC_CLIENT_EVENT_LEADERBOARD_SCOREBOARD;
  client_event.leaderboard = &leaderboard->public_;
  client_event.leaderboard_scoreboard = &sboard;

  lboard_data->client->callbacks.event_handler(&client_event, lboard_data->client);

  if (sboard.top_entries != NULL) {
    free(sboard.top_entries);
  }
}

static void rc_client_submit_leaderboard_entry_callback(const rc_api_server_response_t* server_response, void* callback_data)
{
  rc_client_submit_leaderboard_entry_callback_data_t* lboard_data =
    (rc_client_submit_leaderboard_entry_callback_data_t*)callback_data;
  rc_api_submit_lboard_entry_response_t submit_lboard_entry_response;

  int result = rc_api_process_submit_lboard_entry_server_response(&submit_lboard_entry_response, server_response);
  const char* error_message = rc_client_server_error_message(&result, server_response->http_status_code, &submit_lboard_entry_response.response);

  if (error_message) {
    if (submit_lboard_entry_response.response.error_message && !rc_client_should_retry(server_response)) {
      /* actual error from server */
      RC_CLIENT_LOG_ERR_FORMATTED(lboard_data->client, "Error submitting leaderboard entry %u: %s", lboard_data->id, error_message);
      rc_client_raise_server_error_event(lboard_data->client, "submit_lboard_entry", lboard_data->id, result, submit_lboard_entry_response.response.error_message);
    }
    else if (lboard_data->retry_count++ == 0) {
      /* first retry is immediate */
      RC_CLIENT_LOG_ERR_FORMATTED(lboard_data->client, "Error submitting leaderboard entry %u: %s, retrying immediately", lboard_data->id, error_message);
      rc_client_submit_leaderboard_entry_server_call(lboard_data);
      return;
    }
    else {
      /* double wait time between each attempt until we hit a maximum delay of two minutes */
      /* 1s -> 2s -> 4s -> 8s -> 16s -> 32s -> 64s -> 120s -> 120s -> 120s ...*/
      const uint32_t delay = (lboard_data->retry_count > 8) ? 120 : (1 << (lboard_data->retry_count - 2));
      RC_CLIENT_LOG_ERR_FORMATTED(lboard_data->client, "Error submitting leaderboard entry %u: %s, retrying in %u seconds", lboard_data->id, error_message, delay);

      if (!lboard_data->scheduled_callback_data) {
        lboard_data->scheduled_callback_data = (rc_client_scheduled_callback_data_t*)calloc(1, sizeof(*lboard_data->scheduled_callback_data));
        if (!lboard_data->scheduled_callback_data) {
          RC_CLIENT_LOG_ERR_FORMATTED(lboard_data->client, "Failed to allocate scheduled callback data for reattempt to submit entry for leaderboard %u", lboard_data->id);
          rc_client_raise_server_error_event(lboard_data->client, "submit_lboard_entry", lboard_data->id, RC_OUT_OF_MEMORY, rc_error_str(RC_OUT_OF_MEMORY));
          return;
        }
        lboard_data->scheduled_callback_data->callback = rc_client_submit_leaderboard_entry_retry;
        lboard_data->scheduled_callback_data->data = lboard_data;
        lboard_data->scheduled_callback_data->related_id = lboard_data->id;
      }

      lboard_data->scheduled_callback_data->when =
        lboard_data->client->callbacks.get_time_millisecs(lboard_data->client) + delay * 1000;

      rc_client_schedule_callback(lboard_data->client, lboard_data->scheduled_callback_data);

      rc_client_update_disconnect_state(lboard_data->client);
      return;
    }
  }
  else {
    /* raise event for scoreboard */
    if (lboard_data->retry_count < 2) {
      rc_client_raise_scoreboard_event(lboard_data, &submit_lboard_entry_response);
    }

    /* not currently doing anything with the response */
    if (lboard_data->retry_count) {
      RC_CLIENT_LOG_INFO_FORMATTED(lboard_data->client, "Leaderboard %u submission %d completed after %u attempts",
        lboard_data->id, lboard_data->score, lboard_data->retry_count);
    }
  }

  if (lboard_data->retry_count)
    rc_client_update_disconnect_state(lboard_data->client);

  if (lboard_data->scheduled_callback_data)
    free(lboard_data->scheduled_callback_data);
  free(lboard_data);
}

static void rc_client_submit_leaderboard_entry_server_call(rc_client_submit_leaderboard_entry_callback_data_t* lboard_data)
{
  rc_api_submit_lboard_entry_request_t api_params;
  rc_api_request_t request;
  int result;

  memset(&api_params, 0, sizeof(api_params));
  api_params.username = lboard_data->client->user.username;
  api_params.api_token = lboard_data->client->user.token;
  api_params.leaderboard_id = lboard_data->id;
  api_params.score = lboard_data->score;
  api_params.game_hash = lboard_data->game_hash;

  if (lboard_data->retry_count) {
    const rc_clock_t now = lboard_data->client->callbacks.get_time_millisecs(lboard_data->client);
    api_params.seconds_since_completion = (uint32_t)((now - lboard_data->submit_time) / 1000);
  }

  result = rc_api_init_submit_lboard_entry_request_hosted(&request, &api_params, &lboard_data->client->state.host);
  if (result != RC_OK) {
    RC_CLIENT_LOG_ERR_FORMATTED(lboard_data->client, "Error constructing submit leaderboard entry for leaderboard %u: %s", lboard_data->id, rc_error_str(result));
    free(lboard_data);
    return;
  }

  lboard_data->client->callbacks.server_call(&request, rc_client_submit_leaderboard_entry_callback, lboard_data, lboard_data->client);

  rc_api_destroy_request(&request);
}

static void rc_client_submit_leaderboard_entry(rc_client_t* client, rc_client_leaderboard_info_t* leaderboard)
{
  rc_client_submit_leaderboard_entry_callback_data_t* callback_data;

  if (!client->state.hardcore) {
    RC_CLIENT_LOG_INFO_FORMATTED(client, "Leaderboard %u entry submission not allowed in casual mode", leaderboard->public_.id);
    return;
  }

  if (client->callbacks.can_submit_leaderboard_entry &&
    !client->callbacks.can_submit_leaderboard_entry(leaderboard->public_.id, client)) {
    RC_CLIENT_LOG_INFO_FORMATTED(client, "Leaderboard %u entry submission blocked by client", leaderboard->public_.id);
    return;
  }

  /* don't actually submit leaderboard entries when spectating */
  if (client->state.spectator_mode != RC_CLIENT_SPECTATOR_MODE_OFF) {
    RC_CLIENT_LOG_INFO_FORMATTED(client, "Spectated %s (%d) for leaderboard %u: %s",
      leaderboard->public_.tracker_value, leaderboard->value, leaderboard->public_.id, leaderboard->public_.title);
    return;
  }

  callback_data = (rc_client_submit_leaderboard_entry_callback_data_t*)calloc(1, sizeof(*callback_data));
  if (!callback_data) {
    RC_CLIENT_LOG_ERR_FORMATTED(client, "Failed to allocate callback data for submitting entry for leaderboard %u", leaderboard->public_.id);
    rc_client_raise_server_error_event(client, "submit_lboard_entry", leaderboard->public_.id, RC_OUT_OF_MEMORY, rc_error_str(RC_OUT_OF_MEMORY));
    return;
  }
  callback_data->client = client;
  callback_data->id = leaderboard->public_.id;
  callback_data->score = leaderboard->value;
  callback_data->game_hash = client->game->public_.hash;
  callback_data->submit_time = client->callbacks.get_time_millisecs(client);

  RC_CLIENT_LOG_INFO_FORMATTED(client, "Submitting %s (%d) for leaderboard %u: %s",
    leaderboard->public_.tracker_value, leaderboard->value, leaderboard->public_.id, leaderboard->public_.title);
  rc_client_submit_leaderboard_entry_server_call(callback_data);
}

static void rc_client_subset_reset_leaderboards(rc_client_game_info_t* game, rc_client_subset_info_t* subset)
{
  rc_client_leaderboard_info_t* leaderboard = subset->leaderboards;
  rc_client_leaderboard_info_t* stop = leaderboard + subset->public_.num_leaderboards;

  for (; leaderboard < stop; ++leaderboard) {
    rc_lboard_t* lboard = leaderboard->lboard;
    if (!lboard)
      continue;

    switch (leaderboard->public_.state) {
      case RC_CLIENT_LEADERBOARD_STATE_INACTIVE:
      case RC_CLIENT_LEADERBOARD_STATE_DISABLED:
        continue;

      case RC_CLIENT_LEADERBOARD_STATE_TRACKING:
        rc_client_release_leaderboard_tracker(game, leaderboard);
        /* fallthrough */ /* to default */
      default:
        leaderboard->public_.state = RC_CLIENT_LEADERBOARD_STATE_ACTIVE;
        rc_reset_lboard(lboard);
        break;
    }
  }
}

static void rc_client_reset_leaderboards(rc_client_t* client)
{
  rc_client_subset_info_t* subset;
  for (subset = client->game->subsets; subset; subset = subset->next)
    rc_client_subset_reset_leaderboards(client->game, subset);
}

/* ===== Rich Presence ===== */

static uint32_t rc_client_read_modified_memory_helper(uint32_t address, uint8_t* buffer, uint32_t num_bytes, void* ud)
{
  rc_client_t* client = (rc_client_t*)ud;

  /* if the address is out of range, don't bother processing it. */
  if (address > client->game->max_valid_address)
    return 0;

  return client->callbacks.read_memory(address, buffer, num_bytes, client);
}

static void rc_client_ping_callback(const rc_api_server_response_t* server_response, void* callback_data)
{
  rc_client_t* client = (rc_client_t*)callback_data;
  rc_api_ping_response_t response;

  int result = rc_api_process_ping_server_response(&response, server_response);
  const char* error_message = rc_client_server_error_message(&result, server_response->http_status_code, &response.response);
  if (error_message) {
    RC_CLIENT_LOG_WARN_FORMATTED(client, "Ping response error: %s", error_message);
  }

  rc_api_destroy_ping_response(&response);
}

static void rc_client_ping(rc_client_scheduled_callback_data_t* callback_data, rc_client_t* client, rc_clock_t now)
{
  rc_api_ping_request_t api_params;
  rc_api_request_t request;
  char buffer[256];
  int result;

  /* if no frames have been processed since the last ping, the emulator is idle. let the
   * server session expire. it will be resumed/restarted once frames start getting
   * processed again. */
  if (client->state.frames_processed != client->state.frames_at_last_ping) {
    client->state.frames_at_last_ping = client->state.frames_processed;

    memset(&api_params, 0, sizeof(api_params));
    api_params.username = client->user.username;
    api_params.api_token = client->user.token;
    api_params.game_id = client->game->public_.id;
    api_params.rich_presence = buffer;
    api_params.game_hash = client->game->public_.hash;
    api_params.hardcore = client->state.hardcore;

    if (!client->callbacks.rich_presence_override ||
      !client->callbacks.rich_presence_override(client, buffer, sizeof(buffer))) {
      rc_mutex_lock(&client->state.mutex);

      rc_runtime_get_richpresence(&client->game->runtime, buffer, sizeof(buffer),
        rc_client_read_modified_memory_helper, client, NULL);

      rc_mutex_unlock(&client->state.mutex);
    }

    /* there's a miniscule chance the game will be changed out while we're waiting for the lock.
     * if that happens, discard this ping. the new game will have scheduled its own ping.
     * don't reschedule this one. */
    if (!client->game || client->game->public_.id != api_params.game_id) {
      return;
    }

    result = rc_api_init_ping_request_hosted(&request, &api_params, &client->state.host);
    if (result != RC_OK) {
      RC_CLIENT_LOG_WARN_FORMATTED(client, "Error generating ping request: %s", rc_error_str(result));
    }
    else {
      client->callbacks.server_call(&request, rc_client_ping_callback, client, client);
    }
  }

  callback_data->when = now + 120 * 1000;
  rc_client_schedule_callback(client, callback_data);
}

void rc_client_schedule_ping(rc_client_t* client, rc_client_game_info_t* game)
{
  rc_client_scheduled_callback_data_t* callback_data = (rc_client_scheduled_callback_data_t*)
      rc_buffer_alloc(&game->buffer, sizeof(rc_client_scheduled_callback_data_t));

  memset(callback_data, 0, sizeof(*callback_data));
  callback_data->callback = rc_client_ping;
  callback_data->related_id = game->public_.id;
  callback_data->when = client->callbacks.get_time_millisecs(client) + 30 * 1000;
  rc_client_schedule_callback(client, callback_data);
}

void rc_client_unschedule_ping(rc_client_t* client, rc_client_game_info_t* game)
{
  rc_client_scheduled_callback_data_t** last = &client->state.scheduled_callbacks;
  rc_client_scheduled_callback_data_t* next;

  do {
    next = *last;
    if (!next)
      break;

    /* remove rich presence ping scheduled event for game */
    if (next->callback == rc_client_ping && next->related_id == game->public_.id) {
      *last = next->next;
      continue;
    }

    last = &next->next;
  } while (1);
}

size_t rc_client_get_rich_presence_message(rc_client_t* client, char buffer[], size_t buffer_size)
{
  int result;

  if (!client || !buffer)
    return 0;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client && client->state.external_client->get_rich_presence_message)
    return client->state.external_client->get_rich_presence_message(buffer, buffer_size);
#endif

  if (!client->game)
    return 0;

  rc_mutex_lock(&client->state.mutex);

  result = rc_runtime_get_richpresence(&client->game->runtime, buffer, (unsigned)buffer_size,
    rc_client_read_modified_memory_helper, client, NULL);

  rc_mutex_unlock(&client->state.mutex);

  if (result == 0) {
    result = snprintf(buffer, buffer_size, "Playing %s", client->game->public_.title);
    /* snprintf will return the amount of space needed, we want to return the number of chars written */
    if ((size_t)result >= buffer_size)
      return (buffer_size - 1);
  }

  return result;
}

static void rc_client_reset_richpresence(rc_client_t* client)
{
  rc_runtime_richpresence_t* richpresence = client->game->runtime.richpresence;
  if (richpresence && richpresence->richpresence)
    rc_reset_richpresence(richpresence->richpresence);
}

/* ===== Game ===== */

static void rc_client_invalidate_memref_achievements(rc_client_game_info_t* game, rc_client_t* client, rc_memref_t* memref)
{
  rc_client_subset_info_t* subset = game->subsets;
  for (; subset; subset = subset->next) {
    rc_client_achievement_info_t* achievement = subset->achievements;
    rc_client_achievement_info_t* stop = achievement + subset->public_.num_achievements;
    for (; achievement < stop; ++achievement) {
      if (achievement->public_.state == RC_CLIENT_ACHIEVEMENT_STATE_DISABLED)
        continue;

      if (rc_trigger_contains_memref(achievement->trigger, memref)) {
        achievement->public_.state = RC_CLIENT_ACHIEVEMENT_STATE_DISABLED;
        achievement->public_.bucket = RC_CLIENT_ACHIEVEMENT_BUCKET_UNSUPPORTED;

        if (achievement->trigger)
          achievement->trigger->state = RC_TRIGGER_STATE_DISABLED;

        RC_CLIENT_LOG_WARN_FORMATTED(client, "Disabled achievement %u. Invalid address %06X", achievement->public_.id, memref->address);
      }
    }
  }
}

static void rc_client_invalidate_memref_leaderboards(rc_client_game_info_t* game, rc_client_t* client, rc_memref_t* memref)
{
  rc_client_subset_info_t* subset = game->subsets;
  for (; subset; subset = subset->next) {
    rc_client_leaderboard_info_t* leaderboard = subset->leaderboards;
    rc_client_leaderboard_info_t* stop = leaderboard + subset->public_.num_leaderboards;
    for (; leaderboard < stop; ++leaderboard) {
      if (leaderboard->public_.state == RC_CLIENT_LEADERBOARD_STATE_DISABLED)
        continue;
      if (!leaderboard->lboard)
        continue;

      if (rc_trigger_contains_memref(&leaderboard->lboard->start, memref))
        leaderboard->public_.state = RC_CLIENT_LEADERBOARD_STATE_DISABLED;
      else if (rc_trigger_contains_memref(&leaderboard->lboard->cancel, memref))
        leaderboard->public_.state = RC_CLIENT_LEADERBOARD_STATE_DISABLED;
      else if (rc_trigger_contains_memref(&leaderboard->lboard->submit, memref))
        leaderboard->public_.state = RC_CLIENT_LEADERBOARD_STATE_DISABLED;
      else if (rc_value_contains_memref(&leaderboard->lboard->value, memref))
        leaderboard->public_.state = RC_CLIENT_LEADERBOARD_STATE_DISABLED;
      else
        continue;

      leaderboard->lboard->state = RC_LBOARD_STATE_DISABLED;

      RC_CLIENT_LOG_WARN_FORMATTED(client, "Disabled leaderboard %u. Invalid address %06X", leaderboard->public_.id, memref->address);
    }
  }
}

void rc_client_validate_addresses(rc_client_game_info_t* game, rc_client_t* client)
{
  const rc_memory_regions_t* regions = rc_console_memory_regions(game->public_.console_id);
  const uint32_t max_address = (regions && regions->num_regions > 0) ?
    regions->region[regions->num_regions - 1].end_address : 0xFFFFFFFF;
  uint8_t buffer[8];
  uint32_t total_count = 0;
  uint32_t invalid_count = 0;

  rc_memref_list_t* memref_list = &game->runtime.memrefs->memrefs;
  for (; memref_list; memref_list = memref_list->next) {
    rc_memref_t* memref = memref_list->items;
    const rc_memref_t* memref_end = memref + memref_list->count;
    total_count += memref_list->count;

    for (; memref < memref_end; ++memref) {
      if (memref->address > max_address ||
        client->callbacks.read_memory(memref->address, buffer, 1, client) == 0) {
        memref->value.type = RC_VALUE_TYPE_NONE;

        rc_client_invalidate_memref_achievements(game, client, memref);
        rc_client_invalidate_memref_leaderboards(game, client, memref);

        invalid_count++;
      }
    }
  }

  game->max_valid_address = max_address;
  RC_CLIENT_LOG_VERBOSE_FORMATTED(client, "%u/%u memory addresses valid", total_count - invalid_count, total_count);
}

/* ===== do_frame ===== */

static void rc_client_invalidate_processing_memref(rc_client_t* client)
{
  /* if processing_memref is not set, this occurred following a pointer chain. ignore it. */
  if (!client->state.processing_memref)
    return;

  client->state.processing_memref->value.type = RC_VALUE_TYPE_NONE;

  rc_client_invalidate_memref_achievements(client->game, client, client->state.processing_memref);
  rc_client_invalidate_memref_leaderboards(client->game, client, client->state.processing_memref);

  client->state.processing_memref = NULL;
}

static uint32_t RC_CCONV rc_client_read_memory_validator(uint32_t address, uint8_t* buffer, uint32_t num_bytes, void* ud)
{
  rc_client_t* client = (rc_client_t*)ud;

  uint32_t num_read = client->callbacks.read_memory(address, buffer, num_bytes, client);
  if (num_read < num_bytes)
    rc_client_invalidate_processing_memref(client);

  return num_read;
}

int rc_client_is_processing_required(rc_client_t* client)
{
  if (!client)
    return 0;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client && client->state.external_client->is_processing_required)
    return client->state.external_client->is_processing_required();
#endif

  if (!client->game)
    return 0;

  if (client->game->runtime.trigger_count || client->game->runtime.lboard_count)
    return 1;

  return (client->game->runtime.richpresence && client->game->runtime.richpresence->richpresence);
}

static void rc_client_update_memref_values(rc_client_t* client, rc_eval_state_t* eval_state) {
  rc_memrefs_t* memrefs = client->game->runtime.memrefs;
  rc_memref_list_t* memref_list;
  rc_modified_memref_list_t* modified_memref_list;
  int invalidated_memref = 0;

  memref_list = &memrefs->memrefs;
  do {
    rc_memref_t* memref = memref_list->items;
    const rc_memref_t* memref_stop = memref + memref_list->count;
    uint32_t value;

    for (; memref < memref_stop; ++memref) {
      if (memref->value.type == RC_VALUE_TYPE_NONE)
        continue;

      /* if processing_memref is set, and the memory read fails, all dependent achievements will be disabled */
      client->state.processing_memref = memref;

      value = rc_read_memory(memref->address, memref->value.size, rc_client_read_memory_validator, client);

      if (client->state.processing_memref) {
        rc_update_memref_value(&memref->value, value);
      }
      else {
        /* if the peek function cleared the processing_memref, the memref was invalidated */
        invalidated_memref = 1;
      }
    }

    memref_list = memref_list->next;
  } while (memref_list);

  client->state.processing_memref = NULL;

  modified_memref_list = &memrefs->modified_memrefs;
  if (modified_memref_list->count) {
    do {
      rc_modified_memref_t* modified_memref = modified_memref_list->items;
      const rc_modified_memref_t* modified_memref_stop = modified_memref + modified_memref_list->count;

      for (; modified_memref < modified_memref_stop; ++modified_memref) {
        rc_update_memref_value(&modified_memref->memref.value,
          rc_get_modified_memref_value(modified_memref, eval_state));
      }

      modified_memref_list = modified_memref_list->next;
    } while (modified_memref_list);
  }

  if (invalidated_memref)
    rc_client_update_active_achievements(client->game);
}

static void rc_client_do_frame_process_achievements(rc_client_t* client, rc_client_subset_info_t* subset, rc_eval_state_t* eval_state)
{
  rc_client_achievement_info_t* achievement = subset->achievements;
  rc_client_achievement_info_t* stop = achievement + subset->public_.num_achievements;

  for (; achievement < stop; ++achievement) {
    rc_trigger_t* trigger = achievement->trigger;
    int old_state, new_state;
    uint32_t old_measured_value;

    if (!trigger || achievement->public_.state != RC_CLIENT_ACHIEVEMENT_STATE_ACTIVE)
      continue;

    old_measured_value = trigger->measured_value;
    old_state = trigger->state;
    new_state = rc_test_trigger(trigger, eval_state);

    /* trigger->state doesn't actually change to RESET - RESET just serves as a notification.
     * we don't care about that particular notification, so look at the actual state. */
    if (new_state == RC_TRIGGER_STATE_RESET)
      new_state = trigger->state;

    /* if the measured value changed and the achievement hasn't triggered, show a progress indicator */
    if (trigger->measured_value != old_measured_value && old_measured_value != RC_MEASURED_UNKNOWN &&
        trigger->measured_value <= trigger->measured_target &&
        trigger->measured_target != 0 &&
        rc_trigger_state_active(new_state) && new_state != RC_TRIGGER_STATE_WAITING) {

      /* only show a popup for the achievement closest to triggering */
      float progress = (float)trigger->measured_value / (float)trigger->measured_target;

      if (trigger->measured_as_percent) {
        /* if reporting the measured value as a percentage, only show the popup if the percentage changes */
        const uint32_t old_percent = (uint32_t)(((unsigned long long)old_measured_value * 100) / trigger->measured_target);
        const uint32_t new_percent = (uint32_t)(((unsigned long long)trigger->measured_value * 100) / trigger->measured_target);
        if (old_percent == new_percent)
          progress = -1.0;
      }

      if (progress > client->game->progress_tracker.progress) {
        client->game->progress_tracker.progress = progress;
        client->game->progress_tracker.achievement = achievement;
        client->game->pending_events |= RC_CLIENT_GAME_PENDING_EVENT_PROGRESS_TRACKER;
        subset->pending_events |= RC_CLIENT_SUBSET_PENDING_EVENT_ACHIEVEMENT;
        achievement->pending_events |= RC_CLIENT_ACHIEVEMENT_PENDING_EVENT_UPDATE;
      }
    }

    /* if the state hasn't changed, there won't be any events raised */
    if (new_state == old_state)
      continue;

    /* raise a CHALLENGE_INDICATOR_HIDE event when changing from PRIMED to anything else */
    if (old_state == RC_TRIGGER_STATE_PRIMED)
      achievement->pending_events |= RC_CLIENT_ACHIEVEMENT_PENDING_EVENT_CHALLENGE_INDICATOR_HIDE;

    /* raise events for each of the possible new states */
    if (new_state == RC_TRIGGER_STATE_TRIGGERED)
      achievement->pending_events |= RC_CLIENT_ACHIEVEMENT_PENDING_EVENT_TRIGGERED;
    else if (new_state == RC_TRIGGER_STATE_PRIMED)
      achievement->pending_events |= RC_CLIENT_ACHIEVEMENT_PENDING_EVENT_CHALLENGE_INDICATOR_SHOW;

    subset->pending_events |= RC_CLIENT_SUBSET_PENDING_EVENT_ACHIEVEMENT;
  }
}

void rc_client_hide_progress_tracker(rc_client_t* client, rc_client_game_info_t* game)
{
  /* ASSERT: this should only be called if the mutex is held */

  if (game->progress_tracker.hide_callback &&
      game->progress_tracker.hide_callback->when &&
      game->progress_tracker.action == RC_CLIENT_PROGRESS_TRACKER_ACTION_NONE) {
    rc_client_reschedule_callback(client, game->progress_tracker.hide_callback, 0);
    game->progress_tracker.action = RC_CLIENT_PROGRESS_TRACKER_ACTION_HIDE;
    game->pending_events |= RC_CLIENT_GAME_PENDING_EVENT_PROGRESS_TRACKER;
  }
}

static void rc_client_progress_tracker_timer_elapsed(rc_client_scheduled_callback_data_t* callback_data, rc_client_t* client, rc_clock_t now)
{
  rc_client_event_t client_event;
  memset(&client_event, 0, sizeof(client_event));

  (void)callback_data;
  (void)now;

  rc_mutex_lock(&client->state.mutex);
  if (client->game->progress_tracker.action == RC_CLIENT_PROGRESS_TRACKER_ACTION_NONE) {
    client->game->progress_tracker.hide_callback->when = 0;
    client_event.type = RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_HIDE;
  }
  rc_mutex_unlock(&client->state.mutex);

  if (client_event.type)
    client->callbacks.event_handler(&client_event, client);
}

static void rc_client_do_frame_update_progress_tracker(rc_client_t* client, rc_client_game_info_t* game)
{
  /* ASSERT: this should only be called if the mutex is held */

  if (!game->progress_tracker.hide_callback) {
    game->progress_tracker.hide_callback = (rc_client_scheduled_callback_data_t*)
        rc_buffer_alloc(&game->buffer, sizeof(rc_client_scheduled_callback_data_t));
    memset(game->progress_tracker.hide_callback, 0, sizeof(rc_client_scheduled_callback_data_t));
    game->progress_tracker.hide_callback->callback = rc_client_progress_tracker_timer_elapsed;
  }

  if (game->progress_tracker.hide_callback->when == 0)
    game->progress_tracker.action = RC_CLIENT_PROGRESS_TRACKER_ACTION_SHOW;
  else
    game->progress_tracker.action = RC_CLIENT_PROGRESS_TRACKER_ACTION_UPDATE;

  rc_client_reschedule_callback(client, game->progress_tracker.hide_callback,
    client->callbacks.get_time_millisecs(client) + 3 * 1000);
}

static void rc_client_raise_progress_tracker_events(rc_client_t* client, rc_client_game_info_t* game)
{
  rc_client_event_t client_event;

  memset(&client_event, 0, sizeof(client_event));

  switch (game->progress_tracker.action) {
    case RC_CLIENT_PROGRESS_TRACKER_ACTION_SHOW:
      client_event.type = RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW;
      break;
    case RC_CLIENT_PROGRESS_TRACKER_ACTION_HIDE:
      client_event.type = RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_HIDE;
      break;
    default:
      client_event.type = RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_UPDATE;
      break;
  }
  game->progress_tracker.action = RC_CLIENT_PROGRESS_TRACKER_ACTION_NONE;

  client_event.achievement = &game->progress_tracker.achievement->public_;
  client->callbacks.event_handler(&client_event, client);
}

static void rc_client_raise_achievement_events(rc_client_t* client, rc_client_subset_info_t* subset)
{
  rc_client_achievement_info_t* achievement = subset->achievements;
  rc_client_achievement_info_t* stop = achievement + subset->public_.num_achievements;
  rc_client_event_t client_event;
  time_t recent_unlock_time = 0;

  memset(&client_event, 0, sizeof(client_event));

  for (; achievement < stop; ++achievement) {
    if (achievement->pending_events == RC_CLIENT_ACHIEVEMENT_PENDING_EVENT_NONE)
      continue;

    /* kick off award achievement request first */
    if (achievement->pending_events & RC_CLIENT_ACHIEVEMENT_PENDING_EVENT_TRIGGERED) {
      rc_client_award_achievement(client, achievement);
      client->game->pending_events |= RC_CLIENT_GAME_PENDING_EVENT_UPDATE_ACTIVE_ACHIEVEMENTS;
    }

    /* update display state */
    if (recent_unlock_time == 0)
      recent_unlock_time = time(NULL) - RC_CLIENT_RECENT_UNLOCK_DELAY_SECONDS;
    rc_client_update_achievement_display_information(client, achievement, recent_unlock_time);

    /* raise events */
    client_event.achievement = &achievement->public_;

    if (achievement->pending_events & RC_CLIENT_ACHIEVEMENT_PENDING_EVENT_CHALLENGE_INDICATOR_HIDE) {
      client_event.type = RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_HIDE;
      client->callbacks.event_handler(&client_event, client);
    }
    else if (achievement->pending_events & RC_CLIENT_ACHIEVEMENT_PENDING_EVENT_CHALLENGE_INDICATOR_SHOW) {
      client_event.type = RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW;
      client->callbacks.event_handler(&client_event, client);
    }

    if (achievement->pending_events & RC_CLIENT_ACHIEVEMENT_PENDING_EVENT_TRIGGERED) {
      client_event.type = RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED;
      client->callbacks.event_handler(&client_event, client);
    }

    /* clear pending flags */
    achievement->pending_events = RC_CLIENT_ACHIEVEMENT_PENDING_EVENT_NONE;
  }
}

static void rc_client_raise_mastery_event(rc_client_t* client, rc_client_subset_info_t* subset)
{
  rc_client_event_t client_event;

  memset(&client_event, 0, sizeof(client_event));
  client_event.subset = &subset->public_;

  if (subset == client->game->subsets)
    client_event.type = RC_CLIENT_EVENT_GAME_COMPLETED;
  else
    client_event.type = RC_CLIENT_EVENT_SUBSET_COMPLETED;

  subset->mastery = RC_CLIENT_MASTERY_STATE_SHOWN;

  client->callbacks.event_handler(&client_event, client);
}

static void rc_client_do_frame_process_leaderboards(rc_client_t* client, rc_client_subset_info_t* subset, rc_eval_state_t* eval_state)
{
  rc_client_leaderboard_info_t* leaderboard = subset->leaderboards;
  rc_client_leaderboard_info_t* stop = leaderboard + subset->public_.num_leaderboards;

  for (; leaderboard < stop; ++leaderboard) {
    rc_lboard_t* lboard = leaderboard->lboard;
    int old_state, new_state;

    switch (leaderboard->public_.state) {
      case RC_CLIENT_LEADERBOARD_STATE_INACTIVE:
      case RC_CLIENT_LEADERBOARD_STATE_DISABLED:
        continue;

      default:
        if (!lboard)
          continue;

        break;
    }

    old_state = lboard->state;
    new_state = rc_evaluate_lboard(lboard, &leaderboard->value, eval_state);

    switch (new_state) {
      case RC_LBOARD_STATE_STARTED: /* leaderboard is running */
        if (old_state != RC_LBOARD_STATE_STARTED) {
          leaderboard->public_.state = RC_CLIENT_LEADERBOARD_STATE_TRACKING;
          leaderboard->pending_events |= RC_CLIENT_LEADERBOARD_PENDING_EVENT_STARTED;
          rc_client_allocate_leaderboard_tracker(client->game, leaderboard);
        }
        else {
          rc_client_update_leaderboard_tracker(client->game, leaderboard);
        }
        break;

      case RC_LBOARD_STATE_CANCELED:
        if (old_state != RC_LBOARD_STATE_CANCELED) {
          leaderboard->public_.state = RC_CLIENT_LEADERBOARD_STATE_ACTIVE;
          leaderboard->pending_events |= RC_CLIENT_LEADERBOARD_PENDING_EVENT_FAILED;
          rc_client_release_leaderboard_tracker(client->game, leaderboard);
        }
        break;

      case RC_LBOARD_STATE_TRIGGERED:
        if (old_state != RC_RUNTIME_EVENT_LBOARD_TRIGGERED) {
          leaderboard->public_.state = RC_CLIENT_LEADERBOARD_STATE_ACTIVE;
          leaderboard->pending_events |= RC_CLIENT_LEADERBOARD_PENDING_EVENT_SUBMITTED;

          if (old_state != RC_LBOARD_STATE_STARTED)
            rc_client_allocate_leaderboard_tracker(client->game, leaderboard);
          else
            rc_client_update_leaderboard_tracker(client->game, leaderboard);

          rc_client_release_leaderboard_tracker(client->game, leaderboard);
        }
        break;
    }

    if (leaderboard->pending_events)
      subset->pending_events |= RC_CLIENT_SUBSET_PENDING_EVENT_LEADERBOARD;
  }
}

static void rc_client_raise_leaderboard_tracker_events(rc_client_t* client, rc_client_game_info_t* game)
{
  rc_client_leaderboard_tracker_info_t* tracker = game->leaderboard_trackers;
  rc_client_event_t client_event;

  memset(&client_event, 0, sizeof(client_event));

  tracker = game->leaderboard_trackers;
  for (; tracker; tracker = tracker->next) {
    if (tracker->pending_events == RC_CLIENT_LEADERBOARD_TRACKER_PENDING_EVENT_NONE)
      continue;

    client_event.leaderboard_tracker = &tracker->public_;

    /* update display text for new trackers or updated trackers */
    if (tracker->pending_events & (RC_CLIENT_LEADERBOARD_TRACKER_PENDING_EVENT_SHOW | RC_CLIENT_LEADERBOARD_TRACKER_PENDING_EVENT_UPDATE))
      rc_format_value(tracker->public_.display, sizeof(tracker->public_.display), tracker->raw_value, tracker->format);

    if (tracker->pending_events & RC_CLIENT_LEADERBOARD_TRACKER_PENDING_EVENT_HIDE) {
      if (tracker->pending_events & RC_CLIENT_LEADERBOARD_TRACKER_PENDING_EVENT_SHOW) {
        /* request to show and hide in the same frame - ignore the event */
      }
      else {
        client_event.type = RC_CLIENT_EVENT_LEADERBOARD_TRACKER_HIDE;
        client->callbacks.event_handler(&client_event, client);
      }
    }
    else if (tracker->pending_events & RC_CLIENT_LEADERBOARD_TRACKER_PENDING_EVENT_SHOW) {
      client_event.type = RC_CLIENT_EVENT_LEADERBOARD_TRACKER_SHOW;
      client->callbacks.event_handler(&client_event, client);
    }
    else if (tracker->pending_events & RC_CLIENT_LEADERBOARD_TRACKER_PENDING_EVENT_UPDATE) {
      client_event.type = RC_CLIENT_EVENT_LEADERBOARD_TRACKER_UPDATE;
      client->callbacks.event_handler(&client_event, client);
    }

    tracker->pending_events = RC_CLIENT_LEADERBOARD_TRACKER_PENDING_EVENT_NONE;
  }
}

static void rc_client_raise_leaderboard_events(rc_client_t* client, rc_client_subset_info_t* subset)
{
  rc_client_leaderboard_info_t* leaderboard = subset->leaderboards;
  rc_client_leaderboard_info_t* leaderboard_stop = leaderboard + subset->public_.num_leaderboards;
  rc_client_event_t client_event;

  memset(&client_event, 0, sizeof(client_event));

  for (; leaderboard < leaderboard_stop; ++leaderboard) {
    if (leaderboard->pending_events == RC_CLIENT_LEADERBOARD_PENDING_EVENT_NONE)
      continue;

    client_event.leaderboard = &leaderboard->public_;

    if (leaderboard->pending_events & RC_CLIENT_LEADERBOARD_PENDING_EVENT_FAILED) {
      RC_CLIENT_LOG_VERBOSE_FORMATTED(client, "Leaderboard %u canceled: %s", leaderboard->public_.id, leaderboard->public_.title);
      client_event.type = RC_CLIENT_EVENT_LEADERBOARD_FAILED;
      client->callbacks.event_handler(&client_event, client);
    }
    else if (leaderboard->pending_events & RC_CLIENT_LEADERBOARD_PENDING_EVENT_SUBMITTED) {
      /* kick off submission request before raising event */
      rc_client_submit_leaderboard_entry(client, leaderboard);

      client_event.type = RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED;
      client->callbacks.event_handler(&client_event, client);
    }
    else if (leaderboard->pending_events & RC_CLIENT_LEADERBOARD_PENDING_EVENT_STARTED) {
      RC_CLIENT_LOG_VERBOSE_FORMATTED(client, "Leaderboard %u started: %s", leaderboard->public_.id, leaderboard->public_.title);
      client_event.type = RC_CLIENT_EVENT_LEADERBOARD_STARTED;
      client->callbacks.event_handler(&client_event, client);
    }

    leaderboard->pending_events = RC_CLIENT_LEADERBOARD_PENDING_EVENT_NONE;
  }
}

void rc_client_reset_pending_events(rc_client_t* client)
{
  rc_client_subset_info_t* subset;

  client->game->pending_events = RC_CLIENT_GAME_PENDING_EVENT_NONE;

  for (subset = client->game->subsets; subset; subset = subset->next)
    subset->pending_events = RC_CLIENT_SUBSET_PENDING_EVENT_NONE;
}

static void rc_client_subset_raise_pending_events(rc_client_t* client, rc_client_subset_info_t* subset)
{
  /* raise any pending achievement events */
  if (subset->pending_events & RC_CLIENT_SUBSET_PENDING_EVENT_ACHIEVEMENT)
    rc_client_raise_achievement_events(client, subset);

  /* raise any pending leaderboard events */
  if (subset->pending_events & RC_CLIENT_SUBSET_PENDING_EVENT_LEADERBOARD)
    rc_client_raise_leaderboard_events(client, subset);

  /* raise mastery event if pending */
  if (subset->mastery == RC_CLIENT_MASTERY_STATE_PENDING)
    rc_client_raise_mastery_event(client, subset);
}

void rc_client_raise_pending_events(rc_client_t* client, rc_client_game_info_t* game)
{
  rc_client_subset_info_t* subset;

  /* raise tracker events before leaderboard events so formatted values are updated for leaderboard events */
  if (game->pending_events & RC_CLIENT_GAME_PENDING_EVENT_LEADERBOARD_TRACKER)
    rc_client_raise_leaderboard_tracker_events(client, game);

  for (subset = game->subsets; subset; subset = subset->next)
    rc_client_subset_raise_pending_events(client, subset);

  /* raise progress tracker events after achievement events so formatted values are updated for tracker event */
  if (game->pending_events & RC_CLIENT_GAME_PENDING_EVENT_PROGRESS_TRACKER)
    rc_client_raise_progress_tracker_events(client, game);

  /* if any achievements were unlocked, resync the active achievements list */
  if (game->pending_events & RC_CLIENT_GAME_PENDING_EVENT_UPDATE_ACTIVE_ACHIEVEMENTS) {
    rc_mutex_lock(&client->state.mutex);
    rc_client_update_active_achievements(game);
    rc_mutex_unlock(&client->state.mutex);
  }

  game->pending_events = RC_CLIENT_GAME_PENDING_EVENT_NONE;
}

void rc_client_do_frame(rc_client_t* client)
{
  if (!client)
    return;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client && client->state.external_client->do_frame) {
    client->state.external_client->do_frame();
    return;
  }
#endif

  if (client->game && !client->game->waiting_for_reset) {
    rc_runtime_richpresence_t* richpresence;
    rc_client_subset_info_t* subset;
    rc_eval_state_t eval_state;

    rc_init_eval_state(&eval_state, rc_client_read_modified_memory_helper, client);

    rc_mutex_lock(&client->state.mutex);

    rc_client_reset_pending_events(client);

    rc_client_update_memref_values(client, &eval_state);

    client->game->progress_tracker.progress = 0.0;
    for (subset = client->game->subsets; subset; subset = subset->next) {
      if (subset->active)
        rc_client_do_frame_process_achievements(client, subset, &eval_state);
    }
    if (client->game->pending_events & RC_CLIENT_GAME_PENDING_EVENT_PROGRESS_TRACKER)
      rc_client_do_frame_update_progress_tracker(client, client->game);

    if (client->state.hardcore || client->state.allow_leaderboards_in_casual) {
      for (subset = client->game->subsets; subset; subset = subset->next) {
        if (subset->active)
          rc_client_do_frame_process_leaderboards(client, subset, &eval_state);
      }
    }

    richpresence = client->game->runtime.richpresence;
    if (richpresence && richpresence->richpresence)
      rc_update_richpresence(richpresence->richpresence, &eval_state);

    rc_mutex_unlock(&client->state.mutex);

    rc_client_raise_pending_events(client, client->game);

    ++client->state.frames_processed;
  }

  /* we've processed a frame. if there's a pause delay in effect, process it */
  if (client->state.unpaused_frame_decay > 0) {
    client->state.unpaused_frame_decay--;

    if (client->state.unpaused_frame_decay == 0 &&
      client->state.required_unpaused_frames > RC_MINIMUM_UNPAUSED_FRAMES) {
      /* the full decay has elapsed and a penalty still exists.
       * lower the penalty and reset the decay counter */
      client->state.required_unpaused_frames >>= 1;

      if (client->state.required_unpaused_frames <= RC_MINIMUM_UNPAUSED_FRAMES)
        client->state.required_unpaused_frames = RC_MINIMUM_UNPAUSED_FRAMES;

      client->state.unpaused_frame_decay =
        client->state.required_unpaused_frames * (RC_PAUSE_DECAY_MULTIPLIER - 1) - 1;
    }
  }

  rc_client_idle(client);
}

void rc_client_idle(rc_client_t* client)
{
  rc_client_scheduled_callback_data_t* scheduled_callback;

  if (!client)
    return;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client && client->state.external_client->idle) {
    client->state.external_client->idle();
    return;
  }
#endif

  scheduled_callback = client->state.scheduled_callbacks;
  if (scheduled_callback) {
    const rc_clock_t now = client->callbacks.get_time_millisecs(client);

    do {
      rc_mutex_lock(&client->state.mutex);
      scheduled_callback = client->state.scheduled_callbacks;
      if (scheduled_callback) {
        if (scheduled_callback->when > now) {
          /* not time for next callback yet, ignore it */
          scheduled_callback = NULL;
        }
        else {
          /* remove the callback from the queue while we process it. callback can requeue if desired */
          client->state.scheduled_callbacks = scheduled_callback->next;
          scheduled_callback->next = NULL;
        }
      }
      rc_mutex_unlock(&client->state.mutex);

      if (!scheduled_callback)
        break;

      scheduled_callback->callback(scheduled_callback, client, now);
    } while (1);
  }

  if (client->state.disconnect & ~RC_CLIENT_DISCONNECT_VISIBLE)
    rc_client_raise_disconnect_events(client);
}

int rc_client_can_pause(rc_client_t* client, uint32_t* frames_remaining)
{
  if (!client)
    return 1;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client && client->state.external_client->can_pause)
    return client->state.external_client->can_pause(frames_remaining);
#endif

  if (frames_remaining)
    *frames_remaining = 0;

  /* pause is always allowed in casual mode */
  if (!rc_client_get_hardcore_enabled(client))
    return 1;

  /* a full decay means we haven't processed any frames since the last time this was called. */
  if (client->state.unpaused_frame_decay == client->state.required_unpaused_frames * RC_PAUSE_DECAY_MULTIPLIER)
    return 1;

  /* if less than RC_MINIMUM_UNPAUSED_FRAMES have been processed, don't allow the pause */
  if (client->state.unpaused_frame_decay > client->state.required_unpaused_frames * (RC_PAUSE_DECAY_MULTIPLIER - 1)) {
    if (frames_remaining) {
      *frames_remaining = client->state.unpaused_frame_decay -
        client->state.required_unpaused_frames * (RC_PAUSE_DECAY_MULTIPLIER - 1);
    }
    return 0;
  }

  /* we're going to allow the emulator to pause. calculate how many frames are needed before the next
   * pause will be allowed. */

  if (client->state.unpaused_frame_decay > 0) {
    /* The user has paused within the decay window. Require a longer
     * run of unpaused frames before allowing the next pause */
    if (client->state.required_unpaused_frames < 5 * 60) /* don't make delay longer then 5 seconds */
      client->state.required_unpaused_frames += RC_MINIMUM_UNPAUSED_FRAMES;
  }

  /* require multiple unpaused_frames windows to decay the penalty */
  client->state.unpaused_frame_decay = client->state.required_unpaused_frames * RC_PAUSE_DECAY_MULTIPLIER;

  return 1;
}

static void rc_client_reset_variables(rc_client_t* client)
{
  if (client->game->runtime.richpresence && client->game->runtime.richpresence->richpresence)
    rc_reset_values(client->game->runtime.richpresence->richpresence->values);
}

void rc_client_reset_all(rc_client_t* client)
{
  rc_client_reset_achievements(client);
  rc_client_reset_leaderboards(client);
  rc_client_reset_richpresence(client);
  rc_client_reset_variables(client);
}

void rc_client_reset(rc_client_t* client)
{
  rc_client_game_hash_t* game_hash;
  if (!client)
    return;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client && client->state.external_client->reset) {
    client->state.external_client->reset();
    return;
  }
#endif

  if (!client->game)
    return;

  game_hash = rc_client_find_game_hash(client, client->game->public_.hash);
  if (game_hash && game_hash->game_id != client->game->public_.id) {
    /* current media is not for loaded game. unload game */
    RC_CLIENT_LOG_WARN_FORMATTED(client, "Disabling runtime. Reset with non-game media loaded: %u (%s)",
      (game_hash->game_id == RC_CLIENT_UNKNOWN_GAME_ID) ? 0 : game_hash->game_id, game_hash->hash);
    rc_client_unload_game(client);
    return;
  }

  RC_CLIENT_LOG_INFO(client, "Resetting runtime");

  rc_mutex_lock(&client->state.mutex);

  client->game->waiting_for_reset = 0;
  rc_client_reset_pending_events(client);

  rc_client_hide_progress_tracker(client, client->game);
  rc_client_reset_all(client);

  rc_mutex_unlock(&client->state.mutex);

  rc_client_raise_pending_events(client, client->game);
}

void rc_client_game_mark_ui_to_be_hidden(rc_client_t* client, rc_client_game_info_t* game)
{
  rc_client_achievement_info_t* achievement;
  rc_client_achievement_info_t* achievement_stop;
  rc_client_leaderboard_info_t* leaderboard;
  rc_client_leaderboard_info_t* leaderboard_stop;
  rc_client_subset_info_t* subset;

  for (subset = game->subsets; subset; subset = subset->next) {
    achievement = subset->achievements;
    achievement_stop = achievement + subset->public_.num_achievements;
    for (; achievement < achievement_stop; ++achievement) {
      if (achievement->public_.state == RC_CLIENT_ACHIEVEMENT_STATE_ACTIVE &&
        achievement->trigger && achievement->trigger->state == RC_TRIGGER_STATE_PRIMED) {
        achievement->pending_events |= RC_CLIENT_ACHIEVEMENT_PENDING_EVENT_CHALLENGE_INDICATOR_HIDE;
        subset->pending_events |= RC_CLIENT_SUBSET_PENDING_EVENT_ACHIEVEMENT;
      }
    }

    leaderboard = subset->leaderboards;
    leaderboard_stop = leaderboard + subset->public_.num_leaderboards;
    for (; leaderboard < leaderboard_stop; ++leaderboard) {
      if (leaderboard->public_.state == RC_CLIENT_LEADERBOARD_STATE_TRACKING)
        rc_client_release_leaderboard_tracker(game, leaderboard);
    }
  }

  rc_client_hide_progress_tracker(client, game);
}

/* ===== scheduled callbacks ===== */

void rc_client_schedule_callback(rc_client_t* client, rc_client_scheduled_callback_data_t* scheduled_callback)
{
  rc_client_scheduled_callback_data_t** last;
  rc_client_scheduled_callback_data_t* next;

  rc_mutex_lock(&client->state.mutex);

  last = &client->state.scheduled_callbacks;
  do {
    next = *last;
    if (!next || scheduled_callback->when < next->when) {
      scheduled_callback->next = next;
      *last = scheduled_callback;
      break;
    }

    last = &next->next;
  } while (1);

  rc_mutex_unlock(&client->state.mutex);
}

int rc_client_has_server_retry_scheduled_callback(rc_client_t* client)
{
  /* ASSERT: this should only be called if the mutex is held */

  rc_client_scheduled_callback_data_t* scheduled_callback = client->state.scheduled_callbacks;
  for (; scheduled_callback; scheduled_callback = scheduled_callback->next) {
    if (scheduled_callback->callback == rc_client_award_achievement_retry ||
      scheduled_callback->callback == rc_client_submit_leaderboard_entry_retry) {
      return 1;
    }
  }

  return 0;
}

static void rc_client_reschedule_callback(rc_client_t* client, rc_client_scheduled_callback_data_t* callback, rc_clock_t when)
{
  rc_client_scheduled_callback_data_t** last;
  rc_client_scheduled_callback_data_t* next;

  /* ASSERT: this should only be called if the mutex is held */

  callback->when = when;

  last = &client->state.scheduled_callbacks;
  do {
    next = *last;

    if (next == callback) {
      if (when == 0) {
        /* request to unschedule the callback */
        *last = next->next;
        next->next = NULL;
        break;
      }

      if (!next->next) {
        /* end of list, just append it */
        break;
      }

      if (when < next->next->when) {
        /* already in the correct place */
        break;
      }

      /* remove from current position - will insert later */
      *last = next->next;
      next->next = NULL;
      continue;
    }

    if (!next || (when < next->when && when != 0)) {
      /* insert here */
      callback->next = next;
      *last = callback;
      break;
    }

    last = &next->next;
  } while (1);
}

/* ===== serialize state ===== */

size_t rc_client_progress_size(rc_client_t* client)
{
  size_t result;

  if (!client)
    return 0;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client && client->state.external_client->progress_size)
    return client->state.external_client->progress_size();
#endif

  if (!rc_client_is_game_loaded(client))
    return 0;

  rc_mutex_lock(&client->state.mutex);
  result = rc_runtime_progress_size(&client->game->runtime, NULL);
  rc_mutex_unlock(&client->state.mutex);

  return result;
}

int rc_client_serialize_progress(rc_client_t* client, uint8_t* buffer)
{
  return rc_client_serialize_progress_sized(client, buffer, 0xFFFFFFFF);
}

int rc_client_serialize_progress_sized(rc_client_t* client, uint8_t* buffer, size_t buffer_size)
{
  int result;

  if (!client)
    return RC_NO_GAME_LOADED;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client && client->state.external_client->serialize_progress)
    return client->state.external_client->serialize_progress(buffer, buffer_size);
#endif

  if (!rc_client_is_game_loaded(client))
    return RC_NO_GAME_LOADED;

  if (!buffer)
    return RC_INVALID_STATE;

  rc_mutex_lock(&client->state.mutex);
  result = rc_runtime_serialize_progress_sized(buffer, (uint32_t)buffer_size, &client->game->runtime, NULL);
  rc_mutex_unlock(&client->state.mutex);

  return result;
}

static void rc_client_subset_before_deserialize_progress(rc_client_subset_info_t* subset)
{
  rc_client_achievement_info_t* achievement;
  rc_client_achievement_info_t* achievement_stop;
  rc_client_leaderboard_info_t* leaderboard;
  rc_client_leaderboard_info_t* leaderboard_stop;

  /* flag any visible challenge indicators to be hidden */
  achievement = subset->achievements;
  achievement_stop = achievement + subset->public_.num_achievements;
  for (; achievement < achievement_stop; ++achievement) {
    rc_trigger_t* trigger = achievement->trigger;
    if (trigger && trigger->state == RC_TRIGGER_STATE_PRIMED &&
      achievement->public_.state == RC_CLIENT_ACHIEVEMENT_STATE_ACTIVE) {
      achievement->pending_events |= RC_CLIENT_ACHIEVEMENT_PENDING_EVENT_CHALLENGE_INDICATOR_HIDE;
      subset->pending_events |= RC_CLIENT_SUBSET_PENDING_EVENT_ACHIEVEMENT;
    }
  }

  /* flag any visible trackers to be hidden */
  leaderboard = subset->leaderboards;
  leaderboard_stop = leaderboard + subset->public_.num_leaderboards;
  for (; leaderboard < leaderboard_stop; ++leaderboard) {
    rc_lboard_t* lboard = leaderboard->lboard;
    if (lboard && lboard->state == RC_LBOARD_STATE_STARTED &&
      leaderboard->public_.state == RC_CLIENT_LEADERBOARD_STATE_TRACKING) {
      leaderboard->pending_events |= RC_CLIENT_LEADERBOARD_PENDING_EVENT_FAILED;
      subset->pending_events |= RC_CLIENT_SUBSET_PENDING_EVENT_LEADERBOARD;
    }
  }
}

static void rc_client_subset_after_deserialize_progress(rc_client_game_info_t* game, rc_client_subset_info_t* subset)
{
  rc_client_achievement_info_t* achievement;
  rc_client_achievement_info_t* achievement_stop;
  rc_client_leaderboard_info_t* leaderboard;
  rc_client_leaderboard_info_t* leaderboard_stop;

  /* flag any challenge indicators that should be shown */
  achievement = subset->achievements;
  achievement_stop = achievement + subset->public_.num_achievements;
  for (; achievement < achievement_stop; ++achievement) {
    rc_trigger_t* trigger = achievement->trigger;
    if (!trigger || achievement->public_.state != RC_CLIENT_ACHIEVEMENT_STATE_ACTIVE)
      continue;

    if (trigger->state == RC_TRIGGER_STATE_PRIMED) {
      /* if it's already shown, just keep it. otherwise flag it to be shown */
      if (achievement->pending_events & RC_CLIENT_ACHIEVEMENT_PENDING_EVENT_CHALLENGE_INDICATOR_HIDE) {
        achievement->pending_events &= ~RC_CLIENT_ACHIEVEMENT_PENDING_EVENT_CHALLENGE_INDICATOR_HIDE;
      }
      else {
        achievement->pending_events |= RC_CLIENT_ACHIEVEMENT_PENDING_EVENT_CHALLENGE_INDICATOR_SHOW;
        subset->pending_events |= RC_CLIENT_SUBSET_PENDING_EVENT_ACHIEVEMENT;
      }
    }
    /* ASSERT: only active achievements are serialized, so we don't have to worry about
     *         deserialization deactiving them. */
  }

  /* flag any trackers that need to be shown */
  leaderboard = subset->leaderboards;
  leaderboard_stop = leaderboard + subset->public_.num_leaderboards;
  for (; leaderboard < leaderboard_stop; ++leaderboard) {
    rc_lboard_t* lboard = leaderboard->lboard;
    if (!lboard ||
      leaderboard->public_.state == RC_CLIENT_LEADERBOARD_STATE_INACTIVE ||
      leaderboard->public_.state == RC_CLIENT_LEADERBOARD_STATE_DISABLED)
      continue;

    if (lboard->state == RC_LBOARD_STATE_STARTED) {
      leaderboard->value = (int)lboard->value.value.value;
      leaderboard->public_.state = RC_CLIENT_LEADERBOARD_STATE_TRACKING;

      /* if it's already being tracked, just update tracker. otherwise, allocate one */
      if (leaderboard->pending_events & RC_CLIENT_LEADERBOARD_PENDING_EVENT_FAILED) {
        leaderboard->pending_events &= ~RC_CLIENT_LEADERBOARD_PENDING_EVENT_FAILED;
        rc_client_update_leaderboard_tracker(game, leaderboard);
      }
      else {
        rc_client_allocate_leaderboard_tracker(game, leaderboard);
      }
    }
    else if (leaderboard->pending_events & RC_CLIENT_LEADERBOARD_PENDING_EVENT_FAILED) {
      /* deallocate the tracker (don't actually raise the failed event) */
      leaderboard->pending_events &= ~RC_CLIENT_LEADERBOARD_PENDING_EVENT_FAILED;
      leaderboard->public_.state = RC_CLIENT_LEADERBOARD_STATE_ACTIVE;
      rc_client_release_leaderboard_tracker(game, leaderboard);
    }
  }
}

int rc_client_deserialize_progress(rc_client_t* client, const uint8_t* serialized)
{
  return rc_client_deserialize_progress_sized(client, serialized, 0xFFFFFFFF);
}

int rc_client_deserialize_progress_sized(rc_client_t* client, const uint8_t* serialized, size_t serialized_size)
{
  rc_client_subset_info_t* subset;
  int result;

  if (!client)
    return RC_NO_GAME_LOADED;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client && client->state.external_client->deserialize_progress)
    return client->state.external_client->deserialize_progress(serialized, serialized_size);
#endif

  if (!rc_client_is_game_loaded(client))
    return RC_NO_GAME_LOADED;

  rc_mutex_lock(&client->state.mutex);

  rc_client_reset_pending_events(client);

  for (subset = client->game->subsets; subset; subset = subset->next)
    rc_client_subset_before_deserialize_progress(subset);

  rc_client_hide_progress_tracker(client, client->game);

  if (!serialized) {
    rc_client_reset_all(client);
    result = RC_OK;
  }
  else {
    result = rc_runtime_deserialize_progress_sized(&client->game->runtime, serialized, (uint32_t)serialized_size, NULL);
  }

  for (subset = client->game->subsets; subset; subset = subset->next)
    rc_client_subset_after_deserialize_progress(client->game, subset);

  rc_mutex_unlock(&client->state.mutex);

  rc_client_raise_pending_events(client, client->game);

  return result;
}
