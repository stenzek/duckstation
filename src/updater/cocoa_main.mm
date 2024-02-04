// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "cocoa_progress_callback.h"
#include "updater.h"

#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/scoped_guard.h"
#include "common/string_util.h"
#include "common/timer.h"

#include <cstdlib>
#include <thread>

static void LaunchApplication(const char* path)
{
  @autoreleasepool
  {
    NSTask* task = [[[NSTask alloc] init] autorelease];
    [task setLaunchPath:[NSString stringWithUTF8String:path]];
    [task launch];
  }
}

int main(int argc, char* argv[])
{
  [NSApplication sharedApplication];
  [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

  // Needed for keyboard in put.
  const ProcessSerialNumber psn = {0, kCurrentProcess};
  TransformProcessType(&psn, kProcessTransformToForegroundApplication);

  CocoaProgressCallback progress;

  if (argc != 4)
  {
    progress.ModalError("Expected 3 arguments: update zip, staging directory, output directory.\n\nThis program is not "
                        "intended to be run manually, please use the Qt frontend and click Help->Check for Updates.");
    return EXIT_FAILURE;
  }

  std::string zip_path = argv[1];
  std::string staging_directory = argv[2];
  std::string destination_directory = argv[3];

  if (zip_path.empty() || staging_directory.empty() || destination_directory.empty())
  {
    progress.ModalError("One or more parameters is empty.");
    return EXIT_FAILURE;
  }

  if (const char* home_dir = getenv("HOME"))
  {
    static constexpr char log_file[] = "Library/Application Support/DuckStation/updater.log";
    std::string log_path = Path::Combine(home_dir, log_file);
    Log::SetFileOutputParams(true, log_path.c_str());
  }

  std::string program_to_launch = Path::Combine(destination_directory, "Contents/MacOS/DuckStation");
  int result = EXIT_SUCCESS;

  std::thread worker([&progress, zip_path = std::move(zip_path),
                      destination_directory = std::move(destination_directory),
                      staging_directory = std::move(staging_directory), &result]() {
    ScopedGuard app_stopper([]() { dispatch_async(dispatch_get_main_queue(), []() { [NSApp stop:nil]; }); });

    Updater updater(&progress);
    if (!updater.Initialize(std::move(staging_directory), std::move(destination_directory)))
    {
      progress.ModalError("Failed to initialize updater.");
      result = EXIT_FAILURE;
      return;
    }

    if (!updater.OpenUpdateZip(zip_path.c_str()))
    {
      progress.DisplayFormattedModalError("Could not open update zip '%s'. Update not installed.", zip_path.c_str());
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
    progress.DisplayFormattedInformation("Launching '%s'...", program_to_launch.c_str());
    LaunchApplication(program_to_launch.c_str());
  }

  return result;
}
