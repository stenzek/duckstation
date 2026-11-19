#ifndef RC_CLIENT_DO_FRAME_H
#define RC_CLIENT_DO_FRAME_H

#include "rc_client_callbacks.h" /* rc_clock_t */

#include <stdint.h>

RC_BEGIN_C_DECLS

/*****************************************************************************\
| Enums                                                                       |
\*****************************************************************************/

#define RC_CLIENT_RECENT_UNLOCK_DELAY_SECONDS (10 * 60) /* ten minutes */

#define RC_MINIMUM_UNPAUSED_FRAMES 20
#define RC_PAUSE_DECAY_MULTIPLIER 4

/*****************************************************************************\
| Structures                                                                  |
\*****************************************************************************/

struct rc_client_t;                  /* rc_client_internal.h */
struct rc_client_game_info_t;        /* rc_client_internal.h */
struct rc_client_leaderboard_info_t; /* rc_client_internal.h */

struct rc_client_scheduled_callback_data_t;
typedef void (RC_CCONV* rc_client_scheduled_callback_t)(struct rc_client_scheduled_callback_data_t* callback_data, struct rc_client_t* client, rc_clock_t now);

typedef struct rc_client_scheduled_callback_data_t
{
  /* The rc_clock_t when the callback should be called. */
  rc_clock_t when;

  /* The unique identifier of the record associated to the callback. */
  uint32_t related_id;

  /* The function to call when the target time is reached. */
  rc_client_scheduled_callback_t callback;

  /* Additional data to pass to the callback function. */
  void* data;

  /* Pointer to the next scheduled callback. */
  struct rc_client_scheduled_callback_data_t* next;
} rc_client_scheduled_callback_data_t;

/*****************************************************************************\
| Functions                                                                   |
\*****************************************************************************/

/**
 * Schedules the periodic ping for the provided game.
 */
void rc_client_schedule_ping(struct rc_client_t* client, struct rc_client_game_info_t* game);

/**
 * Stops the periodic ping for the provided game.
 */
void rc_client_unschedule_ping(struct rc_client_t* client, struct rc_client_game_info_t* game);

/**
 * Schedules a callback.
 */
void rc_client_schedule_callback(struct rc_client_t* client, struct rc_client_scheduled_callback_data_t* scheduled_callback);

/**
 * Determines if a server request retry is scheduled.
 */
int rc_client_has_server_retry_scheduled_callback(struct rc_client_t* client);

/**
 * Determines if an award achievement server request retry is scheduled.
 */
int rc_client_is_award_achievement_pending(const struct rc_client_t* client, uint32_t achievement_id);

/**
 * Immediates hides the achievement progress tracker widget (if visible).
 */
void rc_client_hide_progress_tracker(struct rc_client_t* client, struct rc_client_game_info_t* game);

/**
 * Allocates a tracker for displaying a leaderboard value.
 * An existing tracker may be associated to the leaderboard if it shares the same value definition.
 */
void rc_client_allocate_leaderboard_tracker(struct rc_client_game_info_t* game, struct rc_client_leaderboard_info_t* leaderboard);

/**
 * Releases a reference to a tracker for displaying a leaderboard value.
 * If the reference count becomes 0, the tracker will be hidden.
 */
void rc_client_release_leaderboard_tracker(struct rc_client_game_info_t* game, struct rc_client_leaderboard_info_t* leaderboard);

/**
 * Updates the current value of a leaderboard tracker.
 */
void rc_client_update_leaderboard_tracker(struct rc_client_game_info_t* game, struct rc_client_leaderboard_info_t* leaderboard);

/**
 * Dispatches any pending events.
 */
void rc_client_raise_pending_events(struct rc_client_t* client, struct rc_client_game_info_t* game);

/**
 * Discards any pending events.
 */
void rc_client_reset_pending_events(struct rc_client_t* client);

/**
 * Resets the hit counts on all assets, and queues events to hide any associated trackers.
 */
void rc_client_reset_all(struct rc_client_t* client);

/**
 * Marks all UI elements to be hidden.
 */
void rc_client_game_mark_ui_to_be_hidden(struct rc_client_t* client, struct rc_client_game_info_t* game);

/**
 * Attempts to read each direct address to ensure the emulator supports them.
 */
void rc_client_validate_addresses(struct rc_client_game_info_t* game, struct rc_client_t* client);

RC_END_C_DECLS

#endif /* RC_CLIENT_DO_FRAME_H */
