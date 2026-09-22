// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "cocoa_progress_callback.h"
#include "updater.h"

#include "common/cocoa_tools.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/scoped_guard.h"
#include "common/string_util.h"
#include "common/timer.h"

#include <cerrno>
#include <cstdlib>
#include <sys/event.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>

static bool WaitForProcessToExit(pid_t process_id, Error* error)
{
  const int queue = kqueue();
  if (queue < 0)
  {
    Error::SetErrno(error, "kqueue() failed: ", errno);
    return false;
  }

  const ScopedGuard queue_closer = [queue]() { close(queue); };

  struct kevent change;
  EV_SET(&change, static_cast<uintptr_t>(process_id), EVFILT_PROC, EV_ADD | EV_ENABLE | EV_ONESHOT, NOTE_EXIT, 0,
         nullptr);

  struct kevent event;
  int result;
  do
  {
    result = kevent(queue, &change, 1, &event, 1, nullptr);
  } while (result < 0 && errno == EINTR);

  if (result < 0)
  {
    Error::SetErrno(error, "kevent() failed while waiting for DuckStation to exit: ", errno);
    return false;
  }
  if (result == 0)
  {
    Error::SetString(error, "kevent() returned without reporting that DuckStation exited.");
    return false;
  }
  if (event.flags & EV_ERROR)
  {
    // The process can exit between launching the updater and registering the event. In that case it is safe to
    // continue. Any other registration error is fatal, because modifying the bundle while DuckStation is still
    // running could leave the installation unusable.
    const int event_error = static_cast<int>(event.data);
    if (event_error == ESRCH)
      return true;

    Error::SetErrno(error, "Failed to monitor DuckStation: ", event_error);
    return false;
  }

  return true;
}

int main(int argc, char* argv[])
{
  [NSApplication sharedApplication];
  [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

  // Needed for keyboard in put.
  const ProcessSerialNumber psn = {0, kCurrentProcess};
  TransformProcessType(&psn, kProcessTransformToForegroundApplication);

  CocoaProgressCallback progress;

  if (argc != 5)
  {
    progress.ModalError(
      "Expected 4 arguments: parent process id, update zip, staging directory, output directory.\n\nThis program is "
      "not intended to be run manually, please use the Qt frontend and click Help->Check for Updates.");
    return EXIT_FAILURE;
  }

  const int parent_process_id = StringUtil::FromChars<int>(argv[1]).value_or(0);
  std::string zip_path = argv[2];
  std::string staging_directory = argv[3];
  std::string destination_directory = argv[4];

  if (parent_process_id <= 0 || zip_path.empty() || staging_directory.empty() || destination_directory.empty())
  {
    progress.ModalError("One or more parameters is invalid.");
    return EXIT_FAILURE;
  }

  if (const char* home_dir = getenv("HOME"))
  {
    static constexpr char log_file[] = "Library/Application Support/DuckStation/updater.log";
    std::string log_path = Path::Combine(home_dir, log_file);
    Log::SetFileOutputParams(true, log_path.c_str());
  }

  std::string application_to_launch = destination_directory;
  int result = EXIT_SUCCESS;

  std::thread worker([&progress, parent_process_id, zip_path = std::move(zip_path),
                      destination_directory = std::move(destination_directory),
                      staging_directory = std::move(staging_directory), &result]() {
    ScopedGuard app_stopper([]() {
      dispatch_async(dispatch_get_main_queue(), []() {
        [NSApp stop:nil];

        // NSApp stop doesn't immediately exit the event loop, so we'll get stuck waiting until
        // a key is pressed or the mouse is moved. Manually queue an event to ensure the run
        // loop wakes and exits.
        NSEvent* event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                            location:NSMakePoint(0, 0)
                                       modifierFlags:0
                                           timestamp:0
                                        windowNumber:0
                                             context:nil
                                             subtype:0
                                               data1:0
                                               data2:0];
        [NSApp postEvent:event atStart:YES];
      });
    });

    progress.FormatStatusText("Waiting for DuckStation process {} to exit...", parent_process_id);
    Error wait_error;
    if (!WaitForProcessToExit(static_cast<pid_t>(parent_process_id), &wait_error))
    {
      progress.FormatModalError("Failed to wait for DuckStation to exit: {}", wait_error.GetDescription());
      result = EXIT_FAILURE;
      return;
    }

    Updater updater(&progress);
    if (!updater.Initialize(std::move(staging_directory), std::move(destination_directory)))
    {
      progress.ModalError("Failed to initialize updater.");
      result = EXIT_FAILURE;
      return;
    }

    if (!updater.OpenUpdateZip(zip_path.c_str()))
    {
      progress.FormatModalError("Could not open update zip '{}'. Update not installed.", zip_path);
      result = EXIT_FAILURE;
      return;
    }

    if (!updater.PrepareStagingDirectory())
    {
      progress.ModalError("Failed to prepare staging directory. Update not installed.");
      result = EXIT_FAILURE;
      return;
    }

    if (!updater.StageUpdate())
    {
      progress.ModalError("Failed to stage update. Update not installed.");
      result = EXIT_FAILURE;
      return;
    }

    if (!updater.ClearDestinationDirectory())
    {
      progress.ModalError("Failed to clear destination directory. Your installation may be corrupted, please "
                          "re-download a fresh version from GitHub.");
      result = EXIT_FAILURE;
      return;
    }

    if (!updater.CommitUpdate())
    {
      progress.ModalError(
        "Failed to commit update. Your installation may be corrupted, please re-download a fresh version from GitHub.");
      result = EXIT_FAILURE;
      return;
    }

    updater.CleanupStagingDirectory();
    updater.RemoveUpdateZip();

    result = EXIT_SUCCESS;
  });

  [NSApp run];

  worker.join();

  if (result == EXIT_SUCCESS)
  {
    progress.FormatInformation("Launching '{}'...", application_to_launch);
    Error launch_error;
    if (!CocoaTools::LaunchApplication(application_to_launch, {}, &launch_error))
    {
      progress.FormatModalError("The update was installed successfully, but DuckStation could not be restarted: {}\n\n"
                                "Please launch DuckStation manually.",
                                launch_error.GetDescription());
      result = EXIT_FAILURE;
    }
  }

  return result;
}
