#pragma once
#include "../hash_combine.h"
#include "../types.h"
#include "../windows_headers.h"
#include "shader_compiler.h"
#include <cstdio>
#include <d3d11.h>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <wrl/client.h>

namespace D3D11 {

class ShaderCache
{
public:
  template<typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  ShaderCache();
  ~ShaderCache();

  void Open(std::string_view base_path, D3D_FEATURE_LEVEL feature_level, u32 version, bool debug);

  ComPtr<ID3DBlob> GetShaderBlob(ShaderCompiler::Type type, std::string_view shader_code);

  ComPtr<ID3D11VertexShader> GetVertexShader(ID3D11Device* device, std::string_view shader_code);
  ComPtr<ID3D11GeometryShader> GetGeometryShader(ID3D11Device* device, std::string_view shader_code);
  ComPtr<ID3D11PixelShader> GetPixelShader(ID3D11Device* device, std::string_view shader_code);
  ComPtr<ID3D11ComputeShader> GetComputeShader(ID3D11Device* device, std::string_view shader_code);

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

  static std::string GetCacheBaseFileName(const std::string_view& base_path, D3D_FEATURE_LEVEL feature_level,
                                          bool debug);
  static CacheIndexKey GetCacheKey(ShaderCompiler::Type type, const std::string_view& shader_code);

  bool CreateNew(const std::string& index_filename, const std::string& blob_filename);
  bool ReadExisting(const std::string& index_filename, const std::string& blob_filename);
  void Close();

  ComPtr<ID3DBlob> CompileAndAddShaderBlob(const CacheIndexKey& key, std::string_view shader_code);

  std::FILE* m_index_file = nullptr;
  std::FILE* m_blob_file = nullptr;

  CacheIndex m_index;

  D3D_FEATURE_LEVEL m_feature_level = D3D_FEATURE_LEVEL_11_0;
  u32 m_version = 0;
  bool m_debug = false;
};

} // namespace D3D11
