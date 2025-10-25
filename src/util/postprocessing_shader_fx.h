// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "postprocessing_shader.h"

#include "common/thirdparty/SmallVector.h"
#include "common/timer.h"

// reshadefx
#include "effect_module.hpp"

#include <random>

namespace reshadefx {
class codegen;
}

class Error;

namespace PostProcessing {

class ReShadeFXShader final : public Shader
{
public:
  ReShadeFXShader();
  ~ReShadeFXShader();

  bool WantsDepthBuffer() const override;

  bool LoadFromFile(std::string name, std::string filename, bool only_config, Error* error);
  bool LoadFromString(std::string name, std::string filename, std::string code, bool only_config, Error* error);

  bool ResizeTargets(u32 source_width, u32 source_height, GPUTexture::Format target_format, u32 target_width,
                     u32 target_height, u32 viewport_width, u32 viewport_height, Error* error) override;

  bool CompilePipeline(GPUTexture::Format format, u32 width, u32 height, Error* error,
                       ProgressCallback* progress) override;

  GPUDevice::PresentResult Apply(GPUTexture* input_color, GPUTexture* input_depth, GPUTexture* final_target,
                                 GSVector4i final_rect, s32 orig_width, s32 orig_height, s32 native_width,
                                 s32 native_height, u32 target_width, u32 target_height, float time) override;

private:
  using TextureID = s32;

  static constexpr TextureID INPUT_COLOR_TEXTURE = -1;
  static constexpr TextureID INPUT_DEPTH_TEXTURE = -2;
  static constexpr TextureID OUTPUT_COLOR_TEXTURE = -3;

  enum class SourceOptionType
  {
    None,
    Zero,
    HasDepth,
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
    NativeWidth,
    NativeHeight,
    NativeWidthF,
    NativeHeightF,
    UpscaleMultiplier,
    ViewportX,
    ViewportY,
    ViewportWidth,
    ViewportHeight,
    ViewportOffset,
    ViewportSize,
    InternalPixelSize,
    InternalNormPixelSize,
    NativePixelSize,
    NativeNormPixelSize,
    BufferToViewportRatio,

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

  bool CreateModule(s32 buffer_width, s32 buffer_height, reshadefx::codegen* cg, GPUShaderLanguage cg_language,
                    std::string code, Error* error);
  bool CreateOptions(const reshadefx::effect_module& mod, Error* error);
  bool GetSourceOption(const reshadefx::uniform& ui, SourceOptionType* si, Error* error);
  bool CreatePasses(GPUTexture::Format backbuffer_format, const reshadefx::effect_module& mod, Error* error);

  const char* GetTextureNameForID(TextureID id) const;
  GPUTexture* GetTextureByID(TextureID id, GPUTexture* input_color, GPUTexture* input_depth,
                             GPUTexture* final_target) const;

  std::string m_filename;

  struct Texture
  {
    std::unique_ptr<GPUTexture> texture;
    std::string reshade_name; // TODO: we might be able to drop this
    GPUTexture::Format format;
    u32 render_target_width;
    u32 render_target_height;
    bool render_target;
    bool storage_access;
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
    u16 dispatch_size[3];
    bool is_compute;
    bool clear_render_targets;

#ifdef ENABLE_GPU_OBJECT_NAMES
    std::string name;
#endif
  };

  std::vector<Pass> m_passes;
  std::vector<Texture> m_textures;
  std::vector<SourceOption> m_source_options;
  u32 m_uniforms_size = 0;
  bool m_wants_depth_buffer = false;

  Timer m_frame_timer;
  u32 m_frame_count = 0;

  // Specifically using a fixed seed, so that it's consistent from run-to-run.
  std::mt19937 m_random{0x1337};
};

} // namespace PostProcessing