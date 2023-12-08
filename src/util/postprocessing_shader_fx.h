// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "postprocessing_shader.h"

#include "common/thirdparty/SmallVector.h"
#include "common/timer.h"

// reshadefx
#include "effect_module.hpp"

#include <random>

class Error;

namespace PostProcessing {

class ReShadeFXShader final : public Shader
{
public:
  ReShadeFXShader();
  ~ReShadeFXShader();

  bool IsValid() const override;

  bool LoadFromFile(std::string name, std::string filename, bool only_config, Error* error);
  bool LoadFromString(std::string name, std::string filename, std::string code, bool only_config, Error* error);

  bool ResizeOutput(GPUTexture::Format format, u32 width, u32 height) override;
  bool CompilePipeline(GPUTexture::Format format, u32 width, u32 height) override;
  bool Apply(GPUTexture* input, GPUTexture* final_target, s32 final_left, s32 final_top, s32 final_width,
             s32 final_height, s32 orig_width, s32 orig_height, u32 target_width, u32 target_height) override;

private:
  using TextureID = s32;

  static constexpr TextureID INPUT_COLOR_TEXTURE = -1;
  static constexpr TextureID INPUT_DEPTH_TEXTURE = -2;
  static constexpr TextureID OUTPUT_COLOR_TEXTURE = -3;

  enum class SourceOptionType
  {
    None,
    Zero,
    Timer,
    FrameTime,
    FrameCount,
    FrameCountF,
    PingPong,
    MousePoint,
    Random,
    RandomF,
    BufferWidth,
    BufferHeight,
    BufferWidthF,
    BufferHeightF,
    InternalWidth,
    InternalHeight,
    InternalWidthF,
    InternalHeightF,

    MaxCount
  };

  struct SourceOption
  {
    SourceOptionType source;
    u32 offset;
    float min;
    float max;
    float smoothing;
    std::array<float, 2> step;
    ShaderOption::ValueVector value;
  };

  bool CreateModule(s32 buffer_width, s32 buffer_height, reshadefx::module* mod, std::string code, Error* error);
  bool CreateOptions(const reshadefx::module& mod, Error* error);
  bool GetSourceOption(const reshadefx::uniform_info& ui, SourceOptionType* si, Error* error);
  bool CreatePasses(GPUTexture::Format backbuffer_format, reshadefx::module& mod, Error* error);

  const char* GetTextureNameForID(TextureID id) const;
  GPUTexture* GetTextureByID(TextureID id, GPUTexture* input, GPUTexture* final_target) const;

  std::string m_filename;

  struct Texture
  {
    std::unique_ptr<GPUTexture> texture;
    std::string reshade_name; // TODO: we might be able to drop this
    GPUTexture::Format format;
    float rt_scale;
  };

  struct Sampler
  {
    u32 slot;
    TextureID texture_id;
    std::string reshade_name;
    GPUSampler* sampler;
  };

  struct Pass
  {
    std::unique_ptr<GPUPipeline> pipeline;
    llvm::SmallVector<TextureID, GPUDevice::MAX_RENDER_TARGETS> render_targets;
    llvm::SmallVector<Sampler, GPUDevice::MAX_TEXTURE_SAMPLERS> samplers;
    u32 num_vertices;

#ifdef _DEBUG
    std::string name;
#endif
  };

  std::vector<Pass> m_passes;
  std::vector<Texture> m_textures;
  std::vector<SourceOption> m_source_options;
  u32 m_uniforms_size = 0;
  bool m_valid = false;

  Common::Timer m_frame_timer;
  u32 m_frame_count = 0;

  // Specifically using a fixed seed, so that it's consistent from run-to-run.
  std::mt19937 m_random{0x1337};
};

} // namespace PostProcessing