#include "rc_client_async.h"
#include "rc_client_game.h"
#include "rc_client_internal.h"
#include "rc_client_load.h"
#include "rc_client_server.h"

#include "rc_api_info.h"
#include "rc_api_runtime.h"

#include "../runtime/rc_alloc.h"
#include "../runtime/rc_format.h"
#include "../runtime/rc_lboard.h"
#include "../runtime/rc_trigger.h"

#include "../util/rc_compat.h"

/* ===== Achievements ===== */

static void rc_client_update_legacy_runtime_achievements(rc_client_game_info_t* game, uint32_t active_count)
{
  if (active_count > 0) {
    rc_client_achievement_info_t* achievement;
    rc_client_achievement_info_t* stop;
    rc_runtime_trigger_t* trigger;
    rc_client_subset_info_t* subset;

    if (active_count <= game->runtime.trigger_capacity) {
      if (active_count != 0)
        memset(game->runtime.triggers, 0, active_count * sizeof(rc_runtime_trigger_t));
    }
    else {
      if (game->runtime.triggers)
        free(game->runtime.triggers);

      game->runtime.trigger_capacity = active_count;
      game->runtime.triggers = (rc_runtime_trigger_t*)calloc(1, active_count * sizeof(rc_runtime_trigger_t));
    }

    trigger = game->runtime.triggers;
    if (!trigger) {
      /* malloc failed, no way to report error, just bail */
      game->runtime.trigger_count = 0;
      return;
    }

    for (subset = game->subsets; subset; subset = subset->next) {
      if (!subset->active)
        continue;

      achievement = subset->achievements;
      stop = achievement + subset->public_.num_achievements;

      for (; achievement < stop; ++achievement) {
        if (achievement->public_.state == RC_CLIENT_ACHIEVEMENT_STATE_ACTIVE) {
          trigger->id = achievement->public_.id;
          memcpy(trigger->md5, achievement->md5, 16);
          trigger->trigger = achievement->trigger;
          ++trigger;
        }
      }
    }
  }

  game->runtime.trigger_count = active_count;
}

static uint32_t rc_client_subset_count_active_achievements(const rc_client_subset_info_t* subset)
{
  rc_client_achievement_info_t* achievement = subset->achievements;
  rc_client_achievement_info_t* stop = achievement + subset->public_.num_achievements;
  uint32_t active_count = 0;

  for (; achievement < stop; ++achievement) {
    if (achievement->public_.state == RC_CLIENT_ACHIEVEMENT_STATE_ACTIVE)
      ++active_count;
  }

  return active_count;
}

void rc_client_update_active_achievements(rc_client_game_info_t* game)
{
  uint32_t active_count = 0;
  rc_client_subset_info_t* subset = game->subsets;
  for (; subset; subset = subset->next) {
    if (subset->active)
      active_count += rc_client_subset_count_active_achievements(subset);
  }

  rc_client_update_legacy_runtime_achievements(game, active_count);
}

static uint32_t rc_client_subset_toggle_hardcore_achievements(rc_client_subset_info_t* subset, rc_client_t* client, uint8_t active_bit)
{
  rc_client_achievement_info_t* achievement = subset->achievements;
  rc_client_achievement_info_t* stop = achievement + subset->public_.num_achievements;
  uint32_t active_count = 0;

  for (; achievement < stop; ++achievement) {
    if ((achievement->public_.unlocked & active_bit) == 0) {
      /* Encore keeps achievements active, but their previous unlock times remain available. */
      if (client->state.encore_mode) {
        achievement->public_.unlock_time = (client->state.hardcore && achievement->unlock_time_hardcore) ?
            achievement->unlock_time_hardcore : achievement->unlock_time_casual;
      }

      switch (achievement->public_.state) {
        case RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED:
        case RC_CLIENT_ACHIEVEMENT_STATE_INACTIVE:
          rc_reset_trigger(achievement->trigger);
          achievement->public_.state = RC_CLIENT_ACHIEVEMENT_STATE_ACTIVE;
          ++active_count;
          break;

        case RC_CLIENT_ACHIEVEMENT_STATE_ACTIVE:
          ++active_count;
          break;
      }
    }
    else {
      achievement->public_.unlock_time = (active_bit == RC_CLIENT_ACHIEVEMENT_UNLOCKED_HARDCORE) ?
        achievement->unlock_time_hardcore : achievement->unlock_time_casual;

      if (achievement->public_.state == RC_CLIENT_ACHIEVEMENT_STATE_ACTIVE ||
        achievement->public_.state == RC_CLIENT_ACHIEVEMENT_STATE_INACTIVE) {
        /* if it's active despite being unlocked, and we're in encore mode, leave it active */
        if (client->state.encore_mode) {
          ++active_count;
          continue;
        }

        /* switch to inactive */
        achievement->public_.state = RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED;

        if (achievement->trigger && rc_trigger_state_active(achievement->trigger->state)) {
          /* hide any active challenge indicators */
          if (achievement->trigger->state == RC_TRIGGER_STATE_PRIMED) {
            rc_client_event_t client_event;
            memset(&client_event, 0, sizeof(client_event));
            client_event.type = RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_HIDE;
            client_event.achievement = &achievement->public_;
            client->callbacks.event_handler(&client_event, client);
          }

          achievement->trigger->state = RC_TRIGGER_STATE_TRIGGERED;
        }
      }
    }
  }

  return active_count;
}

void rc_client_toggle_hardcore_achievements(rc_client_game_info_t* game, rc_client_t* client, uint8_t active_bit)
{
  uint32_t active_count = 0;
  rc_client_subset_info_t* subset = game->subsets;
  for (; subset; subset = subset->next) {
    if (subset->active)
      active_count += rc_client_subset_toggle_hardcore_achievements(subset, client, active_bit);
  }

  rc_client_update_legacy_runtime_achievements(game, active_count);
}

void rc_client_activate_achievements(rc_client_game_info_t* game, rc_client_t* client)
{
  const uint8_t active_bit = (client->state.encore_mode) ?
    RC_CLIENT_ACHIEVEMENT_UNLOCKED_NONE : (client->state.hardcore) ?
    RC_CLIENT_ACHIEVEMENT_UNLOCKED_HARDCORE : RC_CLIENT_ACHIEVEMENT_UNLOCKED_CASUAL;

  rc_client_toggle_hardcore_achievements(game, client, active_bit);
}

void rc_client_update_achievement_display_information(rc_client_t* client, rc_client_achievement_info_t* achievement, time_t recent_unlock_time)
{
  uint8_t new_bucket = RC_CLIENT_ACHIEVEMENT_BUCKET_UNKNOWN;
  uint32_t new_measured_value = 0;

  if (achievement->public_.bucket == RC_CLIENT_ACHIEVEMENT_BUCKET_UNSUPPORTED)
    return;

  achievement->public_.measured_progress[0] = '\0';

  if (achievement->public_.state == RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED) {
    /* achievement unlocked */
    if (client->state.disconnect && rc_client_is_award_achievement_pending(client, achievement->public_.id))
      new_bucket = RC_CLIENT_ACHIEVEMENT_BUCKET_UNSYNCED;
    else if (achievement->public_.unlock_time >= recent_unlock_time)
      new_bucket = RC_CLIENT_ACHIEVEMENT_BUCKET_RECENTLY_UNLOCKED;
    else
      new_bucket = RC_CLIENT_ACHIEVEMENT_BUCKET_UNLOCKED;
  }
  else {
    /* active achievement */
    new_bucket = (achievement->public_.category == RC_CLIENT_ACHIEVEMENT_CATEGORY_UNPROMOTED) ?
      RC_CLIENT_ACHIEVEMENT_BUCKET_UNPROMOTED : RC_CLIENT_ACHIEVEMENT_BUCKET_LOCKED;

    if (achievement->trigger) {
      if (achievement->trigger->measured_target) {
        if (achievement->trigger->measured_value == RC_MEASURED_UNKNOWN) {
          /* value hasn't been initialized yet, leave progress string empty */
        }
        else {
          /* clamp measured value at target (can't get more than 100%) */
          new_measured_value = (achievement->trigger->measured_value > achievement->trigger->measured_target) ?
            achievement->trigger->measured_target : achievement->trigger->measured_value;

          achievement->public_.measured_percent = ((float)new_measured_value * 100) / (float)achievement->trigger->measured_target;

          if (!achievement->trigger->measured_as_percent) {
            char* ptr = achievement->public_.measured_progress;
            const int buffer_size = (int)sizeof(achievement->public_.measured_progress);
            const int chars = rc_format_value(ptr, buffer_size - 1, (int32_t)new_measured_value, RC_FORMAT_UNSIGNED_VALUE);
            ptr[chars] = '/';
            rc_format_value(ptr + chars + 1, buffer_size - chars - 1, (int32_t)achievement->trigger->measured_target, RC_FORMAT_UNSIGNED_VALUE);
          }
          else {
            snprintf(achievement->public_.measured_progress, sizeof(achievement->public_.measured_progress),
              "%lu%%", (unsigned long)achievement->public_.measured_percent);
          }
        }
      }

      if (achievement->trigger->state == RC_TRIGGER_STATE_PRIMED)
        new_bucket = RC_CLIENT_ACHIEVEMENT_BUCKET_ACTIVE_CHALLENGE;
      else if (achievement->public_.measured_percent >= 80.0)
        new_bucket = RC_CLIENT_ACHIEVEMENT_BUCKET_ALMOST_THERE;
    }

    /* map to unlocked in casual bucket if hardcore is enabled but achievement is unlocked in casual */
    if (client->state.hardcore && (achievement->public_.unlocked & RC_CLIENT_ACHIEVEMENT_UNLOCKED_CASUAL))
      new_bucket = RC_CLIENT_ACHIEVEMENT_BUCKET_UNLOCKED_SOFTCORE;
  }

  achievement->public_.bucket = new_bucket;
}

static const char* rc_client_get_achievement_bucket_label(uint8_t bucket_type)
{
  switch (bucket_type) {
    case RC_CLIENT_ACHIEVEMENT_BUCKET_LOCKED: return "Locked";
    case RC_CLIENT_ACHIEVEMENT_BUCKET_UNLOCKED: return "Unlocked";
    case RC_CLIENT_ACHIEVEMENT_BUCKET_UNSUPPORTED: return "Unsupported";
    case RC_CLIENT_ACHIEVEMENT_BUCKET_UNPROMOTED: return "Unpromoted";
    case RC_CLIENT_ACHIEVEMENT_BUCKET_RECENTLY_UNLOCKED: return "Recently Unlocked";
    case RC_CLIENT_ACHIEVEMENT_BUCKET_ACTIVE_CHALLENGE: return "Active Challenges";
    case RC_CLIENT_ACHIEVEMENT_BUCKET_ALMOST_THERE: return "Almost There";
    case RC_CLIENT_ACHIEVEMENT_BUCKET_UNLOCKED_SOFTCORE: return "Unlocked in Casual";
    case RC_CLIENT_ACHIEVEMENT_BUCKET_UNSYNCED: return "Unlocks Not Synced to Server";
    default: return "Unknown";
  }
}

static const char* rc_client_get_subset_achievement_bucket_label(uint8_t bucket_type, rc_client_game_info_t* game, rc_client_subset_info_t* subset)
{
  const char** ptr;
  const char* label;
  char* new_label;
  size_t new_label_len;

  switch (bucket_type) {
    case RC_CLIENT_ACHIEVEMENT_BUCKET_LOCKED: ptr = &subset->locked_label; break;
    case RC_CLIENT_ACHIEVEMENT_BUCKET_UNLOCKED: ptr = &subset->unlocked_label; break;
    case RC_CLIENT_ACHIEVEMENT_BUCKET_UNSUPPORTED: ptr = &subset->unsupported_label; break;
    case RC_CLIENT_ACHIEVEMENT_BUCKET_UNPROMOTED: ptr = &subset->unpromoted_label; break;
    case RC_CLIENT_ACHIEVEMENT_BUCKET_UNLOCKED_SOFTCORE: ptr = &subset->unlocked_casual_label; break;
    default: return rc_client_get_achievement_bucket_label(bucket_type);
  }

  if (*ptr)
    return *ptr;

  label = rc_client_get_achievement_bucket_label(bucket_type);
  new_label_len = strlen(subset->public_.title) + strlen(label) + 4;
  new_label = (char*)rc_buffer_alloc(&game->buffer, new_label_len);
  snprintf(new_label, new_label_len, "%s - %s", subset->public_.title, label);

  *ptr = new_label;
  return new_label;
}

static int rc_client_compare_achievement_unlock_times(const void* a, const void* b)
{
  const rc_client_achievement_t* unlock_a = *(const rc_client_achievement_t**)a;
  const rc_client_achievement_t* unlock_b = *(const rc_client_achievement_t**)b;
  if (unlock_b->unlock_time == unlock_a->unlock_time)
    return 0;
  return (unlock_b->unlock_time < unlock_a->unlock_time) ? -1 : 1;
}

static int rc_client_compare_achievement_progress(const void* a, const void* b)
{
  const rc_client_achievement_t* unlock_a = *(const rc_client_achievement_t**)a;
  const rc_client_achievement_t* unlock_b = *(const rc_client_achievement_t**)b;
  if (unlock_b->measured_percent == unlock_a->measured_percent) {
    if (unlock_a->id == unlock_b->id)
      return 0;
    return (unlock_a->id < unlock_b->id) ? -1 : 1;
  }
  return (unlock_b->measured_percent < unlock_a->measured_percent) ? -1 : 1;
}

static uint8_t rc_client_map_bucket(uint8_t bucket, int grouping)
{
  if (grouping == RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE) {
    switch (bucket) {
      case RC_CLIENT_ACHIEVEMENT_BUCKET_RECENTLY_UNLOCKED:
      case RC_CLIENT_ACHIEVEMENT_BUCKET_UNSYNCED:
        return RC_CLIENT_ACHIEVEMENT_BUCKET_UNLOCKED;

      case RC_CLIENT_ACHIEVEMENT_BUCKET_ACTIVE_CHALLENGE:
      case RC_CLIENT_ACHIEVEMENT_BUCKET_ALMOST_THERE:
      case RC_CLIENT_ACHIEVEMENT_BUCKET_UNLOCKED_SOFTCORE:
        return RC_CLIENT_ACHIEVEMENT_BUCKET_LOCKED;

      default:
        return bucket;
    }
  }

  return bucket;
}

static int rc_client_is_subset_visible_in_achievement_list(const rc_client_subset_info_t* subset, const rc_client_achievement_list_params_t* params)
{
  if (!subset->active)
    return 0;

  return (params->subset_id == 0 || params->subset_id == subset->public_.id);
}

static int rc_client_is_achievement_visible_in_list(const rc_client_achievement_info_t* achievement, const rc_client_achievement_list_params_t* params)
{
  return (achievement->public_.category & params->category) != 0;
}

rc_client_achievement_list_t* rc_client_create_achievement_list(rc_client_t* client, int category, int grouping)
{
  rc_client_achievement_list_params_t params;
  params.subset_id = 0;
  params.grouping = grouping;
  params.category = category;

  return rc_client_create_subset_achievement_list(client, &params);
}

rc_client_achievement_list_t* rc_client_create_subset_achievement_list(rc_client_t* client, const rc_client_achievement_list_params_t* params)
{
  rc_client_achievement_info_t* achievement;
  rc_client_achievement_info_t* stop;
  const rc_client_achievement_t** bucket_achievements;
  const rc_client_achievement_t** achievement_ptr;
  rc_client_achievement_bucket_t* bucket_ptr;
  rc_client_achievement_list_info_t* list;
  rc_client_subset_info_t* subset;
  const uint32_t list_size = RC_ALIGN(sizeof(*list));
  uint32_t bucket_counts[NUM_RC_CLIENT_ACHIEVEMENT_BUCKETS];
  uint32_t num_buckets;
  uint32_t num_achievements;
  size_t buckets_size;
  uint8_t bucket_type;
  uint32_t num_subsets = 0;
  uint32_t i, j;
  static const uint8_t shared_bucket_order[] = {
    RC_CLIENT_ACHIEVEMENT_BUCKET_ACTIVE_CHALLENGE,
    RC_CLIENT_ACHIEVEMENT_BUCKET_RECENTLY_UNLOCKED,
    RC_CLIENT_ACHIEVEMENT_BUCKET_ALMOST_THERE,
    RC_CLIENT_ACHIEVEMENT_BUCKET_UNSYNCED,
  };
  static const uint8_t subset_bucket_order[] = {
    RC_CLIENT_ACHIEVEMENT_BUCKET_LOCKED,
    RC_CLIENT_ACHIEVEMENT_BUCKET_UNPROMOTED,
    RC_CLIENT_ACHIEVEMENT_BUCKET_UNSUPPORTED,
    RC_CLIENT_ACHIEVEMENT_BUCKET_UNLOCKED_SOFTCORE,
    RC_CLIENT_ACHIEVEMENT_BUCKET_UNLOCKED
  };
  const time_t recent_unlock_time = time(NULL) - RC_CLIENT_RECENT_UNLOCK_DELAY_SECONDS;

  if (!client)
    return (rc_client_achievement_list_t*)calloc(1, sizeof(rc_client_achievement_list_info_t));

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client) {
    if (client->state.external_client->create_subset_achievement_list)
      return (rc_client_achievement_list_t*)client->state.external_client->create_subset_achievement_list(params);

    if (client->state.external_client->create_achievement_list_v3)
      return (rc_client_achievement_list_t*)client->state.external_client->create_achievement_list_v3(params->category, params->grouping);

    if (client->state.external_client->create_achievement_list)
      return rc_client_external_convert_v1_achievement_list(client,
        (rc_client_achievement_list_t*)client->state.external_client->create_achievement_list(params->category, params->grouping));
  }
#endif

  if (!client->game)
    return (rc_client_achievement_list_t*)calloc(1, sizeof(rc_client_achievement_list_info_t));

  memset(&bucket_counts, 0, sizeof(bucket_counts));

  rc_mutex_lock(&client->state.mutex);

  subset = client->game->subsets;
  for (; subset; subset = subset->next) {
    if (!rc_client_is_subset_visible_in_achievement_list(subset, params))
      continue;

    num_subsets++;
    achievement = subset->achievements;
    stop = achievement + subset->public_.num_achievements;
    for (; achievement < stop; ++achievement) {
      if (rc_client_is_achievement_visible_in_list(achievement, params)) {
        rc_client_update_achievement_display_information(client, achievement, recent_unlock_time);
        bucket_counts[rc_client_map_bucket(achievement->public_.bucket, params->grouping)]++;
      }
    }
  }

  num_buckets = 0;
  num_achievements = 0;
  for (i = 0; i < sizeof(bucket_counts) / sizeof(bucket_counts[0]); ++i) {
    if (bucket_counts[i]) {
      int needs_split = 0;

      num_achievements += bucket_counts[i];

      if (num_subsets > 1) {
        for (j = 0; j < sizeof(subset_bucket_order) / sizeof(subset_bucket_order[0]); ++j) {
          if (subset_bucket_order[j] == i) {
            needs_split = 1;
            break;
          }
        }
      }

      if (!needs_split) {
        ++num_buckets;
        continue;
      }

      subset = client->game->subsets;
      for (; subset; subset = subset->next) {
        if (!rc_client_is_subset_visible_in_achievement_list(subset, params))
          continue;

        achievement = subset->achievements;
        stop = achievement + subset->public_.num_achievements;
        for (; achievement < stop; ++achievement) {
          if (rc_client_is_achievement_visible_in_list(achievement, params)) {
            if (rc_client_map_bucket(achievement->public_.bucket, params->grouping) == i) {
              ++num_buckets;
              break;
            }
          }
        }
      }
    }
  }

  buckets_size = RC_ALIGN(num_buckets * sizeof(rc_client_achievement_bucket_t));

  list = (rc_client_achievement_list_info_t*)malloc(list_size + buckets_size + num_achievements * sizeof(rc_client_achievement_t*));
  if (!list) {
    rc_mutex_unlock(&client->state.mutex);
    return NULL;
  }
  list->public_.buckets = bucket_ptr = (rc_client_achievement_bucket_t*)((uint8_t*)list + list_size);
  achievement_ptr = (const rc_client_achievement_t**)((uint8_t*)bucket_ptr + buckets_size);

  if (params->grouping == RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS) {
    for (i = 0; i < sizeof(shared_bucket_order) / sizeof(shared_bucket_order[0]); ++i) {
      bucket_type = shared_bucket_order[i];
      if (!bucket_counts[bucket_type])
        continue;

      bucket_achievements = achievement_ptr;
      for (subset = client->game->subsets; subset; subset = subset->next) {
        if (!rc_client_is_subset_visible_in_achievement_list(subset, params))
          continue;

        achievement = subset->achievements;
        stop = achievement + subset->public_.num_achievements;
        for (; achievement < stop; ++achievement) {
          if (rc_client_is_achievement_visible_in_list(achievement, params) &&
            rc_client_map_bucket(achievement->public_.bucket, params->grouping) == bucket_type) {
            *achievement_ptr++ = &achievement->public_;
          }
        }
      }

      if (achievement_ptr > bucket_achievements) {
        bucket_ptr->achievements = bucket_achievements;
        bucket_ptr->num_achievements = (uint32_t)(achievement_ptr - bucket_achievements);
        bucket_ptr->subset_id = 0;
        bucket_ptr->label = rc_client_get_achievement_bucket_label(bucket_type);
        bucket_ptr->bucket_type = bucket_type;

        if (bucket_type == RC_CLIENT_ACHIEVEMENT_BUCKET_RECENTLY_UNLOCKED)
          qsort((void*)bucket_ptr->achievements, bucket_ptr->num_achievements, sizeof(rc_client_achievement_t*), rc_client_compare_achievement_unlock_times);
        else if (bucket_type == RC_CLIENT_ACHIEVEMENT_BUCKET_ALMOST_THERE)
          qsort((void*)bucket_ptr->achievements, bucket_ptr->num_achievements, sizeof(rc_client_achievement_t*), rc_client_compare_achievement_progress);

        ++bucket_ptr;
      }
    }
  }

  for (subset = client->game->subsets; subset; subset = subset->next) {
    if (!rc_client_is_subset_visible_in_achievement_list(subset, params))
      continue;

    for (i = 0; i < sizeof(subset_bucket_order) / sizeof(subset_bucket_order[0]); ++i) {
      bucket_type = subset_bucket_order[i];
      if (!bucket_counts[bucket_type])
        continue;

      bucket_achievements = achievement_ptr;

      achievement = subset->achievements;
      stop = achievement + subset->public_.num_achievements;
      for (; achievement < stop; ++achievement) {
        if (rc_client_is_achievement_visible_in_list(achievement, params) &&
          rc_client_map_bucket(achievement->public_.bucket, params->grouping) == bucket_type) {
          *achievement_ptr++ = &achievement->public_;
        }
      }

      if (achievement_ptr > bucket_achievements) {
        bucket_ptr->achievements = bucket_achievements;
        bucket_ptr->num_achievements = (uint32_t)(achievement_ptr - bucket_achievements);
        bucket_ptr->subset_id = (num_subsets > 1) ? subset->public_.id : 0;
        bucket_ptr->bucket_type = bucket_type;

        if (num_subsets > 1)
          bucket_ptr->label = rc_client_get_subset_achievement_bucket_label(bucket_type, client->game, subset);
        else
          bucket_ptr->label = rc_client_get_achievement_bucket_label(bucket_type);

        ++bucket_ptr;
      }
    }
  }

  rc_mutex_unlock(&client->state.mutex);

  list->destroy_func = NULL;
  list->public_.num_buckets = (uint32_t)(bucket_ptr - list->public_.buckets);
  return &list->public_;
}

void rc_client_destroy_achievement_list(rc_client_achievement_list_t* list)
{
  rc_client_achievement_list_info_t* info = (rc_client_achievement_list_info_t*)list;
  if (info->destroy_func)
    info->destroy_func(info);
  else
    free(list);
}

int rc_client_has_achievements(rc_client_t* client)
{
  rc_client_subset_info_t* subset;
  int result;

  if (!client)
    return 0;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client && client->state.external_client->has_achievements)
    return client->state.external_client->has_achievements();
#endif

  if (!client->game)
    return 0;

  rc_mutex_lock(&client->state.mutex);

  subset = client->game->subsets;
  result = 0;
  for (; subset; subset = subset->next)
  {
    if (!subset->active)
      continue;

    if (subset->public_.num_achievements > 0) {
      result = 1;
      break;
    }
  }

  rc_mutex_unlock(&client->state.mutex);

  return result;
}

const rc_client_achievement_t* rc_client_subset_get_achievement_info(
  rc_client_t* client, rc_client_subset_info_t* subset, uint32_t id)
{
  rc_client_achievement_info_t* achievement = subset->achievements;
  rc_client_achievement_info_t* stop = achievement + subset->public_.num_achievements;

  for (; achievement < stop; ++achievement) {
    if (achievement->public_.id == id) {
      const time_t recent_unlock_time = time(NULL) - RC_CLIENT_RECENT_UNLOCK_DELAY_SECONDS;
      rc_mutex_lock((rc_mutex_t*)(&client->state.mutex));
      rc_client_update_achievement_display_information(client, achievement, recent_unlock_time);
      rc_mutex_unlock((rc_mutex_t*)(&client->state.mutex));
      return &achievement->public_;
    }
  }

  return NULL;
}

const rc_client_achievement_t* rc_client_get_achievement_info(rc_client_t* client, uint32_t id)
{
  rc_client_subset_info_t* subset;

  if (!client)
    return NULL;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client) {
    if (client->state.external_client->get_achievement_info_v3)
      return client->state.external_client->get_achievement_info_v3(id);

    if (client->state.external_client->get_achievement_info)
      return rc_client_external_convert_v1_achievement(client, client->state.external_client->get_achievement_info(id));
  }
#endif

  if (!client->game)
    return NULL;

  for (subset = client->game->subsets; subset; subset = subset->next) {
    const rc_client_achievement_t* achievement = rc_client_subset_get_achievement_info(client, subset, id);
    if (achievement != NULL)
      return achievement;
  }

  return NULL;
}

const rc_client_achievement_t* rc_client_get_next_achievement_info(rc_client_t* client,
  const rc_client_achievement_t* achievement, int bucket)
{
  const rc_client_achievement_info_t* after = (const rc_client_achievement_info_t*)achievement;
  rc_client_achievement_info_t* achievement_info;
  time_t recent_unlock_time;
  rc_client_subset_info_t* subset;

  if (!client)
    return NULL;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client && client->state.external_client->get_next_achievement_info)
    return client->state.external_client->get_next_achievement_info(achievement ? achievement->id : 0, bucket);
#endif

  if (!client->game)
    return NULL;

  recent_unlock_time = time(NULL) - RC_CLIENT_RECENT_UNLOCK_DELAY_SECONDS;
  for (subset = client->game->subsets; subset; subset = subset->next) {
    if (subset->active && subset->public_.num_achievements > 0) {
      const rc_client_achievement_info_t* start = subset->achievements;
      const rc_client_achievement_info_t* stop = start + subset->public_.num_achievements;
      if (after == NULL || (after >= start && after <= stop)) {
        /* found a subset containing the provided achievement. look for the next
         * achievement matching the requested bucket */
        uint32_t index = after ? (uint32_t)(after - start) + 1 : 0;
        do {
          if (index >= subset->public_.num_achievements) {
            /* done with this subset. find the next active subset with achievements */
            do {
              subset = subset->next;
              if (!subset)
                return NULL;
            } while (!subset->active || subset->public_.num_achievements == 0);

            index = 0;
          }

          /* found an achievement. check to see if it matches the requested bucket. */
          achievement_info = &subset->achievements[index];
          rc_client_update_achievement_display_information(client, achievement_info, recent_unlock_time);
          if (achievement_info->public_.bucket == bucket)
            return &achievement_info->public_;

          ++index;
        } while (1);
      }
    }
  }

  return NULL;
}

int rc_client_achievement_get_image_url(const rc_client_achievement_t* achievement, int state, char buffer[], size_t buffer_size)
{
  const int image_type = (state == RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED) ?
    RC_IMAGE_TYPE_ACHIEVEMENT : RC_IMAGE_TYPE_ACHIEVEMENT_LOCKED;

  if (!achievement || !achievement->badge_name[0])
    return rc_client_get_image_url(buffer, buffer_size, image_type, "00000");

  if (image_type == RC_IMAGE_TYPE_ACHIEVEMENT && achievement->badge_url) {
    const size_t len = strlen(achievement->badge_url);
    if (len >= buffer_size)
      return RC_INSUFFICIENT_BUFFER;

    memcpy(buffer, achievement->badge_url, len + 1);
    return RC_OK;
  }

  if (image_type == RC_IMAGE_TYPE_ACHIEVEMENT_LOCKED && achievement->badge_locked_url) {
    const size_t len = strlen(achievement->badge_locked_url);
    if (len >= buffer_size)
      return RC_INSUFFICIENT_BUFFER;

    memcpy(buffer, achievement->badge_locked_url, len + 1);
    return RC_OK;
  }

  return rc_client_get_image_url(buffer, buffer_size, image_type, achievement->badge_name);
}

/* ===== Leaderboards ===== */

static void rc_client_update_legacy_runtime_leaderboards(rc_client_game_info_t* game, uint32_t active_count)
{
  if (active_count > 0) {
    rc_client_leaderboard_info_t* leaderboard;
    rc_client_leaderboard_info_t* stop;
    rc_client_subset_info_t* subset;
    rc_runtime_lboard_t* lboard;

    if (active_count <= game->runtime.lboard_capacity) {
      if (active_count != 0)
        memset(game->runtime.lboards, 0, active_count * sizeof(rc_runtime_lboard_t));
    }
    else {
      if (game->runtime.lboards)
        free(game->runtime.lboards);

      game->runtime.lboard_capacity = active_count;
      game->runtime.lboards = (rc_runtime_lboard_t*)calloc(1, active_count * sizeof(rc_runtime_lboard_t));
    }

    lboard = game->runtime.lboards;
    if (!lboard) {
      /* malloc failed. no way to report error, just bail */
      game->runtime.lboard_count = 0;
      return;
    }

    for (subset = game->subsets; subset; subset = subset->next) {
      if (!subset->active)
        continue;

      leaderboard = subset->leaderboards;
      stop = leaderboard + subset->public_.num_leaderboards;
      for (; leaderboard < stop; ++leaderboard) {
        if (leaderboard->public_.state == RC_CLIENT_LEADERBOARD_STATE_ACTIVE ||
          leaderboard->public_.state == RC_CLIENT_LEADERBOARD_STATE_TRACKING) {
          lboard->id = leaderboard->public_.id;
          memcpy(lboard->md5, leaderboard->md5, 16);
          lboard->lboard = leaderboard->lboard;
          ++lboard;
        }
      }
    }
  }

  game->runtime.lboard_count = active_count;
}

void rc_client_update_active_leaderboards(rc_client_game_info_t* game)
{
  rc_client_leaderboard_info_t* leaderboard;
  rc_client_leaderboard_info_t* stop;

  uint32_t active_count = 0;
  rc_client_subset_info_t* subset = game->subsets;
  for (; subset; subset = subset->next)
  {
    if (!subset->active)
      continue;

    leaderboard = subset->leaderboards;
    stop = leaderboard + subset->public_.num_leaderboards;

    for (; leaderboard < stop; ++leaderboard)
    {
      switch (leaderboard->public_.state)
      {
        case RC_CLIENT_LEADERBOARD_STATE_ACTIVE:
        case RC_CLIENT_LEADERBOARD_STATE_TRACKING:
          ++active_count;
          break;
      }
    }
  }

  rc_client_update_legacy_runtime_leaderboards(game, active_count);
}

void rc_client_activate_leaderboards(rc_client_game_info_t* game, rc_client_t* client)
{
  rc_client_leaderboard_info_t* leaderboard;
  rc_client_leaderboard_info_t* stop;
  const uint8_t leaderboards_allowed =
    client->state.hardcore || client->state.allow_leaderboards_in_casual;

  uint32_t active_count = 0;
  rc_client_subset_info_t* subset = game->subsets;
  for (; subset; subset = subset->next) {
    if (!subset->active)
      continue;

    leaderboard = subset->leaderboards;
    stop = leaderboard + subset->public_.num_leaderboards;

    for (; leaderboard < stop; ++leaderboard) {
      switch (leaderboard->public_.state) {
        case RC_CLIENT_LEADERBOARD_STATE_DISABLED:
          continue;

        case RC_CLIENT_LEADERBOARD_STATE_INACTIVE:
          if (leaderboards_allowed) {
            rc_reset_lboard(leaderboard->lboard);
            leaderboard->public_.state = RC_CLIENT_LEADERBOARD_STATE_ACTIVE;
            ++active_count;
          }
          break;

        default:
          if (leaderboards_allowed)
            ++active_count;
          else
            leaderboard->public_.state = RC_CLIENT_LEADERBOARD_STATE_INACTIVE;
          break;
      }
    }
  }

  rc_client_update_legacy_runtime_leaderboards(game, active_count);
}

void rc_client_deactivate_leaderboards(rc_client_game_info_t* game, rc_client_t* client)
{
  rc_client_leaderboard_info_t* leaderboard;
  rc_client_leaderboard_info_t* stop;

  rc_client_subset_info_t* subset = game->subsets;
  for (; subset; subset = subset->next) {
    if (!subset->active)
      continue;

    leaderboard = subset->leaderboards;
    stop = leaderboard + subset->public_.num_leaderboards;

    for (; leaderboard < stop; ++leaderboard) {
      switch (leaderboard->public_.state) {
        case RC_CLIENT_LEADERBOARD_STATE_DISABLED:
        case RC_CLIENT_LEADERBOARD_STATE_INACTIVE:
          continue;

        case RC_CLIENT_LEADERBOARD_STATE_TRACKING:
          rc_client_release_leaderboard_tracker(client->game, leaderboard);
          /* fallthrough */ /* to default */
        default:
          leaderboard->public_.state = RC_CLIENT_LEADERBOARD_STATE_INACTIVE;
          break;
      }
    }
  }

  game->runtime.lboard_count = 0;
}

uint8_t rc_client_map_leaderboard_format(int format)
{
  switch (format) {
    case RC_FORMAT_SECONDS:
    case RC_FORMAT_CENTISECS:
    case RC_FORMAT_MINUTES:
    case RC_FORMAT_SECONDS_AS_MINUTES:
    case RC_FORMAT_FRAMES:
      return RC_CLIENT_LEADERBOARD_FORMAT_TIME;

    case RC_FORMAT_SCORE:
      return RC_CLIENT_LEADERBOARD_FORMAT_SCORE;

    case RC_FORMAT_VALUE:
    case RC_FORMAT_FLOAT1:
    case RC_FORMAT_FLOAT2:
    case RC_FORMAT_FLOAT3:
    case RC_FORMAT_FLOAT4:
    case RC_FORMAT_FLOAT5:
    case RC_FORMAT_FLOAT6:
    case RC_FORMAT_FIXED1:
    case RC_FORMAT_FIXED2:
    case RC_FORMAT_FIXED3:
    case RC_FORMAT_TENS:
    case RC_FORMAT_HUNDREDS:
    case RC_FORMAT_THOUSANDS:
    case RC_FORMAT_UNSIGNED_VALUE:
    default:
      return RC_CLIENT_LEADERBOARD_FORMAT_VALUE;
  }
}

const rc_client_leaderboard_info_t* rc_client_subset_get_leaderboard_info(const rc_client_subset_info_t* subset, uint32_t id)
{
  rc_client_leaderboard_info_t* leaderboard = subset->leaderboards;
  rc_client_leaderboard_info_t* stop = leaderboard + subset->public_.num_leaderboards;

  for (; leaderboard < stop; ++leaderboard) {
    if (leaderboard->public_.id == id)
      return leaderboard;
  }

  return NULL;
}

const rc_client_leaderboard_t* rc_client_get_leaderboard_info(const rc_client_t* client, uint32_t id)
{
  rc_client_subset_info_t* subset;

  if (!client)
    return NULL;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client) {
    if (client->state.external_client->get_leaderboard_info_v8)
      return client->state.external_client->get_leaderboard_info_v8(id);

    if (client->state.external_client->get_leaderboard_info)
      return rc_client_external_convert_v1_leaderboard(client, client->state.external_client->get_leaderboard_info(id));
  }
#endif

  if (!client->game)
    return NULL;

  for (subset = client->game->subsets; subset; subset = subset->next) {
    const rc_client_leaderboard_info_t* leaderboard = rc_client_subset_get_leaderboard_info(subset, id);
    if (leaderboard != NULL)
      return &leaderboard->public_;
  }

  return NULL;
}

static const char* rc_client_get_leaderboard_bucket_label(uint8_t bucket_type)
{
  switch (bucket_type) {
    case RC_CLIENT_LEADERBOARD_BUCKET_INACTIVE: return "Inactive";
    case RC_CLIENT_LEADERBOARD_BUCKET_ACTIVE: return "Active";
    case RC_CLIENT_LEADERBOARD_BUCKET_UNSUPPORTED: return "Unsupported";
    case RC_CLIENT_LEADERBOARD_BUCKET_ALL: return "All";
    default: return "Unknown";
  }
}

static const char* rc_client_get_subset_leaderboard_bucket_label(uint8_t bucket_type, rc_client_game_info_t* game, rc_client_subset_info_t* subset)
{
  const char** ptr;
  const char* label;
  char* new_label;
  size_t new_label_len;

  switch (bucket_type) {
    case RC_CLIENT_LEADERBOARD_BUCKET_INACTIVE: ptr = &subset->inactive_label; break;
    case RC_CLIENT_LEADERBOARD_BUCKET_UNSUPPORTED: ptr = &subset->unsupported_label; break;
    case RC_CLIENT_LEADERBOARD_BUCKET_ALL: ptr = &subset->all_label; break;
    default: return rc_client_get_achievement_bucket_label(bucket_type);
  }

  if (*ptr)
    return *ptr;

  label = rc_client_get_leaderboard_bucket_label(bucket_type);
  new_label_len = strlen(subset->public_.title) + strlen(label) + 4;
  new_label = (char*)rc_buffer_alloc(&game->buffer, new_label_len);
  snprintf(new_label, new_label_len, "%s - %s", subset->public_.title, label);

  *ptr = new_label;
  return new_label;
}

static uint8_t rc_client_get_leaderboard_bucket(const rc_client_leaderboard_info_t* leaderboard, int grouping)
{
  switch (leaderboard->public_.state) {
    case RC_CLIENT_LEADERBOARD_STATE_TRACKING:
      return (grouping == RC_CLIENT_LEADERBOARD_LIST_GROUPING_NONE) ?
        RC_CLIENT_LEADERBOARD_BUCKET_ALL : RC_CLIENT_LEADERBOARD_BUCKET_ACTIVE;

    case RC_CLIENT_LEADERBOARD_STATE_DISABLED:
      return RC_CLIENT_LEADERBOARD_BUCKET_UNSUPPORTED;

    default:
      if (grouping == RC_CLIENT_LEADERBOARD_LIST_GROUPING_NONE)
        return RC_CLIENT_LEADERBOARD_BUCKET_ALL;

      if (leaderboard->public_.category == RC_CLIENT_LEADERBOARD_CATEGORY_UNPROMOTED)
        return RC_CLIENT_LEADERBOARD_BUCKET_UNPROMOTED;

      return RC_CLIENT_LEADERBOARD_BUCKET_INACTIVE;
  }
}

static int rc_client_is_subset_visible_in_leaderboard_list(const rc_client_subset_info_t* subset, const rc_client_leaderboard_list_params_t* params)
{
  if (!subset->active)
    return 0;

  return (params->subset_id == 0 || params->subset_id == subset->public_.id);
}

static int rc_client_is_leaderboard_visible_in_list(const rc_client_leaderboard_info_t* leaderboard, const rc_client_leaderboard_list_params_t* params)
{
  if (leaderboard->hidden)
    return 0;

  return (leaderboard->public_.category & params->category) != 0;
}

rc_client_leaderboard_list_t* rc_client_create_leaderboard_list(rc_client_t* client, int grouping)
{
  rc_client_leaderboard_list_params_t params;
  params.subset_id = 0;
  params.category = RC_CLIENT_LEADERBOARD_CATEGORY_PROMOTED;
  params.grouping = grouping;
  return rc_client_create_subset_leaderboard_list(client, &params);
}

rc_client_leaderboard_list_t* rc_client_create_subset_leaderboard_list(rc_client_t* client, const rc_client_leaderboard_list_params_t* params)
{
  rc_client_leaderboard_info_t* leaderboard;
  rc_client_leaderboard_info_t* stop;
  const rc_client_leaderboard_t** bucket_leaderboards;
  const rc_client_leaderboard_t** leaderboard_ptr;
  rc_client_leaderboard_bucket_t* bucket_ptr;
  rc_client_leaderboard_list_info_t* list;
  rc_client_subset_info_t* subset;
  const uint32_t list_size = RC_ALIGN(sizeof(*list));
  uint32_t bucket_counts[8];
  uint32_t num_buckets;
  uint32_t num_leaderboards;
  size_t buckets_size;
  uint8_t bucket_type;
  uint32_t num_subsets = 0;
  uint32_t i, j;
  const uint8_t shared_bucket_order[] = {
    RC_CLIENT_LEADERBOARD_BUCKET_ACTIVE
  };
  const uint8_t subset_bucket_order[] = {
    RC_CLIENT_LEADERBOARD_BUCKET_ALL,
    RC_CLIENT_LEADERBOARD_BUCKET_INACTIVE,
    RC_CLIENT_LEADERBOARD_BUCKET_UNPROMOTED,
    RC_CLIENT_LEADERBOARD_BUCKET_UNSUPPORTED
  };

  if (!client)
    return (rc_client_leaderboard_list_t*)calloc(1, list_size);

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client) {
    if (client->state.external_client->create_subset_leaderboard_list)
      return (rc_client_leaderboard_list_t*)client->state.external_client->create_subset_leaderboard_list(params);

    if (client->state.external_client->create_leaderboard_list)
      return rc_client_external_convert_v1_leaderboard_list(client,
        (rc_client_leaderboard_list_t*)client->state.external_client->create_leaderboard_list(params->grouping));
  }
#endif

  if (!client->game)
    return (rc_client_leaderboard_list_t*)calloc(1, list_size);

  memset(&bucket_counts, 0, sizeof(bucket_counts));

  rc_mutex_lock(&client->state.mutex);

  subset = client->game->subsets;
  for (; subset; subset = subset->next) {
    if (!rc_client_is_subset_visible_in_leaderboard_list(subset, params))
      continue;

    num_subsets++;
    leaderboard = subset->leaderboards;
    stop = leaderboard + subset->public_.num_leaderboards;
    for (; leaderboard < stop; ++leaderboard) {
      if (!rc_client_is_leaderboard_visible_in_list(leaderboard, params))
        continue;

      leaderboard->bucket = rc_client_get_leaderboard_bucket(leaderboard, params->grouping);
      bucket_counts[leaderboard->bucket]++;
    }
  }

  num_buckets = 0;
  num_leaderboards = 0;
  for (i = 0; i < sizeof(bucket_counts) / sizeof(bucket_counts[0]); ++i) {
    if (bucket_counts[i]) {
      int needs_split = 0;

      num_leaderboards += bucket_counts[i];

      if (num_subsets > 1) {
        for (j = 0; j < sizeof(subset_bucket_order) / sizeof(subset_bucket_order[0]); ++j) {
          if (subset_bucket_order[j] == i) {
            needs_split = 1;
            break;
          }
        }
      }

      if (!needs_split) {
        ++num_buckets;
        continue;
      }

      subset = client->game->subsets;
      for (; subset; subset = subset->next) {
        if (!rc_client_is_subset_visible_in_leaderboard_list(subset, params))
          continue;

        leaderboard = subset->leaderboards;
        stop = leaderboard + subset->public_.num_leaderboards;
        for (; leaderboard < stop; ++leaderboard) {
          if (leaderboard->bucket == i) {
            ++num_buckets;
            break;
          }
        }
      }
    }
  }

  buckets_size = RC_ALIGN(num_buckets * sizeof(rc_client_leaderboard_bucket_t));

  list = (rc_client_leaderboard_list_info_t*)malloc(list_size + buckets_size + num_leaderboards * sizeof(rc_client_leaderboard_t*));
  if (!list) {
    rc_mutex_unlock(&client->state.mutex);
    return NULL;
  }
  list->public_.buckets = bucket_ptr = (rc_client_leaderboard_bucket_t*)((uint8_t*)list + list_size);
  leaderboard_ptr = (const rc_client_leaderboard_t**)((uint8_t*)bucket_ptr + buckets_size);

  if (params->grouping == RC_CLIENT_LEADERBOARD_LIST_GROUPING_TRACKING) {
    for (i = 0; i < sizeof(shared_bucket_order) / sizeof(shared_bucket_order[0]); ++i) {
      bucket_type = shared_bucket_order[i];
      if (!bucket_counts[bucket_type])
        continue;

      bucket_leaderboards = leaderboard_ptr;
      for (subset = client->game->subsets; subset; subset = subset->next) {
        if (!rc_client_is_subset_visible_in_leaderboard_list(subset, params))
          continue;

        leaderboard = subset->leaderboards;
        stop = leaderboard + subset->public_.num_leaderboards;
        for (; leaderboard < stop; ++leaderboard) {
          if (leaderboard->bucket == bucket_type && rc_client_is_leaderboard_visible_in_list(leaderboard, params))
            *leaderboard_ptr++ = &leaderboard->public_;
        }
      }

      if (leaderboard_ptr > bucket_leaderboards) {
        bucket_ptr->leaderboards = bucket_leaderboards;
        bucket_ptr->num_leaderboards = (uint32_t)(leaderboard_ptr - bucket_leaderboards);
        bucket_ptr->subset_id = 0;
        bucket_ptr->label = rc_client_get_leaderboard_bucket_label(bucket_type);
        bucket_ptr->bucket_type = bucket_type;
        ++bucket_ptr;
      }
    }
  }

  for (subset = client->game->subsets; subset; subset = subset->next) {
    if (!rc_client_is_subset_visible_in_leaderboard_list(subset, params))
      continue;

    for (i = 0; i < sizeof(subset_bucket_order) / sizeof(subset_bucket_order[0]); ++i) {
      bucket_type = subset_bucket_order[i];
      if (!bucket_counts[bucket_type])
        continue;

      bucket_leaderboards = leaderboard_ptr;

      leaderboard = subset->leaderboards;
      stop = leaderboard + subset->public_.num_leaderboards;
      for (; leaderboard < stop; ++leaderboard) {
        if (leaderboard->bucket == bucket_type && rc_client_is_leaderboard_visible_in_list(leaderboard, params))
          *leaderboard_ptr++ = &leaderboard->public_;
      }

      if (leaderboard_ptr > bucket_leaderboards) {
        bucket_ptr->leaderboards = bucket_leaderboards;
        bucket_ptr->num_leaderboards = (uint32_t)(leaderboard_ptr - bucket_leaderboards);
        bucket_ptr->subset_id = (num_subsets > 1) ? subset->public_.id : 0;
        bucket_ptr->bucket_type = bucket_type;

        if (num_subsets > 1)
          bucket_ptr->label = rc_client_get_subset_leaderboard_bucket_label(bucket_type, client->game, subset);
        else
          bucket_ptr->label = rc_client_get_leaderboard_bucket_label(bucket_type);

        ++bucket_ptr;
      }
    }
  }

  rc_mutex_unlock(&client->state.mutex);

  list->destroy_func = NULL;
  list->public_.num_buckets = (uint32_t)(bucket_ptr - list->public_.buckets);
  return &list->public_;
}

void rc_client_destroy_leaderboard_list(rc_client_leaderboard_list_t* list)
{
  rc_client_leaderboard_list_info_t* info = (rc_client_leaderboard_list_info_t*)list;
  if (info->destroy_func)
    info->destroy_func(info);
  else
    free(list);
}

int rc_client_has_leaderboards(rc_client_t* client)
{
  rc_client_subset_info_t* subset;
  int result;
  uint32_t i;

  if (!client)
    return 0;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client && client->state.external_client->has_leaderboards)
    return client->state.external_client->has_leaderboards();
#endif

  if (!client->game)
    return 0;

  rc_mutex_lock(&client->state.mutex);

  result = 0;
  for (subset = client->game->subsets; subset; subset = subset->next)
  {
    if (!subset->active)
      continue;

    for (i = 0; i < subset->public_.num_leaderboards; ++i) {
      if (!subset->leaderboards[i].hidden) {
        result = 1;
        break;
      }
    }

    if (result)
      break;
  }

  rc_mutex_unlock(&client->state.mutex);

  return result;
}

typedef struct rc_client_fetch_leaderboard_entries_callback_data_t {
  rc_client_t* client;
  rc_client_fetch_leaderboard_entries_callback_t callback;
  void* callback_userdata;
  uint32_t leaderboard_id;
  rc_client_async_handle_t async_handle;
} rc_client_fetch_leaderboard_entries_callback_data_t;

static void rc_client_fetch_leaderboard_entries_callback(const rc_api_server_response_t* server_response, void* callback_data)
{
  rc_client_fetch_leaderboard_entries_callback_data_t* lbinfo_callback_data = (rc_client_fetch_leaderboard_entries_callback_data_t*)callback_data;
  rc_client_t* client = lbinfo_callback_data->client;
  rc_api_fetch_leaderboard_info_response_t lbinfo_response;
  const char* error_message;
  int result;

  result = rc_client_end_async(client, &lbinfo_callback_data->async_handle);
  if (result) {
    if (result != RC_CLIENT_ASYNC_DESTROYED) {
      RC_CLIENT_LOG_VERBOSE(client, "Fetch leaderbord entries aborted");
    }
    free(lbinfo_callback_data);
    return;
  }

  result = rc_api_process_fetch_leaderboard_info_server_response(&lbinfo_response, server_response);
  error_message = rc_client_server_error_message(&result, server_response->http_status_code, &lbinfo_response.response);
  if (error_message) {
    RC_CLIENT_LOG_ERR_FORMATTED(client, "Fetch leaderboard %u info failed: %s", lbinfo_callback_data->leaderboard_id, error_message);
    lbinfo_callback_data->callback(result, error_message, NULL, client, lbinfo_callback_data->callback_userdata);
  }
  else {
    rc_client_leaderboard_entry_list_info_t* info;
    const size_t list_size = sizeof(*info) + sizeof(rc_client_leaderboard_entry_t) * lbinfo_response.num_entries;
    size_t needed_size = list_size;
    uint32_t i;

    for (i = 0; i < lbinfo_response.num_entries; i++)
      needed_size += strlen(lbinfo_response.entries[i].username) + 1;

    info = (rc_client_leaderboard_entry_list_info_t*)malloc(needed_size);
    if (!info) {
      lbinfo_callback_data->callback(RC_OUT_OF_MEMORY, rc_error_str(RC_OUT_OF_MEMORY), NULL, client, lbinfo_callback_data->callback_userdata);
    }
    else {
      rc_client_leaderboard_entry_list_t* list = &info->public_;
      rc_client_leaderboard_entry_t* entry = list->entries = (rc_client_leaderboard_entry_t*)((uint8_t*)info + sizeof(*info));
      char* user = (char*)((uint8_t*)list + list_size);
      const rc_api_lboard_info_entry_t* lbentry = lbinfo_response.entries;
      const rc_api_lboard_info_entry_t* stop = lbentry + lbinfo_response.num_entries;
      const size_t logged_in_user_len = client->user.display_name ? strlen(client->user.display_name) + 1 : 0;
      info->destroy_func = NULL;
      list->user_index = -1;

      for (; lbentry < stop; ++lbentry, ++entry) {
        const size_t len = strlen(lbentry->username) + 1;
        entry->user = user;
        memcpy(user, lbentry->username, len);
        user += len;

        if (len == logged_in_user_len && memcmp(entry->user, client->user.display_name, len) == 0)
          list->user_index = (int)(entry - list->entries);

        entry->index = lbentry->index;
        entry->rank = lbentry->rank;
        entry->submitted = lbentry->submitted;

        rc_format_value(entry->display, sizeof(entry->display), lbentry->score, lbinfo_response.format);
      }

      list->num_entries = lbinfo_response.num_entries;
      list->total_entries = lbinfo_response.total_entries;

      lbinfo_callback_data->callback(RC_OK, NULL, list, client, lbinfo_callback_data->callback_userdata);
    }
  }

  rc_api_destroy_fetch_leaderboard_info_response(&lbinfo_response);
  free(lbinfo_callback_data);
}

static rc_client_async_handle_t* rc_client_begin_fetch_leaderboard_info(rc_client_t* client,
  const rc_api_fetch_leaderboard_info_request_t* lbinfo_request,
  rc_client_fetch_leaderboard_entries_callback_t callback, void* callback_userdata)
{
  rc_client_fetch_leaderboard_entries_callback_data_t* callback_data;
  rc_client_async_handle_t* async_handle;
  rc_api_request_t request;
  int result;
  const char* error_message;

  result = rc_api_init_fetch_leaderboard_info_request_hosted(&request, lbinfo_request, &client->state.host);

  if (result != RC_OK) {
    error_message = rc_error_str(result);
    callback(result, error_message, NULL, client, callback_userdata);
    return NULL;
  }

  callback_data = (rc_client_fetch_leaderboard_entries_callback_data_t*)calloc(1, sizeof(*callback_data));
  if (!callback_data) {
    callback(RC_OUT_OF_MEMORY, rc_error_str(RC_OUT_OF_MEMORY), NULL, client, callback_userdata);
    return NULL;
  }

  callback_data->client = client;
  callback_data->callback = callback;
  callback_data->callback_userdata = callback_userdata;
  callback_data->leaderboard_id = lbinfo_request->leaderboard_id;

  async_handle = &callback_data->async_handle;
  rc_client_begin_async(client, async_handle);
  client->callbacks.server_call(&request, rc_client_fetch_leaderboard_entries_callback, callback_data, client);
  rc_api_destroy_request(&request);

  return rc_client_async_handle_valid(client, async_handle) ? async_handle : NULL;
}

rc_client_async_handle_t* rc_client_begin_fetch_leaderboard_entries(rc_client_t* client, uint32_t leaderboard_id,
  uint32_t first_entry, uint32_t count, rc_client_fetch_leaderboard_entries_callback_t callback, void* callback_userdata)
{
  rc_api_fetch_leaderboard_info_request_t lbinfo_request;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client && client->state.external_client->begin_fetch_leaderboard_entries)
    return client->state.external_client->begin_fetch_leaderboard_entries(client, leaderboard_id, first_entry, count, callback, callback_userdata);
#endif

  memset(&lbinfo_request, 0, sizeof(lbinfo_request));
  lbinfo_request.leaderboard_id = leaderboard_id;
  lbinfo_request.first_entry = first_entry;
  lbinfo_request.count = count;

  return rc_client_begin_fetch_leaderboard_info(client, &lbinfo_request, callback, callback_userdata);
}

rc_client_async_handle_t* rc_client_begin_fetch_leaderboard_entries_around_user(rc_client_t* client, uint32_t leaderboard_id,
  uint32_t count, rc_client_fetch_leaderboard_entries_callback_t callback, void* callback_userdata)
{
  rc_api_fetch_leaderboard_info_request_t lbinfo_request;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client && client->state.external_client->begin_fetch_leaderboard_entries_around_user)
    return client->state.external_client->begin_fetch_leaderboard_entries_around_user(client, leaderboard_id, count, callback, callback_userdata);
#endif

  memset(&lbinfo_request, 0, sizeof(lbinfo_request));
  lbinfo_request.leaderboard_id = leaderboard_id;
  lbinfo_request.username = client->user.username;
  lbinfo_request.count = count;

  if (!lbinfo_request.username) {
    callback(RC_LOGIN_REQUIRED, rc_error_str(RC_LOGIN_REQUIRED), NULL, client, callback_userdata);
    return NULL;
  }

  return rc_client_begin_fetch_leaderboard_info(client, &lbinfo_request, callback, callback_userdata);
}

void rc_client_destroy_leaderboard_entry_list(rc_client_leaderboard_entry_list_t* list)
{
  rc_client_leaderboard_entry_list_info_t* info = (rc_client_leaderboard_entry_list_info_t*)list;
  if (info->destroy_func)
    info->destroy_func(info);
  else
    free(list);
}

int rc_client_leaderboard_entry_get_user_image_url(const rc_client_leaderboard_entry_t* entry, char buffer[], size_t buffer_size)
{
  if (!entry)
    return RC_INVALID_STATE;

  return rc_client_get_image_url(buffer, buffer_size, RC_IMAGE_TYPE_USER, entry->user);
}

/* ===== Rich Presence ===== */

int rc_client_has_rich_presence(rc_client_t* client)
{
  if (!client)
    return 0;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client && client->state.external_client->has_rich_presence)
    return client->state.external_client->has_rich_presence();
#endif

  if (!client->game || !client->game->runtime.richpresence || !client->game->runtime.richpresence->richpresence)
    return 0;

  return 1;
}

/* ===== Hashes ===== */

rc_client_game_hash_t* rc_client_find_game_hash(rc_client_t* client, const char* hash)
{
  rc_client_game_hash_t* game_hash;

  rc_mutex_lock(&client->state.mutex);
  game_hash = client->hashes;
  while (game_hash) {
    if (strcasecmp(game_hash->hash, hash) == 0)
      break;

    game_hash = game_hash->next;
  }

  if (!game_hash) {
    game_hash = (rc_client_game_hash_t*)rc_buffer_alloc(&client->state.buffer, sizeof(rc_client_game_hash_t));
    if (game_hash) {
      memset(game_hash, 0, sizeof(*game_hash));
      snprintf(game_hash->hash, sizeof(game_hash->hash), "%s", hash);
      game_hash->game_id = RC_CLIENT_UNKNOWN_GAME_ID;
      game_hash->next = client->hashes;
      client->hashes = game_hash;
    }
  }
  rc_mutex_unlock(&client->state.mutex);

  return game_hash;
}

void rc_client_add_game_hash(rc_client_t* client, const char* hash, uint32_t game_id)
{
  /* store locally, even if passing to external client */
  rc_client_game_hash_t* game_hash = rc_client_find_game_hash(client, hash);
  game_hash->game_id = game_id;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client && client->state.external_client->add_game_hash)
    client->state.external_client->add_game_hash(hash, game_id);
#endif
}

/* ===== Game ===== */

const rc_client_game_t* rc_client_get_game_info(const rc_client_t* client)
{
  if (!client)
    return NULL;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client) {
    if (client->state.external_client->get_game_info_v3)
      return client->state.external_client->get_game_info_v3();

    if (client->state.external_client->get_game_info)
      return rc_client_external_convert_v1_game(client, client->state.external_client->get_game_info());
  }
#endif

  return client->game ? &client->game->public_ : NULL;
}

int rc_client_game_get_image_url(const rc_client_game_t* game, char buffer[], size_t buffer_size)
{
  if (!game)
    return RC_INVALID_STATE;

  if (game->badge_url) {
    const size_t len = strlen(game->badge_url);
    if (len >= buffer_size)
      return RC_INSUFFICIENT_BUFFER;

    memcpy(buffer, game->badge_url, len + 1);
    return RC_OK;
  }

  return rc_client_get_image_url(buffer, buffer_size, RC_IMAGE_TYPE_GAME, game->badge_name);
}

/* ===== Subsets ===== */

const rc_client_subset_t* rc_client_get_subset_info(rc_client_t* client, uint32_t subset_id)
{
  rc_client_subset_info_t* subset;

  if (!client)
    return NULL;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client) {
    if (client->state.external_client->get_subset_info_v3)
      return client->state.external_client->get_subset_info_v3(subset_id);

    if (client->state.external_client->get_subset_info)
      return rc_client_external_convert_v1_subset(client, client->state.external_client->get_subset_info(subset_id));
  }
#endif

  if (!client->game)
    return NULL;

  for (subset = client->game->subsets; subset; subset = subset->next) {
    if (subset->public_.id == subset_id)
      return &subset->public_;
  }

  return NULL;
}

rc_client_subset_list_t* rc_client_create_subset_list(rc_client_t* client)
{
  rc_client_subset_list_info_t* list;
  const rc_client_subset_info_t* subset;
  const rc_client_subset_t** subset_ptr;
  const uint32_t list_size = RC_ALIGN(sizeof(*list));
  uint32_t num_subsets = 0;

  if (!client)
    return (rc_client_subset_list_t*)calloc(1, list_size);

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client && client->state.external_client->create_subset_list)
    return (rc_client_subset_list_t*)client->state.external_client->create_subset_list();
#endif

  if (!client->game)
    return (rc_client_subset_list_t*)calloc(1, list_size);

  rc_mutex_lock(&client->state.mutex);

  subset = client->game->subsets;
  for (; subset; subset = subset->next) {
    if (subset->active)
      num_subsets++;
  }

  list = (rc_client_subset_list_info_t*)malloc(list_size + num_subsets * sizeof(rc_client_subset_t*));
  if (!list)
    return NULL;
  list->public_.subsets = subset_ptr = (const rc_client_subset_t**)((uint8_t*)list + list_size);

  subset = client->game->subsets;
  for (; subset; subset = subset->next) {
    if (subset->active)
      *subset_ptr++ = &subset->public_;
  }

  rc_mutex_unlock(&client->state.mutex);

  list->destroy_func = NULL;
  list->public_.num_subsets = (uint32_t)(subset_ptr - list->public_.subsets);
  return &list->public_;
}

void rc_client_destroy_subset_list(rc_client_subset_list_t* list)
{
  rc_client_subset_list_info_t* info = (rc_client_subset_list_info_t*)list;
  if (info->destroy_func)
    info->destroy_func(info);
  else
    free(list);
}

/* ===== User ===== */

static void rc_client_subset_get_user_game_summary(const rc_client_t* client,
  const rc_client_subset_info_t* subset, rc_client_user_game_summary_t* summary)
{
  rc_client_achievement_info_t* achievement = subset->achievements;
  rc_client_achievement_info_t* stop = achievement + subset->public_.num_achievements;
  time_t last_unlock_time = 0;
  time_t last_progression_time = 0;
  time_t first_win_time = 0;
  int num_progression_achievements = 0;
  int num_win_achievements = 0;
  int num_unlocked_progression_achievements = 0;
  const uint8_t unlock_bit = (client->state.hardcore) ?
    RC_CLIENT_ACHIEVEMENT_UNLOCKED_HARDCORE : RC_CLIENT_ACHIEVEMENT_UNLOCKED_CASUAL;

  rc_mutex_lock((rc_mutex_t*)&client->state.mutex); /* remove const cast for mutex access */

  for (; achievement < stop; ++achievement) {
    switch (achievement->public_.category) {
      case RC_CLIENT_ACHIEVEMENT_CATEGORY_PROMOTED:
        if (achievement->public_.id >= RC_CLIENT_ACHIEVEMENT_WARNING_ID) {
          /* ignore warning achievements */
          continue;
        }

        ++summary->num_promoted_achievements;
        summary->points_available += achievement->public_.points;

        if (achievement->public_.unlocked & unlock_bit) {
          ++summary->num_unlocked_achievements;
          summary->points_unlocked += achievement->public_.points;

          if (achievement->public_.unlock_time > last_unlock_time)
            last_unlock_time = achievement->public_.unlock_time;

          if (achievement->public_.type == RC_CLIENT_ACHIEVEMENT_TYPE_PROGRESSION) {
            ++num_unlocked_progression_achievements;
            if (achievement->public_.unlock_time > last_progression_time)
              last_progression_time = achievement->public_.unlock_time;
          }
          else if (achievement->public_.type == RC_CLIENT_ACHIEVEMENT_TYPE_WIN) {
            if (first_win_time == 0 || achievement->public_.unlock_time < first_win_time)
              first_win_time = achievement->public_.unlock_time;
          }
        }

        if (achievement->public_.type == RC_CLIENT_ACHIEVEMENT_TYPE_PROGRESSION)
          ++num_progression_achievements;
        else if (achievement->public_.type == RC_CLIENT_ACHIEVEMENT_TYPE_WIN)
          ++num_win_achievements;

        if (achievement->public_.bucket == RC_CLIENT_ACHIEVEMENT_BUCKET_UNSUPPORTED)
          ++summary->num_unsupported_achievements;

        break;

      case RC_CLIENT_ACHIEVEMENT_CATEGORY_UNPROMOTED:
        ++summary->num_unpromoted_achievements;
        summary->points_unpromoted += achievement->public_.points;

        if (achievement->public_.unlocked & unlock_bit) {
          ++summary->num_unlocked_unpromoted_achievements;
          summary->points_unlocked_unpromoted += achievement->public_.points;
        }
        break;

      default:
        continue;
    }
  }

  rc_mutex_unlock((rc_mutex_t*)&client->state.mutex); /* remove const cast for mutex access */

  if (summary->num_unlocked_achievements == summary->num_promoted_achievements)
    summary->completed_time = last_unlock_time;

  if ((first_win_time || num_win_achievements == 0) && num_unlocked_progression_achievements == num_progression_achievements)
    summary->beaten_time = (first_win_time > last_progression_time) ? first_win_time : last_progression_time;
}

void rc_client_get_user_game_summary(const rc_client_t* client, rc_client_user_game_summary_t* summary)
{
  if (!summary)
    return;

  /* External clients may only populate an older prefix of the structure. */
  memset(summary, 0, sizeof(*summary));
  if (!client)
    return;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client) {
    if (client->state.external_client->get_user_game_summary_v5) {
      client->state.external_client->get_user_game_summary_v5(summary);
      return;
    }
    if (client->state.external_client->get_user_game_summary) {
      client->state.external_client->get_user_game_summary(summary);
      return;
    }
  }
#endif

  if (rc_client_is_game_loaded(client))
    rc_client_subset_get_user_game_summary(client, client->game->subsets, summary);
}

void rc_client_get_user_subset_summary(const rc_client_t* client, uint32_t subset_id, rc_client_user_game_summary_t* summary)
{
  if (!summary)
    return;

  /* External clients may only populate an older prefix of the structure. */
  memset(summary, 0, sizeof(*summary));
  if (!client || !subset_id)
    return;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client && client->state.external_client->get_user_subset_summary) {
    client->state.external_client->get_user_subset_summary(subset_id, summary);
    return;
  }
#endif

  if (rc_client_is_game_loaded(client)) {
    const rc_client_subset_info_t* subset = client->game->subsets;
    for (; subset; subset = subset->next) {
      if (subset->public_.id == subset_id) {
        rc_client_subset_get_user_game_summary(client, subset, summary);
        break;
      }
    }
  }
}
