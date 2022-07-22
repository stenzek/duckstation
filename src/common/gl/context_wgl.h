#pragma once
#include "../windows_headers.h"
#include "context.h"
#include "loader.h"

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

  bool Initialize(const Version* versions_to_try, size_t num_versions_to_try);
  bool InitializeDC();
  bool CreateAnyContext(HGLRC share_context, bool make_current);
  bool CreateVersionContext(const Version& version, HGLRC share_context, bool make_current);

  HDC m_dc = {};
  HGLRC m_rc = {};
};

} // namespace GL