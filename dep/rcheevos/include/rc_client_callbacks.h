#ifndef RC_CLIENT_CALLBACKS_H
#define RC_CLIENT_CALLBACKS_H

#include "rc_export.h"

#include <stdint.h>

RC_BEGIN_C_DECLS

struct rc_client_t;              /* rc_client_internal.h */
struct rc_api_request_t;         /* rc_api_request.h */
struct rc_api_server_response_t; /* rc_api_request.h */

typedef uint64_t rc_clock_t;

/**
 * Callback used to read num_bytes bytes from memory starting at address into buffer.
 * Returns the number of bytes read. A return value of 0 indicates the address was invalid.
 */
typedef uint32_t (RC_CCONV *rc_client_read_memory_func_t)(uint32_t address, uint8_t* buffer, uint32_t num_bytes, struct rc_client_t* client);

/**
 * Internal method passed to rc_client_server_call_t to process the server response.
 */
typedef void (RC_CCONV *rc_client_server_callback_t)(const struct rc_api_server_response_t* server_response, void* callback_data);

/**
 * Callback used to issue a request to the server.
 */
typedef void (RC_CCONV *rc_client_server_call_t)(const struct rc_api_request_t* request, rc_client_server_callback_t callback, void* callback_data, struct rc_client_t* client);

/**
 * Generic callback for asynchronous eventing.
 */
typedef void (RC_CCONV *rc_client_callback_t)(int result, const char* error_message, struct rc_client_t* client, void* userdata);

/**
 * Callback for logging or displaying a message.
 */
typedef void (RC_CCONV *rc_client_message_callback_t)(const char* message, const struct rc_client_t* client);

/**
 * Callback to return the current time (as a time_t * 1000 + milliseconds).
 */
typedef rc_clock_t(RC_CCONV* rc_get_time_millisecs_func_t)(const struct rc_client_t* client);

RC_END_C_DECLS

#endif /* RC_CLIENT_CALLBACKS_H */
