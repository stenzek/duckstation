#include "common/file_system.h"
#include "common/log.h"
#include "common/string_util.h"
#include "common/win32_progress_callback.h"
#include "common/windows_headers.h"
#include "updater.h"
#include <shellapi.h>

static void WaitForProcessToExit(int process_id)
{
  HANDLE hProcess = OpenProcess(SYNCHRONIZE, FALSE, process_id);
  if (!hProcess)
    return;

  WaitForSingleObject(hProcess, INFINITE);
  CloseHandle(hProcess);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nShowCmd)
{
  Win32ProgressCallback progress;

  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(lpCmdLine, &argc);
  if (!argv || argc <= 0)
  {
    progress.ModalError("Failed to parse command line.");
    return 1;
  }
  if (argc != 4)
  {
    progress.ModalError("Expected 4 arguments: parent process id, output directory, update zip, program to "
                        "launch.\n\nThis program is not intended to be run manually, please use the Qt frontend and "
                        "click Help->Check for Updates.");
    LocalFree(argv);
    return 1;
  }

  const int parent_process_id = StringUtil::FromChars<int>(StringUtil::WideStringToUTF8String(argv[0])).value_or(0);
  const std::string destination_directory = StringUtil::WideStringToUTF8String(argv[1]);
  const std::string zip_path = StringUtil::WideStringToUTF8String(argv[2]);
  const std::wstring program_to_launch(argv[3]);
  LocalFree(argv);

  if (parent_process_id <= 0 || destination_directory.empty() || zip_path.empty() || program_to_launch.empty())
  {
    progress.ModalError("One or more parameters is empty.");
    return 1;
  }

  progress.SetFormattedStatusText("Waiting for parent process %d to exit...", parent_process_id);
  WaitForProcessToExit(parent_process_id);

  Updater updater(&progress);
  if (!updater.Initialize(destination_directory))
  {
    progress.ModalError("Failed to initialize updater.");
    return 1;
  }

  if (!updater.OpenUpdateZip(zip_path.c_str()))
  {
    progress.DisplayFormattedModalError("Could not open update zip '%s'. Update not installed.", zip_path.c_str());
    return 1;
  }

  if (!updater.PrepareStagingDirectory())
  {
    progress.ModalError("Failed to prepare staging directory. Update not installed.");
    return 1;
  }

  if (!updater.StageUpdate())
  {
    progress.ModalError("Failed to stage update. Update not installed.");
    return 1;
  }

  if (!updater.CommitUpdate())
  {
    progress.ModalError(
      "Failed to commit update. Your installation may be corrupted, please re-download a fresh version from GitHub.");
    return 1;
  }

  updater.CleanupStagingDirectory();

  progress.ModalInformation("Update complete.");

  progress.DisplayFormattedInformation("Launching '%s'...",
                                       StringUtil::WideStringToUTF8String(program_to_launch).c_str());
  ShellExecuteW(nullptr, L"open", program_to_launch.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  return 0;
}
