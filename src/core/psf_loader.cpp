#include "psf_loader.h"
#include "bios.h"
#include "common/assert.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "system.h"
#include "zlib.h"
#include <cctype>
#include <cstring>
Log_SetChannel(PSFLoader);

namespace PSFLoader {

std::optional<std::string> File::GetTagString(const char* tag_name) const
{
  auto it = m_tags.find(tag_name);
  if (it == m_tags.end())
    return std::nullopt;

  return it->second;
}

std::optional<int> File::GetTagInt(const char* tag_name) const
{
  auto it = m_tags.find(tag_name);
  if (it == m_tags.end())
    return std::nullopt;

  return std::atoi(it->second.c_str());
}

std::optional<float> File::GetTagFloat(const char* tag_name) const
{
  auto it = m_tags.find(tag_name);
  if (it == m_tags.end())
    return std::nullopt;

  return static_cast<float>(std::atof(it->second.c_str()));
}

std::string File::GetTagString(const char* tag_name, const char* default_value) const
{
  std::optional<std::string> value(GetTagString(tag_name));
  if (value.has_value())
    return value.value();

  return default_value;
}

int File::GetTagInt(const char* tag_name, int default_value) const
{
  return GetTagInt(tag_name).value_or(default_value);
}

float File::GetTagFloat(const char* tag_name, float default_value) const
{
  return GetTagFloat(tag_name).value_or(default_value);
}

bool File::Load(const char* path)
{
  std::optional<std::vector<u8>> file_data(FileSystem::ReadBinaryFile(path));
  if (!file_data.has_value() || file_data->empty())
  {
    Log_ErrorPrintf("Failed to open/read PSF file '%s'", path);
    return false;
  }

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
    Log_ErrorPrintf("Invalid or incompatible header in PSF '%s'", path);
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
    Log_ErrorPrintf("inflateInit() failed: %d", err);
    return false;
  }

  // we can do this in one pass because we preallocate the max size
  err = inflate(&strm, Z_NO_FLUSH);
  if (err != Z_STREAM_END)
  {
    Log_ErrorPrintf("inflate() failed: %d", err);
    inflateEnd(&strm);
    return false;
  }
  else if (strm.total_in != header.compressed_program_size)
  {
    Log_WarningPrintf("Mismatch between compressed size in header and stream %u/%u", header.compressed_program_size,
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
        Log_DevPrintf("PSF Tag: '%s' = '%s'", tag_key.c_str(), tag_value.c_str());
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

static bool LoadLibraryPSF(const char* path, bool use_pc_sp, u32 depth = 0)
{
  // don't recurse past 10 levels just in case of broken files
  if (depth >= 10)
  {
    Log_ErrorPrintf("Recursion depth exceeded when loading PSF '%s'", path);
    return false;
  }

  File file;
  if (!file.Load(path))
  {
    Log_ErrorPrintf("Failed to load main PSF '%s'", path);
    return false;
  }

  // load the main parent library - this has to be done first so the specified PSF takes precedence
  std::optional<std::string> lib_name(file.GetTagString("_lib"));
  if (lib_name.has_value())
  {
    const std::string lib_path(Path::BuildRelativePath(path, lib_name.value()));
    Log_InfoPrintf("Loading main parent PSF '%s'", lib_path.c_str());

    // We should use the initial SP/PC from the **first** parent lib.
    const bool lib_use_pc_sp = (depth == 0);
    if (!LoadLibraryPSF(lib_path.c_str(), lib_use_pc_sp, depth + 1))
    {
      Log_ErrorPrintf("Failed to load main parent PSF '%s'", lib_path.c_str());
      return false;
    }

    // Don't apply the PC/SP from the minipsf file.
    if (lib_use_pc_sp)
      use_pc_sp = false;
  }

  // apply the main psf
  if (!System::InjectEXEFromBuffer(file.GetProgramData().data(), static_cast<u32>(file.GetProgramData().size()),
                                   use_pc_sp))
  {
    Log_ErrorPrintf("Failed to parse EXE from PSF '%s'", path);
    return false;
  }

  // load any other parent psfs
  u32 lib_counter = 2;
  for (;;)
  {
    lib_name = file.GetTagString(TinyString::FromFormat("_lib%u", lib_counter++));
    if (!lib_name.has_value())
      break;

    const std::string lib_path(Path::BuildRelativePath(path, lib_name.value()));
    Log_InfoPrintf("Loading parent PSF '%s'", lib_path.c_str());
    if (!LoadLibraryPSF(lib_path.c_str(), false, depth + 1))
    {
      Log_ErrorPrintf("Failed to load parent PSF '%s'", lib_path.c_str());
      return false;
    }
  }

  return true;
}

bool Load(const char* path)
{
  Log_InfoPrintf("Loading PSF file from '%s'", path);
  return LoadLibraryPSF(path, true);
}

} // namespace PSFLoader