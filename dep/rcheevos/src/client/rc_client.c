#include "rc_client_async.h"
#include "rc_client_do_frame.h"
#include "rc_client_game.h"
#include "rc_client_internal.h"
#include "rc_client_load.h"

#include "../api/rc_api_common.h"

#include "../util/rc_version.h"

#include <stdarg.h>

#ifdef _WIN32
 #define WIN32_LEAN_AND_MEAN
 #include <windows.h>
#else
 #include <time.h>
#endif

/* ===== natvis extensions ===== */

typedef struct __rc_client_achievement_state_enum_t { uint8_t value; } __rc_client_achievement_state_enum_t;
typedef struct __rc_client_achievement_category_enum_t { uint8_t value; } __rc_client_achievement_category_enum_t;
typedef struct __rc_client_achievement_type_enum_t { uint8_t value; } __rc_client_achievement_type_enum_t;
typedef struct __rc_client_achievement_bucket_enum_t { uint8_t value; } __rc_client_achievement_bucket_enum_t;
typedef struct __rc_client_achievement_unlocked_enum_t { uint8_t value; } __rc_client_achievement_unlocked_enum_t;
typedef struct __rc_client_leaderboard_state_enum_t { uint8_t value; } __rc_client_leaderboard_state_enum_t;
typedef struct __rc_client_leaderboard_format_enum_t { uint8_t value; } __rc_client_leaderboard_format_enum_t;
typedef struct __rc_client_log_level_enum_t { uint8_t value; } __rc_client_log_level_enum_t;
typedef struct __rc_client_event_type_enum_t { uint8_t value; } __rc_client_event_type_enum_t;
typedef struct __rc_client_load_game_state_enum_t { uint8_t value; } __rc_client_load_game_state_enum_t;
typedef struct __rc_client_user_state_enum_t { uint8_t value; } __rc_client_user_state_enum_t;
typedef struct __rc_client_mastery_state_enum_t { uint8_t value; } __rc_client_mastery_state_enum_t;
typedef struct __rc_client_spectator_mode_enum_t { uint8_t value; } __rc_client_spectator_mode_enum_t;
typedef struct __rc_client_disconnect_enum_t { uint8_t value; } __rc_client_disconnect_enum_t;
typedef struct __rc_client_leaderboard_tracker_list_t { rc_client_leaderboard_tracker_info_t* first; } __rc_client_leaderboard_tracker_list_t;
typedef struct __rc_client_subset_info_list_t { rc_client_subset_info_t* first; } __rc_client_subset_info_list_t;
typedef struct __rc_client_media_hash_list_t { rc_client_media_hash_t* first; } __rc_client_media_hash_list_t;
typedef struct __rc_client_subset_info_achievements_list_t { rc_client_subset_info_t info; } __rc_client_subset_info_achievements_list_t;
typedef struct __rc_client_subset_info_leaderboards_list_t { rc_client_subset_info_t info; } __rc_client_subset_info_leaderboards_list_t;
typedef struct __rc_client_scheduled_callback_list_t { rc_client_state_t state; } __rc_client_scheduled_callback_list_t;
typedef struct __rc_client_game_hash_list_t { rc_client_t client; } __rc_client_game_hash_list_t;

static void rc_client_natvis_helper(const rc_client_event_t* event, rc_client_t* client)
{
  struct natvis_extensions {
    __rc_client_achievement_state_enum_t achievement_state;
    __rc_client_achievement_category_enum_t achievement_category;
    __rc_client_achievement_type_enum_t achievement_type;
    __rc_client_achievement_bucket_enum_t achievement_bucket;
    __rc_client_achievement_unlocked_enum_t achievement_unlocked;
    __rc_client_leaderboard_state_enum_t leaderboard_state;
    __rc_client_leaderboard_format_enum_t leaderboard_format;
    __rc_client_log_level_enum_t log_level;
    __rc_client_event_type_enum_t event_type;
    __rc_client_load_game_state_enum_t load_game_state;
    __rc_client_user_state_enum_t user_state;
    __rc_client_mastery_state_enum_t mastery_state;
    __rc_client_spectator_mode_enum_t spectator_mode;
    __rc_client_disconnect_enum_t disconnect;
    __rc_client_leaderboard_tracker_list_t leaderboard_tracker_list;
    __rc_client_subset_info_list_t subset_info_list;
    __rc_client_media_hash_list_t media_hash_list;
    __rc_client_subset_info_achievements_list_t subset_info_achievements_list;
    __rc_client_subset_info_leaderboards_list_t subset_info_leaderboards_list;
    __rc_client_scheduled_callback_list_t scheduled_callback_list;
    __rc_client_game_hash_list_t client_game_hash_list;
  } natvis;

  memset(&natvis, 0, sizeof(natvis));
  (void)event;
  (void)client;

  /* this code should never be executed. it just ensures these constants get defined for
   * the natvis VisualStudio extension as they're not used directly in the code. */
  natvis.achievement_type.value = RC_CLIENT_ACHIEVEMENT_TYPE_STANDARD;
  natvis.achievement_type.value = RC_CLIENT_ACHIEVEMENT_TYPE_MISSABLE;
  natvis.achievement_type.value = RC_CLIENT_ACHIEVEMENT_TYPE_PROGRESSION;
  natvis.achievement_type.value = RC_CLIENT_ACHIEVEMENT_TYPE_WIN;
  natvis.achievement_category.value = RC_CLIENT_ACHIEVEMENT_CATEGORY_NONE;
  natvis.event_type.value = RC_CLIENT_EVENT_TYPE_NONE;
}

/* ===== Construction/Destruction ===== */

static void rc_client_dummy_event_handler(const rc_client_event_t* event, rc_client_t* client)
{
  (void)event;
  (void)client;
}

rc_client_t* rc_client_create(rc_client_read_memory_func_t read_memory_function, rc_client_server_call_t server_call_function)
{
  rc_client_t* client = (rc_client_t*)calloc(1, sizeof(rc_client_t));
  if (!client)
    return NULL;

  client->state.hardcore = 1;
  client->state.required_unpaused_frames = RC_MINIMUM_UNPAUSED_FRAMES;
  client->state.allow_background_memory_reads = 1;

  client->callbacks.read_memory = read_memory_function;
  client->callbacks.server_call = server_call_function;
  client->callbacks.event_handler = rc_client_natvis_helper;
  client->callbacks.event_handler = rc_client_dummy_event_handler;
  rc_client_set_get_time_millisecs_function(client, NULL);

  rc_mutex_init(&client->state.mutex);

  rc_buffer_init(&client->state.buffer);

  return client;
}

void rc_client_destroy(rc_client_t* client)
{
  if (!client)
    return;

  rc_mutex_lock(&client->state.mutex);
  {
    size_t i;
    for (i = 0; i < sizeof(client->state.async_handles) / sizeof(client->state.async_handles[0]); ++i) {
      if (client->state.async_handles[i])
        client->state.async_handles[i]->aborted = RC_CLIENT_ASYNC_DESTROYED;
    }

    if (client->state.load) {
      client->state.load->async_handle.aborted = RC_CLIENT_ASYNC_DESTROYED;
      client->state.load = NULL;
    }
  }
  rc_mutex_unlock(&client->state.mutex);

  rc_client_unload_game(client);

#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
  rc_client_unload_raintegration(client);
#endif

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client && client->state.external_client->destroy)
    client->state.external_client->destroy();
#endif

  rc_buffer_destroy(&client->state.buffer);

  rc_mutex_destroy(&client->state.mutex);

  free(client);
}

/* ===== Logging ===== */

void rc_client_log_message(const rc_client_t* client, const char* message)
{
  if (client->callbacks.log_call)
    client->callbacks.log_call(message, client);
}

static void rc_client_log_message_va(const rc_client_t* client, const char* format, va_list args)
{
  if (client->callbacks.log_call) {
    char buffer[2048];

#ifdef __STDC_SECURE_LIB__
    vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, args);
#elif __STDC_VERSION__ >= 199901L /* vsnprintf requires c99 */
    vsnprintf(buffer, sizeof(buffer), format, args);
#else /* c89 doesn't have a size-limited vsprintf function - assume the buffer is large enough */
    vsprintf(buffer, format, args);
#endif

    client->callbacks.log_call(buffer, client);
  }
}

#ifdef RC_NO_VARIADIC_MACROS

void RC_CLIENT_LOG_ERR_FORMATTED(const rc_client_t* client, const char* format, ...)
{
  if (client->state.log_level >= RC_CLIENT_LOG_LEVEL_ERROR) {
    va_list args;
    va_start(args, format);
    rc_client_log_message_va(client, format, args);
    va_end(args);
  }
}

void RC_CLIENT_LOG_WARN_FORMATTED(const rc_client_t* client, const char* format, ...)
{
  if (client->state.log_level >= RC_CLIENT_LOG_LEVEL_WARN) {
    va_list args;
    va_start(args, format);
    rc_client_log_message_va(client, format, args);
    va_end(args);
  }
}

void RC_CLIENT_LOG_INFO_FORMATTED(const rc_client_t* client, const char* format, ...)
{
  if (client->state.log_level >= RC_CLIENT_LOG_LEVEL_INFO) {
    va_list args;
    va_start(args, format);
    rc_client_log_message_va(client, format, args);
    va_end(args);
  }
}

void RC_CLIENT_LOG_VERBOSE_FORMATTED(const rc_client_t* client, const char* format, ...)
{
  if (client->state.log_level >= RC_CLIENT_LOG_LEVEL_VERBOSE) {
    va_list args;
    va_start(args, format);
    rc_client_log_message_va(client, format, args);
    va_end(args);
  }
}

#else

void rc_client_log_message_formatted(const rc_client_t* client, const char* format, ...)
{
  va_list args;
  va_start(args, format);
  rc_client_log_message_va(client, format, args);
  va_end(args);
}

#endif /* RC_NO_VARIADIC_MACROS */

void rc_client_enable_logging(rc_client_t* client, int level, rc_client_message_callback_t callback)
{
  client->callbacks.log_call = callback;
  client->state.log_level = callback ? (uint8_t)level : RC_CLIENT_LOG_LEVEL_NONE;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client && client->state.external_client->enable_logging)
    client->state.external_client->enable_logging(client, level, callback);
#endif
}

/* ===== Clock ===== */

#if defined(__APPLE__) && defined(__MACH__)
 #ifdef CLOCK_MONOTONIC
  /* clock_gettime() was added to Darwin in iOS 10.0 and macOS 10.12.
   * On earlier deployment targets (like Leopard 10.5), the symbol doesn't exist
   * in libSystem causing an "undefined reference to clock_gettime" linker error.
   * To get the code to use the #else block below, forcibly undefine CLOCK_MONOTONIC
   * when targeting earlier versions. */
  #include <AvailabilityMacros.h>
  #if (defined(MAC_OS_X_VERSION_MIN_REQUIRED) && MAC_OS_X_VERSION_MIN_REQUIRED < 101200)
   #undef CLOCK_MONOTONIC
  #elif (defined(__IPHONE_OS_VERSION_MIN_REQUIRED) && __IPHONE_OS_VERSION_MIN_REQUIRED < 100000)
   #undef CLOCK_MONOTONIC
  #endif
 #endif
#endif

static rc_clock_t rc_client_clock_get_now_millisecs(const rc_client_t* client)
{
#if defined(CLOCK_MONOTONIC)
  struct timespec now;
  (void)client;

  if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
    return 0;

  /* round nanoseconds to nearest millisecond and add to seconds */
  return ((rc_clock_t)now.tv_sec * 1000 + ((rc_clock_t)now.tv_nsec / 1000000));
#elif defined(_WIN32)
  static LARGE_INTEGER freq;
  LARGE_INTEGER ticks;

  (void)client;

  /* Frequency is the number of ticks per second and is guaranteed to not change. */
  if (!freq.QuadPart) {
    if (!QueryPerformanceFrequency(&freq))
      return 0;

    /* convert to number of ticks per millisecond to simplify later calculations */
    freq.QuadPart /= 1000;
  }

  if (!QueryPerformanceCounter(&ticks))
    return 0;

  return (rc_clock_t)(ticks.QuadPart / freq.QuadPart);
#else
  const clock_t clock_now = clock();

  (void)client;

  if (sizeof(clock_t) == 4) {
    static uint32_t clock_wraps = 0;
    static clock_t last_clock = 0;
    static time_t last_timet = 0;
    const time_t time_now = time(NULL);

    if (last_timet != 0) {
      const time_t seconds_per_clock_t = (time_t)(((uint64_t)1 << 32) / CLOCKS_PER_SEC);
      if (clock_now < last_clock) {
        /* clock() has wrapped */
        ++clock_wraps;
      }
      else if (time_now - last_timet > seconds_per_clock_t) {
        /* it's been long enough that clock() has wrapped and is higher than the last time it was read */
        ++clock_wraps;
      }
    }

    last_timet = time_now;
    last_clock = clock_now;

    return (rc_clock_t)((((uint64_t)clock_wraps << 32) | clock_now) / (CLOCKS_PER_SEC / 1000));
  }
  else {
    return (rc_clock_t)(clock_now / (CLOCKS_PER_SEC / 1000));
  }
#endif
}

void rc_client_set_get_time_millisecs_function(rc_client_t* client, rc_get_time_millisecs_func_t handler)
{
  client->callbacks.get_time_millisecs = handler ? handler : rc_client_clock_get_now_millisecs;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client && client->state.external_client->set_get_time_millisecs)
    client->state.external_client->set_get_time_millisecs(client, handler);
#endif
}

/* ===== Processing ===== */

void rc_client_set_event_handler(rc_client_t* client, rc_client_event_handler_t handler)
{
  if (!client)
    return;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client && client->state.external_client->set_event_handler)
    client->state.external_client->set_event_handler(client, handler);
#endif

  client->callbacks.event_handler = handler;
}

void rc_client_set_read_memory_function(rc_client_t* client, rc_client_read_memory_func_t handler)
{
  if (!client)
    return;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client && client->state.external_client->set_read_memory)
    client->state.external_client->set_read_memory(client, handler);
#endif

  client->callbacks.read_memory = handler;
}

void rc_client_set_allow_background_memory_reads(rc_client_t* client, int allowed)
{
  if (!client)
    return;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client && client->state.external_client->set_allow_background_memory_reads)
    client->state.external_client->set_allow_background_memory_reads(allowed);
#endif

  client->state.allow_background_memory_reads = (uint8_t)allowed;
}

/* ===== Toggles ===== */

static void rc_client_enable_hardcore(rc_client_t* client)
{
  client->state.hardcore = 1;

  if (client->game) {
    rc_client_toggle_hardcore_achievements(client->game, client, RC_CLIENT_ACHIEVEMENT_UNLOCKED_HARDCORE);
    rc_client_activate_leaderboards(client->game, client);

    /* disable processing until the client acknowledges the reset event by calling rc_runtime_reset() */
    RC_CLIENT_LOG_INFO(client, "Hardcore enabled, waiting for reset");
    client->game->waiting_for_reset = 1;
  }
  else {
    RC_CLIENT_LOG_INFO(client, "Hardcore enabled");
  }
}

static void rc_client_disable_hardcore(rc_client_t* client)
{
  client->state.hardcore = 0;
  RC_CLIENT_LOG_INFO(client, "Hardcore disabled");

  if (client->game) {
    rc_client_toggle_hardcore_achievements(client->game, client, RC_CLIENT_ACHIEVEMENT_UNLOCKED_CASUAL);

    if (!client->state.allow_leaderboards_in_casual)
      rc_client_deactivate_leaderboards(client->game, client);
  }
}

void rc_client_set_hardcore_enabled(rc_client_t* client, int enabled)
{
  int changed = 0;

  if (!client)
    return;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client && client->state.external_client->get_hardcore_enabled) {
    client->state.external_client->set_hardcore_enabled(enabled);
    return;
  }
#endif

  rc_mutex_lock(&client->state.mutex);

  enabled = enabled ? 1 : 0;
  if (client->state.hardcore != enabled) {
    if (enabled)
      rc_client_enable_hardcore(client);
    else
      rc_client_disable_hardcore(client);

    changed = 1;
  }

  rc_mutex_unlock(&client->state.mutex);

  /* events must be raised outside of lock */
  if (changed && client->game) {
    if (enabled) {
      /* if enabling hardcore, notify client that a reset is requested */
      if (client->game->waiting_for_reset) {
        rc_client_event_t client_event;
        memset(&client_event, 0, sizeof(client_event));
        client_event.type = RC_CLIENT_EVENT_RESET;
        client->callbacks.event_handler(&client_event, client);
      }
    }
    else {
      /* if disabling hardcore, leaderboards will be deactivated. raise events for hiding trackers */
      rc_client_raise_pending_events(client, client->game);
    }
  }
}

int rc_client_get_hardcore_enabled(const rc_client_t* client)
{
  if (!client)
    return 0;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client && client->state.external_client->get_hardcore_enabled)
    return client->state.external_client->get_hardcore_enabled();
#endif

  return client->state.hardcore;
}

void rc_client_set_unpromoted_enabled(rc_client_t* client, int enabled)
{
  if (!client)
    return;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client && client->state.external_client->set_unpromoted_enabled) {
    client->state.external_client->set_unpromoted_enabled(enabled);
    return;
  }
#endif

  RC_CLIENT_LOG_INFO_FORMATTED(client, "Unpromoted %s", enabled ? "enabled" : "disabled");
  client->state.unpromoted_enabled = enabled ? 1 : 0;
}

int rc_client_get_unpromoted_enabled(const rc_client_t* client)
{
  if (!client)
    return 0;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client && client->state.external_client->get_unpromoted_enabled)
    return client->state.external_client->get_unpromoted_enabled();
#endif

  return client->state.unpromoted_enabled;
}

void rc_client_set_encore_mode_enabled(rc_client_t* client, int enabled)
{
  if (!client)
    return;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client && client->state.external_client->set_encore_mode_enabled) {
    client->state.external_client->set_encore_mode_enabled(enabled);
    return;
  }
#endif

  RC_CLIENT_LOG_INFO_FORMATTED(client, "Encore mode %s", enabled ? "enabled" : "disabled");
  client->state.encore_mode = enabled ? 1 : 0;
}

int rc_client_get_encore_mode_enabled(const rc_client_t* client)
{
  if (!client)
    return 0;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client && client->state.external_client->get_encore_mode_enabled)
    return client->state.external_client->get_encore_mode_enabled();
#endif

  return client->state.encore_mode;
}

void rc_client_set_spectator_mode_enabled(rc_client_t* client, int enabled)
{
  if (!client)
    return;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client && client->state.external_client->set_spectator_mode_enabled) {
    client->state.external_client->set_spectator_mode_enabled(enabled);
    return;
  }
#endif

  if (!enabled && client->state.spectator_mode == RC_CLIENT_SPECTATOR_MODE_LOCKED) {
    RC_CLIENT_LOG_WARN(client, "Spectator mode cannot be disabled if it was enabled prior to loading game.");
    return;
  }

  RC_CLIENT_LOG_INFO_FORMATTED(client, "Spectator mode %s", enabled ? "enabled" : "disabled");
  client->state.spectator_mode = enabled ? RC_CLIENT_SPECTATOR_MODE_ON : RC_CLIENT_SPECTATOR_MODE_OFF;
}

int rc_client_get_spectator_mode_enabled(const rc_client_t* client)
{
  if (!client)
    return 0;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client && client->state.external_client->get_spectator_mode_enabled)
    return client->state.external_client->get_spectator_mode_enabled();
#endif

  return (client->state.spectator_mode == RC_CLIENT_SPECTATOR_MODE_OFF) ? 0 : 1;
}

void rc_client_set_userdata(rc_client_t* client, void* userdata)
{
  if (client)
    client->callbacks.client_data = userdata;
}

void* rc_client_get_userdata(const rc_client_t* client)
{
  return client ? client->callbacks.client_data : NULL;
}

void rc_client_set_host(rc_client_t* client, const char* hostname)
{
  if (!client)
    return;

  if (client->state.host.host && hostname && strcmp(hostname, client->state.host.host) == 0)
    return;

  /* clear out any previously specified host information */
  memset(&client->state.host, 0, sizeof(client->state.host));

  if (hostname && (!hostname[0] || strcmp(hostname, rc_api_default_host()) == 0)) {
    RC_CLIENT_LOG_VERBOSE_FORMATTED(client, "Using host: %s", rc_api_default_host());
    hostname = rc_api_default_host();
  }
  else {
    RC_CLIENT_LOG_VERBOSE_FORMATTED(client, "Using host: %s", hostname);
    client->state.host.host = rc_buffer_strcpy(&client->state.buffer, hostname);
  }

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client->state.external_client && client->state.external_client->set_host)
    client->state.external_client->set_host(hostname);
#endif
}

size_t rc_client_get_user_agent_clause(rc_client_t* client, char buffer[], size_t buffer_size)
{
  size_t result;

#ifdef RC_CLIENT_SUPPORTS_EXTERNAL
  if (client && client->state.external_client && client->state.external_client->get_user_agent_clause) {
    result = client->state.external_client->get_user_agent_clause(buffer, buffer_size);
    if (result > 0) {
      if (result < buffer_size)
        result += snprintf(buffer + result, buffer_size - result, " rc_client/" RCHEEVOS_VERSION_STRING);
      else
        result += (sizeof(" rc_client/" RCHEEVOS_VERSION_STRING) - 1);
      buffer[buffer_size - 1] = '\0';
      return result;
    }
  }
#else
  (void)client;
#endif

  result = snprintf(buffer, buffer_size, "rcheevos/" RCHEEVOS_VERSION_STRING);

  /* some implementations of snprintf will fill the buffer without null terminating.
   * make sure the buffer is null terminated */
  buffer[buffer_size - 1] = '\0';
  return result;
}

int rc_client_is_disconnected(rc_client_t* client)
{
  return (client && (client->state.disconnect & (RC_CLIENT_DISCONNECT_VISIBLE | RC_CLIENT_DISCONNECT_SHOW_PENDING)) != 0);
}
