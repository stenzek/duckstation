#pragma once
#include "../windows_headers.h"

#include "context.h"
#include "glad_wgl.h"
#include "loader.h"
#include <optional>

namespace GL {

class ContextWGL final : public Context
{
public:
  ContextWGL(const WindowInfo& wi);
  ~ContextWGL() override;

  static std::unique_ptr<Context> Create(const WindowInfo& wi, const Version* versions_to_try,
                                         size_t num_versions_to_try);

  void* GetProcAddress(const char* name) override;
  bool ChangeSurface(const WindowInfo& new_wi) override;
  void ResizeSurface(u32 new_surface_width = 0, u32 new_surface_height = 0) override;
  bool SwapBuffers() override;
  bool MakeCurrent() override;
  bool DoneCurrent() override;
  bool SetSwapInterval(s32 interval) override;
  std::unique_ptr<Context> CreateSharedContext(const WindowInfo& wi) override;

private:
  ALWAYS_INLINE HWND GetHWND() const { return static_cast<HWND>(m_wi.window_handle); }

  HDC GetDCAndSetPixelFormat(HWND hwnd);

  bool Initialize(const Version* versions_to_try, size_t num_versions_to_try);
  bool InitializeDC();
  void ReleaseDC();
  bool CreatePBuffer();
  bool CreateAnyContext(HGLRC share_context, bool make_current);
  bool CreateVersionContext(const Version& version, HGLRC share_context, bool make_current);

  HDC m_dc = {};
  HGLRC m_rc = {};

  // Can't change pixel format once it's set for a RC.
  std::optional<int> m_pixel_format;

  // Dummy window for creating a PBuffer off when we're surfaceless.
  HWND m_dummy_window = {};
  HDC m_dummy_dc = {};
  HPBUFFERARB m_pbuffer = {};
};

} // namespace GL