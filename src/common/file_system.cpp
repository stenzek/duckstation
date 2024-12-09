// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "file_system.h"
#include "assert.h"
#include "error.h"
#include "log.h"
#include "path.h"
#include "string_util.h"
#include "timer.h"

#include "fmt/format.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <numeric>
#include <utility>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <stdlib.h>
#include <sys/param.h>
#endif

#ifdef __FreeBSD__
#include <sys/sysctl.h>
#endif

#if defined(_WIN32)
#include "windows_headers.h"
#include <io.h>
#include <malloc.h>
#include <pathcch.h>
#include <share.h>
#include <shlobj.h>
#include <winioctl.h>
#else
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

LOG_CHANNEL(FileSystem);

#ifdef _WIN32
static std::time_t ConvertFileTimeToUnixTime(const FILETIME& ft)
{
  // based off https://stackoverflow.com/a/6161842
  static constexpr s64 WINDOWS_TICK = 10000000;
  static constexpr s64 SEC_TO_UNIX_EPOCH = 11644473600LL;

  const s64 full = static_cast<s64>((static_cast<u64>(ft.dwHighDateTime) << 32) | static_cast<u64>(ft.dwLowDateTime));
  return static_cast<std::time_t>(full / WINDOWS_TICK - SEC_TO_UNIX_EPOCH);
}
template<class T>
static bool IsUNCPath(const T& path)
{
  return (path.length() >= 3 && path[0] == '\\' && path[1] == '\\');
}
#endif

static inline bool FileSystemCharacterIsSane(char32_t c, bool strip_slashes)
{
#ifdef _WIN32
  // https://docs.microsoft.com/en-gb/windows/win32/fileio/naming-a-file?redirectedfrom=MSDN#naming-conventions
  if ((c == U'/' || c == U'\\') && strip_slashes)
    return false;

  if (c == U'<' || c == U'>' || c == U':' || c == U'"' || c == U'|' || c == U'?' || c == U'*' || c == 0 ||
      c <= static_cast<char32_t>(31))
  {
    return false;
  }
#else
  if (c == '/' && strip_slashes)
    return false;

  // drop asterisks too, they make globbing annoying
  if (c == '*')
    return false;

    // macos doesn't allow colons, apparently
#ifdef __APPLE__
  if (c == U':')
    return false;
#endif
#endif

  return true;
}

std::string Path::SanitizeFileName(std::string_view str, bool strip_slashes /* = true */)
{
  std::string ret;
  ret.reserve(str.length());

  size_t pos = 0;
  while (pos < str.length())
  {
    char32_t ch;
    pos += StringUtil::DecodeUTF8(str, pos, &ch);
    ch = FileSystemCharacterIsSane(ch, strip_slashes) ? ch : U'_';
    StringUtil::EncodeAndAppendUTF8(ret, ch);
  }

#ifdef _WIN32
  // Windows: Can't end filename with a period.
  if (ret.length() > 0 && ret.back() == '.')
    ret.back() = '_';
#endif

  return ret;
}

void Path::SanitizeFileName(std::string* str, bool strip_slashes /* = true */)
{
  const size_t len = str->length();

  char small_buf[128];
  std::unique_ptr<char[]> large_buf;
  char* str_copy = small_buf;
  if (len >= std::size(small_buf))
  {
    large_buf = std::make_unique<char[]>(len + 1);
    str_copy = large_buf.get();
  }
  std::memcpy(str_copy, str->c_str(), sizeof(char) * (len + 1));
  str->clear();

  size_t pos = 0;
  while (pos < len)
  {
    char32_t ch;
    pos += StringUtil::DecodeUTF8(str_copy + pos, pos - len, &ch);
    ch = FileSystemCharacterIsSane(ch, strip_slashes) ? ch : U'_';
    StringUtil::EncodeAndAppendUTF8(*str, ch);
  }

#ifdef _WIN32
  // Windows: Can't end filename with a period.
  if (str->length() > 0 && str->back() == '.')
    str->back() = '_';
#endif
}

std::string Path::RemoveLengthLimits(std::string_view str)
{
  std::string ret;
#ifdef _WIN32
  ret.reserve(str.length() + (IsUNCPath(str) ? 4 : 6));
#endif
  ret.append(str);
  RemoveLengthLimits(&ret);
  return ret;
}

void Path::RemoveLengthLimits(std::string* path)
{
  DebugAssert(IsAbsolute(*path));
#ifdef _WIN32
  // Any forward slashes should be backslashes.
  for (char& ch : *path)
    ch = (ch == '/') ? '\\' : ch;

  if (IsUNCPath(*path))
  {
    // \\server\path => \\?\UNC\server\path
    DebugAssert((*path)[0] == '\\' && (*path)[1] == '\\');
    path->erase(0, 2);
    path->insert(0, "\\\\?\\UNC\\");
  }
  else
  {
    // C:\file => \\?\C:\file
    path->insert(0, "\\\\?\\");
  }
#endif
}

#ifdef _WIN32

bool FileSystem::GetWin32Path(std::wstring* dest, std::string_view str)
{
  // Just convert to wide if it's a relative path, MAX_PATH still applies.
  if (!Path::IsAbsolute(str))
    return StringUtil::UTF8StringToWideString(*dest, str);

  // PathCchCanonicalizeEx() thankfully takes care of everything.
  // But need to widen the string first, avoid the stack allocation.
  int wlen = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.length()), nullptr, 0);
  if (wlen <= 0) [[unlikely]]
    return false;

  // So copy it to a temp wide buffer first.
  wchar_t* wstr_buf = static_cast<wchar_t*>(_malloca(sizeof(wchar_t) * (static_cast<size_t>(wlen) + 1)));
  wlen = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.length()), wstr_buf, wlen);
  if (wlen <= 0) [[unlikely]]
  {
    _freea(wstr_buf);
    return false;
  }

  // And use PathCchCanonicalizeEx() to fix up any non-direct elements.
  wstr_buf[wlen] = '\0';
  dest->resize(std::max<size_t>(static_cast<size_t>(wlen) + (IsUNCPath(str) ? 9 : 5), 16));
  for (;;)
  {
    const HRESULT hr =
      PathCchCanonicalizeEx(dest->data(), dest->size(), wstr_buf, PATHCCH_ENSURE_IS_EXTENDED_LENGTH_PATH);
    if (SUCCEEDED(hr))
    {
      dest->resize(std::wcslen(dest->data()));
      _freea(wstr_buf);
      return true;
    }
    else if (hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER))
    {
      dest->resize(dest->size() * 2);
      continue;
    }
    else [[unlikely]]
    {
      ERROR_LOG("PathCchCanonicalizeEx() returned {:08X}", static_cast<unsigned>(hr));
      _freea(wstr_buf);
      return false;
    }
  }
}

std::wstring FileSystem::GetWin32Path(std::string_view str)
{
  std::wstring ret;
  if (!GetWin32Path(&ret, str))
    ret.clear();
  return ret;
}

#endif

#ifndef __ANDROID__

template<typename T>
static inline void PathAppendString(std::string& dst, const T& src)
{
  if (dst.capacity() < (dst.length() + src.length()))
    dst.reserve(dst.length() + src.length());

  bool last_separator = (!dst.empty() && dst.back() == FS_OSPATH_SEPARATOR_CHARACTER);

  size_t index = 0;

#ifdef _WIN32
  // special case for UNC paths here
  if (dst.empty() && IsUNCPath(src))
  {
    dst.append("\\\\");
    index = 2;
  }
#endif

  for (; index < src.length(); index++)
  {
    const char ch = src[index];

#ifdef _WIN32
    // convert forward slashes to backslashes
    if (ch == '\\' || ch == '/')
#else
    if (ch == '/')
#endif
    {
      if (last_separator)
        continue;
      last_separator = true;
      dst.push_back(FS_OSPATH_SEPARATOR_CHARACTER);
    }
    else
    {
      last_separator = false;
      dst.push_back(ch);
    }
  }
}

bool Path::IsAbsolute(std::string_view path)
{
#ifdef _WIN32
  return (path.length() >= 3 && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
          path[1] == ':' && (path[2] == '/' || path[2] == '\\')) ||
         IsUNCPath(path);
#else
  return (path.length() >= 1 && path[0] == '/');
#endif
}

std::string Path::RealPath(std::string_view path)
{
  // Resolve non-absolute paths first.
  std::vector<std::string_view> components;
  if (!IsAbsolute(path))
    components = Path::SplitNativePath(Path::Combine(FileSystem::GetWorkingDirectory(), path));
  else
    components = Path::SplitNativePath(path);

  std::string realpath;
  if (components.empty())
    return realpath;

  // Different to path because relative.
  realpath.reserve(std::accumulate(components.begin(), components.end(), static_cast<size_t>(0),
                                   [](size_t l, const std::string_view& s) { return l + s.length(); }) +
                   components.size() + 1);

#ifdef _WIN32
  std::wstring wrealpath;
  std::vector<WCHAR> symlink_buf;
  wrealpath.reserve(realpath.size());
  symlink_buf.resize(path.size() + 1);

  // Check for any symbolic links throughout the path while adding components.
  const bool skip_first = IsUNCPath(path);
  bool test_symlink = true;
  for (const std::string_view& comp : components)
  {
    if (!realpath.empty())
    {
      realpath.push_back(FS_OSPATH_SEPARATOR_CHARACTER);
      realpath.append(comp);
    }
    else if (skip_first)
    {
      realpath.append(comp);
      continue;
    }
    else
    {
      realpath.append(comp);
    }
    if (test_symlink)
    {
      DWORD attribs;
      if (FileSystem::GetWin32Path(&wrealpath, realpath) &&
          (attribs = GetFileAttributesW(wrealpath.c_str())) != INVALID_FILE_ATTRIBUTES)
      {
        // if not a link, go to the next component
        if (attribs & FILE_ATTRIBUTE_REPARSE_POINT)
        {
          const HANDLE hFile =
            CreateFileW(wrealpath.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
          if (hFile != INVALID_HANDLE_VALUE)
          {
            // is a link! resolve it.
            DWORD ret = GetFinalPathNameByHandleW(hFile, symlink_buf.data(), static_cast<DWORD>(symlink_buf.size()),
                                                  FILE_NAME_NORMALIZED);
            if (ret > symlink_buf.size())
            {
              symlink_buf.resize(ret);
              ret = GetFinalPathNameByHandleW(hFile, symlink_buf.data(), static_cast<DWORD>(symlink_buf.size()),
                                              FILE_NAME_NORMALIZED);
            }
            if (ret != 0)
              StringUtil::WideStringToUTF8String(realpath, std::wstring_view(symlink_buf.data(), ret));
            else
              test_symlink = false;

            CloseHandle(hFile);
          }
        }
      }
      else
      {
        // not a file or link
        test_symlink = false;
      }
    }
  }

  // GetFinalPathNameByHandleW() adds a \\?\ prefix, so remove it.
  if (realpath.starts_with("\\\\?\\") && IsAbsolute(std::string_view(realpath.data() + 4, realpath.size() - 4)))
  {
    realpath.erase(0, 4);
  }
  else if (realpath.starts_with("\\\\?\\UNC\\"))
  {
    realpath.erase(0, 7);
    realpath.insert(realpath.begin(), '\\');
  }

#else
  // Why this monstrosity instead of calling realpath()? realpath() only works on files that exist.
  std::string basepath;
  std::string symlink;

  basepath.reserve(realpath.capacity());
  symlink.resize(realpath.capacity());

  // Check for any symbolic links throughout the path while adding components.
  bool test_symlink = true;
  for (const std::string_view& comp : components)
  {
    if (!test_symlink)
    {
      realpath.push_back(FS_OSPATH_SEPARATOR_CHARACTER);
      realpath.append(comp);
      continue;
    }

    basepath = realpath;
    if (realpath.empty() || realpath.back() != FS_OSPATH_SEPARATOR_CHARACTER)
      realpath.push_back(FS_OSPATH_SEPARATOR_CHARACTER);
    realpath.append(comp);

    // Check if the last component added is a symlink
    struct stat sb;
    if (lstat(realpath.c_str(), &sb) != 0)
    {
      // Don't bother checking any further components once we error out.
      test_symlink = false;
      continue;
    }
    else if (!S_ISLNK(sb.st_mode))
    {
      // Nope, keep going.
      continue;
    }

    for (;;)
    {
      ssize_t sz = readlink(realpath.c_str(), symlink.data(), symlink.size());
      if (sz < 0)
      {
        // shouldn't happen, due to the S_ISLNK check above.
        test_symlink = false;
        break;
      }
      else if (static_cast<size_t>(sz) == symlink.size())
      {
        // need a larger buffer
        symlink.resize(symlink.size() * 2);
        continue;
      }
      else
      {
        // is a link, and we resolved it. gotta check if the symlink itself is relative :(
        symlink.resize(static_cast<size_t>(sz));
        if (!Path::IsAbsolute(symlink))
        {
          // symlink is relative to the directory of the symlink
          realpath = basepath;
          if (realpath.empty() || realpath.back() != FS_OSPATH_SEPARATOR_CHARACTER)
            realpath.push_back(FS_OSPATH_SEPARATOR_CHARACTER);
          realpath.append(symlink);
        }
        else
        {
          // Use the new, symlinked path.
          realpath = symlink;
        }

        break;
      }
    }
  }
#endif

  // Get rid of any current/parent directory components before returning.
  // This should be fine on Linux, since any symbolic links have already replaced the leading portion.
  Path::Canonicalize(&realpath);

  return realpath;
}

std::string Path::ToNativePath(std::string_view path)
{
  std::string ret;
  PathAppendString(ret, path);

  // remove trailing slashes
  if (ret.length() > 1)
  {
    while (ret.back() == FS_OSPATH_SEPARATOR_CHARACTER)
      ret.pop_back();
  }

  return ret;
}

void Path::ToNativePath(std::string* path)
{
  *path = Path::ToNativePath(*path);
}

std::string Path::Canonicalize(std::string_view path)
{
  std::vector<std::string_view> components = Path::SplitNativePath(path);
  std::vector<std::string_view> new_components;
  new_components.reserve(components.size());
  for (const std::string_view& component : components)
  {
    if (component == ".")
    {
      // current directory, so it can be skipped, unless it's the only component
      if (components.size() == 1)
        new_components.push_back(component);
    }
    else if (component == "..")
    {
      // parent directory, pop one off if we're not at the beginning, otherwise preserve.
      if (!new_components.empty())
        new_components.pop_back();
      else
        new_components.push_back(component);
    }
    else
    {
      // anything else, preserve
      new_components.push_back(component);
    }
  }

  return Path::JoinNativePath(new_components);
}

void Path::Canonicalize(std::string* path)
{
  *path = Canonicalize(*path);
}

std::string Path::MakeRelative(std::string_view path, std::string_view relative_to)
{
  // simple algorithm, we just work on the components. could probably be better, but it'll do for now.
  std::vector<std::string_view> path_components(SplitNativePath(path));
  std::vector<std::string_view> relative_components(SplitNativePath(relative_to));
  std::vector<std::string_view> new_components;

  // both must be absolute paths
  if (Path::IsAbsolute(path) && Path::IsAbsolute(relative_to))
  {
    // find the number of same components
    size_t num_same = 0;
    for (size_t i = 0; i < path_components.size() && i < relative_components.size(); i++)
    {
      if (path_components[i] == relative_components[i])
        num_same++;
      else
        break;
    }

    // we need at least one same component
    if (num_same > 0)
    {
      // from the relative_to directory, back up to the start of the common components
      const size_t num_ups = relative_components.size() - num_same;
      for (size_t i = 0; i < num_ups; i++)
        new_components.emplace_back("..");

      // and add the remainder of the path components
      for (size_t i = num_same; i < path_components.size(); i++)
        new_components.push_back(std::move(path_components[i]));
    }
    else
    {
      // no similarity
      new_components = std::move(path_components);
    }
  }
  else
  {
    // not absolute
    new_components = std::move(path_components);
  }

  return JoinNativePath(new_components);
}

std::string_view Path::GetExtension(std::string_view path)
{
  const std::string_view::size_type pos = path.rfind('.');
  if (pos == std::string_view::npos)
    return std::string_view();
  else
    return path.substr(pos + 1);
}

std::string_view Path::StripExtension(std::string_view path)
{
  const std::string_view::size_type pos = path.rfind('.');
  if (pos == std::string_view::npos)
    return path;

  return path.substr(0, pos);
}

std::string Path::ReplaceExtension(std::string_view path, std::string_view new_extension)
{
  const std::string_view::size_type pos = path.rfind('.');
  if (pos == std::string_view::npos)
    return std::string(path);

  std::string ret(path, 0, pos + 1);
  ret.append(new_extension);
  return ret;
}

static std::string_view::size_type GetLastSeperatorPosition(std::string_view path, bool include_separator)
{
  std::string_view::size_type last_separator = path.rfind('/');
  if (include_separator && last_separator != std::string_view::npos)
    last_separator++;

#if defined(_WIN32)
  std::string_view::size_type other_last_separator = path.rfind('\\');
  if (other_last_separator != std::string_view::npos)
  {
    if (include_separator)
      other_last_separator++;
    if (last_separator == std::string_view::npos || other_last_separator > last_separator)
      last_separator = other_last_separator;
  }
#endif

  return last_separator;
}

std::string FileSystem::GetDisplayNameFromPath(std::string_view path)
{
  return std::string(Path::GetFileName(path));
}

std::string_view Path::GetDirectory(std::string_view path)
{
  const std::string::size_type pos = GetLastSeperatorPosition(path, false);
  if (pos == std::string_view::npos)
    return {};

  return path.substr(0, pos);
}

std::string_view Path::GetFileName(std::string_view path)
{
  const std::string_view::size_type pos = GetLastSeperatorPosition(path, true);
  if (pos == std::string_view::npos)
    return path;

  return path.substr(pos);
}

std::string_view Path::GetFileTitle(std::string_view path)
{
  const std::string_view filename(GetFileName(path));
  const std::string::size_type pos = filename.rfind('.');
  if (pos == std::string_view::npos)
    return filename;

  return filename.substr(0, pos);
}

std::string Path::ChangeFileName(std::string_view path, std::string_view new_filename)
{
  std::string ret;
  PathAppendString(ret, path);

  const std::string_view::size_type pos = GetLastSeperatorPosition(ret, true);
  if (pos == std::string_view::npos)
  {
    ret.clear();
    PathAppendString(ret, new_filename);
  }
  else
  {
    if (!new_filename.empty())
    {
      ret.erase(pos);
      PathAppendString(ret, new_filename);
    }
    else
    {
      ret.erase(pos - 1);
    }
  }

  return ret;
}

void Path::ChangeFileName(std::string* path, std::string_view new_filename)
{
  *path = ChangeFileName(*path, new_filename);
}

std::string Path::AppendDirectory(std::string_view path, std::string_view new_dir)
{
  std::string ret;
  if (!new_dir.empty())
  {
    const std::string_view::size_type pos = GetLastSeperatorPosition(path, true);

    ret.reserve(path.length() + new_dir.length() + 1);
    if (pos != std::string_view::npos)
      PathAppendString(ret, path.substr(0, pos));

    while (!ret.empty() && ret.back() == FS_OSPATH_SEPARATOR_CHARACTER)
      ret.pop_back();

    if (!ret.empty())
      ret += FS_OSPATH_SEPARATOR_CHARACTER;

    PathAppendString(ret, new_dir);

    if (pos != std::string_view::npos)
    {
      const std::string_view filepart(path.substr(pos));
      if (!filepart.empty())
      {
        ret += FS_OSPATH_SEPARATOR_CHARACTER;
        PathAppendString(ret, filepart);
      }
    }
    else if (!path.empty())
    {
      ret += FS_OSPATH_SEPARATOR_CHARACTER;
      PathAppendString(ret, path);
    }
  }
  else
  {
    PathAppendString(ret, path);
  }

  return ret;
}

void Path::AppendDirectory(std::string* path, std::string_view new_dir)
{
  *path = AppendDirectory(*path, new_dir);
}

std::vector<std::string_view> Path::SplitWindowsPath(std::string_view path)
{
  std::vector<std::string_view> parts;

  std::string::size_type start = 0;
  std::string::size_type pos = 0;

  // preserve unc paths
  if (path.size() > 2 && path[0] == '\\' && path[1] == '\\')
    pos = 2;

  while (pos < path.size())
  {
    if (path[pos] != '/' && path[pos] != '\\')
    {
      pos++;
      continue;
    }

    // skip consecutive separators
    if (pos != start)
      parts.push_back(path.substr(start, pos - start));

    pos++;
    start = pos;
  }

  if (start != pos)
    parts.push_back(path.substr(start));

  return parts;
}

std::string Path::JoinWindowsPath(const std::vector<std::string_view>& components)
{
  return StringUtil::JoinString(components.begin(), components.end(), '\\');
}

std::vector<std::string_view> Path::SplitNativePath(std::string_view path)
{
#ifdef _WIN32
  return SplitWindowsPath(path);
#else
  std::vector<std::string_view> parts;

  std::string::size_type start = 0;
  std::string::size_type pos = 0;
  while (pos < path.size())
  {
    if (path[pos] != '/')
    {
      pos++;
      continue;
    }

    // skip consecutive separators
    // for unix, we create an empty element at the beginning when it's an absolute path
    // that way, when it's re-joined later, we preserve the starting slash.
    if (pos != start || pos == 0)
      parts.push_back(path.substr(start, pos - start));

    pos++;
    start = pos;
  }

  if (start != pos)
    parts.push_back(path.substr(start));

  return parts;
#endif
}

std::string Path::JoinNativePath(const std::vector<std::string_view>& components)
{
  return StringUtil::JoinString(components.begin(), components.end(), FS_OSPATH_SEPARATOR_CHARACTER);
}

std::vector<std::string> FileSystem::GetRootDirectoryList()
{
  std::vector<std::string> results;

#if defined(_WIN32)
  char buf[256];
  const DWORD size = GetLogicalDriveStringsA(sizeof(buf), buf);
  if (size != 0 && size < (sizeof(buf) - 1))
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

std::string Path::BuildRelativePath(std::string_view path, std::string_view new_filename)
{
  std::string new_string;

  std::string_view::size_type pos = GetLastSeperatorPosition(path, true);
  if (pos != std::string_view::npos)
    new_string.assign(path, 0, pos);
  new_string.append(new_filename);
  return new_string;
}

std::string Path::Combine(std::string_view base, std::string_view next)
{
  std::string ret;
  ret.reserve(base.length() + next.length() + 1);

  PathAppendString(ret, base);
  while (!ret.empty() && ret.back() == FS_OSPATH_SEPARATOR_CHARACTER)
    ret.pop_back();

  ret += FS_OSPATH_SEPARATOR_CHARACTER;
  PathAppendString(ret, next);
  while (!ret.empty() && ret.back() == FS_OSPATH_SEPARATOR_CHARACTER)
    ret.pop_back();

  return ret;
}

std::FILE* FileSystem::OpenCFile(const char* path, const char* mode, Error* error)
{
#ifdef _WIN32
  const std::wstring wfilename = GetWin32Path(path);
  const std::wstring wmode = StringUtil::UTF8StringToWideString(mode);
  if (!wfilename.empty() && !wmode.empty())
  {
    std::FILE* fp;
    const errno_t err = _wfopen_s(&fp, wfilename.c_str(), wmode.c_str());
    if (err != 0)
    {
      Error::SetErrno(error, err);
      return nullptr;
    }

    return fp;
  }

  std::FILE* fp;
  const errno_t err = fopen_s(&fp, path, mode);
  if (err != 0)
  {
    Error::SetErrno(error, err);
    return nullptr;
  }

  return fp;
#else
  std::FILE* fp = std::fopen(path, mode);
  if (!fp)
    Error::SetErrno(error, errno);
  return fp;
#endif
}

std::FILE* FileSystem::OpenExistingOrCreateCFile(const char* path, s32 retry_ms, Error* error /*= nullptr*/)
{
#ifdef _WIN32
  const std::wstring wpath = GetWin32Path(path);
  if (wpath.empty())
  {
    Error::SetStringView(error, "Invalid path.");
    return nullptr;
  }

  HANDLE file = CreateFileW(wpath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, NULL);

  // if there's a sharing violation, keep retrying
  if (file == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION && retry_ms >= 0)
  {
    Timer timer;
    while (retry_ms == 0 || timer.GetTimeMilliseconds() <= retry_ms)
    {
      Sleep(1);
      file = CreateFileW(wpath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, NULL);
      if (file != INVALID_HANDLE_VALUE || GetLastError() != ERROR_SHARING_VIOLATION)
        break;
    }
  }

  if (file == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND)
  {
    // try creating it
    file = CreateFileW(wpath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, NULL);
    if (file == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_EXISTS)
    {
      // someone else beat us in the race, try again with existing.
      file = CreateFileW(wpath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, NULL);
    }
  }

  // done?
  if (file == INVALID_HANDLE_VALUE)
  {
    Error::SetWin32(error, "CreateFile() failed: ", GetLastError());
    return nullptr;
  }

  // convert to C FILE
  const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(file), 0);
  if (fd < 0)
  {
    Error::SetErrno(error, "_open_osfhandle() failed: ", errno);
    CloseHandle(file);
    return nullptr;
  }

  // convert to a stream
  std::FILE* cfile = _fdopen(fd, "r+b");
  if (!cfile)
  {
    Error::SetErrno(error, "_fdopen() failed: ", errno);
    _close(fd);
  }

  return cfile;
#else
  std::FILE* fp = std::fopen(path, "r+b");
  if (fp)
    return fp;

  // don't try creating for any error other than "not exist"
  if (errno != ENOENT)
  {
    Error::SetErrno(error, errno);
    return nullptr;
  }

  // try again, but create the file. mode "x" exists on all platforms.
  fp = std::fopen(path, "w+bx");
  if (fp)
    return fp;

  // if it already exists, someone else beat us in the race. try again with existing.
  if (errno == EEXIST)
    fp = std::fopen(path, "r+b");
  if (!fp)
  {
    Error::SetErrno(error, errno);
    return nullptr;
  }

  return fp;
#endif
}

int FileSystem::OpenFDFile(const char* path, int flags, int mode, Error* error)
{
#ifdef _WIN32
  const std::wstring wpath = GetWin32Path(path);
  if (!wpath.empty())
    return _wopen(wpath.c_str(), flags, mode);

  return -1;
#else
  const int fd = open(path, flags, mode);
  if (fd < 0)
    Error::SetErrno(error, errno);
  return fd;
#endif
}

std::FILE* FileSystem::OpenSharedCFile(const char* path, const char* mode, FileShareMode share_mode, Error* error)
{
#ifdef _WIN32
  const std::wstring wpath = GetWin32Path(path);
  const std::wstring wmode = StringUtil::UTF8StringToWideString(mode);
  if (wpath.empty() || wmode.empty())
    return nullptr;

  int share_flags = 0;
  switch (share_mode)
  {
    case FileShareMode::DenyNone:
      share_flags = _SH_DENYNO;
      break;
    case FileShareMode::DenyRead:
      share_flags = _SH_DENYRD;
      break;
    case FileShareMode::DenyWrite:
      share_flags = _SH_DENYWR;
      break;
    case FileShareMode::DenyReadWrite:
    default:
      share_flags = _SH_DENYRW;
      break;
  }

  std::FILE* fp = _wfsopen(wpath.c_str(), wmode.c_str(), share_flags);
  if (fp)
    return fp;

  Error::SetErrno(error, errno);
  return nullptr;
#else
  std::FILE* fp = std::fopen(path, mode);
  if (!fp)
    Error::SetErrno(error, errno);
  return fp;
#endif
}

#endif // __ANDROID__

std::string Path::URLEncode(std::string_view str)
{
  std::string ret;
  ret.reserve(str.length() + ((str.length() + 3) / 4) * 3);

  for (size_t i = 0, l = str.size(); i < l; i++)
  {
    const char c = str[i];
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_' ||
        c == '.' || c == '!' || c == '~' || c == '*' || c == '\'' || c == '(' || c == ')')
    {
      ret.push_back(c);
    }
    else
    {
      ret.push_back('%');

      const unsigned char n1 = static_cast<unsigned char>(c) >> 4;
      const unsigned char n2 = static_cast<unsigned char>(c) & 0x0F;
      ret.push_back((n1 >= 10) ? ('a' + (n1 - 10)) : ('0' + n1));
      ret.push_back((n2 >= 10) ? ('a' + (n2 - 10)) : ('0' + n2));
    }
  }

  return ret;
}

std::string Path::URLDecode(std::string_view str)
{
  std::string ret;
  ret.reserve(str.length());

  for (size_t i = 0, l = str.size(); i < l; i++)
  {
    const char c = str[i];
    if (c == '+')
    {
      ret.push_back(c);
    }
    else if (c == '%')
    {
      if ((i + 2) >= str.length())
        break;

      const char clower = str[i + 1];
      const char cupper = str[i + 2];
      const unsigned char lower =
        (clower >= '0' && clower <= '9') ?
          static_cast<unsigned char>(clower - '0') :
          ((clower >= 'a' && clower <= 'f') ?
             static_cast<unsigned char>(clower - 'a') :
             ((clower >= 'A' && clower <= 'F') ? static_cast<unsigned char>(clower - 'A') : 0));
      const unsigned char upper =
        (cupper >= '0' && cupper <= '9') ?
          static_cast<unsigned char>(cupper - '0') :
          ((cupper >= 'a' && cupper <= 'f') ?
             static_cast<unsigned char>(cupper - 'a') :
             ((cupper >= 'A' && cupper <= 'F') ? static_cast<unsigned char>(cupper - 'A') : 0));
      const char dch = static_cast<char>(lower | (upper << 4));
      ret.push_back(dch);
    }
    else
    {
      ret.push_back(c);
    }
  }

  return std::string(str);
}

std::string Path::CreateFileURL(std::string_view path)
{
  DebugAssert(IsAbsolute(path));

  std::string ret;
  ret.reserve(path.length() + 10);
  ret.append("file://");

  const std::vector<std::string_view> components = SplitNativePath(path);
  Assert(!components.empty());

  const std::string_view& first = components.front();
#ifdef _WIN32
  // Windows doesn't urlencode the drive letter.
  // UNC paths should be omit the leading slash.
  if (first.starts_with("\\\\"))
  {
    // file://hostname/...
    ret.append(first.substr(2));
  }
  else
  {
    // file:///c:/...
    fmt::format_to(std::back_inserter(ret), "/{}", first);
  }
#else
  // Don't append a leading slash for the first component.
  ret.append(first);
#endif

  for (size_t comp = 1; comp < components.size(); comp++)
  {
    fmt::format_to(std::back_inserter(ret), "/{}", URLEncode(components[comp]));
  }

  return ret;
}

FileSystem::AtomicRenamedFileDeleter::AtomicRenamedFileDeleter(std::string temp_path, std::string final_path)
  : m_temp_path(std::move(temp_path)), m_final_path(std::move(final_path))
{
}

FileSystem::AtomicRenamedFileDeleter::~AtomicRenamedFileDeleter() = default;

void FileSystem::AtomicRenamedFileDeleter::operator()(std::FILE* fp)
{
  if (!fp)
    return;

  Error error;

  // final filename empty => discarded.
  if (!m_final_path.empty())
  {
    if (!commit(fp, &error))
    {
      ERROR_LOG("Failed to commit temporary file '{}', discarding. Error was {}.", Path::GetFileName(m_temp_path),
                error.GetDescription());
    }

    return;
  }

  // we're discarding the file, don't care if it fails.
  std::fclose(fp);

  if (!DeleteFile(m_temp_path.c_str(), &error))
    ERROR_LOG("Failed to delete temporary file '{}': {}", Path::GetFileName(m_temp_path), error.GetDescription());
}

bool FileSystem::AtomicRenamedFileDeleter::commit(std::FILE* fp, Error* error)
{
  if (!fp) [[unlikely]]
  {
    Error::SetStringView(error, "File pointer is null.");
    return false;
  }

  if (std::fclose(fp) != 0)
  {
    Error::SetErrno(error, "fclose() failed: ", errno);
    m_final_path.clear();
  }

  // Should not have been discarded.
  if (!m_final_path.empty())
  {
    return RenamePath(m_temp_path.c_str(), m_final_path.c_str(), error);
  }
  else
  {
    Error::SetStringView(error, "File has already been discarded.");
    return DeleteFile(m_temp_path.c_str(), error);
  }
}

void FileSystem::AtomicRenamedFileDeleter::discard()
{
  m_final_path = {};
}

FileSystem::AtomicRenamedFile FileSystem::CreateAtomicRenamedFile(std::string path, Error* error /*= nullptr*/)
{
  std::string temp_path;
  std::FILE* fp = nullptr;
  if (!path.empty())
  {
    // this is disgusting, but we need null termination, and std::string::data() does not guarantee it.
    const size_t path_length = path.length();
    const size_t name_buf_size = path_length + 8;
    std::unique_ptr<char[]> name_buf = std::make_unique<char[]>(name_buf_size);
    std::memcpy(name_buf.get(), path.c_str(), path_length);
    StringUtil::Strlcpy(name_buf.get() + path_length, ".XXXXXX", name_buf_size);

#ifdef _WIN32
    const errno_t err = _mktemp_s(name_buf.get(), name_buf_size);
    if (err == 0)
      fp = OpenCFile(name_buf.get(), "w+b", error);
    else
      Error::SetErrno(error, "_mktemp_s() failed: ", err);

#elif defined(__linux__) || defined(__ANDROID__) || defined(__APPLE__) || defined(__FreeBSD__)
    const int fd = mkstemp(name_buf.get());
    if (fd >= 0)
    {
      fp = fdopen(fd, "w+b");
      if (!fp)
        Error::SetErrno(error, "fdopen() failed: ", errno);
    }
    else
    {
      Error::SetErrno(error, "mkstemp() failed: ", errno);
    }
#else
    mktemp(name_buf.get());
    fp = OpenCFile(name_buf.get(), "w+b", error);
#endif

    if (fp)
      temp_path.assign(name_buf.get(), name_buf_size - 1);
    else
      path.clear();
  }

  return AtomicRenamedFile(fp, AtomicRenamedFileDeleter(std::move(temp_path), std::move(path)));
}

bool FileSystem::WriteAtomicRenamedFile(std::string path, const void* data, size_t data_length,
                                        Error* error /*= nullptr*/)
{
  AtomicRenamedFile fp = CreateAtomicRenamedFile(std::move(path), error);
  if (!fp)
    return false;

  if (data_length > 0 && std::fwrite(data, 1u, data_length, fp.get()) != data_length) [[unlikely]]
  {
    Error::SetErrno(error, "fwrite() failed: ", errno);
    DiscardAtomicRenamedFile(fp);
    return false;
  }

  return true;
}

bool FileSystem::WriteAtomicRenamedFile(std::string path, const std::span<const u8> data, Error* error /* = nullptr */)
{
  return WriteAtomicRenamedFile(std::move(path), data.empty() ? nullptr : data.data(), data.size(), error);
}

void FileSystem::DiscardAtomicRenamedFile(AtomicRenamedFile& file)
{
  file.get_deleter().discard();
}

bool FileSystem::CommitAtomicRenamedFile(AtomicRenamedFile& file, Error* error)
{
  if (file.get_deleter().commit(file.release(), error))
    return true;

  Error::AddPrefix(error, "Failed to commit file: ");
  return false;
}

FileSystem::ManagedCFilePtr FileSystem::OpenManagedCFile(const char* path, const char* mode, Error* error)
{
  return ManagedCFilePtr(OpenCFile(path, mode, error));
}

FileSystem::ManagedCFilePtr FileSystem::OpenExistingOrCreateManagedCFile(const char* path, s32 retry_ms, Error* error)
{
  return ManagedCFilePtr(OpenExistingOrCreateCFile(path, retry_ms, error));
}

FileSystem::ManagedCFilePtr FileSystem::OpenManagedSharedCFile(const char* path, const char* mode,
                                                               FileShareMode share_mode, Error* error)
{
  return ManagedCFilePtr(OpenSharedCFile(path, mode, share_mode, error));
}

int FileSystem::FSeek64(std::FILE* fp, s64 offset, int whence)
{
#ifdef _WIN32
  return _fseeki64(fp, offset, whence);
#else
  // Prevent truncation on platforms which don't have a 64-bit off_t.
  if constexpr (sizeof(off_t) != sizeof(s64))
  {
    if (offset < std::numeric_limits<off_t>::min() || offset > std::numeric_limits<off_t>::max())
      return -1;
  }

  return fseeko(fp, static_cast<off_t>(offset), whence);
#endif
}

bool FileSystem::FSeek64(std::FILE* fp, s64 offset, int whence, Error* error)
{
#ifdef _WIN32
  const int res = _fseeki64(fp, offset, whence);
#else
  // Prevent truncation on platforms which don't have a 64-bit off_t.
  if constexpr (sizeof(off_t) != sizeof(s64))
  {
    if (offset < std::numeric_limits<off_t>::min() || offset > std::numeric_limits<off_t>::max())
    {
      Error::SetStringView(error, "Invalid offset.");
      return false;
    }
  }

  const int res = fseeko(fp, static_cast<off_t>(offset), whence);
#endif

  if (res == 0)
    return true;

  Error::SetErrno(error, errno);
  return false;
}

s64 FileSystem::FTell64(std::FILE* fp)
{
#ifdef _WIN32
  return static_cast<s64>(_ftelli64(fp));
#else
  return static_cast<s64>(ftello(fp));
#endif
}

s64 FileSystem::FSize64(std::FILE* fp, Error* error)
{
  const s64 pos = FTell64(fp);
  if (pos < 0) [[unlikely]]
  {
    Error::SetErrno(error, "FTell64() failed: ", errno);
    return -1;
  }

  if (FSeek64(fp, 0, SEEK_END) != 0) [[unlikely]]
  {
    Error::SetErrno(error, "FSeek64() to end failed: ", errno);
    return -1;
  }

  const s64 size = FTell64(fp);
  if (size < 0) [[unlikely]]
  {
    Error::SetErrno(error, "FTell64() failed: ", errno);
    return -1;
  }

  if (FSeek64(fp, pos, SEEK_SET) != 0)
  {
    Error::SetErrno(error, "FSeek64() to original position failed: ", errno);
    return -1;
  }

  return size;
}

bool FileSystem::FTruncate64(std::FILE* fp, s64 size, Error* error)
{
  const int fd = fileno(fp);
  if (fd < 0)
  {
    Error::SetErrno(error, "fileno() failed: ", errno);
    return false;
  }

#ifdef _WIN32
  const errno_t err = _chsize_s(fd, size);
  if (err != 0)
  {
    Error::SetErrno(error, "_chsize_s() failed: ", err);
    return false;
  }

  return true;
#else
  // Prevent truncation on platforms which don't have a 64-bit off_t.
  if constexpr (sizeof(off_t) != sizeof(s64))
  {
    if (size < std::numeric_limits<off_t>::min() || size > std::numeric_limits<off_t>::max())
    {
      Error::SetStringView(error, "File size is too large.");
      return false;
    }
  }

  if (ftruncate(fd, static_cast<off_t>(size)) < 0)
  {
    Error::SetErrno(error, "ftruncate() failed: ", errno);
    return false;
  }

  return true;
#endif
}

s64 FileSystem::GetPathFileSize(const char* path)
{
  FILESYSTEM_STAT_DATA sd;
  if (!StatFile(path, &sd))
    return -1;

  return sd.Size;
}

std::optional<DynamicHeapArray<u8>> FileSystem::ReadBinaryFile(const char* path, Error* error)
{
  std::optional<DynamicHeapArray<u8>> ret;

  ManagedCFilePtr fp = OpenManagedCFile(path, "rb", error);
  if (!fp)
    return ret;

  ret = ReadBinaryFile(fp.get(), error);
  return ret;
}

std::optional<DynamicHeapArray<u8>> FileSystem::ReadBinaryFile(std::FILE* fp, Error* error)
{
  std::optional<DynamicHeapArray<u8>> ret;

  if (FSeek64(fp, 0, SEEK_END) != 0) [[unlikely]]
  {
    Error::SetErrno(error, "FSeek64() to end failed: ", errno);
    return ret;
  }

  const s64 size = FTell64(fp);
  if (size < 0) [[unlikely]]
  {
    Error::SetErrno(error, "FTell64() for length failed: ", errno);
    return ret;
  }

  if constexpr (sizeof(s64) != sizeof(size_t))
  {
    if (size > static_cast<s64>(std::numeric_limits<long>::max())) [[unlikely]]
    {
      Error::SetStringFmt(error, "File size of {} is too large to read on this platform.", size);
      return ret;
    }
  }

  if (FSeek64(fp, 0, SEEK_SET) != 0) [[unlikely]]
  {
    Error::SetErrno(error, "FSeek64() to start failed: ", errno);
    return ret;
  }

  ret = DynamicHeapArray<u8>(static_cast<size_t>(size));
  if (size > 0 && std::fread(ret->data(), 1u, static_cast<size_t>(size), fp) != static_cast<size_t>(size)) [[unlikely]]
  {
    Error::SetErrno(error, "fread() failed: ", errno);
    ret.reset();
  }

  return ret;
}

std::optional<std::string> FileSystem::ReadFileToString(const char* path, Error* error)
{
  std::optional<std::string> ret;

  ManagedCFilePtr fp = OpenManagedCFile(path, "rb", error);
  if (!fp)
    return ret;

  ret = ReadFileToString(fp.get());
  return ret;
}

std::optional<std::string> FileSystem::ReadFileToString(std::FILE* fp, Error* error)
{
  std::optional<std::string> ret;

  if (FSeek64(fp, 0, SEEK_END) != 0) [[unlikely]]
  {
    Error::SetErrno(error, "FSeek64() to end failed: ", errno);
    return ret;
  }

  const s64 size = FTell64(fp);
  if (size < 0) [[unlikely]]
  {
    Error::SetErrno(error, "FTell64() for length failed: ", errno);
    return ret;
  }

  if constexpr (sizeof(s64) != sizeof(size_t))
  {
    if (size > static_cast<s64>(std::numeric_limits<long>::max())) [[unlikely]]
    {
      Error::SetStringFmt(error, "File size of {} is too large to read on this platform.", size);
      return ret;
    }
  }

  if (FSeek64(fp, 0, SEEK_SET) != 0) [[unlikely]]
  {
    Error::SetErrno(error, "FSeek64() to start failed: ", errno);
    return ret;
  }

  ret = std::string();
  ret->resize(static_cast<size_t>(size));
  // NOTE - assumes mode 'rb', for example, this will fail over missing Windows carriage return bytes
  if (size > 0 && std::fread(ret->data(), 1u, static_cast<size_t>(size), fp) != static_cast<size_t>(size))
  {
    Error::SetErrno(error, "fread() failed: ", errno);
    ret.reset();
  }

  return ret;
}

bool FileSystem::WriteBinaryFile(const char* path, const void* data, size_t data_length, Error* error)
{
  ManagedCFilePtr fp = OpenManagedCFile(path, "wb", error);
  if (!fp)
    return false;

  if (data_length > 0 && std::fwrite(data, 1u, data_length, fp.get()) != data_length)
  {
    Error::SetErrno(error, "fwrite() failed: ", errno);
    return false;
  }

  return true;
}

bool FileSystem::WriteBinaryFile(const char* path, const std::span<const u8> data, Error* error /*= nullptr*/)
{
  return WriteBinaryFile(path, data.empty() ? nullptr : data.data(), data.size(), error);
}

bool FileSystem::WriteStringToFile(const char* path, std::string_view sv, Error* error)
{
  ManagedCFilePtr fp = OpenManagedCFile(path, "wb", error);
  if (!fp)
    return false;

  if (sv.length() > 0 && std::fwrite(sv.data(), 1u, sv.length(), fp.get()) != sv.length())
  {
    Error::SetErrno(error, "fwrite() failed: ", errno);
    return false;
  }

  return true;
}

bool FileSystem::EnsureDirectoryExists(const char* path, bool recursive, Error* error)
{
  if (FileSystem::DirectoryExists(path))
    return true;

  // if it fails to create, we're not going to be able to use it anyway
  return FileSystem::CreateDirectory(path, recursive, error);
}

bool FileSystem::RecursiveDeleteDirectory(const char* path)
{
  FindResultsArray results;
  if (FindFiles(path, "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_FOLDERS | FILESYSTEM_FIND_HIDDEN_FILES, &results))
  {
    for (const FILESYSTEM_FIND_DATA& fd : results)
    {
      if (fd.Attributes & FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY)
      {
        if (!RecursiveDeleteDirectory(fd.FileName.c_str()))
          return false;
      }
      else
      {
        if (!DeleteFile(fd.FileName.c_str()))
          return false;
      }
    }
  }

  return DeleteDirectory(path);
}

bool FileSystem::CopyFilePath(const char* source, const char* destination, bool replace)
{
#ifndef _WIN32
  // TODO: There's technically a race here between checking and opening the file..
  // But fopen doesn't specify any way to say "don't create if it exists"...
  if (!replace && FileExists(destination))
    return false;

  auto in_fp = OpenManagedCFile(source, "rb");
  if (!in_fp)
    return false;

  auto out_fp = OpenManagedCFile(destination, "wb");
  if (!out_fp)
    return false;

  u8 buf[4096];
  while (!std::feof(in_fp.get()))
  {
    size_t bytes_in = std::fread(buf, 1, sizeof(buf), in_fp.get());
    if ((bytes_in == 0 && !std::feof(in_fp.get())) ||
        (bytes_in > 0 && std::fwrite(buf, 1, bytes_in, out_fp.get()) != bytes_in))
    {
      out_fp.reset();
      DeleteFile(destination);
      return false;
    }
  }

  if (std::fflush(out_fp.get()) != 0)
  {
    out_fp.reset();
    DeleteFile(destination);
    return false;
  }

  return true;
#else
  return CopyFileW(GetWin32Path(source).c_str(), GetWin32Path(destination).c_str(), !replace);
#endif
}

#ifdef _WIN32

static u32 TranslateWin32Attributes(u32 w32attrs)
{
  return ((w32attrs & FILE_ATTRIBUTE_DIRECTORY) ? FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY : 0) |
         ((w32attrs & FILE_ATTRIBUTE_READONLY) ? FILESYSTEM_FILE_ATTRIBUTE_READ_ONLY : 0) |
         ((w32attrs & FILE_ATTRIBUTE_COMPRESSED) ? FILESYSTEM_FILE_ATTRIBUTE_COMPRESSED : 0) |
         ((w32attrs & FILE_ATTRIBUTE_REPARSE_POINT) ? FILESYSTEM_FILE_ATTRIBUTE_LINK : 0);
}

static u32 RecursiveFindFiles(const char* origin_path, const char* parent_path, const char* path, const char* pattern,
                              u32 flags, FileSystem::FindResultsArray* results, std::vector<std::string>& visited)
{
  std::string search_dir;
  if (path)
  {
    if (parent_path)
      search_dir = fmt::format("{}\\{}\\{}\\*", origin_path, parent_path, path);
    else
      search_dir = fmt::format("{}\\{}\\*", origin_path, path);
  }
  else
  {
    search_dir = fmt::format("{}\\*", origin_path);
  }

  // holder for utf-8 conversion
  WIN32_FIND_DATAW wfd;
  std::string utf8_filename;
  utf8_filename.reserve((sizeof(wfd.cFileName) / sizeof(wfd.cFileName[0])) * 2);

  const HANDLE hFind = FindFirstFileW(FileSystem::GetWin32Path(search_dir).c_str(), &wfd);
  if (hFind == INVALID_HANDLE_VALUE)
    return 0;

  // small speed optimization for '*' case
  bool hasWildCards = false;
  bool wildCardMatchAll = false;
  u32 nFiles = 0;
  if (std::strpbrk(pattern, "*?"))
  {
    hasWildCards = true;
    wildCardMatchAll = !(std::strcmp(pattern, "*"));
  }

  // iterate results
  do
  {
    if (wfd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN && !(flags & FILESYSTEM_FIND_HIDDEN_FILES))
      continue;

    if (wfd.cFileName[0] == L'.')
    {
      if (wfd.cFileName[1] == L'\0' || (wfd.cFileName[1] == L'.' && wfd.cFileName[2] == L'\0'))
        continue;
    }

    if (!StringUtil::WideStringToUTF8String(utf8_filename, wfd.cFileName))
      continue;

    FILESYSTEM_FIND_DATA outData;
    outData.Attributes = TranslateWin32Attributes(wfd.dwFileAttributes);

    if (wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    {
      if (flags & FILESYSTEM_FIND_RECURSIVE)
      {
        // check that we're not following an infinite symbolic link loop
        std::string real_recurse_dir;
        if (parent_path)
          real_recurse_dir =
            Path::RealPath(fmt::format("{}\\{}\\{}\\{}", origin_path, parent_path, path, utf8_filename));
        else if (path)
          real_recurse_dir = Path::RealPath(fmt::format("{}\\{}\\{}", origin_path, path, utf8_filename));
        else
          real_recurse_dir = Path::RealPath(fmt::format("{}\\{}", origin_path, utf8_filename));
        if (real_recurse_dir.empty() || std::find(visited.begin(), visited.end(), real_recurse_dir) == visited.end())
        {
          if (!real_recurse_dir.empty())
            visited.push_back(std::move(real_recurse_dir));

          // recurse into this directory
          if (parent_path)
          {
            const std::string recurse_dir = fmt::format("{}\\{}", parent_path, path);
            nFiles += RecursiveFindFiles(origin_path, recurse_dir.c_str(), utf8_filename.c_str(), pattern, flags,
                                         results, visited);
          }
          else
          {
            nFiles += RecursiveFindFiles(origin_path, path, utf8_filename.c_str(), pattern, flags, results, visited);
          }
        }
      }

      if (!(flags & FILESYSTEM_FIND_FOLDERS))
        continue;
    }
    else
    {
      if (!(flags & FILESYSTEM_FIND_FILES))
        continue;
    }

    // match the filename
    if (hasWildCards)
    {
      if (!wildCardMatchAll && !StringUtil::WildcardMatch(utf8_filename.c_str(), pattern))
        continue;
    }
    else
    {
      if (std::strcmp(utf8_filename.c_str(), pattern) != 0)
        continue;
    }

    // add file to list
    if (!(flags & FILESYSTEM_FIND_RELATIVE_PATHS))
    {
      if (parent_path)
        outData.FileName = fmt::format("{}\\{}\\{}\\{}", origin_path, parent_path, path, utf8_filename);
      else if (path)
        outData.FileName = fmt::format("{}\\{}\\{}", origin_path, path, utf8_filename);
      else
        outData.FileName = fmt::format("{}\\{}", origin_path, utf8_filename);
    }
    else
    {
      if (parent_path)
        outData.FileName = fmt::format("{}\\{}\\{}", parent_path, path, utf8_filename);
      else if (path)
        outData.FileName = fmt::format("{}\\{}", path, utf8_filename);
      else
        outData.FileName = utf8_filename;
    }

    outData.CreationTime = ConvertFileTimeToUnixTime(wfd.ftCreationTime);
    outData.ModificationTime = ConvertFileTimeToUnixTime(wfd.ftLastWriteTime);
    outData.Size = (static_cast<u64>(wfd.nFileSizeHigh) << 32) | static_cast<u64>(wfd.nFileSizeLow);

    nFiles++;
    results->push_back(std::move(outData));
  } while (FindNextFileW(hFind, &wfd) == TRUE);
  FindClose(hFind);

  return nFiles;
}

bool FileSystem::FindFiles(const char* path, const char* pattern, u32 flags, FindResultsArray* results)
{
  // clear result array
  if (!(flags & FILESYSTEM_FIND_KEEP_ARRAY))
    results->clear();

  // add self if recursive, we don't want to visit it twice
  std::vector<std::string> visited;
  if (flags & FILESYSTEM_FIND_RECURSIVE)
  {
    std::string real_path = Path::RealPath(path);
    if (!real_path.empty())
      visited.push_back(std::move(real_path));
  }

  // enter the recursive function
  if (RecursiveFindFiles(path, nullptr, nullptr, pattern, flags, results, visited) == 0)
    return false;

  if (flags & FILESYSTEM_FIND_SORT_BY_NAME)
  {
    std::sort(results->begin(), results->end(), [](const FILESYSTEM_FIND_DATA& lhs, const FILESYSTEM_FIND_DATA& rhs) {
      // directories first
      if ((lhs.Attributes & FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY) !=
          (rhs.Attributes & FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY))
      {
        return ((lhs.Attributes & FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY) != 0);
      }

      return (StringUtil::Strcasecmp(lhs.FileName.c_str(), rhs.FileName.c_str()) < 0);
    });
  }

  return true;
}

static void TranslateStat64(struct stat* st, const struct _stat64& st64)
{
  static constexpr __int64 MAX_SIZE = static_cast<__int64>(std::numeric_limits<decltype(st->st_size)>::max());
  st->st_dev = st64.st_dev;
  st->st_ino = st64.st_ino;
  st->st_mode = st64.st_mode;
  st->st_nlink = st64.st_nlink;
  st->st_uid = st64.st_uid;
  st->st_rdev = st64.st_rdev;
  st->st_size = static_cast<decltype(st->st_size)>((st64.st_size > MAX_SIZE) ? MAX_SIZE : st64.st_size);
  st->st_atime = static_cast<time_t>(st64.st_atime);
  st->st_mtime = static_cast<time_t>(st64.st_mtime);
  st->st_ctime = static_cast<time_t>(st64.st_ctime);
}

bool FileSystem::StatFile(const char* path, struct stat* st, Error* error)
{
  // convert to wide string
  const std::wstring wpath = GetWin32Path(path);
  if (wpath.empty()) [[unlikely]]
  {
    Error::SetStringView(error, "Path is empty.");
    return false;
  }

  struct _stat64 st64;
  if (_wstati64(wpath.c_str(), &st64) != 0)
    return false;

  TranslateStat64(st, st64);
  return true;
}

bool FileSystem::StatFile(std::FILE* fp, struct stat* st, Error* error)
{
  const int fd = _fileno(fp);
  if (fd < 0)
  {
    Error::SetErrno(error, "_fileno() failed: ", errno);
    return false;
  }

  struct _stat64 st64;
  if (_fstati64(fd, &st64) != 0)
  {
    Error::SetErrno(error, "_fstati64() failed: ", errno);
    return false;
  }

  TranslateStat64(st, st64);
  return true;
}

bool FileSystem::StatFile(const char* path, FILESYSTEM_STAT_DATA* sd, Error* error)
{
  // convert to wide string
  const std::wstring wpath = GetWin32Path(path);
  if (wpath.empty()) [[unlikely]]
  {
    Error::SetStringView(error, "Path is empty.");
    return false;
  }

  // determine attributes for the path. if it's a directory, things have to be handled differently..
  DWORD fileAttributes = GetFileAttributesW(wpath.c_str());
  if (fileAttributes == INVALID_FILE_ATTRIBUTES)
  {
    Error::SetWin32(error, "GetFileAttributesW() failed: ", GetLastError());
    return false;
  }

  // test if it is a directory
  HANDLE hFile;
  if (fileAttributes & FILE_ATTRIBUTE_DIRECTORY)
  {
    hFile = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  }
  else
  {
    hFile = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, 0, nullptr);
  }

  // createfile succeded?
  if (hFile == INVALID_HANDLE_VALUE)
  {
    Error::SetWin32(error, "CreateFileW() failed: ", GetLastError());
    return false;
  }

  // use GetFileInformationByHandle
  BY_HANDLE_FILE_INFORMATION bhfi;
  if (GetFileInformationByHandle(hFile, &bhfi) == FALSE)
  {
    Error::SetWin32(error, "GetFileInformationByHandle() failed: ", GetLastError());
    CloseHandle(hFile);
    return false;
  }

  // close handle
  CloseHandle(hFile);

  // fill in the stat data
  sd->Attributes = TranslateWin32Attributes(bhfi.dwFileAttributes);
  sd->CreationTime = ConvertFileTimeToUnixTime(bhfi.ftCreationTime);
  sd->ModificationTime = ConvertFileTimeToUnixTime(bhfi.ftLastWriteTime);
  sd->Size = static_cast<s64>(((u64)bhfi.nFileSizeHigh) << 32 | (u64)bhfi.nFileSizeLow);
  return true;
}

bool FileSystem::StatFile(std::FILE* fp, FILESYSTEM_STAT_DATA* sd, Error* error)
{
  const int fd = _fileno(fp);
  if (fd < 0)
  {
    Error::SetErrno(error, "_fileno() failed: ", errno);
    return false;
  }

  struct _stat64 st;
  if (_fstati64(fd, &st) != 0)
  {
    Error::SetErrno(error, "_fstati64() failed: ", errno);
    return false;
  }

  // parse attributes
  sd->CreationTime = st.st_ctime;
  sd->ModificationTime = st.st_mtime;
  sd->Attributes = 0;
  if ((st.st_mode & _S_IFMT) == _S_IFDIR)
    sd->Attributes |= FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY;

  // parse size
  if ((st.st_mode & _S_IFMT) == _S_IFREG)
    sd->Size = st.st_size;
  else
    sd->Size = 0;

  return true;
}

bool FileSystem::FileExists(const char* path)
{
  // convert to wide string
  const std::wstring wpath = GetWin32Path(path);
  if (wpath.empty())
    return false;

  // determine attributes for the path. if it's a directory, things have to be handled differently..
  const DWORD fileAttributes = GetFileAttributesW(wpath.c_str());
  if (fileAttributes == INVALID_FILE_ATTRIBUTES)
    return false;

  return ((fileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0);
}

bool FileSystem::DirectoryExists(const char* path)
{
  // convert to wide string
  const std::wstring wpath = GetWin32Path(path);
  if (wpath.empty())
    return false;

  // determine attributes for the path. if it's a directory, things have to be handled differently..
  const DWORD fileAttributes = GetFileAttributesW(wpath.c_str());
  if (fileAttributes == INVALID_FILE_ATTRIBUTES)
    return false;

  return ((fileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
}

bool FileSystem::IsRealDirectory(const char* path)
{
  // convert to wide string
  const std::wstring wpath = GetWin32Path(path);
  if (wpath.empty())
    return false;

  // determine attributes for the path. if it's a directory, things have to be handled differently..
  const DWORD fileAttributes = GetFileAttributesW(wpath.c_str());
  if (fileAttributes == INVALID_FILE_ATTRIBUTES)
    return false;

  return ((fileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != FILE_ATTRIBUTE_DIRECTORY);
}

bool FileSystem::IsDirectoryEmpty(const char* path)
{
  std::wstring wpath = GetWin32Path(path);
  wpath += L"\\*";

  WIN32_FIND_DATAW wfd;
  HANDLE hFind = FindFirstFileW(wpath.c_str(), &wfd);

  if (hFind == INVALID_HANDLE_VALUE)
    return true;

  do
  {
    if (wfd.cFileName[0] == L'.')
    {
      if (wfd.cFileName[1] == L'\0' || (wfd.cFileName[1] == L'.' && wfd.cFileName[2] == L'\0'))
        continue;
    }

    FindClose(hFind);
    return false;
  } while (FindNextFileW(hFind, &wfd));

  FindClose(hFind);
  return true;
}

bool FileSystem::CreateDirectory(const char* Path, bool Recursive, Error* error)
{
  const std::wstring win32_path = GetWin32Path(Path);
  if (win32_path.empty()) [[unlikely]]
  {
    Error::SetStringView(error, "Path is empty.");
    return false;
  }

  // try just flat-out, might work if there's no other segments that have to be made
  if (CreateDirectoryW(win32_path.c_str(), nullptr))
    return true;

  DWORD lastError = GetLastError();
  if (lastError == ERROR_ALREADY_EXISTS)
  {
    // check the attributes
    const u32 Attributes = GetFileAttributesW(win32_path.c_str());
    if (Attributes != INVALID_FILE_ATTRIBUTES && Attributes & FILE_ATTRIBUTE_DIRECTORY)
      return true;
  }

  if (!Recursive)
  {
    Error::SetWin32(error, "CreateDirectoryW() failed: ", lastError);
    return false;
  }

  // check error
  if (lastError == ERROR_PATH_NOT_FOUND)
  {
    // part of the path does not exist, so we'll create the parent folders, then
    // the full path again.
    const size_t pathLength = std::strlen(Path);
    for (size_t i = 0; i < pathLength; i++)
    {
      if (Path[i] == '\\' || Path[i] == '/')
      {
        const std::string_view ppath(Path, i);
        const BOOL result = CreateDirectoryW(GetWin32Path(ppath).c_str(), nullptr);
        if (!result)
        {
          lastError = GetLastError();
          if (lastError != ERROR_ALREADY_EXISTS) // fine, continue to next path segment
          {
            Error::SetWin32(error, "CreateDirectoryW() failed: ", lastError);
            return false;
          }
        }
      }
    }

    // re-create the end if it's not a separator, check / as well because windows can interpret them
    if (Path[pathLength - 1] != '\\' && Path[pathLength - 1] != '/')
    {
      const BOOL result = CreateDirectoryW(win32_path.c_str(), nullptr);
      if (!result)
      {
        lastError = GetLastError();
        if (lastError != ERROR_ALREADY_EXISTS)
        {
          Error::SetWin32(error, "CreateDirectoryW() failed: ", lastError);
          return false;
        }
      }
    }

    // ok
    return true;
  }
  else
  {
    // unhandled error
    Error::SetWin32(error, "CreateDirectoryW() failed: ", lastError);
    return false;
  }
}

bool FileSystem::DeleteFile(const char* path, Error* error)
{
  const std::wstring wpath = GetWin32Path(path);
  const DWORD fileAttributes = GetFileAttributesW(wpath.c_str());
  if (fileAttributes == INVALID_FILE_ATTRIBUTES || fileAttributes & FILE_ATTRIBUTE_DIRECTORY)
  {
    Error::SetStringView(error, "File does not exist.");
    return false;
  }

  if (!DeleteFileW(wpath.c_str()))
  {
    Error::SetWin32(error, "DeleteFileW() failed: ", GetLastError());
    return false;
  }

  return true;
}

bool FileSystem::RenamePath(const char* old_path, const char* new_path, Error* error)
{
  const std::wstring old_wpath = GetWin32Path(old_path);
  const std::wstring new_wpath = GetWin32Path(new_path);

  if (!MoveFileExW(old_wpath.c_str(), new_wpath.c_str(), MOVEFILE_REPLACE_EXISTING)) [[unlikely]]
  {
    Error::SetWin32(error, "MoveFileExW() failed: ", GetLastError());
    return false;
  }

  return true;
}

bool FileSystem::DeleteDirectory(const char* path)
{
  const std::wstring wpath = GetWin32Path(path);
  return RemoveDirectoryW(wpath.c_str());
}

std::string FileSystem::GetProgramPath()
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

  // Windows symlinks don't behave silly like Linux, so no need to RealPath() it.
  return StringUtil::WideStringToUTF8String(buffer);
}

std::string FileSystem::GetWorkingDirectory()
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

bool FileSystem::SetWorkingDirectory(const char* path)
{
  const std::wstring wpath = GetWin32Path(path);
  return (SetCurrentDirectoryW(wpath.c_str()) == TRUE);
}

bool FileSystem::SetPathCompression(const char* path, bool enable)
{
  const std::wstring wpath = GetWin32Path(path);
  const DWORD attrs = GetFileAttributesW(wpath.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES)
    return false;

  const bool isCompressed = (attrs & FILE_ATTRIBUTE_COMPRESSED) != 0;
  if (enable == isCompressed)
  {
    // already compressed/not compressed
    return true;
  }

  const bool isFile = !(attrs & FILE_ATTRIBUTE_DIRECTORY);
  const DWORD flags = isFile ? FILE_ATTRIBUTE_NORMAL : (FILE_FLAG_BACKUP_SEMANTICS | FILE_ATTRIBUTE_DIRECTORY);

  const HANDLE handle = CreateFileW(wpath.c_str(), FILE_GENERIC_WRITE | FILE_GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, flags, nullptr);
  if (handle == INVALID_HANDLE_VALUE)
    return false;

  DWORD bytesReturned = 0;
  DWORD compressMode = enable ? COMPRESSION_FORMAT_DEFAULT : COMPRESSION_FORMAT_NONE;

  bool result = DeviceIoControl(handle, FSCTL_SET_COMPRESSION, &compressMode, 2, nullptr, 0, &bytesReturned, nullptr);

  CloseHandle(handle);
  return result;
}

#elif !defined(__ANDROID__)

static u32 TranslateStatAttributes(struct stat& st)
{
  return (S_ISDIR(st.st_mode) ? FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY : 0) |
         (S_ISLNK(st.st_mode) ? FILESYSTEM_FILE_ATTRIBUTE_LINK : 0);
}

static u32 RecursiveFindFiles(const char* OriginPath, const char* ParentPath, const char* Path, const char* Pattern,
                              u32 Flags, FileSystem::FindResultsArray* pResults, std::vector<std::string>& visited)
{
  std::string tempStr;
  if (Path)
  {
    if (ParentPath)
      tempStr = fmt::format("{}/{}/{}", OriginPath, ParentPath, Path);
    else
      tempStr = fmt::format("{}/{}", OriginPath, Path);
  }
  else
  {
    tempStr = fmt::format("{}", OriginPath);
  }

  DIR* pDir = opendir(tempStr.c_str());
  if (!pDir)
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
    if (pDirEnt->d_name[0] == '.')
    {
      if (pDirEnt->d_name[1] == '\0' || (pDirEnt->d_name[1] == '.' && pDirEnt->d_name[2] == '\0'))
        continue;

      if (!(Flags & FILESYSTEM_FIND_HIDDEN_FILES))
        continue;
    }

    std::string full_path;
    if (ParentPath)
      full_path = fmt::format("{}/{}/{}/{}", OriginPath, ParentPath, Path, pDirEnt->d_name);
    else if (Path)
      full_path = fmt::format("{}/{}/{}", OriginPath, Path, pDirEnt->d_name);
    else
      full_path = fmt::format("{}/{}", OriginPath, pDirEnt->d_name);

    struct stat sDir;
    if (stat(full_path.c_str(), &sDir) < 0)
      continue;

    FILESYSTEM_FIND_DATA outData;
    outData.Attributes = TranslateStatAttributes(sDir);

    if (S_ISDIR(sDir.st_mode))
    {
      if (Flags & FILESYSTEM_FIND_RECURSIVE)
      {
        // check that we're not following an infinite symbolic link loop
        if (std::string real_recurse_dir = Path::RealPath(full_path);
            real_recurse_dir.empty() || std::find(visited.begin(), visited.end(), real_recurse_dir) == visited.end())
        {
          if (!real_recurse_dir.empty())
            visited.push_back(std::move(real_recurse_dir));

          // recurse into this directory
          if (ParentPath)
          {
            const std::string recursive_dir = fmt::format("{}/{}", ParentPath, Path);
            nFiles +=
              RecursiveFindFiles(OriginPath, recursive_dir.c_str(), pDirEnt->d_name, Pattern, Flags, pResults, visited);
          }
          else
          {
            nFiles += RecursiveFindFiles(OriginPath, Path, pDirEnt->d_name, Pattern, Flags, pResults, visited);
          }
        }
      }

      if (!(Flags & FILESYSTEM_FIND_FOLDERS))
        continue;
    }
    else
    {
      if (!(Flags & FILESYSTEM_FIND_FILES))
        continue;
    }

    outData.Size = static_cast<u64>(sDir.st_size);
    outData.CreationTime = sDir.st_ctime;
    outData.ModificationTime = sDir.st_mtime;

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
    if (!(Flags & FILESYSTEM_FIND_RELATIVE_PATHS))
    {
      outData.FileName = std::move(full_path);
    }
    else
    {
      if (ParentPath)
        outData.FileName = fmt::format("{}/{}/{}", ParentPath, Path, pDirEnt->d_name);
      else if (Path)
        outData.FileName = fmt::format("{}/{}", Path, pDirEnt->d_name);
      else
        outData.FileName = pDirEnt->d_name;
    }

    nFiles++;
    pResults->push_back(std::move(outData));
  }

  closedir(pDir);
  return nFiles;
}

bool FileSystem::FindFiles(const char* path, const char* pattern, u32 flags, FindResultsArray* results)
{
  // clear result array
  if (!(flags & FILESYSTEM_FIND_KEEP_ARRAY))
    results->clear();

  // add self if recursive, we don't want to visit it twice
  std::vector<std::string> visited;
  if (flags & FILESYSTEM_FIND_RECURSIVE)
  {
    std::string real_path = Path::RealPath(path);
    if (!real_path.empty())
      visited.push_back(std::move(real_path));
  }

  // enter the recursive function
  if (RecursiveFindFiles(path, nullptr, nullptr, pattern, flags, results, visited) == 0)
    return false;

  if (flags & FILESYSTEM_FIND_SORT_BY_NAME)
  {
    std::sort(results->begin(), results->end(), [](const FILESYSTEM_FIND_DATA& lhs, const FILESYSTEM_FIND_DATA& rhs) {
      // directories first
      if ((lhs.Attributes & FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY) !=
          (rhs.Attributes & FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY))
      {
        return ((lhs.Attributes & FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY) != 0);
      }

      return (StringUtil::Strcasecmp(lhs.FileName.c_str(), rhs.FileName.c_str()) < 0);
    });
  }

  return true;
}

bool FileSystem::StatFile(const char* path, struct stat* st, Error* error)
{
  if (stat(path, st) != 0)
  {
    Error::SetErrno(error, "stat() failed: ", errno);
    return false;
  }

  return true;
}

bool FileSystem::StatFile(std::FILE* fp, struct stat* st, Error* error)
{
  const int fd = fileno(fp);
  if (fd < 0)
  {
    Error::SetErrno(error, "fileno() failed: ", errno);
    return false;
  }

  if (fstat(fd, st) != 0)
  {
    Error::SetErrno(error, "fstat() failed: ", errno);
    return false;
  }

  return true;
}

bool FileSystem::StatFile(const char* path, FILESYSTEM_STAT_DATA* sd, Error* error)
{
  // stat file
  struct stat sysStatData;
  if (stat(path, &sysStatData) < 0)
  {
    Error::SetErrno(error, "stat() failed: ", errno);
    return false;
  }

  // parse attributes
  sd->CreationTime = sysStatData.st_ctime;
  sd->ModificationTime = sysStatData.st_mtime;
  sd->Attributes = TranslateStatAttributes(sysStatData);
  sd->Size = S_ISREG(sysStatData.st_mode) ? sysStatData.st_size : 0;

  // ok
  return true;
}

bool FileSystem::StatFile(std::FILE* fp, FILESYSTEM_STAT_DATA* sd, Error* error)
{
  const int fd = fileno(fp);
  if (fd < 0)
  {
    Error::SetErrno(error, "fileno() failed: ", errno);
    return false;
  }

  // stat file
  struct stat sysStatData;
  if (fstat(fd, &sysStatData) != 0)
  {
    Error::SetErrno(error, "stat() failed: ", errno);
    return false;
  }

  // parse attributes
  sd->CreationTime = sysStatData.st_ctime;
  sd->ModificationTime = sysStatData.st_mtime;
  sd->Attributes = TranslateStatAttributes(sysStatData);
  sd->Size = S_ISREG(sysStatData.st_mode) ? sysStatData.st_size : 0;

  return true;
}

bool FileSystem::FileExists(const char* path)
{
  struct stat sysStatData;
  if (stat(path, &sysStatData) < 0)
    return false;

  if (S_ISDIR(sysStatData.st_mode))
    return false;
  else
    return true;
}

bool FileSystem::DirectoryExists(const char* path)
{
  struct stat sysStatData;
  if (stat(path, &sysStatData) < 0)
    return false;

  return S_ISDIR(sysStatData.st_mode);
}

bool FileSystem::IsRealDirectory(const char* path)
{
  struct stat sysStatData;
  if (lstat(path, &sysStatData) < 0)
    return false;

  return (S_ISDIR(sysStatData.st_mode) && !S_ISLNK(sysStatData.st_mode));
}

bool FileSystem::IsDirectoryEmpty(const char* path)
{
  DIR* pDir = opendir(path);
  if (pDir == nullptr)
    return true;

  // iterate results
  struct dirent* pDirEnt;
  while ((pDirEnt = readdir(pDir)) != nullptr)
  {
    if (pDirEnt->d_name[0] == '.')
    {
      if (pDirEnt->d_name[1] == '\0' || (pDirEnt->d_name[1] == '.' && pDirEnt->d_name[2] == '\0'))
        continue;
    }

    closedir(pDir);
    return false;
  }

  closedir(pDir);
  return true;
}

bool FileSystem::CreateDirectory(const char* path, bool recursive, Error* error)
{
  // has a path
  const size_t pathLength = std::strlen(path);
  if (pathLength == 0)
    return false;

  // try just flat-out, might work if there's no other segments that have to be made
  if (mkdir(path, 0777) == 0)
    return true;

  // check error
  int lastError = errno;
  if (lastError == EEXIST)
  {
    // check the attributes
    struct stat sysStatData;
    if (stat(path, &sysStatData) == 0 && S_ISDIR(sysStatData.st_mode))
      return true;
  }

  if (!recursive)
  {
    Error::SetErrno(error, "mkdir() failed: ", lastError);
    return false;
  }

  else if (lastError == ENOENT)
  {
    // part of the path does not exist, so we'll create the parent folders, then
    // the full path again.
    std::string tempPath;
    tempPath.reserve(pathLength);

    // create directories along the path
    for (size_t i = 0; i < pathLength; i++)
    {
      if (i > 0 && path[i] == '/')
      {
        if (mkdir(tempPath.c_str(), 0777) < 0)
        {
          lastError = errno;
          if (lastError != EEXIST) // fine, continue to next path segment
          {
            Error::SetErrno(error, "mkdir() failed: ", lastError);
            return false;
          }
        }
      }

      tempPath.push_back(path[i]);
    }

    // re-create the end if it's not a separator, check / as well because windows can interpret them
    if (path[pathLength - 1] != '/')
    {
      if (mkdir(path, 0777) < 0)
      {
        lastError = errno;
        if (lastError != EEXIST)
        {
          Error::SetErrno(error, "mkdir() failed: ", lastError);
          return false;
        }
      }
    }

    // ok
    return true;
  }
  else
  {
    // unhandled error
    Error::SetErrno(error, "mkdir() failed: ", lastError);
    return false;
  }
}

bool FileSystem::DeleteFile(const char* path, Error* error)
{
  struct stat sysStatData;
  if (stat(path, &sysStatData) != 0 || S_ISDIR(sysStatData.st_mode))
  {
    Error::SetStringView(error, "File does not exist.");
    return false;
  }

  if (unlink(path) != 0)
  {
    Error::SetErrno(error, "unlink() failed: ", errno);
    return false;
  }

  return true;
}

bool FileSystem::RenamePath(const char* old_path, const char* new_path, Error* error)
{
  if (rename(old_path, new_path) != 0)
  {
    const int err = errno;
    Error::SetErrno(error, "rename() failed: ", err);
    return false;
  }

  return true;
}

bool FileSystem::DeleteDirectory(const char* path)
{
  struct stat sysStatData;
  if (stat(path, &sysStatData) != 0 || !S_ISDIR(sysStatData.st_mode))
    return false;

  return (rmdir(path) == 0);
}

std::string FileSystem::GetProgramPath()
{
#if defined(__linux__)
  static const char* exe_path = "/proc/self/exe";

  int curSize = PATH_MAX;
  char* buffer = static_cast<char*>(std::realloc(nullptr, curSize));
  for (;;)
  {
    int len = readlink(exe_path, buffer, curSize);
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

#elif defined(__FreeBSD__)
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
  char buffer[PATH_MAX];
  size_t cb = sizeof(buffer) - 1;
  int res = sysctl(mib, std::size(mib), buffer, &cb, nullptr, 0);
  if (res != 0)
    return {};

  buffer[cb] = '\0';
  return buffer;
#else
  return {};
#endif
}

std::string FileSystem::GetWorkingDirectory()
{
  std::string buffer;
  buffer.resize(PATH_MAX);
  while (!getcwd(buffer.data(), buffer.size()))
  {
    if (errno != ERANGE)
      return {};

    buffer.resize(buffer.size() * 2);
  }

  buffer.resize(std::strlen(buffer.c_str())); // Remove excess nulls
  return buffer;
}

bool FileSystem::SetWorkingDirectory(const char* path)
{
  return (chdir(path) == 0);
}

bool FileSystem::SetPathCompression(const char* path, bool enable)
{
  return false;
}

#endif

#ifdef HAS_POSIX_FILE_LOCK

static bool SetLock(int fd, bool lock, bool block, Error* error)
{
  // We want to lock the whole file.
  const off_t offs = lseek(fd, 0, SEEK_CUR);
  if (offs < 0)
  {
    if (error)
      error->SetErrno("lseek() failed: ", errno);
    else
      ERROR_LOG("lseek({}) failed: {}", fd, errno);
    return false;
  }

  if (offs != 0 && lseek(fd, 0, SEEK_SET) < 0)
  {
    if (error)
      error->SetErrno("lseek(0) failed: ", errno);
    else
      ERROR_LOG("lseek({}, 0) failed: {}", fd, errno);
    return false;
  }

  // bloody signals...
  bool res;
  for (;;)
  {
    res = (lockf(fd, lock ? (block ? F_TLOCK : F_LOCK) : F_ULOCK, 0) == 0);
    if (!res && errno == EINTR)
      continue;
    else
      break;
  }

  if (!res)
  {
    if (error)
      error->SetErrno("lockf() failed: ", errno);
    else
      ERROR_LOG("lockf() for {} failed: {}", lock ? "lock" : "unlock", errno);
  }

  if (lseek(fd, offs, SEEK_SET) < 0)
    Panic("Repositioning file descriptor after lock failed.");

  return res;
}

FileSystem::POSIXLock::POSIXLock() : m_fd(-1)
{
}

FileSystem::POSIXLock::POSIXLock(int fd, bool block, Error* error) : m_fd(fd)
{
  if (!SetLock(m_fd, true, block, error))
    m_fd = -1;
}

FileSystem::POSIXLock::POSIXLock(std::FILE* fp, bool block, Error* error) : m_fd(fileno(fp))
{
  if (!SetLock(m_fd, true, block, error))
    m_fd = -1;
}

FileSystem::POSIXLock::POSIXLock(POSIXLock&& move)
{
  m_fd = std::exchange(move.m_fd, -1);
}

FileSystem::POSIXLock::~POSIXLock()
{
  Unlock();
}

void FileSystem::POSIXLock::Unlock()
{
  if (m_fd >= 0)
  {
    SetLock(m_fd, false, true, nullptr);
    m_fd = -1;
  }
}

FileSystem::POSIXLock& FileSystem::POSIXLock::operator=(POSIXLock&& move)
{
  m_fd = std::exchange(move.m_fd, -1);
  return *this;
}

#endif
