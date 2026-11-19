#include "rc_client_server.h"

#include "rc_client_do_frame.h"
#include "rc_client_internal.h" /* rc_client */

#include "rc_api_request.h"
#include "rc_api_runtime.h"
#include "rc_error.h"

#include "../util/rc_compat.h"

#include <string.h> /* memset */

const char* rc_client_server_error_message(int* result, int http_status_code, const struct rc_api_response_t* response)
{
  if (!response->succeeded) {
    if (*result == RC_OK) {
      *result = RC_API_FAILURE;
      if (!response->error_message)
        return "Unexpected API failure with no error message";
    }

    if (response->error_message)
      return response->error_message;
  }

  (void)http_status_code;

  if (*result != RC_OK)
    return rc_error_str(*result);

  return NULL;
}

void rc_client_raise_server_error_event(struct rc_client_t* client,
  const char* api, uint32_t related_id, int result, const char* error_message)
{
  rc_client_server_error_t server_error;
  rc_client_event_t client_event;

  server_error.api = api;
  server_error.error_message = error_message;
  server_error.result = result;
  server_error.related_id = related_id;

  memset(&client_event, 0, sizeof(client_event));
  client_event.type = RC_CLIENT_EVENT_SERVER_ERROR;
  client_event.server_error = &server_error;

  client->callbacks.event_handler(&client_event, client);
}

void rc_client_update_disconnect_state(rc_client_t* client)
{
  uint8_t new_state = RC_CLIENT_DISCONNECT_HIDDEN;

  rc_mutex_lock(&client->state.mutex);

  if (rc_client_has_server_retry_scheduled_callback(client))
    new_state = RC_CLIENT_DISCONNECT_VISIBLE;

  if ((client->state.disconnect & RC_CLIENT_DISCONNECT_VISIBLE) != new_state) {
    if (new_state == RC_CLIENT_DISCONNECT_VISIBLE)
      client->state.disconnect = RC_CLIENT_DISCONNECT_HIDDEN | RC_CLIENT_DISCONNECT_SHOW_PENDING;
    else
      client->state.disconnect = RC_CLIENT_DISCONNECT_VISIBLE | RC_CLIENT_DISCONNECT_HIDE_PENDING;
  }
  else {
    client->state.disconnect = new_state;
  }

  rc_mutex_unlock(&client->state.mutex);
}

void rc_client_raise_disconnect_events(struct rc_client_t* client)
{
  rc_client_event_t client_event;
  uint8_t new_state;

  rc_mutex_lock(&client->state.mutex);

  if (client->state.disconnect & RC_CLIENT_DISCONNECT_SHOW_PENDING)
    new_state = RC_CLIENT_DISCONNECT_VISIBLE;
  else
    new_state = RC_CLIENT_DISCONNECT_HIDDEN;
  client->state.disconnect = new_state;

  rc_mutex_unlock(&client->state.mutex);

  memset(&client_event, 0, sizeof(client_event));
  client_event.type = (new_state == RC_CLIENT_DISCONNECT_VISIBLE) ?
    RC_CLIENT_EVENT_DISCONNECTED : RC_CLIENT_EVENT_RECONNECTED;
  client->callbacks.event_handler(&client_event, client);
}

int rc_client_should_retry(const struct rc_api_server_response_t* server_response)
{
  switch (server_response->http_status_code) {
    case 502: /* 502 Bad Gateway */
      /* nginx connection pool full */
      return 1;

    case 503: /* 503 Service Temporarily Unavailable */
      /* site is in maintenance mode */
      return 1;

    case 504: /* 504 Gateway Timeout */
      /* timeout between web server and database server */
      return 1;

    case 429: /* 429 Too Many Requests */
      /* too many unlocks occurred at the same time */
      return 1;

    case 521: /* 521 Web Server is Down */
      /* cloudfare could not find the server */
      return 1;

    case 522: /* 522 Connection Timed Out */
      /* timeout connecting to server from cloudfare */
      return 1;

    case 523: /* 523 Origin is Unreachable */
      /* cloudfare cannot find server */
      return 1;

    case 524: /* 524 A Timeout Occurred */
      /* connection to server from cloudfare was dropped before request was completed */
      return 1;

    case 525: /* 525 SSL Handshake Failed */
      /* web server worker connection pool is exhausted */
      return 1;

    case RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR:
      /* client provided non-HTTP error (explicitly retryable) */
      return 1;

    case RC_API_SERVER_RESPONSE_CLIENT_ERROR:
      /* client provided non-HTTP error (implicitly non-retryable) */
      return 0;

    default:
      /* assume any error not handled above where no response was received should be retried */
      if (server_response->body_length == 0 || !server_response->body || !server_response->body[0])
        return 1;

      return 0;
  }
}

int rc_client_get_image_url(char buffer[], size_t buffer_size, int image_type, const char* image_name)
{
  rc_api_fetch_image_request_t image_request;
  rc_api_request_t request;
  int result;

  if (!buffer)
    return RC_INVALID_STATE;

  memset(&image_request, 0, sizeof(image_request));
  image_request.image_type = image_type;
  image_request.image_name = image_name;
  result = rc_api_init_fetch_image_request_hosted(&request, &image_request, NULL);
  if (result == RC_OK)
  {
    const size_t len = strlen(request.url);
    if (len >= buffer_size)
      result = RC_INSUFFICIENT_BUFFER;
    else
      memcpy(buffer, request.url, len + 1);
  }

  rc_api_destroy_request(&request);
  return result;
}
