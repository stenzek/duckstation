// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "updater.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/minizip_helpers.h"
#include "common/path.h"
#include "common/progress_callback.h"
#include "common/string_util.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#include "common/windows_headers.h"
#include <Shobjidl.h>
#include <shellapi.h>
#include <wrl/client.h>
#else
#include <sys/stat.h>
#endif

#ifdef __APPLE__
#include "common/cocoa_tools.h"
#endif

Updater::Updater(ProgressCallback* progress) : m_progress(progress)
{
  progress->SetTitle("DuckStation Update Installer");
}

Updater::~Updater()
{
  CloseUpdateZip();
}

bool Updater::Initialize(std::string staging_directory, std::string destination_directory)
{
  m_staging_directory = std::move(staging_directory);
  m_destination_directory = std::move(destination_directory);
  m_progress->FormatInformation("Destination directory: '{}'", m_destination_directory);
  m_progress->FormatInformation("Staging directory: '{}'", m_staging_directory);
  return true;
}

bool Updater::OpenUpdateZip(const char* path)
{
  m_zf = MinizipHelpers::OpenUnzFile(path);
  if (!m_zf)
    return false;

  m_zip_path = path;

  m_progress->SetStatusText("Parsing update zip...");
  return ParseZip();
}

void Updater::CloseUpdateZip()
{
  if (m_zf)
  {
    unzClose(m_zf);
    m_zf = nullptr;
  }
}

void Updater::RemoveUpdateZip()
{
  if (m_zip_path.empty())
    return;

  CloseUpdateZip();

  if (!FileSystem::DeleteFile(m_zip_path.c_str()))
    m_progress->FormatError("Failed to remove update zip '{}'", m_zip_path);
}

bool Updater::RecursiveDeleteDirectory(const char* path, bool remove_dir)
{
#ifdef _WIN32
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
#else
  FileSystem::FindResultsArray results;
  if (FileSystem::FindFiles(path, "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_FOLDERS | FILESYSTEM_FIND_HIDDEN_FILES,
                            &results))
  {
    for (const FILESYSTEM_FIND_DATA& fd : results)
    {
      if (fd.Attributes & FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY)
      {
        if (!RecursiveDeleteDirectory(fd.FileName.c_str(), true))
          return false;
      }
      else
      {
        m_progress->FormatInformation("Removing directory '{}'.", fd.FileName);
        if (!FileSystem::DeleteFile(fd.FileName.c_str()))
          return false;
      }
    }
  }

  if (!remove_dir)
    return true;

  m_progress->FormatInformation("Removing directory '{}'.", path);
  return FileSystem::DeleteDirectory(path);
#endif
}

bool Updater::ParseZip()
{
  if (unzGoToFirstFile(m_zf) != UNZ_OK)
  {
    m_progress->ModalError("unzGoToFirstFile() failed");
    return {};
  }

  for (;;)
  {
    char zip_filename_buffer[256];
    unz_file_info64 file_info;
    if (unzGetCurrentFileInfo64(m_zf, &file_info, zip_filename_buffer, sizeof(zip_filename_buffer), nullptr, 0, nullptr,
                                0) != UNZ_OK)
    {
      m_progress->ModalError("unzGetCurrentFileInfo64() failed");
      return false;
    }

    FileToUpdate entry;
    entry.original_zip_filename = zip_filename_buffer;

    // replace forward slashes with backslashes
    size_t len = std::strlen(zip_filename_buffer);
    for (size_t i = 0; i < len; i++)
    {
      if (zip_filename_buffer[i] == '/' || zip_filename_buffer[i] == '\\')
        zip_filename_buffer[i] = FS_OSPATH_SEPARATOR_CHARACTER;
    }

    // should never have a leading slash. just in case.
    while (zip_filename_buffer[0] == FS_OSPATH_SEPARATOR_CHARACTER)
      std::memmove(&zip_filename_buffer[1], &zip_filename_buffer[0], --len);

#ifdef _WIN32
    entry.file_mode = 0;
#else
    // Preserve permissions on Unix.
    static constexpr u32 PERMISSION_MASK = (S_IRWXO | S_IRWXG | S_IRWXU);
    entry.file_mode =
      ((file_info.external_fa >> 16) & 0x01FFu) & PERMISSION_MASK; // https://stackoverflow.com/a/28753385
#endif

    // skip directories (we sort them out later)
    if (len > 0 && zip_filename_buffer[len - 1] != FS_OSPATH_SEPARATOR_CHARACTER)
    {
      bool process_file = true;
      const char* filename_to_add = zip_filename_buffer;
#ifdef _WIN32
      // skip updater itself, since it was already pre-extracted.
      process_file = process_file && (StringUtil::Strcasecmp(zip_filename_buffer, "updater.exe") != 0);
#elif defined(__APPLE__)
      // on MacOS, we want to remove the DuckStation.app prefix.
      static constexpr const char* PREFIX_PATH = "DuckStation.app/";
      const size_t prefix_length = std::strlen(PREFIX_PATH);
      process_file = process_file && (std::strncmp(zip_filename_buffer, PREFIX_PATH, prefix_length) == 0);
      filename_to_add += prefix_length;
#endif
      if (process_file)
      {
        entry.destination_filename = filename_to_add;
        m_progress->FormatInformation("Found file in zip: '{}'", entry.destination_filename);
        m_update_paths.push_back(std::move(entry));
      }
    }

    int res = unzGoToNextFile(m_zf);
    if (res == UNZ_END_OF_LIST_OF_FILE)
      break;
    if (res != UNZ_OK)
    {
      m_progress->ModalError("unzGoToNextFile() failed");
      return false;
    }
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

bool Updater::PrepareStagingDirectory()
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

bool Updater::StageUpdate()
{
  m_progress->SetProgressRange(static_cast<u32>(m_update_paths.size()));
  m_progress->SetProgressValue(0);

  for (const FileToUpdate& ftu : m_update_paths)
  {
    m_progress->FormatStatusText("Extracting '{}' (mode {:o})...", ftu.original_zip_filename, ftu.file_mode);

    if (unzLocateFile(m_zf, ftu.original_zip_filename.c_str(), 0) != UNZ_OK)
    {
      m_progress->FormatModalError("Unable to locate file '{}' in zip", ftu.original_zip_filename);
      return false;
    }
    else if (unzOpenCurrentFile(m_zf) != UNZ_OK)
    {
      m_progress->FormatModalError("Failed to open file '{}' in zip", ftu.original_zip_filename);
      return false;
    }

    m_progress->FormatInformation("Extracting '{}'...", ftu.destination_filename);

    const std::string destination_file = Path::Combine(m_staging_directory, ftu.destination_filename);
    std::FILE* fp = FileSystem::OpenCFile(destination_file.c_str(), "wb");
    if (!fp)
    {
      m_progress->FormatModalError("Failed to open staging output file '{}'", destination_file);
      unzCloseCurrentFile(m_zf);
      return false;
    }

    static constexpr u32 CHUNK_SIZE = 4096;
    u8 buffer[CHUNK_SIZE];
    for (;;)
    {
      int byte_count = unzReadCurrentFile(m_zf, buffer, CHUNK_SIZE);
      if (byte_count < 0)
      {
        m_progress->FormatModalError("Failed to read file '{}' from zip", ftu.original_zip_filename);
        std::fclose(fp);
        FileSystem::DeleteFile(destination_file.c_str());
        unzCloseCurrentFile(m_zf);
        return false;
      }
      else if (byte_count == 0)
      {
        // end of file
        break;
      }

      if (std::fwrite(buffer, static_cast<size_t>(byte_count), 1, fp) != 1)
      {
        m_progress->FormatModalError("Failed to write to file '{}'", destination_file);
        std::fclose(fp);
        FileSystem::DeleteFile(destination_file.c_str());
        unzCloseCurrentFile(m_zf);
        return false;
      }
    }

#ifndef _WIN32
    if (ftu.file_mode != 0)
    {
      const int fd = fileno(fp);
      const int res = (fd >= 0) ? fchmod(fd, ftu.file_mode) : -1;
      if (res < 0)
      {
        m_progress->FormatModalError("Failed to set mode for file '{}' (fd {}) to {:o}: errno {}", destination_file, fd,
                                     res, errno);
        std::fclose(fp);
        FileSystem::DeleteFile(destination_file.c_str());
        unzCloseCurrentFile(m_zf);
        return false;
      }
    }
#endif

    std::fclose(fp);
    unzCloseCurrentFile(m_zf);
    m_progress->IncrementProgressValue();
  }

  return true;
}

bool Updater::CommitUpdate()
{
  m_progress->SetStatusText("Committing update...");

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
#ifdef _WIN32
    const bool result = MoveFileExW(FileSystem::GetWin32Path(staging_file_name).c_str(),
                                    FileSystem::GetWin32Path(dest_file_name).c_str(), MOVEFILE_REPLACE_EXISTING);
    if (!result)
      error.SetWin32(GetLastError());
#elif defined(__APPLE__)
    const bool result = CocoaTools::MoveFile(staging_file_name.c_str(), dest_file_name.c_str(), &error);
#else
    const bool result = (rename(staging_file_name.c_str(), dest_file_name.c_str()) == 0);
    if (!result)
      error.SetErrno(errno);
#endif
    if (!result)
    {
      m_progress->FormatModalError("Failed to rename '{}' to '{}': {}", staging_file_name, dest_file_name,
                                   error.GetDescription());
      return false;
    }
  }

  return true;
}

void Updater::CleanupStagingDirectory()
{
  // remove staging directory itself
  if (!RecursiveDeleteDirectory(m_staging_directory.c_str(), true))
    m_progress->FormatError("Failed to remove staging directory '{}'", m_staging_directory);
}

bool Updater::ClearDestinationDirectory()
{
  return RecursiveDeleteDirectory(m_destination_directory.c_str(), false);
}
