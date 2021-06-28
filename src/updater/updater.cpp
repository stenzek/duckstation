#include "updater.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/minizip_helpers.h"
#include "common/string_util.h"
#include "common/win32_progress_callback.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#include <shellapi.h>
#endif

Updater::Updater(ProgressCallback* progress) : m_progress(progress)
{
  progress->SetTitle("DuckStation Update Installer");
}

Updater::~Updater()
{
  if (m_zf)
    unzClose(m_zf);
}

bool Updater::Initialize(std::string destination_directory)
{
  m_destination_directory = std::move(destination_directory);
  m_staging_directory = StringUtil::StdStringFromFormat("%s" FS_OSPATH_SEPARATOR_STR "%s",
                                                        m_destination_directory.c_str(), "UPDATE_STAGING");
  m_progress->DisplayFormattedInformation("Destination directory: '%s'", m_destination_directory.c_str());
  m_progress->DisplayFormattedInformation("Staging directory: '%s'", m_staging_directory.c_str());

  // log everything to file as well
  Log::SetFileOutputParams(
    true, StringUtil::StdStringFromFormat("%s" FS_OSPATH_SEPARATOR_STR "updater.log", m_destination_directory.c_str())
            .c_str());

  return true;
}

bool Updater::OpenUpdateZip(const char* path)
{
  m_zf = MinizipHelpers::OpenUnzFile(path);
  if (!m_zf)
    return false;

  m_progress->SetStatusText("Parsing update zip...");
  return ParseZip();
}

bool Updater::RecursiveDeleteDirectory(const char* path)
{
#ifdef _WIN32
  // making this safer on Win32...
  std::wstring wpath(StringUtil::UTF8StringToWideString(path));
  wpath += L'\0';

  SHFILEOPSTRUCTW op = {};
  op.wFunc = FO_DELETE;
  op.pFrom = wpath.c_str();
  op.fFlags = FOF_NOCONFIRMATION;

  return (SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted);
#else
  return FileSystem::DeleteDirectory(path, true);
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

    // skip directories (we sort them out later)
    if (len > 0 && zip_filename_buffer[len - 1] != FS_OSPATH_SEPARATOR_CHARACTER)
    {
      // skip updater itself, since it was already pre-extracted.
      if (StringUtil::Strcasecmp(zip_filename_buffer, "updater.exe") != 0)
      {
        entry.destination_filename = zip_filename_buffer;
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
    if (!RecursiveDeleteDirectory(m_staging_directory.c_str()) ||
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
    m_progress->SetFormattedStatusText("Extracting '%s'...", ftu.original_zip_filename.c_str());

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
#ifdef _WIN32
    const bool result =
      MoveFileExW(StringUtil::UTF8StringToWideString(staging_file_name).c_str(),
                  StringUtil::UTF8StringToWideString(dest_file_name).c_str(), MOVEFILE_REPLACE_EXISTING);
#else
    const bool result = (rename(staging_file_name.c_str(), dest_file_name.c_str()) == 0);
#endif
    if (!result)
    {
      m_progress->DisplayFormattedModalError("Failed to rename '%s' to '%s'", staging_file_name.c_str(),
                                             dest_file_name.c_str());
      return false;
    }
  }

  return true;
}

void Updater::CleanupStagingDirectory()
{
  // remove staging directory itself
  if (!RecursiveDeleteDirectory(m_staging_directory.c_str()))
    m_progress->DisplayFormattedError("Failed to remove staging directory '%s'", m_staging_directory.c_str());
}
