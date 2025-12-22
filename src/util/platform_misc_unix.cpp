// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "input_manager.h"
#include "platform_misc.h"

#include "common/dynamic_library.h"
#include "common/error.h"
#include "common/log.h"
#include "common/path.h"
#include "common/scoped_guard.h"
#include "common/small_string.h"

#include <cinttypes>
#include <dbus/dbus.h>
#include <mutex>
#include <signal.h>
#include <unistd.h>

LOG_CHANNEL(PlatformMisc);

bool PlatformMisc::InitializeSocketSupport(Error* error)
{
  // Ignore SIGPIPE, we handle errors ourselves.
  if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
  {
    Error::SetErrno(error, "signal(SIGPIPE, SIG_IGN) failed: ", errno);
    return false;
  }

  return true;
}

static bool SetScreensaverInhibitDBus(const bool inhibit_requested, const char* program_name, const char* reason)
{
#define DBUS_FUNCS(X)                                                                                                  \
  X(dbus_error_is_set)                                                                                                 \
  X(dbus_error_free)                                                                                                   \
  X(dbus_message_unref)                                                                                                \
  X(dbus_error_init)                                                                                                   \
  X(dbus_bus_get)                                                                                                      \
  X(dbus_connection_set_exit_on_disconnect)                                                                            \
  X(dbus_message_new_method_call)                                                                                      \
  X(dbus_message_iter_init_append)                                                                                     \
  X(dbus_message_iter_append_basic)                                                                                    \
  X(dbus_connection_send_with_reply_and_block)                                                                         \
  X(dbus_message_get_args)

  static std::mutex s_screensaver_inhibit_dbus_mutex;
  static DynamicLibrary s_dbus_library;
  static bool s_dbus_library_loaded;
  static dbus_uint32_t s_cookie;
  static DBusConnection* s_comparison_connection;

#define DEFINE_FUNC(F) static decltype(&::F) x##F;
  DBUS_FUNCS(DEFINE_FUNC)
#undef DEFINE_FUNC

  const char* bus_method = (inhibit_requested) ? "Inhibit" : "UnInhibit";
  DBusError error;
  DBusConnection* connection = nullptr;
  DBusMessage* message = nullptr;
  DBusMessage* response = nullptr;
  DBusMessageIter message_itr;

  std::unique_lock lock(s_screensaver_inhibit_dbus_mutex);
  if (!s_dbus_library_loaded)
  {
    Error lerror;
    s_dbus_library_loaded = true;

    if (!s_dbus_library.Open(DynamicLibrary::GetVersionedFilename("dbus-1", 3).c_str(), &lerror))
    {
      ERROR_LOG("Failed to open libdbus: {}", lerror.GetDescription());
      return false;
    }

#define LOAD_FUNC(F)                                                                                                   \
  if (!s_dbus_library.GetSymbol(#F, &x##F))                                                                            \
  {                                                                                                                    \
    ERROR_LOG("Failed to find function {}", #F);                                                                       \
    s_dbus_library.Close();                                                                                            \
    return false;                                                                                                      \
  }
    DBUS_FUNCS(LOAD_FUNC)
#undef LOAD_FUNC
  }

  if (!s_dbus_library.IsOpen())
    return false;

  ScopedGuard cleanup = [&]() {
    if (xdbus_error_is_set(&error))
    {
      ERROR_LOG("SetScreensaverInhibitDBus error: {}", error.message);
      xdbus_error_free(&error);
    }
    if (message)
      xdbus_message_unref(message);
    if (response)
      xdbus_message_unref(response);
  };

  xdbus_error_init(&error);

  // Calling dbus_bus_get() after the first time returns a pointer to the existing connection.
  connection = xdbus_bus_get(DBUS_BUS_SESSION, &error);
  if (!connection || (xdbus_error_is_set(&error)))
    return false;

  if (s_comparison_connection != connection)
  {
    xdbus_connection_set_exit_on_disconnect(connection, false);
    s_cookie = 0;
    s_comparison_connection = connection;
  }

  message = xdbus_message_new_method_call("org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver",
                                          "org.freedesktop.ScreenSaver", bus_method);
  if (!message)
    return false;

  // Initialize an append iterator for the message, gets freed with the message.
  xdbus_message_iter_init_append(message, &message_itr);
  if (inhibit_requested)
  {
    // Guard against repeat inhibitions which would add extra inhibitors each generating a different cookie.
    if (s_cookie)
      return false;

    // Append process/window name.
    if (!xdbus_message_iter_append_basic(&message_itr, DBUS_TYPE_STRING, &program_name))
      return false;

    // Append reason for inhibiting the screensaver.
    if (!xdbus_message_iter_append_basic(&message_itr, DBUS_TYPE_STRING, &reason))
      return false;
  }
  else
  {
    // Only Append the cookie.
    if (!xdbus_message_iter_append_basic(&message_itr, DBUS_TYPE_UINT32, &s_cookie))
      return false;
  }

  // Send message and get response.
  response = xdbus_connection_send_with_reply_and_block(connection, message, DBUS_TIMEOUT_USE_DEFAULT, &error);
  if (!response || xdbus_error_is_set(&error))
    return false;

  s_cookie = 0;
  if (inhibit_requested)
  {
    // Get the cookie from the response message.
    if (!xdbus_message_get_args(response, &error, DBUS_TYPE_UINT32, &s_cookie, DBUS_TYPE_INVALID) ||
        xdbus_error_is_set(&error))
    {
      return false;
    }
  }

  return true;

#undef DBUS_FUNCS
}

static bool SetScreensaverInhibit(bool inhibit)
{
  return SetScreensaverInhibitDBus(inhibit, "DuckStation", "DuckStation VM is running.");
}

static bool s_screensaver_suspended;

void PlatformMisc::SuspendScreensaver()
{
  if (s_screensaver_suspended)
    return;

  if (!SetScreensaverInhibit(true))
  {
    ERROR_LOG("Failed to suspend screensaver.");
    return;
  }

  s_screensaver_suspended = true;
}

void PlatformMisc::ResumeScreensaver()
{
  if (!s_screensaver_suspended)
    return;

  if (!SetScreensaverInhibit(false))
    ERROR_LOG("Failed to resume screensaver.");

  s_screensaver_suspended = false;
}

bool PlatformMisc::SetWindowRoundedCornerState(void* window_handle, bool enabled, Error* error /* = nullptr */)
{
  Error::SetStringView(error, "Unsupported on this platform.");
  return false;
}
