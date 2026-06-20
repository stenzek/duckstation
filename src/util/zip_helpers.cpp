// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "zip_helpers.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/progress_callback.h"

#include <fmt/format.h>
#include <zip.h>

LOG_CHANNEL(Ungrouped);

void ZipHelpers::ZipFileDeleter::operator()(zip_file_t* zf)
{
  if (!zf)
    return;

  zip_fclose(zf);
}

void ZipHelpers::ZipDeleter::operator()(zip_t* zf)
{
  if (!zf)
    return;

  const int err = zip_close(zf);
  if (err != 0)
  {
    ERROR_LOG("Failed to close zip file: {}", err);
    zip_discard(zf);
  }
}

void ZipHelpers::SetErrorObject(Error* error, std::string_view msg, zip_error_t* ze, bool finalize /*= true*/)
{
  Error::SetStringFmt(error, "{}{}", msg, ze ? zip_error_strerror(ze) : "UNKNOWN");
  if (finalize && ze)
    zip_error_fini(ze);
}

ZipHelpers::ManagedZipT ZipHelpers::OpenManagedZipFile(const char* filename, int flags, Error* error /*= nullptr*/)
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

namespace ZipHelpers {
static zip_int64_t CFileSourceCallback(void* userdata, void* data, zip_uint64_t len, zip_source_cmd_t cmd)
{
  std::FILE* fp = static_cast<std::FILE*>(userdata);
  switch (cmd)
  {
    case ZIP_SOURCE_OPEN:
    {
      // file already open
      return 0;
    }

    case ZIP_SOURCE_READ:
    {
      return std::fread(data, 1, static_cast<size_t>(len), fp);
    }

    case ZIP_SOURCE_CLOSE:
    {
      // file closing is caller's responsibility
      return 0;
    }

    case ZIP_SOURCE_STAT:
    {
      FILESYSTEM_STAT_DATA st;
      zip_stat_t* zst = ZIP_SOURCE_GET_ARGS(zip_stat_t, data, len, nullptr);
      if (!zst || !FileSystem::StatFile(fp, &st))
        return -1;

      zst->size = st.Size;
      zst->mtime = st.ModificationTime;
      zst->valid = ZIP_STAT_SIZE | ZIP_STAT_MTIME;
      return 0;
    }

    case ZIP_SOURCE_SEEK:
    {
      const zip_source_args_seek_t* args = ZIP_SOURCE_GET_ARGS(zip_source_args_seek_t, data, len, nullptr);
      if (!args)
        return -1;

      return FileSystem::FSeek64(fp, args->offset, args->whence);
    }

    case ZIP_SOURCE_TELL:
    {
      return FileSystem::FTell64(fp);
    }

    case ZIP_SOURCE_FREE:
    {
      return 0;
    }

    case ZIP_SOURCE_SUPPORTS:
    {
      // we support all commands used by libzip for reading
      return ZIP_SOURCE_SUPPORTS_SEEKABLE;
    }

    default:
      return -1;
  }
}
} // namespace ZipHelpers

ZipHelpers::ManagedZipT ZipHelpers::OpenManagedZipCFile(std::FILE* fp, int flags, Error* error /*= nullptr*/)
{
  zip_error_t ze;
  zip_t* zip;
  zip_source_t* zs = zip_source_function_create(&CFileSourceCallback, fp, &ze);
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

ZipHelpers::ManagedZipT ZipHelpers::OpenManagedZipBuffer(const void* buffer, size_t size, int flags, bool free_buffer,
                                                         Error* error /*= nullptr*/)
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

std::vector<std::string> ZipHelpers::ReadFileListInZip(zip_t* zip)
{
  std::vector<std::string> ret;
  zip_int64_t num_entries = zip_get_num_entries(zip, 0);
  if constexpr (sizeof(size_t) < sizeof(zip_int64_t))
    num_entries = std::min(num_entries, static_cast<zip_int64_t>(std::numeric_limits<size_t>::max()));
  if (num_entries <= 0)
    return ret;

  ret.reserve(static_cast<size_t>(num_entries));
  for (zip_uint64_t i = 0; i < static_cast<zip_uint64_t>(num_entries); i++)
  {
    const char* name = zip_get_name(zip, i, ZIP_FL_ENC_GUESS);
    if (name)
      ret.emplace_back(name);
  }

  return ret;
}

ZipHelpers::ManagedZipFileT ZipHelpers::OpenManagedFileInZip(zip_t* zip, const char* filename, u32 flags,
                                                             Error* error /*= nullptr*/)
{
  zip_file_t* zf = zip_fopen(zip, filename, flags);
  if (!zf)
    SetErrorObject(error, "zip_fopen() failed: ", zip_get_error(zip), false);
  return ManagedZipFileT(zf);
}

ZipHelpers::ManagedZipFileT ZipHelpers::OpenManagedFileIndexInZip(zip_t* zip, u64 index, u32 flags,
                                                                  Error* error /*= nullptr*/)
{
  zip_file_t* zf = zip_fopen_index(zip, index, flags);
  if (!zf)
    SetErrorObject(error, "zip_fopen_index() failed: ", zip_get_error(zip), false);
  return ManagedZipFileT(zf);
}

std::optional<u64> ZipHelpers::GetFileSizeInZip(zip_t* zip, const char* name, bool case_sensitive /*= true*/,
                                                Error* error /*= nullptr*/)
{
  zip_stat_t st;
  if (zip_stat(zip, name, 0, &st) != 0)
  {
    SetErrorObject(error, "zip_stat() failed: ", zip_get_error(zip));
    return std::nullopt;
  }

  if (!(st.valid & ZIP_STAT_SIZE) || st.size < 0)
  {
    Error::SetStringView(error, "zip_stat() did not return valid size for file");
    return std::nullopt;
  }

  return static_cast<u64>(st.size);
}

bool ZipHelpers::ExtractFileToDisk(zip_t* zip, const char* name, std::string disk_name, bool case_sensitive /*= true*/,
                                   u32 chunk_size /*= DEFAULT_EXTRACT_CHUNK_SIZE*/,
                                   ProgressCallback* progress /*= nullptr*/, Error* error /*= nullptr*/)
{
  std::unique_ptr<u8[]> chunk = std::make_unique_for_overwrite<u8[]>(chunk_size);
  return ExtractFileToDisk(zip, name, std::move(disk_name), std::span<u8>(chunk.get(), chunk_size), case_sensitive,
                           progress, error);
}

bool ZipHelpers::ExtractFileToDisk(zip_t* zip, const char* name, std::string disk_name, std::span<u8> chunk_buffer,
                                   bool case_sensitive /*= true */, ProgressCallback* progress /*= nullptr*/,
                                   Error* error /*= nullptr*/)
{
  auto fp = FileSystem::CreateAtomicRenamedFile(std::move(disk_name), error);
  if (!fp)
    return false;

  if (!ExtractFileToDisk(zip, name, fp.get(), chunk_buffer, case_sensitive, progress, error))
  {
    FileSystem::DiscardAtomicRenamedFile(fp);
    return false;
  }

  return FileSystem::CommitAtomicRenamedFile(fp, error);
}

bool ZipHelpers::ExtractFileToDisk(zip_t* zip, const char* name, std::FILE* fp, std::span<u8> chunk_buffer,
                                   bool case_sensitive /* = true */, ProgressCallback* progress /* = nullptr */,
                                   Error* error /* = nullptr */)
{
  const int flags = case_sensitive ? 0 : ZIP_FL_NOCASE;

  const zip_int64_t file_index = zip_name_locate(zip, name, flags);
  if (file_index < 0)
  {
    SetErrorObject(error, "zip_name_locate() failed: ", zip_get_error(zip), false);
    return false;
  }

  zip_file_t* zf = zip_fopen_index(zip, file_index, flags);
  if (!zf)
  {
    SetErrorObject(error, "zip_fopen_index() failed: ", zip_get_error(zip), false);
    return false;
  }

  const u64 chunk_size = static_cast<u64>(chunk_buffer.size());
  bool update_progress = (progress != nullptr);
  if (update_progress)
  {
    zip_stat_t zst;
    update_progress =
      (zip_stat_index(zip, file_index, flags, &zst) == 0 && (zst.valid & ZIP_STAT_SIZE) && zst.size > 0);
    if (update_progress)
    {
      progress->PushState();
      progress->SetState(0, static_cast<u32>((zst.size + chunk_size - 1) / chunk_size));
    }
  }

  for (;;)
  {
    const s64 read = zip_fread(zf, chunk_buffer.data(), chunk_size);
    if (read < 0)
    {
      // read error
      SetErrorObject(error, "zip_fread() failed: ", zip_get_error(zip), false);
      zip_fclose(zf);

      if (update_progress)
        progress->PopState();

      break;
    }

    if (std::fwrite(chunk_buffer.data(), static_cast<size_t>(read), 1, fp) != 1)
    {
      // write error
      Error::SetErrno(error, "fwrite() failed: ", errno);
      zip_fclose(zf);

      if (update_progress)
        progress->PopState();

      break;
    }

    if (update_progress)
      progress->IncrementProgressValue();

    // if less than chunk size, we're EOF
    if (read != static_cast<s64>(chunk_size))
      break;
  }

  if (update_progress)
    progress->PopState();

  zip_fclose(zf);

  if (std::fflush(fp) != 0)
  {
    Error::SetErrno(error, "fflush() failed: ", errno);
    return false;
  }

  return true;
}

namespace ZipHelpers {

template<typename T>
static std::optional<T> ReadFileInZipToContainer(zip_t* zip, const char* name, bool case_sensitive = true,
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
          SetErrorObject(error, "zip_fread() failed: ", zip_get_error(zip), false);
          ret.reset();
        }

        zip_fclose(zf);
      }
    }
    else
    {
      SetErrorObject(error, "zip_stat_index() failed: ", zip_get_error(zip), false);
    }
  }
  else
  {
    SetErrorObject(error, "zip_name_locate() failed: ", zip_get_error(zip), false);
  }

  return ret;
}

template<typename T>
static std::optional<T> ReadFileInZipToContainer(zip_file_t* file, u32 chunk_size = 4096, Error* error = nullptr)
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

} // namespace ZipHelpers

std::optional<std::string> ZipHelpers::ReadFileInZipToString(zip_t* zip, const char* name,
                                                             bool case_sensitive /*= true*/, Error* error /*= nullptr*/)
{
  return ReadFileInZipToContainer<std::string>(zip, name, case_sensitive, error);
}

std::optional<std::vector<u8>> ZipHelpers::ReadBinaryFileInZip(zip_t* zip, const char* name,
                                                               bool case_sensitive /*= true*/,
                                                               Error* error /*= nullptr*/)
{
  return ReadFileInZipToContainer<std::vector<u8>>(zip, name, case_sensitive, error);
}

std::optional<std::vector<u8>> ZipHelpers::ReadBinaryFileInZip(zip_file_t* file,
                                                               u32 chunk_size /*= DEFAULT_READ_CHUNK_SIZE*/,
                                                               Error* error /*= nullptr*/)
{
  return ReadFileInZipToContainer<std::vector<u8>>(file, chunk_size, error);
}

std::optional<std::string> ZipHelpers::ReadFileInZipToString(zip_file_t* file,
                                                             u32 chunk_size /*= DEFAULT_READ_CHUNK_SIZE*/,
                                                             Error* error /*= nullptr*/)
{
  return ReadFileInZipToContainer<std::string>(file, chunk_size, error);
}
