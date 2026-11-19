#ifndef RC_CLIENT_GAME_H
#define RC_CLIENT_GAME_H

#include "rc_client.h"
#include "rc_runtime.h"
#include "rc_util.h"

#include "rc_client_do_frame.h"

#include <stdint.h>

RC_BEGIN_C_DECLS

struct rc_lboard_t;  /* rc_lboard.h */
struct rc_trigger_t; /* rc_trigger.h */

/*****************************************************************************\
| Achievements                                                                |
\*****************************************************************************/

enum {
  RC_CLIENT_ACHIEVEMENT_PENDING_EVENT_NONE = 0,
  RC_CLIENT_ACHIEVEMENT_PENDING_EVENT_TRIGGERED = (1 << 1),
  RC_CLIENT_ACHIEVEMENT_PENDING_EVENT_CHALLENGE_INDICATOR_SHOW = (1 << 2),
  RC_CLIENT_ACHIEVEMENT_PENDING_EVENT_CHALLENGE_INDICATOR_HIDE = (1 << 3),
  RC_CLIENT_ACHIEVEMENT_PENDING_EVENT_UPDATE = (1 << 4) /* not a real event, just triggers update */
};

#define RC_CLIENT_ACHIEVEMENT_WARNING_ID 101000001

typedef struct rc_client_achievement_info_t {
  /* The public information about the achievement. */
  rc_client_achievement_t public_;

  /* The parsed/compiled definition of the achievement. */
  struct rc_trigger_t* trigger;
  /* The checksum of the achievement definition. */
  uint8_t md5[16];

  /* When the user unlocked the achievement in hardcore mode. */
  time_t unlock_time_hardcore;
  /* When the user unlocked the achievement in casual mode. */
  time_t unlock_time_casual;

  /* Recent changes to the achievement state that need to be processed. (RC_CLIENT_ACHIEVEMENT_PENDING_EVENT_*) */
  uint8_t pending_events;

  /* Author of the achievement. */
  const char* author;
  /* When the achievement was created. */
  time_t created_time;
  /* When the achievement was last updated. */
  time_t updated_time;
} rc_client_achievement_info_t;

struct rc_client_achievement_list_info_t;
typedef void (RC_CCONV* rc_client_destroy_achievement_list_func_t)(struct rc_client_achievement_list_info_t* list);

typedef struct rc_client_achievement_list_info_t {
  rc_client_achievement_list_t public_;
  rc_client_destroy_achievement_list_func_t destroy_func;
} rc_client_achievement_list_info_t;

enum {
  RC_CLIENT_PROGRESS_TRACKER_ACTION_NONE,
  RC_CLIENT_PROGRESS_TRACKER_ACTION_SHOW,
  RC_CLIENT_PROGRESS_TRACKER_ACTION_UPDATE,
  RC_CLIENT_PROGRESS_TRACKER_ACTION_HIDE
};

typedef struct rc_client_progress_tracker_t {
  /* The achievement associated to the tracker. */
  const rc_client_achievement_info_t* achievement;
  /* The current amount of progress made towards completing the achievement. */
  float progress;

  /* The scheduled callback for hiding the tracker - can be used to change the duration that the tracker is displayed. */
  rc_client_scheduled_callback_data_t* hide_callback;

  /* Recent changes to the tracker state that need to be processed. (RC_CLIENT_PROGRESS_TRACKER_ACTION_*) */
  uint8_t action;
} rc_client_progress_tracker_t;

/*****************************************************************************\
| Leaderboard Trackers                                                        |
\*****************************************************************************/

enum {
  RC_CLIENT_LEADERBOARD_TRACKER_PENDING_EVENT_NONE = 0,
  RC_CLIENT_LEADERBOARD_TRACKER_PENDING_EVENT_UPDATE = (1 << 1),
  RC_CLIENT_LEADERBOARD_TRACKER_PENDING_EVENT_SHOW = (1 << 2),
  RC_CLIENT_LEADERBOARD_TRACKER_PENDING_EVENT_HIDE = (1 << 3)
};

typedef struct rc_client_leaderboard_tracker_info_t {
  /* The public information about the leaderboard tracker. */
  rc_client_leaderboard_tracker_t public_;

  /* The next leaderboard tracker in the list. */
  struct rc_client_leaderboard_tracker_info_t* next;

  /* The current unformatted value being tracked. */
  int32_t raw_value;

  /* A checksum of the leaderboard value definition which can be used to collapse similar trackers. */
  uint32_t value_djb2;

  /* The format to apply to the raw_value before displaying. (RC_FORMAT_*) */
  uint8_t format;
  /* Recent changes to the tracker state that need to be processed. (RC_CLIENT_LEADERBOARD_TRACKER_PENDING_EVENT_*) */
  uint8_t pending_events;
  /* The number of active leaderboards associated to this tracker. */
  uint8_t reference_count;
  /* Non-zero if the raw_value comes from a hit count instead of from reading memory. Prevents sharing the tracker if the value_djb2 matches in case the leaderboards didn't start on the same frame. */
  uint8_t value_from_hits;
} rc_client_leaderboard_tracker_info_t;

/*****************************************************************************\
| Leaderboards                                                                |
\*****************************************************************************/

enum {
  RC_CLIENT_LEADERBOARD_PENDING_EVENT_NONE = 0,
  RC_CLIENT_LEADERBOARD_PENDING_EVENT_STARTED = (1 << 1),
  RC_CLIENT_LEADERBOARD_PENDING_EVENT_FAILED = (1 << 2),
  RC_CLIENT_LEADERBOARD_PENDING_EVENT_SUBMITTED = (1 << 3)
};

typedef struct rc_client_leaderboard_info_t {
  /* The public information about the leaderboard. */
  rc_client_leaderboard_t public_;

  /* The parsed/compiled definition of the leaderboard. */
  struct rc_lboard_t* lboard;
  /* The checksum of the leaderboard definition. */
  uint8_t md5[16];

  /* The tracker for the leaderboard (if active). */
  rc_client_leaderboard_tracker_info_t* tracker;
  /* A checksum of the leaderboard value definition which can be used to collapse similar trackers. */
  uint32_t value_djb2;
  /* The current unformatted value for the leaderboard. */
  int32_t value;

  /* The format to apply to the raw_value before displaying. (RC_FORMAT_*) */
  uint8_t format;
  /* Recent changes to the leaderboard state that need to be processed. (RC_CLIENT_LEADERBOARD_PENDING_EVENT_*) */
  uint8_t pending_events;
  /* The bucket to categorize the leaderboard into (RC_CLIENT_LEADERBOARD_BUCKET_*) */
  uint8_t bucket;
  /* Non-zero if the leaderboard should be hidden from the user. */
  uint8_t hidden;
} rc_client_leaderboard_info_t;

struct rc_client_leaderboard_list_info_t;
typedef void (RC_CCONV* rc_client_destroy_leaderboard_list_func_t)(struct rc_client_leaderboard_list_info_t* list);

typedef struct rc_client_leaderboard_list_info_t {
  rc_client_leaderboard_list_t public_;
  rc_client_destroy_leaderboard_list_func_t destroy_func;
} rc_client_leaderboard_list_info_t;

struct rc_client_leaderboard_entry_list_info_t;
typedef void (RC_CCONV* rc_client_destroy_leaderboard_entry_list_func_t)(struct rc_client_leaderboard_entry_list_info_t* list);

typedef struct rc_client_leaderboard_entry_list_info_t {
  rc_client_leaderboard_entry_list_t public_;
  rc_client_destroy_leaderboard_entry_list_func_t destroy_func;
} rc_client_leaderboard_entry_list_info_t;

uint8_t rc_client_map_leaderboard_format(int format);

/*****************************************************************************\
| Subsets                                                                     |
\*****************************************************************************/

enum {
  RC_CLIENT_SUBSET_PENDING_EVENT_NONE = 0,
  RC_CLIENT_SUBSET_PENDING_EVENT_ACHIEVEMENT = (1 << 1),
  RC_CLIENT_SUBSET_PENDING_EVENT_LEADERBOARD = (1 << 2)
};

enum {
  RC_CLIENT_MASTERY_STATE_NONE,
  RC_CLIENT_MASTERY_STATE_PENDING,
  RC_CLIENT_MASTERY_STATE_SHOWN
};

typedef struct rc_client_subset_info_t {
  /* The public information about the subset. */
  rc_client_subset_t public_;

  /* The achievements in the subset. */
  rc_client_achievement_info_t* achievements;
  /* The leaderboards in the subset. */
  rc_client_leaderboard_info_t* leaderboards;

  /* The next subset in the list. */
  struct rc_client_subset_info_t* next;

  /* A label to display for the All Achievements bucket for the subset. */
  const char* all_label;
  /* A label to display for the Inactive Achievements bucket for the subset. */
  const char* inactive_label;
  /* A label to display for the Locked Achievements bucket for the subset. */
  const char* locked_label;
  /* A label to display for the Unlocked Achievements bucket for the subset. */
  const char* unlocked_label;
  /* A label to display for the Unlocked in Casual Achievements bucket for the subset. */
  const char* unlocked_casual_label;
  /* A label to display for the Unpromoted Achievements bucket for the subset. */
  const char* unpromoted_label;
  /* A label to display for the Unsupported Achievements bucket for the subset. */
  const char* unsupported_label;

  /* Non-zero if the subset is active */
  uint8_t active;
  /* The mastery state of the subset (RC_CLIENT_MASTERY_STATE_*) */
  uint8_t mastery;

  /* Recent changes to the subset state that need to be processed. (RC_CLIENT_SUBSET_PENDING_EVENT_*) */
  uint8_t pending_events;
} rc_client_subset_info_t;

struct rc_client_subset_list_info_t;
typedef void (RC_CCONV* rc_client_destroy_subset_list_func_t)(struct rc_client_subset_list_info_t* list);

typedef struct rc_client_subset_list_info_t {
  rc_client_subset_list_t public_;
  rc_client_destroy_subset_list_func_t destroy_func;
} rc_client_subset_list_info_t;

const rc_client_achievement_t* rc_client_subset_get_achievement_info(rc_client_t* client, rc_client_subset_info_t* subset, uint32_t id);
const rc_client_leaderboard_info_t* rc_client_subset_get_leaderboard_info(const rc_client_subset_info_t* subset, uint32_t id);

/*****************************************************************************\
| Hashes                                                                      |
\*****************************************************************************/

typedef struct rc_client_game_hash_t {
  /* The hash (32-character hex string). */
  char hash[33];
  /* Non-zero if the server was unable to resolve the hash. */
  uint8_t is_unknown;
  /* The unique identifier of the game associated to the hash. */
  uint32_t game_id;
  /* The next hash in the list. */
  struct rc_client_game_hash_t* next;
} rc_client_game_hash_t;

typedef struct rc_client_media_hash_t {
  /* The hash associated to the media. */
  rc_client_game_hash_t* game_hash;
  /* The next media hash in the list. */
  struct rc_client_media_hash_t* next;
  /* The checksum of the path to the media. */
  uint32_t path_djb2;
} rc_client_media_hash_t;

/*****************************************************************************\
| Game                                                                        |
\*****************************************************************************/

enum {
  RC_CLIENT_GAME_PENDING_EVENT_NONE = 0,
  RC_CLIENT_GAME_PENDING_EVENT_LEADERBOARD_TRACKER = (1 << 1),
  RC_CLIENT_GAME_PENDING_EVENT_UPDATE_ACTIVE_ACHIEVEMENTS = (1 << 2),
  RC_CLIENT_GAME_PENDING_EVENT_PROGRESS_TRACKER = (1 << 3)
};

typedef struct rc_client_game_info_t {
  /* The public information about the game. */
  rc_client_game_t public_;
  /* The list of visible leaderboard trackers. */
  rc_client_leaderboard_tracker_info_t* leaderboard_trackers;
  /* Information about the achievement progress tracker. */
  rc_client_progress_tracker_t progress_tracker;

  /* The subsets associated to the game. */
  rc_client_subset_info_t* subsets;

  /* The media hashes for the game. */
  rc_client_media_hash_t* media_hash;

  /* The runtime for the game. */
  rc_runtime_t runtime;

  /* The maximum valid address for the system associated to the game. */
  uint32_t max_valid_address;

  /* Non-zero if a reset was requested but not yet processed. */
  uint8_t waiting_for_reset;

  /* Recent changes to the game state that need to be processed. (RC_CLIENT_GAME_PENDING_EVENT_*) */
  uint8_t pending_events;

  /* Memory pool for additional data needed by the game. */
  rc_buffer_t buffer;
} rc_client_game_info_t;

/*****************************************************************************\
| Functions                                                                   |
\*****************************************************************************/

/**
 * Activates/deactivates achievements earned in hardcode mode but not casual mode when hardcore mode changes.
 */
void rc_client_toggle_hardcore_achievements(rc_client_game_info_t* game, rc_client_t* client, uint8_t active_bit);

/**
 * Synchronizes the active achievements in the runtime.
 */
void rc_client_update_active_achievements(rc_client_game_info_t* game);

/**
 * Synchronizes the active leaderboards in the runtime.
 */
void rc_client_update_active_leaderboards(rc_client_game_info_t* game);

/**
 * Updates the measured value and bucket for an achievement.
 */
void rc_client_update_achievement_display_information(rc_client_t* client, rc_client_achievement_info_t* achievement, time_t recent_unlock_time);

/**
 * Activates achievements based on the current hardcore mode and encore mode settings.
 */
void rc_client_activate_achievements(struct rc_client_game_info_t* game, rc_client_t* client);

/**
 * Activates leaderboards based on the current hardcore mode setting.
 */
void rc_client_activate_leaderboards(struct rc_client_game_info_t* game, rc_client_t* client);

/**
 * Deactivates leaderboards when hardcore mode is disabled.
 */
void rc_client_deactivate_leaderboards(rc_client_game_info_t* game, rc_client_t* client);

/**
 * Finds the hash entry for a given hash. Returns NULL if not found.
 */
rc_client_game_hash_t* rc_client_find_game_hash(rc_client_t* client, const char* hash);

/**
 * Injects a hash->game_id mapping into the known hashes list.
 */
void rc_client_add_game_hash(rc_client_t* client, const char* hash, uint32_t game_id);

RC_END_C_DECLS

#endif /* RC_CLIENT_GAME_H */
