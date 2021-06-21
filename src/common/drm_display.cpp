#include "drm_display.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/scope_guard.h"
#include "common/string.h"
#include "file_system.h"
#include <cmath>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
Log_SetChannel(DRMDisplay);

enum
{
  MAX_CARDS_TO_TRY = 10
};

DRMDisplay::DRMDisplay(int card /*= 1*/) : m_card_id(card) {}

DRMDisplay::~DRMDisplay()
{
  // restore original buffer
  if (m_prev_crtc)
    RestoreBuffer();

  if (m_connector)
    drmModeFreeConnector(m_connector);

  if (m_card_fd >= 0)
    close(m_card_fd);
}

// https://gist.github.com/Miouyouyou/89e9fe56a2c59bce7d4a18a858f389ef

static uint32_t find_crtc_for_encoder(const drmModeRes* resources, const drmModeEncoder* encoder)
{
  int i;

  for (i = 0; i < resources->count_crtcs; i++)
  {
    /* possible_crtcs is a bitmask as described here:
     * https://dvdhrm.wordpress.com/2012/09/13/linux-drm-mode-setting-api
     */
    const uint32_t crtc_mask = 1 << i;
    const uint32_t crtc_id = resources->crtcs[i];
    if (encoder->possible_crtcs & crtc_mask)
    {
      return crtc_id;
    }
  }

  /* no match found */
  return -1;
}

static uint32_t find_crtc_for_connector(int card_fd, const drmModeRes* resources, const drmModeConnector* connector)
{
  int i;

  for (i = 0; i < connector->count_encoders; i++)
  {
    const uint32_t encoder_id = connector->encoders[i];
    drmModeEncoder* encoder = drmModeGetEncoder(card_fd, encoder_id);

    if (encoder)
    {
      const uint32_t crtc_id = find_crtc_for_encoder(resources, encoder);

      drmModeFreeEncoder(encoder);
      if (crtc_id != 0)
      {
        return crtc_id;
      }
    }
  }

  /* no match found */
  return -1;
}

bool DRMDisplay::Initialize(u32 width, u32 height, float refresh_rate)
{
  if (m_card_id < 0)
  {
    for (int i = 0; i < MAX_CARDS_TO_TRY; i++)
    {
      if (TryOpeningCard(i, width, height, refresh_rate))
        return true;
    }

    return false;
  }

  return TryOpeningCard(m_card_id, width, height, refresh_rate);
}

void DRMDisplay::RestoreBuffer()
{
  if (m_prev_crtc)
  {
    u32 connector_id = m_connector->connector_id;
    drmModeSetCrtc(m_card_fd, m_prev_crtc->crtc_id, m_prev_crtc->buffer_id, m_prev_crtc->x, m_prev_crtc->y,
                   &connector_id, 1, &m_prev_crtc->mode);
    drmModeFreeCrtc(m_prev_crtc);
    m_prev_crtc = nullptr;
  }
}

bool DRMDisplay::TryOpeningCard(int card, u32 width, u32 height, float refresh_rate)
{
  if (m_card_fd >= 0)
    close(m_card_fd);

  m_card_fd = open(TinyString::FromFormat("/dev/dri/card%d", card), O_RDWR);
  if (m_card_fd < 0)
  {
    Log_ErrorPrintf("open(/dev/dri/card%d) failed: %d (%s)", card, errno, strerror(errno));
    return false;
  }

  drmModeRes* resources = drmModeGetResources(m_card_fd);
  if (!resources)
  {
    Log_ErrorPrintf("drmModeGetResources() failed: %d (%s)", errno, strerror(errno));
    return false;
  }

  Assert(!m_connector);

  for (int i = 0; i < resources->count_connectors; i++)
  {
    drmModeConnector* next_connector = drmModeGetConnector(m_card_fd, resources->connectors[i]);
    if (next_connector->connection == DRM_MODE_CONNECTED)
    {
      m_connector = next_connector;
      break;
    }

    drmModeFreeConnector(next_connector);
  }

  if (!m_connector)
  {
    Log_ErrorPrintf("No connector found");
    drmModeFreeResources(resources);
    return false;
  }

  for (int i = 0; i < m_connector->count_modes; i++)
  {
    drmModeModeInfo* next_mode = &m_connector->modes[i];

    const float mode_refresh_rate = (static_cast<float>(next_mode->clock) * 1000.0f) /
                                    (static_cast<float>(next_mode->htotal) * static_cast<float>(next_mode->vtotal));
    Log_DevPrintf("Checking mode %u: %ux%u @ %f hz", i, next_mode->hdisplay, next_mode->vdisplay, mode_refresh_rate);

    if (width == 0 && height == 0)
    {
      // use preferred mode if we're auto selecting
      if (next_mode->type & DRM_MODE_TYPE_PREFERRED)
      {
        m_mode = next_mode;
        break;
      }
      else if (!m_mode)
      {
        m_mode = next_mode;
      }
    }
    else
    {
      if (width == next_mode->hdisplay && height == next_mode->vdisplay &&
          (refresh_rate == 0.0f || std::abs(mode_refresh_rate - refresh_rate) < 0.1f))
      {
        m_mode = next_mode;
        break;
      }
    }
  }

  if (!m_mode)
  {
    Log_ErrorPrintf("No mode found");
    drmModeFreeResources(resources);
    return false;
  }

  drmModeEncoder* encoder = nullptr;
  for (int i = 0; i < resources->count_encoders; i++)
  {
    drmModeEncoder* next_encoder = drmModeGetEncoder(m_card_fd, resources->encoders[i]);
    if (next_encoder->encoder_id == m_connector->encoder_id)
    {
      encoder = next_encoder;
      m_crtc_id = encoder->crtc_id;
      break;
    }

    drmModeFreeEncoder(next_encoder);
  }

  if (encoder)
  {
    drmModeFreeEncoder(encoder);
  }
  else
  {
    m_crtc_id = find_crtc_for_connector(m_card_fd, resources, m_connector);
    if (m_crtc_id == 0)
    {
      Log_ErrorPrintf("No CRTC found");
      drmModeFreeResources(resources);
      return false;
    }
  }

  drmModeFreeResources(resources);

  m_card_id = card;
  m_prev_crtc = drmModeGetCrtc(m_card_fd, m_crtc_id);
  return true;
}

std::optional<u32> DRMDisplay::AddBuffer(u32 width, u32 height, u32 format, u32 handle, u32 pitch, u32 offset)
{
  uint32_t bo_handles[4] = {handle, 0, 0, 0};
  uint32_t pitches[4] = {pitch, 0, 0, 0};
  uint32_t offsets[4] = {offset, 0, 0, 0};

  u32 fb_id;
  int res = drmModeAddFB2(m_card_fd, width, height, format, bo_handles, pitches, offsets, &fb_id, 0);
  if (res != 0)
  {
    Log_ErrorPrintf("drmModeAddFB2() failed: %d", res);
    return std::nullopt;
  }

  return fb_id;
}

void DRMDisplay::RemoveBuffer(u32 fb_id)
{
  drmModeRmFB(m_card_fd, fb_id);
}

void DRMDisplay::PresentBuffer(u32 fb_id, bool wait_for_vsync)
{
  if (!wait_for_vsync)
  {
    u32 connector_id = m_connector->connector_id;
    int res = drmModeSetCrtc(m_card_fd, m_crtc_id, fb_id, 0, 0, &connector_id, 1, m_mode);
    if (res != 0)
      Log_ErrorPrintf("drmModeSetCrtc() failed: %d", res);

    return;
  }

  bool waiting_for_flip = true;
  drmEventContext event_ctx = {};
  event_ctx.version = DRM_EVENT_CONTEXT_VERSION;
  event_ctx.page_flip_handler = [](int fd, unsigned int frame, unsigned int sec, unsigned int usec, void* data) {
    *reinterpret_cast<bool*>(data) = false;
  };

  int res = drmModePageFlip(m_card_fd, m_crtc_id, fb_id, DRM_MODE_PAGE_FLIP_EVENT, &waiting_for_flip);
  if (res != 0)
  {
    Log_ErrorPrintf("drmModePageFlip() failed: %d", res);
    return;
  }

  while (waiting_for_flip)
  {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(m_card_fd, &fds);
    int res = select(m_card_fd + 1, &fds, nullptr, nullptr, nullptr);
    if (res < 0)
    {
      Log_ErrorPrintf("select() failed: %d", errno);
      break;
    }
    else if (res == 0)
    {
      continue;
    }

    drmHandleEvent(m_card_fd, &event_ctx);
  }
}

bool DRMDisplay::GetCurrentMode(u32* width, u32* height, float* refresh_rate, int card, int connector)
{
  int card_fd = -1;
  if (card < 0)
  {
    for (int try_card = 0; try_card < MAX_CARDS_TO_TRY; try_card++)
    {
      card_fd = open(TinyString::FromFormat("/dev/dri/card%d", try_card), O_RDWR);
      if (card_fd >= 0)
        break;
    }
  }
  else
  {
    card_fd = open(TinyString::FromFormat("/dev/dri/card%d", card), O_RDWR);
  }

  if (card_fd < 0)
  {
    Log_ErrorPrintf("open(/dev/dri/card%d) failed: %d (%s)", card, errno, strerror(errno));
    return false;
  }

  Common::ScopeGuard card_guard([card_fd]() { close(card_fd); });

  drmModeRes* resources = drmModeGetResources(card_fd);
  if (!resources)
  {
    Log_ErrorPrintf("drmModeGetResources() failed: %d (%s)", errno, strerror(errno));
    return false;
  }

  Common::ScopeGuard resources_guard([resources]() { drmModeFreeResources(resources); });
  drmModeConnector* connector_ptr = nullptr;
  if (connector < 0)
  {
    for (int i = 0; i < resources->count_connectors; i++)
    {
      connector_ptr = drmModeGetConnector(card_fd, resources->connectors[i]);
      if (connector_ptr->connection == DRM_MODE_CONNECTED)
        break;

      drmModeFreeConnector(connector_ptr);
    }
  }
  else if (connector < resources->count_connectors)
  {
    connector_ptr = drmModeGetConnector(card_fd, resources->connectors[connector]);
  }

  Common::ScopeGuard connector_guard([connector_ptr]() {
    if (connector_ptr)
      drmModeFreeConnector(connector_ptr);
  });
  if (!connector_ptr || connector_ptr->connection != DRM_MODE_CONNECTED)
  {
    Log_ErrorPrintf("No connector found");
    return false;
  }

  drmModeEncoder* encoder = drmModeGetEncoder(card_fd, connector_ptr->encoder_id);
  if (!encoder)
  {
    Log_ErrorPrint("No encoder found");
    return false;
  }

  Common::ScopeGuard encoder_guard([encoder]() { drmModeFreeEncoder(encoder); });

  drmModeCrtc* crtc = drmModeGetCrtc(card_fd, encoder->crtc_id);
  if (!crtc)
  {
    Log_ErrorPrint("No CRTC found");
    return false;
  }

  if (!crtc->mode_valid)
  {
    Log_ErrorPrint("CRTC mode not valid");
    return false;
  }

  const u32 current_width = static_cast<u32>(crtc->mode.hdisplay);
  const u32 current_height = static_cast<u32>(crtc->mode.vdisplay);
  const float current_refresh_rate = (static_cast<float>(crtc->mode.clock) * 1000.0f) /
                                     (static_cast<float>(crtc->mode.htotal) * static_cast<float>(crtc->mode.vtotal));
  Log_InfoPrintf("Current mode for card %d: %ux%u@%f", card, current_width, current_height, current_refresh_rate);

  if (width)
    *width = current_width;
  if (height)
    *height = current_height;
  if (refresh_rate)
    *refresh_rate = current_refresh_rate;

  return true;
}