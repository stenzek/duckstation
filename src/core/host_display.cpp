#include "host_display.h"

HostDisplayTexture::~HostDisplayTexture() = default;

HostDisplay::~HostDisplay() = default;

std::tuple<int, int, int, int> HostDisplay::CalculateDrawRect(int window_width, int window_height, float display_ratio)
{
  const float window_ratio = float(window_width) / float(window_height);
  int left, top, width, height;
  if (window_ratio >= display_ratio)
  {
    width = static_cast<int>(float(window_height) * display_ratio);
    height = static_cast<int>(window_height);
    left = (window_width - width) / 2;
    top = 0;
  }
  else
  {
    width = static_cast<int>(window_width);
    height = static_cast<int>(float(window_width) / display_ratio);
    left = 0;
    top = (window_height - height) / 2;
  }

  return std::tie(left, top, width, height);
}
