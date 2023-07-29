// SPDX-FileCopyrightText: 2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#define IMGUI_DEFINE_MATH_OPERATORS

#include "IconsFontAwesome5.h"
#include "common/align.h"
#include "common/assert.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/string_util.h"
#include "common/timer.h"
#include "common_host.h"
#include "core/controller.h"
#include "core/gpu.h"
#include "core/host.h"
#include "core/host_display.h"
#include "core/host_settings.h"
#include "core/netplay.h"
#include "core/settings.h"
#include "core/spu.h"
#include "core/system.h"
#include "fmt/chrono.h"
#include "fmt/format.h"
#include "fullscreen_ui.h"
#include "gsl/span"
#include "icon.h"
#include "imgui.h"
#include "imgui_fullscreen.h"
#include "imgui_internal.h"
#include "imgui_manager.h"
#include "imgui_overlays.h"
#include "imgui_stdlib.h"
#include "input_manager.h"
#include "util/audio_stream.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
#include <unordered_map>

#if defined(CPU_X64)
#include <emmintrin.h>
#elif defined(CPU_AARCH64)
#ifdef _MSC_VER
#include <arm64_neon.h>
#else
#include <arm_neon.h>
#endif
#endif

Log_SetChannel(ImGuiManager);

namespace ImGuiManager {
static void DrawNetplayMessages();
static void DrawNetplayStats();
static void DrawNetplayChatDialog();
} // namespace ImGuiManager

static std::deque<std::pair<std::string, Common::Timer::Value>> s_netplay_messages;
static constexpr u32 MAX_NETPLAY_MESSAGES = 15;
static constexpr float NETPLAY_MESSAGE_DURATION = 15.0f;
static constexpr float NETPLAY_MESSAGE_FADE_TIME = 2.0f;
static bool s_netplay_chat_dialog_open = false;
static bool s_netplay_chat_dialog_opening = false;
static std::string s_netplay_chat_message;

void Host::OnNetplayMessage(std::string message)
{
  Log_InfoPrintf("Netplay: %s", message.c_str());

  while (s_netplay_messages.size() >= MAX_NETPLAY_MESSAGES)
    s_netplay_messages.pop_front();

  s_netplay_messages.emplace_back(std::move(message), Common::Timer::GetCurrentValue() +
                                                        Common::Timer::ConvertSecondsToValue(NETPLAY_MESSAGE_DURATION));
}

void Host::ClearNetplayMessages()
{
  while (s_netplay_messages.size() > 0)
    s_netplay_messages.pop_front();
}

void ImGuiManager::RenderNetplayOverlays()
{
  DrawNetplayMessages();
  DrawNetplayStats();
  DrawNetplayChatDialog();
}

void ImGuiManager::DrawNetplayMessages()
{
  if (s_netplay_messages.empty())
    return;

  const Common::Timer::Value ticks = Common::Timer::GetCurrentValue();
  const ImGuiIO& io = ImGui::GetIO();
  const float scale = ImGuiManager::GetGlobalScale();
  const float shadow_offset = 1.0f * scale;
  const float margin = 10.0f * scale;
  const float spacing = 5.0f * scale;
  const float msg_spacing = 2.0f * scale;
  ImFont* font = ImGuiManager::GetFixedFont();
  float position_y = io.DisplaySize.y - margin - (100.0f * scale) - font->FontSize - spacing;
  ImDrawList* dl = ImGui::GetBackgroundDrawList();

  // drop expired messages.. because of the reverse iteration below, we can't do it in there :/
  for (auto iter = s_netplay_messages.begin(); iter != s_netplay_messages.end();)
  {
    if (ticks >= iter->second)
      iter = s_netplay_messages.erase(iter);
    else
      ++iter;
  }

  for (auto iter = s_netplay_messages.rbegin(); iter != s_netplay_messages.rend(); ++iter)
  {
    const float remainder = static_cast<float>(Common::Timer::ConvertValueToSeconds(iter->second - ticks));
    const float opacity = std::min(remainder / NETPLAY_MESSAGE_FADE_TIME, 1.0f);
    const u32 alpha = static_cast<u32>(opacity * 255.0f);
    const u32 shadow_alpha = static_cast<u32>(opacity * 100.0f);

    // TODO: line wrapping..
    const char* text_start = iter->first.c_str();
    const char* text_end = text_start + iter->first.length();
    const ImVec2 text_size = font->CalcTextSizeA(font->FontSize, io.DisplaySize.x, 0.0f, text_start, text_end, nullptr);

    dl->AddText(font, font->FontSize, ImVec2(margin + shadow_offset, position_y + shadow_offset),
                IM_COL32(0, 0, 0, shadow_alpha), text_start, text_end);
    dl->AddText(font, font->FontSize, ImVec2(margin, position_y), IM_COL32(255, 255, 255, alpha), text_start, text_end);

    position_y -= text_size.y + msg_spacing;
  }
}

void ImGuiManager::DrawNetplayStats()
{
  // Not much yet.. eventually we'll render chat and such here too.
  // We'll probably want to draw a graph too..

  LargeString text;
  text.AppendFmtString("Ping: {}\n", Netplay::GetPing());

  // temporary show the hostcode here for now
  auto hostcode = Netplay::GetHostCode();
  if (!hostcode.empty())
    text.AppendFmtString("Host Code: {}", hostcode);

  const float scale = ImGuiManager::GetGlobalScale();
  const float shadow_offset = 1.0f * scale;
  const float margin = 10.0f * scale;
  ImFont* font = ImGuiManager::GetFixedFont();
  const float position_y = ImGui::GetIO().DisplaySize.y - margin - (100.0f * scale);

  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  dl->AddText(font, font->FontSize, ImVec2(margin + shadow_offset, position_y + shadow_offset), IM_COL32(0, 0, 0, 100),
              text, text.GetCharArray() + text.GetLength());
  dl->AddText(font, font->FontSize, ImVec2(margin, position_y), IM_COL32(255, 255, 255, 255), text,
              text.GetCharArray() + text.GetLength());
}

void ImGuiManager::DrawNetplayChatDialog()
{
  // TODO: This needs to block controller input...

  if (s_netplay_chat_dialog_opening)
  {
    ImGui::OpenPopup("Netplay Chat");
    s_netplay_chat_dialog_open = true;
    s_netplay_chat_dialog_opening = false;
  }
  else if (!s_netplay_chat_dialog_open)
  {
    return;
  }

  const bool send_message = ImGui::IsKeyPressed(ImGuiKey_Enter);
  const bool close_chat =
    send_message || (s_netplay_chat_message.empty() && (ImGui::IsKeyPressed(ImGuiKey_Backspace)) ||
                     ImGui::IsKeyPressed(ImGuiKey_Escape));

  // sending netplay message
  if (send_message && !s_netplay_chat_message.empty())
    Netplay::SendChatMessage(s_netplay_chat_message);

  const ImGuiIO& io = ImGui::GetIO();
  const ImGuiStyle& style = ImGui::GetStyle();
  const float scale = ImGuiManager::GetGlobalScale();
  const float width = 600.0f * scale;
  const float height = 60.0f * scale;

  ImGui::SetNextWindowSize(ImVec2(width, height));
  ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowFocus();

  if (ImGui::BeginPopupModal("Netplay Chat", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize))
  {
    ImGui::SetNextItemWidth(width - style.WindowPadding.x * 2.0f);
    ImGui::SetKeyboardFocusHere();
    ImGui::InputText("##chatmsg", &s_netplay_chat_message);

    if (!s_netplay_chat_dialog_open)
      ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
  }

  if (close_chat)
  {
    s_netplay_chat_message.clear();
    s_netplay_chat_dialog_open = false;
  }

  s_netplay_chat_dialog_opening = false;
}

void ImGuiManager::OpenNetplayChat()
{
  if (s_netplay_chat_dialog_open)
    return;

  s_netplay_chat_dialog_opening = true;
}
