#pragma once
#include "types.h"
#include <array>
#include <optional>
#include <xf86drm.h>
#include <xf86drmMode.h>

class DRMDisplay
{
public:
  DRMDisplay(int card = -1);
  ~DRMDisplay();

  static bool GetCurrentMode(u32* width, u32* height, float* refresh_rate, int card = -1, int connector = -1);

  bool Initialize(u32 width, u32 height, float refresh_rate);

  /// Restores the buffer saved at startup.
  void RestoreBuffer();

  int GetCardID() const { return m_card_id; }
  int GetCardFD() const { return m_card_fd; }
  u32 GetWidth() const { return m_mode->hdisplay; }
  u32 GetHeight() const { return m_mode->vdisplay; }
  float GetRefreshRate() const
  {
    return (static_cast<float>(m_mode->clock) * 1000.0f) /
           (static_cast<float>(m_mode->htotal) * static_cast<float>(m_mode->vtotal));
  }

  u32 GetModeCount() const { return m_connector->count_modes; }
  u32 GetModeWidth(u32 i) const { return m_connector->modes[i].hdisplay; }
  u32 GetModeHeight(u32 i) const { return m_connector->modes[i].vdisplay; }
  float GetModeRefreshRate(u32 i) const
  {
    return (static_cast<float>(m_connector->modes[i].clock) * 1000.0f) /
           (static_cast<float>(m_connector->modes[i].htotal) * static_cast<float>(m_connector->modes[i].vtotal));
  }

  std::optional<u32> AddBuffer(u32 width, u32 height, u32 format, u32 handle, u32 pitch, u32 offset);
  void RemoveBuffer(u32 fb_id);
  void PresentBuffer(u32 fb_id, bool wait_for_vsync);

private:
  enum : u32
  {
    MAX_BUFFERS = 5
  };

  bool TryOpeningCard(int card, u32 width, u32 height, float refresh_rate);

  int m_card_id = 0;
  int m_card_fd = -1;
  u32 m_crtc_id = 0;

  drmModeRes* m_resources = nullptr;
  drmModeConnector* m_connector = nullptr;
  drmModeModeInfo* m_mode = nullptr;

  drmModeCrtc* m_prev_crtc = nullptr;
};
