// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "error.h"
#include "log.h"

#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <zip.h>

namespace ZipHelpers {

inline void SetErrorObject(Error* error, std::string_view msg, zip_error_t* ze)
{
  Error::SetStringFmt(error, "{}{}", msg, ze ? zip_error_strerror(ze) : "UNKNOWN");
  if (ze)
    zip_error_fini(ze);
}

struct ZipDeleter
{
  void operator()(zip_t* zf)
  {
    if (!zf)
      return;

    const int err = zip_close(zf);
    if (err != 0)
    {
      GENERIC_LOG(Log::Channel::Ungrouped, Log::Level::Error, Log::Color::Default, "Failed to close zip file: {}", err);
      zip_discard(zf);
    }
  }
};

struct ZipFileDeleter
{
  void operator()(zip_file_t* zf)
  {
    if (!zf)
      return;

    zip_fclose(zf);
  }
};

using ManagedZipT = std::unique_ptr<zip_t, ZipDeleter>;
using ManagedZipFileT = std::unique_ptr<zip_file_t, ZipFileDeleter>;

inline ManagedZipT OpenManagedZipFile(const char* filename, int flags, Error* error = nullptr)
{
  zip_error_t ze;
  zip_source_t* zs = zip_source_file_create(filename, 0, 0, &ze);
  zip_t* zip;
  if (!zs)
  {
    SetErrorObject(error, "zip_source_file_create() failed: ", &ze);
    zip = nullptr;
  }
  else
  {
    if (!(zip = zip_open_from_source(zs, flags, &ze)))
    {
      // have to clean up source
      SetErrorObject(error, "zip_open_from_source() failed: {}", &ze);
      zip_source_free(zs);
    }
  }

  return ManagedZipT(zip);
}

inline ManagedZipT OpenManagedZipCFile(std::FILE* fp, int flags, Error* error = nullptr)
{
  zip_error_t ze;
  zip_source_t* zs = zip_source_filep_create(fp, 0, 0, &ze);
  zip_t* zip;
  if (!zs)
  {
    SetErrorObject(error, "zip_source_filep_create() failed: ", &ze);
    std::fclose(fp);
    zip = nullptr;
  }
  else
  {
    if (!(zip = zip_open_from_source(zs, flags, &ze)))
    {
      // have to clean up source
      SetErrorObject(error, "zip_open_from_source() failed: {}", &ze);
      zip_source_free(zs);
    }
  }

  return ManagedZipT(zip);
}

inline ManagedZipT OpenManagedZipBuffer(const void* buffer, size_t size, int flags, bool free_buffer,
                                        Error* error = nullptr)
{
  zip_error_t ze;
  zip_source_t* zs = zip_source_buffer_create(buffer, size, free_buffer, &ze);
  zip_t* zip;
  if (!zs)
  {
    SetErrorObject(error, "zip_source_buffer_create() failed: ", &ze);
    if (free_buffer)
      std::free(const_cast<void*>(buffer));
    zip = nullptr;
  }
  else
  {
    if (!(zip = zip_open_from_source(zs, flags, &ze)))
    {
      // have to clean up source
      SetErrorObject(error, "zip_open_from_source() failed: {}", &ze);
      zip_source_free(zs);
    }
  }

  return ManagedZipT(zip);
}

inline ManagedZipFileT OpenManagedFileInZip(zip_t* zip, const char* filename, zip_flags_t flags, Error* error = nullptr)
{
  zip_file_t* zf = zip_fopen(zip, filename, flags);
  if (!zf)
    SetErrorObject(error, "zip_fopen() failed: ", zip_get_error(zip));
  return ManagedZipFileT(zf);
}

inline ManagedZipFileT OpenManagedFileIndexInZip(zip_t* zip, zip_uint64_t index, zip_flags_t flags,
                                                 Error* error = nullptr)
{
  zip_file_t* zf = zip_fopen_index(zip, index, flags);
  if (!zf)
    SetErrorObject(error, "zip_fopen_index() failed: ", zip_get_error(zip));
  return ManagedZipFileT(zf);
}

template<typename T>
inline std::optional<T> ReadFileInZipToContainer(zip_t* zip, const char* name, bool case_sensitive = true,
                                                 Error* error = nullptr)
{
  const int flags = case_sensitive ? 0 : ZIP_FL_NOCASE;

  std::optional<T> ret;
  const zip_int64_t file_index = zip_name_locate(zip, name, flags);
  if (file_index >= 0)
  {
    zip_stat_t zst;
    if (zip_stat_index(zip, file_index, flags, &zst) == 0)
    {
      zip_file_t* zf = zip_fopen_index(zip, file_index, flags);
      if (zf)
      {
        ret = T();
        ret->resize(static_cast<size_t>(zst.size));
        if (zip_fread(zf, ret->data(), ret->size()) != static_cast<zip_int64_t>(ret->size()))
        {
          SetErrorObject(error, "zip_fread() failed: ", zip_get_error(zip));
          ret.reset();
        }

        zip_fclose(zf);
      }
    }
    else
    {
      SetErrorObject(error, "zip_stat_index() failed: ", zip_get_error(zip));
    }
  }
  else
  {
    SetErrorObject(error, "zip_name_locate() failed: ", zip_get_error(zip));
  }

  return ret;
}

template<typename T>
inline std::optional<T> ReadFileInZipToContainer(zip_file_t* file, u32 chunk_size = 4096, Error* error = nullptr)
{
  std::optional<T> ret = T();
  for (;;)
  {
    const size_t pos = ret->size();
    ret->resize(pos + chunk_size);
    const s64 read = zip_fread(file, ret->data() + pos, chunk_size);
    if (read < 0)
    {
      // read error
      Error::SetStringView(error, "zip_fread() failed");
      break;
    }

    // if less than chunk size, we're EOF
    if (read != static_cast<s64>(chunk_size))
    {
      ret->resize(pos + static_cast<size_t>(read));
      break;
    }
  }

  return ret;
}

inline std::optional<std::string> ReadFileInZipToString(zip_t* zip, const char* name, bool case_sensitive = true,
                                                        Error* error = nullptr)
{
  return ReadFileInZipToContainer<std::string>(zip, name, case_sensitive, error);
}

inline std::optional<std::string> ReadFileInZipToString(zip_file_t* file, u32 chunk_size = 4096, Error* error = nullptr)
{
  return ReadFileInZipToContainer<std::string>(file, chunk_size, error);
}

inline std::optional<std::vector<u8>> ReadBinaryFileInZip(zip_t* zip, const char* name, bool case_sensitive = true,
                                                          Error* error = nullptr)
{
  return ReadFileInZipToContainer<std::vector<u8>>(zip, name, case_sensitive, error);
}

inline std::optional<std::vector<u8>> ReadBinaryFileInZip(zip_file_t* file, u32 chunk_size = 4096,
                                                          Error* error = nullptr)
{
  return ReadFileInZipToContainer<std::vector<u8>>(file, chunk_size, error);
}

} // namespace ZipHelpers