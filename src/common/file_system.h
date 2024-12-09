// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "heap_array.h"
#include "types.h"

#include <cstdio>
#include <ctime>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <sys/stat.h>
#include <vector>

class Error;

enum FILESYSTEM_FILE_ATTRIBUTES
{
  FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY = (1 << 0),
  FILESYSTEM_FILE_ATTRIBUTE_READ_ONLY = (1 << 1),
  FILESYSTEM_FILE_ATTRIBUTE_COMPRESSED = (1 << 2),
  FILESYSTEM_FILE_ATTRIBUTE_LINK = (1 << 3),
};

enum FILESYSTEM_FIND_FLAGS
{
  FILESYSTEM_FIND_RECURSIVE = (1 << 0),
  FILESYSTEM_FIND_RELATIVE_PATHS = (1 << 1),
  FILESYSTEM_FIND_HIDDEN_FILES = (1 << 2),
  FILESYSTEM_FIND_FOLDERS = (1 << 3),
  FILESYSTEM_FIND_FILES = (1 << 4),
  FILESYSTEM_FIND_KEEP_ARRAY = (1 << 5),
  FILESYSTEM_FIND_SORT_BY_NAME = (1 << 6),
};

struct FILESYSTEM_STAT_DATA
{
  std::time_t CreationTime; // actually inode change time on linux
  std::time_t ModificationTime;
  s64 Size;
  u32 Attributes;
};

struct FILESYSTEM_FIND_DATA
{
  std::time_t CreationTime; // actually inode change time on linux
  std::time_t ModificationTime;
  std::string FileName;
  s64 Size;
  u32 Attributes;
};

namespace FileSystem {
using FindResultsArray = std::vector<FILESYSTEM_FIND_DATA>;

/// Returns the display name of a filename. Usually this is the same as the path.
std::string GetDisplayNameFromPath(std::string_view path);

/// Returns a list of "root directories" (i.e. root/home directories on Linux, drive letters on Windows).
std::vector<std::string> GetRootDirectoryList();

/// Search for files
bool FindFiles(const char* path, const char* pattern, u32 flags, FindResultsArray* results);

/// Stat file
bool StatFile(const char* path, struct stat* st, Error* error = nullptr);
bool StatFile(std::FILE* fp, struct stat* st, Error* error = nullptr);
bool StatFile(const char* path, FILESYSTEM_STAT_DATA* sd, Error* error = nullptr);
bool StatFile(std::FILE* fp, FILESYSTEM_STAT_DATA* sd, Error* error = nullptr);
s64 GetPathFileSize(const char* path);

/// File exists?
bool FileExists(const char* path);

/// Directory exists?
bool DirectoryExists(const char* path);
bool IsRealDirectory(const char* path);

/// Directory does not contain any files?
bool IsDirectoryEmpty(const char* path);

/// Delete file
bool DeleteFile(const char* path, Error* error = nullptr);

/// Rename file
bool RenamePath(const char* OldPath, const char* NewPath, Error* error = nullptr);

/// Deleter functor for managed file pointers
struct FileDeleter
{
  ALWAYS_INLINE void operator()(std::FILE* fp)
  {
    if (fp)
      std::fclose(fp);
  }
};

/// open files
using ManagedCFilePtr = std::unique_ptr<std::FILE, FileDeleter>;
ManagedCFilePtr OpenManagedCFile(const char* path, const char* mode, Error* error = nullptr);
std::FILE* OpenCFile(const char* path, const char* mode, Error* error = nullptr);

/// Atomically opens a file in read/write mode, and if the file does not exist, creates it.
/// On Windows, if retry_ms is positive, this function will retry opening the file for this
/// number of milliseconds. NOTE: The file is opened in binary mode.
std::FILE* OpenExistingOrCreateCFile(const char* path, s32 retry_ms = -1, Error* error = nullptr);
ManagedCFilePtr OpenExistingOrCreateManagedCFile(const char* path, s32 retry_ms = -1, Error* error = nullptr);

int FSeek64(std::FILE* fp, s64 offset, int whence);
bool FSeek64(std::FILE* fp, s64 offset, int whence, Error* error);
s64 FTell64(std::FILE* fp);
s64 FSize64(std::FILE* fp, Error* error = nullptr);
bool FTruncate64(std::FILE* fp, s64 size, Error* error = nullptr);

int OpenFDFile(const char* path, int flags, int mode, Error* error = nullptr);

/// Sharing modes for OpenSharedCFile().
enum class FileShareMode
{
  DenyReadWrite, /// Exclusive access.
  DenyWrite,     /// Other processes can read from this file.
  DenyRead,      /// Other processes can write to this file.
  DenyNone,      /// Other processes can read and write to this file.
};

/// Opens a file in shareable mode (where other processes can access it concurrently).
/// Only has an effect on Windows systems.
ManagedCFilePtr OpenManagedSharedCFile(const char* path, const char* mode, FileShareMode share_mode,
                                       Error* error = nullptr);
std::FILE* OpenSharedCFile(const char* path, const char* mode, FileShareMode share_mode, Error* error = nullptr);

/// Atomically-updated file creation.
class AtomicRenamedFileDeleter
{
public:
  AtomicRenamedFileDeleter(std::string temp_path, std::string final_path);
  ~AtomicRenamedFileDeleter();

  void operator()(std::FILE* fp);
  bool commit(std::FILE* fp, Error* error); // closes file
  void discard();

private:
  std::string m_temp_path;
  std::string m_final_path;
};
using AtomicRenamedFile = std::unique_ptr<std::FILE, AtomicRenamedFileDeleter>;
AtomicRenamedFile CreateAtomicRenamedFile(std::string path, Error* error = nullptr);
bool WriteAtomicRenamedFile(std::string path, const void* data, size_t data_length, Error* error = nullptr);
bool WriteAtomicRenamedFile(std::string path, const std::span<const u8> data, Error* error = nullptr);
bool CommitAtomicRenamedFile(AtomicRenamedFile& file, Error* error);
void DiscardAtomicRenamedFile(AtomicRenamedFile& file);

/// Abstracts a POSIX file lock.
#if !defined(_WIN32) && !defined(__ANDROID__)
#define HAS_POSIX_FILE_LOCK 1
#endif

#ifdef HAS_POSIX_FILE_LOCK

class POSIXLock
{
public:
  POSIXLock();
  POSIXLock(int fd, bool block = true, Error* error = nullptr);
  POSIXLock(std::FILE* fp, bool block = true, Error* error = nullptr);
  POSIXLock(POSIXLock&& move);
  POSIXLock(const POSIXLock&) = delete;
  ~POSIXLock();

  POSIXLock& operator=(POSIXLock&& move);
  POSIXLock& operator=(const POSIXLock&) = delete;

  ALWAYS_INLINE bool IsLocked() const { return (m_fd >= 0); }
  void Unlock();

private:
  int m_fd;
};

#endif

std::optional<DynamicHeapArray<u8>> ReadBinaryFile(const char* path, Error* error = nullptr);
std::optional<DynamicHeapArray<u8>> ReadBinaryFile(std::FILE* fp, Error* error = nullptr);
std::optional<std::string> ReadFileToString(const char* path, Error* error = nullptr);
std::optional<std::string> ReadFileToString(std::FILE* fp, Error* error = nullptr);
bool WriteBinaryFile(const char* path, const void* data, size_t data_length, Error* error = nullptr);
bool WriteBinaryFile(const char* path, const std::span<const u8> data, Error* error = nullptr);
bool WriteStringToFile(const char* path, std::string_view sv, Error* error = nullptr);

/// creates a directory in the local filesystem
/// if the directory already exists, the return value will be true.
/// if Recursive is specified, all parent directories will be created
/// if they do not exist.
bool CreateDirectory(const char* path, bool recursive, Error* error = nullptr);

/// Creates a directory if it doesn't already exist.
/// Returns false if it does not exist and creation failed.
bool EnsureDirectoryExists(const char* path, bool recursive, Error* error = nullptr);

/// Removes a directory.
bool DeleteDirectory(const char* path);

/// Recursively removes a directory and all subdirectories/files.
bool RecursiveDeleteDirectory(const char* path);

/// Copies one file to another, optionally replacing it if it already exists.
bool CopyFilePath(const char* source, const char* destination, bool replace);

/// Returns the path to the current executable.
std::string GetProgramPath();

/// Retrieves the current working directory.
std::string GetWorkingDirectory();

/// Sets the current working directory. Returns true if successful.
bool SetWorkingDirectory(const char* path);

/// Enables/disables NTFS compression on a file or directory.
/// Does not apply the compression flag recursively if called for a directory.
/// Does nothing and returns false on non-Windows platforms.
bool SetPathCompression(const char* path, bool enable);

#ifdef _WIN32
// Path limit remover, but also converts to a wide string at the same time.
bool GetWin32Path(std::wstring* dest, std::string_view str);
std::wstring GetWin32Path(std::string_view str);
#endif
}; // namespace FileSystem
