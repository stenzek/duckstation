#include "rc_client_async.h"
#include "rc_client_internal.h"
#include "rc_client_server.h"

#include "rc_api_info.h"
#include "rc_api_user.h"

/* ===== Fetch Game Hashes ===== */

typedef struct rc_client_fetch_hash_library_callback_data_t {
  rc_client_t* client;
  rc_client_fetch_hash_library_callback_t callback;
  void* callback_userdata;
  uint32_t console_id;
  rc_client_async_handle_t async_handle;
} rc_client_fetch_hash_library_callback_data_t;

static void rc_client_fetch_hash_library_callback(const rc_api_server_response_t* server_response, void* callback_data)
{
  rc_client_fetch_hash_library_callback_data_t* hashlib_callback_data =
    (rc_client_fetch_hash_library_callback_data_t*)callback_data;
  rc_client_t* client = hashlib_callback_data->client;
  rc_api_fetch_hash_library_response_t hashlib_response;
  const char* error_message;
  int result;

  result = rc_client_end_async(client, &hashlib_callback_data->async_handle);
  if (result) {
    if (result != RC_CLIENT_ASYNC_DESTROYED)
      RC_CLIENT_LOG_VERBOSE(client, "Fetch hash library aborted");

    free(hashlib_callback_data);
    return;
  }

  result = rc_api_process_fetch_hash_library_server_response(&hashlib_response, server_response);
  error_message =
    rc_client_server_error_message(&result, server_response->http_status_code, &hashlib_response.response);
  if (error_message) {
    RC_CLIENT_LOG_ERR_FORMATTED(client, "Fetch hash library for console %u failed: %s",
                                hashlib_callback_data->console_id, error_message);
    hashlib_callback_data->callback(result, error_message, NULL, client, hashlib_callback_data->callback_userdata);
  } else {
    rc_client_hash_library_t* list;
    const size_t list_size = sizeof(*list) + sizeof(rc_client_hash_library_entry_t) * hashlib_response.num_entries;
    list = (rc_client_hash_library_t*)malloc(list_size);
    if (!list) {
      hashlib_callback_data->callback(RC_OUT_OF_MEMORY, rc_error_str(RC_OUT_OF_MEMORY), NULL, client,
                                      hashlib_callback_data->callback_userdata);
    } else {
      rc_client_hash_library_entry_t* entry = list->entries =
        (rc_client_hash_library_entry_t*)((uint8_t*)list + sizeof(*list));
      const rc_api_hash_library_entry_t* hlentry = hashlib_response.entries;
      const rc_api_hash_library_entry_t* stop = hlentry + hashlib_response.num_entries;

      for (; hlentry < stop; ++hlentry, ++entry) {
        snprintf(entry->hash, sizeof(entry->hash), "%s", hlentry->hash);
        entry->game_id = hlentry->game_id;
      }

      list->num_entries = hashlib_response.num_entries;

      hashlib_callback_data->callback(RC_OK, NULL, list, client, hashlib_callback_data->callback_userdata);
    }
  }

  rc_api_destroy_fetch_hash_library_response(&hashlib_response);
  free(hashlib_callback_data);
}

rc_client_async_handle_t* rc_client_begin_fetch_hash_library(rc_client_t* client, uint32_t console_id,
                                                             rc_client_fetch_hash_library_callback_t callback,
                                                             void* callback_userdata)
{
  rc_api_fetch_hash_library_request_t api_params;
  rc_client_fetch_hash_library_callback_data_t* callback_data;
  rc_client_async_handle_t* async_handle;
  rc_api_request_t request;
  int result;
  const char* error_message;

  if (!client) {
    callback(RC_INVALID_STATE, "client is required", NULL, client, callback_userdata);
    return NULL;
  }

  api_params.console_id = console_id;
  result = rc_api_init_fetch_hash_library_request_hosted(&request, &api_params, &client->state.host);

  if (result != RC_OK) {
    error_message = rc_error_str(result);
    callback(result, error_message, NULL, client, callback_userdata);
    return NULL;
  }

  callback_data = (rc_client_fetch_hash_library_callback_data_t*)calloc(1, sizeof(*callback_data));
  if (!callback_data) {
    callback(RC_OUT_OF_MEMORY, rc_error_str(RC_OUT_OF_MEMORY), NULL, client, callback_userdata);
    return NULL;
  }

  callback_data->client = client;
  callback_data->callback = callback;
  callback_data->callback_userdata = callback_userdata;
  callback_data->console_id = console_id;

  async_handle = &callback_data->async_handle;
  rc_client_begin_async(client, async_handle);
  client->callbacks.server_call(&request, rc_client_fetch_hash_library_callback, callback_data, client);
  rc_api_destroy_request(&request);

  return rc_client_async_handle_valid(client, async_handle) ? async_handle : NULL;
}

void rc_client_destroy_hash_library(rc_client_hash_library_t* list)
{
  free(list);
}

/* ===== Fetch Game Titles ===== */

typedef struct rc_client_fetch_game_titles_callback_data_t {
  rc_client_t* client;
  rc_client_fetch_game_titles_callback_t callback;
  void* callback_userdata;
  rc_client_async_handle_t async_handle;
} rc_client_fetch_game_titles_callback_data_t;

static void rc_client_fetch_game_titles_callback(const rc_api_server_response_t* server_response, void* callback_data)
{
  rc_client_fetch_game_titles_callback_data_t* titles_callback_data =
    (rc_client_fetch_game_titles_callback_data_t*)callback_data;
  rc_client_t* client = titles_callback_data->client;
  rc_api_fetch_game_titles_response_t titles_response;
  const char* error_message;
  int result;

  result = rc_client_end_async(client, &titles_callback_data->async_handle);
  if (result) {
    if (result != RC_CLIENT_ASYNC_DESTROYED)
      RC_CLIENT_LOG_VERBOSE(client, "Fetch game titles aborted");

    free(titles_callback_data);
    return;
  }

  result = rc_api_process_fetch_game_titles_server_response(&titles_response, server_response);
  error_message =
    rc_client_server_error_message(&result, server_response->http_status_code, &titles_response.response);
  if (error_message) {
    RC_CLIENT_LOG_ERR_FORMATTED(client, "Fetch game titles failed: %s", error_message);
    titles_callback_data->callback(result, error_message, NULL, client, titles_callback_data->callback_userdata);
  } else {
    rc_client_game_title_list_t* list;
    size_t strings_size = 0;
    const rc_api_game_title_entry_t* src;
    const rc_api_game_title_entry_t* stop;
    size_t list_size;

    /* calculate string buffer size */
    for (src = titles_response.entries, stop = src + titles_response.num_entries; src < stop; ++src) {
      if (src->title)
        strings_size += strlen(src->title) + 1;
      if (src->image_url)
        strings_size += strlen(src->image_url) + 1;
    }

    list_size = sizeof(*list) + sizeof(rc_client_game_title_entry_t) * titles_response.num_entries + strings_size;
    list = (rc_client_game_title_list_t*)malloc(list_size);
    if (!list) {
      titles_callback_data->callback(RC_OUT_OF_MEMORY, rc_error_str(RC_OUT_OF_MEMORY), NULL, client,
                                     titles_callback_data->callback_userdata);
    } else {
      rc_client_game_title_entry_t* entry = list->entries =
        (rc_client_game_title_entry_t*)((uint8_t*)list + sizeof(*list));
      char* strings = (char*)((uint8_t*)list + sizeof(*list) +
                              sizeof(rc_client_game_title_entry_t) * titles_response.num_entries);

      for (src = titles_response.entries, stop = src + titles_response.num_entries; src < stop; ++src, ++entry) {
        entry->game_id = src->id;

        if (src->title) {
          const size_t len = strlen(src->title) + 1;
          entry->title = strings;
          memcpy(strings, src->title, len);
          strings += len;
        } else {
          entry->title = NULL;
        }

        if (src->image_name)
          snprintf(entry->badge_name, sizeof(entry->badge_name), "%s", src->image_name);
        else
          entry->badge_name[0] = '\0';

        if (src->image_url) {
          const size_t len = strlen(src->image_url) + 1;
          entry->badge_url = strings;
          memcpy(strings, src->image_url, len);
          strings += len;
        }
        else {
          entry->badge_url = NULL;
        }
      }

      list->num_entries = titles_response.num_entries;

      titles_callback_data->callback(RC_OK, NULL, list, client, titles_callback_data->callback_userdata);
    }
  }

  rc_api_destroy_fetch_game_titles_response(&titles_response);
  free(titles_callback_data);
}

rc_client_async_handle_t* rc_client_begin_fetch_game_titles(rc_client_t* client, const uint32_t* game_ids,
                                                            uint32_t num_game_ids,
                                                            rc_client_fetch_game_titles_callback_t callback,
                                                            void* callback_userdata)
{
  rc_api_fetch_game_titles_request_t api_params;
  rc_client_fetch_game_titles_callback_data_t* callback_data;
  rc_client_async_handle_t* async_handle;
  rc_api_request_t request;
  int result;
  const char* error_message;

  if (!client) {
    callback(RC_INVALID_STATE, "client is required", NULL, client, callback_userdata);
    return NULL;
  }

  if (!game_ids || num_game_ids == 0) {
    callback(RC_INVALID_STATE, "game_ids is required", NULL, client, callback_userdata);
    return NULL;
  }

  api_params.game_ids = game_ids;
  api_params.num_game_ids = num_game_ids;
  result = rc_api_init_fetch_game_titles_request_hosted(&request, &api_params, &client->state.host);

  if (result != RC_OK) {
    error_message = rc_error_str(result);
    callback(result, error_message, NULL, client, callback_userdata);
    return NULL;
  }

  callback_data = (rc_client_fetch_game_titles_callback_data_t*)calloc(1, sizeof(*callback_data));
  if (!callback_data) {
    callback(RC_OUT_OF_MEMORY, rc_error_str(RC_OUT_OF_MEMORY), NULL, client, callback_userdata);
    return NULL;
  }

  callback_data->client = client;
  callback_data->callback = callback;
  callback_data->callback_userdata = callback_userdata;

  async_handle = &callback_data->async_handle;
  rc_client_begin_async(client, async_handle);
  client->callbacks.server_call(&request, rc_client_fetch_game_titles_callback, callback_data, client);
  rc_api_destroy_request(&request);

  return rc_client_async_handle_valid(client, async_handle) ? async_handle : NULL;
}

void rc_client_destroy_game_title_list(rc_client_game_title_list_t* list)
{
  free(list);
}

/* ===== Fetch Games List ===== */

typedef struct rc_client_fetch_game_list_callback_data_t {
  rc_client_t* client;
  rc_client_fetch_game_list_callback_t callback;
  void* callback_userdata;
  rc_client_async_handle_t async_handle;
} rc_client_fetch_game_list_callback_data_t;

static void rc_client_fetch_game_list_callback(const rc_api_server_response_t* server_response, void* callback_data)
{
  rc_client_fetch_game_list_callback_data_t* list_callback_data =
    (rc_client_fetch_game_list_callback_data_t*)callback_data;
  rc_client_t* client = list_callback_data->client;
  rc_api_fetch_games_list_response_t list_response;
  const char* error_message;
  int result;

  result = rc_client_end_async(client, &list_callback_data->async_handle);
  if (result) {
    if (result != RC_CLIENT_ASYNC_DESTROYED)
      RC_CLIENT_LOG_VERBOSE(client, "Fetch game list aborted");

    free(list_callback_data);
    return;
  }

  result = rc_api_process_fetch_games_list_server_response(&list_response, server_response);
  error_message =
    rc_client_server_error_message(&result, server_response->http_status_code, &list_response.response);
  if (error_message) {
    RC_CLIENT_LOG_ERR_FORMATTED(client, "Fetch game list failed: %s", error_message);
    list_callback_data->callback(result, error_message, NULL, client, list_callback_data->callback_userdata);
  } else {
    rc_client_game_list_t* list;
    size_t strings_size = 0, hashes_size = 0;
    const rc_api_game_list_entry_t* src;
    const rc_api_game_list_entry_t* stop;
    size_t list_size;
    uint32_t i;

    /* calculate string buffer size */
    for (src = list_response.entries, stop = src + list_response.num_entries; src < stop; ++src) {
      if (src->name)
        strings_size += strlen(src->name) + 1;
      if (src->image_name)
        strings_size += strlen(src->image_name) + 1;
      if (src->image_url)
        strings_size += strlen(src->image_url) + 1;
      hashes_size += src->num_supported_hashes;
      for (i = 0; i < src->num_supported_hashes; i++)
        strings_size += strlen(src->supported_hashes[i]) + 1;
      hashes_size += src->num_unsupported_hashes;
      for (i = 0; i < src->num_unsupported_hashes; i++)
        strings_size += strlen(src->unsupported_hashes[i]) + 1;
    }

    list_size = sizeof(*list) + sizeof(rc_client_game_list_entry_t) * list_response.num_entries +
                sizeof(const char*) * hashes_size + strings_size;
    list = (rc_client_game_list_t*)malloc(list_size);
    if (!list) {
      list_callback_data->callback(RC_OUT_OF_MEMORY, rc_error_str(RC_OUT_OF_MEMORY), NULL, client,
                                     list_callback_data->callback_userdata);
    } else {
      rc_client_game_list_entry_t* entry = list->entries =
        (rc_client_game_list_entry_t*)((uint8_t*)list + sizeof(*list));
      const char** hash_list =
        (const char**)((uint8_t*)entry + sizeof(rc_client_game_list_entry_t) * list_response.num_entries);
      char* strings = (char*)((uint8_t*)hash_list + sizeof(const char*) * hashes_size);
      size_t len;

      for (src = list_response.entries, stop = src + list_response.num_entries; src < stop; ++src, ++entry) {
        entry->id = src->id;
        entry->num_achievements = src->num_achievements;
        entry->num_leaderboards = src->num_leaderboards;
        entry->points = src->points;

        if (src->name) {
          len = strlen(src->name) + 1;
          entry->name = strings;
          memcpy(strings, src->name, len);
          strings += len;
        } else {
          entry->name = NULL;
        }

        if (src->image_name) {
          len = strlen(src->image_name) + 1;
          entry->image_name = strings;
          memcpy(strings, src->image_name, len);
          strings += len;
        } else {
          entry->image_name = NULL;
        }

        if (src->image_url) {
          len = strlen(src->image_url) + 1;
          entry->image_url = strings;
          memcpy(strings, src->image_url, len);
          strings += len;
        } else {
          entry->image_url = NULL;
        }

        if ((entry->num_supported_hashes = src->num_supported_hashes) > 0) {
          entry->supported_hashes = hash_list;
          for (i = 0; i < src->num_supported_hashes; i++) {
            len = strlen(src->supported_hashes[i]) + 1;
            *(hash_list++) = strings;
            memcpy(strings, src->supported_hashes[i], len);
            strings += len;
          }
        } else {
          entry->supported_hashes = NULL;
        }

        if ((entry->num_unsupported_hashes = src->num_unsupported_hashes) > 0) {
          entry->unsupported_hashes = hash_list;
          for (i = 0; i < src->num_unsupported_hashes; i++) {
            len = strlen(src->unsupported_hashes[i]) + 1;
            *(hash_list++) = strings;
            memcpy(strings, src->unsupported_hashes[i], len);
            strings += len;
          }
        } else {
          entry->unsupported_hashes = NULL;
        }
      }

      list->num_entries = list_response.num_entries;

      list_callback_data->callback(RC_OK, NULL, list, client, list_callback_data->callback_userdata);
    }
  }

  rc_api_destroy_fetch_games_list_response(&list_response);
  free(list_callback_data);
}

rc_client_async_handle_t* rc_client_begin_fetch_game_list(rc_client_t* client, uint32_t console_id,
                                                            rc_client_fetch_game_list_callback_t callback,
                                                            void* callback_userdata)
{
  rc_api_fetch_games_list_request_t api_params;
  rc_client_fetch_game_list_callback_data_t* callback_data;
  rc_client_async_handle_t* async_handle;
  rc_api_request_t request;
  int result;
  const char* error_message;

  if (!client) {
    callback(RC_INVALID_STATE, "client is required", NULL, client, callback_userdata);
    return NULL;
  }

  api_params.console_id = console_id;
  result = rc_api_init_fetch_games_list_request_hosted(&request, &api_params, &client->state.host);

  if (result != RC_OK) {
    error_message = rc_error_str(result);
    callback(result, error_message, NULL, client, callback_userdata);
    return NULL;
  }

  callback_data = (rc_client_fetch_game_list_callback_data_t*)calloc(1, sizeof(*callback_data));
  if (!callback_data) {
    callback(RC_OUT_OF_MEMORY, rc_error_str(RC_OUT_OF_MEMORY), NULL, client, callback_userdata);
    return NULL;
  }

  callback_data->client = client;
  callback_data->callback = callback;
  callback_data->callback_userdata = callback_userdata;

  async_handle = &callback_data->async_handle;
  rc_client_begin_async(client, async_handle);
  client->callbacks.server_call(&request, rc_client_fetch_game_list_callback, callback_data, client);
  rc_api_destroy_request(&request);

  return rc_client_async_handle_valid(client, async_handle) ? async_handle : NULL;
}

void rc_client_destroy_game_list(rc_client_game_list_t* list)
{
  free(list);
}

/* ===== Fetch All User Progress ===== */

typedef struct rc_client_fetch_all_user_progress_callback_data_t {
  rc_client_t* client;
  rc_client_fetch_all_user_progress_callback_t callback;
  void* callback_userdata;
  uint32_t console_id;
  rc_client_async_handle_t async_handle;
} rc_client_fetch_all_user_progress_callback_data_t;

static void rc_client_fetch_all_user_progress_callback(const rc_api_server_response_t* server_response,
  void* callback_data)
{
  rc_client_fetch_all_user_progress_callback_data_t* ap_callback_data =
    (rc_client_fetch_all_user_progress_callback_data_t*)callback_data;
  rc_client_t* client = ap_callback_data->client;
  rc_api_fetch_all_user_progress_response_t ap_response;
  const char* error_message;
  int result;

  result = rc_client_end_async(client, &ap_callback_data->async_handle);
  if (result) {
    if (result != RC_CLIENT_ASYNC_DESTROYED)
      RC_CLIENT_LOG_VERBOSE(client, "Fetch all progress aborted");

    free(ap_callback_data);
    return;
  }

  result = rc_api_process_fetch_all_user_progress_server_response(&ap_response, server_response);
  error_message = rc_client_server_error_message(&result, server_response->http_status_code, &ap_response.response);
  if (error_message) {
    RC_CLIENT_LOG_ERR_FORMATTED(client, "Fetch all progress for console %u failed: %s", ap_callback_data->console_id,
      error_message);
    ap_callback_data->callback(result, error_message, NULL, client, ap_callback_data->callback_userdata);
  }
  else {
    rc_client_all_user_progress_t* list;
    const size_t list_size = sizeof(*list) + sizeof(rc_client_all_user_progress_entry_t) * ap_response.num_entries;

    list = (rc_client_all_user_progress_t*)malloc(list_size);
    if (!list) {
      ap_callback_data->callback(RC_OUT_OF_MEMORY, rc_error_str(RC_OUT_OF_MEMORY), NULL, client,
        ap_callback_data->callback_userdata);
    }
    else {
      rc_client_all_user_progress_entry_t* entry = list->entries =
        (rc_client_all_user_progress_entry_t*)((uint8_t*)list + sizeof(*list));
      const rc_api_all_user_progress_entry_t* hlentry = ap_response.entries;
      const rc_api_all_user_progress_entry_t* stop = hlentry + ap_response.num_entries;

      for (; hlentry < stop; ++hlentry, ++entry)
      {
        entry->game_id = hlentry->game_id;
        entry->num_achievements = hlentry->num_achievements;
        entry->num_unlocked_achievements = hlentry->num_unlocked_achievements;
        entry->num_unlocked_achievements_hardcore = hlentry->num_unlocked_achievements_hardcore;
      }

      list->num_entries = ap_response.num_entries;

      ap_callback_data->callback(RC_OK, NULL, list, client, ap_callback_data->callback_userdata);
    }
  }

  rc_api_destroy_fetch_all_user_progress_response(&ap_response);
  free(ap_callback_data);
}

rc_client_async_handle_t* rc_client_begin_fetch_all_user_progress(rc_client_t* client, uint32_t console_id,
  rc_client_fetch_all_user_progress_callback_t callback,
  void* callback_userdata)
{
  rc_api_fetch_all_user_progress_request_t api_params;
  rc_client_fetch_all_user_progress_callback_data_t* callback_data;
  rc_client_async_handle_t* async_handle;
  rc_api_request_t request;
  int result;
  const char* error_message;

  if (!client) {
    callback(RC_INVALID_STATE, "client is required", NULL, client, callback_userdata);
    return NULL;
  }
  else if (client->state.user != RC_CLIENT_USER_STATE_LOGGED_IN) {
    callback(RC_INVALID_STATE, "client must be logged in", NULL, client, callback_userdata);
    return NULL;
  }

  api_params.username = client->user.username;
  api_params.api_token = client->user.token;
  api_params.console_id = console_id;

  result = rc_api_init_fetch_all_user_progress_request_hosted(&request, &api_params, &client->state.host);

  if (result != RC_OK) {
    error_message = rc_error_str(result);
    callback(result, error_message, NULL, client, callback_userdata);
    return NULL;
  }

  callback_data = (rc_client_fetch_all_user_progress_callback_data_t*)calloc(1, sizeof(*callback_data));
  if (!callback_data) {
    callback(RC_OUT_OF_MEMORY, rc_error_str(RC_OUT_OF_MEMORY), NULL, client, callback_userdata);
    return NULL;
  }

  callback_data->client = client;
  callback_data->callback = callback;
  callback_data->callback_userdata = callback_userdata;
  callback_data->console_id = console_id;

  async_handle = &callback_data->async_handle;
  rc_client_begin_async(client, async_handle);
  client->callbacks.server_call(&request, rc_client_fetch_all_user_progress_callback, callback_data, client);
  rc_api_destroy_request(&request);

  return rc_client_async_handle_valid(client, async_handle) ? async_handle : NULL;
}

void rc_client_destroy_all_user_progress(rc_client_all_user_progress_t* list)
{
  free(list);
}
