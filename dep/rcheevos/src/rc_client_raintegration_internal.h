#ifndef RC_CLIENT_RAINTEGRATION_INTERNAL_H
#define RC_CLIENT_RAINTEGRATION_INTERNAL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "rc_client_raintegration.h"

#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION

#include "rc_client_external.h"
#include "rc_compat.h"

#ifndef CCONV
 #define CCONV __cdecl
#endif

typedef void (CCONV* rc_client_raintegration_action_func)(void);
typedef const char* (CCONV* rc_client_raintegration_get_string_func)(void);
typedef int (CCONV* rc_client_raintegration_init_client_func)(HWND hMainWnd, const char* sClientName, const char* sClientVersion);
typedef int (CCONV* rc_client_raintegration_get_external_client)(rc_client_external_t* pClient, int nVersion);

typedef struct rc_client_raintegration_t
{
  HINSTANCE hDLL;

  rc_client_raintegration_get_string_func get_version;
  rc_client_raintegration_get_string_func get_host_url;
  rc_client_raintegration_init_client_func init_client;
  rc_client_raintegration_init_client_func init_client_offline;
  rc_client_raintegration_action_func shutdown;

  rc_client_raintegration_get_external_client get_external_client;

} rc_client_raintegration_t;

#endif /* RC_CLIENT_SUPPORTS_RAINTEGRATION */

#ifdef __cplusplus
}
#endif

#endif /* RC_CLIENT_RAINTEGRATION_INTERNAL_H */
