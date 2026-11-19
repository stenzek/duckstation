#ifndef RC_CLIENT_RAINTEGRATION_H
#define RC_CLIENT_RAINTEGRATION_H

#ifndef _WIN32
 #undef RC_CLIENT_SUPPORTS_RAINTEGRATION /* Windows required for RAIntegration */
#endif

#include <stdint.h>

#include "rc_export.h"

RC_BEGIN_C_DECLS

struct rc_client_t; /* rc_client_internal.h */

/* types needed to implement raintegration */

typedef struct rc_client_raintegration_menu_item_t {
  const char* label;
  uint32_t id;
  uint8_t checked;
  uint8_t enabled;
} rc_client_raintegration_menu_item_t;

typedef struct rc_client_raintegration_menu_t {
  rc_client_raintegration_menu_item_t* items;
  uint32_t num_items;
} rc_client_raintegration_menu_t;

enum {
  RC_CLIENT_RAINTEGRATION_ACHIEVEMENT_STATE_NONE = 0,
  RC_CLIENT_RAINTEGRATION_ACHIEVEMENT_STATE_PUBLISHED = 1,
  RC_CLIENT_RAINTEGRATION_ACHIEVEMENT_STATE_LOCAL = 2,
  RC_CLIENT_RAINTEGRATION_ACHIEVEMENT_STATE_MODIFIED = 3,
  RC_CLIENT_RAINTEGRATION_ACHIEVEMENT_STATE_INSECURE = 4
};

enum {
  RC_CLIENT_RAINTEGRATION_EVENT_TYPE_NONE = 0,
  RC_CLIENT_RAINTEGRATION_EVENT_MENUITEM_CHECKED_CHANGED = 1, /* [menu_item] checked changed */
  RC_CLIENT_RAINTEGRATION_EVENT_HARDCORE_CHANGED = 2, /* hardcore was enabled or disabled */
  RC_CLIENT_RAINTEGRATION_EVENT_PAUSE = 3, /* emulated system should be paused */
  RC_CLIENT_RAINTEGRATION_EVENT_MENU_CHANGED = 4 /* one or more items were added/removed from the menu and it should be rebuilt */
};

typedef struct rc_client_raintegration_event_t {
  uint32_t type;

  const rc_client_raintegration_menu_item_t* menu_item;
} rc_client_raintegration_event_t;

typedef void (RC_CCONV *rc_client_raintegration_event_handler_t)(const rc_client_raintegration_event_t* event,
                                                                 struct rc_client_t* client);

typedef void (RC_CCONV *rc_client_raintegration_write_memory_func_t)(uint32_t address, uint8_t* buffer,
                                                                     uint32_t num_bytes, struct rc_client_t* client);

typedef void (RC_CCONV* rc_client_raintegration_get_game_name_func_t)(char* buffer, uint32_t buffer_size, struct rc_client_t* client);

/* types needed to integrate raintegration */

#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION

#ifndef RC_CLIENT_SUPPORTS_EXTERNAL
 #define RC_CLIENT_SUPPORTS_EXTERNAL /* external rc_client required for RAIntegration */
#endif

#include <wtypes.h> /* HWND */

struct rc_client_async_handle_t; /* rc_client_async.h */

#include "rc_client_callbacks.h"

RC_EXPORT struct rc_client_async_handle_t* RC_CCONV rc_client_begin_load_raintegration(struct rc_client_t* client,
    const wchar_t* search_directory, HWND main_window_handle,
    const char* client_name, const char* client_version,
    rc_client_callback_t callback, void* callback_userdata);

RC_EXPORT struct rc_client_async_handle_t* RC_CCONV rc_client_begin_load_raintegration_deferred(struct rc_client_t* client,
    const wchar_t* search_directory, rc_client_callback_t callback, void* callback_userdata);

RC_EXPORT int RC_CCONV rc_client_finish_load_raintegration(struct rc_client_t* client,
    HWND main_window_handle, const char* client_name, const char* client_version, const char** error_message);

RC_EXPORT void RC_CCONV rc_client_unload_raintegration(struct rc_client_t* client);

RC_EXPORT void RC_CCONV rc_client_raintegration_update_main_window_handle(struct rc_client_t* client, HWND main_window_handle);

RC_EXPORT const rc_client_raintegration_menu_t* RC_CCONV rc_client_raintegration_get_menu(const struct rc_client_t* client);

RC_EXPORT void RC_CCONV rc_client_raintegration_rebuild_submenu(struct rc_client_t* client, HMENU hMenu);
RC_EXPORT void RC_CCONV rc_client_raintegration_update_menu_item(const struct rc_client_t* client, const rc_client_raintegration_menu_item_t* menu_item);
RC_EXPORT int RC_CCONV rc_client_raintegration_activate_menu_item(const struct rc_client_t* client, uint32_t menu_item_id);

RC_EXPORT void RC_CCONV rc_client_raintegration_set_write_memory_function(struct rc_client_t* client, rc_client_raintegration_write_memory_func_t handler);
RC_EXPORT void RC_CCONV rc_client_raintegration_set_get_game_name_function(struct rc_client_t* client, rc_client_raintegration_get_game_name_func_t handler);
RC_EXPORT void RC_CCONV rc_client_raintegration_set_console_id(struct rc_client_t* client, uint32_t console_id);
RC_EXPORT int RC_CCONV rc_client_raintegration_has_modifications(const struct rc_client_t* client);

RC_EXPORT void RC_CCONV rc_client_raintegration_set_event_handler(struct rc_client_t* client,
    rc_client_raintegration_event_handler_t handler);

RC_EXPORT int RC_CCONV rc_client_raintegration_get_achievement_state(const struct rc_client_t* client, uint32_t achievement_id);

#endif /* RC_CLIENT_SUPPORTS_RAINTEGRATION */

RC_END_C_DECLS

#endif /* RC_CLIENT_RAINTEGRATION_H */
