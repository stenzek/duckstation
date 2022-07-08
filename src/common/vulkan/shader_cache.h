#pragma once
#include "../hash_combine.h"
#include "../types.h"
#include "loader.h"
#include "shader_compiler.h"
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Vulkan {

class ShaderCache
{
public:
  ~ShaderCache();

  static void Create(std::string_view base_path, u32 version, bool debug);
  static void Destroy();

  /// Returns a handle to the pipeline cache. Set set_dirty to true if you are planning on writing to it externally.
  VkPipelineCache GetPipelineCache(bool set_dirty = true);

  /// Writes pipeline cache to file, saving all newly compiled pipelines.
  bool FlushPipelineCache();

  std::optional<ShaderCompiler::SPIRVCodeVector> GetShaderSPV(ShaderCompiler::Type type, std::string_view shader_code);
  VkShaderModule GetShaderModule(ShaderCompiler::Type type, std::string_view shader_code);

  VkShaderModule GetVertexShader(std::string_view shader_code);
  VkShaderModule GetGeometryShader(std::string_view shader_code);
  VkShaderModule GetFragmentShader(std::string_view shader_code);
  VkShaderModule GetComputeShader(std::string_view shader_code);

private:
  static constexpr u32 FILE_VERSION = 2;

  struct CacheIndexKey
  {
    u64 source_hash_low;
    u64 source_hash_high;
    u32 source_length;
    ShaderCompiler::Type shader_type;

    bool operator==(const CacheIndexKey& key) const;
    bool operator!=(const CacheIndexKey& key) const;
  };

  struct CacheIndexEntryHasher
  {
    std::size_t operator()(const CacheIndexKey& e) const noexcept
    {
      std::size_t h = 0;
      hash_combine(h, e.source_hash_low, e.source_hash_high, e.source_length, e.shader_type);
      return h;
    }
  };

  struct CacheIndexData
  {
    u32 file_offset;
    u32 blob_size;
  };

  using CacheIndex = std::unordered_map<CacheIndexKey, CacheIndexData, CacheIndexEntryHasher>;

  ShaderCache();

  static std::string GetShaderCacheBaseFileName(const std::string_view& base_path, bool debug);
  static std::string GetPipelineCacheBaseFileName(const std::string_view& base_path, bool debug);
  static CacheIndexKey GetCacheKey(ShaderCompiler::Type type, const std::string_view& shader_code);

  void Open(std::string_view base_path, u32 version, bool debug);

  bool CreateNewShaderCache(const std::string& index_filename, const std::string& blob_filename);
  bool ReadExistingShaderCache(const std::string& index_filename, const std::string& blob_filename);
  void CloseShaderCache();

  bool CreateNewPipelineCache();
  bool ReadExistingPipelineCache();
  void ClosePipelineCache();

  std::optional<ShaderCompiler::SPIRVCodeVector> CompileAndAddShaderSPV(const CacheIndexKey& key,
                                                                        std::string_view shader_code);

  std::FILE* m_index_file = nullptr;
  std::FILE* m_blob_file = nullptr;
  std::string m_pipeline_cache_filename;

  CacheIndex m_index;

  VkPipelineCache m_pipeline_cache = VK_NULL_HANDLE;
  u32 m_version = 0;
  bool m_debug = false;
  bool m_pipeline_cache_dirty = false;
};

} // namespace Vulkan

extern std::unique_ptr<Vulkan::ShaderCache> g_vulkan_shader_cache;
