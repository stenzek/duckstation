#include "rc_client_async.h"

#include "rc_client_internal.h"

#include "../util/rc_compat.h"

int rc_client_async_handle_aborted(struct rc_client_t* client, struct rc_client_async_handle_t* async_handle)
{
  int aborted;

  rc_mutex_lock(&client->state.mutex);
  aborted = async_handle->aborted;
  rc_mutex_unlock(&client->state.mutex);

  return aborted;
}

void rc_client_abort_async(struct rc_client_t* client, struct rc_client_async_handle_t* async_handle)
{
  if (async_handle && client) {
#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
    if (client->state.external_client && client->state.external_client->abort_async) {
      client->state.external_client->abort_async(async_handle);
      return;
    }
#endif

    rc_mutex_lock(&client->state.mutex);
    async_handle->aborted = RC_CLIENT_ASYNC_ABORTED;
    rc_mutex_unlock(&client->state.mutex);
  }
}

void rc_client_begin_async(struct rc_client_t* client, struct rc_client_async_handle_t* async_handle)
{
  size_t i;

  rc_mutex_lock(&client->state.mutex);
  for (i = 0; i < sizeof(client->state.async_handles) / sizeof(client->state.async_handles[0]); ++i) {
    if (!client->state.async_handles[i]) {
      client->state.async_handles[i] = async_handle;
      break;
    }
  }
  rc_mutex_unlock(&client->state.mutex);
}

int rc_client_end_async(struct rc_client_t* client, struct rc_client_async_handle_t* async_handle)
{
  int aborted = async_handle->aborted;

  /* if client was destroyed, mutex doesn't exist and we don't need to remove the handle from the collection */
  if (aborted != RC_CLIENT_ASYNC_DESTROYED) {
    size_t i;

    rc_mutex_lock(&client->state.mutex);
    for (i = 0; i < sizeof(client->state.async_handles) / sizeof(client->state.async_handles[0]); ++i) {
      if (client->state.async_handles[i] == async_handle) {
        client->state.async_handles[i] = NULL;
        break;
      }
    }
    aborted = async_handle->aborted;

    rc_mutex_unlock(&client->state.mutex);
  }

  return aborted;
}

int rc_client_async_handle_valid(struct rc_client_t* client, struct rc_client_async_handle_t* async_handle)
{
  int valid = 0;
  size_t i;

  /* there is a small window of opportunity where the client could have been destroyed before calling
   * this function, but this function assumes the possibility that the handle has been destroyed, so
   * we can't check it for RC_CLIENT_ASYNC_DESTROYED before attempting to scan the client data */
  rc_mutex_lock(&client->state.mutex);

  for (i = 0; i < sizeof(client->state.async_handles) / sizeof(client->state.async_handles[0]); ++i) {
    if (client->state.async_handles[i] == async_handle) {
      valid = 1;
      break;
    }
  }

  rc_mutex_unlock(&client->state.mutex);

  return valid;
}
