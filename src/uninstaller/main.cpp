// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "common/error.h"
#include "common/file_system.h"
#include "common/path.h"
#include "common/scoped_guard.h"
#include "common/string_util.h"
#include "common/windows_headers.h"

#include "installer/installer_params.h"

#include <fmt/format.h>

#include <KnownFolders.h>
#include <ShlObj.h>
#include <Shobjidl.h>
#include <combaseapi.h>
#include <shellapi.h>
#include <wrl/client.h>

static constexpr const wchar_t* WINDOW_TITLE = L"DuckStation Uninstaller";

template<typename... T>
static int FormatMessageBox(HWND hwnd, UINT type, fmt::format_string<T...> fmt, T&&... args)
{
  const std::string message = fmt::format(fmt, std::forward<T>(args)...);
  return MessageBoxW(hwnd, StringUtil::UTF8StringToWideString(message).c_str(), WINDOW_TITLE, type);
}

static std::string GetTempDirectory()
{
  wchar_t temp_path[MAX_PATH];
  if (GetTempPathW(MAX_PATH, temp_path) == 0)
    return {};

  return StringUtil::WideStringToUTF8String(temp_path);
}

static bool RecursiveDeleteDirectory(const std::string& path)
{
  if (!FileSystem::DirectoryExists(path.c_str()))
    return true;

  Microsoft::WRL::ComPtr<IFileOperation> fo;
  HRESULT hr = CoCreateInstance(CLSID_FileOperation, NULL, CLSCTX_ALL, IID_PPV_ARGS(fo.ReleaseAndGetAddressOf()));
  if (FAILED(hr))
  {
    FormatMessageBox(nullptr, MB_ICONERROR | MB_OK, "CoCreateInstance() for IFileOperation failed:\n{}",
                     Error::CreateHResult(hr).GetDescription());
    return false;
  }

  Microsoft::WRL::ComPtr<IShellItem> item;
  hr = SHCreateItemFromParsingName(StringUtil::UTF8StringToWideString(path).c_str(), NULL,
                                   IID_PPV_ARGS(item.ReleaseAndGetAddressOf()));
  if (FAILED(hr))
  {
    FormatMessageBox(nullptr, MB_ICONERROR | MB_OK, "SHCreateItemFromParsingName() failed:\n{}",
                     Error::CreateHResult(hr).GetDescription());
    return false;
  }

  hr = fo->SetOperationFlags(FOF_NOCONFIRMATION | FOF_SILENT);
  if (FAILED(hr))
  {
    FormatMessageBox(nullptr, MB_ICONWARNING | MB_OK, "IFileOperation::SetOperationFlags() failed: {}",
                     Error::CreateHResult(hr).GetDescription());
  }

  hr = fo->DeleteItem(item.Get(), nullptr);
  if (FAILED(hr))
  {
    FormatMessageBox(nullptr, MB_ICONERROR | MB_OK, "IFileOperation::DeleteItem() failed:\n{}",
                     Error::CreateHResult(hr).GetDescription());
    return false;
  }

  item.Reset();
  hr = fo->PerformOperations();
  if (FAILED(hr))
  {
    FormatMessageBox(nullptr, MB_ICONERROR | MB_OK, "IFileOperation::PerformOperations() failed:\n{}",
                     Error::CreateHResult(hr).GetDescription());
    return false;
  }

  return true;
}

static bool RemoveDesktopShortcut()
{
  PWSTR desktop_folder = nullptr;
  HRESULT hr = SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop_folder);
  if (FAILED(hr))
  {
    FormatMessageBox(nullptr, MB_ICONERROR | MB_OK, "SHGetKnownFolderPath(FOLDERID_Desktop) failed: {}",
                     Error::CreateHResult(hr).GetDescription());
    return false;
  }

  const std::string shortcut_path =
    Path::Combine(StringUtil::WideStringToUTF8String(desktop_folder), INSTALLER_SHORTCUT_FILENAME);
  CoTaskMemFree(desktop_folder);

  if (!FileSystem::FileExists(shortcut_path.c_str()))
    return true;

  Error error;
  if (!FileSystem::DeleteFile(shortcut_path.c_str(), &error))
  {
    FormatMessageBox(nullptr, MB_ICONERROR | MB_OK, "Failed to delete desktop shortcut: {}", error.GetDescription());
    return false;
  }

  return true;
}

static bool RemoveStartMenuShortcut()
{
  PWSTR programs_folder = nullptr;
  HRESULT hr = SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs_folder);
  if (FAILED(hr))
  {
    FormatMessageBox(nullptr, MB_ICONERROR | MB_OK, "SHGetKnownFolderPath(FOLDERID_Programs) failed: {}",
                     Error::CreateHResult(hr).GetDescription());
    return false;
  }

  const std::string shortcut_path =
    Path::Combine(StringUtil::WideStringToUTF8String(programs_folder), INSTALLER_SHORTCUT_FILENAME);
  CoTaskMemFree(programs_folder);

  if (!FileSystem::FileExists(shortcut_path.c_str()))
    return true;

  Error error;
  if (!FileSystem::DeleteFile(shortcut_path.c_str(), &error))
  {
    FormatMessageBox(nullptr, MB_ICONERROR | MB_OK, "Failed to delete Start Menu shortcut: ", error.GetDescription());
    return false;
  }

  return true;
}

static bool RemoveUninstallerEntry()
{
  const LSTATUS status = RegDeleteTreeW(HKEY_CURRENT_USER, INSTALLER_UNINSTALL_REG_KEY);
  if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
  {
    FormatMessageBox(nullptr, MB_ICONERROR | MB_OK, "Failed to delete uninstaller registry key: {}",
                     Error::CreateWin32(status).GetDescription());
    return false;
  }

  return true;
}

static std::string GetUserDataDirectory()
{
  // Check LocalAppData\DuckStation (new location)
  PWSTR appdata_directory = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appdata_directory)))
  {
    if (std::wcslen(appdata_directory) > 0)
    {
      std::string path = Path::Combine(StringUtil::WideStringToUTF8String(appdata_directory), "DuckStation");
      CoTaskMemFree(appdata_directory);
      if (FileSystem::DirectoryExists(path.c_str()))
        return path;
    }
    else
    {
      CoTaskMemFree(appdata_directory);
    }
  }

  return {};
}

static bool PerformUninstall(const std::string& install_directory)
{
  RemoveDesktopShortcut();
  RemoveStartMenuShortcut();
  RemoveUninstallerEntry();

  // Remove the installation directory
  if (!RecursiveDeleteDirectory(install_directory))
    return false;

  // Check if user wants to remove user data
  const std::string user_data_dir = GetUserDataDirectory();
  if (!user_data_dir.empty() && FileSystem::DirectoryExists(user_data_dir.c_str()))
  {
    if (FormatMessageBox(nullptr, MB_ICONWARNING | MB_YESNO,
                         "Do you want to remove user data (save games, memory cards, settings)?\n\n"
                         "Location: {}\n\n"
                         "WARNING: This data cannot be recovered once deleted!",
                         user_data_dir) == IDYES)
    {
      RecursiveDeleteDirectory(user_data_dir);
    }
  }

  MessageBoxW(nullptr, L"DuckStation has been uninstalled.", WINDOW_TITLE, MB_ICONINFORMATION | MB_OK);
  return true;
}

static bool CopyToTempAndRelaunch(const std::string& current_exe_path, const std::string_view& install_directory)
{
  const std::string temp_dir = GetTempDirectory();
  if (temp_dir.empty())
  {
    MessageBoxW(nullptr, L"Failed to get temporary directory.", WINDOW_TITLE, MB_ICONERROR | MB_OK);
    return false;
  }

  const std::string temp_exe_path = Path::Combine(temp_dir, "duckstation_uninstall.exe");

  Error error;
  if (!FileSystem::CopyFilePath(current_exe_path.c_str(), temp_exe_path.c_str(), true, &error))
  {
    FormatMessageBox(nullptr, MB_ICONERROR | MB_OK, "Failed to copy uninstaller to temporary location:\n{}",
                     error.GetDescription());
    return false;
  }

  // Quote the install directory in case it contains spaces
  const std::wstring args_w = StringUtil::UTF8StringToWideString(fmt::format("\"{}\"", install_directory));
  const std::wstring temp_exe_path_w = StringUtil::UTF8StringToWideString(temp_exe_path);

  SHELLEXECUTEINFOW sei = {};
  sei.cbSize = sizeof(sei);
  sei.fMask = SEE_MASK_DEFAULT;
  sei.lpFile = temp_exe_path_w.c_str();
  sei.lpParameters = args_w.c_str();
  sei.nShow = SW_SHOWNORMAL;

  if (!ShellExecuteExW(&sei))
  {
    FormatMessageBox(nullptr, MB_ICONERROR | MB_OK, "Failed to launch uninstaller from temporary location:\n{}",
                     Error::CreateWin32(GetLastError()).GetDescription());
    FileSystem::DeleteFile(temp_exe_path.c_str());
    return false;
  }

  return true;
}

static int RunFromInstallDirectory()
{
  Error error;
  const std::string current_exe_path = FileSystem::GetProgramPath(&error);
  if (current_exe_path.empty())
  {
    FormatMessageBox(nullptr, MB_ICONERROR | MB_OK, "Failed to get current executable path:\n{}",
                     error.GetDescription());
    return EXIT_FAILURE;
  }

  const std::string_view install_directory = Path::GetDirectory(current_exe_path);

  // Verify this is a DuckStation installation by checking for the main executable
  const std::string main_exe_path = Path::Combine(install_directory, INSTALLER_PROGRAM_FILENAME);
  if (!FileSystem::FileExists(main_exe_path.c_str()))
  {
    MessageBoxW(nullptr,
                L"This does not appear to be a valid DuckStation installation.\n\n"
                L"The main executable was not found in the same directory as the uninstaller.",
                WINDOW_TITLE, MB_ICONERROR | MB_OK);
    return EXIT_FAILURE;
  }

  // Confirm uninstallation with user
  if (FormatMessageBox(nullptr, MB_ICONQUESTION | MB_YESNO,
                       "This will uninstall DuckStation from:\n\n{}\n\n"
                       "Do you want to continue?",
                       install_directory) != IDYES)
  {
    return EXIT_SUCCESS;
  }

  // Copy to temp and relaunch
  if (!CopyToTempAndRelaunch(current_exe_path, install_directory))
    return EXIT_FAILURE;

  return EXIT_SUCCESS;
}

static int RunFromTemp(const std::string& install_directory)
{
  const bool success = PerformUninstall(install_directory);

#if 0
  // Schedule ourselves for deletion on reboot
  const std::string current_exe_path = FileSystem::GetProgramPath(nullptr);
  if (!current_exe_path.empty())
  {
    const std::wstring current_exe_path_w = FileSystem::GetWin32Path(current_exe_path);
    if (!current_exe_path_w.empty())
    {
      // TODO: This requires admin rights. Find a better way.
      if (!MoveFileExW(current_exe_path_w.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT))
      {
        FormatMessageBox(nullptr, MB_ICONERROR | MB_OK, "Failed to schedule temp uninstaller for deletion: {}",
                         Error::CreateWin32(GetLastError()).GetDescription());
      }
    }
  }
#endif

  return success ? EXIT_SUCCESS : EXIT_FAILURE;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nShowCmd)
{
  // IFileOperation requires single-threaded apartment mode
  const bool com_initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
  const ScopedGuard com_guard = [com_initialized]() {
    if (com_initialized)
      CoUninitialize();
  };

  // Parse command line - we only accept zero or one argument
  int argc = 0;
  LPWSTR* argv = nullptr;
  if (lpCmdLine && std::wcslen(lpCmdLine) > 0 && !(argv = CommandLineToArgvW(lpCmdLine, &argc)))
  {
    MessageBoxW(nullptr, L"Failed to parse command line.", WINDOW_TITLE, MB_ICONERROR | MB_OK);
    return EXIT_FAILURE;
  }

  const ScopedGuard argv_guard = [argv]() {
    if (argv)
      LocalFree(argv);
  };

  // If no arguments, we're running from the install directory - copy to temp and relaunch
  if (argc < 1 || (argc == 1 && std::wcslen(argv[0]) == 0))
    return RunFromInstallDirectory();

  // App uninstaller sets argv[0] to the program name.
  if (argc == 1)
  {
    const std::string install_directory = StringUtil::WideStringToUTF8String(argv[0]);
    if (FileSystem::DirectoryExists(install_directory.c_str()))
      return RunFromTemp(install_directory);
    else
      return RunFromInstallDirectory();
  }

  // Too many arguments
  MessageBoxW(nullptr, L"Invalid command line arguments.\n\nUsage: uninstaller.exe [install_directory]", WINDOW_TITLE,
              MB_ICONERROR | MB_OK);
  return EXIT_FAILURE;
}
