#include "file_system.h"
#include "assert.h"
#include "byte_stream.h"
#include "log.h"
#include "string_util.h"
#include <algorithm>
#include <cstring>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <stdlib.h>
#include <sys/param.h>
#else
#include <malloc.h>
#endif

#if defined(WIN32)
#include <shlobj.h>
#else
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

Log_SetChannel(FileSystem);

namespace FileSystem {

ChangeNotifier::ChangeNotifier(const String& directoryPath, bool recursiveWatch)
  : m_directoryPath(directoryPath), m_recursiveWatch(recursiveWatch)
{
}

ChangeNotifier::~ChangeNotifier() {}

void CanonicalizePath(char* Destination, u32 cbDestination, const char* Path, bool OSPath /*= true*/)
{
  u32 i, j;
  DebugAssert(Destination && cbDestination > 0 && Path);

  // get length
  u32 pathLength = static_cast<u32>(std::strlen(Path));

  // clone to a local buffer if the same pointer
  if (Destination == Path)
  {
    char* pathClone = (char*)alloca(pathLength + 1);
    StringUtil::Strlcpy(pathClone, Path, pathLength + 1);
    Path = pathClone;
  }

  // zero destination
  std::memset(Destination, 0, cbDestination);

  // iterate path
  u32 destinationLength = 0;
  for (i = 0; i < pathLength;)
  {
    char prevCh = (i > 0) ? Path[i - 1] : '\0';
    char currentCh = Path[i];
    char nextCh = (i < pathLength) ? Path[i + 1] : '\0';

    if (currentCh == '.')
    {
      if (prevCh == '\\' || prevCh == '/' || prevCh == '\0')
      {
        // handle '.'
        if (nextCh == '\\' || nextCh == '/' || nextCh == '\0')
        {
          // skip '.\'
          i++;

          // remove the previous \, if we have one trailing the dot it'll append it anyway
          if (destinationLength > 0)
            Destination[--destinationLength] = '\0';

          continue;
        }
        // handle '..'
        else if (nextCh == '.')
        {
          char afterNext = ((i + 1) < pathLength) ? Path[i + 2] : '\0';
          if (afterNext == '\\' || afterNext == '/' || afterNext == '\0')
          {
            // remove one directory of the path, including the /.
            if (destinationLength > 1)
            {
              for (j = destinationLength - 2; j > 0; j--)
              {
                if (Destination[j] == '\\' || Destination[j] == '/')
                  break;
              }

              destinationLength = j;
#ifdef _DEBUG
              Destination[destinationLength] = '\0';
#endif
            }

            // skip the dot segment
            i += 2;
            continue;
          }
        }
      }
    }

    // fix ospath
    if (OSPath && (currentCh == '\\' || currentCh == '/'))
      currentCh = FS_OSPATH_SEPERATOR_CHARACTER;

    // copy character
    if (destinationLength < cbDestination)
    {
      Destination[destinationLength++] = currentCh;
#ifdef _DEBUG
      Destination[destinationLength] = '\0';
#endif
    }
    else
      break;

    // increment position by one
    i++;
  }

  // ensure nullptr termination
  if (destinationLength < cbDestination)
    Destination[destinationLength] = '\0';
  else
    Destination[destinationLength - 1] = '\0';
}

void CanonicalizePath(String& Destination, const char* Path, bool OSPath /* = true */)
{
  // the function won't actually write any more characters than are present to the buffer,
  // so we can get away with simply passing both pointers if they are the same.
  if (Destination.GetWriteableCharArray() != Path)
  {
    // otherwise, resize the destination to at least the source's size, and then pass as-is
    Destination.Reserve(static_cast<u32>(std::strlen(Path)) + 1);
  }

  CanonicalizePath(Destination.GetWriteableCharArray(), Destination.GetBufferSize(), Path, OSPath);
  Destination.UpdateSize();
}

void CanonicalizePath(String& Destination, bool OSPath /* = true */)
{
  CanonicalizePath(Destination, Destination);
}

void CanonicalizePath(std::string& path, bool OSPath /*= true*/)
{
  CanonicalizePath(path.data(), static_cast<u32>(path.size() + 1), path.c_str(), OSPath);
}

static inline bool FileSystemCharacterIsSane(char c, bool StripSlashes)
{
  if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') && c != ' ' && c != ' ' &&
      c != '_' && c != '-')
  {
    if (!StripSlashes && (c == '/' || c == '\\'))
      return true;

    return false;
  }

  return true;
}

void SanitizeFileName(char* Destination, u32 cbDestination, const char* FileName, bool StripSlashes /* = true */)
{
  u32 i;
  u32 fileNameLength = static_cast<u32>(std::strlen(FileName));

  if (FileName == Destination)
  {
    for (i = 0; i < fileNameLength; i++)
    {
      if (!FileSystemCharacterIsSane(FileName[i], StripSlashes))
        Destination[i] = '_';
    }
  }
  else
  {
    for (i = 0; i < fileNameLength && i < cbDestination; i++)
    {
      if (FileSystemCharacterIsSane(FileName[i], StripSlashes))
        Destination[i] = FileName[i];
      else
        Destination[i] = '_';
    }
  }
}

void SanitizeFileName(String& Destination, const char* FileName, bool StripSlashes /* = true */)
{
  u32 i;
  u32 fileNameLength;

  // if same buffer, use fastpath
  if (Destination.GetWriteableCharArray() == FileName)
  {
    fileNameLength = Destination.GetLength();
    for (i = 0; i < fileNameLength; i++)
    {
      if (!FileSystemCharacterIsSane(FileName[i], StripSlashes))
        Destination[i] = '_';
    }
  }
  else
  {
    fileNameLength = static_cast<u32>(std::strlen(FileName));
    Destination.Resize(fileNameLength);
    for (i = 0; i < fileNameLength; i++)
    {
      if (FileSystemCharacterIsSane(FileName[i], StripSlashes))
        Destination[i] = FileName[i];
      else
        Destination[i] = '_';
    }
  }
}

void SanitizeFileName(String& Destination, bool StripSlashes /* = true */)
{
  return SanitizeFileName(Destination, Destination, StripSlashes);
}

std::string ReplaceExtension(std::string_view path, std::string_view new_extension)
{
  std::string_view::size_type pos = path.rfind('.');
  if (pos == std::string::npos)
    return std::string(path);

  std::string ret(path, 0, pos + 1);
  ret.append(new_extension);
  return ret;
}

std::string GetPathDirectory(const char* path)
{
#ifdef WIN32
  const char* forwardslash_ptr = std::strrchr(path, '/');
  const char* backslash_ptr = std::strrchr(path, '\\');
  const char* slash_ptr;
  if (forwardslash_ptr && backslash_ptr)
    slash_ptr = std::max(forwardslash_ptr, backslash_ptr);
  else if (backslash_ptr)
    slash_ptr = backslash_ptr;
  else if (forwardslash_ptr)
    slash_ptr = forwardslash_ptr;
  else
    return {};
#else
  const char* slash_ptr = std::strrchr(path, '/');
  if (!slash_ptr)
    return {};
#endif

  if (slash_ptr == path)
    return {};

  std::string str;
  str.append(path, slash_ptr - path);
  return str;
}

void BuildPathRelativeToFile(char* Destination, u32 cbDestination, const char* CurrentFileName, const char* NewFileName,
                             bool OSPath /* = true */, bool Canonicalize /* = true */)
{
  s32 i;
  u32 currentPos = 0;
  DebugAssert(Destination != nullptr && cbDestination > 0 && CurrentFileName != nullptr && NewFileName != nullptr);

  // clone to a local buffer if the same pointer
  std::string pathClone;
  if (Destination == CurrentFileName)
  {
    pathClone = CurrentFileName;
    CurrentFileName = pathClone.c_str();
  }

  // search for a / or \, copy everything up to and including it to the destination
  i = (s32)std::strlen(CurrentFileName);
  for (; i >= 0; i--)
  {
    if (CurrentFileName[i] == '/' || CurrentFileName[i] == '\\')
    {
      // cap to destination length
      u32 copyLen;
      if (NewFileName[0] != '\0')
        copyLen = std::min((u32)(i + 1), cbDestination);
      else
        copyLen = std::min((u32)i, cbDestination);

      if (copyLen > 0)
      {
        std::memcpy(Destination, CurrentFileName, copyLen);
        if (copyLen == cbDestination)
          Destination[cbDestination - 1] = '\0';

        currentPos = copyLen;
      }

      break;
    }
  }

  // copy the new parts in
  if (currentPos < cbDestination && NewFileName[0] != '\0')
    StringUtil::Strlcpy(Destination + currentPos, NewFileName, cbDestination - currentPos);

  // canonicalize it
  if (Canonicalize)
    CanonicalizePath(Destination, cbDestination, Destination, OSPath);
  else if (OSPath)
    BuildOSPath(Destination, cbDestination, Destination);
}

void BuildPathRelativeToFile(String& Destination, const char* CurrentFileName, const char* NewFileName,
                             bool OSPath /* = true */, bool Canonicalize /* = true */)
{
  s32 i;
  DebugAssert(CurrentFileName != nullptr && NewFileName != nullptr);

  // get curfile length
  u32 curFileLength = static_cast<u32>(std::strlen(CurrentFileName));

  // clone to a local buffer if the same pointer
  if (Destination.GetWriteableCharArray() == CurrentFileName)
  {
    char* pathClone = (char*)alloca(curFileLength + 1);
    StringUtil::Strlcpy(pathClone, CurrentFileName, curFileLength + 1);
    CurrentFileName = pathClone;
  }

  // search for a / or \\, copy everything up to and including it to the destination
  Destination.Clear();
  i = (s32)curFileLength;
  for (; i >= 0; i--)
  {
    if (CurrentFileName[i] == '/' || CurrentFileName[i] == '\\')
    {
      if (NewFileName[0] != '\0')
        Destination.AppendSubString(CurrentFileName, 0, i + 1);
      else
        Destination.AppendSubString(CurrentFileName, 0, i);

      break;
    }
  }

  // copy the new parts in
  if (NewFileName[0] != '\0')
    Destination.AppendString(NewFileName);

  // canonicalize it
  if (Canonicalize)
    CanonicalizePath(Destination, Destination.GetCharArray(), OSPath);
  else if (OSPath)
    BuildOSPath(Destination, Destination.GetCharArray());
}

std::unique_ptr<ByteStream> OpenFile(const char* FileName, u32 Flags)
{
  // has a path
  if (FileName[0] == '\0')
    return nullptr;

  // forward to local file wrapper
  return ByteStream_OpenFileStream(FileName, Flags);
}

FileSystem::ManagedCFilePtr OpenManagedCFile(const char* filename, const char* mode)
{
  return ManagedCFilePtr(OpenCFile(filename, mode), [](std::FILE* fp) { std::fclose(fp); });
}

std::FILE* OpenCFile(const char* filename, const char* mode)
{
#ifdef WIN32
  std::FILE* fp;
  if (fopen_s(&fp, filename, mode) != 0)
    return nullptr;

  return fp;
#else
  return std::fopen(filename, mode);
#endif
}

void BuildOSPath(char* Destination, u32 cbDestination, const char* Path)
{
  u32 i;
  u32 pathLength = static_cast<u32>(std::strlen(Path));

  if (Destination == Path)
  {
    // fast path
    for (i = 0; i < pathLength; i++)
    {
      if (Destination[i] == '/')
        Destination[i] = FS_OSPATH_SEPERATOR_CHARACTER;
    }
  }
  else
  {
    // slow path
    pathLength = std::max(pathLength, cbDestination - 1);
    for (i = 0; i < pathLength; i++)
    {
      Destination[i] = (Path[i] == '/') ? FS_OSPATH_SEPERATOR_CHARACTER : Path[i];
    }

    Destination[pathLength] = '\0';
  }
}

void BuildOSPath(String& Destination, const char* Path)
{
  u32 i;
  u32 pathLength;

  if (Destination.GetWriteableCharArray() == Path)
  {
    // fast path
    pathLength = Destination.GetLength();
    ;
    for (i = 0; i < pathLength; i++)
    {
      if (Destination[i] == '/')
        Destination[i] = FS_OSPATH_SEPERATOR_CHARACTER;
    }
  }
  else
  {
    // slow path
    pathLength = static_cast<u32>(std::strlen(Path));
    Destination.Resize(pathLength);
    for (i = 0; i < pathLength; i++)
    {
      Destination[i] = (Path[i] == '/') ? FS_OSPATH_SEPERATOR_CHARACTER : Path[i];
    }
  }
}

void BuildOSPath(String& Destination)
{
  BuildOSPath(Destination, Destination);
}

#ifdef _WIN32

static u32 TranslateWin32Attributes(u32 Win32Attributes)
{
  u32 r = 0;

  if (Win32Attributes & FILE_ATTRIBUTE_DIRECTORY)
    r |= FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY;
  if (Win32Attributes & FILE_ATTRIBUTE_READONLY)
    r |= FILESYSTEM_FILE_ATTRIBUTE_READ_ONLY;
  if (Win32Attributes & FILE_ATTRIBUTE_COMPRESSED)
    r |= FILESYSTEM_FILE_ATTRIBUTE_COMPRESSED;

  return r;
}

static const u32 READ_DIRECTORY_CHANGES_NOTIFY_FILTER = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                                        FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |
                                                        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION;

class ChangeNotifierWin32 : public FileSystem::ChangeNotifier
{
public:
  ChangeNotifierWin32(HANDLE hDirectory, const String& directoryPath, bool recursiveWatch)
    : FileSystem::ChangeNotifier(directoryPath, recursiveWatch), m_hDirectory(hDirectory),
      m_directoryChangeQueued(false)
  {
    m_bufferSize = 16384;
    m_pBuffer = new byte[m_bufferSize];
  }

  virtual ~ChangeNotifierWin32()
  {
    // if there is outstanding io, cancel it
    if (m_directoryChangeQueued)
    {
      CancelIo(m_hDirectory);

      DWORD bytesTransferred;
      GetOverlappedResult(m_hDirectory, &m_overlapped, &bytesTransferred, TRUE);
    }

    CloseHandle(m_hDirectory);
    delete[] m_pBuffer;
  }

  virtual void EnumerateChanges(EnumerateChangesCallback callback, void* pUserData) override
  {
    DWORD bytesRead;
    if (!GetOverlappedResult(m_hDirectory, &m_overlapped, &bytesRead, FALSE))
    {
      if (GetLastError() == ERROR_IO_INCOMPLETE)
        return;

      CancelIo(m_hDirectory);
      m_directoryChangeQueued = false;

      QueueReadDirectoryChanges();
      return;
    }

    // not queued any more
    m_directoryChangeQueued = false;

    // has any bytes?
    if (bytesRead > 0)
    {
      const byte* pCurrentPointer = m_pBuffer;
      PathString fileName;
      for (;;)
      {
        const FILE_NOTIFY_INFORMATION* pFileNotifyInformation =
          reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(pCurrentPointer);

        // translate the event
        u32 changeEvent = 0;
        if (pFileNotifyInformation->Action == FILE_ACTION_ADDED)
          changeEvent = ChangeEvent_FileAdded;
        else if (pFileNotifyInformation->Action == FILE_ACTION_REMOVED)
          changeEvent = ChangeEvent_FileRemoved;
        else if (pFileNotifyInformation->Action == FILE_ACTION_MODIFIED)
          changeEvent = ChangeEvent_FileModified;
        else if (pFileNotifyInformation->Action == FILE_ACTION_RENAMED_OLD_NAME)
          changeEvent = ChangeEvent_RenamedOldName;
        else if (pFileNotifyInformation->Action == FILE_ACTION_RENAMED_NEW_NAME)
          changeEvent = ChangeEvent_RenamedNewName;

        // translate the filename
        int fileNameLength =
          WideCharToMultiByte(CP_UTF8, 0, pFileNotifyInformation->FileName,
                              pFileNotifyInformation->FileNameLength / sizeof(WCHAR), nullptr, 0, nullptr, nullptr);
        DebugAssert(fileNameLength >= 0);
        fileName.Resize(fileNameLength);
        fileNameLength = WideCharToMultiByte(CP_UTF8, 0, pFileNotifyInformation->FileName,
                                             pFileNotifyInformation->FileNameLength / sizeof(WCHAR),
                                             fileName.GetWriteableCharArray(), fileName.GetLength(), nullptr, nullptr);
        if (fileNameLength != (int)fileName.GetLength())
          fileName.Resize(fileNameLength);

        // prepend the base path
        fileName.PrependFormattedString("%s\\", m_directoryPath.GetCharArray());

        // construct change info
        ChangeInfo changeInfo;
        changeInfo.Path = fileName;
        changeInfo.Event = changeEvent;

        // invoke callback
        callback(&changeInfo, pUserData);

        // has a next entry?
        if (pFileNotifyInformation->NextEntryOffset == 0)
          break;

        pCurrentPointer += pFileNotifyInformation->NextEntryOffset;
        DebugAssert(pCurrentPointer < (m_pBuffer + m_bufferSize));
      }
    }

    // re-queue the operation
    QueueReadDirectoryChanges();
  }

  bool QueueReadDirectoryChanges()
  {
    DebugAssert(!m_directoryChangeQueued);

    std::memset(&m_overlapped, 0, sizeof(m_overlapped));
    if (ReadDirectoryChangesW(m_hDirectory, m_pBuffer, m_bufferSize, m_recursiveWatch,
                              READ_DIRECTORY_CHANGES_NOTIFY_FILTER, nullptr, &m_overlapped, nullptr) == FALSE)
      return false;

    m_directoryChangeQueued = true;
    return true;
  }

private:
  HANDLE m_hDirectory;
  OVERLAPPED m_overlapped;
  bool m_directoryChangeQueued;
  byte* m_pBuffer;
  u32 m_bufferSize;
};

std::unique_ptr<ChangeNotifier> CreateChangeNotifier(const char* path, bool recursiveWatch)
{
  // open the directory up
  HANDLE hDirectory = CreateFileA(path, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
  if (hDirectory == nullptr)
    return nullptr;

  // queue up the overlapped io
  auto pChangeNotifier = std::make_unique<ChangeNotifierWin32>(hDirectory, path, recursiveWatch);
  if (!pChangeNotifier->QueueReadDirectoryChanges())
    return nullptr;

  return pChangeNotifier;
}

static u32 RecursiveFindFiles(const char* OriginPath, const char* ParentPath, const char* Path, const char* Pattern,
                              u32 Flags, FileSystem::FindResultsArray* pResults)
{
  std::string tempStr;
  if (Path)
  {
    if (ParentPath)
      tempStr = StringUtil::StdStringFromFormat("%s\\%s\\%s\\*", OriginPath, ParentPath, Path);
    else
      tempStr = StringUtil::StdStringFromFormat("%s\\%s\\*", OriginPath, Path);
  }
  else
  {
    tempStr = StringUtil::StdStringFromFormat("%s\\*", OriginPath);
  }

  WIN32_FIND_DATA wfd;
  HANDLE hFind = FindFirstFileA(tempStr.c_str(), &wfd);
  if (hFind == INVALID_HANDLE_VALUE)
    return 0;

  // small speed optimization for '*' case
  bool hasWildCards = false;
  bool wildCardMatchAll = false;
  u32 nFiles = 0;
  if (std::strpbrk(Pattern, "*?") != nullptr)
  {
    hasWildCards = true;
    wildCardMatchAll = !(std::strcmp(Pattern, "*"));
  }

  // iterate results
  do
  {
    if (wfd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN && !(Flags & FILESYSTEM_FIND_HIDDEN_FILES))
      continue;

    if (wfd.cFileName[0] == '.')
    {
      if (wfd.cFileName[1] == '\0' || (wfd.cFileName[1] == '.' && wfd.cFileName[2] == '\0'))
        continue;

      if (!(Flags & FILESYSTEM_FIND_HIDDEN_FILES))
        continue;
    }

    FILESYSTEM_FIND_DATA outData;
    outData.Attributes = 0;

    if (wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    {
      if (Flags & FILESYSTEM_FIND_RECURSIVE)
      {
        // recurse into this directory
        if (ParentPath != nullptr)
        {
          const std::string recurseDir = StringUtil::StdStringFromFormat("%s\\%s", ParentPath, Path);
          nFiles += RecursiveFindFiles(OriginPath, recurseDir.c_str(), wfd.cFileName, Pattern, Flags, pResults);
        }
        else
        {
          nFiles += RecursiveFindFiles(OriginPath, Path, wfd.cFileName, Pattern, Flags, pResults);
        }
      }

      if (!(Flags & FILESYSTEM_FIND_FOLDERS))
        continue;

      outData.Attributes |= FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY;
    }
    else
    {
      if (!(Flags & FILESYSTEM_FIND_FILES))
        continue;
    }

    if (wfd.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
      outData.Attributes |= FILESYSTEM_FILE_ATTRIBUTE_READ_ONLY;

    // match the filename
    if (hasWildCards)
    {
      if (!wildCardMatchAll && !StringUtil::WildcardMatch(wfd.cFileName, Pattern))
        continue;
    }
    else
    {
      if (std::strcmp(wfd.cFileName, Pattern) != 0)
        continue;
    }

    // add file to list
    // TODO string formatter, clean this mess..
    if (!(Flags & FILESYSTEM_FIND_RELATIVE_PATHS))
    {
      if (ParentPath != nullptr)
        outData.FileName =
          StringUtil::StdStringFromFormat("%s\\%s\\%s\\%s", OriginPath, ParentPath, Path, wfd.cFileName);
      else if (Path != nullptr)
        outData.FileName = StringUtil::StdStringFromFormat("%s\\%s\\%s", OriginPath, Path, wfd.cFileName);
      else
        outData.FileName = StringUtil::StdStringFromFormat("%s\\%s", OriginPath, wfd.cFileName);
    }
    else
    {
      if (ParentPath != nullptr)
        outData.FileName = StringUtil::StdStringFromFormat("%s\\%s\\%s", ParentPath, Path, wfd.cFileName);
      else if (Path != nullptr)
        outData.FileName = StringUtil::StdStringFromFormat("%s\\%s", Path, wfd.cFileName);
      else
        outData.FileName = wfd.cFileName;
    }

    outData.ModificationTime.SetWindowsFileTime(&wfd.ftLastWriteTime);
    outData.Size = (u64)wfd.nFileSizeHigh << 32 | (u64)wfd.nFileSizeLow;

    nFiles++;
    pResults->push_back(std::move(outData));
  } while (FindNextFileA(hFind, &wfd) == TRUE);
  FindClose(hFind);

  return nFiles;
}

bool FileSystem::FindFiles(const char* Path, const char* Pattern, u32 Flags, FindResultsArray* pResults)
{
  // has a path
  if (Path[0] == '\0')
    return false;

  // clear result array
  if (!(Flags & FILESYSTEM_FIND_KEEP_ARRAY))
    pResults->clear();

  // enter the recursive function
  return (RecursiveFindFiles(Path, nullptr, nullptr, Pattern, Flags, pResults) > 0);
}

bool FileSystem::StatFile(const char* Path, FILESYSTEM_STAT_DATA* pStatData)
{
  // has a path
  if (Path[0] == '\0')
    return false;

  // determine attributes for the path. if it's a directory, things have to be handled differently..
  DWORD fileAttributes = GetFileAttributesA(Path);
  if (fileAttributes == INVALID_FILE_ATTRIBUTES)
    return false;

  // test if it is a directory
  HANDLE hFile;
  if (fileAttributes & FILE_ATTRIBUTE_DIRECTORY)
  {
    hFile = CreateFileA(Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  }
  else
  {
    hFile = CreateFileA(Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, 0, nullptr);
  }

  // createfile succeded?
  if (hFile == INVALID_HANDLE_VALUE)
    return false;

  // use GetFileInformationByHandle
  BY_HANDLE_FILE_INFORMATION bhfi;
  if (GetFileInformationByHandle(hFile, &bhfi) == FALSE)
  {
    CloseHandle(hFile);
    return false;
  }

  // close handle
  CloseHandle(hFile);

  // fill in the stat data
  pStatData->Attributes = TranslateWin32Attributes(bhfi.dwFileAttributes);
  pStatData->ModificationTime.SetWindowsFileTime(&bhfi.ftLastWriteTime);
  pStatData->Size = ((u64)bhfi.nFileSizeHigh) << 32 | (u64)bhfi.nFileSizeLow;
  return true;
}

bool FileSystem::FileExists(const char* Path)
{
  // has a path
  if (Path[0] == '\0')
    return false;

  // determine attributes for the path. if it's a directory, things have to be handled differently..
  DWORD fileAttributes = GetFileAttributesA(Path);
  if (fileAttributes == INVALID_FILE_ATTRIBUTES)
    return false;

  if (fileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    return false;
  else
    return true;
}

bool FileSystem::DirectoryExists(const char* Path)
{
  // has a path
  if (Path[0] == '\0')
    return false;

  // determine attributes for the path. if it's a directory, things have to be handled differently..
  DWORD fileAttributes = GetFileAttributesA(Path);
  if (fileAttributes == INVALID_FILE_ATTRIBUTES)
    return false;

  if (fileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    return true;
  else
    return false;
}

bool FileSystem::GetFileName(String& Destination, const char* FileName)
{
  // fastpath for non-existant files
  DWORD fileAttributes = GetFileAttributesA(FileName);
  if (fileAttributes == INVALID_FILE_ATTRIBUTES)
    return false;

  //     // temp buffer for storing string returned by windows
  //     char tempName[MAX_PATH];
  //     DWORD tempNameLength;
  //
  //     // query windows
  //     if ((tempNameLength = GetFullPathNameA(FileName, countof(tempName), tempName, nullptr)) == 0 || tempNameLength
  //     >= countof(tempName))
  //     {
  //         // something went wrong, or buffer overflow
  //         return false;
  //     }
  //
  //     // move it into destination buffer, doesn't matter if it's the same as FileName, as
  //     // we aren't going to use it any more.
  //     DebugAssert(Destination[tempNameLength] == '\0');
  //     Destination = tempName;
  if (Destination.GetWriteableCharArray() != FileName)
    Destination = FileName;

  return true;
}

bool FileSystem::GetFileName(String& FileName)
{
  return GetFileName(FileName, FileName);
}

bool FileSystem::CreateDirectory(const char* Path, bool Recursive)
{
  u32 i;
  DWORD lastError;

  // has a path
  if (Path[0] == '\0')
    return false;

  // try just flat-out, might work if there's no other segments that have to be made
  if (CreateDirectoryA(Path, nullptr))
    return true;

  // check error
  lastError = GetLastError();
  if (lastError == ERROR_ALREADY_EXISTS)
  {
    // check the attributes
    u32 Attributes = GetFileAttributesA(Path);
    if (Attributes != INVALID_FILE_ATTRIBUTES && Attributes & FILE_ATTRIBUTE_DIRECTORY)
      return true;
    else
      return false;
  }
  else if (lastError == ERROR_PATH_NOT_FOUND)
  {
    // part of the path does not exist, so we'll create the parent folders, then
    // the full path again. allocate another buffer with the same length
    u32 pathLength = static_cast<u32>(std::strlen(Path));
    char* tempStr = (char*)alloca(pathLength + 1);

    // create directories along the path
    for (i = 0; i < pathLength; i++)
    {
      if (Path[i] == '\\' || Path[i] == '/')
      {
        tempStr[i] = '\0';
        if (!CreateDirectoryA(tempStr, nullptr))
        {
          lastError = GetLastError();
          if (lastError != ERROR_ALREADY_EXISTS) // fine, continue to next path segment
            return false;
        }
      }

      tempStr[i] = Path[i];
    }

    // re-create the end if it's not a separator, check / as well because windows can interpret them
    if (Path[pathLength - 1] != '\\' && Path[pathLength - 1] != '\\')
    {
      if (!CreateDirectoryA(Path, nullptr))
      {
        lastError = GetLastError();
        if (lastError != ERROR_ALREADY_EXISTS)
          return false;
      }
    }

    // ok
    return true;
  }
  else
  {
    // unhandled error
    return false;
  }
}

bool FileSystem::DeleteFile(const char* Path)
{
  if (Path[0] == '\0')
    return false;

  DWORD fileAttributes = GetFileAttributesA(Path);
  if (fileAttributes == INVALID_FILE_ATTRIBUTES)
    return false;

  if (!(fileAttributes & FILE_ATTRIBUTE_DIRECTORY))
    return (DeleteFileA(Path) == TRUE);
  else
    return false;
}

bool FileSystem::DeleteDirectory(const char* Path, bool Recursive)
{
  // ensure it exists
  DWORD fileAttributes = GetFileAttributesA(Path);
  if (fileAttributes == INVALID_FILE_ATTRIBUTES || !(fileAttributes & FILE_ATTRIBUTE_DIRECTORY))
    return false;

  // non-recursive case just try removing the directory
  if (!Recursive)
    return (RemoveDirectoryA(Path) == TRUE);

  // doing a recursive delete
  SmallString fileName;
  fileName.Format("%s\\*", Path);

  // is there any files?
  WIN32_FIND_DATA findData;
  HANDLE hFind = FindFirstFileA(fileName, &findData);
  if (hFind == INVALID_HANDLE_VALUE)
    return false;

  // search through files
  do
  {
    // skip . and ..
    if (findData.cFileName[0] == '.')
    {
      if ((findData.cFileName[1] == '\0') || (findData.cFileName[1] == '.' && findData.cFileName[2] == '\0'))
      {
        continue;
      }
    }

    // found a directory?
    fileName.Format("%s\\%s", Path, findData.cFileName);
    if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    {
      // recurse into that
      if (!DeleteDirectory(fileName, true))
        return false;
    }
    else
    {
      // found a file, so delete it
      if (!DeleteFileA(fileName))
        return false;
    }
  } while (FindNextFileA(hFind, &findData));
  FindClose(hFind);

  // nuke the directory itself
  if (!RemoveDirectoryA(Path))
    return false;

  // done
  return true;
}

std::string GetProgramPath()
{
  const HANDLE hProcess = GetCurrentProcess();

  std::string buffer;
  buffer.resize(MAX_PATH);

  for (;;)
  {
    DWORD nChars = static_cast<DWORD>(buffer.size());
    if (!QueryFullProcessImageNameA(GetCurrentProcess(), 0, buffer.data(), &nChars) &&
        GetLastError() == ERROR_INSUFFICIENT_BUFFER)
    {
      buffer.resize(buffer.size() * 2);
      continue;
    }

    buffer.resize(nChars);
    break;
  }

  CanonicalizePath(buffer);
  return buffer;
}

std::string GetWorkingDirectory()
{
  DWORD required_size = GetCurrentDirectoryA(0, nullptr);
  if (!required_size)
    return {};

  std::string buffer;
  buffer.resize(required_size - 1);

  if (!GetCurrentDirectoryA(static_cast<DWORD>(buffer.size() + 1), buffer.data()))
    return {};

  return buffer;
}

bool SetWorkingDirectory(const char* path)
{
  return (SetCurrentDirectoryA(path) == TRUE);
}

#else

std::unique_ptr<ChangeNotifier> CreateChangeNotifier(const char* path, bool recursiveWatch)
{
  Log_ErrorPrintf("FileSystem::CreateChangeNotifier(%s) not implemented", path);
  return nullptr;
}

static u32 RecursiveFindFiles(const char* OriginPath, const char* ParentPath, const char* Path, const char* Pattern,
                              u32 Flags, FindResultsArray* pResults)
{
  std::string tempStr;
  if (Path)
  {
    if (ParentPath)
      tempStr = StringUtil::StdStringFromFormat("%s/%s/%s", OriginPath, ParentPath, Path);
    else
      tempStr = StringUtil::StdStringFromFormat("%s/%s", OriginPath, Path);
  }
  else
  {
    tempStr = StringUtil::StdStringFromFormat("%s", OriginPath);
  }

  DIR* pDir = opendir(tempStr.c_str());
  if (pDir == nullptr)
    return 0;

  // small speed optimization for '*' case
  bool hasWildCards = false;
  bool wildCardMatchAll = false;
  u32 nFiles = 0;
  if (std::strpbrk(Pattern, "*?"))
  {
    hasWildCards = true;
    wildCardMatchAll = (std::strcmp(Pattern, "*") == 0);
  }

  // iterate results
  struct dirent* pDirEnt;
  while ((pDirEnt = readdir(pDir)) != nullptr)
  {
    //        if (wfd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN && !(Flags & FILESYSTEM_FIND_HIDDEN_FILES))
    //            continue;
    //
    if (pDirEnt->d_name[0] == '.')
    {
      if (pDirEnt->d_name[1] == '\0' || (pDirEnt->d_name[1] == '.' && pDirEnt->d_name[2] == '\0'))
        continue;

      if (!(Flags & FILESYSTEM_FIND_HIDDEN_FILES))
        continue;
    }

    FILESYSTEM_FIND_DATA outData;
    outData.Attributes = 0;

    if (pDirEnt->d_type == DT_DIR)
    {
      if (Flags & FILESYSTEM_FIND_RECURSIVE)
      {
        // recurse into this directory
        if (ParentPath != nullptr)
        {
          std::string recursiveDir = StringUtil::StdStringFromFormat("%s/%s", ParentPath, Path);
          nFiles += RecursiveFindFiles(OriginPath, recursiveDir.c_str(), pDirEnt->d_name, Pattern, Flags, pResults);
        }
        else
        {
          nFiles += RecursiveFindFiles(OriginPath, Path, pDirEnt->d_name, Pattern, Flags, pResults);
        }
      }

      if (!(Flags & FILESYSTEM_FIND_FOLDERS))
        continue;

      outData.Attributes |= FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY;
    }
    else
    {
      if (!(Flags & FILESYSTEM_FIND_FILES))
        continue;
    }

    //        if (wfd.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
    //            outData.Attributes |= FILESYSTEM_FILE_ATTRIBUTE_READ_ONLY;

    // match the filename
    if (hasWildCards)
    {
      if (!wildCardMatchAll && !StringUtil::WildcardMatch(pDirEnt->d_name, Pattern))
        continue;
    }
    else
    {
      if (std::strcmp(pDirEnt->d_name, Pattern) != 0)
        continue;
    }

    // add file to list
    // TODO string formatter, clean this mess..
    if (!(Flags & FILESYSTEM_FIND_RELATIVE_PATHS))
    {
      if (ParentPath != nullptr)
        outData.FileName =
          StringUtil::StdStringFromFormat("%s/%s/%s/%s", OriginPath, ParentPath, Path, pDirEnt->d_name);
      else if (Path != nullptr)
        outData.FileName = StringUtil::StdStringFromFormat("%s/%s/%s", OriginPath, Path, pDirEnt->d_name);
      else
        outData.FileName = StringUtil::StdStringFromFormat("%s/%s", OriginPath, pDirEnt->d_name);
    }
    else
    {
      if (ParentPath != nullptr)
        outData.FileName = StringUtil::StdStringFromFormat("%s\\%s\\%s", ParentPath, Path, pDirEnt->d_name);
      else if (Path != nullptr)
        outData.FileName = StringUtil::StdStringFromFormat("%s\\%s", Path, pDirEnt->d_name);
      else
        outData.FileName = pDirEnt->d_name;
    }

    nFiles++;
    pResults->push_back(std::move(outData));
  }

  closedir(pDir);
  return nFiles;
}

bool FindFiles(const char* Path, const char* Pattern, u32 Flags, FindResultsArray* pResults)
{
  // has a path
  if (Path[0] == '\0')
    return false;

  // clear result array
  if (!(Flags & FILESYSTEM_FIND_KEEP_ARRAY))
    pResults->clear();

  // enter the recursive function
  return (RecursiveFindFiles(Path, nullptr, nullptr, Pattern, Flags, pResults) > 0);
}

bool StatFile(const char* Path, FILESYSTEM_STAT_DATA* pStatData)
{
  // has a path
  if (Path[0] == '\0')
    return false;

  // stat file
  struct stat64 sysStatData;
  if (stat64(Path, &sysStatData) < 0)
    return false;

  // parse attributes
  pStatData->Attributes = 0;
  if (S_ISDIR(sysStatData.st_mode))
    pStatData->Attributes |= FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY;

  // parse times
  pStatData->ModificationTime.SetUnixTimestamp((Timestamp::UnixTimestampValue)sysStatData.st_mtime);

  // parse size
  if (S_ISREG(sysStatData.st_mode))
    pStatData->Size = static_cast<u64>(sysStatData.st_size);
  else
    pStatData->Size = 0;

  // ok
  return true;
}

bool FileExists(const char* Path)
{
  // has a path
  if (Path[0] == '\0')
    return false;

  // stat file
  struct stat64 sysStatData;
  if (stat64(Path, &sysStatData) < 0)
    return false;

  if (S_ISDIR(sysStatData.st_mode))
    return false;
  else
    return true;
}

bool DirectoryExists(const char* Path)
{
  // has a path
  if (Path[0] == '\0')
    return false;

  // stat file
  struct stat64 sysStatData;
  if (stat64(Path, &sysStatData) < 0)
    return false;

  if (S_ISDIR(sysStatData.st_mode))
    return true;
  else
    return false;
}

bool GetFileName(String& Destination, const char* FileName)
{
  // fastpath for non-existant files
  struct stat sysStatData;
  if (stat(FileName, &sysStatData) < 0)
    return false;

  if (Destination.GetWriteableCharArray() != FileName)
    Destination = FileName;

  return true;
}

bool GetFileName(String& FileName)
{
  return GetFileName(FileName, FileName);
}

bool CreateDirectory(const char* Path, bool Recursive)
{
  u32 i;
  int lastError;

  // has a path
  if (Path[0] == '\0')
    return false;

  // try just flat-out, might work if there's no other segments that have to be made
  if (mkdir(Path, 0777) == 0)
    return true;

  // check error
  lastError = errno;
  if (lastError == EEXIST)
  {
    // check the attributes
    struct stat sysStatData;
    if (stat(Path, &sysStatData) == 0 && S_ISDIR(sysStatData.st_mode))
      return true;
    else
      return false;
  }
  else if (lastError == ENOENT)
  {
    // part of the path does not exist, so we'll create the parent folders, then
    // the full path again. allocate another buffer with the same length
    u32 pathLength = static_cast<u32>(std::strlen(Path));
    char* tempStr = (char*)alloca(pathLength + 1);

    // create directories along the path
    for (i = 0; i < pathLength; i++)
    {
      if (Path[i] == '/')
      {
        tempStr[i] = '\0';
        if (mkdir(tempStr, 0777) < 0)
        {
          lastError = errno;
          if (lastError != EEXIST) // fine, continue to next path segment
            return false;
        }
      }

      tempStr[i] = Path[i];
    }

    // re-create the end if it's not a separator, check / as well because windows can interpret them
    if (Path[pathLength - 1] != '/')
    {
      if (mkdir(Path, 0777) < 0)
      {
        lastError = errno;
        if (lastError != EEXIST)
          return false;
      }
    }

    // ok
    return true;
  }
  else
  {
    // unhandled error
    return false;
  }
}

bool DeleteFile(const char* Path)
{
  if (Path[0] == '\0')
    return false;

  struct stat sysStatData;
  if (stat(Path, &sysStatData) != 0 || S_ISDIR(sysStatData.st_mode))
    return false;

  return (unlink(Path) == 0);
}

bool DeleteDirectory(const char* Path, bool Recursive)
{
  Log_ErrorPrintf("FileSystem::DeleteDirectory(%s) not implemented", Path);
  return false;
}

std::string GetProgramPath()
{
#if defined(__linux__)
  static const char* exeFileName = "/proc/self/exe";

  int curSize = PATH_MAX;
  char* buffer = static_cast<char*>(std::realloc(nullptr, curSize));
  for (;;)
  {
    int len = readlink(exeFileName, buffer, curSize);
    if (len < 0)
    {
      std::free(buffer);
      return {};
    }
    else if (len < curSize)
    {
      buffer[len] = '\0';
      std::string ret(buffer, len);
      std::free(buffer);
      return ret;
    }

    curSize *= 2;
    buffer = static_cast<char*>(std::realloc(buffer, curSize));
  }

#elif defined(__APPLE__)

  int curSize = PATH_MAX;
  char* buffer = static_cast<char*>(std::realloc(nullptr, curSize));
  for (;;)
  {
    u32 nChars = curSize - 1;
    int res = _NSGetExecutablePath(buffer, &nChars);
    if (res == 0)
    {
      buffer[nChars] = 0;

      char* resolvedBuffer = realpath(buffer, nullptr);
      if (resolvedBuffer == nullptr)
      {
        std::free(buffer);
        return {};
      }

      std::string ret(buffer);
      std::free(buffer);
      return ret;
    }

    curSize *= 2;
    buffer = static_cast<char*>(std::realloc(buffer, curSize + 1));
  }

#else
  return {};
#endif
}

std::string GetWorkingDirectory()
{
  std::string buffer;
  buffer.resize(PATH_MAX);
  while (!getcwd(buffer.data(), buffer.size()))
  {
    if (errno != ERANGE)
      return {};

    buffer.resize(buffer.size() * 2);
  }

  return buffer;
}

bool SetWorkingDirectory(const char* path)
{
  return (chdir(path) == 0);
}

#endif

} // namespace FileSystem
