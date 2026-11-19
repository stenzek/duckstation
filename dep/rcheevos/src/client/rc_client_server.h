#ifndef RC_CLIENT_SERVER_H
#define RC_CLIENT_SERVER_H

#include "rc_export.h"

#include <stdint.h>
#include <stddef.h>

RC_BEGIN_C_DECLS

/*****************************************************************************\
| Enums                                                                       |
\*****************************************************************************/

/*****************************************************************************\
| Structures                                                                  |
\*****************************************************************************/

struct rc_api_response_t;        /* rc_api_request.h */
struct rc_api_server_response_t; /* rc_api_request.h */
struct rc_client_t;              /* rc_client.h */

/*****************************************************************************\
| Functions                                                                   |
\*****************************************************************************/

/**
 * Extracts an error message from a server response.
 *
 * Returns NULL if the server response does not represent an error state.
 */
const char* rc_client_server_error_message(int* result, int http_status_code, const struct rc_api_response_t* response);

/**
 * Returns non-zero if the server response represents a non-fatal error that could be retried.
 */
int rc_client_should_retry(const struct rc_api_server_response_t* server_response);

/**
 * Raises an RC_CLIENT_EVENT_SERVER_ERROR client event
 */
void rc_client_raise_server_error_event(struct rc_client_t* client, const char* api, uint32_t related_id, int result, const char* error_message);

/**
 * Raises any pending RC_CLIENT_EVENT_DISCONNECTED or RC_CLIENT_EVENT_RECONNECTED events
 */
void rc_client_raise_disconnect_events(struct rc_client_t* client);

/**
 * Determines if the client disconnect state has changed and queues events if it has.
 */
void rc_client_update_disconnect_state(struct rc_client_t* client);

/**
 * Constructs a url for the provided image (RC_IMAGE_TYPE_*)
 *
 * Returns RC_OK on success, or the relevent error code on failure.
 */
int rc_client_get_image_url(char buffer[], size_t buffer_size, int image_type, const char* image_name);

RC_END_C_DECLS

#endif /* RC_CLIENT_SERVER_H */
