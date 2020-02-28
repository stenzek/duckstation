#include "host_display.h"

HostDisplayTexture::~HostDisplayTexture() = default;

HostDisplay::~HostDisplay() = default;

void HostDisplay::WindowResized(s32 new_window_width, s32 new_window_height)
{
  m_window_width = new_window_width;
  m_window_height = new_window_height;
}

std::tuple<s32, s32, s32, s32> HostDisplay::CalculateDrawRect() const
{
  const s32 window_width = m_window_width;
  const s32 window_height = m_window_height - m_display_top_margin;
  const float window_ratio = static_cast<float>(window_width) / static_cast<float>(window_height);

  float scale;
  int left, top, width, height;
  if (window_ratio >= m_display_aspect_ratio)
  {
    width = static_cast<int>(static_cast<float>(window_height) * m_display_aspect_ratio);
    height = static_cast<int>(window_height);
    scale = static_cast<float>(window_height) / static_cast<float>(m_display_height);
    left = (window_width - width) / 2;
    top = 0;
  }
  else
  {
    width = static_cast<int>(window_width);
    height = static_cast<int>(float(window_width) / m_display_aspect_ratio);
    scale = static_cast<float>(window_width) / static_cast<float>(m_display_width);
    left = 0;
    top = (window_height - height) / 2;
  }

  // add in padding
  left += static_cast<s32>(static_cast<float>(m_display_area.left) * scale);
  top += static_cast<s32>(static_cast<float>(m_display_area.top) * scale);
  width -= static_cast<s32>(static_cast<float>(m_display_area.left + (m_display_width - m_display_area.right)) * scale);
  height -=
    static_cast<s32>(static_cast<float>(m_display_area.top + (m_display_height - m_display_area.bottom)) * scale);

  // add in margin
  top += m_display_top_margin;
  return std::tie(left, top, width, height);
}
