// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

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
  m_progress->DisplayFormattedInformation("Destination directory: '%s'", m_destination_directory.c_str());
  m_progress->DisplayFormattedInformation("Staging directory: '%s'", m_staging_directory.c_str());
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
    m_progress->DisplayFormattedError("Failed to remove update zip '%s'", m_zip_path.c_str());
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
    m_progress->DisplayFormattedError("CoCreateInstance() for IFileOperation failed: %08X", hr);
    return false;
  }

  Microsoft::WRL::ComPtr<IShellItem> item;
  hr = SHCreateItemFromParsingName(StringUtil::UTF8StringToWideString(path).c_str(), NULL,
                                   IID_PPV_ARGS(item.ReleaseAndGetAddressOf()));
  if (FAILED(hr))
  {
    m_progress->DisplayFormattedError("SHCreateItemFromParsingName() for delete failed: %08X", hr);
    return false;
  }

  hr = fo->SetOperationFlags(FOF_NOCONFIRMATION | FOF_SILENT);
  if (FAILED(hr))
    m_progress->DisplayFormattedWarning("IFileOperation::SetOperationFlags() failed: %08X", hr);

  hr = fo->DeleteItem(item.Get(), nullptr);
  if (FAILED(hr))
  {
    m_progress->DisplayFormattedError("IFileOperation::DeleteItem() failed: %08X", hr);
    return false;
  }

  item.Reset();
  hr = fo->PerformOperations();
  if (FAILED(hr))
  {
    m_progress->DisplayFormattedError("IFileOperation::PerformOperations() failed: %08X", hr);
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
        m_progress->DisplayFormattedInformation("Removing directory '%s'.", fd.FileName.c_str());
        if (!FileSystem::DeleteFile(fd.FileName.c_str()))
          return false;
      }
    }
  }

  if (!remove_dir)
    return true;

  m_progress->DisplayFormattedInformation("Removing directory '%s'.", path);
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
        m_progress->DisplayFormattedInformation("Found file in zip: '%s'", entry.destination_filename.c_str());
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
    m_progress->DisplayFormattedDebugMessage("Directory: %s", dir.c_str());

  return true;
}

bool Updater::PrepareStagingDirectory()
{
  if (FileSystem::DirectoryExists(m_staging_directory.c_str()))
  {
    m_progress->DisplayFormattedWarning("Update staging directory already exists, removing");
    if (!RecursiveDeleteDirectory(m_staging_directory.c_str(), true) ||
        FileSystem::DirectoryExists(m_staging_directory.c_str()))
    {
      m_progress->ModalError("Failed to remove old staging directory");
      return false;
    }
  }
  if (!FileSystem::CreateDirectory(m_staging_directory.c_str(), false))
  {
    m_progress->DisplayFormattedModalError("Failed to create staging directory %s", m_staging_directory.c_str());
    return false;
  }

  // create subdirectories in staging directory
  for (const std::string& subdir : m_update_directories)
  {
    m_progress->DisplayFormattedInformation("Creating subdirectory in staging: %s", subdir.c_str());

    const std::string staging_subdir =
      StringUtil::StdStringFromFormat("%s" FS_OSPATH_SEPARATOR_STR "%s", m_staging_directory.c_str(), subdir.c_str());
    if (!FileSystem::CreateDirectory(staging_subdir.c_str(), false))
    {
      m_progress->DisplayFormattedModalError("Failed to create staging subdirectory %s", staging_subdir.c_str());
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
    m_progress->SetFormattedStatusText("Extracting '%s' (mode %o)...", ftu.original_zip_filename.c_str(),
                                       ftu.file_mode);

    if (unzLocateFile(m_zf, ftu.original_zip_filename.c_str(), 0) != UNZ_OK)
    {
      m_progress->DisplayFormattedModalError("Unable to locate file '%s' in zip", ftu.original_zip_filename.c_str());
      return false;
    }
    else if (unzOpenCurrentFile(m_zf) != UNZ_OK)
    {
      m_progress->DisplayFormattedModalError("Failed to open file '%s' in zip", ftu.original_zip_filename.c_str());
      return false;
    }

    m_progress->DisplayFormattedInformation("Extracting '%s'...", ftu.destination_filename.c_str());

    const std::string destination_file = StringUtil::StdStringFromFormat(
      "%s" FS_OSPATH_SEPARATOR_STR "%s", m_staging_directory.c_str(), ftu.destination_filename.c_str());
    std::FILE* fp = FileSystem::OpenCFile(destination_file.c_str(), "wb");
    if (!fp)
    {
      m_progress->DisplayFormattedModalError("Failed to open staging output file '%s'", destination_file.c_str());
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
        m_progress->DisplayFormattedModalError("Failed to read file '%s' from zip", ftu.original_zip_filename.c_str());
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
        m_progress->DisplayFormattedModalError("Failed to write to file '%s'", destination_file.c_str());
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
        m_progress->DisplayFormattedModalError("Failed to set mode for file '%s' (fd %d) to %u: errno %d",
                                               destination_file.c_str(), fd, res, errno);
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
    const std::string dest_subdir = StringUtil::StdStringFromFormat("%s" FS_OSPATH_SEPARATOR_STR "%s",
                                                                    m_destination_directory.c_str(), subdir.c_str());

    if (!FileSystem::DirectoryExists(dest_subdir.c_str()) && !FileSystem::CreateDirectory(dest_subdir.c_str(), false))
    {
      m_progress->DisplayFormattedModalError("Failed to create target directory '%s'", dest_subdir.c_str());
      return false;
    }
  }

  // move files to target
  for (const FileToUpdate& ftu : m_update_paths)
  {
    const std::string staging_file_name = StringUtil::StdStringFromFormat(
      "%s" FS_OSPATH_SEPARATOR_STR "%s", m_staging_directory.c_str(), ftu.destination_filename.c_str());
    const std::string dest_file_name = StringUtil::StdStringFromFormat(
      "%s" FS_OSPATH_SEPARATOR_STR "%s", m_destination_directory.c_str(), ftu.destination_filename.c_str());
    m_progress->DisplayFormattedInformation("Moving '%s' to '%s'", staging_file_name.c_str(), dest_file_name.c_str());

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
#endif
    if (!result)
    {
      m_progress->DisplayFormattedModalError("Failed to rename '%s' to '%s': %s", staging_file_name.c_str(),
                                             dest_file_name.c_str(), error.GetDescription().c_str());
      return false;
    }
  }

  return true;
}

void Updater::CleanupStagingDirectory()
{
  // remove staging directory itself
  if (!RecursiveDeleteDirectory(m_staging_directory.c_str(), true))
    m_progress->DisplayFormattedError("Failed to remove staging directory '%s'", m_staging_directory.c_str());
}

bool Updater::ClearDestinationDirectory()
{
  return RecursiveDeleteDirectory(m_destination_directory.c_str(), false);
}
