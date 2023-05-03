// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "common/log.h"
#include "common/string.h"
#include "input_manager.h"
#include "platform_misc.h"
#include <cinttypes>
Log_SetChannel(FrontendCommon);

#include <spawn.h>
#include <unistd.h>

#if !defined(USE_DBUS) && defined(USE_X11)
#include <cstdio>
#include <sys/wait.h>

static bool SetScreensaverInhibitX11(bool inhibit, const WindowInfo& wi)
{
  TinyString command;
  command.AppendString("xdg-screensaver");

  TinyString operation;
  operation.AppendString(inhibit ? "suspend" : "resume");

  TinyString id;
  id.Format("0x%" PRIx64, static_cast<u64>(reinterpret_cast<uintptr_t>(wi.window_handle)));

  char* argv[4] = {command.GetWriteableCharArray(), operation.GetWriteableCharArray(), id.GetWriteableCharArray(),
                   nullptr};
  pid_t pid;
  int res = posix_spawnp(&pid, "xdg-screensaver", nullptr, nullptr, argv, environ);
  if (res != 0)
  {
    Log_ErrorPrintf("posix_spawnp() failed: %d", res);
    return false;
  }

  return true;
}

#elif defined(USE_DBUS)
#include <dbus/dbus.h>
bool ChangeScreenSaverStateDBus(const bool inhibit_requested, const char* program_name, const char* reason)
{
  static dbus_uint32_t s_cookie;
  // "error_dbus" doesn't need to be cleared in the end with "dbus_message_unref" at least if there is
  // no error set, since calling "dbus_error_free" reinitializes it like "dbus_error_init" after freeing.
  DBusError error_dbus;
  dbus_error_init(&error_dbus);
  DBusConnection* connection = nullptr;
  DBusMessage* message = nullptr;
  DBusMessage* response = nullptr;
  // Initialized here because initializations should be before "goto" statements.
  const char* bus_method = (inhibit_requested) ? "Inhibit" : "UnInhibit";
  // "dbus_bus_get" gets a pointer to the same connection in libdbus, if exists, without creating a new connection.
  // this doesn't need to be deleted, except if there's an error then calling "dbus_connection_unref", to free it,
  // might be better so a new connection is established on the next try.
  if (!(connection = dbus_bus_get(DBUS_BUS_SESSION, &error_dbus)) || (dbus_error_is_set(&error_dbus)))
    goto cleanup;
  if (!(message = dbus_message_new_method_call("org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver",
                                               "org.freedesktop.ScreenSaver", bus_method)))
    goto cleanup;
  // Initialize an append iterator for the message, gets freed with the message.
  DBusMessageIter message_itr;
  dbus_message_iter_init_append(message, &message_itr);
  if (inhibit_requested)
  {
    // Append process/window name.
    if (!dbus_message_iter_append_basic(&message_itr, DBUS_TYPE_STRING, &program_name))
      goto cleanup;
    // Append reason for inhibiting the screensaver.
    if (!dbus_message_iter_append_basic(&message_itr, DBUS_TYPE_STRING, &reason))
      goto cleanup;
  }
  else
  {
    // Only Append the cookie.
    if (!dbus_message_iter_append_basic(&message_itr, DBUS_TYPE_UINT32, &s_cookie))
      goto cleanup;
  }
  // Send message and get response.
  if (!(response =
          dbus_connection_send_with_reply_and_block(connection, message, DBUS_TIMEOUT_USE_DEFAULT, &error_dbus)) ||
      dbus_error_is_set(&error_dbus))
    goto cleanup;
  if (inhibit_requested)
  {
    // Get the cookie from the response message.
    if (!dbus_message_get_args(response, &error_dbus, DBUS_TYPE_UINT32, &s_cookie, DBUS_TYPE_INVALID))
      goto cleanup;
  }
  dbus_message_unref(message);
  dbus_message_unref(response);
  return true;
cleanup:
  if (dbus_error_is_set(&error_dbus))
    dbus_error_free(&error_dbus);
  if (connection)
    dbus_connection_unref(connection);
  if (message)
    dbus_message_unref(message);
  if (response)
    dbus_message_unref(response);
  return false;
}

#endif

static bool SetScreensaverInhibit(bool inhibit)
{
#ifdef USE_DBUS
  return ChangeScreenSaverStateDBus(inhibit, "DuckStation", "DuckStation VM is running.");
#else

  std::optional<WindowInfo> wi(Host::GetTopLevelWindowInfo());
  if (!wi.has_value())
  {
    Log_ErrorPrintf("No top-level window.");
    return false;
  }

  switch (wi->type)
  {
#ifdef USE_X11
    case WindowInfo::Type::X11:
      return SetScreensaverInhibitX11(inhibit, wi.value());
#endif

    default:
      Log_ErrorPrintf("Unknown type: %u", static_cast<unsigned>(wi->type));
      return false;
  }
#endif
}

static bool s_screensaver_suspended;

void FrontendCommon::SuspendScreensaver()
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

void FrontendCommon::ResumeScreensaver()
{
  if (!s_screensaver_suspended)
    return;

  if (!SetScreensaverInhibit(false))
    Log_ErrorPrint("Failed to resume screensaver.");

  s_screensaver_suspended = false;
}

bool FrontendCommon::PlaySoundAsync(const char* path)
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
