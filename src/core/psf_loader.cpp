#include "psf_loader.h"
#include "common/assert.h"
#include "common/file_system.h"
#include "common/log.h"
#include "zlib.h"
#include <cctype>
#include <cstring>
Log_SetChannel(PSFLoader);

namespace PSFLoader {

std::string File::GetTagString(const char* tag_name, const char* default_value) const
{
  auto it = m_tags.find(tag_name);
  if (it == m_tags.end())
    return default_value;

  return it->second;
}

int File::GetTagInt(const char* tag_name, int default_value) const
{
  auto it = m_tags.find(tag_name);
  if (it == m_tags.end())
    return default_value;

  return std::atoi(it->second.c_str());
}

float File::GetTagFloat(const char* tag_name, float default_value) const
{
  auto it = m_tags.find(tag_name);
  if (it == m_tags.end())
    return default_value;

  return static_cast<float>(std::atof(it->second.c_str()));
}

bool File::Load(const char* path)
{
  auto fp = FileSystem::OpenManagedCFile(path, "rb");
  if (!fp)
    return false;

  // we could mmap this instead
  std::fseek(fp.get(), 0, SEEK_END);
  const u32 file_size = static_cast<u32>(std::ftell(fp.get()));
  std::fseek(fp.get(), 0, SEEK_SET);

  std::vector<u8> file_data(file_size);
  if (std::fread(file_data.data(), 1, file_size, fp.get()) != file_size)
  {
    Log_ErrorPrintf("Failed to read data from PSF '%s'", path);
    return false;
  }

  const u8* file_pointer = file_data.data();
  const u8* file_pointer_end = file_data.data() + file_data.size();

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
        Log_InfoPrintf("PSF Tag: '%s' = '%s'", tag_key.c_str(), tag_value.c_str());
        m_tags.emplace(std::move(tag_key), std::move(tag_value));
      }
    }
  }

  return true;
}

} // namespace PSFLoader