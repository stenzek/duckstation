// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once
#include "common/progress_callback.h"
#include "unzip.h"
#include <string>
#include <vector>

class Updater
{
public:
  Updater(ProgressCallback* progress);
  ~Updater();

  bool Initialize(std::string staging_directory, std::string destination_directory);

  bool OpenUpdateZip(const char* path);
  void RemoveUpdateZip();
  bool PrepareStagingDirectory();
  bool StageUpdate();
  bool CommitUpdate();
  void CleanupStagingDirectory();
  bool ClearDestinationDirectory();

private:
  bool RecursiveDeleteDirectory(const char* path, bool remove_dir);

  struct FileToUpdate
  {
    std::string original_zip_filename;
    std::string destination_filename;
    u32 file_mode;
  };

  bool ParseZip();
  void CloseUpdateZip();

  std::string m_zip_path;
  std::string m_staging_directory;
  std::string m_destination_directory;

  std::vector<FileToUpdate> m_update_paths;
  std::vector<std::string> m_update_directories;

  ProgressCallback* m_progress;
  unzFile m_zf = nullptr;
};
