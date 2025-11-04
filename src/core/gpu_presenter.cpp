// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gpu_presenter.h"
#include "fullscreenui.h"
#include "fullscreenui_widgets.h"
#include "gpu.h"
#include "gpu_backend.h"
#include "gpu_shadergen.h"
#include "gpu_thread.h"
#include "gpu_thread_commands.h"
#include "host.h"
#include "imgui_overlays.h"
#include "performance_counters.h"
#include "save_state_version.h"
#include "settings.h"
#include "system.h"

#include "util/gpu_device.h"
#include "util/image.h"
#include "util/imgui_manager.h"
#include "util/media_capture.h"
#include "util/postprocessing.h"
#include "util/state_wrapper.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/gsvector_formatter.h"
#include "common/log.h"
#include "common/path.h"
#include "common/ryml_helpers.h"
#include "common/settings_interface.h"
#include "common/small_string.h"
#include "common/string_util.h"
#include "common/threading.h"
#include "common/timer.h"

#include <numbers>

LOG_CHANNEL(GPU);

GPUPresenter::GPUPresenter() = default;

GPUPresenter::~GPUPresenter()
{
  DestroyDeinterlaceTextures();
  g_gpu_device->RecycleTexture(std::move(m_chroma_smoothing_texture));
}

bool GPUPresenter::Initialize(Error* error)
{
  // we can't change the format after compiling shaders
  m_present_format =
    g_gpu_device->HasMainSwapChain() ? g_gpu_device->GetMainSwapChain()->GetFormat() : GPUTexture::Format::RGBA8;
  VERBOSE_LOG("Presentation format is {}", GPUTexture::GetFormatName(m_present_format));

  // overlay has to come first, because it sets the alpha blending on the display pipeline
  if (LoadOverlaySettings())
    LoadOverlayTexture();

  if (!CompileDisplayPipelines(true, true, g_gpu_settings.display_24bit_chroma_smoothing, error))
    return false;

  LoadPostProcessingSettings(false);

  return true;
}

bool GPUPresenter::UpdateSettings(const GPUSettings& old_settings, Error* error)
{
  if (g_gpu_settings.display_scaling != old_settings.display_scaling ||
      g_gpu_settings.display_scaling_24bit != old_settings.display_scaling_24bit ||
      g_gpu_settings.display_deinterlacing_mode != old_settings.display_deinterlacing_mode ||
      g_gpu_settings.display_24bit_chroma_smoothing != old_settings.display_24bit_chroma_smoothing)
  {
    // Toss buffers on mode change.
    if (g_gpu_settings.display_deinterlacing_mode != old_settings.display_deinterlacing_mode)
      DestroyDeinterlaceTextures();

    if (!CompileDisplayPipelines(
          g_gpu_settings.display_scaling != old_settings.display_scaling ||
            g_gpu_settings.display_scaling_24bit != old_settings.display_scaling_24bit,
          g_gpu_settings.display_deinterlacing_mode != old_settings.display_deinterlacing_mode,
          g_gpu_settings.display_24bit_chroma_smoothing != old_settings.display_24bit_chroma_smoothing, error))
    {
      Error::AddPrefix(error, "Failed to compile display pipeline on settings change:\n");
      return false;
    }
  }

  return true;
}

bool GPUPresenter::CompileDisplayPipelines(bool display, bool deinterlace, bool chroma_smoothing, Error* error)
{
  const GPUShaderGen shadergen(g_gpu_device->GetRenderAPI(), g_gpu_device->GetFeatures().dual_source_blend,
                               g_gpu_device->GetFeatures().framebuffer_fetch);

  GPUPipeline::GraphicsConfig plconfig;
  plconfig.primitive = GPUPipeline::Primitive::Triangles;
  plconfig.rasterization = GPUPipeline::RasterizationState::GetNoCullState();
  plconfig.depth = GPUPipeline::DepthState::GetNoTestsState();
  plconfig.blend = GPUPipeline::BlendState::GetNoBlendingState();
  plconfig.geometry_shader = nullptr;
  plconfig.depth_format = GPUTexture::Format::Unknown;
  plconfig.samples = 1;
  plconfig.per_sample_shading = false;
  plconfig.render_pass_flags = GPUPipeline::NoRenderPassFlags;

  if (display)
  {
    GPUBackend::SetScreenQuadInputLayout(plconfig);

    plconfig.layout = GPUPipeline::Layout::SingleTextureAndPushConstants;
    plconfig.SetTargetFormats(m_present_format);

    std::unique_ptr<GPUShader> vso = g_gpu_device->CreateShader(GPUShaderStage::Vertex, shadergen.GetLanguage(),
                                                                shadergen.GeneratePassthroughVertexShader(), error);
    if (!vso)
      return false;
    GL_OBJECT_NAME(vso, "Display Vertex Shader");

    std::string fs;
    static constexpr auto compile_display_shader = [](const GPUShaderGen& shadergen, std::string& fs,
                                                      DisplayScalingMode mode, Error* error) {
      switch (mode)
      {
        case DisplayScalingMode::BilinearSharp:
          fs = shadergen.GenerateDisplaySharpBilinearFragmentShader();
          break;
        case DisplayScalingMode::BilinearHybrid:
          fs = shadergen.GenerateDisplayHybridBilinearFragmentShader();
          break;

        case DisplayScalingMode::BilinearSmooth:
        case DisplayScalingMode::BilinearInteger:
          fs = shadergen.GenerateDisplayFragmentShader(true, false);
          break;

        case DisplayScalingMode::Lanczos:
          fs = shadergen.GenerateDisplayLanczosFragmentShader();
          break;

        case DisplayScalingMode::Nearest:
        case DisplayScalingMode::NearestInteger:
        default:
          fs = shadergen.GenerateDisplayFragmentShader(false, true);
          break;
      }

      return g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(), fs, error);
    };

    std::unique_ptr<GPUShader> fso = compile_display_shader(shadergen, fs, g_gpu_settings.display_scaling, error);
    if (!fso)
      return false;
    GL_OBJECT_NAME_FMT(fso, "Display Fragment Shader [{}]",
                       Settings::GetDisplayScalingName(g_gpu_settings.display_scaling));

    plconfig.vertex_shader = vso.get();
    plconfig.fragment_shader = fso.get();
    if (!(m_display_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
      return false;
    GL_OBJECT_NAME_FMT(m_display_pipeline, "Display Pipeline [{}]",
                       Settings::GetDisplayScalingName(g_gpu_settings.display_scaling));

    std::unique_ptr<GPUShader> fso_24bit;
    if (g_gpu_settings.display_scaling_24bit != g_gpu_settings.display_scaling)
    {
      fso_24bit = compile_display_shader(shadergen, fs, g_gpu_settings.display_scaling_24bit, error);
      if (!fso_24bit)
        return false;
      GL_OBJECT_NAME_FMT(fso_24bit, "Display Fragment Shader 24bit [{}]",
                         Settings::GetDisplayScalingName(g_gpu_settings.display_scaling_24bit));

      plconfig.fragment_shader = fso_24bit.get();
      if (!(m_display_24bit_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
        return false;
      GL_OBJECT_NAME_FMT(m_display_24bit_pipeline, "Display Pipeline 24bit [{}]",
                         Settings::GetDisplayScalingName(g_gpu_settings.display_scaling_24bit));
    }
    else
    {
      m_display_24bit_pipeline.reset();
    }

    std::unique_ptr<GPUShader> copy_fso = g_gpu_device->CreateShader(
      GPUShaderStage::Fragment, shadergen.GetLanguage(), shadergen.GenerateCopyFragmentShader(false), error);
    if (!copy_fso)
      return false;
    GL_OBJECT_NAME(copy_fso, "Display Copy Fragment Shader");

    plconfig.fragment_shader = copy_fso.get();
    if (!(m_present_copy_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
      return false;
    GL_OBJECT_NAME(m_present_copy_pipeline, "Display Copy Pipeline");

    // blended variants
    if (m_border_overlay_texture)
    {
      std::unique_ptr<GPUShader> clear_fso = g_gpu_device->CreateShader(
        GPUShaderStage::Fragment, shadergen.GetLanguage(),
        shadergen.GenerateFillFragmentShader(GSVector4::cxpr(0.0f, 0.0f, 0.0f, 1.0f)), error);
      if (!clear_fso)
        return false;
      GL_OBJECT_NAME(clear_fso, "Display Clear Fragment Shader");

      plconfig.fragment_shader = copy_fso.get();
      plconfig.blend = m_border_overlay_alpha_blend ? GPUPipeline::BlendState::GetAlphaBlendingState() :
                                                      GPUPipeline::BlendState::GetNoBlendingState();
      if (!(m_border_overlay_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
        return false;
      GL_OBJECT_NAME(m_border_overlay_pipeline, "Border Overlay Pipeline");

      plconfig.blend = GPUPipeline::BlendState::GetNoBlendingState();
      if (m_border_overlay_destination_alpha_blend)
      {
        // destination blend the main present, not source
        plconfig.blend.enable = true;
        plconfig.blend.src_blend = GPUPipeline::BlendFunc::InvDstAlpha;
        plconfig.blend.blend_op = GPUPipeline::BlendOp::Add;
        plconfig.blend.dst_blend =
          m_border_overlay_alpha_blend ? GPUPipeline::BlendFunc::One : GPUPipeline::BlendFunc::DstAlpha;
        plconfig.blend.src_alpha_blend = GPUPipeline::BlendFunc::One;
        plconfig.blend.alpha_blend_op = GPUPipeline::BlendOp::Add;
        plconfig.blend.dst_alpha_blend = GPUPipeline::BlendFunc::Zero;
      }

      plconfig.fragment_shader = clear_fso.get();
      plconfig.primitive = GPUPipeline::Primitive::Triangles;
      if (!(m_present_clear_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
        return false;
      GL_OBJECT_NAME(m_present_clear_pipeline, "Display Clear Pipeline");

      plconfig.fragment_shader = fso.get();
      plconfig.primitive = GPUPipeline::Primitive::TriangleStrips;
      if (!(m_display_blend_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
        return false;
      GL_OBJECT_NAME_FMT(m_display_blend_pipeline, "Display Pipeline [Blended, {}]",
                         Settings::GetDisplayScalingName(g_gpu_settings.display_scaling));

      if (g_gpu_settings.display_scaling_24bit != g_gpu_settings.display_scaling)
      {
        plconfig.fragment_shader = fso_24bit.get();
        if (!(m_display_24bit_blend_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
          return false;
        GL_OBJECT_NAME_FMT(m_display_24bit_blend_pipeline, "Display Pipeline 24bit [Blended, {}]",
                           Settings::GetDisplayScalingName(g_gpu_settings.display_scaling_24bit));
      }

      plconfig.fragment_shader = copy_fso.get();
      if (!(m_present_copy_blend_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
        return false;
      GL_OBJECT_NAME(m_present_copy_blend_pipeline, "Display Copy Pipeline [Blended]");
    }
    else
    {
      m_border_overlay_pipeline.reset();
      m_present_clear_pipeline.reset();
      m_display_blend_pipeline.reset();
      m_display_24bit_blend_pipeline.reset();
      m_present_copy_blend_pipeline.reset();
    }
  }

  plconfig.input_layout = {};
  plconfig.primitive = GPUPipeline::Primitive::Triangles;
  plconfig.blend = GPUPipeline::BlendState::GetNoBlendingState();

  if (deinterlace)
  {
    std::unique_ptr<GPUShader> vso = g_gpu_device->CreateShader(GPUShaderStage::Vertex, shadergen.GetLanguage(),
                                                                shadergen.GenerateScreenQuadVertexShader(), error);
    if (!vso)
      return false;
    GL_OBJECT_NAME(vso, "Deinterlace Vertex Shader");

    plconfig.layout = GPUPipeline::Layout::SingleTextureAndPushConstants;
    plconfig.vertex_shader = vso.get();
    plconfig.SetTargetFormats(GPUTexture::Format::RGBA8);

    switch (g_gpu_settings.display_deinterlacing_mode)
    {
      case DisplayDeinterlacingMode::Disabled:
      case DisplayDeinterlacingMode::Progressive:
        break;

      case DisplayDeinterlacingMode::Weave:
      {
        std::unique_ptr<GPUShader> fso = g_gpu_device->CreateShader(
          GPUShaderStage::Fragment, shadergen.GetLanguage(), shadergen.GenerateDeinterlaceWeaveFragmentShader(), error);
        if (!fso)
          return false;

        GL_OBJECT_NAME(fso, "Weave Deinterlace Fragment Shader");

        plconfig.layout = GPUPipeline::Layout::SingleTextureAndPushConstants;
        plconfig.vertex_shader = vso.get();
        plconfig.fragment_shader = fso.get();
        if (!(m_deinterlace_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
          return false;

        GL_OBJECT_NAME(m_deinterlace_pipeline, "Weave Deinterlace Pipeline");
      }
      break;

      case DisplayDeinterlacingMode::Blend:
      {
        std::unique_ptr<GPUShader> fso = g_gpu_device->CreateShader(
          GPUShaderStage::Fragment, shadergen.GetLanguage(), shadergen.GenerateDeinterlaceBlendFragmentShader(), error);
        if (!fso)
          return false;

        GL_OBJECT_NAME(fso, "Blend Deinterlace Fragment Shader");

        plconfig.layout = GPUPipeline::Layout::MultiTextureAndPushConstants;
        plconfig.vertex_shader = vso.get();
        plconfig.fragment_shader = fso.get();
        if (!(m_deinterlace_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
          return false;

        GL_OBJECT_NAME(m_deinterlace_pipeline, "Blend Deinterlace Pipeline");
      }
      break;

      case DisplayDeinterlacingMode::Adaptive:
      {
        std::unique_ptr<GPUShader> fso =
          g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(),
                                     shadergen.GenerateFastMADReconstructFragmentShader(), error);
        if (!fso)
          return false;

        GL_OBJECT_NAME(fso, "FastMAD Reconstruct Fragment Shader");

        plconfig.layout = GPUPipeline::Layout::MultiTextureAndPushConstants;
        plconfig.fragment_shader = fso.get();
        if (!(m_deinterlace_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
          return false;

        GL_OBJECT_NAME(m_deinterlace_pipeline, "FastMAD Reconstruct Pipeline");
      }
      break;

      default:
        UnreachableCode();
    }
  }

  if (chroma_smoothing)
  {
    m_chroma_smoothing_pipeline.reset();
    g_gpu_device->RecycleTexture(std::move(m_chroma_smoothing_texture));

    if (g_gpu_settings.display_24bit_chroma_smoothing)
    {
      plconfig.layout = GPUPipeline::Layout::SingleTextureAndPushConstants;
      plconfig.SetTargetFormats(GPUTexture::Format::RGBA8);

      std::unique_ptr<GPUShader> vso = g_gpu_device->CreateShader(GPUShaderStage::Vertex, shadergen.GetLanguage(),
                                                                  shadergen.GenerateScreenQuadVertexShader(), error);
      std::unique_ptr<GPUShader> fso = g_gpu_device->CreateShader(
        GPUShaderStage::Fragment, shadergen.GetLanguage(), shadergen.GenerateChromaSmoothingFragmentShader(), error);
      if (!vso || !fso)
        return false;
      GL_OBJECT_NAME(vso, "Chroma Smoothing Vertex Shader");
      GL_OBJECT_NAME(fso, "Chroma Smoothing Fragment Shader");

      plconfig.vertex_shader = vso.get();
      plconfig.fragment_shader = fso.get();
      if (!(m_chroma_smoothing_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
        return false;
      GL_OBJECT_NAME(m_chroma_smoothing_pipeline, "Chroma Smoothing Pipeline");
    }
  }

  return true;
}

void GPUPresenter::ClearDisplay()
{
  ClearDisplayTexture();

  // Just recycle the textures, it'll get re-fetched.
  DestroyDeinterlaceTextures();
}

void GPUPresenter::ClearDisplayTexture()
{
  m_display_texture = nullptr;
  m_display_texture_view_x = 0;
  m_display_texture_view_y = 0;
  m_display_texture_view_width = 0;
  m_display_texture_view_height = 0;
}

void GPUPresenter::SetDisplayParameters(u16 display_width, u16 display_height, u16 display_origin_left,
                                        u16 display_origin_top, u16 display_vram_width, u16 display_vram_height,
                                        float display_pixel_aspect_ratio, bool display_24bit)
{
  m_display_width = display_width;
  m_display_height = display_height;
  m_display_origin_left = display_origin_left;
  m_display_origin_top = display_origin_top;
  m_display_vram_width = display_vram_width;
  m_display_vram_height = display_vram_height;
  m_display_pixel_aspect_ratio = display_pixel_aspect_ratio;
  m_display_texture_24bit = display_24bit;
}

void GPUPresenter::SetDisplayTexture(GPUTexture* texture, s32 view_x, s32 view_y, s32 view_width, s32 view_height)
{
  DebugAssert(texture);

  if (g_gpu_settings.display_auto_resize_window &&
      (view_width != m_display_texture_view_width || view_height != m_display_texture_view_height))
  {
    Host::RunOnCPUThread([]() { System::RequestDisplaySize(); });
  }

  m_display_texture = texture;
  m_display_texture_view_x = view_x;
  m_display_texture_view_y = view_y;
  m_display_texture_view_width = view_width;
  m_display_texture_view_height = view_height;
}

GPUDevice::PresentResult GPUPresenter::RenderDisplay(GPUTexture* target, const GSVector2i target_size, bool postfx,
                                                     bool apply_aspect_ratio) const
{
  GL_SCOPE_FMT("RenderDisplay: {}x{}", target_size.x, target_size.y);

  if (m_display_texture)
    m_display_texture->MakeReadyForSampling();

  DebugAssert(target || g_gpu_device->HasMainSwapChain());
  DebugAssert(!postfx || target_size.eq(g_gpu_device->GetMainSwapChain()->GetSizeVec()));

  GPUSwapChain* const swap_chain = g_gpu_device->GetMainSwapChain();
  const WindowInfo::PreRotation prerotation = target ? WindowInfo::PreRotation::Identity : swap_chain->GetPreRotation();
  const GSVector2i final_target_size = target ? target->GetSizeVec() : swap_chain->GetPostRotatedSizeVec();
  const bool is_vram_view = g_gpu_settings.gpu_show_vram;
  const bool integer_scale = g_gpu_settings.IsUsingIntegerDisplayScaling(m_display_texture_24bit);
  const bool have_overlay = (postfx && !is_vram_view && HasBorderOverlay());
  const bool have_prerotation = (prerotation != WindowInfo::PreRotation::Identity);
  GL_INS(have_overlay ? "Overlay is ENABLED" : "Overlay is disabled");
  GL_INS_FMT("Prerotation: {}", static_cast<u32>(prerotation));
  GL_INS_FMT("Final target size: {}x{}", target_size.x, target_size.y);

  // Compute draw area.
  GSVector4i display_rect, display_rect_without_overlay;
  GSVector4i draw_rect, draw_rect_without_overlay;
  GSVector4i overlay_display_rect = GSVector4i::zero();
  GSVector4i overlay_rect = GSVector4i::zero();
  if (have_overlay)
  {
    overlay_rect = GSVector4i::rfit(GSVector4i::loadh(target_size), m_border_overlay_texture->GetSizeVec());

    // Align the overlay rectangle to the top/bottom if requested.
    const GSVector2i overlay_rect_size = overlay_rect.rsize();
    if (g_gpu_settings.display_alignment == DisplayAlignment::LeftOrTop)
      overlay_rect = overlay_rect.sub32(overlay_rect.xyxy());
    else if (g_gpu_settings.display_alignment == DisplayAlignment::RightOrBottom)
      overlay_rect = GSVector4i::xyxy(target_size).sub32(GSVector4i(overlay_rect_size));

    const GSVector2 scale = GSVector2(overlay_rect_size) / GSVector2(m_border_overlay_texture->GetSizeVec());
    overlay_display_rect =
      GSVector4i(GSVector4(m_border_overlay_display_rect) * GSVector4::xyxy(scale)).add32(overlay_rect.xyxy());

    // Draw to the overlay area instead of the whole screen. Always align in center, we align the overlay instead.
    CalculateDrawRect(overlay_display_rect.width(), overlay_display_rect.height(), apply_aspect_ratio, integer_scale,
                      false, &display_rect_without_overlay, &draw_rect_without_overlay);

    // Apply overlay area offset.
    display_rect = display_rect_without_overlay.add32(overlay_display_rect.xyxy());
    draw_rect = draw_rect_without_overlay.add32(overlay_display_rect.xyxy());
  }
  else
  {
    CalculateDrawRect(target_size.x, target_size.y, apply_aspect_ratio, integer_scale, true,
                      &display_rect_without_overlay, &draw_rect_without_overlay);
    display_rect = display_rect_without_overlay;
    draw_rect = draw_rect_without_overlay;
  }

  // There's a bunch of scenarios where we need to use intermediate buffers.
  // If we have post-processing and overlays enabled, postfx needs to happen on an intermediate buffer first.
  // If pre-rotation is enabled with post-processing, we need to draw to an intermediate buffer, and apply the
  // rotation at the end. Unscaled/slang post-processing applies rotation after post-processing.
  bool postfx_active = (postfx && !is_vram_view && m_display_postfx && m_display_postfx->IsActive());
  bool postfx_delayed_rotation = false;
  if (postfx_active)
  {
    // Viewport is consistent, but dependent on border overlay.
    GSVector2i postfx_source_size = CalculateDisplayPostProcessSourceSize();
    GSVector2i postfx_viewport_size = display_rect.rsize();
    GSVector2i postfx_target_size = (have_overlay ? overlay_display_rect.rsize() : target_size);

    // If we're using unscaled post-processing, then we do the post-processing without rotation and apply it later.
    if (m_display_postfx->WantsUnscaledInput() &&
        (postfx_delayed_rotation = (g_gpu_settings.display_rotation == DisplayRotation::Rotate90 ||
                                    g_gpu_settings.display_rotation == DisplayRotation::Rotate270)))
    {
      postfx_target_size = postfx_target_size.yx();
      postfx_viewport_size = postfx_viewport_size.yx();
    }

    // This could fail if we run out of VRAM.
    if ((postfx_active = m_display_postfx->CheckTargets(postfx_source_size.x, postfx_source_size.y, m_present_format,
                                                        postfx_target_size.x, postfx_target_size.y,
                                                        postfx_viewport_size.x, postfx_viewport_size.y)))
    {
      GL_INS("Post-processing is ACTIVE this frame");
      GL_INS_FMT("Post-processing source size: {}x{}", postfx_source_size.x, postfx_source_size.y);
      GL_INS_FMT("Post-processing target size: {}x{}", postfx_target_size.x, postfx_target_size.y);
      GL_INS_FMT("Post-processing viewport size: {}x{}", postfx_viewport_size.x, postfx_viewport_size.y);
      GL_INS_FMT("Post-processing input texture size: {}x{}", m_display_postfx->GetInputTexture()->GetWidth(),
                 m_display_postfx->GetInputTexture()->GetHeight());
    }
  }

  // Helper to bind swap chain/final target.
  const auto bind_final_target = [&target, &swap_chain, &final_target_size](bool clear) {
    if (target)
    {
      if (clear)
        g_gpu_device->ClearRenderTarget(target, GPUDevice::DEFAULT_CLEAR_COLOR);
      else
        g_gpu_device->InvalidateRenderTarget(target);
      g_gpu_device->SetRenderTarget(target);
    }
    else
    {
      const GPUDevice::PresentResult res = g_gpu_device->BeginPresent(swap_chain);
      if (res != GPUDevice::PresentResult::OK)
        return res;
    }

    g_gpu_device->SetViewport(GSVector4i::loadh(final_target_size));
    return GPUDevice::PresentResult::OK;
  };

  // If postfx is enabled, we need to draw to an intermediate buffer first.
  if (postfx_active)
  {
    // Display is always drawn to the postfx input.
    GPUTexture* postfx_input = GetDisplayPostProcessInputTexture(
      draw_rect_without_overlay, postfx_delayed_rotation ? DisplayRotation::Normal : g_gpu_settings.display_rotation);
    postfx_input->MakeReadyForSampling();

    // Apply postprocessing to an intermediate texture if we're prerotating or have an overlay.
    if (have_prerotation || have_overlay || postfx_delayed_rotation)
    {
      GPUTexture* const postfx_output = m_display_postfx->GetTextureUnusedAtEndOfChain();
      const GSVector4i postfx_final_rect =
        postfx_delayed_rotation ? display_rect_without_overlay.yxwz() : display_rect_without_overlay;
      ApplyDisplayPostProcess(postfx_output, postfx_input, postfx_final_rect, postfx_output->GetSizeVec());
      postfx_output->MakeReadyForSampling();

      // Start draw to final buffer.
      if (const GPUDevice::PresentResult pres = bind_final_target(have_overlay); pres != GPUDevice::PresentResult::OK)
        return pres;

      // UVs of post-processed/prerotated output are flipped in OpenGL.
      const GSVector4 src_uv_rect = g_gpu_device->UsesLowerLeftOrigin() ? GSVector4::cxpr(0.0f, 1.0f, 1.0f, 0.0f) :
                                                                          GSVector4::cxpr(0.0f, 0.0f, 1.0f, 1.0f);

      // If we have an overlay, draw it, and then copy the postprocessed framebuffer in.
      const DisplayRotation present_rotation =
        postfx_delayed_rotation ? g_gpu_settings.display_rotation : DisplayRotation::Normal;
      if (have_overlay)
      {
        GL_SCOPE_FMT("Draw overlay and postfx buffer");
        g_gpu_device->SetPipeline(m_border_overlay_pipeline.get());
        g_gpu_device->SetTextureSampler(0, m_border_overlay_texture.get(), g_gpu_device->GetLinearSampler());
        DrawScreenQuad(overlay_rect, GSVector4::cxpr(0.0f, 0.0f, 1.0f, 1.0f), target_size, final_target_size,
                       DisplayRotation::Normal, prerotation, nullptr, 0);

        g_gpu_device->SetPipeline(m_present_copy_blend_pipeline.get());
        g_gpu_device->SetTextureSampler(0, postfx_output, g_gpu_device->GetNearestSampler());
        DrawScreenQuad(overlay_display_rect, src_uv_rect, target_size, final_target_size, present_rotation, prerotation,
                       nullptr, 0);
      }
      else
      {
        // Otherwise, just copy the framebuffer.
        GL_SCOPE_FMT("Copy framebuffer for prerotation");
        g_gpu_device->SetPipeline(m_present_copy_pipeline.get());
        g_gpu_device->SetTextureSampler(0, postfx_output, g_gpu_device->GetNearestSampler());
        DrawScreenQuad(GSVector4i::loadh(target_size), src_uv_rect, target_size, final_target_size, present_rotation,
                       prerotation, nullptr, 0);
      }

      // All done
      return GPUDevice::PresentResult::OK;
    }
    else
    {
      // Otherwise apply postprocessing directly to swap chain.
      return ApplyDisplayPostProcess(target, postfx_input, display_rect, target_size);
    }
  }
  else
  {
    // The non-postprocessing cases are much simpler. We always optionally draw the overlay, then draw the display.
    // The only tricky bit is we have to combine the display rotation and prerotation for the latter.
    if (const GPUDevice::PresentResult pres = bind_final_target(true); pres != GPUDevice::PresentResult::OK)
      return pres;

    if (have_overlay)
    {
      GL_SCOPE_FMT("Draw overlay to {}", overlay_rect);
      g_gpu_device->SetPipeline(m_border_overlay_pipeline.get());
      g_gpu_device->SetTextureSampler(0, m_border_overlay_texture.get(), g_gpu_device->GetLinearSampler());

      DrawScreenQuad(overlay_rect, GSVector4::cxpr(0.0f, 0.0f, 1.0f, 1.0f), target_size, final_target_size,
                     DisplayRotation::Normal, prerotation, nullptr, 0);

      if (!overlay_display_rect.eq(draw_rect))
      {
        DrawOverlayBorders(target_size, final_target_size, overlay_display_rect,
                           m_display_texture ? draw_rect : draw_rect.xyxy(), prerotation);
      }
    }

    if (m_display_texture)
    {
      DrawDisplay(target_size, final_target_size, draw_rect, have_overlay && m_border_overlay_destination_alpha_blend,
                  g_gpu_settings.display_rotation, prerotation);
    }

    return GPUDevice::PresentResult::OK;
  }
}

void GPUPresenter::DrawOverlayBorders(const GSVector2i target_size, const GSVector2i final_target_size,
                                      const GSVector4i overlay_display_rect, const GSVector4i draw_rect,
                                      const WindowInfo::PreRotation prerotation) const
{
  GL_SCOPE_FMT("Fill in overlay borders - odisplay={}, draw={}", overlay_display_rect, draw_rect);

  const GSVector2i overlay_display_rect_size = overlay_display_rect.rsize();
  const GSVector4i overlay_display_rect_offset = overlay_display_rect.xyxy();
  const GSVector4i draw_rect_inside_overlay = draw_rect.sub32(overlay_display_rect_offset);
  const GSVector4i padding =
    GSVector4i::xyxy(draw_rect_inside_overlay.xy(), overlay_display_rect_size.sub32(draw_rect_inside_overlay.zw()));

  GPUBackend::ScreenVertex* vertices;
  u32 space;
  u32 base_vertex;
  g_gpu_device->MapVertexBuffer(sizeof(GPUBackend::ScreenVertex), 24, reinterpret_cast<void**>(&vertices), &space,
                                &base_vertex);

  u32 vertex_count = 0;
  const auto add_rect = [&](const GSVector4i& rc) {
    const GSVector4i screen_rect = overlay_display_rect_offset.add32(rc);
    const GSVector4 xy = GPUBackend::GetScreenQuadClipSpaceCoordinates(
      GPUSwapChain::PreRotateClipRect(prerotation, target_size, screen_rect), final_target_size);
    const GSVector2 uv = GSVector2::zero();
    vertices[vertex_count + 0].Set(xy.xy(), uv);
    vertices[vertex_count + 1].Set(xy.zyzw().xy(), uv);
    vertices[vertex_count + 2].Set(xy.xwzw().xy(), uv);
    vertices[vertex_count + 3].Set(xy.zyzw().xy(), uv);
    vertices[vertex_count + 4].Set(xy.xwzw().xy(), uv);
    vertices[vertex_count + 5].Set(xy.zw(), uv);
    vertex_count += 6;
  };

  const s32 left_padding = padding.left;
  const s32 top_padding = padding.top;
  const s32 right_padding = padding.right;
  const s32 bottom_padding = padding.bottom;
  GL_INS_FMT("Padding: left={}, top={}, right={}, bottom={}", left_padding, top_padding, right_padding, bottom_padding);

  // this is blended, so be careful not to overlap two rects
  if (left_padding > 0)
  {
    add_rect(GSVector4i(0, 0, left_padding, overlay_display_rect_size.y));
  }
  if (top_padding > 0)
  {
    add_rect(GSVector4i((left_padding > 0) ? left_padding : 0, 0,
                        overlay_display_rect_size.x - ((right_padding > 0) ? right_padding : 0), top_padding));
  }
  if (right_padding > 0)
  {
    add_rect(GSVector4i(overlay_display_rect_size.x - right_padding, 0, overlay_display_rect_size.x,
                        overlay_display_rect_size.y));
  }
  if (bottom_padding > 0)
  {
    add_rect(GSVector4i((left_padding > 0) ? left_padding : 0, overlay_display_rect_size.y - bottom_padding,
                        overlay_display_rect_size.x - ((right_padding > 0) ? right_padding : 0),
                        overlay_display_rect_size.y));
  }

  g_gpu_device->UnmapVertexBuffer(sizeof(GPUBackend::ScreenVertex), vertex_count);
  if (vertex_count > 0)
  {
    const GSVector4i scissor = GPUSwapChain::PreRotateClipRect(prerotation, target_size, overlay_display_rect);
    g_gpu_device->SetScissor(
      g_gpu_device->UsesLowerLeftOrigin() ? GPUDevice::FlipToLowerLeft(scissor, final_target_size.y) : scissor);
    g_gpu_device->SetPipeline(m_present_clear_pipeline.get());
    g_gpu_device->Draw(vertex_count, base_vertex);
  }
}

void GPUPresenter::DrawDisplay(const GSVector2i target_size, const GSVector2i final_target_size,
                               const GSVector4i display_rect, bool dst_alpha_blend, DisplayRotation rotation,
                               WindowInfo::PreRotation prerotation) const
{
  bool texture_filter_linear = false;

  struct alignas(16) Uniforms
  {
    float src_size[4];
    float clamp_rect[4];
    float params[4];
  } uniforms;
  std::memset(uniforms.params, 0, sizeof(uniforms.params));

  const GSVector2 display_texture_size = GSVector2(m_display_texture->GetSizeVec());

  switch (m_display_texture_24bit ? g_gpu_settings.display_scaling_24bit : g_gpu_settings.display_scaling)
  {
    case DisplayScalingMode::Nearest:
    case DisplayScalingMode::NearestInteger:
      break;

    case DisplayScalingMode::BilinearSmooth:
    case DisplayScalingMode::BilinearInteger:
      texture_filter_linear = true;
      break;

    case DisplayScalingMode::Lanczos:
    {
      const GSVector2 fdisplay_rect_size = GSVector2(display_rect.rsize());
      GSVector2::store<true>(&uniforms.params[0], fdisplay_rect_size);
      GSVector2::store<true>(&uniforms.params[2], GSVector2::cxpr(1.0f) / (fdisplay_rect_size / display_texture_size));
    }
    break;

    case DisplayScalingMode::BilinearHybrid:
    case DisplayScalingMode::BilinearSharp:
    {
      texture_filter_linear = true;
      uniforms.params[0] = std::max(
        std::floor(static_cast<float>(display_rect.width()) / static_cast<float>(m_display_texture_view_width)), 1.0f);
      uniforms.params[1] = std::max(
        std::floor(static_cast<float>(display_rect.height()) / static_cast<float>(m_display_texture_view_height)),
        1.0f);
      uniforms.params[2] = 0.5f - 0.5f / uniforms.params[0];
      uniforms.params[3] = 0.5f - 0.5f / uniforms.params[1];
    }
    break;

    default:
      UnreachableCode();
      break;
  }

  if (m_display_texture_24bit && m_display_24bit_pipeline)
    g_gpu_device->SetPipeline(dst_alpha_blend ? m_display_24bit_blend_pipeline.get() : m_display_24bit_pipeline.get());
  else
    g_gpu_device->SetPipeline(dst_alpha_blend ? m_display_blend_pipeline.get() : m_display_pipeline.get());
  g_gpu_device->SetTextureSampler(
    0, m_display_texture, texture_filter_linear ? g_gpu_device->GetLinearSampler() : g_gpu_device->GetNearestSampler());

  // For bilinear, clamp to 0.5/SIZE-0.5 to avoid bleeding from the adjacent texels in VRAM. This is because
  // 1.0 in UV space is not the bottom-right texel, but a mix of the bottom-right and wrapped/next texel.
  const GSVector4 display_texture_size4 = GSVector4::xyxy(display_texture_size);
  const GSVector4 uv_rect = GSVector4(GSVector4i(m_display_texture_view_x, m_display_texture_view_y,
                                                 m_display_texture_view_x + m_display_texture_view_width,
                                                 m_display_texture_view_y + m_display_texture_view_height)) /
                            display_texture_size4;
  GSVector4::store<true>(
    uniforms.clamp_rect,
    GSVector4(static_cast<float>(m_display_texture_view_x) + 0.5f, static_cast<float>(m_display_texture_view_y) + 0.5f,
              static_cast<float>(m_display_texture_view_x + m_display_texture_view_width) - 0.5f,
              static_cast<float>(m_display_texture_view_y + m_display_texture_view_height) - 0.5f) /
      display_texture_size4);
  GSVector4::store<true>(uniforms.src_size,
                         GSVector4::xyxy(display_texture_size, GSVector2::cxpr(1.0f) / display_texture_size));

  DrawScreenQuad(display_rect, uv_rect, target_size, final_target_size, rotation, prerotation, &uniforms,
                 sizeof(uniforms));
}

void GPUPresenter::DrawScreenQuad(const GSVector4i rect, const GSVector4 uv_rect, const GSVector2i target_size,
                                  const GSVector2i final_target_size, DisplayRotation rotation,
                                  WindowInfo::PreRotation prerotation, const void* push_constants,
                                  u32 push_constants_size)
{
  const GSVector4i real_rect = GPUSwapChain::PreRotateClipRect(prerotation, target_size, rect);
  g_gpu_device->SetScissor(
    g_gpu_device->UsesLowerLeftOrigin() ? GPUDevice::FlipToLowerLeft(real_rect, final_target_size.y) : real_rect);

  GPUBackend::ScreenVertex* vertices;
  u32 space;
  u32 base_vertex;
  g_gpu_device->MapVertexBuffer(sizeof(GPUBackend::ScreenVertex), 4, reinterpret_cast<void**>(&vertices), &space,
                                &base_vertex);

  // Combine display rotation and prerotation together, since the rectangle has already been adjusted.
  const GSVector4 xy = GPUBackend::GetScreenQuadClipSpaceCoordinates(real_rect, final_target_size);
  const DisplayRotation effective_rotation = static_cast<DisplayRotation>(
    (static_cast<u32>(rotation) + static_cast<u32>(prerotation)) % static_cast<u32>(DisplayRotation::Count));
  switch (effective_rotation)
  {
    case DisplayRotation::Normal:
      vertices[0].Set(xy.xy(), uv_rect.xy());
      vertices[1].Set(xy.zyzw().xy(), uv_rect.zyzw().xy());
      vertices[2].Set(xy.xwzw().xy(), uv_rect.xwzw().xy());
      vertices[3].Set(xy.zw(), uv_rect.zw());
      break;

    case DisplayRotation::Rotate90:
      vertices[0].Set(xy.xy(), uv_rect.xwzw().xy());
      vertices[1].Set(xy.zyzw().xy(), uv_rect.xy());
      vertices[2].Set(xy.xwzw().xy(), uv_rect.zw());
      vertices[3].Set(xy.zw(), uv_rect.zyzw().xy());
      break;

    case DisplayRotation::Rotate180:
      vertices[0].Set(xy.xy(), uv_rect.xwzw().xy());
      vertices[1].Set(xy.zyzw().xy(), uv_rect.zw());
      vertices[2].Set(xy.xwzw().xy(), uv_rect.xy());
      vertices[3].Set(xy.zw(), uv_rect.zyzw().xy());
      break;

    case DisplayRotation::Rotate270:
      vertices[0].Set(xy.xy(), uv_rect.zyzw().xy());
      vertices[1].Set(xy.zyzw().xy(), uv_rect.zw());
      vertices[2].Set(xy.xwzw().xy(), uv_rect.xy());
      vertices[3].Set(xy.zw(), uv_rect.xwzw().xy());
      break;

      DefaultCaseIsUnreachable();
  }

  g_gpu_device->UnmapVertexBuffer(sizeof(GPUBackend::ScreenVertex), 4);
  if (push_constants_size > 0)
    g_gpu_device->DrawWithPushConstants(4, base_vertex, push_constants, push_constants_size);
  else
    g_gpu_device->Draw(4, base_vertex);
}

GSVector2i GPUPresenter::CalculateDisplayPostProcessSourceSize() const
{
  DebugAssert(m_display_postfx);

  // Unscaled is easy.
  if (!m_display_postfx->WantsUnscaledInput())
  {
    // Render to an input texture that's viewport sized. Source is the "real" input texture.
    return GSVector2i(m_display_texture_view_width, m_display_texture_view_height);
  }
  else
  {
    // Need to include the borders in the size. This is very janky, since we need to correct upscaling.
    // Source and input is the full display texture size (including padding).
    const GSVector2i input_size = GSVector2i(m_display_texture_view_width, m_display_texture_view_height);
    const GSVector2i native_size = GSVector2i(m_display_vram_width, m_display_vram_height);
    const GSVector2i native_display_size = GSVector2i(m_display_width, m_display_height);
    const GSVector2 scale = GSVector2(input_size) / GSVector2(native_size);
    return GSVector2i((GSVector2(native_display_size) * scale).ceil());
  }
}

GPUTexture* GPUPresenter::GetDisplayPostProcessInputTexture(const GSVector4i draw_rect_without_overlay,
                                                            DisplayRotation rotation) const
{
  DebugAssert(m_display_postfx);

  GPUTexture* postfx_input;
  if (!m_display_postfx->WantsUnscaledInput() || !m_display_texture)
  {
    // Render to postfx input as if it was the final display.
    postfx_input = m_display_postfx->GetInputTexture();
    g_gpu_device->ClearRenderTarget(postfx_input, GPUDevice::DEFAULT_CLEAR_COLOR);
    if (m_display_texture)
    {
      const GSVector2i postfx_input_size = postfx_input->GetSizeVec();
      g_gpu_device->SetRenderTarget(postfx_input);
      g_gpu_device->SetViewport(GSVector4i::loadh(postfx_input_size));

      DrawDisplay(postfx_input_size, postfx_input_size, draw_rect_without_overlay, false, rotation,
                  WindowInfo::PreRotation::Identity);
    }
  }
  else
  {
    postfx_input = m_display_texture;

    // OpenGL needs to flip the correct way around. If the source is exactly the same size without any correction we can
    // pass it through to the chain directly. Except if the swap chain isn't using BGRA8, then we need to blit too.
    if (g_gpu_device->UsesLowerLeftOrigin() || rotation != DisplayRotation::Normal || m_display_origin_left != 0 ||
        m_display_origin_top != 0 || m_display_vram_width != m_display_texture_view_width ||
        m_display_vram_height != m_display_texture_view_height || m_display_texture->GetFormat() != m_present_format)
    {
      GL_SCOPE_FMT("Pre-process postfx source");

      const GSVector2i input_size = GSVector2i(m_display_texture_view_width, m_display_texture_view_height);
      const GSVector2i native_size = GSVector2i(m_display_vram_width, m_display_vram_height);
      const GSVector2 input_scale = GSVector2(input_size) / GSVector2(native_size);
      const GSVector4i input_draw_rect = GSVector4i(
        (GSVector4(GSVector4i(m_display_origin_left, m_display_origin_top, m_display_origin_left + m_display_vram_width,
                              m_display_origin_top + m_display_vram_height)) *
         GSVector4::xyxy(input_scale))
          .floor());

      const GSVector4 src_uv_rect = GSVector4(GSVector4i(m_display_texture_view_x, m_display_texture_view_y,
                                                         m_display_texture_view_x + m_display_texture_view_width,
                                                         m_display_texture_view_y + m_display_texture_view_height)) /
                                    GSVector4::xyxy(GSVector2(m_display_texture->GetSizeVec()));

      postfx_input = m_display_postfx->GetInputTexture();
      m_display_texture->MakeReadyForSampling();

      const GSVector2i postfx_input_size = postfx_input->GetSizeVec();
      g_gpu_device->ClearRenderTarget(postfx_input, GPUDevice::DEFAULT_CLEAR_COLOR);
      g_gpu_device->SetRenderTarget(postfx_input);
      g_gpu_device->SetViewportAndScissor(GSVector4i::loadh(postfx_input_size));
      g_gpu_device->SetPipeline(m_present_copy_pipeline.get());
      g_gpu_device->SetTextureSampler(0, m_display_texture, g_gpu_device->GetNearestSampler());
      DrawScreenQuad(input_draw_rect, src_uv_rect, postfx_input_size, postfx_input_size, rotation,
                     WindowInfo::PreRotation::Identity, nullptr, 0);
    }
    else if (m_display_texture_view_x != 0 || m_display_texture_view_y != 0 ||
             m_display_texture->GetWidth() != static_cast<u32>(m_display_texture_view_width) ||
             m_display_texture->GetHeight() != static_cast<u32>(m_display_texture_view_height))
    {
      GL_SCOPE_FMT("Copy postfx source");

      postfx_input = m_display_postfx->GetInputTexture();
      g_gpu_device->CopyTextureRegion(postfx_input, 0, 0, 0, 0, m_display_texture, m_display_texture_view_x,
                                      m_display_texture_view_y, 0, 0, m_display_texture_view_width,
                                      m_display_texture_view_height);
    }
  }

  return postfx_input;
}

GPUDevice::PresentResult GPUPresenter::ApplyDisplayPostProcess(GPUTexture* target, GPUTexture* input,
                                                               const GSVector4i display_rect,
                                                               const GSVector2i postfx_size) const
{
  DebugAssert(!g_gpu_settings.gpu_show_vram);

  if (!m_display_texture)
  {
    // Avoid passing invalid rectangles into the postfx backend.
    return m_display_postfx->Apply(input, nullptr, target, GSVector4i::loadh(postfx_size), postfx_size.x, postfx_size.y,
                                   postfx_size.x, postfx_size.y);
  }

  // "original size" in postfx includes padding.
  const float upscale_x =
    m_display_texture ? static_cast<float>(m_display_texture_view_width) / static_cast<float>(m_display_vram_width) :
                        1.0f;
  const float upscale_y =
    m_display_texture ? static_cast<float>(m_display_texture_view_height) / static_cast<float>(m_display_vram_height) :
                        1.0f;
  const s32 orig_width = static_cast<s32>(std::ceil(static_cast<float>(m_display_width) * upscale_x));
  const s32 orig_height = static_cast<s32>(std::ceil(static_cast<float>(m_display_height) * upscale_y));

  return m_display_postfx->Apply(input, nullptr, target, display_rect, orig_width, orig_height, m_display_width,
                                 m_display_height);
}

void GPUPresenter::SendDisplayToMediaCapture(MediaCapture* cap)
{
  GPUTexture* target = cap->GetRenderTexture();
  if (!target) [[unlikely]]
  {
    WARNING_LOG("Failed to get video capture render texture.");
    Host::RunOnCPUThread(&System::StopMediaCapture);
    return;
  }

  const bool apply_aspect_ratio =
    (g_gpu_settings.display_screenshot_mode != DisplayScreenshotMode::UncorrectedInternalResolution);
  const bool postfx =
    (g_gpu_settings.display_screenshot_mode == DisplayScreenshotMode::ScreenResolution &&
     g_gpu_device->HasMainSwapChain() && target->GetSizeVec().eq(g_gpu_device->GetMainSwapChain()->GetSizeVec()));

  if (RenderDisplay(target, target->GetSizeVec(), postfx, apply_aspect_ratio) != GPUDevice::PresentResult::OK ||
      !cap->DeliverVideoFrame(target)) [[unlikely]]
  {
    WARNING_LOG("Failed to render/deliver video capture frame.");
    Host::RunOnCPUThread(&System::StopMediaCapture);
    return;
  }
}

void GPUPresenter::DestroyDeinterlaceTextures()
{
  for (std::unique_ptr<GPUTexture>& tex : m_deinterlace_buffers)
    g_gpu_device->RecycleTexture(std::move(tex));
  g_gpu_device->RecycleTexture(std::move(m_deinterlace_texture));
  m_current_deinterlace_buffer = 0;
}

bool GPUPresenter::Deinterlace(u32 field)
{
  GPUTexture* src = m_display_texture;
  const u32 x = m_display_texture_view_x;
  const u32 y = m_display_texture_view_y;
  const u32 width = m_display_texture_view_width;
  const u32 height = m_display_texture_view_height;

  const auto copy_to_field_buffer = [&](u32 buffer) {
    if (!g_gpu_device->ResizeTexture(&m_deinterlace_buffers[buffer], width, height, GPUTexture::Type::Texture,
                                     src->GetFormat(), GPUTexture::Flags::None, false)) [[unlikely]]
    {
      return false;
    }

    GL_OBJECT_NAME_FMT(m_deinterlace_buffers[buffer], "Blend Deinterlace Buffer {}", buffer);

    GL_INS_FMT("Copy {}x{} from {},{} to field buffer {}", width, height, x, y, buffer);
    g_gpu_device->CopyTextureRegion(m_deinterlace_buffers[buffer].get(), 0, 0, 0, 0, m_display_texture, x, y, 0, 0,
                                    width, height);
    return true;
  };

  src->MakeReadyForSampling();

  switch (g_gpu_settings.display_deinterlacing_mode)
  {
    case DisplayDeinterlacingMode::Disabled:
    {
      GL_INS("Deinterlacing disabled, displaying field texture");
      return true;
    }

    case DisplayDeinterlacingMode::Weave:
    {
      GL_SCOPE_FMT("DeinterlaceWeave({{{},{}}}, {}x{}, field={})", x, y, width, height, field);

      const u32 full_height = height * 2;
      if (!DeinterlaceSetTargetSize(width, full_height, true)) [[unlikely]]
      {
        ClearDisplayTexture();
        return false;
      }

      src->MakeReadyForSampling();

      g_gpu_device->SetRenderTarget(m_deinterlace_texture.get());
      g_gpu_device->SetPipeline(m_deinterlace_pipeline.get());
      g_gpu_device->SetTextureSampler(0, src, g_gpu_device->GetNearestSampler());
      g_gpu_device->SetViewportAndScissor(0, 0, width, full_height);

      const u32 uniforms[4] = {x, y, field, 0};
      g_gpu_device->DrawWithPushConstants(3, 0, uniforms, sizeof(uniforms));

      m_deinterlace_texture->MakeReadyForSampling();
      SetDisplayTexture(m_deinterlace_texture.get(), 0, 0, width, full_height);
      return true;
    }

    case DisplayDeinterlacingMode::Blend:
    {
      constexpr u32 NUM_BLEND_BUFFERS = 2;

      GL_SCOPE_FMT("DeinterlaceBlend({{{},{}}}, {}x{}, field={})", x, y, width, height, field);

      const u32 this_buffer = m_current_deinterlace_buffer;
      m_current_deinterlace_buffer = (m_current_deinterlace_buffer + 1u) % NUM_BLEND_BUFFERS;
      GL_INS_FMT("Current buffer: {}", this_buffer);
      if (!DeinterlaceSetTargetSize(width, height, false) || !copy_to_field_buffer(this_buffer)) [[unlikely]]
      {
        ClearDisplayTexture();
        return false;
      }

      copy_to_field_buffer(this_buffer);

      // TODO: could be implemented with alpha blending instead..
      g_gpu_device->InvalidateRenderTarget(m_deinterlace_texture.get());
      g_gpu_device->SetRenderTarget(m_deinterlace_texture.get());
      g_gpu_device->SetPipeline(m_deinterlace_pipeline.get());
      g_gpu_device->SetTextureSampler(0, m_deinterlace_buffers[this_buffer].get(), g_gpu_device->GetNearestSampler());
      g_gpu_device->SetTextureSampler(1, m_deinterlace_buffers[(this_buffer - 1) % NUM_BLEND_BUFFERS].get(),
                                      g_gpu_device->GetNearestSampler());
      g_gpu_device->SetViewportAndScissor(0, 0, width, height);
      g_gpu_device->Draw(3, 0);

      m_deinterlace_texture->MakeReadyForSampling();
      SetDisplayTexture(m_deinterlace_texture.get(), 0, 0, width, height);
      return true;
    }

    case DisplayDeinterlacingMode::Adaptive:
    {
      GL_SCOPE_FMT("DeinterlaceAdaptive({{{},{}}}, {}x{}, field={})", x, y, width, height, field);

      const u32 this_buffer = m_current_deinterlace_buffer;
      const u32 full_height = height * 2;
      m_current_deinterlace_buffer = (m_current_deinterlace_buffer + 1u) % DEINTERLACE_BUFFER_COUNT;
      GL_INS_FMT("Current buffer: {}", this_buffer);
      if (!DeinterlaceSetTargetSize(width, full_height, false) || !copy_to_field_buffer(this_buffer)) [[unlikely]]
      {
        ClearDisplayTexture();
        return false;
      }

      g_gpu_device->SetRenderTarget(m_deinterlace_texture.get());
      g_gpu_device->SetPipeline(m_deinterlace_pipeline.get());
      g_gpu_device->SetTextureSampler(0, m_deinterlace_buffers[this_buffer].get(), g_gpu_device->GetNearestSampler());
      g_gpu_device->SetTextureSampler(1, m_deinterlace_buffers[(this_buffer - 1) % DEINTERLACE_BUFFER_COUNT].get(),
                                      g_gpu_device->GetNearestSampler());
      g_gpu_device->SetTextureSampler(2, m_deinterlace_buffers[(this_buffer - 2) % DEINTERLACE_BUFFER_COUNT].get(),
                                      g_gpu_device->GetNearestSampler());
      g_gpu_device->SetTextureSampler(3, m_deinterlace_buffers[(this_buffer - 3) % DEINTERLACE_BUFFER_COUNT].get(),
                                      g_gpu_device->GetNearestSampler());
      g_gpu_device->SetViewportAndScissor(0, 0, width, full_height);

      const u32 uniforms[] = {field, full_height};
      g_gpu_device->DrawWithPushConstants(3, 0, uniforms, sizeof(uniforms));

      m_deinterlace_texture->MakeReadyForSampling();
      SetDisplayTexture(m_deinterlace_texture.get(), 0, 0, width, full_height);
      return true;
    }

    default:
      UnreachableCode();
  }
}

bool GPUPresenter::DeinterlaceSetTargetSize(u32 width, u32 height, bool preserve)
{
  if (!g_gpu_device->ResizeTexture(&m_deinterlace_texture, width, height, GPUTexture::Type::RenderTarget,
                                   GPUTexture::Format::RGBA8, GPUTexture::Flags::None, preserve)) [[unlikely]]
  {
    return false;
  }

  GL_OBJECT_NAME(m_deinterlace_texture, "Deinterlace target texture");
  return true;
}

bool GPUPresenter::ApplyChromaSmoothing()
{
  const u32 x = m_display_texture_view_x;
  const u32 y = m_display_texture_view_y;
  const u32 width = m_display_texture_view_width;
  const u32 height = m_display_texture_view_height;
  if (!g_gpu_device->ResizeTexture(&m_chroma_smoothing_texture, width, height, GPUTexture::Type::RenderTarget,
                                   GPUTexture::Format::RGBA8, GPUTexture::Flags::None, false))
  {
    ClearDisplayTexture();
    return false;
  }

  GL_OBJECT_NAME(m_chroma_smoothing_texture, "Chroma smoothing texture");

  GL_SCOPE_FMT("ApplyChromaSmoothing({{{},{}}}, {}x{})", x, y, width, height);

  m_display_texture->MakeReadyForSampling();
  g_gpu_device->InvalidateRenderTarget(m_chroma_smoothing_texture.get());
  g_gpu_device->SetRenderTarget(m_chroma_smoothing_texture.get());
  g_gpu_device->SetPipeline(m_chroma_smoothing_pipeline.get());
  g_gpu_device->SetTextureSampler(0, m_display_texture, g_gpu_device->GetNearestSampler());
  g_gpu_device->SetViewportAndScissor(0, 0, width, height);

  const u32 uniforms[] = {x, y, width - 1, height - 1};
  g_gpu_device->DrawWithPushConstants(3, 0, uniforms, sizeof(uniforms));

  m_chroma_smoothing_texture->MakeReadyForSampling();
  SetDisplayTexture(m_chroma_smoothing_texture.get(), 0, 0, width, height);
  return true;
}

void GPUPresenter::CalculateDrawRect(s32 window_width, s32 window_height, bool apply_aspect_ratio, bool integer_scale,
                                     bool apply_alignment, GSVector4i* display_rect, GSVector4i* draw_rect) const
{
  const bool show_vram = g_gpu_settings.gpu_show_vram;
  const u32 display_width = show_vram ? VRAM_WIDTH : m_display_width;
  const u32 display_height = show_vram ? VRAM_HEIGHT : m_display_height;
  const s32 display_origin_left = show_vram ? 0 : m_display_origin_left;
  const s32 display_origin_top = show_vram ? 0 : m_display_origin_top;
  const u32 display_vram_width = show_vram ? VRAM_WIDTH : m_display_vram_width;
  const u32 display_vram_height = show_vram ? VRAM_HEIGHT : m_display_vram_height;
  const float display_pixel_aspect_ratio = (show_vram || !apply_aspect_ratio) ? 1.0f : m_display_pixel_aspect_ratio;
  const DisplayRotation display_rotation = show_vram ? DisplayRotation::Normal : g_gpu_settings.display_rotation;
  const DisplayAlignment display_alignment =
    apply_alignment ? g_gpu_settings.display_alignment : DisplayAlignment::Center;
  GPU::CalculateDrawRect(window_width, window_height, display_width, display_height, display_origin_left,
                         display_origin_top, display_vram_width, display_vram_height, display_rotation,
                         display_alignment, display_pixel_aspect_ratio, integer_scale, display_rect, draw_rect);
}

bool GPUPresenter::PresentFrame(GPUPresenter* presenter, GPUBackend* backend, bool allow_skip_present, u64 present_time)
{
  const bool skip_present = (!g_gpu_device->HasMainSwapChain() ||
                             (allow_skip_present && g_gpu_device->GetMainSwapChain()->ShouldSkipPresentingFrame() &&
                              presenter && presenter->m_skipped_present_count < MAX_SKIPPED_PRESENT_COUNT));

  if (!skip_present)
  {
    // acquire for IO.MousePos and system state.
    std::atomic_thread_fence(std::memory_order_acquire);

    ImGuiManager::RenderDebugWindows();

    FullscreenUI::Render();

    if (backend)
      ImGuiManager::RenderTextOverlays(backend);

    ImGuiManager::RenderOverlayWindows();

    FullscreenUI::RenderOverlays();

    ImGuiManager::RenderOSDMessages();

    if (backend && !GPUThread::IsSystemPaused())
      ImGuiManager::RenderSoftwareCursors();

    ImGuiManager::CreateDrawLists();

    // render offscreen for transitions
    if (FullscreenUI::IsTransitionActive())
    {
      GPUTexture* const rtex = FullscreenUI::GetTransitionRenderTexture(g_gpu_device->GetMainSwapChain());
      if (rtex)
      {
        if (presenter)
          presenter->RenderDisplay(rtex, rtex->GetSizeVec(), true, true);
        else
          g_gpu_device->ClearRenderTarget(rtex, GPUDevice::DEFAULT_CLEAR_COLOR);

        g_gpu_device->SetRenderTarget(rtex);
        ImGuiManager::RenderDrawLists(rtex);
      }
    }
  }

  GPUSwapChain* const swap_chain = g_gpu_device->GetMainSwapChain();
  const GPUDevice::PresentResult pres = skip_present ?
                                          GPUDevice::PresentResult::SkipPresent :
                                          ((presenter && !FullscreenUI::IsTransitionActive()) ?
                                             presenter->RenderDisplay(nullptr, swap_chain->GetSizeVec(), true, true) :
                                             g_gpu_device->BeginPresent(swap_chain));
  if (pres == GPUDevice::PresentResult::OK)
  {
    if (presenter)
      presenter->m_skipped_present_count = 0;

    if (FullscreenUI::IsTransitionActive())
      FullscreenUI::RenderTransitionBlend(swap_chain);
    else
      ImGuiManager::RenderDrawLists(swap_chain);

    const GPUDevice::Features features = g_gpu_device->GetFeatures();
    const bool scheduled_present = (present_time != 0);
    const bool explicit_present = (scheduled_present && (features.explicit_present && !features.timed_present));
    const bool timed_present = (scheduled_present && features.timed_present);

    if (scheduled_present && !explicit_present)
    {
      // No explicit present support, simulate it with Flush.
      g_gpu_device->FlushCommands();
      SleepUntilPresentTime(present_time);
    }

    g_gpu_device->EndPresent(swap_chain, explicit_present, timed_present ? present_time : 0);

    if (g_gpu_device->IsGPUTimingEnabled())
      PerformanceCounters::AccumulateGPUTime();

    if (explicit_present)
    {
      SleepUntilPresentTime(present_time);
      g_gpu_device->SubmitPresent(swap_chain);
    }

    ImGuiManager::NewFrame();
  }
  else
  {
    if (presenter)
      presenter->m_skipped_present_count++;

    if (pres == GPUDevice::PresentResult::DeviceLost) [[unlikely]]
    {
      ERROR_LOG("GPU device lost during present.");
      GPUThread::ReportFatalErrorAndShutdown("GPU device lost. The log may contain more information.");
      return false;
    }

    if (pres == GPUDevice::PresentResult::ExclusiveFullscreenLost) [[unlikely]]
    {
      WARNING_LOG("Lost exclusive fullscreen.");
      Host::SetFullscreen(false);
    }

    if (!skip_present)
      g_gpu_device->FlushCommands();

    // Still need to kick ImGui or it gets cranky.
    if (!skip_present)
    {
      ImGui::EndFrame();
      ImGuiManager::NewFrame();
    }
  }

  return true;
}

void GPUPresenter::SleepUntilPresentTime(u64 present_time)
{
  // Use a spinwait if we undersleep for all platforms except android.. don't want to burn battery.
  // Linux also seems to do a much better job of waking up at the requested time.

#if !defined(__linux__) && !defined(__ANDROID__)
  Timer::SleepUntil(present_time, true);
#else
  Timer::SleepUntil(present_time, false);
#endif
}

bool GPUPresenter::RenderScreenshotToBuffer(u32 width, u32 height, bool postfx, bool apply_aspect_ratio,
                                            Image* out_image, Error* error)
{
  const ImageFormat image_format = GPUTexture::GetImageFormatForTextureFormat(m_present_format);
  if (image_format == ImageFormat::None)
    return false;

  auto render_texture = g_gpu_device->FetchAutoRecycleTexture(width, height, 1, 1, 1, GPUTexture::Type::RenderTarget,
                                                              m_present_format, GPUTexture::Flags::None);
  if (!render_texture)
    return false;

  g_gpu_device->ClearRenderTarget(render_texture.get(), GPUDevice::DEFAULT_CLEAR_COLOR);
  if (RenderDisplay(render_texture.get(), render_texture->GetSizeVec(), postfx, apply_aspect_ratio) !=
      GPUDevice::PresentResult::OK)
  {
    Error::SetStringView(error, "RenderDisplay() failed");
    return false;
  }

  Image image(width, height, image_format);

  std::unique_ptr<GPUDownloadTexture> dltex;
  if (g_gpu_device->GetFeatures().memory_import)
  {
    dltex = g_gpu_device->CreateDownloadTexture(width, height, m_present_format, image.GetPixels(),
                                                image.GetStorageSize(), image.GetPitch(), error);
  }
  if (!dltex)
  {
    if (!(dltex = g_gpu_device->CreateDownloadTexture(width, height, m_present_format, error)))
    {
      Error::AddPrefixFmt(error, "Failed to create {}x{} download texture: ", width, height);
      return false;
    }
  }

  dltex->CopyFromTexture(0, 0, render_texture.get(), 0, 0, width, height, 0, 0, false);
  if (!dltex->ReadTexels(0, 0, width, height, image.GetPixels(), image.GetPitch()))
    return false;

  *out_image = std::move(image);
  return true;
}

GSVector2i GPUPresenter::CalculateScreenshotSize(DisplayScreenshotMode mode) const
{
  if (m_display_texture_view_width != 0 && m_display_texture_view_height != 0)
  {
    if (g_gpu_settings.gpu_show_vram)
    {
      return GSVector2i(m_display_texture_view_width, m_display_texture_view_height);
    }
    else if (mode != DisplayScreenshotMode::ScreenResolution)
    {
      float f_width =
        m_display_width * (static_cast<float>(m_display_texture_view_width) / static_cast<float>(m_display_vram_width));
      float f_height = m_display_height *
                       (static_cast<float>(m_display_texture_view_height) / static_cast<float>(m_display_vram_height));
      if (mode != DisplayScreenshotMode::UncorrectedInternalResolution)
        GPU::ApplyPixelAspectRatioToSize(m_display_pixel_aspect_ratio, &f_width, &f_height);

      // DX11 won't go past 16K texture size.
      const float max_texture_size = static_cast<float>(g_gpu_device->GetMaxTextureSize());
      if (f_width > max_texture_size)
      {
        f_height = f_height / (f_width / max_texture_size);
        f_width = max_texture_size;
      }
      if (f_height > max_texture_size)
      {
        f_height = max_texture_size;
        f_width = f_width / (f_height / max_texture_size);
      }

      return GSVector2i(static_cast<s32>(std::ceil(f_width)), static_cast<s32>(std::ceil(f_height)));
    }
  }

  return g_gpu_device->HasMainSwapChain() ? g_gpu_device->GetMainSwapChain()->GetSizeVec() : GSVector2i(1, 1);
}

void GPUPresenter::LoadPostProcessingSettings(bool force_load)
{
  static constexpr const char* section = PostProcessing::Config::DISPLAY_CHAIN_SECTION;

  auto lock = Host::GetSettingsLock();
  const SettingsInterface& si = GetPostProcessingSettingsInterface(section);

  // This is the initial load, defer creating the chain until it's actually enabled if disabled.
  if (!force_load &&
      (!PostProcessing::Config::IsEnabled(si, section) || PostProcessing::Config::GetStageCount(si, section) == 0))
  {
    return;
  }

  m_display_postfx = std::make_unique<PostProcessing::Chain>(section);
  m_display_postfx->LoadStages(lock, si, true);
}

bool GPUPresenter::UpdatePostProcessingSettings(bool force_reload, Error* error)
{
  if (LoadOverlaySettings())
  {
    // something changed, need to recompile pipelines, the needed pipelines are based on alpha blend
    LoadOverlayTexture();
    if (!CompileDisplayPipelines(true, false, false, error))
      return false;
  }

  // Update postfx settings
  {
    static constexpr const char* section = PostProcessing::Config::DISPLAY_CHAIN_SECTION;

    auto lock = Host::GetSettingsLock();
    const SettingsInterface& si = GetPostProcessingSettingsInterface(section);

    // Don't delete the chain if we're just temporarily disabling.
    if (PostProcessing::Config::GetStageCount(si, section) == 0)
    {
      m_display_postfx.reset();
    }
    else
    {
      if (!m_display_postfx || force_reload)
      {
        if (!m_display_postfx)
          m_display_postfx = std::make_unique<PostProcessing::Chain>(section);
        m_display_postfx->LoadStages(lock, si, true);
      }
      else if (!force_reload)
      {
        m_display_postfx->UpdateSettings(lock, si);
      }
    }
  }

  return true;
}

SettingsInterface& GPUPresenter::GetPostProcessingSettingsInterface(const char* section)
{
  // If PostProcessing/Enable is set in the game settings interface, use that.
  // Otherwise, use the base settings.

  SettingsInterface* game_si = Host::Internal::GetGameSettingsLayer();
  if (game_si && game_si->ContainsValue(section, "Enabled"))
    return *game_si;
  else
    return *Host::Internal::GetBaseSettingsLayer();
}

void GPUPresenter::TogglePostProcessing()
{
  DebugAssert(!GPUThread::IsOnThread());

  GPUThread::RunOnBackend(
    [](GPUBackend* backend) {
      if (!backend)
        return;

      GPUPresenter& presenter = backend->GetPresenter();

      // if it is being lazy loaded, we have to load it here
      if (!presenter.m_display_postfx)
      {
        presenter.LoadPostProcessingSettings(true);
        if (presenter.m_display_postfx && presenter.m_display_postfx->IsActive())
          return;
      }
      if (presenter.m_display_postfx)
        presenter.m_display_postfx->Toggle();
    },
    false, true);
}

void GPUPresenter::ReloadPostProcessingSettings(bool display, bool internal, bool reload_shaders)
{
  DebugAssert(!GPUThread::IsOnThread());

  GPUThread::RunOnBackend(
    [display, internal, reload_shaders](GPUBackend* backend) {
      if (!backend)
        return;

      // OSD message first in case any errors occur.
      if (reload_shaders)
      {
        Host::AddIconOSDMessage("PostProcessing", ICON_FA_PAINT_ROLLER,
                                TRANSLATE_STR("OSDMessage", "Post-processing shaders reloaded."),
                                Host::OSD_QUICK_DURATION);
      }

      if (display)
      {
        Error error;
        if (!backend->GetPresenter().UpdatePostProcessingSettings(reload_shaders, &error))
        {
          GPUThread::ReportFatalErrorAndShutdown(fmt::format("Failed to update settings: {}", error.GetDescription()));
          return;
        }
      }
      if (internal)
        backend->UpdatePostProcessingSettings(reload_shaders);

      // trigger represent of frame
      if (GPUThread::IsSystemPaused())
        GPUThread::Internal::PresentFrameAndRestoreContext();
    },
    false, true);
}

bool GPUPresenter::LoadOverlaySettings()
{
  std::string preset_name = Host::GetStringSettingValue("BorderOverlay", "PresetName");
  std::string image_path;
  GSVector4i display_rect = m_border_overlay_display_rect;
  bool alpha_blend = m_border_overlay_alpha_blend;
  bool destination_alpha_blend = m_border_overlay_destination_alpha_blend;
  if (preset_name == "Custom")
  {
    image_path = Host::GetStringSettingValue("BorderOverlay", "ImagePath");
    display_rect = GSVector4i(Host::GetIntSettingValue("BorderOverlay", "DisplayStartX", 0),
                              Host::GetIntSettingValue("BorderOverlay", "DisplayStartY", 0),
                              Host::GetIntSettingValue("BorderOverlay", "DisplayEndX", 0),
                              Host::GetIntSettingValue("BorderOverlay", "DisplayEndY", 0));
    alpha_blend = Host::GetBoolSettingValue("BorderOverlay", "AlphaBlend", false);
    destination_alpha_blend = Host::GetBoolSettingValue("BorderOverlay", "DestinationAlphaBlend", false);
  }

  // check rect validity.. ignore everything if it's bogus
  if (!image_path.empty() && display_rect.rempty())
  {
    ERROR_LOG("Border overlay rectangle {} is invalid.", display_rect);
    image_path = {};
  }
  if (image_path.empty())
  {
    // using preset?
    if (!preset_name.empty())
    {
      // don't worry about the other settings, the loader will fix them up
      if (m_border_overlay_image_path == preset_name)
        return false;

      image_path = std::move(preset_name);
    }

    display_rect = GSVector4i::zero();
    alpha_blend = false;
  }

  // display rect can be updated without issue
  m_border_overlay_display_rect = display_rect;

  // but images and alphablend require pipeline/texture changes
  const bool image_changed = (m_border_overlay_image_path != image_path);
  const bool changed =
    (image_changed || (!image_path.empty() && (alpha_blend == m_border_overlay_alpha_blend ||
                                               destination_alpha_blend == m_border_overlay_destination_alpha_blend)));
  if (image_changed)
    m_border_overlay_image_path = std::move(image_path);

  m_border_overlay_alpha_blend = alpha_blend;
  m_border_overlay_destination_alpha_blend = destination_alpha_blend;
  return changed;
}

bool GPUPresenter::LoadOverlayTexture()
{
  g_gpu_device->RecycleTexture(std::move(m_border_overlay_texture));
  if (m_border_overlay_image_path.empty())
  {
    m_border_overlay_display_rect = GSVector4i::zero();
    m_border_overlay_image_path = {};
    m_border_overlay_alpha_blend = false;
    return true;
  }

  Image image;
  Error error;

  bool image_load_result;
  if (Path::IsAbsolute(m_border_overlay_image_path))
    image_load_result = image.LoadFromFile(m_border_overlay_image_path.c_str(), &error);
  else
    image_load_result = LoadOverlayPreset(&error, &image);
  if (!image_load_result ||
      !(m_border_overlay_texture = g_gpu_device->FetchAndUploadTextureImage(image, GPUTexture::Flags::None, &error)))
  {
    ERROR_LOG("Failed to load overlay '{}': {}", Path::GetFileName(m_border_overlay_image_path),
              error.GetDescription());
    m_border_overlay_display_rect = GSVector4i::zero();
    m_border_overlay_image_path = {};
    m_border_overlay_alpha_blend = false;
    return false;
  }

  INFO_LOG("Loaded overlay image {}: {}x{}", Path::GetFileName(m_border_overlay_image_path),
           m_border_overlay_texture->GetWidth(), m_border_overlay_texture->GetHeight());
  return true;
}

std::vector<std::string> GPUPresenter::EnumerateBorderOverlayPresets()
{
  static constexpr const char* pattern = "*.yml";

  std::vector<std::string> ret;

  FileSystem::FindResultsArray files;
  FileSystem::FindFiles(Path::Combine(EmuFolders::Resources, "overlays").c_str(), pattern,
                        FILESYSTEM_FIND_RELATIVE_PATHS | FILESYSTEM_FIND_FILES, &files);
  FileSystem::FindFiles(Path::Combine(EmuFolders::UserResources, "overlays").c_str(), pattern,
                        FILESYSTEM_FIND_RELATIVE_PATHS | FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_KEEP_ARRAY, &files);

  ret.reserve(files.size());
  for (FILESYSTEM_FIND_DATA& fd : files)
  {
    const std::string_view name = Path::GetFileTitle(fd.FileName);
    if (StringUtil::IsInStringList(ret, name))
      continue;

    ret.emplace_back(name);
  }

  std::sort(ret.begin(), ret.end());
  return ret;
}

bool GPUPresenter::LoadOverlayPreset(Error* error, Image* image)
{
  SmallString path = SmallString::from_format("overlays/{}.yml", m_border_overlay_image_path);
  std::optional<std::string> yaml_data = Host::ReadResourceFileToString(path, true, error);
  if (!yaml_data.has_value())
    return false;

  const ryml::Tree yaml =
    ryml::parse_in_place(to_csubstr(path), c4::substr(reinterpret_cast<char*>(yaml_data->data()), yaml_data->size()));
  const ryml::ConstNodeRef root = yaml.rootref();
  if (root.empty())
  {
    Error::SetStringView(error, "Configuration is empty.");
    return false;
  }

  std::string_view image_filename;
  GSVector4i display_area = GSVector4i::zero();
  bool alpha_blend = false;
  bool destination_alpha_blend = false;
  if (!GetStringFromObject(root, "image", &image_filename) ||
      !GetUIntFromObject(root, "displayStartX", &display_area.x) ||
      !GetUIntFromObject(root, "displayStartY", &display_area.y) ||
      !GetUIntFromObject(root, "displayEndX", &display_area.z) ||
      !GetUIntFromObject(root, "displayEndY", &display_area.w) ||
      !GetUIntFromObject(root, "alphaBlend", &alpha_blend) ||
      !GetUIntFromObject(root, "destinationAlphaBlend", &destination_alpha_blend))
  {
    Error::SetStringView(error, "One or more parameters is missing.");
    return false;
  }

  path.format("overlays/{}", image_filename);
  std::optional<DynamicHeapArray<u8>> image_data = Host::ReadResourceFile(path, true, error);
  if (!image_data.has_value() || !image->LoadFromBuffer(image_filename, image_data.value(), error))
    return false;

  m_border_overlay_display_rect = display_area;
  m_border_overlay_alpha_blend = alpha_blend;
  m_border_overlay_destination_alpha_blend = destination_alpha_blend;
  return true;
}
