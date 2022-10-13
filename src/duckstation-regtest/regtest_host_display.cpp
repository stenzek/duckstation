#include "regtest_host_display.h"
#include "common/align.h"
#include "common/assert.h"
#include "common/file_system.h"
#include "common/image.h"
#include "common/log.h"
#include "common/string_util.h"
#include <array>
#include <tuple>
Log_SetChannel(RegTestHostDisplay);

RegTestHostDisplay::RegTestHostDisplay() = default;

RegTestHostDisplay::~RegTestHostDisplay() = default;

RenderAPI RegTestHostDisplay::GetRenderAPI() const
{
  return RenderAPI::None;
}

void* RegTestHostDisplay::GetRenderDevice() const
{
  return nullptr;
}

void* RegTestHostDisplay::GetRenderContext() const
{
  return nullptr;
}

bool RegTestHostDisplay::HasRenderDevice() const
{
  return true;
}

bool RegTestHostDisplay::HasRenderSurface() const
{
  return true;
}

bool RegTestHostDisplay::CreateRenderDevice(const WindowInfo& wi)
{
  m_window_info = wi;
  return true;
}

bool RegTestHostDisplay::InitializeRenderDevice()
{
  return true;
}

bool RegTestHostDisplay::MakeRenderContextCurrent()
{
  return true;
}

bool RegTestHostDisplay::DoneRenderContextCurrent()
{
  return true;
}

void RegTestHostDisplay::DestroyRenderSurface() {}

bool RegTestHostDisplay::CreateResources()
{
  return true;
}

void RegTestHostDisplay::DestroyResources() {}

HostDisplay::AdapterAndModeList RegTestHostDisplay::GetAdapterAndModeList()
{
  return {};
}

bool RegTestHostDisplay::CreateImGuiContext()
{
  return true;
}

void RegTestHostDisplay::DestroyImGuiContext()
{
  // noop
}

bool RegTestHostDisplay::UpdateImGuiFontTexture()
{
  // noop
  return true;
}

bool RegTestHostDisplay::ChangeRenderWindow(const WindowInfo& wi)
{
  m_window_info = wi;
  return true;
}

void RegTestHostDisplay::ResizeRenderWindow(s32 new_window_width, s32 new_window_height)
{
  m_window_info.surface_width = new_window_width;
  m_window_info.surface_height = new_window_height;
}

bool RegTestHostDisplay::SupportsFullscreen() const
{
  return false;
}

bool RegTestHostDisplay::IsFullscreen()
{
  return false;
}

bool RegTestHostDisplay::SetFullscreen(bool fullscreen, u32 width, u32 height, float refresh_rate)
{
  return false;
}

bool RegTestHostDisplay::SetPostProcessingChain(const std::string_view& config)
{
  return false;
}

std::unique_ptr<GPUTexture> RegTestHostDisplay::CreateTexture(u32 width, u32 height, u32 layers, u32 levels,
                                                              u32 samples, GPUTexture::Format format, const void* data,
                                                              u32 data_stride, bool dynamic /* = false */)
{
  std::unique_ptr<RegTestTexture> tex = std::make_unique<RegTestTexture>();
  if (!tex->Create(width, height, layers, levels, samples, format))
    return {};

  if (data && !tex->Upload(0, 0, width, height, data, data_stride))
    return {};

  return tex;
}

bool RegTestHostDisplay::BeginTextureUpdate(GPUTexture* texture, u32 width, u32 height, void** out_buffer,
                                            u32* out_pitch)
{
  return static_cast<RegTestTexture*>(texture)->BeginUpload(width, height, out_buffer, out_pitch);
}

void RegTestHostDisplay::EndTextureUpdate(GPUTexture* texture, u32 x, u32 y, u32 width, u32 height)
{
  static_cast<RegTestTexture*>(texture)->EndUpload(x, y, width, height);
}

bool RegTestHostDisplay::UpdateTexture(GPUTexture* texture, u32 x, u32 y, u32 width, u32 height, const void* data,
                                       u32 data_stride)
{
  return static_cast<RegTestTexture*>(texture)->Upload(x, y, width, height, data, data_stride);
}

bool RegTestHostDisplay::DownloadTexture(GPUTexture* texture, u32 x, u32 y, u32 width, u32 height, void* out_data,
                                         u32 out_data_stride)
{
  return static_cast<const RegTestTexture*>(texture)->Download(x, y, width, height, out_data, out_data_stride);
}

bool RegTestHostDisplay::SupportsTextureFormat(GPUTexture::Format format) const
{
  return (format == GPUTexture::Format::RGBA8);
}

void RegTestHostDisplay::SetVSync(bool enabled)
{
  Log_DevPrintf("Ignoring SetVSync(%u)", BoolToUInt32(enabled));
}

bool RegTestHostDisplay::Render(bool skip_present)
{
  return true;
}

bool RegTestHostDisplay::RenderScreenshot(u32 width, u32 height, std::vector<u32>* out_pixels, u32* out_stride,
                                          GPUTexture::Format* out_format)
{
  return false;
}

RegTestTexture::RegTestTexture() = default;

RegTestTexture::~RegTestTexture() = default;

bool RegTestTexture::IsValid() const
{
  return !m_frame_buffer.empty();
}

bool RegTestTexture::Create(u32 width, u32 height, u32 layers, u32 levels, u32 samples, GPUTexture::Format format)
{
  if (width == 0 || height == 0 || layers != 1 || levels != 1 || samples != 1 || format == GPUTexture::Format::Unknown)
    return false;

  m_width = static_cast<u16>(width);
  m_height = static_cast<u16>(height);
  m_layers = static_cast<u8>(layers);
  m_levels = static_cast<u8>(levels);
  m_samples = static_cast<u8>(samples);
  m_format = format;

  m_frame_buffer_pitch = width * GPUTexture::GetPixelSize(format);
  m_frame_buffer.resize(m_frame_buffer_pitch * height);
  return true;
}

bool RegTestTexture::Upload(u32 x, u32 y, u32 width, u32 height, const void* data, u32 data_stride)
{
  if ((static_cast<u64>(x) + width) > m_width || (static_cast<u64>(y) + height) > m_height)
    return false;

  const u32 ps = GetPixelSize(m_format);
  const u32 copy_size = width * ps;
  StringUtil::StrideMemCpy(&m_frame_buffer[y * m_frame_buffer_pitch + x * ps], m_frame_buffer_pitch, data, data_stride,
                           copy_size, height);
  return true;
}

bool RegTestTexture::Download(u32 x, u32 y, u32 width, u32 height, void* data, u32 data_stride) const
{
  if ((static_cast<u64>(x) + width) > m_width || (static_cast<u64>(y) + height) > m_height)
    return false;

  const u32 ps = GetPixelSize(m_format);
  const u32 copy_size = width * ps;
  StringUtil::StrideMemCpy(data, data_stride, &m_frame_buffer[y * m_frame_buffer_pitch + x * ps], m_frame_buffer_pitch,
                           copy_size, height);
  return true;
}

bool RegTestTexture::BeginUpload(u32 width, u32 height, void** out_buffer, u32* out_pitch)
{
  if (width > m_width || height > m_height)
    return false;

  const u32 pitch = GetPixelSize(m_format) * width;
  m_staging_buffer.resize(pitch * height);
  *out_buffer = m_staging_buffer.data();
  *out_pitch = pitch;
  return true;
}

void RegTestTexture::EndUpload(u32 x, u32 y, u32 width, u32 height)
{
  Assert((static_cast<u64>(x) + width) <= m_width && (static_cast<u64>(y) + height) <= m_height);

  const u32 ps = GetPixelSize(m_format);
  const u32 pitch = ps * width;
  StringUtil::StrideMemCpy(&m_frame_buffer[y * m_frame_buffer_pitch + x * ps], m_frame_buffer_pitch,
                           m_staging_buffer.data(), pitch, pitch, height);
}
