// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "input_manager.h"
#include "platform_misc.h"

#include "common/log.h"
#include "common/scoped_guard.h"
#include "common/small_string.h"

#include <cinttypes>
#include <spawn.h>
#include <unistd.h>
#include <dbus/dbus.h>

Log_SetChannel(PlatformMisc);

static bool SetScreensaverInhibitDBus(const bool inhibit_requested, const char* program_name, const char* reason)
{
  static dbus_uint32_t s_cookie;
  const char* bus_method = (inhibit_requested) ? "Inhibit" : "UnInhibit";
  DBusError error;
  DBusConnection* connection = nullptr;
  static DBusConnection* s_comparison_connection;
  DBusMessage* message = nullptr;
  DBusMessage* response = nullptr;
  DBusMessageIter message_itr;

  ScopedGuard cleanup = [&]() {
    if (dbus_error_is_set(&error))
    {
      Log_ErrorPrintf("SetScreensaverInhibitDBus error: %s", error.message);
      dbus_error_free(&error);
    }
    if (message)
      dbus_message_unref(message);
    if (response)
      dbus_message_unref(response);
  };

  dbus_error_init(&error);
  // Calling dbus_bus_get() after the first time returns a pointer to the existing connection.
  connection = dbus_bus_get(DBUS_BUS_SESSION, &error);
  if (!connection || (dbus_error_is_set(&error)))
    return false;
  if (s_comparison_connection != connection)
  {
    dbus_connection_set_exit_on_disconnect(connection, false);
    s_cookie = 0;
    s_comparison_connection = connection;
  }
  message = dbus_message_new_method_call("org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver",
                                         "org.freedesktop.ScreenSaver", bus_method);
  if (!message)
    return false;
  // Initialize an append iterator for the message, gets freed with the message.
  dbus_message_iter_init_append(message, &message_itr);
  if (inhibit_requested)
  {
    // Guard against repeat inhibitions which would add extra inhibitors each generating a different cookie.
    if (s_cookie)
      return false;
    // Append process/window name.
    if (!dbus_message_iter_append_basic(&message_itr, DBUS_TYPE_STRING, &program_name))
      return false;
    // Append reason for inhibiting the screensaver.
    if (!dbus_message_iter_append_basic(&message_itr, DBUS_TYPE_STRING, &reason))
      return false;
  }
  else
  {
    // Only Append the cookie.
    if (!dbus_message_iter_append_basic(&message_itr, DBUS_TYPE_UINT32, &s_cookie))
      return false;
  }
  // Send message and get response.
  response = dbus_connection_send_with_reply_and_block(connection, message, DBUS_TIMEOUT_USE_DEFAULT, &error);
  if (!response || dbus_error_is_set(&error))
    return false;
  s_cookie = 0;
  if (inhibit_requested)
  {
    // Get the cookie from the response message.
    if (!dbus_message_get_args(response, &error, DBUS_TYPE_UINT32, &s_cookie, DBUS_TYPE_INVALID) ||
        dbus_error_is_set(&error))
      return false;
  }
  return true;
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
    Log_ErrorPrintf("Failed to suspend screensaver.");
    return;
  }

  s_screensaver_suspended = true;
}

void PlatformMisc::ResumeScreensaver()
{
  if (!s_screensaver_suspended)
    return;

  if (!SetScreensaverInhibit(false))
    Log_ErrorPrint("Failed to resume screensaver.");

  s_screensaver_suspended = false;
}

bool PlatformMisc::PlaySoundAsync(const char* path)
{
#ifdef __linux__
  // This is... pretty awful. But I can't think of a better way without linking to e.g. gstreamer.
  const char* cmdname = "aplay";
  const char* argv[] = {cmdname, path, nullptr};
  pid_t pid;

  // Since we set SA_NOCLDWAIT in Qt, we don't need to wait here.
  int res = posix_spawnp(&pid, cmdname, nullptr, nullptr, const_cast<char**>(argv), environ);
  return (res == 0);
#else
  return false;
#endif
}
