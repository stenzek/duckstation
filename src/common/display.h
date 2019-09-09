#pragma once
#include "YBaseLib/Common.h"
#include "YBaseLib/String.h"
#include "YBaseLib/Timer.h"
#include "types.h"
#include <memory>
#include <mutex>

class DisplayRenderer;

class Display
{
public:
  enum : u8
  {
    // Priority for primary display. The display with the highest priority will be shown.
    DEFAULT_PRIORITY = 1
  };
  enum : u32
  {
    // Number of colours in paletted modes.
    PALETTE_SIZE = 256
  };
  enum class Type : u8
  {
    Primary,
    Secondary
  };
  enum class FramebufferFormat : u8
  {
    RGB8,
    RGBX8,
    BGR8,
    BGRX8,
    RGB565,
    RGB555,
    BGR565,
    BGR555,
    C8RGBX8, // 8-bit palette, 32-bit colours
  };

  Display(DisplayRenderer* renderer, const String& name, Type type, u8 priority);
  virtual ~Display();

  const String& GetName() const { return m_name; }
  Type GetType() const { return m_type; }
  u8 GetPriority() const { return m_priority; }

  bool IsEnabled() const { return m_enabled; }
  bool IsActive() const { return m_active; }
  void SetEnable(bool enabled);
  void SetActive(bool active);

  u32 GetFramesRendered() const { return m_frames_rendered; }
  float GetFramesPerSecond() const { return m_fps; }
  void ResetFramesRendered() { m_frames_rendered = 0; }

  u32 GetDisplayWidth() const { return m_display_width; }
  u32 GetDisplayHeight() const { return m_display_height; }
  void SetDisplayScale(u32 scale) { m_display_scale = scale; }
  void SetDisplayAspectRatio(u32 numerator, u32 denominator);
  void ResizeDisplay(u32 width = 0, u32 height = 0);

  u32 GetFramebufferWidth() const { return m_framebuffer_width; }
  u32 GetFramebufferHeight() const { return m_framebuffer_height; }
  FramebufferFormat GetFramebufferFormat() const { return m_framebuffer_format; }

  void ClearFramebuffer();
  void ResizeFramebuffer(u32 width, u32 height);
  void ChangeFramebufferFormat(FramebufferFormat new_format);
  void SwapFramebuffer();

  static constexpr u32 PackRGBX(u8 r, u8 g, u8 b)
  {
    return (static_cast<u32>(r) << 0) | (static_cast<u32>(g) << 8) | (static_cast<u32>(b) << 16) |
           (static_cast<u32>(0xFF) << 24);
  }

  // Changes pixels in the backbuffer.
  byte* GetFramebufferPointer() const { return m_back_buffers[0].data; }
  u32 GetFramebufferStride() const { return m_back_buffers[0].stride; }
  void SetPixel(u32 x, u32 y, u8 r, u8 g, u8 b);
  void SetPixel(u32 x, u32 y, u32 rgb);
  void CopyToFramebuffer(const void* pixels, u32 stride);
  void RepeatFrame();

  // Update palette.
  const u32* GetPalettePointer() const { return m_back_buffers[0].palette; }
  void SetPaletteEntry(u8 index, u32 value) const { m_back_buffers[0].palette[index] = value; }
  void CopyPalette(u8 start_index, u32 num_entries, const u32* entries);

  // Returns true if the specified format is a paletted format.
  static constexpr bool IsPaletteFormat(FramebufferFormat format) { return (format == FramebufferFormat::C8RGBX8); }

protected:
  static constexpr u32 NUM_BACK_BUFFERS = 2;

  struct Framebuffer
  {
    byte* data = nullptr;
    u32* palette = nullptr;
    u32 width = 0;
    u32 height = 0;
    u32 stride = 0;
    FramebufferFormat format = FramebufferFormat::RGBX8;
    bool dirty = false;
  };

  void AddFrameRendered();
  void AllocateFramebuffer(Framebuffer* fbuf);
  void DestroyFramebuffer(Framebuffer* fbuf);

  // Updates the front buffer. Returns false if the no swap has occurred.
  bool UpdateFrontbuffer();

  // Helper for converting/copying a framebuffer.
  static void CopyFramebufferToRGBA8Buffer(const Framebuffer* fbuf, void* dst, u32 dst_stride);

  DisplayRenderer* m_renderer;
  String m_name;

  Type m_type;
  u8 m_priority;

  u32 m_framebuffer_width = 0;
  u32 m_framebuffer_height = 0;
  FramebufferFormat m_framebuffer_format = FramebufferFormat::RGBX8;

  Framebuffer m_front_buffer;
  Framebuffer m_back_buffers[NUM_BACK_BUFFERS];
  std::mutex m_buffer_lock;

  u32 m_display_width = 640;
  u32 m_display_height = 480;
  u32 m_display_scale = 1;
  u32 m_display_aspect_numerator = 1;
  u32 m_display_aspect_denominator = 1;

  static constexpr u32 FRAME_COUNTER_FRAME_COUNT = 100;
  Timer m_frame_counter_timer;
  u32 m_frames_rendered = 0;
  float m_fps = 0.0f;

  bool m_enabled = true;
  bool m_active = true;
};
