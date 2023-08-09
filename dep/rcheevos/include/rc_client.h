#ifndef RC_CLIENT_H
#define RC_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "rc_api_request.h"
#include "rc_error.h"

#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* implementation abstracted in rc_client_internal.h */
typedef struct rc_client_t rc_client_t;
typedef struct rc_client_async_handle_t rc_client_async_handle_t;

/*****************************************************************************\
| Callbacks                                                                   |
\*****************************************************************************/

/**
 * Callback used to read num_bytes bytes from memory starting at address into buffer.
 * Returns the number of bytes read. A return value of 0 indicates the address was invalid.
 */
typedef uint32_t (*rc_client_read_memory_func_t)(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* client);

/**
 * Internal method passed to rc_client_server_call_t to process the server response.
 */
typedef void (*rc_client_server_callback_t)(const rc_api_server_response_t* server_response, void* callback_data);

/**
 * Callback used to issue a request to the server.
 */
typedef void (*rc_client_server_call_t)(const rc_api_request_t* request, rc_client_server_callback_t callback, void* callback_data, rc_client_t* client);

/**
 * Generic callback for asynchronous eventing.
 */
typedef void (*rc_client_callback_t)(int result, const char* error_message, rc_client_t* client, void* userdata);

/**
 * Callback for logging or displaying a message.
 */
typedef void (*rc_client_message_callback_t)(const char* message, const rc_client_t* client);

/**
 * Marks an async process as aborted. The associated callback will not be called.
 */
void rc_client_abort_async(rc_client_t* client, rc_client_async_handle_t* async_handle);

/*****************************************************************************\
| Runtime                                                                     |
\*****************************************************************************/

/**
 * Creates a new rc_client_t object.
 */
rc_client_t* rc_client_create(rc_client_read_memory_func_t read_memory_function, rc_client_server_call_t server_call_function);

/**
 * Releases resources associated to a rc_client_t object.
 * Pointer will no longer be valid after making this call.
 */
void rc_client_destroy(rc_client_t* client);

/**
 * Sets whether hardcore is enabled (on by default).
 * Can be called with a game loaded.
 * Enabling hardcore with a game loaded will raise an RC_CLIENT_EVENT_RESET
 * event. Processing will be disabled until rc_client_reset is called.
 */
void rc_client_set_hardcore_enabled(rc_client_t* client, int enabled);

/**
 * Gets whether hardcore is enabled (on by default).
 */
int rc_client_get_hardcore_enabled(const rc_client_t* client);

/**
 * Sets whether encore mode is enabled (off by default).
 * Evaluated when loading a game. Has no effect while a game is loaded.
 */
void rc_client_set_encore_mode_enabled(rc_client_t* client, int enabled);

/**
 * Gets whether encore mode is enabled (off by default).
 */
int rc_client_get_encore_mode_enabled(const rc_client_t* client);

/**
 * Sets whether unofficial achievements should be loaded.
 * Evaluated when loading a game. Has no effect while a game is loaded.
 */
void rc_client_set_unofficial_enabled(rc_client_t* client, int enabled);

/**
 * Gets whether unofficial achievements should be loaded.
 */
int rc_client_get_unofficial_enabled(const rc_client_t* client);

/**
 * Sets whether spectator mode is enabled (off by default).
 * If enabled, events for achievement unlocks and leaderboard submissions will be
 * raised, but server calls to actually perform the unlock/submit will not occur.
 * Can be modified while a game is loaded. Evaluated at unlock/submit time.
 * Cannot be modified if disabled before a game is loaded.
 */
void rc_client_set_spectator_mode_enabled(rc_client_t* client, int enabled);

/**
 * Gets whether spectator mode is enabled (off by default).
 */
int rc_client_get_spectator_mode_enabled(const rc_client_t* client);

/**
 * Attaches client-specific data to the runtime.
 */
void rc_client_set_userdata(rc_client_t* client, void* userdata);

/**
 * Gets the client-specific data attached to the runtime.
 */
void* rc_client_get_userdata(const rc_client_t* client);

/**
 * Sets the name of the server to use.
 */
void rc_client_set_host(const rc_client_t* client, const char* hostname);

/*****************************************************************************\
| Logging                                                                     |
\*****************************************************************************/

/**
 * Sets the logging level and provides a callback to be called to do the logging.
 */
void rc_client_enable_logging(rc_client_t* client, int level, rc_client_message_callback_t callback);
enum
{
  RC_CLIENT_LOG_LEVEL_NONE = 0,
  RC_CLIENT_LOG_LEVEL_ERROR = 1,
  RC_CLIENT_LOG_LEVEL_WARN = 2,
  RC_CLIENT_LOG_LEVEL_INFO = 3,
  RC_CLIENT_LOG_LEVEL_VERBOSE = 4
};

/*****************************************************************************\
| User                                                                        |
\*****************************************************************************/

/**
 * Attempt to login a user.
 */
rc_client_async_handle_t* rc_client_begin_login_with_password(rc_client_t* client,
    const char* username, const char* password,
    rc_client_callback_t callback, void* callback_userdata);

/**
 * Attempt to login a user.
 */
rc_client_async_handle_t* rc_client_begin_login_with_token(rc_client_t* client,
    const char* username, const char* token,
    rc_client_callback_t callback, void* callback_userdata);

/**
 * Logout the user.
 */
void rc_client_logout(rc_client_t* client);

typedef struct rc_client_user_t {
  const char* display_name;
  const char* username;
  const char* token;
  uint32_t score;
  uint32_t score_softcore;
  uint32_t num_unread_messages;
} rc_client_user_t;

/**
 * Gets information about the logged in user. Will return NULL if the user is not logged in.
 */
const rc_client_user_t* rc_client_get_user_info(const rc_client_t* client);

/**
 * Gets the URL for the user's profile picture.
 * Returns RC_OK on success.
 */
int rc_client_user_get_image_url(const rc_client_user_t* user, char buffer[], size_t buffer_size);

typedef struct rc_client_user_game_summary_t
{
  uint32_t num_core_achievements;
  uint32_t num_unofficial_achievements;
  uint32_t num_unlocked_achievements;
  uint32_t num_unsupported_achievements;

  uint32_t points_core;
  uint32_t points_unlocked;
} rc_client_user_game_summary_t;

/**
 * Gets a breakdown of the number of achievements in the game, and how many the user has unlocked.
 * Used for the "You have unlocked X of Y achievements" message shown when the game starts.
 */
void rc_client_get_user_game_summary(const rc_client_t* client, rc_client_user_game_summary_t* summary);

/*****************************************************************************\
| Game                                                                        |
\*****************************************************************************/

/**
 * Start loading an unidentified game.
 */
rc_client_async_handle_t* rc_client_begin_identify_and_load_game(rc_client_t* client,
    uint32_t console_id, const char* file_path,
    const uint8_t* data, size_t data_size,
    rc_client_callback_t callback, void* callback_userdata);

/**
 * Start loading a game.
 */
rc_client_async_handle_t* rc_client_begin_load_game(rc_client_t* client, const char* hash,
    rc_client_callback_t callback, void* callback_userdata);

/**
 * Unloads the current game.
 */
void rc_client_unload_game(rc_client_t* client);

typedef struct rc_client_game_t {
  uint32_t id;
  uint32_t console_id;
  const char* title;
  const char* hash;
  const char* badge_name;
} rc_client_game_t;

/**
 * Get information about the current game. Returns NULL if no game is loaded.
 */
const rc_client_game_t* rc_client_get_game_info(const rc_client_t* client);

/**
 * Gets the URL for the game image.
 * Returns RC_OK on success.
 */
int rc_client_game_get_image_url(const rc_client_game_t* game, char buffer[], size_t buffer_size);

/**
 * Changes the active disc in a multi-disc game.
 */
rc_client_async_handle_t* rc_client_begin_change_media(rc_client_t* client, const char* file_path,
    const uint8_t* data, size_t data_size, rc_client_callback_t callback, void* callback_userdata);

/*****************************************************************************\
| Subsets                                                                     |
\*****************************************************************************/

typedef struct rc_client_subset_t {
  uint32_t id;
  const char* title;
  char badge_name[16];

  uint32_t num_achievements;
  uint32_t num_leaderboards;
} rc_client_subset_t;

const rc_client_subset_t* rc_client_get_subset_info(rc_client_t* client, uint32_t subset_id);

/*****************************************************************************\
| Achievements                                                                |
\*****************************************************************************/

enum {
  RC_CLIENT_ACHIEVEMENT_STATE_INACTIVE = 0, /* unprocessed */
  RC_CLIENT_ACHIEVEMENT_STATE_ACTIVE = 1,   /* eligible to trigger */
  RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED = 2, /* earned by user */
  RC_CLIENT_ACHIEVEMENT_STATE_DISABLED = 3  /* not supported by this version of the runtime */
};

enum {
  RC_CLIENT_ACHIEVEMENT_CATEGORY_NONE = 0,
  RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE = (1 << 0),
  RC_CLIENT_ACHIEVEMENT_CATEGORY_UNOFFICIAL = (1 << 1),
  RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE_AND_UNOFFICIAL = RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE | RC_CLIENT_ACHIEVEMENT_CATEGORY_UNOFFICIAL
};

enum {
  RC_CLIENT_ACHIEVEMENT_BUCKET_UNKNOWN = 0,
  RC_CLIENT_ACHIEVEMENT_BUCKET_LOCKED = 1,
  RC_CLIENT_ACHIEVEMENT_BUCKET_UNLOCKED = 2,
  RC_CLIENT_ACHIEVEMENT_BUCKET_UNSUPPORTED = 3,
  RC_CLIENT_ACHIEVEMENT_BUCKET_UNOFFICIAL = 4,
  RC_CLIENT_ACHIEVEMENT_BUCKET_RECENTLY_UNLOCKED = 5,
  RC_CLIENT_ACHIEVEMENT_BUCKET_ACTIVE_CHALLENGE = 6,
  RC_CLIENT_ACHIEVEMENT_BUCKET_ALMOST_THERE = 7
};

enum {
  RC_CLIENT_ACHIEVEMENT_UNLOCKED_NONE = 0,
  RC_CLIENT_ACHIEVEMENT_UNLOCKED_SOFTCORE = (1 << 0),
  RC_CLIENT_ACHIEVEMENT_UNLOCKED_HARDCORE = (1 << 1),
  RC_CLIENT_ACHIEVEMENT_UNLOCKED_BOTH = RC_CLIENT_ACHIEVEMENT_UNLOCKED_SOFTCORE | RC_CLIENT_ACHIEVEMENT_UNLOCKED_HARDCORE
};

typedef struct rc_client_achievement_t {
  const char* title;
  const char* description;
  char badge_name[8];
  char measured_progress[24];
  float measured_percent;
  uint32_t id;
  uint32_t points;
  time_t unlock_time;
  uint8_t state;
  uint8_t category;
  uint8_t bucket;
  uint8_t unlocked;
} rc_client_achievement_t;

/**
 * Get information about an achievement. Returns NULL if not found.
 */
const rc_client_achievement_t* rc_client_get_achievement_info(rc_client_t* client, uint32_t id);

/**
 * Gets the URL for the achievement image.
 * Returns RC_OK on success.
 */
int rc_client_achievement_get_image_url(const rc_client_achievement_t* achievement, int state, char buffer[], size_t buffer_size);

typedef struct rc_client_achievement_bucket_t {
  rc_client_achievement_t** achievements;
  uint32_t num_achievements;

  const char* label;
  uint32_t subset_id;
  uint8_t bucket_type;
} rc_client_achievement_bucket_t;

typedef struct rc_client_achievement_list_t {
  rc_client_achievement_bucket_t* buckets;
  uint32_t num_buckets;
} rc_client_achievement_list_t;

enum {
  RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE = 0,
  RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS = 1
};

/**
 * Creates a list of achievements matching the specified category and grouping.
 * Returns an allocated list that must be free'd by calling rc_client_destroy_achievement_list.
 */
rc_client_achievement_list_t* rc_client_create_achievement_list(rc_client_t* client, int category, int grouping);

/**
 * Destroys a list allocated by rc_client_get_achievement_list.
 */
void rc_client_destroy_achievement_list(rc_client_achievement_list_t* list);

/*****************************************************************************\
| Leaderboards                                                                |
\*****************************************************************************/

enum {
  RC_CLIENT_LEADERBOARD_STATE_INACTIVE = 0,
  RC_CLIENT_LEADERBOARD_STATE_ACTIVE = 1,
  RC_CLIENT_LEADERBOARD_STATE_TRACKING = 2,
  RC_CLIENT_LEADERBOARD_STATE_DISABLED = 3
};

typedef struct rc_client_leaderboard_t {
  const char* title;
  const char* description;
  const char* tracker_value;
  uint32_t id;
  uint8_t state;
  uint8_t lower_is_better;
} rc_client_leaderboard_t;

/**
 * Get information about a leaderboard. Returns NULL if not found.
 */
const rc_client_leaderboard_t* rc_client_get_leaderboard_info(const rc_client_t* client, uint32_t id);

typedef struct rc_client_leaderboard_tracker_t {
  char display[24];
  uint32_t id;
} rc_client_leaderboard_tracker_t;

typedef struct rc_client_leaderboard_bucket_t {
  rc_client_leaderboard_t** leaderboards;
  uint32_t num_leaderboards;

  const char* label;
  uint32_t subset_id;
  uint8_t bucket_type;
} rc_client_leaderboard_bucket_t;

typedef struct rc_client_leaderboard_list_t {
  rc_client_leaderboard_bucket_t* buckets;
  uint32_t num_buckets;
} rc_client_leaderboard_list_t;

enum {
  RC_CLIENT_LEADERBOARD_BUCKET_UNKNOWN = 0,
  RC_CLIENT_LEADERBOARD_BUCKET_INACTIVE = 1,
  RC_CLIENT_LEADERBOARD_BUCKET_ACTIVE = 2,
  RC_CLIENT_LEADERBOARD_BUCKET_UNSUPPORTED = 3,
  RC_CLIENT_LEADERBOARD_BUCKET_ALL = 4
};

enum {
  RC_CLIENT_LEADERBOARD_LIST_GROUPING_NONE = 0,
  RC_CLIENT_LEADERBOARD_LIST_GROUPING_TRACKING = 1
};

/**
 * Creates a list of leaderboards matching the specified grouping.
 * Returns an allocated list that must be free'd by calling rc_client_destroy_leaderboard_list.
 */
rc_client_leaderboard_list_t* rc_client_create_leaderboard_list(rc_client_t* client, int grouping);

/**
 * Destroys a list allocated by rc_client_get_leaderboard_list.
 */
void rc_client_destroy_leaderboard_list(rc_client_leaderboard_list_t* list);

typedef struct rc_client_leaderboard_entry_t {
  const char* user;
  char display[24];
  time_t submitted;
  uint32_t rank;
  uint32_t index;
} rc_client_leaderboard_entry_t;

typedef struct rc_client_leaderboard_entry_list_t {
  rc_client_leaderboard_entry_t* entries;
  uint32_t num_entries;
  int32_t user_index;
} rc_client_leaderboard_entry_list_t;

typedef void (*rc_client_fetch_leaderboard_entries_callback_t)(int result, const char* error_message,
    rc_client_leaderboard_entry_list_t* list, rc_client_t* client, void* callback_userdata);

/**
 * Fetches a list of leaderboard entries from the server.
 * Callback receives an allocated list that must be free'd by calling rc_client_destroy_leaderboard_entry_list.
 */
rc_client_async_handle_t* rc_client_begin_fetch_leaderboard_entries(rc_client_t* client, uint32_t leaderboard_id,
    uint32_t first_entry, uint32_t count, rc_client_fetch_leaderboard_entries_callback_t callback, void* callback_userdata);

/**
 * Fetches a list of leaderboard entries from the server containing the logged-in user.
 * Callback receives an allocated list that must be free'd by calling rc_client_destroy_leaderboard_entry_list.
 */
rc_client_async_handle_t* rc_client_begin_fetch_leaderboard_entries_around_user(rc_client_t* client, uint32_t leaderboard_id,
    uint32_t count, rc_client_fetch_leaderboard_entries_callback_t callback, void* callback_userdata);

/**
 * Gets the URL for the profile picture of the user associated to a leaderboard entry.
 * Returns RC_OK on success.
 */
int rc_client_leaderboard_entry_get_user_image_url(const rc_client_leaderboard_entry_t* entry, char buffer[], size_t buffer_size);

/**
 * Destroys a list allocated by rc_client_begin_fetch_leaderboard_entries or rc_client_begin_fetch_leaderboard_entries_around_user.
 */
void rc_client_destroy_leaderboard_entry_list(rc_client_leaderboard_entry_list_t* list);

/*****************************************************************************\
| Rich Presence                                                               |
\*****************************************************************************/

/**
 * Gets the current rich presence message.
 * Returns the number of characters written to buffer.
 */
size_t rc_client_get_rich_presence_message(rc_client_t* client, char buffer[], size_t buffer_size);

/*****************************************************************************\
| Processing                                                                  |
\*****************************************************************************/

enum {
  RC_CLIENT_EVENT_TYPE_NONE = 0,
  RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED = 1, /* [achievement] was earned by the player */
  RC_CLIENT_EVENT_LEADERBOARD_STARTED = 2, /* [leaderboard] attempt has started */
  RC_CLIENT_EVENT_LEADERBOARD_FAILED = 3, /* [leaderboard] attempt failed */
  RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED = 4, /* [leaderboard] attempt submitted */
  RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW = 5, /* [achievement] challenge indicator should be shown */
  RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_HIDE = 6, /* [achievement] challenge indicator should be hidden */
  RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW = 7, /* progress indicator should be shown for [achievement] */
  RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_HIDE = 8, /* progress indicator should be hidden */
  RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_UPDATE = 9, /* progress indicator should be updated to reflect new badge/progress for [achievement] */
  RC_CLIENT_EVENT_LEADERBOARD_TRACKER_SHOW = 10, /* [leaderboard_tracker] should be shown */
  RC_CLIENT_EVENT_LEADERBOARD_TRACKER_HIDE = 11, /* [leaderboard_tracker] should be hidden */
  RC_CLIENT_EVENT_LEADERBOARD_TRACKER_UPDATE = 12, /* [leaderboard_tracker] updated */
  RC_CLIENT_EVENT_RESET = 13, /* emulated system should be reset (as the result of enabling hardcore) */
  RC_CLIENT_EVENT_GAME_COMPLETED = 14, /* all achievements for the game have been earned */
  RC_CLIENT_EVENT_SERVER_ERROR = 15 /* an API response returned a [server_error] and will not be retried */
};

typedef struct rc_client_server_error_t
{
  const char* error_message;
  const char* api;
} rc_client_server_error_t;

typedef struct rc_client_event_t
{
  uint32_t type;

  rc_client_achievement_t* achievement;
  rc_client_leaderboard_t* leaderboard;
  rc_client_leaderboard_tracker_t* leaderboard_tracker;
  rc_client_server_error_t* server_error;

} rc_client_event_t;

/**
 * Callback used to notify the client when certain events occur.
 */
typedef void (*rc_client_event_handler_t)(const rc_client_event_t* event, rc_client_t* client);

/**
 * Provides a callback for event handling.
 */
void rc_client_set_event_handler(rc_client_t* client, rc_client_event_handler_t handler);

/**
 * Provides a callback for reading memory.
 */
void rc_client_set_read_memory_function(rc_client_t* client, rc_client_read_memory_func_t handler);

/**
 * Determines if there are any active achievements/leaderboards/rich presence that need processing.
 */
int rc_client_is_processing_required(rc_client_t* client);

/**
 * Processes achievements for the current frame.
 */
void rc_client_do_frame(rc_client_t* client);

/**
 * Processes the periodic queue.
 * Called internally by rc_client_do_frame.
 * Should be explicitly called if rc_client_do_frame is not being called because emulation is paused.
 */
void rc_client_idle(rc_client_t* client);

/**
 * Informs the runtime that the emulator has been reset. Will reset all achievements and leaderboards
 * to their initial state (includes hiding indicators/trackers).
 */
void rc_client_reset(rc_client_t* client);

/**
 * Gets the number of bytes needed to serialized the runtime state.
 */
size_t rc_client_progress_size(rc_client_t* client);

/**
 * Serializes the runtime state into a buffer.
 * Returns RC_OK on success, or an error indicator.
 */
int rc_client_serialize_progress(rc_client_t* client, uint8_t* buffer);

/**
 * Deserializes the runtime state from a buffer.
 * Returns RC_OK on success, or an error indicator.
 */
int rc_client_deserialize_progress(rc_client_t* client, const uint8_t* serialized);

#ifdef __cplusplus
}
#endif

#endif /* RC_RUNTIME_H */
