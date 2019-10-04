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

  bool InitializeSystem(const char* filename, const char* exp1_filename);

  virtual void SetDisplayTexture(GL::Texture* texture, u32 offset_x, u32 offset_y, u32 width, u32 height, float aspect_ratio) = 0;
  virtual void ReportMessage(const char* message) = 0;

  // Adds OSD messages, duration is in seconds.
  virtual void AddOSDMessage(const char* message, float duration = 2.0f) = 0;

  bool LoadState(const char* filename);
  bool SaveState(const char* filename);

protected:
  std::unique_ptr<System> m_system;
  bool m_running = false;
};
