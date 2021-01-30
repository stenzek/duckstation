#pragma once
#include "core/types.h"
#include <array>
#include <optional>
#include <xf86drm.h>
#include <xf86drmMode.h>

class DRMDisplay
{
public:
  DRMDisplay(int card = -1);
  ~DRMDisplay();

  bool Initialize();

  int GetCardID() const { return m_card_id; }
  int GetCardFD() const { return m_card_fd; }
  u32 GetWidth() const { return m_mode->hdisplay; }
  u32 GetHeight() const { return m_mode->vdisplay; }

  std::optional<u32> AddBuffer(u32 width, u32 height, u32 format, u32 handle, u32 pitch, u32 offset);
  void RemoveBuffer(u32 fb_id);
  void PresentBuffer(u32 fb_id, bool wait_for_vsync);

private:
  enum : u32
  {
    MAX_BUFFERS = 5
  };

  bool TryOpeningCard(int card);

  int m_card_id = 0;
  int m_card_fd = -1;
  u32 m_crtc_id = 0;
  drmModeConnector* m_connector = nullptr;
  drmModeModeInfo* m_mode = nullptr;
};
