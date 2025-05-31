// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "imgui_manager.h"
#include "gpu_device.h"
#include "host.h"
#include "image.h"
#include "imgui_fullscreen.h"
#include "imgui_glyph_ranges.inl"
#include "input_manager.h"
#include "shadergen.h"

// TODO: Remove me when GPUDevice config is also cleaned up.
#include "core/fullscreen_ui.h"
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
#include "common/timer.h"

#include "IconsFontAwesome5.h"
#include "fmt/format.h"
#include "imgui.h"
#include "imgui_freetype.h"
#include "imgui_internal.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
#include <type_traits>
#include <unordered_map>

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
static void SetKeyMap();
static bool LoadFontData(Error* error);
static void ReloadFontDataIfActive();
static bool AddImGuiFonts(bool debug_font, bool fullscreen_fonts);
static ImFont* AddTextFont(float size, const ImWchar* glyph_range);
static ImFont* AddFixedFont(float size);
static bool AddIconFonts(float size, const ImWchar* emoji_range);
static bool CompilePipelines(Error* error);
static void RenderDrawLists(u32 window_width, u32 window_height, WindowInfo::PreRotation prerotation);
static bool UpdateImGuiFontTexture();
static void SetCommonIOOptions(ImGuiIO& io);
static void SetImKeyState(ImGuiIO& io, ImGuiKey imkey, bool pressed);
static const char* GetClipboardTextImpl(void* userdata);
static void SetClipboardTextImpl(void* userdata, const char* text);
static void AddOSDMessage(std::string key, std::string message, float duration, bool is_warning);
static void RemoveKeyedOSDMessage(std::string key, bool is_warning);
static void ClearOSDMessages(bool clear_warnings);
static void AcquirePendingOSDMessages(Timer::Value current_time);
static void DrawOSDMessages(Timer::Value current_time);
static void CreateSoftwareCursorTextures();
static void UpdateSoftwareCursorTexture(SoftwareCursor& cursor, const std::string& image_path);
static void DestroySoftwareCursorTextures();
static void DrawSoftwareCursor(const SoftwareCursor& sc, const std::pair<float, float>& pos);

static constexpr float OSD_FADE_IN_TIME = 0.1f;
static constexpr float OSD_FADE_OUT_TIME = 0.4f;

static constexpr std::array<ImWchar, 4> ASCII_FONT_RANGE = {{0x20, 0x7F, 0x00, 0x00}};
static constexpr std::array<ImWchar, 6> DEFAULT_FONT_RANGE = {{0x0020, 0x00FF, 0x2022, 0x2022, 0x0000, 0x0000}};

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
  std::unique_ptr<GPUTexture> imgui_font_texture;

  ImFont* debug_font = nullptr;
  ImFont* osd_font = nullptr;
  ImFont* fixed_font = nullptr;
  ImFont* medium_font = nullptr;
  ImFont* large_font = nullptr;

  std::deque<OSDMessage> osd_active_messages;

  std::array<ImGuiManager::SoftwareCursor, InputManager::MAX_SOFTWARE_CURSORS> software_cursors = {};

  // mapping of host key -> imgui key
  ALIGN_TO_CACHE_LINE std::unordered_map<u32, ImGuiKey> imgui_key_map;

  std::string font_path;
  std::vector<WCharType> font_range;
  std::vector<WCharType> dynamic_font_range;
  std::vector<WCharType> dynamic_emoji_range;

  DynamicHeapArray<u8> standard_font_data;
  DynamicHeapArray<u8> fixed_font_data;
  DynamicHeapArray<u8> icon_fa_font_data;
  DynamicHeapArray<u8> icon_pf_font_data;
  DynamicHeapArray<u8> emoji_font_data;
};

} // namespace

static State s_state;

} // namespace ImGuiManager

void ImGuiManager::SetFontPathAndRange(std::string path, std::vector<WCharType> range)
{
  if (s_state.font_path == path && s_state.font_range == range)
    return;

  s_state.font_path = std::move(path);
  s_state.font_range = std::move(range);
  s_state.standard_font_data = {};
  ReloadFontDataIfActive();
}

void ImGuiManager::SetDynamicFontRange(std::vector<WCharType> font_range, std::vector<WCharType> emoji_range)
{
  if (s_state.dynamic_font_range == font_range && s_state.dynamic_emoji_range == emoji_range)
    return;

  s_state.dynamic_font_range = std::move(font_range);
  s_state.dynamic_emoji_range = std::move(emoji_range);
  ReloadFontDataIfActive();
}

std::vector<ImGuiManager::WCharType> ImGuiManager::CompactFontRange(std::span<const WCharType> range)
{
  std::vector<ImWchar> ret;

  for (auto it = range.begin(); it != range.end();)
  {
    auto next_it = it;
    ++next_it;

    // Combine sequential ranges.
    const ImWchar start_codepoint = *it;
    ImWchar end_codepoint = start_codepoint;
    while (next_it != range.end())
    {
      const ImWchar next_codepoint = *next_it;
      if (next_codepoint != (end_codepoint + 1))
        break;

      // Yep, include it.
      end_codepoint = next_codepoint;
      ++next_it;
    }

    ret.push_back(start_codepoint);
    ret.push_back(end_codepoint);

    it = next_it;
  }

  return ret;
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
  io.BackendFlags |= ImGuiBackendFlags_HasGamepad | ImGuiBackendFlags_RendererHasVtxOffset;
#ifndef __ANDROID__
  // Android has no keyboard, nor are we using ImGui for any actual user-interactable windows.
  io.ConfigFlags |=
    ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NoMouseCursorChange;
#else
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
#endif
  SetCommonIOOptions(io);

  s_state.last_render_time = Timer::GetCurrentValue();
  s_state.window_format = main_swap_chain ? main_swap_chain->GetFormat() : GPUTexture::Format::RGBA8;
  s_state.window_width = main_swap_chain ? static_cast<float>(main_swap_chain->GetWidth()) : 0.0f;
  s_state.window_height = main_swap_chain ? static_cast<float>(main_swap_chain->GetHeight()) : 0.0f;
  io.DisplayFramebufferScale = ImVec2(1, 1); // We already scale things ourselves, this would double-apply scaling
  io.DisplaySize = ImVec2(s_state.window_width, s_state.window_height);

  SetKeyMap();
  SetStyle(s_state.imgui_context->Style, s_state.global_scale);
  FullscreenUI::SetTheme();

  if (!CompilePipelines(error))
    return false;

  if (!AddImGuiFonts(false, false) || !UpdateImGuiFontTexture())
  {
    Error::SetString(error, "Failed to create ImGui font text");
    return false;
  }

  // don't need the font data anymore, save some memory
  io.Fonts->ClearTexData();

  NewFrame();

  CreateSoftwareCursorTextures();
  return true;
}

void ImGuiManager::Shutdown()
{
  DestroySoftwareCursorTextures();

  s_state.debug_font = nullptr;
  s_state.fixed_font = nullptr;
  s_state.medium_font = nullptr;
  s_state.large_font = nullptr;
  ImGuiFullscreen::SetFonts(nullptr, nullptr);

  s_state.imgui_pipeline.reset();
  g_gpu_device->RecycleTexture(std::move(s_state.imgui_font_texture));

  if (s_state.imgui_context)
  {
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

  if ((!HasFullscreenFonts() || !ImGuiFullscreen::UpdateLayoutScale()) && scale == s_state.global_scale)
    return;

  s_state.global_scale = scale;
  SetStyle(s_state.imgui_context->Style, s_state.global_scale);

  if (!AddImGuiFonts(HasDebugFont(), HasFullscreenFonts()))
  {
    GPUThread::ReportFatalErrorAndShutdown("Failed to create ImGui font text");
    return;
  }

  UpdateImGuiFontTexture();
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
  plconfig.layout = GPUPipeline::Layout::SingleTextureAndPushConstants;
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

bool ImGuiManager::UpdateImGuiFontTexture()
{
  ImGuiIO& io = ImGui::GetIO();

  unsigned char* pixels;
  int width, height;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

  Error error;
  const bool result = g_gpu_device->ResizeTexture(
    &s_state.imgui_font_texture, static_cast<u32>(width), static_cast<u32>(height), GPUTexture::Type::Texture,
    GPUTexture::Format::RGBA8, GPUTexture::Flags::None, pixels, sizeof(u32) * width, &error);
  if (!result) [[unlikely]]
    ERROR_LOG("Failed to resize ImGui font texture: {}", error.GetDescription());

  // always update pointer, it could change
  io.Fonts->SetTexID(s_state.imgui_font_texture.get());
  return result;
}

void ImGuiManager::CreateDrawLists()
{
  ImGui::EndFrame();
  ImGui::Render();
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
  g_gpu_device->PushUniformBuffer(&mproj, sizeof(mproj));

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
      g_gpu_device->SetTextureSampler(0, reinterpret_cast<GPUTexture*>(pcmd->TextureId),
                                      g_gpu_device->GetLinearSampler());

      if (pcmd->UserCallback) [[unlikely]]
      {
        pcmd->UserCallback(cmd_list, pcmd);
        g_gpu_device->PushUniformBuffer(&mproj, sizeof(mproj));
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

void ImGuiManager::SetStyle(ImGuiStyle& style, float scale)
{
  style = ImGuiStyle();
  style.WindowMinSize = ImVec2(1.0f, 1.0f);

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
  colors[ImGuiCol_TabActive] = ImVec4(0.27f, 0.32f, 0.38f, 1.00f);
  colors[ImGuiCol_TabUnfocused] = ImVec4(0.11f, 0.15f, 0.17f, 1.00f);
  colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.11f, 0.15f, 0.17f, 1.00f);
  colors[ImGuiCol_PlotLines] = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
  colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
  colors[ImGuiCol_PlotHistogram] = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
  colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
  colors[ImGuiCol_TextSelectedBg] = ImVec4(0.26f, 0.59f, 0.98f, 0.35f);
  colors[ImGuiCol_DragDropTarget] = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
  colors[ImGuiCol_NavHighlight] = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
  colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
  colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
  colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.35f);

  style.ScaleAllSizes(scale);
}

void ImGuiManager::SetKeyMap()
{
  struct KeyMapping
  {
    ImGuiKey index;
    const char* name;
    const char* alt_name;
  };

  static constexpr KeyMapping mapping[] = {
    {ImGuiKey_LeftArrow, "Left", nullptr},
    {ImGuiKey_RightArrow, "Right", nullptr},
    {ImGuiKey_UpArrow, "Up", nullptr},
    {ImGuiKey_DownArrow, "Down", nullptr},
    {ImGuiKey_PageUp, "PageUp", nullptr},
    {ImGuiKey_PageDown, "PageDown", nullptr},
    {ImGuiKey_Home, "Home", nullptr},
    {ImGuiKey_End, "End", nullptr},
    {ImGuiKey_Insert, "Insert", nullptr},
    {ImGuiKey_Delete, "Delete", nullptr},
    {ImGuiKey_Backspace, "Backspace", nullptr},
    {ImGuiKey_Space, "Space", nullptr},
    {ImGuiKey_Enter, "Return", nullptr},
    {ImGuiKey_Escape, "Escape", nullptr},
    {ImGuiKey_LeftCtrl, "LeftControl", "Control"},
    {ImGuiKey_LeftShift, "LeftShift", "Shift"},
    {ImGuiKey_LeftAlt, "LeftAlt", "Alt"},
    {ImGuiKey_LeftSuper, "LeftSuper", "Super"},
    {ImGuiKey_RightCtrl, "RightControl", nullptr},
    {ImGuiKey_RightShift, "RightShift", nullptr},
    {ImGuiKey_RightAlt, "RightAlt", nullptr},
    {ImGuiKey_RightSuper, "RightSuper", nullptr},
    {ImGuiKey_Menu, "Menu", nullptr},
    {ImGuiKey_Tab, "Tab", nullptr},
    // Hack: Report Qt's "Backtab" as Tab, we forward the shift so it gets mapped correctly
    {ImGuiKey_Tab, "Backtab", nullptr},
    {ImGuiKey_0, "0", nullptr},
    {ImGuiKey_1, "1", nullptr},
    {ImGuiKey_2, "2", nullptr},
    {ImGuiKey_3, "3", nullptr},
    {ImGuiKey_4, "4", nullptr},
    {ImGuiKey_5, "5", nullptr},
    {ImGuiKey_6, "6", nullptr},
    {ImGuiKey_7, "7", nullptr},
    {ImGuiKey_8, "8", nullptr},
    {ImGuiKey_9, "9", nullptr},
    {ImGuiKey_A, "A", nullptr},
    {ImGuiKey_B, "B", nullptr},
    {ImGuiKey_C, "C", nullptr},
    {ImGuiKey_D, "D", nullptr},
    {ImGuiKey_E, "E", nullptr},
    {ImGuiKey_F, "F", nullptr},
    {ImGuiKey_G, "G", nullptr},
    {ImGuiKey_H, "H", nullptr},
    {ImGuiKey_I, "I", nullptr},
    {ImGuiKey_J, "J", nullptr},
    {ImGuiKey_K, "K", nullptr},
    {ImGuiKey_L, "L", nullptr},
    {ImGuiKey_M, "M", nullptr},
    {ImGuiKey_N, "N", nullptr},
    {ImGuiKey_O, "O", nullptr},
    {ImGuiKey_P, "P", nullptr},
    {ImGuiKey_Q, "Q", nullptr},
    {ImGuiKey_R, "R", nullptr},
    {ImGuiKey_S, "S", nullptr},
    {ImGuiKey_T, "T", nullptr},
    {ImGuiKey_U, "U", nullptr},
    {ImGuiKey_V, "V", nullptr},
    {ImGuiKey_W, "W", nullptr},
    {ImGuiKey_X, "X", nullptr},
    {ImGuiKey_Y, "Y", nullptr},
    {ImGuiKey_Z, "Z", nullptr},
    {ImGuiKey_F1, "F1", nullptr},
    {ImGuiKey_F2, "F2", nullptr},
    {ImGuiKey_F3, "F3", nullptr},
    {ImGuiKey_F4, "F4", nullptr},
    {ImGuiKey_F5, "F5", nullptr},
    {ImGuiKey_F6, "F6", nullptr},
    {ImGuiKey_F7, "F7", nullptr},
    {ImGuiKey_F8, "F8", nullptr},
    {ImGuiKey_F9, "F9", nullptr},
    {ImGuiKey_F10, "F10", nullptr},
    {ImGuiKey_F11, "F11", nullptr},
    {ImGuiKey_F12, "F12", nullptr},
    {ImGuiKey_Apostrophe, "Apostrophe", nullptr},
    {ImGuiKey_Comma, "Comma", nullptr},
    {ImGuiKey_Minus, "Minus", nullptr},
    {ImGuiKey_Period, "Period", nullptr},
    {ImGuiKey_Slash, "Slash", nullptr},
    {ImGuiKey_Semicolon, "Semicolon", nullptr},
    {ImGuiKey_Equal, "Equal", nullptr},
    {ImGuiKey_LeftBracket, "BracketLeft", nullptr},
    {ImGuiKey_Backslash, "Backslash", nullptr},
    {ImGuiKey_RightBracket, "BracketRight", nullptr},
    {ImGuiKey_GraveAccent, "QuoteLeft", nullptr},
    {ImGuiKey_CapsLock, "CapsLock", nullptr},
    {ImGuiKey_ScrollLock, "ScrollLock", nullptr},
    {ImGuiKey_NumLock, "NumLock", nullptr},
    {ImGuiKey_PrintScreen, "PrintScreen", nullptr},
    {ImGuiKey_Pause, "Pause", nullptr},
    {ImGuiKey_Keypad0, "Keypad0", nullptr},
    {ImGuiKey_Keypad1, "Keypad1", nullptr},
    {ImGuiKey_Keypad2, "Keypad2", nullptr},
    {ImGuiKey_Keypad3, "Keypad3", nullptr},
    {ImGuiKey_Keypad4, "Keypad4", nullptr},
    {ImGuiKey_Keypad5, "Keypad5", nullptr},
    {ImGuiKey_Keypad6, "Keypad6", nullptr},
    {ImGuiKey_Keypad7, "Keypad7", nullptr},
    {ImGuiKey_Keypad8, "Keypad8", nullptr},
    {ImGuiKey_Keypad9, "Keypad9", nullptr},
    {ImGuiKey_KeypadDecimal, "KeypadPeriod", nullptr},
    {ImGuiKey_KeypadDivide, "KeypadDivide", nullptr},
    {ImGuiKey_KeypadMultiply, "KeypadMultiply", nullptr},
    {ImGuiKey_KeypadSubtract, "KeypadMinus", nullptr},
    {ImGuiKey_KeypadAdd, "KeypadPlus", nullptr},
    {ImGuiKey_KeypadEnter, "KeypadReturn", nullptr},
    {ImGuiKey_KeypadEqual, "KeypadEqual", nullptr}};

  s_state.imgui_key_map.clear();
  for (const KeyMapping& km : mapping)
  {
    std::optional<u32> map(InputManager::ConvertHostKeyboardStringToCode(km.name));
    if (!map.has_value() && km.alt_name)
      map = InputManager::ConvertHostKeyboardStringToCode(km.alt_name);
    if (map.has_value())
      s_state.imgui_key_map[map.value()] = km.index;
  }
}

bool ImGuiManager::LoadFontData(Error* error)
{
  if (s_state.standard_font_data.empty())
  {
    std::optional<DynamicHeapArray<u8>> font_data = s_state.font_path.empty() ?
                                                      Host::ReadResourceFile("fonts/Roboto-Regular.ttf", true, error) :
                                                      FileSystem::ReadBinaryFile(s_state.font_path.c_str(), error);
    if (!font_data.has_value())
      return false;

    s_state.standard_font_data = std::move(font_data.value());
  }

  if (s_state.fixed_font_data.empty())
  {
    std::optional<DynamicHeapArray<u8>> font_data = Host::ReadResourceFile("fonts/RobotoMono-Medium.ttf", true, error);
    if (!font_data.has_value())
      return false;

    s_state.fixed_font_data = std::move(font_data.value());
  }

  if (s_state.icon_fa_font_data.empty())
  {
    std::optional<DynamicHeapArray<u8>> font_data = Host::ReadResourceFile("fonts/fa-solid-900.ttf", true, error);
    if (!font_data.has_value())
      return false;

    s_state.icon_fa_font_data = std::move(font_data.value());
  }

  if (s_state.icon_pf_font_data.empty())
  {
    std::optional<DynamicHeapArray<u8>> font_data = Host::ReadResourceFile("fonts/promptfont.otf", true, error);
    if (!font_data.has_value())
      return false;

    s_state.icon_pf_font_data = std::move(font_data.value());
  }

  if (s_state.emoji_font_data.empty())
  {
    std::optional<DynamicHeapArray<u8>> font_data =
      Host::ReadCompressedResourceFile("fonts/TwitterColorEmoji-SVGinOT.ttf.zst", true, error);
    if (!font_data.has_value())
      return false;

    s_state.emoji_font_data = std::move(font_data.value());
  }

  return true;
}

ImFont* ImGuiManager::AddTextFont(float size, const ImWchar* glyph_range)
{
  ImFontConfig cfg;
  cfg.FontDataOwnedByAtlas = false;
  return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(
    s_state.standard_font_data.data(), static_cast<int>(s_state.standard_font_data.size()), size, &cfg, glyph_range);
}

ImFont* ImGuiManager::AddFixedFont(float size)
{
  ImFontConfig cfg;
  cfg.FontDataOwnedByAtlas = false;
  return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(s_state.fixed_font_data.data(),
                                                    static_cast<int>(s_state.fixed_font_data.size()), size, &cfg,
                                                    ASCII_FONT_RANGE.data());
}

bool ImGuiManager::AddIconFonts(float size, const ImWchar* emoji_range)
{
  {
    ImFontConfig cfg;
    cfg.MergeMode = true;
    cfg.PixelSnapH = true;
    cfg.GlyphMinAdvanceX = size;
    cfg.GlyphMaxAdvanceX = size;
    cfg.FontDataOwnedByAtlas = false;

    if (!ImGui::GetIO().Fonts->AddFontFromMemoryTTF(s_state.icon_fa_font_data.data(),
                                                    static_cast<int>(s_state.icon_fa_font_data.size()), size * 0.75f,
                                                    &cfg, FA_ICON_RANGE)) [[unlikely]]
    {
      return false;
    }
  }

  {
    ImFontConfig cfg;
    cfg.MergeMode = true;
    cfg.PixelSnapH = true;
    cfg.GlyphMinAdvanceX = size;
    cfg.GlyphMaxAdvanceX = size;
    cfg.FontDataOwnedByAtlas = false;

    if (!ImGui::GetIO().Fonts->AddFontFromMemoryTTF(s_state.icon_pf_font_data.data(),
                                                    static_cast<int>(s_state.icon_pf_font_data.size()), size * 1.2f,
                                                    &cfg, PF_ICON_RANGE)) [[unlikely]]
    {
      return false;
    }
  }

  {
    ImFontConfig cfg;
    cfg.MergeMode = true;
    cfg.PixelSnapH = true;
    cfg.GlyphMinAdvanceX = size;
    cfg.GlyphMaxAdvanceX = size;
    cfg.FontDataOwnedByAtlas = false;
    cfg.FontBuilderFlags = ImGuiFreeTypeBuilderFlags_LoadColor | ImGuiFreeTypeBuilderFlags_Bitmap;

    if (!ImGui::GetIO().Fonts->AddFontFromMemoryTTF(s_state.emoji_font_data.data(),
                                                    static_cast<int>(s_state.emoji_font_data.size()), size * 0.9f, &cfg,
                                                    emoji_range)) [[unlikely]]
    {
      return false;
    }
  }

  return true;
}

bool ImGuiManager::AddImGuiFonts(bool debug_font, bool fullscreen_fonts)
{
  const float window_scale =
    (g_gpu_device && g_gpu_device->HasMainSwapChain()) ? g_gpu_device->GetMainSwapChain()->GetScale() : 1.0f;
  const float debug_font_size = std::ceil(15.0f * window_scale);
  const float standard_font_size = std::ceil(15.0f * s_state.global_scale);
  const float osd_font_size = std::ceil(17.0f * s_state.global_scale);

  INFO_LOG("Allocating fonts winscale={} globalscale={} debug={} fullscreen={}", window_scale, s_state.global_scale,
           debug_font, fullscreen_fonts);

  // need to generate arrays if dynamic ranges are present
  const ImWchar* text_range = s_state.font_range.empty() ? DEFAULT_FONT_RANGE.data() : s_state.font_range.data();
  const ImWchar* emoji_range = EMOJI_ICON_RANGE;
  std::vector<ImWchar> full_text_range, full_emoji_range;
  if (!s_state.dynamic_font_range.empty())
  {
    // skip the zeros, we'll add them afterwards
    const size_t base_size = s_state.font_range.empty() ? DEFAULT_FONT_RANGE.size() : s_state.font_range.size();
    Assert(base_size > 2);
    full_text_range.reserve(base_size + s_state.dynamic_font_range.size());
    full_text_range.insert(full_text_range.end(), &text_range[0], &text_range[base_size - 2]);
    full_text_range.insert(full_text_range.end(), s_state.dynamic_font_range.begin(), s_state.dynamic_font_range.end());
    full_text_range.insert(full_text_range.end(), 2, 0);
    text_range = full_text_range.data();
  }
  if (!s_state.dynamic_emoji_range.empty())
  {
    // skip the zeros, we'll add them afterwards
    size_t base_size = 0;
    for (const ImWchar* c = EMOJI_ICON_RANGE; *c != 0; c++)
      base_size++;

    Assert(base_size > 2);
    full_emoji_range.reserve(base_size + s_state.dynamic_emoji_range.size());
    full_emoji_range.insert(full_emoji_range.end(), &EMOJI_ICON_RANGE[0], &EMOJI_ICON_RANGE[base_size]);
    full_emoji_range.insert(full_emoji_range.end(), s_state.dynamic_emoji_range.begin(),
                            s_state.dynamic_emoji_range.end());
    full_emoji_range.insert(full_emoji_range.end(), 2, 0);
    emoji_range = full_emoji_range.data();
  }

  ImGuiIO& io = ImGui::GetIO();
  io.Fonts->Clear();

  if (debug_font)
  {
    s_state.debug_font = AddTextFont(debug_font_size, ASCII_FONT_RANGE.data());
    if (!s_state.debug_font)
      return false;
  }

  s_state.fixed_font = AddFixedFont(standard_font_size);
  if (!s_state.fixed_font)
    return false;

  s_state.osd_font = AddTextFont(osd_font_size, text_range);
  if (!s_state.osd_font || !AddIconFonts(osd_font_size, emoji_range))
    return false;
  if (!debug_font)
    s_state.debug_font = s_state.osd_font;

  if (fullscreen_fonts)
  {
    const float medium_font_size = ImGuiFullscreen::LayoutScale(ImGuiFullscreen::LAYOUT_MEDIUM_FONT_SIZE);
    s_state.medium_font = AddTextFont(medium_font_size, text_range);
    if (!s_state.medium_font || !AddIconFonts(medium_font_size, emoji_range))
      return false;

    const float large_font_size = ImGuiFullscreen::LayoutScale(ImGuiFullscreen::LAYOUT_LARGE_FONT_SIZE);
    s_state.large_font = AddTextFont(large_font_size, text_range);
    if (!s_state.large_font || !AddIconFonts(large_font_size, emoji_range))
      return false;
  }
  else
  {
    s_state.medium_font = nullptr;
    s_state.large_font = nullptr;
  }

  ImGuiFullscreen::SetFonts(s_state.medium_font, s_state.large_font);

  return io.Fonts->Build();
}

void ImGuiManager::ReloadFontDataIfActive()
{
  if (!s_state.imgui_context)
    return;

  ImGui::EndFrame();

  if (!LoadFontData(nullptr))
  {
    GPUThread::ReportFatalErrorAndShutdown("Failed to load font data");
    return;
  }

  if (!AddImGuiFonts(HasDebugFont(), HasFullscreenFonts()))
  {
    GPUThread::ReportFatalErrorAndShutdown("Failed to create ImGui font text");
    return;
  }

  UpdateImGuiFontTexture();
  NewFrame();
}

bool ImGuiManager::AddFullscreenFontsIfMissing()
{
  if (HasFullscreenFonts())
    return true;

  // can't do this in the middle of a frame
  ImGui::EndFrame();

  const bool debug_font = HasDebugFont();
  if (!AddImGuiFonts(debug_font, true))
  {
    GPUThread::ReportFatalErrorAndShutdown("Failed to lazily allocate fullscreen fonts.");
    AddImGuiFonts(debug_font, false);
  }

  UpdateImGuiFontTexture();
  NewFrame();

  return HasFullscreenFonts();
}

bool ImGuiManager::HasDebugFont()
{
  return (s_state.debug_font != s_state.osd_font);
}

bool ImGuiManager::AddDebugFontIfMissing()
{
  if (HasDebugFont())
    return true;

  // can't do this in the middle of a frame
  ImGui::EndFrame();

  const bool fullscreen_font = HasFullscreenFonts();
  if (!AddImGuiFonts(true, fullscreen_font))
  {
    ERROR_LOG("Failed to lazily allocate fullscreen fonts.");
    AddImGuiFonts(true, fullscreen_font);
  }

  UpdateImGuiFontTexture();
  NewFrame();

  return HasDebugFont();
}

bool ImGuiManager::HasFullscreenFonts()
{
  return (s_state.medium_font && s_state.large_font);
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

  std::unique_lock<std::mutex> lock(s_state.osd_messages_lock);
  s_state.osd_posted_messages.push_back(std::move(msg));
}

void ImGuiManager::RemoveKeyedOSDMessage(std::string key, bool is_warning)
{
  ImGuiManager::OSDMessage msg = {};
  msg.key = std::move(key);
  msg.duration = 0.0f;
  msg.is_warning = is_warning;

  std::unique_lock<std::mutex> lock(s_state.osd_messages_lock);
  s_state.osd_posted_messages.push_back(std::move(msg));
}

void ImGuiManager::ClearOSDMessages(bool clear_warnings)
{
  {
    std::unique_lock<std::mutex> lock(s_state.osd_messages_lock);
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
  using ImGuiFullscreen::ModAlpha;
  using ImGuiFullscreen::RenderShadowedTextClipped;
  using ImGuiFullscreen::UIStyle;

  static constexpr float MOVE_DURATION = 0.5f;

  ImFont* const font = s_state.osd_font;
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

    const ImVec2 text_size = font->CalcTextSizeA(font->FontSize, max_width, max_width, IMSTR_START_END(msg.text));
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
    RenderShadowedTextClipped(dl, font, text_rect.Min, text_rect.Max,
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

ImFont* ImGuiManager::GetDebugFont()
{
  return s_state.debug_font;
}

ImFont* ImGuiManager::GetOSDFont()
{
  return s_state.osd_font;
}

ImFont* ImGuiManager::GetFixedFont()
{
  return s_state.fixed_font;
}

ImFont* ImGuiManager::GetMediumFont()
{
  AddFullscreenFontsIfMissing();
  return s_state.medium_font;
}

ImFont* ImGuiManager::GetLargeFont()
{
  AddFullscreenFontsIfMissing();
  return s_state.large_font;
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

void ImGuiManager::SetCommonIOOptions(ImGuiIO& io)
{
  io.KeyRepeatDelay = 0.5f;
  io.GetClipboardTextFn = GetClipboardTextImpl;
  io.SetClipboardTextFn = SetClipboardTextImpl;
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

  const auto iter = s_state.imgui_key_map.find(key.data);
  if (iter == s_state.imgui_key_map.end())
    return false;

  GPUThread::RunOnThread([imkey = iter->second, pressed = (value != 0.0f)]() {
    if (!s_state.imgui_context)
      return;

    SetImKeyState(s_state.imgui_context->IO, imkey, pressed);
  });

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

const char* ImGuiManager::GetClipboardTextImpl(void* userdata)
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

void ImGuiManager::SetClipboardTextImpl(void* userdata, const char* text)
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

  AddDebugFontIfMissing();

  state->imgui_context = ImGui::CreateContext(s_state.imgui_context->IO.Fonts);
  state->imgui_context->Viewports[0]->Size = state->imgui_context->IO.DisplaySize =
    ImVec2(static_cast<float>(state->swap_chain->GetWidth()), static_cast<float>(state->swap_chain->GetHeight()));
  state->imgui_context->IO.IniFilename = nullptr;
  state->imgui_context->IO.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
  state->imgui_context->IO.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
  SetCommonIOOptions(state->imgui_context->IO);

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

  ImGui::NewFrame();
  ImGui::PushFont(s_state.debug_font);
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(state->imgui_context->IO.DisplaySize, ImGuiCond_Always);
  if (ImGui::Begin("AuxRenderWindowMain", nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse))
  {
    draw_callback(state->swap_chain->GetScale());
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
      const auto iter = s_state.imgui_key_map.find(param1.uint_param);
      if (iter != s_state.imgui_key_map.end())
        SetImKeyState(io, iter->second, (event == Host::AuxiliaryRenderWindowEvent::KeyPressed));
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
