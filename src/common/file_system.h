#pragma once
#include "timestamp.h"
#include "types.h"
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class ByteStream;

#ifdef WIN32
#define FS_OSPATH_SEPARATOR_CHARACTER '\\'
#define FS_OSPATH_SEPARATOR_STR "\\"
#else
#define FS_OSPATH_SEPARATOR_CHARACTER '/'
#define FS_OSPATH_SEPARATOR_STR "/"
#endif

enum FILESYSTEM_FILE_ATTRIBUTES
{
  FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY = 1,
  FILESYSTEM_FILE_ATTRIBUTE_READ_ONLY = 2,
  FILESYSTEM_FILE_ATTRIBUTE_COMPRESSED = 4,
};

enum FILESYSTEM_FIND_FLAGS
{
  FILESYSTEM_FIND_RECURSIVE = (1 << 0),
  FILESYSTEM_FIND_RELATIVE_PATHS = (1 << 1),
  FILESYSTEM_FIND_HIDDEN_FILES = (1 << 2),
  FILESYSTEM_FIND_FOLDERS = (1 << 3),
  FILESYSTEM_FIND_FILES = (1 << 4),
  FILESYSTEM_FIND_KEEP_ARRAY = (1 << 5),
};

struct FILESYSTEM_STAT_DATA
{
  u32 Attributes;
  Timestamp ModificationTime;
  u64 Size;
};

struct FILESYSTEM_FIND_DATA
{
  std::string FileName;
  Timestamp ModificationTime;
  u32 Attributes;
  u64 Size;
};

struct FILESYSTEM_CHANGE_NOTIFY_DATA
{
  String DirectoryPath;
  bool RecursiveWatch;

  void* pSystemData;
};

namespace FileSystem {

using FindResultsArray = std::vector<FILESYSTEM_FIND_DATA>;

class ChangeNotifier
{
public:
  enum ChangeEvent
  {
    ChangeEvent_FileAdded = (1 << 0),
    ChangeEvent_FileRemoved = (1 << 1),
    ChangeEvent_FileModified = (1 << 2),
    ChangeEvent_RenamedOldName = (1 << 3),
    ChangeEvent_RenamedNewName = (1 << 4),
  };

  struct ChangeInfo
  {
    const char* Path;
    u32 Event;
  };

public:
  virtual ~ChangeNotifier();

  const String& GetDirectoryPath() const { return m_directoryPath; }
  const bool GetRecursiveWatch() const { return m_recursiveWatch; }

  typedef void (*EnumerateChangesCallback)(const ChangeInfo* pChangeInfo, void* pUserData);
  virtual void EnumerateChanges(EnumerateChangesCallback callback, void* pUserData) = 0;

private:
  template<typename CALLBACK_TYPE>
  static void EnumerateChangesTrampoline(const ChangeInfo* pChangeInfo, void* pUserData)
  {
    CALLBACK_TYPE* pRealCallback = reinterpret_cast<CALLBACK_TYPE*>(pUserData);
    (*pRealCallback)(pChangeInfo);
  }

public:
  template<typename CALLBACK_TYPE>
  void EnumerateChanges(CALLBACK_TYPE callback)
  {
    CALLBACK_TYPE* pCallback = &callback;
    EnumerateChanges(&ChangeNotifier::EnumerateChangesTrampoline<CALLBACK_TYPE>, reinterpret_cast<void*>(pCallback));
  }

protected:
  ChangeNotifier(const String& directoryPath, bool recursiveWatch);

  String m_directoryPath;
  bool m_recursiveWatch;
};

// create a change notifier
std::unique_ptr<ChangeNotifier> CreateChangeNotifier(const char* path, bool recursiveWatch);

// canonicalize a path string (i.e. replace .. with actual folder name, etc), if OS path is used, on windows, the
// separators will be \, otherwise /
void CanonicalizePath(char* Destination, u32 cbDestination, const char* Path, bool OSPath = true);
void CanonicalizePath(String& Destination, const char* Path, bool OSPath = true);
void CanonicalizePath(String& Destination, bool OSPath = true);
void CanonicalizePath(std::string& path, bool OSPath = true);

// translates the specified path into a string compatible with the hosting OS
void BuildOSPath(char* Destination, u32 cbDestination, const char* Path);
void BuildOSPath(String& Destination, const char* Path);
void BuildOSPath(String& Destination);

// builds a path relative to the specified file, optionally canonicalizing it
void BuildPathRelativeToFile(char* Destination, u32 cbDestination, const char* CurrentFileName, const char* NewFileName,
                             bool OSPath = true, bool Canonicalize = true);
void BuildPathRelativeToFile(String& Destination, const char* CurrentFileName, const char* NewFileName,
                             bool OSPath = true, bool Canonicalize = true);
String BuildPathRelativeToFile(const char* CurrentFileName, const char* NewFileName, bool OSPath = true,
                               bool Canonicalize = true);

// sanitizes a filename for use in a filesystem.
void SanitizeFileName(char* Destination, u32 cbDestination, const char* FileName, bool StripSlashes = true);
void SanitizeFileName(String& Destination, const char* FileName, bool StripSlashes = true);
void SanitizeFileName(String& Destination, bool StripSlashes = true);
void SanitizeFileName(std::string& Destination, bool StripSlashes = true);

/// Returns true if the specified path is an absolute path (C:\Path on Windows or /path on Unix).
bool IsAbsolutePath(const std::string_view& path);

/// Replaces the extension of a filename with another.
std::string ReplaceExtension(const std::string_view& path, const std::string_view& new_extension);

/// Returns the directory component of a filename.
std::string_view GetPathDirectory(const std::string_view& path);

/// Returns the filename component of a filename.
std::string_view GetFileNameFromPath(const std::string_view& path);

/// Returns the file title (less the extension and path) from a filename.
std::string_view GetFileTitleFromPath(const std::string_view& path);

/// Returns a list of "root directories" (i.e. root/home directories on Linux, drive letters on Windows).
std::vector<std::string> GetRootDirectoryList();

// search for files
bool FindFiles(const char* Path, const char* Pattern, u32 Flags, FindResultsArray* pResults);

// stat file
bool StatFile(const char* Path, FILESYSTEM_STAT_DATA* pStatData);
bool StatFile(std::FILE* fp, FILESYSTEM_STAT_DATA* pStatData);

// file exists?
bool FileExists(const char* Path);

// directory exists?
bool DirectoryExists(const char* Path);

// delete file
bool DeleteFile(const char* Path);

// open files
std::unique_ptr<ByteStream> OpenFile(const char* FileName, u32 Flags);

using ManagedCFilePtr = std::unique_ptr<std::FILE, void (*)(std::FILE*)>;
ManagedCFilePtr OpenManagedCFile(const char* filename, const char* mode);
std::FILE* OpenCFile(const char* filename, const char* mode);

std::optional<std::vector<u8>> ReadBinaryFile(const char* filename);
std::optional<std::vector<u8>> ReadBinaryFile(std::FILE* fp);
std::optional<std::string> ReadFileToString(const char* filename);
std::optional<std::string> ReadFileToString(std::FILE* fp);
bool WriteBinaryFile(const char* filename, const void* data, size_t data_length);
bool WriteFileToString(const char* filename, const std::string_view& sv);

std::string ReadStreamToString(ByteStream* stream, bool seek_to_start = true);
bool WriteStreamToString(const std::string_view& sv, ByteStream* stream);

std::vector<u8> ReadBinaryStream(ByteStream* stream, bool seek_to_start = true);
bool WriteBinaryToSTream(ByteStream* stream, const void* data, size_t data_length);

// creates a directory in the local filesystem
// if the directory already exists, the return value will be true.
// if Recursive is specified, all parent directories will be created
// if they do not exist.
bool CreateDirectory(const char* Path, bool Recursive);

// deletes a directory in the local filesystem
// if the directory has files, unless the recursive flag is set, it will fail
bool DeleteDirectory(const char* Path, bool Recursive);

/// Returns the path to the current executable.
std::string GetProgramPath();

/// Retrieves the current working directory.
std::string GetWorkingDirectory();

/// Sets the current working directory. Returns true if successful.
bool SetWorkingDirectory(const char* path);

}; // namespace FileSystem
