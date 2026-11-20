#include "rc_client_load.h"

#include "rc_client_async.h"
#include "rc_client_do_frame.h"
#include "rc_client_game.h"
#include "rc_client_internal.h"
#include "rc_client_server.h"

#include "rc_api_request.h"
#include "rc_api_runtime.h"
#include "rc_api_user.h"
#include "rc_consoles.h"
#include "rc_error.h"
#include "rc_runtime.h"
#include "rc_util.h"

#ifdef RC_CLIENT_SUPPORTS_HASH
#include "rc_hash.h"
#include "../hash/rc_hash_internal.h"
#endif

#include "../runtime/rc_alloc.h"
#include "../runtime/rc_condition.h"
#include "../runtime/rc_condset.h"
#include "../runtime/rc_lboard.h"
#include "../runtime/rc_parse_state.h"
#include "../runtime/rc_trigger.h"
#include "../runtime/rc_value.h"

#include "../util/rc_compat.h"

static void rc_client_process_resolved_hash(rc_client_load_state_t* load_state);
static rc_client_async_handle_t* rc_client_load_game(rc_client_load_state_t* load_state, const char* hash, const char* file_path);

typedef struct rc_client_pending_media_t
{
#ifdef RC_CLIENT_SUPPORTS_HASH
  const char* file_path;
  uint8_t* data;
  size_t data_size;
#endif
  const char* hash;
  rc_client_callback_t callback;
  void* callback_userdata;
} rc_client_pending_media_t;

static void rc_client_free_game(rc_client_game_info_t* game)
{
  rc_runtime_destroy(&game->runtime);

  rc_buffer_destroy(&game->buffer);

  free(game);
}

static void rc_client_free_load_state(rc_client_load_state_t* load_state)
{
  if (load_state->game)
    rc_client_free_game(load_state->game);

  if (load_state->start_session_response) {
    rc_api_destroy_start_session_response(load_state->start_session_response);
    free(load_state->start_session_response);
  }

#ifdef RC_CLIENT_SUPPORTS_HASH
  rc_hash_destroy_iterator(&load_state->hash_iterator);
#endif

  free(load_state);
}

static void rc_client_begin_load_state(rc_client_load_state_t* load_state, uint8_t state, uint8_t num_requests)
{
  rc_mutex_lock(&load_state->client->state.mutex);

  load_state->progress = state;
  load_state->outstanding_requests += num_requests;

  rc_mutex_unlock(&load_state->client->state.mutex);
}

static int rc_client_end_load_state(rc_client_load_state_t* load_state)
{
  int remaining_requests = 0;
  int aborted = 0;

  rc_mutex_lock(&load_state->client->state.mutex);

  if (load_state->outstanding_requests > 0)
    --load_state->outstanding_requests;
  remaining_requests = load_state->outstanding_requests;

  if (load_state->client->state.load != load_state)
    aborted = 1;

  rc_mutex_unlock(&load_state->client->state.mutex);

  if (aborted) {
    /* we can't actually free the load_state itself if there are any outstanding requests
     * or their callbacks will try to use the free'd memory. As they call end_load_state,
     * the outstanding_requests count will reach zero and the memory will be free'd then. */
    if (remaining_requests == 0) {
      /* if one of the callbacks called rc_client_load_error, progress will be set to
       * RC_CLIENT_LOAD_STATE_ABORTED. There's no need to call the callback with RC_ABORTED
       * in that case, as it will have already been called with something more appropriate. */
      if (load_state->progress != RC_CLIENT_LOAD_GAME_STATE_ABORTED && load_state->callback)
        load_state->callback(RC_ABORTED, "The requested game is no longer active", load_state->client, load_state->callback_userdata);

      rc_client_free_load_state(load_state);
    }

    return -1;
  }

  return remaining_requests;
}

void rc_client_load_error(struct rc_client_load_state_t* load_state, int result, const char* error_message)
{
  int remaining_requests = 0;

  rc_mutex_lock(&load_state->client->state.mutex);

  load_state->progress = RC_CLIENT_LOAD_GAME_STATE_ABORTED;
  if (load_state->client->state.load == load_state)
    load_state->client->state.load = NULL;

  remaining_requests = load_state->outstanding_requests;

  rc_mutex_unlock(&load_state->client->state.mutex);

  RC_CLIENT_LOG_ERR_FORMATTED(load_state->client, "Load failed (%d): %s", result, error_message);

  if (load_state->callback)
    load_state->callback(result, error_message, load_state->client, load_state->callback_userdata);

  /* we can't actually free the load_state itself if there are any outstanding requests
   * or their callbacks will try to use the free'd memory. as they call end_load_state,
   * the outstanding_requests count will reach zero and the memory will be free'd then. */
  if (remaining_requests == 0)
    rc_client_free_load_state(load_state);
}

static void rc_client_load_aborted(rc_client_load_state_t* load_state)
{
  /* prevent callback from being called when manually aborted */
  load_state->callback = NULL;

  /* mark the game as no longer being loaded */
  rc_client_load_error(load_state, RC_ABORTED, NULL);

  /* decrement the async counter and potentially free the load_state object */
  rc_client_end_load_state(load_state);
}

static void rc_client_apply_unlocks(rc_client_subset_info_t* subset, rc_api_unlock_entry_t* unlocks, uint32_t num_unlocks, uint8_t mode)
{
  rc_client_achievement_info_t* start = subset->achievements;
  rc_client_achievement_info_t* stop = start + subset->public_.num_achievements;
  rc_client_achievement_info_t* scan;
  rc_api_unlock_entry_t* unlock = unlocks;
  rc_api_unlock_entry_t* unlock_stop = unlocks + num_unlocks;

  for (; unlock < unlock_stop; ++unlock) {
    for (scan = start; scan < stop; ++scan) {
      if (scan->public_.id == unlock->achievement_id) {
        scan->public_.unlocked |= mode;

        if (mode & RC_CLIENT_ACHIEVEMENT_UNLOCKED_HARDCORE)
          scan->unlock_time_hardcore = unlock->when;
        if (mode & RC_CLIENT_ACHIEVEMENT_UNLOCKED_CASUAL)
          scan->unlock_time_casual = unlock->when;

        if (scan == start)
          ++start;
        else if (scan + 1 == stop)
          --stop;
        break;
      }
    }
  }

  if (subset->next)
    rc_client_apply_unlocks(subset->next, unlocks, num_unlocks, mode);
}

static void rc_client_free_pending_media(rc_client_pending_media_t* pending_media)
{
  if (pending_media->hash)
    free((void*)pending_media->hash);
#ifdef RC_CLIENT_SUPPORTS_HASH
  if (pending_media->data)
    free(pending_media->data);
  free((void*)pending_media->file_path);
#endif
  free(pending_media);
}

static void rc_client_log_active_assets(rc_client_t* client)
{
  uint32_t num_achievements;
  uint32_t num_active_achievements;
  uint32_t num_unsupported_achievements;
  uint32_t num_leaderboards;
  uint32_t num_unsupported_leaderboards;
  const rc_client_achievement_info_t* ach;
  const rc_client_achievement_info_t* ach_stop;
  const rc_client_leaderboard_info_t* lbd;
  const rc_client_leaderboard_info_t* lbd_stop;

  const rc_client_subset_info_t* subset = client->game->subsets;
  for (; subset; subset = subset->next) {
    num_achievements = 0;
    num_active_achievements = 0;
    num_unsupported_achievements = 0;
    num_leaderboards = 0;
    num_unsupported_leaderboards = 0;

    ach = subset->achievements;
    ach_stop = ach + subset->public_.num_achievements;
    for (; ach < ach_stop; ++ach) {
      if (ach->public_.category == RC_CLIENT_ACHIEVEMENT_CATEGORY_PROMOTED) {
        ++num_achievements;
        if (ach->public_.state == RC_CLIENT_ACHIEVEMENT_STATE_ACTIVE)
          ++num_active_achievements;
        else if (ach->public_.state == RC_CLIENT_ACHIEVEMENT_STATE_DISABLED)
          ++num_unsupported_achievements;
      }
    }

    lbd = subset->leaderboards;
    lbd_stop = lbd + subset->public_.num_leaderboards;
    for (; lbd < lbd_stop; ++lbd) {
      ++num_leaderboards;
      if (lbd->public_.state == RC_CLIENT_LEADERBOARD_STATE_DISABLED)
        ++num_unsupported_leaderboards;
    }

    if (num_unsupported_achievements) {
      if (num_unsupported_leaderboards) {
        RC_CLIENT_LOG_INFO_FORMATTED(client, "Set %u: %u/%u achievements active (%u unsupported), %u leaderboards (%u unsupported)",
          subset->public_.id, num_active_achievements, num_achievements, num_unsupported_achievements, num_leaderboards, num_unsupported_leaderboards);
      }
      else {
        RC_CLIENT_LOG_INFO_FORMATTED(client, "Set %u: %u/%u achievements active (%u unsupported), %u leaderboards",
          subset->public_.id, num_active_achievements, num_achievements, num_unsupported_achievements, num_leaderboards);
      }
    }
    else if (num_unsupported_leaderboards) {
      RC_CLIENT_LOG_INFO_FORMATTED(client, "Set %u: %u/%u achievements active, %u leaderboards (%u unsupported)",
        subset->public_.id, num_active_achievements, num_achievements, num_leaderboards, num_unsupported_leaderboards);
    }
    else {
      RC_CLIENT_LOG_INFO_FORMATTED(client, "Set %u: %u/%u achievements active, %u leaderboards",
        subset->public_.id, num_active_achievements, num_achievements, num_leaderboards);
    }
  }
}

/* NOTE: address validation uses the read_memory callback to make sure the client
 *       will return data for the requested address. As such, this function must
 *       respect the `client->state.allow_background_memory_reads setting. Use
 *       rc_client_queue_activate_game to dispatch this function to the do_frame loop.
 */
static void rc_client_activate_game(rc_client_load_state_t* load_state, rc_api_start_session_response_t* start_session_response)
{
  rc_client_t* client = load_state->client;

  rc_mutex_lock(&client->state.mutex);
  load_state->progress = (client->state.load == load_state) ?
    RC_CLIENT_LOAD_GAME_STATE_DONE : RC_CLIENT_LOAD_GAME_STATE_ABORTED;
  rc_mutex_unlock(&client->state.mutex);

  if (load_state->progress != RC_CLIENT_LOAD_GAME_STATE_DONE) {
    /* previous load state was aborted */
    if (load_state->callback)
      load_state->callback(RC_ABORTED, "The requested game is no longer active", client, load_state->callback_userdata);
  }
  else if (!start_session_response && client->state.spectator_mode == RC_CLIENT_SPECTATOR_MODE_OFF) {
    /* unlocks not available - assume malloc failed */
    if (load_state->callback)
      load_state->callback(RC_INVALID_STATE, "Unlock arrays were not allocated", client, load_state->callback_userdata);
  }
  else {
    if (client->state.spectator_mode == RC_CLIENT_SPECTATOR_MODE_OFF) {
      rc_client_apply_unlocks(load_state->subset, start_session_response->hardcore_unlocks,
        start_session_response->num_hardcore_unlocks, RC_CLIENT_ACHIEVEMENT_UNLOCKED_BOTH);
      rc_client_apply_unlocks(load_state->subset, start_session_response->unlocks,
        start_session_response->num_unlocks, RC_CLIENT_ACHIEVEMENT_UNLOCKED_CASUAL);
    }

    /* make the loaded game active if another game is not aleady being loaded. */
    rc_mutex_lock(&client->state.mutex);
    if (client->state.load == load_state) {
      client->game = load_state->game;
      client->state.frames_processed = client->state.frames_at_last_ping = 0;
    }
    else {
      load_state->progress = RC_CLIENT_LOAD_GAME_STATE_ABORTED;
    }
    rc_mutex_unlock(&client->state.mutex);

    if (load_state->progress != RC_CLIENT_LOAD_GAME_STATE_ABORTED) {
      /* if a change media request is pending, kick it off */
      rc_client_pending_media_t* pending_media;

      rc_mutex_lock(&load_state->client->state.mutex);
      pending_media = load_state->pending_media;
      load_state->pending_media = NULL;
      rc_mutex_unlock(&load_state->client->state.mutex);

      if (pending_media) {
        /* rc_client_check_pending_media will fail if it can't find the game in client->game or
         * client->state.load->game. since we've detached the load_state, this has to occur after
         * we've made the game active. */
        if (pending_media->hash) {
          rc_client_begin_change_media(client, pending_media->hash,
            pending_media->callback, pending_media->callback_userdata);
        }
        else {
#ifdef RC_CLIENT_SUPPORTS_HASH
          rc_client_begin_identify_and_change_media(client, pending_media->file_path,
            pending_media->data, pending_media->data_size,
            pending_media->callback, pending_media->callback_userdata);
#endif
        }
        rc_client_free_pending_media(pending_media);
      }

      rc_mutex_lock(&client->state.mutex);
      if (client->state.load != load_state)
        load_state->progress = RC_CLIENT_LOAD_GAME_STATE_ABORTED;
      rc_mutex_unlock(&client->state.mutex);
    }

    /* if the game is still being loaded, make sure all the required memory addresses are accessible
     * so we can mark achievements as unsupported before loading them into the runtime. */
    if (load_state->progress != RC_CLIENT_LOAD_GAME_STATE_ABORTED) {
      /* ASSERT: client->game must be set before calling this function so the read_memory callback can query the console_id */
      rc_client_validate_addresses(load_state->game, client);

      rc_mutex_lock(&client->state.mutex);
      if (client->state.load != load_state)
        load_state->progress = RC_CLIENT_LOAD_GAME_STATE_ABORTED;
      rc_mutex_unlock(&client->state.mutex);
    }

    /* if the game is still being loaded, load any active acheivements/leaderboards into the runtime */
    if (load_state->progress != RC_CLIENT_LOAD_GAME_STATE_ABORTED) {
      rc_client_activate_achievements(load_state->game, client);
      rc_client_activate_leaderboards(load_state->game, client);

      /* detach the load state to indicate that loading is fully complete */
      rc_mutex_lock(&client->state.mutex);
      if (client->state.load == load_state)
        client->state.load = NULL;
      else
        load_state->progress = RC_CLIENT_LOAD_GAME_STATE_ABORTED;
      rc_mutex_unlock(&client->state.mutex);
    }

    /* one last sanity check to make sure the game is still being loaded. */
    if (load_state->progress == RC_CLIENT_LOAD_GAME_STATE_ABORTED) {
      /* game has been unloaded, or another game is being loaded over the top of this game */
      if (load_state->callback)
        load_state->callback(RC_ABORTED, "The requested game is no longer active", client, load_state->callback_userdata);
    }
    else {
      if (load_state->hash->hash[0] != '[') {
        if (client->state.spectator_mode != RC_CLIENT_SPECTATOR_MODE_LOCKED) {
          /* schedule the periodic ping */
          rc_client_schedule_ping(client, load_state->game);
        }

        RC_CLIENT_LOG_INFO_FORMATTED(client, "Game %u loaded, hardcore %s%s", load_state->game->public_.id,
          client->state.hardcore ? "enabled" : "disabled",
          (client->state.spectator_mode != RC_CLIENT_SPECTATOR_MODE_OFF) ? ", spectating" : "");

        if (client->state.log_level >= RC_CLIENT_LOG_LEVEL_INFO)
          rc_client_log_active_assets(client);
      }
      else {
        RC_CLIENT_LOG_INFO_FORMATTED(client, "Subset %u loaded", load_state->subset->public_.id);
      }

      if (load_state->callback)
        load_state->callback(RC_OK, NULL, client, load_state->callback_userdata);

      /* detach the game object so it doesn't get freed by free_load_state */
      load_state->game = NULL;
    }
  }

  rc_client_free_load_state(load_state);
}

static void rc_client_dispatch_activate_game(struct rc_client_scheduled_callback_data_t* callback_data, rc_client_t* client, rc_clock_t now)
{
  rc_client_load_state_t* load_state = (rc_client_load_state_t*)callback_data->data;
  free(callback_data);

  (void)client;
  (void)now;

  rc_client_activate_game(load_state, load_state->start_session_response);
}

static void rc_client_queue_activate_game(rc_client_load_state_t* load_state)
{
  rc_client_scheduled_callback_data_t* scheduled_callback_data =
      (rc_client_scheduled_callback_data_t*)calloc(1, sizeof(rc_client_scheduled_callback_data_t));

  if (!scheduled_callback_data) {
    rc_client_load_error(load_state, RC_OUT_OF_MEMORY, rc_error_str(RC_OUT_OF_MEMORY));
    return;
  }

  scheduled_callback_data->callback = rc_client_dispatch_activate_game;
  scheduled_callback_data->data = load_state;

  rc_client_schedule_callback(load_state->client, scheduled_callback_data);
}

static void rc_client_start_session_callback(const rc_api_server_response_t* server_response, void* callback_data)
{
  rc_client_load_state_t* load_state = (rc_client_load_state_t*)callback_data;
  rc_api_start_session_response_t start_session_response;
  int outstanding_requests;
  const char* error_message;
  int result;

  result = rc_client_end_async(load_state->client, &load_state->async_handle);
  if (result) {
    if (result != RC_CLIENT_ASYNC_DESTROYED) {
      rc_client_t* client = load_state->client;
      rc_client_load_aborted(load_state);
      RC_CLIENT_LOG_VERBOSE(client, "Load aborted while starting session");
    }
    else {
      rc_client_free_load_state(load_state);
    }
    return;
  }

  result = rc_api_process_start_session_server_response(&start_session_response, server_response);
  error_message = rc_client_server_error_message(&result, server_response->http_status_code, &start_session_response.response);
  outstanding_requests = rc_client_end_load_state(load_state);

  if (outstanding_requests < 0) {
    /* previous load state was aborted, load_state was free'd */
  }
  else if (error_message) {
    rc_client_load_error(load_state, result, error_message);
  }
  else if (outstanding_requests == 0 && load_state->client->state.allow_background_memory_reads) {
    rc_client_activate_game(load_state, &start_session_response);
  }
  else {
    load_state->start_session_response =
      (rc_api_start_session_response_t*)malloc(sizeof(rc_api_start_session_response_t));

    if (!load_state->start_session_response) {
      rc_client_load_error(load_state, RC_OUT_OF_MEMORY, rc_error_str(RC_OUT_OF_MEMORY));
    }
    else {
      /* safer to parse the response again than to try to copy it */
      rc_api_process_start_session_response(load_state->start_session_response, server_response->body);
    }

    if (outstanding_requests == 0) {
      if (load_state->client->state.allow_background_memory_reads)
        rc_client_activate_game(load_state, load_state->start_session_response);
      else
        rc_client_queue_activate_game(load_state);
    }
  }

  rc_api_destroy_start_session_response(&start_session_response);
}

static void rc_client_begin_start_session(rc_client_load_state_t* load_state)
{
  rc_api_start_session_request_t start_session_params;
  rc_client_t* client = load_state->client;
  rc_api_request_t start_session_request;
  int result;

  memset(&start_session_params, 0, sizeof(start_session_params));
  start_session_params.username = client->user.username;
  start_session_params.api_token = client->user.token;
  start_session_params.game_id = load_state->hash->game_id;
  start_session_params.game_hash = load_state->hash->hash;
  start_session_params.hardcore = client->state.hardcore;

  result = rc_api_init_start_session_request_hosted(&start_session_request, &start_session_params, &client->state.host);
  if (result != RC_OK) {
    rc_client_load_error(load_state, result, rc_error_str(result));
  }
  else {
    rc_client_begin_load_state(load_state, RC_CLIENT_LOAD_GAME_STATE_STARTING_SESSION, 1);
    RC_CLIENT_LOG_VERBOSE_FORMATTED(client, "Starting session for game %u", start_session_params.game_id);
    rc_client_begin_async(client, &load_state->async_handle);
    client->callbacks.server_call(&start_session_request, rc_client_start_session_callback, load_state, client);
    rc_api_destroy_request(&start_session_request);
  }
}

static void rc_client_copy_achievements(rc_client_load_state_t* load_state,
  rc_client_subset_info_t* subset,
  const rc_api_achievement_definition_t* achievement_definitions, uint32_t num_achievements)
{
  const rc_api_achievement_definition_t* read;
  const rc_api_achievement_definition_t* stop;
  rc_client_achievement_info_t* achievements;
  rc_client_achievement_info_t* achievement;
  rc_client_achievement_info_t* scan;
  rc_buffer_t* buffer;
  rc_preparse_state_t preparse;
  const char* memaddr;
  size_t size;
  rc_trigger_t* trigger;
  int trigger_size;

  subset->achievements = NULL;
  subset->public_.num_achievements = num_achievements;

  if (num_achievements == 0)
    return;

  stop = achievement_definitions + num_achievements;

  /* if not testing unpromoted, filter them out */
  if (!load_state->client->state.unpromoted_enabled) {
    for (read = achievement_definitions; read < stop; ++read) {
      if (read->category != RC_ACHIEVEMENT_CATEGORY_PROMOTED)
        --num_achievements;
    }

    subset->public_.num_achievements = num_achievements;

    if (num_achievements == 0)
      return;
  }

  /* preallocate space for achievements */
  size = 24 /* assume average title length of 24 */
    + 48 /* assume average description length of 48 */
    + sizeof(rc_trigger_t) + sizeof(rc_condset_t) * 2 /* trigger container */
    + sizeof(rc_condition_t) * 8 /* assume average trigger length of 8 conditions */
    + sizeof(rc_client_achievement_info_t);
  buffer = &load_state->game->buffer;
  rc_buffer_reserve(buffer, size * num_achievements);

  /* allocate the achievement array */
  size = sizeof(rc_client_achievement_info_t) * num_achievements;
  achievement = achievements = (rc_client_achievement_info_t*)rc_buffer_alloc(buffer, size);
  memset(achievements, 0, size);

  /* copy the achievement data */
  for (read = achievement_definitions; read < stop; ++read) {
    if (read->category != RC_ACHIEVEMENT_CATEGORY_PROMOTED && !load_state->client->state.unpromoted_enabled)
      continue;

    achievement->public_.title = rc_buffer_strcpy(buffer, read->title);
    achievement->public_.description = rc_buffer_strcpy(buffer, read->description);
    snprintf(achievement->public_.badge_name, sizeof(achievement->public_.badge_name), "%s", read->badge_name);
    achievement->public_.id = read->id;
    achievement->public_.points = read->points;
    achievement->public_.category = (read->category != RC_ACHIEVEMENT_CATEGORY_PROMOTED) ?
      RC_CLIENT_ACHIEVEMENT_CATEGORY_UNPROMOTED : RC_CLIENT_ACHIEVEMENT_CATEGORY_PROMOTED;
    achievement->public_.rarity = read->rarity;
    achievement->public_.rarity_hardcore = read->rarity_hardcore;
    achievement->public_.type = (uint8_t)read->type; /* assert: mapping is 1:1 */
    achievement->public_.badge_url = rc_buffer_strcpy(buffer, read->badge_url);
    achievement->public_.badge_locked_url = rc_buffer_strcpy(buffer, read->badge_locked_url);

    memaddr = read->definition;
    rc_runtime_checksum(memaddr, achievement->md5);

    rc_init_preparse_state(&preparse);
    preparse.parse.existing_memrefs = load_state->game->runtime.memrefs;
    trigger = RC_ALLOC(rc_trigger_t, &preparse.parse);
    rc_parse_trigger_internal(trigger, &memaddr, &preparse.parse);

    trigger_size = preparse.parse.offset;
    if (trigger_size < 0) {
      RC_CLIENT_LOG_WARN_FORMATTED(load_state->client, "Parse error %d processing achievement %u", trigger_size, read->id);
      achievement->public_.state = RC_CLIENT_ACHIEVEMENT_STATE_DISABLED;
      achievement->public_.bucket = RC_CLIENT_ACHIEVEMENT_BUCKET_UNSUPPORTED;
    }
    else {
      /* populate the item, using the communal memrefs pool */
      rc_reset_parse_state(&preparse.parse, rc_buffer_reserve(buffer, trigger_size), trigger_size);
      rc_preparse_reserve_memrefs(&preparse, load_state->game->runtime.memrefs);
      achievement->trigger = RC_ALLOC(rc_trigger_t, &preparse.parse);
      memaddr = read->definition;
      rc_parse_trigger_internal(achievement->trigger, &memaddr, &preparse.parse);

      if (preparse.parse.offset < 0) {
        RC_CLIENT_LOG_WARN_FORMATTED(load_state->client, "Parse error %d processing achievement %u", preparse.parse.offset, read->id);
        achievement->public_.state = RC_CLIENT_ACHIEVEMENT_STATE_DISABLED;
        achievement->public_.bucket = RC_CLIENT_ACHIEVEMENT_BUCKET_UNSUPPORTED;
      }
      else {
        rc_buffer_consume(buffer, (const uint8_t*)preparse.parse.buffer, (uint8_t*)preparse.parse.buffer + preparse.parse.offset);
      }
    }

    rc_destroy_preparse_state(&preparse);

    achievement->created_time = read->created;
    achievement->updated_time = read->updated;

    /* Attempt to share the author string instead of duplicating it. */
    scan = achievement;
    while (scan > achievements) {
      --scan;
      if (strcmp(scan->author, read->author) == 0) {
        achievement->author = scan->author;
        break;
      }
    }
    if (!achievement->author)
      achievement->author = rc_buffer_strcpy(buffer, read->author);

    ++achievement;
  }

  subset->achievements = achievements;
}

static void rc_client_copy_leaderboards(rc_client_load_state_t* load_state,
  rc_client_subset_info_t* subset,
  const rc_api_leaderboard_definition_t* leaderboard_definitions, uint32_t num_leaderboards)
{
  const rc_api_leaderboard_definition_t* read;
  const rc_api_leaderboard_definition_t* stop;
  rc_client_leaderboard_info_t* leaderboards;
  rc_client_leaderboard_info_t* leaderboard;
  rc_buffer_t* buffer;
  rc_preparse_state_t preparse;
  const char* memaddr;
  const char* ptr;
  size_t size;
  rc_lboard_t* lboard;
  int lboard_size;

  subset->leaderboards = NULL;
  subset->public_.num_leaderboards = num_leaderboards;

  if (num_leaderboards == 0)
    return;

  /* preallocate space for achievements */
  size = 24 /* assume average title length of 24 */
    + 48 /* assume average description length of 48 */
    + sizeof(rc_lboard_t) /* lboard container */
    + (sizeof(rc_trigger_t) + sizeof(rc_condset_t) * 2) * 3 /* start/submit/cancel */
    + (sizeof(rc_value_t) + sizeof(rc_condset_t)) /* value */
    + sizeof(rc_condition_t) * 4 * 4 /* assume average of 4 conditions in each start/submit/cancel/value */
    + sizeof(rc_client_leaderboard_info_t);
  rc_buffer_reserve(&load_state->game->buffer, size * num_leaderboards);

  /* allocate the achievement array */
  size = sizeof(rc_client_leaderboard_info_t) * num_leaderboards;
  buffer = &load_state->game->buffer;
  leaderboard = leaderboards = (rc_client_leaderboard_info_t*)rc_buffer_alloc(buffer, size);
  memset(leaderboards, 0, size);

  /* copy the achievement data */
  read = leaderboard_definitions;
  stop = read + num_leaderboards;
  do {
    leaderboard->public_.title = rc_buffer_strcpy(buffer, read->title);
    leaderboard->public_.description = rc_buffer_strcpy(buffer, read->description);
    leaderboard->public_.id = read->id;
    leaderboard->public_.format = rc_client_map_leaderboard_format(read->format);
    leaderboard->public_.lower_is_better = read->lower_is_better;
    leaderboard->public_.category = (read->state == RC_LEADERBOARD_STATE_ACTIVE)
      ? RC_CLIENT_LEADERBOARD_CATEGORY_PROMOTED : RC_CLIENT_LEADERBOARD_CATEGORY_UNPROMOTED;
    leaderboard->format = (uint8_t)read->format;
    leaderboard->hidden = (uint8_t)read->hidden;

    memaddr = read->definition;
    rc_runtime_checksum(memaddr, leaderboard->md5);

    ptr = strstr(memaddr, "VAL:");
    if (ptr != NULL) {
      /* Calculate the DJB2 hash of the VAL portion of the string */
      uint32_t hash = 5381;
      ptr += 4; /* skip 'VAL:' */
      while (*ptr && (ptr[0] != ':' || ptr[1] != ':'))
        hash = (hash << 5) + hash + *ptr++;
      leaderboard->value_djb2 = hash;
    }

    rc_init_preparse_state(&preparse);
    preparse.parse.existing_memrefs = load_state->game->runtime.memrefs;
    lboard = RC_ALLOC(rc_lboard_t, &preparse.parse);
    rc_parse_lboard_internal(lboard, memaddr, &preparse.parse);

    lboard_size = preparse.parse.offset;
    if (lboard_size < 0) {
      RC_CLIENT_LOG_WARN_FORMATTED(load_state->client, "Parse error %d processing leaderboard %u", lboard_size, read->id);
      leaderboard->public_.state = RC_CLIENT_LEADERBOARD_STATE_DISABLED;
    }
    else {
      /* populate the item, using the communal memrefs pool */
      rc_reset_parse_state(&preparse.parse, rc_buffer_reserve(buffer, lboard_size), lboard_size);
      rc_preparse_reserve_memrefs(&preparse, load_state->game->runtime.memrefs);
      leaderboard->lboard = RC_ALLOC(rc_lboard_t, &preparse.parse);
      rc_parse_lboard_internal(leaderboard->lboard, memaddr, &preparse.parse);

      if (preparse.parse.offset < 0) {
        RC_CLIENT_LOG_WARN_FORMATTED(load_state->client, "Parse error %d processing leaderboard %u", preparse.parse.offset, read->id);
        leaderboard->public_.state = RC_CLIENT_LEADERBOARD_STATE_DISABLED;
      }
      else {
        rc_buffer_consume(buffer, (const uint8_t*)preparse.parse.buffer, (uint8_t*)preparse.parse.buffer + preparse.parse.offset);
      }

      rc_destroy_preparse_state(&preparse);
    }

    ++leaderboard;
    ++read;
  } while (read < stop);

  subset->leaderboards = leaderboards;
}

static void rc_client_fetch_game_sets_callback(const rc_api_server_response_t* server_response, void* callback_data)
{
  rc_client_load_state_t* load_state = (rc_client_load_state_t*)callback_data;
  rc_api_fetch_game_sets_response_t fetch_game_sets_response;
  int outstanding_requests;
  const char* error_message;
  int result;

  result = rc_client_end_async(load_state->client, &load_state->async_handle);
  if (result) {
    if (result != RC_CLIENT_ASYNC_DESTROYED) {
      rc_client_t* client = load_state->client;
      rc_client_load_aborted(load_state);
      RC_CLIENT_LOG_VERBOSE(client, "Load aborted while fetching game data");
    }
    else {
      rc_client_free_load_state(load_state);
    }
    return;
  }

  result = rc_api_process_fetch_game_sets_server_response(&fetch_game_sets_response, server_response);
  error_message = rc_client_server_error_message(&result, server_response->http_status_code, &fetch_game_sets_response.response);

  outstanding_requests = rc_client_end_load_state(load_state);

  if (error_message && result != RC_NOT_FOUND) {
    rc_client_load_error(load_state, result, error_message);
  }
  else if (outstanding_requests < 0) {
    /* previous load state was aborted, load_state was free'd */
  }
  else if (fetch_game_sets_response.id == 0) {
    load_state->hash->game_id = 0;
    rc_client_process_resolved_hash(load_state);
  }
  else {
    rc_client_subset_info_t** next_subset;
    rc_client_subset_info_t* first_subset = NULL;
    uint32_t set_index;

    /* hash exists outside the load state - always update it */
    load_state->hash->game_id = fetch_game_sets_response.id;
    RC_CLIENT_LOG_INFO_FORMATTED(load_state->client, "Identified game: %u \"%s\" (%s)", load_state->hash->game_id, fetch_game_sets_response.title, load_state->hash->hash);

    if (load_state->hash->hash[0] != '[') {
      /* not [NO HASH] or [SUBSETxx] */
      load_state->game->public_.id = load_state->hash->game_id;
      load_state->game->public_.hash = load_state->hash->hash;
    }

    if (load_state->game->public_.console_id != RC_CONSOLE_UNKNOWN &&
      fetch_game_sets_response.console_id != load_state->game->public_.console_id) {
      RC_CLIENT_LOG_WARN_FORMATTED(load_state->client, "Data for game %u is for console %u, expecting console %u",
        fetch_game_sets_response.id, fetch_game_sets_response.console_id, load_state->game->public_.console_id);
    }

    /* kick off the start session request while we process the game data */
    rc_client_begin_load_state(load_state, RC_CLIENT_LOAD_GAME_STATE_STARTING_SESSION, 1);
    if (load_state->client->state.spectator_mode != RC_CLIENT_SPECTATOR_MODE_OFF) {
      /* we can't unlock achievements without a session, lock spectator mode for the game */
      load_state->client->state.spectator_mode = RC_CLIENT_SPECTATOR_MODE_LOCKED;
    }
    else {
      rc_client_begin_start_session(load_state);
    }

    /* process the game data */
    next_subset = &first_subset;
    for (set_index = 0; set_index < fetch_game_sets_response.num_sets; ++set_index) {
      rc_api_achievement_set_definition_t* set = &fetch_game_sets_response.sets[set_index];
      rc_client_subset_info_t* subset;

      subset = (rc_client_subset_info_t*)rc_buffer_alloc(&load_state->game->buffer, sizeof(rc_client_subset_info_t));
      memset(subset, 0, sizeof(*subset));
      subset->public_.id = set->id;
      subset->active = 1;
      snprintf(subset->public_.badge_name, sizeof(subset->public_.badge_name), "%s", set->image_name);
      subset->public_.badge_url = rc_buffer_strcpy(&load_state->game->buffer, set->image_url);
      subset->public_.title = rc_buffer_strcpy(&load_state->game->buffer, set->title);

      rc_client_copy_achievements(load_state, subset, set->achievements, set->num_achievements);
      rc_client_copy_leaderboards(load_state, subset, set->leaderboards, set->num_leaderboards);

      if (set->type == RC_ACHIEVEMENT_SET_TYPE_CORE) {
        if (!first_subset)
          next_subset = &subset->next;
        subset->next = first_subset;
        first_subset = subset;
      }
      else {
        *next_subset = subset;
        next_subset = &subset->next;
      }
    }

    if (!first_subset) {
      rc_client_load_error(load_state, RC_NOT_FOUND, "Response contained no sets");
    }
    else {
      load_state->subset = first_subset;

      /* core set */
      rc_mutex_lock(&load_state->client->state.mutex);
      load_state->game->public_.title = rc_buffer_strcpy(&load_state->game->buffer, fetch_game_sets_response.title);
      load_state->game->subsets = first_subset;
      load_state->game->public_.badge_name = first_subset->public_.badge_name;
      load_state->game->public_.badge_url = first_subset->public_.badge_url;
      load_state->game->public_.console_id = fetch_game_sets_response.console_id;
      rc_mutex_unlock(&load_state->client->state.mutex);

      if (fetch_game_sets_response.rich_presence_script && fetch_game_sets_response.rich_presence_script[0]) {
        result = rc_runtime_activate_richpresence(&load_state->game->runtime, fetch_game_sets_response.rich_presence_script, NULL, 0);
        if (result != RC_OK) {
          RC_CLIENT_LOG_WARN_FORMATTED(load_state->client, "Parse error %d processing rich presence", result);
        }
      }

      if (load_state->client->callbacks.post_process_game_sets_response) {
        load_state->client->callbacks.post_process_game_sets_response(server_response,
          &fetch_game_sets_response, load_state->client, load_state->callback_userdata);
      }
    }

    outstanding_requests = rc_client_end_load_state(load_state);
    if (outstanding_requests < 0) {
      /* previous load state was aborted, load_state was free'd */
    }
    else if (outstanding_requests == 0) {
      if (load_state->client->state.allow_background_memory_reads)
        rc_client_activate_game(load_state, load_state->start_session_response);
      else
        rc_client_queue_activate_game(load_state);
    }
  }

  rc_api_destroy_fetch_game_sets_response(&fetch_game_sets_response);
}

static rc_client_game_info_t* rc_client_allocate_game(void)
{
  rc_client_game_info_t* game = (rc_client_game_info_t*)calloc(1, sizeof(*game));
  if (!game)
    return NULL;

  rc_buffer_init(&game->buffer);
  rc_runtime_init(&game->runtime);

  return game;
}

static int rc_client_attach_load_state(rc_client_t* client, rc_client_load_state_t* load_state)
{
  if (client->state.load == NULL) {
    rc_client_unload_game(client);

    if (load_state->game == NULL) {
      load_state->game = rc_client_allocate_game();
      if (!load_state->game) {
        if (load_state->callback)
          load_state->callback(RC_OUT_OF_MEMORY, rc_error_str(RC_OUT_OF_MEMORY), client, load_state->callback_userdata);

        return 0;
      }
    }

    rc_mutex_lock(&client->state.mutex);
    client->state.load = load_state;
    client->state.frames_processed = client->state.frames_at_last_ping = 0;
    rc_mutex_unlock(&client->state.mutex);
  }
  else if (client->state.load != load_state) {
    /* previous load was aborted */
    if (load_state->callback)
      load_state->callback(RC_ABORTED, "The requested game is no longer active", client, load_state->callback_userdata);

    return 0;
  }

  return 1;
}

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL

static void rc_client_external_load_state_callback(int result, const char* error_message, rc_client_t* client, void* userdata)
{
  rc_client_load_state_t* load_state = (rc_client_load_state_t*)userdata;
  int async_aborted;

  client = load_state->client;
  async_aborted = rc_client_end_async(client, &load_state->async_handle);
  if (async_aborted) {
    if (async_aborted != RC_CLIENT_ASYNC_DESTROYED) {
      RC_CLIENT_LOG_VERBOSE(client, "Load aborted during external loading");
    }

    rc_client_unload_game(client); /* unload the game from the external client */
    rc_client_free_load_state(load_state);
    return;
  }

  if (result != RC_OK) {
    rc_client_load_error(load_state, result, error_message);
    return;
  }

  rc_mutex_lock(&client->state.mutex);
  load_state->progress = (client->state.load == load_state) ?
    RC_CLIENT_LOAD_GAME_STATE_DONE : RC_CLIENT_LOAD_GAME_STATE_ABORTED;
  client->state.load = NULL;
  rc_mutex_unlock(&client->state.mutex);

  if (load_state->progress != RC_CLIENT_LOAD_GAME_STATE_DONE) {
    /* previous load state was aborted */
    if (load_state->callback)
      load_state->callback(RC_ABORTED, "The requested game is no longer active", client, load_state->callback_userdata);
  }
  else {
    /* keep partial game object for media_hash management */
    if (client->state.external_client) {
      const rc_client_game_t* info = rc_client_get_game_info(client);
      load_state->game->public_.console_id = info->console_id;
      client->game = load_state->game;
      load_state->game = NULL;
    }

    if (load_state->callback)
      load_state->callback(RC_OK, NULL, client, load_state->callback_userdata);
  }

  rc_client_free_load_state(load_state);
}

#endif

static void rc_client_initialize_unknown_game(rc_client_game_info_t* game)
{
  rc_client_subset_info_t* subset;
  char buffer[64];

  subset = (rc_client_subset_info_t*)rc_buffer_alloc(&game->buffer, sizeof(rc_client_subset_info_t));
  memset(subset, 0, sizeof(*subset));
  subset->public_.title = "";
  game->subsets = subset;

  game->public_.title = "Unknown Game";
  game->public_.badge_name = "";

  rc_client_get_image_url(buffer, sizeof(buffer), RC_IMAGE_TYPE_GAME, "000001");
  game->public_.badge_url = rc_buffer_strcpy(&game->buffer, buffer);
}

static void rc_client_process_resolved_hash(rc_client_load_state_t* load_state)
{
  rc_client_t* client = load_state->client;

  if (load_state->hash->game_id == 0) {
#ifdef RC_CLIENT_SUPPORTS_HASH
    char hash[33];

    if (rc_hash_iterate(hash, &load_state->hash_iterator)) {
      /* found another hash to try */
      load_state->hash_console_id = load_state->hash_iterator.consoles[load_state->hash_iterator.index - 1];
      rc_client_load_game(load_state, hash, NULL);
      return;
    }

    if (load_state->tried_hashes[1]) {
      /* multiple hashes were tried, create a CSV */
      size_t i;
      size_t count = 0;
      char* ptr;
      size_t size = 0;

      for (i = 0; i < sizeof(load_state->tried_hashes) / sizeof(load_state->tried_hashes[0]); ++i) {
        if (!load_state->tried_hashes[i])
          break;

        size += strlen(load_state->tried_hashes[i]->hash) + 1;
        count++;
      }

      ptr = (char*)rc_buffer_alloc(&load_state->game->buffer, size);
      load_state->game->public_.hash = ptr;
      for (i = 0; i < count; i++) {
        const size_t hash_len = strlen(load_state->tried_hashes[i]->hash);
        memcpy(ptr, load_state->tried_hashes[i]->hash, hash_len);
        ptr += hash_len;
        *ptr++ = ',';
      }
      *(ptr - 1) = '\0';

      load_state->game->public_.console_id = RC_CONSOLE_UNKNOWN;
    }
    else {
      /* only a single hash was tried, capture it */
      load_state->game->public_.console_id = load_state->hash_console_id;
      load_state->game->public_.hash = load_state->hash->hash;

      if (client->callbacks.identify_unknown_hash) {
        load_state->hash->game_id = client->callbacks.identify_unknown_hash(
          load_state->hash_console_id, load_state->hash->hash, client, load_state->callback_userdata);

        if (load_state->hash->game_id != 0) {
          load_state->hash->is_unknown = 1;
          RC_CLIENT_LOG_INFO_FORMATTED(load_state->client, "Client says to load game %u for unidentified hash %s",
            load_state->hash->game_id, load_state->hash->hash);
        }
      }
    }

    rc_hash_destroy_iterator(&load_state->hash_iterator); /* done with this now */
#else
    load_state->game->public_.console_id = RC_CONSOLE_UNKNOWN;
    load_state->game->public_.hash = load_state->hash->hash;
#endif /* RC_CLIENT_SUPPORTS_HASH */

    if (load_state->hash->game_id == 0) {
#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
      if (client->state.raintegration && client->state.raintegration->set_console_id) {
        if (load_state->game->public_.console_id != RC_CONSOLE_UNKNOWN)
          client->state.raintegration->set_console_id(load_state->game->public_.console_id);
      }
#endif
      if (client->state.external_client) {
        if (client->state.external_client->load_unknown_game) {
          client->state.external_client->load_unknown_game(load_state->game->public_.hash);
          rc_client_load_error(load_state, RC_NO_GAME_LOADED, "Unknown game");
          return;
        }
        /* no external method specifically for unknown game, just pass the hash through to begin_load_game below */
      }
      else {
#endif
        rc_client_initialize_unknown_game(load_state->game);

        client->game = load_state->game;
        load_state->game = NULL;

        rc_client_load_error(load_state, RC_NO_GAME_LOADED, "Unknown game");
        return;
#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
      }
#endif
    }
  }

  if (load_state->hash->hash[0] != '[') { /* not [NO HASH] or [SUBSETxx] */
    load_state->game->public_.id = load_state->hash->game_id;
    load_state->game->public_.hash = load_state->hash->hash;
  }

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client) {
    if (client->state.external_client->add_game_hash)
      client->state.external_client->add_game_hash(load_state->hash->hash, load_state->hash->game_id);

    if (client->state.external_client->begin_load_game) {
      rc_client_begin_async(client, &load_state->async_handle);
      client->state.external_client->begin_load_game(client, load_state->hash->hash, rc_client_external_load_state_callback, load_state);
    }
    return;
  }
#endif

  rc_client_begin_fetch_game_sets(load_state);
}

void rc_client_load_unknown_game(rc_client_t* client, const char* tried_hashes)
{
  rc_client_game_info_t* game;

  game = rc_client_allocate_game();
  if (!game)
    return;

  rc_client_initialize_unknown_game(game);
  game->public_.console_id = RC_CONSOLE_UNKNOWN;

  if (strlen(tried_hashes) == 32) { /* only one hash tried, add it to the list */
    rc_client_game_hash_t* game_hash = rc_client_find_game_hash(client, tried_hashes);
    game_hash->game_id = 0;
    game->public_.hash = game_hash->hash;
  }
  else {
    game->public_.hash = rc_buffer_strcpy(&game->buffer, tried_hashes);
  }

  rc_client_unload_game(client);
  client->game = game;
}

void rc_client_begin_fetch_game_sets(rc_client_load_state_t* load_state)
{
  rc_api_fetch_game_sets_request_t fetch_game_sets_request;
  rc_client_t* client = load_state->client;
  rc_api_request_t request;
  int result;

  rc_mutex_lock(&client->state.mutex);
  result = client->state.user;
  if (result == RC_CLIENT_USER_STATE_LOGIN_REQUESTED)
    load_state->progress = RC_CLIENT_LOAD_GAME_STATE_AWAIT_LOGIN;
  else
    load_state->progress = RC_CLIENT_LOAD_GAME_STATE_FETCHING_GAME_DATA;
  rc_mutex_unlock(&client->state.mutex);

  switch (result) {
    case RC_CLIENT_USER_STATE_LOGGED_IN:
      break;

    case RC_CLIENT_USER_STATE_LOGIN_REQUESTED:
      /* do nothing, this function will be called again after login completes */
      return;

    default:
      rc_client_load_error(load_state, RC_LOGIN_REQUIRED, rc_error_str(RC_LOGIN_REQUIRED));
      return;
  }

  memset(&fetch_game_sets_request, 0, sizeof(fetch_game_sets_request));
  fetch_game_sets_request.username = client->user.username;
  fetch_game_sets_request.api_token = client->user.token;

  if (load_state->hash->is_unknown) /* lookup failed, but client provided a mapping */
    fetch_game_sets_request.game_id = load_state->hash->game_id;
  else
    fetch_game_sets_request.game_hash = load_state->hash->hash;

  result = rc_api_init_fetch_game_sets_request_hosted(&request, &fetch_game_sets_request, &client->state.host);
  if (result != RC_OK) {
    rc_client_load_error(load_state, result, rc_error_str(result));
    return;
  }

  rc_client_begin_load_state(load_state, RC_CLIENT_LOAD_GAME_STATE_IDENTIFYING_GAME, 1);
  RC_CLIENT_LOG_VERBOSE_FORMATTED(client, "Fetching data for hash %s", fetch_game_sets_request.game_hash);

  rc_client_begin_async(client, &load_state->async_handle);
  client->callbacks.server_call(&request, rc_client_fetch_game_sets_callback, load_state, client);

  rc_api_destroy_request(&request);
}

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
static void rc_client_identify_game_callback(const rc_api_server_response_t* server_response, void* callback_data)
{
  rc_client_load_state_t* load_state = (rc_client_load_state_t*)callback_data;
  rc_client_t* client = load_state->client;
  rc_api_resolve_hash_response_t resolve_hash_response;
  int outstanding_requests;
  const char* error_message;
  int result;

  result = rc_client_end_async(client, &load_state->async_handle);
  if (result) {
    if (result != RC_CLIENT_ASYNC_DESTROYED) {
      rc_client_load_aborted(load_state);
      RC_CLIENT_LOG_VERBOSE(client, "Load aborted during game identification");
    }
    else {
      rc_client_free_load_state(load_state);
    }
    return;
  }

  result = rc_api_process_resolve_hash_server_response(&resolve_hash_response, server_response);
  error_message = rc_client_server_error_message(&result, server_response->http_status_code, &resolve_hash_response.response);

  if (error_message) {
    rc_client_end_load_state(load_state);
    rc_client_load_error(load_state, result, error_message);
  }
  else {
    /* hash exists outside the load state - always update it */
    load_state->hash->game_id = resolve_hash_response.game_id;
    RC_CLIENT_LOG_INFO_FORMATTED(client, "Identified game: %u (%s)", load_state->hash->game_id, load_state->hash->hash);

    /* have to call end_load_state after updating hash in case the load_state gets free'd */
    outstanding_requests = rc_client_end_load_state(load_state);
    if (outstanding_requests < 0) {
      /* previous load state was aborted, load_state was free'd */
    }
    else {
      rc_client_process_resolved_hash(load_state);
    }
  }

  rc_api_destroy_resolve_hash_response(&resolve_hash_response);
}
#endif

static rc_client_async_handle_t* rc_client_load_game(rc_client_load_state_t* load_state,
  const char* hash, const char* file_path)
{
  rc_client_t* client = load_state->client;
  rc_client_game_hash_t* old_hash;
#ifdef RC_CLIENT_SUPPORTS_HASH
  size_t i;
#endif

  if (!rc_client_attach_load_state(client, load_state)) {
    rc_client_free_load_state(load_state);
    return NULL;
  }

  old_hash = load_state->hash;
  load_state->hash = rc_client_find_game_hash(client, hash);

#ifdef RC_CLIENT_SUPPORTS_HASH
  i = 0;
  do {
    if (!load_state->tried_hashes[i]) {
      load_state->tried_hashes[i] = load_state->hash;
      break;
    }

    if (load_state->tried_hashes[i] == load_state->hash)
      break;

    if (++i == sizeof(load_state->tried_hashes) / sizeof(load_state->tried_hashes[0])) {
      RC_CLIENT_LOG_VERBOSE(client, "tried_hashes buffer is full");
      break;
    }
  } while (1);
#endif

  if (file_path) {
    rc_client_media_hash_t* media_hash =
      (rc_client_media_hash_t*)rc_buffer_alloc(&load_state->game->buffer, sizeof(*media_hash));
    media_hash->game_hash = load_state->hash;
    media_hash->path_djb2 = rc_djb2(file_path);
    media_hash->next = load_state->game->media_hash;
    load_state->game->media_hash = media_hash;
  }
  else if (load_state->game->media_hash && load_state->game->media_hash->game_hash == old_hash) {
    load_state->game->media_hash->game_hash = load_state->hash;
  }

  if (load_state->hash->game_id == 0) {
    rc_client_process_resolved_hash(load_state);
  }
#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  else if (load_state->hash->game_id == RC_CLIENT_UNKNOWN_GAME_ID &&
    client->state.external_client && client->state.external_client->add_game_hash) {
    /* if an add_game_hash external handler exists, do the identification locally, then
     * pass the resulting game_id/hash to the external client */
    rc_api_resolve_hash_request_t resolve_hash_request;
    rc_api_request_t request;
    int result;

    memset(&resolve_hash_request, 0, sizeof(resolve_hash_request));
    resolve_hash_request.game_hash = hash;

    result = rc_api_init_resolve_hash_request_hosted(&request, &resolve_hash_request, &client->state.host);
    if (result != RC_OK) {
      rc_client_load_error(load_state, result, rc_error_str(result));
      return NULL;
    }

    rc_client_begin_load_state(load_state, RC_CLIENT_LOAD_GAME_STATE_IDENTIFYING_GAME, 1);

    rc_client_begin_async(client, &load_state->async_handle);
    client->callbacks.server_call(&request, rc_client_identify_game_callback, load_state, client);

    rc_api_destroy_request(&request);
  }
  else if (load_state->hash->game_id != RC_CLIENT_UNKNOWN_GAME_ID &&
    client->state.external_client && client->state.external_client->begin_load_game) {
    rc_client_begin_async(client, &load_state->async_handle);
    client->state.external_client->begin_load_game(client, load_state->hash->hash, rc_client_external_load_state_callback, load_state);
  }
#endif
  else {
    rc_client_begin_fetch_game_sets(load_state);
  }

  return (client->state.load == load_state) ? &load_state->async_handle : NULL;
}

static void rc_client_abort_load_in_progress(rc_client_t* client)
{
  rc_client_load_state_t* load_state;

  rc_mutex_lock(&client->state.mutex);

  load_state = client->state.load;
  if (load_state) {
    /* this mimics rc_client_abort_async without nesting the lock */
    load_state->async_handle.aborted = RC_CLIENT_ASYNC_ABORTED;

    client->state.load = NULL;
  }

  rc_mutex_unlock(&client->state.mutex);

  if (load_state && load_state->callback)
    load_state->callback(RC_ABORTED, "The requested game is no longer active", load_state->client, load_state->callback_userdata);
}

rc_client_async_handle_t* rc_client_begin_load_game(rc_client_t* client, const char* hash, rc_client_callback_t callback, void* callback_userdata)
{
  rc_client_load_state_t* load_state;

  if (!client) {
    callback(RC_INVALID_STATE, "client is required", client, callback_userdata);
    return NULL;
  }

  if (!hash || !hash[0]) {
    callback(RC_INVALID_STATE, "hash is required", client, callback_userdata);
    return NULL;
  }

  rc_client_abort_load_in_progress(client);

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client && client->state.external_client->begin_load_game)
    return client->state.external_client->begin_load_game(client, hash, callback, callback_userdata);
#endif

  load_state = (rc_client_load_state_t*)calloc(1, sizeof(*load_state));
  if (!load_state) {
    callback(RC_OUT_OF_MEMORY, rc_error_str(RC_OUT_OF_MEMORY), client, callback_userdata);
    return NULL;
  }

  load_state->client = client;
  load_state->callback = callback;
  load_state->callback_userdata = callback_userdata;

  return rc_client_load_game(load_state, hash, NULL);
}

#ifdef RC_CLIENT_SUPPORTS_HASH

static void rc_client_log_hash_message_verbose(const char* message, const rc_hash_iterator_t* iterator)
{
  const rc_client_load_state_t* load_state = (const rc_client_load_state_t*)iterator->userdata;
  if (load_state->client->state.log_level >= RC_CLIENT_LOG_LEVEL_INFO)
    rc_client_log_message(load_state->client, message);
}

static void rc_client_log_hash_message_error(const char* message, const rc_hash_iterator_t* iterator)
{
  const rc_client_load_state_t* load_state = (const rc_client_load_state_t*)iterator->userdata;
  if (load_state->client->state.log_level >= RC_CLIENT_LOG_LEVEL_ERROR)
    rc_client_log_message(load_state->client, message);
}

void rc_client_set_hash_callbacks(rc_client_t* client, const struct rc_hash_callbacks* callbacks)
{
  memcpy(&client->callbacks.hash, callbacks, sizeof(*callbacks));

  if (!callbacks->verbose_message)
    client->callbacks.hash.verbose_message = rc_client_log_hash_message_verbose;
  if (!callbacks->error_message)
    client->callbacks.hash.error_message = rc_client_log_hash_message_error;
}

rc_client_async_handle_t* rc_client_begin_identify_and_load_game(rc_client_t* client,
  uint32_t console_id, const char* file_path,
  const uint8_t* data, size_t data_size,
  rc_client_callback_t callback, void* callback_userdata)
{
  rc_client_load_state_t* load_state;
  char hash[33];

  if (!client) {
    callback(RC_INVALID_STATE, "client is required", client, callback_userdata);
    return NULL;
  }

  rc_client_abort_load_in_progress(client);

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  /* if a add_game_hash handler exists, do the identification locally, then pass the
   * resulting game_id/hash to the external client */
  if (client->state.external_client && !client->state.external_client->add_game_hash) {
    if (client->state.external_client->begin_identify_and_load_game)
      return client->state.external_client->begin_identify_and_load_game(client, console_id, file_path, data, data_size, callback, callback_userdata);
  }
#endif

  if (data) {
    if (file_path) {
      RC_CLIENT_LOG_INFO_FORMATTED(client, "Identifying game: %zu bytes at %p (%s)", data_size, data, file_path);
    }
    else {
      RC_CLIENT_LOG_INFO_FORMATTED(client, "Identifying game: %zu bytes at %p", data_size, data);
    }
  }
  else if (file_path) {
    RC_CLIENT_LOG_INFO_FORMATTED(client, "Identifying game: %s", file_path);
  }
  else {
    callback(RC_INVALID_STATE, "either data or file_path is required", client, callback_userdata);
    return NULL;
  }

  if (!file_path)
    file_path = "?";

  load_state = (rc_client_load_state_t*)calloc(1, sizeof(*load_state));
  if (!load_state) {
    callback(RC_OUT_OF_MEMORY, rc_error_str(RC_OUT_OF_MEMORY), client, callback_userdata);
    return NULL;
  }
  load_state->client = client;
  load_state->callback = callback;
  load_state->callback_userdata = callback_userdata;

  /* initialize the iterator */
  rc_hash_initialize_iterator(&load_state->hash_iterator, file_path, data, data_size);
  rc_hash_merge_callbacks(&load_state->hash_iterator, &client->callbacks.hash);
  load_state->hash_iterator.userdata = load_state;

  if (!load_state->hash_iterator.callbacks.verbose_message)
    load_state->hash_iterator.callbacks.verbose_message = rc_client_log_hash_message_verbose;
  if (!load_state->hash_iterator.callbacks.error_message)
    load_state->hash_iterator.callbacks.error_message = rc_client_log_hash_message_error;

  /* calculate the hash */
  if (console_id == RC_CONSOLE_UNKNOWN) {
    if (!rc_hash_iterate(hash, &load_state->hash_iterator)) {
      rc_client_load_error(load_state, RC_INVALID_STATE, "hash generation failed");
      return NULL;
    }

    load_state->hash_console_id = load_state->hash_iterator.consoles[load_state->hash_iterator.index - 1];
  }
  else {
    /* ASSERT: hash_iterator->index and hash_iterator->consoles[0] will be 0 from calloc */
    load_state->hash_console_id = (uint8_t)console_id;

    /* prevent initializing the iterator so it won't try other consoles in rc_client_process_resolved_hash */
    load_state->hash_iterator.index = 0;

    if (!rc_hash_generate(hash, console_id, &load_state->hash_iterator)) {
      rc_client_load_error(load_state, RC_INVALID_STATE, "hash generation failed");
      return NULL;
    }
  }

  return rc_client_load_game(load_state, hash, file_path);
}

#endif /* RC_CLIENT_SUPPORTS_HASH */

int rc_client_get_load_game_state(const rc_client_t* client)
{
  int state = RC_CLIENT_LOAD_GAME_STATE_NONE;
  if (client) {
    const rc_client_load_state_t* load_state = client->state.load;
    if (load_state)
      state = load_state->progress;
    else if (client->game)
      state = RC_CLIENT_LOAD_GAME_STATE_DONE;
  }

  return state;
}

int rc_client_is_game_loaded(const rc_client_t* client)
{
  if (!client)
    return 0;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client) {
    const rc_client_game_t* game = rc_client_get_game_info(client);
    return (game && game->id != 0);
  }
#endif

  return (client->game && client->game->public_.id != 0);
}

void rc_client_unload_game(rc_client_t* client)
{
  rc_client_game_info_t* game;

  if (!client)
    return;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client && client->state.external_client->unload_game) {
    client->state.external_client->unload_game();

    /* a game object may have been allocated to manage hashes */
    game = client->game;
    client->game = NULL;
    if (game != NULL)
      rc_client_free_game(game);

    return;
  }
#endif

  rc_mutex_lock(&client->state.mutex);

  game = client->game;
  client->game = NULL;

  if (client->state.load) {
    /* this mimics rc_client_abort_async without nesting the lock */
    client->state.load->async_handle.aborted = RC_CLIENT_ASYNC_ABORTED;

    /* if the game is still being loaded, let the load process clean it up */
    if (client->state.load->game == game)
      game = NULL;

    client->state.load = NULL;
  }

  if (client->state.spectator_mode == RC_CLIENT_SPECTATOR_MODE_LOCKED)
    client->state.spectator_mode = RC_CLIENT_SPECTATOR_MODE_ON;

  if (game != NULL) {
    rc_client_game_mark_ui_to_be_hidden(client, game);
    rc_client_unschedule_ping(client, game);
  }

  rc_mutex_unlock(&client->state.mutex);

  if (game != NULL) {
    rc_client_raise_pending_events(client, game);

    RC_CLIENT_LOG_INFO_FORMATTED(client, "Unloading game %u", game->public_.id);
    rc_client_free_game(game);
  }
}

static void rc_client_change_media_internal(rc_client_t* client, const rc_client_game_hash_t* game_hash, rc_client_callback_t callback, void* callback_userdata)
{
  client->game->public_.hash = game_hash->hash;

  if (game_hash->game_id == client->game->public_.id) {
    RC_CLIENT_LOG_INFO_FORMATTED(client, "Switching to valid media for game %u: %s", game_hash->game_id, game_hash->hash);
  }
  else if (game_hash->game_id == RC_CLIENT_UNKNOWN_GAME_ID) {
    RC_CLIENT_LOG_INFO(client, "Switching to unknown media");
  }
  else if (game_hash->game_id == 0) {
    if (client->state.hardcore) {
      RC_CLIENT_LOG_WARN_FORMATTED(client, "Disabling hardcore for unidentified media: %s", game_hash->hash);
      rc_client_set_hardcore_enabled(client, 0);
      callback(RC_HARDCORE_DISABLED, "Hardcore disabled. Unidentified media inserted.", client, callback_userdata);
      return;
    }

    RC_CLIENT_LOG_INFO_FORMATTED(client, "Switching to unrecognized media: %s", game_hash->hash);
  }
  else {
    RC_CLIENT_LOG_INFO_FORMATTED(client, "Switching to known media for game %u: %s", game_hash->game_id, game_hash->hash);
  }

  callback(RC_OK, NULL, client, callback_userdata);
}

static void rc_client_identify_changed_media_callback(const rc_api_server_response_t* server_response, void* callback_data)
{
  rc_client_load_state_t* load_state = (rc_client_load_state_t*)callback_data;
  rc_client_t* client = load_state->client;
  rc_api_resolve_hash_response_t resolve_hash_response;

  int result = rc_api_process_resolve_hash_server_response(&resolve_hash_response, server_response);
  const char* error_message = rc_client_server_error_message(&result, server_response->http_status_code, &resolve_hash_response.response);

  const int async_aborted = rc_client_end_async(client, &load_state->async_handle);
  if (async_aborted) {
    if (async_aborted != RC_CLIENT_ASYNC_DESTROYED) {
      RC_CLIENT_LOG_VERBOSE(client, "Media change aborted");
      /* if lookup succeeded, still capture the new hash */
      if (result == RC_OK)
        load_state->hash->game_id = resolve_hash_response.game_id;
    }
  }
  else if (client->game != load_state->game) {
    /* loaded game changed. return success regardless of result */
    load_state->callback(RC_ABORTED, "The requested game is no longer active", client, load_state->callback_userdata);
  }
  else if (error_message) {
    load_state->callback(result, error_message, client, load_state->callback_userdata);
  }
  else {
    load_state->hash->game_id = resolve_hash_response.game_id;

    if (resolve_hash_response.game_id != 0) {
      RC_CLIENT_LOG_INFO_FORMATTED(client, "Identified game: %u (%s)", load_state->hash->game_id, load_state->hash->hash);
    }

    rc_client_change_media_internal(client, load_state->hash, load_state->callback, load_state->callback_userdata);
  }

  free(load_state);
  rc_api_destroy_resolve_hash_response(&resolve_hash_response);
}

static rc_client_async_handle_t* rc_client_begin_change_media_internal(rc_client_t* client,
  rc_client_game_info_t* game, rc_client_game_hash_t* game_hash, rc_client_callback_t callback, void* callback_userdata)
{
  rc_client_load_state_t* callback_data;
  rc_client_async_handle_t* async_handle;
  rc_api_resolve_hash_request_t resolve_hash_request;
  rc_api_request_t request;
  int result;

  if (game_hash->game_id != RC_CLIENT_UNKNOWN_GAME_ID || /* previously looked up */
    game_hash->hash[0] == '[') { /* internal use - don't try to look up */
    rc_client_change_media_internal(client, game_hash, callback, callback_userdata);
    return NULL;
  }

  /* call the server to make sure the hash is valid for the loaded game */
  memset(&resolve_hash_request, 0, sizeof(resolve_hash_request));
  resolve_hash_request.game_hash = game_hash->hash;

  result = rc_api_init_resolve_hash_request_hosted(&request, &resolve_hash_request, &client->state.host);
  if (result != RC_OK) {
    callback(result, rc_error_str(result), client, callback_userdata);
    return NULL;
  }

  callback_data = (rc_client_load_state_t*)calloc(1, sizeof(rc_client_load_state_t));
  if (!callback_data) {
    callback(RC_OUT_OF_MEMORY, rc_error_str(RC_OUT_OF_MEMORY), client, callback_userdata);
    return NULL;
  }

  callback_data->callback = callback;
  callback_data->callback_userdata = callback_userdata;
  callback_data->client = client;
  callback_data->hash = game_hash;
  callback_data->game = game;

  async_handle = &callback_data->async_handle;
  rc_client_begin_async(client, async_handle);
  client->callbacks.server_call(&request, rc_client_identify_changed_media_callback, callback_data, client);

  rc_api_destroy_request(&request);

  /* if handle is no longer valid, the async operation completed synchronously */
  return rc_client_async_handle_valid(client, async_handle) ? async_handle : NULL;
}

static rc_client_game_info_t* rc_client_check_pending_media(rc_client_t* client, const rc_client_pending_media_t* media)
{
  rc_client_game_info_t* game;
  rc_client_pending_media_t* pending_media = NULL;

  rc_mutex_lock(&client->state.mutex);
  if (client->state.load) {
    game = client->state.load->game;
    if (!game || game->public_.console_id == 0) {
      /* still waiting for game data */
      pending_media = client->state.load->pending_media;
      if (pending_media)
        rc_client_free_pending_media(pending_media);

      pending_media = (rc_client_pending_media_t*)malloc(sizeof(*pending_media));
      if (!pending_media) {
        rc_mutex_unlock(&client->state.mutex);
        media->callback(RC_OUT_OF_MEMORY, rc_error_str(RC_OUT_OF_MEMORY), client, media->callback_userdata);
        return NULL;
      }

      memcpy(pending_media, media, sizeof(*pending_media));
      if (media->hash)
        pending_media->hash = strdup(media->hash);

#ifdef RC_CLIENT_SUPPORTS_HASH
      if (media->file_path)
        pending_media->file_path = strdup(media->file_path);

      if (media->data && media->data_size) {
        pending_media->data = (uint8_t*)malloc(media->data_size);
        if (!pending_media->data) {
          rc_mutex_unlock(&client->state.mutex);
          media->callback(RC_OUT_OF_MEMORY, rc_error_str(RC_OUT_OF_MEMORY), client, media->callback_userdata);
          return NULL;
        }
        memcpy(pending_media->data, media->data, media->data_size);
      }
      else {
        pending_media->data = NULL;
      }
#endif

      client->state.load->pending_media = pending_media;
    }
  }
  else {
    game = client->game;
  }
  rc_mutex_unlock(&client->state.mutex);

  if (!game) {
    media->callback(RC_NO_GAME_LOADED, rc_error_str(RC_NO_GAME_LOADED), client, media->callback_userdata);
    return NULL;
  }

  /* still waiting for game data - don't call callback - it's queued */
  if (pending_media)
    return NULL;

  return game;
}

#ifdef RC_CLIENT_SUPPORTS_HASH

rc_client_async_handle_t* rc_client_begin_identify_and_change_media(rc_client_t* client, const char* file_path,
  const uint8_t* data, size_t data_size, rc_client_callback_t callback, void* callback_userdata)
{
  rc_client_pending_media_t media;
  rc_client_game_hash_t* game_hash = NULL;
  rc_client_game_info_t* game;
  rc_client_media_hash_t* media_hash;
  uint32_t path_djb2;

  if (!client) {
    callback(RC_INVALID_STATE, "client is required", client, callback_userdata);
    return NULL;
  }

  if (!data && !file_path) {
    callback(RC_INVALID_STATE, "either data or file_path is required", client, callback_userdata);
    return NULL;
  }

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client && !client->state.external_client->begin_change_media) {
    if (client->state.external_client->begin_identify_and_change_media)
      return client->state.external_client->begin_identify_and_change_media(client, file_path, data, data_size, callback, callback_userdata);
  }
#endif

  memset(&media, 0, sizeof(media));
  media.file_path = file_path;
  media.data = (uint8_t*)data;
  media.data_size = data_size;
  media.callback = callback;
  media.callback_userdata = callback_userdata;

  game = rc_client_check_pending_media(client, &media);
  if (game == NULL)
    return NULL;

  /* check to see if we've already hashed this file */
  path_djb2 = rc_djb2(file_path);
  rc_mutex_lock(&client->state.mutex);
  for (media_hash = game->media_hash; media_hash; media_hash = media_hash->next) {
    if (media_hash->path_djb2 == path_djb2) {
      game_hash = media_hash->game_hash;
      break;
    }
  }
  rc_mutex_unlock(&client->state.mutex);

  if (!game_hash) {
    char hash[33];
    int result;

    if (data != NULL)
      result = rc_hash_generate_from_buffer(hash, game->public_.console_id, data, data_size);
    else
      result = rc_hash_generate_from_file(hash, game->public_.console_id, file_path);

    if (!result) {
      /* when changing discs, if the disc is not supported by the system, allow it. this is
       * primarily for games that support user-provided audio CDs, but does allow using discs
       * from other systems for games that leverage user-provided discs. */
      strcpy_s(hash, sizeof(hash), "[NO HASH]");
    }

    game_hash = rc_client_find_game_hash(client, hash);

    media_hash = (rc_client_media_hash_t*)rc_buffer_alloc(&game->buffer, sizeof(*media_hash));
    media_hash->game_hash = game_hash;
    media_hash->path_djb2 = path_djb2;

    rc_mutex_lock(&client->state.mutex);
    media_hash->next = game->media_hash;
    game->media_hash = media_hash;
    rc_mutex_unlock(&client->state.mutex);

    if (!result) {
#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
      if (client->state.external_client && client->state.external_client->begin_change_media)
        return client->state.external_client->begin_change_media(client, game_hash->hash, callback, callback_userdata);
#endif

      rc_client_change_media_internal(client, game_hash, callback, callback_userdata);
      return NULL;
    }
  }

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client) {
    if (client->state.external_client->add_game_hash)
      client->state.external_client->add_game_hash(game_hash->hash, game_hash->game_id);
    if (client->state.external_client->begin_change_media)
      return client->state.external_client->begin_change_media(client, game_hash->hash, callback, callback_userdata);
  }
#endif

  return rc_client_begin_change_media_internal(client, game, game_hash, callback, callback_userdata);
}

#endif /* RC_CLIENT_SUPPORTS_HASH */

rc_client_async_handle_t* rc_client_begin_change_media(rc_client_t* client, const char* hash,
  rc_client_callback_t callback, void* callback_userdata)
{
  rc_client_pending_media_t media;
  rc_client_game_hash_t* game_hash;
  rc_client_game_info_t* game;

  if (!client) {
    callback(RC_INVALID_STATE, "client is required", client, callback_userdata);
    return NULL;
  }

  if (!hash || !hash[0]) {
    callback(RC_INVALID_STATE, "hash is required", client, callback_userdata);
    return NULL;
  }

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client && client->state.external_client->begin_change_media) {
    return client->state.external_client->begin_change_media(client, hash, callback, callback_userdata);
  }
#endif

  memset(&media, 0, sizeof(media));
  media.hash = hash;
  media.callback = callback;
  media.callback_userdata = callback_userdata;

  game = rc_client_check_pending_media(client, &media);
  if (game == NULL)
    return NULL;

  /* check to see if we've already hashed this file. */
  game_hash = rc_client_find_game_hash(client, hash);
  return rc_client_begin_change_media_internal(client, game, game_hash, callback, callback_userdata);
}
