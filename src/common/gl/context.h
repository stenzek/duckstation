#pragma once
#include "../types.h"
#include "../window_info.h"
#include <array>
#include <memory>
#include <vector>

namespace GL {
class Context
{
public:
  Context(const WindowInfo& wi);
  virtual ~Context();

  enum class Profile
  {
    NoProfile,
    Core,
    ES
  };

  struct Version
  {
    Profile profile;
    int major_version;
    int minor_version;
  };

  struct FullscreenModeInfo
  {
    u32 width;
    u32 height;
    float refresh_rate;
  };

  ALWAYS_INLINE const WindowInfo& GetWindowInfo() const { return m_wi; }
  ALWAYS_INLINE bool IsGLES() const { return (m_version.profile == Profile::ES); }
  ALWAYS_INLINE u32 GetSurfaceWidth() const { return m_wi.surface_width; }
  ALWAYS_INLINE u32 GetSurfaceHeight() const { return m_wi.surface_height; }
  ALWAYS_INLINE WindowInfo::SurfaceFormat GetSurfaceFormat() const { return m_wi.surface_format; }

  virtual void* GetProcAddress(const char* name) = 0;
  virtual bool ChangeSurface(const WindowInfo& new_wi) = 0;
  virtual void ResizeSurface(u32 new_surface_width = 0, u32 new_surface_height = 0) = 0;
  virtual bool SwapBuffers() = 0;
  virtual bool MakeCurrent() = 0;
  virtual bool DoneCurrent() = 0;
  virtual bool SetSwapInterval(s32 interval) = 0;
  virtual std::unique_ptr<Context> CreateSharedContext(const WindowInfo& wi) = 0;

  virtual std::vector<FullscreenModeInfo> EnumerateFullscreenModes();

  static std::unique_ptr<Context> Create(const WindowInfo& wi, const Version* versions_to_try,
                                         size_t num_versions_to_try);

  template<size_t N>
  static std::unique_ptr<Context> Create(const WindowInfo& wi, const std::array<Version, N>& versions_to_try)
  {
    return Create(wi, versions_to_try.data(), versions_to_try.size());
  }

  static std::unique_ptr<Context> Create(const WindowInfo& wi) { return Create(wi, GetAllVersionsList()); }

  static const std::array<Version, 11>& GetAllDesktopVersionsList();
  static const std::array<Version, 12>& GetAllDesktopVersionsListWithFallback();
  static const std::array<Version, 4>& GetAllESVersionsList();
  static const std::array<Version, 16>& GetAllVersionsList();

protected:
#ifdef _WIN32
#endif

  WindowInfo m_wi;
  Version m_version = {};
};
} // namespace GL
