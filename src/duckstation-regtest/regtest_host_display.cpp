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

HostDisplay::RenderAPI RegTestHostDisplay::GetRenderAPI() const
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

bool RegTestHostDisplay::CreateRenderDevice(const WindowInfo& wi, std::string_view adapter_name, bool debug_device,
                                            bool threaded_presentation)
{
  m_window_info = wi;
  return true;
}

bool RegTestHostDisplay::InitializeRenderDevice(std::string_view shader_cache_directory, bool debug_device,
                                                bool threaded_presentation)
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

void RegTestHostDisplay::DestroyRenderDevice()
{
  ClearSoftwareCursor();
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

std::unique_ptr<HostDisplayTexture> RegTestHostDisplay::CreateTexture(u32 width, u32 height, u32 layers, u32 levels,
                                                                      u32 samples, HostDisplayPixelFormat format,
                                                                      const void* data, u32 data_stride,
                                                                      bool dynamic /* = false */)
{
  return nullptr;
}

void RegTestHostDisplay::UpdateTexture(HostDisplayTexture* texture, u32 x, u32 y, u32 width, u32 height,
                                       const void* data, u32 data_stride)
{
}

bool RegTestHostDisplay::DownloadTexture(const void* texture_handle, HostDisplayPixelFormat texture_format, u32 x,
                                         u32 y, u32 width, u32 height, void* out_data, u32 out_data_stride)
{
  const u32 pixel_size = GetDisplayPixelFormatSize(texture_format);
  const u32 input_stride = Common::AlignUpPow2(width * pixel_size, 4);
  const u8* input_start = static_cast<const u8*>(texture_handle) + (x * pixel_size);
  StringUtil::StrideMemCpy(out_data, out_data_stride, input_start, input_stride, width * pixel_size, height);
  return true;
}

bool RegTestHostDisplay::SupportsDisplayPixelFormat(HostDisplayPixelFormat format) const
{
  return (format == HostDisplayPixelFormat::RGBA8);
}

bool RegTestHostDisplay::BeginSetDisplayPixels(HostDisplayPixelFormat format, u32 width, u32 height, void** out_buffer,
                                               u32* out_pitch)
{
  const u32 pitch = Common::AlignUpPow2(width * GetDisplayPixelFormatSize(format), 4);
  const u32 required_size = height * pitch;
  if (m_frame_buffer.size() != (required_size / 4))
  {
    m_frame_buffer.clear();
    m_frame_buffer.resize(required_size / 4);
  }

  // border is already filled here
  m_frame_buffer_pitch = pitch;
  SetDisplayTexture(m_frame_buffer.data(), format, width, height, 0, 0, width, height);
  *out_buffer = reinterpret_cast<u8*>(m_frame_buffer.data());
  *out_pitch = pitch;
  return true;
}

void RegTestHostDisplay::EndSetDisplayPixels()
{
  // noop
}

void RegTestHostDisplay::DumpFrame(const std::string& filename)
{
  if (!HasDisplayTexture())
  {
    if (FileSystem::FileExists(filename.c_str()))
      FileSystem::DeleteFile(filename.c_str());

    return;
  }

  Common::RGBA8Image image(m_display_texture_width, m_display_texture_height,
                           static_cast<const u32*>(m_display_texture_handle));

  // set alpha channel on all pixels
  u32* pixels = image.GetPixels();
  u32* pixels_end = pixels + (image.GetWidth() * image.GetHeight());
  while (pixels != pixels_end)
    *(pixels++) |= 0xFF000000u;

  if (!Common::WriteImageToFile(image, filename.c_str()))
    Log_ErrorPrintf("Failed to dump frame '%s'", filename.c_str());
}

void RegTestHostDisplay::SetVSync(bool enabled)
{
  Log_DevPrintf("Ignoring SetVSync(%u)", BoolToUInt32(enabled));
}

bool RegTestHostDisplay::Render()
{
  return true;
}

bool RegTestHostDisplay::RenderScreenshot(u32 width, u32 height, std::vector<u32>* out_pixels, u32* out_stride,
                                          HostDisplayPixelFormat* out_format)
{
  return false;
}
