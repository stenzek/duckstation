#ifndef RC_CLIENT_INTERNAL_H
#define RC_CLIENT_INTERNAL_H

#include "rc_client.h" /* rc_user_t, rc_client_event_handler_t */
#include "rc_client_do_frame.h"

#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
 #include "rc_client_raintegration_internal.h"
#endif
#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
 #include "rc_client_external.h"
#endif
#ifdef RC_CLIENT_SUPPORTS_HASH
 #include "rc_hash.h"
#endif

#include "rc_api_request.h"    /* rc_api_host_t */
#include "rc_util.h"           /* rc_buffer_t */
#include "../util/rc_compat.h" /* rc_mutex */

RC_BEGIN_C_DECLS

/*****************************************************************************\
| Forward Declarations (internal types)                                       |
\*****************************************************************************/

struct rc_api_fetch_game_sets_response_t; /* rc_api_runtime.h */
struct rc_client_async_handle_t;          /* rc_client_async.h */
struct rc_client_game_hash_t;             /* rc_client_game.h */
struct rc_client_game_info_t;             /* rc_client_game.h */
struct rc_client_load_state_t;            /* rc_client_load.h */
struct rc_memref_t;                       /* rc_memref.h */

/*****************************************************************************\
| Callbacks                                                                   |
\*****************************************************************************/

typedef void (RC_CCONV *rc_client_post_process_game_sets_response_t)(const struct rc_api_server_response_t* server_response,
              struct rc_api_fetch_game_sets_response_t* game_sets_response, struct rc_client_t* client, void* userdata);
typedef int (RC_CCONV *rc_client_can_submit_achievement_unlock_t)(uint32_t achievement_id, struct rc_client_t* client);
typedef int (RC_CCONV *rc_client_can_submit_leaderboard_entry_t)(uint32_t leaderboard_id, struct rc_client_t* client);
typedef int (RC_CCONV *rc_client_rich_presence_override_t)(struct rc_client_t* client, char buffer[], size_t buffersize);
typedef uint32_t (RC_CCONV* rc_client_identify_hash_func_t)(uint32_t console_id, const char* hash,
                  struct rc_client_t* client, void* callback_userdata);

typedef struct rc_client_callbacks_t {
  /* Callback to read memory from the client. */
  rc_client_read_memory_func_t read_memory;

  /* Callback to notify the client that something happened. */
  rc_client_event_handler_t event_handler;

  /* Callback to send a request to the server. */
  rc_client_server_call_t server_call;

  /* Callback to log a message. */
  rc_client_message_callback_t log_call;

  /* Callback to get the current time. */
  rc_get_time_millisecs_func_t get_time_millisecs;

  /* Callback to allow client to specify a game to load when the server was unable to identify a hash. */
  rc_client_identify_hash_func_t identify_unknown_hash;

  /* Callback to let the client view the game_sets response. */
  rc_client_post_process_game_sets_response_t post_process_game_sets_response;

  /* Callacbk to let the client prevent submission of an unlock. */
  rc_client_can_submit_achievement_unlock_t can_submit_achievement_unlock;

  /* Callback to let the client prevent submission of a leaderboard entry. */
  rc_client_can_submit_leaderboard_entry_t can_submit_leaderboard_entry;

  /* Callback to allow the client to provide a custom rich presence for the ping request. */
  rc_client_rich_presence_override_t rich_presence_override;

#ifdef RC_CLIENT_SUPPORTS_HASH
  /* Additional callbacks used by the hashing library. */
  rc_hash_callbacks_t hash;
#endif

  /* Additional data to pass to some callbacks. */
  void* client_data;
} rc_client_callbacks_t;

/*****************************************************************************\
| State                                                                       |
\*****************************************************************************/

enum {
  RC_CLIENT_USER_STATE_NONE,
  RC_CLIENT_USER_STATE_LOGIN_REQUESTED,
  RC_CLIENT_USER_STATE_LOGGED_IN
};

enum {
  RC_CLIENT_SPECTATOR_MODE_OFF,
  RC_CLIENT_SPECTATOR_MODE_ON,
  RC_CLIENT_SPECTATOR_MODE_LOCKED
};

enum {
  RC_CLIENT_DISCONNECT_HIDDEN = 0,
  RC_CLIENT_DISCONNECT_VISIBLE = (1 << 0),
  RC_CLIENT_DISCONNECT_SHOW_PENDING = (1 << 1),
  RC_CLIENT_DISCONNECT_HIDE_PENDING = (1 << 2)
};

typedef struct rc_client_state_t {
  /* Mutex to guard critical sections across threads. */
  rc_mutex_t mutex;

  /* Memory pool for additional data needed by the rc_client_t. */
  rc_buffer_t buffer;

  /* Linked list of callbacks that are scheduled to run at some point in the future. */
  rc_client_scheduled_callback_data_t* scheduled_callbacks;

  /* Information about the server being connected to. */
  rc_api_host_t host;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  /* A collection of alternate implementations for most of the rc_client_ functions. */
  struct rc_client_external_t* external_client;
  /* Memory reserved for converting objects from an older external client to the current version. */
  struct rc_client_external_conversions_t* external_client_conversions;
#endif
#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
  /* Extensions for integrating with the developer toolkit. */
  struct rc_client_raintegration_t* raintegration;
#endif

  /* Total number of frames processed. */
  uint32_t frames_processed;
  /* Number of frames processed at the last ping. */
  uint32_t frames_at_last_ping;
  /* Number of frames that need to be processed before the pause penalty is reduced. */
  uint16_t unpaused_frame_decay;
  /* Number of frames that need to be processed before the user can pause again. */
  uint16_t required_unpaused_frames;

  /* Non-zero if hardcore mode is enabled. */
  uint8_t hardcore;
  /* Non-zero if encore mode is enabled. */
  uint8_t encore_mode;
  /* Whether spectator mode is enabled, and if it can be disabled (RC_CLIENT_SPECTATOR_MODE_). */
  uint8_t spectator_mode;
  /* Non-zero if unpromoted achievements/leaderboards should be activated. */
  uint8_t unpromoted_enabled;
  /* Verbosity level of log messages. */
  uint8_t log_level;
  /* User load state (RC_CLIENT_USER_STATE_*) */
  uint8_t user;
  /* Disconnect state (RC_CLIENT_DISCONNECT_*) */
  uint8_t disconnect;
  /* Non-zero to allow leaderboards to activate in casual mode. (Client must implement can_submit_leaderboard_entry to return false in casual mode). */
  uint8_t allow_leaderboards_in_casual;
  /* Non-zero to allow memory reads on a thread other than the do_frame thread. */
  uint8_t allow_background_memory_reads;

  /* Points to information about a game being loaded.*/
  struct rc_client_load_state_t* load;
  /* Collection of outstanding async handles. */
  struct rc_client_async_handle_t* async_handles[4];
  /* The memref being read. If not set to null by the read function, any achievements dependent on it will be deactivated. */
  struct rc_memref_t* processing_memref;
} rc_client_state_t;

/*****************************************************************************\
| Client                                                                      |
\*****************************************************************************/

struct rc_client_t {
  /* The loaded game. */
  struct rc_client_game_info_t* game;

  /* A cache of known hash->game mappings. */
  struct rc_client_game_hash_t* hashes;

  /* Information about the logged in user. */
  rc_client_user_t user;

  /* Client-provided functions for the rc_client_t to call. */
  rc_client_callbacks_t callbacks;

  /* The current state of the rc_client_t. */
  rc_client_state_t state;
};

/*****************************************************************************\
| Logging                                                                     |
\*****************************************************************************/

#ifdef RC_NO_VARIADIC_MACROS
 void RC_CLIENT_LOG_ERR_FORMATTED(const rc_client_t* client, const char* format, ...);
 void RC_CLIENT_LOG_WARN_FORMATTED(const rc_client_t* client, const char* format, ...);
 void RC_CLIENT_LOG_INFO_FORMATTED(const rc_client_t* client, const char* format, ...);
 void RC_CLIENT_LOG_VERBOSE_FORMATTED(const rc_client_t* client, const char* format, ...);
#else
 void rc_client_log_message_formatted(const rc_client_t* client, const char* format, ...);
 #define RC_CLIENT_LOG_ERR_FORMATTED(client, format, ...) { if (client->state.log_level >= RC_CLIENT_LOG_LEVEL_ERROR) rc_client_log_message_formatted(client, format, __VA_ARGS__); }
 #define RC_CLIENT_LOG_WARN_FORMATTED(client, format, ...) { if (client->state.log_level >= RC_CLIENT_LOG_LEVEL_WARN) rc_client_log_message_formatted(client, format, __VA_ARGS__); }
 #define RC_CLIENT_LOG_INFO_FORMATTED(client, format, ...) { if (client->state.log_level >= RC_CLIENT_LOG_LEVEL_INFO) rc_client_log_message_formatted(client, format, __VA_ARGS__); }
 #define RC_CLIENT_LOG_VERBOSE_FORMATTED(client, format, ...) { if (client->state.log_level >= RC_CLIENT_LOG_LEVEL_VERBOSE) rc_client_log_message_formatted(client, format, __VA_ARGS__); }
#endif

void rc_client_log_message(const rc_client_t* client, const char* message);
#define RC_CLIENT_LOG_ERR(client, message) { if (client->state.log_level >= RC_CLIENT_LOG_LEVEL_ERROR) rc_client_log_message(client, message); }
#define RC_CLIENT_LOG_WARN(client, message) { if (client->state.log_level >= RC_CLIENT_LOG_LEVEL_WARN) rc_client_log_message(client, message); }
#define RC_CLIENT_LOG_INFO(client, message) { if (client->state.log_level >= RC_CLIENT_LOG_LEVEL_INFO) rc_client_log_message(client, message); }
#define RC_CLIENT_LOG_VERBOSE(client, message) { if (client->state.log_level >= RC_CLIENT_LOG_LEVEL_VERBOSE) rc_client_log_message(client, message); }

RC_END_C_DECLS

#endif /* RC_CLIENT_INTERNAL_H */
