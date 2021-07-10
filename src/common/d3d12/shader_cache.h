#pragma once
#include "../hash_combine.h"
#include "../types.h"
#include "../windows_headers.h"
#include <cstdio>
#include <d3d12.h>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <wrl/client.h>

namespace D3D12 {

class ShaderCache
{
public:
  template<typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  enum class EntryType
  {
    VertexShader,
    GeometryShader,
    PixelShader,
    ComputeShader,
    GraphicsPipeline,
  };

  ShaderCache();
  ~ShaderCache();

  void Open(std::string_view base_path, D3D_FEATURE_LEVEL feature_level, bool debug);

  ALWAYS_INLINE ComPtr<ID3DBlob> GetVertexShader(std::string_view shader_code)
  {
    return GetShaderBlob(EntryType::VertexShader, shader_code);
  }
  ALWAYS_INLINE ComPtr<ID3DBlob> GetGeometryShader(std::string_view shader_code)
  {
    return GetShaderBlob(EntryType::GeometryShader, shader_code);
  }
  ALWAYS_INLINE ComPtr<ID3DBlob> GetPixelShader(std::string_view shader_code)
  {
    return GetShaderBlob(EntryType::PixelShader, shader_code);
  }
  ALWAYS_INLINE ComPtr<ID3DBlob> GetComputeShader(std::string_view shader_code)
  {
    return GetShaderBlob(EntryType::ComputeShader, shader_code);
  }

  ComPtr<ID3DBlob> GetShaderBlob(EntryType type, std::string_view shader_code);

  ComPtr<ID3D12PipelineState> GetPipelineState(ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc);

private:
  static constexpr u32 FILE_VERSION = 1;

  struct CacheIndexKey
  {
    u64 source_hash_low;
    u64 source_hash_high;
    u32 source_length;
    EntryType type;

    bool operator==(const CacheIndexKey& key) const;
    bool operator!=(const CacheIndexKey& key) const;
  };

  struct CacheIndexEntryHasher
  {
    std::size_t operator()(const CacheIndexKey& e) const noexcept
    {
      std::size_t h = 0;
      hash_combine(h, e.source_hash_low, e.source_hash_high, e.source_length, e.type);
      return h;
    }
  };

  struct CacheIndexData
  {
    u32 file_offset;
    u32 blob_size;
  };

  using CacheIndex = std::unordered_map<CacheIndexKey, CacheIndexData, CacheIndexEntryHasher>;

  static std::string GetCacheBaseFileName(const std::string_view& base_path, const std::string_view& type,
                                          D3D_FEATURE_LEVEL feature_level, bool debug);
  static CacheIndexKey GetShaderCacheKey(EntryType type, const std::string_view& shader_code);
  static CacheIndexKey GetPipelineCacheKey(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& gpdesc);

  bool CreateNew(const std::string& index_filename, const std::string& blob_filename, std::FILE*& index_file,
                 std::FILE*& blob_file);
  bool ReadExisting(const std::string& index_filename, const std::string& blob_filename, std::FILE*& index_file,
                    std::FILE*& blob_file, CacheIndex& index);
  void InvalidatePipelineCache();
  void Close();

  ComPtr<ID3DBlob> CompileAndAddShaderBlob(const CacheIndexKey& key, std::string_view shader_code);
  ComPtr<ID3D12PipelineState> CompileAndAddPipeline(ID3D12Device* device, const CacheIndexKey& key,
                                                    const D3D12_GRAPHICS_PIPELINE_STATE_DESC& gpdesc);

  std::string m_base_path;

  std::FILE* m_shader_index_file = nullptr;
  std::FILE* m_shader_blob_file = nullptr;
  CacheIndex m_shader_index;

  std::FILE* m_pipeline_index_file = nullptr;
  std::FILE* m_pipeline_blob_file = nullptr;
  CacheIndex m_pipeline_index;

  D3D_FEATURE_LEVEL m_feature_level = D3D_FEATURE_LEVEL_11_0;
  bool m_use_pipeline_cache = false;
  bool m_debug = false;
};

} // namespace D3D12
