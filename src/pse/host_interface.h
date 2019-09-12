#pragma once
#include "types.h"

namespace GL {
class Texture;
}

class HostInterface
{
public:
  virtual void SetDisplayTexture(GL::Texture* texture, u32 offset_x, u32 offset_y, u32 width, u32 height) = 0;
  virtual void ReportMessage(const char* message) = 0;

  // Adds OSD messages, duration is in seconds.
  virtual void AddOSDMessage(const char* message, float duration = 2.0f) = 0;
};
