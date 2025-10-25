// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "postprocessing_shader.h"

#include "common/thirdparty/SmallVector.h"

#include <array>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

typedef struct spvc_compiler_s* spvc_compiler;
struct spvc_reflected_resource;

namespace PostProcessing {

class SlangPresetParser;
class SlangShaderPreprocessor;

class SlangShader final : public Shader
{
public:
  SlangShader();
  ~SlangShader();

  bool WantsUnscaledInput() const override;

  bool LoadFromFile(std::string name, const char* path, Error* error);
  bool LoadFromString(std::string name, std::string_view path, std::string_view code, Error* error);

  bool ResizeTargets(u32 source_width, u32 source_height, GPUTexture::Format target_format, u32 target_width,
                     u32 target_height, u32 viewport_width, u32 viewport_height, Error* error) override;
  bool CompilePipeline(GPUTexture::Format format, u32 width, u32 height, Error* error,
                       ProgressCallback* progress) override;
  GPUDevice::PresentResult Apply(GPUTexture* input_color, GPUTexture* input_depth, GPUTexture* final_target,
                                 GSVector4i final_rect, s32 orig_width, s32 orig_height, s32 native_width,
                                 s32 native_height, u32 target_width, u32 target_height, float time) override;

private:
  using TextureID = u32;
  using UniformType = s32;
  using AliasMap = std::vector<std::pair<std::string, u32>>;

  enum class ScaleType : u8
  {
    Source,
    Viewport,
    Absolute,
    Original,
  };

  static constexpr u32 MAX_ORIGINAL_HISTORY_SIZE = 16;

  static constexpr TextureID TEXTURE_ID_SOURCE = 0xFFFFFFFFu;
  static constexpr TextureID TEXTURE_ID_ORIGINAL = 0xFFFFFFFEu;
  static constexpr TextureID TEXTURE_ID_ORIGINAL_HISTORY_START = TEXTURE_ID_ORIGINAL - MAX_ORIGINAL_HISTORY_SIZE;
  static constexpr TextureID TEXTURE_ID_FEEDBACK = 0x80000000u;

  struct Texture
  {
    std::string name;
    std::string path;
    std::unique_ptr<GPUTexture> texture;
    std::unique_ptr<GPUTexture> feedback_texture;
    GPUSampler* sampler;
    bool linear_filter;
    bool needs_feedback;
    bool generate_mipmaps;
  };

  struct Uniform
  {
    UniformType type;
    TextureID associated_texture;
    bool push_constant;
    u16 size;
    u32 offset;
  };

  struct Pass
  {
    std::unique_ptr<GPUPipeline> pipeline;

    // Textures
    llvm::SmallVector<std::pair<TextureID, u32>, GPUDevice::MAX_TEXTURE_SAMPLERS> samplers;

    // Uniforms
    llvm::SmallVector<Uniform, 8> uniforms;
    u32 uniforms_size = 0;
    u32 push_constants_size = 0;
    u32 frame_count_mod = 0;
    bool is_reflected = false;

    // Framebuffer
    TextureID output_texture_id = 0;
    GPUTexture::Format output_format = GPUTexture::Format::Unknown;
    std::array<std::pair<ScaleType, float>, 2> output_scale = {};
    GPUSampler::Config output_sampler_config = {};

    std::string vertex_shader_code;
    std::string fragment_shader_code;
    DynamicHeapArray<u32> vertex_shader_spv;
    DynamicHeapArray<u32> fragment_shader_spv;

    std::string name;
  };

  static bool ParseScaleType(ScaleType* dst, std::string_view value, Error* error);
  static GPUSampler::AddressMode ParseWrapMode(std::string_view value);

  static std::unique_ptr<GPUPipeline> CreateBlitPipeline(GPUTexture::Format format, Error* error);

  bool ParsePresetFile(std::string_view path, std::string_view code, Error* error);
  bool ParsePresetTextures(std::string_view preset_path, const SlangPresetParser& parser, Error* error);
  bool ParsePresetPass(std::string_view preset_path, const SlangPresetParser& parser, u32 idx, bool is_final_pass,
                       Error* error);

  bool ReflectPass(Pass& pass, Error* error);
  bool ReflectShader(Pass& pass, std::span<u32> spv, GPUShaderStage stage, Error* error);
  bool ReflectPassUniforms(const spvc_compiler& scompiler, const spvc_reflected_resource& resource, Pass& pass,
                           bool push_constant, Error* error);
  bool CompilePass(Pass& pass, Error* error);
  bool UploadLUTTextures(Error* error);

  std::optional<TextureID> FindTextureByName(std::string_view name, Error* error);

  std::optional<Uniform> GetUniformInfo(std::string_view name, bool push_constant, u32 offset, u16 size, Error* error);

  TinyString GetTextureNameForID(TextureID id) const;
  std::tuple<GPUTexture*, GPUSampler*> GetTextureByID(const Pass& pass, TextureID id, GPUTexture* input_color) const;

  void BindPassUniforms(const Pass& pass, u8* const push_constant_data, GPUTexture* input_color, GSVector2i output_size,
                        GSVector4i final_rect, s32 orig_width, s32 orig_height, s32 native_width, s32 native_height,
                        u32 target_width, u32 target_height, float time);

  std::vector<Texture> m_textures;
  std::vector<Pass> m_passes;
  AliasMap m_aliases;
  u32 m_frame_count = 0;

  std::vector<std::unique_ptr<GPUTexture>> m_original_history_textures;

  std::unique_ptr<GPUPipeline> m_output_blit_pipeline;
  GPUTexture::Format m_output_framebuffer_format = GPUTexture::Format::Unknown;
};

} // namespace PostProcessing
