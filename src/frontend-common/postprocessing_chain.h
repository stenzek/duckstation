#pragma once
#include "postprocessing_shader.h"
#include <string_view>
#include <vector>

namespace FrontendCommon {

class PostProcessingChain
{
public:
  PostProcessingChain();
  ~PostProcessingChain();

  ALWAYS_INLINE bool IsEmpty() const { return m_shaders.empty(); }
  ALWAYS_INLINE u32 GetStageCount() const { return static_cast<u32>(m_shaders.size()); }
  ALWAYS_INLINE const PostProcessingShader& GetShaderStage(u32 i) const { return m_shaders[i]; }
  ALWAYS_INLINE PostProcessingShader& GetShaderStage(u32 i) { return m_shaders[i]; }

  void AddShader(PostProcessingShader shader);
  bool AddStage(const std::string_view& name);
  void RemoveStage(u32 index);
  void MoveStageUp(u32 index);
  void MoveStageDown(u32 index);
  void ClearStages();

  std::string GetConfigString() const;

  bool CreateFromString(const std::string_view& chain_config);

  static std::vector<std::string> GetAvailableShaderNames();

private:
  std::vector<PostProcessingShader> m_shaders;
};

} // namespace FrontendCommon
