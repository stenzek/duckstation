#include "rc_client_async.h"
#include "rc_client_load.h"
#include "rc_client_internal.h"
#include "rc_client_server.h"

#include "rc_api_request.h"
#include "rc_api_runtime.h"
#include "rc_api_user.h"

static void rc_client_login_callback(const rc_api_server_response_t* server_response, void* callback_data)
{
  rc_client_generic_callback_data_t* login_callback_data = (rc_client_generic_callback_data_t*)callback_data;
  rc_client_t* client = login_callback_data->client;
  rc_api_login_response_t login_response;
  rc_client_load_state_t* load_state;
  const char* error_message;
  int result;

  result = rc_client_end_async(client, &login_callback_data->async_handle);
  if (result) {
    if (result != RC_CLIENT_ASYNC_DESTROYED)
      rc_client_logout(client); /* logout will reset the user state and call the load game callback */

    free(login_callback_data);
    return;
  }

  if (client->state.user == RC_CLIENT_USER_STATE_NONE) {
    /* logout was called */
    if (login_callback_data->callback)
      login_callback_data->callback(RC_ABORTED, "Login aborted", client, login_callback_data->callback_userdata);

    free(login_callback_data);
    /* logout call will immediately abort load game before this callback gets called */
    return;
  }

  result = rc_api_process_login_server_response(&login_response, server_response);
  error_message = rc_client_server_error_message(&result, server_response->http_status_code, &login_response.response);
  if (error_message) {
    rc_mutex_lock(&client->state.mutex);
    client->state.user = RC_CLIENT_USER_STATE_NONE;
    load_state = client->state.load;
    rc_mutex_unlock(&client->state.mutex);

    RC_CLIENT_LOG_ERR_FORMATTED(client, "Login failed: %s", error_message);
    if (login_callback_data->callback)
      login_callback_data->callback(result, error_message, client, login_callback_data->callback_userdata);

    if (load_state && load_state->progress == RC_CLIENT_LOAD_GAME_STATE_AWAIT_LOGIN)
      rc_client_begin_fetch_game_sets(load_state);
  }
  else {
    client->user.username = rc_buffer_strcpy(&client->state.buffer, login_response.username);

    if (strcmp(login_response.username, login_response.display_name) == 0)
      client->user.display_name = client->user.username;
    else
      client->user.display_name = rc_buffer_strcpy(&client->state.buffer, login_response.display_name);

    client->user.avatar_url = rc_buffer_strcpy(&client->state.buffer, login_response.avatar_url);
    client->user.avatar_last_updated = login_response.avatar_last_updated;
    client->user.token = rc_buffer_strcpy(&client->state.buffer, login_response.api_token);
    client->user.score = login_response.score;
    client->user.score_casual = login_response.score_casual;
    client->user.num_unread_messages = login_response.num_unread_messages;

    rc_mutex_lock(&client->state.mutex);
    client->state.user = RC_CLIENT_USER_STATE_LOGGED_IN;
    load_state = client->state.load;
    rc_mutex_unlock(&client->state.mutex);

    RC_CLIENT_LOG_INFO_FORMATTED(client, "%s logged in successfully", login_response.display_name);

    if (load_state && load_state->progress == RC_CLIENT_LOAD_GAME_STATE_AWAIT_LOGIN)
      rc_client_begin_fetch_game_sets(load_state);

    if (login_callback_data->callback)
      login_callback_data->callback(RC_OK, NULL, client, login_callback_data->callback_userdata);
  }

  rc_api_destroy_login_response(&login_response);
  free(login_callback_data);
}

static rc_client_async_handle_t* rc_client_begin_login(rc_client_t* client,
  const rc_api_login_request_t* login_request, rc_client_callback_t callback, void* callback_userdata)
{
  rc_client_generic_callback_data_t* callback_data;
  rc_api_request_t request;
  int result = rc_api_init_login_request_hosted(&request, login_request, &client->state.host);
  const char* error_message = rc_error_str(result);

  if (result == RC_OK) {
    rc_mutex_lock(&client->state.mutex);

    if (client->state.user == RC_CLIENT_USER_STATE_LOGIN_REQUESTED) {
      error_message = "Login already in progress";
      result = RC_INVALID_STATE;
    }
    client->state.user = RC_CLIENT_USER_STATE_LOGIN_REQUESTED;

    rc_mutex_unlock(&client->state.mutex);
  }

  if (result != RC_OK) {
    callback(result, error_message, client, callback_userdata);
    return NULL;
  }

  callback_data = (rc_client_generic_callback_data_t*)calloc(1, sizeof(*callback_data));
  if (!callback_data) {
    callback(RC_OUT_OF_MEMORY, rc_error_str(RC_OUT_OF_MEMORY), client, callback_userdata);
    return NULL;
  }

  callback_data->client = client;
  callback_data->callback = callback;
  callback_data->callback_userdata = callback_userdata;

  rc_client_begin_async(client, &callback_data->async_handle);
  client->callbacks.server_call(&request, rc_client_login_callback, callback_data, client);

  rc_api_destroy_request(&request);

  /* if the user state has changed, the async operation completed synchronously */
  rc_mutex_lock(&client->state.mutex);
  if (client->state.user != RC_CLIENT_USER_STATE_LOGIN_REQUESTED)
    callback_data = NULL;
  rc_mutex_unlock(&client->state.mutex);

  return callback_data ? &callback_data->async_handle : NULL;
}

rc_client_async_handle_t* rc_client_begin_login_with_password(rc_client_t* client,
  const char* username, const char* password, rc_client_callback_t callback, void* callback_userdata)
{
  rc_api_login_request_t login_request;

  if (!client) {
    callback(RC_INVALID_STATE, "client is required", client, callback_userdata);
    return NULL;
  }

  if (!username || !username[0]) {
    callback(RC_INVALID_STATE, "username is required", client, callback_userdata);
    return NULL;
  }

  if (!password || !password[0]) {
    callback(RC_INVALID_STATE, "password is required", client, callback_userdata);
    return NULL;
  }

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client && client->state.external_client->begin_login_with_password)
    return client->state.external_client->begin_login_with_password(client, username, password, callback, callback_userdata);
#endif

  memset(&login_request, 0, sizeof(login_request));
  login_request.username = username;
  login_request.password = password;

  RC_CLIENT_LOG_INFO_FORMATTED(client, "Attempting to log in %s (with password)", username);
  return rc_client_begin_login(client, &login_request, callback, callback_userdata);
}

rc_client_async_handle_t* rc_client_begin_login_with_token(rc_client_t* client,
  const char* username, const char* token, rc_client_callback_t callback, void* callback_userdata)
{
  rc_api_login_request_t login_request;

  if (!client) {
    callback(RC_INVALID_STATE, "client is required", client, callback_userdata);
    return NULL;
  }

  if (!username || !username[0]) {
    callback(RC_INVALID_STATE, "username is required", client, callback_userdata);
    return NULL;
  }

  if (!token || !token[0]) {
    callback(RC_INVALID_STATE, "token is required", client, callback_userdata);
    return NULL;
  }

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client && client->state.external_client->begin_login_with_token)
    return client->state.external_client->begin_login_with_token(client, username, token, callback, callback_userdata);
#endif

  memset(&login_request, 0, sizeof(login_request));
  login_request.username = username;
  login_request.api_token = token;

  RC_CLIENT_LOG_INFO_FORMATTED(client, "Attempting to log in %s (with token)", username);
  return rc_client_begin_login(client, &login_request, callback, callback_userdata);
}

void rc_client_logout(rc_client_t* client)
{
  rc_client_load_state_t* load_state;

  if (!client)
    return;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client && client->state.external_client->logout) {
    client->state.external_client->logout();
    return;
  }
#endif

  switch (client->state.user) {
    case RC_CLIENT_USER_STATE_LOGGED_IN:
      RC_CLIENT_LOG_INFO_FORMATTED(client, "Logging %s out", client->user.display_name);
      break;

    case RC_CLIENT_USER_STATE_LOGIN_REQUESTED:
      RC_CLIENT_LOG_INFO(client, "Aborting login");
      break;
  }

  rc_mutex_lock(&client->state.mutex);

  client->state.user = RC_CLIENT_USER_STATE_NONE;
  memset(&client->user, 0, sizeof(client->user));

  load_state = client->state.load;

  rc_mutex_unlock(&client->state.mutex);

  rc_client_unload_game(client);

  if (load_state && load_state->progress == RC_CLIENT_LOAD_GAME_STATE_AWAIT_LOGIN)
    rc_client_load_error(load_state, RC_ABORTED, "Login aborted");
}

const rc_client_user_t* rc_client_get_user_info(const rc_client_t* client)
{
  if (!client)
    return NULL;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client) {
    if (client->state.external_client->get_user_info_v3)
      return client->state.external_client->get_user_info_v3();

    if (client->state.external_client->get_user_info)
      return rc_client_external_convert_v1_user(client, client->state.external_client->get_user_info());
  }
#endif

  return (client->state.user == RC_CLIENT_USER_STATE_LOGGED_IN) ? &client->user : NULL;
}

int rc_client_user_get_image_url(const rc_client_user_t* user, char buffer[], size_t buffer_size)
{
  if (!user)
    return RC_INVALID_STATE;

  if (user->avatar_url) {
    const size_t len = strlen(user->avatar_url);
    if (len >= buffer_size)
      return RC_INSUFFICIENT_BUFFER;

    memcpy(buffer, user->avatar_url, len + 1);
    return RC_OK;
  }

  return rc_client_get_image_url(buffer, buffer_size, RC_IMAGE_TYPE_USER, user->display_name);
}
