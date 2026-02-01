// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "installer.h"
#include "installer_params.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/progress_callback.h"
#include "common/scoped_guard.h"
#include "common/string_util.h"

#include "common/windows_headers.h"

#include "7zAlloc.h"
#include "7zCrc.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <KnownFolders.h>
#include <ShlObj.h>
#include <Shobjidl.h>
#include <shellapi.h>
#include <wrl/client.h>

static constexpr size_t kInputBufSize = static_cast<size_t>(1) << 18;
static constexpr ISzAlloc g_Alloc = {SzAlloc, SzFree};

// TODO: Get this outta here
static const char* SZErrorToString(int res)
{
  // clang-format off
  switch (res)
  {
  case SZ_OK: return "SZ_OK";
  case SZ_ERROR_DATA: return "SZ_ERROR_DATA";
  case SZ_ERROR_MEM: return "SZ_ERROR_MEM";
  case SZ_ERROR_CRC: return "SZ_ERROR_CRC";
  case SZ_ERROR_UNSUPPORTED: return "SZ_ERROR_UNSUPPORTED";
  case SZ_ERROR_PARAM: return "SZ_ERROR_PARAM";
  case SZ_ERROR_INPUT_EOF: return "SZ_ERROR_INPUT_EOF";
  case SZ_ERROR_OUTPUT_EOF: return "SZ_ERROR_OUTPUT_EOF";
  case SZ_ERROR_READ: return "SZ_ERROR_READ";
  case SZ_ERROR_WRITE: return "SZ_ERROR_WRITE";
  case SZ_ERROR_PROGRESS: return "SZ_ERROR_PROGRESS";
  case SZ_ERROR_FAIL: return "SZ_ERROR_FAIL";
  case SZ_ERROR_THREAD: return "SZ_ERROR_THREAD";
  case SZ_ERROR_ARCHIVE: return "SZ_ERROR_ARCHIVE";
  case SZ_ERROR_NO_ARCHIVE: return "SZ_ERROR_NO_ARCHIVE";
  default: return "SZ_UNKNOWN";
  }
  // clang-format on
}

// Based on 7-zip SfxSetup.c FindSignature() by Igor Pavlov (public domain)
static bool FindSignature(CSzFile* stream, s64* start_offset)
{
  // How much we're reading at once.
  static constexpr size_t READ_BUFFER_SIZE = 1 << 15;

  // How much of the .exe file to search through for the 7z signature.
  // This should be more than enough for any reasonable SFX stub.
  static constexpr size_t SIGNATURE_SEARCH_LIMIT = (1 << 22);

  u8 buf[READ_BUFFER_SIZE];
  size_t num_prev_bytes = 0;
  *start_offset = 0;
  for (;;)
  {
    if (*start_offset > static_cast<s64>(SIGNATURE_SEARCH_LIMIT))
      return false;

    size_t processed = READ_BUFFER_SIZE - num_prev_bytes;
    if (File_Read(stream, buf + num_prev_bytes, &processed) != 0)
      return false;

    processed += num_prev_bytes;
    if (processed < k7zStartHeaderSize || (processed == k7zStartHeaderSize && num_prev_bytes != 0))
      return false;

    processed -= k7zStartHeaderSize;

    for (size_t pos = 0; pos <= processed; pos++)
    {
      for (; pos <= processed && buf[pos] != '7'; pos++)
        ;
      if (pos > processed)
        break;

      if (std::memcmp(buf + pos, k7zSignature, k7zSignatureSize) == 0)
      {
        u32 file_value;
        std::memcpy(&file_value, buf + pos + 8, sizeof(file_value));
        if (CrcCalc(buf + pos + 12, 20) == file_value)
        {
          *start_offset += pos;
          return true;
        }
      }
    }

    *start_offset += processed;
    num_prev_bytes = k7zStartHeaderSize;
    std::memmove(buf, buf + processed, k7zStartHeaderSize);
  }
}

Installer::Installer(Win32ProgressCallback* progress, std::string destination_directory)
  : m_destination_directory(std::move(destination_directory)), m_progress(progress)
{
  m_staging_directory = Path::Combine(m_destination_directory, "staging");

  progress->SetTitle("DuckStation Installer");
  progress->SetStatusText("Preparing installation...");
}

Installer::~Installer()
{
  CloseArchiveStream();
}

bool Installer::RecursiveDeleteDirectory(const char* path, bool remove_dir)
{
  if (!remove_dir)
    return false;

  Microsoft::WRL::ComPtr<IFileOperation> fo;
  HRESULT hr = CoCreateInstance(CLSID_FileOperation, NULL, CLSCTX_ALL, IID_PPV_ARGS(fo.ReleaseAndGetAddressOf()));
  if (FAILED(hr))
  {
    m_progress->FormatError("CoCreateInstance() for IFileOperation failed: {}",
                            Error::CreateHResult(hr).GetDescription());
    return false;
  }

  Microsoft::WRL::ComPtr<IShellItem> item;
  hr = SHCreateItemFromParsingName(StringUtil::UTF8StringToWideString(path).c_str(), NULL,
                                   IID_PPV_ARGS(item.ReleaseAndGetAddressOf()));
  if (FAILED(hr))
  {
    m_progress->FormatError("SHCreateItemFromParsingName() for delete failed: {}",
                            Error::CreateHResult(hr).GetDescription());
    return false;
  }

  hr = fo->SetOperationFlags(FOF_NOCONFIRMATION | FOF_SILENT);
  if (FAILED(hr))
  {
    m_progress->FormatWarning("IFileOperation::SetOperationFlags() failed: {}",
                              Error::CreateHResult(hr).GetDescription());
  }

  hr = fo->DeleteItem(item.Get(), nullptr);
  if (FAILED(hr))
  {
    m_progress->FormatError("IFileOperation::DeleteItem() failed: {}", Error::CreateHResult(hr).GetDescription());
    return false;
  }

  item.Reset();
  hr = fo->PerformOperations();
  if (FAILED(hr))
  {
    m_progress->FormatError("IFileOperation::PerformOperations() failed: {}",
                            Error::CreateHResult(hr).GetDescription());
    return false;
  }

  return true;
}

bool Installer::OpenArchiveStream()
{
  FileInStream_CreateVTable(&m_archive_stream);
  LookToRead2_CreateVTable(&m_look_stream, False);
  CrcGenerateTable();

  m_look_stream.buf = (Byte*)ISzAlloc_Alloc(&g_Alloc, kInputBufSize);
  if (!m_look_stream.buf)
  {
    m_progress->DisplayError("Failed to allocate input buffer.");
    return false;
  }

  Error error;
  std::string program_path = FileSystem::GetProgramPath(&error);
  if (program_path.empty())
  {
    m_progress->FormatModalError("Failed to get program path: {}", error.GetDescription());
    return false;
  }

  m_progress->FormatInformation("Program/archive path: {}", program_path);

  WRes wres = InFile_OpenW(&m_archive_stream.file, FileSystem::GetWin32Path(program_path).c_str());
  if (wres != 0)
  {
    m_progress->FormatModalError("Failed to open '{}': {}", program_path, static_cast<int>(wres));
    return false;
  }

  m_archive_stream_opened = true;

  s64 archive_start_pos = 0;
  if (!FindSignature(&m_archive_stream.file, &archive_start_pos))
  {
    m_progress->ModalError("Failed to find 7z archive signature in installer. Please try re-downloading from "
                           "duckstation.org, and if you are still having difficulties, chat to us on Discord.");
    return false;
  }

  m_progress->FormatInformation("Found 7z archive in installer at offset {}", archive_start_pos);

  // seek to archive start
  wres = File_Seek(&m_archive_stream.file, &archive_start_pos, SZ_SEEK_SET);
  if (wres != 0)
  {
    m_progress->FormatModalError("Failed to seek to archive start (error {} [{}]).", SZErrorToString(wres),
                                 static_cast<int>(wres));
    return false;
  }

  m_look_stream.bufSize = kInputBufSize;
  m_look_stream.realStream = &m_archive_stream.vt;
  LookToRead2_INIT(&m_look_stream);
  SzArEx_Init(&m_archive);

  SRes res = SzArEx_Open(&m_archive, &m_look_stream.vt, &g_Alloc, &g_Alloc);
  if (res != SZ_OK)
  {
    m_progress->FormatModalError("SzArEx_Open() failed: {} [{}]", SZErrorToString(res), static_cast<int>(res));
    return false;
  }

  m_archive_opened = true;
  return ParseArchive();
}

void Installer::CloseArchiveStream()
{
  if (m_archive_opened)
  {
    SzArEx_Free(&m_archive, &g_Alloc);
    m_archive_opened = false;
  }

  if (m_look_stream.buf)
  {
    ISzAlloc_Free(&g_Alloc, m_look_stream.buf);
    m_look_stream.buf = nullptr;
  }

  if (m_archive_stream_opened)
  {
    File_Close(&m_archive_stream.file);
    m_archive_stream_opened = false;
  }
}

bool Installer::ParseArchive()
{
  std::vector<UInt16> filename_buffer;

  for (u32 file_index = 0; file_index < m_archive.NumFiles; file_index++)
  {
    // skip directories, we handle them ourselves
    if (SzArEx_IsDir(&m_archive, file_index))
      continue;

    size_t filename_len = SzArEx_GetFileNameUtf16(&m_archive, file_index, nullptr);
    if (filename_len <= 1)
      continue;

    filename_buffer.resize(filename_len);
    SzArEx_GetFileNameUtf16(&m_archive, file_index, filename_buffer.data());

    // TODO: This won't work on Linux (4-byte wchar_t).
    FileToUpdate entry;
    entry.file_index = file_index;
    entry.destination_filename = StringUtil::WideStringToUTF8String(reinterpret_cast<wchar_t*>(filename_buffer.data()));
    if (entry.destination_filename.empty())
      continue;

    // replace forward slashes with backslashes
    for (size_t i = 0; i < entry.destination_filename.length(); i++)
    {
      if (entry.destination_filename[i] == '/' || entry.destination_filename[i] == '\\')
        entry.destination_filename[i] = FS_OSPATH_SEPARATOR_CHARACTER;
    }

    // should never have a leading slash. just in case.
    while (entry.destination_filename[0] == FS_OSPATH_SEPARATOR_CHARACTER)
      entry.destination_filename.erase(0, 1);

    // skip directories (we sort them out later)
    if (!entry.destination_filename.empty() && entry.destination_filename.back() != FS_OSPATH_SEPARATOR_CHARACTER)
      m_update_paths.push_back(std::move(entry));
  }

  if (m_update_paths.empty())
  {
    m_progress->ModalError("No files found in update zip.");
    return false;
  }

  for (const FileToUpdate& ftu : m_update_paths)
  {
    const size_t len = ftu.destination_filename.length();
    for (size_t i = 0; i < len; i++)
    {
      if (ftu.destination_filename[i] == FS_OSPATH_SEPARATOR_CHARACTER)
      {
        std::string dir(ftu.destination_filename.begin(), ftu.destination_filename.begin() + i);
        while (!dir.empty() && dir[dir.length() - 1] == FS_OSPATH_SEPARATOR_CHARACTER)
          dir.erase(dir.length() - 1);

        if (std::find(m_update_directories.begin(), m_update_directories.end(), dir) == m_update_directories.end())
          m_update_directories.push_back(std::move(dir));
      }
    }
  }

  std::sort(m_update_directories.begin(), m_update_directories.end());
  for (const std::string& dir : m_update_directories)
    m_progress->FormatDebugMessage("Directory: {}", dir);

  return true;
}

bool Installer::PrepareStagingDirectory()
{
  if (FileSystem::DirectoryExists(m_staging_directory.c_str()))
  {
    m_progress->DisplayWarning("Update staging directory already exists, removing");
    if (!RecursiveDeleteDirectory(m_staging_directory.c_str(), true) ||
        FileSystem::DirectoryExists(m_staging_directory.c_str()))
    {
      m_progress->ModalError("Failed to remove old staging directory");
      return false;
    }
  }
  if (!FileSystem::CreateDirectory(m_staging_directory.c_str(), false))
  {
    m_progress->FormatModalError("Failed to create staging directory {}", m_staging_directory);
    return false;
  }

  // create subdirectories in staging directory
  for (const std::string& subdir : m_update_directories)
  {
    m_progress->FormatInformation("Creating subdirectory in staging: {}", subdir);

    const std::string staging_subdir = Path::Combine(m_staging_directory, subdir);
    if (!FileSystem::CreateDirectory(staging_subdir.c_str(), false))
    {
      m_progress->FormatModalError("Failed to create staging subdirectory {}", staging_subdir);
      return false;
    }
  }

  return true;
}

bool Installer::StageUpdate()
{
  m_progress->SetProgressRange(static_cast<u32>(m_update_paths.size()));
  m_progress->SetProgressValue(0);

  UInt32 block_index = 0xFFFFFFFF; /* it can have any value before first call (if outBuffer = 0) */
  Byte* out_buffer = 0;            /* it must be 0 before first call for each new archive. */
  size_t out_buffer_size = 0;      /* it can have any value before first call (if outBuffer = 0) */
  const ScopedGuard out_buffer_guard([&out_buffer]() {
    if (out_buffer)
      ISzAlloc_Free(&g_Alloc, out_buffer);
  });

  Error error;

  for (const FileToUpdate& ftu : m_update_paths)
  {
    m_progress->FormatStatusText("Extracting '{}'...", ftu.destination_filename);
    m_progress->FormatInformation("Extracting '{}'...", ftu.destination_filename);

    size_t out_offset = 0;
    size_t extracted_size = 0;
    SRes res = SzArEx_Extract(&m_archive, &m_look_stream.vt, ftu.file_index, &block_index, &out_buffer,
                              &out_buffer_size, &out_offset, &extracted_size, &g_Alloc, &g_Alloc);
    if (res != SZ_OK)
    {
      m_progress->FormatModalError("Failed to decompress file '{}' from 7z (file index={}, error={})",
                                   ftu.destination_filename, ftu.file_index, SZErrorToString(res));
      return false;
    }

    const std::string destination_file = Path::Combine(m_staging_directory, ftu.destination_filename);
    if (!FileSystem::WriteBinaryFile(destination_file.c_str(),
                                     std::span<const u8>(out_buffer + out_offset, extracted_size), &error))
    {
      m_progress->FormatModalError("Failed to write output file '{}': {}", ftu.destination_filename,
                                   error.GetDescription());
      FileSystem::DeleteFile(destination_file.c_str());
      return false;
    }

    m_progress->IncrementProgressValue();
  }

  return true;
}

bool Installer::CommitUpdate()
{
  m_progress->SetStatusText("Committing installation...");

  // create directories in target
  for (const std::string& subdir : m_update_directories)
  {
    const std::string dest_subdir = Path::Combine(m_destination_directory, subdir);
    if (!FileSystem::DirectoryExists(dest_subdir.c_str()) && !FileSystem::CreateDirectory(dest_subdir.c_str(), false))
    {
      m_progress->FormatModalError("Failed to create target directory '{}'", dest_subdir);
      return false;
    }
  }

  // move files to target
  for (const FileToUpdate& ftu : m_update_paths)
  {
    const std::string staging_file_name = Path::Combine(m_staging_directory, ftu.destination_filename);
    const std::string dest_file_name = Path::Combine(m_destination_directory, ftu.destination_filename);
    m_progress->FormatInformation("Moving '{}' to '{}'", staging_file_name, dest_file_name);

    Error error;
    const bool result = MoveFileExW(FileSystem::GetWin32Path(staging_file_name).c_str(),
                                    FileSystem::GetWin32Path(dest_file_name).c_str(), MOVEFILE_REPLACE_EXISTING);
    if (!result)
      error.SetWin32(GetLastError());
    if (!result)
    {
      m_progress->FormatModalError("Failed to rename '{}' to '{}': {}", staging_file_name, dest_file_name,
                                   error.GetDescription());
      return false;
    }
  }

  return true;
}

void Installer::CleanupStagingDirectory()
{
  // remove staging directory itself
  if (!RecursiveDeleteDirectory(m_staging_directory.c_str(), true))
    m_progress->FormatError("Failed to remove staging directory '{}'", m_staging_directory);
}

bool Installer::CheckForEmptyDirectory(const std::string& directory)
{
  if (!FileSystem::DirectoryExists(directory.c_str()))
    return true;

  FileSystem::FindResultsArray results;
  if (!FileSystem::FindFiles(directory.c_str(), "*",
                             FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_FOLDERS | FILESYSTEM_FIND_HIDDEN_FILES, &results))
  {
    return true;
  }

  return results.empty();
}

bool Installer::Install()
{
  m_progress->FormatInformation("Destination directory: '{}'", m_destination_directory);
  m_progress->FormatInformation("Staging directory: '{}'", m_staging_directory);

  if (!OpenArchiveStream())
    return false;

  // Create destination directory if it doesn't exist
  if (!FileSystem::DirectoryExists(m_destination_directory.c_str()))
  {
    m_progress->FormatStatusText("Creating directory '{}'...", m_destination_directory);

    // Only create one level of parent - the Programs directory if it doesn't already exist.
    const std::string parent_directory = std::string(Path::GetDirectory(m_destination_directory));
    if (!FileSystem::DirectoryExists(parent_directory.c_str()))
    {
      m_progress->FormatStatusText("Creating parent directory '{}'", parent_directory.c_str());

      Error error;
      if (!FileSystem::CreateDirectory(parent_directory.c_str(), false, &error))
      {
        m_progress->FormatModalError("Failed to create parent directory '{}': {}", parent_directory,
                                     error.GetDescription());
        return false;
      }
    }

    Error error;
    if (!FileSystem::CreateDirectory(m_destination_directory.c_str(), false, &error))
    {
      m_progress->FormatModalError("Failed to create destination directory '{}': {}", m_destination_directory,
                                   error.GetDescription());
      return false;
    }
  }

  if (!PrepareStagingDirectory())
  {
    m_progress->ModalError("Failed to prepare staging directory.");
    CleanupStagingDirectory();
    return false;
  }

  if (!StageUpdate())
  {
    m_progress->ModalError("Failed to stage installation files.");
    CleanupStagingDirectory();
    return false;
  }

  if (!CommitUpdate())
  {
    m_progress->ModalError("Failed to commit installation.");
    CleanupStagingDirectory();
    return false;
  }

  CleanupStagingDirectory();

  CreateUninstallerEntry();

  return true;
}

bool Installer::CreateShellLink(const std::string& link_path, const std::string& target_path)
{
  const std::wstring target_path_w = StringUtil::UTF8StringToWideString(target_path);
  const std::wstring link_path_w = StringUtil::UTF8StringToWideString(link_path);
  const std::wstring working_dir_w = StringUtil::UTF8StringToWideString(m_destination_directory);

  Microsoft::WRL::ComPtr<IShellLinkW> shell_link;
  HRESULT hr =
    CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(shell_link.GetAddressOf()));
  if (FAILED(hr))
  {
    m_progress->FormatModalError("CoCreateInstance(CLSID_ShellLink) failed: {}",
                                 Error::CreateHResult(hr).GetDescription());
    return false;
  }

  shell_link->SetPath(target_path_w.c_str());
  shell_link->SetWorkingDirectory(working_dir_w.c_str());
  shell_link->SetDescription(L"PlayStation 1 Emulator");
  shell_link->SetIconLocation(target_path_w.c_str(), 0);

  Microsoft::WRL::ComPtr<IPersistFile> persist_file;
  hr = shell_link.As(&persist_file);
  if (FAILED(hr))
  {
    m_progress->FormatModalError("IShellLink::QueryInterface(IPersistFile) failed: {}",
                                 Error::CreateHResult(hr).GetDescription());
    return false;
  }

  hr = persist_file->Save(link_path_w.c_str(), TRUE);
  if (FAILED(hr))
  {
    m_progress->FormatModalError("IPersistFile::Save() failed: {}", Error::CreateHResult(hr).GetDescription());
    return false;
  }

  m_progress->FormatInformation("Created shortcut: {}", link_path);
  return true;
}

bool Installer::CreateDesktopShortcut()
{
  m_progress->SetStatusText("Creating desktop shortcut...");

  PWSTR desktop_folder = nullptr;
  HRESULT hr = SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop_folder);
  if (FAILED(hr))
  {
    m_progress->FormatModalError("SHGetKnownFolderPath(FOLDERID_Desktop) failed: {}",
                                 Error::CreateHResult(hr).GetDescription());
    return false;
  }

  const std::string shortcut_path =
    Path::Combine(StringUtil::WideStringToUTF8String(desktop_folder), INSTALLER_SHORTCUT_FILENAME);
  CoTaskMemFree(desktop_folder);

  const std::string target_path = Path::Combine(m_destination_directory, INSTALLER_PROGRAM_FILENAME);
  return CreateShellLink(shortcut_path, target_path);
}

bool Installer::CreateStartMenuShortcut()
{
  m_progress->SetStatusText("Creating Start Menu shortcut...");

  PWSTR programs_folder = nullptr;
  HRESULT hr = SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs_folder);
  if (FAILED(hr))
  {
    m_progress->FormatModalError("SHGetKnownFolderPath(FOLDERID_Programs) failed: {}",
                                 Error::CreateHResult(hr).GetDescription());
    return false;
  }

  const std::string shortcut_path =
    Path::Combine(StringUtil::WideStringToUTF8String(programs_folder), INSTALLER_SHORTCUT_FILENAME);
  CoTaskMemFree(programs_folder);

  const std::string target_path = Path::Combine(m_destination_directory, INSTALLER_PROGRAM_FILENAME);
  return CreateShellLink(shortcut_path, target_path);
}

bool Installer::CreateUninstallerEntry()
{
  m_progress->SetStatusText("Creating uninstaller entry...");

  HKEY key;
  LSTATUS status =
    RegCreateKeyExW(HKEY_CURRENT_USER, INSTALLER_UNINSTALL_REG_KEY, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr);
  if (status != ERROR_SUCCESS)
  {
    m_progress->FormatModalError("Failed to create uninstaller registry key: {}",
                                 Error::CreateWin32(status).GetDescription());
    return false;
  }

  // Estimate the installed size of the application.
  s64 install_size = 0;
  if (FileSystem::FindResultsArray results; FileSystem::FindFiles(
        m_destination_directory.c_str(), "*",
        FILESYSTEM_FIND_RECURSIVE | FILESYSTEM_FIND_RELATIVE_PATHS | FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_FOLDERS,
        &results))
  {
    for (const FILESYSTEM_FIND_DATA& fd : results)
    {
      if (!(fd.Attributes & (FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY | FILESYSTEM_FILE_ATTRIBUTE_LINK)))
        install_size += fd.Size;
    }
  }

  const std::string display_name = "DuckStation";
  const std::string uninstall_path = Path::Combine(m_destination_directory, INSTALLER_UNINSTALLER_FILENAME);
  const std::string install_location = m_destination_directory;
  const std::string display_icon = Path::Combine(m_destination_directory, INSTALLER_PROGRAM_FILENAME);
  const std::string publisher = "Stenzek";
  const DWORD no_modify = 1;
  const DWORD no_repair = 1;

  const std::wstring display_name_w = StringUtil::UTF8StringToWideString(display_name);
  const std::wstring uninstall_path_w = StringUtil::UTF8StringToWideString(uninstall_path);
  const std::wstring install_location_w = StringUtil::UTF8StringToWideString(install_location);
  const std::wstring display_icon_w = StringUtil::UTF8StringToWideString(display_icon);
  const std::wstring publisher_w = StringUtil::UTF8StringToWideString(publisher);

  RegSetValueExW(key, L"DisplayName", 0, REG_SZ, reinterpret_cast<const BYTE*>(display_name_w.c_str()),
                 static_cast<DWORD>((display_name_w.length() + 1) * sizeof(wchar_t)));
  RegSetValueExW(key, L"UninstallString", 0, REG_SZ, reinterpret_cast<const BYTE*>(uninstall_path_w.c_str()),
                 static_cast<DWORD>((uninstall_path_w.length() + 1) * sizeof(wchar_t)));
  RegSetValueExW(key, L"InstallLocation", 0, REG_SZ, reinterpret_cast<const BYTE*>(install_location_w.c_str()),
                 static_cast<DWORD>((install_location_w.length() + 1) * sizeof(wchar_t)));
  RegSetValueExW(key, L"DisplayIcon", 0, REG_SZ, reinterpret_cast<const BYTE*>(display_icon_w.c_str()),
                 static_cast<DWORD>((display_icon_w.length() + 1) * sizeof(wchar_t)));
  RegSetValueExW(key, L"Publisher", 0, REG_SZ, reinterpret_cast<const BYTE*>(publisher_w.c_str()),
                 static_cast<DWORD>((publisher_w.length() + 1) * sizeof(wchar_t)));
  RegSetValueExW(key, L"NoModify", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&no_modify), sizeof(no_modify));
  RegSetValueExW(key, L"NoRepair", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&no_repair), sizeof(no_repair));

  if (const DWORD install_size_kb = static_cast<DWORD>(install_size / 1024); install_size_kb > 0)
  {
    RegSetValueExW(key, L"EstimatedSize", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&install_size_kb),
                   sizeof(install_size_kb));
  }

  RegCloseKey(key);

  m_progress->FormatInformation("Created uninstaller entry");
  return true;
}

bool Installer::LaunchApplication(const std::string& directory, Error* error)
{
  const std::string exe_path = Path::Combine(directory, INSTALLER_PROGRAM_FILENAME);
  const std::wstring exe_path_w = StringUtil::UTF8StringToWideString(exe_path);
  const std::wstring working_dir_w = StringUtil::UTF8StringToWideString(directory);

  SHELLEXECUTEINFOW sei = {};
  sei.cbSize = sizeof(sei);
  sei.fMask = SEE_MASK_DEFAULT;
  sei.lpVerb = L"open";
  sei.lpFile = exe_path_w.c_str();
  sei.lpDirectory = working_dir_w.c_str();
  sei.nShow = SW_SHOWNORMAL;

  if (!ShellExecuteExW(&sei))
  {
    Error::SetStringFmt(error, "ShellExecuteExW() failed: ", GetLastError());
    return false;
  }

  return true;
}
