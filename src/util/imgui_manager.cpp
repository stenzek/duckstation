// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "imgui_manager.h"
#include "gpu_device.h"
#include "host.h"
#include "image.h"
#include "input_manager.h"
#include "shadergen.h"

// TODO: Remove me when GPUDevice config is also cleaned up.
#include "core/fullscreenui.h"
#include "core/fullscreenui_widgets.h"
#include "core/gpu_thread.h"
#include "core/host.h"
#include "core/settings.h"

#include "common/assert.h"
#include "common/bitutils.h"
#include "common/easing.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/string_util.h"
#include "common/thirdparty/usb_key_code_data.h"
#include "common/timer.h"

#include "IconsFontAwesome6.h"
#include "fmt/format.h"
#include "imgui.h"
#include "imgui_freetype.h"
#include "imgui_internal.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <limits>
#include <mutex>
#include <type_traits>

LOG_CHANNEL(ImGuiManager);

namespace ImGuiManager {
namespace {

struct SoftwareCursor
{
  std::string image_path;
  std::unique_ptr<GPUTexture> texture;
  u32 color;
  float scale;
  float extent_x;
  float extent_y;
  std::pair<float, float> pos;
};

struct OSDMessage
{
  std::string key;
  std::string text;
  Timer::Value start_time;
  Timer::Value move_time;
  float duration;
  float target_y;
  float last_y;
  bool is_warning;
};

} // namespace

static_assert(std::is_same_v<WCharType, ImWchar>);

static void UpdateScale();
static void SetStyle(ImGuiStyle& style, float scale);
static bool LoadFontData(Error* error);
static void ReloadFontDataIfActive();
static bool CreateFontAtlas(Error* error);
static bool CompilePipelines(Error* error);
static void RenderDrawLists(u32 window_width, u32 window_height, WindowInfo::PreRotation prerotation);
static void UpdateTextures();
static void SetCommonIOOptions(ImGuiIO& io, ImGuiPlatformIO& pio);
static void SetImKeyState(ImGuiIO& io, ImGuiKey imkey, bool pressed);
static const char* GetClipboardTextImpl(ImGuiContext* ctx);
static void SetClipboardTextImpl(ImGuiContext* ctx, const char* text);
static void AddOSDMessage(std::string key, std::string message, float duration, bool is_warning);
static void RemoveKeyedOSDMessage(std::string key, bool is_warning);
static void ClearOSDMessages(bool clear_warnings);
static void AcquirePendingOSDMessages(Timer::Value current_time);
static void DrawOSDMessages(Timer::Value current_time);
static void CreateSoftwareCursorTextures();
static void UpdateSoftwareCursorTexture(SoftwareCursor& cursor, const std::string& image_path);
static void DestroySoftwareCursorTextures();
static void DrawSoftwareCursor(const SoftwareCursor& sc, const std::pair<float, float>& pos);
static std::optional<ImGuiKey> MapHostKeyEventToImGuiKey(u32 key);

static constexpr float OSD_FADE_IN_TIME = 0.1f;
static constexpr float OSD_FADE_OUT_TIME = 0.4f;

static constexpr std::array<const char*, static_cast<size_t>(TextFont::MaxCount)> TEXT_FONT_NAMES = {{
  "Roboto-VariableFont_wdth,wght.ttf", // Default
  "NotoSansSC-VariableFont_wght.ttf",  // Chinese
  "NotoSansJP-VariableFont_wght.ttf",  // Japanese
  "NotoSansKR-VariableFont_wght.ttf",  // Korean
}};
static constexpr const char* FIXED_FONT_NAME = "RobotoMono-VariableFont_wght.ttf";
static constexpr const char* FA_FONT_NAME = "fa-solid-900.ttf";
static constexpr const char* PF_FONT_NAME = "promptfont.otf";
static constexpr const char* EMOJI_FONT_NAME = "TwitterColorEmoji-SVGinOT.ttf";

namespace {

struct ALIGN_TO_CACHE_LINE State
{
  ImGuiContext* imgui_context = nullptr;

  // cached copies of WantCaptureKeyboard/Mouse, used to know when to dispatch events
  std::atomic_bool imgui_wants_keyboard{false};
  std::atomic_bool imgui_wants_mouse{false};
  std::atomic_bool imgui_wants_text{false};

  std::deque<OSDMessage> osd_posted_messages;
  std::mutex osd_messages_lock;

  // Owned by GPU thread
  ALIGN_TO_CACHE_LINE Timer::Value last_render_time = 0;

  float global_prescale = 0.0f; // before window scale
  float global_scale = 0.0f;
  float screen_margin = 0.0f;

  float window_width = 0.0f;
  float window_height = 0.0f;
  GPUTexture::Format window_format = GPUTexture::Format::Unknown;
  bool scale_changed = false;

  // we maintain a second copy of the stick state here so we can map it to the dpad
  std::array<s8, 2> left_stick_axis_state = {};

  std::unique_ptr<GPUPipeline> imgui_pipeline;

  ImFont* text_font = nullptr;
  ImFont* fixed_font = nullptr;

  std::deque<OSDMessage> osd_active_messages;

  std::array<ImGuiManager::SoftwareCursor, InputManager::MAX_SOFTWARE_CURSORS> software_cursors = {};

  TextFontOrder text_font_order = ImGuiManager::GetDefaultTextFontOrder();

  std::array<DynamicHeapArray<u8>, static_cast<size_t>(TextFont::MaxCount)> text_fonts_data;
  DynamicHeapArray<u8> fixed_font_data;
  DynamicHeapArray<u8> icon_fa_font_data;
  DynamicHeapArray<u8> icon_pf_font_data;
  DynamicHeapArray<u8> emoji_font_data;
};

} // namespace

static State s_state;

} // namespace ImGuiManager

ImGuiManager::TextFontOrder ImGuiManager::GetDefaultTextFontOrder()
{
  return {TextFont::Default, TextFont::Japanese, TextFont::Chinese, TextFont::Korean};
}

void ImGuiManager::SetTextFontOrder(const TextFontOrder& order)
{
  if (s_state.text_font_order == order)
    return;

  s_state.text_font_order = order;
  ReloadFontDataIfActive();
}

void ImGuiManager::SetGlobalScale(float global_scale)
{
  if (s_state.global_prescale == global_scale)
    return;

  s_state.global_prescale = global_scale;
  s_state.scale_changed = true;
}

bool ImGuiManager::Initialize(float global_scale, float screen_margin, Error* error)
{
  if (!LoadFontData(error))
  {
    Error::AddPrefix(error, "Failed to load font data: ");
    return false;
  }

  GPUSwapChain* const main_swap_chain = g_gpu_device->GetMainSwapChain();

  s_state.global_prescale = global_scale;
  s_state.global_scale = std::max((main_swap_chain ? main_swap_chain->GetScale() : 1.0f) * global_scale, 1.0f);
  s_state.screen_margin = std::max(screen_margin, 0.0f);
  s_state.scale_changed = false;

  s_state.imgui_context = ImGui::CreateContext();

  ImGuiIO& io = s_state.imgui_context->IO;
  io.IniFilename = nullptr;
  io.BackendFlags |=
    ImGuiBackendFlags_HasGamepad | ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
#ifndef __ANDROID__
  // Android has no keyboard, nor are we using ImGui for any actual user-interactable windows.
  io.ConfigFlags |=
    ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NoMouseCursorChange;
#else
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
#endif
  SetCommonIOOptions(io, s_state.imgui_context->PlatformIO);

  s_state.last_render_time = Timer::GetCurrentValue();
  s_state.window_format = main_swap_chain ? main_swap_chain->GetFormat() : GPUTexture::Format::RGBA8;
  s_state.window_width = main_swap_chain ? static_cast<float>(main_swap_chain->GetWidth()) : 0.0f;
  s_state.window_height = main_swap_chain ? static_cast<float>(main_swap_chain->GetHeight()) : 0.0f;
  io.DisplayFramebufferScale = ImVec2(1, 1); // We already scale things ourselves, this would double-apply scaling
  io.DisplaySize = ImVec2(s_state.window_width, s_state.window_height);

  SetStyle(s_state.imgui_context->Style, s_state.global_scale);
  FullscreenUI::UpdateTheme();
  FullscreenUI::UpdateLayoutScale();

  if (!CreateFontAtlas(error) || !CompilePipelines(error) || !FullscreenUI::InitializeWidgets(error))
    return false;

  NewFrame();

  CreateSoftwareCursorTextures();
  return true;
}

void ImGuiManager::Shutdown(bool clear_fsui_state)
{
  DestroySoftwareCursorTextures();

  FullscreenUI::ShutdownWidgets(clear_fsui_state);

  s_state.text_font = nullptr;
  s_state.fixed_font = nullptr;
  FullscreenUI::SetFont(nullptr);

  s_state.imgui_pipeline.reset();

  if (s_state.imgui_context)
  {
    for (ImTextureData* tex : s_state.imgui_context->IO.Fonts->TexList)
    {
      if (tex->Status == ImTextureStatus_Destroyed)
        return;

      std::unique_ptr<GPUTexture> gtex(reinterpret_cast<GPUTexture*>(tex->GetTexID()));
      tex->Status = ImTextureStatus_Destroyed;
    }

    ImGui::DestroyContext(s_state.imgui_context);
    s_state.imgui_context = nullptr;
  }
}

ImGuiContext* ImGuiManager::GetMainContext()
{
  return s_state.imgui_context;
}

bool ImGuiManager::IsInitialized()
{
  return (s_state.imgui_context != nullptr);
}

void ImGuiManager::SetScreenMargin(float margin)
{
  s_state.screen_margin = std::max(margin, 0.0f);
}

float ImGuiManager::GetWindowWidth()
{
  return s_state.window_width;
}

float ImGuiManager::GetWindowHeight()
{
  return s_state.window_height;
}

void ImGuiManager::WindowResized(GPUTexture::Format format, float width, float height)
{
  if (s_state.window_format != format) [[unlikely]]
  {
    Error error;
    s_state.window_format = format;
    if (!CompilePipelines(&error))
    {
      error.AddPrefix("Failed to compile pipelines after window format change:\n");
      GPUThread::ReportFatalErrorAndShutdown(error.GetDescription());
      return;
    }
  }

  s_state.window_width = width;
  s_state.window_height = height;
  ImGui::GetMainViewport()->Size = ImGui::GetIO().DisplaySize = ImVec2(width, height);

  // Scale might have changed as a result of window resize.
  RequestScaleUpdate();
}

void ImGuiManager::RequestScaleUpdate()
{
  // Might need to update the scale.
  s_state.scale_changed = true;
}

void ImGuiManager::UpdateScale()
{
  const float window_scale =
    (g_gpu_device && g_gpu_device->HasMainSwapChain()) ? g_gpu_device->GetMainSwapChain()->GetScale() : 1.0f;
  const float scale = std::max(window_scale * s_state.global_prescale, 1.0f);
  const bool scale_changed = (scale != s_state.global_scale);

  if (!FullscreenUI::UpdateLayoutScale() && !scale_changed)
    return;

  if (scale_changed)
  {
    s_state.global_scale = scale;
    SetStyle(s_state.imgui_context->Style, s_state.global_scale);
  }

  // force font GC
  ImGui::GetIO().Fonts->CompactCache();
}

void ImGuiManager::NewFrame()
{
  const Timer::Value current_time = Timer::GetCurrentValue();

  ImGuiIO& io = ImGui::GetIO();
  io.DeltaTime = static_cast<float>(Timer::ConvertValueToSeconds(current_time - s_state.last_render_time));
  s_state.last_render_time = current_time;

  if (s_state.scale_changed)
  {
    s_state.scale_changed = false;
    UpdateScale();
  }

  ImGui::NewFrame();

  // Disable nav input on the implicit (Debug##Default) window. Otherwise we end up requesting keyboard
  // focus when there's nothing there. We use GetCurrentWindowRead() because otherwise it'll make it visible.
  ImGui::GetCurrentWindowRead()->Flags |= ImGuiWindowFlags_NoNavInputs;
  s_state.imgui_wants_keyboard.store(io.WantCaptureKeyboard, std::memory_order_relaxed);
  s_state.imgui_wants_mouse.store(io.WantCaptureMouse, std::memory_order_release);

  const bool wants_text_input = io.WantTextInput;
  if (s_state.imgui_wants_text.load(std::memory_order_relaxed) != wants_text_input)
  {
    s_state.imgui_wants_text.store(wants_text_input, std::memory_order_release);
    if (wants_text_input)
      Host::BeginTextInput();
    else
      Host::EndTextInput();
  }
}

bool ImGuiManager::CompilePipelines(Error* error)
{
  const RenderAPI render_api = g_gpu_device->GetRenderAPI();
  const GPUDevice::Features features = g_gpu_device->GetFeatures();
  const ShaderGen shadergen(render_api, ShaderGen::GetShaderLanguageForAPI(render_api), features.dual_source_blend,
                            features.framebuffer_fetch);

  std::unique_ptr<GPUShader> imgui_vs = g_gpu_device->CreateShader(GPUShaderStage::Vertex, shadergen.GetLanguage(),
                                                                   shadergen.GenerateImGuiVertexShader(), error);
  std::unique_ptr<GPUShader> imgui_fs = g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(),
                                                                   shadergen.GenerateImGuiFragmentShader(), error);
  if (!imgui_vs || !imgui_fs)
  {
    Error::AddPrefix(error, "Failed to compile ImGui shaders: ");
    return false;
  }
  GL_OBJECT_NAME(imgui_vs, "ImGui Vertex Shader");
  GL_OBJECT_NAME(imgui_fs, "ImGui Fragment Shader");

  static constexpr GPUPipeline::VertexAttribute imgui_attributes[] = {
    GPUPipeline::VertexAttribute::Make(0, GPUPipeline::VertexAttribute::Semantic::Position, 0,
                                       GPUPipeline::VertexAttribute::Type::Float, 2, OFFSETOF(ImDrawVert, pos)),
    GPUPipeline::VertexAttribute::Make(1, GPUPipeline::VertexAttribute::Semantic::TexCoord, 0,
                                       GPUPipeline::VertexAttribute::Type::Float, 2, OFFSETOF(ImDrawVert, uv)),
    GPUPipeline::VertexAttribute::Make(2, GPUPipeline::VertexAttribute::Semantic::Color, 0,
                                       GPUPipeline::VertexAttribute::Type::UNorm8, 4, OFFSETOF(ImDrawVert, col)),
  };

  GPUPipeline::GraphicsConfig plconfig;
  plconfig.layout = GPUPipeline::Layout::SingleTextureAndUBO;
  plconfig.input_layout.vertex_attributes = imgui_attributes;
  plconfig.input_layout.vertex_stride = sizeof(ImDrawVert);
  plconfig.primitive = GPUPipeline::Primitive::Triangles;
  plconfig.rasterization = GPUPipeline::RasterizationState::GetNoCullState();
  plconfig.depth = GPUPipeline::DepthState::GetNoTestsState();
  plconfig.blend = GPUPipeline::BlendState::GetAlphaBlendingState();
  plconfig.blend.write_mask = 0x7;
  plconfig.SetTargetFormats(s_state.window_format);
  plconfig.samples = 1;
  plconfig.per_sample_shading = false;
  plconfig.render_pass_flags = GPUPipeline::NoRenderPassFlags;
  plconfig.vertex_shader = imgui_vs.get();
  plconfig.geometry_shader = nullptr;
  plconfig.fragment_shader = imgui_fs.get();

  s_state.imgui_pipeline = g_gpu_device->CreatePipeline(plconfig, error);
  if (!s_state.imgui_pipeline)
  {
    Error::AddPrefix(error, "Failed to compile ImGui pipeline: ");
    return false;
  }

  GL_OBJECT_NAME(s_state.imgui_pipeline, "ImGui Pipeline");
  return true;
}

void ImGuiManager::CreateDrawLists()
{
  ImGui::EndFrame();
  ImGui::Render();
  UpdateTextures();
}

void ImGuiManager::RenderDrawLists(u32 window_width, u32 window_height, WindowInfo::PreRotation prerotation)
{
  const ImDrawData* draw_data = ImGui::GetDrawData();
  if (draw_data->CmdListsCount == 0)
    return;

  const GSVector2i window_size = GSVector2i(static_cast<s32>(window_width), static_cast<s32>(window_height));
  const u32 post_rotated_width =
    WindowInfo::ShouldSwapDimensionsForPreRotation(prerotation) ? window_height : window_width;
  const u32 post_rotated_height =
    WindowInfo::ShouldSwapDimensionsForPreRotation(prerotation) ? window_width : window_height;

  g_gpu_device->SetViewport(0, 0, static_cast<s32>(post_rotated_width), static_cast<s32>(post_rotated_height));
  g_gpu_device->SetPipeline(s_state.imgui_pipeline.get());

  const bool prerotated = (prerotation != WindowInfo::PreRotation::Identity);
  GSMatrix4x4 mproj = GSMatrix4x4::OffCenterOrthographicProjection(0.0f, 0.0f, static_cast<float>(window_width),
                                                                   static_cast<float>(window_height), 0.0f, 1.0f);
  if (prerotated)
    mproj = GSMatrix4x4::RotationZ(WindowInfo::GetZRotationForPreRotation(prerotation)) * mproj;
  g_gpu_device->UploadUniformBuffer(&mproj, sizeof(mproj));

  // Render command lists
  const bool flip = g_gpu_device->UsesLowerLeftOrigin();
  for (int n = 0; n < draw_data->CmdListsCount; n++)
  {
    const ImDrawList* cmd_list = draw_data->CmdLists[n];
    static_assert(sizeof(ImDrawIdx) == sizeof(GPUDevice::DrawIndex));

    u32 base_vertex, base_index;
    g_gpu_device->UploadVertexBuffer(cmd_list->VtxBuffer.Data, sizeof(ImDrawVert), cmd_list->VtxBuffer.Size,
                                     &base_vertex);
    g_gpu_device->UploadIndexBuffer(cmd_list->IdxBuffer.Data, cmd_list->IdxBuffer.Size, &base_index);

    for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++)
    {
      const ImDrawCmd* pcmd = &cmd_list->CmdBuffer[cmd_i];

      if ((pcmd->ElemCount == 0 && !pcmd->UserCallback) || pcmd->ClipRect.z <= pcmd->ClipRect.x ||
          pcmd->ClipRect.w <= pcmd->ClipRect.y)
      {
        continue;
      }

      GSVector4i clip = GSVector4i(GSVector4::load<false>(&pcmd->ClipRect.x));

      if (prerotated)
        clip = GPUSwapChain::PreRotateClipRect(prerotation, window_size, clip);
      if (flip)
        clip = g_gpu_device->FlipToLowerLeft(clip, post_rotated_height);

      g_gpu_device->SetScissor(clip);
      g_gpu_device->SetTextureSampler(0, reinterpret_cast<GPUTexture*>(pcmd->GetTexID()),
                                      g_gpu_device->GetLinearSampler());

      if (pcmd->UserCallback) [[unlikely]]
      {
        pcmd->UserCallback(cmd_list, pcmd);
        g_gpu_device->UploadUniformBuffer(&mproj, sizeof(mproj));
        g_gpu_device->SetPipeline(s_state.imgui_pipeline.get());
      }
      else
      {
        g_gpu_device->DrawIndexed(pcmd->ElemCount, base_index + pcmd->IdxOffset, base_vertex + pcmd->VtxOffset);
      }
    }
  }
}

void ImGuiManager::RenderDrawLists(GPUSwapChain* swap_chain)
{
  RenderDrawLists(swap_chain->GetWidth(), swap_chain->GetHeight(), swap_chain->GetPreRotation());
}

void ImGuiManager::RenderDrawLists(GPUTexture* texture)
{
  RenderDrawLists(texture->GetWidth(), texture->GetHeight(), WindowInfo::PreRotation::Identity);
}

void ImGuiManager::UpdateTextures()
{
  for (ImTextureData* const tex : s_state.imgui_context->IO.Fonts->TexList)
  {
    switch (tex->Status)
    {
      case ImTextureStatus_WantCreate:
      {
        DebugAssert(tex->Format == ImTextureFormat_RGBA32);
        DEV_LOG("Create {}x{} ImGui texture", tex->Width, tex->Height);

        Error error;
        std::unique_ptr<GPUTexture> gtex = g_gpu_device->FetchTexture(
          tex->Width, tex->Height, 1, 1, 1, GPUTexture::Type::Texture, GPUTexture::Format::RGBA8,
          GPUTexture::Flags::None, tex->GetPixels(), tex->GetPitch(), &error);
        if (!gtex) [[unlikely]]
        {
          ERROR_LOG("Failed to create {}x{} imgui texture: {}", tex->Width, tex->Height, error.GetDescription());
          continue;
        }

        tex->SetTexID(reinterpret_cast<ImTextureID>(gtex.release()));
        tex->Status = ImTextureStatus_OK;
      }
      break;

      case ImTextureStatus_WantUpdates:
      {
        // TODO: Do we want to just update the whole dirty area? Probably need a heuristic...
        GPUTexture* const gtex = reinterpret_cast<GPUTexture*>(tex->GetTexID());
        for (const ImTextureRect& rc : tex->Updates)
        {
          DEBUG_LOG("Update {}x{} @ {},{} in {}x{} ImGui texture", rc.w, rc.h, rc.x, rc.y, tex->Width, tex->Height);
          if (!gtex->Update(rc.x, rc.y, rc.w, rc.h, tex->GetPixelsAt(rc.x, rc.y), tex->GetPitch())) [[unlikely]]
          {
            ERROR_LOG("Failed to update {}x{} rect @ {},{} in imgui texture", rc.w, rc.h, rc.x, rc.y);
            continue;
          }
        }

        // Updates is cleared by ImGui NewFrame.
        tex->Status = ImTextureStatus_OK;
      }
      break;

      case ImTextureStatus_WantDestroy:
      {
        std::unique_ptr<GPUTexture> gtex(reinterpret_cast<GPUTexture*>(tex->GetTexID()));
        if (gtex)
        {
          DEV_LOG("Destroy {}x{} ImGui texture", gtex->GetWidth(), gtex->GetHeight());
          g_gpu_device->RecycleTexture(std::move(gtex));
        }

        tex->SetTexID(nullptr);
        tex->Status = ImTextureStatus_Destroyed;
      }
      break;

      case ImTextureStatus_Destroyed:
      case ImTextureStatus_OK:
      default:
        continue;
    }
  }
}

void ImGuiManager::SetStyle(ImGuiStyle& style, float scale)
{
  style = ImGuiStyle();
  style.WindowMinSize = ImVec2(1.0f, 1.0f);
  style.FrameRounding = 8.0f;
  style.FramePadding = ImVec2(8.0f, 6.0f);

  ImVec4* colors = style.Colors;
  colors[ImGuiCol_Text] = ImVec4(0.95f, 0.96f, 0.98f, 1.00f);
  colors[ImGuiCol_TextDisabled] = ImVec4(0.36f, 0.42f, 0.47f, 1.00f);
  colors[ImGuiCol_WindowBg] = ImVec4(0.11f, 0.15f, 0.17f, 1.00f);
  colors[ImGuiCol_ChildBg] = ImVec4(0.15f, 0.18f, 0.22f, 1.00f);
  colors[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.08f, 0.08f, 0.94f);
  colors[ImGuiCol_Border] = ImVec4(0.08f, 0.10f, 0.12f, 1.00f);
  colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
  colors[ImGuiCol_FrameBg] = ImVec4(0.20f, 0.25f, 0.29f, 1.00f);
  colors[ImGuiCol_FrameBgHovered] = ImVec4(0.12f, 0.20f, 0.28f, 1.00f);
  colors[ImGuiCol_FrameBgActive] = ImVec4(0.09f, 0.12f, 0.14f, 1.00f);
  colors[ImGuiCol_TitleBg] = ImVec4(0.09f, 0.12f, 0.14f, 0.65f);
  colors[ImGuiCol_TitleBgActive] = ImVec4(0.08f, 0.10f, 0.12f, 1.00f);
  colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.00f, 0.00f, 0.00f, 0.51f);
  colors[ImGuiCol_MenuBarBg] = ImVec4(0.15f, 0.18f, 0.22f, 1.00f);
  colors[ImGuiCol_ScrollbarBg] = ImVec4(0.02f, 0.02f, 0.02f, 0.39f);
  colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.20f, 0.25f, 0.29f, 1.00f);
  colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.18f, 0.22f, 0.25f, 1.00f);
  colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.09f, 0.21f, 0.31f, 1.00f);
  colors[ImGuiCol_CheckMark] = ImVec4(0.28f, 0.56f, 1.00f, 1.00f);
  colors[ImGuiCol_SliderGrab] = ImVec4(0.28f, 0.56f, 1.00f, 1.00f);
  colors[ImGuiCol_SliderGrabActive] = ImVec4(0.37f, 0.61f, 1.00f, 1.00f);
  colors[ImGuiCol_Button] = ImVec4(0.20f, 0.25f, 0.29f, 1.00f);
  colors[ImGuiCol_ButtonHovered] = ImVec4(0.33f, 0.38f, 0.46f, 1.00f);
  colors[ImGuiCol_ButtonActive] = ImVec4(0.27f, 0.32f, 0.38f, 1.00f);
  colors[ImGuiCol_Header] = ImVec4(0.20f, 0.25f, 0.29f, 0.55f);
  colors[ImGuiCol_HeaderHovered] = ImVec4(0.33f, 0.38f, 0.46f, 1.00f);
  colors[ImGuiCol_HeaderActive] = ImVec4(0.27f, 0.32f, 0.38f, 1.00f);
  colors[ImGuiCol_Separator] = ImVec4(0.20f, 0.25f, 0.29f, 1.00f);
  colors[ImGuiCol_SeparatorHovered] = ImVec4(0.33f, 0.38f, 0.46f, 1.00f);
  colors[ImGuiCol_SeparatorActive] = ImVec4(0.27f, 0.32f, 0.38f, 1.00f);
  colors[ImGuiCol_ResizeGrip] = ImVec4(0.26f, 0.59f, 0.98f, 0.25f);
  colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.33f, 0.38f, 0.46f, 1.00f);
  colors[ImGuiCol_ResizeGripActive] = ImVec4(0.27f, 0.32f, 0.38f, 1.00f);
  colors[ImGuiCol_Tab] = ImVec4(0.11f, 0.15f, 0.17f, 1.00f);
  colors[ImGuiCol_TabHovered] = ImVec4(0.33f, 0.38f, 0.46f, 1.00f);
  colors[ImGuiCol_TabSelected] = ImVec4(0.27f, 0.32f, 0.38f, 1.00f);
  colors[ImGuiCol_TabDimmed] = ImVec4(0.11f, 0.15f, 0.17f, 1.00f);
  colors[ImGuiCol_TabDimmedSelected] = ImVec4(0.11f, 0.15f, 0.17f, 1.00f);
  colors[ImGuiCol_PlotLines] = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
  colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
  colors[ImGuiCol_PlotHistogram] = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
  colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
  colors[ImGuiCol_TextSelectedBg] = ImVec4(0.26f, 0.59f, 0.98f, 0.35f);
  colors[ImGuiCol_DragDropTarget] = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
  colors[ImGuiCol_NavCursor] = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
  colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
  colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
  colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.35f);

  style.ScaleAllSizes(scale);
}

bool ImGuiManager::LoadFontData(Error* error)
{
  Timer load_timer;

  static constexpr auto font_resource_name = [](const std::string_view font_name) {
    return TinyString::from_format("fonts/{}", font_name);
  };

  // only load used text fonts, that way we don't waste memory on mini
  for (const TextFont text_font : s_state.text_font_order)
  {
    const u32 index = static_cast<u32>(text_font);
    if (!s_state.text_fonts_data[index].empty())
      continue;

    std::optional<DynamicHeapArray<u8>> font_data =
      Host::ReadResourceFile(font_resource_name(TEXT_FONT_NAMES[index]), true, error);
    if (!font_data.has_value())
      return false;

    s_state.text_fonts_data[index] = std::move(font_data.value());
  }

  if (s_state.fixed_font_data.empty())
  {
    std::optional<DynamicHeapArray<u8>> font_data =
      Host::ReadResourceFile(font_resource_name(FIXED_FONT_NAME), true, error);
    if (!font_data.has_value())
      return false;

    s_state.fixed_font_data = std::move(font_data.value());
  }

  if (s_state.icon_fa_font_data.empty())
  {
    std::optional<DynamicHeapArray<u8>> font_data =
      Host::ReadResourceFile(font_resource_name(FA_FONT_NAME), true, error);
    if (!font_data.has_value())
      return false;

    s_state.icon_fa_font_data = std::move(font_data.value());
  }

  if (s_state.icon_pf_font_data.empty())
  {
    std::optional<DynamicHeapArray<u8>> font_data =
      Host::ReadResourceFile(font_resource_name(PF_FONT_NAME), true, error);
    if (!font_data.has_value())
      return false;

    s_state.icon_pf_font_data = std::move(font_data.value());
  }

  if (s_state.emoji_font_data.empty())
  {
    std::optional<DynamicHeapArray<u8>> font_data =
      Host::ReadResourceFile(font_resource_name(EMOJI_FONT_NAME), true, error);
    if (!font_data.has_value())
      return false;

    s_state.emoji_font_data = std::move(font_data.value());
  }

  DEV_LOG("Loading font data took {} ms", load_timer.GetTimeMilliseconds());
  return true;
}

bool ImGuiManager::CreateFontAtlas(Error* error)
{
  Timer load_timer;

  ImGuiIO& io = ImGui::GetIO();
  io.Fonts->Clear();

  const float default_text_size = GetOSDFontSize();
  const float default_text_weight = 400.0f;
  const float default_fixed_weight = 500.0f;

  ImFontConfig text_cfg;
  text_cfg.FontDataOwnedByAtlas = false;

  // First text font has to be added before the icon fonts.
  // Remaining fonts are added after the icon font, otherwise the wrong glyphs will be used in the UI.
  const TextFont first_font = s_state.text_font_order.front();
  StringUtil::Strlcpy(text_cfg.Name, TEXT_FONT_NAMES[static_cast<size_t>(first_font)], std::size(text_cfg.Name));
  auto& first_font_data = s_state.text_fonts_data[static_cast<size_t>(first_font)];
  Assert(!first_font_data.empty());
  s_state.text_font =
    ImGui::GetIO().Fonts->AddFontFromMemoryTTF(first_font_data.data(), static_cast<int>(first_font_data.size()),
                                               default_text_size, default_text_weight, &text_cfg);
  if (!s_state.text_font)
  {
    Error::SetStringFmt(error, "Failed to add primary text font {}", static_cast<u32>(s_state.text_font_order.front()));
    return false;
  }

  // Add icon fonts.
  ImFontConfig icon_cfg;
  StringUtil::Strlcpy(icon_cfg.Name, "PromptFont", std::size(icon_cfg.Name));
  icon_cfg.MergeMode = true;
  icon_cfg.FontDataOwnedByAtlas = false;
  icon_cfg.PixelSnapH = true;
  icon_cfg.GlyphMinAdvanceX = default_text_size;
  icon_cfg.GlyphMaxAdvanceX = default_text_size;

  if (!ImGui::GetIO().Fonts->AddFontFromMemoryTTF(s_state.icon_pf_font_data.data(),
                                                  static_cast<int>(s_state.icon_pf_font_data.size()),
                                                  default_text_size * 1.2f, 0.0f, &icon_cfg)) [[unlikely]]
  {
    Error::SetStringView(error, "Failed to add PF icon font");
    return false;
  }

  // Only for emoji font.
  icon_cfg.FontLoaderFlags = ImGuiFreeTypeLoaderFlags_LoadColor | ImGuiFreeTypeLoaderFlags_Bitmap;
  StringUtil::Strlcpy(icon_cfg.Name, "EmojiFont", std::size(icon_cfg.Name));

  if (!ImGui::GetIO().Fonts->AddFontFromMemoryTTF(s_state.emoji_font_data.data(),
                                                  static_cast<int>(s_state.emoji_font_data.size()),
                                                  default_text_size * 0.9f, 0.0f, &icon_cfg)) [[unlikely]]
  {
    Error::SetStringView(error, "Failed to add emoji icon font");
    return false;
  }

  StringUtil::Strlcpy(icon_cfg.Name, "FontAwesomeFont", std::size(icon_cfg.Name));
  if (!ImGui::GetIO().Fonts->AddFontFromMemoryTTF(s_state.icon_fa_font_data.data(),
                                                  static_cast<int>(s_state.icon_fa_font_data.size()),
                                                  default_text_size * 0.75f, 0.0f, &icon_cfg)) [[unlikely]]
  {
    Error::SetStringView(error, "Failed to add FA icon font");
    return false;
  }

  // Now we can add the remaining text fonts.
  text_cfg.MergeMode = true;
  for (size_t i = 1; i < s_state.text_font_order.size(); i++)
  {
    const TextFont text_font_idx = s_state.text_font_order[i];
    if (text_font_idx == first_font)
      continue;

    auto& font_data = s_state.text_fonts_data[static_cast<size_t>(text_font_idx)];
    Assert(!font_data.empty());
    StringUtil::Strlcpy(text_cfg.Name, "TextFont-", std::size(text_cfg.Name));
    if (!ImGui::GetIO().Fonts->AddFontFromMemoryTTF(font_data.data(), static_cast<int>(font_data.size()),
                                                    default_text_size, default_text_weight, &text_cfg))
    {
      Error::SetStringFmt(error, "Failed to add text font {}", static_cast<u32>(text_font_idx));
      return false;
    }
  }

  // Add the fixed-width font separately last.
  ImFontConfig fixed_cfg;
  StringUtil::Strlcpy(fixed_cfg.Name, "FixedFont", std::size(fixed_cfg.Name));
  fixed_cfg.FontDataOwnedByAtlas = false;
  s_state.fixed_font = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(s_state.fixed_font_data.data(),
                                                                  static_cast<int>(s_state.fixed_font_data.size()),
                                                                  GetFixedFontSize(), default_fixed_weight, &fixed_cfg);
  if (!s_state.fixed_font)
  {
    Error::SetStringView(error, "Failed to add fixed-width font");
    return false;
  }

  FullscreenUI::SetFont(s_state.text_font);

  DEV_LOG("Creating font atlas took {} ms", load_timer.GetTimeMilliseconds());
  return true;
}

void ImGuiManager::ReloadFontDataIfActive()
{
  if (!s_state.imgui_context)
    return;

  ImGui::EndFrame();

  Error error;
  if (!CreateFontAtlas(&error)) [[unlikely]]
  {
    GPUThread::ReportFatalErrorAndShutdown(fmt::format("Failed to recreate font atlas:\n{}", error.GetDescription()));
    return;
  }

  NewFrame();
}

void ImGuiManager::AddOSDMessage(std::string key, std::string message, float duration, bool is_warning)
{
  if (!key.empty())
    INFO_LOG("OSD [{}]: {}", key, message);
  else
    INFO_LOG("OSD: {}", message);

  const Timer::Value current_time = Timer::GetCurrentValue();

  OSDMessage msg;
  msg.key = std::move(key);
  msg.text = std::move(message);
  msg.duration = duration;
  msg.start_time = current_time;
  msg.move_time = current_time;
  msg.target_y = -1.0f;
  msg.last_y = -1.0f;
  msg.is_warning = is_warning;

  std::unique_lock lock(s_state.osd_messages_lock);
  s_state.osd_posted_messages.push_back(std::move(msg));
}

void ImGuiManager::RemoveKeyedOSDMessage(std::string key, bool is_warning)
{
  ImGuiManager::OSDMessage msg = {};
  msg.key = std::move(key);
  msg.duration = 0.0f;
  msg.is_warning = is_warning;

  std::unique_lock lock(s_state.osd_messages_lock);
  s_state.osd_posted_messages.push_back(std::move(msg));
}

void ImGuiManager::ClearOSDMessages(bool clear_warnings)
{
  {
    std::unique_lock lock(s_state.osd_messages_lock);
    if (clear_warnings)
    {
      s_state.osd_posted_messages.clear();
    }
    else
    {
      for (auto iter = s_state.osd_posted_messages.begin(); iter != s_state.osd_posted_messages.end();)
      {
        if (!iter->is_warning)
          iter = s_state.osd_posted_messages.erase(iter);
        else
          ++iter;
      }
    }
  }

  if (clear_warnings)
  {
    s_state.osd_active_messages.clear();
  }
  else
  {
    for (auto iter = s_state.osd_active_messages.begin(); iter != s_state.osd_active_messages.end();)
    {
      if (!iter->is_warning)
        s_state.osd_active_messages.erase(iter);
      else
        ++iter;
    }
  }
}

void ImGuiManager::AcquirePendingOSDMessages(Timer::Value current_time)
{
  std::atomic_thread_fence(std::memory_order_acquire);
  if (s_state.osd_posted_messages.empty())
    return;

  std::unique_lock lock(s_state.osd_messages_lock);
  for (;;)
  {
    if (s_state.osd_posted_messages.empty())
      break;

    OSDMessage& new_msg = s_state.osd_posted_messages.front();
    std::deque<OSDMessage>::iterator iter;
    if (!new_msg.key.empty() &&
        (iter = std::find_if(s_state.osd_active_messages.begin(), s_state.osd_active_messages.end(),
                             [&new_msg](const OSDMessage& other) { return new_msg.key == other.key; })) !=
          s_state.osd_active_messages.end())
    {
      iter->text = std::move(new_msg.text);
      iter->duration = new_msg.duration;

      // Don't fade it in again
      const float time_passed = static_cast<float>(Timer::ConvertValueToSeconds(current_time - iter->start_time));
      iter->start_time = current_time - Timer::ConvertSecondsToValue(std::min(time_passed, OSD_FADE_IN_TIME));
    }
    else
    {
      s_state.osd_active_messages.push_back(std::move(new_msg));
    }

    s_state.osd_posted_messages.pop_front();

    static constexpr size_t MAX_ACTIVE_OSD_MESSAGES = 512;
    if (s_state.osd_active_messages.size() > MAX_ACTIVE_OSD_MESSAGES)
      s_state.osd_active_messages.pop_front();
  }
}

void ImGuiManager::DrawOSDMessages(Timer::Value current_time)
{
  using FullscreenUI::ModAlpha;
  using FullscreenUI::RenderShadowedTextClipped;
  using FullscreenUI::UIStyle;

  static constexpr float MOVE_DURATION = 0.5f;

  ImFont* const font = s_state.text_font;
  const float font_size = GetOSDFontSize();
  const float font_weight = UIStyle.NormalFontWeight;
  const float scale = s_state.global_scale;
  const float spacing = std::ceil(6.0f * scale);
  const float margin = std::ceil(s_state.screen_margin * scale);
  const float padding = std::ceil(10.0f * scale);
  const float rounding = std::ceil(10.0f * scale);
  const float max_width = s_state.window_width - (margin + padding) * 2.0f;
  const bool show_messages = g_gpu_settings.display_show_messages;
  float position_x = margin;
  float position_y = margin;

  auto iter = s_state.osd_active_messages.begin();
  while (iter != s_state.osd_active_messages.end())
  {
    OSDMessage& msg = *iter;
    const float time_passed = static_cast<float>(Timer::ConvertValueToSeconds(current_time - msg.start_time));
    if (time_passed >= msg.duration)
    {
      iter = s_state.osd_active_messages.erase(iter);
      continue;
    }

    ++iter;

    const ImVec2 text_size =
      font->CalcTextSizeA(font_size, font_weight, max_width, max_width, IMSTR_START_END(msg.text));
    float box_width = text_size.x + padding + padding;
    const float box_height = text_size.y + padding + padding;

    float opacity;
    bool clip_box = false;
    if (time_passed < OSD_FADE_IN_TIME)
    {
      const float pct = time_passed / OSD_FADE_IN_TIME;
      const float eased_pct = std::clamp(Easing::OutExpo(pct), 0.0f, 1.0f);
      box_width = box_width * eased_pct;
      opacity = pct;
      clip_box = true;
    }
    else if (time_passed > (msg.duration - OSD_FADE_OUT_TIME))
    {
      const float pct = (msg.duration - time_passed) / OSD_FADE_OUT_TIME;
      const float eased_pct = std::clamp(Easing::InExpo(pct), 0.0f, 1.0f);
      box_width = box_width * eased_pct;
      opacity = eased_pct;
      clip_box = true;
    }
    else
    {
      opacity = 1.0f;
    }

    const float expected_y = position_y;
    float actual_y = msg.last_y;
    if (msg.target_y != expected_y)
    {
      if (msg.last_y < 0.0f)
      {
        // First showing.
        msg.last_y = expected_y;
      }
      else
      {
        // We got repositioned, probably due to another message above getting removed.
        const float time_since_move = static_cast<float>(Timer::ConvertValueToSeconds(current_time - msg.move_time));
        const float frac = Easing::OutExpo(time_since_move / MOVE_DURATION);
        msg.last_y = std::floor(msg.last_y - ((msg.last_y - msg.target_y) * frac));
      }

      msg.move_time = current_time;
      msg.target_y = expected_y;
      actual_y = msg.last_y;
    }
    else if (actual_y != expected_y)
    {
      const float time_since_move = static_cast<float>(Timer::ConvertValueToSeconds(current_time - msg.move_time));
      if (time_since_move >= MOVE_DURATION)
      {
        msg.move_time = current_time;
        msg.last_y = msg.target_y;
        actual_y = msg.last_y;
      }
      else
      {
        const float frac = Easing::OutExpo(time_since_move / MOVE_DURATION);
        actual_y = std::floor(msg.last_y - ((msg.last_y - msg.target_y) * frac));
      }
    }

    if (actual_y >= ImGui::GetIO().DisplaySize.y || (!show_messages && !msg.is_warning))
      break;

    const ImVec2 pos = ImVec2(position_x, actual_y);
    const ImVec2 pos_max = ImVec2(pos.x + box_width, pos.y + box_height);
    const ImRect text_rect =
      ImRect(pos.x + padding, pos.y + padding, pos.x + box_width - padding, pos.y + box_height - padding);

    ImDrawList* const dl = ImGui::GetForegroundDrawList();

    if (clip_box)
      dl->PushClipRect(pos, pos_max);

    dl->AddRectFilled(pos, pos_max, ImGui::GetColorU32(ModAlpha(UIStyle.ToastBackgroundColor, opacity * 0.95f)),
                      rounding);
    RenderShadowedTextClipped(dl, font, font_size, font_weight, text_rect.Min, text_rect.Max,
                              ImGui::GetColorU32(ModAlpha(UIStyle.ToastTextColor, opacity)), msg.text, &text_size,
                              ImVec2(0.0f, 0.0f), max_width, &text_rect, scale);

    if (clip_box)
      dl->PopClipRect();

    position_y += box_height + spacing;
  }
}

void ImGuiManager::RenderOSDMessages()
{
  const Timer::Value current_time = Timer::GetCurrentValue();
  AcquirePendingOSDMessages(current_time);
  DrawOSDMessages(current_time);
}

void Host::AddOSDMessage(std::string message, float duration /*= 2.0f*/)
{
  ImGuiManager::AddOSDMessage(std::string(), std::move(message), duration, false);
}

void Host::AddKeyedOSDMessage(std::string key, std::string message, float duration /* = 2.0f */)
{
  ImGuiManager::AddOSDMessage(std::move(key), std::move(message), duration, false);
}

void Host::AddIconOSDMessage(std::string key, const char* icon, std::string message, float duration /* = 2.0f */)
{
  ImGuiManager::AddOSDMessage(std::move(key), fmt::format("{}  {}", icon, message), duration, false);
}

void Host::AddKeyedOSDWarning(std::string key, std::string message, float duration /* = 2.0f */)
{
  ImGuiManager::AddOSDMessage(std::move(key), std::move(message), duration, true);
}

void Host::AddIconOSDWarning(std::string key, const char* icon, std::string message, float duration /* = 2.0f */)
{
  ImGuiManager::AddOSDMessage(std::move(key), fmt::format("{}  {}", icon, message), duration, true);
}

void Host::RemoveKeyedOSDMessage(std::string key)
{
  ImGuiManager::RemoveKeyedOSDMessage(std::move(key), false);
}

void Host::RemoveKeyedOSDWarning(std::string key)
{
  ImGuiManager::RemoveKeyedOSDMessage(std::move(key), true);
}

void Host::ClearOSDMessages(bool clear_warnings)
{
  ImGuiManager::ClearOSDMessages(clear_warnings);
}

float ImGuiManager::GetGlobalScale()
{
  return s_state.global_scale;
}

float ImGuiManager::GetScreenMargin()
{
  return s_state.screen_margin;
}

ImFont* ImGuiManager::GetTextFont()
{
  return s_state.text_font;
}

float ImGuiManager::GetFixedFontSize()
{
  return std::ceil(15.0f * s_state.global_scale);
}

ImFont* ImGuiManager::GetFixedFont()
{
  return s_state.fixed_font;
}

float ImGuiManager::GetDebugFontSize(float window_scale)
{
  return std::ceil(15.0f * window_scale);
}

float ImGuiManager::GetOSDFontSize()
{
  return std::ceil(17.0f * s_state.global_scale);
}

float ImGuiManager::OSDScale(float size)
{
  return std::ceil(size * s_state.global_scale);
}

bool ImGuiManager::WantsTextInput()
{
  return s_state.imgui_wants_keyboard.load(std::memory_order_acquire);
}

bool ImGuiManager::WantsMouseInput()
{
  return s_state.imgui_wants_mouse.load(std::memory_order_acquire);
}

void ImGuiManager::AddTextInput(std::string str)
{
  if (!s_state.imgui_context || !s_state.imgui_wants_keyboard.load(std::memory_order_acquire))
    return;

  GPUThread::RunOnThread([str = std::move(str)]() {
    if (!s_state.imgui_context)
      return;

    s_state.imgui_context->IO.AddInputCharactersUTF8(str.c_str());
  });
}

void ImGuiManager::UpdateMousePosition(float x, float y)
{
  if (!s_state.imgui_context)
    return;

  s_state.imgui_context->IO.MousePos = ImVec2(x, y);
  std::atomic_thread_fence(std::memory_order_release);
}

void ImGuiManager::SetCommonIOOptions(ImGuiIO& io, ImGuiPlatformIO& pio)
{
  io.KeyRepeatDelay = 0.5f;
  pio.Platform_GetClipboardTextFn = GetClipboardTextImpl;
  pio.Platform_SetClipboardTextFn = SetClipboardTextImpl;
}

bool ImGuiManager::ProcessPointerButtonEvent(InputBindingKey key, float value)
{
  if (!s_state.imgui_context || key.data >= std::size(ImGui::GetIO().MouseDown))
    return false;

  // still update state anyway
  const int button = static_cast<int>(key.data);
  const bool pressed = (value != 0.0f);
  GPUThread::RunOnThread([button, pressed]() {
    if (!s_state.imgui_context)
      return;

    s_state.imgui_context->IO.AddMouseButtonEvent(button, pressed);
  });

  return s_state.imgui_wants_mouse.load(std::memory_order_acquire);
}

bool ImGuiManager::ProcessPointerAxisEvent(InputBindingKey key, float value)
{
  if (!s_state.imgui_context || key.data < static_cast<u32>(InputPointerAxis::WheelX))
    return false;

  // still update state anyway
  const bool horizontal = (key.data == static_cast<u32>(InputPointerAxis::WheelX));
  GPUThread::RunOnThread([value, horizontal]() {
    if (!s_state.imgui_context)
      return;

    s_state.imgui_context->IO.AddMouseWheelEvent(horizontal ? value : 0.0f, horizontal ? 0.0f : value);
  });

  return s_state.imgui_wants_mouse.load(std::memory_order_acquire);
}

bool ImGuiManager::ProcessHostKeyEvent(InputBindingKey key, float value)
{
  if (!s_state.imgui_context)
    return false;

  if (const std::optional<ImGuiKey> imkey = MapHostKeyEventToImGuiKey(key.data))
  {
    GPUThread::RunOnThread([imkey = imkey.value(), pressed = (value != 0.0f)]() {
      if (!s_state.imgui_context)
        return;

      SetImKeyState(s_state.imgui_context->IO, imkey, pressed);
    });
  }

  return s_state.imgui_wants_keyboard.load(std::memory_order_acquire);
}

void ImGuiManager::SetImKeyState(ImGuiIO& io, ImGuiKey imkey, bool pressed)
{
  io.AddKeyEvent(imkey, pressed);

  // modifier keys need to be handled separately
  if ((imkey >= ImGuiKey_LeftCtrl && imkey <= ImGuiKey_LeftSuper) ||
      (imkey >= ImGuiKey_RightCtrl && imkey <= ImGuiKey_RightSuper))
  {
    const u32 idx = imkey - ((imkey >= ImGuiKey_RightCtrl) ? ImGuiKey_RightCtrl : ImGuiKey_LeftCtrl);
    io.AddKeyEvent(static_cast<ImGuiKey>(static_cast<u32>(ImGuiMod_Ctrl) << idx), pressed);
  }
}

bool ImGuiManager::ProcessGenericInputEvent(GenericInputBinding key, float value)
{
  static constexpr std::array key_map = {
    ImGuiKey_None,               // Unknown,
    ImGuiKey_GamepadDpadUp,      // DPadUp
    ImGuiKey_GamepadDpadRight,   // DPadRight
    ImGuiKey_GamepadDpadLeft,    // DPadLeft
    ImGuiKey_GamepadDpadDown,    // DPadDown
    ImGuiKey_GamepadLStickUp,    // LeftStickUp
    ImGuiKey_GamepadLStickRight, // LeftStickRight
    ImGuiKey_GamepadLStickDown,  // LeftStickDown
    ImGuiKey_GamepadLStickLeft,  // LeftStickLeft
    ImGuiKey_GamepadL3,          // L3
    ImGuiKey_None,               // RightStickUp
    ImGuiKey_None,               // RightStickRight
    ImGuiKey_None,               // RightStickDown
    ImGuiKey_None,               // RightStickLeft
    ImGuiKey_GamepadR3,          // R3
    ImGuiKey_GamepadFaceUp,      // Triangle
    ImGuiKey_GamepadFaceRight,   // Circle
    ImGuiKey_GamepadFaceDown,    // Cross
    ImGuiKey_GamepadFaceLeft,    // Square
    ImGuiKey_GamepadBack,        // Select
    ImGuiKey_GamepadStart,       // Start
    ImGuiKey_None,               // System
    ImGuiKey_GamepadL1,          // L1
    ImGuiKey_GamepadL2,          // L2
    ImGuiKey_GamepadR1,          // R1
    ImGuiKey_GamepadL2,          // R2
  };

  const ImGuiKey imkey = (static_cast<u32>(key) < key_map.size()) ? key_map[static_cast<u32>(key)] : ImGuiKey_None;
  if (imkey == ImGuiKey_None)
    return false;

  // Racey read, but that's okay, worst case we push a couple of keys during shutdown.
  if (!s_state.imgui_context)
    return false;

  GPUThread::RunOnThread([imkey, value]() {
    if (!s_state.imgui_context)
      return;

    if (imkey >= ImGuiKey_GamepadLStickLeft && imkey <= ImGuiKey_GamepadLStickDown)
    {
      // NOTE: This assumes the source is sending a whole axis value, not half axis.
      const u32 axis = BoolToUInt32(imkey >= ImGuiKey_GamepadLStickUp);
      const s8 old_state = s_state.left_stick_axis_state[axis];
      const s8 new_state = (value <= -0.5f) ? -1 : ((value >= 0.5f) ? 1 : 0);
      if (old_state != new_state)
      {
        static constexpr auto map_to_key = [](u32 axis, s8 state) {
          // 0:-1/1 => ImGuiKey_GamepadDpadLeft/Right, 1:-1/1 => ImGuiKey_GamepadDpadUp/ImGuiKey_GamepadDpadDown
          return static_cast<ImGuiKey>(static_cast<u32>(ImGuiKey_GamepadDpadLeft) + (axis << 1) +
                                       BoolToUInt32(state > 0));
        };

        s_state.left_stick_axis_state[axis] = new_state;
        if (old_state != 0)
          s_state.imgui_context->IO.AddKeyAnalogEvent(map_to_key(axis, old_state), false, 0.0f);
        if (new_state != 0)
          s_state.imgui_context->IO.AddKeyAnalogEvent(map_to_key(axis, new_state), true, 1.0f);
      }
    }
    else
    {
      s_state.imgui_context->IO.AddKeyAnalogEvent(imkey, (value > 0.0f), value);
    }
  });

  return s_state.imgui_wants_keyboard.load(std::memory_order_acquire);
}

const char* ImGuiManager::GetClipboardTextImpl(ImGuiContext* ctx)
{
  const std::string text = Host::GetClipboardText();
  if (text.empty() || text.length() >= std::numeric_limits<int>::max())
    return nullptr;

  const size_t length = text.length();
  GImGui->ClipboardHandlerData.resize(static_cast<int>(length + 1));
  std::memcpy(GImGui->ClipboardHandlerData.Data, text.data(), length);
  GImGui->ClipboardHandlerData.Data[length] = 0;
  return GImGui->ClipboardHandlerData.Data;
}

void ImGuiManager::SetClipboardTextImpl(ImGuiContext* ctx, const char* text)
{
  const size_t length = std::strlen(text);
  if (length == 0)
    return;

  Host::CopyTextToClipboard(std::string_view(text, length));
}

void ImGuiManager::CreateSoftwareCursorTextures()
{
  for (SoftwareCursor& sc : s_state.software_cursors)
  {
    // This would normally be a racey read, but when we're initializing ImGuiManager, it's during a reconfigure of the
    // GPUThread. That means that the CPU thread is waiting for the reconfigure to finish.
    if (!sc.image_path.empty())
      UpdateSoftwareCursorTexture(sc, sc.image_path);
  }
}

void ImGuiManager::DestroySoftwareCursorTextures()
{
  for (SoftwareCursor& sc : s_state.software_cursors)
    sc.texture.reset();
}

void ImGuiManager::UpdateSoftwareCursorTexture(SoftwareCursor& sc, const std::string& image_path)
{
  if (image_path.empty())
  {
    sc.texture.reset();
    return;
  }

  Error error;
  Image image;
  if (!image.LoadFromFile(image_path.c_str(), &error))
  {
    ERROR_LOG("Failed to load software cursor {} image '{}': {}", std::distance(s_state.software_cursors.data(), &sc),
              image_path, error.GetDescription());
    return;
  }
  g_gpu_device->RecycleTexture(std::move(sc.texture));
  sc.texture = g_gpu_device->FetchTexture(image.GetWidth(), image.GetHeight(), 1, 1, 1, GPUTexture::Type::Texture,
                                          GPUTexture::Format::RGBA8, GPUTexture::Flags::None, image.GetPixels(),
                                          image.GetPitch(), &error);
  if (!sc.texture)
  {
    ERROR_LOG("Failed to upload {}x{} software cursor {} image '{}': {}", image.GetWidth(), image.GetHeight(),
              std::distance(s_state.software_cursors.data(), &sc), image_path, error.GetDescription());
    return;
  }

  sc.extent_x = std::ceil(static_cast<float>(image.GetWidth()) * sc.scale * s_state.global_scale) / 2.0f;
  sc.extent_y = std::ceil(static_cast<float>(image.GetHeight()) * sc.scale * s_state.global_scale) / 2.0f;
}

void ImGuiManager::DrawSoftwareCursor(const SoftwareCursor& sc, const std::pair<float, float>& pos)
{
  if (!sc.texture)
    return;

  const ImVec2 min(pos.first - sc.extent_x, pos.second - sc.extent_y);
  const ImVec2 max(pos.first + sc.extent_x, pos.second + sc.extent_y);

  ImDrawList* dl = ImGui::GetForegroundDrawList();

  dl->AddImage(reinterpret_cast<ImTextureID>(sc.texture.get()), min, max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
               sc.color);
}

void ImGuiManager::RenderSoftwareCursors()
{
  // This one's okay to race, worst that happens is we render the wrong number of cursors for a frame.
  const u32 pointer_count = InputManager::GetPointerCount();
  for (u32 i = 0; i < pointer_count; i++)
    DrawSoftwareCursor(s_state.software_cursors[i], InputManager::GetPointerAbsolutePosition(i));

  for (u32 i = InputManager::MAX_POINTER_DEVICES; i < InputManager::MAX_SOFTWARE_CURSORS; i++)
    DrawSoftwareCursor(s_state.software_cursors[i], s_state.software_cursors[i].pos);
}

void ImGuiManager::SetSoftwareCursor(u32 index, std::string image_path, float image_scale, u32 multiply_color)
{
  DebugAssert(index < std::size(s_state.software_cursors));
  SoftwareCursor& sc = s_state.software_cursors[index];
  sc.color = multiply_color | 0xFF000000;
  if (sc.image_path == image_path && sc.scale == image_scale)
    return;

  const bool is_hiding_or_showing = (image_path.empty() != sc.image_path.empty());
  sc.image_path = std::move(image_path);
  sc.scale = image_scale;
  if (GPUThread::IsGPUBackendRequested())
  {
    GPUThread::RunOnThread([index, image_path = sc.image_path]() {
      if (GPUThread::HasGPUBackend())
        UpdateSoftwareCursorTexture(s_state.software_cursors[index], image_path);
    });
  }

  // Hide the system cursor when we activate a software cursor.
  if (is_hiding_or_showing && index <= InputManager::MAX_POINTER_DEVICES)
    InputManager::UpdateRelativeMouseMode();
}

bool ImGuiManager::HasSoftwareCursor(u32 index)
{
  return (index < s_state.software_cursors.size() && !s_state.software_cursors[index].image_path.empty());
}

void ImGuiManager::ClearSoftwareCursor(u32 index)
{
  SetSoftwareCursor(index, std::string(), 0.0f, 0);
}

void ImGuiManager::SetSoftwareCursorPosition(u32 index, float pos_x, float pos_y)
{
  DebugAssert(index >= InputManager::MAX_POINTER_DEVICES);
  SoftwareCursor& sc = s_state.software_cursors[index];
  sc.pos.first = pos_x;
  sc.pos.second = pos_y;
}

std::string ImGuiManager::StripIconCharacters(std::string_view str)
{
  std::string result;
  result.reserve(str.length());

  for (size_t offset = 0; offset < str.length();)
  {
    char32_t utf;
    offset += StringUtil::DecodeUTF8(str, offset, &utf);

    // icon if outside BMP/SMP/TIP, or inside private use area
    if (utf > 0x32FFF || (utf >= 0xE000 && utf <= 0xF8FF))
      continue;

    StringUtil::EncodeAndAppendUTF8(result, utf);
  }

  StringUtil::StripWhitespace(&result);

  return result;
}

std::optional<ImGuiKey> ImGuiManager::MapHostKeyEventToImGuiKey(u32 key)
{
  // mapping of host key -> imgui key
  static constexpr const std::pair<USBKeyCode, ImGuiKey> mapping[] = {
    {USBKeyCode::Super, ImGuiKey_LeftSuper},
    {USBKeyCode::A, ImGuiKey_A},
    {USBKeyCode::B, ImGuiKey_B},
    {USBKeyCode::C, ImGuiKey_C},
    {USBKeyCode::D, ImGuiKey_D},
    {USBKeyCode::E, ImGuiKey_E},
    {USBKeyCode::F, ImGuiKey_F},
    {USBKeyCode::G, ImGuiKey_G},
    {USBKeyCode::H, ImGuiKey_H},
    {USBKeyCode::I, ImGuiKey_I},
    {USBKeyCode::J, ImGuiKey_J},
    {USBKeyCode::K, ImGuiKey_K},
    {USBKeyCode::L, ImGuiKey_L},
    {USBKeyCode::M, ImGuiKey_M},
    {USBKeyCode::N, ImGuiKey_N},
    {USBKeyCode::O, ImGuiKey_O},
    {USBKeyCode::P, ImGuiKey_P},
    {USBKeyCode::Q, ImGuiKey_Q},
    {USBKeyCode::R, ImGuiKey_R},
    {USBKeyCode::S, ImGuiKey_S},
    {USBKeyCode::T, ImGuiKey_T},
    {USBKeyCode::U, ImGuiKey_U},
    {USBKeyCode::V, ImGuiKey_V},
    {USBKeyCode::W, ImGuiKey_W},
    {USBKeyCode::X, ImGuiKey_X},
    {USBKeyCode::Y, ImGuiKey_Y},
    {USBKeyCode::Z, ImGuiKey_Z},
    {USBKeyCode::Digit1, ImGuiKey_1},
    {USBKeyCode::Digit2, ImGuiKey_2},
    {USBKeyCode::Digit3, ImGuiKey_3},
    {USBKeyCode::Digit4, ImGuiKey_4},
    {USBKeyCode::Digit5, ImGuiKey_5},
    {USBKeyCode::Digit6, ImGuiKey_6},
    {USBKeyCode::Digit7, ImGuiKey_7},
    {USBKeyCode::Digit8, ImGuiKey_8},
    {USBKeyCode::Digit9, ImGuiKey_9},
    {USBKeyCode::Digit0, ImGuiKey_0},
    {USBKeyCode::Enter, ImGuiKey_Enter},
    {USBKeyCode::Escape, ImGuiKey_Escape},
    {USBKeyCode::Backspace, ImGuiKey_Backspace},
    {USBKeyCode::Tab, ImGuiKey_Tab},
    {USBKeyCode::Space, ImGuiKey_Space},
    {USBKeyCode::Minus, ImGuiKey_Minus},
    {USBKeyCode::Equal, ImGuiKey_Equal},
    {USBKeyCode::BracketLeft, ImGuiKey_LeftBracket},
    {USBKeyCode::BracketRight, ImGuiKey_RightBracket},
    {USBKeyCode::Backslash, ImGuiKey_Backslash},
    {USBKeyCode::Semicolon, ImGuiKey_Semicolon},
    {USBKeyCode::Quote, ImGuiKey_Apostrophe},
    {USBKeyCode::Backquote, ImGuiKey_GraveAccent},
    {USBKeyCode::Comma, ImGuiKey_Comma},
    {USBKeyCode::Period, ImGuiKey_Period},
    {USBKeyCode::Slash, ImGuiKey_Slash},
    {USBKeyCode::CapsLock, ImGuiKey_CapsLock},
    {USBKeyCode::F1, ImGuiKey_F1},
    {USBKeyCode::F2, ImGuiKey_F2},
    {USBKeyCode::F3, ImGuiKey_F3},
    {USBKeyCode::F4, ImGuiKey_F4},
    {USBKeyCode::F5, ImGuiKey_F5},
    {USBKeyCode::F6, ImGuiKey_F6},
    {USBKeyCode::F7, ImGuiKey_F7},
    {USBKeyCode::F8, ImGuiKey_F8},
    {USBKeyCode::F9, ImGuiKey_F9},
    {USBKeyCode::F10, ImGuiKey_F10},
    {USBKeyCode::F11, ImGuiKey_F11},
    {USBKeyCode::F12, ImGuiKey_F12},
    {USBKeyCode::PrintScreen, ImGuiKey_PrintScreen},
    {USBKeyCode::ScrollLock, ImGuiKey_ScrollLock},
    {USBKeyCode::Pause, ImGuiKey_Pause},
    {USBKeyCode::Insert, ImGuiKey_Insert},
    {USBKeyCode::Home, ImGuiKey_Home},
    {USBKeyCode::PageUp, ImGuiKey_PageUp},
    {USBKeyCode::Delete, ImGuiKey_Delete},
    {USBKeyCode::End, ImGuiKey_End},
    {USBKeyCode::PageDown, ImGuiKey_PageDown},
    {USBKeyCode::ArrowRight, ImGuiKey_RightArrow},
    {USBKeyCode::ArrowLeft, ImGuiKey_LeftArrow},
    {USBKeyCode::ArrowDown, ImGuiKey_DownArrow},
    {USBKeyCode::ArrowUp, ImGuiKey_UpArrow},
    {USBKeyCode::NumLock, ImGuiKey_NumLock},
    {USBKeyCode::NumpadDivide, ImGuiKey_KeypadDivide},
    {USBKeyCode::NumpadMultiply, ImGuiKey_KeypadMultiply},
    {USBKeyCode::NumpadSubtract, ImGuiKey_KeypadSubtract},
    {USBKeyCode::NumpadAdd, ImGuiKey_KeypadAdd},
    {USBKeyCode::NumpadEnter, ImGuiKey_KeypadEnter},
    {USBKeyCode::Numpad1, ImGuiKey_Keypad1},
    {USBKeyCode::Numpad2, ImGuiKey_Keypad2},
    {USBKeyCode::Numpad3, ImGuiKey_Keypad3},
    {USBKeyCode::Numpad4, ImGuiKey_Keypad4},
    {USBKeyCode::Numpad5, ImGuiKey_Keypad5},
    {USBKeyCode::Numpad6, ImGuiKey_Keypad6},
    {USBKeyCode::Numpad7, ImGuiKey_Keypad7},
    {USBKeyCode::Numpad8, ImGuiKey_Keypad8},
    {USBKeyCode::Numpad9, ImGuiKey_Keypad9},
    {USBKeyCode::Numpad0, ImGuiKey_Keypad0},
    {USBKeyCode::NumpadDecimal, ImGuiKey_KeypadDecimal},
    {USBKeyCode::ContextMenu, ImGuiKey_Menu},
    {USBKeyCode::NumpadEqual, ImGuiKey_KeypadEqual},
    {USBKeyCode::ControlLeft, ImGuiKey_LeftCtrl},
    {USBKeyCode::ShiftLeft, ImGuiKey_LeftShift},
    {USBKeyCode::AltLeft, ImGuiKey_LeftAlt},
    {USBKeyCode::ControlRight, ImGuiKey_RightCtrl},
    {USBKeyCode::ShiftRight, ImGuiKey_RightShift},
    {USBKeyCode::AltRight, ImGuiKey_RightAlt},
  };

  static_assert(
    []() {
      for (size_t i = 1; i < std::size(mapping); i++)
      {
        if (mapping[i].first <= mapping[i - 1].first)
          return false;
      }
      return true;
    }(),
    "Key map must be sorted by USBKeyCode");

  const auto it = std::lower_bound(std::begin(mapping), std::end(mapping), static_cast<USBKeyCode>(key),
                                   [](const std::pair<USBKeyCode, ImGuiKey>& a, USBKeyCode b) { return a.first < b; });
  return (it != std::end(mapping) && it->first == static_cast<USBKeyCode>(key)) ? std::optional<ImGuiKey>{it->second} :
                                                                                  std::nullopt;
}

#ifndef __ANDROID__

bool ImGuiManager::CreateAuxiliaryRenderWindow(AuxiliaryRenderWindowState* state, std::string_view title,
                                               std::string_view icon_name, const char* config_section,
                                               const char* config_prefix, u32 default_width, u32 default_height,
                                               Error* error)
{
  constexpr s32 DEFAULT_POSITION = std::numeric_limits<s32>::min();

  // figure out where to position it
  s32 pos_x = DEFAULT_POSITION;
  s32 pos_y = DEFAULT_POSITION;
  u32 width = default_width;
  u32 height = default_height;
  if (config_prefix)
  {
    pos_x = Host::GetBaseIntSettingValue(config_section, TinyString::from_format("{}PositionX", config_prefix),
                                         DEFAULT_POSITION);
    pos_y = Host::GetBaseIntSettingValue(config_section, TinyString::from_format("{}PositionY", config_prefix),
                                         DEFAULT_POSITION);
    width =
      Host::GetBaseUIntSettingValue(config_section, TinyString::from_format("{}Width", config_prefix), default_width);
    height =
      Host::GetBaseUIntSettingValue(config_section, TinyString::from_format("{}Height", config_prefix), default_height);
  }

  WindowInfo wi;
  if (!Host::CreateAuxiliaryRenderWindow(pos_x, pos_y, width, height, title, icon_name, state, &state->window_handle,
                                         &wi, error))
  {
    return false;
  }

  state->swap_chain = g_gpu_device->CreateSwapChain(wi, GPUVSyncMode::Disabled, false, nullptr, std::nullopt, error);
  if (!state->swap_chain)
  {
    Host::DestroyAuxiliaryRenderWindow(state->window_handle);
    state->window_handle = nullptr;
    return false;
  }

  state->imgui_context = ImGui::CreateContext(s_state.imgui_context->IO.Fonts);
  state->imgui_context->Viewports[0]->Size = state->imgui_context->IO.DisplaySize =
    ImVec2(static_cast<float>(state->swap_chain->GetWidth()), static_cast<float>(state->swap_chain->GetHeight()));
  state->imgui_context->IO.IniFilename = nullptr;
  state->imgui_context->IO.BackendFlags |=
    ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
  state->imgui_context->IO.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
  SetCommonIOOptions(state->imgui_context->IO, state->imgui_context->PlatformIO);

  SetStyle(state->imgui_context->Style, state->swap_chain->GetScale());
  state->imgui_context->Style.WindowBorderSize = 0.0f;

  state->close_request = false;
  return true;
}

void ImGuiManager::DestroyAuxiliaryRenderWindow(AuxiliaryRenderWindowState* state, const char* config_section,
                                                const char* config_prefix)
{
  constexpr s32 DEFAULT_POSITION = std::numeric_limits<s32>::min();

  if (!state->window_handle)
    return;

  s32 old_pos_x = DEFAULT_POSITION, old_pos_y = DEFAULT_POSITION;
  u32 old_width = 0, old_height = 0;
  if (config_section)
  {
    old_pos_x = Host::GetBaseIntSettingValue(config_section, TinyString::from_format("{}PositionX", config_prefix),
                                             DEFAULT_POSITION);
    old_pos_y = Host::GetBaseIntSettingValue(config_section, TinyString::from_format("{}PositionY", config_prefix),
                                             DEFAULT_POSITION);
    old_width = Host::GetBaseUIntSettingValue(config_section, TinyString::from_format("{}Width", config_prefix), 0);
    old_height = Host::GetBaseUIntSettingValue(config_section, TinyString::from_format("{}Height", config_prefix), 0);
  }

  ImGui::DestroyContext(state->imgui_context);
  state->imgui_context = nullptr;
  state->swap_chain.reset();
  state->close_request = false;

  // store positioning for config
  s32 new_pos_x = old_pos_x, new_pos_y = old_pos_y;
  u32 new_width = old_width, new_height = old_height;
  Host::DestroyAuxiliaryRenderWindow(std::exchange(state->window_handle, nullptr), &new_pos_x, &new_pos_y, &new_width,
                                     &new_height);

  if (config_section)
  {
    // update config if the window was moved
    if (old_pos_x != new_pos_x || old_pos_y != new_pos_y || old_width != new_width || old_height != new_height)
    {
      Host::SetBaseIntSettingValue(config_section, TinyString::from_format("{}PositionX", config_prefix), new_pos_x);
      Host::SetBaseIntSettingValue(config_section, TinyString::from_format("{}PositionY", config_prefix), new_pos_y);
      Host::SetBaseUIntSettingValue(config_section, TinyString::from_format("{}Width", config_prefix), new_width);
      Host::SetBaseUIntSettingValue(config_section, TinyString::from_format("{}Height", config_prefix), new_height);
      Host::CommitBaseSettingChanges();
    }
  }
}

bool ImGuiManager::RenderAuxiliaryRenderWindow(AuxiliaryRenderWindowState* state, void (*draw_callback)(float scale))
{
  DebugAssert(state->window_handle);
  if (state->close_request)
    return false;

  ImGui::SetCurrentContext(state->imgui_context);

  const float window_scale = state->swap_chain->GetScale();

  ImGui::NewFrame();
  ImGui::PushFont(s_state.text_font, GetDebugFontSize(window_scale), 0.0f);
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(state->imgui_context->IO.DisplaySize, ImGuiCond_Always);
  if (ImGui::Begin("AuxRenderWindowMain", nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse))
  {
    draw_callback(window_scale);
  }

  ImGui::End();
  ImGui::PopFont();

  CreateDrawLists();

  const GPUDevice::PresentResult pres = g_gpu_device->BeginPresent(state->swap_chain.get());
  if (pres == GPUDevice::PresentResult::OK)
  {
    RenderDrawLists(state->swap_chain.get());
    g_gpu_device->EndPresent(state->swap_chain.get(), false);
  }

  ImGui::SetCurrentContext(GetMainContext());
  return true;
}

void ImGuiManager::ProcessAuxiliaryRenderWindowInputEvent(Host::AuxiliaryRenderWindowUserData userdata,
                                                          Host::AuxiliaryRenderWindowEvent event,
                                                          Host::AuxiliaryRenderWindowEventParam param1,
                                                          Host::AuxiliaryRenderWindowEventParam param2,
                                                          Host::AuxiliaryRenderWindowEventParam param3)
{
  // we can get bogus events here after the user closes it, so check we're not being destroyed
  AuxiliaryRenderWindowState* state = static_cast<AuxiliaryRenderWindowState*>(userdata);
  if (!state->window_handle) [[unlikely]]
    return;

  ImGuiIO& io = state->imgui_context->IO;

  switch (event)
  {
    case Host::AuxiliaryRenderWindowEvent::CloseRequest:
    {
      state->close_request = true;
    }
    break;

    case Host::AuxiliaryRenderWindowEvent::Resized:
    {
      Error error;
      if (!state->swap_chain->ResizeBuffers(param1.uint_param, param2.uint_param, param3.float_param, &error))
      {
        ERROR_LOG("Failed to resize aux window swap chain to {}x{}: {}", param1.uint_param, param2.uint_param,
                  error.GetDescription());
        return;
      }

      state->imgui_context->Viewports[0]->Size = state->imgui_context->IO.DisplaySize =
        ImVec2(static_cast<float>(param1.uint_param), static_cast<float>(param2.uint_param));
    }
    break;

    case Host::AuxiliaryRenderWindowEvent::KeyPressed:
    case Host::AuxiliaryRenderWindowEvent::KeyReleased:
    {
      if (const std::optional<ImGuiKey> imkey = MapHostKeyEventToImGuiKey(param1.uint_param))
        SetImKeyState(io, imkey.value(), (event == Host::AuxiliaryRenderWindowEvent::KeyPressed));
    }
    break;

    case Host::AuxiliaryRenderWindowEvent::TextEntered:
    {
      io.AddInputCharacter(param1.uint_param);
    }
    break;

    case Host::AuxiliaryRenderWindowEvent::MouseMoved:
    {
      io.MousePos.x = param1.float_param;
      io.MousePos.y = param2.float_param;
    }
    break;

    case Host::AuxiliaryRenderWindowEvent::MousePressed:
    case Host::AuxiliaryRenderWindowEvent::MouseReleased:
    {
      io.AddMouseButtonEvent(param1.uint_param, (event == Host::AuxiliaryRenderWindowEvent::MousePressed));
    }
    break;

    case Host::AuxiliaryRenderWindowEvent::MouseWheel:
    {
      io.AddMouseWheelEvent(param1.float_param, param2.float_param);
    }
    break;

    default:
      break;
  }
}

#endif // __ANDROID__
