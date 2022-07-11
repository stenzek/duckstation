#include "shader_cache.h"
#include "../file_system.h"
#include "../log.h"
#include "../md5_digest.h"
#include "../path.h"
#include "../string_util.h"
Log_SetChannel(GL::ShaderCache);

namespace GL {

#pragma pack(push, 1)
struct CacheIndexEntry
{
  u64 vertex_source_hash_low;
  u64 vertex_source_hash_high;
  u32 vertex_source_length;
  u64 geometry_source_hash_low;
  u64 geometry_source_hash_high;
  u32 geometry_source_length;
  u64 fragment_source_hash_low;
  u64 fragment_source_hash_high;
  u32 fragment_source_length;
  u32 file_offset;
  u32 blob_size;
  u32 blob_format;
};
#pragma pack(pop)

ShaderCache::ShaderCache() = default;

ShaderCache::~ShaderCache()
{
  Close();
}

bool ShaderCache::CacheIndexKey::operator==(const CacheIndexKey& key) const
{
  return (
    vertex_source_hash_low == key.vertex_source_hash_low && vertex_source_hash_high == key.vertex_source_hash_high &&
    vertex_source_length == key.vertex_source_length && geometry_source_hash_low == key.geometry_source_hash_low &&
    geometry_source_hash_high == key.geometry_source_hash_high &&
    geometry_source_length == key.geometry_source_length && fragment_source_hash_low == key.fragment_source_hash_low &&
    fragment_source_hash_high == key.fragment_source_hash_high && fragment_source_length == key.fragment_source_length);
}

bool ShaderCache::CacheIndexKey::operator!=(const CacheIndexKey& key) const
{
  return (
    vertex_source_hash_low != key.vertex_source_hash_low || vertex_source_hash_high != key.vertex_source_hash_high ||
    vertex_source_length != key.vertex_source_length || geometry_source_hash_low != key.geometry_source_hash_low ||
    geometry_source_hash_high != key.geometry_source_hash_high ||
    geometry_source_length != key.geometry_source_length || fragment_source_hash_low != key.fragment_source_hash_low ||
    fragment_source_hash_high != key.fragment_source_hash_high || fragment_source_length != key.fragment_source_length);
}

void ShaderCache::Open(bool is_gles, std::string_view base_path, u32 version)
{
  m_base_path = base_path;
  m_version = version;
  m_program_binary_supported = is_gles || GLAD_GL_ARB_get_program_binary;
  if (m_program_binary_supported)
  {
    // check that there's at least one format and the extension isn't being "faked"
    GLint num_formats = 0;
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &num_formats);
    Log_InfoPrintf("%u program binary formats supported by driver", num_formats);
    m_program_binary_supported = (num_formats > 0);
  }

  if (!m_program_binary_supported)
  {
    Log_WarningPrintf("Your GL driver does not support program binaries. Hopefully it has a built-in cache, otherwise "
                      "startup will be slow due to compiling shaders.");
    return;
  }

  if (!base_path.empty())
  {
    const std::string index_filename = GetIndexFileName();
    const std::string blob_filename = GetBlobFileName();

    if (!ReadExisting(index_filename, blob_filename))
      CreateNew(index_filename, blob_filename);
  }
}

bool ShaderCache::CreateNew(const std::string& index_filename, const std::string& blob_filename)
{
  if (FileSystem::FileExists(index_filename.c_str()))
  {
    Log_WarningPrintf("Removing existing index file '%s'", index_filename.c_str());
    FileSystem::DeleteFile(index_filename.c_str());
  }
  if (FileSystem::FileExists(blob_filename.c_str()))
  {
    Log_WarningPrintf("Removing existing blob file '%s'", blob_filename.c_str());
    FileSystem::DeleteFile(blob_filename.c_str());
  }

  m_index_file = FileSystem::OpenCFile(index_filename.c_str(), "wb");
  if (!m_index_file)
  {
    Log_ErrorPrintf("Failed to open index file '%s' for writing", index_filename.c_str());
    return false;
  }

  const u32 index_version = FILE_VERSION;
  if (std::fwrite(&index_version, sizeof(index_version), 1, m_index_file) != 1 ||
      std::fwrite(&m_version, sizeof(m_version), 1, m_index_file) != 1)
  {
    Log_ErrorPrintf("Failed to write version to index file '%s'", index_filename.c_str());
    std::fclose(m_index_file);
    m_index_file = nullptr;
    FileSystem::DeleteFile(index_filename.c_str());
    return false;
  }

  m_blob_file = FileSystem::OpenCFile(blob_filename.c_str(), "w+b");
  if (!m_blob_file)
  {
    Log_ErrorPrintf("Failed to open blob file '%s' for writing", blob_filename.c_str());
    std::fclose(m_index_file);
    m_index_file = nullptr;
    FileSystem::DeleteFile(index_filename.c_str());
    return false;
  }

  return true;
}

bool ShaderCache::ReadExisting(const std::string& index_filename, const std::string& blob_filename)
{
  m_index_file = FileSystem::OpenCFile(index_filename.c_str(), "r+b");
  if (!m_index_file)
    return false;

  u32 file_version = 0;
  u32 data_version = 0;
  if (std::fread(&file_version, sizeof(file_version), 1, m_index_file) != 1 || file_version != FILE_VERSION ||
      std::fread(&data_version, sizeof(data_version), 1, m_index_file) != 1 || data_version != m_version)
  {
    Log_ErrorPrintf("Bad file/data version in '%s'", index_filename.c_str());
    std::fclose(m_index_file);
    m_index_file = nullptr;
    return false;
  }

  m_blob_file = FileSystem::OpenCFile(blob_filename.c_str(), "a+b");
  if (!m_blob_file)
  {
    Log_ErrorPrintf("Blob file '%s' is missing", blob_filename.c_str());
    std::fclose(m_index_file);
    m_index_file = nullptr;
    return false;
  }

  std::fseek(m_blob_file, 0, SEEK_END);
  const u32 blob_file_size = static_cast<u32>(std::ftell(m_blob_file));

  for (;;)
  {
    CacheIndexEntry entry;
    if (std::fread(&entry, sizeof(entry), 1, m_index_file) != 1 ||
        (entry.file_offset + entry.blob_size) > blob_file_size)
    {
      if (std::feof(m_index_file))
        break;

      Log_ErrorPrintf("Failed to read entry from '%s', corrupt file?", index_filename.c_str());
      m_index.clear();
      std::fclose(m_blob_file);
      m_blob_file = nullptr;
      std::fclose(m_index_file);
      m_index_file = nullptr;
      return false;
    }

    const CacheIndexKey key{
      entry.vertex_source_hash_low,   entry.vertex_source_hash_high,   entry.vertex_source_length,
      entry.geometry_source_hash_low, entry.geometry_source_hash_high, entry.geometry_source_length,
      entry.fragment_source_hash_low, entry.fragment_source_hash_high, entry.fragment_source_length};
    const CacheIndexData data{entry.file_offset, entry.blob_size, entry.blob_format};
    m_index.emplace(key, data);
  }

  Log_InfoPrintf("Read %zu entries from '%s'", m_index.size(), index_filename.c_str());
  return true;
}

void ShaderCache::Close()
{
  m_index.clear();
  if (m_index_file)
    std::fclose(m_index_file);
  if (m_blob_file)
    std::fclose(m_blob_file);
}

bool ShaderCache::Recreate()
{
  Close();

  const std::string index_filename = GetIndexFileName();
  const std::string blob_filename = GetBlobFileName();

  return CreateNew(index_filename, blob_filename);
}

ShaderCache::CacheIndexKey ShaderCache::GetCacheKey(const std::string_view& vertex_shader,
                                                    const std::string_view& geometry_shader,
                                                    const std::string_view& fragment_shader)
{
  union ShaderHash
  {
    struct
    {
      u64 low;
      u64 high;
    };
    u8 bytes[16];
  };

  ShaderHash vertex_hash = {};
  ShaderHash geometry_hash = {};
  ShaderHash fragment_hash = {};

  MD5Digest digest;
  if (!vertex_shader.empty())
  {
    digest.Update(vertex_shader.data(), static_cast<u32>(vertex_shader.length()));
    digest.Final(vertex_hash.bytes);
  }

  if (!geometry_shader.empty())
  {
    digest.Reset();
    digest.Update(geometry_shader.data(), static_cast<u32>(geometry_shader.length()));
    digest.Final(geometry_hash.bytes);
  }

  if (!fragment_shader.empty())
  {
    digest.Reset();
    digest.Update(fragment_shader.data(), static_cast<u32>(fragment_shader.length()));
    digest.Final(fragment_hash.bytes);
  }

  return CacheIndexKey{vertex_hash.low,   vertex_hash.high,   static_cast<u32>(vertex_shader.length()),
                       geometry_hash.low, geometry_hash.high, static_cast<u32>(geometry_shader.length()),
                       fragment_hash.low, fragment_hash.high, static_cast<u32>(fragment_shader.length())};
}

std::string ShaderCache::GetIndexFileName() const
{
  return Path::Combine(m_base_path, "gl_programs.idx");
}

std::string ShaderCache::GetBlobFileName() const
{
  return Path::Combine(m_base_path, "gl_programs.bin");
}

std::optional<Program> ShaderCache::GetProgram(const std::string_view vertex_shader,
                                               const std::string_view geometry_shader,
                                               const std::string_view fragment_shader, const PreLinkCallback& callback)
{
  if (!m_program_binary_supported || !m_blob_file)
    return CompileProgram(vertex_shader, geometry_shader, fragment_shader, callback, false);

  const auto key = GetCacheKey(vertex_shader, geometry_shader, fragment_shader);
  auto iter = m_index.find(key);
  if (iter == m_index.end())
    return CompileAndAddProgram(key, vertex_shader, geometry_shader, fragment_shader, callback);

  std::vector<u8> data(iter->second.blob_size);
  if (std::fseek(m_blob_file, iter->second.file_offset, SEEK_SET) != 0 ||
      std::fread(data.data(), 1, iter->second.blob_size, m_blob_file) != iter->second.blob_size)
  {
    Log_ErrorPrintf("Read blob from file failed");
    return {};
  }

  Program prog;
  if (prog.CreateFromBinary(data.data(), static_cast<u32>(data.size()), iter->second.blob_format))
    return std::optional<Program>(std::move(prog));

  Log_WarningPrintf(
    "Failed to create program from binary, this may be due to a driver or GPU Change. Recreating cache.");
  if (!Recreate())
    return CompileProgram(vertex_shader, geometry_shader, fragment_shader, callback, false);
  else
    return CompileAndAddProgram(key, vertex_shader, geometry_shader, fragment_shader, callback);
}

std::optional<Program> ShaderCache::CompileProgram(const std::string_view& vertex_shader,
                                                   const std::string_view& geometry_shader,
                                                   const std::string_view& fragment_shader,
                                                   const PreLinkCallback& callback, bool set_retrievable)
{
  Program prog;
  if (!prog.Compile(vertex_shader, geometry_shader, fragment_shader))
    return std::nullopt;

  if (callback)
    callback(prog);

  if (set_retrievable)
    prog.SetBinaryRetrievableHint();

  if (!prog.Link())
    return std::nullopt;

  return std::optional<Program>(std::move(prog));
}

std::optional<Program> ShaderCache::CompileAndAddProgram(const CacheIndexKey& key,
                                                         const std::string_view& vertex_shader,
                                                         const std::string_view& geometry_shader,
                                                         const std::string_view& fragment_shader,
                                                         const PreLinkCallback& callback)
{
  std::optional<Program> prog = CompileProgram(vertex_shader, geometry_shader, fragment_shader, callback, true);
  if (!prog)
    return std::nullopt;

  std::vector<u8> prog_data;
  u32 prog_format = 0;
  if (!prog->GetBinary(&prog_data, &prog_format))
    return std::nullopt;

  if (!m_blob_file || std::fseek(m_blob_file, 0, SEEK_END) != 0)
    return prog;

  CacheIndexData data;
  data.file_offset = static_cast<u32>(std::ftell(m_blob_file));
  data.blob_size = static_cast<u32>(prog_data.size());
  data.blob_format = prog_format;

  CacheIndexEntry entry = {};
  entry.vertex_source_hash_low = key.vertex_source_hash_low;
  entry.vertex_source_hash_high = key.vertex_source_hash_high;
  entry.vertex_source_length = key.vertex_source_length;
  entry.geometry_source_hash_low = key.geometry_source_hash_low;
  entry.geometry_source_hash_high = key.geometry_source_hash_high;
  entry.geometry_source_length = key.geometry_source_length;
  entry.fragment_source_hash_low = key.fragment_source_hash_low;
  entry.fragment_source_hash_high = key.fragment_source_hash_high;
  entry.fragment_source_length = key.fragment_source_length;
  entry.file_offset = data.file_offset;
  entry.blob_size = data.blob_size;
  entry.blob_format = data.blob_format;

  if (std::fwrite(prog_data.data(), 1, entry.blob_size, m_blob_file) != entry.blob_size ||
      std::fflush(m_blob_file) != 0 || std::fwrite(&entry, sizeof(entry), 1, m_index_file) != 1 ||
      std::fflush(m_index_file) != 0)
  {
    Log_ErrorPrintf("Failed to write shader blob to file");
    return prog;
  }

  m_index.emplace(key, data);
  return prog;
}

} // namespace GL
