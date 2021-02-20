#include "file_system.h"
#include "assert.h"
#include "byte_stream.h"
#include "log.h"
#include "string_util.h"
#include <algorithm>
#include <cstdlib>
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
#include <limits.h>
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
          // if there was no previous \, skip past the next one
          else if (nextCh != '\0')
            i++;

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
      currentCh = FS_OSPATH_SEPARATOR_CHARACTER;

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

  // if we end up with the empty string, return '.'
  if (destinationLength == 0)
    Destination[destinationLength++] = '.';

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
      c != '_' && c != '-' && c != '.')
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

void SanitizeFileName(std::string& Destination, bool StripSlashes /* = true*/)
{
  const std::size_t len = Destination.length();
  for (std::size_t i = 0; i < len; i++)
  {
    if (!FileSystemCharacterIsSane(Destination[i], StripSlashes))
      Destination[i] = '_';
  }
}

bool IsAbsolutePath(const std::string_view& path)
{
#ifdef WIN32
  return (path.length() >= 3 && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
          path[1] == ':' && (path[2] == '/' || path[2] == '\\'));
#else
  return (path.length() >= 1 && path[0] == '/');
#endif
}

std::string ReplaceExtension(const std::string_view& path, const std::string_view& new_extension)
{
  std::string_view::size_type pos = path.rfind('.');
  if (pos == std::string::npos)
    return std::string(path);

  std::string ret(path, 0, pos + 1);
  ret.append(new_extension);
  return ret;
}

std::string_view GetPathDirectory(const std::string_view& path)
{
#ifdef _WIN32
  std::string::size_type pos = path.find_last_of("/\\");
#else
  std::string::size_type pos = path.find_last_of("/");
#endif
  if (pos == std::string_view::npos)
    return {};

  return path.substr(0, pos);
}

std::string_view GetFileNameFromPath(const std::string_view& path)
{
#ifdef _WIN32
  std::string::size_type pos = path.find_last_of("/\\");
#else
  std::string::size_type pos = path.find_last_of("/");
#endif
  if (pos == std::string_view::npos)
    return path;

  return path.substr(pos + 1);
}

std::string_view GetFileTitleFromPath(const std::string_view& path)
{
  std::string_view filename(GetFileNameFromPath(path));
  std::string::size_type pos = filename.rfind('.');
  if (pos == std::string_view::npos)
    return filename;

  return filename.substr(0, pos);
}

std::vector<std::string> GetRootDirectoryList()
{
  std::vector<std::string> results;

#ifdef WIN32
  char buf[256];
  if (GetLogicalDriveStringsA(sizeof(buf), buf) != 0)
  {
    const char* ptr = buf;
    while (*ptr != '\0')
    {
      const std::size_t len = std::strlen(ptr);
      results.emplace_back(ptr, len);
      ptr += len + 1u;
    }
  }
#else
  const char* home_path = std::getenv("HOME");
  if (home_path)
    results.push_back(home_path);

  results.push_back("/");
#endif

  return results;
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

String BuildPathRelativeToFile(const char* CurrentFileName, const char* NewFileName, bool OSPath /*= true*/,
                               bool Canonicalize /*= true*/)
{
  String ret;
  BuildPathRelativeToFile(ret, CurrentFileName, NewFileName, OSPath, Canonicalize);
  return ret;
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
  int filename_len = static_cast<int>(std::strlen(filename));
  int mode_len = static_cast<int>(std::strlen(mode));
  int wlen = MultiByteToWideChar(CP_UTF8, 0, filename, filename_len, nullptr, 0);
  int wmodelen = MultiByteToWideChar(CP_UTF8, 0, mode, mode_len, nullptr, 0);
  if (wlen > 0 && wmodelen > 0)
  {
    wchar_t* wfilename = static_cast<wchar_t*>(alloca(sizeof(wchar_t) * (wlen + 1)));
    wchar_t* wmode = static_cast<wchar_t*>(alloca(sizeof(wchar_t) * (wmodelen + 1)));
    wlen = MultiByteToWideChar(CP_UTF8, 0, filename, filename_len, wfilename, wlen);
    wmodelen = MultiByteToWideChar(CP_UTF8, 0, mode, mode_len, wmode, wmodelen);
    if (wlen > 0 && wmodelen > 0)
    {
      wfilename[wlen] = 0;
      wmode[wmodelen] = 0;
      std::FILE* fp;
      if (_wfopen_s(&fp, wfilename, wmode) != 0)
        return nullptr;

      return fp;
    }
  }

  std::FILE* fp;
  if (fopen_s(&fp, filename, mode) != 0)
    return nullptr;

  return fp;
#else
  return std::fopen(filename, mode);
#endif
}

std::optional<std::vector<u8>> ReadBinaryFile(const char* filename)
{
  ManagedCFilePtr fp = OpenManagedCFile(filename, "rb");
  if (!fp)
    return std::nullopt;

  return ReadBinaryFile(fp.get());
}

std::optional<std::vector<u8>> ReadBinaryFile(std::FILE* fp)
{
  std::fseek(fp, 0, SEEK_END);
  long size = std::ftell(fp);
  std::fseek(fp, 0, SEEK_SET);
  if (size < 0)
    return std::nullopt;

  std::vector<u8> res(static_cast<size_t>(size));
  if (size > 0 && std::fread(res.data(), 1u, static_cast<size_t>(size), fp) != static_cast<size_t>(size))
    return std::nullopt;

  return res;
}

std::optional<std::string> ReadFileToString(const char* filename)
{
  ManagedCFilePtr fp = OpenManagedCFile(filename, "rb");
  if (!fp)
    return std::nullopt;

  return ReadFileToString(fp.get());
}

std::optional<std::string> ReadFileToString(std::FILE* fp)
{
  std::fseek(fp, 0, SEEK_END);
  long size = std::ftell(fp);
  std::fseek(fp, 0, SEEK_SET);
  if (size < 0)
    return std::nullopt;

  std::string res;
  res.resize(static_cast<size_t>(size));
  if (size > 0 && std::fread(res.data(), 1u, static_cast<size_t>(size), fp) != static_cast<size_t>(size))
    return std::nullopt;

  return res;
}

bool WriteBinaryFile(const char* filename, const void* data, size_t data_length)
{
  ManagedCFilePtr fp = OpenManagedCFile(filename, "wb");
  if (!fp)
    return false;

  if (data_length > 0 && std::fwrite(data, 1u, data_length, fp.get()) != data_length)
    return false;

  return true;
}

bool WriteFileToString(const char* filename, const std::string_view& sv)
{
  ManagedCFilePtr fp = OpenManagedCFile(filename, "wb");
  if (!fp)
    return false;

  if (sv.length() > 0 && std::fwrite(sv.data(), 1u, sv.length(), fp.get()) != sv.length())
    return false;

  return true;
}

std::string ReadStreamToString(ByteStream* stream, bool seek_to_start /* = true */)
{
  u64 pos = stream->GetPosition();
  u64 size = stream->GetSize();
  if (pos > 0 && seek_to_start)
  {
    if (!stream->SeekAbsolute(0))
      return {};

    pos = 0;
  }

  Assert(size >= pos);
  size -= pos;
  if (size == 0 || size > std::numeric_limits<u32>::max())
    return {};

  std::string ret;
  ret.resize(static_cast<size_t>(size));
  if (!stream->Read2(ret.data(), static_cast<u32>(size)))
    return {};

  return ret;
}

bool WriteStreamToString(const std::string_view& sv, ByteStream* stream)
{
  if (sv.size() > std::numeric_limits<u32>::max())
    return false;

  return stream->Write2(sv.data(), static_cast<u32>(sv.size()));
}

std::vector<u8> ReadBinaryStream(ByteStream* stream, bool seek_to_start /*= true*/)
{
  u64 pos = stream->GetPosition();
  u64 size = stream->GetSize();
  if (pos > 0 && seek_to_start)
  {
    if (!stream->SeekAbsolute(0))
      return {};

    pos = 0;
  }

  Assert(size >= pos);
  size -= pos;
  if (size == 0 || size > std::numeric_limits<u32>::max())
    return {};

  std::vector<u8> ret;
  ret.resize(static_cast<size_t>(size));
  if (!stream->Read2(ret.data(), static_cast<u32>(size)))
    return {};

  return ret;
}

bool WriteBinaryToSTream(ByteStream* stream, const void* data, size_t data_length)
{
  if (data_length > std::numeric_limits<u32>::max())
    return false;

  return stream->Write2(data, static_cast<u32>(data_length));
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
        Destination[i] = FS_OSPATH_SEPARATOR_CHARACTER;
    }
  }
  else
  {
    // slow path
    pathLength = std::max(pathLength, cbDestination - 1);
    for (i = 0; i < pathLength; i++)
    {
      Destination[i] = (Path[i] == '/') ? FS_OSPATH_SEPARATOR_CHARACTER : Path[i];
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
        Destination[i] = FS_OSPATH_SEPARATOR_CHARACTER;
    }
  }
  else
  {
    // slow path
    pathLength = static_cast<u32>(std::strlen(Path));
    Destination.Resize(pathLength);
    for (i = 0; i < pathLength; i++)
    {
      Destination[i] = (Path[i] == '/') ? FS_OSPATH_SEPARATOR_CHARACTER : Path[i];
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

  WIN32_FIND_DATAW wfd;
  HANDLE hFind = FindFirstFileW(StringUtil::UTF8StringToWideString(tempStr).c_str(), &wfd);
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

  // holder for utf-8 conversion
  std::string utf8_filename;
  utf8_filename.reserve(countof(wfd.cFileName) * 2);

  // iterate results
  do
  {
    if (wfd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN && !(Flags & FILESYSTEM_FIND_HIDDEN_FILES))
      continue;

    if (wfd.cFileName[0] == L'.')
    {
      if (wfd.cFileName[1] == L'\0' || (wfd.cFileName[1] == L'.' && wfd.cFileName[2] == L'\0'))
        continue;

      if (!(Flags & FILESYSTEM_FIND_HIDDEN_FILES))
        continue;
    }

    if (!StringUtil::WideStringToUTF8String(utf8_filename, wfd.cFileName))
      continue;

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
          nFiles += RecursiveFindFiles(OriginPath, recurseDir.c_str(), utf8_filename.c_str(), Pattern, Flags, pResults);
        }
        else
        {
          nFiles += RecursiveFindFiles(OriginPath, Path, utf8_filename.c_str(), Pattern, Flags, pResults);
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
      if (!wildCardMatchAll && !StringUtil::WildcardMatch(utf8_filename.c_str(), Pattern))
        continue;
    }
    else
    {
      if (std::strcmp(utf8_filename.c_str(), Pattern) != 0)
        continue;
    }

    // add file to list
    // TODO string formatter, clean this mess..
    if (!(Flags & FILESYSTEM_FIND_RELATIVE_PATHS))
    {
      if (ParentPath != nullptr)
        outData.FileName =
          StringUtil::StdStringFromFormat("%s\\%s\\%s\\%s", OriginPath, ParentPath, Path, utf8_filename.c_str());
      else if (Path != nullptr)
        outData.FileName = StringUtil::StdStringFromFormat("%s\\%s\\%s", OriginPath, Path, utf8_filename.c_str());
      else
        outData.FileName = StringUtil::StdStringFromFormat("%s\\%s", OriginPath, utf8_filename.c_str());
    }
    else
    {
      if (ParentPath != nullptr)
        outData.FileName = StringUtil::StdStringFromFormat("%s\\%s\\%s", ParentPath, Path, utf8_filename.c_str());
      else if (Path != nullptr)
        outData.FileName = StringUtil::StdStringFromFormat("%s\\%s", Path, utf8_filename.c_str());
      else
        outData.FileName = utf8_filename;
    }

    outData.ModificationTime.SetWindowsFileTime(&wfd.ftLastWriteTime);
    outData.Size = (u64)wfd.nFileSizeHigh << 32 | (u64)wfd.nFileSizeLow;

    nFiles++;
    pResults->push_back(std::move(outData));
  } while (FindNextFileW(hFind, &wfd) == TRUE);
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

bool FileSystem::StatFile(const char* path, FILESYSTEM_STAT_DATA* pStatData)
{
  // has a path
  if (path[0] == '\0')
    return false;

  // convert to wide string
  int len = static_cast<int>(std::strlen(path));
  int wlen = MultiByteToWideChar(CP_UTF8, 0, path, len, nullptr, 0);
  if (wlen <= 0)
    return false;

  wchar_t* wpath = static_cast<wchar_t*>(alloca(sizeof(wchar_t) * (wlen + 1)));
  wlen = MultiByteToWideChar(CP_UTF8, 0, path, len, wpath, wlen);
  if (wlen <= 0)
    return false;

  wpath[wlen] = 0;

  // determine attributes for the path. if it's a directory, things have to be handled differently..
  DWORD fileAttributes = GetFileAttributesW(wpath);
  if (fileAttributes == INVALID_FILE_ATTRIBUTES)
    return false;

  // test if it is a directory
  HANDLE hFile;
  if (fileAttributes & FILE_ATTRIBUTE_DIRECTORY)
  {
    hFile = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  }
  else
  {
    hFile = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
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

bool FileSystem::StatFile(std::FILE* fp, FILESYSTEM_STAT_DATA* pStatData)
{
  const int fd = _fileno(fp);
  if (fd < 0)
    return false;

  struct _stat64 st;
  if (_fstati64(fd, &st) != 0)
    return false;

  // parse attributes
  pStatData->Attributes = 0;
  if ((st.st_mode & _S_IFMT) == _S_IFDIR)
    pStatData->Attributes |= FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY;

  // parse times
  pStatData->ModificationTime.SetUnixTimestamp((Timestamp::UnixTimestampValue)st.st_mtime);

  // parse size
  if ((st.st_mode & _S_IFMT) == _S_IFREG)
    pStatData->Size = static_cast<u64>(st.st_size);
  else
    pStatData->Size = 0;

  return true;
}

bool FileSystem::FileExists(const char* path)
{
  // has a path
  if (path[0] == '\0')
    return false;

  // convert to wide string
  int len = static_cast<int>(std::strlen(path));
  int wlen = MultiByteToWideChar(CP_UTF8, 0, path, len, nullptr, 0);
  if (wlen <= 0)
    return false;

  wchar_t* wpath = static_cast<wchar_t*>(alloca(sizeof(wchar_t) * (wlen + 1)));
  wlen = MultiByteToWideChar(CP_UTF8, 0, path, len, wpath, wlen);
  if (wlen <= 0)
    return false;

  wpath[wlen] = 0;

  // determine attributes for the path. if it's a directory, things have to be handled differently..
  DWORD fileAttributes = GetFileAttributesW(wpath);
  if (fileAttributes == INVALID_FILE_ATTRIBUTES)
    return false;

  if (fileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    return false;
  else
    return true;
}

bool FileSystem::DirectoryExists(const char* path)
{
  // has a path
  if (path[0] == '\0')
    return false;

  // convert to wide string
  int len = static_cast<int>(std::strlen(path));
  int wlen = MultiByteToWideChar(CP_UTF8, 0, path, len, nullptr, 0);
  if (wlen <= 0)
    return false;

  wchar_t* wpath = static_cast<wchar_t*>(alloca(sizeof(wchar_t) * (wlen + 1)));
  wlen = MultiByteToWideChar(CP_UTF8, 0, path, len, wpath, wlen);
  if (wlen <= 0)
    return false;

  wpath[wlen] = 0;

  // determine attributes for the path. if it's a directory, things have to be handled differently..
  DWORD fileAttributes = GetFileAttributesW(wpath);
  if (fileAttributes == INVALID_FILE_ATTRIBUTES)
    return false;

  if (fileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    return true;
  else
    return false;
}

bool FileSystem::CreateDirectory(const char* Path, bool Recursive)
{
  std::wstring wpath(StringUtil::UTF8StringToWideString(Path));

  // has a path
  if (wpath[0] == L'\0')
    return false;

  // try just flat-out, might work if there's no other segments that have to be made
  if (CreateDirectoryW(wpath.c_str(), nullptr))
    return true;

  // check error
  DWORD lastError = GetLastError();
  if (lastError == ERROR_ALREADY_EXISTS)
  {
    // check the attributes
    u32 Attributes = GetFileAttributesW(wpath.c_str());
    if (Attributes != INVALID_FILE_ATTRIBUTES && Attributes & FILE_ATTRIBUTE_DIRECTORY)
      return true;
    else
      return false;
  }
  else if (lastError == ERROR_PATH_NOT_FOUND)
  {
    // part of the path does not exist, so we'll create the parent folders, then
    // the full path again. allocate another buffer with the same length
    u32 pathLength = static_cast<u32>(wpath.size());
    wchar_t* tempStr = (wchar_t*)alloca(sizeof(wchar_t) * (pathLength + 1));

    // create directories along the path
    for (u32 i = 0; i < pathLength; i++)
    {
      if (wpath[i] == L'\\' || wpath[i] == L'/')
      {
        tempStr[i] = L'\0';
        if (!CreateDirectoryW(tempStr, nullptr))
        {
          lastError = GetLastError();
          if (lastError != ERROR_ALREADY_EXISTS) // fine, continue to next path segment
            return false;
        }
      }

      tempStr[i] = wpath[i];
    }

    // re-create the end if it's not a separator, check / as well because windows can interpret them
    if (wpath[pathLength - 1] != L'\\' && wpath[pathLength - 1] != L'/')
    {
      if (!CreateDirectoryW(wpath.c_str(), nullptr))
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

  const std::wstring wpath(StringUtil::UTF8StringToWideString(Path));
  DWORD fileAttributes = GetFileAttributesW(wpath.c_str());
  if (fileAttributes == INVALID_FILE_ATTRIBUTES)
    return false;

  if (!(fileAttributes & FILE_ATTRIBUTE_DIRECTORY))
    return (DeleteFileW(wpath.c_str()) == TRUE);
  else
    return false;
}

static bool RecursiveDeleteDirectory(const std::wstring& wpath, bool Recursive)
{
  // ensure it exists
  DWORD fileAttributes = GetFileAttributesW(wpath.c_str());
  if (fileAttributes == INVALID_FILE_ATTRIBUTES || !(fileAttributes & FILE_ATTRIBUTE_DIRECTORY))
    return false;

  // non-recursive case just try removing the directory
  if (!Recursive)
    return (RemoveDirectoryW(wpath.c_str()) == TRUE);

  // doing a recursive delete
  std::wstring fileName = wpath;
  fileName += L"\\*";

  // is there any files?
  WIN32_FIND_DATAW findData;
  HANDLE hFind = FindFirstFileW(fileName.c_str(), &findData);
  if (hFind == INVALID_HANDLE_VALUE)
    return false;

  // search through files
  do
  {
    // skip . and ..
    if (findData.cFileName[0] == L'.')
    {
      if ((findData.cFileName[1] == L'\0') || (findData.cFileName[1] == L'.' && findData.cFileName[2] == L'\0'))
      {
        continue;
      }
    }

    // found a directory?
    fileName = wpath;
    fileName += L"\\";
    fileName += findData.cFileName;
    if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    {
      // recurse into that
      if (!RecursiveDeleteDirectory(fileName, true))
      {
        FindClose(hFind);
        return false;
      }
    }
    else
    {
      // found a file, so delete it
      if (!DeleteFileW(fileName.c_str()))
      {
        FindClose(hFind);
        return false;
      }
    }
  } while (FindNextFileW(hFind, &findData));
  FindClose(hFind);

  // nuke the directory itself
  if (!RemoveDirectoryW(wpath.c_str()))
    return false;

  // done
  return true;
}

bool FileSystem::DeleteDirectory(const char* Path, bool Recursive)
{
  const std::wstring wpath(StringUtil::UTF8StringToWideString(Path));
  return RecursiveDeleteDirectory(wpath, Recursive);
}

std::string GetProgramPath()
{
  std::wstring buffer;
  buffer.resize(MAX_PATH);

  // Fall back to the main module if this fails.
  HMODULE module = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&GetProgramPath), &module);

  for (;;)
  {
    DWORD nChars = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (nChars == static_cast<DWORD>(buffer.size()) && GetLastError() == ERROR_INSUFFICIENT_BUFFER)
    {
      buffer.resize(buffer.size() * 2);
      continue;
    }

    buffer.resize(nChars);
    break;
  }

  std::string utf8_path(StringUtil::WideStringToUTF8String(buffer));
  CanonicalizePath(utf8_path);
  return utf8_path;
}

std::string GetWorkingDirectory()
{
  DWORD required_size = GetCurrentDirectoryW(0, nullptr);
  if (!required_size)
    return {};

  std::wstring buffer;
  buffer.resize(required_size - 1);

  if (!GetCurrentDirectoryW(static_cast<DWORD>(buffer.size() + 1), buffer.data()))
    return {};

  return StringUtil::WideStringToUTF8String(buffer);
}

bool SetWorkingDirectory(const char* path)
{
  const std::wstring wpath(StringUtil::UTF8StringToWideString(path));
  return (SetCurrentDirectoryW(wpath.c_str()) == TRUE);
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
  PathString full_path;
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

    if (ParentPath != nullptr)
      full_path.Format("%s/%s/%s/%s", OriginPath, ParentPath, Path, pDirEnt->d_name);
    else if (Path != nullptr)
      full_path.Format("%s/%s/%s", OriginPath, Path, pDirEnt->d_name);
    else
      full_path.Format("%s/%s", OriginPath, pDirEnt->d_name);

    FILESYSTEM_FIND_DATA outData;
    outData.Attributes = 0;

#if defined(__HAIKU__) || defined(__APPLE__)
    struct stat sDir;
    if (stat(full_path, &sDir) < 0)
      continue;

#else
    struct stat64 sDir;
    if (stat64(full_path, &sDir) < 0)
      continue;
#endif

    if (S_ISDIR(sDir.st_mode))
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

    outData.Size = static_cast<u64>(sDir.st_size);
    outData.ModificationTime.SetUnixTimestamp(static_cast<Timestamp::UnixTimestampValue>(sDir.st_mtime));

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
      outData.FileName = std::string(full_path.GetCharArray());
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
#if defined(__HAIKU__) || defined(__APPLE__)
  struct stat sysStatData;
  if (stat(Path, &sysStatData) < 0)
#else
  struct stat64 sysStatData;
  if (stat64(Path, &sysStatData) < 0)
#endif
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

bool StatFile(std::FILE* fp, FILESYSTEM_STAT_DATA* pStatData)
{
  int fd = fileno(fp);
  if (fd < 0)
    return false;

    // stat file
#if defined(__HAIKU__) || defined(__APPLE__)
  struct stat sysStatData;
  if (fstat(fd, &sysStatData) < 0)
#else
  struct stat64 sysStatData;
  if (fstat64(fd, &sysStatData) < 0)
#endif
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
#if defined(__HAIKU__) || defined(__APPLE__)
  struct stat sysStatData;
  if (stat(Path, &sysStatData) < 0)
#else
  struct stat64 sysStatData;
  if (stat64(Path, &sysStatData) < 0)
#endif
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
#if defined(__HAIKU__) || defined(__APPLE__)
  struct stat sysStatData;
  if (stat(Path, &sysStatData) < 0)
#else
  struct stat64 sysStatData;
  if (stat64(Path, &sysStatData) < 0)
#endif
    return false;

  if (S_ISDIR(sysStatData.st_mode))
    return true;
  else
    return false;
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
