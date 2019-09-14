#pragma once
#include "types.h"
#include <memory>

namespace GL {
class Texture;
}

class System;

class HostInterface
{
public:
  HostInterface();
  virtual ~HostInterface();

  virtual void SetDisplayTexture(GL::Texture* texture, u32 offset_x, u32 offset_y, u32 width, u32 height) = 0;
  virtual void ReportMessage(const char* message) = 0;

  // Adds OSD messages, duration is in seconds.
  virtual void AddOSDMessage(const char* message, float duration = 2.0f) = 0;

protected:
  bool LoadState(const char* filename);
  bool SaveState(const char* filename);

  std::unique_ptr<System> m_system;
  bool m_running = false;
};
