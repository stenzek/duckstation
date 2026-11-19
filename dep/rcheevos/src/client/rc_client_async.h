#ifndef RC_CLIENT_ASYNC_H
#define RC_CLIENT_ASYNC_H

#include "rc_client_callbacks.h"

#include "rc_export.h"

#include <stdint.h>

RC_BEGIN_C_DECLS

/*****************************************************************************\
| Enums                                                                       |
\*****************************************************************************/

enum {
  RC_CLIENT_ASYNC_NOT_ABORTED = 0,
  RC_CLIENT_ASYNC_ABORTED = 1,
  RC_CLIENT_ASYNC_DESTROYED = 2
};

/*****************************************************************************\
| Structures                                                                  |
\*****************************************************************************/

struct rc_client_t; /* rc_client_internal.h */

typedef struct rc_client_async_handle_t {
  uint8_t aborted;
} rc_client_async_handle_t;

typedef struct rc_client_generic_callback_data_t {
  struct rc_client_t* client;
  rc_client_callback_t callback;
  void* callback_userdata;
  rc_client_async_handle_t async_handle;
} rc_client_generic_callback_data_t;

/*****************************************************************************\
| Functions                                                                   |
\*****************************************************************************/

/**
 * Returns non-zero if the rc_client_abort_async has been called for the provided handle.
 */
int rc_client_async_handle_aborted(struct rc_client_t* client, struct rc_client_async_handle_t* async_handle);

/**
 * Marks the handle as aborted.
 *
 * Allows the async callback to stop processing when it checks rc_client_async_handle_aborted.
 */
void rc_client_abort_async(struct rc_client_t* client, struct rc_client_async_handle_t* async_handle);

/**
 * Starts tracking an async handle.
 */
void rc_client_begin_async(struct rc_client_t* client, struct rc_client_async_handle_t* async_handle);

/**
 * Stops tracking an async handle.
 *
 * Returns the async state of the handle. (RC_CLIENT_ASYNC_*)
 */
int rc_client_end_async(struct rc_client_t* client, struct rc_client_async_handle_t* async_handle);

/**
 * Returns non-zero if the provided async handle is still being tracked.
 */
int rc_client_async_handle_valid(struct rc_client_t* client, struct rc_client_async_handle_t* async_handle);

RC_END_C_DECLS

#endif /* RC_CLIENT_ASYNC_H */
