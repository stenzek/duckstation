#include "libretro_host_display.h"
#include "common/assert.h"
#include "common/log.h"
#include "libretro.h"
#include "libretro_host_interface.h"
#include <array>
#include <tuple>
Log_SetChannel(LibretroHostDisplay);

class LibretroDisplayTexture : public HostDisplayTexture
{
public:
  LibretroDisplayTexture(u32 width, u32 height) : m_width(width), m_height(height), m_data(width * height) {}
  ~LibretroDisplayTexture() override = default;

  void* GetHandle() const override { return const_cast<LibretroDisplayTexture*>(this); }
  u32 GetWidth() const override { return m_width; }
  u32 GetHeight() const override { return m_height; }

  const u32* GetData() const { return m_data.data(); }
  u32 GetDataPitch() const { return m_width * sizeof(u32); }

  static void SwapAndCopy(void* dst, const void* src, u32 count)
  {
    // RGBA -> BGRX conversion
    u8* dst_ptr = static_cast<u8*>(dst);
    const u8* src_ptr = static_cast<const u8*>(src);

    for (u32 i = 0; i < count; i++)
    {
      u32 sval;
      std::memcpy(&sval, src_ptr, sizeof(sval));
      src_ptr += sizeof(sval);
      const u32 dval = (sval & 0xFF00FF00u) | ((sval & 0xFF) << 16) | ((sval >> 16) & 0xFFu);
      std::memcpy(dst_ptr, &dval, sizeof(dval));
      dst_ptr += sizeof(dval);
    }
  }

  void Read(u32 x, u32 y, u32 width, u32 height, void* data, u32 data_stride) const
  {
    u8* data_ptr = static_cast<u8*>(data);
    const u32* in_ptr = m_data.data() + y * m_width + x;
    for (u32 i = 0; i < height; i++)
    {
      SwapAndCopy(data_ptr, in_ptr, width);
      data_ptr += data_stride;
      in_ptr += m_width;
    }
  }

  void Write(u32 x, u32 y, u32 width, u32 height, const void* data, u32 data_stride)
  {
    const u8* data_ptr = static_cast<const u8*>(data);
    u32* out_ptr = m_data.data() + y * m_width + x;
    for (u32 i = 0; i < height; i++)
    {
      SwapAndCopy(out_ptr, data_ptr, width);
      data_ptr += data_stride;
      out_ptr += m_width;
    }
  }

  static std::unique_ptr<LibretroDisplayTexture> Create(u32 width, u32 height, const void* initial_data,
                                                        u32 initial_data_stride)
  {
    std::unique_ptr<LibretroDisplayTexture> tex = std::make_unique<LibretroDisplayTexture>(width, height);
    if (initial_data)
      tex->Write(0, 0, width, height, initial_data, initial_data_stride);

    return tex;
  }

private:
  u32 m_width;
  u32 m_height;
  std::vector<u32> m_data;
};

LibretroHostDisplay::LibretroHostDisplay()
{
  // switch to a 32-bit buffer
  retro_pixel_format pf = RETRO_PIXEL_FORMAT_XRGB8888;
  if (!g_retro_environment_callback(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &pf))
    Log_ErrorPrint("Failed to set pixel format to XRGB8888");
}

LibretroHostDisplay::~LibretroHostDisplay() = default;

HostDisplay::RenderAPI LibretroHostDisplay::GetRenderAPI() const
{
  return RenderAPI::None;
}

void* LibretroHostDisplay::GetRenderDevice() const
{
  return nullptr;
}

void* LibretroHostDisplay::GetRenderContext() const
{
  return nullptr;
}

bool LibretroHostDisplay::HasRenderDevice() const
{
  return true;
}

bool LibretroHostDisplay::HasRenderSurface() const
{
  return true;
}

bool LibretroHostDisplay::CreateRenderDevice(const WindowInfo& wi, std::string_view adapter_name, bool debug_device)
{
  m_window_info = wi;
  return true;
}

bool LibretroHostDisplay::InitializeRenderDevice(std::string_view shader_cache_directory, bool debug_device)
{
  return true;
}

bool LibretroHostDisplay::MakeRenderContextCurrent()
{
  return true;
}

bool LibretroHostDisplay::DoneRenderContextCurrent()
{
  return true;
}

void LibretroHostDisplay::DestroyRenderDevice() {}

void LibretroHostDisplay::DestroyRenderSurface() {}

bool LibretroHostDisplay::CreateResources()
{
  return true;
}

void LibretroHostDisplay::DestroyResources() {}

bool LibretroHostDisplay::ChangeRenderWindow(const WindowInfo& wi)
{
  m_window_info = wi;
  return true;
}

void LibretroHostDisplay::ResizeRenderWindow(s32 new_window_width, s32 new_window_height)
{
  m_window_info.surface_width = new_window_width;
  m_window_info.surface_height = new_window_height;
}

bool LibretroHostDisplay::SupportsFullscreen() const
{
  return false;
}

bool LibretroHostDisplay::IsFullscreen()
{
  return false;
}

bool LibretroHostDisplay::SetFullscreen(bool fullscreen, u32 width, u32 height, float refresh_rate)
{
  return false;
}

bool LibretroHostDisplay::SetPostProcessingChain(const std::string_view& config)
{
  return false;
}

std::unique_ptr<HostDisplayTexture> LibretroHostDisplay::CreateTexture(u32 width, u32 height, const void* data,
                                                                       u32 data_stride, bool dynamic)
{
  return LibretroDisplayTexture::Create(width, height, data, data_stride);
}

void LibretroHostDisplay::UpdateTexture(HostDisplayTexture* texture, u32 x, u32 y, u32 width, u32 height,
                                        const void* data, u32 data_stride)
{
  static_cast<LibretroDisplayTexture*>(texture)->Write(x, y, width, height, data, data_stride);
}

bool LibretroHostDisplay::DownloadTexture(const void* texture_handle, u32 x, u32 y, u32 width, u32 height,
                                          void* out_data, u32 out_data_stride)
{
  static_cast<const LibretroDisplayTexture*>(texture_handle)->Read(x, y, width, height, out_data, out_data_stride);
  return true;
}

void LibretroHostDisplay::SetVSync(bool enabled)
{
  // The libretro frontend controls this.
  Log_DevPrintf("Ignoring SetVSync(%u)", BoolToUInt32(enabled));
}

bool LibretroHostDisplay::Render()
{
  if (HasDisplayTexture())
  {
    const LibretroDisplayTexture* tex = static_cast<const LibretroDisplayTexture*>(m_display_texture_handle);
    g_retro_video_refresh_callback(tex->GetData() + m_display_texture_view_y * tex->GetWidth() +
                                     m_display_texture_view_x,
                                   m_display_texture_view_width, m_display_texture_view_height, tex->GetDataPitch());
  }

  return true;
}
