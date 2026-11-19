#ifndef RC_CLIENT_LOAD_H
#define RC_CLIENT_LOAD_H

#include "rc_client_callbacks.h" /* rc_client_callback */
#include "rc_hash.h"             /* rc_hash_iterator_t */
#include "rc_export.h"

#include "rc_client_async.h"     /* rc_client_async_handle_t */

#include <stdint.h>

RC_BEGIN_C_DECLS

/*****************************************************************************\
| Enums                                                                       |
\*****************************************************************************/

#define RC_CLIENT_UNKNOWN_GAME_ID (uint32_t)-1

/*****************************************************************************\
| Structures                                                                  |
\*****************************************************************************/

struct rc_api_start_session_response_t; /* rc_api_user.h */
struct rc_client_t;                     /* rc_client.h */
struct rc_client_game_info_t;           /* rc_client_game.h */
struct rc_client_game_hash_t;           /* rc_client_game.h */
struct rc_client_subset_info_t;         /* rc_client_game.h */
struct rc_client_pending_media_t;       /* rc_client_load.c */

typedef struct rc_client_load_state_t
{
  /* The client into which the game will be loaded. */
  struct rc_client_t* client;

  /* The callback to call once loading completes (or fails). */
  rc_client_callback_t callback;
  /* Additional caller-provided data to pass to the callback. */
  void* callback_userdata;

  /* The game being loaded. */
  struct rc_client_game_info_t* game;

  /* A linked list of subsets retrieved from the server. */
  struct rc_client_subset_info_t* subset;

  /* The hash that identifies the game being loaded. */
  struct rc_client_game_hash_t* hash;

#ifdef RC_CLIENT_SUPPORTS_HASH
  /* Information about the hashes being checked. */
  rc_hash_iterator_t hash_iterator;
  /* Hashes that were checked but didn't match. */
  struct rc_client_game_hash_t* tried_hashes[4];
#endif

  /* An alternate hash to switch to after the load finishes. */
  struct rc_client_pending_media_t* pending_media;

  /* Pointer to the start_session response in case it finished before the get_achievement_sets call. */
  struct rc_api_start_session_response_t* start_session_response;

  /* A handle that tracks whether the request has been aborted. */
  rc_client_async_handle_t async_handle;

  /* How far into the load game process we are. (RC_CLIENT_LOAD_GAME_STATE_*) */
  uint8_t progress;

  /* The number of server requests that are in flight. */
  uint8_t outstanding_requests;

#ifdef RC_CLIENT_SUPPORTS_HASH
  /* The console associated to the hash that identifies the game being loaded. */
  uint8_t hash_console_id;
#endif
} rc_client_load_state_t;

/*****************************************************************************\
| Functions                                                                   |
\*****************************************************************************/

/**
 * Begins fetching the game data once both the hash is resolved and the user is logged in.
 */
void rc_client_begin_fetch_game_sets(struct rc_client_load_state_t* load_state);

/**
 * Aborts the load process with the provided error message.
 */
void rc_client_load_error(struct rc_client_load_state_t* load_state, int result, const char* error_message);

/**
 * Loads an Unknown Game with the specified hash.
 */
void rc_client_load_unknown_game(struct rc_client_t* client, const char* hash);

RC_END_C_DECLS

#endif /* RC_CLIENT_USER_H */
