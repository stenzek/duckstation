// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "updater_progress_callback.h"

#include "common/windows_headers.h"

#include "7z.h"
#include "7zFile.h"

#include <string>
#include <vector>

class Error;

class Installer
{
public:
  Installer(UpdaterProgressCallback* progress, std::string destination_directory);
  ~Installer();

  static bool CheckForEmptyDirectory(const std::string& directory);
  static bool LaunchApplication(const std::string& directory, Error* error);

  bool Install();

  bool CreateDesktopShortcut();
  bool CreateStartMenuShortcut();

private:
  bool OpenArchiveStream();
  void CloseArchiveStream();
  bool ParseArchive();
  bool PrepareStagingDirectory();
  bool StageUpdate();
  bool CommitUpdate();
  void CleanupStagingDirectory();
  bool CreateUninstallerEntry();

  bool RecursiveDeleteDirectory(const char* path, bool remove_dir);
  bool CreateShellLink(const std::string& link_path, const std::string& target_path);

  struct FileToUpdate
  {
    std::string destination_filename;
    u32 file_index;
  };

  std::string m_zip_path;
  std::string m_staging_directory;
  std::string m_destination_directory;

  std::vector<FileToUpdate> m_update_paths;
  std::vector<std::string> m_update_directories;

  UpdaterProgressCallback* m_progress;
  CFileInStream m_archive_stream = {};
  CLookToRead2 m_look_stream = {};
  CSzArEx m_archive = {};

  bool m_archive_stream_opened = false;
  bool m_archive_opened = false;
};
