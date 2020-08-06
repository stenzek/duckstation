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

  bool Initialize(std::string destination_directory);

  bool OpenUpdateZip(const char* path);
  bool PrepareStagingDirectory();
  bool StageUpdate();
  bool CommitUpdate();
  void CleanupStagingDirectory();

private:
  static bool RecursiveDeleteDirectory(const char* path);

  struct FileToUpdate
  {
    std::string original_zip_filename;
    std::string destination_filename;
  };

  bool ParseZip();

  std::string m_destination_directory;
  std::string m_staging_directory;

  std::vector<FileToUpdate> m_update_paths;
  std::vector<std::string> m_update_directories;

  ProgressCallback* m_progress;
  unzFile m_zf = nullptr;
};
