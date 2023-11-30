#ifndef RC_CLIENT_RAINTEGRATION_H
#define RC_CLIENT_RAINTEGRATION_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _WIN32
 #undef RC_CLIENT_SUPPORTS_RAINTEGRATION /* Windows required for RAIntegration */
#endif

#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION

#ifndef RC_CLIENT_SUPPORTS_EXTERNAL
 #define RC_CLIENT_SUPPORTS_EXTERNAL /* external rc_client required for RAIntegration */
#endif

#include "rc_client.h"

#include <wtypes.h> /* HWND */

rc_client_async_handle_t* rc_client_begin_load_raintegration(rc_client_t* client,
    const wchar_t* search_directory, HWND main_window_handle,
    const char* client_name, const char* client_version,
    rc_client_callback_t callback, void* callback_userdata);

void rc_client_unload_raintegration(rc_client_t* client);

#endif /* RC_CLIENT_SUPPORTS_RAINTEGRATION */

#ifdef __cplusplus
}
#endif

#endif /* RC_CLIENT_RAINTEGRATION_H */
