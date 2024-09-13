// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "psf_loader.h"
#include "bios.h"
#include "bus.h"
#include "system.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"

#include "zlib.h"

#include <cstring>

Log_SetChannel(PSFLoader);

namespace PSFLoader {
static bool LoadLibraryPSF(const std::string& path, bool use_pc_sp, Error* error, u32 depth = 0);
}

std::optional<std::string> PSFLoader::File::GetTagString(const char* tag_name) const
{
  auto it = m_tags.find(tag_name);
  if (it == m_tags.end())
    return std::nullopt;

  return it->second;
}

std::optional<int> PSFLoader::File::GetTagInt(const char* tag_name) const
{
  auto it = m_tags.find(tag_name);
  if (it == m_tags.end())
    return std::nullopt;

  return std::atoi(it->second.c_str());
}

std::optional<float> PSFLoader::File::GetTagFloat(const char* tag_name) const
{
  auto it = m_tags.find(tag_name);
  if (it == m_tags.end())
    return std::nullopt;

  return static_cast<float>(std::atof(it->second.c_str()));
}

std::string PSFLoader::File::GetTagString(const char* tag_name, const char* default_value) const
{
  std::optional<std::string> value(GetTagString(tag_name));
  if (value.has_value())
    return value.value();

  return default_value;
}

int PSFLoader::File::GetTagInt(const char* tag_name, int default_value) const
{
  return GetTagInt(tag_name).value_or(default_value);
}

float PSFLoader::File::GetTagFloat(const char* tag_name, float default_value) const
{
  return GetTagFloat(tag_name).value_or(default_value);
}

bool PSFLoader::File::Load(const char* path, Error* error)
{
  std::optional<DynamicHeapArray<u8>> file_data(FileSystem::ReadBinaryFile(path, error));
  if (!file_data.has_value() || file_data->empty())
    return false;

  const u8* file_pointer = file_data->data();
  const u8* file_pointer_end = file_data->data() + file_data->size();
  const u32 file_size = static_cast<u32>(file_data->size());

  PSFHeader header;
  std::memcpy(&header, file_pointer, sizeof(header));
  file_pointer += sizeof(header);
  if (header.id[0] != 'P' || header.id[1] != 'S' || header.id[2] != 'F' || header.version != 0x01 ||
      header.compressed_program_size == 0 ||
      (sizeof(header) + header.reserved_area_size + header.compressed_program_size) > file_size)
  {
    Error::SetStringView(error, "Invalid or incompatible PSF header.");
    return false;
  }

  file_pointer += header.reserved_area_size;

  m_program_data.resize(MAX_PROGRAM_SIZE);

  z_stream strm = {};
  strm.avail_in = static_cast<uInt>(file_pointer_end - file_pointer);
  strm.next_in = static_cast<Bytef*>(const_cast<u8*>(file_pointer));
  strm.avail_out = static_cast<uInt>(m_program_data.size());
  strm.next_out = static_cast<Bytef*>(m_program_data.data());

  int err = inflateInit(&strm);
  if (err != Z_OK)
  {
    Error::SetStringFmt(error, "inflateInit() failed: {}", err);
    return false;
  }

  // we can do this in one pass because we preallocate the max size
  err = inflate(&strm, Z_NO_FLUSH);
  if (err != Z_STREAM_END)
  {
    Error::SetStringFmt(error, "inflate() failed: {}", err);
    inflateEnd(&strm);
    return false;
  }
  else if (strm.total_in != header.compressed_program_size)
  {
    WARNING_LOG("Mismatch between compressed size in header and stream {}/{}", header.compressed_program_size,
                static_cast<u32>(strm.total_in));
  }

  m_program_data.resize(strm.total_out);
  file_pointer += header.compressed_program_size;
  inflateEnd(&strm);

  u32 remaining_tag_data = static_cast<u32>(file_pointer_end - file_pointer);
  static constexpr char tag_signature[] = {'[', 'T', 'A', 'G', ']'};
  if (remaining_tag_data >= sizeof(tag_signature) &&
      std::memcmp(file_pointer, tag_signature, sizeof(tag_signature)) == 0)
  {
    file_pointer += sizeof(tag_signature);

    while (file_pointer < file_pointer_end)
    {
      // skip whitespace
      while (file_pointer < file_pointer_end && *file_pointer <= 0x20)
        file_pointer++;

      std::string tag_key;
      while (file_pointer < file_pointer_end && *file_pointer != '=')
        tag_key += (static_cast<char>(*(file_pointer++)));

      // skip =
      if (file_pointer < file_pointer_end)
        file_pointer++;

      std::string tag_value;
      while (file_pointer < file_pointer_end && *file_pointer != '\n')
        tag_value += (static_cast<char>(*(file_pointer++)));

      if (!tag_key.empty())
      {
        DEV_LOG("PSF Tag: '{}' = '{}'", tag_key, tag_value);
        m_tags.emplace(std::move(tag_key), std::move(tag_value));
      }
    }
  }

  // Region detection.
  m_region = BIOS::GetPSExeDiscRegion(*reinterpret_cast<const BIOS::PSEXEHeader*>(m_program_data.data()));

  // _refresh tag takes precedence.
  const int refresh_tag = GetTagInt("_region", 0);
  if (refresh_tag == 60)
    m_region = DiscRegion::NTSC_U;
  else if (refresh_tag == 50)
    m_region = DiscRegion::PAL;

  return true;
}

bool PSFLoader::LoadLibraryPSF(const std::string& path, bool use_pc_sp, Error* error, u32 depth)
{
  // don't recurse past 10 levels just in case of broken files
  if (depth >= 10)
  {
    Error::SetStringFmt(error, "Recursion depth exceeded when loading PSF '{}'", Path::GetFileName(path));
    return false;
  }

  File file;
  if (!file.Load(path.c_str(), error))
  {
    Error::AddPrefixFmt(error, "Failed to load {} PSF '{}': ", (depth == 0) ? "main" : "parent",
                        Path::GetFileName(path));
    return false;
  }

  // load the main parent library - this has to be done first so the specified PSF takes precedence
  std::optional<std::string> lib_name(file.GetTagString("_lib"));
  if (lib_name.has_value())
  {
    const std::string lib_path = Path::BuildRelativePath(path, lib_name.value());
    INFO_LOG("Loading parent PSF '{}'", Path::GetFileName(lib_path));

    // We should use the initial SP/PC from the **first** parent lib.
    const bool lib_use_pc_sp = (depth == 0);
    if (!LoadLibraryPSF(lib_path.c_str(), lib_use_pc_sp, error, depth + 1))
      return false;

    // Don't apply the PC/SP from the minipsf file.
    if (lib_use_pc_sp)
      use_pc_sp = false;
  }

  // apply the main psf
  if (!Bus::InjectExecutable(file.GetProgramData(), use_pc_sp, error))
  {
    Error::AddPrefixFmt(error, "Failed to inject {} PSF '{}': ", (depth == 0) ? "main" : "parent",
                        Path::GetFileName(path));
    return false;
  }

  // load any other parent psfs
  u32 lib_counter = 2;
  for (;;)
  {
    lib_name = file.GetTagString(TinyString::from_format("_lib{}", lib_counter++));
    if (!lib_name.has_value())
      break;

    const std::string lib_path = Path::BuildRelativePath(path, lib_name.value());
    INFO_LOG("Loading parent PSF '{}'", Path::GetFileName(lib_path));
    if (!LoadLibraryPSF(lib_path.c_str(), false, error, depth + 1))
      return false;
  }

  return true;
}

bool PSFLoader::Load(const std::string& path, Error* error)
{
  INFO_LOG("Loading PSF file from '{}'", path);
  return LoadLibraryPSF(path, true, error);
}
